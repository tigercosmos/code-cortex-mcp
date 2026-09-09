#!/usr/bin/env python3
"""v7.31 candidate foreground lifecycle scanner; no daemon IPC or killing."""
import errno, os, time

def binary_identity(path):
 st=os.stat(path);return {"device":st.st_dev,"inode":st.st_ino}
def _read_stat(pid):
 with open(f"/proc/{pid}/stat") as f: raw=f.read()
 tail=raw.rsplit(")",1)[1].split()
 if len(tail)<20:raise ValueError("malformed proc stat")
 return {"ppid":int(tail[1]),"starttime":int(tail[19])}
def parse_nul_fields(raw,kind):
 if not isinstance(raw,(bytes,bytearray)) or not raw:raise ValueError("empty proc "+kind)
 # Linux procfs uses actual NUL bytes, never the two-byte spelling backslash-zero.
 fields=bytes(raw).split(b"\x00")
 if fields[-1]==b"":fields=fields[:-1]
 if not fields or any(not x for x in fields):raise ValueError("malformed proc "+kind)
 return fields
def parse_cmdline_bytes(raw):
 return " ".join(x.decode("utf-8","strict") for x in parse_nul_fields(raw,"cmdline"))
def parse_environ_bytes(raw):
 return parse_nul_fields(raw,"environ")
def _read_cmdline(pid):
 with open(f"/proc/{pid}/cmdline","rb") as f: return parse_cmdline_bytes(f.read())
def proc_rows(uid=None):
 uid=os.getuid() if uid is None else uid; rows=[]; errors=[]
 for name in os.listdir("/proc"):
  if not name.isdigit():continue
  pid=int(name)
  try:
   st=os.stat(f"/proc/{pid}")
   if st.st_uid!=uid:continue
   stat=_read_stat(pid);argv=_read_cmdline(pid)
   rows.append({"pid":pid,"ppid":stat["ppid"],"starttime":stat["starttime"],"argv":argv,"uid":uid})
  except FileNotFoundError:continue
  except ProcessLookupError:continue
  except (PermissionError,OSError,ValueError) as exc:errors.append({"pid":pid,"error":type(exc).__name__+": "+str(exc)})
 return {"rows":rows,"enumeration_errors":errors,"uid":uid}
def _absent_twice(pid,starttime):
 """ENOENT/ESRCH is a race only after a second confirmed absence."""
 try:
  later=_read_stat(pid)
 except (FileNotFoundError,ProcessLookupError):return {"state":"vanished_confirmed","pid":pid,"starttime":starttime}
 except (PermissionError,OSError,ValueError) as exc:return {"state":"indeterminate","reason":"absence_confirmation_error","error":str(exc)}
 if later["starttime"]!=starttime:return {"state":"indeterminate","reason":"pid_reused_or_starttime_changed","after_starttime":later["starttime"]}
 return {"state":"indeterminate","reason":"transient_proc_read_error_same_pid","after_starttime":later["starttime"]}
def inspect_row(row,identity,cache):
 pid=row["pid"]; start=row["starttime"]
 try: exe=os.stat(f"/proc/{pid}/exe")
 except (FileNotFoundError,ProcessLookupError):return _absent_twice(pid,start)
 except (PermissionError,OSError) as exc:return {"state":"indeterminate","reason":"exe_read_error","error":str(exc)}
 if (exe.st_dev,exe.st_ino)!=(identity["device"],identity["inode"]):return {"state":"other_binary"}
 try:
  stat_after=_read_stat(pid)
 except (FileNotFoundError,ProcessLookupError):return _absent_twice(pid,start)
 except (PermissionError,OSError,ValueError) as exc:return {"state":"indeterminate","candidate_identity":True,"reason":"stat_after_exe_error","error":str(exc)}
 if stat_after["starttime"]!=start:return {"state":"indeterminate","reason":"pid_reused_or_starttime_changed","after_starttime":stat_after["starttime"]}
 try:
  with open(f"/proc/{pid}/environ","rb") as f: env=parse_environ_bytes(f.read())
 except (FileNotFoundError,ProcessLookupError):return _absent_twice(pid,start)
 except (PermissionError,OSError) as exc:return {"state":"indeterminate","candidate_identity":True,"reason":"environ_read_error","error":str(exc)}
 has_cache=("CBM_CACHE_DIR="+str(cache)).encode() in env;disabled=b"CBM_UPDATE_CHECK=0" in env
 try: final=_read_stat(pid)
 except (FileNotFoundError,ProcessLookupError):return _absent_twice(pid,start)
 except (PermissionError,OSError,ValueError) as exc:return {"state":"indeterminate","candidate_identity":True,"reason":"final_stat_after_environ_error","error":str(exc)}
 if final["starttime"]!=start:return {"state":"indeterminate","candidate_identity":True,"reason":"pid_reused_or_starttime_changed_after_environ","after_starttime":final["starttime"]}
 if not has_cache:return {"state":"same_binary_other_cache"}
 if not disabled:return {"state":"candidate_owned_update_check_not_disabled"}
 return {"state":"candidate_owned","update_check_disabled":True}
