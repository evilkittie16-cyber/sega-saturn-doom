# Dense, conservative-er PVS re-bake (2026-11-27). Superset of the shipped
# PVS: re-tests only the pairs the old row marks invisible, with ~20 sample
# points per subsector (inset polygon corners, edge points, interior grid)
# instead of centroid + 6. Same 3D portal ray test as tools/bake_pvs.py.
# usage: python bake_pvs_dense.py <DOOM.WAD> <PAK dir> <out dir> MAP [MAP...]
# Reads the old PVS from <PAK dir>/<MAP>.PAK, writes <out dir>/<MAP>.PVS;
# swap it into the PAK with pak_swap_pvs.py. Env PVS_EDGE/PVS_GRID/PVS_CAP
# (16/32/24) set the sampling. 2026-11-27: all 36 retail maps ~20 min on 32
# threads; validated 0 wrong sight answers (p_sight.c SIGHT_PVS=2).
import struct, sys, os, time, math
from multiprocessing import Pool
CELL_SIZE = 256
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("DOOM_WAD", sys.argv[1] if len(sys.argv) > 1 else "")
from bake_pvs import load_wad, get_map_lumps
def setup_map(mapname, lumps):
    v_data = lumps["VERTEXES"]
    l_data = lumps["LINEDEFS"]
    s_data = lumps["SEGS"]
    ss_data = lumps["SSECTORS"]
    sd_data = lumps["SIDEDEFS"]
    sec_data = lumps["SECTORS"]
    n_data = lumps.get("NODES", b"")

    n_v = len(v_data) // 4
    verts = [struct.unpack_from("<hh", v_data, i * 4) for i in range(n_v)]

    n_l = len(l_data) // 14
    lines = [struct.unpack_from("<hhhhhhh", l_data, i * 14) for i in range(n_l)]

    n_s = len(s_data) // 12
    segs = [struct.unpack_from("<hhhhhh", s_data, i * 12) for i in range(n_s)]

    n_ss = len(ss_data) // 4
    ssect = [struct.unpack_from("<hh", ss_data, i * 4) for i in range(n_ss)]

    n_sec = len(sec_data) // 26
    secs = [struct.unpack_from("<hh8s8shhh", sec_data, i * 26) for i in range(n_sec)]

    n_nodes = len(n_data) // 28
    nodes = [struct.unpack_from("<12h2H", n_data, i * 28) for i in range(n_nodes)]

    def clip_poly(poly, x, y, dx, dy, side):
        out = []
        n = len(poly)
        if n == 0:
            return out
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            va = (a[1] - y) * dx - (a[0] - x) * dy
            vb = (b[1] - y) * dx - (b[0] - x) * dy
            ina = (va <= 1e-4) if side == 0 else (va >= -1e-4)
            inb = (vb <= 1e-4) if side == 0 else (vb >= -1e-4)
            if ina:
                out.append(a)
            if (va < -1e-4 and vb > 1e-4) or (va > 1e-4 and vb < -1e-4):
                t = va / float(va - vb)
                out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
        return out

    leaf_polys = {}
    if n_nodes > 0:
        def walk_bsp_poly(n_idx, poly):
            if n_idx & 0x8000:
                ss_i = n_idx & 0x7FFF
                if ss_i < n_ss:
                    cnt, first = ssect[ss_i]
                    for k in range(cnt):
                        sg = segs[first + k]
                        v0, v1 = verts[sg[0]], verts[sg[1]]
                        c = clip_poly(poly, v0[0], v0[1], v1[0] - v0[0], v1[1] - v0[1], 0)
                        if c:
                            poly = c
                    leaf_polys[ss_i] = poly
                return
            if n_idx >= n_nodes:
                return
            nd = nodes[n_idx]
            x, y, dx, dy = nd[0], nd[1], nd[2], nd[3]
            walk_bsp_poly(nd[12], clip_poly(poly, x, y, dx, dy, 0))
            walk_bsp_poly(nd[13], clip_poly(poly, x, y, dx, dy, 1))

        init_poly = [(-32768.0, -32768.0), (32767.0, -32768.0), (32767.0, 32767.0), (-32768.0, 32767.0)]
        walk_bsp_poly(n_nodes - 1, init_poly)

    min_map_fl = min((sc[0] for sc in secs), default=-32768)
    max_map_ce = max((sc[1] for sc in secs), default=32767)

    # Identify movable sectors (tagged sectors, local doors/lifts, and stair/donut neighbors)
    movable = set(i for i, sc in enumerate(secs) if sc[6] != 0)
    has_stair_or_donut = False
    for ld in lines:
        spec, tag, s0, s1 = ld[3], ld[4], ld[5], ld[6]
        if spec != 0 and spec != 48:
            if s1 >= 0:
                movable.add(struct.unpack_from("<h", sd_data, s1 * 30 + 28)[0])
            if s0 >= 0 and spec in (7, 8, 9, 100, 127):
                has_stair_or_donut = True
            if tag != 0:
                for i, sc in enumerate(secs):
                    if sc[6] == tag:
                        movable.add(i)

    if has_stair_or_donut:
        # Propagate movable status across 2-sided linedefs with matching floor texture for stair builders
        changed = True
        while changed:
            changed = False
            for ld in lines:
                if ld[5] >= 0 and ld[6] >= 0:
                    sec0 = struct.unpack_from("<h", sd_data, ld[5] * 30 + 28)[0]
                    sec1 = struct.unpack_from("<h", sd_data, ld[6] * 30 + 28)[0]
                    if (sec0 in movable) != (sec1 in movable):
                        movable.add(sec0)
                        movable.add(sec1)
                        changed = True

    def sec_bounds(seci):
        fl, ce, fp, cp, ll, sp, tag = secs[seci]
        is_sky = (cp.split(b"\0")[0] == b"F_SKY1")
        if seci in movable:
            return (min_map_fl, max_map_ce, is_sky)
        return (fl, ce, is_sky)

    # Collect all linedefs (1-sided solid walls + 2-sided 3D portals) into a 2D spatial grid
    linedefs_info = []
    grid = {}
    for ld in lines:
        p1 = verts[ld[0]]
        p2 = verts[ld[1]]
        min_x, max_x = min(p1[0], p2[0]), max(p1[0], p2[0])
        min_y, max_y = min(p1[1], p2[1]), max(p1[1], p2[1])
        if ld[6] == -1:
            info = (p1, p2, True, 0, 0, min_x, min_y, max_x, max_y)
        else:
            s0 = struct.unpack_from("<h", sd_data, ld[5] * 30 + 28)[0]
            s1 = struct.unpack_from("<h", sd_data, ld[6] * 30 + 28)[0]
            fl0, ce0, sky0 = sec_bounds(s0)
            fl1, ce1, sky1 = sec_bounds(s1)
            zbot = max(fl0, fl1)
            ztop = 32767 if (sky0 and sky1) else min(ce0, ce1)
            info = (p1, p2, False, zbot, ztop, min_x, min_y, max_x, max_y)
        idx = len(linedefs_info)
        linedefs_info.append(info)
        gx0, gx1 = int(min_x // CELL_SIZE), int(max_x // CELL_SIZE)
        gy0, gy1 = int(min_y // CELL_SIZE), int(max_y // CELL_SIZE)
        for gx in range(gx0, gx1 + 1):
            for gy in range(gy0, gy1 + 1):
                grid.setdefault((gx, gy), []).append(idx)

    # Subsector sample points (Centroid + inset perimeter vertices) and vertical Z bounds
    ss_pts = []
    ss_z = []
    for ss_idx in range(n_ss):
        cnt, first = ssect[ss_idx]
        seg0 = segs[first]
        ld0 = lines[seg0[3]]
        sdi0 = ld0[5] if seg0[4] == 0 else ld0[6]
        seci0 = struct.unpack_from("<h", sd_data, sdi0 * 30 + 28)[0]
        min_f, max_c, _ = sec_bounds(seci0)

        pts = set()
        for k in range(cnt):
            s = segs[first + k]
            pts.add(verts[s[0]])
            pts.add(verts[s[1]])
            ld = lines[s[3]]
            if ld[6] >= 0:
                for sdi in (ld[5], ld[6]):
                    seci = struct.unpack_from("<h", sd_data, sdi * 30 + 28)[0]
                    bf, bc, _ = sec_bounds(seci)
                    if bf < min_f: min_f = bf
                    if bc > max_c: max_c = bc
        ss_z.append((min_f, max_c))

        pts = list(pts)
        bpoly = leaf_polys.get(ss_idx, [])
        all_pts = list(bpoly) if bpoly else list(pts)
        for p in pts:
            if all(abs(p[0] - q[0]) > 1.0 or abs(p[1] - q[1]) > 1.0 for q in all_pts):
                all_pts.append(p)
        if all_pts:
            cx = sum(p[0] for p in all_pts) / len(all_pts)
            cy = sum(p[1] for p in all_pts) / len(all_pts)
            pts_sorted = sorted(all_pts, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2, reverse=True)
            chosen = [(cx, cy)] + [(cx + (px - cx) * 0.98, cy + (py - cy) * 0.98) for px, py in pts_sorted[:6]]
        else:
            chosen = [(0.0, 0.0)]
        ss_pts.append(chosen)

    return dict(n_ss=n_ss, ss_z=ss_z, grid=grid, linedefs_info=linedefs_info,
                leaf_polys=leaf_polys, ss_pts=ss_pts)

G = {}

def dense_points(poly, fallback, step_edge=float(os.environ.get('PVS_EDGE','16')), step_grid=float(os.environ.get('PVS_GRID','32')), cap=int(os.environ.get('PVS_CAP','24'))):
    pts = list(poly) if poly else list(fallback)
    if not pts:
        return fallback
    cx = sum(p[0] for p in pts) / len(pts); cy = sum(p[1] for p in pts) / len(pts)
    def inset(p, d=1.5):
        vx, vy = cx - p[0], cy - p[1]; L = math.hypot(vx, vy)
        if L < 1e-6: return p
        k = min(d / L, 0.5); return (p[0] + vx * k, p[1] + vy * k)
    out = [(cx, cy)] + [inset(p) for p in pts]
    n = len(pts)
    for a in range(n):
        p, q = pts[a], pts[(a + 1) % n]
        L = math.hypot(q[0] - p[0], q[1] - p[1]); m = int(L // step_edge)
        for t in range(1, m):
            out.append(inset((p[0] + (q[0] - p[0]) * t / m, p[1] + (q[1] - p[1]) * t / m), 2.0))
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    def inside(x, y):
        s = 0
        for a in range(n):
            p, q = pts[a], pts[(a + 1) % n]
            c = (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0])
            if c != 0:
                if s == 0: s = 1 if c > 0 else -1
                elif (c > 0) != (s > 0): return False
        return True
    gx = math.floor(min(xs) / step_grid) * step_grid
    while gx <= max(xs):
        gy = math.floor(min(ys) / step_grid) * step_grid
        while gy <= max(ys):
            if inside(gx, gy): out.append((gx, gy))
            gy += step_grid
        gx += step_grid
    if len(out) > cap:
        keep = out[:1 + n] if 1 + n <= cap else out[:cap]
        rest = out[len(keep):]
        need = cap - len(keep)
        if need > 0 and rest:
            stride = len(rest) / need
            keep += [rest[int(k * stride)] for k in range(need)]
        out = keep
    return out

def ray_3d_clear(i, j, p1, p2):
    ss_z = G['ss_z']; grid = G['grid']; linedefs_info = G['linedefs_info']
    rx0, rx1 = min(p1[0], p2[0]), max(p1[0], p2[0])
    ry0, ry1 = min(p1[1], p2[1]), max(p1[1], p2[1])
    gx0, gx1 = int(rx0 // CELL_SIZE), int(rx1 // CELL_SIZE)
    gy0, gy1 = int(ry0 // CELL_SIZE), int(ry1 // CELL_SIZE)
    dx = p2[0] - p1[0]; dy = p2[1] - p1[1]
    portals = [(0.0, ss_z[i][0], ss_z[i][1]), (1.0, ss_z[j][0], ss_z[j][1])]
    tested = set()
    for gx in range(gx0, gx1 + 1):
        for gy in range(gy0, gy1 + 1):
            for wi in grid.get((gx, gy), ()):
                if wi in tested: continue
                tested.add(wi)
                w1, w2, is_solid, zbot, ztop, w_minx, w_miny, w_maxx, w_maxy = linedefs_info[wi]
                if rx1 < w_minx or rx0 > w_maxx or ry1 < w_miny or ry0 > w_maxy: continue
                d = (w2[1] - w1[1]) * dx - (w2[0] - w1[0]) * dy
                if d == 0: continue
                uA = ((w2[0] - w1[0]) * (p1[1] - w1[1]) - (w2[1] - w1[1]) * (p1[0] - w1[0])) / d
                if 0.001 < uA < 0.999:
                    uB = (dx * (p1[1] - w1[1]) - dy * (p1[0] - w1[0])) / d
                    if -1e-5 <= uB <= 1.0 + 1e-5:
                        if is_solid or zbot >= ztop: return False
                        portals.append((uA, zbot, ztop))
    portals.sort(key=lambda x: x[0])
    s_min = -1e9; s_max = 1e9; n = len(portals)
    for a in range(n):
        ua, bota, topa = portals[a]
        for b in range(a + 1, n):
            ub, botb, topb = portals[b]
            du = ub - ua
            if du <= 1e-5:
                if max(bota, botb) > min(topa, topb): return False
                continue
            lo = (botb - topa) / du; hi = (topb - bota) / du
            if lo > s_min: s_min = lo
            if hi < s_max: s_max = hi
            if s_min > s_max: return False
    return True

def init_worker(g):
    G.update(g)

def row_task(i):
    pts = G['dense']; old = G['old']; n = G['n_ss']; found = []
    for j in range(i + 1, n):
        if old[i][j]: continue
        vis = False
        for pi in pts[i]:
            for pj in pts[j]:
                if ray_3d_clear(i, j, pi, pj):
                    vis = True; break
            if vis: break
        if vis: found.append(j)
    return i, found

def read_pak_pvs(path):
    b = open(path, 'rb').read()
    ns = struct.unpack('>H', b[6:8])[0]
    for k in range(ns):
        e = b[16 + 16 * k: 32 + 16 * k]
        if e[:4] == b'PVS ':
            off, size = struct.unpack('>II', e[4:12])
            return b[off * 2048: off * 2048 + size]
    return None

def main():
    wad, pakdir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    maps = sys.argv[4:]
    wad_data, directory = load_wad(wad)
    os.makedirs(outdir, exist_ok=True)
    for m in maps:
        t0 = time.time()
        lumps = get_map_lumps(wad_data, directory, m)
        S = setup_map(m, lumps)
        n = S['n_ss']
        oldb = read_pak_pvs(os.path.join(pakdir, m + '.PAK'))
        rb = struct.unpack('>H', oldb[8:10])[0]
        assert struct.unpack('>H', oldb[6:8])[0] == n
        old = [[(oldb[16 + i * rb + (j >> 3)] >> (j & 7)) & 1 for j in range(n)] for i in range(n)]
        dense = [dense_points(S['leaf_polys'].get(i, []), S['ss_pts'][i]) for i in range(n)]
        g = dict(ss_z=S['ss_z'], grid=S['grid'], linedefs_info=S['linedefs_info'], dense=dense, old=old, n_ss=n)
        added = 0
        new = [row[:] for row in old]
        with Pool(30, initializer=init_worker, initargs=(g,)) as pool:
            for i, found in pool.imap_unordered(row_task, range(n), chunksize=4):
                for j in found:
                    new[i][j] = new[j][i] = 1; added += 1
        out = bytearray(oldb[:16 + n * rb])
        for i in range(n):
            for j in range(n):
                if new[i][j]: out[16 + i * rb + (j >> 3)] |= (1 << (j & 7))
        open(os.path.join(outdir, m + '.PVS'), 'wb').write(out)
        oldvis = sum(map(sum, old)); newvis = sum(map(sum, new))
        print(f'{m}: {n} ss, pts/ss avg {sum(map(len, dense))/n:.1f}, pairs added {added}, vis {oldvis} -> {newvis} (+{100*(newvis-oldvis)/max(oldvis,1):.2f}%), {time.time()-t0:.0f}s', flush=True)

if __name__ == '__main__':
    main()