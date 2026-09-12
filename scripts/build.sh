#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
sh plugin/build.sh
mkdir -p build/bin
for name in a8dj-midid a8dj-audio; do
    case "$name" in
        a8dj-midid) frameworks=(-framework CoreMIDI -framework CoreFoundation) ;;
        a8dj-audio) frameworks=(-framework CoreAudio -framework CoreFoundation) ;;
        a8dj) frameworks=(-framework IOKit -framework CoreFoundation) ;;
    esac
    clang -O2 -Wall -Wextra -Wno-deprecated-declarations \
        -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 -Iplugin \
        -o "build/bin/$name" "tools/$name.c" "${frameworks[@]}"
    codesign --force --sign - --timestamp=none "build/bin/$name"
done
