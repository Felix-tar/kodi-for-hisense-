/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVPlayer.h"

#include "ServiceBroker.h"
#include "TVBridgeIO.h"
#include "TVEsEngine.h"
#include "TVPlayerHttpFeeder.h"
#include "URL.h"
#include "Util.h"
#include "application/Application.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/Interface/InputStreamConstants.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/GUIAudioManager.h"
#include "guilib/GUIComponent.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "threads/SystemClock.h"
#include "utils/JobManager.h"
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XMLUtils.h"
#include "utils/log.h"
#include "video/Bookmark.h"
#include "video/VideoInfoTag.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace std::chrono_literals;
using namespace TVPLAYER;
using namespace KODI::MESSAGING;

namespace
{
// time limits per phase, always larger than flex.js' own (D13, section 3.8)
constexpr auto ACCEPT_TIMEOUT = 5s; // flex.js only takes jobs up to 3 s old
constexpr auto SWITCH_TIMEOUT = 15s; // flex.js gives up after ~10 s (DMP)
constexpr auto PREPARE_TIMEOUT = 30s; // flex.js gives up after 20 s
constexpr auto STATUS_TIMEOUT = 15s; // playing: flex.js gone (launcher restart, hang)
constexpr auto POLL_INTERVAL = 250ms;
constexpr auto CTL_REFRESH = 5s; // ctl with hb++ at least this often
constexpr auto PING_INTERVAL = 2s; // main thread heartbeat (REV:92-95)
constexpr int64_t MAIN_ALIVE_MS = 5000;
constexpr auto REVEAL_DELAY = 1s; // picture safely on the video plane (REVEAL_MS in flex.js)
constexpr auto SEEK_TIMEOUT = 10s; // a seek flex.js never acknowledges
constexpr auto RESUME_SAVE_INTERVAL = std::chrono::minutes(5); // E4
constexpr auto AE_SUSPEND_WAIT = 2000ms; // ExternalPlayer.cpp:298-302
constexpr int64_t MAX_EXTRAPOLATION_MS = 10000; // position counted on from the last status
constexpr int64_t END_SLACK_S = 10; // D11: "ended" counts only this close to the end
constexpr int64_t MAX_PLAYTIME_S = 360000;
constexpr int64_t REPLACEMENT_JUMP_MS = 30000; // FF/RW without trick play (P2)
constexpr double HIGH_BITRATE = 60e6; // bit/s over the network: hint (REV:136)
constexpr unsigned int TOAST_MS = 5000;
constexpr unsigned int TOAST_LONG_MS = 8000;

std::atomic<int64_t> s_mainAliveAt{0}; // file scope: outlives every player object

struct FallbackMsg
{
  ThreadMessageCallback cb; // ApplicationMessenger.h:155-159; not freed by Kodi
  CFileItem item;
  uint64_t generation = 0;
  std::string player = "VideoPlayer";
};
std::atomic<uint64_t> g_pendingFallback{0}; // 0 = none
std::atomic<uint64_t> g_nextGeneration{1};

// files the TV refused in this Kodi run: next time directly to Kodi's player
std::mutex g_rejectedLock;
std::set<std::string> g_rejected;
// files that failed on the ES path in this Kodi run: next time directly to the TV player
std::set<std::string> g_esRejected;

int64_t SteadyMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool EnvSwitch(const char* name)
{
  const char* value = std::getenv(name);
  return value && std::strcmp(value, "1") == 0;
}

// read once; since D19 nobody changes Kodi's environment any more
bool TrickPlaySwitch()
{
  // P2: D21 (speed like hiMedia) is not confirmed, default is Kodi's replacement jump
  static const bool on = EnvSwitch("FLEX_KODI_TRICKPLAY");
  return on;
}

bool UnderlayBlendSwitch()
{
  static const bool on = EnvSwitch("FLEX_KODI_UNDERLAY_BLEND");
  return on;
}

//! ctl.speed / player.json.spd -> Kodi's speed factor (200 -> 2, 100400 -> -4)
float SpeedFactor(int spd)
{
  if (spd >= 200 && spd <= 1600)
    return static_cast<float>(spd) / 100.0f;
  if (spd >= 100200 && spd <= 101600)
    return -static_cast<float>(spd - 100000) / 100.0f;
  return 1.0f;
}

int SpeedValue(float speed)
{
  const float a = std::fabs(speed);
  const int step = a >= 16.0f ? 1600 : (a >= 8.0f ? 800 : (a >= 4.0f ? 400 : 200));
  return speed > 0.0f ? step : 100000 + step;
}

std::string FormatSeconds(int64_t s)
{
  return s >= 3600 ? StringUtils::Format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60)
                   : StringUtils::Format("{}:{:02}", s / 60, s % 60);
}

void Toast(CGUIDialogKaiToast::eMessageType type, const std::string& text, unsigned int ms)
{
  CGUIDialogKaiToast::QueueNotification(type, "TV-Player", text, ms, false);
}

//! hint when Kodi's player takes over (table in section 4.3)
std::string FallbackText(const std::string& why, int err, int64_t resumeS)
{
  if (why == "es")
    return "Echte Bildrate hier nicht möglich, der TV-Player übernimmt";
  if (why == "noBridge" || why == "key")
    return "TV-Player nicht erreichbar, Kodi spielt selbst";
  if (why == "app")
    return "YouTube/Netflix ist im Hintergrund pausiert, Kodi spielt selbst";
  if (why == "dmpForeign")
    return "Quelle 'Medien' ist belegt, Kodi spielt selbst";
  if (why == "notHome" || why == "dialog" || why == "busy" || why == "kodi")
    return "TV gerade nicht frei, Kodi spielt selbst";
  if (why == "bad")
    return "TV-Player: Auftrag ungültig, Kodi spielt selbst";
  if (why == "dmpTimeout" || why == "prepTimeout" || why == "noAnswer")
    return "TV-Player antwortet nicht, Kodi spielt selbst";
  if (why == "player")
    return StringUtils::Format("TV kann die Datei nicht abspielen (Code {}), Kodi spielt selbst", err);
  if (why == "early")
    return "TV-Player hat die Datei vor dem Start beendet, Kodi spielt selbst";
  if (why == "sourceOut")
    return "Eingang am TV gewechselt, Kodi spielt selbst";
  if (why == "noResume")
    return "Datei nicht spulbar, Kodi spielt selbst ab " + FormatSeconds(resumeS);
  if (why == "av1")
    return "TV kann AV1 nicht abspielen, Kodi spielt selbst";
  if (why == "hi10p")
    return "H.264 mit mehr als 8 Bit: Kodi spielt selbst";
  if (why == "noTvAudio")
    return "Tonformat kann der TV nicht abspielen, Kodi spielt selbst";
  if (why == "extSub")
    return "Externe Untertitel zeigt nur Kodis Player, Kodi spielt selbst";
  if (why == "rejectedBefore")
    return "TV hat die Datei schon abgelehnt, Kodi spielt selbst";
  if (why == "probe" || why == "feeder")
    return "TV-Player: Datei nicht lesbar, Kodi spielt selbst";
  return "TV-Player nicht möglich (" + why + "), Kodi spielt selbst";
}

//! message instead of a fallback for 4K that Kodi's software cannot decode (E2): with the way out
std::string ErrorText(const std::string& why, int err)
{
  if (why == "noBridge" || why == "key")
    return "TV-Player nicht erreichbar. Kodi neu starten; hilft das nicht, TV neu starten.";
  if (why == "app")
    return "YouTube/Netflix ist im Hintergrund pausiert und sperrt den TV-Player. Kodi beenden, "
           "die App öffnen und mit Exit beenden (oder eine andere App kurz starten und beenden), "
           "dann Kodi neu starten.";
  if (why == "dmpForeign")
    return "Quelle 'Medien' ist belegt. Kodi beenden, einmal den Eingang wechseln (z. B. TV), "
           "Kodi neu starten.";
  if (why == "notHome" || why == "dialog" || why == "busy" || why == "kodi")
    return "TV gerade nicht frei, bitte gleich noch einmal";
  if (why == "bad")
    return "TV-Player: Auftrag ungültig";
  if (why == "dmpTimeout" || why == "prepTimeout" || why == "noAnswer")
    return "TV-Player antwortet nicht";
  if (why == "player")
    return StringUtils::Format("TV kann die Datei nicht abspielen (Code {})", err);
  if (why == "early")
    return "TV-Player hat die Datei vor dem Start beendet";
  if (why == "sourceOut")
    return "Eingang am TV gewechselt, bitte noch einmal starten";
  if (why == "dvp5")
    return "Dolby Vision Profil 5 wird nicht unterstützt";
  if (why == "av1")
    return "TV kann AV1 nicht abspielen";
  if (why == "hi10p")
    return "H.264 mit mehr als 8 Bit kann der TV nicht abspielen";
  if (why == "rejectedBefore")
    return "TV hat die Datei schon abgelehnt";
  if (why == "probe" || why == "feeder")
    return "TV-Player: Datei nicht lesbar";
  return "TV-Player nicht möglich (" + why + ")";
}

bool IsTextSubtitle(const std::string& ext)
{
  return ext == ".srt" || ext == ".ass" || ext == ".ssa";
}
} // namespace

CTVPlayer::CTVPlayer(IPlayerCallback& callback)
  : IPlayer(callback),
    CThread("TVPlayer"),
    m_outboundEvents(std::make_unique<CJobQueue>(false, 1, CJob::PRIORITY_NORMAL))
{
}

