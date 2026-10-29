# DOOM for Sega Saturn - Jo Engine port
# Full boot path: cart probe -> IWAD load (hybrid cart/CD) -> D_DoomMain.
# ram_test.c is gone ??? its A-bus register addresses (0x25FE0080/0033)
# were wrong and its init sequence conflicted with i_wad.c's
# Saturn_CartInit (correct SCU addresses per Sega SCU User's Manual).

SHELL := cmd.exe

# --- DIVLUT bisect knob (2026-09-08 Kronos boot-wedge hunt) --------------
# The NOOCRAM ISO (no CCR writes, no OCRAM access) wedged in Kronos the
# same as the full OCRAM build, so the CCR/OCRAM path is EXONERATED and
# the bug is somewhere in the SATURN_DIVLUT change set. One knob now
# selects the bisect variant; only one assignment may be active:
#   STEP1   - DIVLUT compiled in, ONLY the d_main install block runs
#             (WRAM table, no CCR/OCRAM writes)
#   STEP2   - + FixedDiv census increments
#   STEP3   - + perf rows C/N/G/M/X
#   STEP4   - + the A+B+C live A/B toggle
#   STEP5   - + the LightRow/light_div LUT consume paths
#   FULL    - everything on with the real OCRAM scratchpad table
#             (the burn candidate, once the ladder passes in Kronos)
#   OFF     - plain production code (ISO A control: BOOTED 2026-09-08)
# Each STEP is cumulative (STEPn enables sites 1..n); each step build
# adds exactly ONE new site so the first wedging boot convicts it.
# 2026-09-08 real-HW verdict: FULL booted+played (boot CCR at the correct
# 0xFFFFFE92 EXONERATED ???????? the power-cycle blue was the flaky cart slot,
# reseat fixed it), but X=110/C~751 with ~1fps: the LUT machinery is
# bounded by C (~0.1 vbl/frame), so the killer is CCR 0x09 itself ???????? it
# halves the master's cache 4KB->2KB and the flat kernel's working set
# (col_wxs+col_wys = 2.5KB) thrashes it. 4KB cache > OCRAM LUT here.
# SHIPPING = STEP5: boot-time install, WRAM table, ZERO CCR writes.
# 2026-09-08 evening: the teal/white/black wedge chronology was BOOT
# FLAKINESS (lost F1 / dead Kronos process), not binary config ???????? the
# OFF control re-booted fine to I_WadBase. SATURN_DIVLUT exonerated 2x.
SATURN_DIVLUT_MODE = STEP5
ifeq ($(SATURN_DIVLUT_MODE),FULL)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=9
else ifeq ($(SATURN_DIVLUT_MODE),STEP1)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=1 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP2)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=2 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP3)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=3 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP4)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=4 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP5)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=5 -DSATURN_DIVLUT_NOOCRAM
else
DIVLUT_DEFS :=
endif

# Match the demo's proven boot configuration exactly.
JO_COMPILE_WITH_VIDEO_MODULE = 1
JO_COMPILE_WITH_SOFTWARE_RENDERER_MODULE = 1
JO_COMPILE_WITH_BACKUP_MODULE = 0
JO_COMPILE_WITH_TGA_MODULE = 1
JO_COMPILE_WITH_AUDIO_MODULE = 0
JO_COMPILE_WITH_3D_MODULE = 0
JO_COMPILE_WITH_PSEUDO_MODE7_MODULE = 0
JO_COMPILE_WITH_EFFECTS_MODULE = 0
JO_COMPILE_WITH_STORYBOARD_MODULE = 1
JO_COMPILE_WITH_FAST_BUT_LESS_ACCURATE_MATH = 0
JO_DEBUG = 1

JO_NTSC = 1
JO_COMPILE_USING_SGL = 1
JO_MAX_FS_BACKGROUND_JOBS = 4

# Jo Engine's RAM-card support must stay OFF in this project. When a 4MB
# extended-RAM cart is present, jo_init_memory() would (a) run its own
# A-bus init with WRONG register addresses (0x25FE0080 / 0x25FE0033 ???
# the same truncated literals the old ram_test.c used; the correct ones
# are ASR0 0x25FE00B0 / AREF 0x25FE00B8, written by Saturn_CartInit),
# and (b) jo_add_memory_zone(0x22400000, 0x200000) ??? claiming the cart's
# lower 2 MB as a jo_malloc heap, exactly where i_wad.c puts the IWAD
# header, directory, and hot lumps. DOOM owns the whole cart.
JO_COMPILE_WITH_RAM_CARD_MODULE = 0

