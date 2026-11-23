# Frame rate and video

This chapter details the Kodi-side route selection, geometry, measurement and known
limitations. The [Hisense VIDAA mod](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod) includes
the native interface, standalone lab data and full decoder-chain description for the 55U8QF reference firmware.

Kodi's DirectFB graphics plane is 1080p. The UHD video plane is rendered separately by
the native player. The controller verifies ownership and uses an alpha-cleared graphics
region to reveal video. The graphics cadence is independent of the content. The measurements
did not establish a universal fixed 60 Hz graphics contribution.


## 1. Summary

| Path | Who decodes | Picture plane | 23.976p film is shown at | Used for |
|---|---|---|---|---|
| **ES path** (stage 2, since 2026-11-05) | TV hardware decoder, fed by Kodi's demuxer through `flex_es_helper` | 4K video plane, TV picture processing | **native ~24 Hz** (vendor branch `mstplayer`) | default for progressive H.264, HEVC, VC-1, MPEG-2 without subtitles |
| **TV player** (stage 1, since 2026-10-29) | TV hardware decoder; the TV's own file player reads the file over a loopback HTTP feeder | 4K video plane, TV picture processing | **59.94 Hz, 3:2 pulldown** (vendor branch `vdplayer`) | subtitles on, interlaced video, 3D, fallback when the ES path fails |
| **Kodi VideoPlayer** (stock Kodi) | FFmpeg in software on 4× 32-bit A55 cores | 1920×1080 GUI plane, fixed 60 Hz, no TV picture processing | 60 Hz (and slow for HD) | DVD menus/ISOs, Blu-ray folders, PVR, adaptive streams, add-on streams, fallback up to 1080p |

Key facts, in the order we learned them:

1. The panel, the scaler and the timing generator **can** do 23.976 Hz: Netflix plays a 24p film with
   IRQ 51 = 23.980/s and the scaler line `Vfreq=239` (measured 2026-10-29).
2. The TV's file player **always** turns 23.976/24p into 59.94 Hz, in 1080p and 2160p, over HTTP and
   `file://`. No menu setting, no profile entry, no app type and no user ID changes that (measured,
   §4).
3. Netflix gets 24 Hz because it does not hand the service a file. It **demuxes itself and pushes
   elementary streams** (ES) into a media item whose description contains the frame rate in mHz. Any
   process can do the same through the TV's Binder client library.
4. Our own ES client reached **IRQ 51 = 23.970/s** (`Vfreq=239`), the same values as Netflix (measured
   2026-11-04). In a 120 s window a film with exactly 24.000 frames/s gave 23.998/s, so the TV really
   follows the film's rate.
5. With a Binder bug in our client fixed, **4K HEVC Main 10 at 50 and 80 Mbit/s average** (152 Mbit/s in
   the strongest second) played without a late frame and without a visible glitch (2026-11-05).
6. Kodi got this as the **ES path**: `CTVEsEngine` in Kodi plus the separate process `flex_es_helper`.
   The helper uses the same call sequence as the measured client. A separate IRQ 51 log of a film
   *started from Kodi* has **not** been recorded (see §5.6).

---

## 3. Measuring the real output rate

The TV's getters do not tell the output rate. `modeljs.video.getVideoFormatInfo()` reported
"…/60Hz" during 50 Hz playback and "720*576p/60Hz" while the scaler received 576i at 50 Hz. The menu
shows nothing. Two sources are reliable.

### 3.1 Interrupt rates (primary indicator)

`tools/irq24.sh` (root-mod repository) reads `/proc/interrupts` twice over a window and prints every line
with 5–300 interrupts per second. It only reads.

```sh
python tools/tvctl.py putlf tools/irq24.sh /tmp/irq24.sh
python tools/tvctl.py run "sh /tmp/irq24.sh 60"     # 60 s window
```

| IRQ | Name | Behaviour on this kernel build |
|---|---|---|
| **51** | (none) | appears **only while video plays** and runs at the video path's output rate (59.94, 50, 30 or ~24/s). Working interpretation: MVOP frame interrupt |
| 42 | `scaler` | ~120/s idle, 239/s with the TV player at 60 Hz, ~167–168/s with Netflix and our ES client at 24 Hz. Fingerprint only |
| 52 | `gop, local dimming` | ~120/s at 60 Hz output and also with Netflix at 23.976 Hz; ~110/s at 50 Hz output. Not an output indicator |

On another firmware the numbers may differ; look for the line that appears only while a video plays.
Measure at least 30 s after the start (a clip change once made IRQ 51 dip to 14.5/s).

**Precision.** `irq24.sh` counts whole interrupts. An 8 s window (±0.125/s) separates 24 from 25/30/60
Hz, but not 23.976 from 24.000; every "23.97" from an 8 s window means "about 24". A 120 s window
(±0.008/s) is needed to see the difference.

### 3.2 The scaler's input line in the kernel log

```sh
python tools/tvctl.py run 'dmesg | grep -a "BW Window" | tail -3'   # read only, never dmesg -c
```

Example: `BW Window … PQSource:9, Hsize:3840, Vsize:2160, interlace:0, Vfreq=250`. `Vfreq` is in 0.1 Hz
(239 = 23.9 Hz, 250 = 25 Hz, 500 = 50 Hz, 599 = 59.9 Hz), `interlace:1` marks interlaced input,
`PQSource:9` is the "media" input (its picture settings apply), `PQSource:6` was an HDMI input.

**Caveat:** this describes the scaler's **input**, not the panel output. A `file://` run of a 23.976p
clip through the TV player showed `Vfreq=239` while IRQ 51 counted 60.07/s. Always read IRQ 51 first.
The kernel ring buffer is small; read it soon after the event.

### 3.3 Which decoder was started

`dmesg | grep -a MApi_VDEC_EX_V2_Init` shows `CodecType:n` for the decoder the service started. We used
it to map codec values (§8.1).

---

## 4. Why the TV player shows 24p at 59.94 Hz

### 4.1 Measurements (2026-10-29)

Test clips played through the launcher's own `PlayMedia` call (the call the TV's USB player and DLNA
use), HTTP over 127.0.0.1, motion settings unchanged:

