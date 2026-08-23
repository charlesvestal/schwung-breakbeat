#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

cc -Wall -Wextra -O0 -g -std=c99 \
    -Isrc/dsp \
    tests/test_slice_select.c src/dsp/slice_select.c \
    -lm \
    -o tests/test_slice_select

./tests/test_slice_select

cc -Wall -Wextra -O0 -g -std=c99 \
    -Isrc/dsp \
    tests/test_bb_timing.c src/dsp/bb_timing.c \
    -lm \
    -o tests/test_bb_timing

./tests/test_bb_timing

mkdir -p build/tests
cc -Wall -Wextra -O0 -g -std=c11 -shared -fPIC \
    -Isrc/dsp \
    src/dsp/breakbeat.c src/dsp/slice_select.c src/dsp/bb_timing.c \
    -lm -pthread \
    -o build/tests/dsp.so

cc -Wall -Wextra -O0 -g -std=c11 \
    -Isrc/dsp \
    tests/test_plugin_runtime.c \
    -pthread \
    -o build/tests/test_plugin_runtime

./build/tests/test_plugin_runtime \
    "$REPO_ROOT/build/tests/dsp.so" \
    "$REPO_ROOT/samples/amen01.wav" \
    "$REPO_ROOT/samples/sesame.wav"
