# tools/bake_flats.py - Offline 3D Floor & Ceiling Quad Mesh Generator for Sega Saturn VDP1
#
# Reads DOOM.WAD levels and pre-calculates watertight, pure 4-vertex convex quadrilaterals
# for every subsector floor and ceiling.
#
# Saturn Hardware Constraints:
# 1. Zero Triangles: Every polygon is tessellated into pure 4-vertex quads.
# 2. Clockwise winding: Point A = Top-Left, Point B = Top-Right, Point C = Bottom-Right, Point D = Bottom-Left.
# 3. CD Map Data: Output binary .FLT files to cd/<MAP>.FLT loaded dynamically into PU_LEVEL.

import struct, sys, os, math
from fractions import Fraction

WAD_PATH = os.environ.get("DOOM_WAD", None)
if not WAD_PATH or not os.path.exists(WAD_PATH):
    for candidate in [
        "cd/DOOM.WAD",
        "cd/DOOM1.WAD",
        "Projects/DOOM-Saturn/cd/DOOM.WAD",
        "Projects/DOOM-Saturn/cd/DOOM1.WAD",
        "../DOOM-Saturn/cd/DOOM.WAD",
        "../DOOM-Saturn/cd/DOOM1.WAD",
        "../../DOOM.WAD",
        "../../DOOM1.WAD",
        "../DOOM.WAD",
        "../DOOM1.WAD",
        "DOOM.WAD",
        "DOOM1.WAD",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cd", "DOOM.WAD"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cd", "DOOM1.WAD"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "DOOM.WAD"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "DOOM1.WAD")
    ]:
        if os.path.exists(candidate):
            WAD_PATH = candidate
            break
skirt_stats = [0, 0]   # [cells skirted, cells total] for the current map

def load_wad(path):
    data = open(path, "rb").read()
    assert data[:4] == b"IWAD", "Not an IWAD"
    numlumps, infotableofs = struct.unpack_from("<ii", data, 4)
    directory = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        fpos, size = struct.unpack_from("<ii", data, off)
        name = data[off + 8:off + 16].rstrip(b"\0").decode()
        directory.append((name, fpos, size))
    return data, directory

def fline_val(p, x0, y0, dx, dy):
    return dx * (p[1] - y0) - dy * (p[0] - x0)

def line_isect_point(P, Q, fP, fQ):
    t = fP / (fP - fQ)
    return (P[0] + (Q[0] - P[0]) * t, P[1] + (Q[1] - P[1]) * t)

def clip_halfplane(poly, x0, y0, dx, dy, sgn):
    out = []
    n = len(poly)
    for i in range(n):
        P, Q = poly[i], poly[(i + 1) % n]
        fP = fline_val(P, x0, y0, dx, dy)
        fQ = fline_val(Q, x0, y0, dx, dy)
        if (fP >= 0) if sgn >= 0 else (fP <= 0):
            out.append(P)
        if (fP > 0 and fQ < 0) or (fP < 0 and fQ > 0):
            out.append(line_isect_point(P, Q, fP, fQ))
    return out

def drop_collinear(poly):
    n = len(poly)
    out = []
    for i in range(n):
        A, B, C = poly[(i - 1) % n], poly[i], poly[(i + 1) % n]
        cross = (B[0] - A[0]) * (C[1] - B[1]) - (B[1] - A[1]) * (C[0] - B[0])
        if cross != 0:
            out.append(B)
    return out

def poly_area2(poly):
    s = Fraction(0)
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        s += x1 * y2 - x2 * y1
    return s

def build_cell(ss, SSECTORS, SEGS, VERTEXES, NODES, paths):
    pad = 32768
    poly = [(Fraction(-pad), Fraction(-pad)), (Fraction(pad), Fraction(-pad)),
            (Fraction(pad), Fraction(pad)), (Fraction(-pad), Fraction(pad))]
    for (ni, side) in paths[ss]:
        x, y, dx, dy = NODES[ni][0], NODES[ni][1], NODES[ni][2], NODES[ni][3]
        poly = clip_halfplane(poly, x, y, dx, dy, -1 if side == 0 else 1)
        if not poly:
            return []
    n, f = SSECTORS[ss]
    used = [f + k for k in range(n) if SEGS[f + k][0] != SEGS[f + k][1]]
    for si in used:
        (x1, y1), (x2, y2) = VERTEXES[SEGS[si][0]], VERTEXES[SEGS[si][1]]
        dxw, dyw = x2 - x1, y2 - y1
        poly = clip_halfplane(poly, x1, y1, dxw, dyw, -1)
        if not poly:
            return []
    poly = drop_collinear(poly)
    if len(poly) < 3:
        return []
    a2 = poly_area2(poly)
    if a2 == 0:
        return []
    if a2 < 0:
        poly.reverse()
    return poly

