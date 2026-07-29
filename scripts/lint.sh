#!/bin/bash
# lint.sh — Run all linters (clang-tidy + cppcheck + clang-format + NOLINT
# policy). SINGLE source of truth for linting.
#
# Usage:
#   scripts/lint.sh                                    # All linters
#   scripts/lint.sh CLANG_FORMAT=clang-format-20       # Override formatter
#   scripts/lint.sh --ci                               # CI mode (skip clang-tidy)
#
# Translated from the former Makefile.cbm lint targets when the tree moved to
# C++23 (cppcheck/clang-tidy now run in C++ mode). The lint source set mirrors
# the old LINT_SRCS: first-party production .cpp, excluding the relaxed-warning
# LSP resolvers (lsp/*), the generated stdlib tables, and vendored/grammar code.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# shellcheck source=env.sh
source "$ROOT/scripts/env.sh"

CI_ONLY=false
for arg in "$@"; do
    case "$arg" in
        --ci) CI_ONLY=true ;;
        CLANG_FORMAT=*) CLANG_FORMAT="${arg#CLANG_FORMAT=}" ;;
        CLANG_TIDY=*)   CLANG_TIDY="${arg#CLANG_TIDY=}" ;;
        CPPCHECK=*)     CPPCHECK="${arg#CPPCHECK=}" ;;
    esac
done

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
CPPCHECK="${CPPCHECK:-cppcheck}"

CBM_DIR="internal/cbm"
TS_INCLUDE="$CBM_DIR/vendored/ts_runtime/include"

# Lint source set (mirrors Makefile.cbm LINT_SRCS, as .cpp).
mapfile -t LINT_SRCS < <(
    ls src/foundation/*.cpp \
       src/store/*.cpp src/cypher/*.cpp src/mcp/*.cpp \
       src/discover/*.cpp src/graph_buffer/*.cpp src/pipeline/*.cpp \
       src/simhash/*.cpp src/semantic/*.cpp \
       src/watcher/*.cpp src/cli/*.cpp src/main.cpp \
       "$CBM_DIR"/cbm.cpp "$CBM_DIR"/extract_*.cpp "$CBM_DIR"/ac.cpp \
       "$CBM_DIR"/helpers.cpp "$CBM_DIR"/lz4_store.cpp "$CBM_DIR"/zstd_store.cpp \
       "$CBM_DIR"/sqlite_writer.cpp 2>/dev/null | sort -u
)
mapfile -t LINT_HDRS < <(ls src/**/*.h src/*.h "$CBM_DIR"/*.h 2>/dev/null | sort -u)

print_env "lint.sh"

run_cppcheck() {
    echo "=== cppcheck ==="
    "$CPPCHECK" --enable=warning,style,performance,portability \
        --std=c++23 --language=c++ \
        --suppressions-list=.cppcheck \
        --error-exitcode=1 \
        --inline-suppr \
        --quiet \
        --suppress=varFuncNullUB \
        --suppress=cstyleCast \
        --suppress=dangerousTypeCast \
        --suppress=passedByValue \
        --suppress=memsetClassFloat \
        --suppress=intToPointerCast \
        --suppress=unusedStructMember \
        --suppress=nullPointerOutOfMemory \
        --suppress=nullPointerArithmeticOutOfMemory \
        --suppress=ctunullpointerOutOfMemory \
        --suppress='nullPointer:internal/cbm/sqlite_writer.cpp' \
        -Isrc -Ivendored -Ivendored/sqlite3 \
        -I"$CBM_DIR" -I"$TS_INCLUDE" \
        "${LINT_SRCS[@]}"
}

run_format() {
    echo "=== clang-format ==="
    "$CLANG_FORMAT" --dry-run --Werror "${LINT_SRCS[@]}" "${LINT_HDRS[@]}"
}

run_no_suppress() {
    echo "=== NOLINT check ==="
    if grep -rn 'NOLINT' src/ "$CBM_DIR"/*.cpp "$CBM_DIR"/*.h 2>/dev/null \
        | grep -v vendored \
        | grep -v 'NOLINT(misc-no-recursion)' \
        | grep -v 'recursion_whitelist.h'; then
        echo "ERROR: Banned NOLINT comment found in source code." >&2
        echo "Only NOLINT(misc-no-recursion) is allowed, and only for whitelisted functions." >&2
        echo "See src/foundation/recursion_whitelist.h for the whitelist." >&2
        exit 1
    fi
    echo "  Checking NOLINT(misc-no-recursion) against whitelist..."
    scripts/check-nolint-whitelist.sh
}

run_tidy() {
    echo "=== clang-tidy ==="
    "$CLANG_TIDY" --quiet "${LINT_SRCS[@]}" -- \
        -std=c++23 -D_DEFAULT_SOURCE -D_GNU_SOURCE \
        -Isrc -Ivendored -Ivendored/sqlite3 -Ivendored/mimalloc/include \
        -I"$CBM_DIR" -I"$TS_INCLUDE"
}

if $CI_ONLY; then
    echo "=== CI mode: cppcheck + clang-format + NOLINT ==="
    run_cppcheck
    run_format
    run_no_suppress
else
    echo "=== Full lint: clang-tidy + cppcheck + clang-format + NOLINT ==="
    run_tidy
    run_cppcheck
    run_format
    run_no_suppress
fi

echo "=== All linters passed ==="