CTVPlayer::~CTVPlayer()
{
  CloseFile();
  while (m_outboundEvents->IsProcessing()) // like VideoPlayer.cpp:657-660
    CThread::Sleep(10ms);
}

bool CTVPlayer::Initialize(TiXmlElement* pConfig)
{
  if (pConfig)
  {
    XMLUtils::GetBoolean(pConfig, "fallback", m_cfgFallback);
    XMLUtils::GetInt(pConfig, "fallbackmaxwidth", m_cfgFallbackMaxWidth, 0, 16384);
    XMLUtils::GetBoolean(pConfig, "probestreams", m_cfgProbe);
    XMLUtils::GetBoolean(pConfig, "suspendae", m_cfgSuspendAE);
    XMLUtils::GetBoolean(pConfig, "periodicresume", m_cfgPeriodicResume);
    XMLUtils::GetBoolean(pConfig, "dvprofile5", m_cfgAllowDvProfile5);
    XMLUtils::GetBoolean(pConfig, "filecache", m_cfgFileCache);
    XMLUtils::GetBoolean(pConfig, "es", m_cfgEs);
    XMLUtils::GetInt(pConfig, "peeruid", m_cfgPeerUid, -1, 0x7fffffff);
    XMLUtils::GetInt(pConfig, "feederconnections", m_cfgFeederConnections, 2, 64);
    XMLUtils::GetInt(pConfig, "feedertimeout", m_cfgFeederTimeoutS, 10, 86400);
    std::string audioIndex;
    if (XMLUtils::GetString(pConfig, "audioindex", audioIndex))
      m_cfgAudioIndex = StringUtils::EqualsNoCase(audioIndex, "file") ? TVAudioIndex::FILE
                                                                      : TVAudioIndex::SUPPORTED;
  }
  m_bridge = std::make_unique<CTVBridge>();
  CLog::Log(LOGDEBUG,
            "CTVPlayer::Initialize - fallback {} (max width {}), probe {}, suspend AE {}, periodic "
            "resume {}, peer uid {}, {} connections, timeout {} s, trick play {}",
            m_cfgFallback, m_cfgFallbackMaxWidth, m_cfgProbe, m_cfgSuspendAE, m_cfgPeriodicResume,
            m_cfgPeerUid, m_cfgFeederConnections, m_cfgFeederTimeoutS, TrickPlaySwitch());
  // never false: CreatePlayer() would return nullptr and "Play" would do nothing
  return true;
}

bool CTVPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  CLog::Log(LOGINFO, "CTVPlayer::OpenFile - {}", CURL::GetRedacted(file.GetDynPath()));

  // ApplicationPlayer reuses this object for the next playlist item (ApplicationPlayer.cpp:137-145)
  StopThread(true);
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    // a new file supersedes the previous session's fallback that has not started yet (an
    // immediate rejection leaves no CloseFile() in between); the thread is gone, so nothing
    // can publish it again
    uint64_t previous = m_generation;
    g_pendingFallback.compare_exchange_strong(previous, 0);
    ResetSession();
    m_item = file;
    m_options = options;
  }
  CDataCacheCore& cache = CServiceBroker::GetDataCacheCore();
  cache.SetPlayTimes(0, 0, 0, 0); // like VideoPlayer.cpp:683
  cache.SetSpeed(1.0f, 1.0f);
  cache.SetVideoRender(false);

  std::string why;
  if (RejectImmediately(file, why))
  {
    CLog::Log(LOGINFO, "CTVPlayer::OpenFile - not for the TV ({}), Kodi's player takes over", why);
    // without the bridge the size is unknown here: the library's stream details decide (E2)
    if ((why == "noBridge" || why == "rejectedBefore") && !FallbackAllowed())
    {
      Toast(CGUIDialogKaiToast::Error, ErrorText(why, 0), TOAST_LONG_MS);
      return true;
    }
    RequestFallback(why); // async: ApplicationPlayer::OpenFile is still on the stack
    return true; // not playing, no OnPlayBackStarted
  }

  cache.SetGuiRender(true); // full redraws (opaque black) until the picture is revealed
  m_playing = true;
  m_startedNotified = true;
  // before the thread exists: its end callback can never overtake this one
  m_callback.OnPlayBackStarted(m_item); // like VideoPlayer.cpp:694: the GUI shows the busy dialog
  Create();
  return true;
}

bool CTVPlayer::CloseFile(bool reopen)
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (m_end == End::NONE)
      m_end = End::STOPPED; // user stop, next item, shutdown
    // a fallback that has not started yet must not start any more; one in progress already
    // cleared it (FallbackOnAppThread), so this does not stop its own switch
    uint64_t mine = m_generation;
    g_pendingFallback.compare_exchange_strong(mine, 0);
    m_desired.stop = 1;
    ++m_desired.seq;
  }
  m_wake.Set();
  {
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext()); // VideoPlayer.cpp:722-725
    StopThread(true); // -> OnExit(): ctl stop:1, feeder, AE, callbacks
  }
  m_revealed = false;
  CServiceBroker::GetDataCacheCore().Reset(); // UPnPPlayer.cpp:465 -> opaque GUI again
  m_playing = false;
  return true;
}

void CTVPlayer::ResetSession()
{
  // m_section is held
  m_sid = RandomHex(8);
  m_generation = g_nextGeneration++;
  m_probe = TVProbe();
  m_desired = Desired();
  m_ctlWritten = false;
  m_phase = Phase::IDLE;
  m_phaseSince = std::chrono::steady_clock::now();
  m_end = End::NONE;
  m_tvPaused = m_seekable = m_caching = m_eof = false;
  m_tvPosS = m_tvDurS = 0;
  m_tvPosMs = -1;
  m_esMode = m_esStartPending = false;
  m_tvRecvAt = std::chrono::steady_clock::time_point{};
  m_tvAck = -1;
  m_tvAud = -1;
  m_tvSpd = 100;
  m_noResume = false;
  m_resumeOrigS = 0;
  m_tvAcodec.clear();
  m_seekTargetMs = -1;
  m_seekSeq = 0;
  m_audioIdx = m_audioPrevIdx = -1;
  m_audioSeq = 0;
  m_audioCheckPending = false;
  m_subIdx = -1;
  m_subVisible = false;
  m_lastResumeSaveMs = -1;
  m_notified.clear();
  m_lastStateKey.clear();
  m_durationLogged = false;
  m_startedNotified = false;
  m_avStartedNotified = false;
  m_revealed = false;
  m_guiSoundsOff = false;
  if (m_bridge)
    m_bridge->Reset();
  m_wake.Reset();
}

bool CTVPlayer::RejectImmediately(const CFileItem& item, std::string& why) const
{
  // independent of the rules the user can change (KI:316-322)
  const std::string path = item.GetDynPath();
  const std::string ext = StringUtils::ToLower(URIUtils::GetExtension(path));
  if (!item.GetProperty(STREAM_PROPERTY_INPUTSTREAM).empty())
    why = "inputstream"; // add-ons only work in VideoPlayer, DRM stays out
  else if (item.IsDiscImage() || item.IsDVDFile() || item.IsBDFile() || URIUtils::IsBluray(path))
    why = "disc";
  else if (item.IsPVR() || item.IsLiveTV())
    why = "pvr";
  else if (!item.IsVideo())
    why = "notVideo";
  else if (ext == ".m3u8" || ext == ".mpd" || ext == ".ism" || ext == ".isml")
    why = "adaptive";
  else if (!CTVBridgeIO::Get().IsUsable() || CTVBridgeIO::Get().IsSwitchedOff())
    why = "noBridge"; // no run key, or the watcher switched the bridge off (section 6)
  else
  {
    std::unique_lock<std::mutex> lock(g_rejectedLock);
    if (g_rejected.count(path))
      why = "rejectedBefore";
  }
  return !why.empty();
}

int64_t CTVPlayer::StartSeconds() const
{
  // 4.4: starttime, else startpercent of the duration; whole seconds (the TV only knows those)
  std::unique_lock<CCriticalSection> lock(m_section);
  double s = m_options.starttime;
  if (s <= 0.0 && m_options.startpercent > 0.0)
  {
    const int64_t total = TotalTimeMs();
    if (total > 0)
      s = m_options.startpercent * static_cast<double>(total) / 100000.0;
  }
  return std::clamp<int64_t>(std::llround(s), 0, MAX_PLAYTIME_S);
}

