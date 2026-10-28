#!/usr/bin/env python3
# tools/bake_walls.py - Master Blueprint Zero-Runtime-Math Wall Baker for Sega Saturn
#
# Generates offline .WL files for DOOM levels on Sega Saturn.
# Implements:
#   1. Y-Axis Immunity (Law 1): Bypasses 2D grid tessellation for walls.
#   2. 1D Wall LOD (Law 2): Pre-bakes 5 pillar sample points [0, 16, 32, 48, 64] per seg
#      to drive runtime 1-cycle screen_width LOD (16/32/64 vertical strips).
#   3. Relocatable 16-bit Sector Indices (Trap 1): Bakes sector indices for load-time pointer swizzling.
#   4. Remainder Cropping & Stride Preservation (Trap 2): Identifies partial seg remainder widths.
#   5. Vertical Quad Tessellation for Tall Walls (Trap 3): Slices walls taller than texture height.
#   6. 1-Cycle Fake Contrast & Z-Light (Trap 4): Bakes directional lighting deltas.
#   7. Deduplicated 2D (X, Z) Floorplan Pillars: 50% SCU-DSP matrix math reduction.
#
# Output format: cd/<MAP>.WL (Big-endian, 16-byte aligned, 32-byte cache-aligned seg structs)

import os
import sys
import struct
import math

WAD_PATH = os.environ.get("DOOM_WAD", None)
if not WAD_PATH or not os.path.exists(WAD_PATH):
    for candidate in ["cd/DOOM.WAD", "cd/DOOM1.WAD", "DOOM.WAD", "DOOM1.WAD"]:
        if os.path.exists(candidate):
            WAD_PATH = candidate
            break
    else:
        WAD_PATH = "cd/DOOM.WAD"

# Wall Flags
SEG_FLAG_SOLID   = 0x01
SEG_FLAG_UPPER   = 0x02
SEG_FLAG_LOWER   = 0x04
SEG_FLAG_MID     = 0x08
SEG_PEG_BOTTOM   = 0x10  # Bottom-anchored: v_bot = th + rowoff, v_top = v_bot - wall_h
SEG_PEG_LOWER_UNPEGGED = 0x20 # Lower unpegged: v_top = (front.ceil - back.floor) + rowoff

def load_wad(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"WAD file not found: {path}")
    data = open(path, "rb").read()
    assert data[:4] == b"IWAD", "Not an IWAD"
    numlumps, infotableofs = struct.unpack_from("<ii", data, 4)
    directory = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        fpos, size = struct.unpack_from("<ii", data, off)
        name = data[off + 8 : off + 16].split(b"\0")[0].decode("latin1")
        directory.append((name, fpos, size))
    return data, directory

def load_texture_dictionary(wad_data, directory):
    """Builds a map of texture name -> (texture_index, width, height) from TEXTURE1/2."""
    lump_map = {name: (pos, sz) for name, pos, sz in directory}
    tex_dict = {"-": (0, 0, 0)}
    cur_idx = 0

    for lump_name in ["TEXTURE1", "TEXTURE2"]:
        if lump_name not in lump_map:
            continue
        pos, sz = lump_map[lump_name]
        tex_data = wad_data[pos : pos + sz]
        num_textures = struct.unpack_from("<i", tex_data, 0)[0]
        tex_offsets = struct.unpack_from(f"<{num_textures}i", tex_data, 4)
        for off in tex_offsets:
            name = tex_data[off : off + 8].split(b"\0")[0].decode("latin1").upper()
            w, h = struct.unpack_from("<hh", tex_data, off + 12)
            tex_dict[name] = (cur_idx, w, h)
            cur_idx += 1

    return tex_dict

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
        lumps[name] = wad_data[fpos : fpos + size]
    return lumps

