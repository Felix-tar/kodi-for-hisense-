/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Interface/StreamInfo.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CFileItem;

namespace TVPLAYER
{

//! one audio or subtitle track of the file, in file order
struct TVTrack
{
  int tvIndex = -1; //!< index handed to the TV (ctl.audio / ctl.sub), -1 = cannot be selected
  std::string language;
  std::string name;
  std::string codec; //!< FFmpeg codec name (dca/dtshd_ma for DTS, as Kodi's demuxer reports it)
  int codecId = 0; //!< AVCodecID (for the conversion to AC-3 on the ES path)
  int profile = -1;
  int channels = 0;
  int sampleRate = 0;
  int bitsPerSample = 0;
  int bitRate = 0;
  StreamFlags flags = StreamFlags::FLAG_NONE;
  bool atmos = false; //!< E-AC3 with JOC
  bool tvSupported = true; //!< the TV can decode/pass it (not: truehd, flac, opus, vorbis)
  bool converted = false; //!< ES path: Kodi turns it into AC-3 (CTVAudioTranscoder)
  bool image = false; //!< bitmap subtitle (VobSub, PGS, DVB)
};

struct TVVideo
{
  bool valid = false;
  int width = 0;
  int height = 0;
  float videoAspectRatio = 0.0f; //!< display aspect ratio (fAspect) -> play.json "dar"
  std::string codecName;
  int profile = -1;
  int bitDepth = 0;
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  //! DV profile 5, or bl_signal_compatibility_id not 1/2/4/6 (no HDR10/SDR/HLG base layer)
  bool dolbyVisionWithoutBase = false;
  std::string stereoMode;
  float fps = 0.0f;
  bool interlaced = false;
};

struct TVProbe
{
  bool valid = false;
  int64_t durationMs = 0;
  TVVideo video;
  std::vector<TVTrack> audio;
  std::vector<TVTrack> subtitles;
  std::vector<std::pair<std::string, int64_t>> chapters; //!< name, start in s (1-based like Kodi)
};

//! how the TV counts audio tracks for setAudioTrack(n) (<audioindex>, P3 open)
enum class TVAudioIndex
{
  FILE, //!< "file": n-th audio track of the file (synthetic file RD:370: 3 = TrueHD)
  //! "supported" (default): n-th track the TV can play, TrueHD/MLP/FLAC/Opus/Vorbis not counted
  //! (owner's hearing test, Matrix file RD:469-470: setAudioTrack(3) = 4th supported = Spanish)
  SUPPORTED,
};

/*!
 * \brief Opens the file briefly with Kodi's demuxer (like CDVDFileInfo::GetFileStreamDetails) and
 *        lists video, audio and subtitle tracks in file order plus the chapters.
 */
bool ProbeStreams(const CFileItem& source, TVProbe& out, TVAudioIndex audioIndex);

//! Kodi's language rule (like PredicateAudioFilter), then the best codec over ARC; -1 = none
int ChooseAudio(const TVProbe& probe);

struct TVSubtitleChoice
{
  int index = -1; //!< internal subtitle (index into TVProbe::subtitles) or -1
  bool visible = false;
  bool external = false; //!< Kodi's rule picks an external file (the TV cannot show it, P3)
  std::string externalLanguage;
};

/*!
 * \brief Kodi's subtitle rule (like PredicateSubtitleFilter/-Priority of VideoPlayer) for the
 *        start choice; external files take part like in VideoPlayer.
 * \param audioLanguage language of the chosen audio track
 */
TVSubtitleChoice ChooseSubtitle(const TVProbe& probe,
                                const std::string& audioLanguage,
                                const std::vector<std::string>& externalLanguages);

enum class TVCanPlay
{
  YES,
  NO, //!< not on the TV: fallback (or error, E2)
  NEVER, //!< not on the TV and not with Kodi's player either: error message
};

/*!
 * \brief Checks before the start whether the TV should get the file (section 4.8).
 * \param[out] why reason for NO/NEVER
 * \param[out] notices hints to show before the start (YES)
 */
TVCanPlay TvCanPlay(const TVProbe& probe,
                    bool allowDvProfile5,
                    std::string& why,
                    std::vector<std::string>& notices);

//! Kodi's language comparison (ISO 639 codes and names); empty never matches
bool SameAudioLanguage(const std::string& a, const std::string& b);

//! "Dolby Digital" etc. from getMetadata()[31] fits the codec; false = the TV refused the track
bool TvAcceptedAudio(const std::string& codec, const std::string& tvCodecText);

} // namespace TVPLAYER
