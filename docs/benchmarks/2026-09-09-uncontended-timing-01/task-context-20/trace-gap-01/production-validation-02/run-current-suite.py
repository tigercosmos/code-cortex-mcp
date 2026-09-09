#!/usr/bin/env python3
import os,pathlib,json,subprocess,time,hashlib,signal,sys
ROOT=pathlib.Path('/Users/tigercosmos/code-cortex-mcp');OUT=pathlib.Path(__file__).resolve().parent
scratch=pathlib.Path('/Users/tigercosmos/tmp/cortex-trace-gap-validation-02');scratch.mkdir(exist_ok=False)
(scratch/'home').mkdir();(scratch/'cache').mkdir()
env={k:v for k,v in os.environ.items() if not k.startswith(('CBM_','CODEX_'))};env.update(HOME=str(scratch/'home'))
(OUT/'environment.json').write_text(json.dumps({'removed_prefixes':['CBM_','CODEX_'],'HOME':env['HOME'],'CBM_CACHE_DIR':None,'CBM_WORKERS':None,'test_main_owns_HOME_isolation':True},indent=2)+'\n')
def run(name,argv,timeout):
 start=time.monotonic();record={'argv':argv,'cwd':str(ROOT),'started_epoch':time.time(),'timeout_seconds':timeout}
 (OUT/(name+'.argv.json')).write_text(json.dumps(record,indent=2)+'\n')
 with (OUT/(name+'.stdout')).open('xb') as out,(OUT/(name+'.stderr')).open('xb') as err:
  p=subprocess.Popen(argv,cwd=ROOT,env=env,stdout=out,stderr=err,start_new_session=True)
  try:rc=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
   os.killpg(p.pid,signal.SIGKILL);p.wait();rc=124;record['timeout']=True
 record.update(returncode=rc,wall_seconds=time.monotonic()-start,ended_epoch=time.time())
 for suffix in ['stdout','stderr']:record[suffix+'_sha256']=hashlib.sha256((OUT/(name+'.'+suffix)).read_bytes()).hexdigest()
 (OUT/(name+'.result.json')).write_text(json.dumps(record,indent=2)+'\n');print(json.dumps({'stage':name,**record}),flush=True)
 if rc:raise SystemExit(rc)
run('configure',['cmake','-S','.','-B','build/nosan','-DCBM_SANITIZE=OFF','-DCBM_TEST_SEAMS=ON'],120)
run('build-tests',['cmake','--build','build/nosan','--target','test-runner','-j','2'],600)
run('tests',['./build/nosan/test-runner'],1200)
