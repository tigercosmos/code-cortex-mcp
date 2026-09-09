#!/usr/bin/env python3
"""v743: lazy MCP startup, pinned identity, and failure-safe lifecycle."""
import argparse, hashlib, importlib.util, json, pathlib, time, subprocess, sys, os, re, stat
import cache_integrity_v743 as cache_integrity
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("base_run",HERE/"base-run-v743.py");base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
lspec=importlib.util.spec_from_file_location("candidate_lifecycle_v743",HERE/"candidate_lifecycle_v743.py");candidate_lifecycle=importlib.util.module_from_spec(lspec);lspec.loader.exec_module(candidate_lifecycle)
BACKENDS=("candidate","upstream");MCP_ARMS={"candidate-mcp":"candidate","upstream-mcp":"upstream"}
sha256=base.sha256
MCP_TIMEOUT_SECONDS=120
def load_body(result): return base.unwrap(json.dumps(result).encode())

def mcp_env(backend):
 clean={key:value for key,value in __import__("os").environ.items() if not key.startswith(("CBM_","CODEX_"))}
 clean.update({"HOME":backend["home"],"XDG_CACHE_HOME":str(pathlib.Path(backend["home"])/".cache"),"TMPDIR":backend["tmp"],"CBM_CACHE_DIR":backend["cache"]})
 if backend.get("update_check_disabled") is True:
  clean["CBM_UPDATE_CHECK"]="0"
 return clean
def normalized_context(task, project, client, backend, output):
    """Do exactly one source-correct production MCP call, then normalize."""
    started = time.monotonic()
    if backend == "candidate":
        if task["archetype"] == "one-shot":
            tool = "inspect_symbol"
            args = {"project": project["project"], "symbol": task["symbol"],
                    "source_lines": 2, "callers_limit": 0, "callees_limit": 0,
                    "max_bytes": 4000}
            body = load_body(client.call(tool, args, timeout=MCP_TIMEOUT_SECONDS))
            symbol = body.get("symbol", {})
            observed = f'{symbol.get("file")}:{symbol.get("start_line")}'
        else:
            tool = "trace_path"
            args = {"project": project["project"], "function_name": task["symbol"],
                    "from_function": task["from_symbol"], "direction": "inbound",
                    "mode": "calls", "depth": 3, "max_work": 128,
                    "source_context": 1, "max_bytes": 6000}
            body = load_body(client.call(tool, args, timeout=MCP_TIMEOUT_SECONDS))
            observed = [node.get("name") for node in body.get("path", [])]
            if not (body.get("path_found") is True and
                    body.get("traversal_strategy") == "targeted_forward_bfs" and
                    body.get("traversal_truncated") is False):
                raise ValueError("candidate targeted trace contract failed")
    elif backend == "upstream":
        if task["archetype"] == "one-shot":
            tool = "search_graph"
            args = {"project": project["project"], "label": "Function",
                    "name_pattern": "^" + task["symbol"] + "$", "limit": 2,
                    "format": "json", "max_output_tokens": 128}
            body = load_body(client.call(tool, args, timeout=MCP_TIMEOUT_SECONDS))
            matches = []
            columns = body.get("cols")
            for group in body.get("groups", []):
                if not isinstance(group, dict) or not isinstance(group.get("file"), str):
                    raise ValueError("upstream lookup group shape invalid")
                file_name = group["file"]
                for row in group.get("rows", group.get("results", [])):
                    if isinstance(row, dict):
                        decoded = row
                    elif isinstance(row, list):
                        if (not isinstance(columns, list) or len(row) != len(columns) or
                                len(set(columns)) != len(columns) or
                                not all(isinstance(key, str) for key in columns)):
                            raise ValueError("upstream lookup grouped row shape invalid")
                        decoded = dict(zip(columns, row))
                    else:
                        raise ValueError("upstream lookup row shape invalid")
                    if decoded.get("name") == task["symbol"] and decoded.get("label") == "Function":
                        lines = decoded.get("lines", decoded.get("line"))
                        if isinstance(lines, list) and lines:
                            start = lines[0]
                        elif isinstance(lines, (str, int)):
                            start = str(lines).split("-", 1)[0]
                        else:
                            raise ValueError("upstream lookup line shape invalid")
                        try:
                            start = int(start)
                        except (TypeError, ValueError):
                            raise ValueError("upstream lookup line is not an integer")
                        if start < 1:
                            raise ValueError("upstream lookup line is not positive")
                        matches.append(f"{file_name}:{start}")
            if len(matches) != 1:
                raise ValueError("upstream lookup must yield exactly one exact Function")
            observed = matches[0]
        else:
            tool = "query_graph"
            q = ('MATCH (a:Function {name: "' + task["from_symbol"] +
                 '"})-[:CALLS]->(b:Function)-[:CALLS]->(c:Function)-[:CALLS]->'
                 '(d:Function {name: "' + task["symbol"] +
                 '"}) RETURN a.name, b.name, c.name, d.name LIMIT 2')
            args = {"project": project["project"], "format": "json",
                    "max_rows": 2, "query": q}
            body = load_body(client.call(tool, args, timeout=MCP_TIMEOUT_SECONDS))
            rows = body.get("rows", body.get("results", []))
            if len(rows) != 1:
                raise ValueError("upstream chain must yield exactly one row")
            row = rows[0]
            if isinstance(row, dict):
                observed = [row.get(key) for key in ("a.name", "b.name", "c.name", "d.name")]
            elif isinstance(row, list):
                observed = row
            else:
                raise ValueError("upstream chain row is not decodable")
            if any(not isinstance(name, str) for name in observed):
                raise ValueError("upstream chain row has no ordered names")
    else:
        raise ValueError("unknown MCP backend")
    if observed != task["expected"]:
        raise ValueError(f"{backend} retrieval is not source-oracle correct")
    context = json.dumps({"definition": observed} if task["archetype"] == "one-shot"
                         else {"path": observed}, sort_keys=True)
    if len(context.encode()) > 6000:
        raise ValueError("normalized context exceeds 6000 bytes")
    base.save_new(output / "retrieval.args.json", args)
    base.save_new(output / "retrieval.response.json", body)
    receipt = {"backend": backend + "_production_mcp_stdio_jsonrpc", "tool": tool,
               "wall_seconds": time.monotonic() - started,
               "context_bytes": len(context.encode()), "source_oracle_correct": True,
               "observed": observed,
               "payload_sha256": hashlib.sha256(
                   json.dumps(body, sort_keys=True).encode()).hexdigest(),
               "normalized_context_sha256": hashlib.sha256(context.encode()).hexdigest()}
    base.save_new(output / "retrieval-receipt.json", receipt)
    (output / "delivered-context.txt").write_text(context)
    return context, receipt


