#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/tests/build"
ARTIFACT_LOG_DIR="$ROOT_DIR/tests/artifacts/logs"
mkdir -p "$BUILD_DIR"
mkdir -p "$ARTIFACT_LOG_DIR"

find "$BUILD_DIR" -maxdepth 1 -type f -delete
find "$ARTIFACT_LOG_DIR" -maxdepth 1 -type f -name '*.log' -delete

export TEST_REPO_ROOT="$ROOT_DIR"
export TEST_ARTIFACTS_DIR="$ARTIFACT_LOG_DIR"

COMMON_SOURCES=(
  "$ROOT_DIR/src/app_config.cpp"
  "$ROOT_DIR/src/capture_core.cpp"
  "$ROOT_DIR/src/runtime_lifecycle.cpp"
  "$ROOT_DIR/src/workflow_ui_model.cpp"
  "$ROOT_DIR/tests/test_support.cpp"
)

SUITES=(
  test_app_config
  test_capture_initialize
  test_capture_workflow
  test_capture_logging
  test_runtime_lifecycle
  test_test_support
  test_workflow_ui_model
)

for suite in "${SUITES[@]}"; do
  c++ -std=c++17 -O2 -pthread \
    -I"$ROOT_DIR/src" \
    -I"$ROOT_DIR/tests" \
    "${COMMON_SOURCES[@]}" \
    "$ROOT_DIR/tests/${suite}.cpp" \
    -o "$BUILD_DIR/$suite"

  "$BUILD_DIR/$suite"
done

if find "$ROOT_DIR" -maxdepth 1 -type f -name '\\tmp\\*.log' | grep -q .; then
  echo "[FAIL] legacy root log artifacts were recreated"
  exit 1
fi

echo "[PASS] All tests passed"
