/* saturn_hw_internal.h — Shared hardware definitions, diagnostic switches,
 * viewport state, and globals for the Sega Saturn VDP1/VDP2 Hardware Renderer.
 */

#ifndef SATURN_HW_INTERNAL_H
#define SATURN_HW_INTERNAL_H

#include "doomdef.h"
#include "r_local.h"
#include "p_local.h"
#include "saturn_hw_render.h"
#include "saturn_vdp1.h"
#include "saturn_tex.h"
#include "saturn_leaf.h"
#include "saturn_segs.h"
#include "saturn_baked_flats.h"
#include "saturn_baked_segs.h"
#include "i_video.h"
#include "i_system.h"
#include <jo/jo.h>
#include "saturn_slave.h"
#include "saturn_light_lut.h"
#include "saturn_memmap.h"
#include "saturn_cmdbuf.h"
#include "saturn_vdp2_plane.h"

typedef struct {
    fixed_t cx;
    fixed_t cz;
    int     s;
    int     px;
} hw_trans_pillar_t;

#ifndef SAT_FRT
#ifdef SATURN_WIN32
extern unsigned short Win32_GetFRT(void);
#define SAT_FRT() Win32_GetFRT()
#else
static inline unsigned short Saturn_HW_ReadFRT(void)
{
    /* SH7604 Hardware Manual: FRT is an 8-bit peripheral. Reading FRCH (0xFFFFFE12)
     * latches FRCL into TEMP, returned when reading FRCL (0xFFFFFE13).
     * Double-checking hi0 == hi1 also guards against ISR preemption between the
     * two byte reads and emulators that do not model the hardware TEMP latch. */
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned short hi0, lo, hi1;
    hi0 = frt[0];
    lo  = frt[1];
    hi1 = frt[0];
    if (__builtin_expect(hi0 != hi1, 0))
    {
        lo  = frt[1];
        hi0 = hi1;
    }
    return (unsigned short)((hi0 << 8) | lo);
}
#define SAT_FRT() Saturn_HW_ReadFRT()
#endif
#endif

#ifdef _MSC_VER
#define HW_NOINLINE __declspec(noinline)
#define HW_HOT_O2
#define HW_COLD_OS
#else
#define HW_NOINLINE __attribute__((noinline))
#define HW_HOT_O2   __attribute__((optimize("O2", "no-reorder-blocks")))
#define HW_COLD_OS  __attribute__((optimize("Os")))
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#ifndef SATURN_WIN32
/* SH7604 Hardware Manual: Software must not write to CCR (0xFFFFFE92) to purge cache
 * while executing from cached memory (Area 3: 0x06000000).
 * Always invoke via HW_PURGE_L1_CACHE() which branches to the uncached Area 1 alias
 * (0x26000000), purges L1, waits 8 pipeline cycles, and returns safely. */
HW_NOINLINE void _hw_purge_cache(void)
{
    /* 0x11 = Bit 4 (CP: Cache Purge) | Bit 0 (CE: Cache Enable, full 4KB 4-way mode) */
    (*(volatile unsigned char *)0xFFFFFE92u) = 0x11u;
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;" ::: "memory");
}
#define HW_PURGE_L1_CACHE() \
    ((void (*)(void))((uintptr_t)_hw_purge_cache | SATURN_WRAM_H_UNCACHED))()

/* Tight in-register 64-cycle bus yield loop:
 * Executes entirely out of the 2-instruction SH-2 prefetch buffer without
 * fetching 64 inline NOPs from SDRAM or polluting 8 L1 instruction cache lines. */
#define HW_BUS_YIELD() do { \
    int _w; \
    __asm__ volatile( \
        "mov #32, %0 \n\t" \
        "1:          \n\t" \
        "dt  %0      \n\t" \
        "bf/s 1b     \n\t" \
        "nop         \n\t" \
        : "=&r"(_w) \
        : \
        : "t", "memory" \
    ); \
} while (0)
#endif

#ifndef SATURN_WIN32
/* SH-2 Hardware DIVU registers at 0xFFFFFF00 */
#define SH2_DIVU_DVSR   (*(volatile int *)0xFFFFFF00u)
#define SH2_DIVU_DVDNTH (*(volatile int *)0xFFFFFF10u)
#define SH2_DIVU_DVDNTL (*(volatile int *)0xFFFFFF14u)
#endif

/* =========================================================================
 * Compile-Time Diagnostic Switches (Set to 0 to isolate subsystems during debug)
 * ========================================================================= */
#ifndef HW_DBG_PVS
#define HW_DBG_PVS       1  /* 1 = enable offline PVS leaf culling, 0 = disable */
#endif
#ifndef HW_DBG_OCCLUSION
#define HW_DBG_OCCLUSION 1  /* 1 = enable 1D solid-wall span occlusion, 0 = disable */
#endif
#ifndef HW_DBG_APERTURE
#define HW_DBG_APERTURE  1  /* 1 = enable vertical portal ytop/ybot aperture, 0 = disable */
#endif
#ifndef HW_DBG_FLAT_LOD
#define HW_DBG_FLAT_LOD  1  /* 1 = enable distant subsector flat LOD1 merge, 0 = disable */
#endif

