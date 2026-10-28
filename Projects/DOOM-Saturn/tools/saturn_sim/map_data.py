import struct
import os
import numpy as np

class MapData:
    def __init__(self, wad_path="cd/DOOM.WAD", map_name="E1M1", flt_dir="cd", wl_dir="cd"):
        self.wad_path = wad_path
        self.map_name = map_name.upper()
        self.flt_dir = flt_dir
        self.wl_dir = wl_dir
        
        self.load_wad()
        self.load_flt()
        self.load_wl()
        
    def load_wad(self):
        with open(self.wad_path, "rb") as f:
            self.wad_bytes = f.read()
            
        header = self.wad_bytes[:4]
        assert header in (b"IWAD", b"PWAD"), f"Invalid WAD header: {header}"
        num_lumps, dir_ofs = struct.unpack_from("<II", self.wad_bytes, 4)
        
        self.lump_map = {}
        self.directory = []
        for i in range(num_lumps):
            ofs, size = struct.unpack_from("<II", self.wad_bytes, dir_ofs + i * 16)
            name = self.wad_bytes[dir_ofs + i * 16 + 8 : dir_ofs + i * 16 + 16].split(b"\0")[0].decode("latin1")
            self.directory.append((name, ofs, size))
            self.lump_map[name] = (ofs, size)
            
        # Load palette (fallback to cd/DOOM.WAD for PWADs)
        if "PLAYPAL" in self.lump_map:
            pal_ofs, _ = self.lump_map["PLAYPAL"]
            pal_raw = self.wad_bytes[pal_ofs : pal_ofs + 768]
            self.palette = np.frombuffer(pal_raw, dtype=np.uint8).reshape((256, 3))
        elif os.path.exists("cd/DOOM.WAD"):
            with open("cd/DOOM.WAD", "rb") as f_base:
                base_wad = f_base.read()
                b_num, b_dir = struct.unpack_from("<II", base_wad, 4)
                for k in range(b_num):
                    b_ofs, b_sz = struct.unpack_from("<II", base_wad, b_dir + k * 16)
                    b_nm = base_wad[b_dir + k * 16 + 8 : b_dir + k * 16 + 16].split(b"\0")[0].decode("latin1")
                    if b_nm == "PLAYPAL":
                        self.palette = np.frombuffer(base_wad[b_ofs : b_ofs + 768], dtype=np.uint8).reshape((256, 3))
                        break
        # Load colormap (fallback to cd/DOOM.WAD)
        if "COLORMAP" in self.lump_map:
            c_ofs, c_sz = self.lump_map["COLORMAP"]
            self.colormap = np.frombuffer(self.wad_bytes[c_ofs : c_ofs + min(8704, c_sz)], dtype=np.uint8).reshape((-1, 256))
        elif os.path.exists("cd/DOOM.WAD"):
            with open("cd/DOOM.WAD", "rb") as f_base:
                base_wad = f_base.read()
                b_num, b_dir = struct.unpack_from("<II", base_wad, 4)
                for k in range(b_num):
                    b_ofs, b_sz = struct.unpack_from("<II", base_wad, b_dir + k * 16)
                    b_nm = base_wad[b_dir + k * 16 + 8 : b_dir + k * 16 + 16].split(b"\0")[0].decode("latin1")
                    if b_nm == "COLORMAP":
                        self.colormap = np.frombuffer(base_wad[b_ofs : b_ofs + min(8704, b_sz)], dtype=np.uint8).reshape((-1, 256))
                        break
        else:
            self.colormap = np.tile(np.arange(256, dtype=np.uint8), (34, 1))

        # Precompute 34 colormap palettes (34, 256, 3)
        num_maps = min(34, len(self.colormap))
        self.colormap_palettes = np.zeros((num_maps, 256, 3), dtype=np.uint8)
        for lvl in range(num_maps):
            self.colormap_palettes[lvl] = self.palette[self.colormap[lvl]]

        # Load Flats
        self.flats = {}
        self.flats_idx = {}
        in_flats = False
        for name, ofs, size in self.directory:
            if name in ("F_START", "F1_START", "F2_START", "F3_START", "FF_START"):
                in_flats = True
                continue
            if name in ("F_END", "FF_END"):
                in_flats = False
                continue
            if in_flats and size == 4096:
                raw_idx = np.frombuffer(self.wad_bytes[ofs : ofs + 4096], dtype=np.uint8).reshape((64, 64))
                self.flats_idx[name] = raw_idx
                self.flats[name] = self.palette[raw_idx]
                
        # Load wall textures
        self.textures = {}
        self.textures_idx = {}
        self._shaded_cache = {}
        self.load_wall_textures()
        
        # Load Map Geometry
        try:
            map_idx = next(i for i, (name, _, _) in enumerate(self.directory) if name == self.map_name)
        except StopIteration:
            raise ValueError(f"Map {self.map_name} not found in {self.wad_path}")
            
        map_lumps = {self.directory[map_idx + k][0]: (self.directory[map_idx + k][1], self.directory[map_idx + k][2])
                     for k in range(1, 11)}
                     
        # Vertices
        v_ofs, v_len = map_lumps["VERTEXES"]
        nv = v_len // 4
        self.verts = [struct.unpack_from("<hh", self.wad_bytes, v_ofs + i * 4) for i in range(nv)]
        
        # Sectors
        sec_ofs, sec_len = map_lumps["SECTORS"]
        nsec = sec_len // 26
        self.sectors = []
        for i in range(nsec):
            fh, ch, fp, cp, light, spec, tag = struct.unpack_from("<hh8s8sHHH", self.wad_bytes, sec_ofs + i * 26)
            self.sectors.append({
                "floorheight": fh,
                "ceilingheight": ch,
                "floorpic": fp.split(b"\0")[0].decode("latin1"),
                "ceilingpic": cp.split(b"\0")[0].decode("latin1"),
                "lightlevel": light,
                "special": spec,
                "tag": tag
            })
            
        # Sidedefs
        sd_ofs, sd_len = map_lumps["SIDEDEFS"]
        nsd = sd_len // 30
        self.sidedefs = []
        for i in range(nsd):
            xoff, yoff, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sH", self.wad_bytes, sd_ofs + i * 30)
            self.sidedefs.append({
                "xoffset": xoff,
                "yoffset": yoff,
                "top": top.split(b"\0")[0].decode("latin1"),
                "bot": bot.split(b"\0")[0].decode("latin1"),
                "mid": mid.split(b"\0")[0].decode("latin1"),
                "sector": sec
            })
            
        # Linedefs
        ld_ofs, ld_len = map_lumps["LINEDEFS"]
        nld = ld_len // 14
        self.linedefs = []
        for i in range(nld):
            v1, v2, flags, spec, tag, s0, s1 = struct.unpack_from("<hhhhhhh", self.wad_bytes, ld_ofs + i * 14)
            self.linedefs.append({
                "v1": v1, "v2": v2, "flags": flags, "special": spec, "tag": tag,
                "s0": s0, "s1": s1
            })
            
        # Segs
        seg_ofs, seg_len = map_lumps["SEGS"]
        nseg = seg_len // 12
        self.segs = []
        for i in range(nseg):
            v1, v2, angle, line, side, offset = struct.unpack_from("<hhhhhh", self.wad_bytes, seg_ofs + i * 12)
            self.segs.append({
                "v1": v1, "v2": v2, "angle": angle, "line": line, "side": side, "offset": offset
            })
            
        # Subsectors
        ss_ofs, ss_len = map_lumps["SSECTORS"]
        nss = ss_len // 4
        self.subsectors = []
        for i in range(nss):
            count, first = struct.unpack_from("<hh", self.wad_bytes, ss_ofs + i * 4)
            first_seg = self.segs[first]
            line = self.linedefs[first_seg["line"]]
            sd_idx = line["s0"] if first_seg["side"] == 0 else line["s1"]
            sec_idx = self.sidedefs[sd_idx]["sector"]
            self.subsectors.append({
                "numlines": count,
                "firstline": first,
                "sector": sec_idx
            })
            
        # Nodes
        nd_ofs, nd_len = map_lumps["NODES"]
        nnd = nd_len // 28
        self.nodes = []
        for i in range(nnd):
            x, y, dx, dy, y1a, y1b, x1a, x1b, y2a, y2b, x2a, x2b, c0, c1 = struct.unpack_from(
                "<hhhhhhhhhhhhHH", self.wad_bytes, nd_ofs + i * 28
            )
            self.nodes.append({
                "x": x, "y": y, "dx": dx, "dy": dy,
                "bbox": [(y1a, y1b, x1a, x1b), (y2a, y2b, x2a, x2b)],
                "children": (c0, c1)
            })

        # Things
        self.player_spawn = (0.0, 0.0, 41.0, 0.0)
        if "THINGS" in map_lumps:
            th_ofs, th_len = map_lumps["THINGS"]
            nth = th_len // 10
            for i in range(nth):
                tx, ty, tang, ttype, tflags = struct.unpack_from("<hhhhh", self.wad_bytes, th_ofs + i * 10)
                if ttype == 1: # Player 1 start
                    spawn_floor = 0.0
                    if self.nodes and self.subsectors and self.sectors:
                        nodenum = len(self.nodes) - 1
                        while not (nodenum & 0x8000):
                            n = self.nodes[nodenum]
                            dx = tx - n["x"]
                            dy = ty - n["y"]
                            side = 0 if (dy * n["dx"] < n["dy"] * dx) else 1
                            nodenum = n["children"][side]
                        ss = nodenum & ~0x8000
                        if ss < len(self.subsectors):
                            sec_idx = self.subsectors[ss]["sector"]
                            if sec_idx < len(self.sectors):
                                spawn_floor = float(self.sectors[sec_idx]["floorheight"])
                    self.player_spawn = (float(tx), float(ty), spawn_floor + 41.0, float(tang))
                    break

    def load_wall_textures(self):
        if "PNAMES" not in self.lump_map:
            return
        pnames_ofs, _ = self.lump_map["PNAMES"]
        num_pnames = struct.unpack_from("<i", self.wad_bytes, pnames_ofs)[0]
        pnames = [self.wad_bytes[pnames_ofs + 4 + i * 8 : pnames_ofs + 4 + (i + 1) * 8].split(b"\0")[0].decode("latin1")
                  for i in range(num_pnames)]
                  
        patches = {}
        for pidx, pname in enumerate(pnames):
            if pname in self.lump_map:
                ofs, size = self.lump_map[pname]
                pdata = self.wad_bytes[ofs : ofs + size]
                if len(pdata) >= 8:
                    pw, ph, lo, to = struct.unpack_from("<hhhh", pdata, 0)
                    coloffsets = struct.unpack_from(f"<{pw}i", pdata, 8)
                    cols = []
                    for c in range(pw):
                        off = coloffsets[c]
                        posts = []
                        while off + 3 <= len(pdata):
                            top = pdata[off]
                            if top == 0xFF: break
                            length = pdata[off + 1]
                            if length < 1 or off + 3 + length > len(pdata): break
                            posts.append((top, length, pdata[off + 3 : off + 3 + length]))
                            off += 4 + length
                        cols.append(posts)
                    patches[pidx] = {"w": pw, "h": ph, "cols": cols}
                    
        self.tex_by_idx = {0: ("-", 0, 0)}
        self.tex_name_to_idx = {"-": 0}
        cur_tex_idx = 1
        for t_lump in ("TEXTURE1", "TEXTURE2"):
            if t_lump not in self.lump_map:
                continue
            t_ofs, _ = self.lump_map[t_lump]
            num_t = struct.unpack_from("<i", self.wad_bytes, t_ofs)[0]
            offsets = struct.unpack_from(f"<{num_t}i", self.wad_bytes, t_ofs + 4)
            for off in offsets:
                base = t_ofs + off
                name = self.wad_bytes[base : base + 8].split(b"\0")[0].decode("latin1").upper()
                w, h = struct.unpack_from("<hh", self.wad_bytes, base + 12)
                self.tex_by_idx[cur_tex_idx] = (name, w, h)
                self.tex_name_to_idx[name] = cur_tex_idx
                cur_tex_idx += 1
                pcount = struct.unpack_from("<h", self.wad_bytes, base + 20)[0]
                
                buf = np.zeros((h, w), dtype=np.uint8)
                pbase = base + 22
                for p in range(pcount):
                    ox, oy, pidx = struct.unpack_from("<hhh", self.wad_bytes, pbase + p * 10)
                    patch = patches.get(pidx)
                    if not patch: continue
                    pw, ph = patch["w"], patch["h"]
                    for c in range(pw):
                        tx = ox + c
                        if 0 <= tx < w:
                            for top, length, pixels in patch["cols"][c]:
                                for k in range(length):
                                    ty = oy + top + k
                                    if 0 <= ty < h:
                                        buf[ty, tx] = pixels[k]
                self.textures_idx[name] = buf
                self.textures[name] = self.palette[buf]

    def load_flt(self):
        flt_path = os.path.join(self.flt_dir, f"{self.map_name}.FLT")
        if not os.path.exists(flt_path):
            try:
                from tools.bake_flats import bake_map_adaptive
                bm = bake_map_adaptive(self.wad_bytes, self.directory, self.map_name)
                if bm:
                    self.baked_flats = []
                    for ss in range(bm["nsssec"]):
                        first = bm["sub_first"][ss]
                        count = bm["sub_count"][ss]
                        self.baked_flats.append(bm["quads"][first : first + count])
                    return
            except Exception as e:
                print(f"On-the-fly flat baking failed: {e}")
            self.baked_flats = None
            return
            
        with open(flt_path, "rb") as f:
            data = f.read()
            
        magic = data[:4]
        if magic == b"FLT2":
            hdr = struct.unpack_from(">4sHHIIIIIIIII", data, 0)
            magic, ver, nsssec, numquads, numverts, ofs_q_first, ofs_q_count, ofs_v_first, ofs_v_count, ofs_bbox, ofs_verts, ofs_quads = hdr
            # Seam fix: reserved[0] (byte 44) = ofs_vert_frac, 1 byte/vertex (fx << 4) | fy in 1/16 units
            ofs_vert_frac = struct.unpack_from(">I", data, 44)[0]
            has_frac = 64 <= ofs_vert_frac and ofs_vert_frac + numverts <= len(data)
            def vert(vi):
                x, y = struct.unpack_from(">2h", data, ofs_verts + vi * 4)
                if has_frac:
                    fr = data[ofs_vert_frac + vi]
                    return (x + (fr >> 4) / 16.0, y + (fr & 15) / 16.0)
                return (x, y)
            sub_q_first = struct.unpack_from(f">{nsssec}I", data, ofs_q_first)
            sub_q_count = struct.unpack_from(f">{nsssec}H", data, ofs_q_count)
            sub_v_first = struct.unpack_from(f">{nsssec}I", data, ofs_v_first)
            self.baked_flats = []
            for ss in range(nsssec):
                first = sub_q_first[ss]
                count = sub_q_count[ss]
                v_first = sub_v_first[ss]
                q_list = []
                for qi in range(count):
                    v0, v1, v2, v3 = struct.unpack_from(">4H", data, ofs_quads + (first + qi) * 8)
                    q_list.append([vert(v_first + v0), vert(v_first + v1), vert(v_first + v2), vert(v_first + v3)])
                self.baked_flats.append(q_list)
        elif magic == b"FLAT":
            hdr = struct.unpack_from(">4sHHIIIIII", data, 0)
            magic, ver, nsssec, numquads, ofs_first, ofs_count, ofs_bbox, ofs_quads = hdr[:8]
            sub_first = struct.unpack_from(f">{nsssec}H", data, ofs_first)
            sub_count = struct.unpack_from(f">{nsssec}H", data, ofs_count)
            self.baked_flats = []
            for ss in range(nsssec):
                first = sub_first[ss]
                count = sub_count[ss]
                q_list = []
                for qi in range(count):
                    qofs = ofs_quads + (first + qi) * 16
                    raw = struct.unpack_from(">4h4h", data, qofs)
                    xs = raw[:4]
                    ys = raw[4:]
                    q = [(xs[0], ys[0]), (xs[1], ys[1]), (xs[2], ys[2]), (xs[3], ys[3])]
                    q_list.append(q)
                self.baked_flats.append(q_list)
        else:
            print(f"Unknown FLT magic: {magic}")
            self.baked_flats = None

    def load_wl(self):
        wl_path = os.path.join(self.wl_dir, f"{self.map_name}.WL")
        if not os.path.exists(wl_path):
            self.baked_walls = None
            return
            
        with open(wl_path, "rb") as f:
            data = f.read()
            
        hdr = struct.unpack_from(">4sHHIIII", data, 0)
        magic, ver, nsssec, total_size, ofs_subsectors, ofs_pillars, ofs_segs = hdr
        if magic != b"MWAL":
            print(f"Invalid WL magic: {magic}")
            self.baked_walls = None
            return
            
        subsector_headers = []
        for i in range(nsssec):
            sh = struct.unpack_from(">HHHHHHI", data, ofs_subsectors + i * 16)
            subsector_headers.append({
                "num_pillars": sh[0],
                "pillar_start": sh[1],
                "num_segs": sh[2],
                "seg_start": sh[3],
                "num_dsp_batches": sh[4],
                "max_batch_size": sh[5]
            })
            
        # Seam fix: reserved[0] (byte 24) = ofs_pillar_frac, placed between pillars and segs
        ofs_pillar_frac = struct.unpack_from(">I", data, 24)[0]
        has_frac = ofs_pillars < ofs_pillar_frac < ofs_segs
        num_pillars = ((ofs_pillar_frac if has_frac else ofs_segs) - ofs_pillars) // 4
        pillars = []
        for i in range(num_pillars):
            px, py = struct.unpack_from(">hh", data, ofs_pillars + i * 4)
            if has_frac:
                fr = data[ofs_pillar_frac + i]
                px += (fr >> 4) / 16.0
                py += (fr & 15) / 16.0
            pillars.append((px, py))
            
        num_segs = (len(data) - ofs_segs) // 32
        baked_segs = []
        for i in range(num_segs):
            raw = struct.unpack_from(">HHHHHBBHHHhHHHHHH", data, ofs_segs + i * 32)
            baked_segs.append({
                "pillar_idx": list(raw[:5]),
                "baked_light": raw[5],
                "flags": raw[6],
                "tex_64": raw[7],
                "tex_32": raw[8],
                "tex_16": raw[9],
                "v_off": raw[10],
                "peg_sec": raw[11],
                "top_sec": raw[12],
                "bot_sec": raw[13],
                "u_start": raw[14],
                "u_span": raw[15]
            })
            
        self.baked_walls = {
            "subsectors": subsector_headers,
            "pillars": pillars,
            "segs": baked_segs
        }


    def get_flat(self, name, lvl=0):
        key = ("f", name, lvl)
        if key in self._shaded_cache:
            return self._shaded_cache[key]
        if hasattr(self, "flats_idx") and name in self.flats_idx:
            lvl = max(0, min(len(self.colormap_palettes) - 1, int(lvl)))
            res = self.colormap_palettes[lvl][self.flats_idx[name]]
            self._shaded_cache[key] = res
            return res
        if name in self.flats:
            return self.flats[name]
        chk = np.zeros((64, 64, 3), dtype=np.uint8)
        chk[:32, :32] = [128, 128, 128]
        chk[32:, 32:] = [128, 128, 128]
        return chk

    def get_texture(self, name, lvl=0):
        key = ("t", name, lvl)
        if key in self._shaded_cache:
            return self._shaded_cache[key]
        if hasattr(self, "textures_idx") and name in self.textures_idx:
            lvl = max(0, min(len(self.colormap_palettes) - 1, int(lvl)))
            res = self.colormap_palettes[lvl][self.textures_idx[name]]
            self._shaded_cache[key] = res
            return res
        if name in self.textures:
            return self.textures[name]
        chk = np.zeros((64, 64, 3), dtype=np.uint8)
        chk[:32, :32] = [160, 60, 60]
        chk[32:, 32:] = [160, 60, 60]
        return chk
