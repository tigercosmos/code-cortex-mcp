#!/usr/bin/env python3
"""Paired analysis of off / old / new arms. usage: analysis.py <graded.json>"""
import json, sys, math, statistics as st, collections
rows = json.load(open(sys.argv[1]))
ARMS = ['off', 'old', 'new']
def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else float('nan')
def sign_p(pairs):
    d = [b - a for a, b in pairs if a is not None and b is not None and b != a]
    n = len(d)
    if n == 0: return 1.0, 0, 0
    pos = sum(1 for x in d if x > 0); k = min(pos, n - pos)
    return min(sum(math.comb(n, i) for i in range(k + 1)) / (2 ** n) * 2, 1.0), pos, n
print(f"runs={len(rows)} tasks={len({r['task'] for r in rows})} reps={len({r['rep'] for r in rows})} model={rows[0]['model']} effort={rows[0]['effort']}")
print(f"\n{'arm':5} {'n':>3} {'wall_med':>8} {'turns':>5} {'tools':>5} {'mcp':>4} {'ctx_med':>8} {'cost_med':>8} {'F1mean':>6} {'F1med':>5} {'F1=1':>5}")
for arm in ARMS:
    rs = [r for r in rows if r['arm'] == arm]
    if not rs: continue
    print(f"{arm:5} {len(rs):3d} {med(r['wall_s'] for r in rs):8.1f} {med(r['num_turns'] for r in rs):5.1f} "
          f"{med(r['n_tool_calls'] for r in rs):5.1f} {med(r['n_mcp_calls'] for r in rs):4.1f} "
          f"{med(r['total_ctx_tok'] for r in rs):8.0f} {med(r['cost_usd'] for r in rs):8.4f} "
          f"{st.mean(r['grade']['f1'] for r in rs):6.3f} {med(r['grade']['f1'] for r in rs):5.2f} "
          f"{sum(1 for r in rs if r['grade']['f1'] >= 0.999):3d}/{len(rs)}")
by = collections.defaultdict(dict)
for r in rows: by[(r['task'], r['rep'])][r['arm']] = r
def paired(a, b):
    print(f"\n-- {b} vs {a} (paired by task,rep; median ratio, exact sign test):")
    for m in ('wall_s', 'num_turns', 'n_tool_calls', 'total_ctx_tok', 'cost_usd', 'F1'):
        pairs = []
        for v in by.values():
            if a in v and b in v:
                x = v[a]['grade']['f1'] if m == 'F1' else v[a].get(m)
                y = v[b]['grade']['f1'] if m == 'F1' else v[b].get(m)
                if x is not None and y is not None: pairs.append((x, y))
        if not pairs: continue
        ma, mb = med(p[0] for p in pairs), med(p[1] for p in pairs)
        ratios = [p[1] / p[0] for p in pairs if p[0]]
        p, pos, n = sign_p(pairs)
        print(f"   {m:14} {ma:9.3f} -> {mb:9.3f}  median ratio x{med(ratios):5.2f}  {b}>{a} in {pos}/{n}  p={p:.4f}")
paired('off', 'old'); paired('off', 'new'); paired('old', 'new')
print("\n-- by archetype: median wall s / ctx k-tokens / mean F1")
for a in sorted({r.get('archetype') or '?' for r in rows}):
    rs = [r for r in rows if (r.get('archetype') or '?') == a]
    cells = []
    for arm in ARMS:
        s = [r for r in rs if r['arm'] == arm]
        cells.append(f"{arm}: {med(r['wall_s'] for r in s):5.1f}s /{med(r['total_ctx_tok'] for r in s)/1000:4.0f}k /{st.mean(r['grade']['f1'] for r in s):.2f}" if s else f"{arm}: -")
    print(f"   {a:10} " + "   ".join(cells))
print("\n-- by repo: median wall s / mean F1")
for repo in sorted({r['repo'] for r in rows}):
    rs = [r for r in rows if r['repo'] == repo]
    cells = []
    for arm in ARMS:
        s = [r for r in rs if r['arm'] == arm]
        cells.append(f"{arm}: {med(r['wall_s'] for r in s):5.1f}s /{st.mean(r['grade']['f1'] for r in s):.2f}" if s else f"{arm}: -")
    print(f"   {repo:16} " + "   ".join(cells))
print("\n-- adoption / payload / tool mix:")
for arm in ARMS:
    rs = [r for r in rows if r['arm'] == arm]
    if not rs: continue
    used = sum(1 for r in rs if r['n_mcp_calls'] > 0)
    mcpb = sum(r['tool_result_bytes'].get('mcp', 0) for r in rs); nm = sum(r['n_mcp_calls'] for r in rs)
    bib = sum(r['tool_result_bytes'].get('builtin', 0) for r in rs)
    nb = sum(v for r in rs for k, v in r['tools'].items() if k in ('Bash', 'Read', 'Grep', 'Glob'))
    tools = collections.Counter()
    for r in rs: tools.update(r['tools'])
    top = ', '.join(f"{k.replace('mcp__code-cortex-mcp__','mcp:')}={v}" for k, v in tools.most_common(8))
    print(f"   {arm:5} sessions using MCP {used}/{len(rs)}; mcp {mcpb/max(nm,1):6.0f} B/call (n={nm}); builtin {bib/max(nb,1):6.0f} B/call (n={nb})")
    print(f"         tools: {top}")
print("\n-- worst F1 per arm (task, rep, f1):")
for arm in ARMS:
    rs = sorted([r for r in rows if r['arm'] == arm], key=lambda r: r['grade']['f1'])[:4]
    print(f"   {arm:5} " + "; ".join(f"{r['task']} r{r['rep']} {r['grade']['f1']:.2f}" for r in rs))