#define HW_MAX_SCREEN_W 352

/* Viewport modes:
 * Option A: SATURN_VIEW_DOS  (320x168, CY=84, STBAR=168, 1:1 DOS pixel parity, locked 30 FPS)
 * Option B: SATURN_VIEW_FULL (320x192, CY=96, STBAR=192, borderless fullscreen)
 */
int saturn_view_w    = 320;
int saturn_view_mode = SATURN_VIEW_DOS; /* Starts in Option A (DOS mode) by default */
int saturn_view_h    = 168;
int saturn_view_cy   = 84;
int saturn_view_cx   = 160;
int saturn_stbar_y   = 168;
int saturn_view_latch = 2;
extern int saturn_hw_sky_latch;
extern int saturn_stbar_dirty_latch;
extern int gametic;

void Saturn_SetViewMode(int mode)
{
    if (mode == SATURN_VIEW_DOS)
    {
        saturn_view_mode = SATURN_VIEW_DOS;
        saturn_view_w  = 320;
        saturn_view_cx = 160;
        saturn_view_h  = 168;
        saturn_view_cy = 84;
        saturn_stbar_y = 168;
#ifndef SATURN_WIN32
        *(volatile unsigned short *)(0x25D0000Au) = 0x58A7u; /* EWRR: X2 = 352, Y2 = 167 (0x00A7) */
#endif
    }
    else
    {
        saturn_view_mode = SATURN_VIEW_FULL;
        saturn_view_w  = 320;
        saturn_view_cx = 160;
        saturn_view_h  = 192;
        saturn_view_cy = 96;
        saturn_stbar_y = 192;
#ifndef SATURN_WIN32
        *(volatile unsigned short *)(0x25D0000Au) = 0x58BFu; /* EWRR: X2 = 352, Y2 = 191 (0x00BF) */
#endif
    }
    Saturn_HW_ForceSkyRepaint();
    saturn_hw_sky_latch      = 2; /* Force background repaint on mode change */
    saturn_view_latch        = 2; /* Force full VDP2 NBG0 copy of rows 168..223 across double-buffered pages */
    saturn_stbar_dirty_latch = 2;
}

void Saturn_ToggleViewMode(void)
{
    Saturn_SetViewMode(saturn_view_mode == SATURN_VIEW_DOS ? SATURN_VIEW_FULL : SATURN_VIEW_DOS);
}

/* Wall Strip Subdivision (Perspective-Correct VDP1 1D Strips):
 * 1 = Enabled (subdivides oblique walls into 16px perspective-correct vertical strips)
 * 0 = Disabled (classic monolithic quads with VDP1 affine trapezoid warping)
 */
int saturn_wall_subdivision = 1;

void Saturn_ToggleWallSubdivision(void)
{
    extern void Saturn_Vdp1Wall_FlushCache(void);
    saturn_wall_subdivision ^= 1;
    Saturn_Vdp1Wall_FlushCache();
#ifdef SATURN_WIN32
    printf("[GRAPHICS] Wall Strip Subdivision: %s\n",
           saturn_wall_subdivision ? "ENABLED (Perspective Correct 8px Strips)" : "DISABLED (Classic Monolithic Quads)");
    fflush(stdout);
#endif
}

#define HW_VIEW_W       320
#define HW_VIEW_H       saturn_view_h
#define HW_VIEW_CY      saturn_view_cy
#define HW_VIEW_CX      160
#define HW_NEAR_Z_UNITS 4
#define HW_NEAR_Z       (HW_NEAR_Z_UNITS << FRACBITS)
#define HW_NEAR_Z_SCALE (10485760 / HW_NEAR_Z_UNITS)
#define HW_FAR_Z_UNITS  16384
#define HW_FAR_Z        (HW_FAR_Z_UNITS << FRACBITS)

/* External helpers from other modules */
extern int  Saturn_SkyTexNum(void);
extern byte *Saturn_SpriteData(int s);
extern byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs);

/* Camera space parameters for current frame */
static fixed_t   hw_viewx, hw_viewy, hw_viewz;
static angle_t   hw_viewangle;
static fixed_t   hw_viewsin, hw_viewcos;
static int       hw_cam_x, hw_cam_y;
sector_t        *hw_psec = NULL;
int              saturn_hw_is_sky = 0;
#define hw_is_sky saturn_hw_is_sky
static int       hw_displayplayer = 0;

/* 1D Solid Wall Screen Occlusion Bitmask:
 * 320 columns / 32 bits = 10 words.
 * Bit b in word w corresponds to column w*32 + b.
 * 1 = column is completely occluded by solid one-sided wall.
 * 0 = column is open / unoccluded.
 */
