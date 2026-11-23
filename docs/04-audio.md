# Kodi audio, ARC and receiver volume

The reference setup uses HDMI ARC and an AVC-X3800H. The TV UI and Kodi volume control
follow the existing audio/CEC path. Capability for formats beyond the tested AC-3 output
depends on the source track, selected playback path and receiving devices.


## 1. Summary

| Topic | State | Since |
|---|---|---|
| Sound of every film comes from the **TV's** decoder chain (Dolby MS12), not from Kodi | built, in daily use | 29 Oct 2026 |
| Link to the receiver | HDMI **ARC** only (no eARC on this TV); lossless formats can never reach the receiver | static analysis |
| ES path (native 24p): AC-3 and E-AC-3 pass unchanged | measured | 4/5 Nov 2026 |
| ES path: **DTS, DTS-HD, TrueHD, FLAC, Opus, Vorbis, AAC, MP3, MP2, PCM** converted to AC-3 (5.1 at 640 kbit/s, stereo at 256 kbit/s) while playing | measured on the TV, all 10 bench checks passed | 17 Nov 2026 |
| Track choice honours the file's **default flag** before format and channels | measured on the TV | 17 Nov 2026 |
| Volume/mute keys in Kodi change the TV (speakers) or the receiver (ARC) | TV speakers measured 29 Oct; ARC confirmed with the volume ring 18 Nov | |
| Kodi's own software volume stays at 100 %, not muted | built | 29 Oct 2026 |
| Kodi's volume ring shows the **receiver's** volume (read from its HTTP status page) | confirmed by the owner on the TV | 18 Nov 2026 |
| Which format the receiver actually decodes for each TV menu setting (PCM / Pass Through / Auto / DD / DD+) | **not measured** | open |

The short version of the design:

- Kodi never outputs film sound itself. On both playback paths the compressed audio goes into the TV's
  media service, through Dolby MS12 and out over ARC. Kodi's ALSA device is released during playback.
- The ES path only knows one audio codec number that is proven to work (5, `E_AUDIO_CODEC_AC3P`,
  used for AC-3 and E-AC-3). Everything else is decoded by Kodi's FFmpeg and re-encoded to AC-3 in the
  demuxer thread. The conversion path outputs AC-3 up to 5.1 anyway, so nothing is lost that would have
  reached the receiver; only a lossy-to-lossy generation is added for DTS.
- Kodi grabs the remote exclusively, so the TV never sees the volume keys. Kodi counts them in a signed
  file; the launcher mod replays them as real key presses through the same call the launcher's voice
  control uses over ARC. The receiver's resulting volume is read back from its status page and shown
  in Kodi's own volume ring.

---

## 2. The physical limit: ARC, not eARC

### 2.1 Evidence that the TV has ARC only

