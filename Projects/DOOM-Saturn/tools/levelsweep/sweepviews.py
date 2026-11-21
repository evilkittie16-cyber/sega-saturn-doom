"""Viewpoints for the all-level render sweep (2026-11-21).

usage: python -I sweepviews.py DOOM.WAD out.txt [npos] [maps]
Per map: the player-1 start (its angle + 3 more) and `npos` subsector
centroids (same rule as saturn_bench.c: every k-th subsector, sector at
least 64 high, centroid inside that subsector) x 4 angles.
Line format (DOOM-Windows -sweep): EP MAP X Y DEG TAG
"""
import struct, sys

def lumps(w):
    n, off = struct.unpack_from('<ii', w, 4)
    return [(struct.unpack_from('<ii8s', w, off + 16 * i)) for i in range(n)]

def main():
    wad = open(sys.argv[1], 'rb').read()
    out = open(sys.argv[2], 'w')
    npos = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    only = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else None
    d = lumps(wad)
    names = [x[2].rstrip(b'\0').decode('ascii', 'replace') for x in d]
    total = 0
    for i, nm in enumerate(names):
        if not (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M'):
            continue
        if only and nm not in only:
            continue
        L = {}
        for j in range(1, 11):
            L[names[i + j]] = wad[d[i + j][0]:d[i + j][0] + d[i + j][1]]
        def arr(name, fmt):
            s = struct.calcsize(fmt); b = L[name]
            return [struct.unpack_from(fmt, b, k * s) for k in range(len(b) // s)]
        things = arr('THINGS', '<hhhhh')
        verts = arr('VERTEXES', '<hh')
        segs = arr('SEGS', '<hhhhhh')        # v1 v2 ang line side off
        lines = arr('LINEDEFS', '<hhhhhhh')  # v1 v2 flags special tag s0 s1
        sides = arr('SIDEDEFS', '<hh8s8s8sh')
        secs = arr('SECTORS', '<hh8s8shhh')
        ssec = arr('SSECTORS', '<hh')        # count first
        nodes = arr('NODES', '<hhhh4h4hHH')
        def seg_sector(sg):
            ln = lines[sg[3]]
            sd = ln[5] if sg[4] == 0 else ln[6]
            return sides[sd][5]
        def point_ss(x, y):
            n = len(nodes) - 1
            while True:
                nx, ny, dx, dy = nodes[n][0:4]
                # vanilla R_PointOnSide: right < left -> 0 (front), else 1:
                # ties go to the BACK side (2026-11-21: '>' here accepted
                # points lying on partition lines = cameras inside walls)
                side = 1 if (dx * (y - ny) - dy * (x - nx)) >= 0 else 0
                c = nodes[n][12 + side]
                if c & 0x8000:
                    return c & 0x7FFF
                n = c
        def inside_margin(s, x, y, margin):
            # point at least `margin` units on the inner (right) side of
            # every seg line of subsector s
            cnt, first = ssec[s]
            for q in range(cnt):
                v1 = verts[segs[first + q][0]]; v2 = verts[segs[first + q][1]]
                ex, ey = v2[0] - v1[0], v2[1] - v1[1]
                ln = (ex * ex + ey * ey) ** 0.5 or 1.0
                cross = (ex * (y - v1[1]) - ey * (x - v1[0])) / ln
                if cross > -margin:
                    return False
            return True
        ep, mp = int(nm[1]), int(nm[3])
        views = []
        for t in things:
            if t[3] == 1:
                views.append((t[0], t[1], t[2], 'start'))
                break
        nss = len(ssec)
        stride = max(1, nss // npos)
        k = 0
        cands = []
        for base in range(stride // 2, nss - 1, stride):
            cands.extend(range(base, min(base + stride, nss - 1)))   # slot base first, then the rest of its stride
        used_slot = set()
        for s in cands:
            if k >= npos:
                break
            slot = (s - stride // 2) // stride
            if slot in used_slot:
                continue
            cnt, first = ssec[s]
            if cnt < 3:
                continue    # segs only run along walls: 1-2 segs = centroid on a wall
            sx = sum(verts[segs[first + q][0]][0] for q in range(cnt)) // cnt
            sy = sum(verts[segs[first + q][0]][1] for q in range(cnt)) // cnt
            sec = secs[seg_sector(segs[first])]
            if sec[1] - sec[0] < 64:
                continue
            if point_ss(sx, sy) != s or not inside_margin(s, sx, sy, 24):
                continue
            views.append((sx, sy, None, 'p%02d' % k))
            used_slot.add(slot)
            k += 1
        for (x, y, a, tag) in views:
            angs = [a, (a + 90) % 360, (a + 180) % 360, (a + 270) % 360] if a is not None else [0, 90, 180, 270]
            for ang in angs:
                out.write('%d %d %d %d %d %s_%s_a%03d\n' % (ep, mp, x, y, ang, nm, tag, ang))
                total += 1
    print('views', total)

main()
