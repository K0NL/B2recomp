#!/bin/sh
# [Recomps#1] B2recomp (Linux builder package: copied to its top as ./B2recomp by scripts/make_package.py --linux).
# The launcher (launcher/launcher_linux.py) with the package's own Python (deps/python, Tk included): the first start builds
# the game from your own extracted dump, then it shows the settings and Play. --build shows the build screen again.
HERE=$(cd "$(dirname "$0")" && pwd)
# a zip unpacked without its Unix permissions (some archive tools): make the bundled programs runnable again
for f in "$HERE/B2recomp" "$HERE/deps/zig/zig" "$HERE/deps/python/bin/"* "$HERE/scripts/play.sh" "$HERE/scripts/report.sh"; do
  if [ -f "$f" ] && [ ! -x "$f" ]; then chmod +x "$f" 2>/dev/null; fi
done
# the default settings (config/defaults, in the package) become config/*.ini only when those are missing: unpacking a
# newer B2recomp over this folder never replaces the player's settings
for f in "$HERE/config/defaults/"*.ini; do
  if [ -f "$f" ] && [ ! -f "$HERE/config/$(basename "$f")" ]; then cp "$f" "$HERE/config/"; fi
done
# Tk with antialiased fonts (deps/python/lib/tk-xft, built with Xft by scripts/package/linux_tk.sh) needs the system's
# libXft, fontconfig and X11 libraries; on a PC without one of them the bundled bitmap-font Tk (tk-bitmap) is used
L="$HERE/deps/python/lib"
if [ -f "$L/tk-xft/libtcl9tk9.0.so" ] && [ -f "$L/tk-bitmap/libtcl9tk9.0.so" ] && command -v ldd > /dev/null 2>&1; then
  if ldd "$L/tk-xft/libtcl9tk9.0.so" 2>/dev/null | grep -q "not found"; then want=tk-bitmap; else want=tk-xft; fi
  if ! cmp -s "$L/$want/libtcl9tk9.0.so" "$L/libtcl9tk9.0.so" 2>/dev/null; then
    cp -f "$L/$want/libtcl9tk9.0.so" "$L/libtcl9tk9.0.so"
  fi
fi
PY="$HERE/deps/python/bin/python3.13"                     # the file itself (python3 is a symlink, not in the zip)
[ -x "$PY" ] || PY="$HERE/deps/python/bin/python3"
[ -x "$PY" ] || PY=$(command -v python3)
[ -n "$PY" ] || { echo "B2recomp: no Python (the package's deps/python is missing)"; exit 1; }
exec "$PY" "$HERE/launcher/launcher_linux.py" "$@"
