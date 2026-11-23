# Kodi source and build record

| Input | Reference |
| --- | --- |
| Kodi upstream | [21.3-Omega](https://github.com/xbmc/xbmc/releases/tag/21.3-Omega), commit `a3a448d26b8d560a65655dab2cd122994dc4e146` |
| Project source | [Kodi overlay](source/kodi-port/kodi-tree), [native helpers](source/native), [TV install and runtime scripts](source/kodi-port/tv) |
| Build host observed | Ubuntu 26.04 x86-64, GCC 15.2.0, CMake 4.2.3, Ninja 1.13.2, Meson 1.10.1 |
| Cross compiler | Zig 0.16.0. Target `arm-linux-gnueabi.2.18` with `softfp` |
| Link and runtime | ARM ELF32 using `/lib/ld-linux.so.3`. DirectFB/Mali from the matching firmware, libc and project helpers |
| German locale | Official Omega `resource.language.de_de` 11.0.113 |
| Source archive hashes | [BUILD-INPUTS.sha256](BUILD-INPUTS.sha256) records 26 cached source/tool archives and the optional locale from the reference setup |
| Compiled code available here | [Checked ARM helper binaries](prebuilt/README.md), reproduced byte for byte |

Use the [build guide](docs/01-build.md) to generate the complete local installer. The archived
Kodi executable from the reference TV was also present in the November 18 build archive.
It embeds personal workstation paths. The source distribution uses pinned inputs and
a neutral compiler filename prefix for a new build.

Library recipes and downloader URLs are in [the dependency scripts](source/kodi-port/deps) and
the pinned upstream `tools/depends` tree. DirectFB and ALSA archive hashes are fixed in their
recipes. The compiler bootstrap verifies Zig; the package script verifies the optional
German ZIP. The current CA bundle is obtained from curl's distribution with its published
hash. Preserve the exact downloads and host tool versions to reproduce bytes across builds.

The full installer links several third-party components statically, including FFmpeg,
Samba, OpenSSL, GnuTLS, libass and curl. The versioned build recipes and upstream sources
identify the matching source inputs. Use your own firmware as the source for the
proprietary link inputs. Kodi license information is in [LICENSE.md](LICENSE.md)
and [LICENSES](LICENSES).
