/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVPlayerProbe.h"

#include "FileItem.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemux.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDFactoryDemuxer.h"
#include "cores/VideoPlayer/DVDInputStreams/DVDFactoryInputStream.h"
#include "cores/VideoPlayer/DVDInputStreams/DVDInputStream.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/LangCodeExpander.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <memory>

extern "C"
{
#include <libavformat/avformat.h> // AV_DISPOSITION_ATTACHED_PIC
}

using namespace TVPLAYER;

namespace
{
// like VideoPlayer.cpp:77-81: return if lh and rh differ in this criterion
#define TV_PREDICATE_RETURN(lh, rh) \
  do \
  { \
    if ((lh) != (rh)) \
      return (lh) > (rh); \
  } while (0)

// FFmpeg H.264 profiles High 10, High 4:2:2, High 4:4:4 Predictive (FF_PROFILE_H264_*)
constexpr int H264_HIGH_10 = 110;
constexpr int H264_HIGH_422 = 122;
constexpr int H264_HIGH_444_PREDICTIVE = 244;

// wider/higher than this counts as "above 1080p" (1088 = coded height of 1080p)
constexpr int HD_WIDTH = 1920;
constexpr int HD_HEIGHT = 1088;

bool IsTvAudioCodec(const std::string& codec)
{
  // the TV refuses FLAC and reports TrueHD as "UNKNOWN" and stays silent (RD:370-380);
  // Opus/Vorbis are not in its format list [assumption until P3]
  return codec != "truehd" && codec != "mlp" && codec != "flac" && codec != "opus" &&
         codec != "vorbis";
}

bool IsImageSubtitle(const std::string& codec)
{
  return codec == "dvd_subtitle" || codec == "hdmv_pgs_subtitle" || codec == "dvb_subtitle" ||
         codec == "xsub";
}

bool IsCommentary(const TVTrack& t)
{
  return (t.flags & StreamFlags::FLAG_COMMENT) != 0 ||
         StringUtils::Contains(t.name, "commentary") ||
         StringUtils::Contains(t.name, "kommentar");
}

int CodecRank(const TVTrack& t)
{
  // what HDMI ARC carries best to the AVR: E-AC3 JOC > E-AC3 > AC3 > DTS > AAC > MPEG
  if (t.codec == "eac3")
    return t.atmos ? 6 : 5;
  if (t.codec == "ac3")
    return 4;
  if (t.codec == "dca" || StringUtils::StartsWith(t.codec, "dts"))
    return 3;
  if (t.codec == "aac" || t.codec == "aac_latm")
    return 2;
  if (t.codec == "mp3" || t.codec == "mp2" || t.codec == "mp1")
    return 1;
  return 0;
}

/*!
 * \brief Dolby Vision whose base layer the TV can show without the DV layer.
 *
 * dv_bl_signal_compatibility_id (Dolby's profiles spec): 0 none (profile 5), 1 HDR10 (8.1,
 * 10.1), 2 SDR (8.2, 4, 9), 4 HLG (8.4), 6 UHD Blu-ray HDR10 (7); 3, 5, 7..15 reserved. Kodi's
 * Android rule {0, 2, 3} (DVDVideoCodecAndroidMediaCodec.cpp:704-706) answers "no HDR10
 * fallback" and would wrongly refuse 8.2 with its SDR base, so only "no or unknown base" counts.
 */
bool HasDvBaseLayer(const AVDOVIDecoderConfigurationRecord& dovi)
{
  if (dovi.dv_profile == 5)
    return false;
  const int id = dovi.dv_bl_signal_compatibility_id;
  return id == 1 || id == 2 || id == 4 || id == 6;
}

int ChannelTier(int channels)
{
  return channels >= 6 ? 2 : (channels > 2 ? 1 : 0);
}

bool SameLanguage(const std::string& a, const std::string& b)
{
  return !a.empty() && !b.empty() && g_LangCodeExpander.CompareISO639Codes(a, b);
}

struct SubtitleCandidate
{
  int index; // internal index or -1
  bool external;
  std::string language;
  int flags;
};

//! audio codec family for comparing Kodi's codec with the TV's getMetadata()[31] text
std::string CodecFamily(const std::string& codec)
{
  if (codec == "eac3")
    return "eac3";
  if (codec == "ac3")
    return "ac3";
  if (codec == "dca" || StringUtils::StartsWith(codec, "dts"))
    return "dts";
  if (codec == "aac" || codec == "aac_latm")
    return "aac";
  if (codec == "mp3" || codec == "mp2" || codec == "mp1")
    return "mpeg";
  if (StringUtils::StartsWith(codec, "pcm"))
    return "pcm";
  return "";
}

std::string TextFamily(const std::string& text)
{
  // "Dolby Digital Plus", "DOLBY_DIGITAL_PLUS", "DTS_HD", "AAC" ... (RD:372-379, TA:333)
  std::string t = text;
  StringUtils::ToUpper(t);
  if (t.find("UNKNOWN") != std::string::npos)
    return "unknown";
  if (t.find("PLUS") != std::string::npos || t.find("EAC3") != std::string::npos ||
      t.find("E-AC3") != std::string::npos || t.find("DDP") != std::string::npos)
    return "eac3";
  if (t.find("DOLBY") != std::string::npos || t.find("AC3") != std::string::npos)
    return "ac3";
  if (t.find("DTS") != std::string::npos)
    return "dts";
  if (t.find("AAC") != std::string::npos)
    return "aac";
  if (t.find("MPEG") != std::string::npos || t.find("MP3") != std::string::npos ||
      t.find("MP2") != std::string::npos)
    return "mpeg";
  if (t.find("PCM") != std::string::npos)
    return "pcm";
  return "";
}
} // namespace

