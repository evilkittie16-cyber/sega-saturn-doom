"""netbusy.py <set_dir> [<set_dir> ...] [--name SETTING] [--views w,p2,m1a]
Net master busy per view (ms per rendered frame) = costmap ALL - I_FinishUpdate
- the slave-wait spin in Saturn_HW_RenderPlayerView (PCs hit > 8000 times).
Reads <set>/<view>/prof_<SETTING>.csv (lodsweep layout) or prof.csv."""
import bisect, csv, os, shutil, subprocess, sys, tempfile

CLK = 28636.36
HERE = os.path.dirname(os.path.abspath(__file__))


def busy(view_dir, name):
    prof = os.path.join(view_dir, 'prof_%s.csv' % name) if name else os.path.join(view_dir, 'prof.csv')
    tmp = tempfile.mkdtemp()
    shutil.copy(prof, os.path.join(tmp, 'prof.csv'))
    shutil.copy(os.path.join(view_dir, 'syms.txt'), os.path.join(tmp, 'syms.txt'))
    out = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'costmap.py'), tmp, '5'],
                         capture_output=True, text=True).stdout
    frames = int(out.split(':')[1].split()[0])
    allv = fu = None
    for l in out.splitlines():
        p = l.split()
        if len(p) == 7 and p[6] == 'ALL' and allv is None:
            allv = float(p[0])
        if len(p) == 7 and p[6].startswith('I_FinishUpdate') and fu is None:
            fu = float(p[0])
    syms = []
    for l in open(os.path.join(tmp, 'syms.txt')):
        p = l.split()
        if len(p) >= 3:
            try:
                syms.append((int(p[0], 16), p[-1]))
            except ValueError:
                pass
    syms.sort()
    addrs = [a for a, _ in syms]
    spin = 0
    for x in csv.reader(open(os.path.join(tmp, 'prof.csv'))):
        if x[0] != 'M':
            continue
        pc = int(x[1], 16) & 0x07FFFFFF
        i = bisect.bisect_right(addrs, pc) - 1
        if syms[i][1] == '_Saturn_HW_RenderPlayerView' and int(x[3]) > 8000:
            spin += int(x[2])
    shutil.rmtree(tmp)
    return allv - fu, spin / CLK / frames


args = sys.argv[1:]
name, views = None, 'w,p2,m1a'
if '--name' in args:
    i = args.index('--name'); name = args[i + 1]; del args[i:i + 2]
if '--views' in args:
    i = args.index('--views'); views = args[i + 1]; del args[i:i + 2]
tot = {s: 0.0 for s in args}
for v in views.split(','):
    cells = []
    for s in args:
        b, sp = busy(os.path.join(s, v), name)
        tot[s] += b - sp
        cells.append('%s: busy %5.2f spin %4.2f net %5.2f' % (os.path.basename(os.path.normpath(s)), b, sp, b - sp))
    print('%-4s %s' % (v, '   '.join(cells)))
print('sum  ' + '   '.join('%s: net %6.2f' % (os.path.basename(os.path.normpath(s)), tot[s]) for s in args))
