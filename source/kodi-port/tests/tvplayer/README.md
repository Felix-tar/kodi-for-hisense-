# Tests für die TVPlayer-Skripte und C++-Korrekturen (Build-VM)

Kopien aus dem Scratchpad der Sitzung vom 29.10.2026. Alle laufen auf der Build-VM (Ubuntu) und
fassen weder den TV noch `~/flex/build-kodi` an.

| Datei | Inhalt | Aufruf (auf der VM) |
|---|---|---|
| `s2setup.sh` | baut eine Testumgebung: Kopien von `kodi.sh`, `kodi-run.sh`, `kodi-watch.sh` unter BusyBox ash, Pfade nach `$T` umgebogen, uid 10009/0 → VM-Benutzer | `bash s2setup.sh <T>` |
| `s2unit.sh` | Einzelprüfungen der Skriptfunktionen (Schlüssel, `islink_to`, `rtdir`) | `bash s2unit.sh <T>` |
| `s2int.sh` | Zusammenspiel Wächter + `kodi.sh` + `kodi-run.sh` mit Kodi-Ersatz, Szenarien 1–7 (Boot, Schlüssel, Manipulation am Brückenordner, `rt` als Ordner, fremdes `flexkodi`, untergeschobener Schlüssel, Sabotage von `running`, Aufräumen) | `bash s2int.sh <T> <szenario>` |
| `watcher-fix/` | Messung und Szenarien zur Wächter-Korrektur (Prozesse je Runde vorher/nachher, `IDLEN`/`BOOTN`): `wsetup.sh`, `wmeasure.sh` (strace), `wint.sh`, Ergebnisse `s2int-*.txt`, `T.txt`, Ausgangsskripte in `before/` | `bash watcher-fix/wsetup.sh <T> before|after [BOOTN]` |
| `cppfix-unit.sh` | Host-Test: übersetzt den echten Quelltext dreier korrigierter Stellen gegen Attrappen (Feeder-Verdrängung, `HasDvBaseLayer`, `IsSwitchedOff`) | `bash cppfix-unit.sh` (liest `~/flex/kodi-port/kodi-tree/...`) |
| `cppfix-pkg.sh` | Prüft das gebaute Paket (Inhalt, keine Sicherungsdateien, Skripte identisch) | `bash cppfix-pkg.sh` |
| `transcode/` | Prüfstand der Tonumwandlung (`TVAudioTranscoder`, 17.11.2026): Testdateien mit einem Ton je Kanal (`make_samples.sh`, am PC mit ffmpeg), Testprogramm für den TV (`transcode_test.cpp`, `build.sh` auf der VM), Auswertung am PC (`check.py`). Ablauf und Ergebnisse: `docs/kodi/TONFORMATE.md` | `bash transcode/build.sh` (VM), Programm und Dateien nach `/tmp` am TV, `python check.py <ordner>` (PC) |
| `phonekbd/` | Handy-Tastatur (17.11.2026): QR-Encoder `FlexQr.cpp` gegen die Rust-Bibliothek `qrcode` 0.14.1 (`qr_test.cpp`, `qrref/`, `compare.py`); `docs/kodi/HANDY-TASTATUR.md` | `bash phonekbd/run_qr_compare.sh` |
| `search/` | Tolerante Bibliothekssuche `FlexFuzzySearch.cpp` (18.11.2026): 60 Prüfungen mit Beispieltiteln; mit `<titel.tsv> "<suche>"` ordnet es eine Titelliste; `docs/kodi/SUCHE.md` | `bash search/run_search_test.sh` |
| `volume/` | Statusseite des AV-Receivers → Lautstärkekreis (`FlexDenonStatus.cpp`, 18.11.2026): 18 Prüfungen; `docs/kodi/LAUTSTAERKE-BALKEN.md` | `bash volume/run_volume_test.sh` |

`<T>` ist ein frischer Ordner unter `/tmp` (z. B. `T=$(mktemp -d)`); danach wieder löschen.
Kopiere vor dem Test den aktuellen Quellstand in deinen Build-Ordner.
fehlen am TV trotzdem (`analysis/tvplayer-20261028/tv-shell-tools.txt`).
