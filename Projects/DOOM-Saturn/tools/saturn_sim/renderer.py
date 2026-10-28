import math
import numpy as np
import cv2

def clip_poly_near_z(poly_cam_uv, near_z=4.0):
    """
    Sutherland-Hodgman clipping of a 2D camera-space polygon against cz >= near_z.
    poly_cam_uv: list of ((cx, cz), (u, v))
    """
    out = []
    n = len(poly_cam_uv)
    for i in range(n):
        cur_pt, cur_uv = poly_cam_uv[i]
        prev_pt, prev_uv = poly_cam_uv[(i + n - 1) % n]
        
        cur_in = cur_pt[1] >= near_z
        prev_in = prev_pt[1] >= near_z
        
        if cur_in:
            if not prev_in:
                t = (near_z - prev_pt[1]) / (cur_pt[1] - prev_pt[1])
                ix = prev_pt[0] + (cur_pt[0] - prev_pt[0]) * t
                iu = prev_uv[0] + (cur_uv[0] - prev_uv[0]) * t
                iv = prev_uv[1] + (cur_uv[1] - prev_uv[1]) * t
                out.append(((ix, near_z), (iu, iv)))
            out.append((cur_pt, cur_uv))
        elif prev_in:
            t = (near_z - prev_pt[1]) / (cur_pt[1] - prev_pt[1])
            ix = prev_pt[0] + (cur_pt[0] - prev_pt[0]) * t
            iu = prev_uv[0] + (cur_uv[0] - prev_uv[0]) * t
            iv = prev_uv[1] + (cur_uv[1] - prev_uv[1]) * t
            out.append(((ix, near_z), (iu, iv)))
    return out