def _allowed(argv,binary):return argv in (str(binary),str(binary)+" cli --tool-server")
def scan(backend,roots=(),active=False,rows_fn=proc_rows,inspect_fn=inspect_row):
 """Enumerate same-UID processes by proc identity first, never argv filtering."""
 snapshot=rows_fn(); rows=snapshot["rows"] if isinstance(snapshot,dict) else snapshot; enum_errors=snapshot.get("enumeration_errors",[]) if isinstance(snapshot,dict) else []
 root_ids={int(x) for x in roots if isinstance(x,int)};related=set(root_ids);changed=True
 while changed:
  changed=False
  for r in rows:
   if r["ppid"] in related and r["pid"] not in related:related.add(r["pid"]);changed=True
 identity=binary_identity(backend["binary"]);out=[];problems=[];vanished=[];indeterminate=[]
 for r in rows:
  relevant=r["pid"] in related
  try: inspected=inspect_fn(r,identity,backend["cache"])
  except BaseException as exc:inspected={"state":"indeterminate","reason":"inspection_exception","error":type(exc).__name__+": "+str(exc)}
  state=inspected.get("state");item={**r,"related":relevant,"inspection":inspected}
  if state=="other_binary":
   if relevant:problems.append({**item,"reason":"disallowed_foreground_descendant"})
   continue
  if state=="vanished_confirmed":vanished.append(item);out.append(item);continue
  if state=="indeterminate":
   if relevant or inspected.get("candidate_identity"):
    indeterminate.append(item);problems.append({**item,"reason":"indeterminate_relevant_inspection"});out.append(item)
   continue
  if state=="same_binary_other_cache":item["classification"]="same_binary_other_cache";out.append(item);continue
  if state=="candidate_owned_update_check_not_disabled":problems.append({**item,"reason":"candidate_update_check_not_disabled"});out.append(item);continue
  if state=="candidate_owned":
   if not _allowed(r["argv"],backend["binary"]):problems.append({**item,"reason":"disallowed_candidate_argv"})
   else:item["classification"]="candidate_allowed"
   out.append(item);continue
  if relevant:problems.append({**item,"reason":"unknown_relevant_inspection_state"});out.append(item)
 for error in enum_errors:
  if error.get("pid") in root_ids:problems.append({"pid":error["pid"],"reason":"indeterminate_root_enumeration","error":error["error"]})
 owned=[x for x in out if x["inspection"].get("state")=="candidate_owned"]
 return {"kind":"foreground_scanner","identity":identity,"roots":sorted(root_ids),"active":active,"processes":out,"owned":owned,"vanished":vanished,"indeterminate":indeterminate,"enumeration_errors":enum_errors,"problems":problems,"passed":not problems}
def sample_zero(backend,roots=(),samples=5,interval=.25,scan_fn=scan):
 started=time.monotonic();collected=[]
 for i in range(samples):
  r=scan_fn(backend,roots,False);collected.append({"ordinal":i,"monotonic":time.monotonic(),"scan":r})
  if i+1<samples:time.sleep(interval)
 elapsed=time.monotonic()-started;passed=elapsed>=1.0 and all(not x["scan"]["owned"] and not x["scan"]["problems"] and not x["scan"]["indeterminate"] for x in collected)
 return {"samples":collected,"sample_count":samples,"interval_seconds":interval,"elapsed_seconds":elapsed,"required_duration_seconds":1.0,"passed":passed}
