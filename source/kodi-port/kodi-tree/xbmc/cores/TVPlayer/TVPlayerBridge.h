/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

namespace TVPLAYER
{

/*!
 * \brief Desired state for flex.js, written as "ctl" (section 3.4 of TVPLAYER-DESIGN.md).
 *
 * A desired state instead of a command stream: a missed intermediate ctl loses nothing, a
 * repeated one has no effect.
 */
struct TVCtl
{
  uint32_t seq = 0; //!< ++ on every change of content, never smaller
  uint32_t hb = 0; //!< ++ on every write, but only while Kodi's main thread is alive
  int pause = 0; //!< 0 play, 1 pause (absolute)
  uint32_t seekN = 0; //!< ++ per jump
  int seekTo = 0; //!< s
  int seekAcc = 0; //!< 0 fast (relative steps), 100 exact (slider, chapter, time entry)
  int speed = 100; //!< 100; 200..1600 forward, 100200..101600 back (FLEX_KODI_TRICKPLAY=1 only)
  int audio = -1; //!< index for setAudioTrack, -1 unchanged; only tvSupported tracks
  int sub = -2; //!< n-th internal subtitle, -1 off, -2 unchanged
  int stop = 0; //!< 1 = end the playback
};

/*!
 * \brief Actual state reported by flex.js in "player.json" (section 3.5), already checked.
 */
struct TVStatus
{
  std::string sid;
  //! switching|esready|preparing|playing|buffering|paused|ended|stopped|error|refused
  std::string st;
  std::string why; //!< reason from the positive list, otherwise ""
  std::string acodec; //!< getMetadata()[31], [A-Za-z0-9 ._+-], at most 32 characters
  std::string win; //!< video window "XxYxWxH" or "" for the full picture (log only)
  int64_t n = 0; //!< write counter per sid, starts at 1
  int64_t ack = -1; //!< highest completely applied ctl.seq
  int mp = -1; //!< raw getMpCtrlStat[2]
  int flags = 0; //!< raw getMpCtrlStat[5]
  int err = 0; //!< error code ([8] or state)
  int64_t pos = 0; //!< s
  int64_t posMs = -1; //!< exact position, only from CTVEsEngine
  int64_t dur = 0; //!< s
  bool seek = false; //!< 1 & getMpCtrlStat[4]
  bool eof = false; //!< [5] == 4 seen
  int aud = -1; //!< index last given to setAudioTrack
  int spd = 100; //!< value last given to setMpctrlFlag
};

/*!
 * \brief Acknowledgement of "tvkeys" by flex.js ("tvack", section 3.6a).
 */
struct TVAck
{
  std::string kid;
  int64_t seq = -1; //!< highest tvkeys.seq received
  bool arc = false; //!< audio goes over HDMI ARC (getSpeakerArc() == 2)
  int vol = -1; //!< UI.store.state.VolumeBar.volume after applying, -1 unknown
  bool mute = false;
};

/*!
 * \brief Kodi's side of the play.json / ctl / player.json protocol for one CTVPlayer.
 *
 * JSON is written with CJSONVariantWriter and read with CJSONVariantParser; everything read is
 * checked against the positive lists and ranges of section 3.5, anything else is ignored (and
 * logged once per playback).
 */
class CTVBridge
{
public:
  //! new playback: forget the last n and the last raw text
  void Reset();

  bool WritePlay(const std::string& sid,
                 uint16_t port,
                 const std::string& tok,
                 const std::string& name,
                 int64_t resumeS,
                 float dar,
                 int ov,
                 int fb);
  //! job for the ES path: flex.js only provides the source, CTVEsEngine plays (no port, no token)
  bool WritePlayEs(const std::string& sid);
  bool WriteCtl(const std::string& sid, const TVCtl& ctl);

  /*!
   * \param maxAck the current ctl.seq (ack must not be larger)
   * \return true for a new, valid status of this sid (n larger than before)
   */
  bool ReadPlayer(const std::string& sid, int64_t maxAck, TVStatus& out);

  //! withdraws the job (flex.js must not accept an abandoned one)
  void RemovePlay();

private:
  int64_t m_lastN = 0;
  std::string m_lastJson;
  bool m_invalidLogged = false;
};

/*!
 * \brief Writes "tvkeys" (Kodi -> flex.js, section 3.6): counters that only grow.
 */
bool WriteTvKeys(const std::string& kid, uint32_t seq, uint32_t up, uint32_t down, uint32_t mute);

/*!
 * \brief Reads "tvack" (flex.js -> Kodi).
 * \return true if a valid acknowledgement for this kid is there
 */
bool ReadTvAck(const std::string& kid, TVAck& out);

} // namespace TVPLAYER
