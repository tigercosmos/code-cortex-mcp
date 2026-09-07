#!/usr/bin/env python3
"""Count tools returned by one tools/list page over stdio."""
import json, subprocess, sys
binary = sys.argv[1]
p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def send(obj):
    p.stdin.write(json.dumps(obj) + '\n'); p.stdin.flush()
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "probe", "version": "0"}}})
print(json.loads(p.stdout.readline())['result']['serverInfo'])
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
r = json.loads(p.stdout.readline())['result']
print('tools on first page:', len(r['tools']), 'nextCursor:', r.get('nextCursor'))
print([t['name'] for t in r['tools']])
p.stdin.close(); p.wait(timeout=10)
