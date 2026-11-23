/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  TVPlayer: hands video files to the TV's built-in media player (Hisense VIDAA / MStar, launcher
 *  API modeljs.mpctrl, driven by the launcher mod flex.js through a signed file bridge).
 */

#pragma once

#include "FileItem.h"
#include "TVPlayerBridge.h"
#include "TVPlayerProbe.h"
#include "cores/IPlayer.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class CJobQueue;

namespace TVPLAYER
{
class CTVEsEngine;
class CTVHttpFeeder;

/*!
 * \brief Player core "tv": Kodi stays the user interface, the TV plays the file.
 *
 * - OpenFile() does not block: it starts the player thread and reports OnPlayBackStarted (Kodi's
 *   busy spinner), unless the file is rejected at once (then the VideoPlayer takes over).
 * - The thread probes the file (tracks, display aspect ratio, chapters), starts a local HTTP
 *   server for it (CTVHttpFeeder) and writes ctl + play.json for flex.js. flex.js switches the
 *   TV to the source "DMP", plays http://127.0.0.1:<port>/<token>/media.<ext> and reports
 *   player.json; Kodi follows it (OnAVStarted once "playing", times, pause, end).
 * - Every operation of Kodi's GUI changes a desired state (ctl) that the thread writes.
 * - About 1 s after "playing" the GUI turns transparent over the TV's video plane
 *   (CDataCacheCore::SetVideoRender, IsRenderingVideoLayer() -> the DirectFB window system).
 * - Refused or failed before playing: back to Kodi's VideoPlayer at the same position (or an
 *   error message for 4K that Kodi's software cannot decode, E2).
 * - Exactly one end callback per OpenFile with OnPlayBackStarted, none without it.
 * - ES path (m_esMode): where the file allows it, flex.js only provides the source DMP, and
 *   CTVEsEngine plays with the content's real frame rate instead of the TV player's 60 Hz. If it
 *   fails before the picture, the TV player takes over, and after that the VideoPlayer as above.
 *
 * Design: docs/kodi/TVPLAYER-DESIGN.md (sections 2 to 4, 6), ES path:
 * docs/kodi/MULTIDECODE-RECHERCHE.md (section 7h).
 */
class CTVPlayer : public IPlayer, public CThread
{
public:
  explicit CTVPlayer(IPlayerCallback& callback);
  ~CTVPlayer() override;

  bool Initialize(TiXmlElement* pConfig) override; // always true (KI:84)
  bool OpenFile(const CFileItem& file, const CPlayerOptions& options) override;
  bool QueueNextFile(const CFileItem& file) override { return false; }
  bool CloseFile(bool reopen = false) override;
  bool IsPlaying() const override { return m_playing; }
  bool CanPause() const override { return true; }
  void Pause() override;
  bool HasVideo() const override { return true; }
  bool HasAudio() const override;
  bool CanSeek() const override;
  void Seek(bool bPlus = true, bool bLargeStep = false, bool bChapterOverride = false) override;
  void SeekPercentage(float fPercent = 0) override;
  void SeekTime(int64_t iTime = 0) override;
  bool SeekTimeRelative(int64_t iTime) override;
  void SetSpeed(float speed) override; // +-30 s per press; FLEX_KODI_TRICKPLAY=1: 2..16x
  void AddSubtitle(const std::string& strSubPath) override; // hint only (4.3)
  bool IsCaching() const override;
  int GetCacheLevel() const override { return -1; }
  bool OnAction(const CAction& action) override; // chapters (KI:422-423)

  int GetAudioStreamCount() const override;
  int GetAudioStream() override;
  void SetAudioStream(int iStream) override;
  void GetAudioStreamInfo(int index, AudioStreamInfo& info) const override;
  int GetSubtitleCount() const override;
  int GetSubtitle() override;
  void GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const override;
  void SetSubtitle(int iStream) override;
  bool GetSubtitleVisible() const override;
  void SetSubtitleVisible(bool bVisible) override;
  int GetVideoStream() const override;
  int GetVideoStreamCount() const override;
  void GetVideoStreamInfo(int streamId, VideoStreamInfo& info) const override;
  int GetChapterCount() const override;
  int GetChapter() const override;
  void GetChapterName(std::string& strChapterName, int chapterIdx = -1) const override;
  int64_t GetChapterPos(int chapterIdx = -1) const override;
  int SeekChapter(int iChapter) override;
  void GetAudioCapabilities(std::vector<int>& caps) const override; // {IPC_AUD_SELECT_STREAM}
  void GetSubtitleCapabilities(std::vector<int>& caps) const override; // {IPC_SUBS_SELECT}

  void Render(bool clear, uint32_t alpha = 255, bool gui = true) override; // black until reveal
  bool IsRenderingVideo() const override { return m_revealed; }

protected:
  void Process() override;
  void OnExit() override;

private:
  enum class Phase
  {
    IDLE,
    PREPARE,
    WAIT_ACCEPT,
    SWITCHING,
    PREPARING,
    PLAYING
  };
  enum class End
  {
    NONE,
    ENDED,
    STOPPED,
    ERROR,
    FALLBACK
  };

  using Desired = TVCtl; // == ctl (section 3.4)

