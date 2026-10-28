/* saturn_memmap.h - Authoritative Sega Saturn Memory & Bus Map
 * Defines all hardware memory domains, fixed buffers, and non-overlap assertions.
 */

#ifndef __SATURN_MEMMAP_H__
#define __SATURN_MEMMAP_H__

#include <stdint.h>

/* --- 1. Memory Domain Bases --- */
#ifdef SATURN_WIN32
#include "saturn_win32.h"
#define SATURN_WRAM_L_BASE          ((uintptr_t)saturn_win32_wram_l)
#define SATURN_WRAM_L_UNCACHED_BASE ((uintptr_t)saturn_win32_wram_l)
#define SATURN_WRAM_L_UNCACHED      0u
#define SATURN_WRAM_H_BASE          ((uintptr_t)saturn_win32_wram_h)
#define SATURN_WRAM_H_UNCACHED      0u
#define SATURN_OCRAM_BASE           ((uintptr_t)saturn_win32_wram_h + 0x80000u)

#define SATURN_VDP1_VRAM            ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_VDP1_FB0             ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_VDP1_FB1             ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_VDP1_REGS            ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_VDP2_VRAM            ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_CRAM                 ((uintptr_t)saturn_win32_vdp1_vram)
#define SATURN_SCU_REGS             ((uintptr_t)saturn_win32_vdp1_vram)
#else
#define SATURN_WRAM_L_BASE          0x00200000u     /* 1 MB Low Work RAM (DRAM, shared CPU bus) */
#define SATURN_WRAM_L_UNCACHED_BASE 0x20200000u
#define SATURN_WRAM_L_UNCACHED      0x20200000u
#define SATURN_WRAM_H_BASE          0x06000000u     /* 1 MB High Work RAM (SDRAM, 32-bit fast) */
#define SATURN_WRAM_H_UNCACHED      0x26000000u
#define SATURN_OCRAM_BASE           0xC0000000u     /* 2 KB On-Chip Scratchpad RAM per CPU */

#define SATURN_VDP1_VRAM            0x25C00000u     /* 512 KB VDP1 VRAM */
#define SATURN_VDP1_FB0             0x25C80000u     /* Frame Buffer 0 (256 KB) */
#define SATURN_VDP1_FB1             0x25CC0000u     /* Frame Buffer 1 (256 KB) */
#define SATURN_VDP1_REGS            0x25D00000u
#define SATURN_VDP2_VRAM            0x25E00000u     /* 512 KB VDP2 VRAM */
#define SATURN_CRAM                 0x25F00000u     /* 4 KB Color RAM (2,048 entries) */
#define SATURN_SCU_REGS             0x25FE0000u
#endif

/* --- 2. Low Work RAM (WRAM-L) Layout --- */
/* 0x00200000 - 0x00203FFF: Reserved / Scratch (16 KB) */
#define SATURN_WRAM_L_SCRATCH_OFF   0x00000000u
#define SATURN_WRAM_L_SCRATCH_SIZE  0x00004000u
#define SATURN_WRAM_L_SCRATCH_ADDR  ((uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_SCRATCH_OFF))
#define SATURN_WRAM_L_SCRATCH_UNCACHED ((uint8_t *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_SCRATCH_OFF))

/* 0x00204000 - 0x0020FFFF: Slave & Master Flat Projected Vertices (48 KB = 3072 verts * 16B) */
#define SATURN_WRAM_L_PROJ_VERTS_OFF 0x00004000u
#define SATURN_WRAM_L_PROJ_VERTS_ADDR ((void *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_PROJ_VERTS_OFF))
#define SATURN_WRAM_L_PROJ_VERTS_UNCACHED ((void *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_PROJ_VERTS_OFF))
#define SATURN_WRAM_L_PROJ_VERTS_SIZE 0x0000C000u

/* 0x00210000 - 0x0021FFFF: Slave IPC, Ring Buffers, Sync (64 KB) */
#define SATURN_WRAM_L_IPC_OFF       0x00010000u
#define SATURN_WRAM_L_IPC_ADDR      (SATURN_WRAM_L_BASE + SATURN_WRAM_L_IPC_OFF)
#define SATURN_WRAM_L_IPC_UNCACHED  (SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_IPC_OFF)
#define SATURN_WRAM_L_IPC_SIZE      0x00010000u

/* 0x00218000 - 0x0021BFFF: Dedicated Slave SH-2 Stack (16 KB) inside the
 * unused middle of the IPC region (between hw_slave_ipc_t [0x00210000..0x00216040]
 * and SECSHADOW [0x0021C000..0x0021FFFF]). Prevents Slave SH-2 call frames from
 * overflowing SGL's tiny default 0x06000FF0 stack into the BIOS SCU Interrupt
 * Callback & Mask tables at 0x06000A00..0x06000AFF. */
