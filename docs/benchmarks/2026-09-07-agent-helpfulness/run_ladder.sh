#!/usr/bin/env bash
# Size-ladder matrix: 28 tasks x off/old/new x REPS reps x 2 models, paired within (task, rep, model).
set -u
E=/private/tmp/claude-501/-Users-tigercosmos-code-cortex-mcp/b671e177-6b86-44ee-b4d7-3ab99a4b2557/scratchpad/exp
TASKS="$E/tasks_ladder.json"
REPS="${REPS:-2}"
PAR="${PAR:-3}"
export EFFORT=medium
for M in claude-fable-5-1 claude-opus-5; do
  short=$(echo "$M" | sed 's/claude-//;s/-5-1/5/;s/-5$/5/')
  OUT="$E/runs_ladder${RUN_TAG:-}_$short"
  rm -rf "$OUT"
  echo "[ladder] === $M -> $OUT === $(date +%H:%M:%S)"
  ARMS="off old new" "$E/orchestrate.sh" "$TASKS" "$M" "$REPS" "$OUT" "$PAR" > "$E/logs/ladder${RUN_TAG:-}_$short.log" 2>&1
  echo "[ladder] $M done: $(ls "$OUT"/*.metrics.json 2>/dev/null | wc -l) runs $(date +%H:%M:%S)"
done
echo "LADDER ALL DONE"
