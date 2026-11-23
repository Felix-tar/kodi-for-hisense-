# Update, modify and troubleshoot

## Build and deploy a verified change

1. Record exact sources and hashes of the installed program, helpers and wrapper. Back up the
   installed runtime separately from Kodi settings and its library. Verify a previous boot or
   install package is present and label it by version.
2. Edit the relevant file under `source/kodi-port/kodi-tree`, `source/kodi-port/tv` or
   `source/native`. Follow [the build guide](01-build.md) to apply the overlay and rebuild for
   the ARM ABI. Reconfigure after changing CMake files or toolchain flags. Repackage helpers,
   scripts and runtime assets together after protocol changes.
3. Inspect the build and runtime outputs before installing. A build-input path in a dynamic
   dependency or a GLIBC import above 2.18 is an ABI mismatch even if the link succeeded.
4. Use only the already authorized transfer mechanism. Stop Kodi and the active native media
   player normally before replacing a package. Keep the previous validated runtime. Verify
   the newly installed package and then resume launcher watcher management.
5. First test direct Kodi start/stop, stock apps, HDMI and a simple local source. Confirm
   the log's player selection before comparing resolution, timing or audio.

## Smoke suite and diagnostics

- Kodi launch: check return code, `kodi.sh status` and the local Kodi log. On an immediate
  failure, distinguish wrong ARM ABI, renderer initialization, occupied DirectFB layer
  and runtime dependency errors.
- UI: check D-pad and back from a normal menu. Hide and restore Kodi while idle, while playing
  DMP, and while playing ES. Check the video plane and ARC state after each restore.
- Source transition: stop/close normally, wait for the complete player release, select HDMI
  and then go back to Home. Check that the source, media state and UI ownership agree.
- Audio: test AC-3, E-AC-3, AAC stereo and a file requiring conversion. On silence examine the
  route, bitstream and receiver status separately. [Audio guide](04-audio.md).
- Video: try both 23.976p and 24p media, then 50p/59.94p, and a non-16:9 video. Monitor
  timing counters, decoded state and input read rate independently. [Video guide](03-frame-rate-and-video.md).
- ES/SMB: pause your own test SMB server temporarily and measure stop latency.
  This exercises the worker/blocked-read problem tracked in the architecture review.
- Search and keyboard: test language add-on, diacritic tolerance, session expiration and
  masked input handling. Keep private Kodi settings and logs out of the repository.

Run the C++, Python and shell test entry points under `source/kodi-port/tests` using
the dependencies in their headers. Some exercise an already configured reference TV.
Treat those runs separately from offline suites and use fresh test data.

## Restore the previous runtime

End Kodi normally and confirm the watcher has stopped handling new launches. Recover the
previous verified program tree with its original permissions from the device-local or offline
backup. Preserve separately backed up user state. Match the launcher bridge protocol to the
Kodi build that you restored. Then test a local file, application start, HDMI and general
input after resuming normal service operation.
