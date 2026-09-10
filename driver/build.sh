#!/bin/zsh
set -euo pipefail

# Resolve everything from this script's directory so the repo can live anywhere.
cd "$(dirname "$0")"
ROOT="$PWD"
SRC="$ROOT/MixrDriver"
BUNDLE="$ROOT/Mixr.driver"

# Fresh bundle with the standard CFBundle layout the audio HAL requires:
#   Mixr.driver/Contents/{Info.plist, MacOS/Mixr, Resources/}
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

echo "compiling..."
clang \
  -bundle \
  -mmacosx-version-min=14.0 \
  -O2 \
  -Wall \
  -fno-exceptions \
  -framework CoreFoundation \
  -framework CoreAudio \
  -o "$BUNDLE/Contents/MacOS/Mixr" \
  "$SRC/MixrDriver.c"

cp "$SRC/Info.plist" "$BUNDLE/Contents/Info.plist"

# Ad-hoc sign the bundle so the audio HAL accepts it (system HALs are signed).
codesign --force --sign - "$BUNDLE" 2>/dev/null || echo "(codesign skipped)"

echo "built bundle: $BUNDLE"
file "$BUNDLE/Contents/MacOS/Mixr"
echo "--- exported symbols (Mixr_Create) ---"
nm -gU "$BUNDLE/Contents/MacOS/Mixr" | grep -i mixr || echo "(no Mixr symbols found)"
echo "--- bundle layout ---"
ls -la "$BUNDLE/Contents"