# Jo heap (BSS in core.o, ends before the SGL/CPK COMMON tail at
# 0x060C0000). Cart-mode consumers: 32 KB shadow fb + 2 x 64 KB
# software-renderer color buffers + 49 KB WRAM-H tex pocket = ~209 KB;
# 226,000 leaves ~17 KB for Jo's own allocations (font/image packs).
# The old 245,904 pushed __bend 4 KB past 0x060C0000 once the seg
# renderer landed. (The no-cart IWAD-dir fallback needs 36,940 on top
# of the video surfaces ? it did not fit at 245,904 either.)
# Round-39: the red-screen telemetry additions (sp_min + map drift)
# pushed __bend 16 bytes OVER the ceiling again ? frozen DarkCyan boot
# (main.c stage before D_DoomMain, jo heap init computes a bogus
# size). 226,000 -> 224,000 restores ~2 KB of headroom.
# 2026-09-05 full-width render: the doubled per-column tables
# (seg_mmid +6.1 KB, dbg clip/lastsec +1.3 KB, hole_blue +0.2 KB)
# overflowed the 2.7 KB __bend headroom; the heap (whose sprite-era
# consumers are long gone) gives the BSS side 7 KB back.
# 206,600 -> 205,000 (2026-09-06): the G_PlayerReborn/G_PlayerFinishLevel
# fix pushed __bend 0x60 past the 0x060C0000 ceiling; the heap's sprite-era
# consumers are long gone, so it pays again.
# 205,000 -> 204,800 (2026-09-06 intermission-freeze fix): the PU_LEVEL
# cache-reset changes (Saturn_CacheZoneReset + I_Error StageTextHi rows)
# pushed __bend 0x50 past the ceiling; 200 bytes off the heap pays again.
# 204,800 -> 202,496 (2026-09-06 OCRAM): the sat_colstate_t WRAM twin
# (1926 B, saturn_segs.c) pushed __bend 0x800 over; the heap pays 1:1.
# (2026-09-06 divtab attempt: 202,496 -> 201,216 for the 1280 B LUT ??????
# RETIRED, divtab eaten by libgcc variable-shift calls; heap restored.)
# 202,496 -> 201,472 (2026-09-06 VDP1 wall prototype): __bend landed
# 0x350 over the ceiling with the prototype's state added; the heap
# pays 1:1 again.
# 201,472 -> 200,960 (2026-09-07 v-correct quad cells): the 16-cell
# quad table (v1q 8->16) plus the cell-split state put __bend 0xF0
# over; the heap pays 512 more.
# 200,960 -> 200,448 (2026-09-07 gouraud shading): the shade slot
# (v1q 9->10) pushed __bend 0x70 over; the heap pays 512 more.
# 200,448 -> 199,936 (2026-09-07 u-phase clip): v1q 10->13 shorts
# (clip rect) pushed __bend 0x1B0 over; the heap pays 512 more.
# 199,936 -> 198,400 (2026-09-07 perf-pass build): __bend read
# 0x060c0400 (+0x400 over the ceiling) at the PERF_ROWS baseline;
# grower unexplained (perf rows add ~0 BSS) - heap pays 1536, and the
# production build must re-derive the delta before shipping.
# 2026-09-08: the 256-cell quad-table experiment REVERTED (table was
# never the cap ???????? census 21 < 32; kills live in saturn_v1w_commit, now
# counted). Heap back to the 05:09-proven value; __bend back to ~f7a0.
# 2026-09-08 STAGE 2a NOTE: shrinking this to 191680 (-4096) wedged
# Kronos boots (teal P_LoadingPlaque hold, then crash-reset loops at
# jo_main dark blue) ???????? REVERTED; the known-good value stays. The BSS
# wall overrun (__bend ~0x910 over 0x060C0000) is tolerated by Kronos
# (the preskip-v2 build booted+played at ~0x060C08A0); revisit with a
# real-BSS reclaim before the next real-HW burn.
# 2026-09-08 STAGE 3A: re-enabling the stage (+0x580 .text in
# saturn_segs.o) pushed jo's sprite arrays past the 0x060C0000 user-RAM
# ceiling ???????? jo_sprite_init's JO_DEBUG check fired ("Please reduce
# JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC"). Pool trimmed by exactly the .text
# delta so __bend/work cells return to the BYPASS=1 layout that booted
# E1M1 in Kronos at 21:28 ???????? zero net layout shift.
# 2026-09-08 GUN DIAG rev 5 (temporary h-row repoint): canary A = wall
# texture (known good), B = weapon texture (+0x80 total) ???????? pool trimmed
# 1:1 so __bend returns to 0x060C0EE0.
# 2026-09-08 GUN DIAG rev 6: canary F (slot 0, full-screen, wall tex) +
# canary L (last slot, bottom half, weapon tex) replace the rev-5 pair
# (+0x60 net) ???????? pool trimmed 1:1, __bend back to 0x060C0EE0.
# 2026-09-08 GUN DIAG rev 7 (h-row repoint to the slot3/user0 vbl
# samples): Saturn_Gun_Debug body SHRANK by 0x320 ???????? pool GROWN back 1:1
# so __bend returns to 0x060C0EE0 (rev-7 first boot with the pin broken
# wedged static DarkCyan in D_DoomLoop frame 1: the shift moved SGL
# lib-common work cells).
# 2026-09-08 GUN DIAG rev 8 (COPR probe on the i row: walk slot index
# at draw end): .text GREW 0x20 ???????? pool trimmed 1:1, __bend back to
# 0x060C0EE0.
# 2026-09-08 GUN DIAG rev 10 (VDP1-FB word probes on the h/q/j rows):
# .text GREW 0x20 ???????? pool trimmed 1:1, __bend back to 0x060C0EE0.
# 2026-09-09 STAGE-3A CLOSE: -DSATURN_V1W_TESTQUAD removed (both canaries
# verified) ???????? .text SHRANK 0x140 ???????? pool GROWN back 1:1, __bend back to
# 0x060C0EE0.
# 2026-09-09 FULL-VDP1 WALLS: quad table 32->64 cells (v1q +896 B .bss,
# saturn_vdp1.h V1W_MAX_QUADS) + the FULL gate's .text delta ???????? pool
# trimmed 1024 so __bend holds at ~0x060C0EE0 (Kronos-tolerated).
# 2026-09-09 FULL+noQOFF link: first link measured 0x060C10E0 ???????? the FULL
# .text estimate was 512 short; pool trimmed the missing 512 (193120 ->
# 192608), __bend landed 0x060C0AE0. THAT BOOT WEDGED (static DarkCyan
# = first D_DoomLoop frame, no glyphs, no red watchdog). All FULL-gate
# code loops are bounded on review, so the prime suspect is the LAYOUT
# ritual violation: every booting build holds __bend at exactly
# 0x060C0EE0 (the 1:1 dance keeps the SGL lib-common work cells, linked
# last in .bss, at proven addresses; +28B shifts wedged 3/3 on
# 2026-09-08). Restore the ritual: pool +1024 -> 193632, __bend back to
# 0x060C0EE0 with IDENTICAL code. If it still wedges, the FULL gate
# itself convicts.
# 2026-09-09 MENU RESTORE: d_main.o menu-ON grew .text 0x20 ???????? pool
# trimmed 1:1 (193632 -> 193600), __bend held at 0x060C0EE0.
# 2026-09-09 5THPLANET CRASH ROOT-CAUSE: __bend drifted to 0x060C1020 (+0x140
# over the ritual 0x060C0EE0) un-rebated, shifting the SGL lib-common tail up
# so cpk_hn_tbl (0x060BFFF0, 0x80) straddles 0x060C0000 - SGL's static SortList
# sort-table base. SGL BlankOut's vblank ch1 INDIRECT DMA then walks game BSS
# bytes as {size,dst,src} triplets -> 32MB of 0x0000/0xFFFF swept across all
# WRAM-H mirrors -> vectors/IRQ dispatcher destroyed -> master PC=0 (repro'd
# deterministic in 5thPlanet ~frame 1250; control game_noskip2.iso clean).
# Trim pool 1:1 (193632 -> 193312) so __bend returns to the ritual
# 0x060C0EE0 and cpk_hn_tbl to 0x060BFEB0.
# 2026-09-09 REAL-HW BISECT (UI-OFF): the user's real Saturn freezes on
# the CYAN STARTING fill with NBG0 garbage showing through (photo
# 072106) - the wedge is between d_main.c STARTING and the first title
# I_FinishUpdate commit. The title/menu path has NEVER run on real HW
# (burn2 went straight into E1M1). Bisect build: SATURN_UI_MENU=0 ->
# the r104-era boot path (straight into E1M1). Pool re-tuned 1:1 after
# the .text delta so __bend holds at 0x060C0EE0. NOTE: the ritual layout itself
# still overlaps 0x060C0000+ and always has - the operative invariant is
# the exact ritual address set (SGL's table reads the ritual bytes fine);
# it is the UN-REBATED DRIFT that changes the bytes at 0x060C0000 and is
# catastrophic. Any future .text/.bss growth must keep rebating the pool.
# 2026-09-09 5thPlanet cross-check: fixed ISO + 4MB cart boots clean to the
# DOOM title (3600+ frames, id_seen=0x5C, mainzone on cart). Cartless runs
# hit ZMALLOC-BADSIZE at frame 1837 (BupPrealloc Z_Malloc(0x4000) vs the
# 12KB z_zone_pool fallback) - pre-existing since round-34, cart-independent,
# NOT a trim regression.
# 2026-09-09 PREAD_TRACE SUPPRESSION BUILD: i_wad.o's trace rodata shifted
# the BSS phase +0x360 (__jo_sprite_def end crossed the 0x060C0000 SGL
# sort-list line ??? jo_sprite_init's JO_DEBUG guard fired the blue error
# screen on real HW, photo 093643). Pool trimmed 1:1 by the +0x360 so
# __bend returns to the 09:05 layout (0x060C1030) that cleared jo init.
# 2026-09-09 BOOT-WEDGE GATE BUILD: the VBl-Out gate experiment (main.c
# sets bit 1 of 0x060FFC1B after jo_core_init; d_main.c clears it at
# menu-up) adds ZERO BSS, but the first link measured __bend 0x060C1050
# = +0x20 over the suppression-build ritual (0x060C1030, the layout the
# boot1/2/3.yss forensics assume). Source of the +0x20 is un-rebated
# drift from edits between that build and this one. Pool trimmed 1:1 by
# the +0x20 (192448 -> 192416) so __bend returns to exactly 0x060C1030
# and the SGL lib-common work cells (cpk_vide/cpk_audi/cpk_deb, snd,
# GFS/gfcd, errno) land back on the forensics addresses.
# 2026-09-10 P4 (PVS blob): the Saturn_PvsLevelLoad/seg_pvs_locate/gate
# code grew .text/.rodata by 0x380, pushing the whole .bss phase (first
# link measured __bend 0x060C13B0). Pool trimmed 1:1 (192416 -> 191520)
# so __bend returns to exactly 0x060C1030 and the SGL lib-common work
# cells land back on the forensics addresses. (The 39 KB blob itself is
# PU_LEVEL zone memory, NOT pool/BSS.)
# 2026-09-10 P5 Phase 1 (quad table -> zone): the 64-cell v1q array
# (1792 B) left .bss for a Z_Malloc'd 512-cell cart-zone table (14 KB,
# NOT pool/BSS); the new pointer/cap/accessor cost ~80 B. First link
# measured __bend 0x060C0980 = 0x6B0 UNDER the ritual. Pool GREW 1:1
# (191520 -> 193232) so __bend returns to exactly 0x060C1030.
# 2026-09-10 P5 Phase 2b cost trim (need_u: capture skips DIVU/u/t_u/
# shade/lastsec on no-mid capture columns; PVS4 fps 60 -> 54): net
# .text +0x20; pool GREW 1:1 (194800 -> 194832) so __bend returns to
# exactly 0x060C1030.
# 2026-09-10 P5 Phase 2b trim 2 (PVS6: duds=19/frame = dead-face re-arm
# commits; re-arm now admission-latched): net .text +0x60; pool GREW
# 1:1 (194832 -> 194928) so __bend returns to exactly 0x060C1030.
# 2026-09-10 fps display fix (the 1-px decimal point made "5.4" read
# as "54" ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â 10x census mis-scale; now a 2x2 dot): net .text -0x40;
# pool SHRANK 1:1 (194928 -> 194864) for the same 0x060C1030.
# 2026-09-10 fps moved to the status-bar metal (fb row 184, x=90; the
# user's TV crops the top ~8 lines so the row-2 readout never reached
# the screen): net .text -0x20; pool SHRANK 1:1 (194864 -> 194832) for
# the same 0x060C1030.
# 2026-09-10 timing-split census rows (r/f/t/v/e vbl rows replace the
# served-out FRT-BSP/quad0-pw/second-reason/benign/last-flags rows):
# net .text +0x90; pool GREW 1:1 (194832 -> 194976) for the same
# 0x060C1030.
# 2026-09-10 input triage rows (B = ticbuttons|paused, I = live tic count;
# kill-census and dud-census rows sacrificed, restore after triage): net
# .text +0x40. NOTE: bend tracks pool AND .text both +1:1 (this session's
# two-point data: +0x40 text -> +0x40 bend at constant pool; +0x40 pool ->
# +0x40 bend at constant text). The Phase-3-era pool GREW WITH .text rule
# was a .bss-shrink coincidence, not a text rule. Pool SHRANK 0x40
# (195232 -> 195168) for the same 0x060C1030.
# 2026-09-10 P5 Phase 3 (masked mids into the depth sort: per-wall runs +
# AddMid at the wall's own table position; flush prescan retired under
# SATURN_V1W_ALL; gate reason 9 re-scoped to software grates only; p row
# repointed to rj[9]): net .text +0x100; pool GREW 1:1 (194976 -> 195232)
# for the same 0x060C1030.
# 2026-09-10 input triage step 2 (B = RAW jo_inputs[0].data pad word,
# I = (pad id byte<<8)|tic count low byte; the SGL JO_KEY enum proved
# bit-identical to SGL's PER_DGT_* constants so the mapping table is
# exonerated — the raw word now fingerprints the format, the sampling
# cadence, and lost key-ups): net .text -0x40; pool GREW 1:1 (195168 ->
# 195232) for the same 0x060C1030.
# 2026-09-10 whole-code bug hunt fix 1 (saturn_menu.c title
# cooldown/timeout were MENU FRAME counts at multi-vbl menu cadence =
# ~7.5 s of dead input; converted to VBLANK deltas with a crossing
# test for the attract countdown): net .text +0x20; pool SHRANK 1:1
# (195232 -> 195200) for the same 0x060C1030.
# 2026-09-16 M2 retail-leaf renderer (saturn_leaf.o loader/accessors +
# seg_subsector_leaf branch + Saturn_Leaf_BlobRead + P_SetupLevel
# LEAF-LOAD slot): first link measured __bend 0x060C1560 = +0x530 over
# the ritual. Pool trimmed 1:1 (195200 -> 193872) so __bend returns to
# Pool adjusted (145488 -> 144768) so __bend returns to exactly 0x060C1030 for FPS counter.
# Pool adjusted (144768 -> 165680) so __bend returns to exactly 0x060C1030 for double-buffer and culling.
# Pool adjusted (165680 -> 142560): 64-grid flat table restored (2900 quads, +0x5A50) after review.
# Pool adjusted (142560 -> 142016): perf pass (persistent VDP1 texture cache, frustum half-space cull, +0x220).
# Pool adjusted (142016 -> 141136): perf HUD (per-phase ms + quad/drop counts, +0x370).
# Pool adjusted (141136 -> 140944): 1D occlusion culling, Lobotomy DIVU pipelining, WRAM quad/tag tables (+0xC0).
# Pool adjusted (140944 -> 142960): SATURN_DIVLUT_MODE=FULL moved LUT table from .bss to OCRAM scratchpad (-0x7E0).
# Pool adjusted (142960 -> 143696): revert flawed 1D hw_occ, increase V1W_MAX_QUADS to 1536 (-0x2E0).
# Pool adjusted (143696 -> 142608): clean 1D subsector occlusion culling (+0x440).
# Pool adjusted (142608 -> 141776): wall perspective correction & 64-unit repetition subdivision (+0x340).
# Pool adjusted (141776 -> 142480): fast 3D perspective subdivision with distance LOD (-0x2C0).
# Pool adjusted (142480 -> 142064): node & subsector bounding box frustum culling (+0x1A0).
# Pool adjusted (142064 -> 141712): solid 2-sided wall occlusion & node BBox 1D occlusion culling (+0x160).
# Pool adjusted (141712 -> 141648): eliminate 64-bit software divisions & flat fast-path (+0x40).
# Pool adjusted (141648 -> 140432): fast baked flat quad emission & subpixel sliver culling (+0x4C0).
# Pool adjusted (140432 -> 140176): OCRAM reciprocal LUT division and shade integration (+0x100).
# Pool adjusted (140176 -> 139792): fix HW_CheckBBox 12-bit shift bug, flat distance cull, flat profiler (+0x180).
# Pool adjusted (139792 -> 188512): move baked flat tables from .rodata to CD-streamed .FLT lumps (-0xBE50).
# Pool adjusted (188512 -> 188480): calibrate __bend to 0x060C1030 (+0x20).
# Pool adjusted (188480 -> 188272): JO_MAX_FS_BACKGROUND_JOBS=2 + WRAM-L flat streaming, calibrate __bend to 0x060C1030 (+0xD0).
# Pool adjusted (188272 -> 188208): bypass letterbox scan & saturn_vdp2_copy_back rows 96..167 during gameplay, calibrate __bend to 0x060C1030 (+0x40).
# Pool adjusted (188208 -> 187840): saturn_baked_segs module + single-step seg renderer, calibrate __bend to 0x060C1030 (+0x170).
# Pool adjusted (187840 -> 187904): generic PVS1 loader for all maps E1M1-E1M9, calibrate __bend to 0x060C1030 (+0x40).
# Pool adjusted (187904 -> 186784): baked textures & sprites loaders (E1M1-E1M9), calibrate __bend to 0x060C1030 (-0x460).
# Pool adjusted (186784 -> 185248): JO_FS_MAX_FILES expanded from 32 to 96 (fixes FONT.TGA / 62 files), calibrate __bend to 0x060C1030 (-0x600).
# Pool adjusted (185248 -> 185152): texture staging buffer free & exact sprite sizing, calibrate __bend to 0x060C1030 (-0x60).
# Pool adjusted (185152 -> 183616): unified level asset package loader (saturn_pak.c, E1M1-E1M9), calibrate __bend to 0x060C1030 (-0x600).
# Pool adjusted (183616 -> 182880): full reciprocal LUT, sprite culling/sorting, unrolled quad transforms, calibrate __bend to 0x060C1030 (-0x2E0).
# Pool adjusted (182880 -> 182480): full 36-map support, TEXTURE2 runtime fallback fix in saturn_tex.c, calibrate __bend to 0x060C1030 (-0x190).
# Pool adjusted (182480 -> 181136): non-culling pipeline optimizations, inlined quad emission, unrolled transforms, calibrate __bend to 0x060C1030 (-0x540).
# Pool adjusted (181136 -> 183008): remove premature 1D occlusion and distance culls, restore seg clipping, calibrate __bend to 0x060C1030 (+0x750).
# Pool adjusted (183008 -> 182752): fast Bounding Sphere frustum culling via SH-2 MAC in HW_CheckBBox, calibrate __bend to 0x060C1030 (-0x100).
# Pool adjusted (182752 -> 183104): offline quad sliver pruning and fast unrolled rotation scoring in HW_EmitFlatQuadFast, calibrate __bend to 0x060C1030 (+0x160).
# Pool adjusted (183104 -> 183456): HW_MulHigh vertex projections, HW_ClampedFrac seg division, HW_EmitFlatQuadDirect, calibrate __bend to 0x060C1030 (+0x160).
# Pool adjusted (183456 -> 177840): 256-entry HW_GetVertex vertex cache & dead-math removal, calibrate __bend to 0x060C1030 (-0x15F0).
# Pool adjusted (177840 -> 177456): struct-pointer vertex cache, direct wall quad emitter, empty portal early-out, calibrate __bend to 0x060C1030 (-0x180).
# Pool adjusted (177456 -> 177488): seg early-rejection hoist & quad loop array hoist, calibrate __bend to 0x060C1030 (+0x20).
# Pool adjusted (177488 -> 177136): unrolled flat quad switch emitter & simplified flat bounds, calibrate __bend to 0x060C1030 (-0x160).
# Pool adjusted (177136 -> 176976): restore subpixel sliver culling, calibrate __bend to 0x060C1030 (-0xA0).
# Pool adjusted (176976 -> 175936): fast 2D map-space AABB frustum test & single-step seg path, calibrate __bend to 0x060C1030 (-0x410).
# Pool adjusted (175936 -> 176144): fast 16-bit MAC flat transforms & conditional seg clamping, calibrate __bend to 0x060C1030 (+0xD0).
# Pool adjusted (176144 -> 175568): bypass volatile RAM reads in FastDiv/Shade, outside-frustum fast accept, seg height hoisting, flat & wall clamping bypass, calibrate __bend to 0x060C1030 (-0x240).
# Pool adjusted (111104 -> 110080): Reclaim 1,024 bytes BSS headroom below 0x060C0000 safety floor (1,680 bytes headroom).
# Pool adjusted (110080 -> 102400): Reclaim 7,680 bytes to bring __bend and __jo_sprite_def completely below 0x060C0000 user RAM ceiling.
# Restored full 100 KB heap for Saturn_CmdBuf_Init (sub_ranges 6KB + cmd buffers 74KB + Jo boot allocations).
# Headroom below 0x060C0000 reclaimed via JO_MAX_SPRITE=64 (-6.7KB) and SATURN_FILL_QUEUE_MAX=768 (-4KB) to support JO_FS_MAX_FILES=208 for all 192 CD files.
JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC = 102400
JO_MAX_SPRITE = 64


