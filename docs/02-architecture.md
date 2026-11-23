# Kodi port architecture

The five layers are the upstream Kodi UI, DirectFB/EGL platform, input handler, TVPlayer
and separate vendor media helper. The Flex launcher and watchdog integrate source
selection and native app handoff with normal television behavior.

Read [the build guide](01-build.md), [frame rate and video](03-frame-rate-and-video.md),
the [Hisense decoder lab](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/02-hardware-decoder.md) and the [Flex launcher guide](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/03-launcher-and-recovery.md) before deploying.


## 1. The big picture

### 1.1 Constraints that shaped the design

| Constraint on this TV | Consequence for the port |
|---|---|
| The graphics stack is an MStar fork of **DirectFB 1.4.2**; the launcher (`hisenseUI`, a Cobalt browser) is a DirectFB client | Kodi joins the running DirectFB session as one more client; there is no X11, Wayland or GBM |
| The launcher runs as **uid 10009** in a sandbox and **cannot start programs**; it can only read and write files below `/var/local/applications/UI` | The Kodi tile writes request files; a **root watcher** starts Kodi. All communication with the launcher goes through small files |
| The IR receiver `/dev/input/event0` is also read by the TV's `systemserver`, which passes keys to the launcher | Kodi reads evdev itself and takes the device **exclusively** (`EVIOCGRAB`) while it runs |
| Kodi's software decoders are too slow: a VC‑1 1080p film needed 110 % of one core and stuttered | Films go to **hardware decoders**: either the TV's own media player, or our own ES push client of the media service |
| The TV's file player (`tvmain` → media service, "vdplayer" branch) always outputs **23.976/24p at 59.94 Hz** (3:2), even for 4K, over HTTP and `file://` | A second hardware path was built: Kodi demuxes and pushes elementary streams like Netflix does, which gets the content's own rate (measured ≈ 23.97 Hz) |
| The vendor media libraries are C++ with a GCC 4.8 ABI, Binder IPC and circular dependencies (`LD_PRELOAD` order required) | They are loaded only in a small **helper process** (`flex_es_helper`), never inside Kodi |
| `/etc/profile` runs `chmod -R 777 /var/local/*` at boot; `/tmp` is world-writable | Every root script that touches a shared folder works "fail closed": inode checks, single `rm -f`, no `mv` into foreign paths, a per-run key, HMAC on every bridge file |
| BusyBox on the TV has no `readlink`, `tr`, `sort`, `od`, `mktemp`, `uniq`, `sha256sum` | Scripts use `ls -ldn`, `awk`, `sed`, `hexdump`, `md5sum` |
| The media service holds the single main decoder and **does not restart after a crash** (no video for any app until reboot) | Strict call order in the helper, no `SIGKILL` of the helper, `MM_Stop` exactly once per item |

### 1.2 Processes and who talks to whom

```
 IR remote --EVIOCGRAB--> kodi-directfb (root, /APPS/kodi)                hisenseUI launcher (uid 10009)
                          |-- GUI: DirectFB window 1920x1080, GLES2       |  flex.js v10
                          |-- CEvdevInputHandler (thread)                 |   |-- Kodi tile, heartbeat reader (1 s)
                          |     tvkeys ------------------------------+    |   |-- kodiPlay module (250 ms per film)
                          |-- CTVPlayer (player core "tv", thread)   |    |   |-- tvkTick (100 ms, volume)
                          |     |-- CTVHttpFeeder 127.0.0.1:<port>   |    |   '-- modeljs.mpctrl / UI.store (DMP)
                          |     |-- CTVBridge: play.json, ctl -------+--> /tmp/flexkodi-rt/b  <-- flexkodi/rt (symlink)
                          |     |   <-- player.json, tvack ----------+---------------------------'
                          |     '-- CTVEsEngine (demuxer thread)
                          |           |  fd 3 data, fd 4 control (socketpairs)
                          |           v
                          |     flex_es_helper (root, vendor libs) --Binder--> MiMwIpcSrv (media service, uid 2900)
                          |                                                    ^
 TV player path:          '-- HTTP GET Range <-- tvmain (uid 2910) <-- PlayMedia(url) from flex.js
 
 root, boot hook ----> kodi-watch.sh (1 s) --flexkodi/start--> kodi.sh start --setsid--> kodi-run.sh --> kodi-directfb
                                           <--flexkodi/running (every 2 s, written by kodi-run.sh)
```

Picture: the video decoders draw on the **video plane** (3840×2160, below all graphics). Kodi's GUI is a
1920×1080 ARGB window on a DirectFB layer above it; the TV's graphics output processor (GOP) scales the
layer to the panel and mixes it with the video plane by per-pixel alpha. During playback Kodi turns
transparent so the film shows through, and the OSD is drawn on top.

Sound: on both hardware paths the media service decodes audio itself (TV loudspeakers, or HDMI ARC to
an AV receiver). Kodi releases its ALSA device during such playback.

### 1.3 The three playback paths

| | ES path (`CTVEsEngine`) | TV player path | Kodi `VideoPlayer` |
|---|---|---|---|
| Who demuxes | Kodi (FFmpeg demuxer) | the TV player, from Kodi's loopback HTTP server | Kodi |
| Who decodes video | media service, "mstplayer"/MultiDecode branch, hardware | media service via `tvmain`, "vdplayer" branch, hardware | Kodi, software |
| Output rate for 24p | content rate (≈ 23.97 Hz measured with the test client) | always 59.94 Hz (3:2) | GUI layer only, no rate switch |
| Video codecs | progressive H.264, HEVC (incl. 10 bit), VC‑1, MPEG‑2 | whatever the TV player takes (MPEG‑2, VC‑1, H.264, HEVC up to 2160p) | anything FFmpeg can do, up to what one core manages |
| Audio | AC‑3/E‑AC‑3 as is; everything else converted to AC‑3 by Kodi | the TV's formats only (no TrueHD/FLAC/Opus/Vorbis) | Kodi's ALSA (PCM) |
| Subtitles | none yet | internal subtitles drawn by the TV | Kodi's |
| Chosen when | default for every file that qualifies (section 5.6) | subtitles on, interlaced material, 3D, ES failure | rejection, fallback, DVD/BD folders, PVR, adaptive streams |

---

## 2. Windowing: Kodi as a DirectFB client

Files: `xbmc/windowing/directfb/WinSystemDirectFB.*`, `WinSystemDirectFBGLESContext.*`, plus small
patches in `platform/linux/PlatformLinux.cpp` (register the window system), the GLES renderer CMake
files, and `xbmc/guilib/GUITextureGLES.cpp`.

### 2.1 Joining the TV's DirectFB session

`kodi.sh` sets the same environment the launcher uses, then starts Kodi with
`--windowing=directfb --audio-backend=alsa`:

```sh
export DIRECTFB_SESSION=0
export LD_LIBRARY_PATH=$K/lib:/lib:/mslib:/mslib/utopia:/std_lib/lib:/tvservice/conflictlib:/system/lib:/usr/local/lib
# DFBARGS = every "name = value" line of /mslib/directfbrc as name=value, comma separated,
# plus MStar's hardware description library:
DFBARGS="<options from /mslib/directfbrc>,dfbinfo-dir=/mslib/libdfbinfo.so"
```

`CWinSystemDirectFB::InitWindowSystem()` calls `DirectFBInit(nullptr, nullptr)` (DirectFB reads
`DFBARGS`, there is no command line to pass), `DirectFBCreate()`, takes the chosen display layer with
`DLSCL_SHARED` and creates **one full-screen window**. It offers exactly one resolution, the layer
size (1920×1080). There is no mode switching and no `OnLostDisplay()`; `SetFullScreen()` only resizes
the GLES viewport. The refresh rate Kodi reports (60 Hz) is a **constant in our code**
(`DEFAULT_REFRESH_RATE`), not a measurement: DirectFB 1.4 has no portable way to query the panel.
Kodi's "adjust display refresh rate" can therefore not work here; the native rate comes from the ES
path instead (section 5).

### 2.2 MStar's structures are larger than the headers

The first Kodi start crashed with `SIGSEGV`, `this == nullptr` in `StartInput()`. A probe that fills a
buffer with a pattern and counts what DirectFB writes showed that MStar's fork writes **36 bytes** into
the 32-byte `DFBDisplayLayerConfig` (`GetConfiguration()`) and **240 bytes** into the 236-byte
`DFBGraphicsDeviceDescription`. The 4 extra bytes overwrote a saved register on the stack. A C probe had
survived only by luck of its stack layout.

Fix: every structure DirectFB fills in or reads is wrapped in a zeroed buffer with spare room:

```cpp
template<typename T>
struct WithSpareRoom { T value; unsigned char spare[256]; };
```

If you port to another MStar TV, assume the same for every DirectFB structure.

### 2.3 Choosing the layer

The TV's DirectFB has three layers (0–2), identically configured, each on its own GOP. The launcher draws
into one opaque full-screen window on one of them, **and which layer that is changed between boots**
(layer 0 one night, layer 1 after a cold boot). A window on the other layer stays hidden below the
launcher, whatever its stacking class or alpha (checked with one colour per layer).

`ChooseLayer()` therefore decides at every start, read-only: for each layer it calls `GetWindow(1)`,
which on MStar returns the layer's **first** window for any id, reads its opacity and picks the last
layer that has a visible (opacity > 0) window, i.e. the launcher's. Fallback: `DLID_PRIMARY`. Override:
`FLEX_KODI_DFB_LAYER=0|1|2`. The log line `CWinSystemDirectFB: using layer N` records the decision.