std::string CTVPlayer::CheckEs(const std::string& path,
                               const TVProbe& probe,
                               int audio,
                               const TVSubtitleChoice& sub,
                               TVProbe& esProbe,
                               int& esAudio,
                               bool& subtitlesDropped) const
{
  subtitlesDropped = false;
  if (!m_cfgEs || EnvSwitch("FLEX_KODI_NO_ES"))
    return "switched off";
  if (!probe.valid || !probe.video.valid)
    return "no probe";
  {
    std::unique_lock<std::mutex> lock(g_rejectedLock);
    if (g_esRejected.count(path))
      return "failed before";
  }
  if (!EsVideoCode(probe.video))
    return "video " + probe.video.codecName + (probe.video.interlaced ? " interlaced" : "");
  if (!EsFrameRate(probe.video.fps))
    return "frame rate unknown";
  if (probe.video.stereoMode != "" && probe.video.stereoMode != "mono")
    return "3D";
  // every track Kodi can decode plays here: AC-3/E-AC-3 as they are, the rest converted to AC-3
  esProbe = probe;
  for (size_t i = 0; i < esProbe.audio.size(); ++i)
  {
    TVTrack& t = esProbe.audio[i];
    t.converted = EsConvertsAudio(t);
    t.tvSupported = EsAudioCode(t.codec) != 0 || t.converted;
    t.tvIndex = t.tvSupported ? static_cast<int>(i) : -1;
  }
  esAudio = ChooseAudio(esProbe);
  if (esAudio < 0)
    return "no audio track for this path";
  const TVTrack& wanted = esProbe.audio[esAudio];
  const bool tvPlayerSame =
      audio >= 0 && audio < static_cast<int>(probe.audio.size()) &&
      (probe.audio[audio].language == wanted.language ||
       SameAudioLanguage(probe.audio[audio].language, wanted.language));
  // this path has no subtitles yet: whoever asked for them keeps them with the TV player, as long
  // as it plays the same language there. Is the language only playable with Kodi's conversion
  // (TrueHD, FLAC, Opus ...), the sound comes first
  if (sub.visible)
  {
    if (tvPlayerSame)
      return "subtitles on";
    subtitlesDropped = true;
  }
  // the TV player's language only in a format this path cannot play at all (no decoder)
  if (audio >= 0 && audio < static_cast<int>(probe.audio.size()) && !tvPlayerSame &&
      !esProbe.audio[audio].tvSupported)
    return "audio language only as " + probe.audio[audio].codec;
  return "";
}

bool CTVPlayer::StartEs()
{
  CTVEsEngine::Params params;
  Desired ctl;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    params.item = m_item;
    params.audioIndex = m_audioIdx;
    params.vcodec = EsVideoCode(m_probe.video);
    params.acodec = m_audioIdx >= 0 ? EsAudioOutCode(m_probe.audio[m_audioIdx]) : 0;
    params.fps = m_probe.video.fps;
    params.fpsMhz = EsFrameRate(m_probe.video.fps);
    params.startMs = m_resumeOrigS * 1000;
    params.dar = m_probe.video.videoAspectRatio;
    params.durationMs = m_probe.durationMs;
    params.audioTracks = m_probe.audio;
    ctl = m_desired;
  }
  auto es = std::make_unique<CTVEsEngine>();
  if (!es->Start(params))
    return false;
  es->Apply(ctl); // the state it starts from
  std::unique_lock<CCriticalSection> lock(m_section);
  m_es = std::move(es);
  return true;
}

bool CTVPlayer::Prepare(std::string& why)
{
  CFileItem item;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    item = m_item;
  }
  const std::string path = item.GetDynPath();

  // 1. tracks, display aspect ratio, chapters (4.8)
  TVProbe probe;
  if (m_cfgProbe && !ProbeStreams(item, probe, m_cfgAudioIndex))
  {
    why = "probe"; // Kodi's player shows the real error
    return false;
  }
  if (m_bStop)
    return false;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    m_probe = probe; // FallbackAllowed() needs the video size from here on
  }

  // 2. can the TV play it at all?
  std::vector<std::string> notices;
  if (probe.valid && TvCanPlay(probe, m_cfgAllowDvProfile5, why, notices) != TVCanPlay::YES)
    return false;

  // 3. start choice of audio and subtitles, already in the first ctl (REV:209)
  int audio = probe.valid ? ChooseAudio(probe) : -1;
  const std::string audioLanguage = audio >= 0 ? probe.audio[audio].language : std::string();

  std::vector<std::string> subtitleFiles;
  CUtil::ScanForExternalSubtitles(path, subtitleFiles);
  std::vector<std::string> externalLanguages;
  std::vector<std::string> textFiles;
  std::vector<std::string> idxFiles;
  for (const std::string& file : subtitleFiles)
  {
    const std::string ext = StringUtils::ToLower(URIUtils::GetExtension(file));
    if (ext == ".sub" && CUtil::IsVobSub(subtitleFiles, file))
      continue; // the .sub of a VobSub pair counts with its .idx
    externalLanguages.push_back(CUtil::GetExternalStreamDetailsFromFilename(path, file).language);
    if (IsTextSubtitle(ext))
      textFiles.push_back(file);
    else if (ext == ".idx")
      idxFiles.push_back(file);
  }
  const TVSubtitleChoice sub =
      probe.valid ? ChooseSubtitle(probe, audioLanguage, externalLanguages) : TVSubtitleChoice();

  // the real frame rate: from here on the tracks are what the ES path can play
  TVProbe esProbe;
  int esAudio = -1;
  bool subtitlesDropped = false;
  const std::string notEs = CheckEs(path, probe, audio, sub, esProbe, esAudio, subtitlesDropped);
  const bool es = notEs.empty();
  CLog::Log(LOGINFO, "CTVPlayer::Prepare - {}{}{}", es ? "ES path" : "TV player: ", notEs,
            es && subtitlesDropped ? " (subtitles off: the audio track plays only here)" : "");
  if (es && subtitlesDropped)
    notices.push_back("Untertitel aus: diese Tonspur geht nur ohne Untertitel");
  if (es)
  {
    probe = std::move(esProbe);
    audio = esAudio;
    std::unique_lock<CCriticalSection> lock(m_section);
    m_probe = probe;
    m_esMode = true;
  }

  if (!es && sub.external && sub.visible)
  {
    // the TV gets no subtitles afterwards (setExternalTitleUri is never used, TA:288); whether it
    // fetches media.srt itself is open (P3)
    if (FallbackAllowed())
    {
      why = "extSub";
      return false;
    }
    notices.push_back("Externe Untertitel: bei 4K nur, wenn der TV sie selbst lädt");
  }
  if (m_bStop)
    return false;

  std::shared_ptr<CTVHttpFeeder> feeder;
  if (!es)
  {
    // 4. local HTTP server for the file (4.7)
    CTVHttpFeeder::Config config;
    config.connections = static_cast<unsigned int>(m_cfgFeederConnections);
    config.timeoutS = static_cast<unsigned int>(m_cfgFeederTimeoutS);
    config.peerUid = m_cfgPeerUid;
    config.fileCache = m_cfgFileCache;
    feeder = std::make_shared<CTVHttpFeeder>(config);
    // exactly one text file, or one .idx/.sub pair: also offered as media.<ext> (P3 shows if the
    // TV player asks for it, the feeder logs that)
    if (textFiles.size() == 1 && idxFiles.empty())
      feeder->AddSubtitleFile(textFiles.front(), URIUtils::GetExtension(textFiles.front()));
    else if (textFiles.empty() && idxFiles.size() == 1)
    {
      std::string subPath;
      if (CUtil::FindVobSubPair(subtitleFiles, idxFiles.front(), subPath))
      {
        feeder->AddSubtitleFile(idxFiles.front(), ".idx");
        feeder->AddSubtitleFile(subPath, ".sub");
      }
    }
    if (!feeder->Start(path, URIUtils::GetExtension(path)))
    {
      why = "feeder";
      return false;
    }

    // 5. high data rate over the network: hint (P14 decides about <filecache>)
    if (probe.durationMs > 0 && (URIUtils::IsSmb(path) || URIUtils::IsInternetStream(path)) &&
        static_cast<double>(feeder->GetLength()) * 8.0 * 1000.0 / probe.durationMs > HIGH_BITRATE)
      notices.push_back("Hohe Datenrate: ruckelt es, besser über USB");
  }

  {
    std::unique_lock<CCriticalSection> lock(m_section);
    m_feeder = feeder;
    m_audioIdx = audio;
    m_desired.audio = audio >= 0 ? probe.audio[audio].tvIndex : -1;
    m_audioSeq = 0;
    m_audioCheckPending = audio >= 0;
    if (probe.valid)
    {
      m_subIdx = es ? -1 : sub.index;
      m_subVisible = !es && sub.visible && sub.index >= 0;
      m_desired.sub = m_subVisible ? probe.subtitles[sub.index].tvIndex : -1;
    }
    // without the probe Kodi has no choice: "unchanged" (-2) stays
  }

  // 6. what the info screens show (KI 7.3)
  CDataCacheCore& cache = CServiceBroker::GetDataCacheCore();
  if (probe.video.valid)
  {
    cache.SetVideoDecoderName((es ? "TV ES " : "TV ") + probe.video.codecName, true);
    cache.SetVideoDimensions(probe.video.width, probe.video.height);
    cache.SetVideoFps(probe.video.fps);
    cache.SetVideoDAR(probe.video.videoAspectRatio);
    cache.SetVideoInterlaced(probe.video.interlaced);
  }
  if (audio >= 0)
  {
    cache.SetAudioDecoderName("TV " + probe.audio[audio].codec +
                              (probe.audio[audio].converted ? " -> AC3" : ""));
    cache.SetAudioChannels(std::to_string(probe.audio[audio].channels));
    cache.SetAudioSampleRate(probe.audio[audio].sampleRate);
    cache.SetAudioBitsPerSample(probe.audio[audio].bitsPerSample);
  }
  cache.SetChapters(probe.chapters);
  cache.SignalVideoInfoChange();
  cache.SignalAudioInfoChange();
  cache.SignalSubtitleInfoChange();

  for (const std::string& notice : notices)
    Toast(CGUIDialogKaiToast::Info, notice, TOAST_LONG_MS);

  CLog::Log(LOGINFO,
            "CTVPlayer::Prepare - {}x{} {} dar {:.4f}, {} audio (start {}), {} subtitles (start {}, "
            "{}), {} chapters, {} s",
            probe.video.width, probe.video.height, probe.video.codecName,
            probe.video.videoAspectRatio, probe.audio.size(), audio, probe.subtitles.size(),
            sub.index, sub.visible ? "on" : "off", probe.chapters.size(), probe.durationMs / 1000);
  // P3: which TV index each audio track gets, so a wrong track can be read from kodi.log
  if (!probe.audio.empty())
  {
    std::string tracks;
    for (size_t i = 0; i < probe.audio.size(); ++i)
    {
      const TVTrack& t = probe.audio[i];
      tracks += StringUtils::Format("{}{} {} {} -> {}", i ? ", " : "", i, t.codec,
                                    t.language.empty() ? "-" : t.language,
                                    t.tvIndex >= 0 ? std::to_string(t.tvIndex) + (t.converted ? " (ac3)" : "")
                                                   : "none");
    }
    CLog::Log(LOGINFO, "CTVPlayer::Prepare - audio file -> TV index ({}): {}",
              m_cfgAudioIndex == TVAudioIndex::FILE ? "audioindex file" : "audioindex supported",
              tracks);
  }
  return true;
}

