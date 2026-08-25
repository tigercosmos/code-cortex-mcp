#!/usr/bin/env bash
# benchmark-index-compare.sh -- time full indexing of checked-out repositories.
#
# Usage: benchmark-index-compare.sh <binary> <engine-label> <bench_dir> <out.csv> <runs> <repo>...
#
# <bench_dir>/<repo> must be a checkout. Each run starts from an empty
# CBM_CACHE_DIR, calls `cli index_repository` in full mode, and appends one CSV
# row: engine,repo,run,seconds,nodes,edges,exit. Works with code-cortex-mcp and
# codebase-memory-mcp binaries alike. See docs/benchmarks/ for recorded results.
set -u
BIN="$1"; ENGINE="$2"; B="$3"; OUT="$4"; RUNS="$5"; shift 5
CACHE="$(mktemp -d)"
[ -f "$OUT" ] || echo "engine,repo,run,seconds,nodes,edges,exit" > "$OUT"
for repo in "$@"; do
  path="$(cd "$B/$repo" && pwd -P)"
  for run in $(seq 1 "$RUNS"); do
    rm -rf "$CACHE"; mkdir -p "$CACHE"
    t0=$(python3 -c 'import time;print(time.time())')
    json=$(CBM_CACHE_DIR="$CACHE" "$BIN" cli index_repository "{\"repo_path\":\"$path\",\"mode\":\"full\"}" 2>"$OUT.$ENGINE.$repo.$run.err")
    rc=$?
    t1=$(python3 -c 'import time;print(time.time())')
    read -r nodes edges <<<"$(printf '%s' "$json" | python3 -c '
import json,sys
try:
    d=json.load(sys.stdin)
    if "content" in d: d=json.loads(d["content"][0]["text"])
    print(d.get("nodes",0), d.get("edges",0))
except Exception: print(0,0)')"
    secs=$(python3 -c "print(round($t1-$t0,2))")
    echo "$ENGINE,$repo,$run,$secs,$nodes,$edges,$rc" | tee -a "$OUT"
  done
done
rm -rf "$CACHE"
echo "RUN DONE $ENGINE"
