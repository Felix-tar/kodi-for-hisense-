/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVEsEngine.h"

#include "TVAudioTranscoder.h"

#include "cores/VideoPlayer/DVDDemuxers/DVDDemux.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxUtils.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDFactoryDemuxer.h"
#include "cores/VideoPlayer/DVDInputStreams/DVDFactoryInputStream.h"
#include "cores/VideoPlayer/DVDInputStreams/DVDInputStream.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/BitstreamConverter.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C"
{
#include <libavformat/avformat.h> // AV_DISPOSITION_ATTACHED_PIC
}

extern char** environ;

using namespace TVPLAYER;
using namespace std::chrono_literals;

namespace
{
// tools/es_run.sh: the vendor libraries depend on each other in a circle, so dlopen alone fails
constexpr const char* HELPER_LIBRARY_PATH =
    "LD_LIBRARY_PATH=/tvservice/mslib:/tvservice/conflictlib:/mslib/utopia:/mslib";
constexpr const char* HELPER_PRELOAD =
    "LD_PRELOAD=libmi_glibc.so libmdroid.so libmmapinfo.so libiniparser.so libdrvIPAUTH.so "
    "liblinux.so libmsrv_mmplayer_ipc.so";
constexpr const char* HELPER_LOG = "/tmp/flexkodi.d/es-helper.log";

constexpr int PANEL_W = 3840;
constexpr int PANEL_H = 2160;
constexpr auto SEEK_ACK_TIMEOUT = 5s;
constexpr int64_t SEEK_LAND_SLACK_MS = 15000; // key frames can be this far before the target
constexpr size_t MAX_KEY_SEARCH_PACKETS = 2000; // about a minute: then play on without a key frame

const uint8_t* FindStartCode(const uint8_t* p, const uint8_t* end)
{
  for (; p + 3 < end; ++p)
  {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
      return p;
  }
  return nullptr;
}

//! key frame in a stream that carries start codes; only the headers matter, 4 KiB are plenty
bool IsKeyFrame(uint32_t vcodec, const uint8_t* data, size_t size)
{
  const uint8_t* end = data + std::min<size_t>(size, 4096);
  for (const uint8_t* p = FindStartCode(data, end); p; p = FindStartCode(p + 3, end))
  {
    const uint8_t b = p[3];
    switch (vcodec)
    {
      case 7: // H.264: IDR or SPS (open GOP streams have no IDR)
        if ((b & 0x1f) == 5 || (b & 0x1f) == 7)
          return true;
        break;
      case 0x14: // HEVC: IRAP or VPS/SPS
      {
        const int type = (b >> 1) & 0x3f;
        if ((type >= 16 && type <= 21) || type == 32 || type == 33)
          return true;
        break;
      }
      case 12: // VC-1: sequence header or entry point
        if (b == 0x0f || b == 0x0e)
          return true;
        break;
      case 1: // MPEG-2: sequence header
        if (b == 0xb3)
          return true;
        break;
      default:
        return true;
    }
  }
  return false;
}

enum class Vc1Picture
{
  ANCHOR,
  B_FRAME,
  UNKNOWN
};

//! picture type from the first frame start code (Advanced Profile, progressive): es_pack.py
Vc1Picture Vc1Type(const uint8_t* data, size_t size)
{
  const uint8_t* end = data + size;
  for (const uint8_t* p = FindStartCode(data, end); p; p = FindStartCode(p + 3, end))
  {
    if (p[3] != 0x0d || p + 4 >= end)
      continue;
    const uint8_t b = p[4];
    if (!(b & 0x80))
      return Vc1Picture::ANCHOR; // P
    if (!(b & 0x40))
      return Vc1Picture::B_FRAME;
    if (!(b & 0x20))
      return Vc1Picture::ANCHOR; // I
    return (b & 0x10) ? Vc1Picture::ANCHOR : Vc1Picture::B_FRAME; // BI counts like B, skipped as S
  }
  return Vc1Picture::UNKNOWN;
}

bool HasVc1SequenceHeader(const uint8_t* data, size_t size)
{
  const uint8_t* end = data + std::min<size_t>(size, 4096);
  for (const uint8_t* p = FindStartCode(data, end); p; p = FindStartCode(p + 3, end))
  {
    if (p[3] == 0x0f)
      return true;
  }
  return false;
}
} // namespace

