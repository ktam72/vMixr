#!/bin/zsh
# Build the Mixr loopback verification tool and device lister.
set -e
cd "$(dirname "$0")"
clang -O2 -Wall -o loopback_test loopback_test.c \
  -framework CoreFoundation \
  -framework CoreAudio \
  -mmacosx-version-min=14.0
echo "built: $(pwd)/loopback_test"
clang -O2 -Wall -o list_devices list_devices.c \
  -framework CoreFoundation \
  -framework CoreAudio \
  -mmacosx-version-min=14.0
echo "built: $(pwd)/list_devices"
# Glitch (seam) measurement: separate writer/reader clients, ramp or sine probe.
clang -O2 -Wall -o glitch_test glitch_test.c \
  -framework CoreFoundation \
  -framework CoreAudio \
  -mmacosx-version-min=14.0
echo "built: $(pwd)/glitch_test"
clang -O2 -Wall -o incap incap.c \
  -framework CoreFoundation \
  -framework CoreAudio \
  -framework AudioToolbox \
  -mmacosx-version-min=14.0
echo "built: $(pwd)/incap"