| Clip | Path | Rate reported by the player (`getMetadata()[39]`) | IRQ 51 | Output |
|---|---|---|---|---|
| synthetic judder clip 23.976 (MPEG-2 1080p) | HTTP | 23976 | 59.94/s | 59.94 Hz, 3:2 |
| same, 24.000 | HTTP | 24000 | 59.94/s | 59.94 Hz, 3:2 |
| same, 25 | HTTP | 25000 | 50.05/s | **50 Hz, even 2:2** |
| 50p (MPEG-2 720p) | HTTP | 50000 | 50.00/s | 50 Hz |
| 59.94p (MPEG-2 720p) | HTTP | 59947 | 59.94/s | 59.94 Hz |
| 23.976 clip from `/tmp` | `file://` (like the USB player) | 23976 | 59.94/s | the path changes nothing |
| 4K HEVC 25p test file | HTTP | 25000 | 50/s | 50 Hz |
| 4K HEVC clip with VUI tick rate 24000/1001 | HTTP (TS) | 23976 | 59.93/s | **4K 24p also runs at 60 Hz** |
| VC-1 1080p23.976 Blu-ray excerpt, only container timestamps changed | HTTP | 23976 | 59.94/s | the TV reads the rate from the VC-1 sequence header |
| same excerpt, sequence headers and Matroska `DefaultDuration` re-flagged to 25/1 (test clip only) | HTTP | 25000 | 49.95/s | 50 Hz, even 2:2 |

Conclusions: the file player outputs 23.976p and 24p **always** at 59.94 Hz; 25p/50p at 50 Hz and
30p/60p at 60 Hz are fine. The rate comes from the video bitstream (VC-1 sequence header, HEVC VUI),
not from the container.

### 4.2 Where the rule lives (static analysis)

- `libmmsdk.so`, inside the mailbox handler that serves only `vdplayer` (function `0x1ef44`–`0x28034`;
  mailbox calls exist only in `libvdplayer.so` and as imports in `libmmsdk.so`, not in `libmmp.so` or
  `libOMX.MStar.so`): 23.5–24 fps → ×5/4 → ×2 = 59.94 (`0x23a40`–`0x23ab4`).
- `libvdplayer.so`: decoder output rate from a table — 25 or 50 → 50, everything else → 60
  (`cmp r1,#0x19; cmpne r1,#0x32; moveq r1,#0x32; movne r1,#0x3c`). The log text "FRC_OUTPUT Set
  OutputRate" exists only there.
- Branch B (`mstplayer`) has no such rule. Its only forced rate is the profile value, which is 0 for
  Netflix' profile. The vsync bridge sets the MVOP timing from the frame info's rate, DMS uses the content
  rate when nothing is forced, and the MVOP driver contains "refine 4k 23976 timing".

### 4.3 Visible cadence

A 23.976p source in the 59.94 Hz family has uneven 3:2 cadence in the internal video path,
which corresponds to judder in slow pans. The 6m70 sits between the SoC and the panel.
A 120/s GOP interrupt does not establish the physical repeat pattern.

### 4.4 Configuration experiments

Media configuration tests left progressive file playback in the 59.94 Hz family. An
interlaced mode produced near 48 fields/s and introduced field artefacts, so it did not
provide useful progressive 24p. The port uses the native ES interface instead.

## 5. Standalone ES evidence


