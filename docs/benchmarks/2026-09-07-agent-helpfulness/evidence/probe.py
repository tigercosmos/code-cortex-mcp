#!/usr/bin/env python3
"""Availability probe: repeated fresh Codex sessions -> did the cortex tools show up, and how long did init take?
usage: probe.py <binary> <repo> <out_dir> <reps> <config-name>...
config-names: optional | required | grace0 | required_grace0
"""
import json, os, subprocess, sys, time
from pathlib import Path
binary, repo, out, reps = sys.argv[1], sys.argv[2], Path(sys.argv[3]), int(sys.argv[4])
configs = sys.argv[5:]
out.mkdir(parents=True, exist_ok=True)
PROMPT = ("Setup check, read-only, do not explore source or run shell commands. "
          "Call the code-cortex-mcp tool list_projects once. Then reply with exactly one line: "
          "CORTEX_OK <number of projects returned>. If no code-cortex tool is available to you, reply exactly: TOOL_MISSING.")
def flags(cfg):
    v = ['approval_policy="never"', 'project_doc_max_bytes=0', 'features.memories=false',
         'memories.use_memories=false', 'memories.generate_memories=false', 'features.multi_agent=false',
         'features.apps=false', 'features.plugins=false', 'web_search="disabled"',
         'check_for_update_on_startup=false', 'model_reasoning_effort="medium"',
         'mcp_servers.code-cortex-mcp.command=' + json.dumps(binary),
         'mcp_servers.code-cortex-mcp.startup_timeout_sec=60',
         'mcp_servers.code-cortex-mcp.tool_timeout_sec=120',
         'mcp_servers.code-cortex-mcp.default_tools_approval_mode="approve"']
    if 'required' in cfg: v.append('mcp_servers.code-cortex-mcp.required=true')
    if 'grace0' in cfg: v.append('mcp_optional_startup_grace_ms=0')
    return [x for val in v for x in ['-c', val]]
rows = []
for rep in range(1, reps + 1):
    for cfg in configs:
        name = f'{cfg}-r{rep}'
        home = out / 'homes' / name; home.mkdir(parents=True, exist_ok=True)
        (home / 'auth.json').symlink_to(Path.home() / '.codex/auth.json')
        env = {k: v for k, v in os.environ.items() if not k.startswith('CODEX_')}
        env['CODEX_HOME'] = str(home)
        env['CBM_LOG_FILE'] = str(out / f'{name}.server.log')
        args = ['codex', 'exec', '--ignore-user-config', '--ignore-rules', '--json', '--sandbox', 'read-only',
                '-C', repo, '-m', 'gpt-6-astra', *flags(cfg), PROMPT]
        t0 = time.time()
        p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=300)
        wall = time.time() - t0
        (out / f'{name}.jsonl').write_text(p.stdout); (out / f'{name}.err').write_text(p.stderr)
        mcp_calls = 0; final = ''
        for ln in p.stdout.splitlines():
            try: ev = json.loads(ln)
            except Exception: continue
            it = ev.get('item') or {}
            if it.get('type') == 'mcp_tool_call':
                mcp_calls += 1
            if it.get('type') == 'agent_message': final = it.get('text', '')
        verdict = 'ok' if 'CORTEX_OK' in final else ('missing' if 'TOOL_MISSING' in final else 'other')
        row = dict(cfg=cfg, rep=rep, wall_s=round(wall, 2), rc=p.returncode, mcp_call_events=mcp_calls,
                   verdict=verdict, final=final.strip()[:120], stderr_tail=p.stderr.strip()[-300:])
        rows.append(row); print(json.dumps(row), flush=True)
        (out / 'rows.jsonl').open('a').write(json.dumps(row) + '\n')
        (home / 'auth.json').unlink()
