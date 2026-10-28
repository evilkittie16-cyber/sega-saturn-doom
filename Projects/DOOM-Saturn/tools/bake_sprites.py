# tools/bake_sprites.py - Offline Rectangular Sprite Bitmap Generator for Sega Saturn
#
# Pre-converts column-major post-chain Doom sprites into row-major W*H rectangular
# bitmaps with 0 as transparent (and original palette-0 pixels remapped to 247).
# Eliminates runtime column post unpacking, patch decompression, and CD reads during gameplay.
#
# Generates:
#   cd/<MAP>.SPR for E1M1 through E4M9 (contains all monster, weapon, projectile, item & deco sprites)

import struct, os, sys

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
        name = data[off + 8:off + 16].split(b"\0")[0].decode("latin1")
        directory.append((name, fpos, size))
    return data, directory

def parse_and_convert_sprite(raw):
    if len(raw) < 8:
        return None
    w, h, lo, to = struct.unpack_from("<hhhh", raw, 0)
    if w < 1 or w > 1024 or h < 1 or h > 1024:
        return None
    if len(raw) < 8 + 4 * w:
        return None
    coloffsets = struct.unpack_from(f"<{w}i", raw, 8)

    dst = bytearray(w * h)
    for c in range(w):
        off = coloffsets[c]
        while off + 3 <= len(raw):
            top = raw[off]
            if top == 0xFF:
                break
            length = raw[off + 1]
            if length < 1 or off + 3 + length > len(raw):
                break
            for k in range(length):
                ty = top + k
                if 0 <= ty < h:
                    val = raw[off + 3 + k]
                    dst[ty * w + c] = 247 if val == 0 else val
            off += 4 + length

    return {
        "w": w, "h": h, "lo": lo, "to": to,
        "pixels": bytes(dst)
    }

def get_map_sprites(wad_data, directory, mapname, spr_lumps):
    map_idx = -1
    for i, (name, fpos, size) in enumerate(directory):
        if name == mapname:
            map_idx = i
            break
    if map_idx == -1:
        return []

    mlumps = {}
    for i in range(map_idx + 1, min(map_idx + 12, len(directory))):
        name, fpos, size = directory[i]
        mlumps[name] = wad_data[fpos:fpos + size]

    if "THINGS" not in mlumps:
        return []

    things = mlumps["THINGS"]
    nth = len(things) // 10

    # Doom Thing Type to sprite prefixes mapping (Episodes 1 - 4)
    thing_prefix = {
        # Players & Start Positions
        1: ['PLAY'], 2: ['PLAY'], 3: ['PLAY'], 4: ['PLAY'], 11: ['PLAY'],
        15: ['PLAY'],
        # Monsters
        3004: ['POSS'], 9: ['SPOS'], 3001: ['TROO', 'BAL1'], 3002: ['SARG'],
        58: ['SARG'], 3005: ['HEAD', 'BAL2'], 3003: ['BOSS', 'BRS1', 'BOSF'], 3006: ['SKUL'],
        16: ['CYBR', 'ROCK'], 7: ['SPID'],
        # Dead bodies & gore
        10: ['BLOD', 'POL2'], 12: ['POL5'], 24: ['POL4'], 25: ['POL1'], 26: ['POL6'],
        27: ['POL1'], 28: ['POL6'], 29: ['POL3'],
        49: ['HDB1'], 50: ['HDB2'], 51: ['HDB3'], 52: ['HDB4'], 53: ['HDB5'],
        59: ['HDB6'], 60: ['POB1'], 61: ['POB2'], 62: ['BRS1'], 63: ['SMBT'],
        73: ['HDB6'], 74: ['HDB6'],
        18: ['POSS'], 19: ['SPOS'], 20: ['TROO'], 21: ['SARG'], 22: ['HEAD'], 23: ['SKUL'],
        # Keys
        5: ['BKEY'], 13: ['RKEY'], 6: ['YKEY'], 40: ['BSKU'], 38: ['RSKU'], 39: ['YSKU'],
        # Powerups / Artifacts
        2013: ['SOUL'], 2022: ['PINV'], 2023: ['PINS'], 2024: ['SUIT'], 2025: ['PMAP'],
        2026: ['PVIS'], 2045: ['PSTR'], 83: ['MEGA'], 8: ['BPAK'],
        # Health & Armor
        2011: ['STIM'], 2012: ['MEDI'], 2014: ['BON1'], 2015: ['BON2'], 2018: ['ARM1'], 2019: ['ARM2'],
        # Weapons
        2001: ['SHOT'], 2002: ['MGUN'], 2003: ['LAUN'], 2004: ['PLAS'], 2005: ['CSAW'], 2006: ['BFUG'],
        # Ammo
        2007: ['CLIP'], 2008: ['AMMO'], 2010: ['ROCK'], 2046: ['BROK'], 2047: ['CELL'],
        2048: ['CELP'], 2049: ['SBOX'], 17: ['CELL'],
        # Props, Light Sources, Trees, Obstacles
        2035: ['BAR1', 'BEXP'], 2028: ['COLU'],
        85: ['TLMP'], 86: ['TLP2'], 34: ['CAND'], 35: ['CBRA'], 37: ['CBRA'],
        44: ['TBLU'], 45: ['TGRN'], 46: ['TRED'], 55: ['SMBT'], 56: ['SMGT'], 57: ['SMRT'],
        47: ['SMIT'], 43: ['TRE1'], 54: ['TRE2'], 48: ['ELEC'], 41: ['CEYE'], 42: ['FSKU'],
        30: ['COL1'], 31: ['COL2'], 32: ['COL3'], 33: ['COL4'], 36: ['COL5'],
        14: ['TFOG']
    }

    # Universal sprites needed on any map
    universal_prefixes = [
        'PLAY', 'PUNG', 'PISG', 'PISF', 'SHTG', 'SHTF', 'CHGG', 'CHGF', 'MISG', 'MISF',
        'SAWG', 'PLSG', 'PLSF', 'BFGG', 'BFGF', 'BLUD', 'PUFF', 'BAL1', 'BAL2', 'MISL',
        'PLSS', 'PLSE', 'BFS1', 'BFE1', 'BFE2', 'BAR1', 'BEXP', 'BON1', 'BON2', 'STIM',
        'MEDI', 'SOUL', 'ARM1', 'ARM2', 'CLIP', 'AMMO', 'SHEL', 'SBOX', 'ROCK', 'BROK',
        'CELL', 'CELP', 'TFOG', 'IFOG'
    ]

    prefixes = set(universal_prefixes)
    for t in range(nth):
        ttype = struct.unpack_from('<h', things, t * 10 + 6)[0]
        if ttype in thing_prefix:
            for p in thing_prefix[ttype]:
                prefixes.add(p)

    matched = []
    for s_idx, (name, fpos, size) in enumerate(spr_lumps):
        if any(name.startswith(p) for p in prefixes):
            matched.append((s_idx, name, fpos, size))

    return matched