def bake_map_walls(mapname, lumps, tex_dict):
    v_data = lumps["VERTEXES"]
    s_data = lumps["SEGS"]
    ss_data = lumps["SSECTORS"]
    l_data = lumps["LINEDEFS"]
    sd_data = lumps["SIDEDEFS"]
    sec_data = lumps["SECTORS"]

    # Parse Vertexes (x, y)
    n_v = len(v_data) // 4
    verts = [struct.unpack_from("<hh", v_data, i * 4) for i in range(n_v)]

    # Parse Subsectors (numsegs, firstseg)
    n_ss = len(ss_data) // 4
    ssects = [struct.unpack_from("<hh", ss_data, i * 4) for i in range(n_ss)]

    # Parse Segs (v1, v2, angle, linedef, side, offset)
    n_s = len(s_data) // 12
    segs = [struct.unpack_from("<hhhhhh", s_data, i * 12) for i in range(n_s)]

    # Parse Linedefs (v1, v2, flags, special, tag, sidenum[2])
    n_l = len(l_data) // 14
    linedefs = [struct.unpack_from("<hhhhhhh", l_data, i * 14) for i in range(n_l)]

    # Parse Sidedefs (textureoffset, rowoffset, top, bot, mid, sector)
    n_sd = len(sd_data) // 30
    sidedefs = []
    for i in range(n_sd):
        txoff, rowoff, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", sd_data, i * 30)
        sidedefs.append({
            "txoff": txoff,
            "rowoff": rowoff,
            "top": top.split(b"\0")[0].decode("latin1").upper(),
            "bot": bot.split(b"\0")[0].decode("latin1").upper(),
            "mid": mid.split(b"\0")[0].decode("latin1").upper(),
            "sec": sec
        })

    # Parse Sectors (floorheight, ceilingheight, floorpic, ceilingpic, lightlevel, special, tag)
    n_sec = len(sec_data) // 26
    sectors = []
    for i in range(n_sec):
        fh, ch, fpic, cpic, light, spec, tag = struct.unpack_from("<hh8s8shhh", sec_data, i * 26)
        sectors.append({
            "floorheight": fh,
            "ceilingheight": ch,
            "ceilingpic": cpic.split(b"\0")[0].decode("latin1").upper(),
            "lightlevel": light
        })

    # Build Baked Subsector Structures
    all_pillars = []
    all_baked_segs = []
    subsector_headers = []

    for ss_idx, (num_segs, first_seg) in enumerate(ssects):
        sub_pillar_map = {}
        sub_pillars = []

        def get_pillar_index(px, py):
            # Seam fix: keep pillars at 1/16-unit precision (texture-repeat
            # points on diagonal walls are fractional). Stored as int16 floor
            # part + one frac byte, see build_wl_blob.
            key = (int(round(px * 16)), int(round(py * 16)))
            if key in sub_pillar_map:
                return sub_pillar_map[key]
            p_idx = len(sub_pillars)
            sub_pillars.append(key)
            sub_pillar_map[key] = p_idx
            return p_idx

        seg_start_idx = len(all_baked_segs)

        for si in range(first_seg, first_seg + num_segs):
            s = segs[si]
            v1_idx, v2_idx = s[0], s[1]
            angle = s[2]
            line_idx = s[3]
            side_idx = s[4]
            seg_offset = s[5]

            p1 = verts[v1_idx]
            p2 = verts[v2_idx]
            dx = p2[0] - p1[0]
            dy = p2[1] - p1[1]
            length = math.hypot(dx, dy)
            if length < 0.5:
                continue

            ld = linedefs[line_idx]
            ld_flags = ld[2]
            sidenum = ld[5] if side_idx == 0 else ld[6]
            if sidenum < 0 or sidenum >= n_sd:
                continue
            sd = sidedefs[sidenum]
            front_sec_idx = sd["sec"]
            front_sec = sectors[front_sec_idx]

            other_sidenum = ld[6] if side_idx == 0 else ld[5]
            back_sec_idx = sidedefs[other_sidenum]["sec"] if other_sidenum >= 0 else None
            back_sec = sectors[back_sec_idx] if back_sec_idx is not None else None

            # Calculate Directional Fake Contrast (Exact PC Doom Parity):
            # In PC Doom (r_segs.c lines 646-649):
            # if (curline->v1->y == curline->v2->y) lightnum--; // horizontal: -1 light level (-16)
            # else if (curline->v1->x == curline->v2->x) lightnum++; // vertical: +1 light level (+16)
            # diagonal lines have NO contrast adjustment (0).
            if dy == 0:
                contrast = -16
            elif dx == 0:
                contrast = 16
            else:
                contrast = 0
            base_light = front_sec["lightlevel"]
            baked_light = max(0, min(255, base_light + contrast))

            # Determine surfaces to generate (SOLID, UPPER, LOWER, MID)
            surfaces = []
            if back_sec is None:
                # 1-Sided Line: SOLID Wall
                tex_name = sd["mid"]
                if tex_name != "-" and tex_name in tex_dict:
                    t_idx, tw, th = tex_dict[tex_name]
                    # Pegging: Lower Unpegged -> Bottom-anchored; Normal -> Top-anchored
                    is_lower_unpegged = bool(ld_flags & 0x0010)
                    peg_flag = SEG_PEG_BOTTOM if is_lower_unpegged else 0
                    peg_sec_idx = front_sec_idx
                    v_off = sd["rowoff"]
                    surfaces.append({
                        "flags": SEG_FLAG_SOLID | peg_flag,
                        "tex_idx": t_idx,
                        "tw": tw,
                        "th": th,
                        "v_off": v_off,
                        "peg_sec": peg_sec_idx,
                        "top_sec": front_sec_idx,
                        "bot_sec": front_sec_idx
                    })
            else:
                # 2-Sided Portal Line: UPPER and/or LOWER and/or MID
                # Upper Wall: bake if top texture is defined (even if ceilings are initially equal,
                # so closing doors and crushers render their upper walls dynamically).
                is_sky = front_sec["ceilingpic"].startswith("F_SKY") and back_sec["ceilingpic"].startswith("F_SKY")
                tex_name = sd["top"]
                if not is_sky and tex_name != "-" and tex_name in tex_dict:
                    t_idx, tw, th = tex_dict[tex_name]
                    # Upper Unpegged (0x0008) -> Top-anchored (0); Normal -> Bottom-anchored
                    is_upper_unpegged = bool(ld_flags & 0x0008)
                    peg_flag = 0 if is_upper_unpegged else SEG_PEG_BOTTOM
                    peg_sec_idx = front_sec_idx if is_upper_unpegged else back_sec_idx
                    v_off = sd["rowoff"]
                    surfaces.append({
                        "flags": SEG_FLAG_UPPER | peg_flag,
                        "tex_idx": t_idx,
                        "tw": tw,
                        "th": th,
                        "v_off": v_off,
                        "peg_sec": peg_sec_idx,
                        "top_sec": front_sec_idx,
                        "bot_sec": back_sec_idx
                    })

                # Lower Wall: bake if bottom texture is defined (even if floors are initially equal,
                # so lowering lifts and stairs render their lower walls dynamically).
                tex_name = sd["bot"]
                if tex_name != "-" and tex_name in tex_dict:
                    t_idx, tw, th = tex_dict[tex_name]
                    # Lower Unpegged (0x0010) -> Peg to Front Ceiling; Normal -> Top-anchored (0)
                    is_lower_unpegged = bool(ld_flags & 0x0010)
                    peg_flag = SEG_PEG_LOWER_UNPEGGED if is_lower_unpegged else 0
                    peg_sec_idx = front_sec_idx  # Front sector for front.ceilingheight
                    v_off = sd["rowoff"]
                    surfaces.append({
                        "flags": SEG_FLAG_LOWER | peg_flag,
                        "tex_idx": t_idx,
                        "tw": tw,
                        "th": th,
                        "v_off": v_off,
                        "peg_sec": peg_sec_idx,
                        "top_sec": back_sec_idx,
                        "bot_sec": front_sec_idx
                    })

                # Masked Mid Texture (Grate / Fence)
                if sd["mid"] != "-" and sd["mid"] in tex_dict:
                    t_idx, tw, th = tex_dict[sd["mid"]]
                    peg_flag = SEG_PEG_BOTTOM if (ld_flags & 0x0010) else 0
                    v_off = sd["rowoff"]
                    surfaces.append({
                        "flags": SEG_FLAG_MID | peg_flag,
                        "tex_idx": t_idx,
                        "tw": tw,
                        "th": th,
                        "v_off": v_off,
                        "peg_sec": front_sec_idx,
                        "top_sec": front_sec_idx,
                        "bot_sec": front_sec_idx
                    })

            if not surfaces:
                continue

            # Subdivide seg horizontally into chunks aligned to texture wrapping per-surface
            u_base = seg_offset + sd["txoff"]
            for surf in surfaces:
                stw = float(surf["tw"]) if surf["tw"] > 0 else 64.0
                curr_dist = 0.0

                while curr_dist < length - 1e-4:
                    u_c = (u_base + curr_dist) % stw
                    rem = stw - (u_c % stw)
                    if rem <= 1e-4:
                        rem = stw

                    chunk_len = min(length - curr_dist, rem)
                    if chunk_len <= 1e-4:
                        break

                    t_start = curr_dist / length
                    t_end = (curr_dist + chunk_len) / length

                    # Generate endpoints: p0 at t_start, p4 at t_end
                    # Indices 1, 2, 3 reuse p0 since only pillar_idx[0] and [4] are read by the renderer
                    px0 = p1[0] + dx * t_start
                    py0 = p1[1] + dy * t_start
                    px4 = p1[0] + dx * t_end
                    py4 = p1[1] + dy * t_end
                    p0_idx = get_pillar_index(px0, py0)
                    p4_idx = get_pillar_index(px4, py4)
                    chunk_pillars = [p0_idx, p0_idx, p0_idx, p0_idx, p4_idx]

                    u_span = int(round(chunk_len))
                    if u_span <= 0:
                        u_span = 1

                    tex_id = surf["tex_idx"]
                    s_u_start = int(round(u_c))
                    all_baked_segs.append({
                        "pillar_idx": chunk_pillars,
                        "baked_light": baked_light,
                        "flags": surf["flags"],
                        "tex_64": tex_id,
                        "tex_32": tex_id,
                        "tex_16": tex_id,
                        "v_off": surf["v_off"],
                        "peg_sec": surf["peg_sec"],
                        "top_sec": surf["top_sec"],
                        "bot_sec": surf["bot_sec"],
                        "u_start": s_u_start,
                        "u_span": u_span
                    })

                    curr_dist += chunk_len

        pillar_start_idx = len(all_pillars)
        all_pillars.extend(sub_pillars)

        np = len(sub_pillars)
        num_batches = (np + 31) // 32 if np > 0 else 0
        max_batch = min(32, np)

        subsector_headers.append({
            "num_pillars": np,
            "pillar_start": pillar_start_idx,
            "num_segs": len(all_baked_segs) - seg_start_idx,
            "seg_start": seg_start_idx,
            "num_dsp_batches": num_batches,
            "max_batch_size": max_batch
        })

    return subsector_headers, all_pillars, all_baked_segs

