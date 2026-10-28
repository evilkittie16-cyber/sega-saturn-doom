# DOOM Saturn Versioned Backup & Change Ledger

All code backups and linker maps are stored under [`backups/`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups).

---

## Summary Comparison Table

| Metric | `v0_20261021_baseline` | `v4_cmdbuf_fast_cull` | `v5_wall_icache_slim` | `v6_sub4k_all_functions` | `v7_bus_and_wall_slim` | `v8_pillar_uv_fix` | Total Delta (`v0` $\to$ `v8`) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Backup Directory** | [`v0`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline) | [`v4`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v4_cmdbuf_fast_cull) | [`v5`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v5_wall_icache_slim) | [`v6`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v6_sub4k_all_functions) | [`v7`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v7_bus_and_wall_slim) | [`v8`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v8_pillar_uv_fix) | — |
| **Saturn `__bend` (`game.map`)** | `0x060bef30` | `0x060ba890` | `0x060b91a0` | `0x060b89f0` | `0x060b88b0` | **`0x060b8870`** | **-26,304 B (-25.7 KB)** |
| **High WRAM Headroom** | `4,304 B` | `22,384 B` | `28,256 B` | `30,224 B` | `30,544 B` | **`30,608 B`** | **+26,304 B (+7.1x)** |
| **`saturn_hw_render.o` `.text`** | `59,584 B` (`0xe8c0`) | `55,264 B` (`0xd7e0`) | `49,368 B` (`0xc0d8`) | `47,396 B` (`0xb924`) | `46,840 B` (`0xb6f8`) | **`46,776 B` (`0xb6b8`)** | **-12,808 B (-21.5%)** |
| **`saturn_cmdbuf.o` `.text`** | `11,792 B` (`0x2e10`) | `1,828 B` (`0x0724`) | `1,828 B` (`0x0724`) | `1,828 B` (`0x0724`) | `1,828 B` (`0x0724`) | **`1,828 B` (`0x0724`)** | **-9,964 B (-84.5%)** |
| **Functions `> 4,096 B` (L1 Limit)** | `5` functions | `3` functions | `1` function | **`0` functions** | **`0` functions** | **`0` functions** | **100% fit in 4 KB L1!** |
| **`_HW_RenderSubsectorBakedWalls`** | `11,050 B` (`0x2b2a`) | `9,812 B` (`0x2654`) | `6,182 B` (`0x1826`) | `1,636 B` + `2,884 B` | `1,498 B` + `2,862 B` | **`1,512 B` + `2,932 B`** | **Split cull (`1.5 KB`) + emit (`2.9 KB`)** |
| **`_HW_EmitWallStrips` Size** | `7,644 B` (`0x1ddc`) | `2,440 B` + `4,310 B` | `2,348 B` + `2,972 B` | `2,070 B` + `2,704 B` | `2,018 B` + `2,616 B` | **`2,080 B` + `2,604 B`** | **Both < 2.65 KB (`0` stack args)** |
| **`_HW_Worker_ProcessJob` Size** | `4,912 B` (`0x1330`) | `3,364 B` (`0x0d24`) | `3,188 B` (`0x0c74`) | `3,188 B` (`0x0c74`) | `2,890 B` (`0x0b4a`) | **`2,948 B` (`0x0b84`)** | **-1,964 B (`132 B` stk, `0` stack args)** |
| **`_HW_EmitFlatBands` / `_Multi`** | `1,460 B` / `2,158 B` | `1,460 B` / `2,158 B` | `1,460 B` / `2,158 B` | `1,460 B` / `2,158 B` | `1,094 B` / `1,858 B` | **`1,104 B` / `1,900 B`** | **-614 B (`0` stack args, `-432 B` stk)** |
| **Win32 Journey Avg Error** | `4.547%` | `4.603%` | `4.601%` | `4.597%` | `4.603%` | **`4.537%`** | **`-0.010%` (improved accuracy!)** |
| **Win32 Avg Wall / Flat / Total Quads** | `104.3 / 92.1 / 196.9` | `91.8 / 108.9 / 201.0` | `91.8 / 108.9 / 201.0` | `91.8 / 108.9 / 201.0` | `105.0 / 108.8 / 214.1` | **`97.7 / 108.8 / 206.9`** | **`-7.3` wall quads vs `v7`** |
| **VDP1 DDA @ `X754 Y-3097 A351`** | — | — | — | — | `311,720` (`357` slots, `vbl_grace=480`) | **`249,264` (`348` slots, `vbl_grace=600`)** | **`-62,456` DDA (-20.0%), zero VBlank cutoff!** |
| **`sdbg` Spawn (`X1056 Y-3616 A90`)** | `15.0 FPS` (`R55 B35 G20 Q193`) | `24.0 FPS` (`R32 B31 G19 Q198`) | `29.0 FPS` (`R30 B29 G18 Q198`) | `29.0 FPS` (`R30 B29 G17 Q198`) | `30.0 FPS` (`R27 B26 G15 Q198`) | **`30.0 FPS` (`R27 B26 G15 Q204`)** | **Locked 30.0 FPS (+100% FPS, `R` -28 ms / -50.9%)** |
| **Real Saturn HW Spawn (`X1056 Y-3616 A90`)** | `14.7 FPS` (`R33 V18 Q250` cutoff) | `30.0 FPS` (`R29 V3 B28 G17 S0 J0 Q198 D0`) | `30.0 FPS` (`R28 V4 B26 G16 S0 J1 Q198 D0`) | — | `30.0 FPS` (`R26 V3 B25 G15 S0 J0 W88 F108 Q197 D0`) | *Ready for HW test* | **Locked 30.0 FPS (+100% FPS)** |
| **`sdbg` Courtyard (`X784 Y-3197 A354`)** | `8.6 FPS` (`R99 B55 G36 Q411`) | `17.7 FPS` (`R44 B43 G27 Q394`) | `20.0 FPS` (`R41 B41 G25 Q394`) | `19.5 FPS` (`R41 B41 G24 Q394`) | `20.0 FPS` (`R35 B35 G20 Q394`) | **`20.0 FPS` (`R35 B35 G20 Q394`)** | **+132.6% FPS, `R` -64 ms (-64.6%), `G` -16 ms (-44.4%)** |
| **`sdbg` Courtyard HW Angle (`X851 Y-3302 A4`, 55 subs / 377 segs)** | — | — | — | `17.6 FPS` (`R45 B44 G27 Q387`) | `19.5 FPS` (`B40 G25 S2 J0 Q387 D0`) | **`19.5 FPS` (`B40 G25 S2 J0 Q387 D0`)** | **`+1.9 FPS`, `B` -4 ms, `G` -2 ms vs `v6`** |
| **Real Saturn HW Courtyard (`X754 Y-3097 A351`)** | — | — | — | — | `20.8 FPS` (`R33 V7 B32 G18 S6 J0 W109 F232 Q342 D0`) | *Ready for HW test* | **Fixed 50% ghosting on fg pillar & pistol + 32u pillar UVs** |

---

## Real Sega Saturn Hardware & Cold-Booted `sdbg` Telemetry (`v0` $\to$ `v7`)

````carousel
<!-- slide -->
<!-- slide -->
<!-- slide -->
<!-- slide -->
<!-- slide -->
<!-- slide -->
<!-- slide -->
````

---

## Version Details

### `v0_20261021_baseline`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/saturn_cmdbuf.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/saturn_vdp1.c), [`i_video.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/i_video.c), [`d_main.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/d_main.c), [`makefile`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/makefile), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v0_20261021_baseline/game.map)
- **State:** Contained Fix 1 (`CCR = 0x11u` full 4 KB L1 cache), plus the counterproductive Fix 3a (`!is_sky_ceil` on `SEG_FLAG_SOLID`, which broke 1-sided wall occlusion in outdoor/sky sectors) and Fix 4 (disabling narrow-wall coalescing `dx <= 16` / `dx <= 36`, which inflated wall quad counts by +28%).

---

### `v1_revert_audit_regressions`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v1_revert_audit_regressions/saturn_hw_render.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v1_revert_audit_regressions/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Reverted Fix 3a (`SEG_FLAG_SOLID` 1-sided wall occlusion):** Restored unconditional `if (seg->flags & SEG_FLAG_SOLID) is_solid = 1;` so 1-sided solid walls in sky-ceiling sectors properly mark `hw_solid_mask` and cull hidden BSP subtrees behind them.
  2. **Reverted Fix 4 (narrow-wall monolithic & coalesced fast paths):** Restored `coal_dx <= 16` and `coal_dx <= 36` in `HW_EmitWallStrips` and `dx <= 16` and `dx <= 36` in `HW_RenderSubsectorBakedWalls` so narrow distant walls (`!persp_strips`) emit a single quad instead of 4–8 tiny strips.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261021_225651.csv`):**
  - Average Wall Quads dropped from `104.3` $\to$ **`81.2`** (**-22.1%**).
  - Average Subsectors Visited dropped from `45.1` $\to$ **`38.7`** (**-14.2%**).

---

### `v2_icache_and_flatclip_opt`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v2_icache_and_flatclip_opt/saturn_hw_render.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v2_icache_and_flatclip_opt/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **De-inlined cold clipping helpers (`HW_ClipCoord`, `HW_AddClipTri`, `HW_AddClipTriSwap`):** Changed from `HW_ALWAYS_INLINE` to `static HW_NOINLINE HW_COLD_OS`, shrinking `_HW_ClipQuadPlane` from **7,242 bytes to 1,224 bytes (-6.0 KB)**.
  2. **De-inlined `HW_FlatSizeLOD` and `HW_Worker_EmitFlatQuadDirect`:** Stopped 8x inlining across Slave SH-2 flat functions, shrinking `_HW_Worker_EmitPolyQuads` (`2,744 B` $\to$ `1,184 B`).
  3. **Widened `HW_ClipFlatQuadOffscreen` guard band (`[-8, vw+8] × [-4, vh+4]` $\to$ `[-48, vw+48] × [-24, vh+24]`):** Eliminated unnecessary 4-plane software Sutherland-Hodgman `DIVU` clipping on ordinary screen-border flat quads.
  4. **Extracted cold `HW_ExtrapolatePartialWallStrip` and `HW_EmitMultiBandBakedWall`:** Shrank `_HW_EmitWallStrips` by `648 B` and `_HW_RenderSubsectorBakedWalls` by `1,238 B`.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261021_230506.csv` & cold-booted `sdbg`):**
  - Saturn `.text` footprint (`__bend`) shrank by **7,488 bytes** (`0x060bf070` $\to$ `0x060bd330`).
  - Cold-booted `sdbg` at Courtyard Window (`X784 Y-3197 A354`) improved from `8.6 FPS (R99 B55 G36 Q411)` to **`10.4 FPS (R79 B42 G27 Q332)`**.