class SaturnRenderer:
    def __init__(self, map_data, rasterizer):
        self.map_data = map_data
        self.rasterizer = rasterizer
        
        self.viewx = 0.0
        self.viewy = 0.0
        self.viewz = 0.0
        self.viewangle = 0.0 # radians
        
        self.enable_occlusion = True
        self.enable_walls = True
        self.enable_flats = True
        self.enable_pvs = True
        
        # Determine horizon centery:
        # PC mode (height 200, 3D window 168): 84.0
        # Saturn mode (height 224, 3D window 192): 96.0
        if self.rasterizer.height == 200:
            self.centery = 84.0
        else:
            self.centery = 96.0
            
        # Stats for current frame
        self.stat_wall_quads = 0
        self.stat_flat_quads = 0
        self.stat_visited_subsectors = 0
        self.solid_span = np.zeros(rasterizer.width, dtype=bool)

    def set_camera(self, x, y, z, angle_deg):
        self.viewx = float(x)
        self.viewy = float(y)
        self.viewz = float(z)
        self.viewangle = math.radians(float(angle_deg))
        self.cos_ang = math.cos(self.viewangle)
        self.sin_ang = math.sin(self.viewangle)

    def to_cam(self, x, y):
        dx = x - self.viewx
        dy = y - self.viewy
        cz = dx * self.cos_ang + dy * self.sin_ang
        cx = dx * self.sin_ang - dy * self.cos_ang
        return cx, cz

    def proj(self, x, y, z):
        cx, cz = self.to_cam(x, y)
        if cz <= 4.0:
            return None
        scale = 160.0 / cz
        px = 160.0 + cx * scale
        py = self.centery - (z - self.viewz) * scale
        return px, py, cz, scale

    def get_light_level(self, lightlevel, dist):
        # Carmack's distance diminishing light formula calibrated for Sega Saturn
        i = (lightlevel / 16.0)
        startmap = (15.0 - i) * 4.0
        scale = (160.0 * 65536.0) / max(1.0, float(dist))
        index = int(scale) >> 12
        level = int(round(startmap - (index * 0.73)))
        return max(0, min(31, level))

    def is_span_occluded(self, x0, x1):
        if not self.enable_occlusion:
            return False
        x0_c = max(0, min(self.rasterizer.width - 1, int(x0)))
        x1_c = max(0, min(self.rasterizer.width, int(math.ceil(x1))))
        if x0_c >= x1_c:
            return False
        return bool(np.all(self.solid_span[x0_c:x1_c]))

    def mark_solid_span(self, x0, x1):
        if not self.enable_occlusion:
            return
        x0_c = max(0, min(self.rasterizer.width - 1, int(x0)))
        x1_c = max(0, min(self.rasterizer.width, int(math.ceil(x1))))
        if x0_c < x1_c:
            self.solid_span[x0_c:x1_c] = True

    def render_frame(self):
        self.rasterizer.clear((0, 0, 0))
        self.solid_span[:] = False
        self.stat_wall_quads = 0
        self.stat_flat_quads = 0
        self.stat_visited_subsectors = 0
        
        self.vdp1_queue = []
        
        # Traverse BSP from root
        if self.map_data.nodes:
            self.walk_bsp(len(self.map_data.nodes) - 1)
        else:
            for ss in range(len(self.map_data.subsectors)):
                self.render_subsector(ss)
                
        # Execute VDP1 queue in reverse (back-to-front painter's order)
        for cmd in reversed(self.vdp1_queue):
            self.rasterizer.draw_quad(
                cmd["pts"],
                cmd["tex"],
                tex_coords=cmd.get("uv"),
                wire_color=cmd.get("wire_color")
            )

    def walk_bsp(self, bspnum):
        if np.all(self.solid_span):
            return
            
        if bspnum & 0x8000:
            ss = bspnum & ~0x8000
            self.render_subsector(ss)
            return
            
        node = self.map_data.nodes[bspnum]
        dx = self.viewx - node["x"]
        dy = self.viewy - node["y"]
        side = 0 if (dy * node["dx"] < node["dy"] * dx) else 1
        
        self.walk_bsp(node["children"][side])
        self.walk_bsp(node["children"][side ^ 1])

    def render_subsector(self, ss_idx):
        if ss_idx >= len(self.map_data.subsectors):
            return
        ss = self.map_data.subsectors[ss_idx]
        sec = self.map_data.sectors[ss["sector"]]
        self.stat_visited_subsectors += 1
        
        # 1. Render subsector flats (queued first so drawn under walls in reverse painter's order)
        if self.enable_flats and self.map_data.baked_flats:
            self.render_subsector_flats(ss_idx, sec)

        # 2. Render subsector walls (queued second so drawn over flats)
        if self.enable_walls:
            if self.map_data.baked_walls:
                self.render_subsector_baked_walls(ss_idx)
            else:
                count = ss["numlines"]
                first = ss["firstline"]
                for i in range(first, first + count):
                    seg = self.map_data.segs[i]
                    self.render_seg(seg, sec)

    def render_subsector_baked_walls(self, ss_idx):
        if not self.map_data.baked_walls or ss_idx >= len(self.map_data.baked_walls["subsectors"]):
            return
        sh = self.map_data.baked_walls["subsectors"][ss_idx]
        num_pillars = sh["num_pillars"]
        pillar_start = sh["pillar_start"]
        num_segs = sh["num_segs"]
        seg_start = sh["seg_start"]
        if num_pillars == 0 or num_segs == 0:
            return

        # 1. Transform deduplicated subsector 2D pillars (SCU-DSP matrix emulation)
        trans_pillars = []
        for p_rel in range(num_pillars):
            px, py = self.map_data.baked_walls["pillars"][pillar_start + p_rel]
            cx, cz = self.to_cam(px, py)
            trans_pillars.append((px, py, cx, cz))

        NEAR_Z = 4.0

        # 2. Iterate baked segs
        for s_i in range(num_segs):
            seg = self.map_data.baked_walls["segs"][seg_start + s_i]
            p_indices = seg["pillar_idx"]
            p0 = trans_pillars[p_indices[0]]
            p4 = trans_pillars[p_indices[4]]

            # Backface culling: (cx * dy - cy * dx) <= 0
            dx = p4[0] - p0[0]
            dy = p4[1] - p0[1]
            cx = self.viewx - p0[0]
            cy = self.viewy - p0[1]
            if (cx * dy - cy * dx) <= 0:
                continue

            cz0 = p0[3]
            cz4 = p4[3]
            if cz0 < NEAR_Z and cz4 < NEAR_Z:
                continue

            # Screen projection of the 64-unit span
            cx0, cz0_c = p0[2], cz0
            cx4, cz4_c = p4[2], cz4
            if cz0_c < NEAR_Z:
                t = (NEAR_Z - cz0_c) / (cz4_c - cz0_c)
                cx0 = cx0 + (cx4 - cx0) * t
                cz0_c = NEAR_Z
            elif cz4_c < NEAR_Z:
                t = (NEAR_Z - cz4_c) / (cz0_c - cz4_c)
                cx4 = cx4 + (cx0 - cx4) * t
                cz4_c = NEAR_Z

            s0 = 160.0 / cz0_c
            s4 = 160.0 / cz4_c
            xA = 160.0 + cx0 * s0
            xB = 160.0 + cx4 * s4
            if xB <= 0 or xA >= self.rasterizer.width or xA >= xB:
                continue
            if self.is_span_occluded(xA, xB):
                continue

            # Law 2: 1D Wall LOD via screen width
            screen_width = abs(xB - xA)
            if screen_width < 32:
                # Tier 2: 1 Quad spanning full 64 units
                strips = [(0, 4, 0.0, 1.0)]
            elif screen_width < 64:
                # Tier 1: 2 Quads (32-unit vertical strips)
                strips = [(0, 2, 0.0, 0.5), (2, 4, 0.5, 1.0)]
            else:
                # Tier 0: 4 Quads (16-unit vertical strips)
                strips = [(0, 1, 0.0, 0.25), (1, 2, 0.25, 0.5), (2, 3, 0.5, 0.75), (3, 4, 0.75, 1.0)]

            # Heights and Pegging
            flags = seg["flags"]
            top_sec = self.map_data.sectors[seg["top_sec"]]
            bot_sec = self.map_data.sectors[seg["bot_sec"]]

            if flags & 0x01: # SOLID
                top_z = top_sec["ceilingheight"]
                bot_z = bot_sec["floorheight"]
            elif flags & 0x02: # UPPER
                top_z = top_sec["ceilingheight"]
                bot_z = bot_sec["ceilingheight"]
            elif flags & 0x04: # LOWER
                top_z = top_sec["floorheight"]
                bot_z = bot_sec["floorheight"]
            elif flags & 0x08: # MID
                top_z = min(top_sec["ceilingheight"], bot_sec["ceilingheight"])
                bot_z = max(top_sec["floorheight"], bot_sec["floorheight"])
            else:
                continue

            if top_z <= bot_z:
                continue

            tex_id = seg["tex_64"]
            tex_name, tw, th = self.map_data.tex_by_idx.get(tex_id, ("-", 64, 64))
            if tex_name == "-" or not tex_name:
                continue

            avg_cz = (cz0_c + cz4_c) * 0.5
            lvl = self.get_light_level(seg["baked_light"], avg_cz)
            tex = self.map_data.get_texture(tex_name, lvl)
            if tex is None:
                continue
            th, tw = tex.shape[:2]

            v_off = seg["v_off"]
            wall_h = top_z - bot_z
            if flags & 0x20: # SEG_PEG_LOWER_UNPEGGED
                peg_sec = self.map_data.sectors[seg["peg_sec"]]
                v_top = (peg_sec["ceilingheight"] - top_sec["floorheight"]) - v_off
                v_bot = v_top + wall_h
            elif flags & 0x10: # SEG_PEG_BOTTOM
                v_bot = th + v_off
                v_top = v_bot - wall_h
            else: # Top-anchored
                v_top = v_off
                v_bot = v_top + wall_h

            for (idx_a, idx_b, u_start, u_end) in strips:
                pa = trans_pillars[p_indices[idx_a]]
                pb = trans_pillars[p_indices[idx_b]]
                cxa, cza = pa[2], pa[3]
                cxb, czb = pb[2], pb[3]
                if cza < NEAR_Z and czb < NEAR_Z:
                    continue
                if cza < NEAR_Z:
                    t = (NEAR_Z - cza) / (czb - cza)
                    cxa = cxa + (cxb - cxa) * t
                    cza = NEAR_Z
                elif czb < NEAR_Z:
                    t = (NEAR_Z - czb) / (cza - czb)
                    cxb = cxb + (cxa - cxb) * t
                    czb = NEAR_Z

                sa = 160.0 / cza
                sb = 160.0 / czb
                x_a = 160.0 + cxa * sa
                x_b = 160.0 + cxb * sb
                if x_b <= 0 or x_a >= self.rasterizer.width or x_a >= x_b:
                    continue
                if self.is_span_occluded(x_a, x_b):
                    continue

                ya_top = self.centery - (top_z - self.viewz) * sa
                yb_top = self.centery - (top_z - self.viewz) * sb
                ya_bot = self.centery - (bot_z - self.viewz) * sa
                yb_bot = self.centery - (bot_z - self.viewz) * sb

                u0 = seg.get("u_start", 0) + u_start * seg.get("u_span", tw)
                u1 = seg.get("u_start", 0) + u_end * seg.get("u_span", tw)

                quad_pts = [(x_a, ya_top), (x_b, yb_top), (x_b, yb_bot), (x_a, ya_bot)]
                uv_pts = [(u0, v_top), (u1, v_top), (u1, v_bot), (u0, v_bot)]

                self.vdp1_queue.append({
                    "pts": quad_pts, "tex": tex, "uv": uv_pts,
                    "wire_color": (255, 120, 50) if len(strips) > 1 else (0, 255, 255)
                })
                self.stat_wall_quads += 1

                if (flags & 0x01) and ya_top <= 0 and yb_top <= 0 and ya_bot >= self.rasterizer.height and yb_bot >= self.rasterizer.height:
                    self.mark_solid_span(x_a, x_b)


    def render_seg(self, seg, frontsec):
        v1 = self.map_data.verts[seg["v1"]]
        v2 = self.map_data.verts[seg["v2"]]
        
        # Backface cull
        dx = v2[0] - v1[0]
        dy = v2[1] - v1[1]
        cx = self.viewx - v1[0]
        cy = self.viewy - v1[1]
        if (cx * dy - cy * dx) <= 0:
            return
            
        cx1, cz1 = self.to_cam(v1[0], v1[1])
        cx2, cz2 = self.to_cam(v2[0], v2[1])
        
        NEAR_Z = 4.0
        if cz1 < NEAR_Z and cz2 < NEAR_Z:
            return
            
        u1_frac, u2_frac = 0.0, 1.0
        if cz1 < NEAR_Z:
            t = (NEAR_Z - cz1) / (cz2 - cz1)
            cx1 = cx1 + (cx2 - cx1) * t
            cz1 = NEAR_Z
            u1_frac = t
        elif cz2 < NEAR_Z:
            t = (NEAR_Z - cz2) / (cz1 - cz2)
            cx2 = cx2 + (cx1 - cx2) * t
            cz2 = NEAR_Z
            u2_frac = 1.0 - t
            
        s1 = 160.0 / cz1
        s2 = 160.0 / cz2
        xA = 160.0 + cx1 * s1
        xB = 160.0 + cx2 * s2
        
        if xB <= 0 or xA >= self.rasterizer.width or xA >= xB:
            return
            
        if self.is_span_occluded(xA, xB):
            return
            
        line = self.map_data.linedefs[seg["line"]]
        sidedef = self.map_data.sidedefs[line["s0"] if seg["side"] == 0 else line["s1"]]
        other_sd = self.map_data.sidedefs[line["s1"] if seg["side"] == 0 else line["s0"]] if line["s1"] != -1 else None
        backsec = self.map_data.sectors[other_sd["sector"]] if other_sd else None
        
        # Fake contrast (North-South vs East-West walls)
        eff_light = frontsec["lightlevel"]
        if v1[1] == v2[1]:
            eff_light = max(0, eff_light - 4)
        elif v1[0] == v2[0]:
            eff_light = min(255, eff_light + 4)
            
        avg_cz = (cz1 + cz2) * 0.5
        lvl = self.get_light_level(eff_light, avg_cz)
        
        # Front ceiling & floor Y
        ya_fceil = self.centery - (frontsec["ceilingheight"] - self.viewz) * s1
        yb_fceil = self.centery - (frontsec["ceilingheight"] - self.viewz) * s2
        ya_ffloor = self.centery - (frontsec["floorheight"] - self.viewz) * s1
        yb_ffloor = self.centery - (frontsec["floorheight"] - self.viewz) * s2
        
        seg_len = math.hypot(dx, dy)
        flags = line["flags"]
        ML_DONTPEGTOP = 8
        ML_DONTPEGBOTTOM = 16
        xoff = sidedef["xoffset"]
        yoff = sidedef["yoffset"]
        
        if not backsec:
            # 1-Sided Solid Wall
            tex_name = sidedef["mid"]
            if tex_name and tex_name != "-":
                tex = self.map_data.get_texture(tex_name, lvl)
                th, tw = tex.shape[:2]
                
                u0 = (seg["offset"] + xoff + seg_len * u1_frac) % tw
                u_span = seg_len * (u2_frac - u1_frac)
                u1 = u0 + u_span
                
                wall_h = frontsec["ceilingheight"] - frontsec["floorheight"]
                if flags & ML_DONTPEGBOTTOM:
                    v_bot = th + yoff
                    v_top = v_bot - wall_h
                else:
                    v_top = yoff
                    v_bot = v_top + wall_h
                    
                quad_pts = [(xA, ya_fceil), (xB, yb_fceil), (xB, yb_ffloor), (xA, ya_ffloor)]
                uv_pts = [(u0, v_top), (u1, v_top), (u1, v_bot), (u0, v_bot)]
                
                self.vdp1_queue.append({
                    "pts": quad_pts, "tex": tex, "uv": uv_pts,
                    "wire_color": (0, 255, 255)
                })
                self.stat_wall_quads += 1
                if ya_fceil <= 0 and yb_fceil <= 0 and ya_ffloor >= self.rasterizer.height and yb_ffloor >= self.rasterizer.height:
                    self.mark_solid_span(xA, xB)
        else:
            # 2-Sided Portal Wall
            # Upper step
            if backsec["ceilingheight"] < frontsec["ceilingheight"]:
                ya_bceil = self.centery - (backsec["ceilingheight"] - self.viewz) * s1
                yb_bceil = self.centery - (backsec["ceilingheight"] - self.viewz) * s2
                tex_name = sidedef["top"]
                if tex_name and tex_name != "-":
                    tex = self.map_data.get_texture(tex_name, lvl)
                    th, tw = tex.shape[:2]
                    u0 = (seg["offset"] + xoff + seg_len * u1_frac) % tw
                    u_span = seg_len * (u2_frac - u1_frac)
                    u1 = u0 + u_span
                    
                    step_h = frontsec["ceilingheight"] - backsec["ceilingheight"]
                    if flags & ML_DONTPEGTOP:
                        v_top = yoff
                        v_bot = v_top + step_h
                    else:
                        v_bot = th + yoff
                        v_top = v_bot - step_h
                        
                    quad_pts = [(xA, ya_fceil), (xB, yb_fceil), (xB, yb_bceil), (xA, ya_bceil)]
                    uv_pts = [(u0, v_top), (u1, v_top), (u1, v_bot), (u0, v_bot)]
                    self.vdp1_queue.append({
                        "pts": quad_pts, "tex": tex, "uv": uv_pts,
                        "wire_color": (0, 200, 255)
                    })
                    self.stat_wall_quads += 1
                    
            # Lower step
            if backsec["floorheight"] > frontsec["floorheight"]:
                ya_bfloor = self.centery - (backsec["floorheight"] - self.viewz) * s1
                yb_bfloor = self.centery - (backsec["floorheight"] - self.viewz) * s2
                tex_name = sidedef["bot"]
                if tex_name and tex_name != "-":
                    tex = self.map_data.get_texture(tex_name, lvl)
                    th, tw = tex.shape[:2]
                    u0 = (seg["offset"] + xoff + seg_len * u1_frac) % tw
                    u_span = seg_len * (u2_frac - u1_frac)
                    u1 = u0 + u_span
                    
                    step_h = backsec["floorheight"] - frontsec["floorheight"]
                    if flags & ML_DONTPEGBOTTOM:
                        v_top = (frontsec["ceilingheight"] - backsec["floorheight"]) - yoff
                        v_bot = (frontsec["ceilingheight"] - frontsec["floorheight"]) - yoff
                    else:
                        v_top = yoff
                        v_bot = v_top + step_h
                        
                    quad_pts = [(xA, ya_bfloor), (xB, yb_bfloor), (xB, yb_ffloor), (xA, ya_ffloor)]
                    uv_pts = [(u0, v_top), (u1, v_top), (u1, v_bot), (u0, v_bot)]
                    self.vdp1_queue.append({
                        "pts": quad_pts, "tex": tex, "uv": uv_pts,
                        "wire_color": (0, 150, 255)
                    })
                    self.stat_wall_quads += 1
                    
            # 2-sided middle texture (doors, fences, transparent textures)
            tex_name = sidedef["mid"]
            if tex_name and tex_name != "-":
                tex = self.map_data.get_texture(tex_name, lvl)
                th, tw = tex.shape[:2]
                u0 = (seg["offset"] + xoff + seg_len * u1_frac) % tw
                u_span = seg_len * (u2_frac - u1_frac)
                u1 = u0 + u_span
                
                top_h = min(frontsec["ceilingheight"], backsec["ceilingheight"])
                bot_h = max(frontsec["floorheight"], backsec["floorheight"])
                
                if flags & ML_DONTPEGBOTTOM:
                    v_bot = th + yoff
                    v_top = v_bot - (top_h - bot_h)
                else:
                    v_top = yoff
                    v_bot = v_top + (top_h - bot_h)
                    
                ya_mtop = self.centery - (top_h - self.viewz) * s1
                yb_mtop = self.centery - (top_h - self.viewz) * s2
                ya_mbot = self.centery - (bot_h - self.viewz) * s1
                yb_mbot = self.centery - (bot_h - self.viewz) * s2
                
                quad_pts = [(xA, ya_mtop), (xB, yb_mtop), (xB, yb_mbot), (xA, ya_mbot)]
                uv_pts = [(u0, v_top), (u1, v_top), (u1, v_bot), (u0, v_bot)]
                self.vdp1_queue.append({
                    "pts": quad_pts, "tex": tex, "uv": uv_pts,
                    "wire_color": (0, 255, 200)
                })
                self.stat_wall_quads += 1
                
            # Closed door occlusion
            if backsec["ceilingheight"] <= backsec["floorheight"]:
                if ya_fceil <= 0 and yb_fceil <= 0 and ya_ffloor >= self.rasterizer.height and yb_ffloor >= self.rasterizer.height:
                    self.mark_solid_span(xA, xB)

    def render_subsector_flats(self, ss_idx, sec):
        if ss_idx >= len(self.map_data.baked_flats):
            return
        quads = self.map_data.baked_flats[ss_idx]
        if not quads:
            return
            
        ceil_h = sec["ceilingheight"]
        ceil_pic = sec["ceilingpic"]
        floor_h = sec["floorheight"]
        floor_pic = sec["floorpic"]
        
        # Ceiling
        if ceil_pic != "F_SKY1" and self.viewz < ceil_h:
            for q in quads:
                xs = [p[0] for p in q]
                ys = [p[1] for p in q]
                min_x = math.floor(min(xs) / 64.0) * 64.0
                max_y = math.ceil(max(ys) / 64.0) * 64.0
                
                poly_cam_uv = []
                for p in q:
                    cx, cz = self.to_cam(p[0], p[1])
                    u = p[0] - min_x
                    v = max_y - p[1]
                    poly_cam_uv.append(((cx, cz), (u, v)))
                
                clipped = clip_poly_near_z(poly_cam_uv, near_z=4.0)
                if len(clipped) < 3:
                    continue
                
                avg_cz = sum(pt[0][1] for pt in clipped) / len(clipped)
                lvl = self.get_light_level(sec["lightlevel"], avg_cz)
                tex_ceil = self.map_data.get_flat(ceil_pic, lvl)
                
                screen_pts = []
                uv_pts = []
                for (cx, cz), (u, v) in clipped:
                    scale = 160.0 / cz
                    px = 160.0 + cx * scale
                    py = self.centery - (ceil_h - self.viewz) * scale
                    screen_pts.append((px, py))
                    uv_pts.append((u, v))
                    
                xs_scr = [p[0] for p in screen_pts]
                if max(xs_scr) <= 0 or min(xs_scr) >= self.rasterizer.width:
                    continue
                
                if len(screen_pts) == 4:
                    self.vdp1_queue.append({
                        "pts": screen_pts, "tex": tex_ceil, "uv": uv_pts,
                        "wire_color": (255, 255, 0)
                    })
                    self.stat_flat_quads += 1
                else:
                    for k in range(1, len(screen_pts) - 1):
                        tri = [screen_pts[0], screen_pts[k], screen_pts[k+1]]
                        tri_uv = [uv_pts[0], uv_pts[k], uv_pts[k+1]]
                        self.vdp1_queue.append({
                            "pts": [tri[0], tri[1], tri[2], tri[2]],
                            "tex": tex_ceil,
                            "uv": [tri_uv[0], tri_uv[1], tri_uv[2], tri_uv[2]],
                            "wire_color": (255, 255, 0)
                        })
                        self.stat_flat_quads += 1
                
        # Floor
        if self.viewz > floor_h:
            for q in quads:
                xs = [p[0] for p in q]
                ys = [p[1] for p in q]
                min_x = math.floor(min(xs) / 64.0) * 64.0
                max_y = math.ceil(max(ys) / 64.0) * 64.0
                
                poly_cam_uv = []
                for p in q:
                    cx, cz = self.to_cam(p[0], p[1])
                    u = p[0] - min_x
                    v = max_y - p[1]
                    poly_cam_uv.append(((cx, cz), (u, v)))
                
                clipped = clip_poly_near_z(poly_cam_uv, near_z=4.0)
                if len(clipped) < 3:
                    continue
                
                avg_cz = sum(pt[0][1] for pt in clipped) / len(clipped)
                lvl = self.get_light_level(sec["lightlevel"], avg_cz)
                tex_floor = self.map_data.get_flat(floor_pic, lvl)
                
                screen_pts = []
                uv_pts = []
                for (cx, cz), (u, v) in clipped:
                    scale = 160.0 / cz
                    px = 160.0 + cx * scale
                    py = self.centery - (floor_h - self.viewz) * scale
                    screen_pts.append((px, py))
                    uv_pts.append((u, v))
                    
                xs_scr = [p[0] for p in screen_pts]
                if max(xs_scr) <= 0 or min(xs_scr) >= self.rasterizer.width:
                    continue
                
                if len(screen_pts) == 4:
                    self.vdp1_queue.append({
                        "pts": screen_pts, "tex": tex_floor, "uv": uv_pts,
                        "wire_color": (255, 0, 255)
                    })
                    self.stat_flat_quads += 1
                else:
                    for k in range(1, len(screen_pts) - 1):
                        tri = [screen_pts[0], screen_pts[k], screen_pts[k+1]]
                        tri_uv = [uv_pts[0], uv_pts[k], uv_pts[k+1]]
                        self.vdp1_queue.append({
                            "pts": [tri[0], tri[1], tri[2], tri[2]],
                            "tex": tex_floor,
                            "uv": [tri_uv[0], tri_uv[1], tri_uv[2], tri_uv[2]],
                            "wire_color": (255, 0, 255)
                        })
                        self.stat_flat_quads += 1
