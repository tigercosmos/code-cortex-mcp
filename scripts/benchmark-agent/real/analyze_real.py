#!/usr/bin/env python3
"""Summarize run_real.py results: analyze_real.py [--lenient] results.jsonl [more.jsonl ...]

Per model: paired wall ratio mcp/off (geometric mean, sum ratio, sign test) over
pairs where both arms are correct, overall / per language+repo / per archetype;
accuracy and mean F1 per arm; turns, cost, shell searches, mcp calls; hook events.
"""
import json
import math
import sys
from collections import defaultdict


def sign_test_p(wins, losses):
    """Exact two-sided binomial sign test (ties dropped)."""
    n = wins + losses
    if n == 0:
        return float("nan")
    k = min(wins, losses)
    p = sum(math.comb(n, i) for i in range(0, k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def gm(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else float("nan")


MODE = {"exact": False}


def ok(r, lenient):
    if MODE["exact"]:
        return bool(r.get("exact_strict", r.get("correct")))
    return bool(r.get("correct_lenient") if lenient else r.get("correct"))


def summarize(label, pairs, lenient):
    if not pairs:
        return
    both = [(o, m) for o, m in pairs if ok(o, lenient) and ok(m, lenient)]
    ratios = [m["wall_s"] / o["wall_s"] for o, m in both]
    wins = sum(1 for r in ratios if r < 1)
    losses = sum(1 for r in ratios if r > 1)
    s_off = sum(o["wall_s"] for o, _ in both)
    s_mcp = sum(m["wall_s"] for _, m in both)
    acc_off = sum(ok(o, lenient) for o, _ in pairs)
    acc_mcp = sum(ok(m, lenient) for _, m in pairs)
    f1_off = mean([o.get("f1") for o, _ in pairs])
    f1_mcp = mean([m.get("f1") for _, m in pairs])
    print(f"  {label:<26} pairs={len(pairs):>3} both_ok={len(both):>3} acc off={acc_off}/{len(pairs)} "
          f"mcp={acc_mcp}/{len(pairs)} F1 off={f1_off:.3f} mcp={f1_mcp:.3f}  gm(mcp/off)={gm(ratios):.3f}  "
          f"sum={s_mcp / s_off if s_off else float('nan'):.3f}  mcp faster {wins}/{wins + losses}  "
          f"p={sign_test_p(wins, losses):.4f}")


def arm_stats(rs, arm):
    xs = [r for r in rs if r["arm"] == arm]
    if not xs:
        return
    n = len(xs)
    cost = sum(r.get("cost_usd") or 0 for r in xs)
    print(f"  {arm:<4} n={n} acc={sum(bool(r.get('correct')) for r in xs)}/{n} "
          f"acc_lenient={sum(bool(r.get('correct_lenient')) for r in xs)}/{n} meanF1={mean([r.get('f1') for r in xs]):.3f} "
          f"cost=${cost:.2f} mean_turns={mean([r.get('num_turns') or 0 for r in xs]):.1f} "
          f"mean_wall={mean([r['wall_s'] for r in xs]):.1f}s errors={sum(1 for r in xs if r.get('error'))}")
    used = sum(1 for r in xs if r["n_mcp_calls"] > 0)
    print(f"       sessions_with_mcp={used}/{n} mean_mcp_calls={mean([r['n_mcp_calls'] for r in xs]):.2f} "
          f"mean_shell_searches={mean([r['n_shell_searches'] for r in xs]):.2f} "
          f"mean_grep_glob_tools={mean([r.get('n_grep_glob_tools', 0) for r in xs]):.2f} "
          f"mean_reads={mean([r.get('n_read_tools', 0) for r in xs]):.2f} "
          f"mean_tool_calls={mean([r['n_tool_calls'] for r in xs]):.2f}")
    ev = defaultdict(int)
    for r in xs:
        for h in r.get("hook_events") or []:
            ev[h["hook_event"]] += 1
    print(f"       hook_response events: {dict(ev)}  "
          f"UserPromptSubmit context present={sum(1 for r in xs if r.get('ups_context_present'))}/{n}  "
          f"SessionStart context present={sum(1 for r in xs if r.get('sessionstart_context_present'))}/{n}")


def report(rows, lenient):
    title = ("EXACT" if MODE["exact"] else "LENIENT (correct_lenient)") if lenient else "STRICT (correct)"
    print(f"\n##### {title}")
    by_model = defaultdict(list)
    for r in rows:
        by_model[r["model"]].append(r)
    for model, rs in by_model.items():
        idx = {(r["task"], r["rep"], r["arm"]): r for r in rs}
        keys = sorted({(r["task"], r["rep"]) for r in rs})
        print(f"\n=== model {model}  hosts={sorted({r['host'] for r in rs})} "
              f"claude={sorted({r['claude_version'] for r in rs})} sessions={len(rs)}")
        print(f"  {'task':<24}{'rep':>3} {'off_wall':>8} {'off':>5} {'offF1':>5} {'mcp_wall':>8} {'mcp':>5} "
              f"{'mcpF1':>5} {'ratio':>6} {'mcp#':>4} {'offsh':>5} {'mcpsh':>5} {'ups':>4}  errors")
        pairs = []
        for k in keys:
            o, m = idx.get((k[0], k[1], "off")), idx.get((k[0], k[1], "mcp"))
            if not (o and m):
                print(f"  {k[0]:<24}{k[1]:>3} incomplete pair")
                continue
            pairs.append((o, m))
            err = "; ".join(f"{x['arm']}: {x['error']}" for x in (o, m) if x.get("error"))
            print(f"  {k[0]:<24}{k[1]:>3} {o['wall_s']:>8.1f} {str(ok(o, lenient))[0]:>5} {o.get('f1', 0):>5.2f} "
                  f"{m['wall_s']:>8.1f} {str(ok(m, lenient))[0]:>5} {m.get('f1', 0):>5.2f} "
                  f"{m['wall_s'] / o['wall_s']:>6.2f} {m['n_mcp_calls']:>4} {o['n_shell_searches']:>5} "
                  f"{m['n_shell_searches']:>5} {str(m.get('ups_context_present'))[0]:>4}  {err}")
        print("  -- wall-time ratio mcp/off over pairs where both arms are correct")
        summarize("overall", pairs, lenient)
        for lang in sorted({o["language"] for o, _ in pairs}):
            for repo in sorted({o["repo_key"] for o, _ in pairs if o["language"] == lang}):
                summarize(f"{lang}/{repo}", [p for p in pairs if p[0]["repo_key"] == repo], lenient)
        arches = ["locate", "callers", "callchain", "impact"]
        arches += sorted({o["archetype"] for o, _ in pairs} - set(arches))
        for ar in arches:
            summarize(f"archetype={ar}", [p for p in pairs if p[0]["archetype"] == ar], lenient)
        if lenient:
            continue
        print("  -- per arm (all sessions)")
        for arm in ("off", "mcp"):
            arm_stats(rs, arm)
        names = defaultdict(int)
        for r in rs:
            if r["arm"] == "mcp":
                for t in r["tools"]:
                    if t["name"].startswith("mcp__"):
                        names[t["name"].split("__")[-1]] += 1
        if names:
            print("  mcp tool counts:", dict(sorted(names.items(), key=lambda x: -x[1])))


def main():
    paths = [p for p in sys.argv[1:] if not p.startswith("--")] or ["results.jsonl"]
    rows = []
    for path in paths:
        with open(path) as f:
            rows += [json.loads(l) for l in f if l.strip()]
    report(rows, lenient=False)
    report(rows, lenient=True)
    if any("exact_strict" in r for r in rows):
        # tasks_prev.json rows: grade.py scores ordered_list order-insensitively and path_and_line
        # with +-3 lines; this view requires the exact ordered list / exact path:line instead.
        MODE["exact"] = True
        print("\n##### EXACT view below: exact_strict where present (prev ordered_list/path_and_line), else correct")
        report(rows, lenient=True)


if __name__ == "__main__":
    main()
