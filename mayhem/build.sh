#!/usr/bin/env bash
#
# libriscv/mayhem/build.sh — build libriscv's fuzz harness as sanitized libFuzzer targets
# plus standalone (non-fuzzer) reproducers.
#
# The harness source is mayhem/fuzz.cpp (an additive overlay of the upstream fuzz/fuzz.cpp)
# which fixes LLVMFuzzerTestOneInput to return int instead of void. The upstream file is left
# pristine; this overlay is compiled in its place.
#
# libriscv is a fast C++17 RISC-V emulator/sandbox library. The fuzz surface is the Machine:
# the single harness fuzz.cpp has two compile-time modes, selected by -D:
#   FUZZ_VM   — feed attacker bytes as a raw RISC-V instruction stream and simulate() them.
#   FUZZ_ELF  — feed attacker bytes as a RISC-V ELF program: the ELF loader parses + maps it,
#               then the emulator executes it (loading+executing an attacker binary in the VM).
# We build two targets, preserving the fork's original Mayhem target name:
#   vmfuzzer32   (FUZZ_VM,  RISCV_ARCH=4)  — the old fork target, kept for parity.
#   elffuzzer64  (FUZZ_ELF, RISCV_ARCH=8)  — the ELF load+execute surface (64-bit guest).
#
# Build contract comes from the org base ENV (CC/CXX/SANITIZER_FLAGS/LIB_FUZZING_ENGINE/SRC/
# STANDALONE_FUZZ_MAIN). We compile the libriscv library ITSELF with $SANITIZER_FLAGS so the
# emulator core (not just the harness) is instrumented.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' — must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# `=` (not `:=`) for SANITIZER_FLAGS so an explicit empty --build-arg builds with NO sanitizers.
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS

# Add -fsanitize=fuzzer-no-link to library compile flags when building with libFuzzer so the
# libriscv core gets SanitizerCoverage edge instrumentation. Without this the library is opaque
# to libFuzzer — coverage counters only exist in fuzz.cpp itself (~354 edges), Mayhem sees 0
# new edges across the interesting emulator code, and runs stall immediately.
# Guard so a non-fuzzer LIB_FUZZING_ENGINE (e.g. AFL) doesn't inject an unknown flag.
LIB_SANITIZER_FLAGS="$SANITIZER_FLAGS"
case "$LIB_FUZZING_ENGINE" in
  *fuzzer*) LIB_SANITIZER_FLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link" ;;
esac

cd "$SRC"

# Use the mayhem overlay harness (int return type fix) rather than the upstream fuzz/fuzz.cpp.
# The overlay is a verbatim copy of upstream plus the LLVMFuzzerTestOneInput signature fix.
FUZZ_SRC="$SRC/mayhem/fuzz.cpp"
[ -f "$FUZZ_SRC" ] || { echo "ERROR: $FUZZ_SRC missing" >&2; exit 1; }

# ── 1) Build the libriscv static library WITH sanitizers + coverage instrumentation ─────────────
# Binary translation (libtcc/tinycc) is fetched from the network and is not needed to fuzz the
# interpreter — disable it for a self-contained, reproducible build. 128-bit is experimental; we
# build the 32- and 64-bit ISAs (the two arches our targets use).
cmake -S "$SRC/lib" -B "$SRC/build" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_C_FLAGS="$LIB_SANITIZER_FLAGS $DEBUG_FLAGS" -DCMAKE_CXX_FLAGS="$LIB_SANITIZER_FLAGS $DEBUG_FLAGS" \
      -DRISCV_BINARY_TRANSLATION=OFF \
      -DRISCV_32I=ON -DRISCV_64I=ON -DRISCV_128I=OFF
cmake --build "$SRC/build" -j"$MAYHEM_JOBS"

RISCV_LIB="$(find "$SRC/build" -name 'libriscv.a' -print -quit)"
[ -n "$RISCV_LIB" ] && [ -f "$RISCV_LIB" ] || {
  echo "ERROR: libriscv.a not produced by the cmake build" >&2
  find "$SRC/build" -name 'libriscv*.a' -print >&2; exit 1; }
echo "built static lib: $RISCV_LIB"

# Include roots: the library's public headers live under $SRC/lib (e.g. <libriscv/machine.hpp>),
# and the generated headers (libriscv_settings.h, config) land in the cmake build dir.
# $SRC/fuzz is needed because mayhem/fuzz.cpp includes "helpers.cpp" (a relative include that
# lives in fuzz/ alongside the upstream harness).
INCLUDES=(-I "$SRC/lib" -I "$SRC/build" -I "$SRC/fuzz")

# libriscv links pthreads and dl.
LINK_LIBS=(-lpthread -ldl)

# Standalone driver compiled as a C object so its extern "C" LLVMFuzzerTestOneInput ref isn't
# mangled by clang++ at link.
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o /tmp/standalone_main.o

# build_target <out-name> <RISCV_ARCH> <MODE-define>
build_target() {
  local name="$1" arch="$2" mode="$3"

  # libFuzzer target -> /mayhem/<name>
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS -std=c++17 \
      -DRISCV_ARCH="$arch" -D"$mode"=1 \
      "${INCLUDES[@]}" \
      "$FUZZ_SRC" $LIB_FUZZING_ENGINE "$RISCV_LIB" "${LINK_LIBS[@]}" \
      -o "/mayhem/$name"

  # standalone reproducer (no libFuzzer runtime) -> /mayhem/<name>-standalone
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS -std=c++17 \
      -DRISCV_ARCH="$arch" -D"$mode"=1 \
      "${INCLUDES[@]}" \
      "$FUZZ_SRC" /tmp/standalone_main.o "$RISCV_LIB" "${LINK_LIBS[@]}" \
      -o "/mayhem/$name-standalone"

  echo "built $name (+ standalone)"
}

# ── 2) Build the two fuzz targets ─────────────────────────────────────────────────────────────────
build_target vmfuzzer32  4 FUZZ_VM     # old fork target (parity): instruction-stream fuzzing, 32-bit
build_target elffuzzer64 8 FUZZ_ELF    # ELF load+execute surface, 64-bit guest

echo "build.sh complete:"
ls -la /mayhem/vmfuzzer32 /mayhem/vmfuzzer32-standalone /mayhem/elffuzzer64 /mayhem/elffuzzer64-standalone
