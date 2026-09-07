#!/usr/bin/env python3
"""Deterministic grader: compares a run's free-text answer to a task's ground truth."""
import json, re, sys, os, glob, difflib

def norm_path(s, repo_path):
    s = s.strip().strip('`"\'').rstrip(',;').lstrip('-*+ ').strip()
    s = re.sub(r'^\./', '', s)
    if repo_path and s.startswith(repo_path.rstrip('/') + '/'):
        s = s[len(repo_path.rstrip('/')) + 1:]
    s = re.sub(r':\d+.*$', '', s)          # drop :line suffix
    return s.strip()

def norm_name(s):
    s = s.strip().strip('`"\'').rstrip(',;.').lstrip('-*+ 0123456789.').strip()
    s = re.sub(r'\(.*$', '', s)            # drop arg list
    s = s.split('::')[-1] if '::' in s and ' ' not in s else s
    return s.strip()

PATH_RE = re.compile(r'[\w./+-]*[\w+-]+\.(?:cpp|hpp|h|cc|cxx|c|py|yml|yaml|txt|cmake|toml|md|sh|in|json|ipp)\b')

PATH_LINE_RE = re.compile(r'^[\w./+-]+\.(?:cpp|hpp|h|cc|cxx|c|py|yml|yaml|txt|cmake|toml|md|sh|in|json|ipp)$')

def extract_paths(text, repo_path):
    """Prefer lines that are ONLY a path (the requested answer format). Falling
    back to a regex over the whole reply would mine paths out of explanatory
    prose, which unfairly penalises answers that explain themselves."""
    strict = []
    for ln in (text or '').splitlines():
        c = ln.strip().strip('`"\'').lstrip('-*+ ').strip().rstrip(',;')
        c = re.sub(r'^\./', '', c)
        if repo_path and c.startswith(repo_path.rstrip('/') + '/'):
            c = c[len(repo_path.rstrip('/')) + 1:]
        if PATH_LINE_RE.match(c) and c not in strict:
            strict.append(c)
    if strict:
        return strict
    out = []
    for m in PATH_RE.finditer(text or ''):
        p = norm_path(m.group(0), repo_path)
        if p and p not in out:
            out.append(p)
    return out

def extract_lines(text):
    out = []
    for ln in (text or '').splitlines():
        ln = ln.strip()
        if not ln: continue
        if ln.endswith(':') or ln.startswith('#'): continue
        n = norm_name(ln)
        if n and 1 < len(n) < 120 and n not in out:
            out.append(n)
    return out

def prf(pred, gold):
    pred, gold = set(pred), set(gold)
    if not gold: return (0.0, 0.0, 0.0)
    tp = len(pred & gold)
    p = tp / len(pred) if pred else 0.0
    r = tp / len(gold)
    f = 2*p*r/(p+r) if p+r else 0.0
    return (p, r, f)

