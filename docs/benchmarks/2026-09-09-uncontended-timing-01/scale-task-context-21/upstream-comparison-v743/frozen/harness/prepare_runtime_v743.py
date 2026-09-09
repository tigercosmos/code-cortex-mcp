#!/usr/bin/env python3
"""v743 availability setup: copy verified v6 usable caches, 40 retrieval checks, no indexing/models."""
import argparse, hashlib, importlib.util, json, pathlib, shutil, os
import cache_integrity_v743 as cache_integrity
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("triarm_run_v743", HERE/"triarm_run_v743.py")
tri=importlib.util.module_from_spec(spec); spec.loader.exec_module(tri)
def digest(path):
    with open(path,"rb") as f: return hashlib.file_digest(f,"sha256").hexdigest()
def save(path,value):
    with path.open("x") as f: json.dump(value,f,indent=2,sort_keys=True); f.write("\n")
def verify_records(records,label):
    checked=[]
    for r in records:
        p=pathlib.Path(r["source"])
        if not p.is_file() or digest(p)!=r["sha256"]: raise ValueError(label+" drift: "+str(p))
        checked.append({"source":str(p),"destination":r["destination"],"role":r["role"],"sha256":r["sha256"],"bytes":p.stat().st_size})
    return checked
def load_observed_lookup_receipt(record):
    path=pathlib.Path(record["path"])
    if not path.is_file() or digest(path)!=record["sha256"]:
        raise ValueError("bound observed upstream receipt drift")
    hits=[]
    for line in path.read_text().splitlines():
        event=json.loads(line)
        message=event.get("message",{})
        if (event.get("direction")=="server_to_client" and message.get("id")==3 and
                isinstance(message.get("result",{}).get("structuredContent"),dict)):
            hits.append(message["result"]["structuredContent"])
    if len(hits)!=1:
        raise ValueError("bound observed upstream receipt shape missing")
    body=hits[0]
    if (body.get("cols")!=["name","label","lines","in","out"] or body.get("count")!=1 or
            not isinstance(body.get("groups"),list) or len(body["groups"])!=1 or
            body["groups"][0].get("rows")!=[["capacity_function_192","Function","601-650",1,1]]):
        raise ValueError("bound observed upstream receipt content mismatch")
    return body
def verify_planned(t,protocol):
    if t.get("state")!="planned" or t.get("model_sessions")!=0: raise ValueError("planned no-model state required")
    if t["availability"]["upstream_tasks"]!=16 or "100m" not in t["availability"]["upstream_excluded_scales"]: raise ValueError("upstream 100m exclusion drift")
    for n,h in t["protocol_hashes"].items():
        if digest(protocol/n)!=h: raise ValueError("protocol drift: "+n)
    for n,h in t["harness_hashes"].items():
        if digest(HERE/n)!=h: raise ValueError("harness drift: "+n)
    embedded=json.loads((protocol/"triarm-protocol-v743-final.json").read_text()).get("harness_hashes",{})
    if embedded != t["harness_hashes"]: raise ValueError("protocol/template harness map drift")
    manifest_rows={line.split(maxsplit=1)[1]:line.split(maxsplit=1)[0] for line in pathlib.Path(t["sha_manifest"]).read_text().splitlines()}
    for n,h in t["harness_hashes"].items():
        if manifest_rows.get("harness/"+n)!=h: raise ValueError("manifest/template harness map drift")
    manifest=pathlib.Path(t["sha_manifest"])
    if digest(manifest)!=t["sha_manifest_sha256"]: raise ValueError("SHA256SUMS drift")
    for line in manifest.read_text().splitlines():
        h,n=line.split(maxsplit=1)
        if digest(manifest.parent/n)!=h: raise ValueError("manifest input drift: "+n)
    pre=pathlib.Path(t["pre_source_oracle"])
    if not pre.is_file() or digest(pre)!=t["pre_source_oracle_sha256"] or json.loads(pre.read_text()).get("passed") is not True:
        raise ValueError("sealed pre source oracle drift")
    if not pathlib.Path(t["source_setup"]).is_file():
        raise ValueError("source inventory missing")
    load_observed_lookup_receipt(t["observed_upstream_lookup_receipt"])
    verified=tri.verify_canonical_external_manifests(t);tri.verify_candidate_lifecycle(t);tri.verify_unique_scope_cgroup_probe(t,verified);tri.verify_upstream_status_creates_cache_probe(t,verified);tri.verify_launch_templates(t)