bool TVPLAYER::ProbeStreams(const CFileItem& source, TVProbe& out, TVAudioIndex audioIndex)
{
  // like CDVDFileInfo::GetFileStreamDetails (DVDFileInfo.cpp:281-328)
  CFileItem item(source.GetDynPath(), false);
  item.SetMimeTypeForInternetFile();
  std::shared_ptr<CDVDInputStream> input = CDVDFactoryInputStream::CreateInputStream(nullptr, item);
  if (!input || input->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) ||
      input->IsStreamType(DVDSTREAM_TYPE_DVD) || !input->Open())
    return false;

  std::unique_ptr<CDVDDemux> demux(CDVDFactoryDemuxer::CreateDemuxer(input, true));
  if (!demux)
    return false;

  TVProbe probe;
  probe.durationMs = demux->GetStreamLength();
  int supportedAudio = 0;
  for (CDemuxStream* s : demux->GetStreams())
  {
    if (!s)
      continue;
    std::string codec = demux->GetStreamCodecName(s->demuxerId, s->uniqueId);
    if (codec.empty())
      codec = avcodec_get_name(s->codec);

    if (s->type == STREAM_VIDEO && !(s->flags & AV_DISPOSITION_ATTACHED_PIC) && !probe.video.valid)
    {
      const CDemuxStreamVideo* v = static_cast<const CDemuxStreamVideo*>(s);
      TVVideo& video = probe.video;
      video.valid = true;
      video.width = v->iWidth;
      video.height = v->iHeight;
      video.videoAspectRatio = static_cast<float>(v->fAspect);
      if (video.videoAspectRatio <= 0.0f && v->iHeight > 0)
        video.videoAspectRatio = static_cast<float>(v->iWidth) / v->iHeight;
      video.codecName = codec;
      video.profile = v->profile;
      video.bitDepth = v->bitDepth;
      video.hdrType = v->hdr_type;
      // profile 5 (no base layer) is refused; 7 and 8.x play their HDR10/SDR/HLG base layer
      if (v->hdr_type == StreamHdrType::HDR_TYPE_DOLBYVISION)
      {
        video.dolbyVisionWithoutBase = !HasDvBaseLayer(v->dovi);
        CLog::Log(LOGINFO, "TVPlayer probe: Dolby Vision profile {} level {}, base layer {} ({})",
                  static_cast<int>(v->dovi.dv_profile), static_cast<int>(v->dovi.dv_level),
                  static_cast<int>(v->dovi.dv_bl_signal_compatibility_id),
                  video.dolbyVisionWithoutBase ? "none, refused" : "the TV plays it");
      }
      video.stereoMode = v->stereo_mode;
      video.fps = v->iFpsScale ? static_cast<float>(v->iFpsRate) / v->iFpsScale : 0.0f;
      video.interlaced = v->interlaced;
    }
    else if (s->type == STREAM_AUDIO)
    {
      const CDemuxStreamAudio* a = static_cast<const CDemuxStreamAudio*>(s);
      TVTrack t;
      t.language = s->language;
      t.name = s->name;
      t.codec = codec;
      t.codecId = s->codec;
      t.profile = s->profile;
      t.channels = a->iChannels;
      t.sampleRate = a->iSampleRate;
      t.bitsPerSample = a->iBitsPerSample;
      t.bitRate = a->iBitRate;
      t.flags = s->flags;
      t.tvSupported = IsTvAudioCodec(codec);
      t.atmos = codec == "eac3" && (StringUtils::Contains(t.name, "atmos") ||
#ifdef FF_PROFILE_EAC3_DDP_ATMOS
                                    s->profile == FF_PROFILE_EAC3_DDP_ATMOS ||
#endif
                                    StringUtils::Contains(s->codecName, "JOC"));
      // setAudioTrack(n), P3 still open, the two measurements disagree: the owner's hearing test
      // with the Matrix file (RD:469-470: eng AC3, TrueHD, eng AC3, deu AC3, spa AC3;
      // setAudioTrack(3) = Spanish) fits SUPPORTED, i.e. TrueHD etc. not counted; the synthetic
      // file (RD:370: index 3 = TrueHD "UNKNOWN") fits FILE. Default SUPPORTED, switch with
      // <audioindex>file</audioindex>. Unsupported tracks are never sent (D10).
      if (audioIndex == TVAudioIndex::FILE)
        t.tvIndex = static_cast<int>(probe.audio.size());
      else
        t.tvIndex = t.tvSupported ? supportedAudio : -1;
      if (t.tvSupported)
        ++supportedAudio;
      probe.audio.push_back(std::move(t));
    }
    else if (s->type == STREAM_SUBTITLE)
    {
      TVTrack t;
      t.tvIndex = static_cast<int>(probe.subtitles.size()); // n-th internal subtitle (TA:278)
      t.language = s->language;
      t.name = s->name;
      t.codec = codec;
      t.flags = s->flags;
      t.image = IsImageSubtitle(codec);
      probe.subtitles.push_back(std::move(t));
    }
  }

  const int chapters = demux->GetChapterCount();
  for (int i = 1; i <= chapters; ++i) // 1-based, positions in s (DVDDemuxFFmpeg.cpp:2061-2071)
  {
    std::string name;
    demux->GetChapterName(name, i);
    probe.chapters.emplace_back(name, demux->GetChapterPos(i));
  }

  probe.valid = true;
  out = std::move(probe);
  return true;
}

