#!/bin/zsh
set -euo pipefail

# Resolve everything from this script's directory so the repo can live anywhere.
cd "$(dirname "$0")"
ROOT="$PWD"
SRC="$ROOT/vMixrDriver"
BUNDLE="$ROOT/vMixr.driver"

# Fresh bundle with the standard CFBundle layout the audio HAL requires:
#   vMixr.driver/Contents/{Info.plist, MacOS/vMixr, Resources/}
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
  -o "$BUNDLE/Contents/MacOS/vMixr" \
  "$SRC/vMixrDriver.c"

cp "$SRC/Info.plist" "$BUNDLE/Contents/Info.plist"

# Ad-hoc sign the bundle so the audio HAL accepts it (system HALs are signed).
codesign --force --sign - "$BUNDLE" 2>/dev/null || echo "(codesign skipped)"

echo "built bundle: $BUNDLE"
file "$BUNDLE/Contents/MacOS/vMixr"
echo "--- exported symbols (vMixr_Create) ---"
nm -gU "$BUNDLE/Contents/MacOS/vMixr" | grep -i mixr || echo "(no vMixr symbols found)"
echo "--- bundle layout ---"
ls -la "$BUNDLE/Contents"
