#!/bin/bash
# mkcfg.sh — build the two isolated Claude Code config dirs the agent benchmark uses.
#
#   cfg-off : logged-in copy of ~/.claude with settings.json = {} (no hooks, no skill)
#   cfg-mcp : the same, then `code-cortex-mcp install` run against it, so it carries
#             the product's hooks (UserPromptSubmit, PreToolUse, PostToolUse,
#             SessionStart, SubagentStart) and the code-cortex skill.
#
# The MCP server itself is passed per session by run.py (--mcp-config), so neither
# dir needs an MCP entry. HOME is pointed at an empty directory during install so
# that Codex, Cursor, Gemini and the real ~/.claude are never touched.
#
# WARNING: `install` sends SIGTERM to every running code-cortex-mcp process.
# Do not run this while a benchmark or an index is running.
set -eu
W=${BENCH_ROOT:-$HOME/bench-agent}
BIN=${BENCH_BIN:-$W/repo/build/c/code-cortex-mcp}
for a in off mcp; do
  D=$W/cfg-$a; mkdir -p $D
  cp ~/.claude/.credentials.json $D/.credentials.json
  cp ~/.claude.json $D/.claude.json
  echo '{}' > $D/settings.json
done
mkdir -p $W/fakehome
CLAUDE_CONFIG_DIR=$W/cfg-mcp HOME=$W/fakehome CBM_CACHE_DIR=$W/cache "$BIN" install -y </dev/null
# The shims must call the benchmarked binary, not whatever install found on PATH.
for f in $W/cfg-mcp/hooks/*; do
  sed -i "s|^BIN=.*|BIN=\"$BIN\"|" "$f"
done
echo "--- cfg-mcp hooks:"; grep -o '"[A-Za-z]*": \[' $W/cfg-mcp/settings.json | sort -u
echo "--- shim binary:"; grep -h '^BIN=' $W/cfg-mcp/hooks/* | sort -u