def upstream_target_pre_copy_guard(backend,out,label,proc_root=pathlib.Path("/proc"),stat_fn=os.stat,environ_reader=None,after_scan=None,metadata_reader=None,cgroup_reader=None,current_cgroup=None,scope=None):
 """Non-adversarial same-user guard: exact current scope is fail-closed; external scope is evidence only."""
 target=pathlib.Path(backend["cache"])
 if target.exists() or target.is_symlink(): raise FileExistsError("target cache exists before copy: "+str(target))
 if environ_reader is None: environ_reader=lambda p:p.read_bytes()
 if metadata_reader is None: metadata_reader=lambda p:p.read_bytes()
 if cgroup_reader is None: cgroup_reader=lambda p:p.read_bytes()
 current=(scope["cgroup"].encode() if scope else current_cgroup)
 if current is None:
  current=b"0::/user.slice/user-1001.slice/user@1001.service/app.slice/cbm-v743-setup-"+b"0"*32+b".service"
 ident=stat_fn(backend["binary"]);uid=os.getuid();matches=[];ind=[];same=[];external=[]
 for child in proc_root.iterdir():
  if not child.name.isdigit(): continue
  pid=child.name
  try:
   if child.stat().st_uid!=uid: continue
   st0=metadata_reader(child/"stat");c0=cgroup_reader(child/"cgroup");st1=metadata_reader(child/"stat");c1=cgroup_reader(child/"cgroup")
  except FileNotFoundError: ind.append({"pid":pid,"stage":"metadata_or_cgroup","error":"FileNotFoundError"});continue
  except OSError as exc: ind.append({"pid":pid,"stage":"metadata_or_cgroup","error":type(exc).__name__});continue
  if not st0 or st0!=st1 or not c0 or c0!=c1: ind.append({"pid":pid,"stage":"stable_identity","error":"starttime_or_cgroup_changed"});continue
  in_scope=c0.strip()==current.strip()
  try: exe=stat_fn(child/"exe")
  except FileNotFoundError:
   try: stat_fn(child/"exe")
   except FileNotFoundError: continue
   except OSError as exc: ind.append({"pid":pid,"stage":"exe_recheck","error":type(exc).__name__})
   continue
  except OSError as exc:
   if in_scope: ind.append({"pid":pid,"stage":"same_scope_exe","error":type(exc).__name__})
   else: external.append({"pid":int(pid),"classification":"unknown_external_scope","error":type(exc).__name__})
   continue
  exact=(exe.st_dev,exe.st_ino)==(ident.st_dev,ident.st_ino)
  if not in_scope and not exact:
   external.append({"pid":int(pid),"classification":"external_scope"});continue
  try: env=environ_reader(child/"environ").split(b"\0")
  except FileNotFoundError: continue
  except OSError as exc:
   ind.append({"pid":pid,"stage":"same_scope_environ" if in_scope else "external_exact_binary_environ","error":type(exc).__name__});continue
  target_match=b"CBM_CACHE_DIR="+str(target).encode() in env
  if target_match: matches.append(int(pid))
  if in_scope: same.append({"pid":int(pid),"exact_upstream_binary":exact,"target_match":target_match})
  else: external.append({"pid":int(pid),"classification":"external_exact_upstream_binary","target_match":target_match})
 if after_scan: after_scan(target)
 created=target.exists() or target.is_symlink()
 r={"schema":"v743-upstream-target-precopy-scope-scan","target":str(target),"target_existed":False,"target_created_during_scan":created,"scope_cgroup":current.decode("utf-8","replace"),"same_scope_processes":same,"same_uid_exact_binary_target_pids":matches,"indeterminate":ind,"external_scope":external,"controlled_non_adversarial_same_user_host_assumption":True,"target_cli_invoked":False,"source_cache_cli_invoked":False,"passed":not created and not matches and not ind}
 save(out/(label+".json"),r)
 if not r["passed"]: raise ValueError("upstream target pre-copy process scan rejected")
 return r

