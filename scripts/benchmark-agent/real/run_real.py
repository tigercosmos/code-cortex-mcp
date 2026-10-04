#!/usr/bin/env python3
"""Agent-level A/B benchmark on real repositories: Claude Code with code-cortex-mcp (mcp) vs nothing (off).

Copy of ~/bench-agent-20261003/run.py adapted to real-repo tasks (tasks_real.json):
each task's repo_path is the session cwd; grading follows the earlier real-repo
trials (path_and_line exact with alternates, set_of_paths P/R/F1, ordered_names
exact ordered list with a lenient variant).

Usage:
  BENCH_BIN=<bin> run_real.py --model claude-opus-5-5 --tasks-file tasks_real.json --out <outdir>
         [--tasks id1,id2,...] [--reps 1] [--timeout 900] [--arms off,mcp]

Also: run_real.py --regrade <results.jsonl> --tasks-file tasks_real.json   (re-grades rows in place
into <results>.regraded.jsonl, e.g. after an oracle fix).

Sessions run strictly serially. Rows are appended to <out>/results.jsonl; a row
already present (same model, arm, task, rep) is skipped, so a rerun resumes.
"""
import argparse
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import time

W = os.path.expanduser("~/bench-agent-20261003")
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("BENCH_BIN") or "/home/anchi/feat-b-20261003/build/c/code-cortex-mcp"
CACHE = os.path.join(W, "cache")
CLAUDE = os.path.expanduser("~/.local/bin/claude")
PREFIX = ("Work only in this repository. Read only: do not edit files, build, or run tests. "
          "Do not use the network. Complete the task without asking questions.\n\n")
SEARCH_RE = re.compile(r"(^|[\s|;&(`$])(grep|egrep|fgrep|rg|find|ag|git grep)(\s|$)")


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"ERR {e}"


