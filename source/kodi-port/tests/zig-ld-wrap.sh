#!/bin/bash
# Kann der Zig-Linker, was Kodis dll-loader beim Linken von libdvdnav-arm.so verlangt?
# (-Wl,-wrap,<fn> und undefinierte Symbole in einer Shared Library)
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
T=$(mktemp -d)
cat > "$T/w.c" <<'EOF'
int open(const char *p, int f, ...);
int undefined_fn(void);
int __wrap_open(const char *p, int f) { return 0; }
int x(void) { return open("a", 0) + undefined_fn(); }
EOF
"$CC" -shared -fPIC -o "$T/w.so" "$T/w.c" -Wl,-wrap,open; echo "wrap rc=$?"
"$CC" -shared -fPIC -o "$T/w2.so" "$T/w.c"; echo "undefined rc=$?"
"$CC" -shared -fPIC -o "$T/w3.so" "$T/w.c" -Wl,--unresolved-symbols=ignore-all; echo "unresolved rc=$?"
"$CC" -shared -fPIC -o "$T/w4.so" "$T/w.c" -Wl,-z,undefs; echo "z undefs rc=$?"
"$NM" -D "$T/w.so" 2>/dev/null | grep -i open
rm -rf "$T"
grep -rln 'udfread' "$KODI_SRC/cmake" "$KODI_SRC/CMakeLists.txt" | head