The standalone clear ES client reached about 23.970 IRQ 51/s for 23.976p material.
The exact 24p sample gave 23.998/s across 120 seconds and `Vfreq=240`. A separate
paired measurement of the final integrated Kodi route is pending. Use the Hisense
[cadence measurement procedure](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/02-hardware-decoder.md#reproduce-the-cadence-measurement) to reproduce the standalone results.


### 5.1 Native app comparison and player profiles

The TV file player uses `[app0]` for local files and `[app2]` through GStreamer `mdmp` for
DLNA and Kodi's HTTP fallback. Both produced about 59.94 Hz for the tested film clips.
Kodi's DMP feeder checks the locally expected TV player account and a per-run URL token.
The separate ES item carries the codec and rate itself. Kodi passes `fps_mhz` to the
helper, which writes field `+0x5c` before starting the native session.

The native Netflix film gave 23.980 internal video interrupts/s. Static inspection of
`libstplayer` ties that route to an ES item in `[app1]`, with content-led timing.
The standalone clear ES test gave 23.970/s on the same hardware. The YouTube, Prime
and browser/HbbTV configuration entries are useful static references. The paired
measurements cover Netflix, DMP and the standalone clear client. The Hisense guide
[Player profiles and cadence](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/07-player-profiles-and-cadence.md) describes the account/profile/session distinction, full comparison and evidence limits.

## 6. The ES path inside Kodi

Source: [`source/kodi-port/kodi-tree/xbmc/cores/TVPlayer/`](../source/kodi-port/kodi-tree/xbmc/cores/TVPlayer/)
(`TVPlayer.cpp`, `TVEsEngine.*`, `TVEsProtocol.h`, `TVAudioTranscoder.*`) and
[`source/native/flex_es_helper.c`](../source/native/flex_es_helper.c).

### 6.1 Architecture

```
Kodi (root, /APPS/kodi)                         flex_es_helper (glibc, vendor libs)        MiMwIpcSrv
CTVPlayer --probe, path choice-->               dlopen libdrvIPAUTH, libmsrv_mmplayer_ipc  mstplayer branch
CTVEsEngine (thread)                            MM_CreateMediaItem / MM_PushPayload ...  -> VDEC, MVOP, XC
  Kodi input stream + demuxer (SMB, HTTP, USB)
  Annex B, VC-1 fix-ups, AC-3 transcoding
  ---- fd 3, SOCK_STREAM: es_rec + payload ------------------->
  <--- fd 4, SOCK_SEQPACKET: es_cmd -> / <- es_status --------->
file bridge (play.json / ctl / player.json, HMAC-signed, tmpfs)
  <-> flex.js (launcher): input -> DMP, home screen transparent, busy TV player stopped, "esready"
```

Sequence of one playback:

1. `CTVPlayer::Prepare` probes the file with Kodi's demuxer (tracks, DAR, fps, interlace, DV, 3D) and
   decides the path (§7). Log: `CTVPlayer::Prepare - ES path` or `CTVPlayer::Prepare - TV player: <reason>`.
2. Kodi writes an ES job (`play.json` with `es:1`) to the launcher mod through the signed file bridge.
3. `flex.js` switches the input to DMP with the launcher's own call, makes the home screen transparent
   (class `flex-kp-video`). If the TV player is busy (any state other than idle/ended/error), it stops it
   and waits `KP_ES_SETTLE_MS` = 2.5 s (on 2026-11-05 an immediate stop hit the freshly created item and
   the first video had no picture). Then it reports `esready` (log: `kodiPlay: Quelle bereit, Kodis Helfer spielt`, "source ready, Kodi's helper
   plays").
4. `CTVEsEngine` spawns the helper, sends `ES_OP_OPEN` (codec codes, fps in mHz) and, for non-16:9 films,
   `ES_OP_RECT`, then demuxes and streams packets.
5. The helper reports state, position (`MM_GetVideoPTS`) and counters every 200 ms; Kodi uses them as the
   player state. Kodi turns its GUI transparent over the video plane 1 s after "playing"
   (`REVEAL_DELAY`; until then it draws opaque black).

### 6.2 Protocol (`TVEsProtocol.h`)

| Message | Fields | Meaning |
|---|---|---|
| `es_rec` (data socket) | `type` (0 audio, 1 video, 2 end of stream), `size`, `gen`, `pts_ms` + payload | one packet; records of an older *generation* are dropped |
| `ES_OP_OPEN` | `vcodec` (`+0x50`), `acodec` (`+0xa0`), `fps_mhz` (`+0x5c`), `gen` | create the first media item |
| `ES_OP_PAUSE` | `flags` 1/0 | `MM_SetTrickMode` `{1,1}` / `{0,1}` |
| `ES_OP_FLUSH` | `gen`, flag `ES_FLUSH_RECREATE` | seek inside the item, or a new item (audio codec change) |
| `ES_OP_RECT` | `x, y, w, h` on 3840×2160 | display window (§11) |
| `ES_OP_STOP` | – | tear down and exit; EOF on the control socket means the same |
| `es_status` | state (connect, preload, playing, paused, ended, error), `gen`, `pts_ms`, `buf_ms`, `err`, counters video/audio/retried/lost/decode errors | helper → Kodi |

Error codes: 1000 vendor library or symbol missing, 1001 `MM_CreateMediaItem` refused (another player
holds the service), 1002 no `INIT_OK` within 10 s, otherwise the service's event number (4 init failed,
17 audio unsupported, 18 not supported).

### 6.3 Video handling in `CTVEsEngine`

- **Input:** Kodi's own input stream and demuxer read the file (SMB, HTTP, local). The TV player's
  loopback HTTP feeder is not involved.
- **H.264/HEVC:** converted to Annex B with Kodi's `CBitstreamConverter`; **Dolby Vision RPUs are
  removed** (`SetRemoveDovi(true)`), the TV gets the base layer.
- **VC-1 from Matroska:** the sequence header from the codec private data is prepended to the first
  frame (without it the decoder shows nothing). Matroska stores VC-1 in VFW mode with decode-order
  stamps only (`pts=N/A` in ffmpeg); passing them as PTS would garble the B-frame display times. The
  engine detects this (stamps that only grow at a B-frame) and rebuilds display times from the picture
  types: an anchor (I/P) is shown after the B-frames that follow it in the stream. B-frames directly
  after the first key frame of a cut belong to the missing group before it and are dropped. Real PTS
  (MPEG-TS) are passed through.
- **Missing stamps** (containers that stamp only some frames): filled at 1/fps after the last one.
- **After a seek:** packets are dropped until a real key frame — H.264 IDR **or SPS** (open-GOP streams
  have no IDR), HEVC IRAP or VPS/SPS, VC-1 sequence header or entry point, MPEG-2 sequence header. Limit
  2000 packets (about a minute), then play on. No audio is sent before the first picture (the service
  would wait for its video).
- **Frame rate:** `EsFrameRate(fps) = round(fps × 1000)` for 1 < fps < 121; otherwise the film is not
  sent to the ES path.

### 6.4 Pacing and buffer control in the helper

- `FLEX_ES_PREROLL_MS` (default 1200): data pushed before `MM_Play`.
- `FLEX_ES_TARGET_MS` (default 1500): lead kept between the last pushed video PTS and the playback
  position (`MM_GetVideoPTS`). `MM_GetVideoDataBufferInfo` kind 3 (behaves like ms in the buffer) jumps
  too coarsely (1.5–3 s at a 1.5 s target) and is only a fallback right after a seek.
- A refused push (full ES buffer) is retried every 5 ms for up to 2 s, like `libstplayer`'s "sample
  retry"; then it counts as lost.
- `MM_Play` runs in its own thread, no status queries are made while it is inside the service, and
  `MM_Stop` is never called from the push thread (a push can block inside the service until the stop) and
  exactly once per item.
- End: `MM_SetDataEnd`, then "decode done" (event 9) **or** a position that has not moved for 3 s.
- The helper closes every inherited descriptor above 4 at start (Kodi would otherwise hand it sockets and
  its exclusive grab on the remote). Kodi starts it with `posix_spawn` + vfork (Kodi's address space is
  far too large to copy) and the environment:

```
LD_LIBRARY_PATH=/tvservice/mslib:/tvservice/conflictlib:/mslib/utopia:/mslib
LD_PRELOAD=libmi_glibc.so libmdroid.so libmmapinfo.so libiniparser.so libdrvIPAUTH.so liblinux.so libmsrv_mmplayer_ipc.so
```

  On the owner's TV `lib/kodi/flex_es_helper` is a small wrapper that starts the binary with `env -i`
  (only `PATH`, `LD_LIBRARY_PATH`, `LD_PRELOAD`), so none of Kodi's environment reaches the vendor
  libraries. The helper log is `/tmp/flexkodi.d/es-helper.log`.

### 6.5 Seek, pause, audio track change

- Seek inside an item: new generation, `MM_SetFlushData(item, 2)` (audio and video), wait for event 24
  (max. 3 s). Kodi counts the seek as done when the helper reports the new generation within 15 s of the
  target (key frames can be that far before it), or after a 5 s timeout.
- Audio track change: the codec may change, so a **new media item** is created at the current position
  (2 s back). On a Blu-ray rip with 14 tracks this worked with 1.4 s buffer and no decode errors.
- Audio on this path: AC-3/E-AC-3 go unchanged (code 5); every other track FFmpeg can decode is
  converted to AC-3 on the fly (5.1 at 640 kbit/s, stereo at 256 kbit/s). Details are in the [audio chapter](04-audio.md).

---

## 7. Path choice: ES path, TV player or Kodi's VideoPlayer

### 7.1 Order of decisions (code)

`CTVPlayer` is a Kodi `IPlayer` (registered as `TVPlayer` in `system/playercorefactory.xml`). The
system file has no rule that selects it; on the owner's TV a test rule in
`userdata/playercorefactory.xml` prepends it for video files:

```xml
<playercorefactory>
  <rules action="prepend">
    <rule name="tvplayer-test" video="true"
          filetypes="mkv|mp4|m4v|ts|m2ts|mts|avi|mov|webm|mpg|mpeg" player="TVPlayer" />
  </rules>
</playercorefactory>
```

(No `<player>` element in userdata: it would create a second player "TVPlayer1".) Then:

1. **`OpenFile` → `RejectImmediately`** → straight to Kodi's VideoPlayer: input-stream add-ons
   (`inputstream`, DRM stays out), DVD/Blu-ray structures and disc images (`disc`), PVR, non-video,
   adaptive streams (`.m3u8 .mpd .ism .isml`), bridge to the launcher not usable (`noBridge`), file the TV
   already refused in this Kodi run (`rejectedBefore`).
2. **Probe** with Kodi's demuxer.
3. **`TvCanPlay`** (TV capability, both hardware paths):
   - AV1 → not on the TV;
   - Dolby Vision without a base layer (profile 5 or reserved compatibility id) → refused with a message
     (`dvp5`; wrong colours otherwise). Profiles 7 and 8.x play their base layer;
   - H.264 with more than 8 bit or 4:2:2/4:4:4 ("Hi10P") → not on the TV;
   - no audio track the TV player can play (only TrueHD/FLAC/Opus/Vorbis): up to HD → Kodi's player;
     above HD → plays with a notice "only <codec> audio: the TV plays the picture without sound".
     Note: in the current code this check runs before the ES check, so an HD file whose *only* audio is
     TrueHD/FLAC goes to Kodi's VideoPlayer, not to the ES path with conversion (code reading).
4. **`CheckEs`** → ES path unless one of the reasons in §7.2 applies.
5. TV player path only: a visible **external** subtitle file → Kodi's player if allowed (`extSub`).
6. **Failure handling:**
   - ES path fails before playing (`es`, `prepTimeout`, `bad`) → the file is remembered for this Kodi run
     and the **TV player** takes over, toast "Echte Bildrate hier nicht möglich, der TV-Player übernimmt"
     ("real frame rate not possible here, the TV player takes over").
   - TV player refuses or fails before playing → **Kodi's VideoPlayer** if `FallbackAllowed()`, otherwise
     an error message with a way out (E2).

### 7.2 Why a film does not take the ES path (log text after `TV player: `)

| Reason | Why |
|---|---|
| `switched off` | `<es>false</es>` in the `TVPlayer` player element, or `FLEX_KODI_NO_ES=1` in Kodi's environment (default: on) |
| `no probe` | the probe failed |
| `failed before` | this file already failed on the ES path in this Kodi run |
| `video <codec>` / `video <codec> interlaced` | only progressive H.264, HEVC, VC-1 and MPEG-2 are mapped; interlaced material is untested on this path and stays with the TV player |
| `frame rate unknown` | no usable rate for `+0x5c` |
| `3D` | stereo modes are not handled |
| `no audio track for this path` | no AC-3/E-AC-3 track and nothing FFmpeg can convert (also: a file without audio) |
| `subtitles on` | the ES path has **no subtitles yet**; if the TV player can play the same audio language, the film goes there |
| `audio language only as <codec>` | the wanted language exists only in a format neither path can play |

If subtitles are wanted but the wanted language exists only in a format that needs conversion (TrueHD,
FLAC …), the sound wins: ES path without subtitles, toast "Untertitel aus: diese Tonspur geht nur ohne
Untertitel" ("subtitles off: this audio track only works without subtitles").

**Practical consequence:** Kodi's subtitle language setting decides the path. With the default
("original"), Kodi enables a forced/default subtitle track, and such a film plays through the TV player
at 59.94 Hz. Turning subtitles off before starting sends a progressive film to the ES path.

### 7.3 When Kodi's own player may take over

`FallbackAllowed()` (code) only where Kodi's software decoder copes:

| Condition | Default |
|---|---|
| `<fallback>` | `true` |
| width ≤ `<fallbackmaxwidth>` | 1920 |
| not HEVC Main 10 / more than 8 bit | – |
| not AV1/VP9 above 1920×1088 | – |

Above that, the user gets an error toast instead of a VideoPlayer that would stall. Remember §2.3: even
1080p VC-1 stalls in software, so the fallback is a safety net, not a quality path.

### 7.4 The three paths compared

| | ES path | TV player | Kodi VideoPlayer |
|---|---|---|---|
| 23.976/24p output | **~24 Hz native** | 59.94 Hz (3:2) | 60 Hz (GUI plane) |
| 25p/50p | content-led (scaler input 25 Hz seen; output not measured) | 50 Hz (measured) | 60 Hz |
| Decoder | TV hardware | TV hardware | FFmpeg software |
| Scaling / picture processing | TV (XC), "media" picture settings | TV (XC), same settings | Kodi bilinear to 1080p, then simple 4K stretch, no TV processing |
| File access | Kodi (SMB/HTTP/USB) | TV player over Kodi's loopback HTTP feeder (accepts only uid 2910, the TV player, plus a per-run token) | Kodi |
| Subtitles | none yet | drawn by the TV | Kodi |
| Interlaced material | not sent here | TV deinterlacer | software bwdif |
| Audio | AC-3/E-AC-3 pass-through, everything else converted to AC-3 | TV formats only (no TrueHD/FLAC) | Kodi |
| Window for non-16:9 | `MM_SetDisplayRectangle` in the item | `setScreenPosition` through the launcher | n/a (inside Kodi's window) |
| Kodi's video settings (scaling, sharpness, deinterlace, refresh rate) | no effect | no effect | apply |

Diagnosis from outside: `XBMC.GetInfoLabels` → `Player.Process(videodecoder)` shows `TV ES hevc`
(ES path), `TV mpeg2video` (TV player) or FFmpeg's name. Under the TVPlayer, `Player.GetItem` returns an
empty file and empty stream details; use `Player.GetProperties` (`currentvideostream`) and the Kodi log
(`CTVPlayer::Prepare`).

---

## 8. Supported codecs

### 8.1 ES path video codes (`ST_MEDIA_INFO +0x50`)

Mapped on the TV with `tools/es_codecprobe.sh` (root-mod repository): create an item without `MM_Play`,
push a preroll, read `CodecType` from `MApi_VDEC_EX_V2_Init` in the kernel log; 13 runs, no crash.

| `+0x50` | VDEC `CodecType` | Decoder | Used by Kodi (`EsVideoCode`) |
|---|---|---|---|
| 1 | 1 | MPEG-2 | yes, progressive only |
| 2 | 3 | MPEG-4 | no |
| 7 | 11 | H.264 | yes |
| 8 | 12 | AVS | no |
| 11 | 13 | MJPEG | no |
| **12** | **7** | **VC-1 Advanced** | yes |
| `0x14` | – | HEVC (from `libstplayer`'s enum, not probed; 10-bit verified in playback, §9) | yes |
| `0x15` | – | VP9 (from `libstplayer`, not probed) | no |
| 3, 4, 5, 6, 9, 10, 13 | – | no decoder started (with the H.264 test clip) | – |

Verified in playback on the ES path: H.264 1080p with B-frames, VC-1 Advanced 1080p from a Blu-ray (VFW
and B-frames), HEVC Main 10 2160p up to 80 Mbit/s average, progressive MPEG-2 DVD rips (picture seen;
output rate not measured). An early mistake: `0x14` was first taken for H.264; it is HEVC.

### 8.2 TV player

The TV's decoder list (`media_codecs.xml`) and library strings list MPEG-2, H.264, VC-1 and HEVC (10-bit,
HDR10) up to 2160p. AV1 is neither in the decoder list nor among the Matroska codec IDs, so Kodi rejects
it. Audio on that path: AC-3/E-AC-3, DTS, AAC, LPCM; FLAC is refused, TrueHD is reported as "UNKNOWN"
and stays silent.

### 8.3 Not on the TV at all

| Material | Handling |
|---|---|
| AV1 | Kodi's player up to 1080p, otherwise a message |
| H.264 Hi10P / 4:2:2 / 4:4:4 | Kodi's player up to 1080p |
| Dolby Vision profile 5 | message (`<dvprofile5>true</dvprofile5>` forces it to the TV anyway) |
| DVD menus (ISO, `VIDEO_TS`), Blu-ray folders | Kodi's player (libbluray is not built) |
| HDR on the ES path | untested |

---

## 9. 4K HEVC 10-bit up to 80 Mbit/s

### 9.1 First 4K run: "picture sharp, judders extremely"

Tears of Steel, HEVC 4K CRF 18 (~30 Mbit/s), streamed into the test client **over SSH** from the PC:
574 frames refused, ~8500 late. The data came too slowly: ~4 MB/s through the test SSH transfer (7 % CPU), at second 60
the delivery was 15 s behind, and the service refuses late frames. Counter-check with a Raspberry Pi 5 on
the cable serving the clip over plain HTTP (`tools/es_mediaserver.py`, `es24p.py run --url …`): raw HTTP
11.7 MB/s — the maximum of the TV's **100 Mbit/s** Ethernet (`/sys/class/net/eth0/speed` = 100) — and
**0 late, 0 refused**. SSH is not a usable feed path for tests. Kodi reads directly (SMB) and is not
affected.

### 9.2 The glitch hunt and its real cause

From the Pi, 3–4 short picture errors remained, always at the same film positions. Narrowing down
(2026-11-04/05):

| Test | Result | Conclusion |
|---|---|---|
| HEVC 4K CRF 18, peak 61 Mbit/s, largest frame 1.24 MB | 3–4 errors, same places | baseline |
| same, all picture processing off (contrast, NR, smoothing, Clear Motion) | errors stay | not the picture processing |
| kernel `_KDrv_XC_GCE_WriteDlcCurveAdl … FAILED` | ~1/min idle, 0 during the film | not the DLC curve |
| playback position logged 20×/s | 0 jumps | no frames skipped in time |
| software decode of the clip, PSNR against the source | no error, no single-frame drop | the clip is clean |
| H.264 1080p, same scene | owner: "not a single error" | tied to the HEVC 4K version |
| HEVC 4K, same encoder settings, 16 Mbit/s, largest frame 0.84 MB | owner: "perfect" | the largest frame stays below a limit |
| short lead (500 ms), 0 late | errors stay | not the ES buffer |
| only the tier bit changed from High to Main | errors stay | not the tier flag |

The cause was in our client. The kernel logged, for every oversized frame:

```
binder_alloc: <pid>: binder_alloc_buf size 1124144 failed, no address space
binder_alloc: allocated: 40 (num: 4 largest: 28), free: 1040344 (num: 3 largest: 1040320)
binder: <client>: transaction failed 29201/-28, size 1124144-0 line 3070
```

The service has about 1 MB of Binder address space. The failed sizes were exactly the clip's largest
frames plus 84–85 bytes (5 of 5 in a 45 s run, byte-exact) — key frames with VPS/SPS/PPS, so the decoder
lacked its reference until the next key frame (up to 2 s). The client library already copies the frame
from `info.addr` into shared memory, in chunks of up to 2 MB (`MMPlayer::MM_PushPayload64`, Binder call
`MM_PushPayloadPartial`). The last two arguments of `MM_PushPayload(item, &info, extra, extra_size)` are
**extra data inside the Binder parcel**. Our client had passed the frame there a second time, so every
frame went twice, and from ~1.04 MB the whole call failed. Because the empty reply is read as 0, the
error was invisible in the return value, and on long runs the kernel ring buffer had already overflowed.

**Fix:** `mm.push(obj, item, &di, NULL, 0)`. Same 30 Mbit/s clip, 60 s from the Pi: 0 Binder errors, 0
jumps, 0 late, IRQ 51 = 23.97/s, 1464 frames and 1907 audio packets; owner: "without bug". (`libstplayer`
uses the variant without extra data on its normal path, so streaming apps are probably not affected; not
measured on Netflix.)

### 9.3 UHD-Blu-ray-like data rates (2026-11-05)

HEVC Main 10, level 5.1 High tier, 60 s each from the Pi (`tools/es_tosclip.sh 210 60 uhd 50 80` and
`uhd 80 100`):

| Clip | Average | Strongest second | Largest frame | Lead | Frames | late / retried / Binder errors / jumps | IRQ 51 (8 s windows) | ES buffer every 10 s (ms) | Owner |
|---|---|---|---|---|---|---|---|---|---|
| uhd50 | 50.1 Mbit/s | 121 Mbit/s | 1.27 MB | 1.0 s | 1440/1440 | 0 / 0 / 0 / 0 | 24.10; 23.97 | 886 839 849 877 845 842 | "super, no error" |
| uhd80 | 80.5 Mbit/s | 152 Mbit/s | 1.54 MB | 1.5 s | 1440/1440 | 0 / 0 / 0 / 0 | 23.97; 24.04 | 936 859 745 1189 1321 1360 | "no error" |

The decoder kept up: a decoder falling behind would have pushed the buffer above the lead in the 10 s
means. 10 bit needed no client change. Buffer values from `MM_GetVideoDataBufferInfo` kind 3.

**Limits of this test:** SDR content (HDR untested); `veryfast` encodes, not a real UHD Blu-ray; no frame
above 2 MB (the chunked path of `MM_PushPayload64` was not exercised); 60 s runs. The 100 Mbit/s port
delivers at most 11.7 MB/s (~94 Mbit/s): sustained higher rates cannot arrive over the network. The
80 Mbit/s clip needed 1.5 s lead and up to 17 MB of buffer by calculation; the service reports about
16 MB. UHD remuxes with long peaks above that belong on USB storage at the TV (USB is 2.0, 480 Mbit/s).

---

## 10. DVD and SD sharpness

The owner found DVD rips over Kodi clearly soft (2026-11-15). Everything on the TV was read only, except
two picture-setting tests done with the owner's consent and restored afterwards.

### 10.1 It is the resolution, and then the TV's SD processing

A DVD has 720×576 pixels (~0.4 MP), the panel 3840×2160 (~8.3 MP): about 20 times as many. Every source
pixel is stretched to roughly 5×4 panel pixels. That sets the ceiling. The same DVD on a PlayStation 5
over HDMI looked clearly sharper than the unchanged MKV rip over Kodi, so the TV path does not reach
that ceiling.

### 10.2 What was measured on the playback path

| Check | Result | Source |
|---|---|---|
| File | MPEG-2 720×576, 25 fps, DAR 16:9, progressive per Kodi's probe; ~5.8 Mbit/s total, ~5 Mbit/s video; running time = cinema time × 24/25 (PAL DVD) | Kodi log `CTVPlayer::Prepare`, JSON-RPC |
| Decoder | `Player.Process(videodecoder)` = "TV mpeg2video": hardware decoder, video plane, Kodi transparent above (`video underlay on`) | JSON-RPC, Kodi log |
| Path | **TV player**, not the ES path, because forced subtitles were on ("TV player: subtitles on") | Kodi log |
| Scaler input | `BW Window … Hsize:720, Vsize:576, interlace:1, Vfreq=500`: even the progressive rip reaches the deinterlacer and scaler as **576i at 50 Hz** | `dmesg` |
| Launcher getter | `getVideoFormatInfo()` said "720*576p/60Hz" at the same time: not reliable | getter, read only |
| Kodi's GUI | 1920×1080 at 60 Hz; affects only menus and subtitles, the video is scaled by the TV directly to 4K | `System.ScreenResolution` |
| "Media" picture settings | picture mode Standard, NR 2 and MPEG NR 2 (scale 0–3), sharpness 50, motion Standard (4) | launcher getters |
| ES path, 4K test clip | `PQSource:9, Hsize:3840, Vsize:2160, interlace:0, Vfreq=250`: same picture source 9 as the TV player, so the "media" settings apply to both paths | `dmesg` |

### 10.3 Picture settings are not the lever

A script (`mods/startscreen/debug/pq-nr-test.py`, root-mod repository) reads and backs up the picture
values, plays a clip in Kodi and shows the same scene three times, each labelled by a Kodi toast, then
restores and re-reads the values. Setters: `setEnumNoiseReduction(n)`, `setMPEGNoise(n)`; mode `memc`
switches motion smoothing. Trap: the getters lag one call behind the setter — re-read after 1–2 s.

- Noise reduction Medium/Low/Off on a 4K re-encode and on the original: the owner saw no difference.
- Motion smoothing Standard (4), Film (2), Off (0): "Film" flickers, otherwise no notable improvement.
- The original MPEG-2 excerpt without subtitles over the **ES path** looked no better to the owner than
  the subtitled film over the TV player.

### 10.4 The PS5 comparison (read only, `pq-watch.py`)

| | Kodi (DVD rip) | PS5 (DVD) | PS5 (Blu-ray) |
|---|---|---|---|
| Signal at the scaler | 720×576 (`PQSource:9`), 576i with subtitles | `PQSource:6, 3840×2160, interlace:0, Vfreq=499`, YUV 4:2:0 | `PQSource:6, 3840×2160, interlace:0, Vfreq=239` |
| Who scales to 4K | the TV's scaler (factor 3.75) | the PS5 | the PS5 (1080p → 4K) |
| Who deinterlaces | the TV (TV player path) | the PS5 | – |
| Picture mode, NR, sharpness, motion | Standard, medium/medium, 50, Standard | **identical** | **identical** |

The settings were identical; the difference is that the PS5 upscales SD itself and sends a finished 4K
picture. That also explains why Blu-ray rips look "super" over Kodi: from 1080p the TV only has to
double.

Side finding (useful for anyone with an external player): with the TV's "HDMI 2.0 format" on
*Standard*, the HDMI input reported only HDMI 1.4 bandwidth (`TMDS … HDMI 1.4 … 296MHz`), `getHdrFlag()`
stayed 0 even with a UHD HDR disc, and the PS5 sent 4:2:0 at 60 Hz. After switching to *Enhanced*
(Settings → System → HDMI & CEC → HDMI 2.0 format, while that input is active) the driver logged
`TMDS switch to HDMI 2.0`, `getHdrFlag()` = 1 at 2160p23.976, and the TV loaded its HDR picture values by
itself. An AV receiver in between must pass 18 Gbit/s and HDR, otherwise the picture stays black.

### 10.5 Re-encoding experiments

Source: a PAL DVD rip of a 2.39:1 film, MPEG-2 720×576p25, PAR 64:45; the black bars are part of the DVD
picture (autocrop 74/74, only 428 active lines). Test clips of 2–3 minutes, AC-3 passed through, no
subtitles, served over HTTP from the build VM.

| Clip | Settings (HandBrakeCLI 1.11.2) | Owner |
|---|---|---|
| A | `-w 1920 -l 1080 --non-anamorphic --crop 0:0:0:0`, x264 RF 18 slow/film, 25 fps CFR | – |
| B | A + `--lapsharp=light --lapsharp-tune film` | "much better" than the original via the TV player, not perfect |
| C | 1080p with clean bars (`--crop 74:74:0:0 -w 1920 -l 802 --pad top=139:bottom=139:color=black`), bt709, `--deblock=light --nlmeans=ultralight --nlmeans-tune film --lapsharp=medium --lapsharp-tune film`, x264 RF 16 | vertical strokes and artefacts |
| D | 4K: `-w 3840 -l 1606 --pad top=277:bottom=277`, bt709, sharpening as B, `x265_10bit` RF 18 medium, main10, level 5.1 | "quite good" |
| E | 4K with C's filter chain, x265 as D | strokes and artefacts again |
| F | D with `-q 16 -x no-sao=1:deblock=-1,-1:psy-rd=2.0:psy-rdoq=1.0:aq-mode=3`, ~13 Mbit/s | open |
| G | F with `--lapsharp=medium` alone | open |

**H** was made with ffmpeg instead:

```
-vf crop=720:428:0:74,format=yuv420p10le,
    scale=3840:1606:flags=lanczos+accurate_rnd+full_chroma_int+full_chroma_inp,setsar=1,
    cas=strength=0.5,colorspace=all=bt709:iall=bt601-6-625:format=yuv420p10,
    pad=3840:2160:0:277:black
-c:v libx265 -preset medium -crf 16 -x265-params <same as F>
```

Exactly 3840×2160, level 5.1, BT.709 tags, ~13.5 Mbit/s. CAS (contrast adaptive sharpening) lifts fine,
low-contrast detail and leaves hard edges almost untouched.

Picture analysis on the PC (`tools/vq_compare.py`): the same frame from source and clips (best match out
of 13 source frames), everything on the active 4K area 3840×1606, the source upscaled bicubically as a
stand-in for the TV's scaler; luma metrics *detail* (mean gradient), *ssim* (after downscaling to
1024×428), *grid* (energy of vertical lines on the source's 8-pixel block grid), *aniso*
(horizontal/vertical gradient ratio). Means over four time points:

| Clip | detail | ssim | grid | aniso |
|---|---|---|---|---|
| original, bicubic | 0.44 | 0.999 | 1.00 | 1.00 |
| A / B | 0.46 / 0.46 | 0.990 | 0.07 | 1.09 / 1.08 |
| C | 0.37 | 0.960 | 0.12 | 0.94 |
| D | 0.53 | 0.985 | 0.42 | 1.03 |
| E | 0.41 | 0.974 | 0.25 | 1.11 |
| F | 0.64 | 0.986 | 0.62 | 1.03 |
| G | 0.65 | 0.986 | 0.93 | 1.07 |
| **H** | **0.69** | 0.988 | 0.26 | 1.02 |

Findings:

- C and E show stair steps on diagonal and curved edges (the owner's "vertical strokes") while detail
  and SSIM drop. G (only `lapsharp=medium`) has none, so the cause is **deblock + NLMeans on SD MPEG-2**.
  For DVD sources: sharpen lightly, do not denoise.
- F has ~20 % more detail than D at equal fidelity: x265's default SAO and deblock visibly smooth
  upscaled SD.
- G sharpens hardly more than F but amplifies the source's block grid (0.93).
- H: most detail, best fidelity of the 4K clips, weakest block grid; single hair strands and skin
  texture visible without halos.
- A second scene was chosen with `tools/find_scene.py` (reads only key frames, finds 90 s windows that
  are bright, detailed and moving). CAS 0.5 (≈20 Mbit/s, x265 fast CRF 16): better than the original,
  close-ups good, wide shots still soft. **ffmpeg's `cas` breaks down at strength 1.0** into a strong noise
  pattern (detail 12.9, SSIM 0.84, 160 Mbit/s); 0.5–0.8 are nearly identical. Black/white levels of all
  clips matched the DVD. In a dark scene (99.5th luma percentile only 64–113 of 255) the owner saw no
  difference between F and H.
- Speed in a Windows VM with 8 threads: A 34.5 fps, B 28.6, C 25.8, D 4.1, E 4.6, x265 `fast` 4K ≈ 5.5
  fps. For a film of almost three hours: 2.1–2.8 h in 1080p, about 13 h (x265 fast) to 16–18 h (x265
  medium) in 4K. Re-encoding a whole DVD collection is therefore not a practical permanent solution.

### 10.6 What helps, by effect

1. **A better source** (Blu-ray, UHD). The only measure with a large effect. A quality tag like
   "Bluray-576p" in a file name says nothing; codec, resolution and frame rate do (MPEG-2, 576, 25 fps =
   DVD).
2. **Move upscaling in front of the TV:** an external player that upscales well, or a one-off conversion
   on a machine with a GPU. The best measured offline chain is H (Lanczos to 4K, CAS 0.5, x265 without
   SAO/deblock). Wide shots stay soft in every variant; more would need edge-directed (nnedi3) or AI
   upscaling.
3. **Picture settings for "media"** (sharpness moderate, NR low, picture mode Cinema, motion Film/Off):
   small effect at most; the NR and motion tests above showed none. The TV menu is unreachable while Kodi
   holds the remote: set them in the TV's own USB player.
4. Kodi's own software upscaling would not be better: its plane is 1080p at a fixed 60 Hz.

Kodi's own video settings (scaling, HQ scaler, sharpness, deinterlacing, refresh rate) have no effect on
the two hardware paths. Only the subtitle language matters indirectly (§7.2).

### 10.7 Open

- Whether a progressive MPEG-2 rip over the ES path reaches the scaler as 576p instead of 576i (the
  kernel ring buffer had already wrapped). Small gain at best: with correct film-mode detection, 576i
  loses nothing.
- A planned test with two H.264 clips that play both on the PS5 (USB) and in Kodi — the unchanged DVD
  picture, and the same scene pre-upscaled to 4K — to see whether the TV's media path degrades even a
  finished 4K picture. Prepared, not yet judged.

---

## 11. Window and aspect ratio for non-16:9 films

Both hardware paths stretch the picture to whatever window they are given; the video plane does not apply
the film's aspect ratio by itself. That was confirmed by accident during the hang in §13: with the window
already reset to full screen, a 4:3 film was visible stretched to full width.

### 11.1 Rule (both paths)

With `DAR` = display aspect ratio from Kodi's probe and `full = 3840/2160`:

- `|DAR − full| ≤ 0.01` → full window `[0, 0, 3840, 2160]`;
- `DAR > full` (wider) → `h = round(3840 / DAR)`, `y = (2160 − h) / 2`, window `[0, y, 3840, h]`;
- `DAR < full` (narrower) → `w = round(2160 × DAR)`, `x = (3840 − w) / 2`, window `[x, 0, w, 2160]`.

| Material | DAR | Window (x, y, w, h) |
|---|---|---|
| 16:9, including 16:9 DVDs with bars inside the picture | 1.778 | 0, 0, 3840, 2160 |
| 4:3 DVD | 1.333 | **480, 0, 2880, 2160** (seen in the log: `kodiPlay: Fenster 480x0x2880x2160 (DAR 1.333)`) |
| 1.85:1 encoded without bars | 1.85 | 0, 42, 3840, 2076 |
| 2.39:1 encoded without bars | 2.39 | 0, ~276, 3840, 1607 |

### 11.2 TV player path

Kodi sends the DAR in the job (`dar`, accepted range 0.5–4). If it is missing, `flex.js` asks the player
(`getMetadata()[26]`/`[27]`, width/height). `kpWindow()` then calls the launcher's own
`window.modeljs.mpctrl.setScreenPosition([x, y, w, h, 1, 0])` — the same call the TV's USB player uses —
and remembers the window in `kp.win`. Inside `tvmain` this ends in `libhisplayer.so`
`Mm_SetDisplayRectangle`. 16:9 films get no own window (`kp.win` stays empty). The window must be reset
to full before the player is stopped, or the next user of the video plane (e.g. the screen saver's photo
player) inherits a letterboxed window — and **how** it is reset turned out to matter (§12, §13).

### 11.3 ES path

`ST_MEDIA_INFO` has no aspect field, so without a window an ES item is always stretched to full screen.
`CTVEsEngine::SendWindow()` computes the same rectangle (log `CTVEsEngine: window 2880x2160+480+0 for
aspect 1.333`) and sends `ES_OP_RECT`. The helper calls
`MSrv_MM_PlayerInterface::MM_SetDisplayRectangle(item, ST_RECT_SIGNED_POSITION{int32 x, y; uint16 w, h}, 0, 0)`
(filled like `libstplayer::setDisplayRectangle`), but only **after `MM_Play` has returned**, and again
when playback starts. The window lives inside our own item, so `tvmain` and its player are not involved,
and the bugs below cannot occur on this path.

---

## 13. The `Mm_Stop` hang and Kodi's Home/minimize

### 13.1 Symptom (2026-11-18)

Since 2026-11-18 Home in Kodi does not quit Kodi but hides it (stop the film, remember the position,
draw nothing, give the remote back to the launcher; the Kodi tile is meant to bring it back in 1–2 s,
which had not yet been tried on the TV at the time of writing). The first test hit a new problem:

- A 4:3 film (own window) played; Home after 17 s. In the same moment `flex.js` reset the window, stopped
  the TV player, and Kodi hid itself.
- The TV log showed `Begin to call Mm_Stop()` and then nothing until the switch went through about
  **100 s** later (`media player -source out- done`).
- For that time the TV stayed on DMP: the film was visible **stretched to full width** (window already
  full, player not yet stopped), and choosing the PS5 gave "no HDMI signal" while its sound already came
  through the AV receiver.
- Counter-check by the owner: a normal stop of the same film with Kodi visible was immediate.

The vendor-side cause was not analysed; it happened once, with window reset and stop back to back while
Kodi's window was being hidden.

### 13.2 Fix

- **`flex.js` `kpEnd`:** with an own window, reset the window first and send the stop
  `KP_WIN_STOP_MS` = **400 ms** later (`kpPendingStop`). A new job runs a pending stop immediately
  (`kpStart` → `kpFlushStop`), so switching films is not slowed down. If the player is meanwhile playing a
  different medium (another URL in the player status), the delayed stop is dropped. 16:9 films (no window)
  are stopped immediately as before. `test1.js` checks "window already back, stop not yet".
- **Kodi `CEvdevInputHandler::Minimize`:** if a film is playing, Kodi stops it first and stays visible,
  holding the remote, for `MINIMIZE_AFTER_STOP_MS` = 1.5 s (`FinishMinimize`), then hides (window opacity
  0, `SetRenderGUI(false)`, state file `minimized`). Switch off minimizing with
  `FLEX_KODI_HOME_MINIMIZE=0` in `kodi.sh`.

On the TV afterwards: same film, Home during playback, then PS5 — `Mm_Stop` began, the switch to HDMI 1
went through with the PS5 selection after a little over 10 s, no hang.

**Rule for anyone driving the TV player:** never call `setScreenPosition` after a stop; reset the window
first, and give the player a moment (we use 0.4 s) before stopping when a window was set.

---

## 14. Open questions

- An IRQ 51 log (§3.1) of a film started from Kodi's ES path, including a 23.976 film in a 120 s window.
- Output rate on the ES path for 25p, 30p, 50p, 60p (only the scaler input `Vfreq=250` for 25p was seen).
- HDR10 on the ES path (does the panel switch to HDR for pushed streams?); Dolby Vision beyond the base
  layer.
- Interlaced material and MPEG-2 output rate on the ES path; 576p vs 576i at the scaler for progressive
  MPEG-2.
- Subtitles on the ES path (Kodi would draw them on its own plane, as Kodi for webOS does).
- AAC as native code 6 (the one run that tried it ended with the TV switching off; not analysed — AAC is
  converted to AC-3 instead).
- Frames above 2 MB (chunked `MM_PushPayload64`) and a real UHD Blu-ray remux.
- The meaning of the unknown `ST_MEDIA_INFO` fields.

---

## DMP cleanup and recovery

Do any window reset while the original native session is still valid. Separate cleanup
from the stop operation and re-check that the same item still owns the video plane.
A previous defect destabilized TV playback when cleanup happened after its session ended.
A stale window on a later source switch requires checking the active session rather than
sending a layout command to an already released player.
