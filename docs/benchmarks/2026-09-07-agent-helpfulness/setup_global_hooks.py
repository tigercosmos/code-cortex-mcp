#!/usr/bin/env python3
"""Make the global Claude Code hooks A/B-switchable by environment variable.

Backs up ~/.claude/settings.json and the three cbm-* hook scripts to
<scratch>/backup_global/, then rewrites the scripts so that:
  CORTEX_AB_OFF=1      -> every hook exits silently (control arm)
  CORTEX_AB_ARM=old    -> SessionStart/SubagentStart print the pre-change directive text
  CORTEX_AB_BIN=<path> -> the shim runs that binary instead of the installed one
and adds the PostToolUse(Edit|Write|MultiEdit) entry the new installer writes.
Default behaviour (no env vars) is what `code-cortex-mcp install` on this branch produces,
pointed at the installed binary.
Run with --restore to put the backups back.
"""
import json, os, shutil, sys, stat
home = os.path.expanduser('~')
cfg = os.path.join(home, '.claude')
hooks = os.path.join(cfg, 'hooks')
backup = '/private/tmp/claude-501/-Users-tigercosmos-code-cortex-mcp/b671e177-6b86-44ee-b4d7-3ab99a4b2557/scratchpad/backup_global'
files = ['settings.json', 'hooks/cbm-code-discovery-gate', 'hooks/cbm-session-reminder', 'hooks/cbm-subagent-reminder']
INSTALLED = '/Users/tigercosmos/.local/bin/code-cortex-mcp'

if '--restore' in sys.argv:
    for f in files:
        src = os.path.join(backup, f.replace('/', '__'))
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(cfg, f)); print('restored', f)
    sys.exit(0)

os.makedirs(backup, exist_ok=True)
for f in files:
    p = os.path.join(cfg, f)
    if os.path.exists(p):
        dst = os.path.join(backup, f.replace('/', '__'))
        if not os.path.exists(dst):
            shutil.copy2(p, dst); print('backed up', f)

OLD_SESSION = '''CRITICAL - Code Discovery Protocol:
1. ALWAYS use code-cortex-mcp tools FIRST for ANY code exploration:
   - search_graph(name_pattern/label/qn_pattern) to find functions/classes/routes
   - trace_path(function_name, mode=calls|data_flow|cross_service) for call chains
   - get_code_snippet(qualified_name) for exact symbol source (precise ranges)
   - query_graph(query) for complex Cypher patterns
   - get_architecture(aspects) for project structure
   - search_code(pattern) for text search (graph-augmented grep)
2. Use Grep/Glob/Read freely for text, configs, non-code files, and
   always Read a file before editing it.
3. If a project is not indexed yet, run index_repository FIRST.'''
NEW_FALLBACK = ('code-cortex-mcp is installed. Use its graph for what grep cannot do: inspect_symbol(<name>) for '
                'direct callers with call-site lines, related tests and cross-language callers; trace_path for '
                'multi-hop call chains; detect_changes for the blast radius of your edits. Plain grep is fine for '
                'text and exact identifiers. The project argument is optional inside an indexed repository; run '
                'index_repository once if it is not indexed.')
OLD_SUB = ('{"hookSpecificOutput":{"hookEventName":"SubagentStart","additionalContext":"Code discovery: prefer '
           'code-cortex-mcp tools (search_graph, trace_path, get_code_snippet, query_graph, get_architecture, '
           'search_code) over grep/file-read for navigating code. Use Grep/Glob/Read for text, configs, and '
           'non-code files."}}')
NEW_SUB = ('{"hookSpecificOutput":{"hookEventName":"SubagentStart","additionalContext":"code-cortex-mcp graph tools '
           'are available. Use inspect_symbol(<name>) for direct callers with call-site lines, related tests and '
           'cross-language callers; trace_path for multi-hop call chains; detect_changes for the blast radius of '
           'edits. Plain grep is fine for text and exact identifiers. The project argument is optional inside this '
           'repository."}}')

gate = f'''#!/usr/bin/env bash
# code-cortex-mcp hook shim (PreToolUse search augment / PostToolUse edit impact).
# NEVER blocks: any failure is silent (exit 0, no output).
# A/B switches: CORTEX_AB_OFF=1 disables; CORTEX_AB_BIN overrides the binary.
[ "${{CORTEX_AB_OFF:-0}}" = "1" ] && exit 0
BIN="${{CORTEX_AB_BIN:-{INSTALLED}}}"
[ -x "$BIN" ] || exit 0
"$BIN" hook-augment 2>/dev/null
exit 0
'''
session = f'''#!/usr/bin/env bash
# SessionStart hook: code-cortex-mcp architecture brief (A/B-switchable).
[ "${{CORTEX_AB_OFF:-0}}" = "1" ] && exit 0
if [ "${{CORTEX_AB_ARM:-}}" = "old" ]; then
cat << 'REMINDER'
{OLD_SESSION}
REMINDER
exit 0
fi
BIN="${{CORTEX_AB_BIN:-{INSTALLED}}}"
if [ -x "$BIN" ]; then
  OUT="$("$BIN" hook-augment 2>/dev/null)"
  if [ -n "$OUT" ]; then printf '%s\\n' "$OUT"; exit 0; fi
fi
cat << 'REMINDER'
{NEW_FALLBACK}
REMINDER
'''
sub = f'''#!/usr/bin/env bash
# SubagentStart hook (A/B-switchable).
[ "${{CORTEX_AB_OFF:-0}}" = "1" ] && exit 0
if [ "${{CORTEX_AB_ARM:-}}" = "old" ]; then
cat << 'REMINDER'
{OLD_SUB}
REMINDER
exit 0
fi
cat << 'REMINDER'
{NEW_SUB}
REMINDER
'''
for name, body in [('cbm-code-discovery-gate', gate), ('cbm-session-reminder', session), ('cbm-subagent-reminder', sub)]:
    p = os.path.join(hooks, name)
    open(p, 'w').write(body)
    os.chmod(p, 0o755)
    print('wrote', p)

sp = os.path.join(cfg, 'settings.json')
d = json.load(open(sp))
post = d.setdefault('hooks', {}).setdefault('PostToolUse', [])
if not any(h.get('matcher') == 'Edit|Write|MultiEdit' for h in post):
    post.append({'matcher': 'Edit|Write|MultiEdit',
                 'hooks': [{'type': 'command', 'command': '~/.claude/hooks/cbm-code-discovery-gate', 'timeout': 5}]})
    json.dump(d, open(sp, 'w'), indent=2)
    print('added PostToolUse hook to settings.json')
else:
    print('PostToolUse hook already present')
