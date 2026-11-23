# Build Kodi 21.3 and its native helpers

## 1. Prepare the build host

Use a Linux x86-64 machine and a new, owned build workspace. The reference build host
is documented in the [build record](../BUILD-RECORD.md). On Debian/Ubuntu the recipes
use the following packages:

```sh
sudo apt-get update
sudo apt-get install \
  build-essential git curl ca-certificates python3 xz-utils bzip2 \
  pkgconf cmake ninja-build meson binutils-arm-linux-gnueabi patchelf \
  autoconf automake libtool libtool-bin autopoint flex bison libparse-yapp-perl \
  libegl-dev libgles-dev libpng-dev libjpeg-dev libgif-dev liblzo2-dev zlib1g-dev
```

The compiler bootstrap uses Zig 0.16.0. The chip uses ARM EABI5 with the soft-float
calling convention, an EGLIBC 2.18 userland and `/lib/ld-linux.so.3`. Kodi links a
newer C++ runtime and uses the project linker layout to accommodate the older loader.
The vendor DirectFB and Mali runtime is already on the reference TV.

## 2. Prepare the source tree and firmware link inputs

From this repository's root:

```sh
FLEX_REPO_ROOT="$(pwd)"
export FLEX_BUILD_ROOT="$(mktemp -d /var/tmp/flex-kodi.XXXXXX)"
mkdir -p "$FLEX_BUILD_ROOT/native" "$FLEX_BUILD_ROOT/sysroot-tv/lib"
cp -a source/kodi-port "$FLEX_BUILD_ROOT/"
cp source/native/* "$FLEX_BUILD_ROOT/native/"
bash tools/vm_setup.sh
test "$(git -C "$FLEX_BUILD_ROOT/src/kodi" rev-parse HEAD)" = \
  a3a448d26b8d560a65655dab2cd122994dc4e146
bash tools/vm_fetch_directfb.sh
bash tools/vm_dfb_headers.sh
```

The source revision is Kodi `21.3-Omega`. The public DirectFB 1.4.2 source provides
headers. The ABI-specific shared libraries come from your own matching TV.

| Reference file under `/tvservice/mslib/` | Name under `$FLEX_BUILD_ROOT/sysroot-tv/lib/` |
| --- | --- |
| `libdirectfb-1.4.so.0.2.0` | `libdirectfb.so` |
| `libdirect-1.4.so.0.2.0` | `libdirect.so` |
| `libfusion-1.4.so.0.2.0` | `libfusion.so` |
| `libMali.so` | `libMali.so` |

Set `TV_SSH_TARGET` to your existing authorized developer connection with a verified
host key. On the reference firmware, the standard-output copy works over SSH without a
pseudo-terminal:

```sh
while read -r firmware_file local_file; do
  target="$FLEX_BUILD_ROOT/sysroot-tv/lib/$local_file"
  ssh -T "$TV_SSH_TARGET" "cat /tvservice/mslib/$firmware_file" > "$target" || exit
  test -s "$target" || exit
  arm-linux-gnueabi-readelf -h "$target" || exit
done <<'EOF'
libdirectfb-1.4.so.0.2.0 libdirectfb.so
libdirect-1.4.so.0.2.0 libdirect.so
libfusion-1.4.so.0.2.0 libfusion.so
libMali.so libMali.so
EOF
```

Check these are ARM ELF32 files. Keep the firmware libraries in the private build
sysroot. The final runtime uses the firmware's existing SONAMEs. You can seed
`$FLEX_BUILD_ROOT/dl` with official dependency archives. Check any cached inputs against
[BUILD-INPUTS.sha256](../BUILD-INPUTS.sha256). The recipe URLs and upstream `tools/depends`
name the corresponding sources. Once downloads or cache files exist:

```sh
(cd "$FLEX_BUILD_ROOT/dl" && sha256sum -c --ignore-missing "$FLEX_REPO_ROOT/BUILD-INPUTS.sha256")
```

## 3. Build and package

