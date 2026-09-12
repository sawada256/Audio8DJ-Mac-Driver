#!/bin/sh
# Run without touching installed drivers, real MIDI FIFOs, or tracked binaries.
set -eu
cd "$(dirname "$0")"
mode=${1:-address}
case "$mode" in
    address) sanitizer=address,undefined,float-cast-overflow ;;
    thread) sanitizer=thread ;;
    *) echo "usage: $0 [address|thread]" >&2; exit 2 ;;
esac
scratch=$(mktemp -d "${TMPDIR:-/tmp}/a8dj-sanitize.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for test in midi_test audio_ring_test midi_transport_test lifecycle_test usb_lifecycle_test; do
    set --
    case "$test" in
        lifecycle_test) set -- -framework CoreAudio -framework CoreFoundation ;;
        usb_lifecycle_test) set -- ../plugin/midi.c -framework CoreAudio -framework CoreFoundation -framework IOKit ;;
    esac
    clang -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
        "-fsanitize=$sanitizer" -I../plugin "$test.c" "$@" -o "$scratch/$test"
    "$scratch/$test"
done
