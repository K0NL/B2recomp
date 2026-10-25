#!/bin/sh
# [Recomps#1] Play the macOS version from a terminal (launcher Play does the same):
# build-darwin/game_native with window and sound, saves in build-darwin/play/userdata, log build-darwin/play/stdout.txt.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build-darwin"
[ -x "$OUT/game_native" ] || { echo "Not built yet: ./B2recomp (Build), or scripts/setup.py --dump <folder>"; exit 1; }
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
export SDL_VIDEODRIVER=cocoa
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
DYLD_LIBRARY_PATH="$OUT:$DYLD_LIBRARY_PATH:/opt/homebrew/lib"
export DYLD_LIBRARY_PATH
[ -d "$ROOT/targets/sysdata/fonts" ] && export SSBU_FONTS="$ROOT/targets/sysdata/fonts"
export SDL_VIDEODRIVER=cocoa VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
cd "$OUT" || exit 1
./game_native "$ROOT/targets/exefs/main.elf" "$@" 2>&1 | tee "$OUT/play/stdout.txt"
