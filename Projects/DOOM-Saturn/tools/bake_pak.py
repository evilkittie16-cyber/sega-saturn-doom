#!/usr/bin/env python3
"""
bake_pak.py - Consolidate offline-baked map assets into a single <MAP>.PAK per level.

Sections included in each PAK:
  - FLAT: Pre-baked floor & ceiling quads (.FLT)
  - SEGS: Pre-subdivided wall segs (.SEG)
  - PVS : Potentially Visible Set bitmasks (.PVS)
  - LEAF: Retail convex-polygon leaf data (.LEAF)
  - MTEX: Composited wall textures (.TEX)
  - MSPR: Row-major sprite bitmaps (.SPR)

Format:
  Sector 0 (2048 bytes):
    Header (16 bytes):
      char     magic[4];       // "MPAK"
      uint16_t version;        // 1
      uint16_t num_sections;   // 6
      uint32_t total_size;     // total file size in bytes
      uint32_t reserved;
    Section Table (16 bytes per entry):
      char     name[4];        // "FLAT", "SEGS", "PVS ", "LEAF", "MTEX", "MSPR"
      uint32_t sec_offset;     // sector index in PAK (file_pos = sec_offset * 2048)
      uint32_t byte_size;      // exact byte size
      uint32_t reserved;
  Sector 1+:
    Section data, each aligned to 2048-byte CD sector boundary.
"""

import os, struct, glob

SECTOR_SIZE = 2048

SECTION_DEFS = [
    ("FLAT", ".FLT"),
    ("WALL", ".WL"),
    ("PVS ", ".PVS"),
    ("LEAF", ".LEAF"),
    ("MTEX", ".TEX"),
    ("MSPR", ".SPR"),
]

def build_pak_for_map(mapname, cd_dir="cd", staging_dir="staging_assets"):
    sections = []
    
    for sec_name, ext in SECTION_DEFS:
        path_cd = os.path.join(cd_dir, f"{mapname}{ext}")
        path_stage = os.path.join(staging_dir, f"{mapname}{ext}")
        if os.path.exists(path_cd) and os.path.exists(path_stage):
            path = path_cd if os.path.getmtime(path_cd) >= os.path.getmtime(path_stage) else path_stage
        elif os.path.exists(path_cd):
            path = path_cd
        elif os.path.exists(path_stage):
            path = path_stage
        else:
            print(f"  [WARN] Missing {path_cd}, skipping section {sec_name}")
            continue
        with open(path, "rb") as f:
            data = f.read()
        sections.append({
            "name": sec_name,
            "data": data,
            "size": len(data),
        })
    
    if not sections:
        print(f"  No sections found for {mapname}!")
        return None
    
    # Calculate sector offsets
    # Sector 0 is Header + TOC
    cur_sector = 1
    for sec in sections:
        sec["sec_offset"] = cur_sector
        sectors_needed = (sec["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE
        cur_sector += sectors_needed
    
    total_bytes = cur_sector * SECTOR_SIZE
    
    # Build Sector 0 (Header + TOC)
    header = bytearray(SECTOR_SIZE)
    # Magic "MPAK", version 1, num_sections
    struct.pack_into(">4sHHII", header, 0, b"MPAK", 1, len(sections), total_bytes, 0)
    
    # Section Table
    for i, sec in enumerate(sections):
        entry_ofs = 16 + i * 16
        name_bytes = sec["name"].encode("ascii")
        if len(name_bytes) < 4:
            name_bytes = name_bytes.ljust(4, b" ")
        struct.pack_into(">4sIII", header, entry_ofs, name_bytes, sec["sec_offset"], sec["size"], 0)
    
    # Write output PAK
    out_path = os.path.join(cd_dir, f"{mapname}.PAK")
    with open(out_path, "wb") as f:
        f.write(header)
        for sec in sections:
            f.write(sec["data"])
            pad = (SECTOR_SIZE - (len(sec["data"]) % SECTOR_SIZE)) % SECTOR_SIZE
            if pad > 0:
                f.write(b"\x00" * pad)
    
    print(f"Built {out_path}: {len(sections)} sections, {total_bytes:,} bytes ({total_bytes/1024:.1f} KB, {cur_sector} sectors)")
    for sec in sections:
        print(f"   [{sec['name']}] sector {sec['sec_offset']:>4d} (offset 0x{sec['sec_offset']*2048:08X}), size {sec['size']:>10,} bytes")
    return out_path

def main():
    print("=== Baking Level PAKs for maps present in cd/ ===")
    for e in range(1, 5):
        for m in range(1, 10):
            mapname = f"E{e}M{m}"
            if os.path.exists(os.path.join("cd", f"{mapname}.WL")):
                build_pak_for_map(mapname)

if __name__ == "__main__":
    main()
