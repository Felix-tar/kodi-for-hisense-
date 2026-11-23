/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVAudioTranscoder.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstdlib>
#include <cstring>

using namespace TVPLAYER;

namespace
{
//! a stamp this far from the running time line is a cut (seek, broken file): start anew
constexpr int64_t JUMP_US = 150000;
//! AC-3 maximum for 5.1, and a transparent rate for stereo
constexpr int64_t BITRATE_SURROUND = 640000;
constexpr int64_t BITRATE_STEREO = 256000;

const AVCodec* Ac3Encoder()
{
  // the floating point encoder (better than ac3_fixed); with NEON cheap enough on the TV
  const AVCodec* enc = avcodec_find_encoder_by_name("ac3");
  return enc ? enc : avcodec_find_encoder(AV_CODEC_ID_AC3);
}

AVSampleFormat EncoderFormat(const AVCodec* enc)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void* cfg = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n) >= 0 &&
      cfg && n > 0)
    return static_cast<const AVSampleFormat*>(cfg)[0];
  return AV_SAMPLE_FMT_FLTP;
#else
  return enc->sample_fmts ? enc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
}

std::string ErrText(int rc)
{
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(rc, buf, sizeof buf);
  return buf;
}

AVCodecContext* OpenDecoder(const AVCodec* dec,
                            const CTVAudioTranscoder::Input& in,
                            bool options,
                            int& rc)
{
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  if (!ctx)
  {
    rc = AVERROR(ENOMEM);
    return nullptr;
  }
  // like CDVDAudioCodecFFmpeg::Open
  ctx->sample_rate = in.sampleRate;
  if (in.channels > 0)
    av_channel_layout_default(&ctx->ch_layout, in.channels);
  ctx->bits_per_coded_sample = in.bitsPerCodedSample;
  ctx->block_align = in.blockAlign;
  ctx->bit_rate = in.bitRate;
  ctx->pkt_timebase = AVRational{1, 1000000};
  if (!in.extradata.empty())
  {
    ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(in.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (ctx->extradata)
    {
      std::memcpy(ctx->extradata, in.extradata.data(), in.extradata.size());
      ctx->extradata_size = static_cast<int>(in.extradata.size());
    }
  }
  AVDictionary* opts = nullptr;
  if (options)
  {
    // what HDMI ARC carries anyway; the lossless parts cost most of the decoding time
    if (dec->id == AV_CODEC_ID_DTS)
      av_dict_set(&opts, "core_only", "1", 0);
    else if (dec->id == AV_CODEC_ID_TRUEHD || dec->id == AV_CODEC_ID_MLP)
      av_dict_set(&opts, "downmix", "5.1", 0);
  }
  rc = avcodec_open2(ctx, dec, &opts);
  av_dict_free(&opts);
  if (rc < 0)
    avcodec_free_context(&ctx);
  return ctx;
}

uint64_t NativeMask(const AVChannelLayout& layout)
{
  return layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
}
} // namespace

CTVAudioTranscoder::~CTVAudioTranscoder()
{
  Close();
}

bool CTVAudioTranscoder::CanConvert(int codecId)
{
  return codecId != AV_CODEC_ID_NONE &&
         avcodec_find_decoder(static_cast<AVCodecID>(codecId)) != nullptr && Ac3Encoder() != nullptr;
}

bool CTVAudioTranscoder::Open(const Input& in, std::string& error)
{
  Close();
  const AVCodec* dec = avcodec_find_decoder(static_cast<AVCodecID>(in.codecId));
  if (!dec)
  {
    error = "no decoder for codec " + std::to_string(in.codecId);
    return false;
  }
  if (!Ac3Encoder())
  {
    error = "no AC-3 encoder";
    return false;
  }
  m_codecName = dec->name;
  int rc = 0;
  m_dec = OpenDecoder(dec, in, true, rc);
  if (!m_dec)
    m_dec = OpenDecoder(dec, in, false, rc); // an option this FFmpeg does not know
  if (!m_dec)
  {
    error = m_codecName + " decoder: " + ErrText(rc);
    return false;
  }
  m_pkt = av_packet_alloc();
  m_encPkt = av_packet_alloc();
  m_frame = av_frame_alloc();
  m_conv = av_frame_alloc();
  m_encFrame = av_frame_alloc();
  if (!m_pkt || !m_encPkt || !m_frame || !m_conv || !m_encFrame)
  {
    error = "out of memory";
    Close();
    return false;
  }
  return true;
}