#define HW_SOLID_WORDS   ((HW_MAX_SCREEN_W + 31) / 32)
static uint32_t  hw_solid_mask[HW_SOLID_WORDS];
static int       hw_solid_count;
static int       hw_solid_words = 0;
int              saturn_hw_sky_visible = 0;
int              saturn_hw_sky_latch = 0;
int              saturn_hw_sky_prime = 0;
int16_t        (*hw_node_bboxes)[2][4] = NULL;
unsigned int saturn_hw_prof_bsp = 0;
unsigned int saturn_hw_prof_segs = 0;
unsigned int saturn_hw_prof_flats = 0;
unsigned int saturn_hw_prof_bg = 0;
unsigned int saturn_hw_prof_hud = 0;
unsigned int saturn_hw_wall_quads = 0;
unsigned int saturn_hw_flat_quads = 0;
unsigned int saturn_hw_wall_strips = 0;
unsigned int saturn_hw_flat_lod0_subs = 0;
unsigned int saturn_hw_flat_lod1_subs = 0;
unsigned int saturn_hw_flat_lod_saved_quads = 0;
unsigned int saturn_hw_slave_timeout = 0;
unsigned int saturn_hw_prof_slave_wait = 0;
unsigned int saturn_hw_bsp_abort = 0;
/* BSP back-child occlusion culling (Doom R_CheckBBox equivalent).
 * 1 = on, 2 = off. Initialised non-zero so it lives in .data (the .bss
 * is zeroed every frame by the known BSS stomper). Toggled live with the
 * L+R+X pad chord (i_system.c); shown on the HUD perf row as O1/O2. */
int saturn_hw_bsp_occl = 1;
int saturn_hw_skydbg = 1;   /* 1 = normal sky, 2 = magenta background (L+R+Y) */
unsigned int saturn_hw_occl_culls = 0;
unsigned int saturn_hw_seg_iter = 0;   /* baked segs iterated / frame */
unsigned int saturn_hw_seg_vis = 0;    /* ... surviving all culls     */
unsigned int saturn_hw_tex_miss = 0;   /* wall pieces with no texture */

typedef struct {
    fixed_t cx;      /* 4 bytes */
    fixed_t cz;      /* 4 bytes */
    int16_t px;      /* 2 bytes */
    int16_t py_c;    /* 2 bytes */
    int16_t py_f;    /* 2 bytes */
    uint8_t valid;   /* 1 byte: 1 if cz >= HW_NEAR_Z */
    uint8_t pad;     /* 1 byte */
} saturn_proj_v_t;   /* Exactly 16 bytes */

#ifndef _MSC_VER
#define HW_ALWAYS_INLINE static inline __attribute__((always_inline))
#define HW_ALIGN16 __attribute__((aligned(16)))
#else
#define HW_ALWAYS_INLINE static __forceinline
#define HW_ALIGN16 __declspec(align(16))
#endif

static HW_NOINLINE HW_COLD_OS unsigned int HW_ProfDelta_Wrap(unsigned short t0, unsigned short t1)
{
    extern volatile int saturn_frt_rate;
    int span = saturn_frt_rate + (int)t1 - (int)t0;
    return (span > 0 && span <= saturn_frt_rate) ? (unsigned int)span : (unsigned int)t1;
}

HW_ALWAYS_INLINE unsigned int HW_ProfDelta(unsigned short t0, unsigned short t1)
{
    if (__builtin_expect(t1 >= t0, 1))
        return (unsigned int)(t1 - t0);
    return HW_ProfDelta_Wrap(t0, t1);
}

/* Prevent int16 wraparound of projected coords, 11-bit VDP1 LocalCoord (+16) adder wrap,
 * and 11-bit VDP1 DDA subtractor overflow (max delta 500 - (-501) = 1001 <= 1023) */
HW_ALWAYS_INLINE int HW_ClampProj(int v)
{
    if (__builtin_expect((unsigned int)v + 500u <= 1000u, 1))
        return v;
    return (v >> 31) ^ 500;
}
#define HW_CLAMP_PROJ(v) HW_ClampProj(v)

/* Non-zero if any of four projected Y coords lies outside VDP1's [-1024, 1023] band (2047u = 0x7FFu bitmask-closed under OR) */
#define HW_ANY_OUT1024(a, b, c, d) \
    (((((unsigned int)(a) + 1024u) | ((unsigned int)(b) + 1024u) | \
       ((unsigned int)(c) + 1024u) | ((unsigned int)(d) + 1024u)) > 2047u))

#ifndef _MSC_VER
typedef uint32_t aliased_uint32_t __attribute__((__may_alias__));
#else
typedef uint32_t aliased_uint32_t;
#endif

#define HW_MAX_FLAT_VERTS        1536
static saturn_proj_v_t *hw_proj_verts_master = (saturn_proj_v_t *)((uint8_t *)SATURN_WRAM_L_PROJ_VERTS_ADDR + 0x6000u);
static saturn_proj_v_t *hw_proj_verts_slave  = (saturn_proj_v_t *)SATURN_WRAM_L_PROJ_VERTS_ADDR;
static uint32_t  sub_solid_mask[HW_SOLID_WORDS];
static int       sub_solid_active = 0;
static int       sub_solid_min_w = HW_SOLID_WORDS;
static int       sub_solid_max_w = -1;
static int       hw_first_flat_job = 0;
static uint16_t  hw_render_frame = 0;
extern unsigned char saturn_slave_enabled;
static int       hw_frame_slave_enabled = 0;


#endif /* SATURN_HW_INTERNAL_H */
