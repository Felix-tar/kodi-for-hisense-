#!/usr/bin/env python3
"""Wertet die Ausgaben von transcode_test aus (am PC, braucht ffmpeg/ffprobe; FFMPEG=... setzen).

  python check.py <ordner>        # Ordner mit <name>.ac3 und <name>.pts je Testdatei

Prüft je Datei: AC-3 lesbar, Kanalzahl (5.1 bzw. Stereo), Kanalzuordnung über die Testtöne aus
make_samples.sh, Zeitstempel (Abstand genau 32 ms, Beginn kurz vor 0, Dauer) und nach einem Sprung
den neuen Beginn. Ausgabe: je Datei OK oder FEHLER mit Grund, am Ende die Zahl der Fehler.
"""
import math
import os
import struct
import subprocess
import sys

FF = os.environ.get("FFMPEG", "ffmpeg")
FP = os.environ.get("FFPROBE") or os.path.join(os.path.dirname(FF), os.path.basename(FF).replace("ffmpeg", "ffprobe"))
DUR_S = 8.0
FRAME_US = 32000  # 1536 Abtastwerte bei 48 kHz
LEAD_US = 5333    # 256 Abtastwerte Vorlauf des AC-3-Encoders

# erwartete Töne je Ausgangskanal (Hz); bei 7.1-Quellen mischt die Umwandlung 1700/1900 hinzu
TONES_51 = [[300], [500], [700], [80], [1100], [1300]]
TONES_71 = [[300], [500], [700], [80], [1100, 1700], [1300, 1900]]
TONES_20 = [[300], [500]]
TONES_MONO = [[440], [440]]
CANDIDATES = [80, 300, 440, 500, 700, 1100, 1300, 1700, 1900]
EXPECT = {
    "dts51": TONES_51, "truehd51": TONES_51, "opus51": TONES_51, "aac51": TONES_51,
    "flac71_96k": TONES_71, "pcm71": TONES_71,
    "vorbis20_44k": TONES_20, "mp3_20_44k": TONES_20, "mp2_mono": TONES_MONO,
}
SEEK = {"dts51_seek": ("dts51", 6.0)}


def decode(path):
    probe = subprocess.run([FP, "-v", "error", "-select_streams", "a:0",
                            "-show_entries", "stream=channels,sample_rate,bit_rate,channel_layout",
                            "-of", "default=nw=1", path], capture_output=True, text=True)
    info = dict(l.split("=", 1) for l in probe.stdout.split() if "=" in l)
    ch = int(info.get("channels", "0"))
    raw = subprocess.run([FF, "-v", "error", "-f", "ac3", "-i", path, "-f", "f32le", "-"],
                         capture_output=True).stdout
    n = len(raw) // 4
    samples = struct.unpack("<%df" % n, raw[: n * 4])
    return ch, info, samples


def goertzel(x, f, rate=48000):
    k = 2.0 * math.cos(2.0 * math.pi * f / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - k * s1 * s2


def check_tones(ch, samples, expect):
    errors = []
    if ch != len(expect):
        return ["%d Kanäle statt %d" % (ch, len(expect))]
    frames = len(samples) // ch
    start = frames // 2
    length = min(24000, frames - start)  # 0,5 s aus der Mitte
    for c in range(ch):
        x = [samples[(start + i) * ch + c] for i in range(length)]
        e = {f: goertzel(x, f) for f in CANDIDATES}
        best = max(e, key=e.get)
        want = expect[c]
        if best not in want:
            errors.append("Kanal %d: stärkster Ton %d Hz statt %s" % (c, best, want))
        for f in want[1:]:
            if e[f] < 0.05 * e[want[0]]:
                errors.append("Kanal %d: %d Hz fehlt in der Mischung" % (c, f))
    return errors


def check_pts(path, seek_to=None):
    errors = []
    rows = [l.split() for l in open(path) if l.strip()]
    parts, cur = [], []
    for r in rows:
        if r[0] == "SEEK":
            parts.append(cur)
            cur = []
        else:
            cur.append(int(r[0]))
    parts.append(cur)
    first = parts[0]
    if not first:
        return ["keine Rahmen"]
    if not -LEAD_US - 30000 <= first[0] <= 0:  # AAC/MP3/Opus beginnen mit Vorlauf vor 0
        errors.append("Beginn %d µs (erwartet etwa %d)" % (first[0], -LEAD_US))
    for i, part in enumerate(parts):
        steps = {b - a for a, b in zip(part, part[1:])}
        if steps - {FRAME_US}:
            errors.append("Teil %d: Abstände %s statt nur %d" % (i, sorted(steps)[:5], FRAME_US))
    if seek_to is None:
        total = len(first)
        want = DUR_S * 1e6 / FRAME_US
        if abs(total - want) > 4:
            errors.append("%d Rahmen statt etwa %d" % (total, want))
    else:
        if len(parts) != 2 or not parts[1]:
            errors.append("kein Teil nach dem Sprung")
        else:
            d = parts[1][0] - int(seek_to * 1e6)
            if not -150000 <= d <= 150000:
                errors.append("nach dem Sprung Beginn %+d µs neben dem Ziel" % d)
    return errors


def main():
    folder = sys.argv[1]
    failed = 0
    for name in sorted(list(EXPECT) + list(SEEK)):
        ac3 = os.path.join(folder, name + ".ac3")
        pts = os.path.join(folder, name + ".pts")
        if not os.path.exists(ac3):
            print("FEHLT  %s" % name)
            failed += 1
            continue
        base, seek_to = SEEK.get(name, (name, None))
        ch, info, samples = decode(ac3)
        errors = check_pts(pts, seek_to)
        if seek_to is None:
            errors += check_tones(ch, samples, EXPECT[base])
        rate = info.get("bit_rate", "?")
        if errors:
            failed += 1
            print("FEHLER %-14s %s" % (name, "; ".join(errors)))
        else:
            print("OK     %-14s %d Kanäle, %s bit/s" % (name, ch, rate))
    print("%d Fehler" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
