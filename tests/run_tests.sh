#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/tests/build"
mkdir -p "$BUILD_DIR"

c++ -std=c++17 -O2 -pthread \
  -I"$ROOT_DIR/src" \
  "$ROOT_DIR/src/app_config.cpp" \
  "$ROOT_DIR/src/capture_core.cpp" \
  "$ROOT_DIR/tests/test_capture_core.cpp" \
  -o "$BUILD_DIR/test_capture_core"

"$BUILD_DIR/test_capture_core"
