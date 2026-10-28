# Sega Saturn DOOM — Pure VDP1 Hardware Quad Renderer & Dual-SH-2 SMP Engine

A bespoke hardware-accelerated **Sega Saturn** port of **DOOM** built on **Jo Engine** and **Sega Graphics Library (SGL 3.02j)**.

Unlike the original 1997 Rage Software port of Saturn DOOM—which discarded VDP1 3D geometry acceleration and ran a slow CPU column/span software renderer bottled through a single VDP1 sprite upload—this engine drives **Sega Saturn's VDP1 32-bit 4-vertex distorted-sprite rasterizer**, **VDP2 background planes**, and **dual Hitachi SH7604 (Master + Slave SH-2) asymmetric multiprocessing (SMP)** directly.

---

## Key Architectural Highlights

1. **Pure 4-Vertex VDP1 Hardware Rasterization (`saturn_hw_render.c`)**
   - Every wall strip, upper/lower portal step, masked mid-texture, floor/ceiling convex polygon, and monster/weapon sprite is emitted directly as a native VDP1 4-vertex textured quadrilateral (`CMDPMOD`, `CMDCOLR`, `CMDSRCA`, `CMDSIZE`, `CMDXA..CMDYD`).
   - **Zero Triangles:** Saturn's VDP1 hardware lacks triangle primitives; offline bakers (`bake_flats.py`, `bake_walls.py`) decompose all subsectors into watertight convex quadrilaterals and 1D LOD wall strips.

2. **Asymmetric Dual-SH-2 SMP Pipeline (`saturn_hw_render.c`, `saturn_slave.c`)**
   - **Master SH-2 (`28.6 MHz`):** Traverses the BSP front-to-back, applies 3D portal PVS + O(1) subtree bitmask culling + 1D horizontal and 2D vertical portal aperture clipping, and emits visible wall/masked-mid/sprite quads.
   - **Slave SH-2 (`28.6 MHz`):** Runs concurrently from a dedicated lock-free queue in uncached WRAM (`0x26000000`), transforming and projecting visible subsector floor/ceiling quads (`HW_SlaveFlatEntry`) into a separated VDP1 command partition using hardware `DMULS.L` and pipelined `DIVU` (`0xFFFFFF00`).

3. **3D Portal Slope PVS & 2D Vertical Portal Aperture (`v15`)**
   - **Offline 3D Portal Slope PVS (`bake_pvs.py`):** Casts 2D rays across every two-sided portal linedef intercepted along `[0, 1]` and clips the vertical slope interval $[m_{\min}, m_{\max}]$ against `[openbottom, opentop]`, accompanied by precomputed per-BSP-node OR-unions of descendant subsector PVS bitmasks for $O(1)$ whole-subtree rejection.
   - **2D Vertical Portal Aperture (`ytop` / `ybot`):** Propagates screen-space vertical portal bounds (`sec_ymin` / `sec_ymax`) and exact wall-edge attachment bands (`sec_yt_attach` / `sec_yb_attach`) across two-sided portals so behind-the-door and upper-alcove subsectors outside the vertical portal window are rejected in $O(1)$ before vertex projection.

4. **Offline Episode 1 Asset Pipeline (`Projects/DOOM-Saturn/cd/`)**
   - Includes **Shareware `DOOM1.WAD`** (`E1M1` through `E1M9`) and all pre-baked binary level packages for the first 9 levels:
     - `<MAP>.FLT` — Watertight 4-vertex floor/ceiling quad meshes (`bake_flats.py`)
     - `<MAP>.WL` — Deduplicated 2D floorplan pillars & 16/32/64-unit 1D LOD wall segs (`bake_walls.py`)
     - `<MAP>.PVS` — 3D Portal Slope Potentially Visible Set bitmasks (`bake_pvs.py`)
     - `<MAP>.LEAF` — Convex subsector polygon leaf loops
     - `<MAP>.SEG` — Pre-subdivided wall segments (`subdivide_segs.py`)
     - `<MAP>.TEX` — Pre-composited wall texture packs (`bake_textures.py`)
     - `<MAP>.SPR` — Pre-unpacked row-major rectangular sprite packs (`bake_sprites.py`)
     - `<MAP>.PAK` — Consolidated 2048-byte sector-aligned level container (`bake_pak.py`)

