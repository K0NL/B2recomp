#!/bin/bash
# [Recomps#1] The prebuilt Linux libraries of the Linux builder package (scripts/make_package.py --linux), made on a Linux
# host (here WSL2 Arch) with zig 0.17.0 for x86_64-linux-gnu.2.35, so they run on any distribution with glibc 2.35 or newer
# (Ubuntu 22.04+, SteamOS 3, Fedora, Arch ...):
#   <out>/SDL3/lib/libSDL3.so.0       SDL 3.4.18 (zlib licence); its X11 / Wayland / PulseAudio / PipeWire / ALSA back ends
#                                      load the system's libraries at run time, so only their headers are needed here
#   <out>/libssbu_glslang.so          runtime/vk_glslang_shim.c + glslang 16.6.0 (static, ENABLE_OPT off) + LLVM libc++
#                                      (static): it needs nothing but glibc
#   usage: linux_deps.sh <zig folder> <SDL3-3.4.18.tar.gz> <glslang-16.6.0 source .tar.gz> <out folder>
# Needs cmake, ninja, pkg-config and the headers of X11, Wayland, xkbcommon, libdecor, PulseAudio, PipeWire, ALSA, udev,
# D-Bus (Arch: pacman -S cmake ninja pkgconf libx11 libxext libxrandr libxcursor libxi libxfixes libxss libxtst wayland
# wayland-protocols libxkbcommon libdecor libpulse pipewire alsa-lib systemd-libs dbus mesa libusb).
set -e
ZIG=$(cd "$1" && pwd)/zig; SDLTAR=$2; GLTAR=$3; OUT=$4
REPO=$(cd "$(dirname "$0")/../.." && pwd)
W=/tmp/b2deps; rm -rf "$W"; mkdir -p "$W/bin" "$OUT"
TARGET=x86_64-linux-gnu.2.35
# zig as the C / C++ compiler and archiver of CMake; the system's headers only AFTER zig's glibc 2.35 headers
printf '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' "$ZIG" "$TARGET" > "$W/bin/zcc"
printf '#!/bin/sh\nexec "%s" c++ -target %s "$@"\n' "$ZIG" "$TARGET" > "$W/bin/zcxx"
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$W/bin/zar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$W/bin/zranlib"
chmod +x "$W/bin/"*
CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$W/bin/zcc" -DCMAKE_CXX_COMPILER="$W/bin/zcxx"
    -DCMAKE_AR="$W/bin/zar" -DCMAKE_RANLIB="$W/bin/zranlib" -DCMAKE_C_FLAGS="-idirafter /usr/include"
    -DCMAKE_CXX_FLAGS="-idirafter /usr/include")

echo "== SDL3"
tar -xzf "$SDLTAR" -C "$W"
cmake -S "$W"/SDL3-* -B "$W/sdl-build" "${CM[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
      -DSDL_EXAMPLES=OFF -DSDL_INSTALL=ON -DCMAKE_INSTALL_PREFIX="$W/sdl-inst" > "$W/sdl-cmake.log" 2>&1 \
  || { tail -40 "$W/sdl-cmake.log"; exit 1; }
grep -E "^-- +(SDL_VIDEO_DRIVER|SDL_AUDIO_DRIVER|SDL_X11|SDL_WAYLAND|SDL_PULSE|SDL_PIPEWIRE|SDL_ALSA|SDL_HIDAPI|SDL_LIBUDEV|SDL_DBUS)" "$W/sdl-cmake.log" | head -20 || true
sed -n '/Enabled backends:/,/^$/p' "$W/sdl-cmake.log" | head -20
cmake --build "$W/sdl-build" > "$W/sdl-build.log" 2>&1 || { tail -40 "$W/sdl-build.log"; exit 1; }
cmake --install "$W/sdl-build" > /dev/null
mkdir -p "$OUT/SDL3/lib"
cp -L "$W/sdl-inst/lib/libSDL3.so.0" "$OUT/SDL3/lib/libSDL3.so.0"
ln -sf libSDL3.so.0 "$OUT/SDL3/lib/libSDL3.so"
rm -rf "$OUT/SDL3/include"; cp -r "$W/sdl-inst/include" "$OUT/SDL3/include"
cp "$W"/SDL3-*/LICENSE.txt "$OUT/SDL3/LICENSE.txt"

echo "== glslang"
mkdir -p "$W/glslang-src" && tar -xzf "$GLTAR" -C "$W/glslang-src" --strip-components=1
cmake -S "$W/glslang-src" -B "$W/gl-build" "${CM[@]}" -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
      -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_ENABLE_INSTALL=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_INSTALL_PREFIX="$W/gl-inst" > "$W/gl-cmake.log" 2>&1 || { tail -40 "$W/gl-cmake.log"; exit 1; }
cmake --build "$W/gl-build" > "$W/gl-build.log" 2>&1 || { tail -40 "$W/gl-build.log"; exit 1; }
cmake --install "$W/gl-build" > /dev/null
L="$W/gl-inst/lib"
"$W/bin/zcxx" -shared -fPIC -O2 -w -I "$W/gl-inst/include" "$REPO/runtime/vk_glslang_shim.c" -x none \
  "$L/libglslang-default-resource-limits.a" "$L/libglslang.a" "$L/libSPIRV.a" "$L/libMachineIndependent.a" \
  "$L/libGenericCodeGen.a" "$L/libOSDependent.a" -lpthread -Wl,-soname,libssbu_glslang.so \
  -o "$OUT/libssbu_glslang.so" 2>"$W/shim.log" || { cat "$W/shim.log"; ls "$L"; exit 1; }
cp "$W/glslang-src/LICENSE.txt" "$OUT/glslang-LICENSE.txt"

strip --strip-unneeded "$OUT/SDL3/lib/libSDL3.so.0" "$OUT/libssbu_glslang.so"       # no debug info in the package
echo "== result"
for f in "$OUT/SDL3/lib/libSDL3.so.0" "$OUT/libssbu_glslang.so"; do
  echo "$f: $(du -h "$f" | cut -f1), newest glibc $(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1), needs: $(objdump -p "$f" | awk '/NEEDED/{printf "%s ", $2}')"
done
