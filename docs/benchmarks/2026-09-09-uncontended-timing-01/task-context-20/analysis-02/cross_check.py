#!/usr/bin/env python3
"""Merge successful audited rows only; invoke unchanged preregistered analysis and compare."""
import hashlib,json,os,pathlib,subprocess,sys
H=pathlib.Path(__file__).resolve().parent.parent;O=pathlib.Path(__file__).resolve().parent
SOURCES=['manual-audit-block-00/audits.json','manual-audit-01/root-block-01.json','manual-audit-01/pcap-block-02.json','manual-audit-block-03/audits.json','manual-audit-block-04/audits.json']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text())
def save(p,v):
 with p.open('x') as f:json.dump(v,f,indent=2);f.write('\n')
merged={};sourcehashes={};bindings={}
for rel in SOURCES:
 p=H/rel;sourcehashes[rel]=sha(p);data=read(p)
 assert not set(data)&set(merged),'duplicate audit keys'
 for name,a in data.items():
  assert a['manual_reviewed'] is True and a['compliant'] is True and a['environment_admissible'] is False
  d=H/'evidence-01'/name
  for k,f in {'summary_sha256':'attempt/summary.json','grade_sha256':'attempt/grade.json','session_report_sha256':'attempt/task/session-report.json','raw_tools_sha256':'attempt/task/selected-raw-tool-events.jsonl','exposure_sha256':'exposure.json'}.items():
   assert a[k]==sha(d/f)
  assert read(d/'attempt/summary.json')['task_completed'] is True and not (d/'setup-failure.json').exists()
  bindings[name]={k:a[k] for k in ['summary_sha256','grade_sha256','session_report_sha256','raw_tools_sha256','exposure_sha256']}
 merged.update(data)
assert len(merged)==15
save(O/'manual-audits.json',merged)
# Preserve the original differing descriptive count-key names: the analyzer passes tool_counts through.
analyzer=H/'analysis.py';before=sha(analyzer)
freeze=read(H/'FROZEN.json')
# Hash is also bound by the independent report's exact input receipt.
assert before=='891986edddf62ab97351c267157318e6e659617d8416469d886b14ce7bed43c4'
argv=[sys.executable,'-B',str(analyzer),'--runs',str(H/'evidence-01'),'--audits',str(O/'manual-audits.json'),'--output',str(O/'report.json')]
save(O/'invocation.json',argv)
env=os.environ.copy();env['PYTHONDONTWRITEBYTECODE']='1'
with (O/'analyzer.stdout').open('xb') as out,(O/'analyzer.stderr').open('xb') as err:
 done=subprocess.run(argv,cwd=H,env=env,stdout=out,stderr=err)
assert done.returncode==0,done.returncode
assert sha(analyzer)==before
r=read(O/'report.json');ind=read(H/'independent-analysis-01/report.json')
assert r['scheduled_rows']==48 and r['setup_failures']==33 and r['eligible_correct_rows']==15 and r['not_attempted_rows']==0
assert sum(x['timing_admissible'] for x in r['rows'])==0
checks={}
for name,c in r['contrasts'].items():
 got=c['correctness_gated_pairs'];expected=ind['contrasts'][name]['pooled']
 assert got['pairs']==expected['pairs']==5
 assert got['geometric_ratio']==expected['geometric_ratio']
 assert c['admissible_correctness_pairs']['pairs']==0
 assert got['sign_equality']['two_sided_p']==expected['sign_vs_1']['two_sided_p']
 assert got['sign_ten_percent_target']['two_sided_p']==expected['sign_vs_0_9']['two_sided_p']
 assert [x['ratio'] for x in got['directions']]==[x['ratio'] for x in expected['individual_pairs']]
 checks[name]={'pairs':5,'geometric_ratio':got['geometric_ratio'],'exact_float_match':True,'sign_equality_matches':True,'sign_target_matches':True,'admissible_pairs':0}
for phase,expected in [('task',ind['task_totals']['historical_estimated_usd']),('smoke',ind['smoke_totals']['known_cost_subtotal_usd'])]:
 assert r['phase_accounting'][phase]['known_cost_subtotal_usd']==expected
assert r['phase_accounting']['smoke']['unknown_cost_phases']==33
save(O/'receipt.json',{'status':'passed','analyzer_returncode':done.returncode,'analyzer_sha256_before':before,'analyzer_sha256_after':sha(analyzer),'source_audit_hashes':sourcehashes,'merged_audits_sha256':sha(O/'manual-audits.json'),'merged_keys':len(merged),'duplicates':0,'original_bindings':bindings,'report_sha256':sha(O/'report.json'),'independent_report_sha256':sha(H/'independent-analysis-01/report.json'),'script_sha256':sha(pathlib.Path(__file__)),'expected_counts':{'scheduled':48,'setup_failures':33,'eligible_correct':15,'admissible':0},'contrasts':checks,'cost_totals_match':True,'schema_notes':['Frozen analyzer not_attempted_rows=0 means every scheduled row has an attempt or setup_failure state;33 task sessions were never launched. Independent report attempted_tasks=15 uses task launch/completion scope.','Audit tool counts retain original key naming: outer_code_calls in blocks00/03/04, orchestration_calls in root01/pcap02. Frozen analyzer passes each dictionary through. Independent calculation normalizes these synonymous keys for totals; no audit or analyzer mutation.'],'frozen_and_raw_unchanged':True})
print(json.dumps({'status':'passed','scheduled':48,'setup_failures':33,'eligible':15,'admissible':0,'pooled_ratios_exact_match':True,'report_sha256':sha(O/'report.json')}))
