#!/usr/bin/env python3
"""Size-ladder analysis. usage: ladder_analysis.py <graded.json> [<graded.json> ...]
Per model: arm medians, paired sign tests, per-repo (by LOC) ratios new/off and old/off,
Spearman(log LOC, ratio), per-archetype table, adoption."""
import json, sys, math, statistics as st, collections
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
def spearman(xs, ys):
    def rank(v):
        order = sorted(range(len(v)), key=lambda i: v[i]); r = [0] * len(v)
        for k, i in enumerate(order): r[i] = k + 1
        return r
    rx, ry = rank(xs), rank(ys); n = len(xs)
    if n < 3: return float('nan')
    d2 = sum((a - b) ** 2 for a, b in zip(rx, ry))
    return 1 - 6 * d2 / (n * (n * n - 1))
for path in sys.argv[1:]:
    rows = json.load(open(path))
    model = rows[0]['model']
    print(f"\n===== {model}  runs={len(rows)} tasks={len({r['task'] for r in rows})} reps={len({r['rep'] for r in rows})} effort={rows[0]['effort']}")
    print(f"{'arm':5} {'n':>3} {'wall_med':>8} {'turns':>5} {'tools':>5} {'ctx_med':>8} {'cost_med':>8} {'F1mean':>6} {'F1=1':>7} {'usingMCP':>8}")
    for arm in ARMS:
        rs = [r for r in rows if r['arm'] == arm]
        if not rs: continue
        print(f"{arm:5} {len(rs):3d} {med(r['wall_s'] for r in rs):8.1f} {med(r['num_turns'] for r in rs):5.1f} "
              f"{med(r['n_tool_calls'] for r in rs):5.1f} {med(r['total_ctx_tok'] for r in rs):8.0f} "
              f"{med(r['cost_usd'] for r in rs):8.4f} {st.mean(r['grade']['f1'] for r in rs):6.3f} "
              f"{sum(1 for r in rs if r['grade']['f1'] >= 0.999):3d}/{len(rs):<3d} "
              f"{sum(1 for r in rs if r['n_mcp_calls'] > 0):3d}/{len(rs)}")
    by = collections.defaultdict(dict)
    for r in rows: by[(r['task'], r['rep'])][r['arm']] = r
    for a, b in (('off', 'old'), ('off', 'new'), ('old', 'new')):
        print(f"-- {b} vs {a}:")
        for m in ('wall_s', 'total_ctx_tok', 'cost_usd', 'F1'):
            pairs = []
            for v in by.values():
                if a in v and b in v:
                    x = v[a]['grade']['f1'] if m == 'F1' else v[a].get(m)
                    y = v[b]['grade']['f1'] if m == 'F1' else v[b].get(m)
                    if x is not None and y is not None: pairs.append((x, y))
            if not pairs: continue
            ratios = [p[1] / p[0] for p in pairs if p[0]]
            p, pos, n = sign_p(pairs)
            print(f"   {m:14} {med(p_[0] for p_ in pairs):9.3f} -> {med(p_[1] for p_ in pairs):9.3f}  ratio x{med(ratios):5.2f}  {b}>{a} {pos}/{n}  p={p:.4f}")
    # per repo by LOC
    loc = {}
    for r in rows: loc[r['repo']] = r.get('repo_loc') or loc.get(r['repo'])
    print(f"-- by repository (LOC ascending): median wall off/old/new, ratio old/off, new/off, mean F1 off/old/new")
    xs, ys_new, ys_old = [], [], []
    for repo in sorted(loc, key=lambda k: loc[k] or 0):
        rs = [r for r in rows if r['repo'] == repo]
        cells = {}
        for arm in ARMS:
            s = [r for r in rs if r['arm'] == arm]
            cells[arm] = (med(r['wall_s'] for r in s), st.mean(r['grade']['f1'] for r in s) if s else float('nan'))
        pr_new = [v['new']['wall_s'] / v['off']['wall_s'] for k, v in by.items() if k[0].startswith(rs[0]['task'].split('_')[0]) and 'off' in v and 'new' in v and v['off']['wall_s']]
        pr_old = [v['old']['wall_s'] / v['off']['wall_s'] for k, v in by.items() if k[0].startswith(rs[0]['task'].split('_')[0]) and 'off' in v and 'old' in v and v['off']['wall_s']]
        rn, ro = med(pr_new), med(pr_old)
        if loc[repo]:
            xs.append(math.log(loc[repo])); ys_new.append(rn); ys_old.append(ro)
        print(f"   {repo:14} {loc[repo] or 0:>9,} LOC  {cells['off'][0]:5.1f}/{cells['old'][0]:5.1f}/{cells['new'][0]:5.1f} s  "
              f"old/off x{ro:4.2f}  new/off x{rn:4.2f}  F1 {cells['off'][1]:.2f}/{cells['old'][1]:.2f}/{cells['new'][1]:.2f}")
    print(f"   Spearman(log LOC, old/off wall ratio) = {spearman(xs, ys_old):+.2f};  (log LOC, new/off) = {spearman(xs, ys_new):+.2f}")
    print("-- by archetype: median wall / ctx k / mean F1")
    for a in sorted({r['archetype'] for r in rows}):
        rs = [r for r in rows if r['archetype'] == a]
        cells = []
        for arm in ARMS:
            s = [r for r in rs if r['arm'] == arm]
            cells.append(f"{arm}: {med(r['wall_s'] for r in s):5.1f}s/{med(r['total_ctx_tok'] for r in s)/1000:4.0f}k/{st.mean(r['grade']['f1'] for r in s):.2f}")
        print(f"   {a:10} " + "   ".join(cells))
    print("-- tool mix:")
    for arm in ARMS:
        rs = [r for r in rows if r['arm'] == arm]
        tools = collections.Counter()
        for r in rs: tools.update(r['tools'])
        nm = sum(r['n_mcp_calls'] for r in rs); mb = sum(r['tool_result_bytes'].get('mcp', 0) for r in rs)
        print(f"   {arm:5} mcp calls={nm} ({mb/max(nm,1):.0f} B/call)  " + ', '.join(f"{k.replace('mcp__code-cortex-mcp__','mcp:')}={v}" for k, v in tools.most_common(7)))
