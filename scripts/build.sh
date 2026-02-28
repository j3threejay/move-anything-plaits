#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Auto-detect or use cross prefix
if [ -z "$CROSS_PREFIX" ]; then
    CROSS_PREFIX="aarch64-linux-gnu-"
fi

# Use Docker if cross-compiler not available; fall back to native g++ if Docker also absent
if [ -z "$IN_DOCKER" ] && ! command -v ${CROSS_PREFIX}g++ &>/dev/null; then
    if command -v docker &>/dev/null; then
        echo "Cross-compiler not found, using Docker..."
        docker build -t move-anything-builder -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        docker run --rm -v "$REPO_ROOT:/build" -w /build \
            -e IN_DOCKER=1 \
            move-anything-builder ./scripts/build.sh
        exit $?
    else
        echo "Cross-compiler and Docker not found; falling back to native g++ for compile check..."
        CROSS_PREFIX=""
    fi
fi

mkdir -p "$REPO_ROOT/build"
cd "$REPO_ROOT"

CXX="${CROSS_PREFIX}g++"
CXXFLAGS="-g -O3 -fPIC -std=c++14 -DTEST"
INCLUDES="-Isrc -Isrc/dsp -Isrc/dsp/plaits -Isrc/dsp/stmlib"

echo "Testing skeleton compile..."
$CXX $CXXFLAGS $INCLUDES -c src/dsp/plaits_plugin.cpp -o build/plaits_plugin.o
echo "Skeleton compile: OK"
