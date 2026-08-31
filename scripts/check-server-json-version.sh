#!/usr/bin/env bash
# Guard the MCP Registry manifest's version against drift.
#
# server.json carries the released version in two places — the `version` field
# and the tag embedded in every asset download URL — and nothing else in the
# tree reads it. docs/RELEASING.md asks for a manual bump, which is exactly the
# kind of step that gets skipped: the file sat at 0.17.0 through v0.18.0 AND
# v0.19.0, pointing the registry at assets two releases stale, and the checklist
# added after the FIRST time it happened did not stop the second.
#
# Usage:
#   check-server-json-version.sh              # internal consistency only
#   check-server-json-version.sh v0.20.0      # ...and must equal this version
#
# With no argument it asserts the `version` field and every download URL agree
# with each other, which is safe to run on any commit: between releases the file
# legitimately holds the LAST released version, so there is no tag to compare
# against. Pass the release version in the release pipeline, where there is.
set -euo pipefail

cd "$(dirname "$0")/.."

EXPECTED="${1:-}"
EXPECTED="${EXPECTED#v}" # accept a tag name (v0.20.0) or a bare version

python3 - "$EXPECTED" <<'PY'
import json
import re
import sys

expected = sys.argv[1]

try:
    with open("server.json", encoding="utf-8") as fh:
        doc = json.load(fh)
except (OSError, ValueError) as exc:
    print(f"FAIL: server.json does not parse: {exc}")
    sys.exit(1)

field = doc.get("version")
if not field:
    print("FAIL: server.json has no version field")
    sys.exit(1)

raw = open("server.json", encoding="utf-8").read()
# Every asset URL embeds the tag: .../releases/download/vX.Y.Z/<asset>
urls = re.findall(r"/releases/download/v([0-9]+\.[0-9]+\.[0-9]+)/", raw)
if not urls:
    print("FAIL: server.json has no /releases/download/vX.Y.Z/ asset URLs")
    sys.exit(1)

problems = []

mismatched = sorted({u for u in urls if u != field})
if mismatched:
    problems.append(
        f"version field is {field} but {len(mismatched)} distinct URL version(s) "
        f"disagree: {', '.join(mismatched)}"
    )

if expected:
    if field != expected:
        problems.append(f"version field is {field}, expected {expected}")
    off = sorted({u for u in urls if u != expected})
    if off:
        problems.append(f"asset URLs carry {', '.join(off)}, expected {expected}")

if problems:
    print("FAIL: server.json version drift")
    for p in problems:
        print(f"  - {p}")
    print()
    print("Fix: set the version field AND every /releases/download/vX.Y.Z/ URL")
    print("to the version being released (docs/RELEASING.md step 3).")
    sys.exit(1)

target = expected or field
print(f"OK: server.json version {target} across the field and {len(urls)} asset URLs")
PY