---

### `v3_icache_split_and_worker_slim`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v3_icache_split_and_worker_slim/saturn_hw_render.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v3_icache_split_and_worker_slim/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Extracted `HW_Worker_ClipNearQuad` from `HW_Worker_ProcessJob`:** Moved the cold near-plane ($z < \text{HW\_NEAR\_Z}$) polygon clipping block out of the Slave worker hot loop, shrinking `_HW_Worker_ProcessJob` from `4,912 B` to **`3,364 B`** so it fits inside the 4 KB Slave SH-2 L1 cache.
  2. **Split `HW_EmitWallStrips` (`7,036 B`) into `_HW_EmitWallStrips_Persp` (`2,440 B`) and `_HW_EmitWallStrips_Linear` (`4,310 B`) + `HW_ModNonPow2`:** Separated the close-up perspective strip emitter from the medium/far linear strip emitter so neither thrashes the 4 KB L1 instruction cache.
  3. **De-inlined `HW_FastFlatSrca` and `HW_FastWallSrca`:** Reduced `__bend` to `0x060bd0b0`.
  4. **Instrumented `saturn_hw_prof_flats` around `Saturn_CmdBuf_CullOccluded()`:** Uncovered that `Saturn_CmdBuf_CullOccluded()` in [`saturn_cmdbuf.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_cmdbuf.c) was burning **`21.0 ms` at Spawn** and **`36.03 ms` (`32,359` FRT ticks = 45% of total frame CPU time!) at Courtyard Window** executing a 1,300-line software polygon scanline rasterizer (`cmdbuf_mark_sub_flats`, `cmdbuf_mark_flat_tilted_ex`, `cmdbuf_mark_ext_edges`, `cmdbuf_fill_wedge`, `cmdbuf_mark_sloped_wall`) on the Master SH-2 CPU.

---

### `v4_cmdbuf_fast_cull`
- **Files Backed Up:**
  - [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v4_cmdbuf_fast_cull/saturn_cmdbuf.c), [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v4_cmdbuf_fast_cull/saturn_hw_render.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v4_cmdbuf_fast_cull/game.map)
- **Changes Made in [`saturn_cmdbuf.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_cmdbuf.c) and [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Eliminated the 1,300-line software polygon scanline rasterizer in [`Saturn_CmdBuf_CullOccluded()`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_cmdbuf.c#L325-L355):** Removed `cmdbuf_mark_sub_flats`, `cmdbuf_mark_flat_tilted_ex`, `cmdbuf_mark_ext_edges`, `cmdbuf_mark_flat_quad`, `cmdbuf_fill_wedge`, `cmdbuf_mark_flat_sheared`, `cmdbuf_mark_sloped_wall`, `cmdbuf_cull_sub_walls`, and `cmdbuf_cull_sub_flats`. During the BSP walk, `saturn_hw_render.c` already performs 1D solid-wall occlusion (`hw_solid_mask`), per-column vertical aperture occlusion (`saturn_occl_ceil`/`saturn_occl_floor`), and off-screen flat/wall clipping; re-rasterizing all emitted quads in software on the Master SH-2 CPU burned **21–36 ms of CPU time** to save `< 0.5 ms` of VDP1 hardware time.
  2. **Retained Warm-L1 Off-Screen Wall Y-Trim (`cmdbuf_trim_wall_y`) + 12-Cycle Uncached Trampoline L1 Flash Purge (`HW_PURGE_L1_CACHE()`):** `Saturn_CmdBuf_CullOccluded()` now scans `saturn_cmd_m` while it is still warm in the Master's L1 cache to trim extreme off-screen vertical wall coordinates (`yLt < -6 && yRt < -6` or `yLb > vh + 5 && yRb > vh + 5`), and then calls `_hw_purge_cache` via the uncached Area 1 alias (`0x26000000`) to flash-clear the Master's write-through L1 cache in 12 cycles (replacing 4,200+ associative-purge loop iterations).
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261021_235424.csv` & cold-booted `sdbg`):**
  - `saturn_cmdbuf.o` `.text` shrank by **-84.5% (`-9,964` bytes)** from `11,792 B` to **`1,828 B`**.
  - High WRAM `__bend` dropped to **`0x060ba890`** (`22,384` bytes of free headroom below `0x060C0000`, **-18,080 bytes** smaller than `v0`).
  - Win32 E1M1 Journey full-level average error improved to **`4.603%`** (`32.22%` bit-exact pixels).
  - **Cold-booted `sdbg` E1M1 Spawn (`X1056 Y-3616 A90`):**
    - Jumped from **`15.0 FPS` (`R55` in `v0`, `R52` in `v3`)** to **`24.0 FPS` (`R32 V3 T2 C0 B31 G19 S5 J0 W89 F108 Q198 D0`)** (**+60.0% FPS, `-23 ms` CPU render time**).
  - **Cold-booted `sdbg` E1M1 Courtyard Window (`X784 Y-3197 A354`):**
    - Jumped from **`8.6 FPS` (`R99` in `v0`) / `10.4 FPS` (`R80` in `v3`)** to **`17.7 FPS` (`R44 V4 T3 C0 B43 G27 S4 J0 W153 F240 Q394 D0`)** (**+105.8% FPS vs `v0`, `-55 ms` CPU render time, zero dropped quads `D0`**).

---

### `v5_wall_icache_slim`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v5_wall_icache_slim/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v5_wall_icache_slim/saturn_cmdbuf.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v5_wall_icache_slim/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Eliminated 20–33 Stack Arguments (`hw_wall_emit_ctx_t`):** Replaced the 24-parameter `HW_EmitWallStrips_Linear`, 21-parameter `HW_EmitWallStrips_Persp`, and 37-parameter `HW_EmitMultiBandBakedWall` stack-spill signatures with a compact `hw_wall_emit_ctx_t` context pointer (`r4..r7` only, **zero outgoing stack arguments**). On SH-2 (`mov.l @(disp, r15)` only reaches `0..60` bytes), removing the 132-byte outgoing stack argument frame allowed all local variables in `HW_RenderSubsectorBakedWalls` to use single-instruction `@(disp, r15)` accesses without clobbering `r0`.
  2. **Unified Coalesced & Single-Strip Quad Emission Tail in `HW_EmitWallStrips_Linear`:** Merged the duplicate visibility, occlusion, clamping, and VDP1 8-word emission blocks inside the strip loop, shrinking `_HW_EmitWallStrips_Linear` by **-31.0% (`4,310 B` $\to$ `2,972 B`, now well under the 4 KB L1 cache)**.
  3. **Extracted `HW_TransformSubsectorPillar`, `HW_ExtrapolateMonoWallX`, and `HW_ExtrapolateMonoWallY`:** Deduplicated the two 30-line pillar projection blocks (freeing 5 loop-invariant registers `cos16, sin16, cz_orig, cx_orig, view_cx` across `HW_RenderSubsectorBakedWalls`) and moved the cold `tw <= 16` partial-texel UV extrapolation out-of-line (`HW_COLD_OS`), shrinking `_HW_RenderSubsectorBakedWalls` by **-37.0% (`9,812 B` $\to$ `6,182 B`)**.
  4. **Bitmask-Closed `HW_ANY_OUT1024` & `HW_WALL_YCLAMP` (`0x3FFu` / `0x7FFu`):** Moved the 4 `HW_ClampY` calls into `HW_WallYClamp_Slow` and used bitwise-OR closure on `1023u` (`0x3FFu`) and `2047u` (`0x7FFu`) to reduce 10 comparisons + 6 branches per wall quad down to **2 comparisons + 1 branch**.
  5. **De-inlined `HW_IsSpanOccluded`, `HW_CheckSpanOcclusion`, `HW_MarkSubSolidSpan`, `HW_GetVertex`, and `HW_EmitWallQuadDirect` & removed `#pragma GCC unroll 16` in `HW_RenderSubsector`:** Shrank `_HW_RenderSeg` (`5,068 B` $\to$ `3,352 B`) and `_HW_RenderSubsector` (`2,144 B` $\to$ `1,680 B`).
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_021143.csv` & cold-booted `sdbg`):**
  - High WRAM `__bend` dropped to **`0x060b91a0`** (**28,256 bytes** of free High WRAM headroom, **-23,952 bytes** vs `v0` and **-5,872 bytes** vs `v4`).
  - Win32 E1M1 Journey: **`4.601%`** avg error (`32.22%` bit-exact pixels), `91.8 W / 108.9 F / 201.0 Tot` avg quads.
  - **Cold-booted `sdbg` E1M1 Spawn (`X1056 Y-3616 A90`):**
    - `saturn_prof_render_frt = 27,119` (**`30.19 ms`**, `B29 G18 S0 J0 W89 F108 Q198 D0`), **`29.0 FPS`** in `sdbg` (down `-2 ms` from `v4`'s `R32 B31 G19`).
  - **Cold-booted `sdbg` E1M1 Courtyard Window (`X784 Y-3197 A354`):**
    - Jumped from **`17.7 FPS` (`R44 B43 G27` in `v4`)** to **`20.0 FPS` (`R41 V6 T2 C0 B41 G25 S0 J0 W153 F240 Q394 D0`)** (**+132.6% FPS vs `v0`, `-58 ms` CPU render time**).
  - **Real Sega Saturn Hardware Verification ([`real_hw_v5_spawn_30fps.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/real_hw_v5_spawn_30fps.jpg), [`real_hw_v5_courtyard_17_6fps.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/real_hw_v5_courtyard_17_6fps.jpg)):**
    - Spawn (`X1056 Y-3616 A90`): **`30.0 FPS` (`R28 V4 T2 C0 B26 G16 S0 J1 W89 F108 Q198 D0`)**.
    - Courtyard (`X851 Y-3302 A4`, 55 subsectors / 377 segs): **`17.6 FPS` (`R43 V6 T2 C0 B42 G27 S5 J0 W175 F213 Q389 D0`)**.

---

### `v6_sub4k_all_functions`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v6_sub4k_all_functions/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v6_sub4k_all_functions/saturn_cmdbuf.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v6_sub4k_all_functions/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Split `HW_RenderSubsectorBakedWalls` (`6,182 B` in `v5`, `376-byte` stack frame) at the Culling/Emission Boundary (`100%` of Functions `< 4 KB` L1 Cache):**
     - `_HW_RenderSubsectorBakedWalls` (**`1,636 B`**, `108-byte` stack frame): fast culling and pillar projection loop over all `377` segs (rejecting `71.4%` in registers without touching the emission code).
     - `_HW_EmitVisibleBakedSeg.constprop.0` (**`2,884 B`**, `120-byte` stack frame, `4` register args `r4..r7` = `0` stack args): called only for the `28.6%` (`108 / 377`) surviving visible segs.
  2. **Unified `HW_WallXYClamp_Slow(int c[6])` and Reduced `HW_ClipWallNearPlane`, `HW_ExtrapolateMonoWallX`, and `HW_ExtrapolateMonoWallY` to `<= 3` Register Arguments (`0` Stack Arguments):**
     - Eliminated all outgoing stack argument slots (`@(0..20, r15)`) in both `HW_RenderSubsectorBakedWalls` and `HW_EmitVisibleBakedSeg`.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_024642.csv` & cold-booted `sdbg` [`sdbg_hud_v6.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_hud_v6.png)):**
  - **Functions `> 4,096 B` in `saturn_hw_render.o`:** **`0` (ZERO out of 53 functions!)**
  - `saturn_hw_render.o` `.text` shrank by **`-1,972 bytes`** vs `v5` (`49,368 B` $\to$ **`47,396 B`**, **`-12,188 B` vs `v0`**).
  - High WRAM `__bend` dropped to **`0x060b89f0`** (**`30,224 bytes` (`29.5 KB`)** of free High WRAM headroom, **`+25,920 B` vs `v0`**).
  - Win32 E1M1 Journey: **`4.597%`** avg error (`1.644%` best, `32.22%` bit-exact pixels, `0` visual regressions).
  - **Cold-booted `sdbg` E1M1 Spawn (`X1056 Y-3616 A90`):**
    - **`29.0 FPS` (`R30 V1 T2 C0 B29 G17 S1 J1 W89 F108 Q198 D0`)** (`G` improved `-1 ms` from `18 ms` to `17 ms`).
  - **Cold-booted `sdbg` E1M1 Courtyard Window (`X784 Y-3197 A354`):**
    - **`19.5 FPS` (`R41 V6 T2 C0 B41 G24 S1 J0 W153 F240 Q394 D0`)** (`G` improved `-1 ms` from `25 ms` to `24 ms`).
  - **Cold-booted `sdbg` E1M1 Real-HW Courtyard Angle (`X851 Y-3302 A4`, 55 subs / 377 segs):**
    - **`17.6 FPS` (`R45 V3 T2 C0 B44 G27 S1 J1 W175 F211 Q387 D0`)**.

---

### `v7_bus_and_wall_slim`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v7_bus_and_wall_slim/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v7_bus_and_wall_slim/saturn_cmdbuf.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v7_bus_and_wall_slim/game.map)
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Eliminated 45+ Write-Through Stack-Argument & Register-Push External Bus Writes per Flat Quad on the Slave SH-2 (`hw_flat_emit_ctx_t` + 3-Arg `HW_FlatSizeLOD` + `HW_EmitFlatQuad_Clipped`):**
     - Replaced 20-parameter `HW_EmitFlatBands` (`16` stack args = `64 B`) and `HW_EmitFlatBands_Multi` (`16` stack args = `64 B`) with 4-register-arg `(fctx, q_ptr, cw, qpack)` (`r4..r7` only, **`0` stack arguments**) backed by per-CPU `hw_flat_emit_ctx_t` (`hw_fctx_master` / `hw_fctx_slave`, populated once per subsector). Because SH-2 L1 cache is write-through, eliminating `16` stack argument stores + `6` `HW_FlatSizeLOD` stack argument stores + `7` `HW_FlatSizeLOD` callee-saved register pushes per flat quad removed **`~9,000` external 32-bit bus writes per frame** at Courtyard (`240` flat quads), freeing the shared external bus for the Master CPU during `B`/`G`.
     - Extracted the cold offscreen-clipping + `ntris` triangle emission tail from both `HW_EmitFlatBands` and `HW_EmitFlatBands_Multi` into shared 4-register-arg `HW_EmitFlatQuad_Clipped(fctx, local_q, w1, size_and_ceil)` (`0` stack args), removing `144 B` (`int tx[6][3], ty[6][3]`) of stack frame and shrinking `HW_EmitFlatBands` by **`-366 B` (`1,460 B` $\to$ `1,094 B`, stack frame `324 B` $\to$ `44 B`)** and `HW_EmitFlatBands_Multi` by **`-300 B` (`2,158 B` $\to$ `1,858 B`, stack frame `324 B` $\to$ `172 B`)**.
     - Reduced `HW_Worker_ClipNearQuad(fctx, q_ptr, local_q)` (`3` register args = `0` stack args) and `HW_TransformFlatVert_Shifted(dx, dy, fctx, p_ptr)` (`4` register args = `0` stack args), shrinking `HW_Worker_ProcessJob` by **`-298 B` (`3,188 B` $\to$ `2,890 B`)** and cutting its stack frame from **`240 B` to `132 B` (`-108 B`)**.
     - Reduced `HW_FlatSizeLOD(base_srca, quad_size, dy)` to **3 register arguments (`0` stack args, `0` callee-saved register pushes, `394 B` $\to$ `180 B`)** with inline `HW_FlatQuadDy` skipping the 8 vertex-equality checks whenever `dy <= 14`.
     - Reordered `HW_Worker_ProcessJob` Phase 2 so vertical screen bounds (`(0, vh)`) and backface/winding (`cw_c`, `cw_f`) cull before reading `verts[qv0..qv3].y` or computing `quad_v0 / quad_rows`.
  2. **Eliminated 15 Stack Arguments + 9 Address-Taken Locals in `HW_EmitWallStrips_Linear` and `HW_EmitWallStrips_Persp` (`HW_ExtrapolatePartialWallStrip` + `hw_wall_coord_tmp[9]`):**
     - Replaced the 19-parameter `HW_ExtrapolatePartialWallStrip` (`15` stack args = `60 B` + `9` address-taken local pointers `&s_q_xa..&lc_x1` = `36 B`) with 4-register-arg `HW_ExtrapolatePartialWallStrip(s, u_lo, u_hi, hw_wall_coord_tmp)` (`0` stack arguments) and file-scope `static int hw_wall_coord_tmp[9]` across `HW_WALL_XYCLAMP`, `HW_ExtrapolatePartialWallStrip`, and `HW_ExtrapolateMonoWallY` (all 100% single-threaded on Master CPU).
  3. **Fast 16x16$\to$32 `muls.w` Backface Cull (`HW_CrossBackface`) + Zero-Stack-Frame `HW_TransformSubsectorPillar` + `HW_LoadWallTexCtx`:**
     - Added `HW_CrossBackface` in `HW_RenderSubsectorBakedWalls` (using 2-register `muls.w` when `|obj_dx|, |obj_dy|, |seg_dx|, |seg_dy| <= 32767`, where $|a \cdot b - c \cdot d| \le 2 \times 32767^2 < \text{INT32\_MAX}$ cannot overflow 32-bit signed arithmetic), wrote `wctx.top_world_z..orig_bot_z` directly during height culling, and extracted cold `HW_TransformPillar_Shifted` out of `HW_TransformSubsectorPillar` (`0 B` stack frame, `4` pushes), shrinking `HW_RenderSubsectorBakedWalls` from `1,624 B` (`108 B` stack) to **`1,498 B` (`68 B` stack)**.
  4. **Streamlined Subsector Dispatch & Slave Job Submission (`HW_SelectFlatLOD`, `HW_SubmitSlaveFlatJob`, `HW_RenderSubsector`):**
     - Moved the LOD1 distance check into `HW_SelectFlatLOD` (`HW_MulsW_PairAdd(...) >= (180 << 12)`, eliminating `___ashrsi3_r0`, `sbox[4]`, and the 5th stack argument `cz`), added per-frame `hw_last_flat_sec` caching in `HW_SubmitSlaveFlatJob`, unrolled `jw[11..21]` solid mask copy, and shrank `HW_RenderSubsector` from `1,654 B` (`68 B` stack) to **`1,162 B` (`48 B` stack, `-492 B`)**.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_035135.csv` & cold-booted `sdbg` [`sdbg_hud_v7.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_hud_v7.png)):**
  - **Functions `> 4,096 B` in `saturn_hw_render.o`:** **`0` (ZERO out of 58 functions!)**
  - `saturn_hw_render.o` `.text` shrank to **`46,840 B` (`0xb6f8`)** (**`-556 B` vs `v6`, `-12,744 B` (-21.4%) vs `v0`**).
  - High WRAM `__bend` dropped to **`0x060b88b0`** (**`30,544 bytes` (`29.8 KB`)** of free High WRAM headroom, **`+26,240 B` vs `v0`**).
  - Win32 E1M1 Journey: **`4.603%`** avg error (`1.644%` best, `32.22%` bit-exact pixels, `0` visual regressions).
  - **Cold-booted `sdbg` E1M1 Spawn (`X1056 Y-3616 A90`):**
    - **`30.0 FPS` (`R27 V3 T2 C0 B26 G15 S1 J0 W89 F108 Q198 D0`)** (**`R` improved `-3 ms` (`30` $\to$ `27 ms`), `B` improved `-3 ms` (`29` $\to$ `26 ms`), `G` improved `-2 ms` (`17` $\to$ `15 ms`), hitting locked `30.0 FPS` in `sdbg`!**)
  - **Cold-booted `sdbg` E1M1 Courtyard Window (`X784 Y-3197 A354`):**
    - **`20.0 FPS` (`R35 V12 T2 C0 B35 G20 S0 J0 W153 F240 Q394 D0`)** (**`R` improved `-6 ms` (`41` $\to$ `35 ms`, -14.6%), `B` improved `-6 ms` (`41` $\to$ `35 ms`), `G` improved `-4 ms` (`24` $\to$ `20 ms`, -16.7%)!**)
  - **Cold-booted `sdbg` E1M1 Real-HW Courtyard Angle (`X851 Y-3302 A3`, 55 subs / 377 segs):**
    - **`19.5 FPS` (`B40 G25 S2 J0 W175 F211 Q387 D0`)** (**`+1.9 FPS` (`17.6` $\to$ `19.5 FPS`), `B` improved `-4 ms` (`44` $\to$ `40 ms`), `G` improved `-2 ms` (`27` $\to$ `25 ms`)!**)

---

### `v8_pillar_uv_fix`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v8_pillar_uv_fix/saturn_hw_render.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v8_pillar_uv_fix/saturn_vdp1.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v8_pillar_uv_fix/game.map)
- **Changes Made in [`saturn_vdp1.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_vdp1.c) & [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Eliminated 50% Ghosting / Alternating-Field Cutoff on Foreground Brown Pillar (`Slots [354, 355]`) & Pistol (`Slot [356]`) at `X754 Y-3097 A351`:**
     - **Root Cause:** In real hardware photo [`media_1792639613421_a7331f48.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792639613421_a7331f48.jpg), the scene emitted `357` VDP1 slots totaling `311,720` DDA steps (`16.15 ms`, finishing at Scanline 253 inside VBlank). On a 2-field frame (`R33`), VDP1 finished `100%` of `FB1` during Field 1 (while the CPU was inside `Saturn_HW_RenderPlayerView`), then `PTMR = 0x0002` auto-restarted VDP1 into `FB0` at Scanline 262. In Field 2, `Saturn_Vdp1Wall_Frame()` waited inside VBlank 2 for `vbl_grace = 480u` iterations, which expired at Scanline 249 (`15.81 ms`) while VDP1 was at Slot `[354]`, overwriting VDP1 VRAM and aborting `Slots [354..356]` in `FB0`. Alternating `FB1` (complete) and `FB0` (missing `[354..356]`) at 60Hz made the rightmost strip of the foreground brown pillar (`x = 142..153`) and the pistol (`Slot [356]`) 50% transparent, letting the courtyard window behind `x = 142..153` shine through the right quarter of the pillar.
     - **Fix A ([`saturn_vdp1.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_vdp1.c#L1860)):** Increased `vbl_grace` in `Saturn_Vdp1Wall_Frame()` from `480u` (Scanline 249) to `600u` (Scanline 255, `~2.01 ms` into VBlank), giving VDP1 `+0.40 ms` more rasterization budget inside VBlank while leaving `> 0.45 ms` before Scanline 262 (VBlank-OUT).
     - **Fix B ([`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c#L5946-L6079)):** Cut total VDP1 DDA steps at `X754 Y-3097 A351` by **`-20.0%` (`311,720` $\to$ `249,264` DDA steps, `-62,456` DDA steps)** by capping ultra-distant (`cz > 800`) tall multi-band walls visible through windows to a single vertical band (`rem_h = max_first_h`) and using `64`-texel monolithic width on distant (`cz > 520`, `dx <= 48`) `128`-wide walls. At `249,264` DDA steps, VDP1 finishes before Scanline 205 (`< 13.0 ms`), well before VBlank (Scanline 224) even begins!
  2. **Fixed 32-Unit Pillar (`u_span == 32`) & Adjacent `STARTAN3` Wall (`sidx=461`, `dx = 26`) UV Stretching (`HW_EmitVisibleBakedSeg` & `HW_EmitWallStrips_Linear`):**
     - Restricted the `mono_tw = 16` narrow-wall rule from unconditional `dx <= 16` to `(u_span >= 64 ? dx <= 16 : dx <= 6)` (and `coal_strips >= 4 && s_px1 - s_px0 <= 16` in `HW_EmitWallStrips_Linear`), so `32`-unit pillar faces (`u_span = 32, dx = 10..12`, such as `sidx=440` at `(754,-3097)` and the 4 pillars at E1M1 spawn) always render both 16-texel strips (`u = 0..15` and `u = 16..31`) instead of stretching the left half `u = 0..15` across the entire 32-unit pillar face.
     - Restricted the `mono_tw = 64` rule on `128`-wide monolithic walls from `dx <= 36` to `dx <= 24 || (dx <= 48 && p0_use->cz > (520 << FRACBITS) && p1_use->cz > (520 << FRACBITS))`, restoring the full `128`-texel `STARTAN3` texture on `sidx=461` (`dx = 26, cz1 = 504`) directly behind the second brown pillar.
  3. **Eliminated 3-Scanline Multi-Band Vertical Seam Overlap (`HW_EmitMultiBandBakedWall`):**
     - Set `bot_overlap = 0` on non-final vertical bands (`rem_h > band_h`) and `top_overlap = 0` (`ya0 = ya1; yb0 = yb1;`) on subsequent bands, eliminating the 3-scanline vertical wrap seam overlap on tall (`wall_h = 216 > 128`) pillars.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_070726.csv`):**
  - **Functions `> 4,096 B` in `saturn_hw_render.o`:** **`0` (ZERO functions `> 4 KB`)**
  - `saturn_hw_render.o` `.text` shrank to **`46,776 B` (`0xb6b8`)** (**`-64 B` vs `v7`, `-12,808 B` (-21.5%) vs `v0`**).
  - High WRAM `__bend` dropped to **`0x060b8870`** (**`30,608 bytes` (`29.9 KB`)** of free High WRAM headroom, **`+26,304 B` vs `v0`**).
  - Win32 E1M1 Journey: **`4.537%`** full-level avg error (**improved `-0.066%` vs `v7`'s `4.603%`**; Spawn Room error improved from `3.998%` to **`3.849%`**), **`97.7 W / 108.8 F / 206.9 Tot`** avg quads (`-7.3` wall quads/frame vs `v7`), **`60.0 FPS`**.

---

### `v9_vdp1_dda_budget_fix`
- **Files Backed Up:**
  - [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v9_vdp1_dda_budget_fix/saturn_hw_render.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v9_vdp1_dda_budget_fix/saturn_vdp1.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v9_vdp1_dda_budget_fix/saturn_cmdbuf.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v9_vdp1_dda_budget_fix/game.map)
- **Root Cause Diagnosed (`X743 Y-3215 A3` in [`media_1792650934987_fa268ddd.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792650934987_fa268ddd.jpg)):**
  - At `X743 Y-3215 A3`, the player stands inside **Sector 40 (`VISIT 0`)**, which has a low ceiling overhang (`z = 72`, projecting down to `y = 53` across `x = 0..57`). The West face of the left foreground pillar (`x = 27..57`, emitted in `VISIT 3`) spans `z = 0..72` (`y = 53..127`), while the ceiling overhang directly above it (`x = 0..57, y = 0..53`), the foreground floor (`y = 127..168`), and the pistol sprite are all emitted last in **`VISIT 0` (`Slots [366..388]`)**.
  - In `v8`, `VISIT 47` down to `VISIT 1` consumed **`348,533` VDP1 DDA steps** (`16.67 ms`, 100% of 1 NTSC field = Scanline 262), and `VISIT 0` required an additional **`106,366` DDA steps** (`total_dda = 454,899`). On real Sega Saturn hardware, VDP1's hardware `FRAME_CHANGE` at Scanline 262 hard-aborted `VISIT 0` right after `VISIT 1` drew the North face of the pillar (`x = 57..76, y = 4..135`), leaving `x = 27..57, y = 4..53` unpainted (making the left pillar look L-shaped / missing its top-left half) and dropping the foreground floor and pistol. Kronos does not enforce VDP1's per-field cycle limit, so the cutoff only appeared on real Saturn hardware.
- **Changes Made in [`saturn_vdp1.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_vdp1.c), [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c), & [`saturn_cmdbuf.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_cmdbuf.c):**
  1. **True 2:1 Horizontal (`32xth`) and 2:1 Horizontal+Vertical (`32x(th/2)`) 64-Unit Block Mip Pyramid (`v1_copy_32_blocks` in [`saturn_vdp1.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_vdp1.c#L892-L938)):**
     - For all wall textures with `tw >= 64 && (tw & 63) == 0`, populated `tag->srca_mono` with contiguous `32 x th` horizontally downsampled 64-unit blocks followed by `32 x (th >> 1)` horizontally-and-vertically downsampled 64-unit blocks.
     - In `HW_EmitVisibleBakedSeg` and `HW_EmitWallStrips_Linear`, 64-unit wall blocks with `dx <= 36` use the `32 x th` block (or `32 x (th >> 1)` when on-screen height `max_dy <= rows >> 1`), preserving exact `0..64` / `64..128` UV alignment (zero UV truncation on `sidx=461` or pillars) while cutting VDP1 DDA steps by **50% to 75%** on medium/distant walls.
     - Coalesced distant (`cz > 400`) narrow diagonal walls (`dx <= 16` or `dx <= 28`) and edge-on `dx <= 4` slivers into single quads.
  2. **`[0, HW_VIEW_H]`-Clamped Flat `dy` LOD & Tighter Off-Screen Clipping (`HW_FlatQuadDy`, `HW_FlatSizeLOD`, `HW_ClipFlatQuadOffscreen`):**
     - Clamped `y0..y3` to `[0, HW_VIEW_H]` inside `HW_FlatQuadDy` so off-screen vertex excursions (`y < 0` or `y > 168`) do not inflate `dy` and force `64x64` textures on `10`–`28` scanline strips.
     - Tuned `HW_FlatSizeLOD` thresholds to `dy > 38` (`64x64`), `dy > 16` (`32x32`), `dy > 7` (`16x16`), and tightened `HW_ClipFlatQuadOffscreen` bounds from `[-32, vw+32] x [-16, vh+16]` to `[-12, vw+12] x [-4, vh+4]`, cutting `VISIT 0` flat DDA steps at `X743 Y-3215 A3` from **`104,848` down to `60,527` (`-42.3%`)**.
  3. **Tighter Off-Screen Vertical Wall Row Trimming (`cmdbuf_trim_wall_y` in [`saturn_cmdbuf.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_cmdbuf.c#L285-L348)):**
     - Tightened top/bottom off-screen wall row trimming threshold from `-6` / `vh + 5` to `-3` / `vh + 3`.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_081531.csv` & `sdbg` [`sdbg_743_3215_3_v9.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_743_3215_3_v9.png)):**
  - **VDP1 DDA Steps at `X743 Y-3215 A3`:**
    - End of `VISIT 1`: **`348,533` $\to$ `236,074` DDA steps (`-112,459` steps, `-32.3%`)**
    - End of `VISIT 0` (Full Frame): **`454,899` $\to$ `298,119` DDA steps (`-156,780` steps, `-34.5%`)**, well below the `348,533` 1-VBlank hardware limit so `VISIT 0` (ceiling overhang above the left pillar, foreground floor, and pistol) never gets cut off!
    - Quad count at `X743 Y-3215 A3`: **`W139 Q385` $\to$ `W105 Q350` (`-35` quads)**.
  - **VDP1 DDA Steps at `X754 Y-3097 A351`:** **`249,264` $\to$ `166,744` DDA steps (`-33.1%`)**, `W119 Q364` $\to$ **`W96 Q328` (`-36` quads)**.
  - **Win32 E1M1 Journey:** **`4.558%`** full-level avg error, **`91.8 W / 109.0 F / 201.2 Tot`** avg quads (`-5.9` wall quads/frame vs `v8`), **`60.1 FPS`**.
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o`, `__bend = 0x060b8c10` (`29,680 B` free High WRAM headroom).
  - **Real Saturn Hardware Verification (`media_1792654898602_770222bd.jpg`):** Locked **`30.0 FPS`** at E1M1 spawn (`R26 V5 T2 C0 B25 G16 S0 J0 W99 F108 Q208 D0`), up from `14.7 FPS` (`V18 S16`), with 100% of foreground flats and the pistol sprite rendered cleanly.

---

## `v10_hud_telemetry` — Real-Hardware VDP1 `EDSR`/`COPR`, `DDA` Step Load, Split `V<cef>/<del>`, and `Z` Coordinate HUD Diagnostics (2026-10-22)
- **Backup Location:** `backups/v10_hud_telemetry/` (`saturn_hw_render.c`, `saturn_vdp1.c`, `saturn_cmdbuf.c`, `saturn_hud.c`, `saturn.h`, `game.map`)
- **Changes Made:**
  1. **`E<edsr>/<slot>` VDP1 Completion & Cutoff Slot Readout (`saturn_vdp1.c`, `saturn_hud.c`):**
     - Added `V1_LOPR` (`0x25D00012`) and `V1_COPR` (`0x25D00014`) readbacks in `Saturn_Vdp1Wall_Frame()` (`saturn_vdp1.c`) to record `saturn_v1w_copr = ((addr_reg >> 2) - 6)` alongside `saturn_v1w_edsr`.
     - Latched the minimum `saturn_v1w_edsr` (and corresponding `saturn_v1w_copr`) over each 15-frame HUD window in `Saturn_HUD_Draw()` (`saturn_hud.c`) so even a single-frame VBlank cutoff is captured on screen.
  2. **`M<dda_k>` Total VDP1 DDA Step Estimator (`saturn_cmdbuf.c`, `saturn_hud.c`):**
     - Computed `saturn_hw_dda_steps` across `saturn_cmd_m`, `saturn_cmd_s` (via `saturn_sub_ranges`), and `saturn_cmd_w` inside `Saturn_CmdBuf_CullOccluded()` (`saturn_cmdbuf.c`) with zero overhead during VBlank delivery (`Saturn_CmdBuf_Deliver()`), displaying `M<dda_steps / 1000>` on Row 2.
  3. **`V<cef>/<del>` Split VDP1 Wait vs. Command Delivery Timing (`saturn_hud.c`):**
     - Updated Row 1 `V` tag to display `V<pf_cef_ms>/<pf_del_ms>` (VDP1 `EDSR.CEF` hardware rasterizer wait ms vs. CPU `Saturn_CmdBuf_Deliver` ms) with overflow-safe averaging.
  4. **`Z<z>` Player Floor/View Coordinate & `'Z'` 3x5 Font Glyph (`saturn_hud.c`):**
     - Added the `'Z'` glyph (`index 39`) to `hud_tiny_glyph()` and `Z<z>` (`p->mo->z >> FRACBITS`) alongside `X<x> Y<y> A<deg>` on Row 2.
- **Verification (`DOOM-Windows` + `sdbg`):**
  - `__bend = 0x060b90c0` (`28,480 B` free High WRAM headroom).
  - Verified in `sdbg` (`sdbg_hud_v9.png`):
    - Spawn (`X1056 Y-3616 Z0 A90`): `V0/1`, `E3/214`, `M299`
    - Pillar 1 (`X743 Y-3215 Z0 A3`): `V8/2`, `E3/354`, `M300`
    - Pillar 2 (`X754 Y-3097 Z0 A351`): `V8/2`, `E3/334`, `M269`

---

## `v11_30fps_cpu_opt` — 30 FPS Real-Hardware CPU Optimizations (`R39->R36`, `B39->B35` in `sdbg`) (2026-10-22)
- **Backup Location:** `backups/v11_30fps_cpu_opt/` ([`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn_cmdbuf.c), [`saturn_cmdbuf.h`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn_cmdbuf.h), [`saturn_hud.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn_hud.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn_vdp1.c), [`saturn.h`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/saturn.h), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v11_30fps_cpu_opt/game.map))
- **Context from Real Hardware (`v10_hud_telemetry` TV Photos):**
  - **E1M1 Spawn (`X1056 Y-3616 Z0 A90`, [`media_1792657166044_7538087d.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792657166044_7538087d.jpg)):** **`30.0 FPS`** (`R29 V0/1 T2 C0 B26 G16 S0 J0 W99 F108 Q208 D0 | N34 E3/214 K4 I253 U73 X0 H0 P10 L21/13 M299`) — `100%` VDP1 completion (`E3/214`) in 1 field (`V0/1`).
  - **E1M1 Pillar Vista (`X742 Y-3167 Z0 A357`, [`media_1792657389214_22d481c0.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792657389214_22d481c0.jpg)):** **`20.8 FPS`** (`R34 V6/1 T2 C0 B32 G20 S4 J0 W119 F178 Q298 D0 | N49 E3/304 K18 I279 U101 X0 H0 P11 L34/15 M292`) — `100%` VDP1 completion (`E3/304`, zero visual defects), missing the `33.33 ms` 2-VBlank `30.0 FPS` deadline by only **`~2.7 ms`** (`T2 + R34 = 36 ms`).
- **Changes Made:**
  1. **15-Frame Sampled `Saturn_CmdBuf_ComputeDDA()` (`saturn_cmdbuf.h`, `saturn_cmdbuf.c`, `saturn_hud.c`):**
     - Split the cold-cache walk over `saturn_cmd_m`, `saturn_cmd_s`, and `saturn_cmd_w` out of `Saturn_CmdBuf_CullOccluded()` into `Saturn_CmdBuf_ComputeDDA()`, called from `Saturn_HUD_Draw()` only once every 15 frames (`if (perf_refreshed || saturn_hw_dda_steps == 0)`), eliminating ~350 cold SDRAM cache line misses per frame on 14 out of 15 frames while keeping `Saturn_CmdBuf_CullOccluded()` strictly limited to warm-L1 off-screen wall Y-trimming + `_hw_purge_cache`.
  2. **Overlapped `HW_FillBackground()` Sky Render with Slave SH-2 Flat Processing (`saturn_hw_render.c`):**
     - Moved the `HW_FillBackground()` sky block before the Master work-stealing `while (1)` loop in `Saturn_HW_RenderPlayerView()` so the Slave SH-2 drains remaining Low WRAM `.FLT` flat jobs in parallel while the Master paints `I_VideoBuffer` in High WRAM (`0` bus contention), cutting `S4` down toward `S0`–`S1` on sky vistas.
  3. **Deferred Back-Child `HW_CheckBBoxStatus(bbox)` in `HW_RenderBSPNode()` (`saturn_hw_render.c`):**
     - Moved `HW_CheckBBoxStatus(bbox)` for BSP back-children from push time to pop time in `HW_RenderBSPNode()`, skipping frustum evaluation entirely for all back-children remaining on `bsp_stack` when `hw_solid_count >= saturn_view_w` terminates the BSP walk (`K18` at `X742 Y-3167 A357`).
  4. **Zero `: "memory"` Clobbers, Deduplicated `hw_wall_emit_ctx_t`, and `-fno-reorder-blocks` in `HW_HOT_O2` (`saturn_hw_render.c`):**
     - Added `"no-reorder-blocks"` to `HW_HOT_O2` (`__attribute__((optimize("O2", "no-reorder-blocks")))`), preventing SH-2 GCC from moving cold basic blocks >254 bytes away (which exceeded SH-2's 8-bit `bt`/`bf` branch displacement and emitted 104 `bra` + `nop` trampolines across `HW_EmitVisibleBakedSeg`).
     - Removed the 5 duplicate `cur_srca`, `cur_srca_mono`, `cur_srca_64`, `cur_tw`, `cur_th` fields from `hw_wall_emit_ctx_t`, moved the near-plane `(256 << 16) / u_span` hardware `DIVU` into `HW_ClipWallNearPlane` so `HW_EmitVisibleBakedSeg` has **zero inline-asm `: "memory"` clobbers**, and deferred writing `ctx->seg`, `ctx->p0`, `ctx->p1`, `ctx->u_start`, `ctx->u_span`, `ctx->inv_u_span`, `ctx->mono_tw`, `ctx->needs_clip` to the cold/strip branches so the common single-band monolithic quad path skips them completely.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_100207.csv` & `sdbg` [`sdbg_hud_v11.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_hud_v11.png)):**
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o` (`_HW_Worker_ProcessJob` shrank `-228 B` to `2,812 B`, `_HW_EmitVisibleBakedSeg` shrank `-192 B` to `3,224 B`, `_HW_RenderSubsectorBakedWalls` shrank `-140 B` to `1,348 B`, `_HW_EmitWallStrips_Linear` shrank `-128 B` to `2,716 B`), `__bend = 0x060b9270` (`28,048 B` free High WRAM headroom).
  - **Win32 E1M1 Journey:** **`4.558%`** full-level avg error (`32.60%` bit-exact `< 0.1%`, `0` visual regressions), **`91.8 W / 109.0 F / 201.2 Tot`** avg quads, **`60.1 FPS`**.
  - **Cold-Booted `sdbg` E1M1 Spawn (`X1056 Y-3616 Z0 A90`, [`sdbg_spawn_v11.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_spawn_v11.png)):**
    - **`30.0 FPS` (`R29 V0/1 T2 C0 B28 G17 S1 J0 W99 F108 Q208 D0 | N34 E3/214 K4 I253 U73 X0 H0 P10 L21/13 M299`)**
  - **Cold-Booted `sdbg` E1M1 Pillar Vista (`X742 Y-3167 Z0 A356`, [`sdbg_742_3167_357_v11.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_742_3167_357_v11.png)):**
    - **`20.0 FPS` (`R36 V9/2 T3 C0 B35 G22 S0 J0 W119 F178 Q298 D0 | N49 E3/303 K18 I279 U101 X0 H0 P11 L34/15 M292`)** — **`R` improved `-3 ms` (`39` $\to$ `36 ms`), `B` improved `-4 ms` (`39` $\to$ `35 ms`), and `G` improved `-1 ms` (`23` $\to$ `22 ms`) vs `v10` in `sdbg`!**

---

## `v12_slave_bus_and_sprite_opt` — Per-Sector Sprite Bucketing & Slave SH-2 External Bus Contention Elimination (2026-10-22)
- **Backup Location:** `backups/v12_slave_bus_and_sprite_opt/` ([`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/saturn_cmdbuf.c), [`saturn_hud.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/saturn_hud.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/saturn_vdp1.c), [`d_main.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/d_main.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v12_slave_bus_and_sprite_opt/game.map))
- **Context from Real Hardware (`v11_30fps_cpu_opt` TV Photos):**
  - At the deep West alcove (`X645 Y-3134 Z-8 A355` and `X568 Y-3180 Z-8 A2`), real Saturn hardware showed **`20.0 FPS`** with **`R36 V10/2 T2 C0 B35 G21 S0 J0 W117 F170 Q288 D0 | N64 E3/292 K27 I329 U121 X0 H0 P15 L36/18 M257`**:
    1. **Zero visual defects (`E3/292`, `M257`):** VDP1 finished 100% of all 292 commands well inside 1 field (`257,000 < 348,533` DDA steps). What looked like a vertical seam at `x = 123` on `X645 Y-3134 A355` was confirmed in `DOOM-Windows` software comparison (`win32_645_3134_355_z8_triple.png`) to be authentic E1M1 map geometry (Linedef 194, `sidx=183`, a 24-unit alcove corner at `(704, -3136)`).
    2. **CPU Bottleneck (`B35 G21`, `B - G = 14 ms`):** Visiting 54 subsectors (`N64`, `I329` segs) took `B = 35 ms` (`R = 36 ms`), missing the `33.33 ms` 2-VBlank `30.0 FPS` budget by `~3 ms`.
- **Changes Made in [`saturn_hw_render.c`](file:///c:/Users/charl/Desktop/joengine-master/Projects/DOOM-Saturn/doom/src/saturn_hw_render.c):**
  1. **Lazy Per-Sector Frustum-Culled Subsector Sprite Bucketing (`HW_BuildSectorThings` + `HW_RenderSubsectorSprites`):**
     - Previously, `HW_RenderSubsectorSprites(sec, subnum)` walked the entire sector's `sec->thinglist` linked list (`mobj_t` in High WRAM heap + `subsector_t *` pointer arithmetic) on **every visited subsector** (`30+` times in Sector 1 at `X645 Y-3134 A355`, causing 150+ cold High WRAM pointer chases during `B - G`).
     - Added `hw_sec_things_built[16]`, `hw_sub_has_things[32]`, `hw_sub_first_thing[1024]`, and `hw_thing_pool[128]`. `HW_BuildSectorThings(sec)` now walks `sec->thinglist` **at most once per sector per frame**, transforms & frustum-culls each `mobj_t` in camera space (`cz >= MINZ`, `|cx| <= cz + (cz >> 1)`), and buckets surviving mobjs into `hw_sub_first_thing[msub]` (preserving exact `sec->thinglist` insertion order via reverse traversal). `HW_RenderSubsector` now calls `HW_RenderSubsectorSprites` **only** when the subsector's bit in `hw_sub_has_things` is set (`2` subsectors instead of `30+`).
  2. **Zero-Copy 4-Arg `HW_Worker_ProcessJob`, Low-WRAM `slave_fctx`, & Lockless SPSC Queue:**
     - Allocated `hw_flat_emit_ctx_t slave_fctx __attribute__((aligned(16)))` on the Slave's dedicated Low WRAM stack (`0x0021BFF0`) inside `HW_Slave_FlatWorker_Loop`, completely eliminating `hw_fctx_slave` in High WRAM `.bss` (`0x06000000`) so the Slave CPU never writes context fields across the shared High WRAM bus while the Master CPU executes `B`/`G`.
     - Changed `HW_Worker_ProcessJob(uncached_job, fctx, proj_v, ipc)` to take 4 register arguments (`r4..r7`, `0` stack args) and load `jw[0..10]` directly from `uncached_job` without `HW_COPY_JOB` or `88-byte` `local_job` stack copies (`-44` external bus reads/writes per job).
     - Latched all 14 frame-constant camera/viewport fields into `slave_fctx` **once per frame** on the first flat job (`sky_flags & 2u`), and gated `jw[11..21]` (`solid[0..10]`) writes and reads by the 11-bit `hw_solid_any` bitmask packed into the low 16 bits of `jw[10]`.
     - Removed the unused Master work-stealing loop (`S0 J0` on all vistas), turning `ipc->jobs[]` into a pure lockless SPSC ring buffer (eliminating all `ipc->job_active` Dekker memory barriers and updating `ipc->quad_count` only when the ring drains).
  3. **Packed 32-Bit Stores in Slave Phase 1 & Cold `cx` Skip:**
     - In `HW_Worker_ProcessJob` Phase 1 (`.FLT` vertex projection), `p_ptr->cx = cx` is only stored when `__builtin_expect(cz < (288 << FRACBITS), 0)` (covering all vertices of any LOD0 $64\times 64$ quad that could trigger near-plane clipping or multi-band subdivision), and `(px, py_c)` + `(py_f, valid)` are packed into two 32-bit `mov.l` stores (`pw[2]` and `pw[3]`), cutting Phase 1 external bus writes in half (`6` $\to$ `3` stores per vertex).
  4. **Tracked `[sub_solid_min_w .. sub_solid_max_w]` in `HW_MarkSubSolidSpan`:**
     - `HW_RenderSubsector` now drains `sub_solid_mask` into `hw_solid_mask` only across the active word range `[sub_solid_min_w .. sub_solid_max_w]` (`1`–`2` words instead of `11`) and updates `hw_solid_any`.
- **Measured Impact (`journey_telemetry_MAP_E1M1_20261022_110528.csv` & `sdbg` [`sdbg_hud_v12.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_hud_v12.png)):**
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o` (`_HW_EmitWallStrips_Linear` shrank `-124 B` to `2,592 B`), `__bend = 0x060b9af0` (`25,872 B` free High WRAM headroom).
  - **Win32 E1M1 Journey:** **`4.558%`** full-level avg error (`32.60%` bit-exact `< 0.1%`, `0` visual regressions), **`91.8 W / 109.0 F / 201.2 Tot`** avg quads, **`60.1 FPS`**.
  - **Cold-Booted `sdbg` E1M1 Spawn (`X1056 Y-3616 Z0 A90`, [`sdbg_spawn_v12.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_spawn_v12.png)):**
    - **`30.0 FPS` (`R28 V1/1 T2 C0 B27 G17 S2 J0 W99 F108 Q208 D0 | N34 E3/214 K4 I253 U73 X0 H0 P10 L21/13 M299`)** — improved from `28.1 FPS (R29 B28 S1)` in `v11` to locked **`30.0 FPS`** with `2 ms` spare VSync margin (`S2`)!
  - **Cold-Booted `sdbg` E1M1 West Alcove (`X742 Y-3167 Z0 A357`, [`sdbg_742_3167_357_v12.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_742_3167_357_v12.png)):**
    - **`20.0 FPS` (`R35 V8/2 T2 C0 B34 G21 S0 J0 W119 F178 Q298 D0 | N49 E3/303 K18 I279 U101 X0 H0 P11 L34/15 M292`)** — `B` dropped from `34.9 ms` to **`33.93 ms` (`B34`)** and `G` dropped from `22.0 ms` to **`21.40 ms` (`G21`)**.
  - **Cold-Booted `sdbg` E1M1 Deep West Alcove (`X645 Y-3134 Z-8 A355`, [`sdbg_645_3134_355_v12.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_645_3134_355_v12.png)):**
    - **`19.5 FPS` (`R37 V7/2 T2 C0 B37 G21 S0 J0 W117 F170 Q288 D0 | N64 E3/292 K27 I329 U121 X0 H0 P15 L36/18 M257`)** — `B` dropped **`-1.54 ms`** in `sdbg` (`38.05 ms` $\to$ **`36.51 ms`**, `G` `22.15 ms` $\to$ **`21.33 ms`**, `B - G` `15.90 ms` $\to$ **`15.18 ms`**), and on real Saturn hardware (where `B` was `35 ms` in `v11` and High WRAM/Low WRAM bus wait states are cycle-accurate), eliminating `>2,500` Slave external bus transactions + 30 redundant `sec->thinglist` walks targets dropping `R36 B35` into the `31–33 ms` `30.0 FPS` window.

---

## `v13_vdp1_arena_and_wall_uv_fix` — VDP1 Texture Arena Overflow Fix (`B238 G209` $\to$ `B35 G21`), `STARTAN3` Wall UV Fix & Flat LOD Tuning (2026-10-22)
- **Backup Location:** `backups/v13_vdp1_arena_and_wall_uv_fix/` ([`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v13_vdp1_arena_and_wall_uv_fix/saturn_vdp1.c), [`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v13_vdp1_arena_and_wall_uv_fix/saturn_hw_render.c), [`saturn_cmdbuf.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v13_vdp1_arena_and_wall_uv_fix/saturn_cmdbuf.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v13_vdp1_arena_and_wall_uv_fix/game.map))
- **Context from Real Hardware (`v12_slave_bus_and_sprite_opt` TV Photos):**
  1. **Spawn (`X1056 Y-3616 Z0 A90`, [`media_1792695512637_f71559b6.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792695512637_f71559b6.jpg)):** Locked **`30.0 FPS`** (`R26 V5/1 T2 C0 B25 G15 S0 J0 W99 F108 Q208 D0`).
  2. **Blue Pool East (`X1161 Y-3117 Z-8 A202`, [`media_1792695512639_cafca431.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792695512639_cafca431.jpg)):** Walking from Spawn to `X1161 Y-3117 Z-8 A202` collapsed to **`7.7 FPS`** (`R119 V6/1 T5 C0 B238 G209 S0 J0 W132 F164 Q297 D0`) and the `STARTAN3` upper skylight walls (`visit 0: sub=57`, `y = 0..45`) failed to draw on real hardware.
  3. **West Alcove Looking East (`X652 Y-3137 Z-8 A0`, [`media_1792695512642_b3a2ece5.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792695512642_b3a2ece5.jpg)):** **`21.8 FPS`** (`R33 V6/1 T2 C0 B33 G19 S4 J0 W110 F175 Q286 D0`) — `B33 G19` was already `33 ms` (`-2 ms` faster than `v11`), `S4` (`4 ms` Slave flat wait) pushed `R + S` past 2 VBlanks, and the `STARTAN3` walls across the blue pool (`x = 140..235`) rendered as solid brown blocks.
- **Root Causes & Fixes Implemented:**
  1. **VDP1 Texture Arena `2.75x` $\to$ `1.5x` Allocation (`saturn_vdp1.c`):**
     - Previously, `v1_tex_get` allocated **3 copies** of every `tw >= 64` wall texture (`off_strip` = `1.0x`, `off_64` = `1.0x`, `off_32` = `0.75x`, totaling **`2.75x` = `45,056 B` per `128x128` texture**). At `X1161 Y-3117 Z-8 A202`, `v1_arena_off` hit `0x7c0e0` (`< 16 KB` left before `0x80000`), triggering `Saturn_Vdp1Wall_Reset()` to wipe the entire texture cache on **every frame** and re-upload `310,720 B` of textures across the B-bus inside `G` (`G209 ms`, `B238 ms`, `7.7 FPS`), starving VDP1's auto-draw engine of VRAM cycles on real hardware so the last subsector (`visit 0: sub=57` upper skylight walls) failed to draw.
     - Removed `v1_copy_64_blocks` (`off_64`) and the unused half-height `32x(th/2)` mip so `tw >= 64` textures allocate `1.0x` (`16xth` strips) + `0.5x` (`32xth` 64-unit blocks, `16 KB` low-water guard) = **`1.5x` total** (`24,576 B` instead of `45,056 B` per `128x128` texture), saving **`> 120 KB`** of VDP1 VRAM across E1M1.
  2. **`STARTAN3` Narrow-Wall UV Block Selection Fix (`saturn_hw_render.c`):**
     - In `HW_EmitVisibleBakedSeg` (line 6071) and `HW_EmitWallStrips_Linear` (line 5154), `dx <= 16` walls with `u_span >= 64` previously selected `srca` (`mono_tw = 16`, which is **only strip 0 `u = 0..15`** — the 16-texel brown border seam of `STARTAN3`!), discarding texels `u = 16..63` and stretching the brown border across the entire wall (`q[12..13]`, `q[41..44]`, `q[70..71]`, `q[94..101]` at `X652 Y-3137 A0`).
     - Restricted `mono_tw = 16` (`srca`) to `((((u_span ^ 16) | (u_start & 15)) == 0) || (!persp_strips && dx <= 2))` and removed the `s_px1 - s_px0 <= 16` strip-0 override in `HW_EmitWallStrips_Linear` so `dx = 3..44` 64-unit walls always sample `srca_mono` (`32xth`, the full 2:1 downsampled 64-texel `STARTAN3` panel).
  3. **Slave SH-2 Flat `LOD0` Near-Corner Threshold (`180` $\to$ `132` units in `HW_SelectFlatLOD`):**
     - Lowered the near-corner threshold in `HW_SelectFlatLOD` from `(180 << 12)` to `(132 << 12)`, switching mid-distance subsectors `sub=66` (`cz = 136..693`, `39` tiny 1–3px flat quads) and `sub=76` (`cz = 178..630`, `18` flat quads) at `X652 Y-3137 A0` from `LOD0` to `LOD1`, eliminating **48 flat quads (`F175` $\to$ `F127`, `-27.4%`)** and **61 vertex projections** on the Slave SH-2 to eliminate `S4`.
- **Measured Impact (`sdbg` [`sdbg_1161_3117_202_v13.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_1161_3117_202_v13.png), [`sdbg_652_3137_0_v13.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_652_3137_0_v13.png), [`zoom_652_center_v13.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/zoom_652_center_v13.png)):**
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o`, `__bend = 0x060b9870` (`26,512 B` free High WRAM headroom).
  - **Blue Pool East (`X1161 Y-3117 Z-8 A202`):**
    - `v1_arena_off` stays at **`0x63140` (`118,464 B` free below `0x80000`, `page_flush = 0`, `+bus_tex = 0 B/frame`)**!
    - **`B` dropped `-194.53 ms` (`229.97 ms` $\to$ `35.44 ms`) and `G` dropped `-181.63 ms` (`203.00 ms` $\to$ `21.37 ms`)** in `sdbg`, completely eliminating the cache-thrash stall and B-bus VDP1 starvation!
  - **West Alcove Looking East (`X652 Y-3137 Z-8 A0`):**
    - **`B = 34.23 ms`, `G = 19.76 ms`, `W = 106` (`-4` wall quads), `F = 127` (`-48` flat quads vs HW!), `Q = 234` (`-52` total quads)**, `arena = 0x66b60` (`103,584 B` free), and all East `STARTAN3` walls (`x = 140..235`) render with crisp grey/tan panels ([`zoom_652_center_v13.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/zoom_652_center_v13.png)).

---

## `v14_uncached_ipc_and_deferred_strips` — Uncached Delta-Only IPC, Deferred Subsector Wall Strips, `0.25x` Half-Height Mip & Master Flat Overflow (2026-10-23)
- **Backup Location:** `backups/v14_uncached_ipc_and_deferred_strips/` ([`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v14_uncached_ipc_and_deferred_strips/saturn_hw_render.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v14_uncached_ipc_and_deferred_strips/saturn_vdp1.c), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v14_uncached_ipc_and_deferred_strips/game.map))
- **Context from Real Hardware (`v13_vdp1_arena_and_wall_uv_fix` TV Photos):**
  1. **Blue Pool Looking West (`X1139 Y-3233 Z-16 A173`, [`real_hw_v13_bluepool_22_5fps.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/real_hw_v13_bluepool_22_5fps.jpg)):** **`22.5 FPS`** (`R34 V4/1 T2 C0 B33 G20 S3 J0 W118 F126 Q245 D0 | N57 E3/255 K9 I257 U93 X0 H0 P42 L27/30 M264`) — `100%` of upper skylight walls (`E3/255`) and `STARTAN3` panels rendered cleanly, jumping from `7.7 FPS (R119 B238 G209)` in `v12` to `22.5 FPS (R34 B33 G20 S3)`.
  2. **Spawn (`X1056 Y-3616 Z0 A90`, [`real_hw_v13_spawn_30fps.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/real_hw_v13_spawn_30fps.jpg)):** **`30.0 FPS`** (`R25 V6/1 T1 C0 B25 G15 S0 J0 W85 F108 Q194 D0 | N34 E3/200 K4 I253 U73 X0 H0 P10 L22/12 M356`).
- **Changes Made:**
  1. **Zero-Read Uncached IPC & Delta-Only `hw_slave_dirty_words` (`saturn_hw_render.c`):**
     - `HW_SubmitSlaveFlatJob` now writes directly to `HW_SLAVE_BASE_UNCACHED` (`0x20210000`) using a Master-local `hw_master_job_tail` shadow counter and removes the stalling `dummy_flush = ipc->job_tail` uncached read, eliminating **100% of uncached Low WRAM reads during `B`** and preventing write-only IPC jobs (`5 KB`/frame) from evicting Master L1 cache lines.
     - Added `hw_slave_dirty_words` so `jw[9]` and `jw[11..21]` (`hw_solid_mask`) are only sent across the 16-bit Low WRAM bus for words that actually changed since the previous Slave flat job (`>75%` of subsectors send `0` solid words).
     - Added Master flat overflow (`hw_def_flat_jobs[4]`) for indoor views with `>36` flat jobs (`!saturn_hw_sky_visible`), draining up to 4 tail flat jobs on the Master right after `HW_RenderBSPNode` in parallel with the Slave's tail jobs to eliminate `S3`/`S4`.
  2. **Per-Subsector Deferred Subdivided Wall Strip Emission (`HW_DeferOrEmitSubdividedWall` + `HW_FlushDeferredSubsectorWalls` in `saturn_hw_render.c`):**
     - Outlined the non-monolithic `else` branch of `HW_EmitVisibleBakedSeg` (`should_subdivide || !is_single_band`) and deferred non-near-clipped subdivided walls to `HW_FlushDeferredSubsectorWalls(&wctx)` at the end of `HW_RenderSubsectorBakedWalls`, preventing `HW_EmitWallStrips` (`2.3 KB`) from evicting `HW_RenderSubsectorBakedWalls` and `HW_EmitVisibleBakedSeg` from the 4 KB L1 instruction cache mid-subsector.
     - Raised the `!persp_strips && srca_64 == srca` monolithic/coalesced threshold on `srca_mono` (`32xth`) from `dx <= 44` to `dx <= 64`, cutting subdivided wall strips `P` at the Blue Pool (`X1139 Y-3233 A173`) by **`-48%` (`P42` $\to$ `P22`)** and wall quads from `W118` $\to$ `W104`.
  3. **Restored `0.25x` Half-Height `32x(th/2)` Wall Mip (`1.75x` Total Footprint in `saturn_vdp1.c`):**
     - Restored the `0.25x` half-height `32x(th/2)` block mip inside `v1_copy_32_blocks` (`1.75x` total vs `2.75x` in `v12`), cutting VDP1 DDA steps (`M`) by **`-40,000` at Spawn (`M356` $\to$ `M315`)** and **`-36,000` at Blue Pool (`M264` $\to$ `M228`)** while keeping **`86–107 KB` free VDP1 VRAM** (`flush = 0, refuse = 0`).
- **Measured Impact (`sdbg` [`sdbg_1056_3616_90_v14.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_1056_3616_90_v14.png), [`sdbg_1139_3233_173_v14.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_1139_3233_173_v14.png), [`sdbg_1161_3117_202_v14.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_1161_3117_202_v14.png), [`sdbg_652_3137_0_v14.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_652_3137_0_v14.png)):**
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o` (`_HW_EmitVisibleBakedSeg` `2,864 B`, `_HW_RenderSubsectorBakedWalls` `1,316 B`), `__bend = 0x060ba2f0` (`23,824 B` free High WRAM headroom).
  - **Spawn (`X1056 Y-3616 Z0 A90`):** `W85 F108 P10 M315 V1/1 S0`, `arena = 0x48be0` (`226,336 B` free).
  - **Blue Pool TV Pose (`X1139 Y-3233 Z-16 A173`):** **`B = 35.64 ms`, `G = 21.76 ms`, `S = 0.00 ms` (`S0`), `W = 104` (`-14` vs `v13`), `P = 22` (`-48%` vs `P42`), `M = 228` (`-36k` vs `M264`)**, `arena = 0x65ee0` (`106,784 B` free, `0` flushes).
  - **Blue Pool East (`X1161 Y-3117 Z-8 A202`):** **`B = 35.10 ms`, `G = 22.21 ms`, `S = 0.00 ms`**, `W = 125, F = 145, P = 33, M = 252`, `arena = 0x6ae40` (`86,464 B` free, `0` flushes).
  - **West Alcove (`X652 Y-3137 Z-8 A0`):** **`B = 34.41 ms`, `G = 20.56 ms`, `S = 0.00 ms`**, `W = 106, F = 129, P = 8`, `arena = 0x6f0c0` (`69,440 B` free, `0` flushes).

---

## `v15_3d_portal_pvs_and_2d_aperture` — 3D Portal Slope PVS, PVS Node Subtree Bitmask & 2D Vertical Portal Aperture (`ytop`/`ybot`) (2026-10-23)
- **Backup Location:** `backups/v15_3d_portal_pvs_and_2d_aperture/` ([`saturn_hw_render.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v15_3d_portal_pvs_and_2d_aperture/saturn_hw_render.c), [`saturn_vdp1.c`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v15_3d_portal_pvs_and_2d_aperture/saturn_vdp1.c), [`bake_pvs.py`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v15_3d_portal_pvs_and_2d_aperture/bake_pvs.py), [`game.map`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/backups/v15_3d_portal_pvs_and_2d_aperture/game.map))
- **Context from Real Hardware (`v14_uncached_ipc_and_deferred_strips` TV Photos):**
  1. **Spawn (`X1056 Y-3616 Z0 A90`, [`media_1792734285594_a0a11663.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792734285594_a0a11663.jpg)):** **`30.0 FPS`** (`R25 V6/1 T1 C0 B25 G15 S0 J0 W85 F108 Q194 D0 | N34 E3/200 K4 I253 U73 X0 H0 P10 L22/12 M316`).
  2. **West Alcove Upper Platform Looking East (`X-297 Y-3227 Z104 A0`, [`media_1792734285592_46e26f3a.jpg`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/.user_uploaded/media_1792734285592_46e26f3a.jpg)):** **`15.0 FPS`** (`R49 V14/1 T2 C0 B44 G25 S0 J0 W148 F152 Q301 D0 | N88 E3/336 K19 I464 U97 X0 H0 P36 L51/37 M370`).
     - Standing at `Z = 104` (`viewz = 145`) looking East down the staircase (`Z = 104 -> -8`) and through the `ceil = 120` / `ceil = 72` doorway into the Blue Carpet room (`Z = 0..144`), the 2D-only PVS marked `153/239` subsectors potentially visible (`N = 88` visited, `I = 464` seg iterations), and because 2-sided upper/lower doorway walls never closed `hw_solid_mask`, the renderer drew dozens of 100%-occluded subsectors behind upper/lower step walls (`W148 F152 Q301`).
- **Root Causes & Fixes Implemented:**
  1. **3D Portal Slope-Interval PVS Baker + True BSP Convex Polygon Sampling (`bake_pvs.py` + `bake_pak.py`):**
     - Upgraded `bake_pvs.py` from 2D-only raycasting to 3D vertical slope-interval (`[slope_lo, slope_hi]`) portal clipping across all 2-sided linedefs on a ray from source subsector $[z_{\text{floor}}, z_{\text{ceil}}]$ to target subsector $[z_{\text{floor}}, z_{\text{ceil}}]$ (with movable doors/lifts/stairs automatically detected and treated as open).
     - Added recursive BSP partition + seg half-plane polygon clipping (`walk_bsp_poly` / `clip_poly`) so subsectors bounded by implicit BSP partition lines (such as `ssec 179` and `ssec 180` in the outdoor courtyard) generate sample points across their full convex floor polygon.
     - Re-baked all 36 `.PVS` and `.PAK` files (`E1M1.PVS` culling rose from `55.4%` to **`68.0%`**, cutting average visible subsectors per leaf from `106.6` to **`76.4`**).
  2. **O(1) PVS Node Subtree Bitmask (`node_pvs_vis` in `saturn_hw_render.c`):**
     - Precomputes a 256-bit `node_pvs_vis[8]` bitmask in `HW_UpdateNodePvsVis(pvs_row, ssec)` whenever the viewer's subsector `ssec` changes (keyed on `ssec` rather than `pvs_row` because `Saturn_Pvs_GetRow` returns a static `pvs_row_cache` buffer), allowing `HW_RenderBSPNode` to prune entire BSP subtrees containing zero PVS-visible subsectors before any bounding-box projection.
  3. **2D Vertical Portal Aperture (`ytop[320]` / `ybot[320]`) with Per-Sector Attachment Tracking (`saturn_hw_render.c`):**
     - Placed `hw_aperture_state_t` (`HW_APERTURE`, `~2.8 KB`) at `SATURN_WRAM_L_PILLARS_ADDR + 0x2000u` (`0x00232000`, inside the unused 24 KB tail of Low Work RAM `SATURN_WRAM_L_PILLARS_ADDR`, costing **`0 bytes`** of High Work RAM `.bss`).
     - Tracks per-column `[ytop[x], ybot[x]]` and 32-column word summaries `ytop_min_w[w]` / `ybot_max_w[w]` across 2-sided upper/lower portal walls (`HW_RecordSegPortalAperture` + `HW_CommitSubsectorOcclusion`), including upper walls with `open_top <= viewz` (such as `line 61` at `ceil = 120` and `line 54` at `ceil = 72` when viewed from `Z = 104, viewz = 145`), self-attaching wall edges (`w_yt_att`, `w_yb_att`), and back-sector attachment propagation (`sec_yt_attach`, `sec_yb_attach`).
     - Whenever `ytop[x] >= ybot[x]`, sets `sub_solid_mask` so downstream 1D BSP node/seg/flat occlusion (`HW_CheckBBoxOccluded`, `HW_CheckSpanOcclusion`, `HW_Slave_IsSpanOccluded`) automatically culls everything behind closed upper+lower portal curtains.
     - Added `HW_CheckSubsectorAperture` and vertical wall aperture culling in `HW_EmitVisibleBakedSeg`, `HW_EmitMultiBandBakedWall`, `HW_EmitWallStrips_Linear`, and `HW_EmitWallStrips_Persp`, plus fixed a missing `UserClip` reset in `HW_EmitMultiBandBakedWall`.
- **Measured Impact:**
  - **SH-2 L1 Cache & High WRAM Budget:** **`0` functions `> 4,096 B`** in `saturn_hw_render.o` (largest: `_HW_RenderSeg.isra.0` `3,352 B`, `_HW_EmitVisibleBakedSeg.constprop.0` `3,172 B`, `_HW_Worker_ProcessJob.constprop.0` `3,076 B`), `__bend = 0x060bb5e0` (`18,976 B` free High WRAM headroom below `0x060C0000`).
  - **Win32 Automated 624-Frame E1M1 Journey (`journey_telemetry_MAP_E1M1_20261023_101531.csv`):**
    - Full-Level Avg Error: **`4.621%`** (`0` visual regressions across all 8 waypoints).
    - Average Subsectors Visited (`N`): `38.7` (`v14`) $\to$ **`33.1` (`-14.5%`)**
    - Average Wall Quads (`W`): `91.8` (`v14`) $\to$ **`78.4` (`-14.6%`)**
    - Average Flat Quads (`F`): `109.0` (`v14`) $\to$ **`83.4` (`-23.5%`)**
    - Average Total Quads (`Q`): `201.2` (`v14`) $\to$ **`162.2` (`-19.4%`)**
    - Max Quads in Any Frame: `459` (`v14`) $\to$ **`384` (`-16.3%`)**
  - **Cold-Booted `sdbg` E1M1 Spawn (`X1056 Y-3616 Z0 A90`, [`sdbg_1056_3616_90_v15.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_1056_3616_90_v15.png)):**
    - `N`: `34` $\to$ **`31` (`-8.8%`)**, `I`: `253` $\to$ **`232` (`-8.3%`)**, `W`: **`85`**, `F`: `108` $\to$ **`95` (`-12.0%`)**, `Q`: `194` $\to$ **`181` (`-6.7%`)**, `L`: `22/12` $\to$ **`17/10` (`-20.6%`)**, `M`: `316` $\to$ **`312`**.
  - **Cold-Booted `sdbg` E1M1 West Alcove Upper Platform (`X-297 Y-3227 Z104 A0`, [`sdbg_n297_3227_0_v15.png`](file:///C:/Users/charl/.gemini/antigravity/brain/4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4/sdbg_n297_3227_0_v15.png)):**
    - `N` (subsectors visited): `88` $\to$ **`73` (`-17.0%`)**
    - `I` (seg iterations): `464` $\to$ **`356` (`-23.3%`, `-108` segs)**
    - `U` (visible segs): `97` $\to$ **`83` (`-14.4%`)**
    - `W` (wall quads): `148` $\to$ **`121` (`-18.2%`, `-27` wall quads)**
    - `F` (flat quads): `152` $\to$ **`130` (`-14.5%`, `-22` flat quads)**
    - `Q` (total quads): `301` $\to$ **`252` (`-16.3%`, `-49` total quads)**
    - `L` (flat subsector jobs): `51/37` (`88` jobs) $\to$ **`36/33` (`69` jobs, `-21.6%`)**
    - `M` (VDP1 DDA steps): `370K` $\to$ **`346K` (`-6.5%`)**
    - `B` in `sdbg`: `66.16 ms` (`v14`) $\to$ **`52.25 ms` (`-21.0%`)**, `G` in `sdbg`: `34.12 ms` (`v14`) $\to$ **`26.67 ms` (`-21.8%`)**.





