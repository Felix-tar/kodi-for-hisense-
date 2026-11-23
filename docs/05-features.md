# Language, input, library and Home

These features live in the authoritative overlay and launcher. The German interface uses the
official German Kodi language-pack add-on. Keep language packs and user-generated data
as device-local inputs. Set a new phone keyboard pairing token locally where required.


## 0. Conventions and background you need first

### 0.1 Paths on the TV

| Path | What it is |
|---|---|
| `/APPS/kodi/` | the Kodi installation (partition p36, ext4, read-write; it also holds the launcher and the boot hook) |
| `/APPS/kodi/lib/kodi/kodi-directfb` | the Kodi binary (about 54 MB; Kodi names the binary after its only platform) |
| `/APPS/kodi/share/kodi/` | `KODI_HOME` as exported by `kodi.sh` (system add-ons, including the Estuary skin) |
| `/APPS/kodi-home/.kodi/` | the user data (`special://home`) when no writable USB stick is present; otherwise `<stick>/flexkodi/` (see `data_dir()` in `kodi.sh`). The owner's TV uses `/APPS/kodi-home`. |
| `/APPS/kodi/kodi.sh` | start/stop/status script (`kodi.sh start`, `kodi.sh stop`, …) |
| `/APPS/kodi/kodi-run.sh` | wrapper around one Kodi run; writes the heartbeat for the launcher |
| `/APPS/kodi/kodi-watch.sh` | root watcher that turns the launcher's request files into `kodi.sh start/stop` |
| `/var/local/applications/UI/flexkodi/` | request and heartbeat files shared with the launcher (`start`, `stop`, `running`) |
| `/tmp/flexkodi.d/` | root-only (0700) run directory; Kodi's state files (`ready`, `lastkey`, `minimized`, `restore`) |
| `/OAD/` | partition p37, ext4, the vendor's work area for firmware updates (used for the image cache, section 4) |

### 0.2 How Kodi sits on this TV (short version)

Kodi runs as an **additional DirectFB client on top of the VIDAA launcher**. While it runs, it grabs the
remote control exclusively (`EVIOCGRAB` on `/dev/input/event0`); when it exits or crashes, the kernel
returns the remote to the TV automatically. The [Flex launcher mod](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/03-launcher-and-recovery.md)
starts Kodi through files and a root watcher, because the launcher runs sandboxed as uid 10009 and cannot start programs.
Films can be handed to the TV's own media player (the "TVPlayer"/ES path); that is described in the
playback chapters of this repository.

### 0.3 Important constraints that shaped every feature here

- **This Kodi is built without Python** (`ENABLE_PYTHON=OFF`, part of the deliberately small feature set
  in the build: Python, UPnP, AirTunes, optical drives, DVDCSS, event clients are all off). Consequence:
  **no Python add-on runs** – no Global Search, no YouTube add-on, no TMDB scrapers. Every feature in this
  chapter is therefore implemented in C++ inside Kodi or in shell/JavaScript outside it. The library is
  filled from NFO files with the `metadata.local` "scraper", which does not need Python.
- **Space on `/APPS` is tight.** `kodi.sh` refuses to start Kodi when less than 150 MB
  (`APPS_FLOOR_KB=150000`) are free on `/APPS` while the user data lives there, and `kodi-run.sh` stops a
  running Kodi below 80 MB (`APPS_STOP_KB=80000`), because `/APPS` also carries the launcher and the boot
  hook. Keep at most one backup copy of `kodi-directfb` on the TV.
- **The TV's BusyBox (1.22.1) is minimal**: no `readlink`, `tr`, `sort`, `od` or `mktemp`. Scripts on the
  TV must work without them.
- **TV scripts must have LF line endings.** If you edit them on Windows, check with `file -b` before
  copying them over.

### 0.4 Replacing the Kodi binary (used by sections 2–5)

Sections 2, 3, 4 and 5 all ship as a new `kodi-directfb`. Build it as described in the build chapter,
then on the TV (adapt the copy step to your tooling; the project uses its own helper scripts):

```sh
# on the TV, as root
/APPS/kodi/kodi.sh stop
df -k /APPS                      # the old binary copy needs ~54 MB, keep 150 MB free afterwards
cp /APPS/kodi/lib/kodi/kodi-directfb /APPS/kodi/lib/kodi/kodi-directfb.pre-<feature>
# copy the new binary from the build machine, e.g.:
#   Transfer the verified build as /APPS/kodi/lib/kodi/kodi-directfb.new.
chmod 755 /APPS/kodi/lib/kodi/kodi-directfb.new
mv /APPS/kodi/lib/kodi/kodi-directfb.new /APPS/kodi/lib/kodi/kodi-directfb
/APPS/kodi/kodi.sh start         # or press OK on the Kodi tile
```

Rollback is always the reverse: stop Kodi, move the `.pre-*` copy back. Older binaries are kept on the
build PC, not on the TV.

---

## 1. German user interface