#define SATURN_WRAM_L_SLAVE_STACK_OFF  (SATURN_WRAM_L_IPC_OFF + 0x00008000u)
#define SATURN_WRAM_L_SLAVE_STACK_SIZE 0x00004000u
#define SATURN_WRAM_L_SLAVE_STACK_TOP  (SATURN_WRAM_L_BASE + SATURN_WRAM_L_SLAVE_STACK_OFF + SATURN_WRAM_L_SLAVE_STACK_SIZE - 16u)

/* 0x0021C000 - 0x0021FFFF: Master-only sector shadow (16 KB) inside the IPC
 * region's unused tail (hw_slave_ipc_t is ~24 KB, asserted <= 0x8000 in
 * saturn_hw_render.c). Same stride as sector_t; only the leading
 * floor/ceiling/pics/light words are refreshed each frame. Accessed via the
 * CACHED mirror by the master only. */
#define SATURN_WRAM_L_SECSHADOW_OFF  (SATURN_WRAM_L_IPC_OFF + 0x0000C000u)
#define SATURN_WRAM_L_SECSHADOW_ADDR ((void *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_SECSHADOW_OFF))
#define SATURN_WRAM_L_SECSHADOW_SIZE 0x00004000u

/* 0x00220000 - 0x0022FFFF: Fast Sky Composite Buffer (64 KB) */
#define SATURN_WRAM_L_SKY_OFF       0x00020000u
#define SATURN_WRAM_L_SKY_ADDR      ((uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_SKY_OFF))
#define SATURN_WRAM_L_SKY_UNCACHED  ((uint8_t *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_SKY_OFF))
#define SATURN_WRAM_L_SKY_SIZE      0x00010000u

/* 0x00230000 - 0x00237FFF: Transformed Pillars Scratch Buffer (32 KB reserved, 8 KB used = 512 entries * 16B) */
#define SATURN_WRAM_L_PILLARS_OFF    0x00030000u
#define SATURN_WRAM_L_PILLARS_ADDR   ((void *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_PILLARS_OFF))
#define SATURN_WRAM_L_PILLARS_UNCACHED ((void *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_PILLARS_OFF))
#define SATURN_WRAM_L_PILLARS_SIZE   0x00008000u

/* 0x00238000 - 0x0023FFFF: Hardware Division LUT (32 KB = 8192 entries * 4B) */
#define SATURN_WRAM_L_DIVLUT_OFF    0x00038000u
#define SATURN_WRAM_L_DIVLUT_ADDR   (SATURN_WRAM_L_BASE + SATURN_WRAM_L_DIVLUT_OFF)
#define SATURN_WRAM_L_DIVLUT_UNCACHED (SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_DIVLUT_OFF)
#define SATURN_WRAM_L_DIVLUT_SIZE   0x00008000u

/* 0x00240000 - 0x002CFFFF: Level geometry window (576 KB), packed per level:
 *   [ .FLT flats | .WL walls + swizzled segs ] -> saturn_wram_l_geom_end
 * Flats always start at 0x240000; walls start right after the flats
 * (saturn_wram_l_seg_addr, set by the flats loader). saturn_tex.c's WRAM-L
 * texture tier (0x280000+) starts above saturn_wram_l_geom_end, so big
 * levels shrink the texture tier instead of being overwritten by it.
 * History: FLAT was a fixed 128 KB (E1M8/E3M6 flats overflowed it) and the
 * fixed SEG region ran into the texture tier on large levels. Largest
 * combined level today: E3M6 = 421,416 B. */
#define SATURN_WRAM_L_FLAT_OFF      0x00040000u
#define SATURN_WRAM_L_FLAT_ADDR     ((uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_FLAT_OFF))
#define SATURN_WRAM_L_FLAT_UNCACHED ((uint8_t *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_FLAT_OFF))
#define SATURN_WRAM_L_FLAT_CAP      0x00046000u     /* largest single .FLT (E3M6 284,544 B) */

/* Walls (.WL blob + swizzled segs, 40 B each): packed after the flats.
 * Largest use: E2M7 = 299,528 B. */
#define SATURN_WRAM_L_GEOM_END_OFF  0x000D0000u     /* = SHADOW_FB_OFF */
extern uint8_t *saturn_wram_l_seg_addr;             /* saturn_baked_flats.c */
extern uint8_t *saturn_wram_l_geom_end;             /* end of flats+walls this level */
#define SATURN_WRAM_L_SEG_ADDR      (saturn_wram_l_seg_addr)
#define SATURN_WRAM_L_SEG_CAP       ((uint32_t)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_GEOM_END_OFF) \
                                     - (uint32_t)saturn_wram_l_seg_addr)