void CTVAudioTranscoder::Close()
{
  CloseOutput();
  avcodec_free_context(&m_dec);
  av_packet_free(&m_pkt);
  av_packet_free(&m_encPkt);
  av_frame_free(&m_frame);
  av_frame_free(&m_conv);
  av_frame_free(&m_encFrame);
  m_baseUs = NO_PTS;
  m_written = 0;
  m_encoded = 0;
}

void CTVAudioTranscoder::CloseOutput()
{
  avcodec_free_context(&m_enc);
  swr_free(&m_swr);
  if (m_fifo)
  {
    av_audio_fifo_free(m_fifo);
    m_fifo = nullptr;
  }
  if (m_conv)
    av_frame_unref(m_conv);
  if (m_encFrame)
    av_frame_unref(m_encFrame);
  m_inRate = 0;
  m_inFormat = -1;
  m_inChannels = 0;
  m_inMask = 0;
}

void CTVAudioTranscoder::Reset()
{
  if (m_dec)
    avcodec_flush_buffers(m_dec);
  CloseOutput();
  m_baseUs = NO_PTS;
  m_written = 0;
  m_encoded = 0;
}

void CTVAudioTranscoder::Feed(const uint8_t* data, size_t size, int64_t ptsUs, const Sink& sink)
{
  if (!m_dec || !data || !size)
    return;
  if (av_new_packet(m_pkt, static_cast<int>(size)) < 0)
  {
    ++m_errors;
    return;
  }
  std::memcpy(m_pkt->data, data, size);
  m_pkt->pts = ptsUs == NO_PTS ? AV_NOPTS_VALUE : ptsUs;
  m_pkt->dts = AV_NOPTS_VALUE;
  for (int tries = 0; tries < 2; ++tries)
  {
    const int rc = avcodec_send_packet(m_dec, m_pkt);
    if (rc == AVERROR(EAGAIN))
    {
      Drain(sink); // the decoder has frames waiting: take them, then the packet goes in
      continue;
    }
    if (rc < 0)
      ++m_errors; // a broken packet: the decoder goes on with the next one
    break;
  }
  av_packet_unref(m_pkt);
  Drain(sink);
}

void CTVAudioTranscoder::Drain(const Sink& sink)
{
  for (;;)
  {
    const int rc = avcodec_receive_frame(m_dec, m_frame);
    if (rc < 0)
    {
      if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
        ++m_errors;
      return;
    }
    Handle(m_frame, sink);
    av_frame_unref(m_frame);
  }
}

bool CTVAudioTranscoder::SetupOutput(const AVFrame* frame)
{
  AVChannelLayout inLayout{};
  if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&frame->ch_layout))
    av_channel_layout_default(&inLayout, frame->ch_layout.nb_channels);
  else if (av_channel_layout_copy(&inLayout, &frame->ch_layout) < 0)
    return false;
  if (inLayout.nb_channels <= 0)
    return false;

  const bool surround = inLayout.nb_channels >= 3;
  const AVCodec* enc = Ac3Encoder();
  m_enc = avcodec_alloc_context3(enc);
  if (!m_enc)
  {
    av_channel_layout_uninit(&inLayout);
    return false;
  }
  av_channel_layout_from_mask(&m_enc->ch_layout, surround ? AV_CH_LAYOUT_5POINT1 : AV_CH_LAYOUT_STEREO);
  m_enc->sample_rate = OUT_RATE;
  m_enc->sample_fmt = EncoderFormat(enc);
  m_enc->bit_rate = surround ? BITRATE_SURROUND : BITRATE_STEREO;
  m_enc->time_base = AVRational{1, OUT_RATE};
  int rc = avcodec_open2(m_enc, enc, nullptr);
  if (rc >= 0)
    rc = swr_alloc_set_opts2(&m_swr, &m_enc->ch_layout, m_enc->sample_fmt, OUT_RATE, &inLayout,
                             static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                             nullptr);
  if (rc >= 0)
    rc = swr_init(m_swr);
  av_channel_layout_uninit(&inLayout);
  if (rc < 0 || m_enc->frame_size <= 0)
  {
    CloseOutput();
    return false;
  }
  m_fifo = av_audio_fifo_alloc(m_enc->sample_fmt, m_enc->ch_layout.nb_channels, m_enc->frame_size * 4);
  m_encFrame->format = m_enc->sample_fmt;
  m_encFrame->sample_rate = OUT_RATE;
  m_encFrame->nb_samples = m_enc->frame_size;
  if (!m_fifo || av_channel_layout_copy(&m_encFrame->ch_layout, &m_enc->ch_layout) < 0 ||
      av_frame_get_buffer(m_encFrame, 0) < 0)
  {
    CloseOutput();
    return false;
  }
  m_inRate = frame->sample_rate;
  m_inFormat = frame->format;
  m_inChannels = frame->ch_layout.nb_channels;
  m_inMask = NativeMask(frame->ch_layout);
  m_describe = m_codecName + " " + std::to_string(m_inChannels) + " ch " + std::to_string(m_inRate) +
               " Hz -> ac3 " + std::to_string(m_enc->ch_layout.nb_channels) + " ch " +
               std::to_string(m_enc->bit_rate / 1000) + " kbit/s";
  return true;
}

