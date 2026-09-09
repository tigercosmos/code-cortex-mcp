#!/usr/bin/env python3
"""Run the frozen matched complete-task comparison, one session at a time."""

import argparse
import datetime
import gzip
import hashlib
import json
import os
import pathlib
import select
import shutil
import subprocess
import time

HERE = pathlib.Path(__file__).resolve().parent
BOUND = 32 * 1024 * 1024
MCP_TIMEOUT_SECONDS = 120

class FatalCodexmonSurvivor(RuntimeError):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def timestamp_seconds(value):
    if not isinstance(value, str):
        raise ValueError("terminal status has no ended_at timestamp")
    return datetime.datetime.fromisoformat(value).timestamp()


def save_new(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def append_json(path, value):
    with path.open("a") as stream:
        stream.write(json.dumps(value, sort_keys=True) + "\n")


class MCPClient:
    def __init__(self, binary, env, output, cwd, timeout_seconds=MCP_TIMEOUT_SECONDS):
        self.transcript = output / "mcp-transcript.jsonl"
        self.stderr_stream = (output / "mcp-server.stderr").open("xb")
        self.proc = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.stderr_stream,
                                     env=env, cwd=cwd, bufsize=0)
        self.pending = b""
        self.timeout_seconds = timeout_seconds
        self.next_id = 0
        try:
            initialized = self.rpc("initialize", {
                "protocolVersion": "2024-11-05", "capabilities": {},
                "clientInfo": {"name": "scale-task-context-21", "version": "1"},
            }, timeout=self.timeout_seconds)
            if "error" in initialized:
                raise RuntimeError("MCP initialize failed")
            self.notify("notifications/initialized")
            listed = self.rpc("tools/list", timeout=self.timeout_seconds)
            if "error" in listed:
                raise RuntimeError("MCP tools/list failed")
            self.tools = listed.get("result", {}).get("tools", [])
        except BaseException:
            self.close()
            raise

    def send(self, value):
        append_json(self.transcript, {"direction": "client_to_server", "message": value})
        self.proc.stdin.write((json.dumps(value, separators=(",", ":")) + "\n").encode())
        self.proc.stdin.flush()

    def receive(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("MCP response timeout")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                raise TimeoutError("MCP response timeout")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("MCP server closed stdout")
            self.pending += chunk
            if len(self.pending) > BOUND:
                raise RuntimeError("MCP response exceeds 32 MiB")
        line, self.pending = self.pending.split(b"\n", 1)
        value = json.loads(line)
        append_json(self.transcript, {"direction": "server_to_client", "message": value})
        return value

    def rpc(self, method, params=None, timeout=None):
        timeout = self.timeout_seconds if timeout is None else timeout
        self.next_id += 1
        request_id = self.next_id
        value = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            value["params"] = params
        self.send(value)
        deadline = time.monotonic() + timeout
        while True:
            response = self.receive(max(0.001, deadline - time.monotonic()))
            if response.get("id") == request_id:
                return response

    def notify(self, method, params=None):
        value = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            value["params"] = params
        self.send(value)

    def call(self, name, arguments, timeout=None):
        response = self.rpc("tools/call", {"name": name, "arguments": arguments}, timeout)
        if "error" in response:
            raise RuntimeError("MCP tools/call failed: " + json.dumps(response["error"]))
        return response.get("result")

    def close(self):
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)
        if self.proc.stdout:
            self.proc.stdout.close()
        self.stderr_stream.close()


def command(argv, output, stem, env=None, timeout=30, check=True):
    save_new(output / f"{stem}.argv.json", argv)
    started = time.monotonic()
    try:
        result = subprocess.run(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout)
        code, stdout, stderr, error = result.returncode, result.stdout, result.stderr, None
    except subprocess.TimeoutExpired as exc:
        code, stdout, stderr, error = None, exc.stdout or b"", exc.stderr or b"", "timeout"
    if len(stdout) > BOUND or len(stderr) > BOUND:
        raise RuntimeError(stem + " output exceeds 32 MiB")
    (output / f"{stem}.stdout").write_bytes(stdout)
    (output / f"{stem}.stderr").write_bytes(stderr)
    save_new(output / f"{stem}.result.json",
             {"returncode": code, "error": error,
              "wall_seconds": time.monotonic() - started})
    if check and code != 0:
        raise RuntimeError(stem + " failed")
    return stdout, code


