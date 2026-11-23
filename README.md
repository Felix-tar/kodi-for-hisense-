# Kodi 21.3 for Hisense VIDAA U4

Kodi 21.3 for the Hisense 55U8QF running `V0000.01.00C.P0219`. DirectFB and Mali GLES 2
render the interface. The TVPlayer core uses the firmware's file player or streams clear
codec packets to a native ES helper, with software playback as a compatible fallback.

## Flex TV projects

- [Hisense VIDAA mod](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod)
- [Kodi for Hisense VIDAA](https://github.com/Felix-tar/kodi-for-hisense-)
- [Denon AVR integration](https://github.com/Felix-tar/denon-avr-integration-on-smarttv-)

## Decoder, player profiles and frame rate

- [Hardware decoder and ES helper architecture](docs/03-frame-rate-and-video.md#6-the-es-path-inside-kodi)
- [Native Netflix comparison and player profiles](docs/03-frame-rate-and-video.md#51-native-app-comparison-and-player-profiles)
- [Cadence measurements and how to interpret the IRQs](docs/03-frame-rate-and-video.md#3-measuring-the-real-output-rate)
- [Path selection and reference Kodi profile rule](docs/03-frame-rate-and-video.md#7-path-choice-es-path-tv-player-or-kodis-videoplayer)

The Hisense project documents the full silicon, player-profile and measurement investigation.

- [Player profiles and cadence](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/07-player-profiles-and-cadence.md)
- [Hardware decoder lab](https://github.com/Felix-tar/hisense-vidda-root-ssh-mod/blob/main/docs/02-hardware-decoder.md)

## Source and build

1. [Follow the complete cross-build and install guide](docs/01-build.md). It lists the
   Linux host packages, exact Kodi revision and four firmware libraries to obtain
   privately from your matching development TV. It also documents rollback,
   language-pack preparation and activation of `TVPlayer`.
2. [Check the build record](BUILD-RECORD.md) and [source archive digests](BUILD-INPUTS.sha256).
   The reproducible source entry point is the upstream Kodi tag plus our
   [overlay](source/kodi-port/kodi-tree). The [native sources](source/native) include
   the decoder helper, app handoff and old-loader linker layout.
3. [Verify the included ARM helper builds](prebuilt/README.md). The full
   `kodi-tv.tar.gz` installer is produced by the documented build. The existing
   historical Kodi executable is retained in the private project backup because
   it embeds personal build paths.

## Configure and troubleshoot

- [Port architecture](docs/02-architecture.md) and [audio/ARC](docs/04-audio.md)
- [Localization, phone keyboard, search, artwork and Home](docs/05-features.md)
- [Updates, tests and troubleshooting](docs/06-update-modify-and-troubleshoot.md)
- [Upstream Kodi license notices](LICENSE.md) and [license texts](LICENSES)