def copy_verified(backend,records,evidence):
    cache=pathlib.Path(backend["cache"]); cache.mkdir(parents=True,exist_ok=False)
    before=verify_records(records,"v6 preservation pre-copy"); copied=[]
    for r in records:
        dst=cache/r["destination"]; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(r["source"],dst)
        if digest(dst)!=r["sha256"]: raise ValueError("copied hash mismatch: "+str(dst))
        copied.append({"source":r["source"],"destination":str(dst),"role":r["role"],"sha256":r["sha256"],"bytes":dst.stat().st_size})
    after=verify_records(records,"v6 preservation post-copy")
    if before!=after: raise ValueError("v6 changed during copy")
    save(evidence/"copied-db-records.json",{"source_verified_before":before,"copied_hash_verified":copied,"source_verified_after":after})
    return copied
def audit(t,path,expected,label):
    report=tri.source_manifest_audit(t["sources"]); save(path,report)
    if not report["passed"] or report!=expected: raise ValueError(label+" changed source")
def _err(stage,exc): return {"stage":stage,"type":type(exc).__name__,"message":str(exc)}
def _raise(primary,cleanup):
 details="; ".join(x["stage"]+": "+x["type"]+": "+x["message"] for x in cleanup)
 if primary is not None: raise RuntimeError("setup failed: "+type(primary).__name__+": "+str(primary)+("; cleanup: "+details if details else "")) from primary
 if cleanup: raise RuntimeError("setup cleanup failed: "+details)
def retrieval(name,backend,env,tasks,evidence):
 out=evidence/name/"exposure-oracle";out.mkdir(parents=True);client=None;root_pid=None;primary=None;cleanup=[];results=None
 try:
  client=tri.base.MCPClient(backend["binary"],env,out,str(evidence));root_pid=client.proc.pid;client.transcript=out/"mcp-transcript.jsonl"
  required=("inspect_symbol","trace_path") if name=="candidate" else ("search_graph","query_graph")
  if not all(x in {v.get("name") for v in client.tools} for x in required):raise ValueError(name+" exposure missing")
  results=[]
  for task in tasks:
   taskout=out/task["id"];taskout.mkdir();context,receipt,_=tri.retrieve_then_scan(task,backend["projects"][task["scale"]],client,name,backend,taskout,root_pid)
   results.append({"task":task["id"],"context_sha256":hashlib.sha256(context.encode()).hexdigest(),"context_bytes":len(context.encode()),"source_oracle_correct":receipt["source_oracle_correct"],"receipt":receipt})
  save(out/"tools-list.json",client.tools)
 except BaseException as exc:primary=exc
 finally:
  if client is not None:
   try:client.close()
   except BaseException as exc:cleanup.append(_err("client_close",exc))
  try:tri.backend_cleanup(name,backend,out,"backend-setup-cleanup-"+name,(root_pid,) if root_pid else ())
  except BaseException as exc:cleanup.append(_err("backend_cleanup",exc))
  if primary is not None or cleanup:save(out/"retrieval-cleanup.json",{"primary":_err("retrieval",primary) if primary else None,"cleanup_errors":cleanup,"zero_check_attempted":True})
 if primary is not None or cleanup:_raise(primary,cleanup)
 return results
ORACLE_SCHEMA={"task","context_sha256","context_bytes","source_oracle_correct","receipt"}
def validate_oracles(candidate,upstream,tasks):
 candidate_ids={t["id"] for t in tasks};upstream_ids={t["id"] for t in tasks if t["scale"] in ("10k","1m")}
 def keyed(entries,expected,label):
  if len(entries)!=len(expected):raise ValueError(label+" oracle count mismatch")
  result={}
  for entry in entries:
   if set(entry)!=ORACLE_SCHEMA or entry["task"] in result or entry["task"] not in expected:raise ValueError(label+" oracle schema/task mismatch")
   if not isinstance(entry["context_bytes"],int) or entry["context_bytes"]<=0 or entry["source_oracle_correct"] is not True:raise ValueError(label+" oracle context invalid")
   result[entry["task"]]=entry
  if set(result)!=expected:raise ValueError(label+" oracle task set mismatch")
  return result
 c=keyed(candidate,candidate_ids,"candidate");u=keyed(upstream,upstream_ids,"upstream");compared=[]
 for task in sorted(upstream_ids):
  if c[task]["context_sha256"]!=u[task]["context_sha256"] or c[task]["context_bytes"]!=u[task]["context_bytes"]:raise ValueError("shared normalized context mismatch: "+task)
  compared.append({"task":task,"context_sha256":c[task]["context_sha256"],"context_bytes":c[task]["context_bytes"]})
 return {"candidate_tasks":len(c),"upstream_tasks":len(u),"shared_tasks":len(compared),"shared_contexts":compared}