void CTVPlayer::Process()
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    m_phase = Phase::PREPARE;
    m_phaseSince = std::chrono::steady_clock::now();
  }

  uint32_t ctlSeqWritten = 0;
  std::string why;
  if (!Prepare(why))
  {
    if (!m_bStop)
      FailBeforePlaying(why); // then wait below for CloseFile()
  }
  else if (!m_bStop)
  {
    Desired ctl;
    uint16_t port = 0;
    std::string tok;
    std::string name;
    float dar = 0.0f;
    bool es = false;
    {
      std::unique_lock<CCriticalSection> lock(m_section);
      ++m_desired.hb; // m_desired.audio/sub already hold the start choice
      ctl = m_desired;
      m_resumeOrigS = StartSeconds();
      es = m_esMode;
      if (m_feeder)
      {
        port = m_feeder->GetPort();
        tok = m_feeder->GetToken();
        name = m_feeder->GetName();
      }
      dar = m_probe.video.videoAspectRatio;
    }
    // the watcher may have switched the bridge off since OpenFile (probe and feeder take
    // seconds): flex.js would never see the job, so fall back now instead of after 5 s (section 6)
    const bool switchedOff = CTVBridgeIO::Get().IsSwitchedOff();
    bool ok = false;
    if (!switchedOff)
    {
      // ctl (seq 0) BEFORE play.json (FB:252)
      const bool ctlOk = m_bridge->WriteCtl(m_sid, ctl);
      {
        std::unique_lock<CCriticalSection> lock(m_section);
        m_ctlWritten = ctlOk;
      }
      ctlSeqWritten = ctl.seq;
      ok = ctlOk && (es ? m_bridge->WritePlayEs(m_sid)
                        : m_bridge->WritePlay(m_sid, port, tok, name, m_resumeOrigS, dar,
                                              UnderlayBlendSwitch() ? 1 : 0, // ov (D16)
                                              FallbackAllowed() ? 1 : 0)); // fb (4.4)
    }
    if (!ok)
    {
      if (switchedOff)
        CLog::Log(LOGWARNING, "CTVPlayer::Process - the watcher switched the bridge off, no job");
      else
        CLog::Log(LOGERROR, "CTVPlayer::Process - the bridge to flex.js cannot be written");
      FailBeforePlaying("noBridge");
    }
    else
    {
      std::unique_lock<CCriticalSection> lock(m_section);
      m_phase = Phase::WAIT_ACCEPT;
      m_phaseSince = std::chrono::steady_clock::now();
      CLog::Log(LOGINFO, "CTVPlayer::Process - job {} for flex.js, start at {} s", m_sid,
                m_resumeOrigS);
    }
  }

  auto lastCtl = std::chrono::steady_clock::now();
  auto lastStatus = lastCtl;
  auto lastPing = lastCtl - 10s;
  while (!m_bStop)
  {
    const End end = m_end;
    if (end == End::FALLBACK)
    {
      AbortableWait(m_wake, 1000ms); // CloseFile from ApplicationPlayer follows (KI:356-361)
      continue;
    }
    if (end != End::NONE)
      break; // ERROR (also from FailBeforePlaying without fallback), or CloseFile

    TVStatus st;
    int64_t maxAck = 0;
    {
      std::unique_lock<CCriticalSection> lock(m_section);
      maxAck = m_desired.seq;
    }
    if (m_bridge->ReadPlayer(m_sid, maxAck, st)) // frame, HMAC, sid, n > last (4.6)
    {
      lastStatus = std::chrono::steady_clock::now();
      if (!OnStatus(st))
        break;
      if (m_end != End::NONE)
        continue;
    }

    // ES path: flex.js has provided the source, now the helper plays and reports like a player
    if (m_esStartPending)
    {
      m_esStartPending = false;
      if (!StartEs())
      {
        FailBeforePlaying("es");
        continue;
      }
    }
    if (m_es && m_es->GetStatus(st))
    {
      lastStatus = std::chrono::steady_clock::now();
      if (!OnStatus(st))
        break;
      if (m_end != End::NONE)
        continue;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastPing >= PING_INTERVAL)
    {
      PingMainThread();
      lastPing = now;
    }
    if (const char* timeout = PhaseTimedOut(now))
    {
      CLog::Log(LOGWARNING, "CTVPlayer::Process - no answer from flex.js in time ({})", timeout);
      FailBeforePlaying(timeout); // RemovePlay() first
      continue;
    }

    Desired ctl;
    bool writeCtl = false;
    bool flexGone = false;
    {
      std::unique_lock<CCriticalSection> lock(m_section);
      if (m_phase == Phase::PLAYING && now - lastStatus > STATUS_TIMEOUT)
        flexGone = true;
      else if (m_desired.seq != ctlSeqWritten || now - lastCtl >= CTL_REFRESH)
      {
        // GUI hangs -> hb stands -> flex.js stops the film after 15 s (REV:92-95)
        if (MainThreadAlive() || m_phase != Phase::PLAYING)
          ++m_desired.hb;
        ctl = m_desired;
        writeCtl = true;
        ctlSeqWritten = ctl.seq;
        lastCtl = now;
      }
      if (!m_revealed && m_avStartedNotified && now >= m_revealAt)
      {
        m_revealed = true;
        CServiceBroker::GetDataCacheCore().SetVideoRender(true); // -> IsRenderingVideoLayer()
        CServiceBroker::GetDataCacheCore().SetGuiRender(false); // redraw only on change
        CLog::Log(LOGINFO, "CTVPlayer::Process - Kodi transparent over the TV picture");
      }
      if (m_seekTargetMs >= 0 && now - m_seekSince > SEEK_TIMEOUT)
      {
        CLog::Log(LOGWARNING, "CTVPlayer::Process - seek to {} s not acknowledged",
                  m_seekTargetMs / 1000);
        m_seekTargetMs = -1;
        CServiceBroker::GetDataCacheCore().SetStateSeeking(false);
      }
    }
    if (flexGone)
    {
      CLog::Log(LOGERROR, "CTVPlayer::Process - no status from flex.js for 15 s, giving up");
      if (SetEnd(End::ERROR)) // a stop by the user that came first stays a stop
        Toast(CGUIDialogKaiToast::Error, "TV-Player antwortet nicht mehr", TOAST_MS);
      break;
    }
    if (writeCtl)
    {
      m_bridge->WriteCtl(m_sid, ctl);
      if (m_es)
        m_es->Apply(ctl);
    }

    UpdatePlayTimes();
    SavePeriodicResume();
    if (m_guiSoundsOff && CServiceBroker::GetGUI()) // undo Application.cpp:1255 (pause), E3
      CServiceBroker::GetGUI()->GetAudioManager().Enable(false);
    AbortableWait(m_wake, POLL_INTERVAL);
  }
}

const char* CTVPlayer::PhaseTimedOut(std::chrono::steady_clock::time_point now) const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  const auto in = now - m_phaseSince;
  switch (m_phase)
  {
    case Phase::WAIT_ACCEPT:
      return in > ACCEPT_TIMEOUT ? "noBridge" : nullptr; // flex.js answers within ~1 s
    case Phase::SWITCHING:
      return in > SWITCH_TIMEOUT ? "dmpTimeout" : nullptr;
    case Phase::PREPARING:
      return in > PREPARE_TIMEOUT ? "prepTimeout" : nullptr;
    default:
      return nullptr;
  }
}