# Sources for the boot milestone. i_wad.c owns the single cart probe
# (Saturn_CartInit); main.c just reports its cached result.
SRCS = main.c \
       doom/src/d_main.c \
       doom/src/doomstat.c \
       doom/src/dstrings.c \
       doom/src/g_game.c \
       doom/src/i_sound.c \
       doom/src/i_system.c \
       doom/src/i_video.c \
       doom/src/i_wad.c \
       doom/src/m_argv.c \
       doom/src/m_cheat.c \
       doom/src/m_misc.c \
       doom/src/saturn_render.c \
       doom/src/saturn_segs.c \
       doom/src/saturn_leaf.c \
       doom/src/saturn_vdp1.c \
       doom/src/saturn_cmdbuf.c \
       doom/src/saturn_hw_render.c \
       doom/src/saturn_vdp2_plane.c \
       doom/src/saturn_baked_flats.c \
       doom/src/saturn_baked_segs.c \
       doom/src/saturn_pak.c \
       doom/src/saturn_dsp.c \
       doom/src/saturn_slave.c \
       doom/src/saturn_tex.c \
       doom/src/saturn_vgl.c \
       doom/src/saturn_hud.c \
       doom/src/saturn_menu.c \
       doom/src/saturn_inter.c \
       doom/src/saturn_demo.c \
       doom/src/st_stub.c \
       doom/src/s_sound.c \
       doom/src/d32xr_stubs.c \
       doom/d32xr/info.c \
       doom/d32xr/p_base.c \
       doom/d32xr/p_ceilng.c \
       doom/d32xr/p_change.c \
       doom/d32xr/p_doors.c \
       doom/d32xr/p_enemy.c \
       doom/d32xr/p_floor.c \
       doom/d32xr/p_inter.c \
       doom/d32xr/p_lights.c \
       doom/d32xr/p_map.c \
       doom/d32xr/p_maputl.c \
       doom/d32xr/p_mobj.c \
       doom/d32xr/p_move.c \
       doom/d32xr/p_plats.c \
       doom/d32xr/p_pspr.c \
       doom/d32xr/p_setup.c \
       doom/d32xr/p_shoot.c \
       doom/d32xr/p_sight.c \
       doom/d32xr/p_slide.c \
       doom/d32xr/p_spec.c \
       doom/d32xr/p_switch.c \
       doom/d32xr/p_telept.c \
       doom/d32xr/p_tick.c \
       doom/d32xr/p_user.c \
       doom/d32xr/r_data.c \
       doom/d32xr/r_main.c \
       doom/d32xr/sounds.c \
       doom/d32xr/tables.c \
       doom/d32xr/w_wad.c \
       doom/d32xr/z_zone.c