def derive_runtime_paths(t,output,scope=None):
    """Assign fresh, output-contained backend homes/caches before any copy."""
    output=pathlib.Path(output)
    if output.is_symlink() or not output.is_dir(): raise ValueError("fresh output directory required")
    nonce=(scope or {"nonce":"0"*32})["nonce"]
    runtime=output/"runtime"/nonce
    if runtime.exists() or runtime.is_symlink(): raise FileExistsError("fresh output runtime path required")
    if output.resolve() not in runtime.resolve().parents: raise ValueError("runtime path escapes output")
    for name in tri.BACKENDS:
        root=runtime/name
        if root.is_symlink() or root.exists(): raise FileExistsError("fresh backend runtime path required: "+name)
        b=t["backends"][name]
        b["cache"]=str(root/"cache");b["home"]=str(root/"home");b["tmp"]=str(root/"tmp")
    return runtime
def emit_ready(ready,output):
 rp=output/"setup-runtime-ready.json";seal=output/"SETUP-RUNTIME-READY.sha256";pending=output/"setup-runtime-ready.pending.json";pending_seal=output/"SETUP-RUNTIME-READY.pending.sha256";ready["ready_seal"]=str(seal);save(pending,ready);pending_seal.write_text(digest(pending)+"  "+rp.name+"\n");os.replace(pending,rp);os.replace(pending_seal,seal);return rp,seal
