#!/usr/bin/env python3
"""Summarize results.jsonl files from run.py: analyze.py results.jsonl [more.jsonl ...]"""
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


def summarize(label, pairs):
    both = [(o, m) for o, m in pairs if o["correct"] and m["correct"]]
    ratios = [m["wall_s"] / o["wall_s"] for o, m in both]
    wins = sum(1 for r in ratios if r < 1)
    losses = sum(1 for r in ratios if r > 1)
    s_off = sum(o["wall_s"] for o, _ in both)
    s_mcp = sum(m["wall_s"] for _, m in both)
    acc_off = sum(o["correct"] for o, _ in pairs)
    acc_mcp = sum(m["correct"] for _, m in pairs)
    print(f"  {label:<12} pairs={len(pairs):>3} both_ok={len(both):>3} acc off={acc_off}/{len(pairs)} "
          f"mcp={acc_mcp}/{len(pairs)}  gm(mcp/off)={gm(ratios):.3f}  "
          f"sum ratio={s_mcp / s_off if s_off else float('nan'):.3f}  "
          f"mcp faster {wins}/{wins + losses}  sign p={sign_test_p(wins, losses):.4f}")


def lenient_ok(r, expected):
    """Answer content right even if wrapped in prose: the trailing lines match."""
    lines = [l.strip().strip("`") for l in (r.get("answer") or "").strip().splitlines() if l.strip().strip("`")]
    want = expected if isinstance(expected, list) else [expected]
    return lines[-len(want):] == want


def main():
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    tasks = {t["id"]: t for t in json.load(open(os.path.join(here, "tasks.json")))}
    rows = []
    for path in sys.argv[1:] or ["results.jsonl"]:
        with open(path) as f:
            rows += [json.loads(l) for l in f if l.strip()]
    lenient_rows = []
    for r in rows:
        lr = dict(r)
        lr["correct"] = lenient_ok(r, tasks[r["task"]]["expected"])
        lenient_rows.append(lr)
    report(rows, "STRICT grading (exact match)")
    report(lenient_rows, "LENIENT grading (trailing lines match; prose preamble allowed)", brief=True)


def report(rows, title, brief=False):
    print(f"\n##### {title}")
    by_model = defaultdict(list)
    for r in rows:
        by_model[r["model"]].append(r)
    for model, rs in by_model.items():
        idx = {(r["task"], r["rep"], r["arm"]): r for r in rs}
        keys = sorted({(r["task"], r["rep"]) for r in rs}, key=lambda k: (
            ["10k", "1m", "100m"].index(idx.get((k[0], k[1], "off"), idx.get((k[0], k[1], "mcp")))["scale"]),
            k[0], k[1]))
        hosts = sorted({r["host"] for r in rs})
        vers = sorted({r["claude_version"] for r in rs})
        print(f"\n=== model {model}  hosts={hosts} claude={vers} sessions={len(rs)}")
        if brief:
            pairs = [(idx[(k[0], k[1], "off")], idx[(k[0], k[1], "mcp")]) for k in keys
                     if (k[0], k[1], "off") in idx and (k[0], k[1], "mcp") in idx]
            summarize("overall", pairs)
            for sc in ("10k", "1m", "100m"):
                summarize(f"scale={sc}", [p for p in pairs if p[0]["scale"] == sc])
            for ar in ("one-shot", "multi-step"):
                summarize(ar, [p for p in pairs if p[0]["archetype"] == ar])
            continue
        print(f"  {'task':<14}{'rep':>3} {'off_wall':>9} {'off_ok':>6} {'mcp_wall':>9} {'mcp_ok':>6} "
              f"{'ratio':>6} {'mcp#':>4} {'off_sh':>6} {'mcp_sh':>6}  errors")
        pairs = []
        for k in keys:
            o, m = idx.get((k[0], k[1], "off")), idx.get((k[0], k[1], "mcp"))
            if not (o and m):
                print(f"  {k[0]:<14}{k[1]:>3} incomplete pair")
                continue
            pairs.append((o, m))
            err = "; ".join(f"{x['arm']}: {x['error']}" for x in (o, m) if x.get("error"))
            print(f"  {k[0]:<14}{k[1]:>3} {o['wall_s']:>9.1f} {str(o['correct']):>6} {m['wall_s']:>9.1f} "
                  f"{str(m['correct']):>6} {m['wall_s'] / o['wall_s']:>6.2f} {m['n_mcp_calls']:>4} "
                  f"{o['n_shell_searches']:>6} {m['n_shell_searches']:>6}  {err}")
        print("  -- wall-time ratio mcp/off over pairs where both arms are correct")
        summarize("overall", pairs)
        for sc in ("10k", "1m", "100m"):
            summarize(f"scale={sc}", [p for p in pairs if p[0]["scale"] == sc])
        for ar in ("one-shot", "multi-step"):
            summarize(ar, [p for p in pairs if p[0]["archetype"] == ar])
        print("  -- cost / turns (all pairs)")
        for arm in ("off", "mcp"):
            xs = [r for r in rs if r["arm"] == arm]
            cost = sum(r.get("cost_usd") or 0 for r in xs)
            turns = [r.get("num_turns") or 0 for r in xs]
            print(f"  {arm:<4} n={len(xs)} total_cost=${cost:.2f} mean_turns={sum(turns) / len(xs):.1f} "
                  f"mean_wall={sum(r['wall_s'] for r in xs) / len(xs):.1f}s "
                  f"errors={sum(1 for r in xs if r.get('error'))}")
        print("  -- adoption")
        for arm in ("off", "mcp"):
            xs = [r for r in rs if r["arm"] == arm]
            n = len(xs)
            used = sum(1 for r in xs if r["n_mcp_calls"] > 0)
            print(f"  {arm:<4} sessions_with_mcp={used}/{n} mean_mcp_calls={sum(r['n_mcp_calls'] for r in xs) / n:.2f} "
                  f"mean_shell_searches={sum(r['n_shell_searches'] for r in xs) / n:.2f} "
                  f"mean_grep_glob_tools={sum(r.get('n_grep_glob_tools', 0) for r in xs) / n:.2f}")
        names = defaultdict(int)
        for r in rs:
            if r["arm"] == "mcp":
                for t in r["tools"]:
                    if t["name"].startswith("mcp__"):
                        names[t["name"]] += 1
        if names:
            print("  mcp tool counts:", dict(sorted(names.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main()