```sh
cd "$FLEX_BUILD_ROOT/kodi-port"
bash setup-toolchain.sh
bash native-tools.sh
bash build-deps.sh
bash apply-platform.sh
bash configure-kodi.sh
bash build-kodi.sh
```

`build-deps.sh` builds the dependency recipes in numeric order with a log per recipe.
`apply-platform.sh` copies the authoritative overlay onto the exact upstream checkout.
`configure-kodi.sh` selects DirectFB, GLES 2 and Ninja. Keep a new workspace for applying
the overlay, so deleted or older sources cannot survive as stale build inputs.

For the optional German UI, obtain the [official Omega German ZIP](https://mirrors.kodi.tv/addons/omega/resource.language.de_de/resource.language.de_de-11.0.113.zip) and set
`FLEX_KODI_GERMAN_PACK` to its absolute local path before packaging. The package script
verifies the `11.0.113` ZIP against the recorded SHA-256. The [localization guide](05-features.md) explains how to select German
and where Kodi stores user settings.

```sh
bash package-kodi.sh
arm-linux-gnueabi-readelf -h -l -d -V "$FLEX_BUILD_ROOT/stage-kodi/APPS/kodi/lib/kodi/kodi-directfb"
tar -tzf "$FLEX_BUILD_ROOT/kodi-tv.tar.gz" | head
sha256sum "$FLEX_BUILD_ROOT/kodi-tv.tar.gz"
```

Expected output is `$FLEX_BUILD_ROOT/kodi-tv.tar.gz`. It contains `kodi/` with the
application, assets, scripts and native helpers. The ES wrapper `flex_es_helper` starts
`flex_es_helper.bin` with the vendor media environment. The older C library needs
the linker layout in `glibc218-relocations.ld`. The main `kodi.sh` runtime script
sets the OpenSSL ARM counter workaround, ALSA configuration and HTTPS trust bundle.

Check the ARM interpreter, dynamic library names, GLIBC imports up to 2.18 and the
absence of absolute host dependencies. Inspect the archive contents, including ELF
strings, for personal paths and device data. Save the build log, input digest list and
output checksum. The [build record](../BUILD-RECORD.md) describes the existing
device package and the checked helper binaries available with this source.

## 4. Install and activate the hardware player

Stop Kodi and the active native player. Back up `/APPS/kodi` and Kodi's user-state
directory separately. Check that the TV has room for the new runtime and a rollback.
Use your locally built package and its verified checksum.

```sh
package="$FLEX_BUILD_ROOT/kodi-tv.tar.gz"
package_md5="$(md5sum "$package" | awk '{print $1}')"
ssh -T "$TV_SSH_TARGET" 'cat > /tmp/kodi-tv.tar.gz' < "$package"
ssh -T "$TV_SSH_TARGET" 'cat > /tmp/install-kodi.sh' < "$FLEX_REPO_ROOT/source/kodi-port/tv/install.sh"
ssh -T "$TV_SSH_TARGET" "sh /tmp/install-kodi.sh /tmp/kodi-tv.tar.gz $package_md5"
```

The TV installation helper uses MD5 for the transfer check and keeps a previous runtime
when space allows. The host SHA-256 is the artifact integrity record. During rollback,
stop the app and use the previously verified runtime with its expected permissions.
[Update and rollback guide](06-update-modify-and-troubleshoot.md)

Confirm `/APPS/kodi/kodi.sh status` and run a direct start/stop smoke test. Use your
authorized service manager to run `kodi-watch.sh` for the launcher handoff. Merge the
[sample player rule](../source/kodi-port/tv/testconfig/playercorefactory.xml) into the
existing Kodi profile after saving a backup. It selects the TVPlayer core for the listed
video files. The Kodi log identifies the chosen DMP, ES or software route.
[Player selection and frame-rate verification](03-frame-rate-and-video.md)

Check Home, Kodi hide/restore, the DMP and ES clear samples, HDMI handoff and ARC on
the reference device. Use [audio](04-audio.md) and [features](05-features.md) for the
remaining controls.