void CTVAudioTranscoder::Handle(AVFrame* frame, const Sink& sink)
{
  if (frame->nb_samples <= 0)
    return;
  const int64_t pts =
      frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;

  // another layout, rate or format (a stream switching between 2.0 and 5.1): new output
  if (!m_enc || frame->sample_rate != m_inRate || frame->format != m_inFormat ||
      frame->ch_layout.nb_channels != m_inChannels || NativeMask(frame->ch_layout) != m_inMask)
  {
    CloseOutput();
    if (!SetupOutput(frame))
    {
      ++m_errors;
      return;
    }
    m_baseUs = NO_PTS;
  }

  if (pts != AV_NOPTS_VALUE)
  {
    if (m_baseUs == NO_PTS)
    {
      m_baseUs = pts;
      m_written = 0;
      m_encoded = 0;
    }
    else
    {
      const int64_t pending = swr_get_delay(m_swr, OUT_RATE);
      const int64_t expected = m_baseUs + av_rescale(m_written + pending, 1000000, OUT_RATE);
      if (std::llabs(pts - expected) > JUMP_US)
      {
        // a cut in the stamps: a new time line, the encoder's stamps start anew with it
        ++m_jumps;
        CloseOutput();
        if (!SetupOutput(frame))
        {
          ++m_errors;
          return;
        }
        m_baseUs = pts;
        m_written = 0;
        m_encoded = 0;
      }
    }
  }
  else if (m_baseUs == NO_PTS)
    return; // nothing to hang the first samples on yet

  const int outMax = swr_get_out_samples(m_swr, frame->nb_samples);
  if (outMax <= 0)
    return;
  if (m_conv->nb_samples < outMax || m_conv->format != m_enc->sample_fmt)
  {
    av_frame_unref(m_conv);
    m_conv->format = m_enc->sample_fmt;
    m_conv->sample_rate = OUT_RATE;
    m_conv->nb_samples = outMax;
    if (av_channel_layout_copy(&m_conv->ch_layout, &m_enc->ch_layout) < 0 ||
        av_frame_get_buffer(m_conv, 0) < 0)
    {
      av_frame_unref(m_conv);
      ++m_errors;
      return;
    }
  }
  const int n = swr_convert(m_swr, m_conv->extended_data, outMax,
                            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
  if (n < 0)
  {
    ++m_errors;
    return;
  }
  if (n > 0)
  {
    if (av_audio_fifo_write(m_fifo, reinterpret_cast<void**>(m_conv->extended_data), n) < n)
    {
      ++m_errors;
      return;
    }
    m_written += n;
  }
  Encode(sink);
}

void CTVAudioTranscoder::Encode(const Sink& sink)
{
  const int frameSize = m_enc->frame_size;
  while (av_audio_fifo_size(m_fifo) >= frameSize)
  {
    if (av_frame_make_writable(m_encFrame) < 0 ||
        av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(m_encFrame->extended_data), frameSize) <
            frameSize)
    {
      ++m_errors;
      return;
    }
    m_encFrame->nb_samples = frameSize;
    m_encFrame->pts = m_encoded;
    m_encoded += frameSize;
    if (avcodec_send_frame(m_enc, m_encFrame) < 0)
    {
      ++m_errors;
      continue;
    }
    while (avcodec_receive_packet(m_enc, m_encPkt) >= 0)
    {
      // the encoder's own stamp already accounts for its 256 samples of lead-in
      const int64_t at = m_encPkt->pts != AV_NOPTS_VALUE ? m_encPkt->pts : m_encoded - frameSize;
      sink(m_encPkt->data, static_cast<size_t>(m_encPkt->size),
           m_baseUs + av_rescale(at, 1000000, OUT_RATE));
      av_packet_unref(m_encPkt);
    }
  }
}
