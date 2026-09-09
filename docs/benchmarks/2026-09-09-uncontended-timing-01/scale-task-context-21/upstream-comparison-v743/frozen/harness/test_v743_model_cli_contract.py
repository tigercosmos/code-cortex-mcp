import copy, datetime, hashlib, importlib.util, json, os, pathlib, tempfile, time, unittest
ROOT=pathlib.Path(__file__).resolve().parent
BASE=ROOT.parent
def load(name,file):
 spec=importlib.util.spec_from_file_location(name,ROOT/file);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=load("base_v743","base-run-v743.py")
tri=load("tri_v743","triarm_run_v743.py")
CLI="/fake/pinned-codex"; MON="/fake/pinned-codexmon"
TASK={"archetype":"one-shot","symbol":"x","expected":"x","question":"q"}
ROW={"arm":"shell","model":"gpt-5.6-sol","run":"fake"}
def digest(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
class V743ModelCliContract(unittest.TestCase):
 def template(self): return json.loads((BASE/"triarm-setup-template-v743-final.json").read_text())
 def test_contract_exact_history_sample_and_pin(self):
  t=self.template();r=t["model_cli_contract"];p=pathlib.Path(r["path"]);self.assertEqual(digest(p),r["sha256"])
  x=json.loads(p.read_text());self.assertEqual(x["schema"],"v743-model-cli-contract");self.assertEqual(x["historical_success_count"],96);self.assertEqual(x["successful_receipts"]["count"],96)
  sample=json.loads((p.parent/"sample-success-terminal-status.json").read_text());self.assertEqual((sample["state"],sample["exit_code"],sample["agent_bin"]),("completed",0,x["cli"]["path"]));self.assertEqual(x["sample_terminal_requirements"]["args"],sample["args"]);self.assertEqual(sample["args"][:4],["exec","--ignore-user-config","--ignore-rules","--skip-git-repo-check"])
  self.assertEqual(t["launch_requirements"]["pins"]["codex"]["path"],x["cli"]["path"])
 def test_received_pin_rejects_symlink_hash_and_codexmon_mismatch(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d); c=d/"codex";m=d/"codexmon";c.write_bytes(b"codex");m.write_bytes(b"mon");c.chmod(0o755);m.chmod(0o755)
   setup={"launch_requirements":{"pins":{"codex":{"path":str(c),"sha256":digest(c),"device":os.lstat(c).st_dev,"inode":os.lstat(c).st_ino,"mode":os.lstat(c).st_mode & 0o777},"codexmon":{"path":str(m),"sha256":digest(m),"device":os.lstat(m).st_dev,"inode":os.lstat(m).st_ino,"mode":os.lstat(m).st_mode & 0o777}}}}
   self.assertTrue(tri.verify_received_binaries(setup,c,m))
   link=d/"link";link.symlink_to(c);setup["launch_requirements"]["pins"]["codex"]["path"]=str(link)
   with self.assertRaises(ValueError): tri.verify_received_binaries(setup,link,m)
   setup["launch_requirements"]["pins"]["codex"]["path"]=str(c);c.write_bytes(b"tamper")
   with self.assertRaises(ValueError): tri.verify_received_binaries(setup,c,m)
   c.write_bytes(b"codex"); setup["launch_requirements"]["pins"]["codex"]["sha256"]=digest(c); setup["launch_requirements"]["pins"]["codex"]["inode"]=os.lstat(c).st_ino
   with self.assertRaises(ValueError): tri.verify_received_binaries(setup,c,d/"wrong")
 def test_received_pin_rejects_same_content_replacement_inode(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d);c=d/"codex";m=d/"mon";c.write_bytes(b"same");m.write_bytes(b"m");c.chmod(0o755);m.chmod(0o755)
   setup={"launch_requirements":{"pins":{"codex":{"path":str(c),"sha256":digest(c),"device":os.lstat(c).st_dev,"inode":os.lstat(c).st_ino,"mode":os.lstat(c).st_mode & 0o777},"codexmon":{"path":str(m),"sha256":digest(m),"device":os.lstat(m).st_dev,"inode":os.lstat(m).st_ino,"mode":os.lstat(m).st_mode & 0o777}}}}
   clone=d/"clone";clone.write_bytes(b"same");os.replace(clone,c)
   with self.assertRaisesRegex(ValueError,"frozen launch pin"):tri.verify_received_binaries(setup,c,m)
 def test_launch_template_rejects_symlink_pin(self):
  t=copy.deepcopy(self.template())
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d); real=d/"codex";real.write_bytes(pathlib.Path(t["launch_requirements"]["pins"]["codex"]["path"]).read_bytes());link=d/"link";link.symlink_to(real)
   pin=t["launch_requirements"]["pins"]["codex"];old=pin["path"];pin.update({"path":str(link),"sha256":digest(real),"device":os.lstat(real).st_dev,"inode":os.lstat(real).st_ino});t["launch_requirements"]["run"]["argv"]=[str(link) if x==old else x for x in t["launch_requirements"]["run"]["argv"]]
   with self.assertRaises(ValueError):tri.verify_launch_templates(t)
 def call_fake(self,state="completed",exit_code=0,job_bin=CLI,terminal_bin=CLI,start_raw=None,fresh_count=0,cleanup_state="cancelled",cancel_raises=False,cleanup_bad_json=False,status_id=None,fresh_agent="codex",fresh_thresholds=None):
  old_command,old_archive=base.command,base.archive_rollout;calls=[];seen={"status":0};session={"native":None,"cwd":None};baseline=[{"id":"stale","agent":"codex","agent_bin":CLI,"args":["old"],"cwd":"/old","json_mode":True,"thresholds":{"heartbeat_sec":10,"slow_after_sec":30,"stalled_sec":120,"tool_stuck_sec":90,"wall_sec":300}}]
  def fresh():
   return [{"id":"fresh"+str(i),"agent":fresh_agent,"agent_bin":CLI,"args":session["native"],"cwd":session["cwd"],"json_mode":True,"thresholds":(fresh_thresholds if fresh_thresholds is not None else {"heartbeat_sec":10,"slow_after_sec":30,"stalled_sec":120,"tool_stuck_sec":90,"wall_sec":300})} for i in range(fresh_count)]
  def fake(argv,*args,**kwargs):
   calls.append(argv)
   if len(argv)>1 and argv[1]=="list":
    label=args[1] if len(args)>1 else ""
    return json.dumps(baseline if label=="jobs-before-start" else baseline+fresh()),None
   if len(argv)>1 and argv[1]=="start":
    i=argv.index("--");session["native"]=argv[i+1:];session["cwd"]=argv[argv.index("-C")+1]
    raw=start_raw if start_raw is not None else json.dumps({"id":"one","agent_bin":job_bin})
    return raw,None
   if len(argv)>1 and argv[1]=="cancel":
    if cancel_raises: raise OSError("cancel fault")
    return "",None
   if len(argv)>1 and argv[1]=="status":
    seen["status"]+=1
    fresh_status=argv[2].startswith("fresh")
    if cleanup_bad_json and (fresh_status or job_bin!=CLI or seen["status"]>1): return "{bad",None
    if fresh_status or job_bin!=CLI or seen["status"]>1: st,ec,ab=cleanup_state,1,CLI
    else: st,ec,ab=state,exit_code,terminal_bin
    time.sleep(.02);return json.dumps({"id":(status_id or argv[2]),"state":st,"exit_code":ec,"agent_bin":ab,"ended_at":datetime.datetime.now(datetime.timezone.utc).isoformat()}),None
   raise AssertionError(argv)
  try:
   base.command=fake;base.archive_rollout=lambda *a,**k:None
   with tempfile.TemporaryDirectory() as d:
    root=pathlib.Path(d);out=root/"out";out.mkdir()
    try: result=base.run_session(TASK,ROW,{"repo_root":str(root)},{"home":str(root/"home")},out,pathlib.Path(CLI),pathlib.Path(MON),None);error=None
    except Exception as exc: result=None;error=exc
    rejection=json.loads((out/("post-start-failure.json" if (out/"post-start-failure.json").exists() else "start-response-failure.json")).read_text()) if ((out/"post-start-failure.json").exists() or (out/"start-response-failure.json").exists()) else None
    recovery=json.loads((out/"fresh-start-recovery.json").read_text()) if (out/"fresh-start-recovery.json").exists() else None
   return result,error,calls,rejection,recovery
  finally: base.command,base.archive_rollout=old_command,old_archive
 def test_exact_agent_bin_argv_and_completed_zero_accept(self):
  result,error,calls,reject,cleanup=self.call_fake("completed",0);self.assertIsNone(error);self.assertEqual(result["state"],"completed");self.assertIsNone(reject);self.assertIsNone(cleanup)
  starts=[x for x in calls if len(x)>1 and x[1]=="start"];self.assertEqual(len(starts),1);self.assertEqual(starts[0][starts[0].index("--agent-bin")+1],CLI);i=starts[0].index("--");self.assertEqual(starts[0][i+1:i+5],["exec","--ignore-user-config","--ignore-rules","--skip-git-repo-check"])
 def test_every_post_start_rejection_cancels_and_verifies_terminal(self):
  for state,code,agent in [("failed",0,CLI),("completed",None,CLI),("completed",2,CLI),("completed",0,"/wrong")]:
   result,error,calls,reject,recovery=self.call_fake(state,code,terminal_bin=agent);self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertEqual(len([x for x in calls if len(x)>1 and x[1]=="cancel"]),1);self.assertTrue(reject["cleanup"]["cancel_attempted"]);self.assertTrue(reject["cleanup"]["terminal_verified"]);self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_zero_two_and_stale_rollout_reject(self):
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d);out=root/"out";out.mkdir();home=root/"home";home.mkdir()
   with self.assertRaisesRegex(ValueError,"exactly one"):base.archive_rollout(home,out)
   q=home/"sessions"/"a";q.mkdir(parents=True);(q/"one.jsonl").write_text("{}\\n");(q/"two.jsonl").write_text("{}\\n")
   with self.assertRaisesRegex(ValueError,"exactly one"):base.archive_rollout(home,out)
   out2=root/"out2";out2.mkdir();ns=hashlib.sha256(str(out2.parent.resolve()).encode()).hexdigest()[:16];stale=root/"configured"/"sessions"/ns/"fake";stale.mkdir(parents=True)
   with self.assertRaises(FileExistsError):base.run_session(TASK,ROW,{"repo_root":str(root)},{"home":str(root/"configured")},out2,pathlib.Path(CLI),pathlib.Path(MON),None)
 def test_start_response_malformed_missing_and_nonstring_id_recover_exact_fresh_job(self):
  for raw in ("{bad", "{}", "{\"id\":7,\"agent_bin\":\"/fake/pinned-codex\"}"):
   result,error,calls,reject,recovery=self.call_fake(start_raw=raw,fresh_count=1)
   self.assertIsNone(result);self.assertIsNotNone(error);self.assertEqual(recovery["fresh_ids"],["fresh0"]);self.assertEqual(recovery["matching_ids"],["fresh0"]);self.assertEqual(recovery["cancelled"],["fresh0"]);self.assertEqual(len([x for x in calls if len(x)>1 and x[1]=="cancel"]),1);self.assertIsNotNone(reject);self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_start_response_zero_or_multiple_fresh_candidates_fail_closed(self):
  for count,expected_cancels in ((0,0),(2,2)):
   result,error,calls,reject,recovery=self.call_fake(start_raw="{bad",fresh_count=count)
   self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertEqual(len(recovery["matching_ids"]),count);self.assertEqual(len([x for x in calls if len(x)>1 and x[1]=="cancel"]),expected_cancels);self.assertIsNotNone(reject);self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_start_recovery_cleanup_running_preserves_primary_and_fails_closed(self):
  result,error,calls,reject,recovery=self.call_fake(start_raw="{bad",fresh_count=1,cleanup_state="running")
  self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertEqual(reject["primary"]["type"],"JSONDecodeError");self.assertTrue(recovery["errors"]);self.assertEqual(recovery["terminal"]["fresh0"]["state"],"running")
 def test_received_pin_rejects_chmod_nonexecutable(self):
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d);c=d/"codex";m=d/"mon";c.write_bytes(b"c");m.write_bytes(b"m");c.chmod(0o755);m.chmod(0o755)
   setup={"launch_requirements":{"pins":{"codex":{"path":str(c),"sha256":digest(c),"device":os.lstat(c).st_dev,"inode":os.lstat(c).st_ino,"mode":os.lstat(c).st_mode & 0o777},"codexmon":{"path":str(m),"sha256":digest(m),"device":os.lstat(m).st_dev,"inode":os.lstat(m).st_ino,"mode":os.lstat(m).st_mode & 0o777}}}}
   c.chmod(0o644)
   with self.assertRaises(ValueError):tri.verify_received_binaries(setup,c,m)

 def test_post_start_cleanup_cancel_and_status_faults_are_recorded(self):
  for kwargs in ({"cancel_raises":True},{"cleanup_bad_json":True}):
   result,error,calls,reject,recovery=self.call_fake(state="failed",exit_code=0,**kwargs)
   self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertIsNotNone(reject);self.assertTrue(reject["cleanup"]["errors"]);self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_bound_list_status_interface_evidence_has_required_contract(self):
  t=self.template();r=t["canonical_external_manifests"]["codexmon_start_recovery_interface"];p=pathlib.Path(r["path"]);self.assertEqual(digest(p),r["sha256"]);x=json.loads(p.read_text());self.assertEqual(x["schema"],"v735-codexmon-start-recovery-interface");self.assertEqual(x["contract"]["list_argv"],["list","--json"]);self.assertEqual(x["contract"]["status_argv"],["status","<id>","--json"]);self.assertIn("id",x["observed"]["sample_keys"])

 def test_recovery_requires_codex_agent_and_full_threshold_object(self):
  exact={"heartbeat_sec":10,"slow_after_sec":30,"stalled_sec":120,"tool_stuck_sec":90,"wall_sec":300}
  for kwargs in ({"fresh_agent":"cursor"},{"fresh_thresholds":{**exact,"slow_after_sec":31}},{"fresh_thresholds":{k:v for k,v in exact.items() if k!="heartbeat_sec"}}):
   result,error,calls,reject,recovery=self.call_fake(start_raw="{bad",fresh_count=1,**kwargs)
   self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertEqual(recovery["matching_ids"],[]);self.assertIsNotNone(reject)
 def test_normal_and_cleanup_status_id_mismatch_fail_closed_with_survivor_receipt(self):
  result,error,calls,reject,recovery=self.call_fake(status_id="wrong")
  self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertEqual(reject["reason"],"status-rejection");self.assertFalse(reject["cleanup"]["terminal_verified"]);self.assertIsNotNone(reject["cleanup"]["survivor_evidence"]);self.assertEqual(len(reject["cleanup"]["status_attempts"]),3)
 def test_known_cleanup_running_has_three_bounded_attempts_and_no_accepted_time(self):
  result,error,calls,reject,recovery=self.call_fake(state="failed",exit_code=0,cleanup_state="running")
  self.assertIsNone(result);self.assertIsInstance(error,RuntimeError);self.assertFalse(reject["cleanup"]["terminal_verified"]);self.assertEqual(len(reject["cleanup"]["status_attempts"]),3);self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_fatal_survivor_contract_skips_normal_schedule_success_artifacts(self):
  source=(ROOT/"triarm_run_v743.py").read_text();base_source=(ROOT/"base-run-v743.py").read_text()
  self.assertIn("class FatalCodexmonSurvivor",base_source);self.assertIn("if not cleanup.get(\"terminal_verified\")",base_source);self.assertIn("except base.FatalCodexmonSurvivor as exc",source);self.assertIn("fatal-survivor.json",source)
  fatal={"state":"fatal_codexmon_survivor","accepted_task_wall_seconds":None};self.assertFalse(fatal.get("terminal_observed",False));self.assertNotIn("run-report.json",fatal);self.assertNotIn("cache-integrity-post.json",fatal)
 def test_malformed_start_matching_running_job_is_fatal_survivor(self):
  result,error,calls,reject,recovery=self.call_fake(start_raw="{bad",fresh_count=1,cleanup_state="running")
  self.assertIsNone(result);self.assertIsInstance(error,base.FatalCodexmonSurvivor);self.assertIsNotNone(reject);self.assertEqual(reject["reason"],"start-response-rejection");self.assertEqual(recovery["terminal"]["fresh0"]["state"],"running");self.assertIsNone(reject["accepted_task_wall_seconds"])
 def test_exact_v738_failed_timed_manifest_schema_is_accepted(self):
  t=self.template();r=t["canonical_external_manifests"]["v731_failed_timed_run"];self.assertEqual(json.loads(pathlib.Path(r["path"]).read_text())["schema"],"v738-sealed-external-manifest");self.assertEqual(tri.verify_sealed_manifest(r)["kind"],"v731_failed_timed_run")
 def test_planned_verifier_contains_exact_harness_map_crosschecks(self):
  source=(ROOT/"prepare_runtime_v743.py").read_text();self.assertIn("protocol/template harness map drift",source);self.assertIn("manifest/template harness map drift",source)
 def test_ready_shaped_contract_verifier_has_no_stale_v734_reference(self):
  t=self.template();self.assertTrue(tri.verify_model_cli_contract(t));source=(ROOT/"triarm_run_v743.py").read_text();self.assertNotIn("prior_v734_contract",source)
