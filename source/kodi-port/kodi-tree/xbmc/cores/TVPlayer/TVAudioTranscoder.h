/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

struct AVAudioFifo;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace TVPLAYER
{

/*!
 * \brief Turns an audio track the TV's media service does not take on the ES path (DTS, DTS-HD,
 *        TrueHD, FLAC, Opus, Vorbis, AAC, MP3, PCM ...) into AC-3 (Dolby Digital), which it plays
 *        (E_AUDIO_CODEC_AC3P, the same code as for AC-3 tracks of the file).
 *
 * Decodes with FFmpeg, mixes to 5.1 (three or more channels) or stereo at 48 kHz, and encodes
 * AC-3 with 640 or 256 kbit/s. DTS is decoded without its lossless extension and TrueHD only up to
 * its 5.1 substream: HDMI ARC carries neither the extensions nor more than 5.1 to the AVR anyway.
 *
 * Only FFmpeg, no Kodi headers, so it can be tested on its own (kodi-port/tests/tvplayer/transcode).
 * Not thread safe: one demuxer thread feeds it.
 */
class CTVAudioTranscoder
{
public:
  struct Input
  {
    int codecId = 0; //!< AVCodecID of the track
    int sampleRate = 0;
    int channels = 0;
    int bitsPerCodedSample = 0;
    int blockAlign = 0;
    int64_t bitRate = 0;
    std::vector<uint8_t> extradata;
  };
  //! one AC-3 frame with its presentation time in microseconds (may be slightly below the first input)
  using Sink = std::function<void(const uint8_t* data, size_t size, int64_t ptsUs)>;
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();
  static constexpr int OUT_RATE = 48000;

  CTVAudioTranscoder() = default;
  ~CTVAudioTranscoder();
  CTVAudioTranscoder(const CTVAudioTranscoder&) = delete;
  CTVAudioTranscoder& operator=(const CTVAudioTranscoder&) = delete;

  //! FFmpeg has a decoder for this AVCodecID and an AC-3 encoder
  static bool CanConvert(int codecId);

  bool Open(const Input& in, std::string& error);
  /*!
   * \brief One packet of the track as the demuxer delivers it.
   * \param ptsUs presentation time in microseconds, NO_PTS if the packet has none
   */
  void Feed(const uint8_t* data, size_t size, int64_t ptsUs, const Sink& sink);
  //! after a seek or a track change: forget everything buffered, the next stamp starts anew
  void Reset();

  //! e.g. "dts 6 ch 48000 Hz -> ac3 6 ch 640 kbit/s", empty before the first frame
  std::string Describe() const { return m_describe; }
  uint64_t Errors() const { return m_errors; }
  uint64_t Jumps() const { return m_jumps; }

private:
  void Close();
  void CloseOutput();
  void Drain(const Sink& sink);
  void Handle(AVFrame* frame, const Sink& sink);
  bool SetupOutput(const AVFrame* frame);
  void Encode(const Sink& sink);

  AVCodecContext* m_dec = nullptr;
  AVCodecContext* m_enc = nullptr;
  SwrContext* m_swr = nullptr;
  AVAudioFifo* m_fifo = nullptr;
  AVPacket* m_pkt = nullptr;
  AVPacket* m_encPkt = nullptr;
  AVFrame* m_frame = nullptr;
  AVFrame* m_conv = nullptr;
  AVFrame* m_encFrame = nullptr;

  // what the output was set up for
  int m_inRate = 0;
  int m_inFormat = -1;
  int m_inChannels = 0;
  uint64_t m_inMask = 0;

  // time line: m_baseUs is the time of the first sample written to the FIFO since the last start
  int64_t m_baseUs = NO_PTS;
  int64_t m_written = 0; //!< samples (48 kHz) written to the FIFO since m_baseUs
  int64_t m_encoded = 0; //!< samples handed to the encoder since m_baseUs

  uint64_t m_errors = 0;
  uint64_t m_jumps = 0;
  std::string m_codecName;
  std::string m_describe;
};

} // namespace TVPLAYER