### 2.4 The window

| Step | Why |
|---|---|
| `DWCAPS_ALPHACHANNEL`, `DSPF_ARGB`, size of the layer | the surface format the TV's EGL is proven to work with |
| optional `DSCAPS_PREMULTIPLIED` (`FLEX_KODI_DFB_PREMULT=1`) | creation-time only; for blending experiments |
| `SetPerPixelAlpha(false)`: clear `DWOP_ALPHACHANNEL` | compositing **opaque** by default (section 2.6) |
| `Clear(0,0,0,0xff)` and `Flip()` | opaque black instead of uninitialised memory until the first frame |
| `SetOpacity(0xff)` | new DirectFB windows are invisible |
| `SetStackingClass(DWSC_UPPER)`, `RaiseToTop()` | the launcher's windows use the default `DWSC_MIDDLE`; once the launcher's screen saver started behind Kodi and Kodi disappeared. In the upper class the launcher can never cover Kodi |
| optional `DWOP_GHOST` (`FLEX_KODI_DFB_GHOST=1`) | no focus and no DirectFB key events; test switch only (Kodi reads keys from evdev anyway) |

On exit `ReleaseDirectFB()` sets opacity 0, destroys and releases window, surface, layer and the
`IDirectFB` interface in reverse order.

### 2.5 EGL and GLES 2

`CWinSystemDirectFBGLESContext` uses `EGL_DEFAULT_DISPLAY` and an ES 2 context. The TV's Mali EGL has a
DirectFB backend: the **window's `IDirectFBSurface*` is passed as `EGLNativeWindowType`**, and every
`eglSwapBuffers()` is a `Flip()` of that surface. Order matters: DirectFB first (EGL attaches to the
DirectFB session of the process), and the EGL surface must be destroyed before the DirectFB surface is
released (the EGL helper is a member, so it is destroyed before the base classes). If EGL fails, the
window system is torn down at once so that no black window stays on top of the TV UI.

### 2.6 Alpha: opaque by default, transparent over a film

**Why Kodi touches the alpha channel at all.** The GOP mixes each graphics layer with what lies below it
(other layers and the video plane) by per-pixel alpha. Kodi clears with alpha 0, and several GLES draws
(the dim screen saver quad, cross-fades, the black bars around video) leave destination alpha below 1.
On the TV a "transparent" Kodi therefore showed the video plane, for example the last photo of the
launcher's slideshow. Two measures keep Kodi opaque when it should be:

1. the window is composited without `DWOP_ALPHACHANNEL` (a plain copy for the window manager);
2. before every swap, `MakeAlphaOpaque()` clears **only the alpha channel** to 1
   (`glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE)` + `glClear`), colours stay untouched.

**Video underlay.** During hardware playback the film is on the video plane *below* Kodi, so Kodi must
become transparent where it draws nothing. The switch is per frame and needs no player-specific code in
the window system:

```
CTVPlayer: CDataCacheCore::SetVideoRender(true)   (1 s after "playing")
   -> CApplicationPlayer::IsRenderingVideoLayer() == true
   -> CWinSystemDirectFBGLESContext::PresentRender(rendered, videoLayer=true)
        underlay changed? -> ApplyVideoUnderlay(on), MarkDirty() (redraw a full frame for the new mode)
        PresentRenderImpl(): MakeAlphaOpaque() is skipped while m_underlay is set
```

Kodi's own GLES renderer never sets `SetVideoRender(true)`, so without the TVPlayer every frame stays
opaque exactly as before.

Two compositing modes exist:

| Mode | Switch | What happens | Launcher |
|---|---|---|---|
| **copy** (default, in use) | none | the window stays without `DWOP_ALPHACHANNEL`; the window manager copies Kodi's ARGB **including alpha** into the layer, covering the launcher window; the GOP mixes that alpha with the video plane | untouched |
| blend | `FLEX_KODI_UNDERLAY_BLEND=1` | `DWOP_ALPHACHANNEL` on while the underlay is active; Kodi is blended with the launcher window below | must be transparent itself: Kodi sends `ov:1` in `play.json`, `flex.js` sets the CSS class `flex-kp-video` (opacity 0) on the launcher and removes it whenever Kodi is not running |

Related details:

- `GUITextureGLES.cpp`: `DrawQuad` (the pause dimmer) used `glBlendFunc`, which wrote a wrong
  destination alpha. It now uses `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
  GL_ONE_MINUS_DST_ALPHA, GL_ONE)`; colour factors are unchanged.
- Kodi's GUI already leaves premultiplied colour with correct coverage alpha in the frame buffer. Should
  the GOP turn out to mix with straight alpha, `FLEX_KODI_GOP_STRAIGHT=1` enables a pass that divides
  colour by alpha in place using `GL_ARM_shader_framebuffer_fetch`. This is a prepared switch; the
  mixing-formula probe was not completed (section 9).
- `CRenderSystemGLES` only sleeps between frames when there is *no* video layer. With the TV player
  nothing paces Kodi's render loop, so `PresentRender` sleeps 20 ms when nothing was rendered.
- `CTVPlayer::Render()` clears to opaque black until the picture is revealed, so old frame-buffer
  contents never show through.
- Kodi's overscan calibration does not apply to the video plane; the player never derives the video
  window from GUI coordinates.

### 2.7 Hide and show

`Hide()` sets the window opacity to 0, `Show(raise)` back to 0xff and raises it. Both are used by the
Home/minimize feature (section 3.6). Surface, EGL surface, layer and stacking class stay.

### 2.8 Window-system switches

All switches are environment variables, active only with the exact value `1` unless noted:

| Variable | Effect |
|---|---|
| `FLEX_KODI_DFB_LAYER=0..2` | force the DirectFB layer |
| `FLEX_KODI_WINDOW_ALPHA` | keep Kodi's alpha always (test) |
| `FLEX_KODI_UNDERLAY_BLEND` | blend mode instead of copy mode |
| `FLEX_KODI_UNDERLAY_TEST` | underlay on from the start, without a player (transparency test) |
| `FLEX_KODI_DFB_PREMULT`, `FLEX_KODI_DFB_GHOST`, `FLEX_KODI_GOP_STRAIGHT` | blending/focus experiments |
| `FLEX_KODI_INPUT` | evdev device (default `/dev/input/event0`) |

---

## 3. Input: `CEvdevInputHandler`

File: `xbmc/windowing/directfb/EvdevInputHandler.*`. Started by the window system after the window
exists, so "ready" really means "window there and remote taken".

### 3.1 Evdev and the exclusive grab

DirectFB keys are not used. A reader thread opens the IR receiver (`"MStar Smart TV IR Receiver"`,
`/dev/input/event0`, `O_NONBLOCK`) and polls it every 100 ms. With `FLEX_KODI_GRAB=1` (set by
`kodi.sh`) it calls `ioctl(fd, EVIOCGRAB, 1)`: `systemserver` and with it the launcher see no keys while
Kodi runs. **The kernel releases the grab when the process exits or crashes**, so the TV always gets its
remote back. If the device cannot be opened, Kodi keeps running and retries every 2 s.

Keys are injected as `XBMC_KEYDOWN`/`XBMC_KEYUP` through `CServiceBroker::GetAppPort()->OnEvent()`.
Every `(key code, scan code)` pair is logged once, and so is every scan code that arrives without a key
event, which lets you learn an unknown remote from `kodi.log`.

### 3.2 Key map

