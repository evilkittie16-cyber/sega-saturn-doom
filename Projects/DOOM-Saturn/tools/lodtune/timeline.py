"""timeline.py <dir> [frames_to_draw] [ms_per_char]
<dir> holds prof.csv, prof.csv.tl.csv (sdbg `prof N out.csv EVERY`) and syms.txt.
Splits the both-CPU PC timeline into frames (a frame starts when the master
leaves I_FinishUpdate), prints character strips per frame and a table of what
the master is doing while the slave idles (and vice versa).

Master classes: W I_FinishUpdate, w slave-wait spin, D delivery,
V visibility/walk asm, E wall emit, F flat submit/steal, S sprites/things,
R other renderer, g game logic/other.
Slave classes: . idle (worker loop), f flats, o other."""
import bisect, collections, csv, os, sys

CLK = 28636.36
d = sys.argv[1]
ndraw = int(sys.argv[2]) if len(sys.argv) > 2 else 3
mspc = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5

syms = []
for l in open(os.path.join(d, 'syms.txt')):
    p = l.split()
    if len(p) >= 3 and p[-2] in 'tT':
        syms.append((int(p[0], 16), p[-1].lstrip('_')))
syms.sort()
addrs = [a for a, _ in syms]


def fn(pc):
    i = bisect.bisect_right(addrs, pc & 0x07FFFFFF) - 1
    return syms[i][1] if i >= 0 else '?'


spin = set()
for x in csv.reader(open(os.path.join(d, 'prof.csv'))):
    if x[0] == 'M' and int(x[3]) > 8000 and fn(int(x[1], 16)) == 'Saturn_HW_RenderPlayerView':
        spin.add(int(x[1], 16) & 0x07FFFFFF)

MCLS = [('W', ('I_FinishUpdate',)),
        ('D', ('Saturn_CmdBuf_Deliver', 'Saturn_CmdBuf_CullOccluded', 'Saturn_CmdBuf_ComputeDDA')),
        ('V', ('hw_asm_walk', 'hw_asm_subap', 'hw_asm_spanap', 'hw_asm_subsec', 'hw_asm_cullbox', 'hw_asm_bsp',
               'hw_asm_rescan', 'hw_asm_spanocc', 'hw_asm_marksub', 'hw_asm_commit', 'hw_asm_portal',
               'HW_RenderSubsector', 'HW_EvalSubsector', 'HW_Check', 'HW_RecordSubsectorUnbaked')),
        ('E', ('hw_asm_lingen', 'hw_asm_multiband', 'HW_EmitWall', 'HW_EmitSubdivided', 'HW_EmitStrip',
               'HW_LoadWallTex', 'HW_TransformPillar', 'HW_EmitMono', 'HW_TryCoalesce', 'HW_ExtrapolateMono',
               'HW_WallXYClamp')),
        ('F', ('hw_asm_flatsub', 'HW_SubmitSlaveFlat', 'HW_RingFlat', 'HW_CacheFlatSector', 'HW_FlatAlt', 'HW_Steal',
               'HW_SplitFlat', 'Saturn_Vdp1_GetFlatSrca', 'Saturn_Vdp1_FlatAvg', 'HW_Slave')),
        ('S', ('HW_RenderSubsectorSprites', 'HW_BuildSectorThings', 'HW_SubsectorHasThings', 'Saturn_Vdp1Spr',
               'Saturn_SpritePatch', 'HW_DrawPSprites', 'HW_Sprite')),
        ('R', ('HW_', 'hw_', 'Saturn_HW', 'Saturn_Vdp1', 'v1_', 'Saturn_Vdp2', 'Saturn_CmdBuf'))]


def mclass(pc):
    pc &= 0x07FFFFFF
    if pc in spin:
        return 'w'
    f = fn(pc)
    for c, pre in MCLS:
        if f.startswith(pre):
            return c
    return 'g'


def sclass(pc):
    f = fn(pc)
    if f == 'HW_Slave_FlatWorker_Loop':
        return '.'
    if f.startswith(('HW_', 'hw_', 'Saturn_Vdp1')):
        return 'f'
    return 'o'


rows = []
for x in csv.DictReader(open(os.path.join(d, 'prof.csv.tl.csv'))):
    rows.append((int(x['cycle']), mclass(int(x['mpc'], 16)), sclass(int(x['spc'], 16))))
# A frame starts when the master leaves a real I_FinishUpdate wait: a W run
# of >= 8 samples (interrupt handlers inside the wait only blip it), and
# at least 4 ms after the previous frame start.
frames, cur, wrun, last_start = [], [], 0, None
for r in rows:
    if r[1] == 'W':
        wrun += 1
    else:
        if cur and wrun >= 8 and (last_start is None or r[0] - last_start >= 4 * CLK):
            frames.append(cur); cur = []; last_start = r[0]
        if wrun and r[1] != 'g':
            wrun = 0
    cur.append(r)
frames = frames[1:]  # first is partial
if not frames:
    sys.exit('no frames')

step = rows[1][0] - rows[0][0]
print('%d frames, sample %d cycles, avg frame %.2f ms' % (len(frames), step,
      sum(f[-1][0] - f[0][0] for f in frames) / len(frames) / CLK))
for f in frames[:ndraw]:
    t0 = f[0][0]
    n = int((f[-1][0] - t0) / CLK / mspc) + 1
    ms_, ss_ = [' '] * n, [' '] * n
    for c, m, s in f:
        k = int((c - t0) / CLK / mspc)
        ms_[k], ss_[k] = m, s
    print('\nframe %.2f ms (one char = %.2f ms)' % ((f[-1][0] - t0) / CLK, mspc))
    print('  M ' + ''.join(ms_))
    print('  S ' + ''.join(ss_))

tot = collections.Counter()
for f in frames:
    for c, m, s in f:
        tot[(m, s)] += 1
nf = len(frames)
ms = lambda k: k * step / CLK / nf
print('\nper frame, ms: master class x slave class')
mk = 'WwDVEFSRg'
print('      ' + ''.join('%7s' % s for s in '.fo') + '   total')
for m in mk:
    v = [ms(tot[(m, s)]) for s in '.fo']
    print('  M %s ' % m + ''.join('%7.2f' % x for x in v) + '  %7.2f' % sum(v))
print('  sum ' + ''.join('%7.2f' % sum(ms(tot[(m, s)]) for m in mk) for s in '.fo'))