def unwrap(raw):
    envelope = json.loads(raw)
    if envelope.get("isError"):
        raise ValueError("MCP returned isError")
    body = envelope.get("structuredContent")
    if isinstance(body, dict):
        return body
    for block in envelope.get("content", []):
        if block.get("type") == "text":
            value = json.loads(block["text"])
            if isinstance(value, dict):
                return value
    raise ValueError("MCP response has no object payload")


def snapshot(label, related_root_pids=()):
    memory = {}
    meminfo = pathlib.Path("/proc/meminfo")
    if meminfo.is_file():
        for line in meminfo.read_text().splitlines():
            key, value = line.split(":", 1)
            if key in ("MemTotal", "MemAvailable", "SwapFree"):
                memory[key] = value.strip()
    processes = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,pcpu=,rss=,comm=,args=", "--sort=-pcpu"],
        capture_output=True, text=True, timeout=10, check=True).stdout.splitlines()
    return {"label": label, "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "monotonic": time.monotonic(), "loadavg": list(os.getloadavg()),
            "memory": memory, "processes": processes,
            "related_root_pids": sorted({os.getpid(), *[pid for pid in related_root_pids
                                                         if isinstance(pid, int)]})}


def graph_context(task, project, client, output):
    started = time.monotonic()
    if task["archetype"] == "one-shot":
        name = "inspect_symbol"
        args = {"project": project["project"], "symbol": task["symbol"],
                "source_lines": 2, "callers_limit": 0, "callees_limit": 0,
                "max_bytes": 4000}
    else:
        name = "trace_path"
        args = {"project": project["project"], "function_name": task["symbol"],
                "from_function": task["from_symbol"], "direction": "inbound",
                "mode": "calls", "depth": 3, "max_work": 128,
                "source_context": 1, "max_bytes": 6000}
    args_path = output / "retrieval.args.json"
    save_new(args_path, args)
    body = unwrap(json.dumps(client.call(name, args, timeout=20)).encode())
    save_new(output / "retrieval.response.json", body)
    if task["archetype"] == "one-shot":
        symbol = body.get("symbol", {})
        observed = f"{symbol.get('file')}:{symbol.get('start_line')}"
        correct = observed == task["expected"]
        projection = {key: body.get(key) for key in ("project", "symbol", "source", "source_start_line",
                                                      "source_end_line", "source_clipped") if key in body}
    else:
        observed = [node.get("name") for node in body.get("path", [])]
        correct = (body.get("path_found") is True and observed == task["expected"] and
                   body.get("traversal_strategy") == "targeted_forward_bfs" and
                   body.get("traversal_truncated") is False)
        projection = {key: body.get(key) for key in
                      ("from_function", "traversal_strategy", "traversal_examined_edges",
                       "traversal_visited_nodes", "traversal_truncated", "path_found", "path",
                       "caller_edges", "source_note") if key in body}
    if not correct:
        raise ValueError("retrieval is not source-oracle correct")
    context = json.dumps(projection, ensure_ascii=False, sort_keys=True)
    if len(context.encode()) > 6000:
        raise ValueError("retrieved context exceeds frozen budget")
    receipt = {"backend": "production_mcp_stdio_jsonrpc", "tool": name,
               "wall_seconds": time.monotonic() - started, "context_bytes": len(context.encode()),
               "source_oracle_correct": True, "observed": observed,
               "payload_sha256": hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()}
    save_new(output / "retrieval-receipt.json", receipt)
    (output / "delivered-context.txt").write_text(context)
    return context, receipt


def config_flags():
    values = [
        'model_reasoning_effort="medium"', 'approval_policy="never"', 'project_doc_max_bytes=0',
        'features.memories=false', 'memories.use_memories=false', 'memories.generate_memories=false',
        'features.multi_agent=false', 'features.apps=false', 'features.plugins=false',
        'features.hooks=false', 'web_search="disabled"', 'check_for_update_on_startup=false',
    ]
    disabled = set()
    for root in (pathlib.Path.home() / ".agents/skills", pathlib.Path.home() / ".codex/skills"):
        if root.exists():
            for entry in root.iterdir():
                skill = entry / "SKILL.md"
                if skill.is_file():
                    disabled.update((str(skill), str(skill.resolve())))
    entries = ",".join("{path=" + json.dumps(path) + ",enabled=false}" for path in sorted(disabled))
    values.append("skills.config=[" + entries + "]")
    return [item for value in values for item in ("-c", value)]