5. **Dual-Target Hardware-Accurate Windows Verification Harness (`Projects/DOOM-Windows/`)**
   - Compiles the exact same `Projects/DOOM-Saturn/doom/src/saturn_hw_render.c` C source into a native Win32 executable (`doom_saturn_win.exe`) backed by a cycle/pixel-accurate VDP1 fixed-point forward-stepping quad emulator (`vdp1_emu.c`) and a side-by-side DOS DOOM software renderer oracle (`saturn_oracle.py`, `analyze_journey.py`).

---

## Repository Structure

```text
sega-saturn-doom-vdp1/
├── Compiler/COMMON/              # Sega Saturn SGL 3.02j headers, libraries, linker scripts & IP.BIN
├── jo_engine/                    # Modified Jo Engine Saturn framework (expanded GFS & VDP1 command support)
├── Projects/
│   ├── DOOM-Saturn/              # Sega Saturn SH-2 target & offline asset bakers
│   │   ├── Makefile              # SH-2 ELF / ISO build rules & WRAM BSS safety calibration
│   │   ├── compile.bat           # Builds game.elf & game.iso using SH-2 GCC + mkisofs
│   │   ├── doom/src/             # Hardware renderer (saturn_hw_render.c), VDP1/VDP2/SMP/PAK drivers
│   │   ├── doom/d32xr/           # PlaySim, thinker, physics, WAD & zone memory core
│   │   ├── tools/                # Offline Python asset bakers (bake_flats, bake_walls, bake_pvs, etc.)
│   │   └── cd/                   # DOOM1.WAD + pre-baked E1M1..E1M9 (.FLT/.WL/.PVS/.LEAF/.SEG/.TEX/.SPR/.PAK)
│   └── DOOM-Windows/             # Native Windows VDP1 hardware-accurate twin & automated journey harness
│       ├── build.bat             # MSVC x86 build script for doom_saturn_win.exe
│       ├── analyze_journey.py    # 624-frame automated visual & telemetry comparator vs DOS DOOM
│       ├── src/                  # Win32 host & pixel-accurate VDP1 rasterizer emulator (vdp1_emu.c)
│       └── dos_ref/              # Standalone DOS DOOM software renderer reference DLL
└── docs/
    ├── rendering_plan.md         # Master Architectural Blueprint & SH-2 / VDP1 Hardware Guide
    ├── version_changelog.md      # Version-by-version engineering & performance telemetry log (v1..v15)
    └── implementation_plan.md    # 3D Portal PVS & 2D Vertical Portal Aperture specification
```

---

## Building & Running

### 1. Windows Hardware-Accurate Verification Harness (`Projects/DOOM-Windows`)
Requires Visual Studio Build Tools (x86) and Python 3:
```bat
cd Projects\DOOM-Windows
call build.bat
doom_saturn_win.exe
```
To run the automated 624-frame E1M1 visual & performance comparison against the DOS DOOM software renderer:
```bat
python analyze_journey.py --map E1M1
```

### 2. Sega Saturn Disc Image (`Projects/DOOM-Saturn`)
Requires the Jo Engine `sh-elf-gcc` toolchain in `..\..\Compiler\WINDOWS\bin`:
```bat
cd Projects\DOOM-Saturn
call compile.bat
```
This produces `game.elf`, `game.map`, and bootable `game.iso` (verified `__bend < 0x060C0000` to preserve the SGL DMA sort list).

### 3. Re-baking Episode 1 (`E1M1`–`E1M9`) Level Assets from `DOOM1.WAD`
```bat
cd Projects\DOOM-Saturn
python tools/bake_flats.py
python tools/bake_walls.py
python tools/bake_pvs.py
python tools/subdivide_segs.py
python tools/bake_textures.py
python tools/bake_sprites.py
python tools/bake_pak.py
```