def execute(t,protocol,output):
 if output.exists():raise FileExistsError("output must be new")
 output.mkdir(parents=True);scope=tri.require_unique_v743_scope("setup");save(output/"setup-scope-receipt.json",scope);runtime=derive_runtime_paths(t,output,scope);evidence=output/"evidence";evidence.mkdir();save(output/"runtime-path-policy.json",{"schema":"v743-output-contained-runtime","output":str(output.resolve()),"runtime":str(runtime.resolve()),"nonce":scope["nonce"],"scope_unit":scope["unit"],"backends":{name:{k:t["backends"][name][k] for k in ("cache","home","tmp")} for name in tri.BACKENDS}});backend_out={name:evidence/name for name in tri.BACKENDS}
 for path in backend_out.values():path.mkdir()
 primary=None;cleanup=[];ready=None;upstream_target_copy_complete=False
 try:
  verify_planned(t,protocol);candidate_pre=tri.candidate_zero(t["backends"]["candidate"],output,"daemon-zero-before-cache-candidate");upstream_pre=upstream_target_pre_copy_guard(t["backends"]["upstream"],output,"daemon-zero-before-cache-upstream-proc",scope=scope);save(output/"daemon-zero-before-cache.json",{"candidate":candidate_pre,"upstream_target_proc_scan":upstream_pre})
  source_pre=tri.source_manifest_audit(t["sources"]);save(output/"source-manifest-pre.json",source_pre)
  if not source_pre["passed"]:raise ValueError("pre source rejected")
  v6pre=verify_records(t["v6_preservation_records"],"v6 preservation pre-setup");save(output/"v6-preservation-pre.json",v6pre);copied={}
  for name in tri.BACKENDS:
   b=t["backends"][name];pathlib.Path(b["home"]).mkdir(parents=True,exist_ok=False);pathlib.Path(b["tmp"]).mkdir(parents=True,exist_ok=False);out=backend_out[name]
   if name=="upstream":upstream_target_pre_copy_guard(b,output,"daemon-zero-immediately-before-upstream-atomic-copy",scope=scope)
   copied[name]=copy_verified(b,t["v6_copy_records"][name],out);audit(t,out/"source-after-copy.json",source_pre,name+" copy")
  upstream_target_copy_complete=True
  tri.zero(t,output,"daemon-zero-after-copy-before-target-cli")
  tasks=json.loads((protocol/"tasks.json").read_text());upstream=[x for x in tasks if x["scale"] in ("10k","1m")]
  if len(upstream)!=16:raise ValueError("upstream availability tasks must be 16")
  cenv=tri.mcp_env(t["backends"]["candidate"]);cenv.update({"CBM_WORKERS":"4","CBM_MEM_BUDGET_MB":"4096","CBM_CACHE_DIR":t["backends"]["candidate"]["cache"],"CBM_RESULT_STORE":"1","CBM_MEM_PROFILE":"1"})
  uenv=tri.mcp_env(t["backends"]["upstream"]);uenv.update({"CBM_WORKERS":"4","CBM_MEM_BUDGET_MB":"4096","CBM_CACHE_DIR":t["backends"]["upstream"]["cache"]})
  coracle=retrieval("candidate",t["backends"]["candidate"],cenv,tasks,evidence);uoracle=retrieval("upstream",t["backends"]["upstream"],uenv,upstream,evidence);comparison=validate_oracles(coracle,uoracle,tasks)
  tri.zero(t,output,"daemon-zero-before-ready");audit(t,output/"source-manifest-post.json",source_pre,"setup");v6final=verify_records(t["v6_preservation_records"],"v6 preservation final")
  if v6pre!=v6final:raise ValueError("v6 preservation changed")
  ready={**t,"state":"ready","task_oracle_passed":True,"model_sessions":0,"index_and_exposure":{"candidate":{"copied_indexes":copied["candidate"],"retrieval_oracle":coracle},"upstream":{"copied_indexes":copied["upstream"],"retrieval_oracle":uoracle},"shared_context_gate":comparison},"v6_preservation_verified_after_setup":v6final,"source_manifest_pre_sha256":digest(output/"source-manifest-pre.json"),"source_manifest_post_sha256":digest(output/"source-manifest-post.json"),"smoke_gate":{"passed":True,"checks":["verified v6 usable cache hashes before/after copy","candidate 24 plus upstream 16 normalized retrieval checks","upstream 100m excluded after sealed 4GiB/32GiB failures"]}}
 except BaseException as exc:primary=exc
 finally:
  for name in tri.BACKENDS:
   if name=="upstream" and not upstream_target_copy_complete:
    save(backend_out[name]/"backend-setup-final-upstream-precopy-skip.json",{"schema":"v743-upstream-precopy-cleanup-skip","reason":"target cache copy did not complete; no upstream CLI may touch target","target_cli_invoked":False})
    continue
   try:tri.backend_cleanup(name,t["backends"][name],backend_out[name],"backend-setup-final-"+name)
   except BaseException as exc:cleanup.append(_err("final_backend_cleanup_"+name,exc))
  try:
   if upstream_target_copy_complete:tri.zero(t,output,"daemon-zero-final")
   else:tri.candidate_zero(t["backends"]["candidate"],output,"candidate-zero-final-precopy")
  except BaseException as exc:cleanup.append(_err("final_zero",exc))
  if primary is not None or cleanup:save(output/"setup-cleanup.json",{"primary":_err("setup",primary) if primary else None,"cleanup_errors":cleanup,"ready_emitted":False,"upstream_target_copy_complete":upstream_target_copy_complete})
 _raise(primary,cleanup)
 return ready
def run(t,protocol,output):
 ready=execute(t,protocol,output)
 # All backend cleanup and final zero gates completed in execute finally.
 cache_seal_path=output/"runtime-cache-manifest.json";cache_seal=cache_integrity.write(ready,cache_seal_path)
 ready["runtime_cache_seal"]={"path":str(cache_seal_path),"sha256":digest(cache_seal_path),"entry_count":cache_seal["entry_count"],"aggregate_sha256":cache_seal["aggregate_sha256"],"sealed_after_cleanup_final_zero":True}
 return emit_ready(ready,output)
def main():
 ap=argparse.ArgumentParser();ap.add_argument("--template",type=pathlib.Path,required=True);ap.add_argument("--protocol",type=pathlib.Path,required=True);ap.add_argument("--output",type=pathlib.Path,required=True);ap.add_argument("--scope-probe-only",action="store_true");a=ap.parse_args()
 if a.scope_probe_only:
  a.output.mkdir(parents=True,exist_ok=False);save(a.output/"setup-scope-receipt.json",tri.require_unique_v743_scope("setup"));raise SystemExit("intentional v743 setup pre-work probe failure")
 t=json.loads(a.template.read_text());run(t,a.protocol,a.output);print(json.dumps({"state":"ready","setup":str(a.output/"setup-runtime-ready.json"),"seal":str(a.output/"SETUP-RUNTIME-READY.sha256")}))
if __name__=="__main__":main()
