#!/usr/bin/env python3
import os,pathlib,json,subprocess,time,hashlib,signal,sys,shutil,sqlite3
sys.dont_write_bytecode=True
ROOT=pathlib.Path('/Users/tigercosmos/code-cortex-mcp');OUT=pathlib.Path(__file__).resolve().parent;H=OUT.parent.parent
sys.path.insert(0,str(H));from run import source_state
sha=lambda p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def save(p,x):p.write_text(json.dumps(x,indent=2)+'\n')
assert json.loads((OUT/'tests.result.json').read_text())['returncode']==0
assert '5853 passed' in (OUT/'tests.stdout').read_text() and 'FAIL' not in (OUT/'tests.stdout').read_text()
SCRATCH=pathlib.Path('/Users/tigercosmos/tmp/cortex-trace-gap-validation-02')
for n in ['production-home','jansson-home','jansson-cache']:(SCRATCH/n).mkdir(exist_ok=False)
env={k:v for k,v in os.environ.items() if not k.startswith(('CBM_','CODEX_'))};env.update(HOME=str(SCRATCH/'production-home'))
save(OUT/'production-environment.json',{'removed_prefixes':['CBM_','CODEX_'],'HOME':env['HOME']})
def run(name,argv,timeout,env):
 rec={'argv':argv,'cwd':str(ROOT),'started_epoch':time.time(),'timeout_seconds':timeout};save(OUT/(name+'.argv.json'),rec);start=time.monotonic()
 with (OUT/(name+'.stdout')).open('xb') as so,(OUT/(name+'.stderr')).open('xb') as se:
  p=subprocess.Popen(argv,cwd=ROOT,env=env,stdout=so,stderr=se,start_new_session=True)
  try:rc=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait();rc=124;rec['timeout']=True
 rec.update(returncode=rc,wall_seconds=time.monotonic()-start,ended_epoch=time.time(),stdout_sha256=sha(OUT/(name+'.stdout')),stderr_sha256=sha(OUT/(name+'.stderr')));save(OUT/(name+'.result.json'),rec);print(json.dumps({'stage':name,**rec}),flush=True)
 if rc:raise SystemExit(rc)
 return (OUT/(name+'.stdout')).read_bytes()
for name in ['flags.make','link.txt']:
 shutil.copy2(ROOT/'build/c/CMakeFiles/code-cortex-mcp.dir'/name,OUT/('production-before-'+name))
run('build-production',[str(ROOT/'scripts/build.sh')],1200,env)
binary=SCRATCH/'code-cortex-mcp';shutil.copy2(ROOT/'build/c/code-cortex-mcp',binary)
run('version',[str(binary),'--version'],30,env)
run('doctor',[str(binary),'doctor'],60,env)
for name in ['flags.make','link.txt']:shutil.copy2(ROOT/'build/c/CMakeFiles/code-cortex-mcp.dir'/name,OUT/('production-after-'+name))
project=json.loads((H/'setup-01/jansson/project.json').read_text());before=source_state(project);assert before==project['manifest'];save(OUT/'jansson-source-before.json',before)
source_files={n:sha(ROOT/n) for n in json.loads((OUT.parent/'proposal-02/before-hashes.json').read_text())}
binding={'binary':str(binary),'binary_sha256':sha(binary),'build_binary_sha256':sha(ROOT/'build/c/code-cortex-mcp'),'source_files':source_files,'repository':project['repo_root'],'original_commit':project['original_commit'],'source_manifest_sha256':sha(OUT/'jansson-source-before.json')};save(OUT/'candidate-binding.json',binding)
env.update(HOME=str(SCRATCH/'jansson-home'),CBM_CACHE_DIR=str(SCRATCH/'jansson-cache'),CBM_WORKERS='2');save(OUT/'jansson-environment.json',{k:env[k] for k in ['HOME','CBM_CACHE_DIR','CBM_WORKERS']})
args={'repo_path':project['repo_root'],'name':project['project'],'mode':'fast','persistence':False};save(OUT/'index.args.json',args)
def payload(raw):
 body=json.loads(raw);assert body.get('isError') is not True
 if isinstance(body.get('structuredContent'),dict):return body['structuredContent']
 return json.loads(next(x['text'] for x in body['content'] if x['type']=='text'))
indexed=payload(run('index',[str(binary),'cli','--json','index_repository','--args-file',str(OUT/'index.args.json')],180,env));assert indexed['status']=='indexed' and indexed['project']==project['project']
args={'project':project['project'],'function_name':'jsonp_free','from_function':'json_object_clear','direction':'inbound','depth':6,'source_context':2,'max_bytes':12000,'include_tests':False};save(OUT/'trace.args.json',args)
body=payload(run('trace',[str(binary),'cli','--json','trace_path','--args-file',str(OUT/'trace.args.json')],30,env));save(OUT/'trace-payload.json',body)
want=['json_object_clear','hashtable_clear','hashtable_do_clear','jsonp_free'];assert body['path_found'] is True and [x['name'] for x in body['path']]==want;assert len(body['caller_edges'])==3
assert [(x['from'],x['to'],x['from_file'],x['line']) for x in body['caller_edges']]==[(want[0],want[1],'src/value.c',188),(want[1],want[2],'src/hashtable.c',283),(want[2],want[3],'src/hashtable.c',136)]
assert [(x['file'],x['start_line']) for x in body['path']]==[('src/value.c',181),('src/hashtable.c',280),('src/hashtable.c',128),('src/memory.c',32)]
assert len(json.dumps(body,separators=(',',':')).encode())<=12000
for file,line,callee in [('src/value.c',188,'hashtable_clear('),('src/hashtable.c',283,'hashtable_do_clear('),('src/hashtable.c',136,'jsonp_free(')]:assert callee in (pathlib.Path(project['repo_root'])/file).read_text().splitlines()[line-1]
after=source_state(project);save(OUT/'jansson-source-after.json',after);assert after==before;assert sha(binary)==binding['binary_sha256'];db=SCRATCH/'jansson-cache'/f"{project['project']}.db"
conn=sqlite3.connect('file:'+str(db)+'?mode=ro',uri=True);assert conn.execute('pragma quick_check').fetchone()[0]=='ok';conn.close()
save(OUT/'measurement.json',{'passed':True,'path_names':want,'call_lines':[188,283,136],'declaration_start_lines':[181,280,128,32],'traversal_truncated':body['traversal_truncated'],'source_unchanged':True,'source_manifest_paths':len(before),'candidate_sha256':sha(binary),'database_sha256':sha(db),'index_wall_seconds':json.loads((OUT/'index.result.json').read_text())['wall_seconds'],'trace_wall_seconds':json.loads((OUT/'trace.result.json').read_text())['wall_seconds'],'limits':['One correctness replay; setup/index and tool subprocess wall are descriptive, not whole-agent speed evidence.','Candidate scan cap1000 and accepted cap100 are separate from existing recursive SQL grouping work.','Inherited SQLite error propagation limitations remain.']})
print((OUT/'measurement.json').read_text(),flush=True)
