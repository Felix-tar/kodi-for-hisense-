// Prüfstand für CTVAudioTranscoder (xbmc/cores/TVPlayer/TVAudioTranscoder.cpp) ohne Kodi.
// Liest die erste Tonspur einer Datei mit libavformat, gibt die Pakete wie CTVEsEngine (Zeit in
// Mikrosekunden) an den Umwandler und schreibt die AC-3-Rahmen als rohen Strom sowie ihre Zeiten.
// Auf dem TV ausführen: gleiche FFmpeg-Fassung (6.0.1, NEON) wie Kodi, misst die Rechenzeit.
//
//   transcode_test <eingabe> <ausgabe.ac3> <ausgabe.pts> [nach_s sprung_s]
//
// Mit nach_s/sprung_s: nach nach_s Sekunden Reset() und Sprung nach sprung_s (wie ein Spulen).
// Ausgabe .pts: je Rahmen "<pts_us> <bytes>", ein Sprung als Zeile "SEEK <ziel_us>".

#include "TVAudioTranscoder.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <sys/time.h>

using TVPLAYER::CTVAudioTranscoder;

static double CpuSeconds()
{
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

int main(int argc, char** argv)
{
  if (argc != 4 && argc != 6)
  {
    std::fprintf(stderr, "Aufruf: %s <eingabe> <ausgabe.ac3> <ausgabe.pts> [nach_s sprung_s]\n", argv[0]);
    return 2;
  }
  const double seekAfter = argc == 6 ? std::atof(argv[4]) : -1.0;
  const double seekTo = argc == 6 ? std::atof(argv[5]) : -1.0;

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0)
  {
    std::fprintf(stderr, "FEHLER: %s nicht lesbar\n", argv[1]);
    return 1;
  }
  const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (idx < 0)
  {
    std::fprintf(stderr, "FEHLER: keine Tonspur\n");
    return 1;
  }
  AVStream* st = fmt->streams[idx];
  const AVCodecParameters* par = st->codecpar;

  CTVAudioTranscoder::Input in;
  in.codecId = par->codec_id;
  in.sampleRate = par->sample_rate;
  in.channels = par->ch_layout.nb_channels;
  in.bitsPerCodedSample = par->bits_per_coded_sample;
  in.blockAlign = par->block_align;
  in.bitRate = par->bit_rate;
  if (par->extradata && par->extradata_size > 0)
    in.extradata.assign(par->extradata, par->extradata + par->extradata_size);

  std::printf("Eingabe: %s, Codec %s, %d Hz, %d Kanäle, CanConvert %d\n", argv[1],
              avcodec_get_name(par->codec_id), par->sample_rate, par->ch_layout.nb_channels,
              CTVAudioTranscoder::CanConvert(par->codec_id) ? 1 : 0);

  CTVAudioTranscoder tc;
  std::string error;
  if (!tc.Open(in, error))
  {
    std::fprintf(stderr, "FEHLER: Open: %s\n", error.c_str());
    return 1;
  }
  FILE* out = std::fopen(argv[2], "wb");
  FILE* pts = std::fopen(argv[3], "w");
  if (!out || !pts)
  {
    std::fprintf(stderr, "FEHLER: Ausgabe nicht schreibbar\n");
    return 1;
  }
  long frames = 0;
  const auto sink = [&](const uint8_t* data, size_t size, int64_t ptsUs) {
    std::fwrite(data, 1, size, out);
    std::fprintf(pts, "%lld %zu\n", static_cast<long long>(ptsUs), size);
    ++frames;
  };

  AVPacket* pkt = av_packet_alloc();
  const double cpu0 = CpuSeconds();
  int64_t firstUs = CTVAudioTranscoder::NO_PTS;
  int64_t lastUs = 0;
  double media = 0.0;
  bool seeked = seekAfter < 0.0;
  while (av_read_frame(fmt, pkt) >= 0)
  {
    if (pkt->stream_index != idx)
    {
      av_packet_unref(pkt);
      continue;
    }
    const int64_t us = pkt->pts == AV_NOPTS_VALUE ? CTVAudioTranscoder::NO_PTS
                                                  : av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000000});
    if (us != CTVAudioTranscoder::NO_PTS)
    {
      if (firstUs == CTVAudioTranscoder::NO_PTS)
        firstUs = us;
      lastUs = us;
    }
    if (pkt->duration > 0)
      media += pkt->duration * av_q2d(st->time_base);
    tc.Feed(pkt->data, static_cast<size_t>(pkt->size), us, sink);
    av_packet_unref(pkt);

    if (!seeked && us != CTVAudioTranscoder::NO_PTS && us >= static_cast<int64_t>(seekAfter * 1e6))
    {
      seeked = true;
      const int64_t target = static_cast<int64_t>(seekTo * 1e6);
      tc.Reset();
      av_seek_frame(fmt, idx, av_rescale_q(target, AVRational{1, 1000000}, st->time_base), AVSEEK_FLAG_BACKWARD);
      std::fprintf(pts, "SEEK %lld\n", static_cast<long long>(target));
    }
  }
  const double cpu = CpuSeconds() - cpu0;
  std::printf("Umwandlung: %s\n", tc.Describe().c_str());
  std::printf("Rahmen %ld, Fehler %llu, Zeitsprünge %llu\n", frames,
              static_cast<unsigned long long>(tc.Errors()), static_cast<unsigned long long>(tc.Jumps()));
  std::printf("Eingabe-Zeiten %.3f .. %.3f s, gelesene Dauer %.1f s\n", firstUs / 1e6, lastUs / 1e6, media);
  std::printf("CPU %.2f s für %.1f s Ton = %.1f %% eines Kerns\n", cpu, media, media > 0 ? 100.0 * cpu / media : 0.0);
  av_packet_free(&pkt);
  std::fclose(out);
  std::fclose(pts);
  avformat_close_input(&fmt);
  return frames > 0 ? 0 : 1;
}
