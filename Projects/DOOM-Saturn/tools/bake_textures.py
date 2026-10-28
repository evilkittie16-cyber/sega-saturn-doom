# tools/bake_textures.py - Offline Composite Wall Texture Generator for Sega Saturn
#
# Composites all wall textures from DOOM.WAD's TEXTURE1 & TEXTURE2 & PNAMES offline.
# Eliminates runtime composite_texture(), patch unpacking, column post-chain parsing,
# and CD-ROM reads during gameplay.
#
# Generates:
#   cd/TEXTURES.BIN   - All pre-composited textures (global master pack)
#   cd/<MAP>.TEX      - Per-map pre-composited texture packs for E1M1 through E4M9
#                       (sidedef textures + animated textures + switches + sky)

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

def parse_patch(patch_data):
    if len(patch_data) < 8:
        return None
    pw, ph, lo, to = struct.unpack_from("<hhhh", patch_data, 0)
    if pw < 1 or pw > 1024 or ph < 1 or ph > 1024:
        return None
    if len(patch_data) < 8 + 4 * pw:
        return None
    coloffsets = struct.unpack_from(f"<{pw}i", patch_data, 8)
    
    # Unpack columns into 2D dict/array
    cols = []
    for c in range(pw):
        off = coloffsets[c]
        posts = []
        while off + 3 <= len(patch_data):
            top = patch_data[off]
            if top == 0xFF:
                break
            length = patch_data[off + 1]
            if length < 1 or off + 3 + length > len(patch_data):
                break
            pixels = patch_data[off + 3:off + 3 + length]
            posts.append((top, length, pixels))
            off += 4 + length
        cols.append(posts)
    return {"w": pw, "h": ph, "lo": lo, "to": to, "cols": cols}

def bake_all_textures(wad_data, directory):
    lump_map = {}
    for name, fpos, size in directory:
        lump_map[name.upper()] = wad_data[fpos:fpos + size]

    pnames_data = lump_map["PNAMES"]
    num_pnames = struct.unpack_from("<i", pnames_data, 0)[0]
    pnames = []
    for i in range(num_pnames):
        name = pnames_data[4 + i * 8:4 + (i + 1) * 8].split(b"\0")[0].decode("latin1").upper()
        pnames.append(name)

    # Pre-parse all patches
    patches = {}
    for pidx, pname in enumerate(pnames):
        if pname in lump_map:
            patches[pidx] = parse_patch(lump_map[pname])
        else:
            patches[pidx] = None

    textures = []

    def parse_tex_lump(lump_name, base_idx):
        if lump_name not in lump_map:
            return 0
        tex_data = lump_map[lump_name]
        numtextures = struct.unpack_from("<i", tex_data, 0)[0]
        tex_offsets = struct.unpack_from(f"<{numtextures}i", tex_data, 4)

        for i, off in enumerate(tex_offsets):
            t_idx = base_idx + i
            name = tex_data[off:off + 8].split(b"\0")[0].decode("latin1")
            masked = struct.unpack_from("<i", tex_data, off + 8)[0]
            width, height = struct.unpack_from("<hh", tex_data, off + 12)
            pcount = struct.unpack_from("<h", tex_data, off + 20)[0]

            # Clamp dimensions
            if width < 1: width = 1
            if width > 512: width = 512
            if height < 1: height = 1
            if height > 512: height = 512

            # Create bitmap
            buf = bytearray(width * height)

            patch_base = off + 22
            # Vanilla DOOM parity (linuxdoom-1.10/r_data.c R_GenerateLookup & R_GetColumn):
            # Any column covered by only a single patch (col_patchcount[x] == 1) ignores
            # patch->originy (oy = 0). Multi-patch columns composite with patch->originy.
            col_patchcount = [0] * width
            for p in range(pcount):
                ox, oy, pindex, stepdir, colormap = struct.unpack_from("<hhhhh", tex_data, patch_base + p * 10)
                patch = patches.get(pindex)
                if not patch:
                    continue
                pw = patch["w"]
                x1 = max(0, ox)
                x2 = min(width, ox + pw)
                for x in range(x1, x2):
                    col_patchcount[x] += 1

            for p in range(pcount):
                ox, oy, pindex, stepdir, colormap = struct.unpack_from("<hhhhh", tex_data, patch_base + p * 10)
                patch = patches.get(pindex)
                if not patch:
                    continue

                pw, ph = patch["w"], patch["h"]
                for c in range(pw):
                    texcol = ox + c
                    if texcol < 0 or texcol >= width:
                        continue
                    oy_col = 0 if col_patchcount[texcol] == 1 else oy
                    for top, length, pixels in patch["cols"][c]:
                        for k in range(length):
                            ty = oy_col + top + k
                            if 0 <= ty < height:
                                buf[ty * width + texcol] = 247 if pixels[k] == 0 else pixels[k]

            holes = 1 if (0 in buf) else 0
            textures.append({
                "idx": t_idx,
                "name": name,
                "w": width,
                "h": height,
                "holes": holes,
                "pixels": bytes(buf)
            })
        return numtextures

    # TEXTURE1
    tex1_count = parse_tex_lump("TEXTURE1", 0)
    print(f"Parsed TEXTURE1: {tex1_count} textures")

    # TEXTURE2
    tex2_count = parse_tex_lump("TEXTURE2", tex1_count)
    print(f"Parsed TEXTURE2: {tex2_count} textures")

    return textures

