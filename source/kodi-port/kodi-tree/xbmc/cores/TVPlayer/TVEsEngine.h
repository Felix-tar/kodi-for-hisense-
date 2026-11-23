/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "FileItem.h"
#include "TVEsProtocol.h"
#include "TVPlayerBridge.h"
#include "TVPlayerProbe.h"
#include "threads/Thread.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

class CBitstreamConverter;
class CDVDDemux;
class CDVDInputStream;
struct DemuxPacket;

namespace TVPLAYER
{

class CTVAudioTranscoder;

//! ST_MEDIA_INFO values for a track; 0 = this path cannot play it
uint32_t EsVideoCode(const TVVideo& video);
//! the TV takes the track as it is (AC-3, E-AC-3); 0 = it does not
uint32_t EsAudioCode(const std::string& codec);
//! not taken as it is, but Kodi can turn it into AC-3 for the TV (CTVAudioTranscoder)
bool EsConvertsAudio(const TVTrack& track);
//! what the media item gets for this track: its own code, AC-3 when converted, 0 = no audio
uint32_t EsAudioOutCode(const TVTrack& track);
//! frame rate in mHz for ST_MEDIA_INFO +0x5c; 0 = unknown
uint32_t EsFrameRate(float fps);

/*!
 * \brief Plays a file with the content's real frame rate: Kodi's demuxer reads it, the helper
 *        process native/flex_es_helper.c hands the packets to the TV's media service (ES push).
 *
 * Only that helper loads the vendor libraries, so Kodi stays free of their C++ runtime. If Kodi
 * dies, the helper sees the end of its control socket and tears the media item down: the service
 * does not restart after a crash, and apps would have no video until the TV reboots.
 *
 * Seen from CTVPlayer this replaces flex.js' player: Apply() takes the same desired state, and
 * GetStatus() answers with the same status (docs/kodi/MULTIDECODE-RECHERCHE.md section 7h).
 */
class CTVEsEngine : public CThread
{
public:
  struct Params
  {
    CFileItem item;
    int audioIndex = -1; //!< n-th audio track of the file, -1 = none
    uint32_t vcodec = 0;
    uint32_t acodec = 0;
    uint32_t fpsMhz = 0;
    float fps = 0.0f;
    int64_t startMs = 0;
    float dar = 0.0f;
    int64_t durationMs = 0;
    std::vector<TVTrack> audioTracks;
  };

  CTVEsEngine();
  ~CTVEsEngine() override;

  //! starts the helper and the demuxer thread; false = the helper could not be started
  bool Start(const Params& params);
  //! the desired state of CTVPlayer: pause, seek and audio track; stop is Stop()
  void Apply(const TVCtl& ctl);
  /*!
   * \brief Latest state of the helper as a status.
   * \return true if there is a new one
   */
  bool GetStatus(TVStatus& out);
  //! Tears the helper down cleanly, at most a few seconds. Never uses SIGKILL: see the .cpp
  void Stop();

protected:
  void Process() override;

private:
  bool SpawnHelper();
  void ReapHelper();
  bool OpenSource();
  void HandleRequests();
  void SendVideo(uint8_t* data, int size, double pts, double dts);
  void SendVc1(uint8_t* data, int size, double pts);
  void FlushVc1Group();
  void SendPacket(uint32_t type, const uint8_t* data, size_t size, double ptsUs);
  bool WriteAll(const void* data, size_t size);
  void SendCmd(uint32_t op, uint32_t gen, uint32_t flags, int x = 0, int y = 0, unsigned w = 0, unsigned h = 0);
  void SendWindow();
  void BeginSeek(int64_t ms, bool newItem);
  //! converter for the current audio track if the TV does not take it as it is (demuxer thread)
  void SetupAudio(int index);
  void SendAudio(DemuxPacket* packet);

  Params m_params;
  int m_dataFd = -1;
  int m_ctlFd = -1;
  pid_t m_helper = -1;

  std::shared_ptr<CDVDInputStream> m_input;
  std::unique_ptr<CDVDDemux> m_demux;
  std::unique_ptr<CBitstreamConverter> m_converter;
  int m_videoId = -1;
  int m_audioId = -1;
  int64_t m_demuxerId = -1;
  std::vector<int> m_audioIds;
  std::vector<uint8_t> m_videoExtra;
  bool m_extraPending = false;
  std::unique_ptr<CTVAudioTranscoder> m_transcoder;
  bool m_transcodeLogged = false;

  // demuxer thread: position in the current generation
  uint32_t m_gen = 1;
  bool m_waitKey = false;
  double m_keyPtsUs = -1.0;
  double m_lastVideoPtsUs = -1.0;
  bool m_eosSent = false;

  // VC-1 from Matroska has no PTS (MULTIDECODE-RECHERCHE section 7g): anchor plus following B frames
  struct Vc1Frame
  {
    std::vector<uint8_t> data;
    double stampUs;
    bool bFrame;
  };
  std::vector<Vc1Frame> m_vc1Group;
  bool m_vc1Vfw = false;
  bool m_vc1Known = false;
  bool m_vc1SkipLeading = true;

  // requests from the player thread
  std::atomic<int64_t> m_seekReqMs{-1};
  std::atomic<int> m_audioReq{-1};
  std::atomic<uint32_t> m_applied{0};
  std::atomic<uint32_t> m_seekSeq{0};
  std::atomic<uint32_t> m_seekGen{0};

  // player thread
  TVCtl m_lastCtl;
  bool m_haveCtl = false;
  es_status m_status{};
  int m_curAudio = -1;
  std::chrono::steady_clock::time_point m_seekSince{};
};

} // namespace TVPLAYER