JO_ENGINE_SRC_DIR=../../jo_engine
COMPILER_DIR=../../Compiler
CCFLAGS += -I./doom/src -I./doom/d32xr
CCFLAGS += -fpermissive
CCFLAGS += -DMARS=1 -DSATURN=1
CCFLAGS += -DJO_FS_MAX_FILES=208
CCFLAGS += -fno-strict-aliasing

include $(COMPILER_DIR)/COMMON/jo_engine_makefile

# saturn_draw.s must NOT go into SRCS: OBJS = $(SRCS:.c=.o) would leave
# the raw .s in the clean target's RM list (make re DELETED the source
# once). Add the object explicitly instead.
OBJS += doom/src/saturn_draw.o

# jo_engine_makefile's "game.elf: $(OBJS)" prerequisite list is expanded
# when the rule is PARSED ? before the += above runs ? so make never
# schedules saturn_draw.o for building (the recipe still references it,
# since recipes expand late). Re-attach the prerequisite; make merges
# prerequisite lists for the same target.
game.elf: doom/src/saturn_draw.o

# Per-file -O2 for the render hot path. jo_engine_makefile compiles EVERY
# game source with -Os (its :264, one generic .c.o suffix rule at :333);
# -Os measurably hurts the raycaster's inner loops. Explicit rules beat
# GNU make's suffix rule, and `clean` still works because OBJS keeps all
# objects. Size gate: saturn_render.o's .text grows under -O2, which eats
# the .bss headroom 1:1 ??? after any -O2 list change, game.map's __bend
# must stay < 0x060C0000. If it overflows: drop -funroll-loops first, then
# remove saturn_tex.o from this list.
CCFLAGS_OPT = $(filter-out -Os,$(CCFLAGS)) -O2