/* 0x002D0000 - 0x002E3FFF: Software Shadow Framebuffer (up to 352x224 = 78,848 B <= 80 KB) */
#define SATURN_WRAM_L_SHADOW_FB_OFF   0x000D0000u
#define SATURN_WRAM_L_SHADOW_FB_ADDR  ((uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_SHADOW_FB_OFF))
#define SATURN_WRAM_L_SHADOW_FB_UNCACHED ((uint8_t *)(SATURN_WRAM_L_UNCACHED_BASE + SATURN_WRAM_L_SHADOW_FB_OFF))
#define SATURN_WRAM_L_SHADOW_FB_SIZE  0x00014000u     /* 80 KB (352 * 224 = 78,848 B) */

/* --- 3. Hardware Register & Video Domains (B-Bus) --- */
#ifndef SATURN_WIN32
#define SATURN_VDP1_VRAM            0x25C00000u     /* 512 KB VDP1 VRAM */
#define SATURN_VDP1_FB0             0x25C80000u     /* Frame Buffer 0 (256 KB) */
#define SATURN_VDP1_FB1             0x25CC0000u     /* Frame Buffer 1 (256 KB) */
#define SATURN_VDP1_REGS            0x25D00000u
#define SATURN_VDP2_VRAM            0x25E00000u     /* 512 KB VDP2 VRAM */
#define SATURN_CRAM                 0x25F00000u     /* 4 KB Color RAM (2,048 entries) */
#define SATURN_SCU_REGS             0x25FE0000u
#endif

/* VDP1 Command Table Offsets */
#define SATURN_V1_SLOT_SYSTEM_CLIP  0x0000u         /* Slot 0: System Clip (SGL owned) */
#define SATURN_V1_SLOT_USER_CLIP    0x0020u         /* Slot 1: User Clip (SGL owned) */
#define SATURN_V1_SLOT_LOCAL_COORD  0x0040u         /* Slot 2: Local Coord (SGL owned) */
#define SATURN_V1_SLOT_END_JUMP     0x0060u         /* Slot 3: Draw End / Jump Patch (SGL owned) */
#define SATURN_V1_USER0             0x0080u         /* Slot 4+: Engine user command list starts */

/* --- 4. Non-Overlap & Alignment Assertions --- */
#if defined(__GNUC__) && !defined(__cplusplus)
_Static_assert(SATURN_WRAM_L_SCRATCH_OFF + SATURN_WRAM_L_SCRATCH_SIZE <= SATURN_WRAM_L_PROJ_VERTS_OFF,
               "WRAM-L: Scratch region overlaps PROJ_VERTS region");
_Static_assert(SATURN_WRAM_L_PROJ_VERTS_OFF + SATURN_WRAM_L_PROJ_VERTS_SIZE <= SATURN_WRAM_L_IPC_OFF,
               "WRAM-L: PROJ_VERTS region overlaps IPC region");
_Static_assert(SATURN_WRAM_L_IPC_OFF + SATURN_WRAM_L_IPC_SIZE <= SATURN_WRAM_L_SKY_OFF,
               "WRAM-L: IPC region overlaps SKY region");
_Static_assert(SATURN_WRAM_L_SKY_OFF + SATURN_WRAM_L_SKY_SIZE <= SATURN_WRAM_L_PILLARS_OFF,
               "WRAM-L: SKY region overlaps PILLARS region");
_Static_assert(SATURN_WRAM_L_PILLARS_OFF + SATURN_WRAM_L_PILLARS_SIZE <= SATURN_WRAM_L_DIVLUT_OFF,
               "WRAM-L: PILLARS region overlaps DIVLUT region");
_Static_assert(SATURN_WRAM_L_DIVLUT_OFF + SATURN_WRAM_L_DIVLUT_SIZE <= SATURN_WRAM_L_FLAT_OFF,
               "WRAM-L: DIVLUT region overlaps FLAT region");
_Static_assert(SATURN_WRAM_L_FLAT_OFF + SATURN_WRAM_L_FLAT_CAP < SATURN_WRAM_L_GEOM_END_OFF,
               "WRAM-L: FLAT cap exceeds the geometry window");
_Static_assert(SATURN_WRAM_L_GEOM_END_OFF <= SATURN_WRAM_L_SHADOW_FB_OFF,
               "WRAM-L: geometry window overlaps SHADOW_FB region");
_Static_assert(SATURN_WRAM_L_SHADOW_FB_OFF + SATURN_WRAM_L_SHADOW_FB_SIZE <= 0x00100000u,
               "WRAM-L: SHADOW_FB region exceeds 1 MB physical DRAM boundary");
#endif

#endif /* __SATURN_MEMMAP_H__ */
