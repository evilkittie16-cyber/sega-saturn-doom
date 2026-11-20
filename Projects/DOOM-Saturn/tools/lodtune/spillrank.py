"""spillrank.py <view_dir> <prof_csv> <sym> [<sym> ...]
Per function: executed instructions per rendered frame, share that touch the
stack (r15-relative, excluding push/pop), and work/stall split from the profile."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
d, pf = sys.argv[1], sys.argv[2]
print('%-48s %8s %8s %6s %7s' % ('function', 'insn/fr', 'stack/fr', 'stk%', 'ms'))
for sym in sys.argv[3:]:
    out = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'fnpcs.py'), d, pf, sym],
                         capture_output=True, text=True).stdout.splitlines()
    if not out:
        print('%-48s (not found)' % sym); continue
    fr = int(out[0].split(',')[1].split()[0]); ms = float(out[0].split(',')[2].split()[0])
    ins = stk = 0
    for l in out[2:]:
        p = l.split()
        if len(p) < 7 or not p[1].replace('.', '').isdigit(): continue
        n = int(p[2]); txt = ' '.join(p[6:])
        ins += n
        if 'r15' in txt and '@-r15' not in txt and '@r15+' not in txt and 'r15,r14' not in txt:
            stk += n
    print('%-48s %8d %8d %5.1f%% %7.3f' % (sym, ins // fr, stk // fr, 100.0 * stk / max(ins, 1), ms))
