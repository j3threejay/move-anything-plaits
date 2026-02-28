#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODULE_ID="plaits"

# Auto-detect cross prefix
if [ -z "$CROSS_PREFIX" ]; then
    CROSS_PREFIX="aarch64-linux-gnu-"
fi

# Use Docker if cross-compiler not available
if [ -z "$IN_DOCKER" ] && ! command -v ${CROSS_PREFIX}g++ &>/dev/null; then
    if command -v docker &>/dev/null; then
        echo "Cross-compiler not found, using Docker..."
        docker build -t move-anything-builder -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        docker run --rm -v "$REPO_ROOT:/build" -w /build \
            -e IN_DOCKER=1 \
            move-anything-builder ./scripts/build.sh
        exit $?
    else
        echo "Cross-compiler and Docker not found; falling back to native compiler for compile check..."
        CROSS_PREFIX=""
    fi
fi

CXX="${CROSS_PREFIX}g++"
CXXFLAGS="-g -O3 -fPIC -std=c++14 -DTEST"
INCLUDES="-Isrc -Isrc/dsp -Isrc/dsp/plaits -Isrc/dsp/stmlib"

BUILD_DIR="$REPO_ROOT/build"
DIST_DIR="$REPO_ROOT/dist/$MODULE_ID"

rm -rf "$BUILD_DIR" "$REPO_ROOT/dist"
mkdir -p "$BUILD_DIR"
mkdir -p "$DIST_DIR"

echo "==> Compiling Plaits DSP sources..."

SOURCES=(
    # Chord support
    src/dsp/plaits/dsp/chords/chord_bank.cc
    # Engine (original 16)
    src/dsp/plaits/dsp/engine/additive_engine.cc
    src/dsp/plaits/dsp/engine/bass_drum_engine.cc
    src/dsp/plaits/dsp/engine/chord_engine.cc
    src/dsp/plaits/dsp/engine/fm_engine.cc
    src/dsp/plaits/dsp/engine/grain_engine.cc
    src/dsp/plaits/dsp/engine/hi_hat_engine.cc
    src/dsp/plaits/dsp/engine/modal_engine.cc
    src/dsp/plaits/dsp/engine/noise_engine.cc
    src/dsp/plaits/dsp/engine/particle_engine.cc
    src/dsp/plaits/dsp/engine/snare_drum_engine.cc
    src/dsp/plaits/dsp/engine/speech_engine.cc
    src/dsp/plaits/dsp/engine/string_engine.cc
    src/dsp/plaits/dsp/engine/swarm_engine.cc
    src/dsp/plaits/dsp/engine/virtual_analog_engine.cc
    src/dsp/plaits/dsp/engine/waveshaping_engine.cc
    src/dsp/plaits/dsp/engine/wavetable_engine.cc
    # Engine2 (8 newer engines)
    src/dsp/plaits/dsp/engine2/chiptune_engine.cc
    src/dsp/plaits/dsp/engine2/phase_distortion_engine.cc
    src/dsp/plaits/dsp/engine2/six_op_engine.cc
    src/dsp/plaits/dsp/engine2/string_machine_engine.cc
    src/dsp/plaits/dsp/engine2/virtual_analog_vcf_engine.cc
    src/dsp/plaits/dsp/engine2/wave_terrain_engine.cc
    # FM support (for Six-Op engine)
    src/dsp/plaits/dsp/fm/algorithms.cc
    src/dsp/plaits/dsp/fm/dx_units.cc
    # Physical modelling
    src/dsp/plaits/dsp/physical_modelling/modal_voice.cc
    src/dsp/plaits/dsp/physical_modelling/resonator.cc
    src/dsp/plaits/dsp/physical_modelling/string.cc
    src/dsp/plaits/dsp/physical_modelling/string_voice.cc
    # Speech synthesis
    src/dsp/plaits/dsp/speech/lpc_speech_synth.cc
    src/dsp/plaits/dsp/speech/lpc_speech_synth_controller.cc
    src/dsp/plaits/dsp/speech/lpc_speech_synth_phonemes.cc
    src/dsp/plaits/dsp/speech/lpc_speech_synth_words.cc
    src/dsp/plaits/dsp/speech/naive_speech_synth.cc
    src/dsp/plaits/dsp/speech/sam_speech_synth.cc
    # Voice
    src/dsp/plaits/dsp/voice.cc
    # Lookup tables (wavetables, speech LPC, etc.)
    src/dsp/plaits/resources.cc
    # stmlib DSP utilities (NOT system/ which has STM32 hardware deps)
    src/dsp/stmlib/dsp/atan.cc
    src/dsp/stmlib/dsp/units.cc
    src/dsp/stmlib/utils/random.cc
    # Plugin wrapper
    src/dsp/plaits_plugin.cpp
)

OBJECTS=()
for SRC in "${SOURCES[@]}"; do
    # Create a unique object name from the path
    OBJ_NAME=$(echo "$SRC" | sed 's|/|_|g' | sed 's|\.cc$|.o|' | sed 's|\.cpp$|.o|')
    OBJ="$BUILD_DIR/$OBJ_NAME"
    echo "  Compiling: $SRC"
    $CXX $CXXFLAGS $INCLUDES -c "$REPO_ROOT/$SRC" -o "$OBJ"
    OBJECTS+=("$OBJ")
done

echo "==> Linking dsp.so..."
$CXX -shared "${OBJECTS[@]}" -o "$BUILD_DIR/dsp.so" -lm

echo "==> Verifying binary architecture..."
file "$BUILD_DIR/dsp.so"
# Both "aarch64" (Linux cross-compiler) and "arm64" (macOS native, same ISA) are valid
if file "$BUILD_DIR/dsp.so" | grep -qE "aarch64|arm64"; then
    echo "  Architecture: OK (ARM64/aarch64)"
else
    echo "  WARNING: Binary is not ARM64 — cross-compilation may be required for Ableton Move deployment"
fi

echo "==> Packaging..."
cp "$BUILD_DIR/dsp.so" "$DIST_DIR/dsp.so"
cp "$REPO_ROOT/src/module.json" "$DIST_DIR/module.json"

if [ -d "$REPO_ROOT/src/chain_patches" ] && [ "$(ls -A "$REPO_ROOT/src/chain_patches" 2>/dev/null)" ]; then
    mkdir -p "$DIST_DIR/chain_patches"
    for patch in "$REPO_ROOT/src/chain_patches"/*.json; do
        [ -f "$patch" ] && cp "$patch" "$DIST_DIR/chain_patches/$(basename "$patch")"
    done
fi

echo "==> Creating tarball..."
cd "$REPO_ROOT/dist"
tar -czvf "${MODULE_ID}-module.tar.gz" "$MODULE_ID/"
cd "$REPO_ROOT"

echo ""
echo "Build complete: dist/${MODULE_ID}-module.tar.gz"
