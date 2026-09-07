#!/usr/bin/env bash
# Re-index every repository the experiments need, under path-derived names, with the new binary.
BIN=/Users/tigercosmos/code-cortex-mcp/.claude/worktrees/agent-helpfulness/build/c/code-cortex-mcp
LOG=/private/tmp/claude-501/-Users-tigercosmos-code-cortex-mcp/b671e177-6b86-44ee-b4d7-3ab99a4b2557/scratchpad/exp/logs/reindex.log
: > "$LOG"
for repo in /Users/tigercosmos/_bench/jansson /Users/tigercosmos/_bench/lz4 /Users/tigercosmos/elfuse \
            /Users/tigercosmos/PcapPlusPlus /Users/tigercosmos/modmesh /Users/tigercosmos/Yatagarasu \
            /Users/tigercosmos/cgal /Users/tigercosmos/_bench/opencv; do
  START=$(date +%s)
  OUT=$("$BIN" cli --json index_repository "{\"repo_path\":\"$repo\"}" 2>/dev/null | python3 -c "import json,sys; r=json.load(sys.stdin); t=json.loads(r['content'][0]['text']); print(t.get('project'), t.get('status'), 'nodes=',t.get('nodes'), 'edges=',t.get('edges'), 'partial=',t.get('parse_partial_count'), 'err=',t.get('error'))")
  echo "$(date +%H:%M:%S) $repo -> $OUT ($(( $(date +%s) - START )) s)" >> "$LOG"
done
echo "REINDEX DONE" >> "$LOG"
