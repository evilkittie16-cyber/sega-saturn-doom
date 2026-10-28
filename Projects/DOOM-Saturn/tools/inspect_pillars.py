import sys
import os
sys.path.insert(0, os.path.abspath('.'))
from saturn_sim.map_data import MapData

m = MapData(wad_path='../cd/DOOM.WAD', map_name='E1M1', flt_dir='../cd', wl_dir='../cd')
bw = m.baked_walls
print(f"Num subsectors: {len(bw['subsectors'])}, pillars: {len(bw['pillars'])}, segs: {len(bw['segs'])}")

for sub_i, ss in enumerate(bw['subsectors']):
    start = ss['seg_start']
    count = ss['num_segs']
    pil_start = ss['pillar_start']
    for si in range(start, start + count):
        seg = bw['segs'][si]
        tex_id = seg['tex_64']
        p0_idx = seg['pillar_idx'][0] + pil_start
        p4_idx = seg['pillar_idx'][4] + pil_start
        p0 = bw['pillars'][p0_idx]
        p4 = bw['pillars'][p4_idx]
        tex_name = m.tex_by_idx.get(tex_id, (f"Unknown({tex_id})", 0, 0))[0]
        # Only print segs near spawn: player is at (1056, -3616)
        if 800 <= p0[0] <= 1300 and -3700 <= p0[1] <= -3000:
            print(f"Sub {sub_i} Seg {si}: p0={p0} -> p4={p4}, tex={tex_name} (id {tex_id}), u_start={seg['u_start']}, u_span={seg['u_span']}, flags={hex(seg['flags'])}")
