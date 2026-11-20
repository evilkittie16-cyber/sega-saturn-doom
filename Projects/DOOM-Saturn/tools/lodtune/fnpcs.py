"""fnpcs.py <view_dir> <prof_csv> <symbol>
Per-PC master cost (ms per rendered frame) inside one function, with disassembly
(objdump needs the cygwin dlls in Compiler/WINDOWS/bin on PATH)."""
import bisect, csv, os, subprocess, sys
CLK = 28636.36
ROOT = r'C:\Users\charl\Desktop\joengine-2026.1\Compiler\WINDOWS'
d, pf, sym = sys.argv[1:4]
syms = []   # nm -S (addr size type name) or nm -n (addr type name; size = gap to next)
for l in open(d + '/syms.txt'):
    p = l.split()
    if len(p) >= 4 and p[2] in 'tT':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), None, p[2]))
syms.sort(key=lambda t: t[0])
for i, (a, s, n) in enumerate(syms):
    if s is None:
        syms[i] = (a, (syms[i + 1][0] - a) if i + 1 < len(syms) else 2, n)
lo, size = [(a, s) for a, s, n in syms if n == sym][0]
hi = lo + size
rv = [a for a, s, n in syms if n == '_Saturn_HW_RenderPlayerView'][0]
fr = 0; pcs = {}
for r in csv.DictReader(open(pf)):
    if r['cpu'] != 'M': continue
    pc = int(r['pc'], 16) & 0x07FFFFFF
    if pc == rv: fr += int(r['count'])
    if lo <= pc < hi:
        pcs[pc] = (int(r['cycles']), int(r['count']), int(r['imiss']), int(r['dmiss']), int(r['data_unc']))
env = dict(os.environ); env['PATH'] = ROOT + r'\bin;' + env['PATH']
out = subprocess.run([ROOT + r'\sh-elf\bin\objdump.exe', '-d', '--start-address=0x%x' % lo,
                      '--stop-address=0x%x' % hi, d + '/game.elf'], capture_output=True, text=True, env=env).stdout
tot = sum(c[0] for c in pcs.values())
print('%s: %d bytes, %d frames, %.3f ms/frame' % (sym, size, fr, tot / CLK / fr))
print('pc       ms      count  imiss dmiss unc  insn')
for l in out.splitlines():
    p = l.split(':', 1)
    try: pc = int(p[0].strip(), 16)
    except ValueError: continue
    c = pcs.get(pc)
    ins = p[1].split('\t', 2)[-1].strip() if len(p) > 1 else ''
    if c:
        print('%x %7.4f %6d %5d %5d %4d  %s' % (pc, c[0] / CLK / fr, c[1], c[2], c[3], c[4], ins))
    else:
        print('%x %33s %s' % (pc, '', ins))