| evdev code(s) | Kodi key |
|---|---|
| `KEY_UP/DOWN/LEFT/RIGHT` | arrows |
| `KEY_OK`, `KEY_SELECT`, `KEY_ENTER`, `KEY_KPENTER` | Return |
| `KEY_BACK`, `KEY_EXIT` | `XBMCK_BROWSER_BACK` |
| `KEY_BACKSPACE` / `KEY_ESC` | Backspace / Escape |
| `KEY_MENU`, `KEY_OPTION`, `KEY_CONTEXT_MENU` | Menu (context menu) |
| `KEY_HOME`, `KEY_HOMEPAGE` | minimize (with the launcher integration), otherwise `XBMCK_HOMEPAGE` |
| `KEY_INFO` | `i` with unicode 0 (bound to Info in every relevant window, types nothing into text fields) |
| `KEY_EPG`, `KEY_FAVORITES`, `KEY_SEARCH`, `KEY_TEXT` | EPG, favourites, browser search, teletext |
| `KEY_PAGEUP/DOWN`, `KEY_CHANNELUP/DOWN` | Page up/down |
| `KEY_SETUP`, `KEY_CONFIG` | `XBMCK_CONFIG` (opens Settings) |
| `KEY_ZOOM`, `KEY_SCREEN` | Zoom |
| colour keys | red/green/yellow/blue |
| `KEY_PLAY*`, `KEY_PAUSE*` | Play/Pause |
| `KEY_STOP*`, `KEY_REWIND`, `KEY_FASTFORWARD`, `KEY_NEXT*`, `KEY_PREVIOUS*`, `KEY_RECORD` | media keys |
| `KEY_0..9`, keypad and `KEY_NUMERIC_*` | digits |
| `KEY_VOLUMEUP/DOWN`, `KEY_MUTE` | to the TV/receiver via `tvkeys` (3.5); Kodi's own volume as fallback |
| `KEY_POWER`, `KEY_SLEEP` | quit Kodi (3.3) |
| `KEY_EJECTCD`, `KEY_SHUFFLE` | unmapped (no entry in Kodi's key table) |

Autorepeat events are forwarded as further key-downs, except for toggle keys (`KEY_MUTE`, `KEY_RECORD`).
`FLEX_KODI_REPEAT_DELAY_MS` optionally drops repeats that come too soon after the press. After
`SYN_DROPPED` (kernel buffer overflow) everything up to the next `SYN_REPORT` is ignored and every held
key is released.

### 3.3 Quitting: Power, and Power held for 5 s

- **Power/Sleep** (only while grabbed) posts `XBMC_QUIT` → `TMSG_QUIT`. Kodi closes the player first
  (which stops the film), then the window system destroys the handler, which releases the grab. A
  second Power press then reaches the TV and puts it into standby.
- **Held for 5 s** (`FORCE_QUIT_HOLD_MS`) without Kodi having quit: the reader thread still runs even if
  Kodi's main thread hangs, so it writes one line to `stderr` and calls `_exit(3)`. No logging, no
  destructors (they could hang too). The kernel releases the grab.

### 3.4 State files for the launcher

With `FLEX_KODI_STATE_DIR=/tmp/flexkodi.d` (root 0700) the handler writes small files that `kodi-run.sh`
turns into the heartbeat (6.4). All are opened with `O_NOFOLLOW`.

| File | Written when | Meaning |
|---|---|---|
| `ready` | remote grabbed | Kodi is usable; enables the volume bridge and the key hand-over to `flex.js` |
| `lastkey` | every key press (Unix seconds) | the launcher sees no keys while Kodi holds the grab; `flex.js` uses this to keep the TV's auto-standby from firing during active use |
| `minimized` | after Home | Kodi is hidden, the launcher has the remote |
| `restore` | by `kodi.sh start` (not by Kodi) | request to come back; Kodi deletes it when it acts |

### 3.5 TV volume through `flex.js` (`tvkeys` / `tvack`)

**Problem.** While Kodi holds the grab, volume keys never reach the TV. Kodi's own software volume is
useless during hardware playback (Kodi's ALSA is released) and cannot control an AV receiver on HDMI
ARC. The launcher's `SetVolume` does not work in ARC mode; the launcher's own voice control instead
injects single key presses.

**Design.** Kodi counts presses in the signed bridge file `tvkeys`; `flex.js` replays the difference the
way the launcher does (over ARC: `UI.send_key_to_dfb`; otherwise through the launcher's `VolumeBar`
store) and acknowledges in `tvack`.

```json
tvkeys: {"v":1,"kid":"6721a3f0-1a2b","seq":17,"up":12,"down":4,"mute":1}
tvack:  {"v":1,"kid":"6721a3f0-1a2b","seq":17,"arc":1,"vol":23,"mute":0}
```

| State | Behaviour |
|---|---|
| `OFF` | no `FLEX_KODI_TVKEYS=1`, no grab, or no bridge: Kodi's own volume |
| `REGISTERING` | Kodi writes `tvkeys` with all counters 0 every second (a new `kid` only makes `flex.js` adopt the counters, it never executes them) until a `tvack` with this `kid` arrives, at most 10 s (`flex.js` only has the run key a few seconds after "ready") |
| `ON` | presses are counted; each write must be acknowledged within **1 s** |
| `FAILED` | no registration or no acknowledgement: hint "TV-Lautstärke nicht erreichbar", Kodi's own volume for the rest of the run |

Details: `up`/`down`/`mute` only grow; `kid` = `<time hex>-<pid hex>` of this Kodi run. A held volume key
is thinned to one step per 140 ms (kernel autorepeat is 70 ms); mute is never repeated. `flex.js`
injects at most one key per 100 ms tick and keeps at most 8 steps pending; mute is applied by parity.
Once registered, Kodi sets its own volume to 100 % and unmuted (`TMSG_SET_VOLUME`, `TMSG_SET_MUTE`)
so that its PCM output is never attenuated. The ack wait is checked in every 100 ms pass of the reader
loop; the thread never blocks on it.

**Display.** The launcher's volume bar lies below Kodi's window, so Kodi shows its own volume bar
(`FlexVolumeBar.*`, window properties `flexvol.pct`/`flexvol.text` read by the skin's
`DialogVolumeBar.xml`) 300 ms after the last press:

- TV loudspeakers: the TV value from `tvack`.
- HDMI ARC: Kodi reads the receiver's status page **read-only**
  (Denon/Marantz `goform/formMainZone_MainZoneXmlStatusLite.xml`, URL from `/APPS/kodi/avr.url`, passed
  as `FLEX_KODI_AVR_URL`) twice, after 150 ms and after another 650 ms (the last CEC steps arrive late),
  and shows `MasterVolume` in dB + 80 (the receiver's 0–98 display scale, half steps) or the mute symbol.
  Without an answer: a text hint ("AVR lauter/leiser/stumm"). Kodi never sends commands to the receiver.

Tested on the TV with the TV loudspeakers and with a Denon receiver over ARC (the number in the bar was
confirmed by the owner). While the audio engine is suspended *without* `tvkeys`,
`CTVPlayer::OnAction` swallows volume actions and shows "use the TV or AVR remote" once.

### 3.6 Home: minimize instead of quit

Requested by the owner: Home in Kodi should step aside, not end Kodi; any other app ends it.

```
Home pressed (grabbed, FLEX_KODI_STATE_DIR set, FLEX_KODI_HOME_MINIMIZE != 0)
  Minimize(): release held keys, post TMSG_MEDIA_STOP
     film running?  -> stay visible with the remote for 1.5 s (MINIMIZE_AFTER_STOP_MS)
  FinishMinimize():
     TMSG_CALLBACK on the app thread: SetRenderGUI(false), CWinSystemDirectFB::Hide()
     delete an old "restore", write "minimized", EVIOCGRAB 0 (remote back to the launcher)
  while minimized: every key is ignored; every 0.3 s look for "restore"

Kodi tile pressed again -> flexkodi/start -> kodi-watch.sh -> kodi.sh start
     Kodi running and "minimized" present -> touch restore ("Kodi zurückgeholt")
  CheckRestore(): take "restore", EVIOCGRAB 1, delete "minimized", write "lastkey",
     TMSG_CALLBACK: SetRenderGUI(true), Show(raise), MarkDirty()
```

While hidden Kodi draws nothing (0 % CPU measured). The 1.5 s delay exists because hiding Kodi at the
same moment as `flex.js` reset the TV player's window and stopped it left the vendor `Mm_Stop` hanging
for about 100 s (the TV stayed on the media input, the stretched film showed through, a console on HDMI
had no picture). See 4.6 for the matching change in `flex.js`.

Status: minimizing (also during a 4:3 film) and "another source ends the minimized Kodi" were tested on
the TV; restoring through the tile is implemented and harness-tested but was **not yet confirmed on
the TV** in the sources.

---

## 4. The TVPlayer core

Files: `xbmc/cores/TVPlayer/` (`TVPlayer`, `TVPlayerProbe`, `TVPlayerHttpFeeder`, `TVPlayerBridge`,
`TVBridgeIO`, `TVEsEngine`, `TVEsProtocol.h`, `TVAudioTranscoder`), `cmake/treedata/linux/directfb/tvplayer.txt`,
`xbmc/cores/playercorefactory/PlayerCoreFactory.cpp` and `PlayerCoreConfig.cpp`,
`system/playercorefactory.xml`.

### 4.1 A new player core of type `tv`

`TVPLAYER::CTVPlayer` is an `IPlayer` plus `CThread`, modelled on `VideoPlayer` (non-blocking
`OpenFile`, times and state through `CDataCacheCore`). Kodi stays the user interface: library, resume
points, OSD, seek bar, audio and subtitle menus, watched state. The TV only decodes.

- Not `external`/`remote`: those types have side effects in several places of Kodi; a new type `tv` has
  none. XML `type="TVPlayer"` maps to the internal type `tv` and is compiled only with
  `TARGET_DIRECTFB`.
- `OpenFile()` either rejects at once (then requests a fallback, no `OnPlayBackStarted`) or calls
  `OnPlayBackStarted` **before** creating the thread (the end callback can never overtake it) and
  returns. Exactly one end callback follows per started playback.
- Selection: the system file registers the player without a rule (it appears under "Play using…").
  On the owner's TV a rule in `special://profile/playercorefactory.xml` prepends
  `video="true" filetypes="mkv|mp4|m4v|ts|m2ts|mts|avi|mov|webm|mpg|mpeg" player="TVPlayer"`; DVD
  folders, ISOs etc. keep Kodi's rules. Player options belong in the **system** file
  (`/APPS/kodi/share/kodi/system/playercorefactory.xml`); a `<player name="TVPlayer">` in userdata would
  create a second player "TVPlayer1".

| `<player>` option | Default | Meaning |
|---|---|---|
| `fallback` | true | refused/unsuitable → `VideoPlayer` |
| `fallbackmaxwidth` | 1920 | wider: error message instead of fallback (Kodi's software cannot decode it) |
| `probestreams` | true | probe tracks, aspect ratio and chapters with Kodi's demuxer |
| `suspendae` | true | release ALSA during TV playback |
| `periodicresume` | true | save the resume point every 5 min |
| `es` | true | use the ES path where the file allows it |
| `peeruid` | 2910 | only this uid may connect to the feeder (−1 = log only) |
| `feederconnections` / `feedertimeout` | 16 / 3600 s | feeder limits |
| `filecache` | false | Kodi's `CFileCache` for the main feeder connection |
| `audioindex` | `supported` | how the TV counts audio tracks (4.9) |
| `dvprofile5` | false | test switch for Dolby Vision profile 5 |

### 4.2 How a file finds its path

```
OpenFile
 '- RejectImmediately?  inputstream add-on | disc image / DVD / BD files | PVR / live TV | not video |
 |                      m3u8 mpd ism isml | no bridge (no run key, or watcher switched it off) |
 |                      refused by the TV before in this Kodi run
 |      yes -> fallback to VideoPlayer (or a message for 4K, 4.7)
 '- thread: Prepare()
      1. ProbeStreams(): duration, video (size, DAR, codec, profile, bit depth, HDR, DV, fps,
         interlaced, 3D), audio and subtitle tracks in file order, chapters
      2. TvCanPlay(): AV1 -> no | Dolby Vision without HDR10/SDR/HLG base layer (profile 5) -> message |
         H.264 > 8 bit or 4:2:2/4:4:4 -> VideoPlayer up to 1080p, message above |
         no TV-playable audio: <= 1080p -> VideoPlayer, 4K -> picture without sound + hint |
         > 60 Mbit/s over the network -> hint only
      3. ChooseAudio(), ChooseSubtitle() with Kodi's own language rules (start choice)
      4. CheckEs() -> "" = ES path, otherwise the reason (logged: "CTVPlayer::Prepare - TV player: <why>")
      5. TV player path only: start CTVHttpFeeder (and offer one sidecar subtitle file)
      6. fill CDataCacheCore (codec "TV ES hevc" / "TV hevc", size, fps, DAR, audio, chapters)
 '- write ctl (seq 0) BEFORE play.json; play.json = normal job or ES job
```

Note on the order: `TvCanPlay()` runs **before** `CheckEs()` and judges audio by the TV player's
formats. Reading the code, a file up to 1080p whose *only* audio tracks are TrueHD/FLAC/Opus/Vorbis
therefore goes to `VideoPlayer` although the ES path could convert them, and for such a 4K file the
"picture without sound" hint is queued even if the ES path then plays it with converted sound. Files
with at least one TV-playable track are not affected. This was not checked on the TV.

### 4.3 The loopback HTTP feeder (`CTVHttpFeeder`)

The TV player plays URLs, and it reads with several parallel byte-range connections. Kodi's own web
server was not used: it listens on the LAN and has no access check without a password. The feeder:

- libmicrohttpd, bound to **127.0.0.1** on a port picked by the kernel, one thread per connection (a
  blocked SMB read only holds its own connection);
- serves only `/<32 hex token>/media.<ext>` (extension lowercased without the dot, otherwise just
  `media`; no film title leaves Kodi) and optionally one sidecar subtitle as `media.srt/.ass/.ssa` or an
  `.idx/.sub` pair; only `GET`/`HEAD`, byte ranges via Kodi's `CHttpRanges`, everything else 404/405;
- reads through Kodi's VFS (`XFILE::CFile`: SMB, USB, HTTP), each connection with its own file handle;
- checks every connection on accept: peer must be 127.0.0.1 **and** its socket must belong to uid 2910
  (looked up in `/proc/net/tcp`). Probe P18 showed that the TV player connects as uid 2910
  (`tvmain`, also used by other system processes); this rejects the launcher-uid apps;
- at most 16 connections; a further one closes the connection that was idle longest; idle timeout
  3600 s so that a long pause does not cut the stream;
- `Stop()` waits at most 2 s; a connection blocked in an SMB read is finished in the background (the
  object stays alive through `shared_from_this`). A hanging SMB read must never block Kodi's
  `CloseFile`;
- logs only offsets, never the token or the path.

### 4.4 The signed file bridge

Kodi (root) and `flex.js` (uid 10009) have no shared IPC except files. The launcher can only use paths
below `/var/local/applications/UI` (eMMC); writing several files every 250 ms there would wear the flash.

| Path on the TV | Owner, mode | Created by | Purpose |
|---|---|---|---|
| `/tmp/flexkodi-rt` | root 0755 | watcher | parent; only root may create or rename inside |
| `/tmp/flexkodi-rt/b` | 10009:10009 0700 | watcher | the bridge directory in RAM: only uid 10009 and root |
| `/var/local/applications/UI/flexkodi` | 10009:10009 0755 | watcher (re-set on every full check) | launcher folder; 0755 so nobody else can swap `rt` or plant FIFOs |
| `…/flexkodi/rt` | symlink → `/tmp/flexkodi-rt/b` | watcher | `flex.js` reaches the RAM folder through it (`hisense.file.read/write('flexkodi/rt/…')`) |
| `/tmp/flexkodi.d/key` | root 0600 in root 0700 | `kodi.sh start` | per-run key for Kodi |
| `/tmp/flexkodi-rt/b/key` | root 0444 | `kodi.sh start` | the same key for `flex.js`, deleted after its acknowledgement |
| `/tmp/flexkodi.d/bridge-off` | root | watcher | "bridge off for this boot" |

Kodi always uses the real path (`FLEX_KODI_BRIDGE_DIR=/tmp/flexkodi-rt/b`), never the symlink.

**Frame.** Every file is one line of printable ASCII, at most 2048 bytes:

```
<64 hex HMAC-SHA256(K, "<file name>\n<json>")> <json>\n
```

The file name is part of the MAC, so a valid `ctl` cannot be replayed as `play.json`. `flex.js`
implements SHA‑256/HMAC in ES5 (Cobalt; measured 1.78 ms per HMAC over 300 bytes, RFC 4231 test case
correct). Both sides drop anything with a bad frame or MAC and re-read on the next tick.

**Per-run key `K`.** `kodi.sh start` makes 16 random bytes (`hexdump` from `/dev/urandom`, fallback
`md5sum`), writes them root-only for Kodi and as a **hard link** to a free name `rt/key` for `flex.js`
(see 6.3 for why not `set -C`). Kodi only gets the *paths* (`FLEX_KODI_BRIDGE_KEYFILE`,
`FLEX_KODI_BRIDGE_DIR`, `FLEX_KODI_TVKEYS=1`) and reads the key once (`O_NOFOLLOW`, regular file,
owner 0, 32 hex) — no secret in any environment, no `unsetenv()` in a multithreaded process. `flex.js`
reads `rt/key` once Kodi reports ready and answers with `rt/keyok`; the watcher then deletes `rt/key`
(at the latest 10 rounds after "ready", always when Kodi is gone). Key and token never appear in a log.
After a launcher restart `flex.js` has no key; Kodi then falls back after 5 s.

**Writing and reading.** Kodi writes atomically: `openat(".kp.<random>", O_CREAT|O_EXCL|O_NOFOLLOW)`,
write, `fchmod 0644`, `renameat()` onto the name (rename replaces a planted symlink instead of following
it). Kodi reads with `O_NOFOLLOW|O_NONBLOCK`, accepts only a small regular file and compares the MAC with
`CRYPTO_memcmp`. `flex.js` writes with `hisense.file.write` (not atomic; a half-read file fails the
MAC and is re-read). Probe P5 showed that `hisense.file.write` **follows symlinks** and creates files
as 10009:10009 mode 0666 — one more reason why the folders must be closed to everyone else and why the
watcher removes anything that is not a small regular file. `hisense.file.read` has no size or type
check at all; a FIFO in its path could stall the launcher, so the design rules that case out by
permissions instead of testing it.

**Files.**

| File | Direction | When |
|---|---|---|
| `play.json` | Kodi → flex.js | once per playback (the job); deleted before any fallback |
| `ctl` | Kodi → flex.js | on every change, else every 5 s (desired state) |
| `player.json` | flex.js → Kodi | on every change, else every 5 s (actual state) |
| `tvkeys`, `tvack` | both | volume (3.5) |
| `key`, `keyok` | root → flex.js → root | key hand-over |

```json
play.json (TV player): {"v":1,"sid":"3f9a0c1d22e47b10","t":1730140800,"port":37631,"tok":"<32 hex>","name":"media.mkv","resume":1834,"dar":1.7778,"ov":0,"fb":1}
play.json (ES path):   {"v":1,"sid":"<16 hex>","t":<unix s>,"es":1}
ctl:         {"v":1,"sid":"…","seq":7,"hb":42,"pause":0,"seekN":3,"seekTo":2410,"seekAcc":0,"speed":100,"audio":0,"sub":-1,"stop":0}
player.json: {"v":1,"sid":"…","n":118,"ack":7,"st":"playing","why":"","mp":3,"flags":0,"err":0,"pos":2411,"dur":6904,"seek":1,"eof":0,"acodec":"Dolby Digital","aud":0,"spd":100,"win":"0x220x3840x1720","t":…}
```

- `play.json`: `sid` random per playback; `t` must be ≤ 3 s old for `flex.js` (Kodi waits 5 s, so an
  abandoned job is never taken); `flex.js` builds the URL itself (`http://127.0.0.1:<port>/<tok>/<name>`,
  host fixed, no query string, name must match `^media(\.[a-z0-9]{1,5})?$`); `resume` start second;
  `dar` display aspect ratio; `ov` blend mode; `fb` fallback allowed. A validly signed job with an
  invalid value is answered at once with `refused`/`bad`.
- `ctl` is a **desired state**, not a command stream: a missed intermediate `ctl` loses nothing, a
  repeated one changes nothing. `seq` grows with every content change; `hb` grows with every write **but
  only while Kodi's main thread is alive** (4.5); `seekN` counts jumps, `seekTo` target second,
  `seekAcc` 0 = fast/relative, 100 = exact (slider, chapter, time entry); `audio`/`sub` track indices
  (−1 off, −2 unchanged); `stop:1` ends the playback. A valid `ctl` with a *different* `sid` ends the
  old playback in `flex.js` immediately.
- `player.json`: `n` strictly increasing per `sid`; `ack` = highest **completely applied** `seq` (a
  seek counts only when it has settled); `st` from a positive list (`switching`, `esready`,
  `preparing`, `playing`, `buffering`, `paused`, `ended`, `stopped`, `error`, `refused`); `why` from a
  positive list (`key`, `kodi`, `notHome`, `dialog`, `app`, `busy`, `bad`, `dmpForeign`, `dmpTimeout`,
  `prepTimeout`, `player`, `sourceOut`, `taken`, `superseded`, `kodiGone`, `noSeek`, `noResume`,
  `early`, `stopped`); raw player state, flags and error code; position and duration in whole seconds;
  audio codec text from the launcher's metadata field `[31]`; `t` is for the log only.

**Why counters instead of clocks.** The TV's clock jumps when NTP sets it after boot. Replay protection
and liveness therefore use counters (`hb`, `n`, heartbeat content); Kodi extrapolates the position from
its own `steady_clock` receive time (at most 10 s), never from `flex.js`' wall-clock `t`.

**Threat model, in short.** uid 10009 is not a single process (about ten vendor services run under it,
some with `CAP_DAC_OVERRIDE`; those are firmware and cannot be defended against with file modes). The
goal is that processes of *other* uids reach nothing despite the 0777 folders, and that 10009 processes
get as little as possible: no permanent key, a token per playback, one file per token, feeder accepts
only uid 2910. If anything about the folders is wrong, the watcher switches the bridge off for the boot
and Kodi simply plays everything itself ("fail closed").

### 4.5 The state machine

Kodi side (`CTVPlayer::Process`, 250 ms loop):

| Phase | Entered | Left | Kodi's limit | flex.js' own limit |
|---|---|---|---|---|
| `PREPARE` | `OpenFile` → thread | probe, path decision, feeder, `ctl` + `play.json` written | none | – |
| `WAIT_ACCEPT` | `play.json` written | first `player.json` of this `sid` | **5 s** → `noBridge` | takes jobs ≤ 3 s old |
| `SWITCHING` | `switching` | `preparing`/`esready` | **15 s** → `dmpTimeout` | DMP: 33 × 300 ms ≈ 10 s |
| `PREPARING` | `preparing`/`esready` | `playing`/`paused`/`buffering` | **30 s** → `prepTimeout` | 20 s to player state 2 |
| `PLAYING` | first playing state | `ended`/`stopped`/`error`/`CloseFile` | 15 s without a new valid `player.json` → error | 60 ticks (15 s) without a new `hb` → stop |

Kodi's limits are always larger than `flex.js`' so that the side that gives up first is the one that
can clean up the TV.

On the **first active status** (also if the first one Kodi reads is already `playing`): suspend the
audio engine (`IAE::Suspend()`, wait up to 2 s, ALSA is free for the TV), switch GUI sounds off for
the whole playback, and post `TMSG_SWITCHTOFULLSCREEN`. On the first playing state: `OnAVStarted`
(spinner goes), and **1 s later** `SetVideoRender(true)` and `SetGuiRender(false)` — Kodi becomes
transparent. Until then the full-screen window is drawn opaque black.

**Main-thread heartbeat.** If Kodi's GUI hangs while the TVPlayer thread keeps writing `ctl`, the film
would run on with a dead remote. So the thread posts a static `TMSG_CALLBACK` every 2 s; the callback
only stores a timestamp (it touches no player object, so a late pong is harmless). `hb` grows only if
the main thread answered within the last 5 s; `flex.js` stops the film after 15 s without a new `hb`.

**End evidence.** `ended` counts as "watched" only with evidence: end-of-file flag seen, or position
within 10 s of the duration — the same 10 s in `flex.js` and in Kodi. Otherwise it is a stop at the
real position. Duration: the TV reports whole seconds; the demuxer's duration wins if they differ by
more than 2 %.

**Stops before the picture.** `refused`, `error` before playing, a phase timeout, and `stopped` before
playing with `why` = `early`, `notHome` or `sourceOut` all go through `FailBeforePlaying()` (4.7).
`dialog`, `taken`, `kodiGone`, `superseded` and Kodi's own stop remain plain stops.

**Controls.** Pause, seek and track changes modify `Desired` on the GUI thread (`seq++`), the thread
writes `ctl` within 250 ms, `flex.js` applies it within 250 ms and reports `ack`. While a change is not
yet acknowledged, the OSD shows Kodi's wish (no flicker). FF/RW: the TV player's speed modes
(`setMpctrlFlag`) did not really wind in probe P2 (400 gave about 1.3×), so by default each press jumps
±30 s; `FLEX_KODI_TRICKPLAY=1` enables 2×–16× through the TV player. Chapters come from the probe.

### 4.6 The launcher side of the TV player path (`kodiPlay` in `flex.js`)

Every second, while Kodi reports ready, `kodiTick` loads the key and reads `play.json`. A job is
refused (with a `why`) unless: the home screen is idle and in front, no system dialog, the front module
is the one Kodi started from, no paused YouTube/Netflix (unless the user released it), no screen saver or
picture page, and `DMP` (the TV's "media" input) is not owned by someone else. These checks implement
the launcher mod's protection rules (only touch source and player next to no app, only a source you
switched yourself, delayed steps re-check when they fire).

Then:

1. `UI.store.commit('Source/setCurrentSource','DMP')` (the call the launcher uses itself), wait until
   `DMP` is active (≤ 33 × 300 ms). If our `DMP` is still active from the previous film, skip.
2. `modeljs.mpctrl.PlayMedia([url,null,null,null,null,null,4,null,null,null,'Media'])` — type 4 like
   the launcher's own media player and DLNA. If the player still reports another URL after 3 s, send it
   once more.
3. Poll `getMpCtrlStat([])` every 250 ms. On the TV it returns a comma string
   `<session>,<url>,<state>,<speed>,<caps>,<flags>,…,Media,<error>`. **Start only at state 2**
   ("prepared", visible after about 2.3 s in probe P1); then read the duration, set the video window
   from Kodi's DAR (16:9 → `[0,0,3840,2160,1,0]`, 4:3 → `[480,0,2880,2160,1,0]`) and start: resume with
   `MpCtrl([8,null,<s>])`, else `MpCtrl([1])`. Four seconds after a resume, if the position is more
   than 10 s short, jump exactly (`seekAcc 100`).
4. States 3/4/23/24/2009 → `playing`/`paused`/`buffering`; a set of player error codes ends with
   `error`/`player`; state 6/9 counts as end only after preparation.
5. Apply `ctl`: pause, seeks (fast = relative like the launcher's player, exact = seek flag 100 plus
   position), audio track (`setAudioTrack`, then read `[31]` from 1.5 s every 500 ms up to 5 s before
   acknowledging, so Kodi can tell whether the TV accepted the codec), subtitle track plus five style
   values like the launcher's player.
6. The source must stay `DMP`: two consecutive misses → `stopped`/`sourceOut`, and the source counts as
   foreign until Kodi ends.

**Ending playback** (`kpEnd`). Clear the video window while the same item still owns it.
Allow the window operation to finish before the scheduled stop. Re-check the playback
session identity before completing a deferred stop. DMP remains selected while Kodi
still owns playback. This avoids interfering with a native application or HDMI input.

**Next episode.** A new `play.json` while the old `sid` plays: the player switches directly with the
next `PlayMedia` (measured in P2, the launcher does the same), without a stop and without a source change.

### 4.7 Fallback

`FailBeforePlaying(why)`:

1. If `ctl` was written: `stop:1` (flex.js may still be switching).
2. **Delete `play.json` first**, so an abandoned job can never be taken later.
3. `why == "player"` (the TV refused the file): remember the path for this Kodi run; next time directly
   to Kodi's player.
4. On the ES path (`why` = `es`, `prepTimeout` or `bad`): remember the file for the ES path and fall back
   to the **TV player** (5.7).
5. Otherwise, if `FallbackAllowed()`: `RequestFallback()`; else an error toast with a way out (E2).

Dolby Vision profile 5 (`dvp5`) never falls back (wrong colours either way). `FallbackAllowed()` is
false with `<fallback>false</fallback>`, for video wider than `fallbackmaxwidth` (1920), for HEVC
Main 10, and for AV1/VP9 above 1080p — Kodi's software decoder cannot play those on this CPU, so a
fallback would only produce a slideshow. Without probe data the library's stream details decide.

`RequestFallback()` uses one cancel mechanism: an atomic `g_pendingFallback` holds the generation of the
only fallback that may still start. It is set together with the check `m_end == NONE` under the
player's lock; `CloseFile()` and `OpenFile()` clear it under the same lock; the main thread takes it by
compare-and-swap right before `g_application.PlayFile(item, "VideoPlayer", true)` — the same path as
Kodi's "switch player" action, with the start offset and start percent of the original request. A user
stop or another file during a pending fallback therefore never starts the old film in `VideoPlayer`, and
"the first end wins" (`SetEnd`): a user stop never turns into an error dialog.

Typical hints (German UI): "TV-Player nicht erreichbar, Kodi spielt selbst" (`noBridge`), "YouTube/Netflix
ist im Hintergrund pausiert…" (`app`), "Quelle 'Medien' ist belegt…" (`dmpForeign`), "TV kann die Datei
nicht abspielen (Code n)" (`player`), "Echte Bildrate hier nicht möglich, der TV-Player übernimmt" (`es`).

### 4.8 Resume points and watched state

- Start position: `starttime`, else `startpercent` × duration, rounded to whole seconds (the TV knows
  only seconds), passed as `resume` (TV player) or `startMs` (ES engine).
- At the end `OnExit` writes the bookmark and **exactly one** end callback, in this order: real position
  (or the duration for a proven `ENDED`), then `OnPlayBackEnded`/`Stopped`/`Error`. A fallback saves
  nothing (the next player continues).
- Not seekable with a resume point (`noResume`): up to 1080p `flex.js` refuses and Kodi's player resumes
  at the right second; for 4K the TV starts at 0 and Kodi keeps the **old** resume point unless the user
  watched past it.
- Every 5 min of playback an intermediate `OnPlayerCloseFile` saves the position, so a crash, `kill -9`
  or Power held for 5 s costs at most 5 minutes. It never saves beyond Kodi's "watched" threshold or in
  the ignore-at-end zone, so "watched" cannot flip early.

### 4.9 Audio and subtitles on the TV player path

- `ChooseAudio()`: Kodi's language rule first (the file's default flag counts before channels and format),
  then the best codec over ARC: E‑AC‑3 with Atmos (JOC) > E‑AC‑3 > AC‑3 5.1 > DTS > multichannel AAC > stereo.
- TrueHD, FLAC, Opus and Vorbis are never handed to the TV player (`tvSupported=false`): FLAC is refused
  and TrueHD is accepted but stays silent ("UNKNOWN").
- Track numbering for `setAudioTrack(n)` is not settled (probe P3): `<audioindex>supported</audioindex>`
  counts only TV-playable tracks — a hearing test with a multi-language Blu-ray rip matched this — while
  a synthetic test file matched plain file order (`file`). Every film logs
  `audio file -> TV index (audioindex …): 0 ac3 eng -> 0, …`.
- If the codec text in `player.json` does not fit the chosen track, Kodi sends the previous track again
  and shows "TV: Tonformat nicht unterstützt".
- Subtitles: internal tracks by index; one sidecar text file (or `.idx/.sub`) is offered by the feeder
  as `media.<ext>` in case the player fetches it (whether it does is open). Subtitles added later
  (download) cannot be given to the TV; Kodi offers "Play using… → VideoPlayer" (up to 1080p).
- Lip sync over ARC: only the TV's "digital audio delay" setting has an effect, stored per input.

---

## 5. The ES path: native frame rate

### 5.1 Why

The TV's file player (USB player, DLNA, `PlayMedia`, and with it the TV player path) always outputs
23.976p and 24p at 59.94 Hz with 3:2 pulldown; 25p/50p get 50 Hz, 30p/60p get 60 Hz. No motion setting,
no `mm_cfg.ini` key for the file-player profiles (local `[app0]` and HTTP `[app2]`; one interlace experiment changed cadence),
no app type, uid or process name changed that. Netflix, however, shows 24p natively. Its player
library creates a media item **from codec attributes** and pushes elementary-stream packets; the media
service then uses its MultiDecode branch ("mstplayer"), which passes the content rate to the panel.
The full research is in the root-mod chapter 06; the results that matter here:

| Playback | IRQ 51 (video path output) | scaler input `Vfreq` (0.1 Hz) |
|---|---|---|
| Netflix film, 24p | 23.980/s | 239 |
| TV player, 23.976p file | 59.94–60.2/s (60.07/s over `file://`) | 239 or 599 |
| own ES test client, 1080p H.264 + E‑AC‑3 | 23.970/s | 239 |
| own ES test client, "Tears of Steel" 4K HEVC (exactly 24.000 fps), 120 s window | 23.998/s | 240 |
| own ES test client, HEVC Main 10, 50 and 80 Mbit/s (peaks 121/152 Mbit/s) | ≈ 24 (8 s window) | – |

IRQ 51 appears only during video playback and runs at the video path's output rate; it is the reliable
indicator (the launcher's `getVideoFormatInfo()` is not). `Vfreq` in the kernel's `BW Window` line
describes the scaler's *input*: the TV player run shows that it can read 239 while the output is
60 Hz. An 8 s window separates 24 from 25/30/60 Hz but not 23.976 from 24.000. The TV's 100 Mbit/s
network port delivers at most about 11.7 MB/s, so higher sustained rates cannot arrive over the
network.

### 5.2 Process split

```
Kodi process (static libc++)                          flex_es_helper (glibc, vendor C++ libs)        media service
CTVPlayer --(esready)--> CTVEsEngine                  dlopen libdrvIPAUTH.so, libmsrv_mmplayer_ipc   MiMwIpcSrv
  demuxer thread: Kodi's FFmpeg demuxer               MI_AOUT_Init, MM_Initialize, AddEventRecipient  "mstplayer"
  Annex B, DV RPU removal, VC-1 fix-ups, AC-3         MM_CreateMediaItem, MM_PushPayload, ...  --Binder-->
  -- fd 3: SOCK_STREAM, es_rec + payload ------->     push thread (lead control, retries)
  -- fd 4: SOCK_SEQPACKET, es_cmd -------------->     control thread
  <------------------------------- es_status (every 200 ms, status thread)
```

Why a separate process:

- the vendor libraries need `LD_PRELOAD` in a fixed order (circular dependency between
  `libdrvIPAUTH.so` and `liblinux.so`; a single `dlopen` fails) and their own old C++ runtime, which
  must not meet Kodi's in one address space;
- if Kodi dies, the helper sees the end of its control socket and tears the item down cleanly
  (`MM_Stop`, `MM_DeleteMediaItem`, `RemoveEventRecipient`, `MM_Finalize`) — the media service does not
  restart after a crash, and apps would have no video until the TV reboots.

Spawning (`CTVEsEngine::SpawnHelper`): two `socketpair`s (`SOCK_STREAM` for data, `SOCK_SEQPACKET` for
control), `posix_spawn` with `POSIX_SPAWN_USEVFORK` (Kodi's address space is far too large to copy for
one `exec`), stdin `/dev/null`, stdout/stderr → `/tmp/flexkodi.d/es-helper.log`, the sockets dup'ed to
**fd 3 and fd 4**. Kodi's `LD_PRELOAD` (libSegFault) and `LD_LIBRARY_PATH` are replaced by:

```sh
LD_LIBRARY_PATH=/tvservice/mslib:/tvservice/conflictlib:/mslib/utopia:/mslib
LD_PRELOAD="libmi_glibc.so libmdroid.so libmmapinfo.so libiniparser.so libdrvIPAUTH.so liblinux.so libmsrv_mmplayer_ipc.so"
```

The helper first closes every file descriptor above 4 (Kodi would otherwise hand it sockets, files and
the grab on the remote). On the owner's TV `lib/kodi/flex_es_helper` is additionally a small wrapper
script that starts the real binary with `env -i` (only `PATH`, `LD_LIBRARY_PATH`, `LD_PRELOAD`); a
package install replaces that hand setup with the plain binary. The helper is plain C, built in
`package-kodi.sh` with the project's cross compiler (`zig cc` for glibc 2.18), `-ldl -lpthread` and
the linker script `native/glibc218-relocations.ld`, which keeps unwind tables out of the range the TV's
old loader merges (`.rel.dyn`/`.rel.plt`); without it the loader crashes before `main`. It links no C++
runtime at all.

### 5.3 Protocol (`TVEsProtocol.h`)

Same machine, same process image: the structures go over the sockets as they are.

| Message | Fields | Meaning |
|---|---|---|
| `es_rec` (fd 3) | `type` (0 audio, 1 video, 2 EOS), `size`, `gen`, `pts_ms` + payload | one packet; packets of an older generation are dropped by the helper |
| `ES_OP_OPEN` | `vcodec`, `acodec`, `fps_mhz`, `gen` | create the first media item |
| `ES_OP_PAUSE` | `flags` 1/0 | `MM_SetTrickMode` `{1,1}` pause / `{0,1}` normal |
| `ES_OP_FLUSH` | `gen`, `ES_FLUSH_RECREATE` | seek: flush the item, or a new item (other audio codec) |
| `ES_OP_RECT` | `x, y, w, h` on 3840×2160 | `MM_SetDisplayRectangle` |
| `ES_OP_STOP` | – | tear down and exit; EOF on the control socket means the same |
| `es_status` (fd 4) | `state` (connect, preload, playing, paused, ended, error), `gen`, `pts_ms` (`MM_GetVideoPTS`), `buf_ms`, `err`, counters video/audio/retried/lost/decode errors | every 200 ms |

Error codes: 1000 vendor library or symbol missing, 1001 `MM_CreateMediaItem` refused (another player
holds the service, e.g. the screen saver's photo player), 1002 no `INIT_OK` within 10 s, otherwise the
service's event number (init fail 4, audio unsupported 17, not supported 18).

`CTVEsEngine::GetStatus()` turns `es_status` into the same `TVStatus` that `player.json` produces, with
an exact `posMs`. For `CTVPlayer` the engine replaces `flex.js`' player: `Apply(ctl)` takes the same
desired state. A seek is acknowledged when the helper reports the new generation within 15 s of the
target (key frames can be that far before it), or after 5 s.

### 5.4 The engine (`CTVEsEngine`, Kodi side)

- Opens the file with Kodi's own input stream and demuxer (`CDVDFactoryInputStream`,
  `CDVDFactoryDemuxer`), so SMB, HTTP and local files work; there is no HTTP server on this path.
- **Video codes** for `ST_MEDIA_INFO +0x50` (measured on the TV by creating items without `MM_Play` and
  reading the decoder type from the kernel log): MPEG‑2 `1`, H.264 `7`, VC‑1 Advanced `12`, HEVC `0x14`.
  Interlaced material returns 0 (not tested on this path, stays with the TV player).
- **Frame rate** for `+0x5c`: `round(fps × 1000)` in mHz, accepted for 1–121 fps.
- H.264/HEVC are converted to Annex B by `CBitstreamConverter` with `SetRemoveDovi(true)`: Dolby Vision
  RPUs are removed, the TV gets the base layer.
- **VC‑1 from Matroska has no PTS** (VFW mode, only decode-order stamps). The engine detects this
  (real PTS run backwards at B frames, VFW stamps only grow), prepends the sequence header from the
  codec private data to the first frame, and rebuilds display times per group: an anchor (I/P) is shown
  after the B frames that follow it in the stream. B frames right after the first key frame of a cut
  belong to the missing group before it and are dropped.
- Missing stamps (containers that stamp only some frames) are filled at 1/fps.
- After a start position or seek, packets are dropped until a real key frame (H.264 IDR or SPS — open-GOP
  streams have no IDR —, HEVC IRAP or VPS/SPS, VC‑1 sequence header or entry point, MPEG‑2 sequence
  header), at most 2000 packets; no audio before the first picture.
- **Audio**: AC‑3 and E‑AC‑3 pass unchanged (code 5). Every other track FFmpeg can decode is converted
  on the fly by `CTVAudioTranscoder`: DTS core only, TrueHD up to its 5.1 substream, resampled to 48 kHz,
  ≥ 3 channels → AC‑3 5.1 at 640 kbit/s (7.1 down-mixed), else stereo at 256 kbit/s; timestamps from the
  packets, a new timeline after each seek. Measured on the TV, one core of four: DTS 5.1 9.5 %, TrueHD
  ≈ 13 %, FLAC 7.1/96 kHz 21.5 %, Opus 5.1 12.7 %, AAC 5.1 7.3 %, PCM 7.1 6.1 %. AAC is converted rather
  than sent as code 6 because the code says a run that tried code 6 ended with the TV switching off
  (not analysed).
- **Audio track change** creates a new media item (`ES_FLUSH_RECREATE`), because the codec may change.
  It restarts 2 s before the last pushed picture (which runs about 1.5 s ahead of the playback
  position). Tested with a 14-track Blu-ray rip: start on the default DTS-HD MA track (converted), then
  two switches to other DTS tracks, each with a new item at the same place, 1.4 s buffer, no decode
  errors.
- **Window**: `ST_MEDIA_INFO` has no aspect-ratio field, so for non-16:9 content the engine sends a
  letter- or pillarbox rectangle (e.g. 4:3 → 2880×2160 at x = 480).
- `WriteAll()` blocks with `poll` when the helper is not reading: that is the normal back-pressure when
  the TV's buffer is full.

### 5.5 The helper (`native/flex_es_helper.c`)

Call order (the same as the measured test client and as Netflix' library):

1. `dlopen` the libraries, resolve the `MSrv_MM_PlayerInterface` methods by their mangled names
   (`MM_Initialize`, `MM_CreateMediaItem`, `MM_PushPayload`, `MM_Play`, `MM_Stop`,
   `MM_DeleteMediaItem`, `MM_Finalize`, `MM_SetTrickMode`, `MM_SetFlushData`, `MM_SetDataEnd`,
   `MM_SetDisplayRectangle`, `MM_GetVideoPTS`, `MM_GetVideoDataBufferInfo`, `AddEventRecipient`, …) and
   `MI_AOUT_Init`.
2. `MI_AOUT_Init` (its parameter block needs its own size in the first 16 bits, otherwise "unknown
   error"), construct the interface object, `MM_Initialize()` — no app type is needed —,
   `AddEventRecipient()`.
3. Wait for `ES_OP_OPEN`, fill the 0x1b0-byte `ST_MEDIA_INFO` like Netflix' library does (version,
   magic, size, video code `+0x50`, frame rate `+0x5c`, audio code `+0xa0`, the other constant fields as
   observed), `MM_CreateMediaItem`.
4. **Preroll**: push up to `FLEX_ES_PREROLL_MS` (default 1200 ms) of video PTS and wait for event
   `INIT_OK` (3, came after about 0.4–0.5 s in the tests; error 1002 after 10 s). **Never call
   `MM_Play` before `INIT_OK`**: the "mstplayer" branch aborts on a failed `CHECK` and takes the whole
   media service down.
5. `MM_Play` in its own thread (it can take a while inside the service; no status queries while it is
   there, they would block). After `PLAYING_OK` (19): state `playing`, apply window and pause.
6. Push with lead control: keep `FLEX_ES_TARGET_MS` (default 1500 ms) between the last pushed video PTS
   and the playback position from `MM_GetVideoPTS`. `MM_GetVideoDataBufferInfo` kind 3 behaves like
   milliseconds in the buffer but jumps too coarsely (1.5–3 s at a 1.5 s target) and is only used until
   the position belongs to the current generation.
7. **`MM_PushPayload(item, &{addr, size, pts_ms, type}, NULL, 0)`** — the last two arguments are extra
   data inside the Binder transaction. The first client passed the frame there too, so every frame went
   twice, and from about 1.04 MB the whole call failed **silently** (return 0, a kernel line
   `binder_alloc_buf … failed, no address space`): key frames of a 4K HEVC stream were lost. The frame
   itself travels through shared memory via `info.addr`. Refused pushes (full ES buffer) are retried for
   up to 2 s like Netflix' "sample retry".
8. Seek: `MM_SetFlushData(item, 2)` (audio and video), wait for event 24 (`ES_FLUSH_DATA_RDY`, at most
   3 s), continue with the new generation. Recreate: stop and delete the item, create a new one.
9. End: on EOS `MM_SetDataEnd`, then `DECODE_DONE` (9) **or** a position that has not moved for 3 s
   while not paused → `ended`.
10. Teardown: `MM_Stop` exactly once per item and never from the push thread (a push can wait inside the
    service and only returns after `MM_Stop`), `MM_DeleteMediaItem`, `RemoveEventRecipient`,
    `MM_Finalize`. `SIGTERM`/`SIGINT` set the stop flag; `SIGPIPE` is ignored.

Kodi stops the helper with `ES_OP_STOP`, closes both sockets and waits up to 3 s (`SIGTERM` after
1.5 s). **It never sends `SIGKILL`**: according to the source comment, after a `kill -9` in a test the
next playback ended with the TV switching off. If the helper does not end, it is left alone and logged.

### 5.6 Which files take the ES path (`CheckEs`)

The ES path is the default. The film goes to the TV player instead when the log says:

| Reason | Why |
|---|---|
| `switched off` | `<es>false</es>` or `FLEX_KODI_NO_ES=1` |
| `no probe` | no probe data |
| `failed before` | this file already failed on the ES path in this Kodi run |
| `video <codec> [interlaced]` | not one of the four mapped progressive codecs |
| `frame rate unknown` | no usable fps |
| `3D` | stereo modes not handled |
| `no audio track for this path` | nothing AC‑3/E‑AC‑3 or convertible |
| `subtitles on` | the ES path has no subtitles yet; the TV player can play the same language with subtitles |
| `audio language only as <codec>` | the wanted language exists only in a format neither path can play |

If subtitles are wanted but the chosen language exists only in a format that needs conversion
(TrueHD, FLAC, …), sound wins: ES path without subtitles and the hint "Untertitel aus: diese Tonspur
geht nur ohne Untertitel". On the ES path the audio menu marks converted tracks "(als Dolby Digital)",
and Kodi's info screen shows e.g. `TV dtshd_ma -> AC3`.

### 5.7 The launcher side and the fallback chain

The ES job (`{"es":1}`) has no URL. `flex.js` runs the same acceptance checks and the same `DMP`
switch, then:

- If the TV's media player is still busy (a URL loaded and a state other than 0/5/6/7/9), it stops it
  with `MpCtrl([3,-1,0,'Media'])` and waits **2.5 s** (`KP_ES_SETTLE_MS`): on 2026‑11‑05 an immediate
  stop took effect late and hit the media item Kodi had just created — the first video had no picture.
- It reports `st:"esready"` ("source ready, Kodi's helper plays"). Kodi then starts `CTVEsEngine`.
- From then on `flex.js` only watches the source (two misses → `sourceOut`) and Kodi's `hb`, and
  acknowledges `ctl.seq`; position and state come from the helper.
- The launcher is not made transparent (the ES job has `ov` 0); Kodi's window in copy mode carries the
  alpha.

Fallback chain: if the ES path fails **before the picture** (helper cannot start, `error`/`es`, the
helper vanishes, `prepTimeout`), `RequestFallback("es")` restarts the same `TVPlayer` object for the same
file with the TV player path (`m_startedNotified` is reset so no end callback overtakes the new start),
and the file is not tried on the ES path again in this Kodi run. If the TV player then refuses it too,
the normal fallback to `VideoPlayer` (or the 4K message) follows.

On `OnExit` the ES engine is stopped **before** `ctl stop:1` is written: `flex.js` must not touch the
source while the media item still exists.

Verified in daily use: Blu-ray rips (judged smooth by the owner), a 4K HEVC 25p clip whose scaler
input line read `Vfreq=250`, progressive MPEG-2 DVD rips. **Not recorded** in the sources: an IRQ 51
measurement of a film started from Kodi's ES path (the rate measurements above are from the test
client with the same call sequence), HDR content, and whether a progressive MPEG‑2 rip reaches the
scaler as 576p rather than 576i.

---

## 7. Lifecycle walkthroughs

### 7.1 Start

```
OK on Kodi tile ──> flex.js: checks (home idle, no dialog, apps handled), Denon to TV audio,
                    write flexkodi/start, toast "Kodi startet …" (waits up to 25 s)
watcher (≤ 1 s) ──> fresh? -> kodi.sh start: lock, data dir, flex_app_close, env, key, setsid kodi-run.sh
kodi-run.sh     ──> pid, heartbeat "… 0 0 0" every 2 s, exec kodi-directfb
Kodi            ──> DirectFB window (layer choice, DWSC_UPPER), EGL, GUI
                    CEvdevInputHandler: open event0, EVIOCGRAB, write "ready" -> heartbeat "… 1 0"
                    tvkeys registration (every 1 s, ≤ 10 s)
flex.js         ──> heartbeat ready: toast off; read rt/key, write rt/keyok; answer tvkeys with tvack
watcher         ──> sees keyok -> deletes rt/key
```

### 7.2 Play (ES path)

```
t=0     OpenFile -> OnPlayBackStarted (spinner) -> thread: probe, CheckEs = ES
        ctl {seq 0, hb 1}, then play.json {"es":1}                              phase WAIT_ACCEPT
≤1 s    flex.js: job valid, guard ok -> player.json "switching", DMP command     phase SWITCHING
        Kodi: AE suspended, GUI sounds off, fullscreen
        DMP active; media player busy? stop + 2.5 s
        player.json "esready"                                                   phase PREPARING
        Kodi: CTVEsEngine::Start -> posix_spawn helper, ES_OP_OPEN, ES_OP_RECT (if not 16:9)
        helper: libs, MM_Initialize, MM_CreateMediaItem; engine: demux, seek to resume, wait key frame
        helper: preroll 1.2 s -> INIT_OK -> MM_Play -> PLAYING_OK -> es_status "playing"  phase PLAYING
        Kodi: OnAVStarted (spinner off)
+1 s    SetVideoRender(true): Kodi transparent, film visible; OSD on top
loop    every 250 ms: es_status -> position; ctl hb++ every 5 s (flex.js watches DMP + hb)
        every 5 min: intermediate resume point
```

### 7.3 Play (TV player path)

Same start, but Kodi starts the HTTP feeder and writes the normal `play.json`; `flex.js` calls
`PlayMedia` (state 0 → 1 → 2 in about 2.3 s in probe P1), sets the DAR window at state 2, starts or
resumes, reports `preparing` → `playing`; the TV player fetches the file from `127.0.0.1` with range
requests (uid 2910 checked). Seeks, pause and tracks go through `ctl`/`player.json`; the design
estimate is 0.3–0.6 s between key press and effect (four 250 ms hops at most). Measured on
2026‑10‑29: about 7 % CPU for Kodi during playback, a 25p DVD rip output at 50 Hz.

### 7.4 Stop, next file

```
Stop in Kodi -> CloseFile: m_end = STOPPED, ctl stop:1
OnExit: SetVideoRender(false) (opaque again)
        ES: engine Stop -> ES_OP_STOP -> helper MM_Stop, MM_DeleteMediaItem, MM_Finalize; reap (≤ 3 s)
        ctl stop:1 written, play.json removed
        TV player path: flex.js window first, MpCtrl stop 0.4 s later; feeder Stop (≤ 2 s, rest in background)
        AE resumed (ALSA back), bookmark at real position, exactly one OnPlayBackStopped
Source: DMP stays while Kodi runs.
Next file: new sid; the old ctl is foreign to flex.js -> old playback ends; DMP still ours -> no switch.
```

### 7.5 Minimize and restore

See 3.6. During a film: stop first (resume point saved), Kodi stays visible 1.5 s, then hides, releases
the remote and reports `minimized`; `flex.js` treats launcher keys as wanted and does not refresh its
timers. The Kodi tile restores in 1–2 s without a restart; any other app or source ends the minimized
Kodi; after 10 min `flex.js` ends it so that screen saver and standby work again.

### 7.6 Quit

- **Power once**: Kodi quits (player closed first → `ctl stop:1`, film stops within 250 ms), the grab is
  released; `kodi-run.sh` removes `running`; `flex.js` sees "gone" after 2 ticks, leaves `DMP` after 3
  (to a physical last input). Power again: TV standby.
- **Kodi's own exit menu**: the same, with the "Kodi wird beendet …" hint in the launcher while Kodi
  takes seconds to exit.
- **Power held 5 s**: `_exit(3)` from the input thread; treated like a crash.

### 7.7 Crash and hang matrix

| Event | Who notices, how fast | Kodi | `flex.js` / helper | Input afterwards | Resume point |
|---|---|---|---|---|---|
| Kodi crashes | `kodi-run.sh` removes `running` about 1 s after the exit; `flex.js` after 2 ticks | libSegFault backtrace in `run.log`, exit status 139 | stop the TV player; ES helper sees socket EOF and tears its item down; `DMP` left after 3 ticks | last physical input, else stays `DMP` | last periodic save (≤ 5 min) |
| Kodi and `kodi-run.sh` killed (`running` stays) | content unchanged for 15 ticks | – | stop the player, **`DMP` stays** (end not certain) | `DMP` | last periodic save |
| Kodi's GUI hangs, TVPlayer thread alive | `hb` stops (no main-thread pong); `flex.js` after 15 s | Power held 5 s ends it | stop the player | `DMP` until Kodi ends | last periodic save |
| `flex.js` gone (launcher restart, practically only with a TV reboot) | Kodi: 15 s without `player.json` | error toast; `INIT_LOADING` makes `flex.js` stop Kodi | new instance has no key and touches nothing | orphaned `DMP`; next job `dmpForeign` | saved |
| Standby (`STR_SUSPENDING`) | `flex.js` | ended via `flexkodi/stop` | source and player left to the launcher | launcher's | saved if Kodi ends in time |
| App / cast / DIAL / CEC input switch | front module changes | ended | `kpForeign`: never touch source or player again | the app's | saved |
| Source changes without a module change (e.g. CEC from the receiver) | two missed `DMP` checks | playback ends (`sourceOut`), Kodi keeps running | further jobs refused (`dmpForeign`) | new source | saved |
| TV refuses the file before the start | player error code | fallback to `VideoPlayer` at the same position (4K: message) | `error`/`player` | `DMP` | unchanged |
| Network/SMB breaks | TV buffers, then error codes | "buffering", then error; feeder stop does not block | `buffering` → `error` | `DMP` | saved |
| Bridge files tampered | MAC/frame | ignored → timeouts → fallback | ignored | – | – |
| Watcher detects swapped folders | `rtdir` | next playback `noBridge` (Kodi plays itself) | `rt` symlink gone | – | – |
| Media service crashes | – | – | – | no video for any app until reboot (`reboot` suffices) | – |

---

## 8. Pitfalls and dead ends

- **DirectFB structure sizes** (2.2) and **the launcher's layer moving between boots** (2.3).
- **Alpha below 1 shows the video plane** (2.6). Clear only the alpha channel before each swap.
- **OpenSSL 1.1.1 `ARMV7_TICK`** → `SIGILL` on some cores; set `OPENSSL_armcap` (6.3).
- **Package ownership**: an archive built as uid 1000 becomes the TV's `SystemService` user, which would
  own scripts root runs. Pack with `--owner=0 --group=0 --numeric-owner`; `install.sh` runs
  `chown -R 0:0 /APPS/kodi`.
- **`/APPS` space**: below 150 MB free Kodi does not start without a USB stick; keep at most one backup
  on the TV. Image caching and thumbnail extraction from video files filled `/APPS` and, worse, Kodi's
  non-cancellable thumbnail jobs kept it hanging for 15 min at exit; video-frame thumbnails are off.
- **Native session lifecycle.** Validate the active item before a display operation,
  wait for the ready event before starting the decoder, and pass each access unit once.
  Release the owned session before opening the next player.
- **Launcher getters lag behind setters**; always wait and read again.
- **The screen saver mutes the main mixer**; a fresh switch to the media input lifts it (no sound in the
  first film after the screen saver otherwise).
- **Paused Netflix/YouTube keep the launcher's background flags set** until the next app start, which
  blocks the TV player under the protection rules; hence the release flow and `flex_app_close`.
- Dead ends for 24p, all measured: `mm_cfg.ini` keys for the file player (only `force_interlace` changed
  the rate, to 48 fields), faking the display getters inside the media service, app type/uid/process
  name for file playback, GStreamer `mdmp` with an app-type hint, GStreamer ES into `dfbvideosink` (renders
  on the UI layer). Rejected by the owner: 25p speed-up, an HDMI player, re-encoding the library.
- Tracing Netflix with `LD_PRELOAD` does not work (it stays at the logo); its player library was read
  statically instead.

## 9. Open points

- Restoring a minimized Kodi through the tile: not yet confirmed on the TV.
- OSD over a running film: the transparency mechanism is built and in use; the systematic test (S5),
  the GOP mixing formula (P10) and possible dark seams at half-transparent edges (P15) are open; the
  `blend`/`straight` switches stay off.
- Audio track numbering for the TV player (`audioindex`, P3) and whether the TV player fetches a
  sidecar subtitle file.
- ES path: no subtitles, interlaced material untested, AAC native untested, HDR untested, no IRQ 51
  measurement of a Kodi-started ES film, 23.976 vs. 24.000 not separated in a long window for a
  23.976 film.
- Error matrix (S7) not walked row by row on the TV; default player rules (S8) still a userdata rule.
- Kodi sometimes needs several seconds to exit (ALSA `Broken pipe`, "Stopping all"); the launcher
  shows a hint meanwhile.
- `tvkeys` does not check `bridge-off`: after the bridge is switched off mid-run one press is lost
  before Kodi's own volume applies.