def build_wl_blob(subsector_headers, all_pillars, all_baked_segs):
    # Header: 32 bytes (Big-endian)
    # 0..3: "MWAL"
    # 4..5: u16 version = 1
    # 6..7: u16 num_subsectors
    # 8..11: u32 total_size
    # 12..15: u32 ofs_subsectors
    # 16..19: u32 ofs_pillars
    # 20..23: u32 ofs_segs
    # 24..27: u32 ofs_pillar_frac (seam fix; 0 in older files)
    # 28..31: u32 reserved = 0
    hdr_size = 32

    ofs_subsectors = hdr_size
    subsectors_bytes = len(subsector_headers) * 16 # 16 bytes per subsector header

    ofs_pillars = (ofs_subsectors + subsectors_bytes + 15) & ~15
    pillars_bytes = len(all_pillars) * 4 # 2 int16 (x, z) = 4 bytes per pillar

    # Seam fix: 1 frac byte per pillar ((fx << 4) | fz). Must sit BEFORE the
    # segs: the loader derives num_segs from total_size - ofs_segs.
    ofs_pillar_frac = ofs_pillars + pillars_bytes

    ofs_segs = (ofs_pillar_frac + len(all_pillars) + 15) & ~15
    segs_bytes = len(all_baked_segs) * 32 # Exactly 32 bytes per packed_seg_disc_t

    total_size = (ofs_segs + segs_bytes + 15) & ~15

    buf = bytearray(total_size)
    struct.pack_into(">4sHHIIIIII", buf, 0,
                     b"MWAL", 1, len(subsector_headers), total_size,
                     ofs_subsectors, ofs_pillars, ofs_segs, ofs_pillar_frac, 0)

    # Pack Subsector Headers (16 bytes each: saturn_wl_subsector_t)
    for i, sh in enumerate(subsector_headers):
        struct.pack_into(">HHHHHHI", buf, ofs_subsectors + i * 16,
                         sh["num_pillars"], sh["pillar_start"],
                         sh["num_segs"], sh["seg_start"],
                         sh["num_dsp_batches"], sh["max_batch_size"],
                         0)

    # Pack 2D Pillars (4 bytes each: short x, short z) + frac bytes.
    # p = (X16, Z16) in 1/16 units.
    for i, p in enumerate(all_pillars):
        struct.pack_into(">hh", buf, ofs_pillars + i * 4, p[0] >> 4, p[1] >> 4)
        buf[ofs_pillar_frac + i] = ((p[0] & 15) << 4) | (p[1] & 15)

    # Pack Segs (32 bytes each: packed_seg_disc_t)
    # Format: >HHHHHBBHHHhHHHHHH (17 items, 32 bytes)
    #   5x H  : pillar_idx[0..4] (10 bytes)
    #   2x B  : baked_light, flags (2 bytes)
    #   3x H  : tex_64, tex_32, tex_16 (6 bytes)
    #   1x h  : mid_static_v_offset (signed 2 bytes)
    #   3x H  : peg_sec, top_sec, bot_sec (6 bytes)
    #   3x H  : reserved[3] padding (6 bytes)
    # Total = 32 bytes
    for i, s in enumerate(all_baked_segs):
        p0, p1, p2, p3, p4 = s["pillar_idx"]
        struct.pack_into(">HHHHHBBHHHhHHHHHH", buf, ofs_segs + i * 32,
                         p0, p1, p2, p3, p4,
                         s["baked_light"], s["flags"],
                         s["tex_64"], s["tex_32"], s["tex_16"],
                         s["v_off"],
                         s["peg_sec"], s["top_sec"], s["bot_sec"],
                         s["u_start"], s["u_span"], 0)

    return buf