def quadify_poly(poly):
    """Decompose any 2D convex polygon into pure 4-vertex quadrilaterals via direct convex fan.
    Produces ceil((n - 2) / 2) quads and zero synthetic interior vertices (vs n quads and n+1
    extra vertices for Catmull-Clark), eliminating both diagonal UV swirling and 50%+ of N-gon quads."""
    n = len(poly)
    if n < 3:
        return []
    if n == 3:
        return [[poly[0], poly[1], poly[2], poly[2]]]
    if n == 4:
        return [poly]
    quads = []
    k = 1
    while n - k >= 3:
        quads.append([poly[0], poly[k], poly[k + 1], poly[k + 2]])
        k += 2
    if n - k == 2:
        quads.append([poly[0], poly[k], poly[k + 1], poly[k + 1]])
    return quads

def clip_box(poly, x0, x1, y0, y1):
    out = list(poly)
    for var, sgn, val in [('x', 1, x0), ('x', -1, -x1), ('y', 1, y0), ('y', -1, -y1)]:
        nin = list(out)
        out = []
        if not nin:
            return []
        for i in range(len(nin)):
            p1, p2 = nin[i], nin[(i + 1) % len(nin)]
            v1 = (p1[0] if var == 'x' else p1[1]) * sgn - val
            v2 = (p2[0] if var == 'x' else p2[1]) * sgn - val
            if v1 >= 0:
                out.append(p1)
            if (v1 > 0 and v2 < 0) or (v1 < 0 and v2 > 0):
                t = v1 / (v1 - v2)
                out.append((p1[0] + (p2[0] - p1[0]) * t, p1[1] + (p2[1] - p1[1]) * t))
    return out

# --- Seam fix (rendering_plan.md §31) -------------------------------------
# 1. Vertices are stored at 1/16-unit precision: int16 floor part in the
#    existing vertex array + one byte (fx << 4) | fy in a parallel array.
# 2. "Skirt": cell edges lying on a ONE-SIDED seg of the subsector are pushed
#    1 unit outward (behind the wall). Painter's order draws flats before
#    walls, so the wall hides the extra strip and no floor/wall gap can show.
#    The cell is rebuilt as the intersection of its edge half-planes with
#    exact Fractions, so corners slide along the neighbouring (grid) lines and
#    adjacent cells compute bit-identical points. No new cells are created.
FRAC_BITS  = 4
FRAC_ONE   = 1 << FRAC_BITS
SKIRT      = 1          # map units pushed behind one-sided walls
SKIRT_MAX2 = 4 * 4      # skip a cell's skirt if any corner would move > 4 units

def quant16(v):
    """Float/Fraction coordinate -> integer count of 1/16 units."""
    return int(round(float(v) * FRAC_ONE))

def clip_plane(poly, nx, ny, c):
    """Keep the part of convex poly with nx*x + ny*y <= c (exact)."""
    out = []
    n = len(poly)
    for i in range(n):
        P, Q = poly[i], poly[(i + 1) % n]
        vP = c - (nx * P[0] + ny * P[1])
        vQ = c - (nx * Q[0] + ny * Q[1])
        if vP >= 0:
            out.append(P)
        if (vP > 0 and vQ < 0) or (vP < 0 and vQ > 0):
            t = vP / (vP - vQ)
            out.append((P[0] + (Q[0] - P[0]) * t, P[1] + (Q[1] - P[1]) * t))
    return out

def edge_on_seg(P, Q, sg):
    """True if cell edge P->Q lies on seg sg's line and inside its span."""
    x1, y1, dx, dy = sg
    if fline_val(P, x1, y1, dx, dy) != 0 or fline_val(Q, x1, y1, dx, dy) != 0:
        return False
    mx = (P[0] + Q[0]) / 2 - x1
    my = (P[1] + Q[1]) / 2 - y1
    t = (mx * dx + my * dy) / (dx * dx + dy * dy)
    return 0 < t < 1

