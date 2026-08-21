#!/usr/bin/env bash
# Build the C++ W backtest (w_backtest, w_search, w_tests).
#
# Usage:
#   ./build.sh              # Release (default)
#   ./build.sh -debug       # Debug
#   ./build.sh -j 8         # cap parallel jobs
#
# Env overrides: BUILD_DIR CMAKE_BUILD_TYPE WORKERS
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/cpp/build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
WORKERS="${WORKERS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)}"

usage() {
  cat <<EOF
Usage: $0 [-debug] [-j N] [-h]

  -debug, --debug    CMAKE_BUILD_TYPE=Debug (default: Release)
  -j, --jobs N       Parallel build jobs (default: $WORKERS)
  -h, --help         Show this help

Env: BUILD_DIR CMAKE_BUILD_TYPE WORKERS
Binaries: $BUILD_DIR/w_backtest  $BUILD_DIR/w_search  $BUILD_DIR/w_tests
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -debug|--debug) CMAKE_BUILD_TYPE=Debug ;;
    -j|--jobs)
      shift
      WORKERS="${1:?error: -j requires a number}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "error: unknown flag: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      echo "error: unexpected argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

echo "=== cmake ($CMAKE_BUILD_TYPE) → $BUILD_DIR ==="
cmake -S "$ROOT/cpp" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
echo "=== build -j$WORKERS ==="
cmake --build "$BUILD_DIR" -j "$WORKERS" --target w_backtest w_search w_tests
echo "Built:"
echo "  $BUILD_DIR/w_backtest"
echo "  $BUILD_DIR/w_search"
echo "  $BUILD_DIR/w_tests"
