#!/bin/zsh
# Deploy the locally built vMixr.driver to the system HAL plug-in directory and
# reload coreaudiod. Run with sudo:
#   sudo zsh ~/Documents/apps/vMixr/driver/deploy.sh
set -euo pipefail

SRC="$HOME/Documents/apps/vMixr/driver/vMixr.driver"
DST="/Library/Audio/Plug-Ins/HAL/vMixr.driver"

if [ ! -d "$SRC" ]; then
  echo "missing build: $SRC (run driver/build.sh first)" >&2
  exit 1
fi

rm -rf "$DST"
cp -R "$SRC" "$DST"
chown -R root:wheel "$DST"

# Clear the driver log so the next run is readable.
: > /tmp/mixr_driver.log 2>/dev/null || true
chown _coreaudiod:wheel /tmp/mixr_driver.log 2>/dev/null || true

# Reload the HAL so the new driver is picked up.
killall coreaudiod 2>/dev/null || true

echo "deployed: $DST"
echo "coreaudiod restarted, /tmp/mixr_driver.log cleared"