uint32_t TVPLAYER::EsVideoCode(const TVVideo& video)
{
  // docs/kodi/MULTIDECODE-RECHERCHE.md section 7g, measured with tools/es_codecprobe.sh
  if (video.interlaced)
    return 0; // not tested on this path yet: the TV player keeps these
  if (video.codecName == "h264")
    return 7;
  if (video.codecName == "hevc")
    return 0x14;
  if (video.codecName == "vc1")
    return 12;
  if (video.codecName == "mpeg2video")
    return 1;
  return 0;
}

uint32_t TVPLAYER::EsAudioCode(const std::string& codec)
{
  // AAC (6) needs its own test first: the TV went off in the run that tried it (section 7h)
  if (codec == "ac3" || codec == "eac3")
    return 5;
  return 0;
}

bool TVPLAYER::EsConvertsAudio(const TVTrack& track)
{
  return EsAudioCode(track.codec) == 0 && CTVAudioTranscoder::CanConvert(track.codecId);
}

uint32_t TVPLAYER::EsAudioOutCode(const TVTrack& track)
{
  const uint32_t own = EsAudioCode(track.codec);
  if (own)
    return own;
  return EsConvertsAudio(track) ? EsAudioCode("ac3") : 0;
}

uint32_t TVPLAYER::EsFrameRate(float fps)
{
  if (!(fps > 1.0f && fps < 121.0f))
    return 0;
  return static_cast<uint32_t>(std::lround(static_cast<double>(fps) * 1000.0));
}

CTVEsEngine::CTVEsEngine() : CThread("TVEsEngine")
{
}

CTVEsEngine::~CTVEsEngine()
{
  Stop();
}

bool CTVEsEngine::SpawnHelper()
{
  int data[2];
  int ctl[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data) != 0)
    return false;
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctl) != 0)
  {
    close(data[0]);
    close(data[1]);
    return false;
  }

  // the numbers the helper expects may be taken here: copies above them for the duplication
  const int childData = fcntl(data[1], F_DUPFD_CLOEXEC, ES_FD_CTL + 10);
  const int childCtl = fcntl(ctl[1], F_DUPFD_CLOEXEC, ES_FD_CTL + 10);

  std::vector<std::string> env;
  for (char** e = environ; e && *e; ++e)
  {
    if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0 && std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0)
      env.emplace_back(*e);
  }
  env.emplace_back(HELPER_LIBRARY_PATH);
  env.emplace_back(HELPER_PRELOAD);
  std::vector<char*> envp;
  for (std::string& e : env)
    envp.push_back(e.data());
  envp.push_back(nullptr);

  const std::string path = CSpecialProtocol::TranslatePath("special://xbmcbin/flex_es_helper");
  char* argv[] = {const_cast<char*>(path.c_str()), nullptr};

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  // vfork: Kodi's address space is far too large to copy for one exec
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_USEVFORK);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, HELPER_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawn_file_actions_adddup2(&fa, childData, ES_FD_DATA);
  posix_spawn_file_actions_adddup2(&fa, childCtl, ES_FD_CTL);
  const int rc = posix_spawn(&m_helper, path.c_str(), &fa, &attr, argv, envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);

  close(childData);
  close(childCtl);
  close(data[1]);
  close(ctl[1]);
  if (rc != 0)
  {
    CLog::Log(LOGERROR, "CTVEsEngine: helper {} cannot be started: {}", path, std::strerror(rc));
    m_helper = -1;
    close(data[0]);
    close(ctl[0]);
    return false;
  }
  m_dataFd = data[0];
  m_ctlFd = ctl[0];
  fcntl(m_dataFd, F_SETFL, fcntl(m_dataFd, F_GETFL) | O_NONBLOCK);
  CLog::Log(LOGINFO, "CTVEsEngine: helper started, pid {}", static_cast<int>(m_helper));
  return true;
}

