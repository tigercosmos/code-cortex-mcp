#!/usr/bin/env bash
# usage: orchestrate.sh <tasks.json> <model> <reps> <outdir> <pairs_in_parallel>
# Runs every (task, rep) as a triple of arms launched together (off/old/new),
# PAIRS triples at a time. ARMS, REP_START, EFFORT are env overrides.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
TASKS="$1"; MODEL="$2"; REPS="$3"; OUT="$4"; PAR="${5:-3}"
mkdir -p "$OUT" "$D/prompts" "$D/logs"
python3 - "$TASKS" "$D/prompts" "$OUT/_joblist.tsv" <<'PY'
import json, sys, os
tasks = json.load(open(sys.argv[1])); out = sys.argv[2]
with open(sys.argv[3], 'w') as jl:
    for t in tasks:
        open(os.path.join(out, t['id'] + '.txt'), 'w').write(t['question'].rstrip() + "\n")
        jl.write(t['id'] + '\t' + t['repo_path'] + '\n')
PY
ARMS="${ARMS:-off old new}"
run_triple() {
  local id="$1" repo="$2" rep="$3"
  local pids=""
  for a in $ARMS; do
    "$D/runner.sh" "$a" "$repo" "$id" "$rep" "$MODEL" "$D/prompts/$id.txt" "$OUT" >>"$D/logs/orch.log" 2>&1 &
    pids="$pids $!"
  done
  wait $pids
  echo "done $id rep$rep $(date +%H:%M:%S)"
}
n=0
for rep in $(seq "${REP_START:-1}" "$REPS"); do
  while IFS=$'\t' read -r id repo; do
    [ -n "$id" ] || continue
    run_triple "$id" "$repo" "$rep" &
    n=$((n+1))
    if [ $((n % PAR)) -eq 0 ]; then wait; fi
  done < "$OUT/_joblist.tsv"
done
wait
echo "ALL DONE"
