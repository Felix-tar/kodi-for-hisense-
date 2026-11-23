#!/usr/bin/env python3
"""Vergleicht qr_test (FlexQr) mit qrref (Rust): Die Rust-Matrix muss einer der acht Masken gleichen.
Aufruf (VM): python3 compare.py <qr_test> <qrref> <text>..."""
import subprocess, sys

qt, ref, texts = sys.argv[1], sys.argv[2], sys.argv[3:]
fails = 0
for t in texts:
    out = subprocess.run([qt, t], capture_output=True, text=True).stdout.splitlines()
    head = out[1].split()
    ver, lvl, best = head[1], head[3], int(head[5])
    masks, cur = {}, None
    for line in out[2:]:
        if line.startswith("MASK "):
            cur = int(line.split()[1]); masks[cur] = []
        else:
            masks[cur].append(line)
    r = subprocess.run([ref, ver, lvl, t], capture_output=True, text=True)
    rows = r.stdout.split()
    hit = [m for m, rows_m in masks.items() if rows_m == rows]
    ok = len(hit) == 1
    fails += 0 if ok else 1
    print("%s V%s-%s Rust=Maske %s, FlexQr wählt %d: %s" % ("OK  " if ok else "FEHL", ver, lvl, hit or "-", best, t))
    if not ok and r.stderr:
        print(r.stderr[:300])
print("%d Fehler" % fails)
sys.exit(1 if fails else 0)