# BSP/seg wall renderer (saturn_segs.c): hot ? -O2 + the build flag
# that switches both render dispatches to it. The flag lives HERE
# (not CCFLAGS) so only these two objects see it.
#   Measurement variants (garbage image, A/B/C protocol): add
#   -DSATURN_SEG_NULLWALL=2 (= C: traversal only) or =1 (= B: traversal
#   + wall math, no wall pixels). Wall-math cost = D_B - D_C,
#   wall-pixel cost = D_A - D_B. Remove the flag for normal builds.
# PRODUCTION (2026-09-02 02:40): -DSATURN_SEG_DIFF removed from all three
# rules after the diff campaign proved parity (#62-#69: pegs/texnums/spans
# equal every frame; remaining diff red = 0-13-texel u-drift, d32xr-
# authentic). To re-run the seg-vs-raycaster diff: add -DSATURN_SEG_DIFF
# back to saturn_segs.o, i_video.o AND saturn_render.o below.
# Round 25 (2026-09-02 19:55): production seg build again. A bisect
# session left -DSATURN_SPRITES_OFF (no things) and -DSATURN_FLAT_DIAG
# (every flat a solid colour + red run outlines + magenta/black run
# strokes) on these rules ? the 19:42 image shipped them, and the
# "white floor/sky, red seams, magenta bars" screenshot of 19:47 is
# that diagnostic overlay, not a renderer bug. Both removed.
# 2026-09-08 STAGE 1 (operator: full VDP1/VDP2 move, software = reference):
# -DSATURN_VDP1_WALL restored on saturn_segs.o ???????? quad capture + paint-skip
# live, kill census (B/S/j rows) decides the dominant kill fix.
# Control build = remove this define only (saturn_vdp1.o keeps its define;
# i_video.c census rows reference its symbols).
# 2026-09-09 RED-BLOCK BISECT: the salmon blocks = the 0x20 fb CLEAR
# (saturn_render.c ~3976 "mid-tone") showing where NOTHING painted ????????
# palette math proved it (uniform 153,49,50 = PLAYPAL[32] shade 0, no
# fallback base matches). -DSATURN_V1W_NOSKIP forces the software paint
# everywhere (wall faces + masked mids); quads still draw on top.
# Red gone = quad side broken (missing/displaced quads); red persists
# = software coverage hole (fill/clip). REMOVE FOR PRODUCTION.
# 2026-09-09 CENSUS REPOINT BUILD: production config restored (preskip
# live ???????? the rj counters only move in this config); i_video.c rows
# repointed to decode the rj breakdown at the bug view:
#   q=(id1<<8|c1) top actionable reject  j=skipcols  h=skipdie
#   F=rj[3] MIDTEXTURE  s=rj[9] mmid grate.
# 2026-09-09 SOFTWARE-RENDER CONTROL (operator request, testing): the
# define OFF = no quad capture, no paint-skip ???????? software owns every
# wall; the quad table stays empty. ISO preserved as game_softrend.iso.
# 2026-09-09 FULL-VDP1 WALLS (operator: "move to full vdp1 rendering,
# forget software rendering"): -DSATURN_V1W_FULL drops capture-gate
# reason 8 (open-clip-band) and the 8-row minimum span. Quads now
# cover the face's FULL projected span; occlusion = painter's draw
# order (walk near-to-far, writer reverses far-to-near). The residual
# software wall paint is a fallback only: masked-mid-grate columns
# (reason 9 kept), nearer-sprite columns (reason 10 kept), safe-range
# / degenerate-u / texture-refuse runs, solid-mid doors, and any
# column after the quad table fills (saturn_v1w_captured). Census
# rj[] + skipcols/skipdie decide the next kill to fix.
# 2026-09-09 NOSW TRIAGE RETIRED: -DSATURN_V1W_NOSW=1 (software wall
# paint off, quads-only) decoded from red walls.yss ??? the red regions
# are the 0x20 clear where NO quad was committed (capture-gate rejects
# 8 clip-band ??45, 10 sprite/weapon ??251, 9 mmid, + benign) and the
# honest hybrid paints those columns in software. NOSW removed from
# the rule and the two ifdef sites; the census-driven gate kills
# (8 clamped-commit, 10 draw-order) are the next fixes, not NOSW.
doom/src/saturn_segs.o: doom/src/saturn_segs.c
#	(SOFTREND CONTROL 2026-09-09, IN-LOOP wedge bisect: built once with
#	only -DSATURN_SEG_RENDERER ??? no quad capture, no paint-skip ??? then
#	renamed game_softrend.iso; defines restored for production.)
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_SEG_RENDERER -DSATURN_LEAF_RENDERER -DSATURN_VDP1_WALL=1 -DSATURN_V1W_FULL=1 -DSATURN_V1W_ALL -DSATURN_PVS -o $@
# (M2 CONTROL BISECT 09-16: -DSATURN_LEAF_RENDERER REMOVED from this rule
# for one build — the orange level-start wedge A/B. saturn_leaf.o keeps
# its define so its code still links; with this flag off seg_subsector
# never consults leaf_active(), i.e. the exact production build one
# step before the leaf branch went live. Restore for production.)
# M2 (2026-09-16 retail-leaf renderer): -DSATURN_LEAF_RENDERER switches
# seg_subsector to the LEAF blob's edge runs (saturn_leaf.c) while the
# wall painters stay untouched; any load/validation failure fail-opens
# to the plain SSECTORS seg walk. saturn_leaf.o carries the define too
# (its accessors are #ifdef'd behind it).
doom/src/saturn_leaf.o: doom/src/saturn_leaf.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_LEAF_RENDERER -o $@
# (2026-09-07: -DSATURN_V1W_FORCE_SHADE removed ?????? its additive gouraud
# rate tables blew dark texels out to white (the pale strip). Shading
# now = shaded CRAM banks (COLORMAP rows 0,4,..,28), exact vs software.
# STRIPES removed 2026-09-07 after the u-phase A/B diff (stripes build
# vs production, same key-driven view) showed quad cells pixel-identical.)
# (SPRCLIP_DIAG removed 2026-09-06 for the VDP1 wall prototype verify:
# the white diagnostic rows garbled the scene and union'd the V1 row.)