int TVPLAYER::ChooseAudio(const TVProbe& probe)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string langSetting = settings->GetString(CSettings::SETTING_LOCALE_AUDIOLANGUAGE);
  const bool preferDefault = settings->GetBool(CSettings::SETTING_VIDEOPLAYER_PREFERDEFAULTFLAG);
  const bool hearingImpaired = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_AUDIOHEARING);
  const bool visuallyImpaired = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_AUDIOVISUAL);

  // step A: Kodi's language rule (VideoPlayer.cpp:171-183), only among the tracks the TV plays
  std::vector<int> candidates;
  for (size_t i = 0; i < probe.audio.size(); ++i)
  {
    if (probe.audio[i].tvSupported && probe.audio[i].tvIndex >= 0)
      candidates.push_back(static_cast<int>(i));
  }
  if (candidates.empty())
    return -1;

  if (!StringUtils::EqualsNoCase(langSetting, "mediadefault"))
  {
    std::vector<int> language;
    const bool original = StringUtils::EqualsNoCase(langSetting, "original");
    const std::string audioLanguage = g_langInfo.GetAudioLanguage();
    for (const int i : candidates)
    {
      const TVTrack& t = probe.audio[i];
      if (original ? (t.flags & StreamFlags::FLAG_ORIGINAL) != 0
                   : SameLanguage(audioLanguage, t.language))
        language.push_back(i);
    }
    // the language only exists in a format the TV cannot play: another language then
    if (!language.empty())
      candidates = std::move(language);
  }

  // step B: best codec for HDMI ARC (KA:497-509)
  auto better = [&](const TVTrack& lh, const TVTrack& rh) {
    const bool lhHi = (lh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0;
    const bool rhHi = (rh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0;
    TV_PREDICATE_RETURN(hearingImpaired ? lhHi : !lhHi, hearingImpaired ? rhHi : !rhHi);
    const bool lhVi = (lh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED) != 0;
    const bool rhVi = (rh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED) != 0;
    TV_PREDICATE_RETURN(visuallyImpaired ? lhVi : !lhVi, visuallyImpaired ? rhVi : !rhVi);
    TV_PREDICATE_RETURN(!IsCommentary(lh), !IsCommentary(rh));
    // the file's default track before channels and codec, like Kodi's PredicateAudioFilter (17.11.2026:
    // with "mediadefault" a Spanish AC-3 track beat the file's own DTS tracks)
    if (preferDefault)
      TV_PREDICATE_RETURN((lh.flags & StreamFlags::FLAG_DEFAULT) != 0,
                          (rh.flags & StreamFlags::FLAG_DEFAULT) != 0);
    TV_PREDICATE_RETURN(ChannelTier(lh.channels), ChannelTier(rh.channels));
    // ES path: a track the TV takes as it is before one Kodi has to convert
    TV_PREDICATE_RETURN(!lh.converted, !rh.converted);
    TV_PREDICATE_RETURN(CodecRank(lh), CodecRank(rh));
    TV_PREDICATE_RETURN(lh.channels, rh.channels);
    TV_PREDICATE_RETURN(lh.bitRate, rh.bitRate);
    return false; // equal: the earlier track stays
  };

  int best = candidates.front();
  for (const int i : candidates)
  {
    if (better(probe.audio[i], probe.audio[best]))
      best = i;
  }
  return best;
}

TVSubtitleChoice TVPLAYER::ChooseSubtitle(const TVProbe& probe,
                                          const std::string& audioLanguage,
                                          const std::vector<std::string>& externalLanguages)
{
  TVSubtitleChoice choice;

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string subtitleSetting =
      settings->GetString(CSettings::SETTING_LOCALE_SUBTITLELANGUAGE);
  const bool original = StringUtils::EqualsNoCase(subtitleSetting, "original");
  const bool noSub = StringUtils::EqualsNoCase(subtitleSetting, "none");
  const bool onlyForced = StringUtils::EqualsNoCase(subtitleSetting, "forced_only");
  const std::string subtitleLanguage = g_langInfo.GetSubtitleLanguage();
  const bool hearingImpaired = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_SUBHEARING);
  const bool subsOn = CMediaSettings::GetInstance().GetDefaultVideoSettings().m_SubtitleOn;

  std::vector<SubtitleCandidate> list;
  for (size_t i = 0; i < probe.subtitles.size(); ++i)
    list.push_back({static_cast<int>(i), false, probe.subtitles[i].language,
                    static_cast<int>(probe.subtitles[i].flags)});
  for (const std::string& language : externalLanguages)
    list.push_back({-1, true, language, 0});
  if (list.empty())
    return choice;

  // PredicateSubtitleFilter (VideoPlayer.cpp:83-150): true = relevant
  auto relevant = [&](const SubtitleCandidate& s) {
    const bool forced = (s.flags & StreamFlags::FLAG_FORCED) != 0;
    const bool def = (s.flags & StreamFlags::FLAG_DEFAULT) != 0;
    if (noSub)
      return false;
    if (onlyForced)
      return forced && SameLanguage(s.language, audioLanguage);
    if (s.external)
      return true;
    if (forced && SameLanguage(s.language, audioLanguage))
      return true;
    if (forced && def)
      return true;
    if (s.language == "cc" && (s.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0)
      return true;
    if (!original)
      return SameLanguage(subtitleLanguage, s.language);
    return def;
  };

  // PredicateSubtitlePriority (VideoPlayer.cpp:230-303): lh better than rh
  auto better = [&](const SubtitleCandidate& lh, const SubtitleCandidate& rh) {
    TV_PREDICATE_RETURN(relevant(lh), relevant(rh));
    TV_PREDICATE_RETURN(lh.external, rh.external);
    const bool lhForced = (lh.flags & StreamFlags::FLAG_FORCED) != 0;
    const bool rhForced = (rh.flags & StreamFlags::FLAG_FORCED) != 0;
    const bool lhDefault = (lh.flags & StreamFlags::FLAG_DEFAULT) != 0;
    const bool rhDefault = (rh.flags & StreamFlags::FLAG_DEFAULT) != 0;
    if (!subsOn || original)
    {
      TV_PREDICATE_RETURN(lhForced && SameLanguage(lh.language, audioLanguage),
                          rhForced && SameLanguage(rh.language, audioLanguage));
      TV_PREDICATE_RETURN(lhDefault && SameLanguage(lh.language, audioLanguage),
                          rhDefault && SameLanguage(rh.language, audioLanguage));
      TV_PREDICATE_RETURN(SameLanguage(lh.language, audioLanguage),
                          SameLanguage(rh.language, audioLanguage));
      TV_PREDICATE_RETURN(lhForced && lhDefault, rhForced && rhDefault);
    }
    if (!original)
    {
      TV_PREDICATE_RETURN(lh.external && SameLanguage(subtitleLanguage, lh.language),
                          rh.external && SameLanguage(subtitleLanguage, rh.language));
      TV_PREDICATE_RETURN(SameLanguage(subtitleLanguage, lh.language),
                          SameLanguage(subtitleLanguage, rh.language));
      const bool lhHi = (lh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0;
      const bool rhHi = (rh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0;
      TV_PREDICATE_RETURN(hearingImpaired ? lhHi : !lhHi, hearingImpaired ? rhHi : !rhHi);
    }
    TV_PREDICATE_RETURN(lhDefault, rhDefault);
    return false;
  };

  size_t best = 0;
  for (size_t i = 1; i < list.size(); ++i)
  {
    if (better(list[i], list[best]))
      best = i;
  }

  // like CVideoPlayer::OpenDefaultStreams (VideoPlayer.cpp:928-950)
  const SubtitleCandidate& chosen = list[best];
  bool visible = subsOn;
  if (!relevant(chosen))
    visible = false;
  else if ((chosen.flags & StreamFlags::FLAG_FORCED) != 0)
    visible = true;

  choice.index = chosen.index;
  choice.visible = visible;
  choice.external = chosen.external;
  choice.externalLanguage = chosen.external ? chosen.language : std::string();
  return choice;
}

TVCanPlay TVPLAYER::TvCanPlay(const TVProbe& probe,
                              bool allowDvProfile5,
                              std::string& why,
                              std::vector<std::string>& notices)
{
  const TVVideo& v = probe.video;
  const bool aboveHd = v.width > HD_WIDTH || v.height > HD_HEIGHT;

  // 1. AV1: the TV's decoder has no AV1 (RD:317)
  if (v.valid && v.codecName == "av1")
  {
    why = "av1";
    return TVCanPlay::NO;
  }

  // 2. Dolby Vision without a base layer the TV can show (profile 5, or a reserved
  //    compatibility id): wrong colours on the TV (RD:337). 7/8.1/8.2/8.4 play the base layer.
  if (v.valid && v.dolbyVisionWithoutBase && !allowDvProfile5)
  {
    why = "dvp5";
    return TVCanPlay::NEVER;
  }

  // 3. H.264 with more than 8 bit or 4:2:2/4:4:4 ("Hi10P"): up to 1080p Kodi's player
  if (v.valid && v.codecName == "h264" &&
      (v.bitDepth > 8 || v.profile == H264_HIGH_10 || v.profile == H264_HIGH_422 ||
       v.profile == H264_HIGH_444_PREDICTIVE))
  {
    why = "hi10p";
    return TVCanPlay::NO;
  }

  // 4./5. no audio track the TV can play (TrueHD/FLAC only)
  bool anySupported = false;
  for (const TVTrack& t : probe.audio)
    anySupported = anySupported || (t.tvSupported && t.tvIndex >= 0);
  if (!probe.audio.empty() && !anySupported)
  {
    if (!aboveHd)
    {
      // Kodi's player can do SD/HD in software (KI:522-527)
      why = "noTvAudio";
      return TVCanPlay::NO;
    }
    // 4K: Kodi's software cannot play it either (E2): picture without sound, said loudly
    std::string codec = probe.audio.front().codec;
    StringUtils::ToUpper(codec);
    notices.push_back(StringUtils::Format("Nur {}-Ton: der TV spielt das Bild ohne Ton", codec));
  }

  return TVCanPlay::YES;
}

bool TVPLAYER::SameAudioLanguage(const std::string& a, const std::string& b)
{
  return SameLanguage(a, b);
}

bool TVPLAYER::TvAcceptedAudio(const std::string& codec, const std::string& tvCodecText)
{
  if (tvCodecText.empty())
    return true; // nothing read: no verdict
  const std::string text = TextFamily(tvCodecText);
  if (text == "unknown")
    return false; // TrueHD-like: accepted by the API, but silent (RD:377)
  const std::string expected = CodecFamily(codec);
  // only a clear contradiction counts as refused [assumption until P3]
  return text.empty() || expected.empty() || text == expected;
}
