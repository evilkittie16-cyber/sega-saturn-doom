import sys
sys.path.insert(0, '.')
from tools.bake_walls import load_wad, load_texture_dictionary, get_map_lumps, WAD_PATH
import tools.bake_walls as bw

wad_data, directory = load_wad(WAD_PATH)
tex_dict = load_texture_dictionary(wad_data, directory)
lumps = get_map_lumps(wad_data, directory, 'E1M8')
sh, pil, segs = bw.bake_map_walls('E1M8', lumps, tex_dict)
print(f'Current E1M8 segs: {len(segs)}')

# Test with modified bake_map_walls
with open('tools/bake_walls.py', 'r') as f:
    src = f.read()

src_mod = src.replace(
    'stw = float(surf["tw"]) if surf["tw"] > 0 else 64.0',
    'stw = float(surf["tw"]) if surf["tw"] > 0 else 64.0\n                if front_sec["ceilingpic"].startswith("F_SKY"): stw = max(stw, 256.0)'
)
d = {}
exec(src_mod, d)
sh2, pil2, segs2 = d['bake_map_walls']('E1M8', lumps, tex_dict)
print(f'With sky stw >= 256: {len(segs2)} segs')

src_mod512 = src.replace(
    'stw = float(surf["tw"]) if surf["tw"] > 0 else 64.0',
    'stw = float(surf["tw"]) if surf["tw"] > 0 else 64.0\n                if front_sec["ceilingpic"].startswith("F_SKY"): stw = max(stw, 512.0)'
)
d512 = {}
exec(src_mod512, d512)
sh3, pil3, segs3 = d512['bake_map_walls']('E1M8', lumps, tex_dict)
print(f'With sky stw >= 512: {len(segs3)} segs')