bool CTVEsEngine::Start(const Params& params)
{
  m_params = params;
  m_curAudio = params.audioIndex;
  if (!SpawnHelper())
    return false;
  SendCmd(ES_OP_OPEN, m_gen, 0);
  SendWindow();
  Create();
  return true;
}

void CTVEsEngine::SendCmd(uint32_t op, uint32_t gen, uint32_t flags, int x, int y, unsigned w, unsigned h)
{
  if (m_ctlFd < 0)
    return;
  es_cmd c{};
  c.magic = ES_MAGIC;
  c.op = op;
  c.gen = gen;
  c.flags = flags;
  c.vcodec = m_params.vcodec;
  c.acodec = m_params.acodec;
  c.fps_mhz = m_params.fpsMhz;
  c.x = x;
  c.y = y;
  c.w = w;
  c.h = h;
  if (send(m_ctlFd, &c, sizeof c, MSG_NOSIGNAL) < 0)
    CLog::Log(LOGDEBUG, "CTVEsEngine: helper does not take command {}: {}", op, std::strerror(errno));
}

void CTVEsEngine::SendWindow()
{
  // ST_MEDIA_INFO carries no aspect ratio: the window keeps the picture from being stretched
  const float dar = m_params.dar;
  const float full = static_cast<float>(PANEL_W) / PANEL_H;
  if (dar <= 0.0f || std::fabs(dar - full) <= 0.01f)
    return;
  int x = 0, y = 0, w = PANEL_W, h = PANEL_H;
  if (dar > full)
  {
    h = static_cast<int>(std::lround(static_cast<double>(PANEL_W) / static_cast<double>(dar)));
    y = (PANEL_H - h) / 2;
  }
  else
  {
    w = static_cast<int>(std::lround(static_cast<double>(PANEL_H) * static_cast<double>(dar)));
    x = (PANEL_W - w) / 2;
  }
  CLog::Log(LOGINFO, "CTVEsEngine: window {}x{}+{}+{} for aspect {:.3f}", w, h, x, y,
            static_cast<double>(dar));
  SendCmd(ES_OP_RECT, 0, 0, x, y, static_cast<unsigned>(w), static_cast<unsigned>(h));
}

bool CTVEsEngine::OpenSource()
{
  // like TVPlayerProbe.cpp, here for playing
  CFileItem item(m_params.item.GetDynPath(), false);
  item.SetMimeTypeForInternetFile();
  m_input = CDVDFactoryInputStream::CreateInputStream(nullptr, item);
  if (!m_input || !m_input->Open())
    return false;
  m_demux.reset(CDVDFactoryDemuxer::CreateDemuxer(m_input, false));
  if (!m_demux)
    return false;
  m_demuxerId = m_demux->GetDemuxerId();

  CDemuxStream* video = nullptr;
  m_audioIds.clear();
  for (CDemuxStream* s : m_demux->GetStreams())
  {
    if (!s)
      continue;
    if (s->type == STREAM_VIDEO && !video && !(s->flags & AV_DISPOSITION_ATTACHED_PIC))
      video = s;
    else if (s->type == STREAM_AUDIO)
      m_audioIds.push_back(s->uniqueId);
  }
  if (!video)
    return false;
  m_videoId = video->uniqueId;
  if (m_curAudio >= 0 && m_curAudio < static_cast<int>(m_audioIds.size()))
    m_audioId = m_audioIds[m_curAudio];

  // Dolby Vision: the TV gets the base layer, an RPU in the stream is of no use to it
  if (video->codec == AV_CODEC_ID_H264 || video->codec == AV_CODEC_ID_HEVC)
  {
    m_converter = std::make_unique<CBitstreamConverter>();
    if (!m_converter->Open(video->codec, video->extraData.GetData(),
                           static_cast<int>(video->extraData.GetSize()), true))
      m_converter.reset(); // Annex B already: the packets go as they are
    else
      m_converter->SetRemoveDovi(true);
  }
  else if (video->extraData && video->codec == AV_CODEC_ID_VC1)
  {
    // Matroska keeps the sequence header here; without it in the first frame the decoder sees nothing
    m_videoExtra.assign(video->extraData.GetData(),
                        video->extraData.GetData() + video->extraData.GetSize());
    const uint8_t* begin = m_videoExtra.data();
    const uint8_t* start = FindStartCode(begin, begin + m_videoExtra.size());
    if (start)
      m_videoExtra.erase(m_videoExtra.begin(), m_videoExtra.begin() + (start - begin));
    else
      m_videoExtra.clear();
    m_extraPending = !m_videoExtra.empty();
  }
  SetupAudio(m_curAudio);
  return true;
}