- The datasheet lists `HDMI1 HDMI2.0--ARC`.
- `modeljs.sound` and `modeljs.cec` (the launcher's JavaScript API) have no function containing
  "earc". The libraries in `/mslib`, `/tvservice` and `/usr/local/lib` contain only ARC identifiers:
  `cecInitiateArcRx`, `cecTerminateArcRx` (`libhalsystem.so`), `E_MI_AOUT_PATH_HDMI_ARC`,
  `MI_AOUT_HdmiArcCapability_s`, `MW_Sound::setHDMIArcOutputType(EN_DIGITAL_…)` (`libMW.so`).
- `modeljs.sound.getDolbyAudioCertVersion()` returns
  `"Dolby MS12 Multistream Decoder with Atmos (no HE AAC)"` (measured, read-only getter).

### 2.2 What ARC can and cannot carry

ARC is essentially S/PDIF (IEC 60958, compressed payloads framed per IEC 61937). It carries at most
the four-times 48 kHz frame that Dolby Digital Plus needs. TrueHD and Dolby MAT need the HBR frame,
which exists only on the forward HDMI path or on eARC.

| Format | Over ARC to the receiver |
|---|---|
| Dolby Digital (AC-3), up to 5.1 | yes |
| Dolby Digital Plus (E-AC-3), including Atmos as JOC | yes (Denon's own FAQ says DD+ over ARC works "without guarantee") |
| DTS core, up to 5.1 | yes |
| PCM 2.0 | yes |
| Dolby TrueHD, TrueHD Atmos, Dolby MAT | **no** |
| DTS-HD MA (lossless part), DTS:X | **no** (only the DTS core) |
| Multichannel PCM (5.1/7.1) | **no** |

Consequences that shaped the whole design:

- The best a Blu-ray track can ever deliver to the receiver through this TV is lossy 5.1
  (AC-3 or DTS core), or DD+ for streaming-style E-AC-3 files.
- Atmos is only possible from E-AC-3 tracks with JOC. TrueHD Atmos cannot be turned into DD+ Atmos:
  FFmpeg's E-AC-3 encoder writes at most 5.1 and no JOC, and the TV has no TrueHD decoder whose output
  MS12 could re-encode.
- Lossless audio would need the player on one of the receiver's HDMI **inputs** (or an eARC TV). Then
  the TV's own hardware decoder is not involved, which is outside this project.

---

## 3. The audio chain inside the TV

```
 Kodi (demuxer, track choice, optional AC-3 transcoder)
   ├─ TV-player path: local HTTP feeder 127.0.0.1 → modeljs.mpctrl.PlayMedia → libmmp (MKV reader)
   └─ ES path:        flex_es_helper → MM_CreateMediaItem / MM_PushPayload → mstar.mmplayer service
        └─ OMX audio decoder (OMX.MS.AC3.Decoder, OMX.MS.DTS.Decoder, AAC, LPCM, FLAC)
           or OMX.MS.Passthrough.Decoder
             └─ Dolby MS12 v2.2 (decoder, mixer)
                  └─ SPDIF branch of the MStar audio DSP ─ HDMI ARC ─ receiver input "TV Audio"
 CEC in parallel: Initiate ARC, the receiver's Short Audio Descriptors, System Audio Mode,
                  volume as <User Control Pressed>
```

What each link is based on:

| Link | Finding | Level |
|---|---|---|
| TV player over HTTP | MKV over HTTP plays; the player reads with several parallel `Range` requests | measured |
| Decoders | `media_codecs.xml`: `OMX.MS.AC3.Decoder` for `audio/ac3` and `audio/ac3p`; `OMX.MS.DTS.Decoder` for `audio/dts`, `audio/dts-lbr`, `audio/dts-hd`; AAC, LPCM, FLAC decoders; `OMX.MS.Passthrough.Decoder`; **no TrueHD decoder** | static |
| Passthrough | strings "passthrough mode enabled, force use mime type", `OMX_IndexParamAudioPassthrough`, per-codec permits `GetPassthroughPermitDts/Aac/Ac3` | static; meaning of the permits unverified |
| Output format | menu "Digital Audio Output" → `setDigitalAudioOut` → `onSetSpdifMode` → `MI_AOUT_SetDigitalMode`. How values 1–5 map to DSP modes lives in a library that is not on the device | chain static, mapping unknown |
| Receiver capabilities | the TV reads the receiver's SADs over CEC including the Atmos bit (`GetCecSadInfo`, `bDolbyAtmosSupport`); string "Set SPDIF mode according to supported audio decoders" | static |
| Fallback | string "When tv power on.Arc is not connect.set it to speaker": if no ARC device answers at power-on, the TV switches to its speakers | static |

### 3.1 Getters you can read (never set them from code)

The project rule is that our code only **reads** TV settings. The output is chosen by the owner in the
TV menu.

| Getter | Values |
|---|---|
| `modeljs.sound.getSpeakerArc()` | 1 = TV Speaker, 2 = ARC, 3 = SPDIF Only, 4 = Bluetooth Speaker |
| `modeljs.sound.getDigitalAudioOut()` | 1 = Pass Through (German UI: "Bitstream"), 2 = PCM, 3 = Auto, 4 = Dolby Audio - Dolby Digital, 5 = Dolby Audio - Dolby Digital Plus |
| `modeljs.sound.getAudioFormat()` | `[codec, channel mode, "ATMOS" \| "NON_ATMOS"]` (how the launcher evaluates it; whether it is filled for media playback is unverified) |
| `modeljs.sound.getMainVolume()`, `getMainMute()` | the TV's volume store value; whether it follows the receiver under ARC is **unknown** |
| `modeljs.cec.getFunctionality()`, `getHdmiDevicesArcState()`, `modeljs.sound.getArcDevExist()` | CEC on/off and ARC device presence |

The values are taken from the launcher's own menu code and language files. A read-only snapshot on
28 Oct 2026 (receiver probably off) gave `getDigitalAudioOut() = 2` (PCM) and
`getSpeakerArc() = 1` (TV speakers): with factory settings the receiver gets stereo PCM at best.

### 3.2 Kodi's own ALSA device cannot do surround

Kodi's log on the TV shows exactly one usable ALSA device, `sysdefault:CARD=MStarMADNo00`: FL/FR
only, 8–48 kHz, `S32NE/S16NE/S16LE`, "No passthrough capabilities". `surround40/51/71` cannot be
opened. Kodi only offers passthrough on devices whose name starts with `hdmi`, `iec958` or `spdif`.
Defining an `iec958` device on `hw:MStarMADNo00,0` was considered and rejected: it is unlikely that the
MStar PCM path hands IEC 61937 frames bit-exact to ARC (the result would be noise at the receiver;
unverified, not tried).

**Surround from Kodi is therefore only possible by handing the compressed stream to the TV's media
service.** This is the reason both playback paths exist in the first place.

---

## 4. Setup by hand: TV and receiver menus

Nothing in this project changes these settings. Set them once in the menus.

### 4.1 Cabling

- TV: the port labelled **ARC** (HDMI 1 on this model, check the label on the set).
- Denon AVC-X3800H: **HDMI OUT MONITOR 1** (eARC/ARC). MONITOR 2 has no HDMI Control.
- A "High Speed with Ethernet" cable.

The project lost time once to a cable in the wrong port; if `getSpeakerArc()` never reports 2,
check the cabling first.

### 4.2 TV menus

Menu names from the launcher's English language file (German UI in brackets).

| Menu | Setting | Why |
|---|---|---|
| Settings › System › HDMI & CEC › **CEC Control** (CEC-Steuerung) | **On** | without CEC the menu shows "CEC Control: On" as a hint when ARC is chosen; volume keys travel over CEC |
| Settings › Sound › Speakers › **Audio Output** (Audio Ausgang) | **ARC** | value 2 of `getSpeakerArc()` |
| Sound › Speakers › **Digital Audio Output** (Digitaler Audioausgang) | **Pass Through** (Bitstream). Never "PCM" | "PCM" turns every format into stereo. Only selectable with Audio Output ARC or SPDIF Only |
| Sound › Speakers › **Digital Audio Delay** (Digitale Ton-Verzögerung) | 0 to start with, see section 11 | stored per source and timing |
| **Lip Sync** (Bild-Ton-Synchronisation) | irrelevant | only affects the TV speakers |
| **Device Auto Power On** (Gerät automatisch einschalten) | owner's choice | the design recommended On; the owner later turned it Off so a game console no longer starts with the TV (the receiver still does) |

What MS12 does with each codec in each "Digital Audio Output" position is **not measured**. The
following is the usual behaviour of MS12 TVs and is an assumption for this set:

| Position (value) | AC-3 | E-AC-3 / E-AC-3 JOC | DTS | AAC / multichannel LPCM |
|---|---|---|---|---|
| PCM (2) | PCM 2.0 | PCM 2.0, Atmos lost | PCM 2.0 | PCM 2.0 |
| **Pass Through (1)** | unchanged | unchanged, Atmos kept | unchanged if permitted, else PCM | PCM 2.0 or DD re-encode |
| Auto (3) | by the receiver's SADs, so like Pass Through | like Pass Through; on another Hisense model "Auto" was reported to give DD+ 5.1 instead of Atmos | like Pass Through | rather DD re-encode |
| Dolby Digital (4) | AC-3 | DD 5.1, Atmos lost | DD or PCM | DD 5.1 |
| Dolby Digital Plus (5) | AC-3 or DD+ | unchanged | unknown | DD+ or DD |

For everything this project sends on the ES path (AC-3, E-AC-3, and AC-3 from the transcoder),
"Pass Through" is the safe choice: AC-3 is the format every ARC receiver takes.

### 4.3 Receiver menus (Denon AVC-X3800H, from Denon's online manual)

| Menu | Setting |
|---|---|
| Setup › Video › HDMI Setup › **HDMI Control** | On (then switch all HDMI devices off and on once) |
| Setup › Video › HDMI Setup › **ARC** | On (required for ARC TVs; also allows the volume from the TV remote) |
| Setup › Video › HDMI Setup › **TV Audio Switching** | On |
| Setup › Video › HDMI Setup › **Power Off Control** | All (optional) |
| Setup › Inputs › Input Assign › **TV Audio** | no HDMI input assigned |
| OPTION › **Audio Delay** › Auto Lip Sync | On; "Adjust" 0–500 ms is stored per input source |
| Input source | **TV Audio** |

Notes:

- Switch the receiver on **before or together with** the TV. If the TV finds no ARC device at power-on
  it falls back to its own speakers (string quoted in section 3). Whether it later returns to ARC on its
  own ("Dev switch to ARC" also exists as a string) was not tested.
- With ARC On, Denon documents that the receiver volume can be changed with the TV remote even with
  HDMI Control off.
- What the receiver is decoding is shown on its front display (decoder and input signal indicators).
  Its on-screen "Information › Audio" page appears on the receiver's HDMI output, which the TV is not
  showing while its own media player runs (unverified, but expected).

---

## 5. Two playback paths, two sets of formats

The Kodi port plays video in one of two ways. Both are described in the TVPlayer / ES-path chapter of
this repository; here only the audio side.

### 5.1 TV-player path (file URL into the TV's media player)

Kodi serves the file over a local HTTP server; the launcher mod starts the TV's own player
(`modeljs.mpctrl.PlayMedia`) and selects the track with `setAudioTrack(n)`. The TV player shows
23.976/24p content at 59.94 Hz (that is why the ES path exists), but it is still used for
interlaced MPEG-2, 3D, content with subtitles in the same language, and anything the ES path cannot
take.

What the TV player reported for a synthetic test file with one stereo track per codec (measured,
`getMetadata([])[29]`/`[31]`):

| Track | TV reports | Result |
|---|---|---|
| AC-3 | "Dolby Digital" | plays |
| E-AC-3 | "Dolby Digital Plus" | plays |
| DTS | "DTS" | plays |
| TrueHD | "UNKNOWN" | silent |
| AAC | "AAC" | plays |
| FLAC | not accepted: the track stays on the previous one, event `[5] = 1` | refused |

Only stereo test tracks were measured. 5.1 tracks, a real DTS-HD MA track and HE-AAC were never
measured on this path (MS12 here is explicitly "no HE AAC").

Rules in the code (`TVPlayerProbe.cpp`, `IsTvAudioCodec`):

- `truehd`, `mlp`, `flac`, `opus` and `vorbis` are never sent to the TV player. Choosing one of them in
  Kodi's audio menu shows "TV kann TRUEHD nicht abspielen" (the TV cannot play TrueHD), plus a hint to
  use "Play using… → VideoPlayer" where Kodi's software player can take over (up to 1080p).
- After every track change the launcher mod reads `getMetadata([])[31]` from 1.5 s on, every 500 ms,
  for up to 5 s. If the reported codec family does not match the chosen track
  (`TvAcceptedAudio`), Kodi shows "TV: Tonformat nicht unterstützt" (format not supported) and
  actively re-sends the previous track.
- Track numbering for `setAudioTrack(n)`: the TV may count only the tracks it can decode. The default
  `<audioindex>supported</audioindex>` in `system/playercorefactory.xml` skips TrueHD/MLP/FLAC/Opus/
  Vorbis when counting; `<audioindex>file</audioindex>` counts every audio track of the file. Two
  measurements disagree: a hearing test with a Blu-ray rip (English AC-3, TrueHD, English AC-3, German
  AC-3, Spanish AC-3; `setAudioTrack(3)` played Spanish) fits "supported", while the synthetic file
  (index 3 = TrueHD "UNKNOWN") fits "file". This is still open; `kodi.log` lists
  "audio file -> TV index" for every film so a wrong mapping can be found.

### 5.2 ES path (native frame rate)

Kodi's own demuxer reads the file, and a small helper process (`flex_es_helper`) pushes the
elementary streams into the TV's media service (`MM_CreateMediaItem`, `MM_PushPayload`), the same way
the Netflix app does. This is what gives true 23.976/24 Hz output.

The media item describes its audio codec in one 32-bit field of `ST_MEDIA_INFO`:

| `+0xa0` | Meaning (from enum names in the vendor player library) | Used |
|---|---|---|
| 5 | `E_AUDIO_CODEC_AC3P`, used for **both AC-3 and E-AC-3** | yes, measured with E-AC-3 and with an unmodified AC-3 5.1 Blu-ray track |
| 6 | AAC | **no**: a comment in the code records that the TV switched off in the one run that tried it; the run is not documented further, so AAC is converted instead |
| others | unknown | – |

```cpp
// xbmc/cores/TVPlayer/TVEsEngine.cpp
uint32_t TVPLAYER::EsAudioCode(const std::string& codec)
{
  // AAC (6) needs its own test first: the TV went off in the run that tried it
  if (codec == "ac3" || codec == "eac3")
    return 5;
  return 0;
}
```

Until 17 Nov 2026 that meant: a film whose wanted language existed only as DTS (very common for
Blu-ray rips) could not use the 24p path in that language. The audio menu showed those tracks as
"(TV: nicht unterstützt)". Section 6 is the fix.

---

## 6. The AC-3 transcoder (`CTVAudioTranscoder`)

### 6.1 Why convert, and why in the demuxer thread

The problem (seen on the TV): a Blu-ray rip with German and English **DTS** tracks offered only its
Spanish, Portuguese and commentary **AC-3** tracks on the ES path. On top of that, with Kodi's
language setting "Media default" Kodi started the Spanish track (section 7).

Alternatives that were considered in the 28 Oct design, before the ES path existed:

| Idea | Why not taken |
|---|---|
| Remux the file into a continuous MPEG-TS with an AC-3 track inside Kodi's HTTP feeder for the TV player | estimated at 1.5–3 weeks; seeking would restart the stream; the TV player has no native 24p anyway |
| HLS VOD with segments from the MKV cues | only if continuous TS failed; +1–2 weeks |
| Strip the DTS core with FFmpeg's `dca_core` bitstream filter (no re-encode) | needs a DTS codec number on the ES path, which is not known |
| Add an AC-3 track to the files on the PC/NAS once (`ffmpeg … -c:a:N ac3 -b:a:N 640k`) | works for any player without code, but changes the library; still a valid manual workaround (untested in this project) |

On the ES path Kodi already owns the demuxer and hands every packet over itself. Decoding and
re-encoding there, packet by packet, needs no container work, no extra stream and no seek logic of
its own: a seek simply resets the transcoder. It is about 420 lines of plain FFmpeg code.

### 6.2 What it does

File: `xbmc/cores/TVPlayer/TVAudioTranscoder.{h,cpp}`. It uses only FFmpeg headers, no Kodi headers,
so it can be tested on its own. One demuxer thread feeds it; it is not thread safe.

```
 packet from Kodi's demuxer (any codec FFmpeg can decode)
   └─ decode (avcodec_send_packet / receive_frame)
        DTS:    option core_only=1      (skip the lossless/extension parts)
        TrueHD: option downmix=5.1      (decode only up to the 5.1 substream; also for MLP)
   └─ swresample → 48 kHz, layout 5.1 if the input has 3 or more channels, else stereo
        (7.1 is mixed down to 5.1; mono becomes 2.0)
   └─ audio FIFO → frames of exactly frame_size (1536 samples = 32 ms)
   └─ FFmpeg "ac3" encoder (floating point; falls back to any AC-3 encoder)
        5.1: 640 kbit/s (the AC-3 maximum)    2.0: 256 kbit/s
   └─ sink(data, size, ptsUs) → helper → media service, audio codec 5
```

Why these choices:

- **DTS core only / TrueHD 5.1 substream:** ARC carries neither the lossless extensions nor more than
  5.1, and the lossless parts cost most of the decoding time. If this FFmpeg build does not know the
  option, the decoder is opened again without options.
- **Floating-point `ac3` encoder rather than `ac3_fixed`:** better quality, and with NEON cheap enough
  on the TV (see the load table below).
- **640 kbit/s:** the highest AC-3 rate, the same value Kodi's own `CAEEncoderFFmpeg` uses for 5.1.
  E-AC-3 output would not help: FFmpeg's E-AC-3 encoder is limited to 5.1 and has no JOC.

Time stamps (the part that makes or breaks A/V sync):

- The first decoded frame with a time stamp sets the base of the time line (`m_baseUs`). Output
  packets are stamped `base + encoded samples / 48000`, taken from the encoder's own packet stamp,
  which already accounts for its 256-sample lead-in. The first packet can therefore be stamped slightly
  before the first input; the ES engine clamps negative stamps to 0.
- Every following input stamp is compared with the expected position
  (`base + (written + swr delay) / 48000`). A difference of more than **150 ms** (`JUMP_US`) is
  treated as a cut (seek, broken file): output is rebuilt and a new time line starts at the new stamp.
- A change of sample rate, sample format or channel layout mid-stream (a track switching between 2.0
  and 5.1) also rebuilds the output.
- Frames without a stamp before the first stamped frame are dropped (nothing to hang them on).

Errors: a broken packet increments an error counter and decoding continues with the next one. If the
decoder or encoder cannot be opened at all, the media item still expects AC-3: the picture runs
without sound and the log says why.

The core of the setup, shortened:

```cpp
constexpr int64_t JUMP_US = 150000;            // a cut in the stamps: new time line
constexpr int64_t BITRATE_SURROUND = 640000;   // AC-3 maximum for 5.1
constexpr int64_t BITRATE_STEREO = 256000;

// decoder options (retried without options if FFmpeg rejects them)
if (dec->id == AV_CODEC_ID_DTS)
  av_dict_set(&opts, "core_only", "1", 0);
else if (dec->id == AV_CODEC_ID_TRUEHD || dec->id == AV_CODEC_ID_MLP)
  av_dict_set(&opts, "downmix", "5.1", 0);

// output: 5.1 for three or more channels, else stereo, always 48 kHz
const bool surround = inLayout.nb_channels >= 3;
av_channel_layout_from_mask(&m_enc->ch_layout, surround ? AV_CH_LAYOUT_5POINT1 : AV_CH_LAYOUT_STEREO);
m_enc->sample_rate = 48000;
m_enc->bit_rate = surround ? BITRATE_SURROUND : BITRATE_STEREO;
swr_alloc_set_opts2(&m_swr, &m_enc->ch_layout, m_enc->sample_fmt, 48000,
                    &inLayout, frame->format, frame->sample_rate, 0, nullptr);
```

### 6.3 How the ES engine uses it

In `xbmc/cores/TVPlayer/TVEsEngine.cpp`:

| Function | Audio behaviour |
|---|---|
| `EsConvertsAudio(track)` | true if `EsAudioCode` is 0 and FFmpeg has a decoder for the track plus an AC-3 encoder (`CTVAudioTranscoder::CanConvert`) |
| `EsAudioOutCode(track)` | 5 for AC-3/E-AC-3, 5 for a convertible track, else 0. Written into `+0xa0` of the media item |
| `SetupAudio(index)` | creates and opens a transcoder only for tracks the TV does not take as they are, with sample rate, channels, bits, block align, bit rate and extradata from Kodi's `CDemuxStreamAudio` |
| `SendAudio(packet)` | without transcoder: the packet goes out unchanged; with transcoder: `Feed()`, each AC-3 frame is sent as an audio record |
| `BeginSeek` | `m_transcoder->Reset()` (flush decoder, drop FIFO, new time line) |
| `HandleRequests` | a **track change** may change the codec, so it creates a **new media item** at the current position: the engine restarts about 2 s before the last video time stamp |
| `Process` | audio packets before the first picture after a start or seek are not sent (the service would wait for its video) |
| `GetStatus` | a converted track has no TV verdict of its own; only AC-3/E-AC-3 tracks report their codec back |

Log lines to look for in `kodi.log`:

```
CTVPlayer::Prepare - ES path
CTVEsEngine: audio track 0 (dts) goes to the TV as Dolby Digital
CTVEsEngine: converting dca 6 ch 48000 Hz -> ac3 6 ch 640 kbit/s
CTVEsEngine: audio conversion had 0 errors, 1 cuts in the stamps
```

What the user sees:

- Kodi's audio menu marks converted tracks with " (als Dolby Digital)" ("as Dolby Digital"); tracks
  that cannot play at all carry " (TV: nicht unterstützt)".
- Kodi's codec info shows for example `TV dtshd_ma -> AC3`.

The UI strings of this build are German; they are plain string literals in `TVPlayer.cpp` and easy to
translate.

### 6.4 Test bench and measured load

Folder `kodi-port/tests/tvplayer/transcode/`:

| File | Purpose |
|---|---|
| `make_samples.sh` | creates nine 8-second test files with ffmpeg, a **different sine tone per channel** (FL 300, FR 500, FC 700, LFE 80, BL/SL 1100, BR/SR 1300 Hz, for 7.1 also SL 1700 and SR 1900 Hz; stereo FL 300/FR 500; mono 440 Hz). Freely generated, no third-party content |
| `transcode_test.cpp`, `build.sh` | test program for the TV, linked against the **same static FFmpeg 6.0.1 with NEON** as Kodi, measures CPU time |
| `check.py` | on the PC: checks the channel mapping after conversion (Goertzel per expected tone), the bit rate, the time stamps (exactly 32 ms apart, start slightly before 0, duration) and the new time line after a jump |

Two pitfalls found while writing `make_samples.sh`: without an explicit `map=` the `join` filter
assigns mono inputs by name and shifts everything; and FFmpeg's "5.1" means FL FR FC LFE BL BR, so the
tests use `5.1(side)`. `libopus` accepts 5.1 only as FL FR FC LFE BL BR, so the Opus sample renames the
surround channels to the back positions first.

```bash
# build VM: samples and the test program
FFMPEG=/path/to/ffmpeg bash kodi-port/tests/tvplayer/transcode/make_samples.sh ~/flex/transcode-samples
bash ~/flex/kodi-port/tests/tvplayer/transcode/build.sh      # -> ~/flex/transcode_test
# copy transcode_test and the samples to /tmp on the TV (tools/vmctl.py get, tools/tvctl.py put),
# run it there, copy the AC-3 output back and run check.py on the PC
```

Result on the TV, 17 Nov 2026, **all ten checks passed**:

| Input | Output | CPU load (one core of four) |
|---|---|---|
| DTS 5.1 48 kHz | AC-3 5.1 640 kbit/s | 9.5 % |
| TrueHD 5.1 | AC-3 5.1 | about 13 % |
| FLAC 7.1 96 kHz | AC-3 5.1 (mixed down) | 21.5 % |
| Opus 5.1 | AC-3 5.1 | 12.7 % |
| AAC 5.1 | AC-3 5.1 | 7.3 % |
| PCM 24 bit 7.1 | AC-3 5.1 (mixed down) | 6.1 % |
| Vorbis / MP3 stereo 44.1 kHz | AC-3 2.0 256 kbit/s | 4.4 % / 4.7 % |
| MP2 mono | AC-3 2.0 | 2.7 % |
| DTS with a jump 2 s → 6 s | new time line from 5.99 s | – |

Real file on the TV: a Blu-ray rip with 14 audio tracks (DTS-HD MA, DTS, AC-3). It took the ES path and
started with the file's default track (DTS-HD MA, converted). Switching to English DTS and to German
DTS each created a new media item at the same place, with 1.4 s in the buffer and no decoding
errors. After the last switch the helper reported "retried 6, lost 1", followed by steady playback.

### 6.5 Installing only this change

The whole change lives in the Kodi binary `kodi-directfb`. On the owner's TV the ES helper had been set
up by hand on 5 Nov 2026 (`lib/kodi/flex_es_helper` is a start script that runs the real helper with a
clean environment via `env -i`, keeping only `PATH`, `LD_LIBRARY_PATH` and `LD_PRELOAD`;
`flex_es_helper.bin` is the newer helper). A full package with `install.sh` would replace that, so
only the binary was swapped:

```bash
# build VM
bash ~/flex/kodi-port/apply-platform.sh && bash ~/flex/kodi-port/build-kodi.sh
bash ~/flex/kodi-port/package-kodi.sh        # leaves the stripped binary in ~/flex/stage-kodi/

# PC (tools from the root-mod repository)
python tools/vmctl.py get ~/flex/stage-kodi/APPS/kodi/lib/kodi/kodi-directfb build/kodi-directfb
python tools/tvctl.py put build/kodi-directfb /APPS/kodi/lib/kodi/kodi-directfb.new

# TV, with Kodi stopped
cd /APPS/kodi/lib/kodi && mv kodi-directfb kodi-directfb.pre-transcode \
  && mv kodi-directfb.new kodi-directfb && chmod 755 kodi-directfb
```

Rollback: stop Kodi, `mv kodi-directfb.pre-transcode kodi-directfb` in `/APPS/kodi/lib/kodi`.

Mind the space on `/APPS`: with less than 150 MB free Kodi does not start. Keep at most one backup
binary (`kodi-directfb.pre-*`) on the TV and the rest on the PC.

---

## 7. Track choice and path choice

### 7.1 The bug that led to the current rule

The 28 Oct design ranked the codec ("best format for ARC") **before** the file's default flag and fixed
the language first to compensate. In practice, with Kodi's language setting "Media default" and a
file whose default tracks were DTS, the ranking picked a Spanish AC-3 5.1 track. Kodi's own
`PredicateAudioFilter` evaluates the default flag before codec and channels. Since 17 Nov 2026 the
TVPlayer does the same.

### 7.2 `ChooseAudio` as built

File `xbmc/cores/TVPlayer/TVPlayerProbe.cpp`. Candidates are the tracks the current path can play
(on the ES path: every track FFmpeg can decode; on the TV-player path: everything except
TrueHD/MLP/FLAC/Opus/Vorbis).

**Step A, language** (Kodi's rule from `VideoPlayer.cpp`):

- `locale.audiolanguage` = a language, or "default" (= Kodi's UI language): keep the candidates in that
  language.
- "original": keep the candidates with `FLAG_ORIGINAL`.
- "mediadefault": no language filter; the default flag in step B decides.
- If the language filter leaves nothing (the language exists only in a format this path cannot play),
  all candidates stay.

**Step B, ranking** (the first criterion that differs decides):

| # | Criterion | Note |
|---|---|---|
| 1 | hearing-impaired flag matches `accessibility.audiohearing` | `FLAG_HEARING_IMPAIRED` |
| 2 | visually-impaired flag matches `accessibility.audiovisual` | `FLAG_VISUAL_IMPAIRED` (audio description) |
| 3 | not a commentary | `FLAG_COMMENT`, or the title contains "commentary"/"kommentar" |
| 4 | **file default flag** | only if `videoplayer.preferdefaultflag` is on (Kodi's default) |
| 5 | channel tier: 6+ before 3–5 before ≤2 | |
| 6 | plays as it is before converted | ES path only |
| 7 | codec rank: E-AC-3 with Atmos 6, E-AC-3 5, AC-3 4, DTS 3, AAC 2, MP1/MP2/MP3 1, everything else 0 | order of what ARC carries best |
| 8 | more channels | |
| 9 | higher bit rate | |
| 10 | earlier track in the file | |

Atmos detection: FFmpeg 6.0.1 (Kodi 21.3's version) has no E-AC-3 Atmos profile yet; it came with
FFmpeg 6.1. The code therefore treats an E-AC-3 track as Atmos if its title contains "atmos" or the
codec name contains "JOC" (and uses `FF_PROFILE_EAC3_DDP_ATMOS` only if a future FFmpeg defines it).
This only affects ranking; the track itself passes unchanged.

**Recommendation for users:** "Media default" follows each file's default track. If you always want
one language, set Kodi › Settings › Player › Language › Preferred audio language to it.

### 7.3 `CheckEs`: when the ES path is taken, and how audio and subtitles interact

File `xbmc/cores/TVPlayer/TVPlayer.cpp`. In order:

1. ES path switched off (`FLEX_KODI_NO_ES` or config), no probe, or the file failed on the ES path
   before in this session → TV player.
2. Video not on the ES list (H.264 `7`, HEVC `0x14`, VC-1 `12`, MPEG-2 `1`; interlaced is excluded),
   unknown frame rate, or 3D → TV player.
3. All audio tracks are re-classified for the ES path: `converted = EsConvertsAudio(t)`,
   `tvSupported = AC-3/E-AC-3 or converted`, `tvIndex` = position in the file. `ChooseAudio` runs
   again on this list. No playable track → TV player ("no audio track for this path").
4. **Subtitles:** the ES path draws no subtitles yet.
   - If subtitles are on and the TV player can play the **same language** as the ES choice, the film
     stays with the TV player (it shows subtitles).
   - If the wanted language is playable only through the transcoder (only TrueHD or FLAC, for
     example), **sound wins**: the ES path runs without subtitles and Kodi shows "Untertitel aus: diese
     Tonspur geht nur ohne Untertitel" (subtitles off: this audio track only plays without subtitles).
5. If the TV player's chosen language exists only in a format neither path can decode → TV player
   ("audio language only as <codec>").

On the TV-player path the old limits stay: TrueHD, FLAC, Opus and Vorbis cannot be picked by hand
there. The automatic choice at start moves to the ES path when that is the only way to play the
language.

---

## 8. Volume keys: Kodi counts, the launcher presses

### 8.1 Why the keys do not simply reach the TV

- Kodi reads the remote from `/dev/input/event0` with an **exclusive grab** (`EVIOCGRAB`,
  `FLEX_KODI_GRAB=1` in `kodi.sh`); otherwise the launcher underneath would react to every navigation
  key. The TV never sees Vol+/Vol−/Mute while Kodi runs.
- There is no per-key grab: `EVIOCGRAB` applies to the whole device, `EVIOCSMASK` only to the own
  reader (Linux 4.9 `evdev.c`).
- Re-injecting into evdev is not possible: the TV has no `/dev/uinput`, and writing back into `event0`
  is dropped by the kernel or, under a grab, delivered only to Kodi itself.
- Kodi's software volume has no effect on sound that the TV's media service plays.
- With ARC active, the launcher itself **never sets an absolute volume** (log string
  "SetVolume do not work in ARC mode"). Its voice control sends single key presses instead:
  `UI.send_key_to_dfb(411)` (up), `(412)` (down), `(413)` (mute), which ends in
  `modeljs.system.setKeyToDfb([key])`. Even an absolute volume request from a voice assistant becomes
  **one** step up or down under ARC.

So the design replays the keys through exactly the call the vendor software uses under ARC.

### 8.2 The bridge: `tvkeys` and `tvack`

```
 remote: KEY_VOLUMEUP 115 / KEY_VOLUMEDOWN 114 / KEY_MUTE 113
   └─ Kodi, EvdevInputHandler (exclusive grab)
        └─ counters → tvkeys   (signed file in the RAM bridge directory)
             └─ flex.js tvkTick, every 100 ms while Kodi reports "ready"
                  └─ UI.send_key_to_dfb(411 | 412 | 413), at most one per tick
                       ├─ ARC active: TV → CEC <User Control Pressed> → receiver
                       └─ TV speakers: TV volume
             ◄─ tvack {seq, arc, vol, mute}   (flex.js → Kodi)
   └─ Kodi: volume ring (section 9) or hint
```

**File format.** One line: `<64 hex digits HMAC-SHA256> <JSON>`. The HMAC covers `"tvkeys\n" + JSON`
(the file name, a newline, the JSON). The key is a random 128-bit **run key** that `kodi.sh` creates
for each Kodi start and hands to the launcher mod; Kodi reads it once (`CTVBridgeIO`). The bridge
directory is `/tmp/flexkodi-rt/b` in RAM (owner uid 10009 = the launcher, mode 0700), which the
launcher reaches through the symlink `flexkodi/rt`. The same frame is used for the other bridge files
(`play.json`, `ctl`, `player.json`).

```json
tvkeys (Kodi → flex.js):  {"v":1,"kid":"<run id>","seq":17,"up":12,"down":4,"mute":1}
tvack  (flex.js → Kodi):  {"v":1,"kid":"<run id>","seq":17,"arc":1,"vol":23,"mute":0}
```

- `kid` identifies the Kodi run (`<time hex>-<pid hex>`). A new `kid` makes flex.js only **take over**
  the counters, never execute them, so a Kodi restart cannot replay old presses.
- Counters instead of commands: a missed tick loses nothing, several presses merge.
- `arc` is `getSpeakerArc() == 2`; `vol`/`mute` come from the launcher's `VolumeBar` store after the
  keys were applied.

**Kodi side** (`xbmc/windowing/directfb/EvdevInputHandler.cpp`):

| Phase | Behaviour |
|---|---|
| Enable | only if `FLEX_KODI_TVKEYS=1`. `kodi.sh` sets it together with the bridge variables, i.e. only when the run key could be created and handed over |
| Register | writes `tvkeys` with all counters 0, repeated every 1 s, for at most **10 s**, until a `tvack` with the same `kid` arrives (flex.js gets the run key only up to 3 s after Kodi reports "ready"). Then state ON and Kodi's volume is reset to 100 % (8.3). No answer → Kodi's own volume for this run |
| Key press | `KEY_VOLUMEUP`/`DOWN`/`MUTE` while grabbed → counter +1, `seq` +1, file rewritten |
| Auto-repeat | the kernel repeats after 250 ms every 70 ms; Kodi takes a repeat only if 140 ms have passed since the last step (about 7 steps per second). Mute is never repeated (it toggles) |
| Acknowledgement | every write must be acknowledged (`tvack.seq` ≥ own `seq`) within **1 s**, polled every 100 ms in the input thread (it never blocks). No acknowledgement → tvkeys off for this run, the keys control Kodi's own volume again, Kodi shows "TV-Lautstärke nicht erreichbar" (TV volume not reachable) for 5 s |
| Display | 300 ms after the last key, the last acknowledged value is shown (section 9) |

**flex.js side** (`tvkTick` in `mods/startscreen/flex/flex.js`, launcher-mod repository):

- Runs every 100 ms, but does nothing unless Kodi is running, reports "ready" and the run key is
  known.
- Reads `tvkeys` and verifies the HMAC only when the raw text changed.
- Net steps (`up − down` since the last read) are queued, clamped to **±8**; the rest is dropped so a
  hung launcher cannot cause a volume jump after it recovers. Mute is kept as a parity bit.
- Per tick it sends **one** key: a pending mute first, else one step up or down. The launcher's voice
  control also sends exactly one key per command; whether DirectFB/CEC pass several keys sent
  back-to-back individually is not known, so the 100 ms spacing is deliberate.
- It writes `tvack` right after reading (so Kodi's 1 s timer is satisfied) and again when all queued
  steps are sent (so Kodi can show the final value). A failed `tvack` write is retried in the next
  tick.
- **Always the key path**, also for the TV speakers. The first version used the launcher's
  `VolumeBar/setVolume` store call when ARC was off. On 5 Nov 2026 that turned out to set the values
  while the TV stayed silent until a real Vol+ arrived, so since then the real key is injected in
  both cases. (The root-mod architecture chapter still describes the earlier store path for the TV
  speakers; the key path is current.)
- It never creates DOM key events; `send_key_to_dfb` goes through DirectFB like a remote key.

Timing overview:

| Stage | Rule |
|---|---|
| Kernel | auto-repeat after 250 ms, every 70 ms |
| Kodi | first press at once; repeats at most every 140 ms; mute never repeated |
| flex.js | 100 ms tick; queue ±8 steps; one key per tick |
| Acknowledgement | ≤ 1 s, else fallback to Kodi's own volume |
| Display | after 300 ms without keys |

### 8.3 Kodi's own volume pinned at 100 %

There must be exactly one volume control, the TV's or the receiver's. Kodi's software volume would
otherwise silently attenuate Kodi's own PCM output (GUI sounds, music, the VideoPlayer fallback) and
confuse the user.

- When the registration succeeds, `ResetKodiVolume()` posts `TMSG_SET_VOLUME` with 100 (percent) and
  `TMSG_SET_MUTE` with 0 to Kodi's main loop, once per run. These are the same messages Kodi's UPnP
  renderer uses. The arguments must be `int64_t`: an `int` argument selects the `PostMsg` overload with
  `param1`, and `param3` (which the handler reads) would stay unset.
- Kodi's `VolumeChanged()` calls `SetVolume()`/`SetMute()` of the running player. `CTVPlayer`
  deliberately does **not** override them (the `IPlayer` defaults are empty). Otherwise every reset to
  100 % would set the TV or receiver volume to 100.
- During TV playback Kodi's audio engine is suspended. If tvkeys is **not** active (bridge off,
  registration failed), `CTVPlayer::OnAction` swallows `ACTION_VOLUME_UP/DOWN` and `ACTION_MUTE` and
  shows once "Lautstärke bitte mit der TV- bzw. AVR-Fernbedienung" (please use the TV or receiver
  remote), so Kodi's volume does not drift.
- Not covered: a change of Kodi's volume through JSON-RPC (remote apps), the slider in Kodi's audio OSD
  or a keyboard changes Kodi's own PCM volume and does not reach the TV or receiver. Redirecting that
  would need a small core patch (`CPeripherals::OnAction`); not done.

### 8.4 Status

- TV speakers: passed on 29 Oct 2026 (Vol+/Vol− in Kodi change the TV volume).
- ARC/receiver: the volume ring of section 9, which reads the receiver after each key press, was
  confirmed by the owner on 18 Nov 2026 ("yes, with the number"), i.e. the keys reach the receiver.
- Not recorded: how many dB one injected CEC key changes on the receiver, and whether
  `setKeyToDfb` produces both `<User Control Pressed>` and `<User Control Released>`. (A missing
  release could make some receivers keep ramping; the planned probe for this started with "down" at a
  low volume for safety.)

---

## 9. The volume ring with the receiver's value

### 9.1 Why

The launcher's own volume bar is drawn below Kodi's graphics layer and is invisible while Kodi runs.
The first versions therefore showed only a toast "AVR lauter" / "AVR leiser" (receiver louder/softer)
without a number. The value in `tvack` cannot help under ARC: it is the TV's `VolumeBar` store, and
whether it follows the receiver is unknown.

### 9.2 The receiver's status page

Denon (and Marantz) receivers publish their state in the home network without login:

```
http://<avr-ip>:8080/goform/formMainZone_MainZoneXmlStatusLite.xml
```

```xml
<item>
  <Power><value>ON</value></Power>
  <InputFuncSelect><value>TV</value></InputFuncSelect>
  <VolumeDisplay><value>Absolute</value></VolumeDisplay>
  <MasterVolume><value>-32.5</value></MasterVolume>
  <Mute><value>off</value></Mute>
</item>
```

Measured on the AVC-X3800H: answer in about 8 ms. Port 80 redirects to HTTPS, which answers 403, so
port 8080 is the one to use. **Kodi only reads**; it never sends commands to the receiver. Input
switching stays with the receiver bridge (section 12).

### 9.3 How Kodi shows it

Files: `xbmc/windowing/directfb/FlexVolumeBar.{h,cpp}`, `FlexDenonStatus.cpp` (parser without Kodi
dependencies), `addons/skin.estuary/xml/DialogVolumeBar.xml`.

1. 300 ms after the last volume key, if `tvack` says `arc = 1` and a receiver URL is configured,
   `FLEXVOL::ShowReceiver()` starts a background job (high priority).
2. The job waits **150 ms**, reads the page (2 s timeout), shows the value; waits another **650 ms**,
   reads again and updates the ring if the value changed. The second read catches CEC steps that
   arrive after the last key left the TV.
3. A global generation counter makes a newer key press cancel older jobs, so a stale value is never
   shown over a newer one.
4. If the page cannot be read, the old toast ("AVR lauter/leiser/stumm") is shown instead and a
   warning is logged once.

Parsing (`FLEXVOL::ParseDenonStatus`):

| Input | Rule |
|---|---|
| `MasterVolume` | decimal dB with at most one decimal place, parsed without locale; "`--`" means the very bottom (−80 dB) |
| Range | −80.0 … +18.0 dB; outside → rejected |
| Absolute scale | `dB + 80` → 0 … 98 in half steps, exactly as the receiver's display |
| Ring | `round(absolute / 9.8)` percent, so the ring shows the share of 98 |
| Text | the absolute value with a decimal comma (e.g. "47,5"); `Mute` = on → "Stumm" (mute icon instead of the number); `VolumeDisplay` = Relative → the dB value, e.g. "-32,5 dB" |

The ring is Kodi's own `DialogVolumeBar`. The code sets two window properties on the Home window,
`flexvol.pct` (0–100) and `flexvol.text`, under the GUI lock, and then opens the bar the same way a
volume action does (`TMSG_VOLUME_SHOW` with `ACTION_VOLUME_UP`). The skin shows the property values
when `flexvol.text` is not empty and Kodi's own values otherwise:

```xml
<!-- FLEX-TV: the volume of the TV or the AV receiver (Kodi's own stays at 100 %) -->
<control type="image">
  <left>22</left><top>15</top><width>68</width><height>68</height>
  <texture>$INFO[Window(Home).Property(flexvol.pct),dialogs/volume/progress/p,.png]</texture>
  <visible>!String.IsEmpty(Window(Home).Property(flexvol.text))</visible>
</control>
<control type="label">
  <left>22</left><top>15</top><width>68</width><height>68</height>
  <font>font12</font><align>center</align><aligny>center</aligny><textcolor>white</textcolor>
  <label>$INFO[Window(Home).Property(flexvol.text)]</label>
  <visible>!String.IsEmpty(Window(Home).Property(flexvol.text)) + !String.IsEqual(Window(Home).Property(flexvol.text),Stumm)</visible>
</control>
```

The existing Estuary controls for Kodi's own ring and icon get the opposite `<visible>` condition, and
a mute icon is shown when the text is "Stumm".

With the **TV speakers** (no ARC) the ring shows the TV volume from `tvack` directly (`vol` 0–100,
or "Stumm"). If the bridge to flex.js fails during a run, `FLEXVOL::Clear()` empties the properties
and the bar shows Kodi's own values again.

### 9.4 Configuration

```sh
# on the TV: one line, not part of the repository
echo 'http://<avr-ip>:8080/goform/formMainZone_MainZoneXmlStatusLite.xml' > /APPS/kodi/avr.url
```

`kodi.sh` reads the first line (CR removed) and exports it as `FLEX_KODI_AVR_URL`. The code accepts
only `http://` URLs up to 200 characters. Without the file the old toast stays.

### 9.5 Tests and rollback

`kodi-port/tests/tvplayer/volume/run_volume_test.sh` builds the parser with the host compiler
(`-Wall -Wextra -Werror`) and runs 18 checks: half steps, whole values, bottom and "`--`", top, mute,
relative display, relative zero, blanks around the value, comma instead of point (rejected), below and
above the scale, no number, empty value, page without volume, an HTTP error page, an empty answer and
a cut-off answer.

```bash
python tools/vmctl.py run "bash ~/flex/kodi-port/tests/tvplayer/volume/run_volume_test.sh"
```

Rollback on the TV: restore `DialogVolumeBar.xml.pre-vol`, delete `/APPS/kodi/avr.url` (then the
old toast appears again), restore the binary `kodi-directfb.pre-vol`.

Other receivers: anything without this status page simply gets the toast. Marantz is named in the
code comment because it shares Denon's web interface; it was not tested.

---

## 10. Kodi's own audio output

Kodi's own sound (GUI sounds, music through PAPlayer, the VideoPlayer fallback) goes through ALSA in
stereo, reaching the receiver as PCM 2.0 over the same ARC link (assumed; a stereo upmix can be chosen
on the receiver). The TVPlayer entry in `playercorefactory.xml` is `audio="false" video="true"`, so
music never uses the TV's media player.

While the TV plays a film:

- `CTVPlayer::SuspendAE()` suspends Kodi's audio engine (`IAE::Suspend()`, waiting until
  `IsSuspended()`, the same pattern as Kodi's `ExternalPlayer`) right after the launcher accepted the
  film; `ResumeAE()` on stop, end, error and destruction. Configurable with `<suspendae>`.
  Reason: no Kodi PCM next to the bitstream. MS12 is expected to mix system sounds only into
  re-encoded outputs, so Kodi PCM during a Dolby bitstream would make the receiver switch formats
  (assumption).
- GUI sounds are switched off (`GetAudioManager().Enable(false)`) and re-disabled on every poll,
  because Kodi switches them on again when playback is paused. Kodi's "only when not playing" GUI
  sound mode would not help: it only checks whether Kodi itself has audio streams, which the TVPlayer
  does not.

Recommended Kodi audio settings (not shipped as defaults; set them in Kodi):

| Setting | Recommendation | Why |
|---|---|---|
| Audio output device | Default (`ALSA:sysdefault:CARD=MStarMADNo00`) | the only device |
| Number of channels | 2.0 | the device has two |
| Output configuration | Fixed, 48000 Hz | no rate changes, no re-locking at the receiver (assumption) |
| Passthrough | off | the device has none |
| Stereo upmix | off | let the receiver upmix |
| Keep audio device alive | Off | no permanent PCM that holds the receiver on PCM; Kodi's log showed an underrun (`snd_pcm_writei(-32)`) about every 10 s when idle |
| Play GUI sounds | Never | see above |
| Volume | 100 %, not muted | done automatically (8.3) |

Open issue: Kodi's ALSA output logs `snd_pcm_writei(-32) Broken pipe` from start, and on quit
`Stopping all` waits for it; quitting Kodi takes several seconds. The launcher mod shows a "Kodi is
closing" hint meanwhile. Not fixed yet.

---

## 11. Lip sync

- Kodi's audio offset (video settings) has **no effect** on either path: the TV outputs the sound.
- Two places can delay the sound: the TV's **Digital Audio Delay** (range 0–25, unit unknown, stored
  per source and timing; the string table shows this) and the receiver's **OPTION › Audio Delay**
  (0–500 ms, stored per input source, so for all of "TV Audio"). Both can only delay sound, not
  advance it. If the sound comes late, the only lever is less picture processing on the TV
  (assumption).
- The TV menu cannot be opened while Kodi or a probe holds the remote. The plan was to set the delay
  while a sync clip plays in the TV's own USB media player (same media source type) and verify through
  Kodi afterwards.
- Not measured yet: the actual offset on either path, whether the TV's delay also applies to a
  bitstream (not just PCM), whether the receiver's Auto Lip Sync works with this TV.

A sync clip (white flash and 1 kHz click once per second, 5.1 AC-3) can be generated with standard
ffmpeg. This is the clip proposed in the design; the measurement itself has not been done:

```bash
ffmpeg -y -f lavfi -i "color=c=black:s=1280x720:r=24:d=60,drawbox=enable='lt(mod(t\,1)\,0.04)':x=0:y=0:w=iw:h=ih:color=white:t=fill" \
  -f lavfi -i "aevalsrc=0.3*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.02):s=48000:c=5.1:d=60" \
  -c:v libx264 -c:a ac3 -b:a 640k sync24.mkv
```

Make one per frame rate you use (24, 25, 50), because the TV stores the delay per timing.

---

## 12. Interplay with the receiver bridge

The separate Denon integration (its own repository) runs a small native bridge on the TV that talks
to the receiver over its Telnet control port (23) and adds receiver tiles to the launcher. Audio
aspects that matter for Kodi:

- The receiver does **not** follow the TV back to "TV Audio" on its own after it was switched to
  another HDMI input (a game console, for example). The launcher mod therefore switches the receiver
  to TV Audio (`SITV`) when Kodi or an app tile is started, waits for the confirmation (plus 2.5 s),
  shows "Kodi startet gleich …" (Kodi starts shortly) meanwhile, and gives up after 25 s.
- A console with One-Touch Play sends `Image View On` + `Active Source` over CEC; the launcher then
  jumps to HDMI1 and Kodi is ended mid-film. Turn One-Touch Play off on consoles.
- Kodi reads the receiver's status page only; it never writes to the receiver.

---

## 13. Known limits and open points

| # | Limit | Detail |
|---|---|---|
| 1 | No lossless audio, ever | ARC: no TrueHD, no DTS-HD MA lossless, no multichannel PCM. Converted tracks are always AC-3, at most 5.1, 640 kbit/s; 7.1 is mixed down |
| 2 | DTS is re-encoded on the ES path | no DTS codec number for the ES media item is known, so the DTS core is decoded and re-encoded to AC-3 (one more lossy generation). On the TV-player path DTS goes to the TV's DTS decoder; whether it then leaves as DTS bitstream or as PCM was not measured |
| 3 | AAC is converted on the ES path | code 6 is untested (the one attempt ended with the TV switching off) |
| 4 | Atmos | only E-AC-3 JOC passes; detection is by title/codec name; whether the receiver shows Atmos through this TV was not tested |
| 5 | Format per TV menu position not measured | what reaches the receiver for "Pass Through" vs "Auto" vs "PCM", per codec, is an assumption (section 4.2) |
| 6 | TV-player path keeps its formats | TrueHD, FLAC, Opus, Vorbis cannot be chosen by hand there; 5.1 tracks, real DTS-HD MA and HE-AAC never measured on it |
| 7 | Files with no TV-player-capable track (from code reading, untested) | `TvCanPlay` runs before the path choice: a file of 1080p or less whose only tracks are TrueHD/MLP/FLAC/Opus/Vorbis goes to Kodi's VideoPlayer (stereo, software decode) without trying the ES path; for 4K such a file shows the older notice "Nur TRUEHD-Ton: der TV spielt das Bild ohne Ton" (picture without sound) even if the ES path then converts the track |
| 8 | Track change on the ES path | creates a new media item: a short interruption, playback resumes about 2 s before the previous position |
| 9 | No subtitles on the ES path | a language playable only through the transcoder turns subtitles off (with a hint) |
| 10 | Kodi volume via JSON-RPC / OSD slider | changes Kodi's own PCM volume, not the TV or receiver |
| 11 | `tvkeys` ignores a "bridge off" mid-run | one press is lost, then Kodi's own volume applies |
| 12 | Receiver step size per CEC key, CEC key release | not recorded |
| 13 | Receiver ring needs Denon/Marantz | other receivers get the plain toast |
| 14 | Receiver must be on with the TV | otherwise the TV falls back to its speakers at power-on |
| 15 | Lip sync | Kodi's offset has no effect; delays can only be added; nothing measured |
| 16 | Slow Kodi quit | ALSA "Broken pipe" from start, `Stopping all` waits; open |
| 17 | German UI strings | track labels, hints and the ring's number format are German string literals |
| 18 | Track numbering on the TV-player path | `audioindex` "supported" vs "file" still undecided (5.1) |

---
