#!/usr/bin/env python3
"""Tool-level caller/impact precision & recall of inspect_symbol against the study's
source-backed ground truth (file sets). No agent involved: one call per task.
usage: fixtures_eval.py <binary> <tasks_main.json>"""
import json, subprocess, sys
binary, tasks_path = sys.argv[1], sys.argv[2]
tasks = json.load(open(tasks_path))
CASES = {
    # task id -> (project, symbol, which sets to union, filter)
    'pcapplusplus_callers': ('Users-tigercosmos-PcapPlusPlus', 'fnvHash', 'callers+tests', lambda f: not f.startswith('3rdParty')),
    'pcapplusplus_impact':  ('Users-tigercosmos-PcapPlusPlus', 'hexStringToByteArray', 'callers+tests+def', lambda f: not f.startswith('3rdParty')),
    'modmesh_callers':      ('Users-tigercosmos-modmesh', 'ReflectionSession.advance', 'callers+tests', lambda f: True),
    'modmesh_impact':       ('Users-tigercosmos-modmesh', 'world_from_screen', 'callers+tests+def', lambda f: True),
    'codecortex_callers':   ('Users-tigercosmos-code-cortex-mcp', 'cbm_validate_shell_arg', 'callers', lambda f: f.startswith('src/')),
    'codecortex_impact':    ('Users-tigercosmos-code-cortex-mcp', 'cbm_json_escape', 'callers+def', lambda f: f.startswith('src/')),
}
rows = []
for t in tasks:
    if t['id'] not in CASES:
        continue
    project, symbol, sets, keep = CASES[t['id']]
    args = {'project': project, 'symbol': symbol, 'source_lines': 0, 'callers_limit': 5}
    p = subprocess.run([binary, 'cli', '--json', 'inspect_symbol', json.dumps(args)], capture_output=True, text=True)
    r = json.loads(json.loads(p.stdout)['content'][0]['text'])
    pred = set()
    for f in r.get('caller_files', []):
        if 'tests' in sets or not f.get('test'):
            pred.add(f['file'])
    if 'def' in sets and 'symbol' in r:
        pred.add(r['symbol']['file'])
        for d in r.get('declared_in', []):
            pred.add(d['file'])
    pred = {f for f in pred if keep(f)}
    gold = set(t['ground_truth'])
    tp = len(pred & gold)
    prec = tp / len(pred) if pred else 0.0
    rec = tp / len(gold) if gold else 0.0
    rows.append((t['id'], symbol, len(pred), len(gold), tp, prec, rec, sorted(gold - pred), sorted(pred - gold)))
print(f"{'task':22} {'symbol':26} pred gold  tp   P     R   missing / extra")
for tid, sym, np_, ng, tp, pr, rc, miss, extra in rows:
    print(f"{tid:22} {sym:26} {np_:4} {ng:4} {tp:3}  {pr:.2f}  {rc:.2f}  miss={miss} extra={extra}")
