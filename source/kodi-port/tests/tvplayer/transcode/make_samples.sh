#!/bin/bash
# Ohne map= ordnet join Mono-Eingänge (FC) nach Namen zu und verschiebt alles; "5.1" heißt bei FFmpeg
# FL FR FC LFE BL BR, darum 5.1(side).
# Erzeugt Testdateien für transcode_test: je Kanal ein eigener Sinuston, damit die Auswertung
# (check.py) die Kanalzuordnung nach der Umwandlung prüfen kann. Frei erzeugt, keine fremden Inhalte.
#   FFMPEG=/pfad/zu/ffmpeg bash make_samples.sh <ausgabeordner>
# Töne: FL 300, FR 500, FC 700, LFE 80, BL/SL 1100, BR/SR 1300, bei 7.1 SL 1700, SR 1900 Hz;
# Stereo FL 300, FR 500; Mono 440 Hz. Dauer 8 s.
set -eu
FF=${FFMPEG:-ffmpeg}
O=${1:?Ausgabeordner}
mkdir -p "$O"
D=8
tone() { echo "sine=f=$1:d=$D:sample_rate=$2"; }
mk51() { # rate -> Filter für 5.1(side)
  echo "$(tone 300 $1)[a];$(tone 500 $1)[b];$(tone 700 $1)[c];$(tone 80 $1)[d];$(tone 1100 $1)[e];$(tone 1300 $1)[f];[a][b][c][d][e][f]join=inputs=6:channel_layout=5.1(side):map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-SL|5.0-SR[o]"
}
mk71() {
  echo "$(tone 300 $1)[a];$(tone 500 $1)[b];$(tone 700 $1)[c];$(tone 80 $1)[d];$(tone 1100 $1)[e];$(tone 1300 $1)[f];$(tone 1700 $1)[g];$(tone 1900 $1)[h];[a][b][c][d][e][f][g][h]join=inputs=8:channel_layout=7.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR|6.0-SL|7.0-SR[o]"
}
mk20() {
  echo "$(tone 300 $1)[a];$(tone 500 $1)[b];[a][b]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR[o]"
}
run() { # name filter codec-optionen...
  local n=$1 f=$2; shift 2
  if "$FF" -v error -y -filter_complex "$f" -map "[o]" "$@" "$O/$n" 2>"$O/$n.err"; then
    rm -f "$O/$n.err"; echo "ok   $n"
  else
    echo "FEHL $n: $(head -c 200 "$O/$n.err")"
  fi
}
run dts51.mkv        "$(mk51 48000)" -c:a dca -strict -2 -b:a 1509k
run truehd51.mkv     "$(mk51 48000)" -c:a truehd -strict -2
run flac71_96k.mkv   "$(mk71 96000)" -c:a flac -sample_fmt s32
# libopus nimmt 5.1 nur als FL FR FC LFE BL BR: Surround auf die hinteren Kanäle umbenennen
run opus51.mkv       "$(mk51 48000 | sed 's/\[o\]$/[j];[j]channelmap=map=FL-FL|FR-FR|FC-FC|LFE-LFE|SL-BL|SR-BR:channel_layout=5.1[o]/')" \
  -c:a libopus -mapping_family 1 -b:a 384k
run vorbis20_44k.mkv "$(mk20 44100)" -c:a libvorbis -q:a 6
run aac51.mkv        "$(mk51 48000)" -c:a aac -b:a 384k
run mp3_20_44k.mkv   "$(mk20 44100)" -c:a libmp3lame -b:a 256k
run pcm71.mkv        "$(mk71 48000)" -c:a pcm_s24le
run mp2_mono.mkv     "$(tone 440 48000)[o]" -c:a mp2 -b:a 192k
ls -la "$O"