void CTVEsEngine::SetupAudio(int index)
{
  m_transcoder.reset();
  m_transcodeLogged = false;
  if (m_audioId < 0 || index < 0 || index >= static_cast<int>(m_params.audioTracks.size()))
    return;
  const TVTrack& track = m_params.audioTracks[index];
  if (EsAudioCode(track.codec) != 0)
    return; // the TV takes it as it is
  const CDemuxStream* s = m_demux->GetStream(m_demuxerId, m_audioId);
  if (!s || s->type != STREAM_AUDIO)
  {
    CLog::Log(LOGERROR, "CTVEsEngine: audio track {} not found in the demuxer", index);
    return;
  }
  const CDemuxStreamAudio* a = static_cast<const CDemuxStreamAudio*>(s);
  CTVAudioTranscoder::Input in;
  in.codecId = a->codec;
  in.sampleRate = a->iSampleRate;
  in.channels = a->iChannels;
  in.bitsPerCodedSample = a->iBitsPerSample;
  in.blockAlign = a->iBlockAlign;
  in.bitRate = a->iBitRate;
  if (a->extraData)
    in.extradata.assign(a->extraData.GetData(), a->extraData.GetData() + a->extraData.GetSize());
  auto transcoder = std::make_unique<CTVAudioTranscoder>();
  std::string error;
  if (!transcoder->Open(in, error))
  {
    // the media item expects AC-3: the picture runs, without sound
    CLog::Log(LOGERROR, "CTVEsEngine: audio track {} ({}) cannot be converted: {}", index,
              track.codec, error);
    return;
  }
  CLog::Log(LOGINFO, "CTVEsEngine: audio track {} ({}) goes to the TV as Dolby Digital", index,
            track.codec);
  m_transcoder = std::move(transcoder);
}

void CTVEsEngine::SendAudio(DemuxPacket* packet)
{
  if (!m_transcoder)
  {
    SendPacket(ES_REC_AUDIO, packet->pData, static_cast<size_t>(packet->iSize), packet->pts);
    return;
  }
  const int64_t pts = packet->pts == DVD_NOPTS_VALUE ? CTVAudioTranscoder::NO_PTS
                                                     : static_cast<int64_t>(std::llround(packet->pts));
  m_transcoder->Feed(packet->pData, static_cast<size_t>(packet->iSize), pts,
                     [this](const uint8_t* data, size_t size, int64_t ptsUs) {
                       SendPacket(ES_REC_AUDIO, data, size,
                                  ptsUs > 0 ? static_cast<double>(ptsUs) : 0.0);
                     });
  if (!m_transcodeLogged && !m_transcoder->Describe().empty())
  {
    m_transcodeLogged = true;
    CLog::Log(LOGINFO, "CTVEsEngine: converting {}", m_transcoder->Describe());
  }
}