# VDP1 scaled-sprite wall prototype (2026-09-06, DoomFS2 thesis):
# walls as VDP1 distorted sprites. saturn_segs.o carries the same
# define (quad capture in the wall walk); i_video.o only calls the
# functions so it needs no define. Remove BOTH defines to disable.
# (2026-09-08 CONTROL BUILD: the define STAYS here ???????? i_video.c's census
# rows reference saturn_v1w_cells/slot3_at_vbl which only exist under it.
# The control comes from saturn_segs.o losing the define: no capture,
# no paint-skip, the quad table stays empty. Restore the segs.o define
# for the wall prototype.)
doom/src/saturn_vdp1.o: doom/src/saturn_vdp1.c
	$(CC) $< $(CCFLAGS) -DSATURN_VDP1_WALL=1 -o $@

doom/src/saturn_cmdbuf.o: doom/src/saturn_cmdbuf.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

SATURN_HW_DEPS = doom/src/saturn_hw_internal.h doom/src/saturn_hw_occl.c doom/src/saturn_hw_math.h doom/src/saturn_hw_sky.c doom/src/saturn_hw_sprites.c doom/src/saturn_hw_flats.c doom/src/saturn_hw_walls.c
doom/src/saturn_hw_render.o: doom/src/saturn_hw_render.c $(SATURN_HW_DEPS)
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_VDP1_WALL=1 -DSATURN_LEAF_RENDERER -DSATURN_PVS -o $@

doom/src/saturn_baked_flats.o: doom/src/saturn_baked_flats.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

doom/src/saturn_pak.o: doom/src/saturn_pak.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@
# (2026-09-09 FULL-VDP1 WALLS: -DSATURN_V1W_QOFF=40 REMOVED ???????? that was
# the paint-skip forensics ghost-shift (quads 40 px right of their true
# faces); the FULL build's quads must land exactly on their faces.)
# (2026-09-09: -DSATURN_V1W_TESTQUAD REMOVED for stage-3a close. Both
# canaries verified in the Kronos Debug VDP1 command list (rows 5+/tail)
# and on screen; canary1 full-screen-tiling v1q[count-1]'s texture was
# polluting the live view (the "wall mosaic" left half, and the colorful
# noise when firing = weapon/flash texture tiled). Sprite verification
# continues via the live view + census rows only.)
# (2026-09-06: -DSATURN_OCRAM_STATE=1 removed ?????? real-HW verdict, see
# saturn_cache.h: the probe's CCR write wedged the bus twice at boot
# (green OCPRB freeze, reseat + retry). sat_colstate stays in WRAM; the
# cs-> plumbing is unconditional, only the pointer redirect needed the
# define. Heap stays 202496 ?????? the 1926 B WRAM twin is now the only copy.)

# PERF PASS (2026-09-03): -DSATURN_PERF_ROWS re-enables the frame-budget
# HUD rows (D/T/W/L/q/p/j/i/x/F/w) for the optimization session ? one
# fps_num per row, no union hazard, all rows <= 136. Remove for
# production once the pass banks its numbers.
# Round 29 (2026-09-04): PERF_ROWS OFF ? the whole probe HUD (1408..1876)
# wiped fb cols 46..89 on every 8-line band = the photographed
# "corruption" column at screen x 92..176. The display path itself was
# proven bit-correct by the -DSATURN_FB_TESTPAT probe boot (40 bands,
# edge-to-edge, 2:1 zoom applied). Re-add the define to bank numbers;
# kill it to see the clean picture.
# Round 30b (2026-09-09): SEG_SHOW on i_video.o retired with the
# saturn_render.o bisect (cyan boot wedge, 3/3).
doom/src/i_video.o: doom/src/i_video.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_PERF_ROWS $(DIVLUT_DEFS) -o $@
# (2026-09-07: -DSATURN_PERF_ROWS = the perf-pass baseline build ?????? fps
# row + D/i/W rows re-enabled in i_video.c. Remove for production.)

# FILL-SYNC BISECT (2026-09-05): r103/r105 wedge repro boot ? force the
# background cart fill through the sync copy path (no GFS_NwFread batch).
# Paired with the SATURN_FILL_SYNC_ONLY guard in i_wad.c. Diagnostic
# only: remove this rule for production.
# REMOVED 2026-09-05 (burn 2): burn 1 (sync-only + cached staging) passed
# its in-game leg; the async Nw path returns under the v5.13 staging
# tripwire. Re-add -DSATURN_FILL_SYNC_ONLY=1 here if the wedge returns.

# FILL-SYNC RE-ADDED 2026-09-09, RETIRED 2026-09-09 (verdict): the
# sync-only build STILL wedged real HW with the IDENTICAL cyan dithered
# garbage (photo 20260909_072106) ??? the async GFS_NwFread path is
# EXONERATED as the cause. Root cause found in saturn_vdp1.c instead:
# Saturn_Vdp1Wall_Init sets v1_have_frame=1 at INIT time, so from the
# first committed frame (pre-WAD smoke test) the vblank callback patched
# SGL's slot-3 END into LOCAL_COORD and the VDP1 auto-draw walked into
# the NEVER-WRITTEN user command region (the menu path commits zero
# quads, so v1_write_commands' END marker never lands) ??? real HW executed
# power-up VRAM garbage as draw commands. Fix = Init arms an END marker
# at V1_USER0 (see saturn_vdp1.c). Standard async build restored; the
# wedge fires BEFORE any fill choice matters, so the bisect is done.
# Re-add -DSATURN_FILL_SYNC_ONLY=1 ONLY if a fill-path wedge ever
# returns (and delete doom/src/i_wad.o first).

