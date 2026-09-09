#!/usr/bin/env python3
"""Analyze v743 availability-aware pairs without imputing upstream 100m timing."""
import argparse,json,math,pathlib
def geom(xs): return math.exp(sum(math.log(x) for x in xs)/len(xs)) if xs else None
def main():
 ap=argparse.ArgumentParser(); ap.add_argument("--protocol",type=pathlib.Path,required=True); ap.add_argument("--runs",type=pathlib.Path,required=True); ap.add_argument("--source-oracle",type=pathlib.Path,required=True); ap.add_argument("--output",type=pathlib.Path,required=True); a=ap.parse_args()
 tasks={t["id"]:t for t in json.loads((a.protocol/"tasks.json").read_text())}; sched=json.loads((a.protocol/"schedule-v743.json").read_text())["rows"]; oracle=json.loads(a.source_oracle.read_text())
 rows=[]
 for p in sched:
  sfile=a.runs/p["run"]/"summary.json"; gfile=a.runs/p["run"]/"grade.json"; s=json.loads(sfile.read_text()) if sfile.is_file() else {}; g=json.loads(gfile.read_text()) if gfile.is_file() else {}
  arm=p["arm"]; backend={"candidate-mcp":"candidate_production_mcp_stdio_jsonrpc","upstream-mcp":"upstream_production_mcp_stdio_jsonrpc"}.get(arm,"none")
  ok=s.get("state")=="completed" and s.get("task_wall_seconds") is not None and g.get("correct") is True and s.get("no_session_mcp_calls") is True and s.get("retrieval_backend")==backend and (arm=="shell" or (s.get("retrieval_context_bytes",0)>0 and s.get("source_oracle_correct") is True)) and oracle.get("passed") is True
  rows.append({**p,"scale":tasks[p["task"]]["scale"],"archetype":tasks[p["task"]]["archetype"],"seconds":s.get("task_wall_seconds"),"eligible":ok})
 bytask={}
 for r in rows: bytask.setdefault(r["task"],{})[r["arm"]]=r
 def pairs(left,right):
  out=[]
  for task,arms in sorted(bytask.items()):
   x,y=arms.get(left),arms.get(right)
   if x and y: out.append({"task":task,"scale":tasks[task]["scale"],"archetype":tasks[task]["archetype"],"eligible":bool(x["eligible"] and y["eligible"]),"ratio":x["seconds"]/y["seconds"] if x["eligible"] and y["eligible"] else None})
  return out
 def report(ps):
  groups={"all":ps}
  for field in ("scale","archetype"):
   for key in sorted({p[field] for p in ps}): groups[field+":"+key]=[p for p in ps if p[field]==key]
  return {k:{"planned":len(v),"eligible":sum(p["eligible"] for p in v),"geometric_ratio":geom([p["ratio"] for p in v if p["eligible"]])} for k,v in groups.items()}
 result={"rows":rows,"pair_reports":{"candidate_to_shell":report(pairs("candidate-mcp","shell")),"upstream_to_shell":report(pairs("upstream-mcp","shell")),"candidate_to_upstream":report(pairs("candidate-mcp","upstream-mcp"))},"source_oracle_passed":oracle.get("passed") is True,"upstream_100m":{"setup_failures":["4GiB","32GiB"],"no_timing_estimate":True}}
 with a.output.open("x") as f: json.dump(result,f,indent=2,sort_keys=True);f.write("\n")
if __name__=="__main__": main()