The port ships with Kodi's built-in English strings only. The German interface is the **official
language add-on `resource.language.de_de`, version 11.0.113, from the Kodi Omega repository**, unpacked by
hand into the user add-on folder. (Installing add-ons from inside this Kodi does not work at the moment,
see [3.2](#32-why-the-global-search-add-on-cannot-work-and-why-repository-installs-fail); a language pack
contains no Python, so copying it in is enough.)

Steps as done on the TV (2026-11-17):

```sh
# on the build machine: fetch the add-on zip from the Omega repository
#   (mirrors.kodi.tv/addons/omega/resource.language.de_de/), check it, unpack it
unzip resource.language.de_de-11.0.113.zip            # creates resource.language.de_de/

# copy the folder to the TV (tar stream over SSH; no unzip needed on the TV)
# Transfer the folder into /APPS/kodi-home/.kodi/addons using your authorized transport.
```

Then, **with Kodi stopped** (Kodi rewrites `guisettings.xml` when it exits), the following settings were
changed in `/APPS/kodi-home/.kodi/userdata/guisettings.xml` (backups first:
`guisettings.xml.pre-de-20261117` and `guisettings.xml.pre-de-ui-20261117`):

| Kodi setting | Value chosen |
|---|---|
| `locale.language` | `resource.language.de_de` |
| `locale.keyboardlayouts` | "German QWERTZ" and "English QWERTY" |
| `locale.country` (region) | "Deutschland" |
| `locale.audiolanguage` | German |

The same options exist in Kodi's GUI under the regional settings; the project edited the file directly.

Two parts of this project are **hard-coded German** and are not affected by the language pack: the two
label lines under the QR code in the skin (`DialogKeyboard.xml`) and the phone page of the phone keyboard
(the `PAGE` string in `FlexPhoneKeyboard.cpp`). Translate those two places if you want another language.

**Rollback:** stop Kodi, copy `guisettings.xml.pre-de-20261117` (or `.pre-de-ui-20261117`) back to
`guisettings.xml`, delete `addons/resource.language.de_de/`.

---

## 2. Phone keyboard: type into Kodi from the phone

Typing a search term with the TV remote on Kodi's on-screen keyboard is slow. The phone keyboard puts a
QR code next to Kodi's keyboard; the phone scans it and gets a small web page with a text field.

### 2.1 What the user sees

- **A keyboard dialog is open on the TV** (search, names, filters, …): a 320×320 QR code is shown at the
  bottom left with the text "Mit dem Handy tippen: / QR-Code scannen" ("Type with your phone: scan the QR
  code"). On the phone page, every character appears on the TV immediately. The button "Fertig" ("Done")
  sends the text and confirms the dialog.
- **No keyboard dialog is open on the TV:** type a term on the phone and tap "Suchen" ("Search"). Kodi
  closes all dialogs, opens the movie library, runs the library search with that term and shows the
  results (the search of [section 3](#3-tolerant-library-search)). The page stays usable, so you can search
  again and again without the remote and without scanning again.
- The page stays connected for the whole Kodi run **and across Kodi restarts** (same code in the URL).
  When Kodi is not running, it shows "Kodi nicht erreichbar" ("Kodi not reachable") and reconnects on its
  own.
- **Password fields (hidden input) never get a QR code.**

### 2.2 Why it is built into Kodi

- No Python, so the obvious route (a Python service add-on with its own web server) is not available.
- On this TV, Kodi's own web server (port 8080) is **off** and JSON-RPC (TCP 9090) and the EventServer
  (UDP 9777) listen on **127.0.0.1 only** (see the firewall chapter of the root-mod repository). Phone
  remote apps that need JSON-RPC over the network therefore cannot reach Kodi. The phone keyboard is a
  separate, very small endpoint that can only do two things: put text into an open keyboard dialog, and
  start a library search.

A first build went onto the TV on 2026-11-17; the build of 2026-11-18 made the phone page persistent (it
stays connected across Kodi restarts). The state described here is that of 2026-11-18.

### 2.3 Architecture

```
 phone browser                                   Kodi (kodi-directfb, root)
 ----------------------------                    ---------------------------------------------------
 scan QR:                                        CGUIDialogKeyboardGeneric (Kodi's keyboard dialog)
 http://<tv-ip>:8781/k/<code>  ----- GET ----->    ctor:  CFlexPhoneKeyboard::Startup()  -> MHD daemon
                                                   OnInitWindow: DialogOpened() -> QR PNG -> window
 poll GET  .../s   (1 s, 2 s if tab hidden) ->       property "flexkbd.qr" -> skin shows the image
 POST .../t   (text while typing)  --------->    SendText(): GUI_MSG_SET_TEXT to the dialog's edit
 POST .../ok  (text + confirm)     --------->       control (id 312), param1 = 1 confirms (OnOK)
 POST .../search (no dialog open)  --------->    StartSearch(): Dialog.Close(all),
                                                   ActivateWindow(Videos,videodb://movies/titles/,return),
                                                   VideoLibrary.Search; text waits <= 10 s for the
                                                   next keyboard dialog, then goes in and confirms
                                                 dtor:  CFlexPhoneKeyboard::Shutdown()
```

### 2.4 Files

| File (in `source/kodi-port/kodi-tree/`) | Content |
|---|---|
| `xbmc/dialogs/FlexPhoneKeyboard.h/.cpp` | the web server (libmicrohttpd, already a Kodi dependency), the phone page, token handling, QR image, text/search hand-over |
| `xbmc/dialogs/FlexQr.h/.cpp` | own QR encoder (ISO/IEC 18004, byte mode, versions 1–10, level M, L only if M does not fit); no Kodi headers, so it can be tested on its own |
| `xbmc/dialogs/GUIDialogKeyboardGeneric.h/.cpp` | hooks: `Startup()` in the constructor, `Shutdown()` in the (new) destructor, `DialogOpened()` when the dialog initialises (sets the window property `flexkbd.qr`), `DialogClosed()` when it closes (clears it) |
| `addons/skin.estuary/xml/DialogKeyboard.xml` | a group at the bottom left (`left 40`, `bottom 30`, 400×420): the QR image 320×320 (`aspectratio keep`) and two label lines; visible only when `Window.Property(flexkbd.qr)` is not empty |

Lifetime: Kodi creates its windows at startup and destroys them at shutdown, so the server starts with
Kodi and ends with Kodi. `Shutdown()` runs in the keyboard dialog's destructor, i.e. **before the
application messenger is destroyed**; it marks the object as closing (requests still in flight get 503),
then `MHD_stop_daemon()` waits for running handlers. The handlers never wait for the GUI thread (all
messages are posted asynchronously), so this cannot deadlock. The mutex is never held while the server is
started or stopped.

### 2.5 The server

| Property | Value |
|---|---|
| Port | **TCP 8781** (next to ports used by other components of this project, 8770 and 8780) |
| Address family | **IPv4 only.** The TV has no `ip6tables`, so IPv6 could not be filtered; the accept callback refuses everything that is not `AF_INET`. |
| Allowed clients | private ranges only: RFC 1918 and loopback (checked in libmicrohttpd's accept callback, before any request is read) |
| Threading | `MHD_USE_INTERNAL_POLLING_THREAD` |
| Limits | max 8 connections, 4 per client address, 15 s connection timeout, 32 KB memory per connection, 1 KB request body (longer: 413), text cut to 256 bytes at a UTF-8 character boundary |
| Text cleaning | CR, LF and TAB become spaces; other control characters and DEL are dropped |
| Methods | `GET` and `POST` only, everything else 405 |
| Response headers | `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` |
| Page | one self-contained HTML page, same origin only, no external resources (ASCII source, umlauts as escapes) |
| Kodi log | `CFlexPhoneKeyboard: phone keyboard on port 8781`, `… library search from the phone`; neither the code nor the typed text is logged |

If the port cannot be opened, Kodi logs `CFlexPhoneKeyboard: port 8781 cannot be opened, no phone
keyboard` and simply shows no QR code.

### 2.6 HTTP interface

Everything lives under `/k/<code>`. A wrong or missing code gives 404 for every path.

| Request | Answer |
|---|---|
| `GET /k/<code>` (or `/k/<code>/`) | the phone page (`text/html`) |
| `GET /k/<code>/s` | state as JSON: `{"open":true/false,"session":<n>,"heading":"…","text":"…"}` – `open` = a keyboard dialog is open; `session` counts opened dialogs (the page takes over the dialog's current text and heading when a new one opens) |
| `POST /k/<code>/t` | body = the whole current text (`text/plain;charset=utf-8`); put into the open dialog without confirming. 409 if no dialog is open |
| `POST /k/<code>/ok` | like `/t`, and confirms the dialog. 409 if no dialog is open |
| `POST /k/<code>/search` | no dialog open: start a library search with the body as search term (empty body: the search dialog just opens). If a dialog has opened meanwhile, behaves like `/ok` |
| any request while Kodi is shutting down | 503 |

The page sends the **whole text** on every change (the dialog replaces its content), and serialises its
requests (a new change waits until the previous `POST` is answered), so characters cannot arrive out of
order. It polls `/s` every second, every 2 s while the browser tab is hidden. Status lines on the page:
"Kodi nicht erreichbar (läuft Kodi?)" for no answer/503, "Code ungültig: QR-Code am TV neu scannen"
(invalid code, scan again) for 404, "Das Eingabefeld am TV ist schon zu" (the input field on the TV is
already closed) for 409.

### 2.7 How the text gets into Kodi's dialog

Kodi already has two ways in which text arrives in the keyboard dialog from outside: speech recognition
and JSON-RPC `Input.SendText`. Both send `GUI_MSG_SET_TEXT` to the dialog. The phone keyboard does the
same:

```cpp
CGUIMessage msg(GUI_MSG_SET_TEXT, dialogId, CTL_EDIT /* 312 */, done ? 1 : 0);
msg.SetLabel(text);
CServiceBroker::GetAppMessenger()->SendGUIMessage(msg, dialogId);
```

The message is handled in the GUI thread; `param1 = 1` makes the dialog call `OnOK()`. Because this is the
existing, supported path, no change to the dialog's own text handling was necessary.

### 2.8 "Search" from the phone without an open dialog

`StartSearch()` stores the text as "pending" for 10 seconds and posts three built-in commands to the
application thread – exactly what the "Search library" entry of Estuary's search dialog does after the
change of section 3:

```
Dialog.Close(all)
ActivateWindow(Videos,videodb://movies/titles/,return)
VideoLibrary.Search
```

`VideoLibrary.Search` opens Kodi's keyboard for the search term. When that dialog opens,
`DialogOpened()` finds the pending text (still within 10 s), puts it in and confirms it, so the search runs
immediately. An empty search term only opens the search; the user then types live.

### 2.9 The QR code

`DialogOpened()` builds the URL `http://<tv-ip>:8781/k/<code>` from the first connected network interface.
If there is no IPv4 address (no network, IPv6 only, `0.0.0.0`), there is no QR code.

The QR encoder `FlexQr.cpp` implements the standard directly: byte mode, Reed–Solomon over GF(256)
(polynomial 0x11D), versions 1–10, error correction level M (L only if M does not fit), all eight masks
with the standard's penalty rules (N1 = 3, N2 = 3, N3 = 40, N4 = 10) and the lowest-penalty mask chosen.
For a URL of this length (46–54 bytes) the result is version 4, 33×33 modules (calculated from the
capacity table: version 3-M holds 42 bytes, version 4-M 62).

The image is drawn with 8 pixels per module and a quiet zone of 4 modules, as a BGRA buffer, and written to
`special://temp/flexkbd-qr-<n>.png` with Kodi's `CPicture::CreateThumbnailFromSurface()`. A new URL (new IP
address, new code) gets a **new file name**, because Kodi's texture manager caches images by path; the
old file is deleted. The skin reads the path from the window property `flexkbd.qr`.

### 2.10 The code in the URL

- 24 hex characters = **96 bits** from `std::random_device` (kernel randomness).
- Stored in `userdata/flexkbd.token`, i.e. `/APPS/kodi-home/.kodi/userdata/flexkbd.token`, mode **0600**
  (written under `umask 077`, then `chmod 0600`). An existing file with a valid code is reused; that is
  why the phone page survives Kodi restarts. If the file cannot be written, Kodi logs a warning and makes a
  new code on every start.
- **New code:** delete the file and restart Kodi. Old phone pages then get 404.
- Treat the code as a secret: never paste it into logs, issues or documentation. Anyone in the LAN who
  knows it can type into an open Kodi keyboard on your TV.

### 2.11 Switching it off

Either create the file `/APPS/kodi-phonekbd.off` or set `FLEX_KODI_PHONEKBD=0` in Kodi's environment
(`kodi.sh`). The switch is checked at Kodi start and whenever a keyboard dialog opens; a server that is
already running stays up until Kodi exits, so restart Kodi after creating the file.

### 2.12 Tests

| Test | Result |
|---|---|
| QR encoder vs. the Rust crate `qrcode` 0.14.1 (`source/kodi-port/tests/tvplayer/phonekbd/run_qr_compare.sh`, on the build VM, offline cargo build): `qr_test` prints FlexQr's matrix for all eight masks; `compare.py` requires the Rust matrix to be identical to exactly one of them | 9 test strings (URLs of the real form, short texts, 60/120/200-byte strings), **0 deviations** |
| page script | `node --check` |
| server on the TV, with `curl` from the PC | page 200, state JSON, wrong code 404, text without open dialog 409, `PUT` 405 |
| on the TV, by the owner | scan the QR code, live typing, "Fertig", then "Suchen" from the phone several times in a row |

The compare test verifies version, level, data and error-correction placement and the format bits; which
of the eight masks each implementation picks is printed, but not required to be the same.

Manual checks you can repeat (replace `<code>` with the content of `flexkbd.token`; do it on your own LAN
only):

```sh
curl -s  http://<tv-ip>:8781/k/<code>/s
curl -s -o /dev/null -w '%{http_code}\n' http://<tv-ip>:8781/k/000000000000000000000000/s   # 404
curl -s -o /dev/null -w '%{http_code}\n' -X PUT http://<tv-ip>:8781/k/<code>/s              # 405
curl -s -X POST -H 'Content-Type: text/plain;charset=utf-8' \
     --data-binary 'tears of steel' http://<tv-ip>:8781/k/<code>/search                     # runs a search
```

### 2.13 Rollback

| What | How |
|---|---|
| Kodi binary | an older `kodi-directfb` (kept on the build PC) back to `/APPS/kodi/lib/kodi/kodi-directfb`, restart Kodi |
| Skin | copy `DialogKeyboard.xml.pre-phonekbd` back to `DialogKeyboard.xml` |
| Code | delete `userdata/flexkbd.token` |

---

## 3. Tolerant library search

### 3.1 The problem

- Estuary's magnifier → **"Search library"** calls the add-on **Global Search** (`script.globalsearch`).
  That is a Python add-on; in this Kodi the menu entry simply did nothing.
- Kodi's own video search (`VideoLibrary.Search`, `CGUIWindowVideoNav::DoSearch`) only finds the typed text
  **literally** inside the title stored in the database.
- The library's titles come from NFO files (`metadata.local`) and are **German**; the **original title**
  (mostly English) is stored next to it but was not searched. A typo, a missing umlaut, or "Spiderman"
  instead of "Spider-Man" found nothing.

### 3.2 Why the Global Search add-on cannot work, and why repository installs fail

- Global Search is written in Python. With `ENABLE_PYTHON=OFF` there is no interpreter in this Kodi; the
  add-on can be registered, but never runs. For the experiment it was entered by hand into
  `userdata/Database/Addons33.db` and later removed again (backup `Addons33.db.pre-globalsearch-20261118`).
  Enabling Python would need Kodi's SWIG-generated bindings (which in turn need Java on the build host)
  and a Python runtime for the TV; the port is built without it.
- **Side finding, still open:** installing add-ons from the Kodi repository fails on this TV even for
  non-Python add-ons. The download works, but while unpacking the zip Kodi converts file names from CP437
  to UTF-8, and the TV's glibc has no gconv module for that conversion:

  ```
  customConvert: iconv_open() for "CP437" -> "UTF-8" failed
  ```

  Kodi then reports an "invalid package". Unpacking add-ons by hand (as in section 1) works around it.
  (Untested idea: providing the missing gconv module and pointing `GCONV_PATH` at it.)

### 3.3 The solution in three parts

1. **Menu entry** (`addons/skin.estuary/xml/Custom_1107_SearchDialog.xml`): "Search library" no longer
   calls the Python add-on but Kodi's own video search:

   ```xml
   <onclick>Dialog.Close(all)</onclick>
   <onclick>ActivateWindow(Videos,videodb://movies/titles/,return)</onclick>
   <onclick>VideoLibrary.Search</onclick>
   ```

   Selecting a hit jumps to its place in the movie list.
2. **Ranking** (`xbmc/video/FlexFuzzySearch.h/.cpp`, namespace `FLEXSEARCH`, no Kodi dependencies so the
   host test can build it as is): every title gets a score; the German title and the original title are
   both scored and the better score counts.
3. **Search** (`xbmc/video/windows/GUIWindowVideoNav.cpp`, `DoSearch`): movies, TV shows and movie actors
   are ranked with this score and listed first; the rest of Kodi's search follows as before.

From the phone, "Suchen" on the phone keyboard page uses exactly this search (section 2.8).

### 3.4 Normalisation (`FLEXSEARCH::Fold`)

Title and query are folded the same way before they are compared:

| Input | Becomes |
|---|---|
| upper case A–Z | lower case |
| `ä ö ü`, accented letters (U+00C0–U+00FF, U+0100–U+017F) | the base letter (`é` → `e`, `ł` → `l`, …) |
| `ß` | `ss` |
| `ae`, `oe`, `ue` (after the steps above) | `a`, `o`, `u` – so "Muenchen", "Munchen" and "München" are the same |
| `'`, `` ` ``, `´` (U+00B4), U+02BC, U+2018, U+2019 (apostrophes) | dropped ("Schindler's" = "Schindlers") |
| `&` | the word `and` |
| other punctuation, Latin-1 symbols, no-break space, U+2000–U+206F (dashes, quotes, spaces), U+3000, `×`, `÷` | a single space |
| other scripts | kept as they are |
| invalid UTF-8 bytes | read as Latin-1 (old files) |

The `ae/oe/ue` rule is applied everywhere, also where it is not a typed umlaut (for example "blue"
becomes "blu"). Because query and title are folded identically, this does not hurt matching.

The folded text is then split into words. **Roman numerals** `ii, iii, iv, vi, vii, viii, ix` become
`2, 3, 4, 6, 7, 8, 9` (single letters `i`, `v`, `x` are left alone). **Stop words** are removed from the
query's "key" words, unless the query consists only of stop words:

`a an and am at auf das de dem den der des die du ein eine einem einen einer eines el il im in la le les
of on part teil the to und vom von zu zum zur`

### 3.5 Scores (`FLEXSEARCH::Score`)

The first rule that applies decides (query = folded query, title = folded title):

| Case | Score |
|---|---|
| title and query identical | **1000** |
| title starts with the query, followed by a word boundary | 960 |
| title starts with the query, mid-word (still typing) | 940 |
| query found at the start of a later word, ending at a word boundary | 920 |
| query found at the start of a later word, ending mid-word | 900 |
| query (≥ 4 characters) found anywhere | 860 |
| query without spaces (≥ 4 characters) found in the title without spaces ("spiderman" = "spider man") | **840** (`PHRASE`) |
| word by word (below) | 400 + 350 × average word similarity + 50 × title coverage, **at most 800** |
| no match | 0 |

The literal checks need a query of at least 2 characters.

**Word by word:** every key word of the query must be found in the title. For each key the best title
word counts:

| Match of one query word against one title word | Similarity |
|---|---|
| identical | 1.0 |
| the key equals the item's **year** (e.g. "2012") | 1.0 |
| prefix (cut off while typing): key ≥ 3 letters, or ≥ 2 letters if it is the last word typed | 0.9 |
| typing errors: distance `d` within the allowance | 0.85 − 0.1 × d |
| written together: the key matches this title word joined with the next one | 0.95 × the similarity against the joined word |
| cut off **and** mistyped (only if the allowance is ≥ 2): key vs. the title word cut to the key's length, with one error less | 0.7 − 0.1 × d |
| **numbers** (all digits, on either side) | only identical counts |

The distance is the *optimal string alignment* distance (Levenshtein plus swapped neighbours, so "teh" →
"the" is one error), with an early exit once it surely exceeds the allowance. The allowance depends on
the length of the query word:

| Query word length | Allowed errors |
|---|---|
| 1–3 | 0 |
| 4–5 | 1 |
| 6–9 | 2 |
| 10+ | 3 |

The "one error less" rule for cut-off words exists because, in the test set, a six-letter query word
with two errors otherwise matched the beginning of a longer, unrelated title word whose first letters
are nearly the same.

**Missing words:** a key of at most 3 letters that is not a number may be unmatched (a mistyped short word
such as "wy" for "way") – but only if at least 2 keys matched and at least 75 % of all keys matched. A
longer key, or any number, that does not match makes the score 0.

**Coverage:** the share of the title's non-stop words that were used by a match (0…1), worth up to 50
points. A query that covers the whole title wins over one that covers half of it.

**Cut-off** (`FLEXSEARCH::Cutoff`): if the best hit has a literal match (≥ 840), loose word-by-word hits
below **650** are dropped. Otherwise everything above 0 is shown.

### 3.6 Worked examples

Computed by hand from the code, for one library item with the original title **"Tears of Steel"**, year
2012, and – to show the German side – a *hypothetical* German NFO title "Tränen aus Stahl":

| Query | Best of German / original | Score | Why |
|---|---|---|---|
| `tränen`, `traenen` or `tranen` | German | 960 | all three fold to `tranen`; title starts with it, word boundary follows |
| `tears of steel` | original | 1000 | identical; the entry is labelled "[Movie] Tränen aus Stahl (2012) - Tears of Steel" |
| `tearsofsteel` | original | 840 | found without spaces |
| `tears of stel` | original | 756 | words: `tears` 1.0, `stel`→`steel` one error 0.75; coverage 1.0 → 400 + 306 + 50 |
| `tears of steal` | original | 756 | `steal`→`steel` one error |
| `steel 2012` | original | 775 | `steel` 1.0, `2012` = year 1.0; coverage 1/2 → 400 + 350 + 25 |
| `stahl tränen` | German | 783 | both words exact, order does not matter; coverage 2/3 (`aus` is not a stop word) |
| `tears of steel 2` | original | 0 | the number `2` matches nothing, and numbers may never be missing |

A hypothetical sequel "Tears of Steel II" would score 800 for `tears of steel 2`, because `ii` is read
as `2`.

### 3.7 How `DoSearch` builds the result list

```
1. ranked (FlexRank / FlexAppend):
   - all movies   (GetMoviesByWhere  videodb://movies/titles/)   \  one list, sorted by score,
   - all TV shows (GetTvShowsByWhere videodb://tvshows/titles/)  /   at most 40 entries
     label: "[Movie] <title> (<year>)" or "[TV show] …", plus " - <original title>" if the original
     title scored better
   - movie actors (GetActorsNav, movies)                             at most 10 entries
     label: "[Actor - Movies] <name>"
2. Kodi's original categories, literal as before, without entries already listed (by path):
   episodes, music videos, music video albums, movie/TV show/music video genres, TV show actors,
   music video artists, movie/TV show/music video directors, episode plots, movie plots
```

The prefixes are Kodi's localised strings (IDs 20338, 20364, 20337 and 20342), so they appear in German
with the language pack. Sorting is
stable, so equal scores keep the database order. The log shows the time of the ranked part without the
search term: `CGUIWindowVideoNav::DoSearch: <n> ranked hits in <ms> ms`.

All movies and shows are loaded and scored in memory on each search. Scoring the owner's real title list
took about 1 ms per search **on the build VM**; the time on the TV is written to the log but was not
recorded in the notes.

### 3.8 Tests

`source/kodi-port/tests/tvplayer/search/run_search_test.sh` builds `FlexFuzzySearch.cpp` with the host
compiler (`-std=c++17 -O2 -Wall -Wextra -Werror`) on the build VM and runs **60 checks** against
self-written example titles: swapped letters, missing letters in several words, the umlaut written as
`ue` together with a Roman numeral, a hyphenated title typed as one word, part of a title plus the year –
and counter-checks that must *not* match (a wrong sequel number, look-alike words).

A second mode ranks a title list of your own:

```sh
run_search_test.sh <titles.tsv> "<query>"     # lines: title<TAB>original title<TAB>year; prints the top 10
```

The owner's real list was only used locally and is not part of this repository.

### 3.9 Rollback

Copy `Custom_1107_SearchDialog.xml.pre-search` back on the TV; Kodi binary as in section 0.4. The manually
registered Global Search is already removed again (`Addons33.db.pre-globalsearch-20261118` is the state
before the experiment).

---

## 4. Artwork: pre-caching, two workers, cache on `/OAD`, no video thumbnails

### 4.1 Findings (2026-11-18)

- The library's artwork – in the owner's library 522 posters, 510 fanart images and a little over 7400 actor
  photos – is referenced as **original-size images on TMDB** (`https://image.tmdb.org/t/p/original/…`); TV
  show images are on the media server (SMB).
- Kodi caches an image only **the first time it becomes visible**: download, decode, scale down (posters
  to 480×720), store as JPEG in `userdata/Thumbnails`. On this TV that costs **about one second of CPU per
  image, on one core**. Paging through "Movies A–Z" waited for every uncached poster; only about two
  thirds of the posters had been cached so far.
- Kodi's image queue (`CTextureCache`, a `CJobQueue`) worked on **one image at a time**.
- **Thumbnail extraction from video files** (`myvideos.extractthumb`, on by default): for every movie
  without its own thumbnail, Kodi decodes a frame from the video file – for 4K HEVC over SMB **in
  software** on this CPU. 384 such images were already in the cache. These jobs **cannot be cancelled**:
  on exit, `CJobManager::CancelJobs` waits for them. On 2026-11-18 Kodi hung for **15 minutes** at
  "Stopping all", the main loop at about 30 % CPU, a `FileCache` thread still reading the video.
- **Space:** the cache was on `/APPS` (49 MB and growing). Below 150 MB free on `/APPS` Kodi does not start
  (section 0.3).
- Checked and rejected: requesting **smaller images from TMDB** (`w780`/`w1280` instead of `original`). The
  original posters are only about 240 KB each, and the actor photos (original 300×450) would even get
  bigger.

### 4.2 Changes

| What | Where | Effect |
|---|---|---|
| Pre-cache | `xbmc/video/FlexArtPrecache.h/.cpp`, called from `CGUIWindowHome::OnInitWindow()` (`xbmc/windows/GUIWindowHome.cpp`) | once per Kodi start, all posters and fanart of movies, sets, TV shows and seasons are queued in the background |
| Two images at a time | `xbmc/TextureCache.cpp` | `CJobQueue(false, 2, CJob::PRIORITY_LOW_PAUSABLE)` instead of 1 worker |
| Cache on `/OAD` | `userdata/Thumbnails` → symlink to `/OAD/flexkodi/Thumbnails`; `kodi.sh` (`thumbs_dir`) creates the target on every start | `/APPS` stays above the start limit |
| No thumbnails from video files | `guisettings.xml`: `myvideos.extractthumb` = false, `myvideos.extractchapterthumbs` = false | no software decoding of 4K frames while browsing, no 15-minute hang on exit; `myvideos.extractflags` (reading resolution/codec) stays on |

### 4.3 Pre-cache in detail

`FLEXART::StartPrecache()` runs the first time the home window opens in a Kodi run (an atomic flag makes
later calls do nothing). It adds one job at `PRIORITY_LOW_PAUSABLE`. The job opens the video database and
reads the `art` table directly, because `CVideoDatabase` has no call for "all posters":

```sql
SELECT url FROM art
 WHERE media_type IN ('movie','set','tvshow','season') AND type IN ('poster','fanart')
 GROUP BY url
 ORDER BY MIN(CASE type WHEN 'poster' THEN 0 ELSE 10 END +
              CASE media_type WHEN 'movie' THEN 0 WHEN 'set' THEN 1 WHEN 'tvshow' THEN 2 ELSE 3 END)
```

So **movie posters come first** (what "Movies A–Z" shows), then set, show and season posters, then all
fanart. For each URL it calls `CTextureCache::BackgroundCacheImage(url)`, which skips images that are
already cached and queues the rest. Actor photos (over 7400 in the owner's library) are not part of the
query. The queue's jobs are low-priority and pausable, so they pause while a video plays. Log line:

```
FlexArtPrecache: <n> posters and fanart checked in <ms> ms, missing ones are loaded in the background
```

### 4.4 Two cache workers

The only change in `TextureCache.cpp` is the worker count of the queue in the constructor (1 → 2). The
reasoning noted in the code: decoding a full-size poster takes about a second of one CPU core on the TV,
and Kodi's job manager allows two low-priority pausable jobs at a time. The jobs keep
`PRIORITY_LOW_PAUSABLE`, so they still pause during video playback.

### 4.5 Image cache on `/OAD`

`/OAD` (partition p37, ext4, read-write) is the vendor's work area for firmware updates (OTA). It was
empty on this TV, with about 950 MB free. It is only reformatted in the firmware's update mode (in that
mode `rcS` mounts an 800 MB tmpfs on `/tmp` and `/OAD` instead of the normal mounts), and firmware updates
are blocked in this project. Putting the cache there was the owner's choice. The cache is disposable: if
`/OAD` is ever wiped, Kodi downloads the images again; if `/OAD` is missing, Kodi runs but stores no
images.

The move was done on 2026-11-18 (842 files). The notes record the result, not the exact commands; these
steps are equivalent:

```sh
/APPS/kodi/kodi.sh stop
cd /APPS/kodi-home/.kodi/userdata
mkdir -p /OAD/flexkodi/Thumbnails
cp -a Thumbnails/. /OAD/flexkodi/Thumbnails/
rm -rf Thumbnails
ln -s /OAD/flexkodi/Thumbnails Thumbnails
```

`kodi.sh` makes sure the target exists on every start, but only if the link really points there (the
TV's BusyBox has no `readlink`, so it parses `ls -ld`):

```sh
THUMBS_OAD=/OAD/flexkodi/Thumbnails
thumbs_dir() {
  t="$1/.kodi/userdata/Thumbnails"
  [ -L "$t" ] || return 0
  case "$(ls -ld "$t" 2>/dev/null)" in *"-> $THUMBS_OAD") ;; *) return 0 ;; esac
  grep -q ' /OAD ' /proc/mounts || { echo "note: /OAD missing, images are not stored"; return 0; }
  mkdir -p "$THUMBS_OAD" 2>/dev/null
  return 0
}
# in start(), after the data directory is known:  thumbs_dir "$HOME"
```

Note: Kodi's texture database (`userdata/Database/Textures*.db`) stays in `userdata`; only the image files
move. The project's state backup leaves `/OAD/flexkodi/Thumbnails` out, since the cache rebuilds itself.

### 4.6 Why thumbnail extraction from video files is off

On this TV, extracting a frame for a movie without its own thumbnail means opening a 4K HEVC file over
SMB and decoding it **in software**. That is slow, keeps the CPU busy while you browse, and – the decisive
point – these jobs **cannot be cancelled**: `CJobManager::CancelJobs()` waits for them when Kodi exits.
That caused the 15-minute hang at "Stopping all".

Set, with Kodi stopped, in `userdata/guisettings.xml` (backup `guisettings.xml.pre-thumbs-20261118`):

```xml
<setting id="myvideos.extractthumb">false</setting>
<setting id="myvideos.extractchapterthumbs">false</setting>
```

`myvideos.extractflags` (stream details such as resolution and codec) stays on. (Escape hatch for any
hang on exit: holding the Power key for 5 s ends Kodi immediately.)

### 4.7 Measurements

| Measurement | Value |
|---|---|
| pre-cache scan | 1394 URLs checked in 1.2 s |
| background loading afterwards | about 70 images per minute (noted as "one thread"; whether this was measured before or after the two-worker change is not stated, and no separate two-worker figure exists) |
| free space after the move | `/APPS` 264 MB, `/OAD` 922 MB |

### 4.8 Rollback

| What | How |
|---|---|
| image cache location | stop Kodi; delete the link `userdata/Thumbnails`; copy `/OAD/flexkodi/Thumbnails` to `userdata/Thumbnails` (**check the free space on `/APPS` first**); restore `kodi.sh.pre-oad` |
| thumbnail extraction | stop Kodi, restore `guisettings.xml.pre-thumbs-20261118` |
| pre-cache / two workers | older Kodi binary (section 0.4) |

---

## 5. Home minimizes Kodi, the Kodi tile restores it

### 5.1 Behaviour

The owner's request (2026-11-18): Home in Kodi should **minimize** Kodi, so that other apps can be
started, and those apps then end Kodi.

1. **Home** in Kodi: a running film is stopped (Kodi keeps the resume point); Kodi stays visible for
   1.5 s while doing so. Then it hides, stops drawing (**0 % CPU measured**) and gives the remote back to
   the TV home screen.
2. **Kodi tile** on the home screen: brings the running Kodi back in 1–2 s, without a restart.
3. **Another app, input or launcher module** (for example the game console via the console row): the
   launcher ends Kodi as before ("module in front").
4. **Minimized for more than 10 minutes:** the launcher ends Kodi, so that the screen saver and auto-standby
   work normally again (while a Kodi heartbeat is present, the launcher suppresses its screen saver).

Power behaves as before: pressing it ends Kodi, holding it for 5 s ends it immediately.

### 5.2 The chain

```
Home pressed in Kodi
  Kodi  CEvdevInputHandler::Minimize()
        - film playing? TMSG_MEDIA_STOP, wait MINIMIZE_AFTER_STOP_MS = 1500 ms (Kodi visible, keeps remote)
        - FinishMinimize(): app thread: SetRenderGUI(false), window opacity 0 (CWinSystemDirectFB::Hide)
                            delete stale "restore", write state file "minimized", EVIOCGRAB 0
  kodi-run.sh   heartbeat flexkodi/running = "<now> <last key> <ready> 1"     (every 2 s)
  flex.js       kodiTick(): st.min -> "Kodi: minimiert (Home)", launcher keys are wanted now

OK on the Kodi tile
  flex.js       startKodi(): Kodi active and minimized -> kodiRestore(): write flexkodi/start "<unix s>"
  kodi-watch.sh fresh request -> /APPS/kodi/kodi.sh start
  kodi.sh       Kodi running and /tmp/flexkodi.d/minimized exists -> create "restore"
                ("Kodi zurückgeholt"), no second instance, no app closing
  Kodi          CheckRestore() every 0.3 s: take "restore", EVIOCGRAB 1, delete "minimized",
                write "lastkey", app thread: window opacity 0xff + RaiseToTop, SetRenderGUI(true)

Another module in front / 10 min minimized
  flex.js       write flexkodi/stop -> kodi-watch.sh -> kodi.sh stop
```

### 5.3 Kodi side (`xbmc/windowing/directfb/EvdevInputHandler.cpp/.h`)

Kodi reads the remote itself through evdev (`FLEX_KODI_INPUT=/dev/input/event0`, `FLEX_KODI_GRAB=1`). The
changes:

- `KEY_HOME` and `KEY_HOMEPAGE` map to the new action `KeyAction::MINIMIZE` **only** when minimizing is
  enabled and Kodi currently holds the grab. Enabled means `FLEX_KODI_STATE_DIR` is set (it is, by
  `kodi.sh`) and `FLEX_KODI_HOME_MINIMIZE` is not `0`. Otherwise Home keeps Kodi's normal meaning.
- `Minimize()` releases all held keys, posts `TMSG_MEDIA_STOP`, logs
  `CEvdevInputHandler::Minimize - Home: Kodi steps aside …` and either finishes immediately (nothing
  playing) or schedules `FinishMinimize()` 1.5 s later (`MINIMIZE_AFTER_STOP_MS`, why: see 5.6).
- `FinishMinimize()` posts a `TMSG_CALLBACK` to the application thread that calls
  `CApplicationPowerHandling::SetRenderGUI(false)` (nothing is drawn; the render loop sleeps 40 ms per
  frame) and `CWinSystemDirectFB::Hide()` (`IDirectFBWindow::SetOpacity(0)`). It removes a stale `restore`
  file first (an old request must not bring Kodi straight back), writes `minimized` into the state
  directory and releases the grab (`ioctl(fd, EVIOCGRAB, 0)`).
- While minimized (or about to be), **Kodi ignores every key**; the launcher has the remote.
- `CheckRestore()` runs from the input handler's periodic processing and looks for the state file
  `restore` every 0.3 s.
  If found, it takes it, grabs the remote again (if the grab fails, Kodi logs a warning and shows itself
  anyway), deletes `minimized`, updates `lastkey`, and posts the "show" callback: `Show(true)` = opacity
  0xff plus `RaiseToTop`, `SetRenderGUI(true)`, mark the GUI dirty. Log:
  `CEvdevInputHandler::CheckRestore - Kodi tile: Kodi is back (exclusive grab: yes)`.

Hiding the window with opacity instead of destroying it keeps all of Kodi's state (current window, list
positions, resume points); the DirectFB window simply stops covering the launcher.

### 5.4 Script side

- **`kodi-run.sh`** (heartbeat): the heartbeat file `flexkodi/running` gained a 4th field:
  `"<now> <last key> <ready> <minimized>"`, all from state files Kodi writes into `/tmp/flexkodi.d`
  (`lastkey`, `ready`, `minimized`). It is written every 2 s through a randomly named temp file and `mv`
  (the folder is reachable by the launcher user, so symlink tricks are guarded against). `minimized` and
  `restore` are removed at the start and end of every run.
- **`kodi.sh start`**: if Kodi already runs and `minimized` exists, it only creates `restore` and prints
  "Kodi zurückgeholt" ("Kodi brought back"); without `minimized` it prints "Kodi läuft schon" ("Kodi is
  already running"). This check comes before the code that closes paused apps, so restoring never ends
  other apps.
- The tile's path is unchanged: `flex.js` writes `flexkodi/start` → `kodi-watch.sh` (accepts only fresh
  requests, at most 5 s old) → `kodi.sh start` → `restore` → Kodi.

### 5.5 Launcher side (`flex.js`, in the root-mod repository)

Functions touched: `kodiState` (reads the 4th field as `st.min`), `kodiMinimized`, `kodiRestore`,
`startKodi`, `kodiTick`, `onKey`.

- While minimized, keys at the launcher do **not** count as "Kodi is exiting" (normally a key reaching the
  launcher means Kodi has released the remote because it is closing, and the hint "Kodi wird beendet …" is
  shown). A hint left over from the seconds before the next heartbeat is removed again.
- While minimized, the launcher does not refresh its screen-saver/auto-standby timers on Kodi's behalf.
- After `KODI_MIN_STOP_MS = 10 * 60 * 1000` minimized, `stopKodi('minimiert seit 10 min')`.
- OK on the Kodi tile with a minimized Kodi writes the start request (`kodiRestore`) instead of showing
  "Kodi läuft schon". As for a normal start, the Denon module's `denonBeforeStart` runs first, which
  switches the AV receiver to TV audio.
- When the heartbeat shows `min = 0` again: log "Kodi: wieder vorne" ("Kodi in front again").

### 5.6 The TV player hang after Home during a film, and its fix

**Finding (2026-11-18):** a 4:3 film was playing through the TV's own player. For aspect ratios other than
16:9, `flex.js` sets its own output window on the TV player (`setScreenPosition`). Home was pressed after
17 s. At the same moment `flex.js` reset the window and stopped the TV player, and Kodi hid itself. The
TV's log showed `Begin to call Mm_Stop()` and then nothing until the switch went through about **100 s**
later (`media player -source out- done`). For that whole time the TV stayed on the DMP source: the film
was visible, stretched to full width, and when the game console was selected the TV
said "no HDMI signal" while the console's sound already came through the AV receiver. Counter-check by the
owner: a normal stop of the same film with Kodi visible was immediate.

This involved two asynchronous lifecycle operations. The window must be cleared while the session
is valid and the stop must finish before Home or HDMI takes ownership.

**Fix, in two places:**

- `flex.js` `kpEnd()`: with an own window, reset the window first and send the stop **0.4 s later**
  (`KP_WIN_STOP_MS = 400`, held in `kpPendingStop`). A new playback request flushes a pending stop
  immediately (`kpStart` → `kpFlushStop`). If the TV player is meanwhile playing a different medium, the
  delayed stop is dropped. Films without an own window (16:9) are stopped immediately as before.
  `test1.js` checks the state "window already reset, stop not yet sent".
- Kodi `Minimize()`: if a film is playing, stop it first and stay visible with the remote for 1.5 s
  (`MINIMIZE_AFTER_STOP_MS`, `FinishMinimize`), so hiding never coincides with the player teardown.

**On the TV afterwards:** same film, Home during playback, then the game console: `Mm_Stop` began, the
switch to HDMI 1 went through with the console selection after a little over 10 s, no hang.

### 5.7 Tests and what was seen on the TV

- `mods/startscreen/tests/test-minimize.js` in the root-mod repository (`node test-minimize.js
  [path/to/flex.js]`), **14 checks**: minimized without the exit hint; a short hint before the first
  heartbeat disappears again; the tile restores (start request, no "Kodi läuft schon"); "in front again"
  is detected; not minimized, a key still means "Kodi is exiting"; an app ends a minimized Kodi; still
  running after 9 min, ended after 10 min. The other launcher tests (`test1`, `test3`–`test6`,
  `test-dmpactive`) pass unchanged.
- **Seen on the TV (2026-11-18):** `CEvdevInputHandler::Minimize - Home: Kodi steps aside …`, `flex.js`
  "Kodi: minimiert (Home)", heartbeat ending in `… 1 1`, Kodi at 0 % CPU. Then the game console selected →
  "Kodi: beenden (Modul vorne: LiveTV …)", Kodi ended after 6 s. Minimizing during a 4:3 film was also
  seen (after the fix in 5.6).
- **Not yet verified on the TV:** bringing Kodi back with the Kodi tile. The mechanism is covered by the
  launcher test and by the code path above, but at the time of writing the notes record no test on the
  TV itself.
- Cosmetic: when Kodi ends, the launcher logs "Kodi: wieder vorne" once, because the state file
  `minimized` disappears before the last heartbeat does.

### 5.8 Switching off and rollback

- Only disable minimizing: in `kodi.sh` add `export FLEX_KODI_HOME_MINIMIZE=0`. Home then behaves as in
  stock Kodi.
- Full rollback: on the TV restore `kodi-directfb.pre-min`, `kodi.sh.pre-min` and `kodi-run.sh.pre-min`;
  for the launcher restore `flex.js.prev` and reload the launcher.

---

## 6. Open points and unverified items (summary)

| Item | State |
|---|---|
| Restoring a minimized Kodi via the Kodi tile | implemented and unit-tested; not yet verified on the TV |
| Add-on installation from the Kodi repository | fails (no CP437 gconv module on the TV); manual unpacking works; no fix yet |
| Python add-ons (Global Search, YouTube, scrapers) | impossible in this build (`ENABLE_PYTHON=OFF`) |
| Search time on the TV | logged by Kodi, not recorded in the notes (≈1 ms scoring on the build VM) |
| Image throughput with two workers | not measured separately (≈70 images/min recorded with one worker) |
| Phone keyboard and phone page texts | German only, hard-coded |

Other Kodi additions for this TV – the AV receiver's volume in Kodi's volume ring, audio transcoding for
the TV player path – are described in their own chapters (German notes: `de/LAUTSTAERKE-BALKEN.md`,
`de/TONFORMATE.md`).