# Tier kill switches (saturn_tex.c) back to default = ON. The BISECT
# build (-DSATURN_T0_OFF=1 -DSATURN_T1_OFF=1) showed the render window
# (R row) at ~23 vbl with every texel read from the cart's A-bus;
# re-enabling the WRAM-L and WRAM-H hot caches tests whether placement
# was ever the problem or the earlier "tiers hurt" read was noise.
# doom/src/saturn_tex.o: doom/src/saturn_tex.c
# 	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_T0_OFF=1 -DSATURN_T1_OFF=1 -o $@

# DUAL-SH-2 MEASURED, REVERTED (2026-08-31 19:16): R 3 -> 40 vbl with
# exec/join live (slave handshake under this main-loop boot path costs
# ~35+ vbl/frame; picture renders but fps drops 4.0 -> 3.1). Single
# core stays. Phase-4 fallback options if ever revisited: hand-rolled
# SMPC SETSINT slave-launch per jo_engine/slave.c:154-181, or offload
# the I_FinishUpdate expand to the slave instead of column-splitting.
# RE-ENABLED (2026-09-01, user: "fix performance first"): the 19:16
# numbers predate the 74->16 vbl optimization sweep (walk-deferral,
# candidate cache) AND that era's digit misreads ("old J=34 chase").
# Healthy math now: master half ~8 + slave half ~8-9 -> D ~11-12 vbl
# (~5-6 fps). The slave cb enables its OC at entry if SGL left it off
# (saturn_prof_sccr* rows), purges stale tables, and the ticket guard
# blocks stale re-entries. HUD verdict rows: D = whole render window,
# R = join (dispatch..join), n = slave half's own vbl, E = slave CCR
# after enable (bit0 = OC on). If D lands ~30+ the slave is still
# sick -> flip this flag back and read sccr/join/slave again.
# asm-kernel bisect: -DSATURN_NO_ASM_FLAT=1 / -DSATURN_NO_ASM_WALL=1
# disable one kernel; -DSATURN_NO_ASM_DRAW=1 disables both.
# BISECT HISTORY (2026-09-01): the R1 freeze was FOUR stacked causes in
# saturn_draw.s, all fixed and sim-proven (sim/sim_wall.py, sim_flat.py
# run the ACTUAL linked bytes vs the C loops): swap.w high-half garbage,
# unencodable mov.w stack load silently mis-encoded by no-arch-flag gas,
# fb stride 512 instead of 320, and SHAD/SHLD (SH-3+ only!) ? both
# kernels now use dmuls/swap.w+exts.w instead. Wall kernel: HW-VALIDATED
# (renders, textured walls). This build: flat kernel ON as well.
# NOTE: this is the ONE saturn_render.o rule (make takes the LAST rule
# for a target). PRODUCTION: seg renderer only, no diff pass. Add
# -DSATURN_SEG_DIFF here (and to saturn_segs.o / i_video.o above) to
# re-enable the seg-vs-raycaster mismatch view (~3 fps).
# -DSATURN_SEG_SHOW (2026-09-02): fallback-identification view ? dark-red
# unpainted clear, white sky fallback, salmon ceil-flat / brown floor-flat
# fallbacks, and green/magenta/cyan/yellow wall-fallback reason colors.
# Diagnostic only: remove the define for production.
# Round 25 (2026-09-02): production painters (asm kernels + COLORMAP
# shading, no SEG_SHOW, no SH_SPLIT). Fill-overwrite fix, round 25
# edition: the clip narrows d32xr-authentic (NEWFLOOR/NEWCEILING only ?
# the round-23 SOLIDSIL close was wrong and caused solid-red unpainted
# slabs), and the final open-band fill now skips any column a textured
# SOLID wall painted (nearpaint[] flag). Round 24's first-painter span
# was the wrong span: when a nearer two-sided wall (step face / opening
# jamb) painted other rows first, the fill repainted the distant solid
# wall's face with lastsec flats ? "walls are not rendered until I move
# close". HUD 'u' row now shows segwalls_count*100 + SEGW_MAX quota
# drops (any non-zero last two digits = quota dropping distant walls);
# 'i' row = saturn_ls_bad (expect 0).
# Round 27k (2026-09-03): -DSATURN_FLAT_DIAG removed after the
# pose-dependent clear bar at fb cols [23,35) was root-caused (the
# fr_merge_column extend path assumed column-monotonic walk order;
# fix = require pending x1 == x for extends) and verified in capture
# 002303 (pending[0] = [23,35]x[10,78] painted, fd_stage 0 -> 2,
# zero salmon). The 27j HUD probe rows (H/I/P/M/C/E at saturn_segs.c
# ~2065) stay until the col-60 black slab is confirmed fixed too.
# Round 28 (2026-09-04): -DSATURN_SEG_SHOW ON for one diagnostic boot ?
# the freeze brackets are REMOVED (they painted NBG0 every frame and
# corrupted the live display), so this screen's wrong colours are the
# SEG_SHOW fingerprint itself: dark red = unpainted, salmon/brown/white
# = flat fallbacks, green/magenta/cyan/yellow = wall-fallback reasons.
# Remove the define for production.
# Round 29 (2026-09-04): SEG_SHOW OFF again ? the SEG_SHOW boot showed the
# scene fully painted (no dark-red 0x20, no fallback colours), exonerating
# the renderer; the visible corruption was the fps_num erase band itself.
# Round 30b (2026-09-09): SEG_SHOW bisect ATTEMPTED and RETIRED ???????? the
# +47B .bss counters wedge the boot 3/3 on pure-cyan STARTING (BSS-shift
# work-cell wedge at current __bend 0x060c0ee0). The salmon/red wall-band
# blocks in the E1M1 courtyard are pre-3a (stage-2 era, m2b5_full.png
# 09-08 17:25 shows the same block) = the open-band fill / fallback
# family. Any further renderer diagnostics MUST be zero-BSS (rj-slot
# technique) ???????? see the salmon-block memory note.
doom/src/saturn_render.o: doom/src/saturn_render.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_SEG_RENDERER $(DIVLUT_DEFS) -o $@

# PHASE A (parallelization plan, 2026-08-31), edition 3 (2026-09-01):
# raw SMPC/SETSINT takeover is a STRUCTURAL dead end ? with bounded SMPC
# waits it still froze at "SLV 2 A": DisableSlaveCPU kills SGL's
# resident slave program and SGL's master frame flow depends on it (the
# next slSynch never returns). This build stays on SGL's resident
# program (SlaveSHmain, LIBCPK slavesh.o ? protocol area at
# 0x06043940/44/48) and CAPTURES the post protocol empirically: slave
# callback snapshots SlaveCommand/State/Param through the P2 mirror;
# HUD rows N (cmd*100+state), D (param low bits), H (execs*10 + match).
#   Phase A kill switch: add -DSATURN_NO_SLAVE_HS=1 to CCFLAGS below.
doom/src/saturn_slave.o: doom/src/saturn_slave.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

