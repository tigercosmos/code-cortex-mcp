#!/bin/bash
# security.sh — Run the full security audit suite (replaces the former
# `make -f Makefile.cbm security` target). Builds the production binary first,
# then runs every layer against it.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN="$ROOT/build/c/code-cortex-mcp"

echo "=== Building production binary ==="
scripts/build.sh "$@"

echo "=== Running security audit suite ==="
scripts/security-audit.sh
scripts/security-strings.sh "$BIN"
scripts/security-install.sh "$BIN"
scripts/security-network.sh "$BIN"
scripts/security-fuzz.sh "$BIN"
scripts/security-vendored.sh
echo "=== All security checks passed ==="
