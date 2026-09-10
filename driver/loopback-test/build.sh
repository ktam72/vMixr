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
clang -O2 -Wall -o volume_test volume_test.c \
  -framework CoreFoundation \
  -framework CoreAudio \
  -mmacosx-version-min=14.0
echo "built: $(pwd)/volume_test"
