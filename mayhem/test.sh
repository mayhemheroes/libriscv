#!/usr/bin/env bash
#
# libriscv/mayhem/test.sh — BUILD AND RUN libriscv's real Catch2 unit suite (tests/unit/) and emit a
# CTRF summary. exit 0 iff every ctest case passes.
#
# Unlike the draco/ericw-tools test.sh (which only RUN prebuilt gtest binaries), this suite must be
# COMPILED here: each unit test cross-compiles tiny RISC-V guest programs with a riscv64 cross-gcc,
# then executes them inside the libriscv emulator under the Catch2 host harness. That mirrors what
# upstream's tests/unit/run_unit_tests.sh and .github/workflows/unittests.yml do. The tests assert
# real emulator BEHAVIOR (basic/brutal instruction semantics, FP, heap, memory traps/protections,
# ELF load+execute, vmcall, serialization, security) so a no-op patch cannot pass.
#
# This script is ADDITIVE — it only reads upstream files and builds into tests/unit/build. It does
# NOT modify any upstream file. The unit CMakeLists already compiles with its OWN
# -fsanitize=address,undefined; that is the project's choice and we leave it (we do NOT layer our
# SANITIZER_FLAGS on top).
#
# Build deps: the org base image ships clang/cmake/make/ctest/git but NOT the riscv64 cross-compiler.
# We apt-get install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu (the trixie/forky default
# meta-packages, which pull in the versioned riscv64-linux-gnu-gcc-<N>). The base runs test.sh as the
# unprivileged `mayhem` user, so the apt install is normally done as ROOT in mayhem/Dockerfile before
# this runs; if the packages are already present we skip the install, and if we happen to be root we
# install them ourselves (covers running this script standalone in a root shell).
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Default SRC to the repo root so this runs both in-image (/mayhem) and from a bare checkout.
: "${SRC:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

fail() { echo "test.sh: $*" >&2; emit_ctrf "ctest" 0 1 0; exit 2; }

# ── 1) Ensure the riscv64 cross-compiler ─────────────────────────────────────────────────────────
# Upstream's tests/unit/scripts/find_compiler.sh only probes for gcc-10/11/12; the base image's
# default cross-gcc is newer (e.g. gcc-14), so find_compiler.sh would leave RCC/RCXX unset. We
# therefore detect the installed version ourselves and export RCC/RCXX, which codebuilder.cpp reads
# (env_with_default("RCC"/"RCXX")) in preference to its gcc-12 defaults.
ensure_cross_compiler() {
  # Already installed? (any version of the riscv64 cross-gcc)
  if ls /usr/bin/riscv64-linux-gnu-gcc-* >/dev/null 2>&1 || command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
    return 0
  fi
  local PKGS="gcc-riscv64-linux-gnu g++-riscv64-linux-gnu"
  echo "test.sh: installing riscv64 cross-compiler ($PKGS)"
  local SUDO=""
  if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
      echo "test.sh: not root and no sudo — the cross-compiler must be apt-installed as root in mayhem/Dockerfile before test.sh runs" >&2
      return 1
    fi
  fi
  # Re-pin the apt snapshot for reproducibility when available (ships in the org base), matching the
  # house build.sh convention; harmless if absent.
  if [ -n "${SOURCE_DATE_EPOCH:-}" ] && command -v repro-sources-list.sh >/dev/null 2>&1; then
    $SUDO repro-sources-list.sh || true
  fi
  $SUDO apt-get update -qq || true
  $SUDO apt-get install -y --no-install-recommends $PKGS
}
ensure_cross_compiler || fail "could not obtain the riscv64 cross-compiler"

# Pick the highest installed cross-gcc version and export RCC/RCXX for codebuilder.cpp.
GVER="$(ls /usr/bin/riscv64-linux-gnu-gcc-* 2>/dev/null | grep -oE '[0-9]+$' | sort -rn | head -1)"
if [ -n "$GVER" ]; then
  export RCC="riscv64-linux-gnu-gcc-$GVER" RCXX="riscv64-linux-gnu-g++-$GVER"
elif command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
  export RCC="riscv64-linux-gnu-gcc" RCXX="riscv64-linux-gnu-g++"
else
  fail "no riscv64-linux-gnu cross-compiler on PATH after install"
fi
echo "test.sh: using RCC=$RCC RCXX=$RCXX"
command -v "$RCC" >/dev/null 2>&1 || fail "$RCC not found on PATH"
command -v "$RCXX" >/dev/null 2>&1 || fail "$RCXX not found on PATH"

