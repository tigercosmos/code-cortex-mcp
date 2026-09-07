#!/usr/bin/env python3
"""Pooled paired comparison across graded files (pairs keyed by model, task, rep)."""
import json, sys, math, statistics as st, collections
rows = []
for p in sys.argv[1:]:
    rows += json.load(open(p))
by = collections.defaultdict(dict)
for r in rows: by[(r['model'], r['task'], r['rep'])][r['arm']] = r
def med(xs): return st.median(xs) if xs else float('nan')
def sign_p(pairs):
    d = [b - a for a, b in pairs if b != a]; n = len(d)
    if n == 0: return 1.0, 0, 0
    pos = sum(1 for x in d if x > 0); k = min(pos, n - pos)
    return min(sum(math.comb(n, i) for i in range(k + 1)) / (2 ** n) * 2, 1.0), pos, n
print(f"pooled runs={len(rows)} pairs={len(by)}")
for a, b in (('off', 'new'), ('old', 'new'), ('off', 'old')):
    print(f"-- {b} vs {a}:")
    for m in ('wall_s', 'total_ctx_tok', 'cost_usd', 'n_builtin_search'):
        pairs = [(v[a][m], v[b][m]) for v in by.values() if a in v and b in v and v[a].get(m) is not None and v[b].get(m) is not None]
        ratios = [y / x for x, y in pairs if x]
        p, pos, n = sign_p(pairs)
        print(f"   {m:16} {med([x for x,_ in pairs]):9.3f} -> {med([y for _,y in pairs]):9.3f}  median ratio x{med(ratios):5.2f}  {b}>{a} {pos}/{n}  p={p:.4f}")
for arm in ('off', 'old', 'new'):
    rs = [r for r in rows if r['arm'] == arm]
    print(f"{arm}: F1=1 in {sum(1 for r in rs if r['grade']['f1'] >= 0.999)}/{len(rs)}, mean F1 {st.mean(r['grade']['f1'] for r in rs):.3f}, sessions using MCP {sum(1 for r in rs if r['n_mcp_calls'])}/{len(rs)}, shell searches {sum(r['n_builtin_search'] for r in rs)}")