bool CTVEsEngine::WriteAll(const void* data, size_t size)
{
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (size)
  {
    const ssize_t n = send(m_dataFd, p, size, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0)
    {
      p += n;
      size -= static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    {
      // the helper takes the next packet when the TV's buffer has room; this is the normal case
      if (m_bStop)
        return false;
      pollfd pf{m_dataFd, POLLOUT, 0};
      poll(&pf, 1, 100);
      continue;
    }
    return false; // helper gone: GetStatus() reports that
  }
  return true;
}

void CTVEsEngine::SendPacket(uint32_t type, const uint8_t* data, size_t size, double ptsUs)
{
  es_rec rec{};
  rec.type = type;
  rec.size = static_cast<uint32_t>(size);
  rec.gen = m_gen;
  rec.pts_ms = ptsUs > 0.0 ? static_cast<uint32_t>(ptsUs / 1000.0) : 0;
  if (WriteAll(&rec, sizeof rec) && size)
    WriteAll(data, size);
}

void CTVEsEngine::FlushVc1Group()
{
  if (m_vc1Group.empty())
    return;
  const double frameUs = 1000000.0 / static_cast<double>(m_params.fps > 1.0f ? m_params.fps : 23.976f);
  // display order: the B frames after an anchor come first, the anchor last (es_pack.py). The stamps
  // count frames in decode order, so the anchor's stamp is where the group starts
  const double start = m_vc1Group.front().stampUs;
  const size_t count = m_vc1Group.size();
  for (size_t i = 0; i < count; ++i)
  {
    const Vc1Frame& f = m_vc1Group[i];
    const double display = i == 0 ? start + static_cast<double>(count - 1) * frameUs
                                  : start + static_cast<double>(i - 1) * frameUs;
    m_lastVideoPtsUs = std::max(m_lastVideoPtsUs, display);
    SendPacket(ES_REC_VIDEO, f.data.data(), f.data.size(), display);
  }
  m_vc1Group.clear();
}

void CTVEsEngine::SendVc1(uint8_t* data, int size, double pts)
{
  const Vc1Picture type = Vc1Type(data, static_cast<size_t>(size));
  const bool isB = type == Vc1Picture::B_FRAME;

  // Real PTS (MPEG-TS) run backwards at B frames; Matroska's VFW stamps only ever grow
  if (!m_vc1Known && isB && !m_vc1Group.empty())
  {
    m_vc1Vfw = pts > m_vc1Group.front().stampUs;
    m_vc1Known = true;
    CLog::Log(LOGINFO, "CTVEsEngine: VC-1 {}", m_vc1Vfw ? "without PTS, display times from the picture types"
                                                       : "with real PTS");
    if (!m_vc1Vfw)
    {
      for (const Vc1Frame& f : m_vc1Group)
        SendPacket(ES_REC_VIDEO, f.data.data(), f.data.size(), f.stampUs);
      m_vc1Group.clear();
    }
  }
  if (m_vc1Known && !m_vc1Vfw)
  {
    m_lastVideoPtsUs = std::max(m_lastVideoPtsUs, pts);
    SendPacket(ES_REC_VIDEO, data, static_cast<size_t>(size), pts);
    return;
  }

  // B frames right after the first key frame belong to the group before the cut: their reference is missing
  if (isB && m_vc1SkipLeading && m_vc1Group.size() <= 1)
    return;
  if (!isB)
  {
    if (!m_vc1Group.empty())
      m_vc1SkipLeading = false;
    FlushVc1Group();
  }
  m_vc1Group.push_back({std::vector<uint8_t>(data, data + size), pts, isB});
}

void CTVEsEngine::SendVideo(uint8_t* data, int size, double pts, double dts)
{
  double stamp = pts != DVD_NOPTS_VALUE ? pts : dts;
  if (stamp == DVD_NOPTS_VALUE) // some containers stamp only every few frames
    stamp = m_lastVideoPtsUs + 1000000.0 / static_cast<double>(m_params.fps > 1.0f ? m_params.fps : 23.976f);
  if (m_converter)
  {
    if (!m_converter->Convert(data, size))
      return;
    data = m_converter->GetConvertBuffer();
    size = m_converter->GetConvertSize();
    if (!data || size <= 0)
      return;
  }
  if (m_waitKey)
  {
    if (!IsKeyFrame(m_params.vcodec, data, static_cast<size_t>(size)))
      return;
    m_waitKey = false;
    m_keyPtsUs = stamp;
    CLog::Log(LOGINFO, "CTVEsEngine: first picture after the seek at {:.3f} s", stamp / 1000000.0);
  }

  if (m_params.vcodec == 12)
  {
    std::vector<uint8_t> joined;
    if (m_extraPending)
    {
      m_extraPending = false;
      if (!HasVc1SequenceHeader(data, static_cast<size_t>(size)))
      {
        joined = m_videoExtra;
        joined.insert(joined.end(), data, data + size);
        data = joined.data();
        size = static_cast<int>(joined.size());
      }
    }
    SendVc1(data, size, stamp);
    return;
  }
  m_lastVideoPtsUs = std::max(m_lastVideoPtsUs, stamp);
  SendPacket(ES_REC_VIDEO, data, static_cast<size_t>(size), stamp);
}

void CTVEsEngine::BeginSeek(int64_t ms, bool newItem)
{
  ++m_gen;
  m_vc1Group.clear();
  m_vc1SkipLeading = true;
  m_lastVideoPtsUs = -1.0;
  m_keyPtsUs = -1.0;
  m_eosSent = false;
  m_extraPending = !m_videoExtra.empty();
  m_waitKey = true;
  // the helper drops what is still on the way, and the service what it has buffered
  SendCmd(ES_OP_FLUSH, m_gen, newItem ? ES_FLUSH_RECREATE : 0);
  m_seekGen = m_gen;
  if (m_converter)
    m_converter->ResetStartDecode();
  if (m_transcoder)
    m_transcoder->Reset();
  double start = 0.0;
  if (!m_demux->SeekTime(static_cast<double>(ms), true, &start))
    CLog::Log(LOGWARNING, "CTVEsEngine: demuxer cannot seek to {} s", ms / 1000);
  else
    CLog::Log(LOGINFO, "CTVEsEngine: seek to {} s, demuxer at {:.1f} s", ms / 1000, start / 1000000.0);
}

void CTVEsEngine::HandleRequests()
{
  const int audio = m_audioReq.exchange(-1);
  const int64_t seek = m_seekReqMs.exchange(-1);
  if (audio >= 0 && audio < static_cast<int>(m_audioIds.size()))
  {
    // another track can be another codec: new media item at the current position
    m_audioId = m_audioIds[audio];
    if (audio < static_cast<int>(m_params.audioTracks.size()))
      m_params.acodec = EsAudioOutCode(m_params.audioTracks[audio]);
    SetupAudio(audio);
    const int64_t at = seek >= 0 ? seek
                                 : static_cast<int64_t>(std::max(m_lastVideoPtsUs, 0.0) / 1000.0) - 2000;
    BeginSeek(std::max<int64_t>(at, 0), true);
  }
  else if (seek >= 0)
    BeginSeek(seek, false);
}

void CTVEsEngine::Process()
{
  if (!OpenSource())
  {
    CLog::Log(LOGERROR, "CTVEsEngine: the file cannot be opened");
    SendCmd(ES_OP_STOP, 0, 0);
    return;
  }
  if (m_params.startMs > 0)
  {
    double start = 0.0;
    if (m_demux->SeekTime(static_cast<double>(m_params.startMs), true, &start))
      CLog::Log(LOGINFO, "CTVEsEngine: start at {} s, demuxer at {:.1f} s", m_params.startMs / 1000,
                start / 1000000.0);
    m_waitKey = true;
  }

  size_t keySearch = 0;
  while (!m_bStop)
  {
    HandleRequests();
    DemuxPacket* packet = m_demux->Read();
    if (!packet)
    {
      if (m_input->IsEOF() && !m_eosSent)
      {
        FlushVc1Group();
        CLog::Log(LOGINFO, "CTVEsEngine: end of file");
        SendPacket(ES_REC_EOS, nullptr, 0, 0.0);
        m_eosSent = true;
      }
      CThread::Sleep(50ms);
      continue;
    }
    if (packet->iStreamId == m_videoId && packet->iSize > 0)
    {
      SendVideo(packet->pData, packet->iSize, packet->pts, packet->dts);
      if (m_waitKey && ++keySearch > MAX_KEY_SEARCH_PACKETS)
      {
        CLog::Log(LOGWARNING, "CTVEsEngine: no key frame found after the seek, playing on");
        m_waitKey = false;
      }
      else if (!m_waitKey)
        keySearch = 0;
    }
    else if (packet->iStreamId == m_audioId && m_audioId >= 0 && packet->iSize > 0 && !m_waitKey)
    {
      // nothing before the first picture: the service would have to wait for its video
      if (m_keyPtsUs < 0.0 || packet->pts == DVD_NOPTS_VALUE || packet->pts >= m_keyPtsUs)
        SendAudio(packet);
    }
    CDVDDemuxUtils::FreeDemuxPacket(packet);
  }
  if (m_transcoder && (m_transcoder->Errors() || m_transcoder->Jumps()))
    CLog::Log(LOGINFO, "CTVEsEngine: audio conversion had {} errors, {} cuts in the stamps",
              m_transcoder->Errors(), m_transcoder->Jumps());
  m_transcoder.reset();
  m_converter.reset();
  m_demux.reset();
  m_input.reset();
}

void CTVEsEngine::Apply(const TVCtl& ctl)
{
  const bool first = !m_haveCtl;
  if (!first && ctl.pause != m_lastCtl.pause)
    SendCmd(ES_OP_PAUSE, 0, ctl.pause ? 1 : 0);
  else if (first && ctl.pause)
    SendCmd(ES_OP_PAUSE, 0, 1);

  if (!first && ctl.seekN != m_lastCtl.seekN)
  {
    m_seekSeq = ctl.seq;
    m_seekGen = 0; // set by the thread when it has started the seek
    m_seekSince = std::chrono::steady_clock::now();
    m_seekReqMs = static_cast<int64_t>(ctl.seekTo) * 1000;
  }
  if (!first && ctl.audio >= 0 && ctl.audio != m_lastCtl.audio && ctl.audio != m_curAudio)
  {
    // CTVPlayer gives tvIndex, which is the file's audio index on this path
    m_curAudio = ctl.audio;
    m_seekSeq = ctl.seq;
    m_seekGen = 0;
    m_seekSince = std::chrono::steady_clock::now();
    m_audioReq = ctl.audio;
  }
  m_lastCtl = ctl;
  m_haveCtl = true;
  m_applied = ctl.seq;
}

bool CTVEsEngine::GetStatus(TVStatus& out)
{
  if (m_ctlFd < 0)
    return false;
  bool fresh = false;
  bool gone = false;
  for (;;)
  {
    es_status received;
    const ssize_t n = recv(m_ctlFd, &received, sizeof received, MSG_DONTWAIT);
    if (n == static_cast<ssize_t>(sizeof received) && received.magic == ES_MAGIC)
    {
      m_status = received;
      fresh = true;
      continue;
    }
    gone = n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    break;
  }
  if (!fresh && !gone)
    return false;
  const es_status& s = m_status;

  out = TVStatus();
  out.n = 1;
  out.seek = true;
  out.dur = m_params.durationMs / 1000;
  out.aud = m_curAudio;
  // a converted track has no TV verdict of its own (TvAcceptedAudio: empty = accepted)
  if (m_curAudio >= 0 && m_curAudio < static_cast<int>(m_params.audioTracks.size()) &&
      EsAudioCode(m_params.audioTracks[m_curAudio].codec) != 0)
  {
    out.acodec = m_params.audioTracks[m_curAudio].codec;
    StringUtils::ToUpper(out.acodec);
  }

  if (gone)
  {
    CLog::Log(LOGWARNING, "CTVEsEngine: the helper has gone");
    out.st = "error";
    out.why = "es";
    out.err = 1;
    out.posMs = s.pts_ms;
    out.pos = s.pts_ms / 1000;
    close(m_ctlFd);
    m_ctlFd = -1;
    return true;
  }
  switch (s.state)
  {
    case ES_ST_PLAYING:
      out.st = "playing";
      break;
    case ES_ST_PAUSED:
      out.st = "paused";
      break;
    case ES_ST_ENDED:
      out.st = "ended";
      out.eof = true;
      break;
    case ES_ST_ERROR:
      out.st = "error";
      out.why = "es";
      out.err = static_cast<int>(s.err);
      break;
    default:
      out.st = "preparing";
      break;
  }
  if (s.n_fail || s.n_decerr)
    CLog::Log(LOGDEBUG, "CTVEsEngine: {} packets lost, {} decode errors", s.n_fail, s.n_decerr);

  // a seek counts as done once the helper shows the new generation near its target
  const uint32_t applied = m_applied;
  uint32_t ack = applied;
  const uint32_t seekSeq = m_seekSeq;
  if (seekSeq && seekSeq <= applied)
  {
    const uint32_t gen = m_seekGen;
    const int64_t target = static_cast<int64_t>(m_lastCtl.seekTo) * 1000;
    const bool landed = gen != 0 && s.gen == gen && s.pts_ms != 0 &&
                        std::llabs(static_cast<int64_t>(s.pts_ms) - target) <= SEEK_LAND_SLACK_MS;
    const bool timedOut = std::chrono::steady_clock::now() - m_seekSince > SEEK_ACK_TIMEOUT;
    if (landed || timedOut)
      m_seekSeq = 0;
    else
      ack = seekSeq - 1;
  }
  out.ack = ack;
  out.posMs = s.pts_ms;
  out.pos = s.pts_ms / 1000;
  return true;
}

void CTVEsEngine::ReapHelper()
{
  if (m_helper <= 0)
    return;
  // Never SIGKILL: after kill -9 in a test the next playback ended with the TV switched off
  // (MULTIDECODE-RECHERCHE section 7h). The socket's end and SIGTERM both take the clean way.
  for (int i = 0; i < 300; ++i)
  {
    int status = 0;
    const pid_t w = waitpid(m_helper, &status, WNOHANG);
    if (w == m_helper || (w < 0 && errno == ECHILD))
    {
      CLog::Log(LOGINFO, "CTVEsEngine: helper ended, status {}", status);
      m_helper = -1;
      return;
    }
    if (i == 150)
    {
      CLog::Log(LOGWARNING, "CTVEsEngine: helper still running, SIGTERM");
      kill(m_helper, SIGTERM);
    }
    CThread::Sleep(10ms);
  }
  CLog::Log(LOGERROR, "CTVEsEngine: helper does not end, left alone");
  m_helper = -1;
}

void CTVEsEngine::Stop()
{
  SendCmd(ES_OP_STOP, 0, 0);
  StopThread(true);
  if (m_dataFd >= 0)
  {
    close(m_dataFd);
    m_dataFd = -1;
  }
  if (m_ctlFd >= 0)
  {
    close(m_ctlFd);
    m_ctlFd = -1;
  }
  ReapHelper();
}