bool CTVPlayer::OnStatus(const TVStatus& st)
{
  std::unique_lock<CCriticalSection> lock(m_section);
  const auto now = std::chrono::steady_clock::now();
  // state changes at LOGINFO, the periodic repeats only at LOGDEBUG
  const std::string stateKey = st.st + "/" + st.why;
  const bool stateChanged = stateKey != m_lastStateKey;
  m_lastStateKey = stateKey;
  // on the ES path flex.js only reports on the source: position and player come from the helper
  if (!m_esMode || st.posMs >= 0)
  {
    m_tvAck = st.ack;
    m_tvPosS = st.pos;
    m_tvPosMs = st.posMs;
    m_tvDurS = st.dur;
    m_tvRecvAt = now;
    m_seekable = st.seek;
    m_eof = m_eof || st.eof;
    m_tvAud = st.aud;
    m_tvAcodec = st.acodec;
    m_tvSpd = st.spd;
  }
  if (st.why == "noResume")
    m_noResume = true; // 4.4
  CLog::Log(stateChanged ? LOGINFO : LOGDEBUG,
            "CTVPlayer: {} n {} st {} why '{}' mp {} flags {} err {} ack {} pos {}/{} s seek {} "
            "audio {} '{}' speed {}",
            m_sid, st.n, st.st, st.why, st.mp, st.flags, st.err, st.ack, st.pos, st.dur, st.seek,
            st.aud, st.acodec, st.spd);

  // 3.7/6: before PLAYING, "stopped" because the TV player let go of the file (state 6/9 between
  // 2 and 3, "early"), the home screen was no longer free ("notHome") or the source changed
  // ("sourceOut") counts like a refusal: RemovePlay, then Kodi's player (E2: message for 4K).
  // "dialog", "taken", "kodiGone", "superseded" and Kodi's own stop stay a stop (Kodi is being
  // closed, or the stop came from Kodi).
  const bool stoppedBeforeStart =
      st.st == "stopped" && !m_avStartedNotified &&
      (st.why == "early" || st.why == "notHome" || st.why == "sourceOut");
  if (st.st == "refused" || (st.st == "error" && !m_avStartedNotified) || stoppedBeforeStart)
  {
    lock.unlock();
    FailBeforePlaying(st.why.empty() ? st.st : st.why, st.err);
    return true; // stay until CloseFile()
  }

  const bool active = st.st == "switching" || st.st == "esready" || st.st == "preparing" ||
                      st.st == "playing" || st.st == "paused" || st.st == "buffering";
  if (active && m_phase == Phase::WAIT_ACCEPT)
  {
    // flex.js took the job (the first status read may already be a later one)
    m_phase = Phase::SWITCHING;
    m_phaseSince = now;
    const bool fullscreen = m_options.fullscreen;
    lock.unlock();
    SuspendAE(); // ALSA free for the TV player (D17)
    m_guiSoundsOff = true; // E3
    if (CServiceBroker::GetGUI())
      CServiceBroker::GetGUI()->GetAudioManager().Enable(false);
    if (fullscreen)
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN); // VideoPlayer.cpp:2096-2099
    lock.lock();
  }

  if (st.st == "esready" && !m_es && m_esMode)
    m_esStartPending = true; // the source is ours (repeated while playing: m_es exists)

  if (st.st == "switching" || st.st == "esready" || st.st == "preparing")
  {
    const Phase next = st.st == "switching" ? Phase::SWITCHING : Phase::PREPARING;
    if (m_phase != next && m_phase != Phase::PLAYING)
    {
      m_phase = next;
      m_phaseSince = now;
    }
    return true;
  }

  if (st.st == "playing" || st.st == "paused" || st.st == "buffering")
  {
    m_phase = Phase::PLAYING;
    m_caching = st.st == "buffering";
    if (m_seekTargetMs >= 0 && m_tvAck >= static_cast<int64_t>(m_seekSeq)) // acked once settled
    {
      m_seekTargetMs = -1;
      CServiceBroker::GetDataCacheCore().SetStateSeeking(false);
    }
    if (st.why == "noResume")
      NotifyOnce("noResume", "Datei nicht spulbar, Start von vorn");
    else if (st.why == "noSeek")
      NotifyOnce("noSeek", "Spulen ist bei dieser Datei nicht möglich");
    CheckAudioAccepted(); // D10
    if (!m_avStartedNotified)
    {
      m_avStartedNotified = true;
      m_revealAt = now + REVEAL_DELAY;
      m_lastResumeSave = now;
      const CFileItem item = m_item;
      IPlayerCallback* cb = &m_callback;
      Submit([cb, item]() { cb->OnAVStarted(item); });
    }
    const bool paused = st.st == "paused";
    if (paused != m_tvPaused)
    {
      m_tvPaused = paused;
      IPlayerCallback* cb = &m_callback;
      Submit([cb, paused]() {
        if (paused)
          cb->OnPlayBackPaused();
        else
          cb->OnPlayBackResumed();
      });
    }
    // the TV drops to normal speed at position 0/1 when rewinding (TA:224): follow it
    if (m_desired.speed >= 100100 && m_tvPosS <= 1)
      Change([](Desired& d) { d.speed = 100; });
    // until flex.js applied the last change, Kodi's own wish counts (no flicker in the OSD)
    const bool applied = m_tvAck >= static_cast<int64_t>(m_desired.seq);
    const bool showPaused = applied ? paused : m_desired.pause != 0;
    const float factor = applied ? SpeedFactor(m_tvSpd) : SpeedFactor(m_desired.speed);
    CServiceBroker::GetDataCacheCore().SetSpeed(1.0f, showPaused ? 0.0f : factor);
    return true;
  }

  if (st.st == "ended")
  {
    // D11, second guard: only a real end of file counts as "watched" (same 10 s as flex.js)
    const int64_t durS = TotalTimeMs() / 1000;
    const bool atEnd = m_eof || (durS > 0 && m_tvPosS >= durS - END_SLACK_S);
    SetEnd(atEnd ? End::ENDED : End::STOPPED);
    if (!atEnd)
      CLog::Log(LOGINFO, "CTVPlayer: \"ended\" at {} of {} s without end of file: stopped",
                m_tvPosS, durS);
    return false;
  }
  if (st.st == "stopped")
  {
    if (!m_avStartedNotified && !st.why.empty())
    {
      lock.unlock();
      NotifyOnce("stopped", "TV-Player gestoppt (" + st.why + ")");
      lock.lock();
    }
    SetEnd(End::STOPPED);
    return false;
  }
  if (st.st == "error")
  {
    if (!SetEnd(End::ERROR))
      return false; // stopped by Kodi meanwhile: no error dialog
    lock.unlock();
    Toast(CGUIDialogKaiToast::Error,
          StringUtils::Format("TV-Player: Fehler bei der Wiedergabe (Code {})", st.err), TOAST_MS);
    return false;
  }
  return true;
}

void CTVPlayer::FailBeforePlaying(const std::string& why, int err)
{
  Desired ctl;
  bool writeCtl = false;
  std::string path;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (m_end != End::NONE || m_avStartedNotified)
      return;
    path = m_item.GetDynPath();
    // flex.js may still be switching/preparing: stop that at once
    if (m_ctlWritten)
    {
      m_desired.stop = 1;
      ++m_desired.seq;
      ++m_desired.hb;
      ctl = m_desired;
      writeCtl = true;
    }
  }
  // first withdraw the job: flex.js must not take an abandoned one any more (REV:106-111)
  m_bridge->RemovePlay();
  if (writeCtl)
    m_bridge->WriteCtl(m_sid, ctl);

  if (why == "player")
  {
    std::unique_lock<std::mutex> lock(g_rejectedLock);
    g_rejected.insert(path);
  }
  if (why == "bad")
    CLog::Log(LOGERROR, "CTVPlayer: flex.js calls the job invalid, this is a bug in Kodi's side");

  bool esFailed = false;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    esFailed = m_esMode && (why == "es" || why == "prepTimeout" || why == "bad");
  }
  if (esFailed)
  {
    // the TV player next, and from there on the usual way; not this file on this path again
    CLog::Log(LOGINFO, "CTVPlayer: ES path failed ({}, code {}), the TV player takes over", why, err);
    {
      std::unique_lock<std::mutex> lock(g_rejectedLock);
      g_esRejected.insert(path);
    }
    RequestFallback("es", err);
    return;
  }

  const bool fallback = why != "dvp5" && FallbackAllowed();
  CLog::Log(LOGINFO, "CTVPlayer: not on the TV ({}{}), {}", why,
            err ? StringUtils::Format(", code {}", err) : std::string(),
            fallback ? "Kodi's player takes over" : "error message (E2)");
  if (fallback)
  {
    RequestFallback(why, err); // does nothing if CloseFile() came first
    return;
  }
  if (!SetEnd(End::ERROR))
    return; // stopped by the user meanwhile: no message, no error dialog
  Toast(CGUIDialogKaiToast::Error, ErrorText(why, err), TOAST_LONG_MS);
  m_wake.Set();
}

bool CTVPlayer::SetEnd(End end)
{
  // m_section is recursive: also called with it held (OnStatus)
  std::unique_lock<CCriticalSection> lock(m_section);
  if (m_end != End::NONE)
    return false;
  m_end = end;
  return true;
}

bool CTVPlayer::FallbackAllowed() const
{
  // only where Kodi's software decoder copes (RD:26, RD:58), otherwise an error message (E2)
  std::unique_lock<CCriticalSection> lock(m_section);
  if (!m_cfgFallback)
    return false;
  const TVVideo& v = m_probe.video;
  int width = v.width;
  int height = v.height;
  std::string codec = v.codecName;
  int bitDepth = v.bitDepth;
  int profile = v.profile;
  if (!v.valid && m_item.HasVideoInfoTag())
  {
    const CStreamDetails& details = m_item.GetVideoInfoTag()->m_streamDetails;
    width = details.GetVideoWidth();
    height = details.GetVideoHeight();
    codec = details.GetVideoCodec();
    bitDepth = 0;
    profile = -1;
  }
  if (width > m_cfgFallbackMaxWidth)
    return false;
  // HEVC Main 10 (FF_PROFILE_HEVC_MAIN_10 = 2)
  if (codec == "hevc" && (bitDepth > 8 || profile == 2))
    return false;
  if ((codec == "av1" || codec == "vp9") && (width > 1920 || height > 1088))
    return false;
  return true;
}

