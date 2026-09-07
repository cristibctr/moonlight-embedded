#!/bin/sh
# Run from any directory. TV must already be streaming this Mac.
set -eu
capture_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
work_dir=$(dirname "$capture_dir")
capture_python=${MOONLIGHT_TEST_PYTHON:-python3}
capture_ffmpeg=${MOONLIGHT_FFMPEG:-ffmpeg}
if [ "$#" -lt 1 ]; then
    printf 'Usage: sh run-test.sh unique-label [--seconds 10 --interval .25 --type 16]\n' >&2
    exit 2
fi
case "$1" in ''|*[!a-zA-Z0-9_-]*) printf 'Use a label with letters, digits, dash or underscore.\n' >&2; exit 2;; esac
capture_output="$capture_dir/runs/$1"
shift
if [ -e "$capture_output" ]; then
    printf 'This run already exists: %s\n' "$capture_output" >&2
    exit 2
fi
cd "$work_dir/.."
export ZIG_GLOBAL_CACHE_DIR="$work_dir/zig-cache"
"$work_dir/vidaa-zig-cc" -O2 -Wall -Wextra -o "$capture_dir/capture" "$capture_dir/capture.c" -ldl
swiftc -module-cache-path "$work_dir/swift-module-cache" -O "$capture_dir/TimeMarker.swift" -o "$capture_dir/time-marker"
"$capture_python" -m unittest discover -s "$capture_dir" -p 'test_*.py'
if command -v node >/dev/null 2>&1; then
    node --test "$capture_dir/test_web_marker.js"
fi
ssh -T -o BatchMode=yes -o ConnectTimeout=5 vidaa-tv 'dd of=/tmp/moonlight-capture.new bs=65536 2>/dev/null' < "$capture_dir/capture"
ssh -T -o BatchMode=yes -o ConnectTimeout=5 vidaa-tv 'dd of=/tmp/run-moonlight-capture.new bs=65536 2>/dev/null' < "$capture_dir/run-capture"
ssh -T -o BatchMode=yes -o ConnectTimeout=5 vidaa-tv 'chmod 755 /tmp/moonlight-capture.new /tmp/run-moonlight-capture.new && mv /tmp/moonlight-capture.new /tmp/moonlight-capture && mv /tmp/run-moonlight-capture.new /tmp/run-moonlight-capture'
"$capture_python" "$capture_dir/measure.py" --output "$capture_output" "$@"
if command -v "$capture_ffmpeg" >/dev/null 2>&1; then
    "$capture_ffmpeg" -hide_banner -loglevel error -f concat -safe 0 \
        -i "$capture_output/recording.ffconcat" -c:v libx264 -crf 18 \
        -pix_fmt yuv420p -movflags +faststart "$capture_output/tv-recording.mp4"
fi
printf 'Saved: %s\n' "$capture_output"