def prompt(task, context):
    common = (
        "Work only in this frozen synthetic repository. Read only: do not edit files, build, or "
        "run tests. Do not use the network, other agents, external repositories, graph services, "
        "graph CLIs/databases, prior sessions, benchmarks, answer keys, or skill files. Complete "
        "the task without questions. Use efficient rg and focused file reads when retrieval context "
        "below is empty, missing, or inconsistent. If the client supplied complete context, answer "
        "directly from it; the client source-audited it before this session.\n\n")
    return common + task["question"] + "\n\nClient retrieval context:\n" + context


def archive_rollout(home, output):
    files = list(home.glob("sessions/**/*.jsonl"))
    if len(files) != 1:
        raise ValueError("expected exactly one fresh rollout")
    source = files[0]
    if source.stat().st_size > BOUND:
        raise ValueError("rollout exceeds 32 MiB")
    kept = []
    for line in source.read_text().splitlines():
        event = json.loads(line)
        payload = event.get("payload", {})
        if event.get("type") == "response_item":
            if payload.get("type") == "reasoning":
                continue
            if payload.get("type") == "message" and (payload.get("role") in ("system", "developer")
                                                       or payload.get("channel") == "analysis"):
                continue
        if event.get("type") == "event_msg" and payload.get("type") == "agent_reasoning":
            continue
        kept.append(event)
    with gzip.open(output / "rollout-public.jsonl.gz", "wt") as stream:
        for event in kept:
            stream.write(json.dumps(event, sort_keys=True) + "\n")
    save_new(output / "rollout-receipt.json",
             {"source_path": str(source), "source_sha256": sha256(source),
              "source_bytes": source.stat().st_size, "retained_records": len(kept),
              "omitted": "hidden reasoning and system/developer message bodies"})


def grade(task, answer):
    value = answer.strip() if isinstance(answer, str) else None
    if task["archetype"] == "one-shot":
        observed = value
    else:
        observed = value.splitlines() if value is not None else None
    return {"correct": observed == task["expected"], "observed": observed,
            "expected": task["expected"]}


