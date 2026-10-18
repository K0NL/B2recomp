#!/bin/sh
# [Recomps#1] Play the Linux version from a terminal (scripts/play.ps1 on Windows; the launcher's Play does the same):
# build-linux/game_native with window and sound, saves in build-linux/play/userdata, log build-linux/play/stdout.txt.
# Extra arguments go to the game. For Steam: add this file as a non-Steam game.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build-linux"
[ -x "$OUT/game_native" ] || { echo "Not built yet: ./B2recomp (Build), or deps/python/bin/python3 scripts/setup.py --dump <folder>"; exit 1; }
for f in "$ROOT/config/defaults/"*.ini; do          # the package's default settings, when config/ has none yet
  if [ -f "$f" ] && [ ! -f "$ROOT/config/$(basename "$f")" ]; then cp "$f" "$ROOT/config/"; fi
done
mkdir -p "$OUT/play/userdata"
if [ ! -d "$OUT/play/userdata/save" ] && [ -d "$ROOT/build/saves/after-prologue/save" ]; then
  cp -r "$ROOT/build/saves/after-prologue/." "$OUT/play/userdata/"
fi
unset SSBU_SEQ SSBU_SCRIPT SSBU_PRESS SSBU_SHOT SSBU_SHOT_EVERY SSBU_SHOT_DIR SSBU_PROF15 SSBU_HITCH SSBU_TRACE SSBU_PAD_LAYOUT
export SSBU_ROMFS="$ROOT/targets/romfs" SSBU_USERDATA="$OUT/play/userdata" SSBU_SHOW=1 SSBU_VOLUME=100 SSBU_FOCUS_NOTIFY=0
export SSBU_GRAPHICS_CONFIG="$ROOT/config/graphics.ini" SSBU_CONFIG="$ROOT/config/controllers.ini"
[ -d "$ROOT/targets/sysdata/fonts" ] && export SSBU_FONTS="$ROOT/targets/sysdata/fonts"
cd "$OUT" || exit 1
./game_native "$ROOT/targets/exefs/main.elf" "$@" 2>&1 | tee "$OUT/play/stdout.txt"