void CTVPlayer::RequestFallback(const std::string& why, int err)
{
  // One cancel mechanism: g_pendingFallback holds the generation of the only fallback that may
  // still start. It is set here and cleared by CloseFile() (stop, next item, shutdown) and
  // OpenFile() (new file on this object), all under m_section, so a stop can never fall between
  // the check and the publication. FallbackOnAppThread() takes it on the main thread right
  // before PlayFile().
  auto msg = std::make_unique<FallbackMsg>();
  std::string text;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (m_end != End::NONE || m_avStartedNotified)
      return; // a stop (or an error/fallback) came first: it stays
    m_end = End::FALLBACK;
    msg->item = m_item;
    msg->item.SetStartOffset(CUtil::ConvertSecsToMilliSecs(m_options.starttime)); // Application.cpp:2418
    if (m_options.startpercent > 0)
      msg->item.SetProperty("StartPercent", m_options.startpercent); // :2412-2416
    msg->generation = m_generation;
    if (why == "es")
    {
      msg->player = "TVPlayer";
      // same player object: OpenFile() follows on it, an end callback would overtake its start
      m_startedNotified = false;
    }
    text = FallbackText(why, err, StartSeconds());
    g_pendingFallback = m_generation;
  }
  m_bridge->RemovePlay();

  Toast(CGUIDialogKaiToast::Info, text, TOAST_MS);
  FallbackMsg* raw = msg.release(); // freed in FallbackOnAppThread
  raw->cb.callback = &CTVPlayer::FallbackOnAppThread;
  raw->cb.userptr = raw;
  CServiceBroker::GetAppMessenger()->PostMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&raw->cb));
  m_wake.Set();
}

void CTVPlayer::FallbackOnAppThread(void* userptr) // main thread (ApplicationMessenger.cpp:227-234)
{
  std::unique_ptr<FallbackMsg> msg(static_cast<FallbackMsg*>(userptr));
  // the last check, right before PlayFile(); CloseFile()/OpenFile() from CApplicationPlayer run
  // on this thread too, so a stop or another file either came before (CAS fails) or after
  uint64_t expected = msg->generation;
  if (!g_pendingFallback.compare_exchange_strong(expected, 0))
  {
    CLog::Log(LOGINFO, "CTVPlayer: fallback to VideoPlayer cancelled (stop or another file)");
    return;
  }
  CLog::Log(LOGINFO, "CTVPlayer: switching to {}", msg->player);
  // same path as ACTION_SWITCH_PLAYER (Application.cpp:1338-1352): playlist and stack stay
  g_application.PlayFile(msg->item, msg->player, true);
}

void CTVPlayer::SuspendAE()
{
  if (!m_cfgSuspendAE || m_aeSuspended)
    return;
  IAE* ae = CServiceBroker::GetActiveAE(); // ServiceBroker.h:190
  if (!ae)
    return;
  ae->Suspend(); // ExternalPlayer.cpp:296
  XbmcThreads::EndTime<> timer(AE_SUSPEND_WAIT);
  while (!timer.IsTimePast() && !ae->IsSuspended() && !m_bStop)
    CThread::Sleep(50ms); // ExternalPlayer.cpp:298-302
  m_aeSuspended = true;
  CLog::Log(LOGINFO, "CTVPlayer: audio engine suspended (ALSA free for the TV player)");
}

void CTVPlayer::ResumeAE()
{
  if (!m_aeSuspended.exchange(false))
    return;
  IAE* ae = CServiceBroker::GetActiveAE();
  if (ae && !ae->Resume())
    CLog::Log(LOGERROR, "CTVPlayer: audio engine did not resume");
}

void CTVPlayer::OnExit()
{
  // 1. opaque again before the TV player stops (FB:1119)
  m_revealed = false;
  CServiceBroker::GetDataCacheCore().SetVideoRender(false);

  // 2. the helper first: flex.js must not touch the source while the media item still exists
  std::unique_ptr<CTVEsEngine> es;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    es = std::move(m_es);
  }
  if (es)
    es->Stop();
  es.reset();

  // stop the TV player and withdraw the job
  Desired ctl;
  bool writeCtl = false;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    m_desired.stop = 1;
    ++m_desired.seq;
    ++m_desired.hb;
    ctl = m_desired;
    writeCtl = m_ctlWritten;
  }
  if (writeCtl)
    m_bridge->WriteCtl(m_sid, ctl);
  m_bridge->RemovePlay();

  // 3. HTTP server off, waiting at most 2 s (D14)
  std::shared_ptr<CTVHttpFeeder> feeder;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    feeder = std::move(m_feeder);
  }
  if (feeder)
    feeder->Stop();
  feeder.reset();

  // 4. ALSA back (ExternalPlayer.cpp:384-389)
  ResumeAE();

  // 5. resume point and exactly one end callback, in this order (VideoPlayer.cpp:2556-2597)
  CBookmark bm;
  End end = End::STOPPED;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    end = m_end == End::NONE ? End::STOPPED : m_end.load();
    m_end = end;
    const int64_t totalMs = TotalTimeMs();
    bm.totalTimeInSeconds = totalMs / 1000.0;
    bm.timeInSeconds = 0.0;
    if (m_avStartedNotified && end != End::FALLBACK && totalMs > 0)
    {
      // ENDED only with evidence (D11, OnStatus), otherwise the real position
      int64_t timeMs = end == End::ENDED ? totalMs : CurrentTimeMs();
      // started at 0 although there was a resume point (noResume): keep the old one (REV:154)
      if (m_noResume && timeMs / 1000 < m_resumeOrigS)
        timeMs = 0;
      bm.timeInSeconds = timeMs / 1000.0;
    }
    bm.player = m_name;
    m_phase = Phase::IDLE;
  }
  const bool notify = m_startedNotified.exchange(false);
  m_guiSoundsOff = false; // PlaybackCleanup switches them on again
  m_playing = false; // before ENDED/STOPPED reach the GUI (Application.cpp:2614)

  CLog::Log(LOGINFO, "CTVPlayer::OnExit - {} end {} at {:.0f} of {:.0f} s", m_sid,
            end == End::ENDED     ? "ended"
            : end == End::ERROR   ? "error"
            : end == End::FALLBACK ? "fallback"
                                   : "stopped",
            bm.timeInSeconds, bm.totalTimeInSeconds);

  const CFileItem item = m_item;
  IPlayerCallback* cb = &m_callback;
  Submit([cb, item, bm, end, notify]() {
    if (bm.timeInSeconds > 0.0 && bm.totalTimeInSeconds > 0.0)
      cb->OnPlayerCloseFile(item, bm); // ApplicationPlayerCallback.cpp:94-166
    if (!notify)
      return;
    switch (end)
    {
      case End::ENDED:
        cb->OnPlayBackEnded();
        break;
      case End::ERROR:
        cb->OnPlayBackError(); // calls OnPlayBackStopped() itself
        break;
      default:
        cb->OnPlayBackStopped(); // STOPPED, FALLBACK
        break;
    }
  });
}

bool CTVPlayer::PeriodicResumeAllowed(int64_t posMs, int64_t totalMs) const
{
  // m_section is held
  if (posMs <= 0 || totalMs <= 0)
    return false;
  if (m_noResume && posMs / 1000 < m_resumeOrigS)
    return false; // keeps the old resume point
  // never count as "watched" in between, never delete the resume point near the end
  const auto& as = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  const double percent = 100.0 * static_cast<double>(posMs) / static_cast<double>(totalMs);
  const double playCountPercent = static_cast<double>(as->m_videoPlayCountMinimumPercent);
  if (playCountPercent > 0.0 && percent >= playCountPercent)
    return false;
  if (100.0 - percent < static_cast<double>(as->m_videoIgnorePercentAtEnd))
    return false;
  return true;
}

void CTVPlayer::SavePeriodicResume()
{
  // E4: an intermediate OnPlayerCloseFile every 5 min, against crashes/kill -9 (KI:570-574)
  if (!m_cfgPeriodicResume || !m_avStartedNotified)
    return;
  CBookmark bm;
  CFileItem item;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    const auto now = std::chrono::steady_clock::now();
    if (m_phase != Phase::PLAYING || now - m_lastResumeSave < RESUME_SAVE_INTERVAL)
      return;
    m_lastResumeSave = now;
    const int64_t posMs = CurrentTimeMs();
    const int64_t totalMs = TotalTimeMs();
    if (posMs / 1000 == m_lastResumeSaveMs / 1000 || !PeriodicResumeAllowed(posMs, totalMs))
      return; // paused since the last save, or not allowed
    m_lastResumeSaveMs = posMs;
    bm.timeInSeconds = posMs / 1000.0;
    bm.totalTimeInSeconds = totalMs / 1000.0;
    bm.player = m_name;
    item = m_item;
  }
  CLog::Log(LOGDEBUG, "CTVPlayer: resume point {:.0f} s saved in between", bm.timeInSeconds);
  IPlayerCallback* cb = &m_callback;
  Submit([cb, item, bm]() { cb->OnPlayerCloseFile(item, bm); });
}

void CTVPlayer::MainThreadPong(void*) // app thread
{
  s_mainAliveAt = SteadyMs(); // touches no player object -> a late pong is harmless
}

void CTVPlayer::PingMainThread() // TVPlayer thread, every 2 s
{
  static ThreadMessageCallback cb{&CTVPlayer::MainThreadPong, nullptr}; // static: never freed
  CServiceBroker::GetAppMessenger()->PostMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&cb));
}

bool CTVPlayer::MainThreadAlive() const
{
  return SteadyMs() - s_mainAliveAt < MAIN_ALIVE_MS;
}