def write_map_spr(wad_data, mapname, matched_sprites, out_path):
    # Header: "MSPR", ver=1 (u16), count (u16), total_size (u32), res (u32) = 16 bytes
    # Directory: count * 32 bytes:
    #   sprite_idx (u16), w (u16), h (u16), lo (i16), to (i16), pad (u16),
    #   offset (u32), size (u32), name (8s), pad (4s) = 32 bytes
    converted = []
    for s_idx, name, fpos, size in matched_sprites:
        c = parse_and_convert_sprite(wad_data[fpos:fpos + size])
        if c:
            converted.append((s_idx, name, c))

    n = len(converted)
    header_size = 16
    entry_size = 32
    dir_size = n * entry_size
    pixel_offset = header_size + dir_size

    total_pixels = sum(len(c["pixels"]) for s_idx, name, c in converted)
    total_file_size = pixel_offset + total_pixels

    buf = bytearray(total_file_size)
    struct.pack_into(">4sHHII", buf, 0, b"MSPR", 1, n, total_file_size, 0)

    cur_pix_off = pixel_offset
    for i, (s_idx, name, c) in enumerate(converted):
        name8 = name.encode("latin1").ljust(8, b"\0")[:8]
        pix_len = len(c["pixels"])
        struct.pack_into(">HHHhhHII8s4s", buf, header_size + i * entry_size,
                         s_idx, c["w"], c["h"], c["lo"], c["to"], 0,
                         cur_pix_off, pix_len, name8, b"\0\0\0\0")
        buf[cur_pix_off:cur_pix_off + pix_len] = c["pixels"]
        cur_pix_off += pix_len

    with open(out_path, "wb") as f:
        f.write(buf)

    print(f"Baked {out_path:12s} ({len(buf):7d} B): {n:3d} sprites ({total_pixels/1024:.1f} KB pixels)")

def main():
    wad_data, directory = load_wad(WAD_PATH)
    s_start = [i for i, (n,_,_) in enumerate(directory) if n == 'S_START'][0]
    s_end = [i for i, (n,_,_) in enumerate(directory) if n == 'S_END'][0]
    spr_lumps = directory[s_start + 1:s_end]
    print(f"Total sprites in IWAD: {len(spr_lumps)}")

    for e in range(1, 5):
        for m in range(1, 10):
            mapname = f"E{e}M{m}"
            matched = get_map_sprites(wad_data, directory, mapname, spr_lumps)
            if not matched:
                continue
            out_path = os.path.join("cd", f"{mapname}.SPR")
            write_map_spr(wad_data, mapname, matched, out_path)

if __name__ == "__main__":
    main()