def main():
    print("=== Sega Saturn DOOM Master Blueprint Wall Baker (bake_walls.py) ===")
    wad_data, directory = load_wad(WAD_PATH)
    tex_dict = load_texture_dictionary(wad_data, directory)
    print(f"Loaded {len(tex_dict)} textures from WAD")

    os.makedirs("cd", exist_ok=True)
    maps = [f"E{e}M{m}" for e in range(1, 5) for m in range(1, 10)]

    total_maps_baked = 0
    for m in maps:
        lumps = get_map_lumps(wad_data, directory, m)
        if not lumps:
            continue

        headers, pillars, baked_segs = bake_map_walls(m, lumps, tex_dict)
        blob = build_wl_blob(headers, pillars, baked_segs)

        out_path = os.path.join("cd", f"{m}.WL")
        with open(out_path, "wb") as f:
            f.write(blob)

        total_maps_baked += 1
        orig_segs = len(lumps["SEGS"]) // 12
        total_dsp_batches = sum(sh["num_dsp_batches"] for sh in headers)
        max_pil = max(sh["num_pillars"] for sh in headers) if headers else 0
        print(f"Baked {out_path:11s}: {len(headers):3d} ssecs, {len(pillars):5d} pillars (max {max_pil:3d}/ss, {total_dsp_batches} DSP batches <=32), {orig_segs:4d} -> {len(baked_segs):4d} baked segs ({len(blob)/1024:.1f} KB)")

    print(f"Successfully baked {total_maps_baked} .WL maps to cd/")

if __name__ == "__main__":
    main()