def file_sha256(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError as e:
        return f"ERR {e}"


def mcp_config(arm):
    if arm == "off":
        return json.dumps({"mcpServers": {}})
    return json.dumps({"mcpServers": {"code-cortex-mcp": {
        "command": BIN, "env": {"CBM_CACHE_DIR": CACHE}}}})


# ---------------------------------------------------------------- grading
# Normalisation ported from the earlier trials' grade.py (norm_path / norm_name /
# path-only-line preference), widened to every source extension.

def _clean(s):
    return s.strip().strip('`"\'').rstrip(',;').lstrip('-*+ ').strip()


def norm_path(s, repo_path):
    s = _clean(s)
    s = re.sub(r'^\./', '', s)
    rp = (repo_path or '').rstrip('/')
    if rp and s.startswith(rp + '/'):
        s = s[len(rp) + 1:]
    return s.strip()


def norm_path_line(s, repo_path):
    """'./a/b.c:12' or '/abs/repo/a/b.c:12' -> 'a/b.c:12' (keeps the line)."""
    s = norm_path(s, repo_path)
    m = re.match(r'^(.*?):(\d+)(?::\d+)?\s*$', s)
    return f"{m.group(1)}:{int(m.group(2))}" if m else s


def norm_name(s):
    s = s.strip().strip('`"\'').rstrip(',;.').lstrip('-*+ ').strip()
    s = re.sub(r'^\d+[.)]\s*', '', s)          # numbering
    s = s.strip('`"\'')
    s = re.sub(r'\(.*$', '', s)                  # drop arg list
    if ' ' not in s:                              # drop qualification Class::m, mod.f, Class#m
        s = re.split(r'::|\.|#', s)[-1] if re.search(r'::|\.|#', s) else s
    return s.strip()


PATH_LINE_RE = re.compile(r'^[\w./+@-]+\.[A-Za-z0-9_]{1,6}$')
PATH_RE = re.compile(r'[\w./+@-]*[\w+-]+\.[A-Za-z][A-Za-z0-9_]{0,5}\b')
IDENT_RE = re.compile(r'^[A-Za-z_$][\w$]*[!?=]?$')


def answer_lines(text):
    return [l for l in (text or '').splitlines() if l.strip() and not l.strip().startswith('```')]


def extract_paths(text, repo_path):
    """Prefer lines that are ONLY a path (the requested format); fall back to a regex
    over the whole reply only if there are none (as grade.py did)."""
    strict = []
    for ln in answer_lines(text):
        c = norm_path(ln, repo_path)
        c = re.sub(r':\d+.*$', '', c)
        if PATH_LINE_RE.match(c) and c not in strict:
            strict.append(c)
    if strict:
        return strict, True
    out = []
    for m in PATH_RE.finditer(text or ''):
        p = re.sub(r':\d+.*$', '', norm_path(m.group(0), repo_path))
        if p and '/' in p and p not in out:
            out.append(p)
    return out, False


def prf(pred, gold):
    pred, gold = set(pred), set(gold)
    if not gold:
        return 0.0, 0.0, 0.0
    tp = len(pred & gold)
    p = tp / len(pred) if pred else 0.0
    r = tp / len(gold)
    f = 2 * p * r / (p + r) if p + r else 0.0
    return p, r, f


PREV_KINDS = {"ordered_list", "set_of_names", "exact_string", "compound"}


def is_prev_task(task):
    """Tasks from the earlier audited trials (tasks_prev.json) are graded exactly as grade.py does."""
    return "source_set" in task or task["answer_kind"] in PREV_KINDS


def grade_prev(task, answer):
    """Delegate to the original grade.py (copied verbatim as grade_prev.py). correct = score 1.0.
    Also records an order-sensitive exact flag for ordered_list (grade.py's score is 1.0 when all
    gold names appear anywhere, in any order) and an exact path:line flag for path_and_line."""
    import grade_prev  # noqa: PLC0415  (sits next to this file)
    g = grade_prev.grade(task, answer or "")
    f = float(g.get("f1") or 0.0)
    ok = f >= 1.0 - 1e-9
    extra = {}
    if task["answer_kind"] == "ordered_list":
        cands = [task["ground_truth"]] + [a for a in (task.get("alternates") or []) if isinstance(a, list)]
        cands = [[norm_name(x) for x in c] for c in cands]
        lines = [norm_name(l) for l in answer_lines(answer or "")]
        extra["exact_order"] = lines in cands
    if task["answer_kind"] == "path_and_line":
        ok_set = {norm_path_line(task["ground_truth"][0], task.get("repo_path", ""))}
        for a in task.get("alternates") or []:
            for s in (a if isinstance(a, list) else [a]):
                ok_set.add(norm_path_line(s, task.get("repo_path", "")))
        lines = answer_lines(answer or "")
        extra["exact_order"] = len(lines) == 1 and norm_path_line(lines[0], task.get("repo_path", "")) in ok_set
    pred = g.get("pred")
    return dict(correct=ok, correct_lenient=ok, f1=round(f, 4), precision=round(float(g.get("precision") or 0), 4),
                recall=round(float(g.get("recall") or 0), 4), pred=pred if isinstance(pred, (list, str)) else str(pred),
                **extra)


def grade(task, answer):
    """Returns dict(correct, correct_lenient, f1, precision, recall, pred)."""
    if is_prev_task(task):
        return grade_prev(task, answer)
    kind = task["answer_kind"]
    rp = task.get("repo_path", "")
    gold = task["ground_truth"]
    ans = answer or ""
    if kind == "path_and_line":
        ok_set = {norm_path_line(g, rp) for g in gold} | {norm_path_line(a, rp) for a in task.get("alternates") or []
                                                          if isinstance(a, str)}
        lines = answer_lines(ans)
        strict = len(lines) == 1 and norm_path_line(lines[0], rp) in ok_set
        lenient = bool(lines) and norm_path_line(lines[-1], rp) in ok_set
        pred = norm_path_line(lines[-1], rp) if lines else ""
        f = 1.0 if strict else 0.0
        return dict(correct=strict, correct_lenient=lenient or strict, f1=f, precision=f, recall=f, pred=pred)
    if kind == "set_of_paths":
        pred, only_paths = extract_paths(ans, rp)
        gold_n = [norm_path(g, rp) for g in gold]
        p, r, f = prf(pred, gold_n)
        ok = f == 1.0
        return dict(correct=ok, correct_lenient=ok, f1=round(f, 4), precision=round(p, 4), recall=round(r, 4),
                    pred=pred[:60], path_only_lines=only_paths)
    if kind == "ordered_names":
        cands = [gold] + [a for a in (task.get("alternates") or []) if isinstance(a, list)]
        cands = [[norm_name(x) for x in c] for c in cands]
        lines = answer_lines(ans)
        strict_pred = [norm_name(l) for l in lines]
        strict = strict_pred in cands
        # lenient: tolerate a prose preamble (drop non-identifier lines) and a one-line arrow chain
        toks = []
        for l in lines:
            parts = re.split(r'\s*(?:->|→|⇒)\s*', l.strip())
            if len(parts) > 1:
                toks += [norm_name(x) for x in parts if x.strip()]
            else:
                n = norm_name(l)
                if IDENT_RE.match(n):
                    toks.append(n)
        lenient = strict or toks in cands or any(toks[-len(c):] == c and len(toks) >= len(c) for c in cands)
        f = 1.0 if strict else 0.0
        return dict(correct=strict, correct_lenient=lenient, f1=f, precision=f, recall=f, pred=strict_pred[:20])
    raise ValueError(f"unknown answer_kind {kind}")


# ---------------------------------------------------------------- stream parsing

def parse_stream(path):
    result = None
    tools = []
    hook_events = []
    init = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            t = ev.get("type")
            if t == "result":
                result = ev
            elif t == "system" and ev.get("subtype") == "init":
                init = ev
            elif t == "system" and ev.get("subtype") == "hook_response":
                out = str(ev.get("output") or ev.get("stdout") or "")
                ctx = out
                try:  # UserPromptSubmit returns JSON with additionalContext
                    j = json.loads(out)
                    ctx = ((j.get("hookSpecificOutput") or {}).get("additionalContext")) or ""
                except ValueError:
                    pass
                hook_events.append({
                    "hook_event": ev.get("hook_event") or ev.get("hook_event_name"),
                    "hook_name": ev.get("hook_name"),
                    "output_len": len(out),
                    "context": ctx[:4000],
                })
            elif t == "assistant":
                for blk in (ev.get("message") or {}).get("content") or []:
                    if blk.get("type") != "tool_use":
                        continue
                    name = blk.get("name", "")
                    inp = blk.get("input") or {}
                    rec = {"name": name}
                    if name == "Bash":
                        rec["command"] = inp.get("command", "")
                    elif name.startswith("mcp__"):
                        rec["input"] = inp
                    elif name in ("Grep", "Glob", "Read"):
                        rec["input"] = {k: inp.get(k) for k in ("pattern", "path", "file_path", "glob")
                                        if inp.get(k) is not None}
                    elif name in ("Agent", "Task"):
                        rec["input"] = {"description": inp.get("description"),
                                        "subagent_type": inp.get("subagent_type")}
                    tools.append(rec)
    return init, result, tools, hook_events


def done_keys(results_path):
    keys = set()
    if os.path.exists(results_path):
        with open(results_path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                keys.add((r["model"], r["arm"], r["task"], r["rep"]))
    return keys


def apply_grade(row, task):
    g = grade(task, row.get("answer"))
    row.update({
        "correct": g["correct"], "correct_lenient": g["correct_lenient"], "f1": g["f1"],
        "precision": g["precision"], "recall": g["recall"], "pred": g["pred"],
    })
    if "exact_order" in g:
        row["exact_strict"] = g["exact_order"]
    return row


def sync_credentials():
    """cfg-off and cfg-mcp hold copies of one OAuth login. The refresh token rotates, so when one
    config refreshes, the other's copy goes dead ("OAuth session expired"). Before each session,
    copy the credentials with the later expiresAt over the other one (atomic replace)."""
    files = [os.path.join(W, f"cfg-{a}", ".credentials.json") for a in ("off", "mcp")]
    try:
        docs = [json.load(open(f)) for f in files]
        exp = [((d.get("claudeAiOauth") or {}).get("expiresAt") or 0) for d in docs]
        src, dst = (0, 1) if exp[0] >= exp[1] else (1, 0)
        if docs[src] == docs[dst]:
            return
        tmp = files[dst] + ".tmp"
        with open(tmp, "w") as f:
            json.dump(docs[src], f)
        os.chmod(tmp, 0o600)
        os.replace(tmp, files[dst])
    except (OSError, ValueError) as e:
        print(f"sync_credentials: {e}", flush=True)


def run_session(task, arm, model, rep, out, timeout, meta):
    sync_credentials()
    row_id = f"{model}__{task['id']}__{arm}__r{rep}"
    rdir = os.path.join(out, row_id)
    os.makedirs(rdir, exist_ok=True)
    stream_path = os.path.join(rdir, "stream.jsonl")
    cwd = task["repo_path"]
    cfg = os.path.join(W, f"cfg-{arm}")
    env = dict(os.environ)
    env["CLAUDE_CONFIG_DIR"] = cfg
    env["CBM_CACHE_DIR"] = CACHE
    env.pop("ANTHROPIC_API_KEY", None)
    prompt = PREFIX + task["question"]
    argv = [CLAUDE, "-p", prompt, "--model", model, "--output-format", "stream-json", "--verbose",
            "--include-hook-events", "--dangerously-skip-permissions", "--strict-mcp-config",
            "--mcp-config", mcp_config(arm)]
    with open(os.path.join(rdir, "argv.json"), "w") as f:
        json.dump({"argv": argv, "cwd": cwd, "CLAUDE_CONFIG_DIR": cfg, "CBM_CACHE_DIR": CACHE}, f, indent=1)
    error = None
    t0 = time.monotonic()
    with open(stream_path, "w") as so, open(os.path.join(rdir, "stderr.txt"), "w") as se:
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdout=so, stderr=se, stdin=subprocess.DEVNULL,
                             start_new_session=True)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            error = f"timeout after {timeout}s"
            try:
                os.killpg(p.pid, signal.SIGTERM)
                time.sleep(3)
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            rc = p.wait()
    wall = time.monotonic() - t0
    init, res, tools, hooks = parse_stream(stream_path)
    answer = None
    row = {
        "host": meta["host"], "claude_version": meta["claude_version"], "binary_sha256": meta["binary_sha256"],
        "binary_version": meta["binary_version"], "model": model, "arm": arm, "task": task["id"],
        "repo_key": task["repo_key"], "language": task["language"], "archetype": task["archetype"],
        "answer_kind": task["answer_kind"], "rep": rep, "wall_s": round(wall, 3), "returncode": rc,
    }
    if res is None:
        error = error or f"no result event (rc={rc})"
    else:
        answer = res.get("result")
        if res.get("is_error") and not error:
            error = f"result is_error subtype={res.get('subtype')}"
        u = res.get("usage") or {}
        row.update({
            "duration_ms": res.get("duration_ms"), "duration_api_ms": res.get("duration_api_ms"),
            "num_turns": res.get("num_turns"), "cost_usd": res.get("total_cost_usd"),
            "tokens": {k: u.get(k) for k in ("input_tokens", "output_tokens",
                                             "cache_creation_input_tokens", "cache_read_input_tokens")},
            "session_id": res.get("session_id"),
        })
    shell_cmds = [t["command"] for t in tools if t["name"] == "Bash"]
    ups = [h for h in hooks if h["hook_event"] == "UserPromptSubmit"]
    ss = [h for h in hooks if h["hook_event"] == "SessionStart"]
    row.update({
        "answer": answer,
        "tools": tools,
        "n_tool_calls": len(tools),
        "n_shell_searches": sum(1 for c in shell_cmds if SEARCH_RE.search(c or "")),
        "n_grep_glob_tools": sum(1 for t in tools if t["name"] in ("Grep", "Glob")),
        "n_read_tools": sum(1 for t in tools if t["name"] == "Read"),
        "n_mcp_calls": sum(1 for t in tools if t["name"].startswith("mcp__")),
        "mcp_servers_loaded": [s.get("name") for s in (init or {}).get("mcp_servers", [])] if init else None,
        "hook_events": hooks,
        "ups_context_present": any(h["context"].strip() for h in ups),
        "sessionstart_context_present": any(h["context"].strip() for h in ss),
        "error": error,
    })
    return apply_grade(row, task)


def regrade(path, tasks):
    out = path + ".regraded.jsonl"
    n = 0
    with open(path) as f, open(out, "w") as g:
        for line in f:
            if not line.strip():
                continue
            r = json.loads(line)
            if r["task"] in tasks:
                apply_grade(r, tasks[r["task"]])
            g.write(json.dumps(r) + "\n")
            n += 1
    print(f"regraded {n} rows -> {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model")
    ap.add_argument("--out")
    ap.add_argument("--tasks-file", default=os.path.join(HERE, "tasks_real.json"))
    ap.add_argument("--tasks", default="")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--rep-start", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--arms", default="off,mcp")
    ap.add_argument("--regrade")
    a = ap.parse_args()

    tasks = json.load(open(a.tasks_file))
    if a.regrade:
        regrade(a.regrade, {t["id"]: t for t in tasks})
        return
    if not (a.model and a.out):
        ap.error("--model and --out are required")
    if a.tasks:
        want = a.tasks.split(",")
        tasks = sorted([t for t in tasks if t["id"] in want], key=lambda t: want.index(t["id"]))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "run.pid"), "w") as f:
        f.write(str(os.getpid()))
    results = os.path.join(a.out, "results.jsonl")
    meta = {
        "host": socket.gethostname(),
        "claude_version": sh([CLAUDE, "--version"]),
        "binary_sha256": file_sha256(BIN),
        "binary_version": sh([BIN, "--version"]),
    }
    print("meta", json.dumps(meta), flush=True)
    done = done_keys(results)
    arms = a.arms.split(",")
    for rep in range(a.rep_start, a.rep_start + a.reps):
        for i, task in enumerate(tasks):
            # alternate arm order per task so neither arm always runs first (warm caches)
            order = arms if (i + rep) % 2 == 0 else list(reversed(arms))
            for arm in order:
                if (a.model, arm, task["id"], rep) in done:
                    continue
                row = run_session(task, arm, a.model, rep, a.out, a.timeout, meta)
                with open(results, "a") as f:
                    f.write(json.dumps(row) + "\n")
                print(f"{time.strftime('%H:%M:%S')} {task['id']} {arm} r{rep} wall={row['wall_s']:.1f} "
                      f"correct={row['correct']} f1={row['f1']} mcp={row['n_mcp_calls']} "
                      f"sh={row['n_shell_searches']} ups={row['ups_context_present']} err={row['error']}",
                      flush=True)


if __name__ == "__main__":
    main()
