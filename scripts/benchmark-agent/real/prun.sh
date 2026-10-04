#!/bin/bash
# usage: BENCH_BIN=<bin> SFX=-v6 SHARDS=4 [REPS=1] [ARMS=off,mcp] ./prun.sh
# Runs both task sets for both models with SHARDS parallel processes per (model,set).
set -u
cd ~/realbench-20261003
BENCH_BIN=${BENCH_BIN:?}; SFX=${SFX:?}; SHARDS=${SHARDS:-4}; REPS=${REPS:-1}; ARMS=${ARMS:-off,mcp}
grep -q "BIN=\"$BENCH_BIN\"" ~/bench-agent-20261003/cfg-mcp/hooks/cbm-code-discovery-gate || { echo "ABORT: hooks BIN differs"; exit 1; }
export BENCH_BIN
P=$HOME/prevbench-20261003/tasks_prev.json
start=$(date +%s)
for spec in "real tasks_real.json" "prev $P"; do
  set -- $spec; name=$1; tf=$2
  for model in claude-opus-5-5 claude-fable-5-1; do
    short=${model#claude-}; short=${short%%-*}
    python3 - "$tf" "$SHARDS" > /tmp/shards.$$ <<PY
import json,sys
t=[x["id"] for x in json.load(open(sys.argv[1]))]; k=int(sys.argv[2])
for i in range(k): print(",".join(t[i::k]))
PY
    i=0
    while read ids; do
      i=$((i+1))
      python3 run_real.py --model $model --tasks-file $tf --out $name-$short$SFX.s$i --tasks "$ids" --reps $REPS --arms $ARMS > $name-$short$SFX.s$i.log 2>&1 &
    done < /tmp/shards.$$
  done
done
wait
for name in real prev; do for short in opus fable; do mkdir -p $name-$short$SFX; cat $name-$short$SFX.s*/results.jsonl > $name-$short$SFX/results.jsonl; done; done
echo "PRUN_DONE $SFX $(( $(date +%s) - start )) s" | tee -a prun.log
