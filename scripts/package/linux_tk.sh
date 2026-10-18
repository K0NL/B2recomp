#!/bin/bash
# [Recomps#1] Tk 9.0.4 with Xft for the Linux builder package (scripts/make_package.py --linux): the Python of the package
# (python-build-standalone) carries a Tk built without Xft, which draws every text in X11's bitmap font "fixed" (no sizes,
# no antialiasing). This builds the same Tk version with Xft, with zig for x86_64-linux-gnu.2.35 (runs on glibc 2.35 or
# newer), as a drop-in replacement of deps/python/lib/libtcl9tk9.0.so (same Tcl, same scripts in lib/tk9.0). It loads the
# system's libX11, libXft, libfontconfig and libfreetype, present on desktop Linux and SteamOS; ./B2recomp falls back to
# the bundled bitmap-font Tk (deps/python/lib/tk-bitmap/) when one is missing.
#   <out>/libtcl9tk9.0.so   <out>/license.terms (Tcl/Tk licence)
#   usage: linux_tk.sh <zig folder> <tcl9.0.4-src.tar.gz> <tk9.0.4-src.tar.gz> <out folder>
# Needs make and the headers of X11, Xft, fontconfig, freetype (Arch: pacman -S make libx11 libxft fontconfig freetype2).
set -e
ZIG=$(cd "$1" && pwd)/zig; TCLTAR=$2; TKTAR=$3; OUT=$4
W=/tmp/b2tk; rm -rf "$W"; mkdir -p "$W/bin" "$W/xinc" "$W/xlib" "$OUT"
TARGET=x86_64-linux-gnu.2.35
printf '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' "$ZIG" "$TARGET" > "$W/bin/zcc"
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$W/bin/zar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$W/bin/zranlib"
chmod +x "$W/bin/"*
# X11 headers from a private folder (an -I/usr/include would put the system's glibc headers before zig's 2.35 ones; the
# other headers come after them with -idirafter); the X libraries to link against through a private folder of links (an
# -L/usr/lib would bring the system's libc.so instead of zig's)
cp -r /usr/include/X11 "$W/xinc/"
for l in X11 Xft fontconfig freetype Xrender Xext; do ln -sf "$(readlink -f /usr/lib/lib$l.so)" "$W/xlib/lib$l.so"; done
export CC="$W/bin/zcc" AR="$W/bin/zar" RANLIB="$W/bin/zranlib" CFLAGS="-O2 -idirafter /usr/include" LDFLAGS="-L$W/xlib"

echo "== Tcl 9.0.4 (only for its headers, tclConfig.sh and the stub library Tk links)"
tar -xzf "$TCLTAR" -C "$W"
cd "$W"/tcl9.0.4/unix
./configure --enable-shared --disable-zipfs --prefix="$W/inst" > "$W/tcl-configure.log" 2>&1 || { tail -30 "$W/tcl-configure.log"; exit 1; }
STUB=$(. ./tclConfig.sh && echo "$TCL_STUB_LIB_FILE")            # only the stub library (not tclsh and its packages)
make -j"$(nproc)" "$STUB" > "$W/tcl-make.log" 2>&1 || { tail -30 "$W/tcl-make.log"; exit 1; }
echo "built $STUB"

echo "== Tk 9.0.4 with Xft"
tar -xzf "$TKTAR" -C "$W"
cd "$W"/tk9.0.4/unix
./configure --enable-shared --disable-zipfs --enable-xft --disable-xss --with-tcl="$W/tcl9.0.4/unix" \
            --x-includes="$W/xinc" --x-libraries="$W/xlib" --prefix="$W/inst" > "$W/tk-configure.log" 2>&1 \
  || { tail -30 "$W/tk-configure.log"; exit 1; }
grep -E "checking (for xft|whether to use xft|for XScreenSaver)|xft" "$W/tk-configure.log" | head -5
make -j"$(nproc)" libtcl9tk9.0.so > "$W/tk-make.log" 2>&1 || { tail -40 "$W/tk-make.log"; exit 1; }
cp libtcl9tk9.0.so "$OUT/libtcl9tk9.0.so"
strip --strip-unneeded "$OUT/libtcl9tk9.0.so"
cp ../license.terms "$OUT/license.terms"

echo "== result"
f="$OUT/libtcl9tk9.0.so"
echo "$f: $(du -h "$f" | cut -f1), soname $(objdump -p "$f" | awk '/SONAME/{print $2}'), newest glibc $(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "needs: $(objdump -p "$f" | awk '/NEEDED/{printf "%s ", $2}')"
nm -D --defined-only "$f" | grep -c " T " | sed 's/^/exported functions: /'
