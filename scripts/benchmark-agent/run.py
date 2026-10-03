#!/usr/bin/env python3
"""Agent-level A/B benchmark: Claude Code with code-cortex-mcp installed (mcp) vs nothing (off).

Usage:
  run.py --model claude-opus-5-5 --src-root <dir with source-10k/1m/100m> --out <outdir>
         [--tasks id1,id2,...] [--reps 1] [--timeout 600]

Sessions run strictly serially. Rows are appended to <out>/results.jsonl; a row
already present (same model, arm, task, rep) is skipped, so a rerun resumes.
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import time

W = os.path.expanduser(os.environ.get("BENCH_ROOT", "~/bench-agent"))
BIN = os.environ.get("BENCH_BIN") or os.path.join(W, "repo/build/c/code-cortex-mcp")
CACHE = os.path.join(W, "cache")
CLAUDE = os.environ.get("BENCH_CLAUDE") or os.path.expanduser("~/.local/bin/claude")
PREFIX = ("Work only in this repository. Read only: do not edit files, build, or run tests. "
          "Do not use the network. Complete the task without asking questions.\n\n")
SEARCH_RE = re.compile(r"(^|[\s|;&(`$])(grep|egrep|fgrep|rg|find|ag)(\s|$)")


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"ERR {e}"


def mcp_config(arm):
    if arm == "off":
        return json.dumps({"mcpServers": {}})
    return json.dumps({"mcpServers": {"code-cortex-mcp": {
        "command": BIN, "env": {"CBM_CACHE_DIR": CACHE}}}})


def grade(task, answer):
    if answer is None:
        return False
    if task["archetype"] == "one-shot":
        return answer.strip() == task["expected"]
    return answer.strip().splitlines() == task["expected"]


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
            elif t == "system" and "hook" in str(ev.get("subtype", "")):
                hook_events.append({
                    "subtype": ev.get("subtype"),
                    "hook_event": ev.get("hook_event") or ev.get("hook_event_name"),
                    "hook_name": ev.get("hook_name"),
                    "output_len": len(str(ev.get("output") or ev.get("stdout") or "")),
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


def run_session(task, arm, model, rep, cwd, out, timeout, meta):
    row_id = f"{model}__{task['id']}__{arm}__r{rep}"
    rdir = os.path.join(out, row_id)
    os.makedirs(rdir, exist_ok=True)
    stream_path = os.path.join(rdir, "stream.jsonl")
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
        "host": meta["host"], "claude_version": meta["claude_version"], "binary_sha": meta["binary_sha"],
        "binary_version": meta["binary_version"], "model": model, "arm": arm, "task": task["id"],
        "scale": task["scale"], "archetype": task["archetype"], "rep": rep, "wall_s": round(wall, 3),
        "returncode": rc,
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
    row.update({
        "correct": grade(task, answer),
        "answer": answer,
        "tools": tools,
        "n_tool_calls": len(tools),
        "n_shell_searches": sum(1 for c in shell_cmds if SEARCH_RE.search(c or "")),
        "n_grep_glob_tools": sum(1 for t in tools if t["name"] in ("Grep", "Glob")),
        "n_mcp_calls": sum(1 for t in tools if t["name"].startswith("mcp__")),
        "mcp_servers_loaded": [s.get("name") for s in (init or {}).get("mcp_servers", [])] if init else None,
        "hook_events": hooks,
        "error": error,
    })
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--src-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tasks-file", default=os.path.join(W, "tasks.json"))
    ap.add_argument("--tasks", default="")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--arms", default="off,mcp")
    a = ap.parse_args()

    tasks = json.load(open(a.tasks_file))
    if a.tasks:
        want = a.tasks.split(",")
        tasks = [t for t in tasks if t["id"] in want]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "run.pid"), "w") as f:
        f.write(str(os.getpid()))
    results = os.path.join(a.out, "results.jsonl")
    meta = {
        "host": socket.gethostname(),
        "claude_version": sh([CLAUDE, "--version"]),
        "binary_sha": sh(["git", "-C", os.path.join(W, "repo"), "rev-parse", "HEAD"]),
        "binary_version": sh([BIN, "--version"]),
    }
    print("meta", json.dumps(meta), flush=True)
    done = done_keys(results)
    arms = a.arms.split(",")
    for rep in range(1, a.reps + 1):
        for task in tasks:
            cwd = os.path.join(a.src_root, f"source-{task['scale']}")
            for arm in arms:
                if (a.model, arm, task["id"], rep) in done:
                    continue
                row = run_session(task, arm, a.model, rep, cwd, a.out, a.timeout, meta)
                with open(results, "a") as f:
                    f.write(json.dumps(row) + "\n")
                print(f"{time.strftime('%H:%M:%S')} {task['id']} {arm} r{rep} wall={row['wall_s']:.1f} "
                      f"correct={row['correct']} mcp={row['n_mcp_calls']} sh={row['n_shell_searches']} "
                      f"err={row['error']}", flush=True)


if __name__ == "__main__":
    main()