  bool RejectImmediately(const CFileItem& item, std::string& why) const;
  bool Prepare(std::string& why);
  /*!
   * \brief May this file take the ES path?
   * \param[out] esProbe tracks as this path offers them: tvIndex = the file's audio index
   * \param[out] esAudio start choice in esProbe
   * \param[out] subtitlesDropped yes, although subtitles were wanted: their audio language plays
   *             only on this path (converted)
   * \return "" for yes, otherwise why not (for the log)
   */
  std::string CheckEs(const std::string& path,
                      const TVProbe& probe,
                      int audio,
                      const TVSubtitleChoice& sub,
                      TVProbe& esProbe,
                      int& esAudio,
                      bool& subtitlesDropped) const;
  bool StartEs();
  bool OnStatus(const TVStatus& st); // false: leave Process()
  const char* PhaseTimedOut(std::chrono::steady_clock::time_point now) const;
  void FailBeforePlaying(const std::string& why, int err = 0); // fallback or error (E2)
  //! the first end wins: false if m_end was already set (stop by the user, fallback ...)
  bool SetEnd(End end);
  //! only from End::NONE; publishes g_pendingFallback in the same critical section in which
  //! CloseFile()/OpenFile() cancel it
  void RequestFallback(const std::string& why, int err = 0);
  //! app thread: starts VideoPlayer only if this generation is still the pending fallback
  static void FallbackOnAppThread(void* userptr);
  bool FallbackAllowed() const; // <fallback>, E2 (width/codec)
  void SeekTo(int64_t ms, int acc);
  void SuspendAE();
  void ResumeAE();
  void Change(const std::function<void(Desired&)>& f);
  void Submit(std::function<void()> f); // ordered callbacks (VideoPlayer.cpp:616)
  int64_t CurrentTimeMs() const;
  int64_t TotalTimeMs() const;
  void UpdatePlayTimes();
  void ResetSession(); // new sid (16 hex), generation, clears state
  void PingMainThread(); // every 2 s: TMSG_CALLBACK -> s_mainAliveAt
  static void MainThreadPong(void* userptr); // runs on the app thread
  bool MainThreadAlive() const; // pong younger than 5 s
  void NotifyOnce(const std::string& key, const std::string& text);
  void CheckAudioAccepted();
  void SavePeriodicResume();
  int PreviousChapter() const;
  int64_t StartSeconds() const;
  bool PeriodicResumeAllowed(int64_t posMs, int64_t totalMs) const;

  // <player> options (playercorefactory.xml, 4.9)
  bool m_cfgFallback = true;
  int m_cfgFallbackMaxWidth = 1920; // E2: wider -> error instead of fallback
  bool m_cfgSuspendAE = true;
  bool m_cfgProbe = true;
  bool m_cfgPeriodicResume = true; // E4: save the resume point every 5 min
  bool m_cfgAllowDvProfile5 = false; // 4.8, only for P14
  int m_cfgPeerUid = 2910; // 4.7: uid of the TV player (P18), -1 log only
  int m_cfgFeederConnections = 16;
  int m_cfgFeederTimeoutS = 3600; // 4.7, P11
  bool m_cfgFileCache = false; // after P14
  bool m_cfgEs = true; // <es>: real frame rate through CTVEsEngine where the file allows it
  // <audioindex>: how ctl.audio counts (TVPlayerProbe.h). P3 is open; default "supported" after
  // the owner's hearing test (Matrix file, RD:469-470), "file" matches the synthetic file RD:370
  TVAudioIndex m_cfgAudioIndex = TVAudioIndex::SUPPORTED;

  mutable CCriticalSection m_section;
  CFileItem m_item;
  CPlayerOptions m_options;
  std::string m_sid;
  uint64_t m_generation = 0;
  TVProbe m_probe;
  Desired m_desired;
  bool m_ctlWritten = false;
  Phase m_phase = Phase::IDLE;
  std::chrono::steady_clock::time_point m_phaseSince{};
  std::atomic<End> m_end{End::NONE};
  bool m_tvPaused = false, m_seekable = false, m_caching = false, m_eof = false;
  int64_t m_tvPosS = 0, m_tvDurS = 0; // last status
  int64_t m_tvPosMs = -1; // exact position, only on the ES path
  bool m_esMode = false, m_esStartPending = false;
  std::chrono::steady_clock::time_point m_tvRecvAt{}; // when it was read (not flex.js' wall clock t)
  int64_t m_tvAck = -1;
  int m_tvAud = -1, m_tvSpd = 100;
  bool m_noResume = false; // started at 0 although a resume point existed
  int64_t m_resumeOrigS = 0; // resume point at OpenFile (4.4)
  std::string m_tvAcodec;
  int64_t m_seekTargetMs = -1;
  uint32_t m_seekSeq = 0;
  std::chrono::steady_clock::time_point m_seekSince{};
  int m_audioIdx = -1, m_audioPrevIdx = -1;
  uint32_t m_audioSeq = 0;
  bool m_audioCheckPending = false;
  int m_subIdx = -1;
  bool m_subVisible = false;
  std::chrono::steady_clock::time_point m_revealAt{};
  std::chrono::steady_clock::time_point m_lastResumeSave{};
  int64_t m_lastResumeSaveMs = -1;
  std::set<std::string> m_notified;
  std::string m_lastStateKey; // st/why of the last status, for the log
  mutable bool m_durationLogged = false;

  std::atomic<bool> m_playing{false};
  std::atomic<bool> m_startedNotified{false};
  std::atomic<bool> m_avStartedNotified{false};
  std::atomic<bool> m_revealed{false};
  std::atomic<bool> m_aeSuspended{false};
  std::atomic<bool> m_guiSoundsOff{false};

  CEvent m_wake;
  std::unique_ptr<CTVBridge> m_bridge;
  std::shared_ptr<CTVHttpFeeder> m_feeder; // shared: may outlive us while stopping (D14)
  std::unique_ptr<CTVEsEngine> m_es;
  std::unique_ptr<CJobQueue> m_outboundEvents;
};
} // namespace TVPLAYER
