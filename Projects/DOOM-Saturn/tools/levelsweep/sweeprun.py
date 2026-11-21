"""Drive DOOM-Windows -sweep over a view list, one process per map, skipping
views whose DOS reference crashes (vanilla renderer limits).
usage: python sweeprun.py <DOOM-Windows dir> <views.txt> <out.log> [exe]
"""
import os, subprocess, sys, collections

wd, views, outlog = sys.argv[1], sys.argv[2], sys.argv[3]
exe = sys.argv[4] if len(sys.argv) > 4 else 'doom_saturn_asm.exe'
lines = [l.split() for l in open(views) if l.strip()]
bymap = collections.OrderedDict()
for l in lines:
    bymap.setdefault((l[0], l[1]), []).append(l)
out = open(outlog, 'w')
for key, todo in bymap.items():
    while todo:
        tmp = os.path.join(wd, 'sw_part.txt')
        with open(tmp, 'w') as f:
            for l in todo:
                f.write(' '.join(l) + '\n')
        try:
            r = subprocess.run([os.path.join(wd, exe), '-sweep', 'sw_part.txt'], cwd=wd,
                               capture_output=True, text=True, timeout=900, errors='replace')
            text = r.stdout
        except subprocess.TimeoutExpired as e:
            text = (e.stdout or b'').decode('utf8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            text += '\nTIMEOUT\n'
        done = set()
        for t in text.splitlines():
            if t.startswith('[SWEEP] ') and not t.startswith('[SWEEP] done'):
                out.write(t + '\n')
                done.add(t.split()[1])
        rest = [l for l in todo if l[5] not in done]
        if 'FATAL CRASH' in text or 'TIMEOUT' in text:
            if rest:
                out.write('[SWEEP] %s E%sM%s %s %s %s | REFCRASH\n' % (rest[0][5], rest[0][0], rest[0][1], rest[0][2], rest[0][3], rest[0][4]))
                rest = rest[1:]
        elif rest:
            out.write('[SWEEP] %s ... | INCOMPLETE (%d views left, exit %s)\n' % (key, len(rest), r.returncode))
            rest = []
        out.flush()
        todo = rest
    print('E%sM%s done' % key, flush=True)
