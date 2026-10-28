# tools/bake_pvs.py - Offline Potentially Visible Set (PVS) Generator for Sega Saturn
#
# Reads DOOM.WAD and generates 2D visibility bitmasks for subsectors across
# all maps E1M1 through E4M9 to enable instant 1-cycle occlusion culling in the
# hardware renderer (HW_RenderSubsector).
#
# Output: cd/<MAP>.PVS

import struct, sys, os, time

WAD_PATH = os.environ.get("DOOM_WAD", None)
if not WAD_PATH or not os.path.exists(WAD_PATH):
    for candidate in ["cd/DOOM.WAD", "cd/DOOM1.WAD", "DOOM.WAD", "DOOM1.WAD"]:
        if os.path.exists(candidate):
            WAD_PATH = candidate
            break
    else:
        WAD_PATH = "cd/DOOM.WAD"
CELL_SIZE = 256

def load_wad(path):
    data = open(path, "rb").read()
    assert data[:4] == b"IWAD", "Not an IWAD"
    numlumps, infotableofs = struct.unpack_from("<ii", data, 4)
    directory = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        fpos, size = struct.unpack_from("<ii", data, off)
        name = data[off + 8:off + 16].split(b"\0")[0].decode("latin1")
        directory.append((name, fpos, size))
    return data, directory

def get_map_lumps(wad_data, directory, mapname):
    map_idx = -1
    for i, (name, fpos, size) in enumerate(directory):
        if name == mapname:
            map_idx = i
            break
    if map_idx == -1:
        return None

    lumps = {}
    for i in range(map_idx + 1, min(map_idx + 12, len(directory))):
        name, fpos, size = directory[i]
        lumps[name] = wad_data[fpos:fpos + size]
    return lumps

def seg_intersect(p1, p2, p3, p4):
    d = (p4[1] - p3[1]) * (p2[0] - p1[0]) - (p4[0] - p3[0]) * (p2[1] - p1[1])
    if d == 0:
        return False
    uA = ((p4[0] - p3[0]) * (p1[1] - p3[1]) - (p4[1] - p3[1]) * (p1[0] - p3[0])) / d
    uB = ((p2[0] - p1[0]) * (p1[1] - p3[1]) - (p2[1] - p1[1]) * (p1[0] - p3[0])) / d
    return (0.001 < uA < 0.999) and (0.001 < uB < 0.999)

def compute_map_pvs(mapname, lumps):
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

    pvs = [[0] * n_ss for _ in range(n_ss)]

    def ray_3d_clear(i, j, p1, p2):
        rx0, rx1 = min(p1[0], p2[0]), max(p1[0], p2[0])
        ry0, ry1 = min(p1[1], p2[1]), max(p1[1], p2[1])
        gx0, gx1 = int(rx0 // CELL_SIZE), int(rx1 // CELL_SIZE)
        gy0, gy1 = int(ry0 // CELL_SIZE), int(ry1 // CELL_SIZE)
        dx = p2[0] - p1[0]
        dy = p2[1] - p1[1]
        portals = [(0.0, ss_z[i][0], ss_z[i][1]), (1.0, ss_z[j][0], ss_z[j][1])]
        tested = set()
        for gx in range(gx0, gx1 + 1):
            for gy in range(gy0, gy1 + 1):
                for wi in grid.get((gx, gy), ()):
                    if wi in tested:
                        continue
                    tested.add(wi)
                    w1, w2, is_solid, zbot, ztop, w_minx, w_miny, w_maxx, w_maxy = linedefs_info[wi]
                    if rx1 < w_minx or rx0 > w_maxx or ry1 < w_miny or ry0 > w_maxy:
                        continue
                    d = (w2[1] - w1[1]) * dx - (w2[0] - w1[0]) * dy
                    if d == 0:
                        continue
                    uA = ((w2[0] - w1[0]) * (p1[1] - w1[1]) - (w2[1] - w1[1]) * (p1[0] - w1[0])) / d
                    if 0.001 < uA < 0.999:
                        uB = (dx * (p1[1] - w1[1]) - dy * (p1[0] - w1[0])) / d
                        if -1e-5 <= uB <= 1.0 + 1e-5:
                            if is_solid or zbot >= ztop:
                                return False
                            portals.append((uA, zbot, ztop))

        portals.sort(key=lambda x: x[0])
        s_min = -1e9
        s_max = 1e9
        n = len(portals)
        for a in range(n):
            ua, bota, topa = portals[a]
            for b in range(a + 1, n):
                ub, botb, topb = portals[b]
                du = ub - ua
                if du <= 1e-5:
                    if max(bota, botb) > min(topa, topb):
                        return False
                    continue
                lo = (botb - topa) / du
                hi = (topb - bota) / du
                if lo > s_min: s_min = lo
                if hi < s_max: s_max = hi
                if s_min > s_max:
                    return False
        return True

    for i in range(n_ss):
        pvs[i][i] = 1
        pts_i = ss_pts[i]
        for j in range(i + 1, n_ss):
            pts_j = ss_pts[j]
            # 1. Centroid-to-centroid ray (fast path)
            if ray_3d_clear(i, j, pts_i[0], pts_j[0]):
                pvs[i][j] = 1
                pvs[j][i] = 1
                continue

            # 2. Check representative perimeter rays
            vis = False
            for pi in pts_i:
                for pj in pts_j:
                    if ray_3d_clear(i, j, pi, pj):
                        vis = True
                        break
                if vis:
                    break

            if vis:
                pvs[i][j] = 1
                pvs[j][i] = 1

    rowbytes = (n_ss + 7) >> 3
    total_data_size = 16 + n_ss * rowbytes
    out_buf = bytearray(total_data_size)

    # Header: "PVS1", version=1, numsubsectors, rowbytes, reserved=0
    struct.pack_into(">4sHHHI", out_buf, 0, b"PVS1", 1, n_ss, rowbytes, 0)

    for i in range(n_ss):
        base = 16 + i * rowbytes
        for j in range(n_ss):
            if pvs[i][j]:
                out_buf[base + (j >> 3)] |= (1 << (j & 7))

    avg_vis = sum(sum(r) for r in pvs) / float(n_ss)
    cull_pct = 100.0 - (avg_vis / float(n_ss) * 100.0)
    return out_buf, avg_vis, cull_pct

def main():
    wad_data, directory = load_wad(WAD_PATH)
    maps = sys.argv[1:] if len(sys.argv) > 1 else [f"E{e}M{m}" for e in range(1, 5) for m in range(1, 10)]

    for m in maps:
        lumps = get_map_lumps(wad_data, directory, m)
        if not lumps:
            print(f"Skipping {m}: not found")
            continue

        t0 = time.time()
        buf, avg_vis, cull_pct = compute_map_pvs(m, lumps)
        dt = time.time() - t0

        out_path = os.path.join("cd", f"{m}.PVS")
        with open(out_path, "wb") as f:
            f.write(buf)

        win_dir = os.path.join("..", "DOOM-Windows")
        if os.path.isdir(win_dir):
            with open(os.path.join(win_dir, f"{m}.PVS"), "wb") as f:
                f.write(buf)

        n_ss = len(lumps["SSECTORS"]) // 4
        print(f"Baked {out_path:12s} ({len(buf):5d} B) in {dt:5.2f}s: {n_ss:3d} subsectors, avg vis {avg_vis:4.1f} ({100.0-cull_pct:4.1f}%), culling {cull_pct:4.1f}%")

if __name__ == "__main__":
    main()