void CTVPlayer::NotifyOnce(const std::string& key, const std::string& text)
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (!m_notified.insert(key).second)
      return;
  }
  Toast(CGUIDialogKaiToast::Info, text, TOAST_MS);
}

void CTVPlayer::CheckAudioAccepted()
{
  // m_section is held. D10: flex.js reads [31] after a change for up to 5 s and acknowledges
  // the change only after that (REV:180)
  if (!m_audioCheckPending || m_tvAck < static_cast<int64_t>(m_audioSeq) || m_audioIdx < 0 ||
      m_audioIdx >= static_cast<int>(m_probe.audio.size()))
    return;
  m_audioCheckPending = false;
  const TVTrack& track = m_probe.audio[m_audioIdx];
  if (TvAcceptedAudio(track.codec, m_tvAcodec))
    return;

  CLog::Log(LOGINFO, "CTVPlayer: the TV refused audio track {} ({}), it reports '{}'", m_audioIdx,
            track.codec, m_tvAcodec);
  Toast(CGUIDialogKaiToast::Info, "TV: Tonformat nicht unterstützt", TOAST_MS);
  // send the previous track actively again (REV:181)
  if (m_audioPrevIdx >= 0 && m_audioPrevIdx != m_audioIdx &&
      m_audioPrevIdx < static_cast<int>(m_probe.audio.size()))
  {
    m_audioIdx = m_audioPrevIdx;
    m_audioPrevIdx = -1;
    m_desired.audio = m_probe.audio[m_audioIdx].tvIndex;
    m_audioSeq = ++m_desired.seq;
    m_audioCheckPending = true;
    m_wake.Set();
    CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  }
}

void CTVPlayer::Change(const std::function<void(Desired&)>& f)
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    f(m_desired);
    ++m_desired.seq;
  }
  m_wake.Set();
}

void CTVPlayer::Submit(std::function<void()> f)
{
  m_outboundEvents->Submit(std::move(f)); // utils/JobManager.h:119
}

void CTVPlayer::UpdatePlayTimes()
{
  CServiceBroker::GetDataCacheCore().SetPlayTimes(0, CurrentTimeMs(), 0, TotalTimeMs());
}

int64_t CTVPlayer::CurrentTimeMs() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (m_seekTargetMs >= 0)
    return m_seekTargetMs;
  int64_t ms = m_tvPosMs >= 0 ? m_tvPosMs : m_tvPosS * 1000;
  // counted on from Kodi's own receive time, not flex.js' wall clock (REV:36), at most 10 s
  if (m_avStartedNotified && !m_tvPaused && !m_caching &&
      m_tvRecvAt != std::chrono::steady_clock::time_point{})
  {
    const int64_t since = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - m_tvRecvAt)
                              .count();
    ms += static_cast<int64_t>(
        static_cast<double>(std::clamp<int64_t>(since, 0, MAX_EXTRAPOLATION_MS)) *
        static_cast<double>(SpeedFactor(m_tvSpd)));
  }
  const int64_t total = TotalTimeMs();
  ms = std::max<int64_t>(ms, 0);
  return total > 0 ? std::min(ms, total) : ms;
}

int64_t CTVPlayer::TotalTimeMs() const
{
  // m_section is recursive (CCriticalSection)
  std::unique_lock<CCriticalSection> lock(m_section);
  const int64_t tv = m_tvDurS * 1000;
  const int64_t demuxer = m_probe.durationMs;
  // the TV reports whole seconds (RD:365); the demuxer wins if they differ by more than 2 %
  if (tv > 0 && demuxer > 0 && std::llabs(tv - demuxer) * 50 > demuxer)
  {
    if (!m_durationLogged)
      CLog::Log(LOGINFO, "CTVPlayer: duration TV {} s, file {} s: using the file's", tv / 1000,
                demuxer / 1000);
    m_durationLogged = true;
    return demuxer;
  }
  if (tv > 0)
    return tv;
  if (demuxer > 0)
    return demuxer;
  if (m_item.HasVideoInfoTag())
    return static_cast<int64_t>(m_item.GetVideoInfoTag()->GetDuration()) * 1000;
  return 0;
}

bool CTVPlayer::HasAudio() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return !m_probe.valid || !m_probe.audio.empty();
}

bool CTVPlayer::CanSeek() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_seekable && m_phase == Phase::PLAYING;
}

bool CTVPlayer::IsCaching() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_caching;
}

void CTVPlayer::Pause()
{
  bool paused = false;
  Change([&paused](Desired& d) {
    d.pause = d.pause ? 0 : 1;
    paused = d.pause != 0;
    d.speed = 100; // like hiMedia on PLAY/PAUSE while winding (OAPP:26809-26816)
  });
  CServiceBroker::GetDataCacheCore().SetSpeed(1.0f, paused ? 0.0f : 1.0f); // UPnPPlayer.cpp:473-494
  // OnPlayBackPaused/Resumed follow from the confirmed "paused"/"playing" status
}

void CTVPlayer::SeekTo(int64_t ms, int acc)
{
  if (!CanSeek())
  {
    bool playing = false;
    {
      std::unique_lock<CCriticalSection> lock(m_section);
      playing = m_phase == Phase::PLAYING;
    }
    // while starting nothing can be sought yet; while playing the TV says "not seekable"
    if (playing)
      NotifyOnce("noSeek", "Spulen ist bei dieser Datei nicht möglich");
    return;
  }
  const int64_t before = CurrentTimeMs();
  const int64_t total = TotalTimeMs();
  int64_t target = std::max<int64_t>(ms, 0);
  if (total > 1000)
    target = std::min<int64_t>(target, total - 1000);
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    ++m_desired.seekN;
    m_desired.seekTo = static_cast<int>(target / 1000);
    m_desired.seekAcc = acc;
    m_desired.speed = 100;
    m_seekSeq = ++m_desired.seq;
    m_seekTargetMs = target;
    m_seekSince = std::chrono::steady_clock::now();
  }
  m_wake.Set();
  CServiceBroker::GetDataCacheCore().SetStateSeeking(true);
  IPlayerCallback* cb = &m_callback;
  Submit([cb, target, before]() { cb->OnPlayBackSeek(target, target - before); });
}

void CTVPlayer::SeekTime(int64_t iTime)
{
  SeekTo(iTime, 100); // slider, chapter, time entry: exact
}

bool CTVPlayer::SeekTimeRelative(int64_t iTime)
{
  SeekTo(CurrentTimeMs() + iTime, 0); // small/large steps: fast, like hiMedia un()
  return true;
}

void CTVPlayer::SeekPercentage(float fPercent)
{
  const int64_t total = TotalTimeMs();
  if (total > 0)
    SeekTo(static_cast<int64_t>(static_cast<double>(total) * static_cast<double>(fPercent) / 100.0),
           100);
}

void CTVPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride) // VideoPlayer.cpp:3184-3251
{
  if (bLargeStep && bChapterOverride && GetChapter() > 0 && GetChapterCount() > 1)
  {
    if (!bPlus)
    {
      SeekChapter(PreviousChapter());
      return;
    }
    if (GetChapter() < GetChapterCount())
    {
      SeekChapter(GetChapter() + 1);
      return;
    }
  }
  const auto& as = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  const int64_t current = CurrentTimeMs();
  const int64_t total = TotalTimeMs();
  int64_t target = 0;
  if (as->m_videoUseTimeSeeking && total > 2000LL * as->m_videoTimeSeekForwardBig)
    target = current + 1000LL * (bLargeStep ? (bPlus ? as->m_videoTimeSeekForwardBig
                                                     : as->m_videoTimeSeekBackwardBig)
                                            : (bPlus ? as->m_videoTimeSeekForward
                                                     : as->m_videoTimeSeekBackward));
  else
    target = current + total * (bLargeStep ? (bPlus ? as->m_videoPercentSeekForwardBig
                                                    : as->m_videoPercentSeekBackwardBig)
                                           : (bPlus ? as->m_videoPercentSeekForward
                                                    : as->m_videoPercentSeekBackward)) /
                           100;
  SeekTo(std::max<int64_t>(0, target), 0);
}

void CTVPlayer::SetSpeed(float speed)
{
  bool paused = false;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    paused = m_desired.pause != 0;
  }
  if (speed == 0.0f || speed == 1.0f)
  {
    if (speed == 0.0f ? !paused : paused)
      Pause();
    else if (TrickPlaySwitch())
    {
      Change([](Desired& d) { d.speed = 100; });
      CServiceBroker::GetDataCacheCore().SetSpeed(1.0f, paused ? 0.0f : 1.0f);
    }
    return;
  }

  if (!TrickPlaySwitch())
  {
    // P2: the TV's speed (D21) did not wind in the test; Kodi's replacement jump, 30 s per press.
    // The data cache keeps speed 1, so every press of FF/RW gives one more jump.
    SeekTimeRelative(speed > 0.0f ? REPLACEMENT_JUMP_MS : -REPLACEMENT_JUMP_MS);
    return;
  }

  // FLEX_KODI_TRICKPLAY=1: like hiMedia videoSpeedFF/FR (setMpctrlFlag, only if seekable)
  if (!CanSeek())
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (m_phase == Phase::PLAYING) // while starting nothing can be wound yet
      NotifyOnce("noSeek", "Spulen ist bei dieser Datei nicht möglich");
    return;
  }
  if (paused)
    return;
  const int value = SpeedValue(speed); // Kodi's 32x becomes 16x
  Change([value](Desired& d) { d.speed = value; });
  CServiceBroker::GetDataCacheCore().SetSpeed(1.0f, SpeedFactor(value));
}

