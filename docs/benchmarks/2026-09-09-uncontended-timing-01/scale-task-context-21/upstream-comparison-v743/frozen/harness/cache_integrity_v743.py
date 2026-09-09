#!/usr/bin/env python3
"""v7.31 closed-tree runtime cache seal for pre-run tamper rejection."""
import hashlib,json,os,pathlib,stat
def digest(path):
 h=hashlib.sha256()
 with open(path,"rb") as f:
  for b in iter(lambda:f.read(1024*1024),b""):h.update(b)
 return h.hexdigest()
def classify(name):
 if name=="_config.db":return "dynamic_config"
 if name.endswith(("-wal","-shm")):return "dynamic_wal_shm"
 if name.endswith((".log",".log.jsonl")):return "dynamic_log"
 if name.endswith(".db"):return "immutable_project_db"
 return "immutable_other"
def tree(backend,root):
 root=pathlib.Path(root)
 if root.is_symlink() or not root.is_dir():raise ValueError("cache root type")
 rows=[]
 for base,dirs,files in os.walk(root,followlinks=False):
  p=pathlib.Path(base);rel=str(p.relative_to(root)) or "."
  st=p.lstat();rows.append({"backend":backend,"path":rel,"type":"dir","mode":stat.S_IMODE(st.st_mode),"device":st.st_dev,"inode":st.st_ino})
  for name in sorted(dirs+files):
   q=p/name;st=q.lstat();r=str(q.relative_to(root))
   if stat.S_ISLNK(st.st_mode):raise ValueError("cache symlink: "+r)
   if stat.S_ISDIR(st.st_mode):continue
   if not stat.S_ISREG(st.st_mode):raise ValueError("cache unsupported type: "+r)
   rows.append({"backend":backend,"path":r,"type":"file","mode":stat.S_IMODE(st.st_mode),"device":st.st_dev,"inode":st.st_ino,"bytes":st.st_size,"sha256":digest(q),"classification":classify(name)})
 return sorted(rows,key=lambda x:(x["backend"],x["path"],x["type"]))
def make(setup):
 roots={b:str(pathlib.Path(setup["backends"][b]["cache"]).resolve()) for b in ("candidate","upstream")}
 rows=[]
 for b,p in roots.items():rows+=tree(b,p)
 return {"schema":"v743-runtime-cache-closed-tree","roots":roots,"entries":rows,"entry_count":len(rows),"aggregate_sha256":hashlib.sha256(json.dumps(rows,sort_keys=True,separators=(",",":")).encode()).hexdigest()}
def write(setup,path):
 m=make(setup);pathlib.Path(path).write_text(json.dumps(m,sort_keys=True,indent=2)+chr(10));return m
def verify(setup,seal_path,seal_sha256,immutable_only=False):
 p=pathlib.Path(seal_path)
 if not p.is_file() or digest(p)!=seal_sha256:raise ValueError("runtime cache seal drift")
 m=json.loads(p.read_text())
 if m.get("schema")!="v743-runtime-cache-closed-tree" or m.get("entry_count")!=len(m.get("entries",[])):raise ValueError("runtime cache seal schema")
 expected_roots={b:str(pathlib.Path(setup["backends"][b]["cache"]).resolve()) for b in ("candidate","upstream")}
 if m.get("roots")!=expected_roots:raise ValueError("runtime cache root binding drift")
 actual=make(setup);expected=m["entries"]
 if immutable_only:
  expected=[x for x in expected if x.get("classification")=="immutable_project_db"]
  actual=[x for x in actual["entries"] if x.get("classification")=="immutable_project_db"]
 else:actual=actual["entries"]
 if actual!=expected:raise ValueError("runtime cache artifact tamper")
 return {"passed":True,"immutable_only":immutable_only,"entries_checked":len(actual),"aggregate_sha256":m["aggregate_sha256"]}