# r105 UI ENABLE (2026-09-05): run Saturn_Menu_Run + the early HUD cache
# (paired with the SATURN_UI_BISECT / SATURN_UI_MENU guards in d_main.c).
# BISECT A (2026-09-05 16:15): HUD cache only, menu OFF ? wedge hunt.
# BISECT B (2026-09-05 16:20): menu ON, HUD cache OFF ? wedge moved to
# the menu; A survived, so the culprit is inside Saturn_Menu_Run.
# UI BUILD (2026-09-05 16:35): menu ON ? the TITLEPIC 57 KB read goes
# through Saturn_CachePatchBig (sliced copy into cart; the normal path's
# whole-lump memcpy is the banned sustained A-bus burst).
# (2026-09-08: menu OFF for the VDP1-wall census campaign ???????? boot straight
# into E1M1 with no title/demo so every boot lands on the same view. The
# "UI-OFF BISECT" fall-through in d_main.c supplies episode/skill; the
# TEST RUN block already pins act==1 demos to E1M1.)
# 2026-09-09 MENU RESTORED (=1, operator: "the game should load to the
# title screen no matter what we do with the level renderer; only engage
# that code after a request to load a level"). Boot now lands on the
# title every time; the idle-attract demo is GATED OFF in d_main.c for
# the FULL-VDP1 campaign (idle stays on the title; restore = delete the
# gate). The census-spawn view is reached by driving in-game as usual.
# 2026-09-09 REAL-HW BISECT (UI-OFF) REJECTED by the operator ??? the
# wedge hunt moved to the SCUDSP DMA rig (title_repro.c staged vehicle);
# the menu stays ON (=1) permanently for ship builds.
doom/src/d_main.o: doom/src/d_main.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_UI_MENU=1 -DSATURN_VDP1_WALL=1 $(DIVLUT_DEFS) -o $@

# SCU DSP RETIRED (2026-09-07, WEDGE SUSPECT): the engine is off by
# policy (saturn_dsp.c v5.9b) but the boot probe + the cart engine's
# self-test still EX-ran DSP programs EVERY boot ???????? the self-test's
# 8-pair shape is the demo1-convicted mid-chain stall on real silicon,
# real-HW boot report reads SCU-DSP-FAIL, and a DSP stalled mid-DMA is
# a bus master holding SCU state (the level-start wedge strikes at the
# first heavy A-bus train after boot). SATURN_DSP_OFF skips both
# self-tests. Remove this rule to re-arm the diagnostics.
doom/src/saturn_dsp.o: doom/src/saturn_dsp.c
	$(CC) $< $(DFLAGS) $(CCFLAGS) -DSATURN_DSP_OFF -o $@

# WEDGE BRACKET PROBE (2026-09-07, edition 2): SATURN_PREAD_TRACE paints
# the death point on the stage-COLOR channel (ORANGE = pread entered,
# SALMON = copy phase, LIME/YELLOW/WHITE = >=1/4/8 chunks, C= in-flight
# chunk text). RESULT: the channels work, and 7/7 boots played WITH the
# probe vs 1 wedge in 4 on the identical build without it ???????? the extra
# VDP2-fill A-bus traffic interleaved with the CD read train SUPPRESSES
# the wedge (timing perturbation; dose/mechanism unknown). Diagnostic
# only: remove this rule for production.
# RE-ENABLED 2026-09-09 as the LEVEL-START WEDGE SUPPRESSION (softrend
# control verdict: renderer exonerated ??? game_softrend.iso 2E52E027
# reaches the menu and still wedges level load, solid blue 09:28 photo).
# This is the proven 7/7 build, now shipping as production candidate.
# GOTCHA (bit us 2026-09-07): toggling this rule does NOT stale i_wad.o
# ??? DELETE doom/src/i_wad.o before make or the ISO silently ships the
# old config. Cosmetic cost: full-screen flashing during COLD reads
# during play = the probe's fills, expected.
doom/src/i_wad.o: doom/src/i_wad.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@
# (2026-09-07 second probe round, park-first REMOVED from i_wad.c first:
# probe build booted and PLAYED ???????? skipdie=0, edsr=3, skipcols~40, V1 row
# live, scene clean with the face-selected preskip fix in.)
# (the VDP1_WALL define here only silences the retired O row ???????? the V1
# telemetry owns (8,168) in this build)
# (the VDP1_WALL define here only silences the retired O row ?????? the V1
# telemetry owns (8,168) in this build)
# (2026-09-06: -DSATURN_OCRAM_PROBE/KEEP_TW/STATE removed ?????? the probe's
# CCR write wedged real HW twice at the green OCPRB stage; verdict in
# saturn_cache.h. The probe block is define-gated in d_main.c and no
# longer compiles.)

# d32xr-derived hand-scheduled SH-2 pixel kernels (saturn_draw.s).
#   Kill switch: add -DSATURN_NO_ASM_DRAW=1 to the saturn_render rule.

# OCRAM DIVIDE LUT (2026-09-08): explicit rule for d32xr_stubs.o so the
# FixedDiv census sees -DSATURN_DIVLUT (the generic suffix rule cannot).
# d_main.o / saturn_render.o / i_video.o carry the define above. Remove
# the define from all four rules to ship without the LUT (rollback =
# define removal only; the CCR sequence never runs then).
# DIVIDE-HELPER ABI FIX (2026-09-08, crash12.yss convicted): gcc's SH-2
# divide convention lets a caller keep r2/r3 (and r6/r7) live across the
# jsr to __udivsi3 ???????? the fill loop in Saturn_Divlut_Install held its
# dt-counter in r2 and d in r3 exactly so. But __udivsi3 compiled as
# normal C (gcc splits it into __udivsi3.part.0) allocates r2 for the
# bb<<1 temp ???????? the counter never reached 0, the fill loop ran past its
# 511 bound writing ceil(2^32/d) for d=512.. over .bss (the "BSS
# stomper", caught red-handed in crash12 at d=570) and would wrap the
# 1MB WRAM mirror into code. -ffixed-rN makes the helper preserve them.
# Every divide call site in the game was a corruption lottery until now.
doom/src/d32xr_stubs.o: doom/src/d32xr_stubs.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) $(DIVLUT_DEFS) -ffixed-r2 -ffixed-r3 -ffixed-r6 -ffixed-r7 -o $@
doom/src/saturn_draw.o: doom/src/saturn_draw.s
	$(CC) $< $(DFLAGS) -c -o $@

# Replace the jo_engine_makefile's broken `$(TARGET2):` rule.
TARGET2 := game.iso
$(TARGET2): $(TARGET1)
	$(CONV) -O binary $(TARGET) cd/0.bin
	cd $(CURDIR) && .\make-iso.bat

override MKISOFS = cmd.exe
override MKISOFS_FLAGS = /c rem jo_engine_makefile mkisofs step bypassed by make-iso.bat

TARGET3 := game.cue
override CUE_MAKER = cmd.exe /c rem
override CUE_MAKER_FLAGS =
$(TARGET3): $(TARGET1) $(TARGET2)
	@cmd.exe /c rem