def grade(task, answer):
    kind = task['answer_kind']; rp = task.get('repo_path','')
    gold = task['ground_truth']
    ans = answer or ''
    if kind in ('set_of_paths',):
        pred = extract_paths(ans, rp)
        gold_n = [norm_path(g, rp) for g in gold]
        # match on basename too, to be lenient about path prefixes
        p, r, f = prf(pred, gold_n)
        if f < 1.0:
            pb = [os.path.basename(x) for x in pred]; gb = [os.path.basename(x) for x in gold_n]
            p2, r2, f2 = prf(pb, gb)
            if f2 > f: p, r, f = p2, r2, f2
        return dict(precision=p, recall=r, f1=f, pred=pred, gold=gold_n)
    if kind == 'set_of_names':
        goldn = [norm_name(g) for g in gold]
        # Prefer lines that are a bare identifier - the requested output format.
        strict = []
        for ln in (ans or '').splitlines():
            c = ln.strip().strip('`"\'').lstrip('-*+ ').rstrip(',;.').strip()
            c = re.sub(r'^\d+[.)]\s*', '', c)
            if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:<>]{1,80}', c) and c not in strict:
                strict.append(norm_name(c))
        cand = strict
        if not cand:
            cand = extract_lines(ans)
            extra = [norm_name(w) for w in re.findall(r'\b[A-Z][A-Za-z0-9_]{2,}\b', ans)]
            cand = cand + [e for e in extra if e not in cand]
        p, r, f = prf(cand, goldn)
        return dict(precision=p, recall=r, f1=f, pred=cand[:60], gold=goldn)
    if kind == 'ordered_list':
        cands = [gold] + [a for a in (task.get('alternates') or [])]
        best = 0.0; bestgold = gold
        # Accept one-per-line OR a single arrow/comma-separated chain, and
        # compare on the method name so a missing Class:: prefix is not a zero.
        flat = re.split(r'\s*(?:->|\u2192|\u21d2|,|;)\s*', (ans or '').replace('\n', '\n'))
        toks = []
        for chunk in flat:
            for ln in chunk.splitlines():
                n = norm_name(ln)
                if n and 1 < len(n) < 120:
                    toks.append(n)
        pred = [t.split('::')[-1] for t in toks]
        for g in cands:
            gn = [norm_name(x).split('::')[-1] for x in g]
            # subsequence containment score
            sm = difflib.SequenceMatcher(None, [x.lower() for x in pred], [x.lower() for x in gn])
            sc = sm.ratio()
            hits = sum(1 for x in gn if any(x.lower() == y.lower() for y in pred))
            sc = max(sc, hits/len(gn) if gn else 0)
            if sc > best: best, bestgold = sc, gn
        return dict(precision=best, recall=best, f1=best, pred=pred[:40], gold=bestgold)
    if kind == 'exact_string':
        g = gold[0] if gold else ''
        ok = g.strip().lower() in ans.lower()
        if not ok:
            gt = re.sub(r'\s+','',g.lower()); at = re.sub(r'\s+','',ans.lower())
            ok = bool(gt) and gt in at
        return dict(precision=float(ok), recall=float(ok), f1=float(ok), pred=ans[:200], gold=g)
    if kind == 'path_and_line':
        g = gold[0] if gold else ''
        gp, _, gl = g.rpartition(':')
        okp = bool(gp) and (norm_path(gp, rp) in [norm_path(x, rp) for x in extract_paths(ans, rp)] or os.path.basename(gp) in ans)
        nums = [int(x) for x in re.findall(r'\b(\d{1,6})\b', ans)]
        okl = any(abs(n - int(gl)) <= 3 for n in nums) if gl.isdigit() else False
        sc = (0.5 if okp else 0.0) + (0.5 if okl else 0.0)
        return dict(precision=sc, recall=sc, f1=sc, pred=ans[:200], gold=g, path_ok=okp, line_ok=okl)
    if kind == 'compound':
        subs = task['subquestions']
        parts = re.split(r'(?im)^\s*(?:###\s*)?Q(\d)\s*[:.)-]', ans)
        chunks = {}
        if len(parts) > 1:
            for i in range(1, len(parts)-1, 2):
                chunks[int(parts[i])] = parts[i+1]
        scores=[]; detail=[]
        for i, sq in enumerate(subs, start=1):
            sub_ans = chunks.get(i, ans)
            sub_task = dict(sq); sub_task['repo_path']=rp
            g = grade(sub_task, sub_ans)
            scores.append(g['f1']); detail.append({'q':i,'f1':g['f1'],'p':g['precision'],'r':g['recall']})
        m = sum(scores)/len(scores) if scores else 0.0
        return dict(precision=m, recall=m, f1=m, pred=detail, gold=[sq['ground_truth'] for sq in subs],
                    split_ok=len(chunks)==len(subs))
    return dict(precision=0.0, recall=0.0, f1=0.0, pred=ans[:200], gold=gold)

if __name__ == '__main__':
    tasks = {t['id']: t for t in json.load(open(sys.argv[1]))}
    rows = []
    for f in sorted(glob.glob(os.path.join(sys.argv[2], '*.metrics.json'))):
        m = json.load(open(f))
        t = tasks.get(m['task'])
        if not t: continue
        # drop runs the CLI aborted (e.g. usage-limit refusals) - they carry no measurement
        if m.get('is_error') or m.get('rc'):
            continue
        g = grade(t, m.get('result_text'))
        m['grade'] = g
        m['favours'] = t.get('favours')
        m['archetype'] = t.get('archetype')
        m['repo_loc'] = t.get('repo_loc')
        rows.append(m)
    json.dump(rows, open(sys.argv[3], 'w'), indent=1)
    print(f'graded {len(rows)} runs')
