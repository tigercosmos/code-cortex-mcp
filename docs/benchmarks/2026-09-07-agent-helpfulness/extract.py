#!/usr/bin/env python3
import json,sys,os,collections
jsonl,outp,arm,repo,task,rep,model,start,end,rc = sys.argv[1:11]
tools=collections.Counter(); hooks=collections.Counter(); res=None; msgs=0; init=None
tool_by_id={}; res_bytes=collections.Counter()
try:
    for line in open(jsonl):
        line=line.strip()
        if not line: continue
        try: o=json.loads(line)
        except Exception: continue
        t=o.get('type')
        if t=='system' and o.get('subtype')=='init': init=o
        if t=='assistant':
            msgs+=1
            for c in o.get('message',{}).get('content',[]) or []:
                if isinstance(c,dict) and c.get('type')=='tool_use':
                    tools[c.get('name','?')]+=1
                    tool_by_id[c.get('id')]=c.get('name','?')
        if t=='user':
            for c in o.get('message',{}).get('content',[]) or []:
                if isinstance(c,dict) and c.get('type')=='tool_result':
                    body=c.get('content')
                    body=body if isinstance(body,str) else json.dumps(body)
                    tid=c.get('tool_use_id')
                    nm=tool_by_id.get(tid,'?')
                    if nm.startswith('mcp__'): res_bytes['mcp']+=len(body)
                    elif nm in ('Bash','Grep','Glob','Read'): res_bytes['builtin']+=len(body)
                    else: res_bytes['other']+=len(body)
        if t=='result': res=o
        if t in ('hook_event','system') and 'hook' in json.dumps(o)[:400].lower():
            hn=o.get('hook_event_name') or o.get('subtype') or 'hook'
            hooks[str(hn)]+=1
except FileNotFoundError:
    pass
u=(res or {}).get('usage',{}) or {}
m={
 'task':task,'arm':arm,'rep':int(rep),'model':model,'repo':os.path.basename(repo),'rc':int(rc),
 'effort':os.environ.get('EFFORT','medium'),
 'wall_s':round(float(end)-float(start),3),
 'duration_ms':(res or {}).get('duration_ms'),
 'duration_api_ms':(res or {}).get('duration_api_ms'),
 'ttft_ms':(res or {}).get('ttft_ms'),
 'num_turns':(res or {}).get('num_turns'),
 'assistant_msgs':msgs,
 'cost_usd':(res or {}).get('total_cost_usd'),
 'is_error':(res or {}).get('is_error'),
 'subtype':(res or {}).get('subtype'),
 'in_tok':u.get('input_tokens'),'out_tok':u.get('output_tokens'),
 'cache_create':u.get('cache_creation_input_tokens'),'cache_read':u.get('cache_read_input_tokens'),
 'total_ctx_tok':(u.get('input_tokens') or 0)+(u.get('cache_creation_input_tokens') or 0)+(u.get('cache_read_input_tokens') or 0),
 'tools':dict(tools),
 'hook_events':dict(hooks),
 'tool_result_bytes':dict(res_bytes),
 'n_tool_calls':sum(tools.values()),
 'n_mcp_calls':sum(v for k,v in tools.items() if k.startswith('mcp__')),
 'n_builtin_search':sum(v for k,v in tools.items() if k in ('Grep','Glob','Read','Bash')),
 'mcp_servers_init':[s.get('name') for s in (init or {}).get('mcp_servers',[]) or []],
 'tools_available_n':len((init or {}).get('tools',[]) or []),
 'result_text':(res or {}).get('result'),
}
json.dump(m, open(outp,'w'), indent=1)
print(json.dumps({k:m[k] for k in ('task','arm','rep','wall_s','num_turns','n_tool_calls','n_mcp_calls','total_ctx_tok','cost_usd','rc')}))