def write_textures_bin(textures, out_path):
    n = len(textures)
    header_size = 16
    entry_size = 24
    dir_size = n * entry_size
    pixel_offset = header_size + dir_size

    total_pixels = sum(len(t["pixels"]) for t in textures)
    total_file_size = pixel_offset + total_pixels

    buf = bytearray(total_file_size)
    struct.pack_into(">4sHHII", buf, 0, b"TEX1", 1, n, total_file_size, 0)

    cur_pix_off = pixel_offset
    for i, t in enumerate(textures):
        name_bytes = t["name"].encode("latin1").ljust(8, b"\0")[:8]
        pix_len = len(t["pixels"])
        struct.pack_into(">8sHHBBI I", buf, header_size + i * entry_size,
                         name_bytes, t["w"], t["h"], t["holes"], 0, cur_pix_off, pix_len)
        buf[cur_pix_off:cur_pix_off + pix_len] = t["pixels"]
        cur_pix_off += pix_len

    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"Wrote {out_path} ({len(buf)} bytes, {n} textures)")

def write_map_tex(mapname, textures, wad_data, directory, out_path):
    map_idx = -1
    for i, (name, fpos, size) in enumerate(directory):
        if name == mapname:
            map_idx = i
            break
    if map_idx == -1:
        return

    mlumps = {}
    for i in range(map_idx + 1, min(map_idx + 12, len(directory))):
        name, fpos, size = directory[i]
        mlumps[name] = wad_data[fpos:fpos + size]

    if "SIDEDEFS" not in mlumps:
        return

    sides = mlumps["SIDEDEFS"]
    nsides = len(sides) // 30
    used_names = set()
    for s in range(nsides):
        for o in (4, 12, 20):
            t = sides[s*30+o:s*30+o+8].split(b"\0")[0].decode("latin1").upper()
            if t and t != "-":
                used_names.add(t)

    all_names = {t["name"].upper(): t for t in textures}

    # Switch twins: if a switch appears on a map, include its toggled twin
    twins = set()
    for name in used_names:
        if name.startswith("SW1"):
            twin = "SW2" + name[3:]
            if twin in all_names:
                twins.add(twin)
        elif name.startswith("SW2"):
            twin = "SW1" + name[3:]
            if twin in all_names:
                twins.add(twin)
    used_names.update(twins)

    # Sky textures
    ep = int(mapname[1]) if (len(mapname) >= 2 and mapname[1].isdigit()) else 1
    used_names.add(f"SKY{ep}")
    if ep == 4:
        used_names.add("SKY3")

    map_tex = [t for t in textures if t["name"].upper() in used_names]

    # Header (16 bytes): "MTEX", ver=1 (u16), count (u16), total_size (u32), reserved (u32)
    # Directory: count * 24 bytes:
    #   orig_texnum (u16), w (u16), h (u16), holes (u8), pad (u8), offset (u32), size (u32), name(8s)
    n = len(map_tex)
    header_size = 16
    entry_size = 24
    dir_size = n * entry_size
    pixel_offset = header_size + dir_size

    total_pixels = sum(len(t["pixels"]) for t in map_tex)
    total_file_size = pixel_offset + total_pixels

    buf = bytearray(total_file_size)
    struct.pack_into(">4sHHII", buf, 0, b"MTEX", 1, n, total_file_size, 0)

    cur_pix_off = pixel_offset
    for i, t in enumerate(map_tex):
        name8 = t["name"].encode("latin1").ljust(8, b"\0")[:8]
        pix_len = len(t["pixels"])
        struct.pack_into(">HHHBBII8s", buf, header_size + i * entry_size,
                         t["idx"], t["w"], t["h"], t["holes"], 0, cur_pix_off, pix_len, name8)
        buf[cur_pix_off:cur_pix_off + pix_len] = t["pixels"]
        cur_pix_off += pix_len

    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"Baked {out_path:12s} ({len(buf):7d} B): {n:2d} textures ({total_pixels/1024:.1f} KB pixels)")

def main():
    wad_data, directory = load_wad(WAD_PATH)
    print("Compositing all textures from IWAD...")
    textures = bake_all_textures(wad_data, directory)
    print(f"Composited {len(textures)} textures total.")

    # Write global textures pack
    write_textures_bin(textures, "cd/TEXTURES.BIN")

    # Write per-map texture packs for all 36 maps
    for e in range(1, 5):
        for m in range(1, 10):
            mapname = f"E{e}M{m}"
            out_path = os.path.join("cd", f"{mapname}.TEX")
            write_map_tex(mapname, textures, wad_data, directory, out_path)

if __name__ == "__main__":
    main()
