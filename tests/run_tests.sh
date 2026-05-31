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
    tests/test_perf.c src/dsp/perf.c \
    -lm \
    -o tests/test_perf

./tests/test_perf
