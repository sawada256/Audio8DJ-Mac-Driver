#!/bin/sh
# Build Audio8DJ.driver and ad-hoc sign it.
#
# Produces an ad-hoc signed local build. Downloaded distributions may require
# macOS approval; this script does not perform Developer ID signing/notarization.

set -eu

cd "$(dirname "$0")"

BUNDLE=../build/Audio8DJ.driver
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"

cp Info.plist "$BUNDLE/Contents/Info.plist"

clang -bundle \
      -arch arm64 -arch x86_64 \
      -O2 -Wall -Wextra \
      -mmacosx-version-min=12.0 \
      -framework CoreFoundation -framework CoreAudio -framework IOKit \
      -o "$BUNDLE/Contents/MacOS/Audio8DJ" \
      Audio8DJ.c usb_engine.c midi.c

codesign --force --sign - --timestamp=none "$BUNDLE"

echo "built: $(cd "$(dirname "$BUNDLE")" && pwd)/$(basename "$BUNDLE")"
codesign -dv "$BUNDLE" 2>&1 | sed 's/^/  /'