def retrieve_then_scan(task,project,client,backend,backend_item,output,root_pid):
 """v743 shared production retrieval then exact candidate topology scan; caller timer includes this."""
 context,receipt=normalized_context(task,project,client,backend,output)
 if backend!="candidate" or backend_item.get("enforce_exact_topology") is not True:return context,receipt,None
 active=candidate_lifecycle.scan(backend_item,(root_pid,),active=True);owned=active["owned"];binary=str(backend_item["binary"]);root=[x for x in owned if x["pid"]==root_pid and x["argv"]==binary];child=[x for x in owned if x["ppid"]==root_pid and x["argv"]==binary+" cli --tool-server"]
 if not (active["passed"] and len(owned)==2 and len(root)==1 and len(child)==1 and len(active["processes"])==2 and not active["indeterminate"]):raise ValueError("candidate exact root/tool-server topology rejected")
 base.save_new(output/"post-retrieval-topology.json",{"root_pid":root_pid,"tool_server_pid":child[0]["pid"],"active_scan":active})
 return context,receipt,active

def source_manifest_audit(sources):
    """Hash every manifest entry and reject missing, altered, or added source."""
    projects = {}
    for scale, source in sorted(sources.items()):
        root = pathlib.Path(source["repo_root"])
        manifest = pathlib.Path(source["manifest"])
        aggregate = hashlib.sha256()
        failures, entries = [], 0
        for encoded in manifest.read_bytes().splitlines(keepends=True):
            aggregate.update(encoded)
            record = json.loads(encoded)
            path = root / record["file"]
            entries += 1
            if (not path.is_file() or path.stat().st_size != record["bytes"] or
                    sha256(path) != record["sha256"]):
                failures.append(record["file"])
                if len(failures) >= 20:
                    break
        actual = sum(1 for _ in root.glob("part_*/*.cpp"))
        expected = source["source_audit"]["files"]
        expected_manifest = source["source_audit"]["manifest_sha256"]
        projects[scale] = {"manifest_sha256": aggregate.hexdigest(), "sealed_manifest_sha256": expected_manifest, "entries_checked": entries,
                           "actual_files": actual, "expected_files": expected,
                           "failures": failures,
                           "passed": not failures and entries == actual == expected and aggregate.hexdigest() == expected_manifest}
    return {"passed": all(v["passed"] for v in projects.values()), "projects": projects}


def foreign_cpu(samples, related_pids=()):
    related = set(related_pids)
    bad = []
    for sample in samples:
        related_now = set(related) | {os.getpid()}
        parsed = []
        for line in sample.get("processes", []):
            fields = line.strip().split(None, 5)
            if len(fields) < 6:
                continue
            try:
                value = {"pid": int(fields[0]), "ppid": int(fields[1]),
                         "cpu_percent": float(fields[2]), "rss_kib": int(fields[3]),
                         "command": fields[4], "argv": fields[5]}
            except ValueError:
                continue
            parsed.append(value)
        changed = True
        while changed:
            changed = False
            for item in parsed:
                if item["pid"] not in related_now and item["ppid"] in related_now:
                    related_now.add(item["pid"])
                    changed = True
        bad.extend({**item, "utc": sample.get("utc")} for item in parsed
                   if item["pid"] not in related_now and item["cpu_percent"] >= 50.0)
    return bad


def contention_admission(samples, related_pids=()):
    loads = [sample["loadavg"][0] for sample in samples]
    foreign = foreign_cpu(samples, related_pids)
    return {"samples": len(samples), "max_load_1m": max(loads, default=float("inf")),
            "foreign_processes_at_least_50_percent_cpu": foreign,
            "thresholds": {"max_load_1m": 8.0, "foreign_process_cpu_percent": 50.0},
            "admitted": len(samples) >= 3 and max(loads, default=float("inf")) <= 8.0 and
                        not foreign}


