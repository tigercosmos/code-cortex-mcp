#!/usr/bin/env bash
# A/B runner: off (no MCP, hooks disabled) | old (installed binary + old directive hooks)
#             | new (this branch's binary + new context hooks).
# usage: runner.sh <off|old|new> <repo_path> <task_id> <rep> <model> <prompt_file> <outdir>
set -u
ARM="$1"; REPO="$2"; TASK="$3"; REP="$4"; MODEL="$5"; PROMPT_FILE="$6"; OUT="$7"
OLD_BIN=/Users/tigercosmos/.local/bin/code-cortex-mcp
NEW_BIN="${CORTEX_NEW_BIN:-/Users/tigercosmos/code-cortex-mcp/build/c/code-cortex-mcp}"
CLAUDE=/Users/tigercosmos/.local/bin/claude
D="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
STEM="$OUT/${TASK}__${ARM}__r${REP}"

case "$ARM" in
  off)
    MCPCFG='{"mcpServers":{}}'
    export CORTEX_AB_OFF=1; unset CORTEX_AB_ARM CORTEX_AB_BIN ;;
  old)
    MCPCFG="{\"mcpServers\":{\"code-cortex-mcp\":{\"command\":\"$OLD_BIN\"}}}"
    export CORTEX_AB_OFF=0 CORTEX_AB_ARM=old CORTEX_AB_BIN="$OLD_BIN" ;;
  new)
    MCPCFG="{\"mcpServers\":{\"code-cortex-mcp\":{\"command\":\"$NEW_BIN\"}}}"
    export CORTEX_AB_OFF=0 CORTEX_AB_ARM=new CORTEX_AB_BIN="$NEW_BIN" ;;
  *) echo "bad arm $ARM" >&2; exit 2 ;;
esac

START=$(python3 -c 'import time;print(time.time())')
cd "$REPO" || exit 3
PROMPT="$(cat "$PROMPT_FILE")"
ANTHROPIC_API_KEY= "$CLAUDE" -p "$PROMPT" \
  --output-format stream-json --verbose --include-hook-events \
  --model "$MODEL" \
  --effort "${EFFORT:-medium}" \
  --strict-mcp-config --mcp-config "$MCPCFG" \
  --permission-mode bypassPermissions \
  --no-session-persistence \
  --disable-slash-commands \
  > "$STEM.jsonl" 2> "$STEM.err"
RC=$?
END=$(python3 -c 'import time;print(time.time())')
EFFORT="${EFFORT:-medium}" python3 "$D/extract.py" "$STEM.jsonl" "$STEM.metrics.json" "$ARM" "$REPO" "$TASK" "$REP" "$MODEL" "$START" "$END" "$RC"
