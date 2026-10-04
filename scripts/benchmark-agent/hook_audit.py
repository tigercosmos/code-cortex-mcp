#!/usr/bin/env python3
"""Audit the UserPromptSubmit hook against benchmark task files, without a model.

For each task the hook receives what Claude Code sends before the first turn
(the benchmark preamble plus the question) and its additionalContext is graded
against the task's ground truth.

Per task it reports:
  ms / bytes   hook wall time and payload size
  answer       locate: the gold path:line is present; set tasks: recall of the
               gold files over every path printed; chains: the gold chain is
               printed, in order
  evidence     what a verification grep would show, printed as match lines
               (`path:line: <source>`): locate: the gold path:line has its own
               line; set tasks: every gold file has at least one match line;
               chains: every hop has a call-site line
  graph        set tasks: gold files the graph resolves as callers vs not, and
               non-gold files it lists as callers
  stale        "stale" marks
  leaks        synthetic internal names (__decl_...) in the output
  noise        symbol blocks whose name is not the asked symbol

Usage:
  hook_audit.py --bin <code-cortex-mcp> --cache <CBM_CACHE_DIR> --home <HOME>
                --tasks tasks.json[,more.json] --repos <dir>[,<dir>] [--out audit.json]
                [--only id,id] [--show]

repo_path in a task file may point at another host: the repository is taken
from <repos dir>/<repo_key lowercased> (the n-th --repos entry for the n-th
task file) and the path inside the question is rewritten to it.
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
PATH_RE = re.compile(r"(?<![\w./+-])((?:[\w.@+-]+/)+[\w.@+-]+\.[A-Za-z0-9]+)(?=[:\s,;)\]]|$)")
MATCH_LINE_RE = re.compile(r"((?:[\w.@+-]+/)*[\w.@+-]+\.[A-Za-z0-9]+):(\d+): ")

# Asked symbols for task files that do not carry a "symbol" field.
PREV_SYMBOLS = {
    "ladder_jansson_locate": "error_set",
    "ladder_jansson_callers": "json_object_set_new",
    "ladder_jansson_callchain": "json_load_file -> hashtable_set",
    "ladder_jansson_impact": "json_array_append_new",
    "ladder_lz4_locate": "LZ4F_decompress",
    "ladder_lz4_callers": "LZ4_decompress_safe",
    "ladder_lz4_callchain": "LZ4IO_decompressFilename -> LZ4F_decompress",
    "ladder_lz4_impact": "LZ4_decompress_safe",
    "ladder_elfuse_locate": "hvf_apply_file_overlay",
    "ladder_elfuse_callers": "fd_alloc",
    "ladder_elfuse_callchain": "sys_munmap -> timespec_normalize",
    "ladder_elfuse_impact": "fd_alloc",
    "ladder_pcapplusplus_locate": "getFieldByName",
    "ladder_pcapplusplus_callers": "hexStringToByteArray",
    "ladder_pcapplusplus_callchain": "fromPEMFile -> decodeToByteArray",
    "ladder_cgal_locate": "centroid",
    "ladder_cgal_callers": "sdf_values",
    "ladder_cgal_callchain": "halfspace_intersection_3 -> find_visible_set",
    "ladder_cgal_impact": "polygon_soup_to_polygon_mesh",
    "ladder_opencv_locate": "getMatVector",
    "ladder_opencv_callers": "borderInterpolate",
    "ladder_opencv_callchain": "seamlessClone -> dst",
    "ladder_opencv_impact": "typeToStr",
    "main_pcapplusplus_callers": "fnvHash",
    "main_pcapplusplus_callchain": "fromPEMFile -> decodeToByteArray",
    "main_pcapplusplus_impact": "hexStringToByteArray",
    "main_pcapplusplus_hierarchy": "TLVRecord",
    "main_pcapplusplus_locate": "getNextPacket",
    "main_pcapplusplus_textcfg": "",
    "main_codecortex_callers": "cbm_validate_shell_arg",
    "main_codecortex_callchain": "cbm_discover_ex2 -> cbm_disambiguate_m",
    "main_codecortex_impact": "cbm_json_escape",
    "main_codecortex_hierarchy": "seq_pass_fn",
    "main_codecortex_textcfg": "",
}
OUT_OF_SCOPE_KINDS = {"set_of_names", "exact_string"}
TABLE_RE = re.compile(r"^\s{4}(\S+)  calls=(\d+) other=(\d+)$")
SUMMARY_RE = re.compile(r"^\s{4}(\S+): \d+ matches \(")


def grep_files(repo, name):
    """Files a plain `grep -rlw NAME` over git-tracked text files returns."""
    try:
        out = subprocess.run(["git", "-C", repo, "grep", "-lwI", "-F", "-e", name],
                             capture_output=True, text=True, timeout=60).stdout
    except Exception:  # noqa: BLE001
        return None
    return set(line for line in out.splitlines() if line)


def residual(task, text, repo):
    """Checks derived from the searches agents still ran after the block."""
    kind = task["answer_kind"]
    r = {}
    r["no_more_k"] = not re.search(r"\(\+\d+( more)?\)|more in this file", text)
    if kind == "set_of_paths":
        names = asked_names(task)
        table = set()
        for line in text.splitlines():
            m = TABLE_RE.match(line)
            if m:
                table.add(m.group(1))
        want = set()
        for n in names:
            g = grep_files(repo, n)
            if g is not None:
                want |= g
        r["table_eq_grep"] = bool(table) and table == want
        r["table_missing"] = sorted(want - table)[:10]
        r["table_extra"] = sorted(table - want)[:10]
        r["alias_line"] = "aliases:" in text
    elif kind == "path_and_line":
        r["exact_count_line"] = "exact-name definitions:" in text
    elif kind in ("ordered_names", "ordered_list"):
        r["shortest_line"] = "shortest:" in text
    return r


def bare(name):
    return re.split(r"::|\.|#", name.strip())[-1]


def asked_names(task):
    sym = task.get("symbol", PREV_SYMBOLS.get(task["id"], ""))
    return {bare(part) for part in sym.split("->") if part.strip()}


def run_hook(binary, env, cwd, prompt):
    payload = json.dumps({"hook_event_name": "UserPromptSubmit", "cwd": cwd, "prompt": prompt,
                          "session_id": "audit"})
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


def chain_evidence(text):
    """Hop call-site lines printed under a chain: '    A -> B: path:line: src'."""
    hops = []
    for line in text.splitlines():
        m = re.match(r"\s+(\S+) -> (\S+): " + MATCH_LINE_RE.pattern, line)
        if m:
            hops.append((bare(m.group(1)), bare(m.group(2))))
    return hops


def symbol_blocks(text):
    names = []
    for line in text.splitlines():
        m = re.match(r"- (?!call chain)((?:[^:\s(]|::)+)[:\s]", line)
        if m:
            names.append(m.group(1))
    return names


def sections(text):
    """Map each printed path to the section it appears under."""
    out = {"located": set(), "graph": set(), "other": set(), "mention": set(), "evidence": set()}
    section = "graph"
    cur_dir = ""
    for line in text.splitlines():
        s = line.strip()
        low = s.lower()
        # Directory-grouped listings: "    dir/" then "      file:line: src".
        if re.match(r"^\s{4}\S+/$", line):
            cur_dir = s if s != "./" else ""
            continue
        g = re.match(r"^\s{6}([^\s:]+?)(?::(\d+): | \(\d+ matches\))", line)
        if g and cur_dir is not None and "/" not in g.group(1):
            path = cur_dir + g.group(1)
            out["mention" if section == "mention" else section].add(path)
            if g.group(2):
                out["evidence"].add(path)
            continue
        if not line.startswith("      "):
            cur_dir = ""
        if line.startswith("- "):
            section = "located" if " at " in line or "(" in line else "graph"
        elif low.startswith(("definition", "declaration", "declared in")):
            section = "located"
        elif low.startswith(("calls (graph", "caller files")):
            section = "graph"
        elif low.startswith("other whole-word") or "also mention" in low:
            section = "mention"
        for m in MATCH_LINE_RE.finditer(line):
            out["evidence"].add(m.group(1))
        for rx in (TABLE_RE, SUMMARY_RE):
            mm = rx.match(line)
            if mm:
                out["evidence"].add(mm.group(1))
                out[section].add(mm.group(1))
        found = set(PATH_RE.findall(line))
        key = "mention" if "also mention" in low else section
        out[key].update(found)
        if low.startswith(("caller files", "declared in")):
            # header lines that list files inline (older format)
            pass
    return out


def grade(task, text):
    kind = task["answer_kind"]
    gold = task["ground_truth"]
    res = {"scored": kind not in OUT_OF_SCOPE_KINDS}
    if kind == "path_and_line":
        hit = False
        evid = False
        alts = []
        for a in task.get("alternates", []):
            alts.extend(a if isinstance(a, list) else [a])
        for g in gold + alts:
            for m in re.finditer(re.escape(g), text):
                end = m.end()
                if end >= len(text) or not text[end].isdigit():
                    hit = True
                    if text[end:end + 2] == ": ":
                        evid = True
        res["ok"] = hit
        res["evidence"] = evid
    elif kind == "set_of_paths":
        gold_set = set(gold)
        sec = sections(text)
        union = sec["located"] | sec["graph"] | sec["mention"] | sec["other"]
        res["recall"] = len(gold_set & union) / len(gold_set) if gold_set else 1.0
        res["graph_recall"] = (len(gold_set & (sec["graph"] | sec["located"])) / len(gold_set)
                               if gold_set else 1.0)
        res["graph_extra"] = sorted(sec["graph"] - gold_set)
        res["missing"] = sorted(gold_set - union)
        res["graph_missing"] = sorted(gold_set - sec["graph"] - sec["located"])
        res["mention_extra"] = sorted(sec["mention"] - gold_set)
        res["evidence_missing"] = sorted(gold_set - sec["evidence"])
        res["ok"] = res["recall"] == 1.0
        res["evidence"] = not res["evidence_missing"]
    elif kind in ("ordered_names", "ordered_list"):
        want = [bare(g) for g in gold]
        res["chains"] = chain_lines(text)
        res["ok"] = any(c == want for c in res["chains"])
        hops = set(chain_evidence(text))
        res["evidence"] = res["ok"] and all((want[i], want[i + 1]) in hops
                                            for i in range(len(want) - 1))
    else:
        res["ok"] = False
        res["evidence"] = False
    res["stale"] = len(re.findall(r"\bstale\b", text))
    res["leaks"] = len(re.findall(r"__decl_", text))
    asked = asked_names(task)
    res["noise"] = [n for n in symbol_blocks(text) if asked and bare(n) not in asked]
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
    task_files = args.tasks.split(",")
    repo_dirs = args.repos.split(",")
    tasks = []
    for i, tf in enumerate(task_files):
        for t in json.load(open(tf)):
            t["_repos"] = os.path.expanduser(repo_dirs[min(i, len(repo_dirs) - 1)])
            tasks.append(t)
    if args.only:
        keep = set(args.only.split(","))
        tasks = [t for t in tasks if t["id"] in keep]
    env = dict(os.environ, HOME=args.home, CBM_CACHE_DIR=args.cache)
    rows = []
    for t in tasks:
        repo = os.path.join(t["_repos"], t["repo_key"].lower())
        question = t["question"].replace(t["repo_path"], repo)
        ms, nbytes, text = run_hook(args.bin, env, repo, PREAMBLE + question)
        g = grade(t, text)
        g["residual"] = residual(t, text, repo) if g["scored"] else {}
        rows.append({"id": t["id"], "kind": t["answer_kind"], "ms": round(ms, 1), "bytes": nbytes,
                     **g, "text": text})
        if args.show:
            print(f"===== {t['id']} ({ms:.0f} ms, {nbytes} B)\n{text}\n")

    print(f"{'task':30} {'ms':>7} {'bytes':>6}  {'answer':24} {'evid':5} {'resid':6} stale leak "
          "noise")
    for r in rows:
        if not r["scored"]:
            res = "out of scope"
        elif r["kind"] == "set_of_paths":
            res = f"recall {r['recall']:.2f} graph {r['graph_recall']:.2f} +{len(r['graph_extra'])}"
        else:
            res = "OK" if r["ok"] else "miss"
        ev = "-" if not r["scored"] else ("yes" if r["evidence"] else "NO")
        rs = r.get("residual", {})
        checks = [v for k, v in rs.items() if isinstance(v, bool)]
        resid = "-" if not checks else ("ok" if all(checks) else
                                        ",".join(k for k, v in rs.items() if v is False)[:6])
        print(f"{r['id']:30} {r['ms']:7.1f} {r['bytes']:6}  {res:24} {ev:5} {resid:6} "
              f"{r['stale']:5} {r['leaks']:4} {','.join(r['noise'])[:50]}")
    times = sorted(r["ms"] for r in rows)
    p95 = times[min(len(times) - 1, int(round(0.95 * (len(times) - 1))))] if times else 0
    by = {}
    for r in rows:
        k = r["kind"]
        by.setdefault(k, []).append(r)
    print()
    for k, rs in sorted(by.items()):
        rs_s = [r for r in rs if r["scored"]]
        print(f"{k:14} answer ok {sum(r['ok'] for r in rs_s)}/{len(rs_s)}  "
              f"evidence complete {sum(r['evidence'] for r in rs_s)}/{len(rs_s)}"
              + (f"  ({len(rs) - len(rs_s)} out of scope)" if len(rs) != len(rs_s) else ""))
    scored = [r for r in rows if r["scored"]]
    for key in ("table_eq_grep", "no_more_k", "alias_line", "exact_count_line", "shortest_line"):
        vals = [r["residual"][key] for r in scored if key in r.get("residual", {})]
        if vals:
            print(f"residual {key:17} {sum(vals)}/{len(vals)}")
    for r in scored:
        rs = r.get("residual", {})
        if rs.get("table_eq_grep") is False:
            print(f"  {r['id']}: table missing {rs.get('table_missing')} extra {rs.get('table_extra')}")
    print(f"evidence complete {sum(r['evidence'] for r in scored)}/{len(scored)}, "
          f"noise blocks {sum(len(r['noise']) for r in rows)}, stale marks "
          f"{sum(r['stale'] for r in rows)}, leaked internal names {sum(r['leaks'] for r in rows)}")
    if times:
        print(f"hook ms: median {statistics.median(times):.1f}, p95 {p95:.1f}, max {times[-1]:.1f}")
    print("\ngraph vs gold (set tasks):")
    for r in rows:
        if r["kind"] != "set_of_paths":
            continue
        print(f"  {r['id']}: not graph-resolved {r['graph_missing']} | graph extra "
              f"{r['graph_extra']} | no match line {r['evidence_missing']} | non-gold mentions "
              f"{len(r['mention_extra'])}")
    if args.out:
        json.dump(rows, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
