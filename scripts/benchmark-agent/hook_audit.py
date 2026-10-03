#!/usr/bin/env python3
"""Audit the UserPromptSubmit hook against the real-repo benchmark tasks.

No model is involved: for each task, the hook receives exactly what Claude Code
would send before the first turn (the benchmark preamble plus the question),
and its additionalContext is graded against the task's ground truth.

Per task it reports:
  ms / bytes    hook wall time and payload size
  locate        the gold path:line is present
  callers/impact
                recall of the gold files over the union of every path printed;
                graph recall (paths outside "also mention" lines only); and the
                number of non-gold files the graph lists as callers (precision)
  callchain     the gold chain (bare names, in order) is printed
  stale         number of "stale" marks
  noise         symbol blocks whose name is not the asked symbol

Usage:
  hook_audit.py --bin <code-cortex-mcp> --cache <CBM_CACHE_DIR> --home <HOME>
                --tasks tasks_real.json --repos ~/bench [--out audit.json]
                [--only id,id] [--show]

repo_path in the task file may point at another host; the repository is taken
from --repos/<repo_key lowercased> and the path inside the question is rewritten.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import time

PREAMBLE = ("Work only in this repository. Read only: do not edit files, build, or run tests. "
            "Do not use the network. Complete the task without asking questions.\n\n")
PATH_RE = re.compile(r"(?<![\w./-])((?:[\w.@+-]+/)+[\w.@+-]+\.[A-Za-z0-9]+)(?=[:\s,;)\]]|$)")


def bare(name):
    return re.split(r"::|\.|#", name.strip())[-1]


def asked_names(task):
    return {bare(part) for part in task.get("symbol", "").split("->") if part.strip()}


def run_hook(binary, env, cwd, prompt):
    payload = json.dumps({"hook_event_name": "UserPromptSubmit", "cwd": cwd, "prompt": prompt})
    t0 = time.perf_counter()
    proc = subprocess.run([binary, "hook-augment"], input=payload, capture_output=True, text=True,
                          env=env, timeout=30)
    ms = (time.perf_counter() - t0) * 1000.0
    out = proc.stdout.strip()
    text = ""
    if out:
        try:
            text = json.loads(out)["hookSpecificOutput"]["additionalContext"]
        except Exception:  # noqa: BLE001
            text = "<unparseable output>"
    return ms, len(out.encode()), text


def chain_lines(text):
    chains = []
    for line in text.splitlines():
        if "call chain" not in line or ":" not in line:
            continue
        body = line.split("): ", 1)[1] if "): " in line else line.split(": ", 1)[1]
        hops = [bare(re.sub(r"\s*\(.*$", "", h.strip()).split(" [")[0]) for h in body.split(" -> ")]
        chains.append(hops)
    return chains


def symbol_blocks(text):
    names = []
    for line in text.splitlines():
        m = re.match(r"- (?!call chain)([^:\s][^\s]*?): ", line)
        if m:
            names.append(m.group(1))
    return names


def grade(task, text):
    kind = task["answer_kind"]
    gold = task["ground_truth"]
    res = {}
    if kind == "path_and_line":
        hit = False
        for g in gold + task.get("alternates", []):
            for m in re.finditer(re.escape(g), text):
                end = m.end()
                if end >= len(text) or not text[end].isdigit():
                    hit = True
        res["ok"] = hit
    elif kind == "set_of_paths":
        gold_set = set(gold)
        # graph: files the graph lists as callers ("caller files" lines, or any
        # path outside the location/declaration/also-mention lines for older
        # output formats); located: the definition and declaration lines.
        graph, mention, located = set(), set(), set()
        for line in text.splitlines():
            found = set(PATH_RE.findall(line))
            if "also mention" in line:
                mention.update(found)
            elif line.startswith("- ") and " at " in line or "declared in:" in line:
                located.update(found)
            else:
                graph.update(found)
        union = graph | mention | located
        graph_or_located = graph | located
        res["recall"] = len(gold_set & union) / len(gold_set) if gold_set else 1.0
        res["graph_recall"] = (len(gold_set & graph_or_located) / len(gold_set)
                               if gold_set else 1.0)
        res["graph_extra"] = sorted(graph - gold_set)
        res["missing"] = sorted(gold_set - union)
        res["graph_missing"] = sorted(gold_set - graph_or_located)
        res["located_extra"] = sorted(located - gold_set)
        res["mention_extra"] = sorted(mention - gold_set)
        res["ok"] = res["recall"] == 1.0
    elif kind == "ordered_names":
        want = [bare(g) for g in gold]
        res["ok"] = any(c == want for c in chain_lines(text))
        res["chains"] = chain_lines(text)
    res["stale"] = text.count("stale")
    asked = asked_names(task)
    res["noise"] = [n for n in symbol_blocks(text) if bare(n) not in asked]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("--tasks", required=True)
    ap.add_argument("--repos", required=True)
    ap.add_argument("--out")
    ap.add_argument("--only")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()
    tasks = json.load(open(args.tasks))
    if args.only:
        keep = set(args.only.split(","))
        tasks = [t for t in tasks if t["id"] in keep]
    env = dict(os.environ, HOME=args.home, CBM_CACHE_DIR=args.cache)
    rows = []
    for t in tasks:
        repo = os.path.join(os.path.expanduser(args.repos), t["repo_key"].lower())
        question = t["question"].replace(t["repo_path"], repo)
        ms, nbytes, text = run_hook(args.bin, env, repo, PREAMBLE + question)
        g = grade(t, text)
        rows.append({"id": t["id"], "kind": t["answer_kind"], "ms": round(ms, 1), "bytes": nbytes,
                     **g, "text": text})
        if args.show:
            print(f"===== {t['id']} ({ms:.0f} ms, {nbytes} B)\n{text}\n")

    print(f"{'task':26} {'ms':>7} {'bytes':>6}  {'result':28} stale noise")
    for r in rows:
        if r["kind"] == "set_of_paths":
            res = (f"recall {r['recall']:.2f} graph {r['graph_recall']:.2f} "
                   f"+{len(r['graph_extra'])}")
        else:
            res = "OK" if r["ok"] else "miss"
        print(f"{r['id']:26} {r['ms']:7.1f} {r['bytes']:6}  {res:28} {r['stale']:5} "
              f"{','.join(r['noise'])[:60]}")
    times = sorted(r["ms"] for r in rows)
    p95 = times[min(len(times) - 1, int(round(0.95 * (len(times) - 1))))] if times else 0
    by = {}
    for r in rows:
        k = r["id"].split("_", 1)[1]
        by.setdefault(k, []).append(r)
    print()
    for k, rs in sorted(by.items()):
        print(f"{k:10} ok {sum(r['ok'] for r in rs)}/{len(rs)}")
    print(f"noise blocks {sum(len(r['noise']) for r in rows)}, stale marks "
          f"{sum(r['stale'] for r in rows)}, median {statistics.median(times):.1f} ms, "
          f"p95 {p95:.1f} ms, max {times[-1]:.1f} ms" if times else "no tasks")
    print("\ngraph vs gold (set tasks):")
    for r in rows:
        if r["kind"] != "set_of_paths":
            continue
        print(f"  {r['id']}: graph missing {r['graph_missing']} | graph extra {r['graph_extra']}"
              f" | definition/declaration not in gold {r['located_extra']}"
              f" | mention extra {len(r['mention_extra'])}")
    if args.out:
        json.dump(rows, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
