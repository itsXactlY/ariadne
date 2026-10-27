"""Build the corpus `dd replay` calibrates against, from the live session log.

Two things are harvested, and neither is asserted:

1. **The asks.** Genuine operator prompts from Daedalus's own session log. Harness text is
   not a prompt — the memory dumps, the policy block and the context notes all arrive in the
   `user` role — so injected text is dropped by marker, not by length. What survives is the
   operator's own words, deduplicated.

2. **The labels.** The skill ids the harness actually *opened* after each ask, read out of the
   assistant's `skill_view` calls in the same session. Ground truth is behavioural: an ask that
   opened nothing is labeled nothing, which is the false-offer case — and that is the half of the
   curve that matters most. Only ids present in the live catalogue count, so a stale skill name
   never becomes a phantom "right" answer.

    ./build/ddcli skills --home examples/daedalus > build/catalogue.txt
    python3 bench/replay_corpus.py build/catalogue.txt build/corpus.jsonl
    ./build/ddcli replay build/corpus.jsonl --home examples/daedalus --k 3

Read the numbers as a floor, not as truth: the labels are what the model did, not what it
should have done, so a top hit that names an adjacent skill is counted wrong when the operator
opened the other one. Precision is therefore conservative. The measured curve and the numbers
it fitted are recorded in docs/PARITY.md, "Calibration".
"""
import json, glob, re, sys, collections

CAT, OUT = sys.argv[1], sys.argv[2]
SESSIONS = sys.argv[3] if len(sys.argv) > 3 else '/home/alca/.daedalus/sessions/session_*.json'

SKILL_ARG = re.compile(r'"(?:skill|name|uri|id|skill_name)"\s*:\s*"?([A-Za-z0-9][A-Za-z0-9._-]*)')
MARKERS = ['[SYSTEM POLICY', 'MAZEMAKER-FIRST', 'mazemaker_recall', 'mcp_', '<memory',
           '=== USER ===', '=== ASSISTANT ===', 'memory-context', 'Recalled for this turn',
           'The harness looks things up', 'PULSE —', 'tool_search', '{"id":', '[rebased]',
           '=== REASONING ===', 'Catalogues you can search']
SKIP = re.compile('|'.join(re.escape(m) for m in MARKERS))
SHORTHAND = re.compile(r'^(hmm+|ok|yes|no|go for|start |continue|prove it|hu\??|!?)\s*$')

ids = set()
for line in open(CAT, encoding='utf-8'):
    m = re.match(r'^  \[[^\]]*\] (\S+) — ', line)   # the dump is `  [group] id — gist`; every entry counts
    if m:
        ids.add(m.group(1))
print(f'catalogue ids: {len(ids)}')

asks, bad, dropped = [], 0, collections.Counter()
for f in sorted(glob.glob(SESSIONS)):
    try:
        d = json.load(open(f, encoding='utf-8', errors='replace'))
    except Exception:
        bad += 1
        continue
    msgs = d if isinstance(d, list) else (d.get('messages') or d.get('conversation') or [])
    cur, opened = None, []

    def flush():
        if cur:
            asks.append((cur, sorted(set(opened)), f.split('/')[-1]))

    for m in msgs:
        role, c = m.get('role'), m.get('content')
        if role == 'user':
            flush()
            cur, opened = None, []
            t = (c or '').strip() if isinstance(c, str) else ''
            if not t:
                dropped['empty'] += 1
            elif SKIP.search(t):
                dropped['injected'] += 1
            elif len(t) < 24:
                dropped['short'] += 1
            elif SHORTHAND.match(t):
                dropped['shorthand'] += 1
            else:
                cur = t
        elif role == 'assistant':
            for tc in (m.get('tool_calls') or []):
                fn = tc.get('function') or {}
                if fn.get('name') not in ('skill_view', 'skill_manage'):
                    continue
                for mm in SKILL_ARG.finditer(str(fn.get('arguments') or '')):
                    if mm.group(1) in ids:
                        opened.append(mm.group(1))
    flush()

print(f'{len(glob.glob(SESSIONS))} session files ({bad} unreadable)')
for k, n in dropped.most_common():
    print(f'  dropped {k}: {n}')

seen, dedup = set(), []
for t, opened, f in asks:
    k = re.sub(r'\s+', ' ', t).lower()
    if k in seen:
        continue
    seen.add(k)
    dedup.append({'text': t, 'want': opened, 'src': f})

with_skill = [r for r in dedup if r['want']]
print(f'{len(asks)} asks -> {len(dedup)} unique: {len(with_skill)} opened a skill, {len(dedup) - len(with_skill)} opened none')
print('opened-per-ask:', sorted(collections.Counter(len(r['want']) for r in dedup).items()))
terms = collections.Counter()
for r in dedup:
    for w in re.findall(r'[a-zäöüß]{5,}', r['text'].lower()):
        terms[w] += 1
print('top terms:', [w for w, _ in terms.most_common(18)])

with open(OUT, 'w', encoding='utf-8') as fh:
    for r in dedup:
        fh.write(json.dumps(r, ensure_ascii=False) + '\n')
print('wrote', OUT)
