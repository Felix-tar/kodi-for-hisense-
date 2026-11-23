# ARM helper binaries

These helpers were built from this source tree with Zig 0.16.0 and reproduced byte
for byte. They are little-endian ARMv7 with soft-float argument passing. The ELF
loader is `/lib/ld-linux.so.3`. The highest required glibc version is 2.17.

```sh
sha256sum -c SHA256SUMS
```

| File | Source and role |
| --- | --- |
| `flex_es_helper.bin` | [Native ES session helper](../source/native/flex_es_helper.c). The normal [wrapper](../source/kodi-port/tv/flex_es_helper) supplies the process environment. |
| `flex_app_close` | [App lifecycle helper](../source/native/flex_app_close.c) used by the reference launcher handoff. |

The [full build](../docs/01-build.md) produces the Kodi executable, wrappers,
data files and matching helpers as one installer for the reference TV ABI.
