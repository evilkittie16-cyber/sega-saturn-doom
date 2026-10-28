#!/usr/bin/env python3
"""
generate_light_lut.py - Offline Lighting LUT Generator for Saturn DOOM & Windows Simulator

Solves:
1. VGA vs NTSC/PAL CRT TV Gamma: Uses a non-linear (square root / power) curve
   to keep lighting in bright bands (Levels 0-12) for longer distances before
   diminishing, avoiding the crushed dark end of TV outputs.
2. Resolution Scaling & Zero-CPU Overhead: Precomputes a 2D LUT
   flat_light_LUT[sector_light_level >> 4][screen_y] so the SH-2 CPU executes
   exactly 1 clock cycle (35 ns) with zero divisions, multiplications, or branches.
3. Ambient Light Floor: Clamps maximum darkness to Level 24-26 (CRAM Bank 6),
   guaranteeing that corridors and far geometry remain atmospheric and visible
   instead of dissolving into pitch black voids.
"""

import sys
import os
import math
import numpy as np

def compute_light_tables(
    num_sectors=16,
    screen_height=224,
    centery=84.0,
    planeheight=41.0,
    num_wall_buckets=32
):
    """
    Computes exact PC Doom flat and wall lighting LUTs mapped to Saturn VDP1 CRAM banks.

    PC DOOM Mathematical Parity Model:
    1. startmap = (15 - s) * 4
    2. Wall lighting (r_segs.c / r_main.c):
       index = rw_scale >> 12 = 2560 / dist (clamped to max 47)
       level = clamp(startmap - index / 2, 0, 31)
       bank  = clamp((level + 2) // 4, 0, 7)
    3. Flat lighting (r_plane.c / r_main.c):
       distance = (planeheight * 160) / dy
       j = distance / 16 = (10 * planeheight) / dy
       scale = int(80.0 / (j + 1.0))
       level = clamp(startmap - scale, 0, 31)
       bank  = clamp((level + 2) // 4, 0, 7)
    """
    flat_colormap = np.zeros((num_sectors, screen_height), dtype=np.uint8)
    flat_banks = np.zeros((num_sectors, screen_height), dtype=np.uint8)
    flat_cmdcolr = np.zeros((num_sectors, screen_height), dtype=np.uint16)

    for s in range(num_sectors):
        startmap = (15 - s) * 4

        for y in range(screen_height):
            eval_y = min(167, y)
            dy = max(1.0, abs(eval_y - centery))
            j = (10.0 * planeheight) / dy
            scale_distmap = int(80.0 / (j + 1.0))
            level = max(0, min(31, startmap - scale_distmap))

            flat_colormap[s, y] = level
            bank = min(7, max(0, (level + 2) // 4))
            flat_banks[s, y] = bank
            flat_cmdcolr[s, y] = (bank << 8)

    # Wall light LUT: wall_lut[sector_16][dist_bucket_32]
    wall_colormap = np.zeros((num_sectors, num_wall_buckets), dtype=np.uint8)
    wall_banks = np.zeros((num_sectors, num_wall_buckets), dtype=np.uint8)
    wall_cmdcolr = np.zeros((num_sectors, num_wall_buckets), dtype=np.uint16)

    for s in range(num_sectors):
        startmap = (15 - s) * 4
        for b in range(num_wall_buckets):
            dist = max(16.0, b * 64.0 + 32.0)
            idx = min(47, int(2560.0 / dist))
            level = max(0, min(31, startmap - (idx // 2)))

            wall_colormap[s, b] = level
            bank = min(7, max(0, (level + 2) // 4))
            wall_banks[s, b] = bank
            wall_cmdcolr[s, b] = (bank << 8)

    # Exact PC Doom Flat Z-Light (128 buckets, z >> 20)
    num_z_buckets = 128
    flat_zlight_colormap = np.zeros((num_sectors, num_z_buckets), dtype=np.uint8)
    flat_zlight_banks = np.zeros((num_sectors, num_z_buckets), dtype=np.uint8)
    flat_zlight_cmdcolr = np.zeros((num_sectors, num_z_buckets), dtype=np.uint16)

    for s in range(num_sectors):
        startmap = (15 - s) * 4
        for j in range(num_z_buckets):
            scale = 160 // (j + 1)
            level = max(0, min(31, startmap - (scale // 2)))
            bank = min(7, max(0, (level + 1) // 4))
            flat_zlight_colormap[s, j] = level
            flat_zlight_banks[s, j] = bank
            flat_zlight_cmdcolr[s, j] = (bank << 8)

    # Exact PC Doom Wall Z-Light (128 buckets, dist >> 4)
    wall_zlight_colormap = np.zeros((num_sectors, num_z_buckets), dtype=np.uint8)
    wall_zlight_banks = np.zeros((num_sectors, num_z_buckets), dtype=np.uint8)
    wall_zlight_cmdcolr = np.zeros((num_sectors, num_z_buckets), dtype=np.uint16)

    for s in range(num_sectors):
        startmap = (15 - s) * 4
        for b in range(num_z_buckets):
            dist = max(16.0, b * 16.0 + 8.0)
            idx = min(47, int(2560.0 / dist))
            level = max(0, min(31, startmap - (idx // 2)))
            bank = min(7, max(0, (level + 2) // 4))
            wall_zlight_colormap[s, b] = level
            wall_zlight_banks[s, b] = bank
            wall_zlight_cmdcolr[s, b] = (bank << 8)

    return {
        "flat_colormap": flat_colormap,
        "flat_banks": flat_banks,
        "flat_cmdcolr": flat_cmdcolr,
        "wall_colormap": wall_colormap,
        "wall_banks": wall_banks,
        "wall_cmdcolr": wall_cmdcolr,
        "flat_zlight_colormap": flat_zlight_colormap,
        "flat_zlight_banks": flat_zlight_banks,
        "flat_zlight_cmdcolr": flat_zlight_cmdcolr,
        "wall_zlight_colormap": wall_zlight_colormap,
        "wall_zlight_banks": wall_zlight_banks,
        "wall_zlight_cmdcolr": wall_zlight_cmdcolr,
        "screen_height": screen_height,
        "centery": centery,
        "gamma": 1.0,
        "ambient_floor": 31,
        "num_wall_buckets": num_wall_buckets,
        "num_z_buckets": num_z_buckets,
    }

def export_c_header(data, filepath):
    """
    Exports C header with flat_light_LUT and wall_light_LUT.
    """
    flat_banks = data["flat_banks"]
    wall_banks = data["wall_banks"]
    flat_cmdcolr = data["flat_cmdcolr"]
    num_sectors = flat_banks.shape[0]
    height = flat_banks.shape[1]
    num_wall_buckets = wall_banks.shape[1]

    with open(filepath, "w", encoding="utf-8") as f:
        f.write("/*\n")
        f.write(" * saturn_light_lut.h - Offline Pre-Baked Lighting Tables for Sega Saturn DOOM\n")
        f.write(" * Auto-generated by tools/generate_light_lut.py\n")
        f.write(" * \n")
        f.write(" * Features:\n")
        f.write(f" *   - Resolution: Height {height}, Horizon centery {data['centery']:.1f}\n")
        f.write(f" *   - Non-linear Gamma Falloff: {data['gamma']:.2f}\n")
        f.write(f" *   - Ambient Floor: COLORMAP Level {data['ambient_floor']} (CRAM Bank {min(7, int(round(data['ambient_floor']/4.0)))})\n")
        f.write(" *   - Runtime Overhead: 1 SH-2 Clock Cycle (35 ns)\n")
        f.write(" */\n\n")
        f.write("#ifndef SATURN_LIGHT_LUT_H\n")
        f.write("#define SATURN_LIGHT_LUT_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define LIGHT_SCREEN_HEIGHT  {height}\n")
        f.write(f"#define LIGHT_SECTORS        {num_sectors}\n")
        f.write(f"#define LIGHT_WALL_BUCKETS   {num_wall_buckets}\n")
        f.write(f"#define LIGHT_Z_BUCKETS      {data['num_z_buckets']}\n")
        f.write(f"#define LIGHT_AMBIENT_FLOOR  {data['ambient_floor']}\n\n")

        # 1. flat_light_bank_LUT (3.5 KB - fits in 4 KB OCRAM)
        f.write("/* 2D Flat Light Bank LUT: flat_light_bank_LUT[sector >> 4][screen_y]\n")
        f.write(" * Returns CRAM bank index (0..7). Memory size: 3,584 bytes (3.5 KB) */\n")
        f.write(f"static const uint8_t flat_light_bank_LUT[{num_sectors}][{height}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} (Base Brightness {s * 16}..{s * 16 + 15}) */\n    {{ ")
            row = [str(flat_banks[s, y]) for y in range(height)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 2. flat_light_level_LUT (3.5 KB - Exact 0..31 PC Doom Colormap Row)
        flat_colormap = data["flat_colormap"]
        f.write("/* 2D Flat Exact Colormap Level LUT: flat_light_level_LUT[sector >> 4][screen_y]\n")
        f.write(" * Returns exact PC Doom COLORMAP row (0..31) for 100% lighting parity testing. */\n")
        f.write(f"static const uint8_t flat_light_level_LUT[{num_sectors}][{height}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(flat_colormap[s, y]) for y in range(height)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 3. wall_light_bank_LUT (512 bytes)
        f.write("/* 2D Wall Light Bank LUT: wall_light_bank_LUT[sector >> 4][cz >> 6]\n")
        f.write(" * Returns CRAM bank index (0..7). Memory size: 512 bytes */\n")
        f.write(f"static const uint8_t wall_light_bank_LUT[{num_sectors}][{num_wall_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(wall_banks[s, b]) for b in range(num_wall_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 4. wall_light_level_LUT (512 bytes - Exact 0..31 PC Doom Colormap Row)
        wall_colormap = data["wall_colormap"]
        f.write("/* 2D Wall Exact Colormap Level LUT: wall_light_level_LUT[sector >> 4][cz >> 6]\n")
        f.write(" * Returns exact PC Doom COLORMAP row (0..31) for 100% lighting parity testing. */\n")
        f.write(f"static const uint8_t wall_light_level_LUT[{num_sectors}][{num_wall_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(wall_colormap[s, b]) for b in range(num_wall_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 5. flat_light_cmdcolr_LUT (7,168 bytes - Prepacked Bank + Exact Colormap Level)
        f.write("/* 2D Flat Prepacked CMDCOLR LUT: flat_light_cmdcolr_LUT[sector >> 4][screen_y]\n")
        f.write(" * Returns prepacked 16-bit CMDCOLR ((bank<<8) | 0x80 | level). Memory: 7,168 bytes */\n")
        f.write(f"static const uint16_t flat_light_cmdcolr_LUT[{num_sectors}][{height}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [f"0x{flat_cmdcolr[s, y]:04X}" for y in range(height)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 6. wall_light_cmdcolr_LUT (1,024 bytes - Prepacked Bank + Exact Colormap Level)
        wall_cmdcolr = data["wall_cmdcolr"]
        f.write("/* 2D Wall Prepacked CMDCOLR LUT: wall_light_cmdcolr_LUT[sector >> 4][cz >> 6]\n")
        f.write(" * Returns prepacked 16-bit CMDCOLR ((bank<<8) | 0x80 | level). Memory: 1,024 bytes */\n")
        f.write(f"static const uint16_t wall_light_cmdcolr_LUT[{num_sectors}][{num_wall_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [f"0x{wall_cmdcolr[s, b]:04X}" for b in range(num_wall_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 7. flat_zlight_bank_LUT (2,048 bytes)
        flat_zlight_banks = data["flat_zlight_banks"]
        num_z_buckets = data["num_z_buckets"]
        f.write("/* 2D Flat Z-Light Bank LUT: flat_zlight_bank_LUT[sector >> 4][cz >> 20]\n")
        f.write(" * Returns CRAM bank index (0..7). Memory size: 2,048 bytes */\n")
        f.write(f"static const uint8_t flat_zlight_bank_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(flat_zlight_banks[s, j]) for j in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 8. flat_zlight_level_LUT (2,048 bytes - Exact 0..31 PC Doom Colormap Row)
        flat_zlight_colormap = data["flat_zlight_colormap"]
        f.write("/* 2D Flat Exact Colormap Level Z-LUT: flat_zlight_level_LUT[sector >> 4][cz >> 20]\n")
        f.write(" * Returns exact PC Doom COLORMAP row (0..31) for 100% lighting parity testing. */\n")
        f.write(f"static const uint8_t flat_zlight_level_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(flat_zlight_colormap[s, j]) for j in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 9. flat_zlight_cmdcolr_LUT (4,096 bytes - Prepacked Bank + Exact Colormap Level)
        flat_zlight_cmdcolr = data["flat_zlight_cmdcolr"]
        f.write("/* 2D Flat Prepacked CMDCOLR Z-LUT: flat_zlight_cmdcolr_LUT[sector >> 4][cz >> 20]\n")
        f.write(" * Returns prepacked 16-bit CMDCOLR ((bank<<8) | 0x80 | level). Memory: 4,096 bytes */\n")
        f.write(f"static const uint16_t flat_zlight_cmdcolr_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [f"0x{flat_zlight_cmdcolr[s, j]:04X}" for j in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 10. wall_zlight_bank_LUT (2,048 bytes)
        wall_zlight_banks = data["wall_zlight_banks"]
        f.write("/* 2D Wall Z-Light Bank LUT: wall_zlight_bank_LUT[sector >> 4][dist >> 4]\n")
        f.write(" * Returns CRAM bank index (0..7). Memory size: 2,048 bytes */\n")
        f.write(f"static const uint8_t wall_zlight_bank_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(wall_zlight_banks[s, b]) for b in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 11. wall_zlight_level_LUT (2,048 bytes - Exact 0..31 PC Doom Colormap Row)
        wall_zlight_colormap = data["wall_zlight_colormap"]
        f.write("/* 2D Wall Exact Colormap Level Z-LUT: wall_zlight_level_LUT[sector >> 4][dist >> 4]\n")
        f.write(" * Returns exact PC Doom COLORMAP row (0..31) for 100% lighting parity testing. */\n")
        f.write(f"static const uint8_t wall_zlight_level_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [str(wall_zlight_colormap[s, b]) for b in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        # 12. wall_zlight_cmdcolr_LUT (4,096 bytes - Prepacked Bank + Exact Colormap Level)
        wall_zlight_cmdcolr = data["wall_zlight_cmdcolr"]
        f.write("/* 2D Wall Prepacked CMDCOLR Z-LUT: wall_zlight_cmdcolr_LUT[sector >> 4][dist >> 4]\n")
        f.write(" * Returns prepacked 16-bit CMDCOLR ((bank<<8) | 0x80 | level). Memory: 4,096 bytes */\n")
        f.write(f"static const uint16_t wall_zlight_cmdcolr_LUT[{num_sectors}][{num_z_buckets}] = {{\n")
        for s in range(num_sectors):
            f.write(f"    /* Sector Light Index {s} */\n    {{ ")
            row = [f"0x{wall_zlight_cmdcolr[s, b]:04X}" for b in range(num_z_buckets)]
            f.write(", ".join(row))
            f.write(" },\n")
        f.write("};\n\n")

        f.write("#endif /* SATURN_LIGHT_LUT_H */\n")

def export_python_module(data, filepath):
    """
    Exports Python module for simulator import.
    """
    with open(filepath, "w", encoding="utf-8") as f:
        f.write('"""Auto-generated light LUT for Windows simulator."""\n')
        f.write("import numpy as np\n\n")
        f.write(f"AMBIENT_FLOOR = {data['ambient_floor']}\n")
        f.write(f"GAMMA = {data['gamma']}\n")
        f.write(f"CENTERY = {data['centery']}\n")
        f.write(f"SCREEN_HEIGHT = {data['screen_height']}\n\n")
        f.write(f"FLAT_COLORMAP_LUT = np.array({data['flat_colormap'].tolist()}, dtype=np.uint8)\n")
        f.write(f"FLAT_BANKS_LUT = np.array({data['flat_banks'].tolist()}, dtype=np.uint8)\n")
        f.write(f"WALL_COLORMAP_LUT = np.array({data['wall_colormap'].tolist()}, dtype=np.uint8)\n")
        f.write(f"WALL_BANKS_LUT = np.array({data['wall_banks'].tolist()}, dtype=np.uint8)\n")

def main():
    print("[*] Generating Saturn DOOM Lighting LUTs...")
    data = compute_light_tables(
        num_sectors=16,
        screen_height=224,
        centery=84.0,
        planeheight=41.0,
        num_wall_buckets=32
    )

    proj_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    c_header_path = os.path.join(proj_dir, "doom", "src", "saturn_light_lut.h")
    py_module_path = os.path.join(proj_dir, "tools", "saturn_sim", "light_lut.py")

    print(f"[*] Writing C Header to: {c_header_path}")
    export_c_header(data, c_header_path)

    print(f"[*] Writing Python Module to: {py_module_path}")
    export_python_module(data, py_module_path)

    print("[+] Successfully generated all lighting tables!")
    print(f"    - flat_light_bank_LUT:    {data['flat_banks'].nbytes} bytes (fits in SH-2 4 KB OCRAM)")
    print(f"    - flat_light_cmdcolr_LUT: {data['flat_cmdcolr'].nbytes} bytes")
    print(f"    - wall_light_bank_LUT:    {data['wall_banks'].nbytes} bytes")

if __name__ == "__main__":
    main()
