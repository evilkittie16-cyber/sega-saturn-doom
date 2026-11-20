"""idlefns.py <dir> [top]  - master functions (ms per rendered frame) while the
slave sits in its idle worker loop, from a `prof N out EVERY` timeline.
Rendered frames = executions of Saturn_HW_RenderPlayerView's entry PC."""
import bisect, collections, csv, os, sys
CLK = 28636.36
d = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
syms = []
for l in open(os.path.join(d, 'syms.txt')):
    p = l.split()
    if len(p) >= 3 and p[-2] in 'tT':
        syms.append((int(p[0], 16), p[-1].lstrip('_')))
syms.sort(); addrs = [a for a, _ in syms]
fn = lambda pc: syms[bisect.bisect_right(addrs, pc & 0x07FFFFFF) - 1][1]
rv = [a for a, n in syms if n == 'Saturn_HW_RenderPlayerView'][0]
frames = sum(int(x[3]) for x in csv.reader(open(os.path.join(d, 'prof.csv')))
             if x[0] == 'M' and (int(x[1], 16) & 0x07FFFFFF) == rv)
rows = list(csv.DictReader(open(os.path.join(d, 'prof.csv.tl.csv'))))
step = int(rows[1]['cycle']) - int(rows[0]['cycle'])
idle, busy = collections.Counter(), collections.Counter()
for r in rows:
    m = fn(int(r['mpc'], 16))
    (idle if fn(int(r['spc'], 16)) == 'HW_Slave_FlatWorker_Loop' else busy)[m] += 1
k = step / CLK / frames
print('%d rendered frames; master fn: ms while slave idle | ms while slave busy' % frames)
for f, n in idle.most_common(top):
    print('  %6.2f %6.2f  %s' % (n * k, busy[f] * k, f))
print('  total idle-overlap %.2f ms, busy-overlap %.2f ms' % (sum(idle.values()) * k, sum(busy.values()) * k))
