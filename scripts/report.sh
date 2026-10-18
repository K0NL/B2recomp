#!/bin/sh
# [Recomps#1] Makes b2recomp-report.tar.gz in the B2recomp folder: what someone helping you needs to see - this PC
# (Linux version, desktop, CPU, graphics driver when vulkaninfo is there), the build logs and the game's log with its
# freeze reports. Nothing of the game: no dump, no targets/, no translated code, no game files.
#   sh scripts/report.sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
{
  echo "B2recomp $(cat VERSION 2>/dev/null)"
  grep PRETTY_NAME /etc/os-release 2>/dev/null
  uname -r
  ldd --version 2>/dev/null | head -1
  echo "session: $XDG_SESSION_TYPE $XDG_CURRENT_DESKTOP"
  grep -m1 "model name" /proc/cpuinfo
  echo "threads: $(nproc 2>/dev/null)"
  if command -v vulkaninfo > /dev/null 2>&1; then
    vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName|driverInfo|apiVersion"
  fi
} > tester-info.txt 2>&1
files=tester-info.txt
for f in build/logs build/setup_state.json build-linux/play/stdout.txt build-linux/boot_trace.txt \
         build-linux/*_freeze.txt build-linux/*_flash.txt build-linux/*_stalls.txt; do
  if [ -e "$f" ]; then files="$files $f"; fi
done
if tar czf b2recomp-report.tar.gz $files; then
  echo "Made $ROOT/b2recomp-report.tar.gz"
  echo "Send that file to whoever helps you. It holds logs and this PC's details, nothing of the game."
else
  echo "Could not make the report (tar failed)."
fi
