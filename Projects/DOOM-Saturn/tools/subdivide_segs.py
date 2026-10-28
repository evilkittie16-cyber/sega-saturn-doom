# tools/subdivide_segs.py - Offline Wall Seg Subdivision for Sega Saturn VDP1
#
# Reads DOOM.WAD and generates pre-subdivided wall segs (<= 64 map units)
# for maps E1M1 through E1M9 to eliminate runtime affine texture swimming
# and avoid expensive runtime subdivision on the Master SH-2.
#
# Saturn Hardware Constraints:
# 1. VDP1 affine texture mapping: Segs > 64 units exhibit affine distortion.
# 2. Pure 4-vertex quads: Subdivided segs become direct 1-shot quad emissions.
# 3. Output binary .SEG files to cd/<MAP>.SEG for WRAM-L loading.

import struct, sys, os, math

WAD_PATH = os.environ.get("DOOM_WAD", None)
if not WAD_PATH or not os.path.exists(WAD_PATH):
    for candidate in ["cd/DOOM.WAD", "cd/DOOM1.WAD", "DOOM.WAD", "DOOM1.WAD"]:
        if os.path.exists(candidate):
            WAD_PATH = candidate
            break
    else:
        WAD_PATH = "cd/DOOM.WAD"

def load_wad(path):
    data = open(path, "rb").read()
    assert data[:4] == b"IWAD", "Not an IWAD"
    numlumps, infotableofs = struct.unpack_from("<ii", data, 4)
    directory = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        fpos, size = struct.unpack_from("<ii", data, off)
        name = data[off + 8:off + 16].rstrip(b"\0").decode("latin1")
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

def subdivide_map_segs(mapname, lumps, max_len=64.0):
    v_data = lumps["VERTEXES"]
    s_data = lumps["SEGS"]
    ss_data = lumps["SSECTORS"]

    # Original vertices: list of (x, y)
    n_v = len(v_data) // 4
    verts = [list(struct.unpack_from("<hh", v_data, i * 4)) for i in range(n_v)]

    # Original subsectors: (numsegs, firstseg)
    n_ss = len(ss_data) // 4
    ssect_orig = [struct.unpack_from("<hh", ss_data, i * 4) for i in range(n_ss)]

    # Original segs: (v1, v2, angle, linedef, side, offset)
    n_s = len(s_data) // 12
    segs_orig = [struct.unpack_from("<hhhhhh", s_data, i * 12) for i in range(n_s)]

    # Vertex map to avoid duplicate split vertices: (x, y) -> index
    vert_map = {(v[0], v[1]): i for i, v in enumerate(verts)}

    def get_or_add_vert(x, y):
        rx, ry = int(round(x)), int(round(y))
        if (rx, ry) in vert_map:
            return vert_map[(rx, ry)]
        idx = len(verts)
        verts.append([rx, ry])
        vert_map[(rx, ry)] = idx
        return idx

    new_segs = []
    new_ssect_first = []
    new_ssect_count = []

    for ss_idx in range(n_ss):
        count, first = ssect_orig[ss_idx]
        cur_first_line = len(new_segs)
        new_ssect_first.append(cur_first_line)

        for si in range(first, first + count):
            s = segs_orig[si]
            v1_idx, v2_idx = s[0], s[1]
            angle = s[2]
            linedef = s[3]
            side = s[4]
            offset = s[5]

            p1 = verts[v1_idx]
            p2 = verts[v2_idx]
            dx = p2[0] - p1[0]
            dy = p2[1] - p1[1]
            length = math.hypot(dx, dy)

            steps = max(1, int(math.ceil(length / max_len)))

            # If no split needed
            if steps == 1:
                # Store seg: linedef (with side packed), v1, v2
                # In Jaguar/32X/Saturn DOOM: linedef is packed: (linedef << 1) | side
                packed_ld = (linedef << 1) | (1 if side else 0)
                new_segs.append((packed_ld, v1_idx, v2_idx))
            else:
                prev_v = v1_idx
                for step in range(1, steps + 1):
                    if step == steps:
                        next_v = v2_idx
                    else:
                        t = step / float(steps)
                        sx = p1[0] + dx * t
                        sy = p1[1] + dy * t
                        next_v = get_or_add_vert(sx, sy)

                    packed_ld = (linedef << 1) | (1 if side else 0)
                    new_segs.append((packed_ld, prev_v, next_v))
                    prev_v = next_v

        new_ssect_count.append(len(new_segs) - cur_first_line)

    # End guard subsector firstline
    new_ssect_first.append(len(new_segs))

    return verts, new_segs, new_ssect_first, new_ssect_count

def build_seg_blob(verts, segs, ssect_first):
    # Header: 32 bytes (all big-endian)
    # 0..3: "SEGS"
    # 4..5: u16 version = 1
    # 6..7: u16 numvertexes
    # 8..11: u32 numsegs
    # 12..15: u32 numsubsectors
    # 16..19: u32 ofs_vertexes
    # 20..23: u32 ofs_segs
    # 24..27: u32 ofs_ssect_first
    # 28..31: u32 reserved = 0
    header_size = 32
    ofs_verts = header_size
    verts_bytes = len(verts) * 4 # 2 shorts (x, y) big-endian

    ofs_segs = ofs_verts + verts_bytes
    # Pad ofs_segs to 16-byte alignment
    ofs_segs = (ofs_segs + 15) & ~15
    segs_bytes = len(segs) * 6 # 3 shorts (linedef, v1, v2) big-endian

    ofs_ssect = ofs_segs + segs_bytes
    # Pad ofs_ssect to 16-byte alignment
    ofs_ssect = (ofs_ssect + 15) & ~15
    ssect_bytes = len(ssect_first) * 2 # uint16 big-endian

    total_size = ofs_ssect + ssect_bytes

    buf = bytearray(total_size)
    struct.pack_into(">4sHHIIIIII", buf, 0,
                     b"SEGS", 1, len(verts), len(segs), len(ssect_first) - 1,
                     ofs_verts, ofs_segs, ofs_ssect, 0)

    # Pack vertices (short x, short y) in big-endian
    for i, v in enumerate(verts):
        struct.pack_into(">hh", buf, ofs_verts + i * 4, v[0], v[1])

    # Pack segs (short linedef, short v1, short v2) in big-endian
    for i, s in enumerate(segs):
        struct.pack_into(">hhh", buf, ofs_segs + i * 6, s[0], s[1], s[2])

    # Pack ssect_first (uint16) in big-endian
    for i, sf in enumerate(ssect_first):
        struct.pack_into(">H", buf, ofs_ssect + i * 2, sf)

    return buf

def main():
    wad_data, directory = load_wad(WAD_PATH)
    maps = [f"E{e}M{m}" for e in range(1, 5) for m in range(1, 10)]

    for m in maps:
        lumps = get_map_lumps(wad_data, directory, m)
        if not lumps:
            print(f"Skipping {m}: not found")
            continue

        verts, segs, ssect_first, ssect_count = subdivide_map_segs(m, lumps, max_len=64.0)
        blob = build_seg_blob(verts, segs, ssect_first)

        out_path = os.path.join("cd", f"{m}.SEG")
        with open(out_path, "wb") as f:
            f.write(blob)

        orig_segs = len(lumps["SEGS"]) // 12
        orig_verts = len(lumps["VERTEXES"]) // 4
        print(f"Baked {out_path:12s}: verts {orig_verts}->{len(verts)}, segs {orig_segs}->{len(segs)}, size {len(blob):d} B")

if __name__ == "__main__":
    main()
