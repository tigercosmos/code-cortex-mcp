#!/bin/bash
# usage: index.sh <src_root>   (contains source-10k source-1m source-100m)
set -u
W=${BENCH_ROOT:-$HOME/bench-agent}
BIN=$W/repo/build/c/code-cortex-mcp
export CBM_CACHE_DIR=$W/cache
mkdir -p $CBM_CACHE_DIR $W/index
for s in 10k 1m 100m; do
  R=$1/source-$s
  printf '{"repo_path":"%s","mode":"fast"}' "$R" > $W/index/$s.args.json
  /usr/bin/time -v $BIN cli --json index_repository --args-file $W/index/$s.args.json > $W/index/$s.stdout 2> $W/index/$s.stderr
  echo "$s rc=$?" >> $W/index/done.log
done