# ── 2) Ensure the two test submodules (Catch2 host harness + lodepng for the png test) ───────────
# This is a fork checkout, so tests/Catch2 and tests/unit/ext/lodepng may be empty gitlink dirs.
# Prefer `git submodule update --init`; fall back to a direct clone at the pinned commit if the
# submodule machinery is unhappy (e.g. .git is a worktree/gitdir pointer).
ensure_submodule() {
  local path="$1"
  # Considered populated if a CMakeLists.txt / source header exists inside.
  if [ -f "$SRC/$path/CMakeLists.txt" ] || [ -f "$SRC/$path/lodepng.h" ]; then return 0; fi
  echo "test.sh: initializing submodule $path"
  git config --global --add safe.directory '*' 2>/dev/null || true
  if git -C "$SRC" submodule update --init "$path" >/dev/null 2>&1; then
    [ -n "$(ls -A "$SRC/$path" 2>/dev/null)" ] && return 0
  fi
  # Offline/worktree fallback: clone from the .gitmodules URL at the pinned gitlink commit.
  local url commit
  url="$(git -C "$SRC" config --file .gitmodules --get "submodule.$path.url" 2>/dev/null)"
  commit="$(git -C "$SRC" ls-tree HEAD "$path" 2>/dev/null | awk '{print $3}')"
  [ -n "$url" ] || { echo "test.sh: no .gitmodules URL for $path" >&2; return 1; }
  rm -rf "$SRC/$path"
  git clone "$url" "$SRC/$path" || return 1
  if [ -n "$commit" ]; then git -C "$SRC/$path" checkout -q "$commit" || true; fi
}
ensure_submodule tests/Catch2            || fail "could not populate tests/Catch2"
ensure_submodule tests/unit/ext/lodepng  || fail "could not populate tests/unit/ext/lodepng"
[ -f "$SRC/tests/Catch2/CMakeLists.txt" ] || fail "tests/Catch2 has no CMakeLists.txt after init"

# ── 3) Configure + build the unit tests exactly as run_unit_tests.sh / the CI does ────────────────
# Flags from tests/unit/run_unit_tests.sh: Debug, memory traps ON (this also enables virtual paging,
# which adds the memtrap+protect tests), threading OFF, flat RW arena OFF, binary translation OFF
# (the last avoids a network fetch of libtcc). The unit CMakeLists supplies its own ASan+UBSan flags.
BUILD="$SRC/tests/unit/build"
: "${MAYHEM_JOBS:=$(nproc 2>/dev/null || echo 4)}"

echo "=== configure (cmake) ==="
cmake -S "$SRC/tests/unit" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DRISCV_MEMORY_TRAPS=ON \
      -DRISCV_THREADED=OFF \
      -DRISCV_FLAT_RW_ARENA=OFF \
      -DRISCV_BINARY_TRANSLATION=OFF || fail "cmake configure failed"

echo "=== build (cmake --build -j$MAYHEM_JOBS) ==="
cmake --build "$BUILD" -j"$MAYHEM_JOBS" || fail "unit-test build failed"

# ── 4) Sanity check: verify a test binary actually runs and produces test output ────────────────
# This catches sabotage/exit(0) injection via LD_PRELOAD or similar. Run one test with a timeout
# and verify it produces Catch2 output.
SANITY_TEST="$BUILD/crc32c"
if [ -x "$SANITY_TEST" ]; then
  sanity_out="$(timeout 30 "$SANITY_TEST" 2>&1 || true)"
  if ! printf '%s\n' "$sanity_out" | grep -qE "All tests passed|assertions|Catch2"; then
    echo "test.sh: sanity check failed — test binary $SANITY_TEST did not produce expected output (possible injection/sabotage)" >&2
    echo "Output: $sanity_out" >&2
    fail "sanity check: test binary did not produce test output"
  fi
fi

# ── 5) Run ctest and parse the pass/fail tally ────────────────────────────────────────────────────
echo "=== ctest ==="
out="$(cd "$BUILD" && ctest -j"$MAYHEM_JOBS" --output-on-failure 2>&1)"; rc=$?
echo "$out"

# ctest summary line: "NN% tests passed, F tests failed out of T".
PASSED_PCT_LINE="$(printf '%s\n' "$out" | sed -n 's/.*tests passed, \([0-9][0-9]*\) tests* failed out of \([0-9][0-9]*\).*/\1 \2/p' | tail -1)"
FAILED="${PASSED_PCT_LINE%% *}"
TOTAL="${PASSED_PCT_LINE##* }"
if [ -z "$TOTAL" ] || [ -z "$FAILED" ]; then
  # No summary parsed: derive failed count from rc so we never report a false pass.
  echo "test.sh: could not parse ctest summary" >&2
  emit_ctrf "ctest" 0 "$([ "$rc" -eq 0 ] && echo 0 || echo 1)" 0
  exit "$([ "$rc" -eq 0 ] && echo 0 || echo 2)"
fi
PASSED=$(( TOTAL - FAILED )); [ "$PASSED" -lt 0 ] && PASSED=0

emit_ctrf "ctest" "$PASSED" "$FAILED"
