# tools/check_all_maps.py - Full 36-map asset and PAK audit
import os, glob

maps = [f"E{e}M{m}" for e in range(1, 5) for m in range(1, 10)]
print(f"=== DOOM SEGA SATURN: ALL {len(maps)} MAPS SUMMARY ===")
header = f"{'Map':<6} | {'FLT (B)':<8} | {'SEG (B)':<8} | {'PVS (B)':<8} | {'LEAF (B)':<8} | {'TEX (B)':<9} | {'SPR (B)':<10} | {'PAK Total':<10}"
print(header)
print("-" * len(header))

total_pak = 0
for m in maps:
    flt_sz = os.path.getsize(f"cd/{m}.FLT")
    seg_sz = os.path.getsize(f"cd/{m}.SEG")
    pvs_sz = os.path.getsize(f"cd/{m}.PVS")
    leaf_sz = os.path.getsize(f"cd/{m}.LEAF")
    tex_sz = os.path.getsize(f"cd/{m}.TEX")
    spr_sz = os.path.getsize(f"cd/{m}.SPR")
    pak_sz = os.path.getsize(f"cd/{m}.PAK")
    total_pak += pak_sz
    print(f"{m:<6} | {flt_sz:<8d} | {seg_sz:<8d} | {pvs_sz:<8d} | {leaf_sz:<8d} | {tex_sz:<9d} | {spr_sz:<10d} | {pak_sz/1024:>7.1f} KB")

print("-" * len(header))
print(f"Total PAK archives: {len(maps)} files, {total_pak/1024/1024:.2f} MB across all 36 maps.")