def run_session(task, row, project, setup, output, codex_bin, codexmon_bin, mcp_client):
    namespace = hashlib.sha256(str(output.parent.resolve()).encode()).hexdigest()[:16]
    home = pathlib.Path(setup["home"]) / "sessions" / namespace / row["run"]
    home.mkdir(parents=True, exist_ok=False)
    auth = home / "auth.json"
    auth.symlink_to(pathlib.Path.home() / ".codex/auth.json")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CODEX_", "CBM_"))}
    env.update({"CODEX_HOME": str(home)})
    context = ""
    retrieval = {"backend": "none", "wall_seconds": 0.0, "context_bytes": 0,
                 "source_oracle_correct": None}
    task_started = time.monotonic()
    task_started_wall = time.time()
    if row["arm"] == "mcp-context":
        context, retrieval = graph_context(task, project, mcp_client, output)
    else:
        save_new(output / "retrieval-receipt.json", retrieval)
        (output / "delivered-context.txt").write_text("")
    final_prompt = prompt(task, context)
    (output / "prompt.txt").write_text(final_prompt)
    answer_path = output / "answer.txt"
    native = ["exec", "--ignore-user-config", "--ignore-rules", "--skip-git-repo-check",
              "--json", "--sandbox", "read-only", "-C", project["repo_root"],
              "-m", row["model"], *config_flags(), "-o", str(answer_path), final_prompt]
    save_new(output / "invocation.json", native)
    def snapshot_jobs(phase):
        raw, _ = command([str(codexmon_bin), "list", "--json"], output, "jobs-" + phase, env=env, timeout=15)
        try:
            rows = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise ValueError("codexmon list JSON rejected: " + str(exc))
        if not isinstance(rows, list) or any(not isinstance(row, dict) or not isinstance(row.get("id"), str) for row in rows):
            raise ValueError("codexmon list schema rejected")
        ids = [row["id"] for row in rows]
        if len(ids) != len(set(ids)):
            raise ValueError("codexmon list duplicate IDs rejected")
        save_new(output / ("jobs-" + phase + "-parsed.json"), rows)
        return rows
    before_jobs = snapshot_jobs("before-start")
    before_ids = {row["id"] for row in before_jobs}
    def cleanup_fresh_start_jobs(reason):
        receipt = {"reason": reason, "before_ids": sorted(before_ids), "fresh_ids": [], "matching_ids": [], "cancelled": [], "terminal": {}, "errors": []}
        try:
            after_jobs = snapshot_jobs("after-start-failure")
            fresh = [row for row in after_jobs if row["id"] not in before_ids]
            receipt["fresh_ids"] = [row["id"] for row in fresh]
            expected_thresholds = {"heartbeat_sec": 10, "slow_after_sec": 30, "stalled_sec": 120, "tool_stuck_sec": 90, "wall_sec": 300}
            matching = [row for row in fresh if row.get("agent") == "codex" and row.get("agent_bin") == str(codex_bin) and row.get("args") == native and row.get("cwd") == project["repo_root"] and row.get("json_mode") is True and row.get("thresholds") == expected_thresholds]
            receipt["matching_ids"] = [row["id"] for row in matching]
            for row in matching:
                jid = row["id"]
                try:
                    command([str(codexmon_bin), "cancel", jid], output, "cancel-fresh-" + jid, env=env, timeout=15, check=False)
                    receipt["cancelled"].append(jid)
                except BaseException as exc:
                    receipt["errors"].append({"id": jid, "stage": "cancel", "type": type(exc).__name__, "message": str(exc)})
                try:
                    raw, _ = command([str(codexmon_bin), "status", jid, "--json"], output, "cleanup-fresh-status-" + jid, env=env, timeout=15, check=False)
                    terminal = json.loads(raw); receipt["terminal"][jid] = terminal
                    if not isinstance(terminal, dict) or terminal.get("id") != jid or terminal.get("agent_bin") != str(codex_bin) or terminal.get("state") in ("queued", "running"):
                        receipt["errors"].append({"id": jid, "stage": "status", "type": "ValueError", "message": "terminal identity/state rejected"})
                except BaseException as exc:
                    receipt["errors"].append({"id": jid, "stage": "status", "type": type(exc).__name__, "message": str(exc)})
        except BaseException as exc:
            receipt["errors"].append({"stage": "after-list", "type": type(exc).__name__, "message": str(exc)})
        save_new(output / "fresh-start-recovery.json", receipt)
        return receipt
    job = None
    try:
        start_raw, _ = command([str(codexmon_bin), "start", "--agent", "codex", "--agent-bin", str(codex_bin), "--wall-timeout", "300", "--tool-timeout", "90", "--idle-timeout", "120", "--json", "-C", project["repo_root"], "--", *native], output, "start", env=env, timeout=30)
        job = json.loads(start_raw)
        if not isinstance(job, dict) or not isinstance(job.get("id"), str):
            raise ValueError("codexmon start JSON schema/id rejected")
    except BaseException as exc:
        recovery = cleanup_fresh_start_jobs("start-response-rejection")
        failure = {"reason": "start-response-rejection", "primary": {"type": type(exc).__name__, "message": str(exc)}, "cleanup": recovery, "accepted_task_wall_seconds": None, "timing_accepted": False}
        save_new(output / "start-response-failure.json", failure)
        if recovery["errors"] or len(recovery["matching_ids"]) != 1:
            if recovery.get("matching_ids") and any((not isinstance(recovery.get("terminal",{}).get(j),dict) or recovery["terminal"][j].get("state") in ("queued","running")) for j in recovery["matching_ids"]):
                raise FatalCodexmonSurvivor("codexmon survivor after malformed start response") from exc
            raise RuntimeError("start-response recovery ambiguous or cleanup failed") from exc
        raise
    save_new(output / "job.json", job)
    def cleanup_started_job(reason):
        receipt={"reason":reason,"job_id":job.get("id"),"cancel_attempted":False,"terminal_verified":False,"errors":[],"status_attempts":[],"survivor_evidence":None}
        receipt["cancel_attempted"]=True
        try:
            command([str(codexmon_bin),"cancel",job["id"]],output,"cancel-"+reason,env=env,timeout=15,check=False)
        except BaseException as exc:
            receipt["errors"].append({"stage":"cancel","type":type(exc).__name__,"message":str(exc)})
        for attempt in range(3):
            try:
                raw,_=command([str(codexmon_bin),"status",job["id"],"--json"],output,"cleanup-status-"+reason+"-"+str(attempt),env=env,timeout=15,check=False)
                final=json.loads(raw);receipt["status_attempts"].append(final);receipt["terminal"]=final
                if isinstance(final,dict) and final.get("id")==job["id"] and final.get("agent_bin")==str(codex_bin) and final.get("state") not in ("queued","running"):
                    receipt["terminal_verified"]=True;break
                receipt["errors"].append({"stage":"status","type":"ValueError","message":"terminal identity/state rejected"})
            except BaseException as exc:
                receipt["errors"].append({"stage":"status","type":type(exc).__name__,"message":str(exc)})
        if not receipt["terminal_verified"]: receipt["survivor_evidence"]={"job_id":job["id"],"status_attempt_count":len(receipt["status_attempts"]),"reason":"bounded cleanup could not verify terminal"}
        save_new(output/"post-start-cleanup.json",receipt)
        return receipt
    def fail_post_start(reason, primary, details=None):
        cleanup = cleanup_started_job(reason)
        failure = {"reason": reason, "primary": primary, "cleanup": cleanup, "accepted_task_wall_seconds": None, "timing_accepted": False}
        if details is not None: failure["details"] = details
        save_new(output / "post-start-failure.json", failure)
        if not cleanup.get("terminal_verified"):
            raise FatalCodexmonSurvivor("codexmon survivor after " + reason)
        raise RuntimeError("post-start rejection: " + reason)
    if job.get("agent_bin") != str(codex_bin):
        fail_post_start("job-identity-rejection", "codexmon job agent_bin identity rejected", {"expected_agent_bin": str(codex_bin), "observed_agent_bin": job.get("agent_bin")})
    terminal = None
    terminal_observation_wall = None
    deadline = time.monotonic() + 330
    ordinal = 0
    try:
        while time.monotonic() < deadline:
            status_raw, _ = command([str(codexmon_bin), "status", job["id"], "--json"], output,
                                    f"status-{ordinal:04d}", env=env, timeout=15)
            terminal = json.loads(status_raw)
            if not isinstance(terminal, dict) or terminal.get("id") != job["id"] or terminal.get("agent_bin") != str(codex_bin): raise ValueError("normal poll identity rejected")
            if terminal["state"] not in ("queued", "running"):
                terminal_observation_wall = time.monotonic() - task_started
            append_json(output / "contention-samples.jsonl",
                        snapshot(f"poll-{ordinal}",
                                 (terminal.get("worker_pid"), terminal.get("agent_pid"))))
            ordinal += 1
            if terminal["state"] not in ("queued", "running"):
                break
            time.sleep(1)
    except BaseException as exc:
        fail_post_start("status-rejection", type(exc).__name__ + ": " + str(exc))
    if terminal is None or terminal["state"] in ("queued", "running"):
        fail_post_start("nonterminal-rejection", "controller did not observe a terminal session")
    save_new(output / "terminal-status.json", terminal)
    if terminal.get("agent_bin") != str(codex_bin) or terminal.get("state") != "completed" or terminal.get("exit_code") != 0:
        fail_post_start("terminal-rejection", "codexmon terminal rejected", {"expected_agent_bin": str(codex_bin), "observed_agent_bin": terminal.get("agent_bin"), "state": terminal.get("state"), "exit_code": terminal.get("exit_code")})
    task_wall = timestamp_seconds(terminal.get("ended_at")) - task_started_wall
    if task_wall <= 0 or task_wall > terminal_observation_wall + 1.0:
        raise ValueError("invalid recorded complete-task duration")
    for key, name in (("events_file", "events.jsonl"), ("log_file", "events.log"),
                      ("result_file", "result.txt")):
        path = terminal.get(key)
        if path and pathlib.Path(path).is_file():
            shutil.copy2(path, output / name)
    archive_rollout(home, output)
    answer = answer_path.read_text() if answer_path.is_file() else None
    verdict = grade(task, answer)
    save_new(output / "grade.json", verdict)
    events = []
    if (output / "events.jsonl").is_file():
        events = [json.loads(line) for line in (output / "events.jsonl").read_text().splitlines()
                  if line.strip()]
    items = [event.get("item", {}) for event in events if event.get("type") == "item.completed"]
    tool_audit = {"mcp_calls": sum(item.get("type") == "mcp_tool_call" for item in items),
                  "command_calls": sum(item.get("type") == "command_execution" for item in items),
                  "item_types": [item.get("type") for item in items],
                  "commands": [item.get("command") for item in items
                               if item.get("type") == "command_execution"]}
    save_new(output / "tool-audit.json", tool_audit)
    summary = {**row, "state": terminal["state"], "task_wall_seconds": task_wall,
               "agent_exit_code": terminal.get("exit_code"),
               "timing_basis": "retrieval start through codexmon ended_at",
               "task_started_epoch_seconds": task_started_wall,
               "terminal_observation_seconds": terminal_observation_wall,
               "retrieval_backend": retrieval["backend"],
               "retrieval_context_bytes": retrieval["context_bytes"],
               "retrieval_wall_seconds": retrieval["wall_seconds"],
               "answer_correct": verdict["correct"], "source_oracle_correct":
               (retrieval["source_oracle_correct"] is not False),
               "no_session_mcp_calls": tool_audit["mcp_calls"] == 0,
               "usage": terminal.get("usage"), "terminal_observed": True}
    save_new(output / "summary.json", summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup", type=pathlib.Path, required=True)
    parser.add_argument("--protocol", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--codex-bin", type=pathlib.Path, required=True)
    parser.add_argument("--codexmon-bin", type=pathlib.Path, required=True)
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()
    setup = json.loads(args.setup.read_text())
    tasks = {task["id"]: task for task in json.loads((args.protocol / "tasks.json").read_text())}
    schedule = json.loads((args.protocol / "schedule.json").read_text())["rows"]
    if setup.get("state") != "ready" or not setup.get("task_oracle_passed"):
        raise ValueError("setup is not ready")
    if sha256(pathlib.Path(setup["binary"])) != setup["binary_sha256"]:
        raise ValueError("candidate binary drift")
    for name, digest in setup["protocol_hashes"].items():
        if sha256(args.protocol / name) != digest:
            raise ValueError("protocol drift: " + name)
    for name, digest in setup["harness_hashes"].items():
        if sha256(HERE / name) != digest:
            raise ValueError("harness drift: " + name)
    args.output.mkdir(parents=True, exist_ok=False)
    mcp_env = {key: value for key, value in os.environ.items()
               if not key.startswith(("CBM_", "CODEX_"))}
    mcp_env.update({"HOME": setup["home"],
                    "XDG_CACHE_HOME": str(pathlib.Path(setup["home"]) / ".cache"),
                    "TMPDIR": setup["tmp"], "CBM_CACHE_DIR": setup["cache"]})
    mcp_client = MCPClient(setup["binary"], mcp_env, args.output,
                           str(pathlib.Path(setup["home"]).parent))
    trace_tools = [tool for tool in mcp_client.tools if tool.get("name") == "trace_path"]
    if len(trace_tools) != 1:
        mcp_client.close()
        raise ValueError("trace_path is absent or duplicated in MCP tools/list")
    trace_properties = trace_tools[0].get("inputSchema", {}).get("properties", {})
    if not {"from_function", "max_work"}.issubset(trace_properties):
        mcp_client.close()
        raise ValueError("optimized trace_path fields are absent from MCP tools/list")
    save_new(args.output / "mcp-client.json",
             {"binary": setup["binary"], "binary_sha256": setup["binary_sha256"],
              "pid": mcp_client.proc.pid, "transport": "stdio_jsonrpc",
              "initialized_before_timed_schedule": True,
              "tools_list_sha256": hashlib.sha256(
                  json.dumps(mcp_client.tools, sort_keys=True).encode()).hexdigest(),
              "trace_path_required_fields": ["from_function", "max_work"]})
    save_new(args.output / "environment-pre.json", snapshot("trial-pre"))
    save_new(args.output / "controller.json",
             {"setup": str(args.setup), "setup_sha256": sha256(args.setup),
              "candidate_sha256": setup["binary_sha256"], "schedule_rows": len(schedule),
              "harness_hashes": setup["harness_hashes"],
              "limit": args.limit, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
    results = []
    try:
        for row in schedule[:args.limit]:
            output = args.output / row["run"]
            output.mkdir()
            save_new(output / "contention-pre.json", snapshot("task-pre"))
            try:
                result = run_session(
                    tasks[row["task"]], row,
                    setup["projects"][tasks[row["task"]]["scale"]], setup, output,
                    args.codex_bin, args.codexmon_bin, mcp_client)
            except Exception as exc:
                result = {**row, "state": "controller_failure",
                          "error": type(exc).__name__ + ": " + str(exc),
                          "terminal_observed": False, "answer_correct": False}
                save_new(output / "summary.json", result)
            save_new(output / "contention-post.json", snapshot("task-post"))
            results.append(result)
            append_json(args.output / "results.jsonl", result)
            print(json.dumps(result, sort_keys=True), flush=True)
            if not result.get("terminal_observed"):
                break
    finally:
        mcp_client.close()
    save_new(args.output / "environment-post.json", snapshot("trial-post"))
    save_new(args.output / "run-report.json",
             {"scheduled": len(schedule), "attempted": len(results),
              "completed": sum(row.get("state") == "completed" for row in results),
              "correct": sum(row.get("answer_correct") is True for row in results),
              "all_terminal": all(row.get("terminal_observed") for row in results)})


if __name__ == "__main__":
    main()