def _runtime_source_oracle(setup, protocol, output, label):
    target = output / ("source-oracle-" + label + ".json")
    result = subprocess.run([sys.executable, str(HERE / "source_oracle.py"),
                             "--setup", setup["source_setup"], "--tasks",
                             str(protocol / "tasks.json"), "--output", str(target)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    base.save_new(output / ("source-oracle-" + label + ".command.json"),
                  {"argv": result.args, "returncode": result.returncode,
                   "stdout": result.stdout, "stderr": result.stderr})
    if result.returncode != 0:
        raise ValueError("runtime source oracle failed: " + label)
    report = json.loads(target.read_text())
    if report.get("passed") is not True:
        raise ValueError("runtime source oracle reported false: " + label)
    return report


def _ctl(backend,out,stem,action):
 raw,code=base.command([str(backend["binary"]),"daemon",action],out,stem,env=mcp_env(backend),timeout=30,check=False)
 return {"action":action,"returncode":code,"stdout":raw.decode(errors="replace"),"cache":backend["cache"],"binary_sha256":backend["binary_sha256"]}
def status(backend,out,stem):
 r=_ctl(backend,out,stem,"status"); text=r["stdout"]; r["active"]="daemon: active" in text; r["absent"]="daemon: not running" in text; r["pid"]=None; r["build_prefix"]=None; r["build_version"]=None
 for line in text.splitlines():
  line=line.strip()
  if line.startswith("pid:"):
   try:r["pid"]=int(line.split(":",1)[1].strip())
   except ValueError:pass
  if line.startswith("build:"):
   fields=line.split()
   if len(fields)>=3 and fields[2].startswith("(") and fields[2].endswith("...)"):
    r["build_version"]=fields[1];r["build_prefix"]=fields[2][1:-4]
 if r["active"]==r["absent"] or (r["active"] and (r["returncode"]!=0 or r["pid"] is None)) or (r["absent"] and r["returncode"]==0): raise ValueError("daemon status malformed")
 if r["active"]:
  expected=backend["binary_sha256"][:12]
  if r["build_prefix"] != expected: raise ValueError("daemon build identity mismatch: expected "+expected+", observed "+str(r["build_prefix"]))
 r["environment_binding"]={"binary":str(backend["binary"]),"binary_sha256":backend["binary_sha256"],"cbm_cache_dir":backend["cache"],"home":backend["home"],"tmpdir":backend["tmp"],"status_exposes_cache":False,"association":"status build identity plus exact isolated client environment; daemon status protocol does not expose cache path"}
 return r
def candidate_owned(backend,roots=()):
 return candidate_lifecycle.scan(backend,roots,active=bool(roots))["owned"]
def candidate_zero(backend,out,label,roots=()):
 receipt={"backend":"candidate_foreground_stdio","binary":str(backend["binary"]),"binary_sha256":backend["binary_sha256"],"isolated_env":{"HOME":backend.get("home"),"TMPDIR":backend.get("tmp"),"CBM_CACHE_DIR":backend["cache"],"CBM_UPDATE_CHECK":"0"},"scanner":"candidate_lifecycle_v743.py","update_check_policy":"CBM_UPDATE_CHECK=0 is a source-backed, task-neutral disable; active tree permits only pinned cli --tool-server and rejects curl/sh/index workers","zero":candidate_lifecycle.sample_zero(backend,roots)}
 receipt["passed"]=receipt["zero"]["passed"]
 base.save_new(out/(label+".json"),receipt)
 if not receipt["passed"]:raise ValueError("candidate owned process remains, invalid argv, or indeterminate relevant inspection after close")
 return receipt
def backend_cleanup(name,backend,out,label,roots=()):
 if name=="candidate":return candidate_zero(backend,out,label,roots)
 return stop_verify(backend,out,label)
def zero(setup,out,label):
 receipts={}
 for name in BACKENDS:
  if name=="candidate":receipts[name]=candidate_zero(setup["backends"][name],out,label+"-candidate")
  else:
   receipts[name]=status(setup["backends"][name],out,label+"-upstream")
   if not receipts[name]["absent"]:raise ValueError("experiment daemon remains: upstream")
 base.save_new(out/(label+".json"),receipts);return receipts
def stop_verify(backend,out,label):
 before=status(backend,out,label+"-before");stopped=None
 if before["active"]:
  stopped=_ctl(backend,out,label+"-stop","stop")
  if stopped["returncode"]!=0 or not ("daemon: stopping" in stopped["stdout"] or "daemon: not running" in stopped["stdout"]):raise ValueError("supported daemon stop rejected")
 polls=[]
 for i in range(10):
  r=status(backend,out,label+"-post-%02d"%i);polls.append(r)
  if r["absent"]:break
  time.sleep(.25)
 receipt={"before":before,"stop":stopped,"post":polls,"passed":bool(polls and polls[-1]["absent"])};base.save_new(out/(label+".json"),receipt)
 if not receipt["passed"]:raise ValueError("experiment daemon remains after stop")
 return receipt
def interface(backend,tools):
 d={x.get("name"):x for x in tools}
 if backend=="candidate": ok="inspect_symbol" in d and {"from_function","max_work"}.issubset(d.get("trace_path",{}).get("inputSchema",{}).get("properties",{}))
 else: ok={"project","label","name_pattern","format","max_output_tokens"}.issubset(d.get("search_graph",{}).get("inputSchema",{}).get("properties",{})) and {"project","format","max_rows","query"}.issubset(d.get("query_graph",{}).get("inputSchema",{}).get("properties",{}))
 if not ok: raise ValueError("pinned MCP interface audit failed: "+backend)
def _error(label,exc): return {"stage":label,"type":type(exc).__name__,"message":str(exc)}
def scope_unit_v743(role, nonce):
 if role not in ("setup","run") or not re.fullmatch(r"[0-9a-f]{32}",nonce or ""): raise ValueError("v743 role/128-bit nonce invalid")
 return "cbm-v743-"+role+"-"+nonce+".service"
def require_unique_v743_scope(role, cgroup_path=pathlib.Path("/proc/self/cgroup"), environ=None):
 """Require exact environment unit and exact final app.slice component."""
 env=os.environ if environ is None else environ; unit=env.get("CBM_EXPECTED_SCOPE_UNIT")
 match=re.fullmatch(r"cbm-v743-"+re.escape(role)+r"-([0-9a-f]{32})\.service",unit or "")
 if not match or unit!=scope_unit_v743(role,match.group(1) if match else ""): raise ValueError("CBM_EXPECTED_SCOPE_UNIT exact v743 unit required")
 try: lines=cgroup_path.read_bytes().splitlines()
 except OSError as exc: raise ValueError("v743 scope cgroup unreadable: "+type(exc).__name__) from exc
 found=[]
 for line in lines:
  if not line.startswith(b"0::"): continue
  parts=line[3:].split(b"/")
  if len(parts)>=3 and parts[-2]==b"app.slice" and parts[-1]==unit.encode() and line.count(unit.encode())==1: found.append(line)
 if len(found)!=1: raise ValueError("exact final v743 scope cgroup mismatch")
 cgroup=found[0].decode("utf-8","strict")
 return {"schema":"v743-exact-user-systemd-scope-receipt","role":role,"unit":unit,"nonce":match.group(1),"cgroup":cgroup,"cgroup_path":cgroup[3:],"verified":True}
def open_row(backend,setup,out):
 item=setup["backends"][backend];mo=out/("mcp-"+backend);mo.mkdir();client=None;primary=None;teardown=[]
 try:
  client=base.MCPClient(item["binary"],mcp_env(item),mo,str(out.parent));client.transcript=mo/"mcp-transcript.jsonl";interface(backend,client.tools)
  if backend=="candidate":
   roots=(client.proc.pid,);active_scan=candidate_lifecycle.scan(item,roots,active=True)
   if not active_scan["passed"]:raise ValueError("candidate active lifecycle tree rejected")
   owned=active_scan["owned"];own={"backend":backend,"foreground_client_pid":client.proc.pid,"candidate_owned_active":owned,"candidate_lifecycle_active_scan":active_scan,"lifecycle":"foreground stdio process plus supervised descendants; no daemon status/stop contract","isolated_env":{"HOME":item["home"],"TMPDIR":item["tmp"],"CBM_CACHE_DIR":item["cache"]},"tools_list_sha256":hashlib.sha256(json.dumps(client.tools,sort_keys=True).encode()).hexdigest()}
   snap=base.snapshot("foreground-candidate-related",roots);base.save_new(out/"foreground-process-tree.json",{"related_root_pids":list(roots),"snapshot":snap,"candidate_owned":owned});base.save_new(out/"mcp-client-ownership.json",own);return client,own
  active=status(item,out,"daemon-row-active-upstream")
  if not active["active"]:raise ValueError("lazy MCP startup did not expose upstream daemon")
  own={"backend":backend,"foreground_client_pid":client.proc.pid,"daemon_pid":active["pid"],"daemon_status":active,"tools_list_sha256":hashlib.sha256(json.dumps(client.tools,sort_keys=True).encode()).hexdigest()};roots=(client.proc.pid,active["pid"]);snap=base.snapshot("foreground-upstream-related",roots);base.save_new(out/"foreground-process-tree.json",{"related_root_pids":list(roots),"snapshot":snap,"foreign_at_or_above_50_percent_cpu":foreign_cpu([snap],roots)});base.save_new(out/"mcp-client-ownership.json",own);return client,own
 except BaseException as exc:
  primary=exc
  if client is not None:
   try:client.close()
   except BaseException as close_exc:teardown.append(_error("client_close",close_exc))
  try:backend_cleanup(backend,item,out,"backend-startup-failure-cleanup-"+backend,((getattr(getattr(client,"proc",None),"pid",None),) if client else ()))
  except BaseException as cleanup_exc:teardown.append(_error("backend_cleanup",cleanup_exc))
  base.save_new(out/"mcp-startup-failure.json",{"backend":backend,"primary":_error("startup",primary),"teardown_errors":teardown,"zero_check_attempted":True});detail="; ".join(x["stage"]+": "+x["type"]+": "+x["message"] for x in teardown);raise RuntimeError("lazy MCP startup failed: "+type(primary).__name__+": "+str(primary)+("; teardown: "+detail if detail else "")) from primary
def runtime_source_oracle(setup,protocol,out,label): return _runtime_source_oracle(setup,protocol,out,label)
def verify_sealed_manifest(record):
 path=pathlib.Path(record["path"])
 if not path.is_file() or sha256(path)!=record["sha256"]:raise ValueError("sealed external manifest hash drift")
 body=json.loads(path.read_text());schema=body.get("schema")
 allowed={"v726-sealed-external-manifest","v726-preserved-failed-probe-manifest","v726-failed-setup-preservation-manifest","v725-sealed-external-manifest","v725-preserved-failed-probe-manifest","v725-failed-setup-preservation-manifest","v723-sealed-external-manifest","v723-preserved-failed-probe-manifest","v723-failed-setup-preservation-manifest","v724-sealed-external-manifest","v724-preserved-failed-probe-manifest","v724-failed-setup-preservation-manifest","v728-sealed-external-manifest","v728-preserved-failed-probe-manifest","v728-failed-setup-preservation-manifest","v729-sealed-external-manifest","v729-preserved-failed-probe-manifest","v729-failed-setup-preservation-manifest","v743-sealed-external-manifest","v743-preserved-failed-probe-manifest","v743-failed-setup-preservation-manifest","v722-sealed-external-manifest","v722-preserved-failed-probe-manifest","v722-failed-setup-preservation-manifest","v721-sealed-external-manifest","v721-preserved-failed-probe-manifest","v721-failed-setup-preservation-manifest","v720-sealed-external-manifest","v720-preserved-failed-probe-manifest","v720-failed-setup-preservation-manifest","v719-sealed-external-manifest","v719-preserved-failed-probe-manifest","v719-failed-setup-preservation-manifest","v718-sealed-external-manifest","v718-preserved-failed-probe-manifest","v717-sealed-external-manifest","v717-preserved-failed-probe-manifest"}
 allowed.update({"v731-sealed-external-manifest","v731-preserved-failed-probe-manifest","v731-failed-setup-preservation-manifest"})
 allowed.update({"v731-sealed-external-manifest","v731-preserved-failed-probe-manifest","v731-failed-setup-preservation-manifest","v732-sealed-external-manifest","v732-preserved-failed-probe-manifest","v732-failed-setup-preservation-manifest"})
 allowed.update({"v735-codexmon-start-recovery-interface","v735-codexmon-start-recovery-interface","v736-no-model-test-evidence","v738-no-model-test-evidence","v741-no-model-test-evidence","v743-no-model-test-evidence","v738-sealed-external-manifest","v743-no-model-test-evidence"})
 if schema not in allowed or not isinstance(body.get("artifacts"),list) or not body["artifacts"] or len({a.get("path") for a in body["artifacts"]})!=len(body["artifacts"]):raise ValueError("sealed external manifest schema")
 names={a.get("path") for a in body["artifacts"]}
 for item in body["artifacts"]:
  rel=pathlib.PurePosixPath(item.get("path",""))
  if rel.is_absolute() or ".." in rel.parts or str(rel) in {".",""}:raise ValueError("external manifest traversal")
  artifact=path.parent/rel
  try: resolved=artifact.resolve(strict=True)
  except (OSError,RuntimeError):raise ValueError("sealed external artifact unresolved")
  root=path.parent.resolve()
  if artifact.is_symlink() or root not in resolved.parents or not resolved.is_file() or sha256(resolved)!=item.get("sha256"):raise ValueError("sealed external artifact drift")
 if schema in {"v743-preserved-failed-probe-manifest","v731-preserved-failed-probe-manifest","v719-preserved-failed-probe-manifest","v717-preserved-failed-probe-manifest"}:
  expected=body.get("expected_presence")
  if body.get("kind")!="failed_candidate_lifecycle_probe" or body.get("status")!="failed" or not isinstance(expected,dict) or set(expected)!={"lifecycle-report.json"}:raise ValueError("failed probe preservation schema")
  has="lifecycle-report.json" in names
  if has is not expected["lifecycle-report.json"]:raise ValueError("failed probe expected presence mismatch")
  exit_file=path.parent/"exit-code"
  if "exit-code" not in names or not exit_file.is_file() or exit_file.read_text().strip()=="0":raise ValueError("failed probe exit state missing")
  if has:
   report=json.loads((path.parent/"lifecycle-report.json").read_text())
   if report.get("passed") is not False:raise ValueError("failed probe report is not failed")
 if schema in {"v743-failed-setup-preservation-manifest","v731-failed-setup-preservation-manifest"}:
  if body.get("kind")!="failed_setup_runtime" or body.get("status")!="failed" or body.get("expected_absent")!=["setup-runtime-ready.json","SETUP-RUNTIME-READY.sha256"]:raise ValueError("v721 failed setup preservation schema")
  cleanup=json.loads((path.parent/"setup-cleanup.json").read_text());absence=json.loads((path.parent/"ready-absence.json").read_text())
  if cleanup.get("primary",{}).get("type")!="ValueError" or cleanup.get("ready_emitted") is not False or absence.get("observed_absent") is not True:raise ValueError("v721 failed setup preservation content")
 if schema in {"v720-failed-setup-preservation-manifest","v719-failed-setup-preservation-manifest"}:
  if body.get("kind")!="failed_setup_runtime" or body.get("status")!="failed" or body.get("expected_absent")!=["setup-runtime-ready.json","SETUP-RUNTIME-READY.sha256"]:raise ValueError("failed setup preservation schema")
  cleanup=json.loads((path.parent/"setup-cleanup.json").read_text());absence=json.loads((path.parent/"ready-absence.json").read_text())
  if cleanup.get("primary",{}).get("type")!="FileExistsError" or cleanup.get("ready_emitted") is not False or absence.get("observed_absent") is not True:raise ValueError("failed setup preservation content")
 if schema=="v743-sealed-external-manifest" and body.get("kind")=="v731_failed_timed_run":
  expected={"scheduled":64,"attempted":1,"completed":0,"first_row":"10k-lookup-1-shell","state":"controller_failure","agent_exit_code":2}
  if body.get("status")!="failed" or body.get("expected")!=expected or not isinstance(body.get("original_evidence"),list): raise ValueError("v731 timed failure binding schema")
  for original in body["original_evidence"]:
   op=pathlib.Path(original.get("path",""))
   if not op.is_file() or sha256(op)!=original.get("sha256"): raise ValueError("v731 timed failure original evidence drift")
  terminal=json.loads((path.parent/"terminal-status.json").read_text()); report=json.loads((path.parent/"run-report.json").read_text()); final=body.get("final_zero",{})
  if terminal.get("exit_code")!=2 or "codex exited 2" not in terminal.get("error","") or report!={"all_terminal":False,"attempted":1,"completed":0,"correct":0,"scheduled":64} or final.get("sha256")!="d86f01030d7563f1bd7fddda2bcb2cecd23220312d837e2021311d19fa931b91": raise ValueError("v731 timed failure content")
  fp=pathlib.Path(final.get("original_path",""))
  if not fp.is_file() or sha256(fp)!=final["sha256"] or not isinstance(body.get("expected_absent"),list) or len(body["expected_absent"])!=5 or any(pathlib.Path(item).exists() for item in body["expected_absent"]): raise ValueError("v731 timed failure absence/final-zero binding")
 return body
def verify_canonical_external_manifests(setup):
 records=setup.get("canonical_external_manifests",{})
 expected={"active_probe","build","suite","prior_failures","v715_failed_probe_01","v715_failed_probe_02","v715_failed_probe_03","v717_failed_probe_01","v717_failed_probe_02","v718_failed_setup","v719_failed_setup","upstream_status_creates_cache_probe","v721_failed_setup","v722_failed_setup","systemd_scope_cgroup_probe","v723_frozen_no_go","v725_launcher_scope_probe","v725_incomplete_launch_failure","v726_complete_launcher_argv_probes","v724_runner_launch_failed_audit","v727_failed_fixture_probe_order","v728_failed_setup_timeout","candidate_100m_timeout_margin_probe","v730_real_cache_validation","v731_real_ready_cache_seal","v731_failed_timed_run","codexmon_start_recovery_interface","no_model_test_evidence"}
 if set(records)!=expected:raise ValueError("canonical external manifest map drift")
 verified={k:verify_sealed_manifest(v) for k,v in records.items()}
 if verified["v731_failed_timed_run"].get("kind")!="v731_failed_timed_run" or verified["v731_failed_timed_run"].get("status")!="failed" or verified["active_probe"].get("kind")!="candidate_lifecycle_probe" or verified["build"].get("kind")!="candidate_build" or verified["suite"].get("kind")!="correct_cwd_full_suite" or verified["prior_failures"].get("kind")!="prior_required_failures" or verified["v723_frozen_no_go"].get("kind")!="v723_frozen_no_go_preservation" or verified["v724_runner_launch_failed_audit"].get("kind")!="v724_runner_launch_failed_audit_preservation" or verified["v725_incomplete_launch_failure"].get("kind")!="v725_incomplete_command_failure_preservation" or verified["v726_complete_launcher_argv_probes"].get("kind")!="v726_complete_launcher_argv_probes" or verified["v727_failed_fixture_probe_order"].get("kind")!="v727_failed_fixture_probe_order_preservation" or verified["v727_failed_fixture_probe_order"].get("status")!="failed" or verified["v728_failed_setup_timeout"].get("kind")!="v728_failed_setup_timeout_preservation" or verified["v728_failed_setup_timeout"].get("status")!="failed" or verified["candidate_100m_timeout_margin_probe"].get("kind")!="candidate_100m_timeout_margin_probe" or verified["candidate_100m_timeout_margin_probe"].get("status")!="passed" or verified["v730_real_cache_validation"].get("kind")!="v730_real_runtime_cache_validation" or verified["v730_real_cache_validation"].get("status")!="passed" or verified["v731_real_ready_cache_seal"].get("kind")!="v731_real_ready_cache_seal" or verified["v731_real_ready_cache_seal"].get("status")!="passed" or verified["codexmon_start_recovery_interface"].get("kind")!="codexmon_start_recovery_interface" or verified["codexmon_start_recovery_interface"].get("status")!="passed" or verified["no_model_test_evidence"].get("kind")!="no_model_test_evidence" or verified["no_model_test_evidence"].get("status")!="passed" or verified["no_model_test_evidence"].get("results",{}).get("preserved_warning_error",{}).get("tests")!=107 or verified["no_model_test_evidence"].get("results",{}).get("current_warning_error",{}).get("tests")!=21:raise ValueError("canonical external manifest kind drift")
 return verified
def verify_unique_scope_cgroup_probe(setup, verified=None):
 record=setup["canonical_external_manifests"].get("systemd_scope_cgroup_probe")
 body=(verified or verify_canonical_external_manifests(setup)).get("systemd_scope_cgroup_probe")
 if not isinstance(record,dict) or body is None or body.get("schema") not in {"v723-sealed-external-manifest","v724-sealed-external-manifest","v724-preserved-failed-probe-manifest","v724-failed-setup-preservation-manifest","v731-sealed-external-manifest","v743-sealed-external-manifest"} or body.get("kind")!="unique_systemd_scope_cgroup_probe" or body.get("status")!="passed": raise ValueError("unique scope probe binding rejected")
 root=pathlib.Path(record["path"]).parent
 child=(root/"child-cgroup").read_text().strip();parent=(root/"parent-cgroup").read_text().strip();unit=(root/"unit").read_text().strip();observed=json.loads((root/"observed-cgroups.json").read_text())
 version="v723" if body.get("schema")=="v723-sealed-external-manifest" else "v743"
 if ("cbm-"+version+"-scope-probe-" not in child or not child.endswith(".service") or unit not in child or child==parent or (root/"exit-code").read_text().strip()!="0" or observed.get("schema")!=version+"-observed-opaque-cgroups" or set(observed.get("observed",{}))!={"802618","3993244","3993310","3654723"} or "cbm-"+version+"-setup-<unique>" not in observed.get("launch_requirement","")): raise ValueError("unique scope probe content rejected")
 return {"child_cgroup":child,"parent_cgroup":parent,"unit":unit,"observed":observed}
def verify_upstream_status_creates_cache_probe(setup, verified=None):
 """Verify the sealed v720 status receipt before any cache work or timed row."""
 record=setup["canonical_external_manifests"].get("upstream_status_creates_cache_probe")
 if not isinstance(record,dict):raise ValueError("upstream status cache probe binding missing")
 body=(verified or verify_canonical_external_manifests(setup)).get("upstream_status_creates_cache_probe")
 if body is None:body=verify_sealed_manifest(record)
 if body.get("schema")!="v721-sealed-external-manifest" or body.get("kind")!="upstream_status_creates_cache_probe" or body.get("status")!="passed":raise ValueError("upstream status cache probe manifest schema")
 probe_path=pathlib.Path(record["path"]).parent/"probe.json"; exit_path=pathlib.Path(record["path"]).parent/"exit-code"; stdout_path=pathlib.Path(record["path"]).parent/"status.stdout"
 probe=json.loads(probe_path.read_text());expected_env=body.get("expected_env")
 upstream=setup["backends"]["upstream"]
 if (body.get("expected_binary")!=upstream.get("binary") or not isinstance(expected_env,dict) or set(expected_env)!={"CBM_CACHE_DIR"} or expected_env["CBM_CACHE_DIR"]!=probe.get("target_cache") or body.get("expected_status_returncode")!=1 or probe.get("schema")!="v720-upstream-status-cache-creation-probe" or probe.get("binary")!=upstream.get("binary") or probe.get("target_existed_before") is not False or probe.get("target_exists_after") is not True or probe.get("forbidden_pre_copy") is not True or probe.get("status_returncode")!=1 or probe.get("passed") is not True or exit_path.read_text().strip()!="1" or "daemon: not running" not in stdout_path.read_text()):raise ValueError("upstream status cache probe content rejected")
 return {"probe":probe,"expected_env":expected_env,"manifest":body}
def verify_required_preserved_evidence(setup):
 required=setup.get("required_preserved_evidence_manifests",[])
 roles={x.get("role") for x in required};expected={"exact 0aa36e53 build evidence and binary/source equivalence","correct-CWD full suite evidence: 5842 passed, 0 failed, 8 skipped"}
 if roles!=expected:raise ValueError("required preserved evidence role set drift")
 for item in required:
  path=pathlib.Path(item.get("path", ""))
  if not path.is_file() or sha256(path)!=item.get("sha256"):raise ValueError("required preserved evidence manifest drift")
  body=json.loads(path.read_text())
  if body.get("assertion")!=item["role"] or body.get("schema") not in {"v720-preserved-evidence-manifest","v731-preserved-evidence-manifest","v743-preserved-evidence-manifest"}:raise ValueError("required preserved evidence manifest schema drift")
  for entry in body.get("entries",[]):
   ep=pathlib.Path(entry.get("path", ""))
   if not ep.is_file() or sha256(ep)!=entry.get("sha256"):raise ValueError("required preserved evidence input drift")
def verify_candidate_lifecycle(setup):
 external=verify_canonical_external_manifests(setup)
 candidate=setup["backends"]["candidate"]; lifecycle=candidate.get("lifecycle",{})
 if (candidate.get("update_check_disabled") is not True or "daemon_status_identity" in candidate or lifecycle.get("kind")!="foreground_scanner" or any("daemon" in key for key in lifecycle) or lifecycle.get("update_check_env")!="CBM_UPDATE_CHECK=0"):
  raise ValueError("candidate foreground-scanner/update-check binding drift")
 probe_record=setup["canonical_external_manifests"]["active_probe"];probe=pathlib.Path(probe_record["path"]);report=probe.parent/"lifecycle-report.json"
 if not report.is_file():raise ValueError("candidate lifecycle probe report missing")
 r=json.loads(report.read_text());zero=r.get("post_close_zero",{});receipt=r.get("retrieval_receipt",{});active=r.get("active_scan_after_retrieval",{})
 owned_rows=active.get("owned",[]);owned={x.get("pid") for x in owned_rows};root=r.get("launched_mcp_root_pid");binary=str(candidate["binary"])
 roots=[x for x in owned_rows if x.get("pid")==root and x.get("argv")==binary]
 children=[x for x in owned_rows if x.get("ppid")==root and x.get("argv")==binary+" cli --tool-server"]
 if not (r.get("passed") is True and r.get("env",{}).get("CBM_UPDATE_CHECK")=="0" and r.get("initialize_and_tools_list") is True and r.get("exact_retrieval_task")=="10k-lookup-1" and receipt.get("source_oracle_correct") is True and active.get("passed") is True and not active.get("indeterminate") and len(owned)==2 and len(roots)==1 and len(children)==1 and {roots[0].get("pid"),children[0].get("pid")}==owned and zero.get("passed") is True and zero.get("sample_count",0)>=5 and zero.get("elapsed_seconds",0)>=1.0):raise ValueError("candidate lifecycle probe ownership/report rejected")
 for entry in setup.get("preserved_evidence",[]):
  path=pathlib.Path(entry.get("path", ""))
  if not path.is_file() or sha256(path)!=entry.get("sha256"):raise ValueError("sealed preserved evidence drift: "+entry.get("role","unknown"))
def verify_model_cli_contract(setup):
 record=setup.get("model_cli_contract",{})
 path=pathlib.Path(record.get("path",""))
 if not path.is_file() or sha256(path)!=record.get("sha256"): raise ValueError("model CLI contract seal drift")
 body=json.loads(path.read_text())
 if body.get("schema")!="v743-model-cli-contract" or body.get("kind")!="model_cli_contract" or body.get("status")!="passed": raise ValueError("model CLI contract schema")
 cli=body.get("cli",{});host=body.get("host",{});mon=body.get("codexmon",{})
 pins=setup.get("launch_requirements",{}).get("pins",{})
 if cli.get("path")!=pins.get("codex",{}).get("path") or cli.get("sha256")!=pins.get("codex",{}).get("sha256") or host.get("supported_contract")!="--listen only; exec rejected" or mon.get("path")!=pins.get("codexmon",{}).get("path") or body.get("historical_success_count")!=96 or body.get("successful_receipts",{}).get("count")!=96: raise ValueError("model CLI pin/receipt contract")
 sample=json.loads((path.parent/"sample-success-terminal-status.json").read_text()); req=body.get("sample_terminal_requirements",{}); predecessors=["prior_v735_contract","prior_v736_contract","prior_v738_contract","prior_v739_contract","prior_v740_contract"]
 if sample.get("state")!="completed" or sample.get("exit_code")!=0 or sample.get("agent_bin")!=cli.get("path") or req.get("args")!=sample.get("args") or sample.get("args",[])[:4]!=["exec","--ignore-user-config","--ignore-rules","--skip-git-repo-check"]: raise ValueError("model CLI sample terminal contract")
 for field in predecessors:
  prior=body.get(field,{})
  prior_path=pathlib.Path(prior.get("path",""))
  if not prior_path.is_file() or sha256(prior_path)!=prior.get("sha256"): raise ValueError("predecessor model CLI contract drift: "+field)
 for item in body.get("artifacts",[]):
  rel=pathlib.PurePosixPath(item.get("path",""))
  if rel.is_absolute() or ".." in rel.parts or not (path.parent/rel).is_file() or sha256(path.parent/rel)!=item.get("sha256"): raise ValueError("model CLI contract artifact")
 return body
def verify_launch_templates(setup):
 l=setup.get("launch_requirements",{}); pins=l.get("pins",{}); expected={"prepare","runner","codex","codexmon"}
 if l.get("schema")!="v743-complete-executable-launch-template" or set(pins)!=expected: raise ValueError("complete launch pins/schema rejected")
 for item in pins.values():
  p=pathlib.Path(item.get("path",""))
  try: mode=os.lstat(p).st_mode
  except OSError: mode=0
  if not p.is_absolute() or not stat.S_ISREG(mode) or p.is_symlink() or sha256(p)!=item.get("sha256"): raise ValueError("launch pin drift")
  if p.name in ("codex","codexmon") and (item.get("device") != os.lstat(p).st_dev or item.get("inode") != os.lstat(p).st_ino or item.get("mode") != stat.S_IMODE(os.lstat(p).st_mode) or not (os.lstat(p).st_mode & 0o111)): raise ValueError("launch pin identity or executable-mode drift")
 placeholders=l.get("placeholders",{}); out=placeholders.get("output");ready=placeholders.get("ready_setup")
 if out!="/__CBM_V743_OUTPUT__" or ready!="/__CBM_V743_READY_SETUP__": raise ValueError("launch placeholder drift")
 for role,script,required in (("setup","prepare",("--template","--protocol","--output")),("run","runner",("--setup","--protocol","--output","--codex-bin","--codexmon-bin"))):
  row=l.get(role,{});argv=row.get("argv")
  if not isinstance(argv,list) or any(not isinstance(x,str) or "\x00" in x for x in argv): raise ValueError("launch argv invalid")
  nonce="0"*32;unit=scope_unit_v743(role,nonce)
  if "--unit="+unit.replace(nonce,"<nonce>") not in argv or "--setenv=CBM_EXPECTED_SCOPE_UNIT="+unit.replace(nonce,"<nonce>") not in argv or pins[script]["path"] not in argv or any(flag not in argv for flag in required): raise ValueError("launch argv required field missing")
 return l
def verify_received_binaries(setup, codex_bin, codexmon_bin):
 pins=setup.get("launch_requirements",{}).get("pins",{})
 for label,received in (("codex",codex_bin),("codexmon",codexmon_bin)):
  expected=pins.get(label,{})
  path=pathlib.Path(received)
  try: mode=os.lstat(path).st_mode
  except OSError: mode=0
  if not path.is_absolute() or str(path)!=expected.get("path") or not stat.S_ISREG(mode) or path.is_symlink() or sha256(path)!=expected.get("sha256") or expected.get("device") != os.lstat(path).st_dev or expected.get("inode") != os.lstat(path).st_ino or expected.get("mode") != stat.S_IMODE(os.lstat(path).st_mode) or not (os.lstat(path).st_mode & 0o111):
   raise ValueError("received "+label+" binary does not match frozen launch pin")
 return True
def verify_ready_cache_paths(setup,setup_path):
 root=pathlib.Path(setup_path).parent
 scope=json.loads((root/"setup-scope-receipt.json").read_text())
 nonce=scope.get("nonce")
 if not isinstance(nonce,str) or not re.fullmatch(r"[0-9a-f]{32}",nonce):raise ValueError("ready scope nonce invalid")
 expected={b:str((root/"runtime"/nonce/b/"cache").resolve()) for b in BACKENDS}
 actual={b:str(pathlib.Path(setup["backends"][b]["cache"]).resolve()) for b in BACKENDS}
 if actual!=expected:raise ValueError("ready cache root derivation drift")
 seal=setup.get("runtime_cache_seal",{})
 if seal.get("roots") is not None and seal["roots"]!=expected:raise ValueError("ready manifest root binding drift")
 return expected
def verify_setup(setup,protocol,setup_path=None):
 required={"tasks.json","schedule-v743.json","adapter-v743.json","triarm-protocol-v743-final.json","source-setup-triarm-v743.json","source-oracle-pre-v743.json","v743-availability-v6-preservation.json","upstream-100m-failures-v743.json","observed-upstream-lookup-receipt-v743.json"}
 if setup.get("state")!="ready" or not setup.get("task_oracle_passed") or set(setup["protocol_hashes"])!=required: raise ValueError("ready state/protocol set invalid")
 seal_record=setup.get("runtime_cache_seal")
 if not isinstance(seal_record,dict):raise ValueError("runtime cache seal missing")
 verify_ready_cache_paths(setup,setup_path)
 verified=verify_canonical_external_manifests(setup);verify_model_cli_contract(setup);verify_candidate_lifecycle(setup);verify_unique_scope_cgroup_probe(setup,verified);verify_upstream_status_creates_cache_probe(setup,verified);verify_launch_templates(setup)
 for name in BACKENDS:
  item=setup["backends"][name]; expected={"10k","1m","100m"} if name=="candidate" else {"10k","1m"}
  if sha256(pathlib.Path(item["binary"]))!=item["binary_sha256"] or set(item["projects"])!=expected:raise ValueError(name+" binary/project drift")
 for n,h in setup["protocol_hashes"].items():
  if sha256(protocol/n)!=h:raise ValueError("protocol drift: "+n)
 for n,h in setup["harness_hashes"].items():
  if sha256(HERE/n)!=h:raise ValueError("harness drift: "+n)
 pre=pathlib.Path(setup["pre_source_oracle"])
 if sha256(pre)!=setup["pre_source_oracle_sha256"] or json.loads(pre.read_text()).get("passed") is not True:raise ValueError("pre source oracle drift")
 cache_integrity.verify(setup,seal_record.get("path",""),seal_record.get("sha256",""))
 if setup_path is None:raise ValueError("ready path required")
 seal,n=pathlib.Path(setup["ready_seal"]).read_text().strip().split(maxsplit=1)
 if n!=pathlib.Path(setup_path).name or seal!=sha256(pathlib.Path(setup_path)):raise ValueError("ready seal mismatch")
 man=pathlib.Path(setup["sha_manifest"])
 if sha256(man)!=setup["sha_manifest_sha256"]:raise ValueError("manifest drift")
 for line in man.read_text().splitlines():
  h,n=line.split(maxsplit=1)
  if sha256(man.parent/n)!=h:raise ValueError("manifest input drift: "+n)
def run_one(task,row,project,setup,out,codex,codexmon):
 original=base.graph_context;opened={}; primary=None; cleanup=[]; result=None
 try:
  def supplied(*unused):
   if row["arm"]=="shell":
    base.save_new(out/"retrieval-receipt.json",{"backend":"none","wall_seconds":0.0,"context_bytes":0,"source_oracle_correct":None});(out/"delivered-context.txt").write_text("");return "",{"backend":"none","wall_seconds":0.0,"context_bytes":0,"source_oracle_correct":None}
   backend=MCP_ARMS[row["arm"]]
   if opened:raise RuntimeError("MCP arm attempted more than one retrieval")
   client,ownership=open_row(backend,setup,out);opened.update({"backend":backend,"client":client,"ownership":ownership})
   return retrieve_then_scan(task,project,client,backend,setup["backends"][backend],out,getattr(getattr(client,"proc",None),"pid",None))[:2]
  base.graph_context=supplied; bridged={**row,"triarm_arm":row["arm"],"arm":"mcp-context" if row["arm"] in MCP_ARMS else "shell"}
  result=base.run_session(task,bridged,project,setup,out,codex,codexmon,None)
  if row["arm"] in MCP_ARMS and not opened:raise RuntimeError("MCP arm skipped retrieval")
  result["arm"]=row["arm"]
 except BaseException as exc: primary=exc
 finally:
  base.graph_context=original
  if opened:
   try: opened["client"].close()
   except BaseException as close_exc: cleanup.append(_error("client_close",close_exc))
   try: backend_cleanup(opened["backend"],setup["backends"][opened["backend"]],out,"backend-row-cleanup-"+opened["backend"],(getattr(getattr(opened["client"],"proc",None),"pid",None),))
   except BaseException as stop_exc: cleanup.append(_error("backend_cleanup",stop_exc))
  if cleanup:
   base.save_new(out/"mcp-teardown-failure.json",{"primary":_error("task",primary) if primary else None,"teardown_errors":cleanup,"zero_check_attempted":bool(opened)})
 if primary is not None: raise primary
 if cleanup: raise RuntimeError("MCP teardown failed: "+"; ".join(x["stage"]+": "+x["type"]+": "+x["message"] for x in cleanup))
 (out/"summary.json").unlink();base.save_new(out/"summary.json",result);return result
def main():
 p=argparse.ArgumentParser();p.add_argument("--setup",type=pathlib.Path,required=True);p.add_argument("--protocol",type=pathlib.Path,required=True);p.add_argument("--output",type=pathlib.Path,required=True);p.add_argument("--codex-bin",type=pathlib.Path,required=True);p.add_argument("--codexmon-bin",type=pathlib.Path,required=True);p.add_argument("--scope-probe-only",action="store_true");a=p.parse_args()
 if a.scope_probe_only:
  a.output.mkdir(parents=True,exist_ok=False);base.save_new(a.output/"run-scope-receipt.json",require_unique_v743_scope("run"));raise SystemExit("intentional v743 run pre-work probe failure")
 setup=json.loads(a.setup.read_text());verify_setup(setup,a.protocol,a.setup);verify_received_binaries(setup,a.codex_bin,a.codexmon_bin);tasks={x["id"]:x for x in json.loads((a.protocol/"tasks.json").read_text())};schedule=json.loads((a.protocol/"schedule-v743.json").read_text())["rows"];a.output.mkdir(parents=True,exist_ok=False)
 scope=require_unique_v743_scope("run");base.save_new(a.output/"runner-scope-receipt.json",scope)
 pre=source_manifest_audit(setup["sources"]);base.save_new(a.output/"source-manifest-pre.json",pre)
 if not pre["passed"]:raise ValueError("source pre failed")
 runtime_source_oracle(setup,a.protocol,a.output,"runtime-pre");zero(setup,a.output,"daemon-zero-before-schedule");results=[];fatal_survivor=None
 try:
  for row in schedule:
   out=a.output/row["run"];out.mkdir()
   try:
    zero(setup,out,"daemon-zero-before-admission");samples=[]
    for i in range(3):
     samples.append(base.snapshot("admission-"+str(i),()))
     if i<2:time.sleep(1)
    admit=contention_admission(samples,());base.save_new(out/"contention-admission-samples.json",samples);base.save_new(out/"contention-admission.json",admit)
    if not admit["admitted"]:raise ValueError("contention admission rejected")
    base.save_new(out/"contention-pre.json",samples[-1]);backend=MCP_ARMS.get(row["arm"]);project=setup["backends"][backend]["projects"][tasks[row["task"]]["scale"]] if backend else setup["sources"][tasks[row["task"]]["scale"]]
    result=run_one(tasks[row["task"]],row,project,setup,out,a.codex_bin,a.codexmon_bin)
   except base.FatalCodexmonSurvivor as exc:
    fatal={**row,"state":"fatal_codexmon_survivor","error":type(exc).__name__+": "+str(exc),"terminal_observed":False,"answer_correct":False,"accepted_task_wall_seconds":None}
    base.save_new(out/"fatal-survivor.json",fatal)
    raise
   except Exception as exc:
    result={**row,"state":"controller_failure","error":type(exc).__name__+": "+str(exc),"terminal_observed":False,"answer_correct":False};base.save_new(out/"summary.json",result)
   base.save_new(out/"contention-post.json",base.snapshot("task-post"));results.append(result);base.append_json(a.output/"results.jsonl",result);print(json.dumps(result,sort_keys=True),flush=True)
   if not result.get("terminal_observed"):break
  zero(setup,a.output,"daemon-zero-after-schedule");base.save_new(a.output/"cache-integrity-post.json",cache_integrity.verify(setup,setup["runtime_cache_seal"]["path"],setup["runtime_cache_seal"]["sha256"],immutable_only=True));base.save_new(a.output/"run-report.json",{"scheduled":len(schedule),"attempted":len(results),"completed":sum(x.get("state")=="completed" for x in results),"correct":sum(x.get("answer_correct") is True for x in results),"all_terminal":all(x.get("terminal_observed") for x in results)})
 except base.FatalCodexmonSurvivor as exc:
  fatal_survivor=exc
  raise
 finally:
  cleanup_errors=[]
  try:
   post=source_manifest_audit(setup["sources"]);base.save_new(a.output/"source-manifest-post.json",post)
   if not post["passed"] or post!=pre:raise ValueError("source changed")
  except BaseException as exc: cleanup_errors.append(_error("source_cleanup",exc))
  try: runtime_source_oracle(setup,a.protocol,a.output,"runtime-post")
  except BaseException as exc: cleanup_errors.append(_error("source_oracle_cleanup",exc))
  if cleanup_errors:
   base.save_new(a.output/("fatal-survivor-cleanup-errors.json" if fatal_survivor else "post-cleanup-errors.json"),{"primary":_error("fatal_survivor",fatal_survivor) if fatal_survivor else None,"cleanup_errors":cleanup_errors})
   if not fatal_survivor: raise RuntimeError("post-run cleanup failed")
if __name__=="__main__":main()
