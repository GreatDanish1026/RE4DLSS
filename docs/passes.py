# Compact pass list of the last complete frame of a capture: each pass with its space0 bindings
# on one line. Usage: passes.py capture.txt [shader ids to keep, comma-separated]
import re
import sys

lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
keep = set(sys.argv[2].split(',')) if len(sys.argv) > 2 else None
presents = [i for i, l in enumerate(lines) if l.startswith('==== PRESENT')]
start = presents[0] + 1

cur = None
out = []
for i in range(start, presents[-1]):
    s = lines[i].strip()
    if s.startswith('FULLSCREEN') or s.startswith('DISPATCH'):
        if cur:
            out.append(cur)
        head = re.sub(r' vs[0-9a-f]{8}', '', s)
        head = re.sub(r' depth #\S+ tex \S+ \S+', '', head)
        cur = [i + 1, head, []]
    elif s.startswith('copy'):
        if cur:
            out.append(cur)
            cur = None
        out.append([i + 1, s, []])
    elif cur and (s.startswith('table') or s.startswith('root')) and not re.search(r'space[1-9]', s):
        m = re.match(r'table param \d+: (\w\d+) space0 = (.*)', s)
        if m:
            what = re.sub(r' as \S+', '', m.group(2)).replace(' tex ', ' ')
            cur[2].append(m.group(1) + '=' + what)
        elif s.startswith('root'):
            m = re.match(r'root (cbv|constants) param (\d+) \((\w+) space0\)(.*)', s)
            if m:
                cur[2].append(m.group(3) + ('' if m.group(1) == 'cbv' else ' consts' + m.group(4)[:60]))
if cur:
    out.append(cur)
for n, head, b in out:
    ident = head.split()[1] if len(head.split()) > 1 else ''
    if keep and ident not in keep and not head.startswith('copy'):
        continue
    print(f'{n}: {head[:150]}')
    if b:
        print('      ' + ' | '.join(b))
