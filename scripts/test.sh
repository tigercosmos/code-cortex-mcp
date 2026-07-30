#!/bin/bash
# test.sh — Clean build + run the C++23 test suite with ASan + UBSan.
#
# Usage:
#   scripts/test.sh                          # Auto-detect everything
#   scripts/test.sh --arch x86_64            # Force x86_64 build
#   scripts/test.sh CC=gcc-14 CXX=g++-14    # Override compiler
#
# This script is the SINGLE source of truth for running tests.
# Used identically in local development and CI workflows.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Parse --arch flag before sourcing env.sh
for arg in "$@"; do
    case "$arg" in
        --arch) :;; # next arg is the value, handled below
        arm64|x86_64)
            # Check if previous arg was --arch
            if [[ "${prev_arg:-}" == "--arch" ]]; then
                export CBM_ARCH="$arg"
            fi
            ;;
    esac
    prev_arg="$arg"
done

# Also support --arch=value
for arg in "$@"; do
    case "$arg" in
        --arch=*) export CBM_ARCH="${arg#--arch=}" ;;
    esac
done

# shellcheck source=env.sh
source "$ROOT/scripts/env.sh"

# Forward CC/CXX and collect make-passthrough args
MAKE_ARGS=""
for arg in "$@"; do
    case "$arg" in
        CC=*|CXX=*) export "${arg}" ;;
        --arch|--arch=*) ;; # already handled
        arm64|x86_64) ;; # already handled
        *=*) MAKE_ARGS="$MAKE_ARGS $arg" ;; # forward any VAR=VAL to make
    esac
done

print_env "test.sh"

# Verify compiler supports target arch
verify_compiler "$CC"

# Step 1: Clean
scripts/clean.sh

# Step 2: Configure with ASan+UBSan (mirrors the old `make test`). $MAKE_ARGS
# carries any VAR=VAL passthrough; CC/CXX are already exported above and fed to
# CMake explicitly so a re-run with a different compiler reconfigures cleanly.
CMAKE_ARGS=(-DCBM_SANITIZE=ON)
[[ -n "${CC:-}" ]]  && CMAKE_ARGS+=(-DCMAKE_C_COMPILER="$CC")
[[ -n "${CXX:-}" ]] && CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="$CXX")
# Windows (msys2): use Unix Makefiles + the msys2 clang toolchain, not the
# default Visual Studio/MSVC generator (which rejects the GCC-style flags).
[[ "$OS" == "windows" ]] && CMAKE_ARGS+=(-G "Unix Makefiles")
$ARCH_PREFIX cmake -S "$ROOT" -B "$ROOT/build/c" "${CMAKE_ARGS[@]}"

# Step 3: Build the test runner
$ARCH_PREFIX cmake --build "$ROOT/build/c" -j"$NPROC" --target test-runner

# Step 4: Run the full suite from the repo root (tests use CWD-relative fixtures)
"$ROOT/build/c/test-runner"

echo "=== All tests passed ==="