void CTVPlayer::AddSubtitle(const std::string& strSubPath)
{
  // the TV player gets no subtitles afterwards (setExternalTitleUri is never used, TA:288)
  CLog::Log(LOGINFO, "CTVPlayer: subtitle {} cannot be given to the TV",
            CURL::GetRedacted(strSubPath));
  Toast(CGUIDialogKaiToast::Info,
        FallbackAllowed() ? "Untertitel nur mit Kodis Player: Abspielen mit... -> VideoPlayer"
                          : "Nachträgliche Untertitel sind bei 4K nicht möglich",
        TOAST_LONG_MS);
}

bool CTVPlayer::OnAction(const CAction& action)
{
  switch (action.GetID())
  {
    case ACTION_NEXT_ITEM: // VideoPlayer.cpp:4518-4528
      if (GetChapter() > 0 && GetChapter() < GetChapterCount())
      {
        SeekChapter(GetChapter() + 1);
        return true;
      }
      return false;
    case ACTION_PREV_ITEM: // VideoPlayer.cpp:4529-4538
      if (GetChapter() > 0)
      {
        SeekChapter(PreviousChapter());
        return true;
      }
      return false;
    case ACTION_VOLUME_UP:
    case ACTION_VOLUME_DOWN:
    case ACTION_MUTE:
      // only without tvkeys (EvdevInputHandler): Kodi's ALSA is released, its volume does nothing
      if (m_aeSuspended)
      {
        NotifyOnce("volume", "Lautstärke bitte mit der TV- bzw. AVR-Fernbedienung");
        return true;
      }
      return false;
    default:
      return false;
  }
}

void CTVPlayer::Render(bool clear, uint32_t alpha, bool gui) // OV:585-592
{
  // CGUIWindowFullScreen never clears without a video layer: until the picture is on the
  // plane, opaque black instead of old frame-buffer contents
  if (clear && gui && !m_revealed)
    CServiceBroker::GetWinSystem()->GetGfxContext().Clear(0xFF000000);
}

int CTVPlayer::GetAudioStreamCount() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return static_cast<int>(m_probe.audio.size());
}

int CTVPlayer::GetAudioStream()
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_probe.audio.empty() ? -1 : m_audioIdx;
}

void CTVPlayer::SetAudioStream(int iStream)
{
  std::string refused;
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (iStream < 0 || iStream >= static_cast<int>(m_probe.audio.size()) || iStream == m_audioIdx)
      return;
    const TVTrack& track = m_probe.audio[iStream];
    if (!track.tvSupported || track.tvIndex < 0)
    {
      // D10: TrueHD/FLAC/Opus/Vorbis never go to the TV (TrueHD would stay silent)
      refused = track.codec;
      StringUtils::ToUpper(refused);
    }
    else
    {
      m_audioPrevIdx = m_audioIdx;
      m_audioIdx = iStream;
      m_desired.audio = track.tvIndex;
      m_audioSeq = ++m_desired.seq;
      m_audioCheckPending = true;
      CServiceBroker::GetDataCacheCore().SetAudioDecoderName(
          "TV " + track.codec + (track.converted ? " -> AC3" : ""));
      CServiceBroker::GetDataCacheCore().SetAudioChannels(std::to_string(track.channels));
    }
  }
  if (!refused.empty())
  {
    Toast(CGUIDialogKaiToast::Info,
          "TV kann " + refused + " nicht abspielen" +
              (FallbackAllowed() ? ": für diese Spur Abspielen mit... -> VideoPlayer" : ""),
          TOAST_LONG_MS);
    return;
  }
  m_wake.Set();
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
}

void CTVPlayer::GetAudioStreamInfo(int index, AudioStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (index == CURRENT_STREAM)
    index = m_audioIdx;
  if (index < 0 || index >= static_cast<int>(m_probe.audio.size()))
    return;
  const TVTrack& t = m_probe.audio[index];
  info.valid = true;
  info.language = t.language;
  info.name = !t.tvSupported ? t.name + " (TV: nicht unterstützt)"
              : t.converted ? t.name + " (als Dolby Digital)"
                            : t.name;
  info.codecName = t.codec;
  info.bitrate = t.bitRate;
  info.channels = t.channels;
  info.samplerate = t.sampleRate;
  info.bitspersample = t.bitsPerSample;
  info.flags = t.flags;
}

int CTVPlayer::GetSubtitleCount() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return static_cast<int>(m_probe.subtitles.size());
}

int CTVPlayer::GetSubtitle()
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_probe.subtitles.empty() ? -1 : m_subIdx;
}

void CTVPlayer::GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (index == CURRENT_STREAM)
    index = m_subIdx;
  if (index < 0 || index >= static_cast<int>(m_probe.subtitles.size()))
    return;
  const TVTrack& t = m_probe.subtitles[index];
  info.valid = true;
  info.language = t.language;
  info.name = t.name;
  info.codecName = t.codec;
  info.flags = t.flags;
}

void CTVPlayer::SetSubtitle(int iStream)
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (iStream < 0 || iStream >= static_cast<int>(m_probe.subtitles.size()))
      return;
    if (m_esMode)
    {
      lock.unlock();
      NotifyOnce("esSub", "Untertitel gibt es in echter Bildrate noch nicht");
      return;
    }
    m_subIdx = iStream;
    if (m_subVisible)
    {
      m_desired.sub = m_probe.subtitles[iStream].tvIndex; // setMpCtrlSubtitleNo(i) (TA:278)
      ++m_desired.seq;
    }
  }
  m_wake.Set();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

bool CTVPlayer::GetSubtitleVisible() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_subVisible;
}

void CTVPlayer::SetSubtitleVisible(bool bVisible)
{
  {
    std::unique_lock<CCriticalSection> lock(m_section);
    if (m_esMode)
    {
      lock.unlock();
      if (bVisible)
        NotifyOnce("esSub", "Untertitel gibt es in echter Bildrate noch nicht");
      return;
    }
    if (bVisible && m_subIdx < 0 && !m_probe.subtitles.empty())
      m_subIdx = 0;
    m_subVisible = bVisible && m_subIdx >= 0;
    m_desired.sub = m_subVisible ? m_probe.subtitles[m_subIdx].tvIndex : -1;
    ++m_desired.seq;
  }
  m_wake.Set();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

int CTVPlayer::GetVideoStream() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_probe.video.valid ? 0 : -1;
}

int CTVPlayer::GetVideoStreamCount() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return m_probe.video.valid ? 1 : 0;
}

void CTVPlayer::GetVideoStreamInfo(int streamId, VideoStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (!m_probe.video.valid || (streamId != CURRENT_STREAM && streamId != 0))
    return;
  const TVVideo& v = m_probe.video;
  info.valid = true;
  info.width = v.width;
  info.height = v.height;
  info.videoAspectRatio = v.videoAspectRatio;
  info.codecName = v.codecName;
  info.hdrType = v.hdrType;
  info.stereoMode = v.stereoMode;
  info.SrcRect = CRect(0.0f, 0.0f, static_cast<float>(v.width), static_cast<float>(v.height));
}

int CTVPlayer::GetChapterCount() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  return static_cast<int>(m_probe.chapters.size());
}

int CTVPlayer::GetChapter() const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  const int64_t s = CurrentTimeMs() / 1000;
  int chapter = 0;
  for (size_t i = 0; i < m_probe.chapters.size(); ++i)
  {
    if (s >= m_probe.chapters[i].second)
      chapter = static_cast<int>(i) + 1; // 1-based like VideoPlayer.cpp:4610-4614
  }
  return chapter;
}

void CTVPlayer::GetChapterName(std::string& strChapterName, int chapterIdx) const
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (chapterIdx == -1)
    chapterIdx = GetChapter();
  if (chapterIdx > 0 && chapterIdx <= static_cast<int>(m_probe.chapters.size()))
    strChapterName = m_probe.chapters[chapterIdx - 1].first;
}

int64_t CTVPlayer::GetChapterPos(int chapterIdx) const // seconds (DVDDemuxFFmpeg.cpp:2061-2071)
{
  std::unique_lock<CCriticalSection> lock(m_section);
  if (chapterIdx == -1)
    chapterIdx = GetChapter();
  if (chapterIdx > 0 && chapterIdx <= static_cast<int>(m_probe.chapters.size()))
    return m_probe.chapters[chapterIdx - 1].second;
  return -1;
}

int CTVPlayer::PreviousChapter() const // like VideoPlayer.cpp:4651-4661 (5 s grace)
{
  const int chapter = GetChapter();
  return (chapter > 0 && CurrentTimeMs() < (GetChapterPos(chapter) + 5) * 1000) ? chapter - 1
                                                                                  : chapter;
}

int CTVPlayer::SeekChapter(int iChapter)
{
  const int64_t pos = GetChapterPos(std::max(1, iChapter));
  if (pos >= 0)
  {
    SeekTo(pos * 1000, 100);
    IPlayerCallback* cb = &m_callback;
    Submit([cb, iChapter]() { cb->OnPlayBackSeekChapter(iChapter); });
  }
  return 0;
}

void CTVPlayer::GetAudioCapabilities(std::vector<int>& caps) const
{
  caps.assign(1, IPC_AUD_SELECT_STREAM); // no delay, amplification, stereo downmix
}

void CTVPlayer::GetSubtitleCapabilities(std::vector<int>& caps) const
{
  caps.assign(1, IPC_SUBS_SELECT); // no offset, no download into the TV player
}