def skirt_cell(cp, skirt_segs):
    """Push cp's edges that lie on one-sided segs SKIRT units outward.
    cp must be convex and CCW (positive poly_area2). Returns cp unchanged if
    nothing qualifies or the result would be degenerate / move too far."""
    if not skirt_segs:
        return cp
    n = len(cp)
    planes = []
    shifted = False
    for i in range(n):
        P, Q = cp[i], cp[(i + 1) % n]
        sg = next((s for s in skirt_segs if edge_on_seg(P, Q, s)), None)
        if sg is not None:
            # Seg inside is fline <= 0  <=>  (-dy)*x + dx*y <= (-dy)*x1 + dx*y1
            x1, y1, dx, dy = sg
            nx, ny = Fraction(-dy), Fraction(dx)
            c = nx * x1 + ny * y1 + Fraction(round(math.hypot(dx, dy) * 256 * SKIRT), 256)
            shifted = True
        else:
            # CCW edge: inside is left of P->Q  <=>  dY*x - dX*y <= dY*Px - dX*Py
            dX, dY = Q[0] - P[0], Q[1] - P[1]
            nx, ny = dY, -dX
            c = nx * P[0] + ny * P[1]
        planes.append((nx, ny, c))
    if not shifted:
        return cp
    pad = Fraction(65536)
    poly = [(-pad, -pad), (pad, -pad), (pad, pad), (-pad, pad)]
    for (nx, ny, c) in planes:
        poly = clip_plane(poly, nx, ny, c)
        if not poly:
            return cp
    poly = drop_collinear(poly)
    if len(poly) < 3 or poly_area2(poly) <= 0:
        return cp
    for p in poly:
        if min((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 for q in cp) > SKIRT_MAX2:
            return cp
    return poly

def mesh_subsector_poly(poly, skirt_segs, grid_size, track_skirt=False):
    xs = [float(p[0]) for p in poly]
    ys = [float(p[1]) for p in poly]

    gx0 = int(min(xs) // grid_size) * grid_size
    gx1 = int(max(xs) // grid_size + 1) * grid_size
    gy0 = int(min(ys) // grid_size) * grid_size
    gy1 = int(max(ys) // grid_size + 1) * grid_size

    valid_quads = []
    for gx in range(gx0, gx1, grid_size):
        for gy in range(gy0, gy1, grid_size):
            cp = clip_box(poly, Fraction(gx), Fraction(gx + grid_size), Fraction(gy), Fraction(gy + grid_size))
            if cp:
                cp = drop_collinear(cp)
                if len(cp) >= 3 and abs(poly_area2(cp)) > 0:
                    skirted = skirt_cell(cp, skirt_segs)
                    if track_skirt:
                        if skirted is not cp:
                            skirt_stats[0] += 1
                        skirt_stats[1] += 1
                    cp = skirted
                    f_cp = [(float(p[0]), float(p[1])) for p in cp]
                    corners = [(gx, gy), (gx + grid_size, gy), (gx + grid_size, gy + grid_size), (gx, gy + grid_size)]
                    if len(f_cp) == 4:
                        k = min(range(4), key=lambda i: (f_cp[i][0] - gx)**2 + (f_cp[i][1] - gy)**2)
                        valid_quads.append([f_cp[(k + j) % 4] for j in range(4)])
                    elif len(f_cp) == 3:
                        q = [min(f_cp, key=lambda pt: (pt[0] - c[0])**2 + (pt[1] - c[1])**2) for c in corners]
                        valid_quads.append(q)
                    else:
                        valid_quads.extend(quadify_poly(f_cp))

    sub_verts = []
    vert_map = {}
    sub_quad_indices = []

    for q in valid_quads:
        q_idx = []
        for p in q:
            pt = (quant16(p[0]), quant16(p[1]))
            if pt not in vert_map:
                vert_map[pt] = len(sub_verts)
                sub_verts.append(pt)
            q_idx.append(vert_map[pt])
        if len(set(q_idx)) >= 3:
            p0 = sub_verts[q_idx[0]]
            p1 = sub_verts[q_idx[1]]
            p2 = sub_verts[q_idx[2]]
            p3 = sub_verts[q_idx[3]]
            area = (p0[0]*p1[1] - p1[0]*p0[1] +
                    p1[0]*p2[1] - p2[0]*p1[1] +
                    p2[0]*p3[1] - p3[0]*p2[1] +
                    p3[0]*p0[1] - p0[0]*p3[1])
            if area != 0:
                sub_quad_indices.append(q_idx)

    return sub_verts, sub_quad_indices

def bake_map_adaptive(wad_data, directory, map_name):
    try:
        idx = next(i for i, (name, _, _) in enumerate(directory) if name == map_name)
    except StopIteration:
        print(f"Map {map_name} not found in WAD.")
        return None
    lumps = {directory[i][0]: (directory[i][1], directory[i][2]) for i in range(idx, idx + 11)}

    def i16s(name, per):
        fpos, size = lumps[name]
        n = size // (per * 2)
        return [struct.unpack_from("<%dh" % per, wad_data, fpos + i * per * 2) for i in range(n)]

    def hu16s(name, per):
        fpos, size = lumps[name]
        n = size // (per * 2)
        return [struct.unpack_from("<%dH" % per, wad_data, fpos + i * per * 2) for i in range(n)]

    VERTEXES = i16s("VERTEXES", 2)
    SEGS     = i16s("SEGS", 6)
    LINEDEFS = i16s("LINEDEFS", 7)   # (v1, v2, flags, special, tag, side0, side1); side1 == -1: one-sided
    SSECTORS = hu16s("SSECTORS", 2)
    NODES    = i16s("NODES", 14)
    NODES = [n[:12] + struct.unpack_from("<2H", wad_data, lumps["NODES"][0] + i * 28 + 24) for i, n in enumerate(NODES)]
    nsssec = len(SSECTORS)
    NF_SUBSECTOR = 0x8000

    # Sector lookup per subsector to identify outdoor (sky ceiling) areas
    sides_pos, sides_size = lumps["SIDEDEFS"]
    num_sides = sides_size // 30
    sidedef_sector = [struct.unpack_from("<h", wad_data, sides_pos + i * 30 + 28)[0] for i in range(num_sides)]

    sec_pos, sec_size = lumps["SECTORS"]
    num_sec = sec_size // 26
    sec_is_sky = []
    for i in range(num_sec):
        ceilt = wad_data[sec_pos + i * 26 + 12 : sec_pos + i * 26 + 20].rstrip(b"\0").decode("ascii", "ignore")
        sec_is_sky.append(ceilt == "F_SKY1")

    ss_is_sky = []
    ss_sector = []
    for ss in range(nsssec):
        n_sg, f_sg = SSECTORS[ss]
        ld = SEGS[f_sg][3]
        side = SEGS[f_sg][4]
        sd = LINEDEFS[ld][5 if side == 0 else 6]
        sec = sidedef_sector[sd]
        ss_sector.append(sec)
        ss_is_sky.append(sec_is_sky[sec])

    sec_bbox = {}
    for ld in LINEDEFS:
        v1, v2 = ld[0], ld[1]
        sd1, sd2 = ld[5], ld[6]
        s1 = sidedef_sector[sd1] if sd1 != -1 else -1
        s2 = sidedef_sector[sd2] if sd2 != -1 else -1
        for s in (s1, s2):
            if s != -1:
                if s not in sec_bbox:
                    sec_bbox[s] = [99999, 99999, -99999, -99999]
                bb = sec_bbox[s]
                for vx, vy in (VERTEXES[v1], VERTEXES[v2]):
                    bb[0] = min(bb[0], vx)
                    bb[1] = min(bb[1], vy)
                    bb[2] = max(bb[2], vx)
                    bb[3] = max(bb[3], vy)
    paths = [None] * nsssec
    def walk(u, path):
        if u & NF_SUBSECTOR:
            ss = u & (NF_SUBSECTOR - 1)
            paths[ss] = path
            return
        c0, c1 = NODES[u][12], NODES[u][13]
        walk(c0, path + [(u, 0)])
        walk(c1, path + [(u, 1)])
    walk(len(NODES) - 1, [])

    all_quads   = []
    all_verts   = []
    sub_q_first = []
    sub_q_count = []
    sub_v_first = []
    sub_v_count = []
    lod1_sub_q_first = []
    lod1_sub_q_count = []
    lod1_sub_v_first = []
    lod1_sub_v_count = []
    sub_bbox    = []

    sub_sector = []
    sub_skirt_segs = []
    for ss_idx in range(nsssec):
        n_sg, f_sg = SSECTORS[ss_idx]
        sk_segs = []
        if n_sg > 0:
            seg = SEGS[f_sg]
            ld = LINEDEFS[seg[3]]
            sd_idx = ld[5] if seg[4] == 0 else ld[6]
            sec = sidedef_sector[sd_idx] if sd_idx != -1 else 0
            for k in range(n_sg):
                si = f_sg + k
                sg = SEGS[si]
                if sg[0] == sg[1]: continue
                ld_i = LINEDEFS[sg[3]]
                if ld_i[6] == -1: # one-sided wall
                    (x1, y1), (x2, y2) = VERTEXES[sg[0]], VERTEXES[sg[1]]
                    sk_segs.append((Fraction(x1), Fraction(y1), Fraction(x2 - x1), Fraction(y2 - y1)))
        else:
            sec = 0
        sub_sector.append(sec)
        sub_skirt_segs.append(sk_segs)

    grid_size = 64

    # Build bounding boxes for all 1-sided solid walls (ld[6] == -1).
    # Cells touching 1-sided walls must never merge across subsectors.
    one_sided_walls = []
    for ld in LINEDEFS:
        if ld[6] == -1:
            v1, v2 = VERTEXES[ld[0]], VERTEXES[ld[1]]
            one_sided_walls.append((min(v1[0], v2[0]), max(v1[0], v2[0]), min(v1[1], v2[1]), max(v1[1], v2[1])))

    def touches_1sided_wall(gx, gy):
        for (wx0, wx1, wy0, wy1) in one_sided_walls:
            if wx1 >= gx and wx0 <= gx + grid_size and wy1 >= gy and wy0 <= gy + grid_size:
                return True
        return False

    # Pass 1: Build subsector polygons and accumulate cell area per sector
    sub_polys = []
    sec_cell_areas = {}
    sec_cell_max_sub = {}
    for ss in range(nsssec):
        poly = build_cell(ss, SSECTORS, SEGS, VERTEXES, NODES, paths)
        sub_polys.append(poly)
        if not poly:
            continue
        sec = sub_sector[ss]
        xs = [float(p[0]) for p in poly]
        ys = [float(p[1]) for p in poly]
        gx0 = int(min(xs) // grid_size) * grid_size
        gx1 = int(max(xs) // grid_size + 1) * grid_size
        gy0 = int(min(ys) // grid_size) * grid_size
        gy1 = int(max(ys) // grid_size + 1) * grid_size
        for gx in range(gx0, gx1, grid_size):
            for gy in range(gy0, gy1, grid_size):
                cp = clip_box(poly, Fraction(gx), Fraction(gx + grid_size),
                              Fraction(gy), Fraction(gy + grid_size))
                if cp:
                    cp = drop_collinear(cp)
                    a = float(abs(poly_area2(cp)) / 2)
                    if a > 0:
                        sec_cell_areas.setdefault(sec, {}).setdefault((gx, gy), 0.0)
                        sec_cell_areas[sec][(gx, gy)] += a
                        cur_max = sec_cell_max_sub.get((sec, gx, gy), (0.0, -1))
                        if a > cur_max[0]:
                            sec_cell_max_sub[(sec, gx, gy)] = (a, ss)

    def orient_cell_quads(cp, gx, gy):
        f_cp = [(float(p[0]), float(p[1])) for p in cp]
        if len(f_cp) == 4:
            k = min(range(4), key=lambda i: (f_cp[i][0] - gx)**2 + (f_cp[i][1] - gy)**2)
            q = [f_cp[(k + j) % 4] for j in range(4)]
            p0, p1, p2, p3 = q[0], q[1], q[2], q[3]
            area = (p0[0]*p1[1] - p1[0]*p0[1] +
                    p1[0]*p2[1] - p2[0]*p1[1] +
                    p2[0]*p3[1] - p3[0]*p2[1] +
                    p3[0]*p0[1] - p0[0]*p3[1])
            if area < 0:
                q = [q[0], q[3], q[2], q[1]]
            return [q]
        elif len(f_cp) == 3:
            p0, p1, p2 = f_cp[0], f_cp[1], f_cp[2]
            area = (p0[0]*p1[1] - p1[0]*p0[1] +
                    p1[0]*p2[1] - p2[0]*p1[1] +
                    p2[0]*p0[1] - p0[0]*p2[1])
            if area < 0:
                f_cp = [p0, p2, p1]
            k = min(range(3), key=lambda i: (f_cp[i][0] - gx)**2 + (f_cp[i][1] - gy)**2)
            q = [f_cp[(k + j) % 3] for j in range(3)]
            return [[q[0], q[1], q[2], q[2]]]
        else:
            a = sum(f_cp[i][0]*f_cp[(i+1)%len(f_cp)][1] - f_cp[(i+1)%len(f_cp)][0]*f_cp[i][1] for i in range(len(f_cp)))
            if a < 0:
                f_cp = list(reversed(f_cp))
            k0 = min(range(len(f_cp)), key=lambda i: (f_cp[i][0] - gx)**2 + (f_cp[i][1] - gy)**2)
            f_cp = [f_cp[(k0 + j) % len(f_cp)] for j in range(len(f_cp))]
            raw_quads = quadify_poly(f_cp)
            res = []
            for q in raw_quads:
                qa = (q[0][0]*q[1][1] - q[1][0]*q[0][1] +
                      q[1][0]*q[2][1] - q[2][0]*q[1][1] +
                      q[2][0]*q[3][1] - q[3][0]*q[2][1] +
                      q[3][0]*q[0][1] - q[0][0]*q[3][1])
                if qa < 0:
                    if q[2] == q[3]:
                        q = [q[0], q[2], q[1], q[1]]
                    else:
                        q = [q[0], q[3], q[2], q[1]]
                res.append(q)
            return res

    # Pass 2: Generate watertight, sector-bounded flat quads
    for ss in range(nsssec):
        poly = sub_polys[ss]
        if not poly:
            sub_q_first.append(len(all_quads))
            sub_q_count.append(0)
            sub_v_first.append(len(all_verts))
            sub_v_count.append(0)
            lod1_sub_q_first.append(0)
            lod1_sub_q_count.append(0)
            lod1_sub_v_first.append(0)
            lod1_sub_v_count.append(0)
            sub_bbox.append((0, 0, 0, 0))
            continue

        sec = sub_sector[ss]
        xs = [float(p[0]) for p in poly]
        ys = [float(p[1]) for p in poly]
        gx0 = int(min(xs) // grid_size) * grid_size
        gx1 = int(max(xs) // grid_size + 1) * grid_size
        gy0 = int(min(ys) // grid_size) * grid_size
        gy1 = int(max(ys) // grid_size + 1) * grid_size

        sub_quads = []
        for gx in range(gx0, gx1, grid_size):
            for gy in range(gy0, gy1, grid_size):
                cp = clip_box(poly, Fraction(gx), Fraction(gx + grid_size),
                              Fraction(gy), Fraction(gy + grid_size))
                if cp:
                    cp = drop_collinear(cp)
                    a = float(abs(poly_area2(cp)) / 2)
                    if len(cp) >= 3 and a > 0:
                        if a >= 4096.0 - 0.5:
                            # Interior cell: 100% enclosed within this subsector.
                            corners = [(gx, gy), (gx + grid_size, gy), (gx + grid_size, gy + grid_size), (gx, gy + grid_size)]
                            sub_quads.append(corners)
                        else:
                            # Boundary or BSP-split cell: clip strictly to this subsector's polygon
                            # so every subsector covers 100% of its own area across LOD0, LOD1, and frustum/occlusion culling.
                            cp_sk = skirt_cell(cp, sub_skirt_segs[ss])
                            sub_quads.extend(orient_cell_quads(cp_sk, gx, gy))

        # Build subsector bounding box from poly, seg vertices, AND emitted flat quads
        xs_all = [int(round(float(p[0]))) for p in poly]
        ys_all = [int(round(float(p[1]))) for p in poly]
        n_sg, f_sg = SSECTORS[ss]
        for sg_i in range(f_sg, f_sg + n_sg):
            seg = SEGS[sg_i]
            v1 = VERTEXES[seg[0]]
            v2 = VERTEXES[seg[1]]
            xs_all.extend([v1[0], v2[0]])
            ys_all.extend([v1[1], v2[1]])
        for q in sub_quads:
            for p in q:
                xs_all.append(int(round(float(p[0]))))
                ys_all.append(int(round(float(p[1]))))
        if xs_all:
            sub_bbox.append((min(xs_all), max(xs_all), min(ys_all), max(ys_all)))
        else:
            sub_bbox.append((0, 0, 0, 0))

        if not sub_quads:
            sub_q_first.append(len(all_quads))
            sub_q_count.append(0)
            sub_v_first.append(len(all_verts))
            sub_v_count.append(0)
            lod1_sub_q_first.append(0)
            lod1_sub_q_count.append(0)
            lod1_sub_v_first.append(0)
            lod1_sub_v_count.append(0)
            continue

        vert_map = {}
        sub_verts = []
        sub_q_indices = []
        for q in sub_quads:
            q_idx = []
            for p in q:
                pt = (quant16(p[0]), quant16(p[1]))
                if pt not in vert_map:
                    vert_map[pt] = len(sub_verts)
                    sub_verts.append(pt)
                q_idx.append(vert_map[pt])
            if len(set(q_idx)) >= 3:
                p0 = sub_verts[q_idx[0]]
                p1 = sub_verts[q_idx[1]]
                p2 = sub_verts[q_idx[2]]
                p3 = sub_verts[q_idx[3]]
                area = (p0[0]*p1[1] - p1[0]*p0[1] +
                        p1[0]*p2[1] - p2[0]*p1[1] +
                        p2[0]*p3[1] - p3[0]*p2[1] +
                        p3[0]*p0[1] - p0[0]*p3[1])
                if area != 0:
                    sub_q_indices.append(q_idx)

        if not sub_q_indices:
            sub_q_first.append(len(all_quads))
            sub_q_count.append(0)
            sub_v_first.append(len(all_verts))
            sub_v_count.append(0)
            lod1_sub_q_first.append(0)
            lod1_sub_q_count.append(0)
            lod1_sub_v_first.append(0)
            lod1_sub_v_count.append(0)
            continue

        sub_q_first.append(len(all_quads))
        sub_q_count.append(len(sub_q_indices))
        sub_v_first.append(len(all_verts))
        sub_v_count.append(len(sub_verts))

        # Multi-level LOD:
        # Generate coarse 128x128 grid quads for LOD1 when distant (>= 384 units)
        lod1_verts = []
        lod1_q_indices = []
        if len(sub_q_indices) > 1 and poly and len(poly) >= 3:
            grid_l1 = 128
            l1_gx0 = int(min(xs) // grid_l1) * grid_l1
            l1_gx1 = int(max(xs) // grid_l1 + 1) * grid_l1
            l1_gy0 = int(min(ys) // grid_l1) * grid_l1
            l1_gy1 = int(max(ys) // grid_l1 + 1) * grid_l1
            raw_lod1 = []
            for lgx in range(l1_gx0, l1_gx1, grid_l1):
                for lgy in range(l1_gy0, l1_gy1, grid_l1):
                    lcp = clip_box(poly, Fraction(lgx), Fraction(lgx + grid_l1),
                                   Fraction(lgy), Fraction(lgy + grid_l1))
                    if lcp:
                        lcp = drop_collinear(lcp)
                        la = float(abs(poly_area2(lcp)) / 2)
                        if len(lcp) >= 3 and la > 0:
                            if la >= float(grid_l1 * grid_l1) - 0.5:
                                raw_lod1.append([(lgx, lgy), (lgx + grid_l1, lgy),
                                                 (lgx + grid_l1, lgy + grid_l1), (lgx, lgy + grid_l1)])
                            else:
                                lcp_sk = skirt_cell(lcp, sub_skirt_segs[ss])
                                raw_lod1.extend(orient_cell_quads(lcp_sk, lgx, lgy))
            lod1_vmap = {}
            for q in raw_lod1:
                q_idx = []
                for p in q:
                    pt = (quant16(p[0]), quant16(p[1]))
                    if pt not in lod1_vmap:
                        lod1_vmap[pt] = len(lod1_verts)
                        lod1_verts.append(pt)
                    q_idx.append(lod1_vmap[pt])
                if len(set(q_idx)) >= 3:
                    p0 = lod1_verts[q_idx[0]]
                    p1 = lod1_verts[q_idx[1]]
                    p2 = lod1_verts[q_idx[2]]
                    p3 = lod1_verts[q_idx[3]]
                    area = (p0[0]*p1[1] - p1[0]*p0[1] +
                            p1[0]*p2[1] - p2[0]*p1[1] +
                            p2[0]*p3[1] - p3[0]*p2[1] +
                            p3[0]*p0[1] - p0[0]*p3[1])
                    if area != 0:
                        lod1_q_indices.append(q_idx)

        # Only activate LOD1 if it offers genuine quad reduction
        if len(lod1_q_indices) > 0 and len(lod1_q_indices) < len(sub_q_indices):
            all_quads.extend(sub_q_indices)
            all_verts.extend(sub_verts)
            lod1_sub_q_first.append(len(all_quads))
            lod1_sub_q_count.append(len(lod1_q_indices))
            lod1_sub_v_first.append(len(all_verts))
            lod1_sub_v_count.append(len(lod1_verts))
            all_quads.extend(lod1_q_indices)
            all_verts.extend(lod1_verts)
        else:
            lod1_sub_q_first.append(0)
            lod1_sub_q_count.append(0)
            lod1_sub_v_first.append(0)
            lod1_sub_v_count.append(0)
            all_quads.extend(sub_q_indices)
            all_verts.extend(sub_verts)

    return {
        "name": map_name,
        "nsssec": nsssec,
        "sub_q_first": sub_q_first,
        "sub_q_count": sub_q_count,
        "sub_v_first": sub_v_first,
        "sub_v_count": sub_v_count,
        "lod1_sub_q_first": lod1_sub_q_first,
        "lod1_sub_q_count": lod1_sub_q_count,
        "lod1_sub_v_first": lod1_sub_v_first,
        "lod1_sub_v_count": lod1_sub_v_count,
        "sub_bbox": sub_bbox,
        "verts": all_verts,
        "quads": all_quads
    }

def write_flt_file(bm, out_path):
    nsssec = bm['nsssec']
    numquads = len(bm['quads'])
    numverts = len(bm['verts'])

    header_size = 64

    ofs_q_first = header_size
    q_first_bytes = struct.pack(f">{nsssec}I", *bm['sub_q_first'])

    ofs_q_count = ofs_q_first + len(q_first_bytes)
    q_count_bytes = struct.pack(f">{nsssec}H", *bm['sub_q_count'])
    pad_q_count = (4 - (len(q_count_bytes) % 4)) % 4
    q_count_bytes += b'\x00' * pad_q_count

    ofs_v_first = ofs_q_count + len(q_count_bytes)
    v_first_bytes = struct.pack(f">{nsssec}I", *bm['sub_v_first'])

    ofs_v_count = ofs_v_first + len(v_first_bytes)
    v_count_bytes = struct.pack(f">{nsssec}H", *bm['sub_v_count'])
    pad_v_count = (4 - (len(v_count_bytes) % 4)) % 4
    v_count_bytes += b'\x00' * pad_v_count

    ofs_bbox = ofs_v_count + len(v_count_bytes)
    bbox_bytes = bytearray()
    for b in bm['sub_bbox']:
        bbox_bytes += struct.pack(">4h", b[0], b[1], b[2], b[3])
    pad_bbox = (16 - ((ofs_bbox + len(bbox_bytes)) % 16)) % 16
    bbox_bytes += b'\x00' * pad_bbox

    # LOD 1 tables
    ofs_lod1_q_first = ofs_bbox + len(bbox_bytes)
    lod1_q_first_bytes = struct.pack(f">{nsssec}I", *bm['lod1_sub_q_first'])

    ofs_lod1_q_count = ofs_lod1_q_first + len(lod1_q_first_bytes)
    lod1_q_count_bytes = struct.pack(f">{nsssec}H", *bm['lod1_sub_q_count'])
    pad_l1_qc = (4 - (len(lod1_q_count_bytes) % 4)) % 4
    lod1_q_count_bytes += b'\x00' * pad_l1_qc

    ofs_lod1_v_first = ofs_lod1_q_count + len(lod1_q_count_bytes)
    lod1_v_first_bytes = struct.pack(f">{nsssec}I", *bm['lod1_sub_v_first'])

    ofs_lod1_v_count = ofs_lod1_v_first + len(lod1_v_first_bytes)
    lod1_v_count_bytes = struct.pack(f">{nsssec}H", *bm['lod1_sub_v_count'])
    pad_l1_vc = (4 - (len(lod1_v_count_bytes) % 4)) % 4
    lod1_v_count_bytes += b'\x00' * pad_l1_vc

    ofs_verts = ofs_lod1_v_count + len(lod1_v_count_bytes)
    verts_bytes = bytearray()
    frac_bytes = bytearray()
    for v in bm['verts']:
        # v = (X16, Y16) in 1/16 units: floor part -> int16, remainder -> frac byte
        verts_bytes += struct.pack(">2h", v[0] >> FRAC_BITS, v[1] >> FRAC_BITS)
        frac_bytes.append(((v[0] & (FRAC_ONE - 1)) << 4) | (v[1] & (FRAC_ONE - 1)))
    pad_verts = (4 - (len(verts_bytes) % 4)) % 4
    verts_bytes += b'\x00' * pad_verts

    ofs_quads = ofs_verts + len(verts_bytes)
    quads_bytes = bytearray()
    for q in bm['quads']:
        quads_bytes += struct.pack(">4H", q[0], q[1], q[2], q[3])

    # Seam fix: per-vertex fraction bytes appended after the quads.
    # Old loaders ignore it (they never read reserved[0]).
    ofs_vert_frac = ofs_quads + len(quads_bytes)
    frac_bytes += b'\x00' * ((4 - (len(frac_bytes) % 4)) % 4)

    header = struct.pack(
        ">4sHHIIIIIIIII5I",
        b"FLT2",
        2,          # version
        nsssec,     # numsubsectors
        numquads,   # numquads
        numverts,   # numvertexes
        ofs_q_first,
        ofs_q_count,
        ofs_v_first,
        ofs_v_count,
        ofs_bbox,
        ofs_verts,
        ofs_quads,
        ofs_vert_frac,
        ofs_lod1_q_first,
        ofs_lod1_q_count,
        ofs_lod1_v_first,
        ofs_lod1_v_count
    )

    data = (header + q_first_bytes + q_count_bytes + v_first_bytes + v_count_bytes +
            bbox_bytes + lod1_q_first_bytes + lod1_q_count_bytes +
            lod1_v_first_bytes + lod1_v_count_bytes +
            verts_bytes + quads_bytes + frac_bytes)
    with open(out_path, "wb") as f:
        f.write(data)
    lod1_active_subs = sum(1 for c in bm['lod1_sub_q_count'] if c > 0)
    print(f"Wrote {out_path}: {len(data)} bytes ({nsssec} subsectors, {numquads} quads, {numverts} unique verts, "
          f"{lod1_active_subs} subs with LOD1, {skirt_stats[0]}/{skirt_stats[1]} cells skirted)")

def main():
    wad_data, directory = load_wad(WAD_PATH)
    maps_to_bake = sys.argv[1:] if len(sys.argv) > 1 else [
        f"E{e}M{m}" for e in range(1, 5) for m in range(1, 10)
    ]

    script_dir = os.path.dirname(os.path.abspath(__file__))
    saturn_dir = os.path.abspath(os.path.join(script_dir, ".."))
    cd_dir = os.path.join(saturn_dir, "cd")
    stage_dir = os.path.join(saturn_dir, "staging_assets")

    os.makedirs(cd_dir, exist_ok=True)
    os.makedirs(stage_dir, exist_ok=True)
    win_cd_dir = os.path.abspath(os.path.join(saturn_dir, "..", "DOOM-Windows", "cd"))
    if os.path.exists(win_cd_dir):
        os.makedirs(win_cd_dir, exist_ok=True)

    for m in maps_to_bake:
        skirt_stats[0] = skirt_stats[1] = 0
        bm = bake_map_adaptive(wad_data, directory, m)
        if bm:
            out_file = os.path.join(cd_dir, f"{m}.FLT")
            write_flt_file(bm, out_file)
            out_stage = os.path.join(stage_dir, f"{m}.FLT")
            write_flt_file(bm, out_stage)
            if os.path.exists(win_cd_dir):
                out_win = os.path.join(win_cd_dir, f"{m}.FLT")
                write_flt_file(bm, out_win)

if __name__ == "__main__":
    main()
