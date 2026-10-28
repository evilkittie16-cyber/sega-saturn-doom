/* saturn_hw_render.c — High-performance Sega Saturn Hardware Renderer
 *
 * Implements Ezra Dreisbach's Lobotomy-style VDP1 distorted quad architecture
 * with hardware DIVU pipelining and VDP2 background planes.
 */

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
    unsigned short hi0 = frt[0];
    __asm__ volatile("" ::: "memory");
    unsigned short lo = frt[1];
    __asm__ volatile("" ::: "memory");
    unsigned short hi1 = frt[0];
    if (__builtin_expect(hi0 != hi1, 0))
    {
        __asm__ volatile("" ::: "memory");
        lo = frt[1];
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
#define SH2_DIVU_DVDNT  (*(volatile int *)0xFFFFFF04u)
#define SH2_DIVU_DVDNTH (*(volatile int *)0xFFFFFF10u)
#define SH2_DIVU_DVDNTL (*(volatile int *)0xFFFFFF14u)
#endif

#define HW_MAX_SCREEN_W 352
#define HW_SCREEN_W     320
#define HW_SCREEN_H     224

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

#define HW_VIEW_W       saturn_view_w
#define HW_VIEW_H       saturn_view_h
#define HW_VIEW_CY      saturn_view_cy
#define HW_VIEW_CX      saturn_view_cx
#define HW_NEAR_Z_UNITS 4
#define HW_NEAR_Z       (HW_NEAR_Z_UNITS << FRACBITS)
#define HW_NEAR_Z_SCALE (10485760 / HW_NEAR_Z_UNITS)
#define HW_FAR_Z_UNITS  16384
#define HW_FAR_Z        (HW_FAR_Z_UNITS << FRACBITS)

/* External helpers from other modules */
extern byte Saturn_LightRow(int lightlevel, int dist);
extern int  Saturn_SkyTexNum(void);
extern byte *Saturn_SpriteData(int s);
extern byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs);
extern byte *I_WorkBuffer(void);

/* Camera space parameters for current frame */
static fixed_t   hw_viewx, hw_viewy, hw_viewz;
static angle_t   hw_viewangle;
static fixed_t   hw_viewsin, hw_viewcos;
static fixed_t   cam_cz_origin, cam_cx_origin;
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
unsigned int saturn_hw_seg_count = 0;
unsigned int saturn_hw_wall_quads = 0;
unsigned int saturn_hw_flat_quads = 0;
unsigned int saturn_hw_wall_strips = 0;
unsigned int saturn_hw_flat_lod0_subs = 0;
unsigned int saturn_hw_flat_lod1_subs = 0;
unsigned int saturn_hw_flat_lod_saved_quads = 0;
unsigned int saturn_hw_clamp_count = 0;
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

/* Prevent int16 wraparound of projected coords, 11-bit VDP1 LocalCoord (+16) adder wrap,
 * and 11-bit VDP1 DDA subtractor overflow (max delta 500 - (-501) = 1001 <= 1023) */
HW_ALWAYS_INLINE int HW_ClampProj(int v)
{
    if (__builtin_expect((unsigned int)v + 500u <= 1000u, 1))
        return v;
    return (v >> 31) ^ 500;
}
#define HW_CLAMP_PROJ(v) HW_ClampProj(v)
/* Non-zero if any of four projected coords lies outside HW_ClampProj's [-500, 500] band */
#define HW_ANY_OUT500(a, b, c, d) \
    ((((unsigned int)(a) + 500u) > 1000u) | (((unsigned int)(b) + 500u) > 1000u) | \
     (((unsigned int)(c) + 500u) > 1000u) | (((unsigned int)(d) + 500u) > 1000u))

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
static int16_t  *saturn_vertex_floor_clamp;
extern unsigned char saturn_slave_enabled;
static int       hw_frame_slave_enabled = 0;

static const uint32_t hw_bit_lut[32] = {
    1u<<0,  1u<<1,  1u<<2,  1u<<3,  1u<<4,  1u<<5,  1u<<6,  1u<<7,
    1u<<8,  1u<<9,  1u<<10, 1u<<11, 1u<<12, 1u<<13, 1u<<14, 1u<<15,
    1u<<16, 1u<<17, 1u<<18, 1u<<19, 1u<<20, 1u<<21, 1u<<22, 1u<<23,
    1u<<24, 1u<<25, 1u<<26, 1u<<27, 1u<<28, 1u<<29, 1u<<30, 1u<<31
};

static const uint32_t hw_span_mask_lut[33] = {
    0x00000000u, 0x00000001u, 0x00000003u, 0x00000007u,
    0x0000000Fu, 0x0000001Fu, 0x0000003Fu, 0x0000007Fu,
    0x000000FFu, 0x000001FFu, 0x000003FFu, 0x000007FFu,
    0x00000FFFu, 0x00001FFFu, 0x00003FFFu, 0x00007FFFu,
    0x0000FFFFu, 0x0001FFFFu, 0x0003FFFFu, 0x0007FFFFu,
    0x000FFFFFu, 0x001FFFFFu, 0x003FFFFFu, 0x007FFFFFu,
    0x00FFFFFFu, 0x01FFFFFFu, 0x03FFFFFFu, 0x07FFFFFFu,
    0x0FFFFFFFu, 0x1FFFFFFFu, 0x3FFFFFFFu, 0x7FFFFFFFu,
    0xFFFFFFFFu
};

static const uint32_t hw_mask_left[32] = {
    0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFCu, 0xFFFFFFF8u,
    0xFFFFFFF0u, 0xFFFFFFE0u, 0xFFFFFFC0u, 0xFFFFFF80u,
    0xFFFFFF00u, 0xFFFFFE00u, 0xFFFFFC00u, 0xFFFFF800u,
    0xFFFFF000u, 0xFFFFE000u, 0xFFFFC000u, 0xFFFF8000u,
    0xFFFF0000u, 0xFFFE0000u, 0xFFFC0000u, 0xFFF80000u,
    0xFFF00000u, 0xFFE00000u, 0xFFC00000u, 0xFF800000u,
    0xFF000000u, 0xFE000000u, 0xFC000000u, 0xF8000000u,
    0xF0000000u, 0xE0000000u, 0xC0000000u, 0x80000000u
};

static const uint32_t hw_mask_right[32] = {
    0x00000001u, 0x00000003u, 0x00000007u, 0x0000000Fu,
    0x0000001Fu, 0x0000003Fu, 0x0000007Fu, 0x000000FFu,
    0x000001FFu, 0x000003FFu, 0x000007FFu, 0x00000FFFu,
    0x00001FFFu, 0x00003FFFu, 0x00007FFFu, 0x0000FFFFu,
    0x0001FFFFu, 0x0003FFFFu, 0x0007FFFFu, 0x000FFFFFu,
    0x001FFFFFu, 0x003FFFFFu, 0x007FFFFFu, 0x00FFFFFFu,
    0x01FFFFFFu, 0x03FFFFFFu, 0x07FFFFFFu, 0x0FFFFFFFu,
    0x1FFFFFFFu, 0x3FFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu
};

static uint32_t hw_solid_summary = 0; /* Bitmask of fully solid words (0xFFFFFFFFu) */
static uint32_t hw_solid_any = 0;     /* Bitmask of words with ANY solid pixels (!= 0u) */
static uint32_t hw_slave_dirty_words = 0; /* Words modified in hw_solid_mask since last Slave flat job */

static HW_NOINLINE HW_HOT_O2 int HW_IsSpanOccluded(int x0, int x1)
{
    if (hw_solid_count == 0)
        return 0;
    if (hw_solid_count >= saturn_view_w)
        return 1;
    if (x0 < 0) x0 = 0;
    if (x1 > saturn_view_w) x1 = saturn_view_w;
    if (x0 >= x1)
        return 0;

    int w0 = x0 >> 5;
    int w1 = (x1 - 1) >> 5;
    int b0 = x0 & 31;
    int b1 = (x1 - 1) & 31;

    if (w0 == w1)
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        return ((hw_solid_mask[w0] & mask) == mask);
    }

    uint32_t mask0 = hw_mask_left[b0];
    if ((hw_solid_mask[w0] & mask0) != mask0)
        return 0;

    /* O(1) Occlusion Check: Static mask table XOR eliminates SH-2 barrel shifter emulation */
    if (w1 > w0 + 1)
    {
        uint32_t req_bits = hw_span_mask_lut[w1] ^ hw_span_mask_lut[w0 + 1];
        if ((hw_solid_summary & req_bits) != req_bits)
            return 0;
    }

    uint32_t mask1 = hw_mask_right[b1];
    return ((hw_solid_mask[w1] & mask1) == mask1);
}

/* 3-state span occlusion query:
 *   0 = 100% open (zero solid bits in [x0, x1]; sub-strips need no occlusion test)
 *   1 = partially occluded (some solid bits in [x0, x1])
 *   2 = 100% occluded (all bits in [x0, x1] are solid)
 */
static HW_NOINLINE HW_HOT_O2 int HW_CheckSpanOcclusion(int x0, int x1)
{
    if (hw_solid_count == 0)
        return 0;
    if (hw_solid_count >= saturn_view_w)
        return 2;
    if (x0 < 0) x0 = 0;
    if (x1 > saturn_view_w) x1 = saturn_view_w;
    if (x0 >= x1)
        return 0;

    int w0 = x0 >> 5;
    int w1 = (x1 - 1) >> 5;
    int b0 = x0 & 31;
    int b1 = (x1 - 1) & 31;

    if (w0 == w1)
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        uint32_t hit  = hw_solid_mask[w0] & mask;
        if (hit == 0)
            return 0;
        return (hit == mask) ? 2 : 1;
    }

    uint32_t mask0 = hw_mask_left[b0];
    uint32_t hit0  = hw_solid_mask[w0] & mask0;
    uint32_t mask1 = hw_mask_right[b1];
    uint32_t hit1  = hw_solid_mask[w1] & mask1;

    if (w1 == w0 + 1)
    {
        if ((hit0 | hit1) == 0)
            return 0;
        return (hit0 == mask0 && hit1 == mask1) ? 2 : 1;
    }

    if ((hit0 | hit1) == 0)
    {
        uint32_t req_bits = hw_span_mask_lut[w1] ^ hw_span_mask_lut[w0 + 1];
        return ((hw_solid_any & req_bits) == 0) ? 0 : 1;
    }

    if (hit0 == mask0 && hit1 == mask1)
    {
        uint32_t req_bits = hw_span_mask_lut[w1] ^ hw_span_mask_lut[w0 + 1];
        if ((hw_solid_summary & req_bits) == req_bits)
            return 2;
    }

    return 1;
}

/* Defer solid span masking until subsector completion so walls never occlude flats in the same subsector */
static HW_NOINLINE HW_HOT_O2 void HW_MarkSubSolidSpan(int x0, int x1)
{
    if (x0 < 0) x0 = 0;
    if (x1 > saturn_view_w) x1 = saturn_view_w;
    if (x0 >= x1)
        return;

    int w0 = x0 >> 5;
    int w1 = (x1 - 1) >> 5;
    if (!sub_solid_active)
    {
        sub_solid_active = 1;
        sub_solid_min_w  = w0;
        sub_solid_max_w  = w1;
    }
    else
    {
        if (w0 < sub_solid_min_w) sub_solid_min_w = w0;
        if (w1 > sub_solid_max_w) sub_solid_max_w = w1;
    }
    int b0 = x0 & 31;
    int b1 = (x1 - 1) & 31;

    if (w0 == w1)
    {
        sub_solid_mask[w0] |= (hw_mask_left[b0] & hw_mask_right[b1]);
    }
    else
    {
        sub_solid_mask[w0] |= hw_mask_left[b0];
        for (int w = w0 + 1; w < w1; w++)
        {
            sub_solid_mask[w] = 0xFFFFFFFFu;
        }
        sub_solid_mask[w1] |= hw_mask_right[b1];
    }
}

typedef struct {
    int16_t sx;
    int16_t ex;
    int16_t ytop;
    int16_t ybot;
    int16_t yt_att;
    int16_t yb_att;
} hw_portal_span_t;

#define HW_NODE_PVS_WORDS   32
#define HW_MAX_PORTAL_SPANS 32
#define HW_MAX_AP_SECTORS   256

typedef struct {
    uint32_t         node_pvs_vis[HW_NODE_PVS_WORDS];
    int16_t          ytop[HW_MAX_SCREEN_W];
    int16_t          ybot[HW_MAX_SCREEN_W];
    int16_t          ytop_min_w[HW_SOLID_WORDS];
    int16_t          ybot_max_w[HW_SOLID_WORDS];
    int16_t          sec_yt_attach[HW_MAX_AP_SECTORS];
    int16_t          sec_yb_attach[HW_MAX_AP_SECTORS];
    hw_portal_span_t portal_spans[HW_MAX_PORTAL_SPANS];
    const byte      *last_pvs_row;
    int              last_pvs_numnodes;
    int              portal_span_count;
    int              aperture_active;
    int16_t          cur_sub_yt_attach;
    int16_t          cur_sub_yb_attach;
} hw_aperture_state_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(hw_aperture_state_t) <= SATURN_WRAM_L_SECSHADOW_SIZE,
               "hw_aperture_state_t must fit inside SATURN_WRAM_L_SECSHADOW_ADDR");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_aperture_state_t) <= SATURN_WRAM_L_SECSHADOW_SIZE,
              "hw_aperture_state_t must fit inside SATURN_WRAM_L_SECSHADOW_ADDR");
#endif

#define HW_APERTURE ((hw_aperture_state_t *)((uintptr_t)SATURN_WRAM_L_PILLARS_ADDR + 0x2000u))

HW_ALWAYS_INLINE void HW_RecordPortalSpan(int sx, int ex, int ytop, int ybot, int yt_att, int yb_att)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    if (sx >= ex || (ytop <= 0 && ybot >= HW_VIEW_H))
        return;
    if (ytop < 0) ytop = 0;
    if (ybot > HW_VIEW_H) ybot = HW_VIEW_H;
    if (yt_att < 0) yt_att = 0;
    if (yb_att > HW_VIEW_H) yb_att = HW_VIEW_H;
    int idx = ap->portal_span_count;
    if (idx > 0 && ap->portal_spans[idx - 1].sx == sx && ap->portal_spans[idx - 1].ex == ex)
    {
        if (ytop > ap->portal_spans[idx - 1].ytop) ap->portal_spans[idx - 1].ytop = (int16_t)ytop;
        if (ybot < ap->portal_spans[idx - 1].ybot) ap->portal_spans[idx - 1].ybot = (int16_t)ybot;
        if (yt_att < ap->portal_spans[idx - 1].yt_att) ap->portal_spans[idx - 1].yt_att = (int16_t)yt_att;
        if (yb_att > ap->portal_spans[idx - 1].yb_att) ap->portal_spans[idx - 1].yb_att = (int16_t)yb_att;
        return;
    }
    if (idx < HW_MAX_PORTAL_SPANS)
    {
        ap->portal_spans[idx].sx     = (int16_t)sx;
        ap->portal_spans[idx].ex     = (int16_t)ex;
        ap->portal_spans[idx].ytop   = (int16_t)ytop;
        ap->portal_spans[idx].ybot   = (int16_t)ybot;
        ap->portal_spans[idx].yt_att = (int16_t)yt_att;
        ap->portal_spans[idx].yb_att = (int16_t)yb_att;
        ap->portal_span_count = idx + 1;
    }
}

static HW_NOINLINE HW_HOT_O2 int HW_GetSpanAperture(int sx, int ex, int16_t *out_ytop_min, int16_t *out_ybot_max)
{
    const hw_aperture_state_t *ap = HW_APERTURE;
    if (sx < 0) sx = 0;
    if (ex > saturn_view_w) ex = saturn_view_w;
    if (!ap->aperture_active || sx >= ex)
    {
        *out_ytop_min = 0;
        *out_ybot_max = (int16_t)HW_VIEW_H;
        return 0;
    }
    int w0 = sx >> 5;
    int w1 = (ex - 1) >> 5;
    int yt_min = HW_VIEW_H;
    int yb_max = 0;
    for (int w = w0; w <= w1; w++)
    {
        if (ap->ytop_min_w[w] < yt_min) yt_min = ap->ytop_min_w[w];
        if (ap->ybot_max_w[w] > yb_max) yb_max = ap->ybot_max_w[w];
    }
    *out_ytop_min = (int16_t)yt_min;
    *out_ybot_max = (int16_t)yb_max;
    return (yt_min >= yb_max);
}

static HW_NOINLINE HW_HOT_O2 void HW_CommitSubsectorOcclusion(void)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int p_count = ap->portal_span_count;
    uint32_t touched_words = 0;
    const int vw = saturn_view_w;

    if (p_count > 0)
    {
        ap->portal_span_count = 0;
        int16_t * __restrict ytop = ap->ytop;
        int16_t * __restrict ybot = ap->ybot;

        for (int i = 0; i < p_count; i++)
        {
            int sx = ap->portal_spans[i].sx;
            int ex = ap->portal_spans[i].ex;
            int new_yt = ap->portal_spans[i].ytop;
            int new_yb = ap->portal_spans[i].ybot;
            int yt_att = ap->portal_spans[i].yt_att;
            int yb_att = ap->portal_spans[i].yb_att;
            if (sx < 0) sx = 0;
            if (ex > vw) ex = vw;
            if (sx >= ex)
                continue;

            int w0 = sx >> 5;
            int w1 = (ex - 1) >> 5;
            touched_words |= (hw_span_mask_lut[w1 + 1] ^ hw_span_mask_lut[w0]);

            for (int x = sx; x < ex; x++)
            {
                int yt = ytop[x];
                int yb = ybot[x];
                if (yt_att <= yt + 2 && new_yt > yt) { yt = new_yt; ytop[x] = (int16_t)yt; }
                if (yb_att >= yb - 2 && new_yb < yb) { yb = new_yb; ybot[x] = (int16_t)yb; }
                if (yt >= yb)
                {
                    int w = x >> 5;
                    sub_solid_mask[w] |= hw_bit_lut[x & 31];
                    if (!sub_solid_active)
                    {
                        sub_solid_active = 1;
                        sub_solid_min_w = w;
                        sub_solid_max_w = w;
                    }
                    else
                    {
                        if (w < sub_solid_min_w) sub_solid_min_w = w;
                        if (w > sub_solid_max_w) sub_solid_max_w = w;
                    }
                }
            }
        }
        if (touched_words != 0)
            ap->aperture_active = 1;
    }

    if (sub_solid_active)
    {
        int w_min = sub_solid_min_w;
        int w_max = sub_solid_max_w;
        for (int i = w_min; i <= w_max; i++)
        {
            uint32_t bits = sub_solid_mask[i];
            if (!bits)
                continue;
            sub_solid_mask[i] = 0;
            uint32_t old_v = hw_solid_mask[i];
            uint32_t new_v = old_v | bits;
            if (new_v != old_v)
            {
                hw_solid_mask[i] = new_v;
                uint32_t wbit = hw_bit_lut[i];
                hw_solid_any |= wbit;
                hw_slave_dirty_words |= wbit;
                if (ap->aperture_active)
                    touched_words |= wbit;
                if (new_v == 0xFFFFFFFFu)
                {
                    hw_solid_words++;
                    hw_solid_summary |= wbit;
                }
            }
        }
        sub_solid_active = 0;
        int req_words = (vw + 31) >> 5;
        if (hw_solid_words >= req_words)
            hw_solid_count = vw;
        else
            hw_solid_count = 1;
    }

    if (touched_words != 0)
    {
        const int vh = saturn_view_h;
        const int16_t * __restrict ytop = ap->ytop;
        const int16_t * __restrict ybot = ap->ybot;
        int n_words = (vw + 31) >> 5;
        for (int w = 0; w < n_words; w++)
        {
            if (!(touched_words & hw_bit_lut[w]))
                continue;
            uint32_t sm = hw_solid_mask[w];
            if (sm == 0xFFFFFFFFu)
            {
                ap->ytop_min_w[w] = (int16_t)vh;
                ap->ybot_max_w[w] = 0;
                continue;
            }
            int x_start = w << 5;
            int x_end = x_start + 32;
            if (x_end > vw) x_end = vw;
            int min_yt = vh;
            int max_yb = 0;
            for (int x = x_start; x < x_end; x++)
            {
                if (sm & hw_bit_lut[x & 31])
                    continue;
                int yt = ytop[x];
                int yb = ybot[x];
                if (yt < min_yt) min_yt = yt;
                if (yb > max_yb) max_yb = yb;
            }
            if (min_yt > max_yb)
            {
                min_yt = vh;
                max_yb = 0;
            }
            ap->ytop_min_w[w] = (int16_t)min_yt;
            ap->ybot_max_w[w] = (int16_t)max_yb;
        }
    }
}

static HW_NOINLINE HW_COLD_OS void HW_UpdateNodePvsVis(const byte *pvs_row, const void *ssec_key)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    uint32_t *vis = ap->node_pvs_vis;
    int n_words = (numnodes + 31) >> 5;
    for (int w = 0; w < n_words; w++)
        vis[w] = 0;

    for (int i = 0; i < numnodes; i++)
    {
        int c0 = (int)nodes[i].children[0];
        int v = 0;
        if ((int16_t)c0 < 0)
        {
            if ((int16_t)c0 != -1)
            {
                int ssec = c0 & 0x7FFFu;
                v = (pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]) != 0;
            }
        }
        else if ((unsigned)c0 < (unsigned)numnodes)
        {
            v = (vis[c0 >> 5] & hw_bit_lut[c0 & 31]) != 0;
        }

        if (!v)
        {
            int c1 = (int)nodes[i].children[1];
            if ((int16_t)c1 < 0)
            {
                if ((int16_t)c1 != -1)
                {
                    int ssec = c1 & 0x7FFFu;
                    v = (pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]) != 0;
                }
            }
            else if ((unsigned)c1 < (unsigned)numnodes)
            {
                v = (vis[c1 >> 5] & hw_bit_lut[c1 & 31]) != 0;
            }
        }

        if (v)
            vis[i >> 5] |= hw_bit_lut[i & 31];
    }
    ap->last_pvs_row = (const byte *)ssec_key;
    ap->last_pvs_numnodes = numnodes;
}

/* 2D Map-space AABB Frustum Plane Constants (12-bit fixed point, computed once per frame) */
typedef struct {
    int16_t Nx, Ny;
    int16_t Lx, Ly;
    int16_t Rx, Ry;
    int32_t Cn, Cl, Cr;
    int8_t  idx_Nx, idx_Ny;
    int8_t  idx_Lx, idx_Ly;
    int8_t  idx_Rx, idx_Ry;
    int8_t  nidx_Nx, nidx_Ny;
    int8_t  nidx_Lx, nidx_Ly;
    int8_t  nidx_Rx, nidx_Ry;
} hw_frust_t;
static hw_frust_t hw_frust;

#define hw_bbox_Nx (hw_frust.Nx)
#define hw_bbox_Ny (hw_frust.Ny)
#define hw_bbox_Lx (hw_frust.Lx)
#define hw_bbox_Ly (hw_frust.Ly)
#define hw_bbox_Rx (hw_frust.Rx)
#define hw_bbox_Ry (hw_frust.Ry)
#define hw_bbox_Cn (hw_frust.Cn)
#define hw_bbox_Cl (hw_frust.Cl)
#define hw_bbox_Cr (hw_frust.Cr)
#define hw_bbox_idx_Nx (hw_frust.idx_Nx)
#define hw_bbox_idx_Ny (hw_frust.idx_Ny)
#define hw_bbox_idx_Lx (hw_frust.idx_Lx)
#define hw_bbox_idx_Ly (hw_frust.idx_Ly)
#define hw_bbox_idx_Rx (hw_frust.idx_Rx)
#define hw_bbox_idx_Ry (hw_frust.idx_Ry)
#define hw_bbox_nidx_Nx (hw_frust.nidx_Nx)
#define hw_bbox_nidx_Ny (hw_frust.nidx_Ny)
#define hw_bbox_nidx_Lx (hw_frust.nidx_Lx)
#define hw_bbox_nidx_Ly (hw_frust.nidx_Ly)
#define hw_bbox_nidx_Rx (hw_frust.nidx_Rx)
#define hw_bbox_nidx_Ry (hw_frust.nidx_Ry)

#define MULS_W(a, b) ((int)(int16_t)(a) * (int)(int16_t)(b))

HW_ALWAYS_INLINE int32_t HW_MulsW_PairAdd(int a, int b, int c, int d, int32_t e)
{
#ifndef SATURN_WIN32
    int32_t res, tmp;
    __asm__(
        "muls.w %2, %3 \n\t"
        "mov    %6, %0 \n\t"      /* Hides 1st MAC stall with mov of e into res */
        "sts    macl, %1 \n\t"
        "muls.w %4, %5 \n\t"
        "add    %1, %0 \n\t"      /* Hides 2nd MAC stall with add of 1st product */
        "sts    macl, %1 \n\t"
        "add    %1, %0 \n\t"
        : "=&r"(res), "=&r"(tmp)
        : "r"(a), "r"(b), "r"(c), "r"(d), "r"(e)
        : "mach", "macl"
    );
    return res;
#else
    return ((int32_t)(int16_t)a * (int16_t)b) + ((int32_t)(int16_t)c * (int16_t)d) + e;
#endif
}

#define SATURN_OCLUT_ENTRIES 2048u
#define SATURN_DIVLUT_ADDR SATURN_WRAM_L_DIVLUT_ADDR
#define SATURN_OCLUT ((const unsigned int *)SATURN_WRAM_L_DIVLUT_ADDR)
#define HW_FAST_SKY_ADDR   ((uint8_t *)SATURN_WRAM_L_SKY_ADDR)
static unsigned int hw_last_sky_angle = 0xFFFFFFFFu;
static int          hw_last_sky_cy    = -1;
static int          hw_sky_dirty      = 1;

static void HW_Divlut_Init(void)
{
    static int initialized = 0;
    if (__builtin_expect(initialized, 1))
        return;
    initialized = 1;

    volatile unsigned int *lut = (volatile unsigned int *)SATURN_DIVLUT_ADDR;
    lut[0] = 10485760u / HW_NEAR_Z_UNITS;
    lut[1] = 10485760u / HW_NEAR_Z_UNITS;
    for (unsigned int d = 2; d < SATURN_OCLUT_ENTRIES; d++)
        lut[d] = 10485760u / d;
}

/* SH-2 Hardware 32x32->64 bit MAC multiplier with 16.16 fixed-point extraction:
 * Replaces C FixedMul function calls with a 4-cycle hardware assembly sequence. */
HW_ALWAYS_INLINE fixed_t HW_FixedMul_SH2(fixed_t a, fixed_t b)
{
#ifndef SATURN_WIN32
    register int res;
    register int hi;
    __asm__(
        "dmuls.l %2, %3\n\t"
        "sts mach, %1\n\t"
        "sts macl, %0\n\t"
        "xtrct %1, %0"
        : "=&r"(res), "=&r"(hi)
        : "r"(a), "r"(b)
        : "mach", "macl");
    return res;
#else
    return (fixed_t)(((int64_t)a * (int64_t)b) >> 16);
#endif
}
HW_ALWAYS_INLINE void HW_FixedMulPair(fixed_t a1, fixed_t b1, fixed_t a2, fixed_t b2,
                                       fixed_t *out1, fixed_t *out2)
{
#ifndef SATURN_WIN32
    int hi1, hi2;
    __asm__(
        "dmuls.l %4, %5 \n\t"   /* m1 = a1 * b1 */
        "sts     mach, %2 \n\t" /* hi1 */
        "sts     macl, %0 \n\t" /* lo1 */
        "dmuls.l %6, %7 \n\t"   /* m2 = a2 * b2 */
        "xtrct   %2, %0 \n\t"   /* *out1 = xtrct(hi1, lo1) while m2 runs */
        "sts     mach, %3 \n\t" /* hi2 */
        "sts     macl, %1 \n\t" /* lo2 */
        "xtrct   %3, %1 \n\t"   /* *out2 = xtrct(hi2, lo2) */
        : "=&r"(*out1), "=&r"(*out2), "=&r"(hi1), "=&r"(hi2)
        : "r"(a1), "r"(b1), "r"(a2), "r"(b2)
        : "mach", "macl"
    );
#else
    *out1 = (fixed_t)(((int64_t)a1 * (int64_t)b1) >> 16);
    *out2 = (fixed_t)(((int64_t)a2 * (int64_t)b2) >> 16);
#endif
}
#undef FixedMul
#define FixedMul(a, b) HW_FixedMul_SH2(a, b)

/* Direct 32x32->upper 32-bit hardware signed multiplier:
 * For multiplying two 16.16 fixed-point numbers and extracting the integer part.
 * Replaces (FixedMul(a, b) >> FRACBITS) with 0-stall hardware assembly sequence. */
HW_ALWAYS_INLINE int HW_MulHigh(int a, int b)
{
#ifndef SATURN_WIN32
    int res;
    __asm__(
        "dmuls.l %1, %2 \n\t"
        "sts     mach, %0"
        : "=r" (res)
        : "r" (a), "r" (b)
        : "mach", "macl"
    );
    return res;
#else
    return (int)(((int64_t)a * (int64_t)b) >> 32);
#endif
}

/* Fast 0-stall X screen projection absorbing HW_VIEW_CX add into dmuls.l latency */
HW_ALWAYS_INLINE int HW_ProjX(int cx, int s, int view_cx)
{
#ifndef SATURN_WIN32
    int px, tmp;
    __asm__(
        "dmuls.l %2, %3 \n\t"
        "mov     %4, %0 \n\t"   /* px = view_cx (hides 1st cycle latency of dmuls.l!) */
        "sts     mach, %1 \n\t" /* tmp = hi(cx * s) */
        "add     %1, %0 \n\t"   /* px = view_cx + tmp */
        : "=&r"(px), "=&r"(tmp)
        : "r"(cx), "r"(s), "r"(view_cx)
        : "mach", "macl"
    );
    return px;
#else
    return view_cx + (int)(((int64_t)cx * (int64_t)s) >> 32);
#endif
}

/* Fast 0-stall dual X screen projection saturating the SH-2 MAC pipeline */
HW_ALWAYS_INLINE void HW_ProjXPair(int cx1, int s1, int cx2, int s2, int view_cx, int *px1, int *px2)
{
#ifndef SATURN_WIN32
    int r1, r2, tmp;
    __asm__(
        "dmuls.l %4, %5 \n\t"   /* Start m1 = cx1 * s1 */
        "mov     %3, %0 \n\t"   /* px1 = view_cx (hides 1st cycle latency!) */
        "mov     %3, %1 \n\t"   /* px2 = view_cx (hides 2nd cycle latency!) */
        "sts     mach, %2 \n\t" /* tmp = hi(m1) (0 STALLS) */
        "dmuls.l %6, %7 \n\t"   /* Start m2 = cx2 * s2 */
        "add     %2, %0 \n\t"   /* px1 = view_cx + tmp */
        "sts     mach, %2 \n\t" /* tmp = hi(m2) */
        "add     %2, %1 \n\t"   /* px2 = view_cx + tmp */
        : "=&r"(r1), "=&r"(r2), "=&r"(tmp)
        : "r"(view_cx), "r"(cx1), "r"(s1), "r"(cx2), "r"(s2)
        : "mach", "macl"
    );
    *px1 = r1; *px2 = r2;
#else
    *px1 = view_cx + (int)(((int64_t)cx1 * (int64_t)s1) >> 32);
    *px2 = view_cx + (int)(((int64_t)cx2 * (int64_t)s2) >> 32);
#endif
}

/* Fast 0-stall Y screen projection absorbing HW_VIEW_CY load into dmuls.l latency */
HW_ALWAYS_INLINE int HW_ProjY(int dz, int s, int view_cy)
{
#ifndef SATURN_WIN32
    int py, tmp;
    __asm__(
        "dmuls.l %2, %3 \n\t"
        "mov     %4, %0 \n\t"   /* py = view_cy (hides 1st cycle latency of dmuls.l!) */
        "sts     mach, %1 \n\t" /* tmp = hi(dz * s) */
        "sub     %1, %0 \n\t"   /* py = view_cy - tmp */
        : "=&r"(py), "=&r"(tmp)
        : "r"(dz), "r"(s), "r"(view_cy)
        : "mach", "macl"
    );
    return py;
#else
    return view_cy - (int)(((int64_t)dz * (int64_t)s) >> 32);
#endif
}

/* Projects 2D pillar coordinates (dx, dy) into camera space (cz, cx)
 * using interleaved dual-MAC scheduling with ZERO multiplier latency stalls (Hitachi Law C compliant). */
HW_ALWAYS_INLINE void ProjectPillar_2D(int dx, int dy, fixed_t vcos, fixed_t vsin,
                                       fixed_t cz_orig, fixed_t cx_orig,
                                       fixed_t *out_cz, fixed_t *out_cx)
{
#ifndef SATURN_WIN32
    int cz, cx, tmp;
    __asm__(
        "dmuls.l %3, %6 \n\t"   /* MAC = dx * vsin */
        "mov     %8, %0 \n\t"   /* cx = cx_orig (Fills 1-cycle MAC latency slot!) */
        "sts     macl, %2 \n\t" /* tmp = dx * vsin (0 stalls) */

        "dmuls.l %4, %5 \n\t"   /* MAC = dy * vcos */
        "sub     %2, %0 \n\t"   /* cx = cx_orig - dx * vsin */
        "sts     macl, %2 \n\t" /* tmp = dy * vcos */

        "dmuls.l %3, %5 \n\t"   /* MAC = dx * vcos */
        "add     %2, %0 \n\t"   /* cx = cx_orig - dx*vsin + dy*vcos */
        "sts     macl, %2 \n\t" /* tmp = dx * vcos */

        "dmuls.l %4, %6 \n\t"   /* MAC = dy * vsin */
        "neg     %0, %0 \n\t"   /* cx = dx*vsin - dy*vcos - cx_orig (cx is done!) */
        "sts     macl, %1 \n\t" /* cz = dy * vsin */

        "add     %2, %1 \n\t"   /* cz = dy*vsin + dx*vcos */
        "sub     %7, %1 \n\t"   /* cz = dy*vsin + dx*vcos - cz_orig (cz is done!) */
        : "=&r"(cx), "=&r"(cz), "=&r"(tmp)
        : "r"(dx), "r"(dy), "r"(vcos), "r"(vsin), "r"(cz_orig), "r"(cx_orig)
        : "mach", "macl"
    );
    *out_cz = cz;
    *out_cx = cx;
#else
    *out_cz = (fixed_t)(((int64_t)dx * vcos + (int64_t)dy * vsin) - cz_orig);
    *out_cx = (fixed_t)(((int64_t)dx * vsin - (int64_t)dy * vcos) - cx_orig);
#endif
}

static HW_NOINLINE void ProjectPillar_2D_Shifted(int dx, int dy, fixed_t vcos, fixed_t vsin,
                                                 fixed_t cz_orig, fixed_t cx_orig,
                                                 fixed_t *out_cz, fixed_t *out_cx)
{
    unsigned int abs_dx = (unsigned int)((dx ^ (dx >> 31)) - (dx >> 31));
    unsigned int abs_dy = (unsigned int)((dy ^ (dy >> 31)) - (dy >> 31));
    int shift = 1 + ((abs_dx | abs_dy) > 524288u);
    fixed_t cz, cx;
    ProjectPillar_2D(dx >> shift, dy >> shift, vcos, vsin, cz_orig >> shift, cx_orig >> shift, &cz, &cx);
    fixed_t limit = 0x7FFFFFFF >> shift;
    if (cz > limit) cz = limit;
    else if (cz < -limit) cz = -limit;
    if (cx > limit) cx = limit;
    else if (cx < -limit) cx = -limit;
    *out_cz = (fixed_t)((uint32_t)cz << shift);
    *out_cx = (fixed_t)((uint32_t)cx << shift);
}

/* Shared height projection (floor/wall seam fix).
 * Walls and flats MUST project heights with identical math, otherwise their
 * shared edges land on different rows (gaps that flicker with view bob).
 * Before the fix, walls dropped the viewz fraction ((z - vz) >> 16) and
 * floors used CY + hi((vz - floor) * s), which rounds the other way.
 *   dz = z - vz in 16.16 (|dz| < 32768 units, cannot overflow),
 *   s  = OCLUT scale in 16.16 px/unit,
 *   screen y = HW_VIEW_CY - hi32(dz * s)   (exact floor, no shifts:
 *   the SH-2 has no barrel shifter). |dz * s| < 2^49, fits dmuls.l. */
#define HW_DZQ(z, vz)     ((int)(z) - (int)(vz))
#define HW_PROJ_Y(dz, s)  HW_ProjY((dz), (s), HW_VIEW_CY)


/* Two independent Y projections cy - hi32(dz * s), absorbing cy - hi32 into dmuls.l latency slots (0 stalls) */
HW_ALWAYS_INLINE void HW_ProjYPair(int cy, int dz1, int s1, int dz2, int s2,
                                   int *out1, int *out2)
{
#ifndef SATURN_WIN32
    int r1, r2, tmp;
    __asm__(
        "dmuls.l %4, %5 \n\t"   /* Start m1 = dz1 * s1 */
        "mov     %3, %0 \n\t"   /* %0 = cy (hides 1st cycle latency of m1) */
        "mov     %3, %1 \n\t"   /* %1 = cy (hides 2nd cycle latency of m1!) */
        "sts     mach, %2 \n\t" /* tmp = mach(m1) (0 STALLS!) */
        "dmuls.l %6, %7 \n\t"   /* Start m2 = dz2 * s2 */
        "sub     %2, %0 \n\t"   /* %0 = cy - tmp (hides 1st cycle latency of m2) */
        "sts     mach, %2 \n\t" /* tmp = mach(m2) (SH-2 auto-interlocks on mach if needed, saving 2B per call) */
        "sub     %2, %1 \n\t"   /* %1 = cy - tmp */
        : "=&r"(r1), "=&r"(r2), "=&r"(tmp)
        : "r"(cy), "r"(dz1), "r"(s1), "r"(dz2), "r"(s2)
        : "mach", "macl"
    );
    *out1 = r1;
    *out2 = r2;
#else
    *out1 = cy - (int)(((int64_t)dz1 * (int64_t)s1) >> 32);
    *out2 = cy - (int)(((int64_t)dz2 * (int64_t)s2) >> 32);
#endif
}

/* Zero-Stall 64-bit Cross Product Sign for 2D Map-Space Backface Culling:
 * Evaluates sign64((int64_t)obj_dy * seg_dx - (int64_t)obj_dx * seg_dy) using 32x32->64 dmuls.l.
 * Immune to 32-bit signed overflow up to the full 65535x65535 Doom map span (~8.58B delta)
 * while preserving exact 1-unit seg precision and eliminating I-cache NOP padding. */
HW_ALWAYS_INLINE int HW_Cross32(int obj_dy, int seg_dx, int obj_dx, int seg_dy)
{
#ifndef SATURN_WIN32
    int res, lo1, lo2;
    __asm__(
        "dmuls.l %3, %4 \n\t"   /* p1 = obj_dy * seg_dx */
        "clrt           \n\t"   /* T = 0 (hides 1st dmuls.l latency slot!) */
        "sts     macl, %1 \n\t" /* lo1 (0 stalls) */
        "sts     mach, %0 \n\t" /* hi1 */
        "dmuls.l %5, %6 \n\t"   /* p2 = obj_dx * seg_dy */
        "sts     macl, %2 \n\t" /* lo2 (SH-2 hardware interlocks 1 cycle, no nop needed) */
        "subc    %2, %1 \n\t"   /* lo = lo1 - lo2, T = unsigned borrow */
        "sts     mach, %2 \n\t" /* hi2 */
        "subc    %2, %0 \n\t"   /* hi = hi1 - hi2 - borrow */
        "clrt           \n\t"   /* T = 0 */
        "negc    %1, %1 \n\t"   /* T = (lo != 0) */
        "addc    %0, %0 \n\t"   /* res = (hi * 2) + (lo != 0): exact <0, ==0, >0 sign! */
        : "=&r"(res), "=&r"(lo1), "=&r"(lo2)
        : "r"(obj_dy), "r"(seg_dx), "r"(obj_dx), "r"(seg_dy)
        : "mach", "macl", "t"
    );
    return res;
#else
    int64_t d = (int64_t)obj_dy * seg_dx - (int64_t)obj_dx * seg_dy;
    return (d > 0) - (d < 0);
#endif
}

/* Fast 16x16->32 Cross Product for Baked Wall Backface Culling:
 * For any four int16_t values in [-32767, 32767], |a*b - c*d| <= 2 * 32767^2 = 2,147,352,578 < INT32_MAX,
 * so 32-bit signed overflow is mathematically impossible and uses 2-register muls.w instead of 7-register dmuls.l. */
HW_ALWAYS_INLINE int HW_CrossBackface(int obj_dy, int seg_dx, int obj_dx, int seg_dy)
{
    if (__builtin_expect((((unsigned int)(obj_dx + 32767) | (unsigned int)(obj_dy + 32767) |
                           (unsigned int)(seg_dx + 32767) | (unsigned int)(seg_dy + 32767)) <= 65534u), 1))
    {
        return ((int)(int16_t)obj_dy * (int)(int16_t)seg_dx) - ((int)(int16_t)obj_dx * (int)(int16_t)seg_dy);
    }
    return HW_Cross32(obj_dy, seg_dx, obj_dx, seg_dy);
}

/* Note on SH-2 DIVU interrupt safety:
 * Status register (SR) interrupts are masked (IMASK=15) across the 39..55 cycle
 * (~1.3..1.9 us) hardware division window and restored immediately when DVDNTL is
 * read in HW_FinishDivu_Background, guaranteeing no ISR can ever preempt and
 * overwrite the shared SH-2 hardware divider registers. */
HW_ALWAYS_INLINE uint32_t HW_StartDivu_Background(unsigned int t, unsigned int step_dx)
{
#ifndef SATURN_WIN32
    if (__builtin_expect(((t | step_dx) & 0x80000000u) != 0, 0))
    {
        t >>= 2;
        step_dx >>= 2;
        if (step_dx == 0) step_dx = 1;
    }
    uint32_t sr_save;
    __asm__ volatile("stc sr, %0" : "=r"(sr_save));
    __asm__ volatile("ldc %0, sr" : : "r"(sr_save | 0xF0u) : "memory");
    SH2_DIVU_DVSR   = step_dx;
    SH2_DIVU_DVDNTH = (t >> 16);
    SH2_DIVU_DVDNTL = (t << 16);
    return sr_save;
#else
    (void)t; (void)step_dx;
    return 0;
#endif
}

HW_ALWAYS_INLINE uint32_t HW_FinishDivu_Background(uint32_t sr_save)
{
#ifndef SATURN_WIN32
    uint32_t res = (uint32_t)SH2_DIVU_DVDNTL;
    __asm__ volatile("ldc %0, sr" : : "r"(sr_save) : "memory");
    return res;
#else
    (void)sr_save;
    return 0;
#endif
}

/* SH-2 Hardware 64/32 divider for screen clamping:
 * Computes (t << 16) / step_dx using on-chip DIVU in 39 cycles.
 * noinline: the 40-NOP wait was inlined at every (rare-path) clamp
 * site, ~90 B each; the divide latency dwarfs the call. */
static __attribute__((noinline)) int HW_ClampedFrac(unsigned int t, unsigned int step_dx)
{
    if (__builtin_expect(t >= step_dx, 0))
        return step_dx == 0 ? 0 : 65536;
#ifndef SATURN_WIN32
    uint32_t sr_save = HW_StartDivu_Background(t, step_dx);
    return (int)HW_FinishDivu_Background(sr_save); /* Reading stalls CPU until 39-cycle hardware divide completes */
#else
    return (int)(((uint64_t)t << 16) / step_dx);
#endif
}

/* Fast branchless bounds check for VDP1 11-bit Y range [-1024, 1023] */
HW_ALWAYS_INLINE int HW_ClampY(int v)
{
    if (__builtin_expect((unsigned int)v + 1024u <= 2047u, 1))
        return v;
    return (v >> 31) ^ 1023;
}

static int hw_wall_coord_tmp[9];

/* Unified X & Y DDA Overflow & Bow-Tie Protection (1 register argument r4 = c[6], 0 stack args):
 * c[0]=xa, c[1]=xb, c[2]=ya0, c[3]=yb0, c[4]=ya1, c[5]=yb1. */
static HW_NOINLINE HW_COLD_OS void HW_WallXYClamp_Slow(int *c)
{
    int xa = c[0], xb = c[1], ya0 = c[2], yb0 = c[3], ya1 = c[4], yb1 = c[5];
    if (xa < -500 || xb > 500)
    {
        int dx = xb - xa;
        if (dx > 0)
        {
            int o_ya0 = ya0, o_yb0 = yb0, o_ya1 = ya1, o_yb1 = yb1;
            int dya0 = o_yb0 - o_ya0;
            int dya1 = o_yb1 - o_ya1;
            if (xa < -500)
            {
                int f = HW_ClampedFrac((unsigned int)(-500 - xa), (unsigned int)dx);
                if (f > FRACUNIT) f = FRACUNIT;
                ya0 = o_ya0 + FixedMul(dya0, f);
                ya1 = o_ya1 + FixedMul(dya1, f);
                xa  = -500;
            }
            if (xb > 500)
            {
                int f = HW_ClampedFrac((unsigned int)(500 - c[0]), (unsigned int)dx);
                if (f > FRACUNIT) f = FRACUNIT;
                yb0 = o_ya0 + FixedMul(dya0, f);
                yb1 = o_ya1 + FixedMul(dya1, f);
                xb  = 500;
            }
            if (xb <= xa)
                xb = xa + 1;
        }
        else
        {
            xa = HW_CLAMP_PROJ(xa);
            xb = HW_CLAMP_PROJ(xb);
        }
        c[0] = xa;
        c[1] = xb;
    }
    if ((((unsigned int)(ya1 - ya0) | (unsigned int)(yb1 - yb0)) > 1023u) |
        HW_ANY_OUT1024(ya0, yb0, ya1, yb1))
    {
        if (ya1 < ya0) {
            ya1 = ya0;
        } else if (ya1 - ya0 > 1023) {
            if (ya0 < -400 && ya1 > 623) { ya0 = -400; ya1 = 623; }
            else if (ya0 < -400) { ya0 = ya1 - 1023; }
            else if (ya1 > 623) { ya1 = ya0 + 1023; }
        }
        if (yb1 < yb0) {
            yb1 = yb0;
        } else if (yb1 - yb0 > 1023) {
            if (yb0 < -400 && yb1 > 623) { yb0 = -400; yb1 = 623; }
            else if (yb0 < -400) { yb0 = yb1 - 1023; }
            else if (yb1 > 623) { yb1 = yb0 + 1023; }
        }
        c[2] = HW_ClampY(ya0);
        c[3] = HW_ClampY(yb0);
        c[4] = HW_ClampY(ya1);
        c[5] = HW_ClampY(yb1);
    }
    else
    {
        c[2] = ya0;
        c[3] = yb0;
        c[4] = ya1;
        c[5] = yb1;
    }
}

#define HW_WALL_XYCLAMP(xa, xb, ya0, yb0, ya1, yb1)                                        \
    do {                                                                                   \
        if (__builtin_expect(((xa) < -500) | ((xb) > 500) |                                \
                             ((((unsigned int)((ya1) - (ya0)) |                            \
                                (unsigned int)((yb1) - (yb0))) > 1023u) |                  \
                              HW_ANY_OUT1024(ya0, yb0, ya1, yb1)), 0))                     \
        {                                                                                  \
            hw_wall_coord_tmp[0] = (xa);  hw_wall_coord_tmp[1] = (xb);                     \
            hw_wall_coord_tmp[2] = (ya0); hw_wall_coord_tmp[3] = (yb0);                    \
            hw_wall_coord_tmp[4] = (ya1); hw_wall_coord_tmp[5] = (yb1);                    \
            HW_WallXYClamp_Slow(hw_wall_coord_tmp);                                        \
            (xa)  = hw_wall_coord_tmp[0]; (xb)  = hw_wall_coord_tmp[1];                    \
            (ya0) = hw_wall_coord_tmp[2]; (yb0) = hw_wall_coord_tmp[3];                    \
            (ya1) = hw_wall_coord_tmp[4]; (yb1) = hw_wall_coord_tmp[5];                    \
        }                                                                                  \
    } while (0)

HW_ALWAYS_INLINE int HW_FastDiv_Slow(unsigned int dist)
{
    /* Evaluates to shift = 1, 2, 3, or 4 with zero pipeline flushes */
    unsigned int shift = 1 + (dist >= 4096) + (dist >= 8192) + (dist >= 16384);
    unsigned int idx = dist >> shift;
    if (idx >= SATURN_OCLUT_ENTRIES)
        idx = SATURN_OCLUT_ENTRIES - 1;
    return (int)(SATURN_OCLUT[idx] >> shift);
}

HW_ALWAYS_INLINE int HW_FastDiv(fixed_t cz)
{
    unsigned int dist = (unsigned int)cz >> FRACBITS;
    if (__builtin_expect(dist < SATURN_OCLUT_ENTRIES, 1))
        return (int)SATURN_OCLUT[dist];
    return HW_FastDiv_Slow(dist);
}

#define HW_VCACHE_SIZE 1024
#define HW_VCACHE_MASK (HW_VCACHE_SIZE - 1)

/* Padded to exactly 32 bytes for 1-cycle bit-shift array indexing (idx << 5).
 * Mapped directly to unused Low Work RAM buffer SATURN_WRAM_L_PILLARS_ADDR (32 KB),
 * eliminating 5,120 bytes of High Work RAM BSS to strictly protect __bend. */
typedef struct {
    int32_t  v_idx;   /* Replaces int16_t v_idx + uint16_t pad1 */
    uint32_t frame;
    fixed_t  cx;
    fixed_t  cz;
    int      s;
    int      px;
    int      pad[2]; /* FIX: Pad to 32-bytes for bit-shift array indexing */
} saturn_vcache_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(saturn_vcache_t) == 32, "saturn_vcache_t must be exactly 32 bytes for bit-shift indexing");
#elif defined(_MSC_VER)
static_assert(sizeof(saturn_vcache_t) == 32, "saturn_vcache_t must be exactly 32 bytes for bit-shift indexing");
#endif

static saturn_vcache_t *hw_vcache = (saturn_vcache_t *)SATURN_WRAM_L_PILLARS_ADDR;
static uint32_t        hw_vcache_frame = 0;
static int             hw_seg_last_line_idx = -1;
static int             hw_seg_last_side_idx = -1;
static side_t         *hw_seg_last_sidedef = NULL;
static sector_t       *hw_seg_last_backsec = NULL;
static int             hw_seg_last_mid_srca, hw_seg_last_mid_tw, hw_seg_last_mid_rows;
static int             hw_seg_last_top_srca, hw_seg_last_top_tw, hw_seg_last_top_rows;
static int             hw_seg_last_bot_srca, hw_seg_last_bot_tw, hw_seg_last_bot_rows;
static int             hw_seg_last_mmid_srca, hw_seg_last_mmid_tw, hw_seg_last_mmid_th;

static HW_NOINLINE HW_COLD_OS const saturn_vcache_t *HW_GetVertex(int v_idx, const mapvertex_t *v)
{
    saturn_vcache_t *entry = &hw_vcache[v_idx & HW_VCACHE_MASK];

    if (entry->v_idx == v_idx && entry->frame == hw_vcache_frame)
    {
        return entry;
    }

    int dx = v->x - hw_cam_x;
    int dy = v->y - hw_cam_y;
    int shift = 0;

    if (__builtin_expect((((unsigned int)dx + 16384u) | ((unsigned int)dy + 16384u)) > 32767u, 0)) {
        if ((((unsigned int)dx + 32768u) | ((unsigned int)dy + 32768u)) > 65535u) {
            dx >>= 2; dy >>= 2; shift = 2;
        } else {
            dx >>= 1; dy >>= 1; shift = 1;
        }
    }

    fixed_t cz, cx;
    ProjectPillar_2D(dx, dy, hw_viewcos, hw_viewsin, cam_cz_origin >> shift, cam_cx_origin >> shift, &cz, &cx);
    if (shift) {
        fixed_t limit = 0x7FFFFFFF >> shift;
        if (cz > limit) cz = limit;
        else if (cz < -limit) cz = -limit;
        if (cx > limit) cx = limit;
        else if (cx < -limit) cx = -limit;
        cz = (fixed_t)((uint32_t)cz << shift);
        cx = (fixed_t)((uint32_t)cx << shift);
    }
    int s = 0, px = 0;

    if (cz >= HW_NEAR_Z)
    {
        s = HW_FastDiv(cz);
        px = HW_ProjX(cx, s, HW_VIEW_CX);
    }

    entry->v_idx = v_idx;
    entry->frame = hw_vcache_frame;
    entry->cx    = cx;
    entry->cz    = cz;
    entry->s     = s;
    entry->px    = px;

    return entry;
}

typedef struct {
    const uint16_t *cmdcolr_lut;
} hw_shade_ctx_t;

HW_ALWAYS_INLINE int HW_WallLightBank(int lightlevel, int dist)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)dist >> 4;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)wall_zlight_bank_LUT[s][zb];
}

HW_ALWAYS_INLINE int HW_WallLightLevel(int lightlevel, int dist)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)dist >> 4;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)wall_zlight_level_LUT[s][zb];
}

HW_ALWAYS_INLINE uint16_t HW_WallLightColr(int lightlevel, int dist)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)dist >> 4;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return wall_zlight_cmdcolr_LUT[s][zb] & 0xFF00u;
}

HW_ALWAYS_INLINE int HW_FlatLightBank(int lightlevel, int cz)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)cz >> 20;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)flat_zlight_bank_LUT[s][zb];
}

HW_ALWAYS_INLINE int HW_FlatLightLevel(int lightlevel, int cz)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)cz >> 20;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)flat_zlight_level_LUT[s][zb];
}

HW_ALWAYS_INLINE uint16_t HW_FlatLightColr(int lightlevel, int cz)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)cz >> 20;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return flat_zlight_cmdcolr_LUT[s][zb] & 0xFF00u;
}

HW_ALWAYS_INLINE void HW_InitShadeCtx(hw_shade_ctx_t *ctx, int lightlevel)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    ctx->cmdcolr_lut = wall_zlight_cmdcolr_LUT[s];
}

HW_ALWAYS_INLINE int HW_FastShadeCtx(const hw_shade_ctx_t *ctx, int dist)
{
    unsigned int zb = (unsigned int)dist >> 4;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)ctx->cmdcolr_lut[zb];
}

HW_ALWAYS_INLINE int HW_FastShade(int lightlevel, int dist)
{
    return (int)HW_WallLightColr(lightlevel, dist);
}

HW_ALWAYS_INLINE int HW_FastLightRow(int lightlevel, int dist)
{
    return (int)HW_WallLightLevel(lightlevel, dist);
}

/* Fast emitter for VDP1 user clip area commands (CMD_USERCLIP = 0x0008). */
static HW_NOINLINE HW_COLD_OS void HW_EmitUserClip(int x0, int y0, int x1, int y1)
{
    uint16_t cnt = saturn_cmd_m_count;
    if (cnt < CMDBUF_M_CAP)
    {
        int x_offset = (HW_MAX_SCREEN_W - saturn_view_w) >> 1;
        x0 += x_offset;
        x1 += x_offset;
        aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[cnt];
        cmd32[0] = 0x00080000u;
        cmd32[1] = 0;
        cmd32[2] = 0;
        cmd32[3] = ((uint32_t)x0 << 16) | (uint16_t)y0;
        cmd32[4] = 0;
        cmd32[5] = ((uint32_t)x1 << 16) | (uint16_t)y1;
        cmd32[6] = 0;
        cmd32[7] = 0;

        saturn_cmd_m_count = cnt + 1;
    }
}

/* Direct fast emitter for wall quads: writes directly into VDP1 distorted quad
 * table, bypassing intermediate wrapper functions and redundant texture lookups. */
static HW_NOINLINE HW_COLD_OS void HW_EmitWallQuadDirect(int xa, int ya0, int ya1,
                                         int xb, int yb0, int yb1,
                                         int srca, int tw, int rows, int k_shade, int mid)
{
    uint16_t cnt = saturn_cmd_m_count;
    if (cnt < CMDBUF_M_CAP)
    {
        aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[cnt];
        uint32_t pmod = ((mid ? (PMOD_WALL & ~0x0040u) : PMOD_WALL) | PMOD_CLIP_IN) & 0xFFFFu;
        uint32_t colr = (uint32_t)k_shade & 0xFF00u;
        uint32_t size = ((uint32_t)(tw >> 3) << 8) | ((uint32_t)rows & 0xFFu);

        cmd32[0] = 0x00020000u;
        cmd32[1] = (pmod << 16) | colr;
        cmd32[2] = ((uint32_t)srca << 16) | size;
        cmd32[3] = ((uint32_t)xa << 16) | (uint16_t)ya0;
        cmd32[4] = ((uint32_t)xb << 16) | (uint16_t)yb0;
        cmd32[5] = ((uint32_t)xb << 16) | (uint16_t)yb1;
        cmd32[6] = ((uint32_t)xa << 16) | (uint16_t)ya1;
        cmd32[7] = 0;

        saturn_cmd_m_count = cnt + 1;
        saturn_v1w_cells++;
        saturn_hw_wall_quads++;
    }
    else
    {
        saturn_v1w_drop++;
        saturn_v1w_captured = 1;
    }
}


/* 1 = the camera-space point set (a convex region's corners) lies
 * entirely outside one frustum plane, so nothing inside it can be on
 * screen. Planes: near (cz >= HW_NEAR_Z), left (cx + k*cz >= 0) and
 * right (cx - k*cz <= 0), k = 1.25 (the 90-degree FOV's k = 1 plus a
 * 25% margin for rounding and sprite overhang). The side planes pass
 * through the eye, so the half-space test is exact for points BEHIND
 * the camera too — unlike an "all corners in front and off to one side"
 * test, it also rejects regions behind-and-beside the player.
 * Evaluated at 1/4 scale so cx + 1.25*cz can't overflow 32 bits. */
HW_ALWAYS_INLINE int HW_OutsideFrustum(const fixed_t *cx, const fixed_t *cz, int n)
{
    if (n == 4)
    {
        fixed_t qx0 = cx[0] >> 2; fixed_t qz0 = (cz[0] >> 2) + (cz[0] >> 4);
        fixed_t qx1 = cx[1] >> 2; fixed_t qz1 = (cz[1] >> 2) + (cz[1] >> 4);
        fixed_t qx2 = cx[2] >> 2; fixed_t qz2 = (cz[2] >> 2) + (cz[2] >> 4);
        fixed_t qx3 = cx[3] >> 2; fixed_t qz3 = (cz[3] >> 2) + (cz[3] >> 4);

        if (((qx0 + qz0) & (qx1 + qz1) & (qx2 + qz2) & (qx3 + qz3)) < 0) return 1;
        if (((qz0 - qx0) & (qz1 - qx1) & (qz2 - qx2) & (qz3 - qx3)) < 0) return 1;

        if (((cz[0] - HW_NEAR_Z) & (cz[1] - HW_NEAR_Z) & (cz[2] - HW_NEAR_Z) & (cz[3] - HW_NEAR_Z)) < 0) return 1;

        return 0;
    }
    else
    {
        int k, any_behind = 0, all_behind = 1, left = 1, right = 1;
        for (k = 0; k < n; k++)
        {
            if (cz[k] >= HW_NEAR_Z) all_behind = 0;
            else any_behind = 1;

            fixed_t qx = cx[k] >> 2;
            fixed_t qz = (cz[k] >> 2) + (cz[k] >> 4);
            if (qx + qz >= 0) left = 0;
            if (qx - qz <= 0) right = 0;
        }
        if (all_behind || left || right) return 1;
        return 0;
    }
}

#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
static void Saturn_ExpandNodeBBoxesWithFlats(void)
{
    if (!hw_node_bboxes || !saturn_baked_flat_bboxes || numnodes <= 0)
        return;

    int16_t (*node_flat)[4] = (int16_t (*)[4])Z_Malloc(numnodes * 4 * sizeof(int16_t), PU_LEVEL);
    uint8_t *node_state = (uint8_t *)Z_Malloc(numnodes * sizeof(uint8_t), PU_LEVEL);
    int16_t *stack = (int16_t *)Z_Malloc(numnodes * sizeof(int16_t), PU_LEVEL);
    if (!node_flat || !node_state || !stack)
    {
        if (stack) Z_Free(stack);
        if (node_state) Z_Free(node_state);
        if (node_flat) Z_Free(node_flat);
        return;
    }

    D_memset(node_state, 0, (size_t)numnodes);

    /* Strict O(N) iterative post-order traversal:
     * Guarantees single-pass resolution regardless of whether the node builder
     * ordered BSP nodes bottom-up or top-down, with cycle protection (state 3). */
    for (int root = numnodes - 1; root >= 0; root--)
    {
        if (node_state[root])
            continue;

        int sp = 0;
        stack[sp++] = (int16_t)root;

        while (sp > 0)
        {
            int i = (int)stack[sp - 1];
            if (node_state[i] == 0)
            {
                node_state[i] = 3; /* Mark children being pushed */
                int c0 = (int)nodes[i].children[0];
                if ((int16_t)c0 >= 0 && c0 < numnodes && node_state[c0] == 0 && sp < numnodes)
                    stack[sp++] = (int16_t)c0;
                int c1 = (int)nodes[i].children[1];
                if ((int16_t)c1 >= 0 && c1 < numnodes && node_state[c1] == 0 && sp < numnodes)
                    stack[sp++] = (int16_t)c1;
                continue;
            }

            sp--;
            if (node_state[i] != 3)
                continue;

            int c0 = (int)nodes[i].children[0];
            int has0 = 0;
            int16_t b0[4];

            if ((int16_t)c0 < 0)
            {
                if ((int16_t)c0 != -1)
                {
                    int subnum = c0 & 0x7FFFu;
                    if (subnum >= 0 && subnum < numsubsectors)
                    {
                        const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
                        if (fb->min_x != fb->max_x || fb->min_y != fb->max_y)
                        {
                            b0[BOXTOP]    = fb->max_y;
                            b0[BOXBOTTOM] = fb->min_y;
                            b0[BOXLEFT]   = fb->min_x;
                            b0[BOXRIGHT]  = fb->max_x;
                            has0 = 1;
                        }
                    }
                }
            }
            else if (c0 >= 0 && c0 < numnodes && node_state[c0] == 2)
            {
                b0[BOXTOP]    = node_flat[c0][BOXTOP];
                b0[BOXBOTTOM] = node_flat[c0][BOXBOTTOM];
                b0[BOXLEFT]   = node_flat[c0][BOXLEFT];
                b0[BOXRIGHT]  = node_flat[c0][BOXRIGHT];
                has0 = 1;
            }

            int c1 = (int)nodes[i].children[1];
            int has1 = 0;
            int16_t b1[4];

            if ((int16_t)c1 < 0)
            {
                if ((int16_t)c1 != -1)
                {
                    int subnum = c1 & 0x7FFFu;
                    if (subnum >= 0 && subnum < numsubsectors)
                    {
                        const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
                        if (fb->min_x != fb->max_x || fb->min_y != fb->max_y)
                        {
                            b1[BOXTOP]    = fb->max_y;
                            b1[BOXBOTTOM] = fb->min_y;
                            b1[BOXLEFT]   = fb->min_x;
                            b1[BOXRIGHT]  = fb->max_x;
                            has1 = 1;
                        }
                    }
                }
            }
            else if (c1 >= 0 && c1 < numnodes && node_state[c1] == 2)
            {
                b1[BOXTOP]    = node_flat[c1][BOXTOP];
                b1[BOXBOTTOM] = node_flat[c1][BOXBOTTOM];
                b1[BOXLEFT]   = node_flat[c1][BOXLEFT];
                b1[BOXRIGHT]  = node_flat[c1][BOXRIGHT];
                has1 = 1;
            }

            if (has0)
            {
                int16_t *orig = hw_node_bboxes[i][0];
                if (b0[BOXTOP] > orig[BOXTOP])       orig[BOXTOP] = b0[BOXTOP];
                if (b0[BOXBOTTOM] < orig[BOXBOTTOM]) orig[BOXBOTTOM] = b0[BOXBOTTOM];
                if (b0[BOXLEFT] < orig[BOXLEFT])     orig[BOXLEFT] = b0[BOXLEFT];
                if (b0[BOXRIGHT] > orig[BOXRIGHT])   orig[BOXRIGHT] = b0[BOXRIGHT];
            }
            if (has1)
            {
                int16_t *orig = hw_node_bboxes[i][1];
                if (b1[BOXTOP] > orig[BOXTOP])       orig[BOXTOP] = b1[BOXTOP];
                if (b1[BOXBOTTOM] < orig[BOXBOTTOM]) orig[BOXBOTTOM] = b1[BOXBOTTOM];
                if (b1[BOXLEFT] < orig[BOXLEFT])     orig[BOXLEFT] = b1[BOXLEFT];
                if (b1[BOXRIGHT] > orig[BOXRIGHT])   orig[BOXRIGHT] = b1[BOXRIGHT];
            }

            if (has0 && has1)
            {
                node_flat[i][BOXTOP]    = (b0[BOXTOP] > b1[BOXTOP]) ? b0[BOXTOP] : b1[BOXTOP];
                node_flat[i][BOXBOTTOM] = (b0[BOXBOTTOM] < b1[BOXBOTTOM]) ? b0[BOXBOTTOM] : b1[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = (b0[BOXLEFT] < b1[BOXLEFT]) ? b0[BOXLEFT] : b1[BOXLEFT];
                node_flat[i][BOXRIGHT]  = (b0[BOXRIGHT] > b1[BOXRIGHT]) ? b0[BOXRIGHT] : b1[BOXRIGHT];
                node_state[i] = 2;
            }
            else if (has0)
            {
                node_flat[i][BOXTOP]    = b0[BOXTOP];
                node_flat[i][BOXBOTTOM] = b0[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = b0[BOXLEFT];
                node_flat[i][BOXRIGHT]  = b0[BOXRIGHT];
                node_state[i] = 2;
            }
            else if (has1)
            {
                node_flat[i][BOXTOP]    = b1[BOXTOP];
                node_flat[i][BOXBOTTOM] = b1[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = b1[BOXLEFT];
                node_flat[i][BOXRIGHT]  = b1[BOXRIGHT];
                node_state[i] = 2;
            }
            else
            {
                node_state[i] = 1;
            }
        }
    }

    Z_Free(stack);
    Z_Free(node_state);
    Z_Free(node_flat);

#ifdef SATURN_WIN32
    printf("Saturn_ExpandNodeBBoxesWithFlats: expanded %d node bboxes with baked flat bounds (O(N) post-order)\n", numnodes);
    fflush(stdout);
#endif
}

/* Set on level init; the next frame bumps ipc->job_done so the slave purges its L1 */
static unsigned char hw_slave_level_dirty = 1;

#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
void Saturn_HW_RenderInit(void)
{
    Saturn_CmdBuf_Init();
    Saturn_Vdp1_ClearPages();

    int s;
    for (s = 0; s < numsectors; s++)
    {
        int f;
        if ((f = sectors[s].floorpic) >= 0 && f < numflats)
        {
            Saturn_FlatData(f);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && f >= a->basepic && f <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_FlatData(p);
                    }
                }
            }
        }
        if ((f = sectors[s].ceilingpic) >= 0 && f < numflats)
        {
            Saturn_FlatData(f);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && f >= a->basepic && f <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_FlatData(p);
                    }
                }
            }
        }
    }
    for (s = 0; s < numsides; s++)
    {
        int k, tex_arr[3];
        tex_arr[0] = sides[s].toptexture;
        tex_arr[1] = sides[s].midtexture;
        tex_arr[2] = sides[s].bottomtexture;
        for (k = 0; k < 3; k++)
        {
            int t = tex_arr[k];
            if (t > 0 && t < numtextures)
            {
                Saturn_TexData(t);
                if (anims && lastanim)
                {
                    const anim_t *a;
                    for (a = anims; a < lastanim; a++)
                    {
                        if (a->istexture && t >= a->basepic && t <= a->picnum)
                        {
                            int p;
                            for (p = a->basepic; p <= a->picnum; p++)
                                Saturn_TexData(p);
                        }
                    }
                }
                if (switchlist)
                {
                    int sw;
                    for (sw = 0; sw < MAXSWITCHES * 2 && switchlist[sw]; sw++)
                    {
                        if (switchlist[sw] == t)
                        {
                            int opp = switchlist[sw ^ 1];
                            if (opp > 0 && opp < numtextures)
                                Saturn_TexData(opp);
                        }
                    }
                }
            }
        }
    }
    /* Preload all sprites for things present in the level so rendering never hits CD */
    if (firstsprite > 0 && numsprites > 0)
    {
        uint8_t spr_used[NUMSPRITES];
        memset(spr_used, 0, sizeof(spr_used));

        /* Weapon sprites */
        spr_used[SPR_PUNG] = 1;
        spr_used[SPR_PISG] = 1;
        spr_used[SPR_PISF] = 1;
        spr_used[SPR_SHTG] = 1;
        spr_used[SPR_SHTF] = 1;

        for (s = 0; s < numsectors; s++)
        {
            mobj_t *mo = (mobj_t *)sectors[s].thinglist;
            while (mo)
            {
                if (mo->state >= 0 && mo->state < NUMSTATES)
                {
                    int sp = states[mo->state].sprite;
                    if (sp >= 0 && sp < NUMSPRITES)
                        spr_used[sp] = 1;
                }
                if (mo->type >= 0 && mo->type < NUMMOBJTYPES)
                {
                    const mobjinfo_t *mi = &mobjinfo[mo->type];
                    /* S_NULL (0) maps to SPR_TROO: skip it, otherwise every mobj with an
                     * unused state slot drags the full imp sprite set into hram. */
#define HW_PRELOAD_STATE(st) do { if ((st) > S_NULL && (st) < NUMSTATES) spr_used[states[(st)].sprite] = 1; } while (0)
                    HW_PRELOAD_STATE(mi->spawnstate);
                    HW_PRELOAD_STATE(mi->seestate);
                    HW_PRELOAD_STATE(mi->painstate);
                    HW_PRELOAD_STATE(mi->meleestate);
                    HW_PRELOAD_STATE(mi->missilestate);
                    HW_PRELOAD_STATE(mi->deathstate);
                    HW_PRELOAD_STATE(mi->xdeathstate);
#undef HW_PRELOAD_STATE
                }
                mo = (mobj_t *)mo->snext;
            }
        }

        for (s = 0; s < NUMSPRITES; s++)
        {
            if (!spr_used[s])
                continue;
            const spritedef_t *sprdef = &sprites[s];
            for (int f = 0; f < sprdef->numframes; f++)
            {
                spriteframe_t *sf = &spriteframes[sprdef->firstframe + f];
                int l_idx = sf->lump;
                int count = (spritelumps[l_idx] & SL_SINGLESIDED) ? 1 : 8;
                for (int r = 0; r < count; r++)
                {
                    int lump = (int)(spritelumps[l_idx + r] & SL_LUMPMASK);
                    if (lump >= firstsprite && lump < firstsprite + numsprites)
                    {
                        Saturn_SpriteData(lump - firstsprite);
                    }
                }
            }
        }
    }

    /* Preload SKY1 composite into fast WRAM-L (0x00220000) so background fill never hits CD or slow bus */
    int skytex = Saturn_SkyTexNum();
    if (skytex >= 0)
    {
        const uint8_t *sky = (const uint8_t *)Saturn_TexData(skytex);
        if (sky)
        {
            int w = Saturn_TexWidth(skytex);
            int h = Saturn_TexHeight(skytex);
            int sz = w * h;
            if (sz > 32768) sz = 32768;
            D_memcpy(HW_FAST_SKY_ADDR, sky, sz);
        }
    }
    hw_last_sky_angle = 0xFFFFFFFFu;
    hw_last_sky_cy    = -1;

    /* New level: reset VDP1 arena pages and preload all level flats (and animated frames) to both pages */
    Saturn_Vdp1_ClearPages();
    for (s = 0; s < numsectors; s++)
    {
        if (sectors[s].floorpic >= 0)
        {
            Saturn_Vdp1_PreloadFlat(sectors[s].floorpic);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && sectors[s].floorpic >= a->basepic && sectors[s].floorpic <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_Vdp1_PreloadFlat(p);
                    }
                }
            }
        }
        if (sectors[s].ceilingpic != -1)
        {
            Saturn_Vdp1_PreloadFlat(sectors[s].ceilingpic);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && sectors[s].ceilingpic >= a->basepic && sectors[s].ceilingpic <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_Vdp1_PreloadFlat(p);
                    }
                }
            }
        }
    }
    Saturn_Vdp1_LockPreloadedFlats();
    saturn_hw_sky_latch = 0;
    saturn_hw_sky_prime = 0;
    saturn_view_latch   = 2;
    if (I_VideoBuffer)
        D_memset(I_VideoBuffer, 0xF7, 320 * 168);

    if (!hw_vcache)
        hw_vcache = (saturn_vcache_t *)SATURN_WRAM_L_PILLARS_ADDR;
    memset(hw_vcache, 0, HW_VCACHE_SIZE * sizeof(saturn_vcache_t));
    hw_vcache_frame = 1;
    hw_slave_level_dirty = 1;
    HW_APERTURE->last_pvs_row = NULL;
    HW_APERTURE->last_pvs_numnodes = 0;

    /* Precompute 1-sided convex pillar corner floor clamps */
    saturn_vertex_floor_clamp = NULL;
    if (numvertexes > 0 && numlines > 0)
    {
        int v, i, j;
        saturn_vertex_floor_clamp = (int16_t *)Z_Malloc(numvertexes * sizeof(int16_t), PU_LEVEL);
        for (v = 0; v < numvertexes; v++)
            saturn_vertex_floor_clamp[v] = -32767;

        /* I_WorkBuffer() is 48 KB (d32xr_stubs.c scratch_buf) - not 64 KB. */
        int max_entries = ((48 * 1024) - (int)numvertexes * 2) / 4;
        if (max_entries > 0)
        {
            int16_t *v_head = (int16_t *)I_WorkBuffer();
            int16_t *v_next = v_head + numvertexes;
            int16_t *v_line = v_next + max_entries;
            int entry_count = 0;

            for (v = 0; v < numvertexes; v++)
                v_head[v] = -1;

            for (i = 0; i < numlines; i++)
            {
                line_t *l = &lines[i];
                if (l->sidenum[1] >= 0 || l->sidenum[0] < 0) continue;
                if (sides[l->sidenum[0]].midtexture <= 0) continue;

                if (entry_count + 2 <= max_entries)
                {
                    int v1 = l->v1;
                    int v2 = l->v2;
                    if ((unsigned)v1 < (unsigned)numvertexes)
                    {
                        v_line[entry_count] = (int16_t)i;
                        v_next[entry_count] = v_head[v1];
                        v_head[v1] = (int16_t)entry_count++;
                    }
                    if ((unsigned)v2 < (unsigned)numvertexes)
                    {
                        v_line[entry_count] = (int16_t)i;
                        v_next[entry_count] = v_head[v2];
                        v_head[v2] = (int16_t)entry_count++;
                    }
                }
            }

            for (v = 0; v < numvertexes; v++)
            {
                int e1;
                for (e1 = v_head[v]; e1 >= 0; e1 = v_next[e1])
                {
                    int i1 = v_line[e1];
                    line_t *l1 = &lines[i1];
                    int s1_mid = sides[l1->sidenum[0]].midtexture;
                    int16_t f1 = (int16_t)(sectors[sides[l1->sidenum[0]].sector].floorheight >> FRACBITS);
                    int other1 = (l1->v1 == v) ? l1->v2 : l1->v1;

                    int e2;
                    for (e2 = v_next[e1]; e2 >= 0; e2 = v_next[e2])
                    {
                        int i2 = v_line[e2];
                        line_t *l2 = &lines[i2];
                        if (sides[l2->sidenum[0]].midtexture != s1_mid) continue;
                        int16_t f2 = (int16_t)(sectors[sides[l2->sidenum[0]].sector].floorheight >> FRACBITS);
                        if (f1 == f2) continue;

                        int other2 = (l2->v1 == v) ? l2->v2 : l2->v1;
                        if ((unsigned)other1 < (unsigned)numvertexes && (unsigned)other2 < (unsigned)numvertexes)
                        {
                            int dx1 = (int)vertexes[other1].x - (int)vertexes[v].x;
                            int dy1 = (int)vertexes[other1].y - (int)vertexes[v].y;
                            int dx2 = (int)vertexes[other2].x - (int)vertexes[v].x;
                            int dy2 = (int)vertexes[other2].y - (int)vertexes[v].y;
                            int cross = HW_Cross32(dy2, dx1, dx2, dy1);
                            if (cross != 0)
                            {
                                int16_t max_f = (f1 > f2) ? f1 : f2;
                                if (max_f > saturn_vertex_floor_clamp[v])
                                    saturn_vertex_floor_clamp[v] = max_f;
                            }
                        }
                    }
                }
            }
        }
    }

    extern VINT gameepisode;
    extern VINT gamemap;

    /* Expand BSP node bboxes so subsector flats are never culled prematurely */
    Saturn_ExpandNodeBBoxesWithFlats();

    /* VDP2 RBG0 ground plane deactivated: large outdoor sectors render via Large LOD VDP1 quads */
    saturn_vdp2_plane_active = 0;
}

void Saturn_HW_ForceSkyRepaint(void)
{
    hw_last_sky_angle = 0xFFFFFFFFu;
    hw_last_sky_cy    = -1;
    hw_sky_dirty      = 1;
}

/* Background fill in I_VideoBuffer: Paints the sky texture across rows 0..95
 * when visible in skybox openings. VDP2 saturn_vdp2_copy_back transfers this
 * region to NBG0 bitmap plane, where it displays behind VDP1 3D walls/floors. */
static HW_HOT_O2 void HW_FillBackground(void)
{
    if (!saturn_hw_sky_visible && saturn_hw_sky_prime <= 0)
        return;

    int skytex = Saturn_SkyTexNum();
    if (skytex < 0)
        return;
    const uint8_t *sky = HW_FAST_SKY_ADDR;
    int w = Saturn_TexWidth(skytex);
    int h = Saturn_TexHeight(skytex);
    if (w <= 0 || h <= 0)
        return;
    /* Prevent OOB reads if custom sky exceeds 32 KB buffer */
    if (w * h > 32768)
    {
        h = 32768 / w;
        if (h <= 0)
            return;
    }

    /* When camera has not rotated or changed pitch/height, I_VideoBuffer rows 0..95
     * already contain the exact sky raster from the previous frame.
     * Note: saturn_vdp2_copy_back() in i_video.c owns decrementing saturn_hw_sky_latch
     * (2 -> 1 -> 0) across two frames so both double-buffered VDP2 NBG0 pages receive
     * the updated I_VideoBuffer sky. */
    if (!hw_sky_dirty && hw_viewangle == hw_last_sky_angle && HW_VIEW_CY == hw_last_sky_cy)
        return;

    hw_last_sky_angle   = hw_viewangle;
    hw_last_sky_cy      = HW_VIEW_CY;
    hw_sky_dirty        = 0;
    saturn_hw_sky_latch = 2;

    /* 1-time angle table initialization */
    static int hw_col_bam[HW_MAX_SCREEN_W];
    static int last_bam_w = 0;
    if (__builtin_expect(last_bam_w != saturn_view_w, 0))
    {
        int cx = saturn_view_w >> 1;
        int x;
        for (x = 0; x < saturn_view_w; x++)
        {
            int adx = x - cx;
            int aadx = adx < 0 ? -adx : adx;
            int lo = 0, hi = FINEANGLES / 4, ix;
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;
                if ((int32_t)finesine(mid) * cx >= (int32_t)finecosine(mid) * aadx)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            ix = lo;
            hw_col_bam[x] = (adx >= 0) ? (ix << ANGLETOFINESHIFT)
                                       : -(ix << ANGLETOFINESHIFT);
        }
        last_bam_w = saturn_view_w;
    }

    int16_t col_sky[HW_MAX_SCREEN_W];
    int x;
    if ((w & (w - 1)) == 0)
    {
        int wmask = w - 1;
        for (x = 0; x < saturn_view_w; x++)
            col_sky[x] = (int16_t)(((hw_viewangle - (unsigned)hw_col_bam[x]) >> 22) & wmask);
    }
    else
    {
        uint32_t uw = (uint32_t)w;
        for (x = 0; x < saturn_view_w; x++)
        {
            uint32_t val = (hw_viewangle - (unsigned)hw_col_bam[x]) >> 22;
            col_sky[x] = (int16_t)((val * uw) >> 10);
        }
    }

    int y;
    /* Vanilla placement: skytexturemid = 100, so texture row 100 sits on
     * the horizon (centre row) and the top of the view shows row 100 - CY.
     * Fill all the way down to HW_VIEW_H so low-ceiling outdoor sectors and
     * distant outdoor walls/floors that sit below the horizon don't leave black gaps. */
    int row_ofs = 100 - HW_VIEW_CY;
    int prev_texrow = -1;

    /* 16-byte aligned line buffer so 32-byte unrolled copies map cleanly to SH-2 16-byte L1 cache lines */
    HW_ALIGN16 uint32_t cached_sky_row[HW_MAX_SCREEN_W / 4];

    int max_sky_y = HW_VIEW_H;
    for (y = 0; y < max_sky_y; y++)
    {
        int texrow = y + row_ofs;
        if (texrow < 0) texrow = 0;
        if (texrow >= h) texrow = h - 1;
        uint8_t *dst = I_VideoBuffer + y * saturn_view_w;

        if (texrow == prev_texrow)
        {
            /* Fast 32-byte cache-line unrolled duplicate of identical clamped sky row from L1 CACHE */
            aliased_uint32_t *d32 = (aliased_uint32_t *)dst;
            const aliased_uint32_t *s32 = (const aliased_uint32_t *)cached_sky_row;
            int count = saturn_view_w >> 5;
            while (count--)
            {
                uint32_t r0 = *s32++, r1 = *s32++, r2 = *s32++, r3 = *s32++;
                *d32++ = r0; *d32++ = r1; *d32++ = r2; *d32++ = r3;

                uint32_t r4 = *s32++, r5 = *s32++, r6 = *s32++, r7 = *s32++;
                *d32++ = r4; *d32++ = r5; *d32++ = r6; *d32++ = r7;
            }
            continue;
        }
        prev_texrow = texrow;

        const uint8_t *src = sky + texrow * w;
        aliased_uint32_t *d32 = (aliased_uint32_t *)dst;
        const int16_t *csky = col_sky;
        int n_words = saturn_view_w >> 2;
        if (texrow == 0 || texrow == h - 1)
        {
            aliased_uint32_t *c32 = (aliased_uint32_t *)cached_sky_row;
            for (x = 0; x < n_words; x += 2, csky += 8)
            {
#ifdef SATURN_WIN32
                uint32_t r0 = ((uint32_t)src[csky[0]]) | ((uint32_t)src[csky[1]] << 8) |
                              ((uint32_t)src[csky[2]] << 16) | ((uint32_t)src[csky[3]] << 24);
                uint32_t r1 = ((uint32_t)src[csky[4]]) | ((uint32_t)src[csky[5]] << 8) |
                              ((uint32_t)src[csky[6]] << 16) | ((uint32_t)src[csky[7]] << 24);
#else
                uint32_t r0 = ((((uint32_t)src[csky[0]] << 8) | (uint32_t)src[csky[1]]) << 16) |
                              ((uint32_t)src[csky[2]] << 8) | (uint32_t)src[csky[3]];
                uint32_t r1 = ((((uint32_t)src[csky[4]] << 8) | (uint32_t)src[csky[5]]) << 16) |
                              ((uint32_t)src[csky[6]] << 8) | (uint32_t)src[csky[7]];
#endif
                d32[x + 0] = r0; d32[x + 1] = r1;
                c32[x + 0] = r0; c32[x + 1] = r1;
            }
        }
        else
        {
            for (x = 0; x < n_words; x += 2, csky += 8)
            {
#ifdef SATURN_WIN32
                uint32_t r0 = ((uint32_t)src[csky[0]]) | ((uint32_t)src[csky[1]] << 8) |
                              ((uint32_t)src[csky[2]] << 16) | ((uint32_t)src[csky[3]] << 24);
                uint32_t r1 = ((uint32_t)src[csky[4]]) | ((uint32_t)src[csky[5]] << 8) |
                              ((uint32_t)src[csky[6]] << 16) | ((uint32_t)src[csky[7]] << 24);
#else
                uint32_t r0 = ((((uint32_t)src[csky[0]] << 8) | (uint32_t)src[csky[1]]) << 16) |
                              ((uint32_t)src[csky[2]] << 8) | (uint32_t)src[csky[3]];
                uint32_t r1 = ((((uint32_t)src[csky[4]] << 8) | (uint32_t)src[csky[5]]) << 16) |
                              ((uint32_t)src[csky[6]] << 8) | (uint32_t)src[csky[7]];
#endif
                d32[x + 0] = r0; d32[x + 1] = r1;
            }
        }
    }
}

/* Queue player weapon and muzzle flash: weapon first (psprites[0]), muzzle flash second (psprites[1]),
 * ensuring muzzle flash renders on top in VDP1 Painter's algorithm order */
#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
static void HW_RenderPlayerWeapon(void)
{
    player_t *p = &players[hw_displayplayer];
    int base = Saturn_Vdp1Wall_Base();
    int ok = 1;
    int i;

    if (!spriteframes || !sprites || !p || !p->mo || !p->mo->subsector)
        return;

    for (i = 0; ok && i < NUMPSPRITES; i++)
    {
        pspdef_t *psp = &p->psprites[i];
        const state_t *st;
        const spritedef_t *sprdef;
        spriteframe_t *sf;
        unsigned lumpv;
        int lump, scache, w, h, lofs, tofs;
        int x1, y0, r0, r1, sh, pw, xA, xB;
        int secnum;

        if (!psp->state || (unsigned)psp->state >= NUMSTATES)
            continue;
        st = &states[psp->state];
        if (st->sprite < 0 || st->sprite >= NUMSPRITES)
            continue;
        sprdef = &sprites[st->sprite];
        if (sprdef->numframes <= 0)
            continue;
        if ((st->frame & FF_FRAMEMASK) >= sprdef->numframes)
            continue;
        sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];
        lumpv = (unsigned short)spritelumps[sf->lump];
        lump = (int)(lumpv & SL_LUMPMASK);
        if (lump < firstsprite || lump >= firstsprite + numsprites)
            continue;
        scache = lump - firstsprite;
        if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
            continue;
        if (w < 1 || h < 1)
            continue;
        x1 = (psp->sx >> FRACBITS) - lofs + ((saturn_view_w - 320) >> 1);
        y0 = 2 + (psp->sy >> FRACBITS) - tofs;
        r0 = 0;
        r1 = h - 1;
        if (y0 + r0 < 0) r0 = 0 - y0;
        if (y0 + r1 >= HW_VIEW_H) r1 = HW_VIEW_H - 1 - y0;
        if (r0 > r1)
            continue;
        secnum = (p->mo && p->mo->subsector) ? p->mo->subsector->sector : -1;
        sh = (secnum >= 0 && secnum < numsectors)
           ? HW_FastLightRow(sectors[secnum].lightlevel, 0) : 0;
        if (st->frame & FF_FULLBRIGHT)
            sh = 0;
        pw = (w + 7) & ~7;
        xA = x1;
        xB = x1 + pw - 1;
        if (xA < -1024 || xA > 1023 || xB < -1024 || xB > 1023
            || y0 + r0 < -1024 || y0 + r0 > 1023
            || y0 + r1 < -1024 || y0 + r1 > 1023)
        {
            ok = 0;
            break;
        }
        if (!Saturn_Vdp1Spr_Add(scache, xA, xB,
                                y0 + r0, y0 + r1,
                                r0, r1 - r0 + 1, sh, 1))
        {
            ok = 0;
            break;
        }
    }
    if (!ok)
        Saturn_Vdp1Wall_Restore(base);
}

/* Process a single seg in the current subsector (fallback when baked walls are inactive) */
#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
static void HW_RenderSeg(seg_t *seg, sector_t *frontsec, const hw_shade_ctx_t *shade_ctx)
{
    int v1_idx, v2_idx;
    mapvertex_t *v1, *v2;
    fixed_t cz1, cx1, cz2, cx2;
    int s1, s2, xA, xB;
    int line_idx, side_idx;
    register line_t *ldef;
    register side_t *sidedef;
    register sector_t *backsec;
    const fixed_t vz   = hw_viewz;
    saturn_hw_seg_count++;

    v1_idx = SEG_UNPACK_V1(seg);
    v2_idx = SEG_UNPACK_V2(seg);
    if ((unsigned)v1_idx >= (unsigned)numvertexes || (unsigned)v2_idx >= (unsigned)numvertexes || v1_idx == v2_idx)
        return;
    v1 = &vertexes[v1_idx];
    v2 = &vertexes[v2_idx];

    /* 2D Map-space Backface Rejection:
     * A seg from v1 to v2 is only visible if the viewer is to the right of the directed line.
     * Evaluates (cam_x - v1.x)*dy - (cam_y - v1.y)*dx. If <= 0, the seg faces away from the camera.
     * Rejects ~42% of segs in ~8 cycles before vertex transformations, divisions, or cache lookups! */
    {
        int dx = (int)v2->x - (int)v1->x;
        int dy = (int)v2->y - (int)v1->y;
        int cx = hw_cam_x - (int)v1->x;
        int cy = hw_cam_y - (int)v1->y;

        if (HW_Cross32(cx, dy, cy, dx) <= 0)
            return;
    }

    /* Cached or transformed vertex lookups */
    const saturn_vcache_t *vc1 = HW_GetVertex(v1_idx, v1);
    const saturn_vcache_t *vc2 = HW_GetVertex(v2_idx, v2);

    cx1 = vc1->cx; cz1 = vc1->cz; s1 = vc1->s; xA = vc1->px;
    cx2 = vc2->cx; cz2 = vc2->cz; s2 = vc2->s; xB = vc2->px;

    /* Fast path: if both vertices in front of near plane (98%+ of segs), test cached screen projection directly */
    if (cz1 >= HW_NEAR_Z && cz2 >= HW_NEAR_Z)
    {
        if (xB <= 0 || xA >= saturn_view_w || xB <= xA)
            return;
    }
    else
    {
        /* If both behind near plane, discard */
        if (cz1 < HW_NEAR_Z && cz2 < HW_NEAR_Z)
            return;

        /* Near-plane clipping against cz >= HW_NEAR_Z */
        if (cz1 < HW_NEAR_Z)
        {
            fixed_t t = HW_ClampedFrac((unsigned int)(HW_NEAR_Z - cz1), (unsigned int)(cz2 - cz1));
            if (t > FRACUNIT) t = FRACUNIT;

            cx1 = (int32_t)((uint32_t)cx1 + ((uint32_t)FixedMul(t, (cx2 >> 1) - (cx1 >> 1)) << 1));
            cz1 = HW_NEAR_Z;
            s1  = HW_NEAR_Z_SCALE;
            xA  = HW_ProjX(cx1, s1, HW_VIEW_CX);
        }
        else
        {
            fixed_t t = HW_ClampedFrac((unsigned int)(HW_NEAR_Z - cz2), (unsigned int)(cz1 - cz2));
            if (t > FRACUNIT) t = FRACUNIT;

            cx2 = (int32_t)((uint32_t)cx2 + ((uint32_t)FixedMul(t, (cx1 >> 1) - (cx2 >> 1)) << 1));
            cz2 = HW_NEAR_Z;
            s2  = HW_NEAR_Z_SCALE;
            xB  = HW_ProjX(cx2, s2, HW_VIEW_CX);
        }

        /* Screen projection & back-face check after clipping */
        if (xB <= 0 || xA >= saturn_view_w || xB <= xA)
            return;
    }

    int seg_sx = (xA < 0) ? 0 : xA;
    int seg_ex = (xB > saturn_view_w) ? saturn_view_w : xB;
    if (seg_ex <= seg_sx)
        return;

    /* 1D Solid Wall Occlusion Check: if all columns in [seg_sx, seg_ex) are already covered by solid walls, skip */
    if (HW_IsSpanOccluded(seg_sx, seg_ex))
        return;

    if (__builtin_expect(saturn_cmd_m_count + 6 >= CMDBUF_M_CAP, 0))
        return;

    /* Linedef and sidedef lookup ONLY after passing all visibility tests */
    line_idx = SEG_UNPACK_LINEDEF(seg);
    side_idx = SEG_UNPACK_SIDE(seg);
    if ((unsigned)line_idx >= (unsigned)numlines || (unsigned)side_idx > 1)
        return;
    ldef = &lines[line_idx];

    int sidenum = ldef->sidenum[side_idx];
    if ((unsigned)sidenum >= (unsigned)numsides)
        return;

    int seg_cache_hit = (line_idx == hw_seg_last_line_idx && side_idx == hw_seg_last_side_idx);
    if (__builtin_expect(seg_cache_hit, 1))
    {
        sidedef = hw_seg_last_sidedef;
        backsec = hw_seg_last_backsec;
    }
    else
    {
        sidedef = &sides[sidenum];
        int other_sidenum = ldef->sidenum[side_idx ^ 1];
        if ((unsigned)other_sidenum < (unsigned)numsides)
            backsec = &sectors[sides[other_sidenum].sector];
        else
            backsec = NULL;
    }

    /* Skip invisible intra-sector linedefs */
    if (backsec == frontsec && sidedef->midtexture == 0)
        return;

    /* Fast early-out for 2-sided portal linedefs with no vertical step (no wall surfaces to draw) */
    if (backsec != NULL)
    {
        if (sidedef->midtexture == 0 &&
            backsec->ceilingheight >= frontsec->ceilingheight &&
            backsec->floorheight <= frontsec->floorheight)
        {
            return;
        }
    }
    int mid_srca, mid_tw, mid_rows;
    int top_srca, top_tw, top_rows;
    int bot_srca, bot_tw, bot_rows;
    int mmid_srca, mmid_tw, mmid_th;

    if (__builtin_expect(seg_cache_hit, 1))
    {
        mid_srca  = hw_seg_last_mid_srca;  mid_tw  = hw_seg_last_mid_tw;  mid_rows  = hw_seg_last_mid_rows;
        top_srca  = hw_seg_last_top_srca;  top_tw  = hw_seg_last_top_tw;  top_rows  = hw_seg_last_top_rows;
        bot_srca  = hw_seg_last_bot_srca;  bot_tw  = hw_seg_last_bot_tw;  bot_rows  = hw_seg_last_bot_rows;
        mmid_srca = hw_seg_last_mmid_srca; mmid_tw = hw_seg_last_mmid_tw; mmid_th   = hw_seg_last_mmid_th;

        if (!backsec)
        {
            if (!mid_srca)
                return;
        }
        else
        {
            if (!top_srca && !bot_srca && !mmid_srca)
                return;
        }
    }
    else
    {
        hw_seg_last_line_idx = line_idx;
        hw_seg_last_side_idx = side_idx;
        hw_seg_last_sidedef  = sidedef;
        hw_seg_last_backsec  = backsec;
        mid_srca = mid_tw = mid_rows = 0;
        top_srca = top_tw = top_rows = 0;
        bot_srca = bot_tw = bot_rows = 0;
        mmid_srca = mmid_tw = mmid_th = 0;

        if (!backsec)
        {
            int texnum = sidedef->midtexture;
            if (texnum > 0)
            {
                int th = 0;
                mid_srca = Saturn_Vdp1_GetTexSrca(texnum, &mid_tw, &th);
                if (mid_srca > 0)
                {
                    int wall_h = (frontsec->ceilingheight - frontsec->floorheight) >> FRACBITS;
                    mid_rows = (wall_h < th && wall_h > 0) ? wall_h : th;
                    if (mid_rows < 1) mid_rows = 1;
                    if (mid_rows > 255) mid_rows = 255;
                }
            }
            hw_seg_last_mid_srca = mid_srca;
            hw_seg_last_mid_tw   = mid_tw;
            hw_seg_last_mid_rows = mid_rows;
            if (!mid_srca)
                return;
        }
        else
        {
            if (backsec->ceilingheight < frontsec->ceilingheight &&
                ((int16_t)backsec->ceilingpic != -1 || (int16_t)frontsec->ceilingpic != -1))
            {
                int texnum = sidedef->toptexture;
                if (texnum > 0)
                {
                    int th = 0;
                    top_srca = Saturn_Vdp1_GetTexSrca(texnum, &top_tw, &th);
                    if (top_srca > 0)
                    {
                        int wall_h = (frontsec->ceilingheight - backsec->ceilingheight) >> FRACBITS;
                        top_rows = (wall_h < th && wall_h > 0) ? wall_h : th;
                        if (top_rows < 1) top_rows = 1;
                        if (top_rows > 255) top_rows = 255;
                    }
                }
            }

            if (backsec->floorheight > frontsec->floorheight)
            {
                int texnum = sidedef->bottomtexture;
                if (texnum > 0)
                {
                    int th = 0;
                    bot_srca = Saturn_Vdp1_GetTexSrca(texnum, &bot_tw, &th);
                    if (bot_srca > 0)
                    {
                        int wall_h = (backsec->floorheight - frontsec->floorheight) >> FRACBITS;
                        bot_rows = (wall_h < th && wall_h > 0) ? wall_h : th;
                        if (bot_rows < 1) bot_rows = 1;
                        if (bot_rows > 255) bot_rows = 255;
                    }
                }
            }

            if (sidedef->midtexture != 0)
            {
                fixed_t mbot = (frontsec->floorheight > backsec->floorheight)
                             ? frontsec->floorheight : backsec->floorheight;
                fixed_t mtop = (frontsec->ceilingheight < backsec->ceilingheight)
                             ? frontsec->ceilingheight : backsec->ceilingheight;
                if (mtop > mbot)
                {
                    mmid_srca = Saturn_Vdp1_GetTexSrca(sidedef->midtexture, &mmid_tw, &mmid_th);
                    if (mmid_th > 255) mmid_th = 255; /* size word packs rows & 0xFF */
                }
            }

            hw_seg_last_top_srca  = top_srca;
            hw_seg_last_top_tw    = top_tw;
            hw_seg_last_top_rows  = top_rows;
            hw_seg_last_bot_srca  = bot_srca;
            hw_seg_last_bot_tw    = bot_tw;
            hw_seg_last_bot_rows  = bot_rows;
            hw_seg_last_mmid_srca = mmid_srca;
            hw_seg_last_mmid_tw   = mmid_tw;
            hw_seg_last_mmid_th   = mmid_th;

            if (!top_srca && !bot_srca && !mmid_srca)
                return;
        }
    }

    {
        fixed_t f1 = frontsec->floorheight;
        fixed_t f2 = frontsec->floorheight;
        if (!backsec && saturn_vertex_floor_clamp)
        {
            if (saturn_vertex_floor_clamp[v1_idx] > (f1 >> FRACBITS))
                f1 = (fixed_t)saturn_vertex_floor_clamp[v1_idx] << FRACBITS;
            if (saturn_vertex_floor_clamp[v2_idx] > (f2 >> FRACBITS))
                f2 = (fixed_t)saturn_vertex_floor_clamp[v2_idx] << FRACBITS;
        }

        /* Shared height projection (floor/wall seam fix): 16.16 precision via HW_DZQ & HW_MulHighPair */
        int dz_fceil    = HW_DZQ(frontsec->ceilingheight, vz);
        int dz_ffloor_a = HW_DZQ(f1, vz);
        int dz_ffloor_b = HW_DZQ(f2, vz);
        int dz_bceil    = (top_srca > 0) ? HW_DZQ(backsec->ceilingheight, vz) : 0;
        int dz_bfloor   = (bot_srca > 0) ? HW_DZQ(backsec->floorheight, vz) : 0;

        int dist    = (cz1 + cz2) >> (FRACBITS + 1);
        int k_shade = HW_FastShadeCtx(shade_ctx, dist);

        int ya_fceil, yb_fceil;
        HW_ProjYPair(HW_VIEW_CY, dz_fceil, s1, dz_fceil, s2, &ya_fceil, &yb_fceil);

        int ya_ffloor, yb_ffloor;
        HW_ProjYPair(HW_VIEW_CY, dz_ffloor_a, s1, dz_ffloor_b, s2, &ya_ffloor, &yb_ffloor);

        int ya_bceil  = 0, yb_bceil = 0, ya_bfloor = 0, yb_bfloor = 0;
        if (top_srca > 0)
        {
            HW_ProjYPair(HW_VIEW_CY, dz_bceil, s1, dz_bceil, s2, &ya_bceil, &yb_bceil);
        }
        if (bot_srca > 0)
        {
            HW_ProjYPair(HW_VIEW_CY, dz_bfloor, s1, dz_bfloor, s2, &ya_bfloor, &yb_bfloor);
        }

        int xa = xA;
        int xb = xB;

        /* 2D screen-space clamping to [-400, 400]: only if wall actually exceeds clamp */
        if (__builtin_expect(xa < -400 || xb > 400, 0))
        {
            int orig_xa = xa;
            int orig_xb = xb;
            int step_dx = orig_xb - orig_xa;
            if (step_dx > 0)
            {
                int orig_ya_fceil  = ya_fceil;
                int orig_yb_fceil  = yb_fceil;
                int orig_ya_ffloor = ya_ffloor;
                int orig_yb_ffloor = yb_ffloor;
                int orig_ya_bceil  = ya_bceil;
                int orig_yb_bceil  = yb_bceil;
                int orig_ya_bfloor = ya_bfloor;
                int orig_yb_bfloor = yb_bfloor;

                if (xa < -400)
                {
                    int t = -400 - orig_xa;
                    int frac_a = HW_ClampedFrac((unsigned int)t, (unsigned int)step_dx);
                    ya_fceil  = orig_ya_fceil  + FixedMul(orig_yb_fceil  - orig_ya_fceil,  frac_a);
                    ya_ffloor = orig_ya_ffloor + FixedMul(orig_yb_ffloor - orig_ya_ffloor, frac_a);
                    if (top_srca > 0)
                        ya_bceil  = orig_ya_bceil  + FixedMul(orig_yb_bceil  - orig_ya_bceil,  frac_a);
                    if (bot_srca > 0)
                        ya_bfloor = orig_ya_bfloor + FixedMul(orig_yb_bfloor - orig_ya_bfloor, frac_a);
                    xa = -400;
                }
                if (xb > 400)
                {
                    int t = 400 - orig_xa;
                    int frac_b = HW_ClampedFrac((unsigned int)t, (unsigned int)step_dx);
                    yb_fceil  = orig_ya_fceil  + FixedMul(orig_yb_fceil  - orig_ya_fceil,  frac_b);
                    yb_ffloor = orig_ya_ffloor + FixedMul(orig_yb_ffloor - orig_ya_ffloor, frac_b);
                    if (top_srca > 0)
                        yb_bceil  = orig_ya_bceil  + FixedMul(orig_yb_bceil  - orig_ya_bceil,  frac_b);
                    if (bot_srca > 0)
                        yb_bfloor = orig_ya_bfloor + FixedMul(orig_yb_bfloor - orig_ya_bfloor, frac_b);
                    xb = 400;
                }
            }
        }

        /* Wall surfaces */
        if (!backsec)
        {
            /* Solid one-sided wall */
            int wya0 = ya_fceil - 1;
            int wyb0 = yb_fceil - 1;
            int wya1 = ya_ffloor + 1;
            int wyb1 = yb_ffloor + 1;

            if (wya1 > 0 || wyb1 > 0)
            {
                if (wya0 < HW_VIEW_H || wyb0 < HW_VIEW_H)
                {
                    if (__builtin_expect(HW_ANY_OUT1024(wya0, wyb0, wya1, wyb1), 0))
                    {
                        wya0 = HW_ClampY(wya0);
                        wyb0 = HW_ClampY(wyb0);
                        wya1 = HW_ClampY(wya1);
                        wyb1 = HW_ClampY(wyb1);
                    }
                    if (wya0 < wya1 || wyb0 < wyb1)
                    {
                        HW_EmitWallQuadDirect(xa, wya0, wya1,
                                              xb, wyb0, wyb1,
                                              mid_srca, mid_tw, mid_rows, k_shade,
                                              0);

                        HW_MarkSubSolidSpan(xa, xb);
                    }
                }
            }
        }
        else
        {
            /* Two-sided portal line */
            if (top_srca > 0)
            {
                int min_yc = (ya_fceil < ya_bceil) ? ya_fceil : ya_bceil;
                int max_yc = (ya_fceil > ya_bceil) ? ya_fceil : ya_bceil;
                int min_yd = (yb_fceil < yb_bceil) ? yb_fceil : yb_bceil;
                int max_yd = (yb_fceil > yb_bceil) ? yb_fceil : yb_bceil;
                int wya0 = min_yc - 1;
                int wyb0 = min_yd - 1;
                int wya1 = max_yc + 1;
                int wyb1 = max_yd + 1;

                if (wya1 > 0 || wyb1 > 0)
                {
                    if (wya0 < HW_VIEW_H || wyb0 < HW_VIEW_H)
                    {
                        if (__builtin_expect(HW_ANY_OUT1024(wya0, wyb0, wya1, wyb1), 0))
                        {
                            wya0 = HW_ClampY(wya0);
                            wyb0 = HW_ClampY(wyb0);
                            wya1 = HW_ClampY(wya1);
                            wyb1 = HW_ClampY(wyb1);
                        }
                        if (wya0 < wya1 || wyb0 < wyb1)
                        {
                            HW_EmitWallQuadDirect(xa, wya0, wya1,
                                                  xb, wyb0, wyb1,
                                                  top_srca, top_tw, top_rows, k_shade,
                                                  0);
                        }
                    }
                }
            }

            if (bot_srca > 0)
            {
                int min_yf = (ya_ffloor < ya_bfloor) ? ya_ffloor : ya_bfloor;
                int max_yf = (ya_ffloor > ya_bfloor) ? ya_ffloor : ya_bfloor;
                int min_ye = (yb_ffloor < yb_bfloor) ? yb_ffloor : yb_bfloor;
                int max_ye = (yb_ffloor > yb_bfloor) ? yb_ffloor : yb_bfloor;
                int wya0 = min_yf - 1;
                int wyb0 = min_ye - 1;
                int wya1 = max_yf + 1;
                int wyb1 = max_ye + 1;

                if (wya1 > 0 || wyb1 > 0)
                {
                    if (wya0 < HW_VIEW_H || wyb0 < HW_VIEW_H)
                    {
                        if (__builtin_expect(HW_ANY_OUT1024(wya0, wyb0, wya1, wyb1), 0))
                        {
                            wya0 = HW_ClampY(wya0);
                            wyb0 = HW_ClampY(wyb0);
                            wya1 = HW_ClampY(wya1);
                            wyb1 = HW_ClampY(wyb1);
                        }
                        if (wya0 < wya1 || wyb0 < wyb1)
                        {
                            HW_EmitWallQuadDirect(xa, wya0, wya1,
                                                  xb, wyb0, wyb1,
                                                  bot_srca, bot_tw, bot_rows, k_shade,
                                                  0);
                        }
                    }
                }
            }

            if (mmid_srca > 0)
            {
                fixed_t mbot = (frontsec->floorheight > backsec->floorheight)
                             ? frontsec->floorheight : backsec->floorheight;
                fixed_t mtop = (frontsec->ceilingheight < backsec->ceilingheight)
                             ? frontsec->ceilingheight : backsec->ceilingheight;
                int dz_mtop = HW_DZQ(mtop, vz);
                int dz_mbot = HW_DZQ(mbot, vz);
                int mya0, myb0, mya1, myb1;
                HW_ProjYPair(HW_VIEW_CY, dz_mtop, s1, dz_mtop, s2, &mya0, &myb0);
                HW_ProjYPair(HW_VIEW_CY, dz_mbot, s1, dz_mbot, s2, &mya1, &myb1);

                if (mya1 > 0 || myb1 > 0)
                {
                    if (mya0 < HW_VIEW_H || myb0 < HW_VIEW_H)
                    {
                        if (__builtin_expect(HW_ANY_OUT1024(mya0, myb0, mya1, myb1), 0))
                        {
                            mya0 = HW_ClampY(mya0);
                            myb0 = HW_ClampY(myb0);
                            mya1 = HW_ClampY(mya1);
                            myb1 = HW_ClampY(myb1);
                        }
                        if (mya0 < mya1 || myb0 < myb1)
                        {
                            HW_EmitWallQuadDirect(xa, mya0, mya1,
                                                  xb, myb0, myb1,
                                                  mmid_srca, mmid_tw, mmid_th, k_shade,
                                                  1);
                        }
                    }
                }
            }

            /* Closed portal occlusion (closed door, raised lift, lowered ceiling):
             * if the vertical opening between front and back sector is <= 0,
             * it is 100% solid wall from floor to ceiling. Mark the span solid! */
            fixed_t mbot = (frontsec->floorheight > backsec->floorheight) ? frontsec->floorheight : backsec->floorheight;
            fixed_t mtop = (frontsec->ceilingheight < backsec->ceilingheight) ? frontsec->ceilingheight : backsec->ceilingheight;
            if ((int16_t)frontsec->ceilingpic != -1 && (int16_t)backsec->ceilingpic != -1 && mtop <= mbot)
            {
                HW_MarkSubSolidSpan(xa, xb);
            }
        }
    }
}

typedef struct {
    mobj_t  *mo;
    fixed_t  cz;
    fixed_t  cx;
    uint8_t  next;
    uint8_t  pad[3];
} hw_thing_entry_t; /* 16 bytes (shift-indexed by << 4) */

#define HW_THING_POOL_CAP 128
static uint32_t         hw_sec_things_built[16]; /* Up to 512 sectors */
static uint32_t         hw_sub_has_things[32];   /* Up to 1024 subsectors */
static uint8_t          hw_sub_first_thing[1024];
static hw_thing_entry_t hw_thing_pool[HW_THING_POOL_CAP];
static int              hw_thing_pool_count = 0;

/* Walk sec->thinglist ONCE per sector per frame, frustum-cull in camera space,
 * and bucket surviving visible things by subsector index. */
static HW_NOINLINE HW_HOT_O2 void HW_BuildSectorThings(sector_t *sec)
{
    mobj_t *mo = (mobj_t *)sec->thinglist;
    const mobj_t *player_mo = players[hw_displayplayer].mo;
    const fixed_t vx   = hw_viewx;
    const fixed_t vy   = hw_viewy;
    const fixed_t vcos = hw_viewcos;
    const fixed_t vsin = hw_viewsin;

    while (mo)
    {
        const subsector_t *msub_ptr = mo->subsector;
        if (mo != player_mo && msub_ptr &&
            !(mo->flags & MF_NOSECTOR) && mo->state && (unsigned)mo->state < NUMSTATES)
        {
            int msub = (int)(msub_ptr - subsectors);
            if ((unsigned)msub < (unsigned)numsubsectors && (unsigned)msub < 1024u)
            {
                fixed_t dx = mo->x - vx;
                fixed_t dy = mo->y - vy;
                fixed_t cz1, cz2;
                HW_FixedMulPair(dx, vcos, dy, vsin, &cz1, &cz2);
                fixed_t cz = cz1 + cz2;

                if (cz >= (12 << FRACBITS) && cz <= HW_FAR_Z)
                {
                    fixed_t cx1, cx2;
                    HW_FixedMulPair(dx, vsin, dy, vcos, &cx1, &cx2);
                    fixed_t cx = cx1 - cx2;
                    fixed_t max_cx = cz + (cz >> 2) + (128 << FRACBITS);

                    if (cx >= -max_cx && cx <= max_cx && hw_thing_pool_count < HW_THING_POOL_CAP)
                    {
                        uint32_t sbit = hw_bit_lut[msub & 31];
                        uint8_t next_idx = 0xFFu;
                        if (hw_sub_has_things[msub >> 5] & sbit)
                            next_idx = hw_sub_first_thing[msub];
                        else
                            hw_sub_has_things[msub >> 5] |= sbit;

                        int idx = hw_thing_pool_count++;
                        hw_thing_pool[idx].mo   = mo;
                        hw_thing_pool[idx].cz   = cz;
                        hw_thing_pool[idx].cx   = cx;
                        hw_thing_pool[idx].next = next_idx;
                        hw_sub_first_thing[msub] = (uint8_t)idx;
                    }
                }
            }
        }
        mo = (mobj_t *)mo->snext;
    }
}

/* Render pre-bucketed visible mobjs (monsters, items, pickups) in subsector */
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsectorSprites(sector_t *sec, int subnum)
{
    uint8_t thing_list[64];
    int n_things = 0;
    for (uint8_t tidx = hw_sub_first_thing[subnum]; tidx != 0xFFu && n_things < 64; tidx = hw_thing_pool[tidx].next)
        thing_list[n_things++] = tidx;
    if (n_things <= 0)
        return;

    typedef struct {
        fixed_t cz;
        int16_t scache;
        int16_t xA, xB, yt, yb;
        int16_t h;
        int16_t shade;
        int16_t pad;
    } hw_sec_spr_t;

    static hw_sec_spr_t spr_queue[64];
    int          spr_count = 0;
    int          i;
    const fixed_t vx = hw_viewx;
    const fixed_t vy = hw_viewy;
    const fixed_t vz = hw_viewz;

    for (int ti = n_things - 1; ti >= 0; ti--)
    {
        const hw_thing_entry_t *te = &hw_thing_pool[thing_list[ti]];
        const mobj_t *mo = te->mo;
        fixed_t cz = te->cz;
        fixed_t cx = te->cx;

        const state_t *st = &states[mo->state];
        if (st->sprite >= 0 && st->sprite < NUMSPRITES)
        {
            const spritedef_t *sprdef = &sprites[st->sprite];
            if (sprdef->numframes > 0 && (st->frame & FF_FRAMEMASK) < sprdef->numframes)
            {
                spriteframe_t *sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];
                const VINT *sl = &spritelumps[sf->lump];
                unsigned short lumpv = (unsigned short)sl[0];
                int flip = 0;

                if (!(lumpv & SL_SINGLESIDED))
                {
                    angle_t ang = R_PointToAngle(vx, vy, mo->x, mo->y);
                    int rot = (int)(((ang - mo->angle + (unsigned)(ANG45 / 2) * 9) >> 29) & 7);
                    lumpv = (unsigned short)sl[rot];
                }
                flip = (lumpv & SL_FLIPPED) ? 1 : 0;
                int lump = (int)(lumpv & SL_LUMPMASK);

                if (lump >= firstsprite && lump < firstsprite + numsprites)
                {
                    int scache = lump - firstsprite;
                    int w, h, lofs, tofs;

                    if (Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs) && w >= 1 && h >= 1)
                    {
                        int s, cx_screen, xl, x2t, pw, wpx, xA, xB, yt, yb;
                        fixed_t y_world;
                        int shade;

                        s = HW_FastDiv(cz);
                        cx_screen = HW_ProjX(cx, s, HW_VIEW_CX);
                        pw  = (w + 7) & ~7;
                        wpx = (pw * s) >> FRACBITS;
                        xl  = cx_screen - ((lofs * s) >> FRACBITS);
                        x2t = xl + ((w * s) >> FRACBITS) - 1;
                        xA  = flip ? x2t : xl;
                        xB  = flip ? xA - wpx + 1 : xl + wpx - 1;

                        y_world = mo->z + ((fixed_t)tofs << FRACBITS) - vz;
                        yt = HW_ProjY(y_world, s, HW_VIEW_CY);
                        yb = yt + ((h * s) >> FRACBITS);

                        if (xB - xA > 1023)
                        {
                            if (xA < -(1023 - saturn_view_w)) xA = xB - 1023;
                            else xB = xA + 1023;
                        }
                        else if (xA - xB > 1023)
                        {
                            if (xB < -(1023 - saturn_view_w)) xB = xA - 1023;
                            else xA = xB + 1023;
                        }
                        if (yb - yt > 1023)
                        {
                            if (yt < -(1023 - HW_VIEW_H)) yt = yb - 1023;
                            else yb = yt + 1023;
                        }

                        if (xA < -1024) xA = -1024; else if (xA > 1023) xA = 1023;
                        if (xB < -1024) xB = -1024; else if (xB > 1023) xB = 1023;
                        if (yt < -1024) yt = -1024; else if (yt > 1023) yt = 1023;
                        if (yb < -1024) yb = -1024; else if (yb > 1023) yb = 1023;

                        int diff = xA - xB;
                        int sign = diff >> 31;
                        int x_min = xB + (diff & sign);
                        int x_max = xA - (diff & sign);

                        if (x_max > 0 && x_min < saturn_view_w && x_min != x_max &&
                            yb > 0 && yt < HW_VIEW_H && yt < yb)
                        {
                            int cull_x0 = x_min < 0 ? 0 : x_min;
                            int cull_x1 = x_max > saturn_view_w ? saturn_view_w : x_max;
                            if (hw_solid_count == 0 || !HW_IsSpanOccluded(cull_x0, cull_x1))
                            {
                                shade = (st->frame & FF_FULLBRIGHT)
                                      ? 0 : HW_FastLightRow(sec->lightlevel, cz >> FRACBITS);
                                if (spr_count < 64)
                                {
                                    spr_queue[spr_count].cz     = cz;
                                    spr_queue[spr_count].scache = (int16_t)scache;
                                    spr_queue[spr_count].xA     = (int16_t)xA;
                                    spr_queue[spr_count].xB     = (int16_t)xB;
                                    spr_queue[spr_count].yt     = (int16_t)yt;
                                    spr_queue[spr_count].yb     = (int16_t)yb;
                                    spr_queue[spr_count].h      = (int16_t)h;
                                    spr_queue[spr_count].shade  = (int16_t)shade;
                                    spr_count++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Depth sort buffered sector sprites descending by cz (far first, near last).
     * NOTE ON TOPOLOGICAL DELIVERY: While Saturn_CmdBuf_Deliver() reverses the order
     * of SUBSECTORS (back-to-front), commands within each subsector are copied linearly
     * forward into VRAM (walls first, then sprites) and executed forward by VDP1.
     * Therefore, within a single subsector, the farthest sprite must be queued first
     * (at index 0) so it is drawn underneath nearer sprites in the same subsector.
     * Uses a 4-gap Shell Sort (O(N^1.25)) to prevent O(N^2) stalls with dense sprite clusters. */
    uint8_t spr_order[64];
    for (i = 0; i < spr_count; i++)
        spr_order[i] = (uint8_t)i;

    if (spr_count > 1)
    {
        static const int gaps[4] = { 40, 13, 4, 1 };
        int g = (spr_count <= 4) ? 3 : ((spr_count <= 13) ? 2 : ((spr_count <= 40) ? 1 : 0));
        for (; g < 4; g++)
        {
            int gap = gaps[g];
            for (i = gap; i < spr_count; i++)
            {
                uint8_t key_idx = spr_order[i];
                fixed_t key_cz = spr_queue[key_idx].cz;
                int j = i;
                while (j >= gap)
                {
                    uint8_t prev_idx = spr_order[j - gap];
                    fixed_t prev_cz = spr_queue[prev_idx].cz;

                    /* Swap if out of order by depth, OR if depths are equal but original sequence order is inverted */
                    if (prev_cz < key_cz || (prev_cz == key_cz && prev_idx > key_idx))
                    {
                        spr_order[j] = prev_idx;
                        j -= gap;
                    }
                    else
                    {
                        break;
                    }
                }
                spr_order[j] = key_idx;
            }
        }
    }

    for (i = 0; i < spr_count; i++)
    {
        int idx = (int)spr_order[i];
        Saturn_Vdp1Spr_Add(spr_queue[idx].scache,
                           spr_queue[idx].xA, spr_queue[idx].xB,
                           spr_queue[idx].yt, spr_queue[idx].yb,
                           0, spr_queue[idx].h,
                           spr_queue[idx].shade, 0);
    }
}

static HW_NOINLINE int HW_FastFlatSrca(int flatnum)
{
    if ((unsigned)flatnum >= (unsigned)numflats)
        return 0;
    int f = flattranslation ? flattranslation[flatnum] : flatnum;
    if ((unsigned)f >= (unsigned)numflats)
        f = flatnum;
    extern v1_tag_t *saturn_v1_flat_tags;
    if (saturn_v1_flat_tags)
    {
        unsigned short s = saturn_v1_flat_tags[f].srca;
#ifdef SATURN_WIN32
        static int s_logged_srca[128] = {0};
        if (flatnum >= 0 && flatnum < 128 && !s_logged_srca[flatnum]) {
            s_logged_srca[flatnum] = 1;
            printf("[FastFlatSrca] flatnum=%d translated_f=%d tag_srca=0x%04X\n",
                   flatnum, f, s);
            fflush(stdout);
        }
#endif
        if (s != 0 && s != 0xFFFFu)
            return (int)s;
    }
    /* v1_tex_get() applies flattranslation itself: pass the UNtranslated flatnum,
     * otherwise animated flats resolve to T(T(x)) and populate the wrong tag slot. */
    return Saturn_Vdp1_GetFlatSrca(flatnum);
}

static HW_NOINLINE int HW_FastWallSrca(int texnum, int *tw, int *th, int *srca_mono, int *srca_64)
{
    if ((unsigned)texnum >= (unsigned)numtextures || texnum <= 0)
    {
        if (srca_mono) *srca_mono = 0;
        if (srca_64)   *srca_64   = 0;
        return 0;
    }
    int t = texturetranslation ? texturetranslation[texnum] : texnum;
    if ((unsigned)t >= (unsigned)numtextures)
        t = texnum;
    extern v1_tag_t *saturn_v1_wall_tags;
    if (saturn_v1_wall_tags)
    {
        unsigned short s = saturn_v1_wall_tags[t].srca;
        if (s != 0 && s != 0xFFFFu)
        {
            *tw = (int)saturn_v1_wall_tags[t].w8 << 3;
            *th = (int)saturn_v1_wall_tags[t].h;
            if (srca_mono) *srca_mono = (int)saturn_v1_wall_tags[t].srca_mono;
            if (srca_64)   *srca_64   = (int)saturn_v1_wall_tags[t].srca_64;
            return (int)s;
        }
    }
    int _tw = 0, _th = 0;
    int s = Saturn_Vdp1_GetTexSrca(texnum, &_tw, &_th); /* translates internally */
    *tw = _tw;
    *th = _th;
    if (srca_mono)
    {
        if (saturn_v1_wall_tags && saturn_v1_wall_tags[t].srca_mono)
            *srca_mono = (int)saturn_v1_wall_tags[t].srca_mono;
        else
            *srca_mono = s;
    }
    if (srca_64)
    {
        if (saturn_v1_wall_tags && saturn_v1_wall_tags[t].srca_64)
            *srca_64 = (int)saturn_v1_wall_tags[t].srca_64;
        else
            *srca_64 = s;
    }
    return s;
}




/* =========================================================================
 * Slave SH-2 Flat Quad Worker Pipeline
 * Offloads floor and ceiling quadrilateral transformation, clipping, and
 * projection to the Slave SH-2 CPU in parallel with Master seg rendering.
 * Located in Low Work RAM (0x00210000) for zero CPU-bus contention.
 * ========================================================================= */
#define HW_SLAVE_BASE            SATURN_WRAM_L_IPC_ADDR
#define HW_SLAVE_BASE_UNCACHED   SATURN_WRAM_L_IPC_UNCACHED

/* saturn_proj_v_t defined above */

#define HW_SLAVE_PROJ_VERTS_ADDR  ((saturn_proj_v_t *)SATURN_WRAM_L_PROJ_VERTS_ADDR)
#define HW_MASTER_PROJ_VERTS_ADDR ((saturn_proj_v_t *)((uint8_t *)SATURN_WRAM_L_PROJ_VERTS_ADDR + 0x6000u))
#define HW_PROJ_VERTS_ADDR        HW_SLAVE_PROJ_VERTS_ADDR

#define HW_JOB_WORDS_BEFORE_PAD (11 + HW_SOLID_WORDS)
#define HW_JOB_PAD_WORDS (((HW_JOB_WORDS_BEFORE_PAD + 3) & ~3) - HW_JOB_WORDS_BEFORE_PAD)

typedef struct {
    const saturn_flat_vert_t     *verts;
    const saturn_flat_quad_idx_t *quads;
    uint16_t v_count;
    uint16_t q_count;
    uint8_t  emit_floor;
    uint8_t  emit_ceil;
    uint8_t  fully_inside;
    uint8_t  has_sky;
    uint16_t floor_srca;
    uint16_t ceil_srca;
    fixed_t  y_floor_w;     /* floor - viewz (HW_DZQ) */
    const uint8_t *vfrac;   /* per-vertex 1/16-unit fractions (seam fix) */
    fixed_t  y_ceil_w;      /* ceiling - viewz (HW_DZQ) */
    int16_t  sec_light;
    int16_t  sub_idx;
    uint32_t solid_summary; /* Summary of fully occluded words (0xFFFFFFFFu) */
    int16_t  solid_count;   /* hw_solid_count at submit time */
    int16_t  pad;
    uint32_t solid[HW_SOLID_WORDS]; /* hw_solid_mask snapshot at submit time */
#if HW_JOB_PAD_WORDS > 0
    uint32_t align_pad[HW_JOB_PAD_WORDS]; /* Pad to 16-byte SH-2 burst alignment */
#endif
} hw_flat_job_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(hw_flat_job_t) == ((HW_JOB_WORDS_BEFORE_PAD + 3) & ~3) * 4, "hw_flat_job_t must be 16-byte aligned for IPC burst copy");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_flat_job_t) == ((HW_JOB_WORDS_BEFORE_PAD + 3) & ~3) * 4, "hw_flat_job_t must be 16-byte aligned for IPC burst copy");
#endif

#define HW_JOB_WORDS (sizeof(hw_flat_job_t) / 4)

#define HW_COPY_JOB(dst, src) do { \
    const volatile aliased_uint32_t *_src = (const volatile aliased_uint32_t *)(src); \
    aliased_uint32_t *_dst = (aliased_uint32_t *)(dst); \
    for (int _w = 0; _w < 10; _w++) *_dst++ = *_src++; \
    uint32_t _w10 = *_src++; \
    *_dst++ = _w10; \
    if ((int16_t)(_w10 >> 16) > 0) { \
        for (int _w = 11; _w < (int)HW_JOB_WORDS; _w++) *_dst++ = *_src++; \
    } \
} while (0)

#define HW_JOB_RING      256            /* power of two ring buffer */

typedef struct {
    /* Camera parameters latched per-frame (32 bytes) */
    fixed_t viewx;
    fixed_t viewy;
    fixed_t viewz;
    fixed_t viewcos;
    fixed_t viewsin;
    fixed_t cam_cz_origin;
    fixed_t cam_cx_origin;
    int16_t psec_ceilingpic;
    int16_t view_w;

    /* SPSC synchronization flags (24 bytes). job_tail / job_head are free-running
     * counters; slot = counter & (HW_JOB_RING - 1). */
    volatile unsigned int job_tail;
    volatile int job_done;  /* Level generation: master bumps after a level load so the
                             * slave purges its L1 (flat geometry is reloaded in place) */
    volatile int quad_count;
    volatile int sky_visible;
    volatile unsigned int job_head;
    volatile unsigned int job_active;
    int16_t view_cx;
    int16_t view_cy;
    int16_t view_h;
    int16_t align_pad;      /* Pad header to 64 bytes for 16-byte burst alignment */

    /* Job queue (ring) */
    hw_flat_job_t jobs[HW_JOB_RING];
} hw_slave_ipc_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(hw_slave_ipc_t) <= 0x8000u, "hw_slave_ipc_t must fit below SATURN_WRAM_L_SLAVE_STACK_OFF");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_slave_ipc_t) <= 0x8000u, "hw_slave_ipc_t must fit below SATURN_WRAM_L_SLAVE_STACK_OFF");
#endif

/* HW_Slave_IsSpanOccluded defined below hw_flat_emit_ctx_t */

static int saturn_master_flat_q = HW_SLAVE_FLAT_Q_CAP;
static int saturn_master_flat_q_done = 0;
#define HW_MAX_DEF_FLAT_JOBS 4
static HW_ALIGN16 hw_flat_job_t hw_def_flat_jobs[HW_MAX_DEF_FLAT_JOBS];
static int hw_def_flat_count = 0;
static int hw_frame_flat_jobs = 0;
static unsigned int hw_master_job_tail = 0;

HW_ALWAYS_INLINE int HW_FlatQuadDy(int x0, int y0, int x1, int y1,
                                   int x2, int y2, int x3, int y3)
{
    int cy0 = (y0 < 0) ? 0 : ((y0 > HW_VIEW_H) ? HW_VIEW_H : y0);
    int cy1 = (y1 < 0) ? 0 : ((y1 > HW_VIEW_H) ? HW_VIEW_H : y1);
    int cy2 = (y2 < 0) ? 0 : ((y2 > HW_VIEW_H) ? HW_VIEW_H : y2);
    int cy3 = (y3 < 0) ? 0 : ((y3 > HW_VIEW_H) ? HW_VIEW_H : y3);
    int d03 = cy3 - cy0; if (d03 < 0) d03 = -d03;
    int d12 = cy2 - cy1; if (d12 < 0) d12 = -d12;
    int d01 = cy1 - cy0; if (d01 < 0) d01 = -d01;
    int d32 = cy2 - cy3; if (d32 < 0) d32 = -d32;
    int dy_depth = (d03 > d12) ? d03 : d12;
    int dy = dy_depth;
    if (d01 > dy) dy = d01;
    if (d32 > dy) dy = d32;
    if (__builtin_expect(dy > 16, 1))
    {
        if (((x0 == x1 && y0 == y1) | (x1 == x2 && y1 == y2) |
             (x2 == x3 && y2 == y3) | (x3 == x0 && y3 == y0)))
            dy = 16;
    }
    /* Flag grazing oblique quads where horizontal edge shear >> vertical depth span */
    int dx03 = x3 - x0; if (dx03 < 0) dx03 = -dx03;
    int dx12 = x2 - x1; if (dx12 < 0) dx12 = -dx12;
    int dx_shear = (dx03 > dx12) ? dx03 : dx12;
    if (dy_depth <= 6 && dx_shear > (dy_depth << 1))
        dy |= 0x4000;
    return dy;
}

HW_ALWAYS_INLINE int HW_FlatTriDy(int y0, int y1, int y2)
{
    int d02 = y2 - y0; if (d02 < 0) d02 = -d02;
    int d12 = y2 - y1; if (d12 < 0) d12 = -d12;
    int d01 = y1 - y0; if (d01 < 0) d01 = -d01;
    int dy = (d02 > d12) ? d02 : d12;
    if (d01 > dy) dy = d01;
    if (dy > 16) dy = 16;
    return dy;
}

/* Takes 3 register arguments (r4=base_srca, r5=quad_size, r6=dy), 0 stack arguments, 0 callee-saved reg pushes */
static HW_NOINLINE uint32_t HW_FlatSizeLOD(uint16_t base_srca, uint32_t quad_size, int dy)
{
    int grazing = (dy & 0x4000);
    dy &= 0x3FFF;
    int v0   = (int)(quad_size >> 16);
    int rows = (int)(quad_size & 0xFFu);
    if (__builtin_expect(grazing != 0 && rows >= 32, 0))
    {
        v0   += 8;
        rows -= 16;
    }

    if (dy > 24)
    {
        uint32_t srca = (uint32_t)base_srca + (uint32_t)(v0 << 3);
        return (srca << 16) | 0x0800u | (uint32_t)rows;
    }
    if (dy > 10)
    {
        int v0_32   = v0 >> 1;
        int rows_32 = (rows + 1) >> 1;
        if (rows_32 < 1) rows_32 = 1;
        if (v0_32 + rows_32 > 32) rows_32 = 32 - v0_32;
        uint32_t srca = (uint32_t)base_srca + 512u + (uint32_t)(v0_32 << 2);
        return (srca << 16) | 0x0400u | (uint32_t)rows_32;
    }
    if (dy > 3)
    {
        int v0_16   = v0 >> 2;
        int rows_16 = (rows + 3) >> 2;
        if (rows_16 < 1) rows_16 = 1;
        if (v0_16 + rows_16 > 16) rows_16 = 16 - v0_16;
        uint32_t srca = (uint32_t)base_srca + 640u + (uint32_t)(v0_16 << 1);
        return (srca << 16) | 0x0200u | (uint32_t)rows_16;
    }
    {
        int v0_8   = v0 >> 3;
        int rows_8 = (rows + 7) >> 3;
        if (rows_8 < 1) rows_8 = 1;
        if (v0_8 + rows_8 > 8) rows_8 = 8 - v0_8;
        uint32_t srca = (uint32_t)base_srca + 672u + (uint32_t)v0_8;
        return (srca << 16) | 0x0100u | (uint32_t)rows_8;
    }
}

static HW_NOINLINE void HW_Worker_EmitFlatQuadDirect(vdp1_cmd_t *cmd_s_out, int *qcount_ptr, int qcap,
                                                int x0, int y0, int x1, int y1,
                                                int x2, int y2, int x3, int y3,
                                                int srca, int shade)
{
    /* Fast zero-stall dual-MAC cross product without destructive shifts */
    int cw = HW_MulsW_PairAdd(x2 - x0, y3 - y1, x1 - x3, y2 - y0, 0);
    if (cw == 0) return;

    int qcount = *qcount_ptr;
    if (qcount < qcap)
    {
        aliased_uint32_t *out32 = (aliased_uint32_t *)&cmd_s_out[qcount];
        uint32_t pmod = (PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu;
        uint32_t colr = (uint32_t)shade & 0xFF00u;
        uint32_t w0, w3, w4, w5, w6;

        if (cw < 0)
        {
            w0 = 0x00320000u; /* Distorted sprite + VDP1 H-FLIP + V-FLIP */
            w3 = ((uint32_t)x1 << 16) | (uint16_t)y1;
            w4 = ((uint32_t)x0 << 16) | (uint16_t)y0;
            w5 = ((uint32_t)x3 << 16) | (uint16_t)y3;
            w6 = ((uint32_t)x2 << 16) | (uint16_t)y2;
        }
        else
        {
            w0 = 0x00220000u; /* Distorted sprite + VDP1 V-FLIP */
            w3 = ((uint32_t)x0 << 16) | (uint16_t)y0;
            w4 = ((uint32_t)x1 << 16) | (uint16_t)y1;
            w5 = ((uint32_t)x2 << 16) | (uint16_t)y2;
            w6 = ((uint32_t)x3 << 16) | (uint16_t)y3;
        }

        uint32_t w1 = (pmod << 16) | colr;
        int dy = HW_FlatQuadDy(x0, y0, x1, y1, x2, y2, x3, y3);
        uint32_t w2 = HW_FlatSizeLOD((uint16_t)srca, 0x0840u, dy);
        *qcount_ptr = qcount + 1;

        /* Sequential burst writes coalesced by SH-2 uncached write-through buffer */
        out32[0] = w0;
        out32[1] = w1;
        out32[2] = w2;
        out32[3] = w3;
        out32[4] = w4;
        out32[5] = w5;
        out32[6] = w6;
        out32[7] = 0;
    }
}

HW_ALWAYS_INLINE int HW_ClampProjWide(int v)
{
    if (__builtin_expect((unsigned int)v + 8192u <= 16384u, 1))
        return v;
    return (v >> 31) ^ 8191;
}
#define HW_CLAMP_PROJ_WIDE(v) HW_ClampProjWide(v)

static HW_NOINLINE HW_COLD_OS int HW_ClipCoord(int a_c, int a_p, int b_c, int b_p, int clip_p)
{
    if (a_c == b_c) return a_c;
    int dp = b_p - a_p;
    int num = clip_p - a_p;
    if (dp < 0)
    {
        dp = -dp;
        num = -num;
    }
    if (dp <= 0 || num <= 0) return a_c;
    if (num >= dp) return b_c;
    int frac = HW_ClampedFrac((unsigned int)num, (unsigned int)dp);
    return a_c + FixedMul(b_c - a_c, frac);
}

static HW_NOINLINE HW_COLD_OS void HW_AddClipTri(
    int *ntris, int tx[][3], int ty[][3],
    int x0, int y0, int x1, int y1, int x2, int y2,
    int vw, int vh)
{
    if (!ntris || *ntris >= 6) return;
    if ((x0 <= 0 && x1 <= 0 && x2 <= 0) || (x0 >= vw && x1 >= vw && x2 >= vw) ||
        (y0 <= 0 && y1 <= 0 && y2 <= 0) || (y0 >= vh && y1 >= vh && y2 >= vh))
        return;
    x0 = HW_CLAMP_PROJ(x0); y0 = HW_CLAMP_PROJ(y0);
    x1 = HW_CLAMP_PROJ(x1); y1 = HW_CLAMP_PROJ(y1);
    x2 = HW_CLAMP_PROJ(x2); y2 = HW_CLAMP_PROJ(y2);
    if ((x0 == x1 && y0 == y1) || (x1 == x2 && y1 == y2) || (x2 == x0 && y2 == y0))
        return;
    int idx = *ntris;
    tx[idx][0] = x0; ty[idx][0] = y0;
    tx[idx][1] = x1; ty[idx][1] = y1;
    tx[idx][2] = x2; ty[idx][2] = y2;
    *ntris = idx + 1;
}

static HW_NOINLINE HW_COLD_OS void HW_AddClipTriSwap(
    int *ntris, int tx[][3], int ty[][3],
    int c0, int p0, int c1, int p1, int c2, int p2,
    int swap_xy, int vw, int vh)
{
    if (swap_xy)
        HW_AddClipTri(ntris, tx, ty, p0, c0, p1, c1, p2, c2, vw, vh);
    else
        HW_AddClipTri(ntris, tx, ty, c0, p0, c1, p1, c2, p2, vw, vh);
}

#ifndef _MSC_VER
static __attribute__((noinline, optimize("Os"))) void HW_ClipQuadPlane(
#else
static __declspec(noinline) void HW_ClipQuadPlane(
#endif
    int *c0_p, int *p0_p, int *c1_p, int *p1_p,
    int *c2_p, int *p2_p, int *c3_p, int *p3_p,
    int clip_p, int mask, int dir, int swap_xy,
    int *ntris, int tx[][3], int ty[][3], int vw, int vh)
{
    int c0 = *c0_p, p0 = *p0_p;
    int c1 = *c1_p, p1 = *p1_p;
    int c2 = *c2_p, p2 = *p2_p;
    int c3 = *c3_p, p3 = *p3_p;
    int clip_far = clip_p - (dir * (swap_xy ? 4 : 2));

    switch (mask)
    {
        case 9:
            c0 = HW_ClipCoord(c1, p1, c0, p0, clip_p); p0 = clip_p;
            c3 = HW_ClipCoord(c2, p2, c3, p3, clip_p); p3 = clip_p;
            break;
        case 3:
            c0 = HW_ClipCoord(c3, p3, c0, p0, clip_p); p0 = clip_p;
            c1 = HW_ClipCoord(c2, p2, c1, p1, clip_p); p1 = clip_p;
            break;
        case 6:
            c1 = HW_ClipCoord(c0, p0, c1, p1, clip_p); p1 = clip_p;
            c2 = HW_ClipCoord(c3, p3, c2, p2, clip_p); p2 = clip_p;
            break;
        case 12:
            c2 = HW_ClipCoord(c1, p1, c2, p2, clip_p); p2 = clip_p;
            c3 = HW_ClipCoord(c0, p0, c3, p3, clip_p); p3 = clip_p;
            break;
        case 14:
            c1 = HW_ClipCoord(c0, p0, c1, p1, clip_p); p1 = clip_p;
            c3 = HW_ClipCoord(c0, p0, c3, p3, clip_p); p3 = clip_p;
            c2 = c3; p2 = clip_p;
            break;
        case 13:
            c0 = HW_ClipCoord(c1, p1, c0, p0, clip_p); p0 = clip_p;
            c2 = HW_ClipCoord(c1, p1, c2, p2, clip_p); p2 = clip_p;
            c3 = c0; p3 = clip_p;
            break;
        case 11:
            c1 = HW_ClipCoord(c2, p2, c1, p1, clip_p); p1 = clip_p;
            c3 = HW_ClipCoord(c2, p2, c3, p3, clip_p); p3 = clip_p;
            c0 = c3; p0 = clip_p;
            break;
        case 7:
            c0 = HW_ClipCoord(c3, p3, c0, p0, clip_p); p0 = clip_p;
            c2 = HW_ClipCoord(c3, p3, c2, p2, clip_p); p2 = clip_p;
            c1 = c2; p1 = clip_p;
            break;
        case 1:
        {
            if ((p0 - clip_far) * dir >= 0) break;
            int ca = HW_ClipCoord(c1, p1, c0, p0, clip_far);
            int cb = HW_ClipCoord(c3, p3, c0, p0, clip_far);
            if ((p3 - p1) * dir <= 0) {
                HW_AddClipTriSwap(ntris, tx, ty, ca, clip_far, c3, p3, cb, clip_far, swap_xy, vw, vh);
                c0 = ca; p0 = clip_far;
            } else {
                HW_AddClipTriSwap(ntris, tx, ty, ca, clip_far, c1, p1, cb, clip_far, swap_xy, vw, vh);
                c0 = cb; p0 = clip_far;
            }
            break;
        }
        case 2:
        {
            if ((p1 - clip_far) * dir >= 0) break;
            int ca = HW_ClipCoord(c0, p0, c1, p1, clip_far);
            int cb = HW_ClipCoord(c2, p2, c1, p1, clip_far);
            if ((p2 - p0) * dir <= 0) {
                HW_AddClipTriSwap(ntris, tx, ty, ca, clip_far, cb, clip_far, c2, p2, swap_xy, vw, vh);
                c1 = ca; p1 = clip_far;
            } else {
                HW_AddClipTriSwap(ntris, tx, ty, c0, p0, ca, clip_far, cb, clip_far, swap_xy, vw, vh);
                c1 = cb; p1 = clip_far;
            }
            break;
        }
        case 4:
        {
            if ((p2 - clip_far) * dir >= 0) break;
            int ca = HW_ClipCoord(c1, p1, c2, p2, clip_far);
            int cb = HW_ClipCoord(c3, p3, c2, p2, clip_far);
            if ((p3 - p1) * dir <= 0) {
                HW_AddClipTriSwap(ntris, tx, ty, ca, clip_far, cb, clip_far, c3, p3, swap_xy, vw, vh);
                c2 = ca; p2 = clip_far;
            } else {
                HW_AddClipTriSwap(ntris, tx, ty, c1, p1, ca, clip_far, cb, clip_far, swap_xy, vw, vh);
                c2 = cb; p2 = clip_far;
            }
            break;
        }
        case 8:
        {
            if ((p3 - clip_far) * dir >= 0) break;
            int ca = HW_ClipCoord(c0, p0, c3, p3, clip_far);
            int cb = HW_ClipCoord(c2, p2, c3, p3, clip_far);
            if ((p2 - p0) * dir <= 0) {
                HW_AddClipTriSwap(ntris, tx, ty, ca, clip_far, c2, p2, cb, clip_far, swap_xy, vw, vh);
                c3 = ca; p3 = clip_far;
            } else {
                HW_AddClipTriSwap(ntris, tx, ty, c0, p0, cb, clip_far, ca, clip_far, swap_xy, vw, vh);
                c3 = cb; p3 = clip_far;
            }
            break;
        }
        default:
            break;
    }

    *c0_p = c0; *p0_p = p0;
    *c1_p = c1; *p1_p = p1;
    *c2_p = c2; *p2_p = p2;
    *c3_p = c3; *p3_p = p3;
}

#ifndef _MSC_VER
static __attribute__((noinline, optimize("Os"))) int HW_ClipFlatQuadOffscreen(
#else
static __declspec(noinline) int HW_ClipFlatQuadOffscreen(
#endif
    int *p_x0, int *p_y0, int *p_x1, int *p_y1,
    int *p_x2, int *p_y2, int *p_x3, int *p_y3,
    int vw, int vh,
    int *out_ntris, int tx[][3], int ty[][3])
{
    int x0 = *p_x0, y0 = *p_y0;
    int x1 = *p_x1, y1 = *p_y1;
    int x2 = *p_x2, y2 = *p_y2;
    int x3 = *p_x3, y3 = *p_y3;
    int ntris = 0;

    const int y_top = -4;
    int m_top = (y0 < y_top) | ((y1 < y_top) << 1) | ((y2 < y_top) << 2) | ((y3 < y_top) << 3);
    if (m_top == 15) return 0;
    if (m_top != 0)
        HW_ClipQuadPlane(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, y_top, m_top, 1, 0, &ntris, tx, ty, vw, vh);

    const int y_bot = vh + 4;
    int m_bot = (y0 > y_bot) | ((y1 > y_bot) << 1) | ((y2 > y_bot) << 2) | ((y3 > y_bot) << 3);
    if (m_bot == 15) return 0;
    if (m_bot != 0)
        HW_ClipQuadPlane(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, y_bot, m_bot, -1, 0, &ntris, tx, ty, vw, vh);

    if (ntris == 0)
    {
        if ((x0 <= 0 && x1 <= 0 && x2 <= 0 && x3 <= 0) ||
            (x0 >= vw && x1 >= vw && x2 >= vw && x3 >= vw))
            return 0;
    }

    const int x_left = -12;
    int m_left = (x0 < x_left) | ((x1 < x_left) << 1) | ((x2 < x_left) << 2) | ((x3 < x_left) << 3);
    if (m_left == 15 && ntris == 0) return 0;
    if (m_left != 0 && m_left != 15)
        HW_ClipQuadPlane(&y0, &x0, &y1, &x1, &y2, &x2, &y3, &x3, x_left, m_left, 1, 1, &ntris, tx, ty, vw, vh);

    const int x_right = vw + 12;
    int m_right = (x0 > x_right) | ((x1 > x_right) << 1) | ((x2 > x_right) << 2) | ((x3 > x_right) << 3);
    if (m_right == 15 && ntris == 0) return 0;
    if (m_right != 0 && m_right != 15)
        HW_ClipQuadPlane(&y0, &x0, &y1, &x1, &y2, &x2, &y3, &x3, x_right, m_right, -1, 1, &ntris, tx, ty, vw, vh);

    if (m_left != 0 || m_right != 0)
    {
        int mt2 = (y0 < y_top) | ((y1 < y_top) << 1) | ((y2 < y_top) << 2) | ((y3 < y_top) << 3);
        if (mt2 == 15 && ntris == 0) return 0;
        if (mt2 != 0 && mt2 != 15)
            HW_ClipQuadPlane(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, y_top, mt2, 1, 0, &ntris, tx, ty, vw, vh);

        int mb2 = (y0 > y_bot) | ((y1 > y_bot) << 1) | ((y2 > y_bot) << 2) | ((y3 > y_bot) << 3);
        if (mb2 == 15 && ntris == 0) return 0;
        if (mb2 != 0 && mb2 != 15)
            HW_ClipQuadPlane(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, y_bot, mb2, -1, 0, &ntris, tx, ty, vw, vh);
    }

    x0 = HW_CLAMP_PROJ(x0); y0 = HW_CLAMP_PROJ(y0);
    x1 = HW_CLAMP_PROJ(x1); y1 = HW_CLAMP_PROJ(y1);
    x2 = HW_CLAMP_PROJ(x2); y2 = HW_CLAMP_PROJ(y2);
    x3 = HW_CLAMP_PROJ(x3); y3 = HW_CLAMP_PROJ(y3);

    *p_x0 = x0; *p_y0 = y0;
    *p_x1 = x1; *p_y1 = y1;
    *p_x2 = x2; *p_y2 = y2;
    *p_x3 = x3; *p_y3 = y3;
    if (out_ntris)
        *out_ntris = ntris;
    return 1;
}

HW_ALWAYS_INLINE void HW_Worker_EmitNormalizedQuad(vdp1_cmd_t *cmd_s_out, int *qcount_ptr, int qcap,
                                                int x0, int y0, int x1, int y1,
                                                int x2, int y2, int x3, int y3,
                                                int srca, int shade, int vw, int vh)
{
    int m_neg = (x0 & x1 & x2 & x3) | (y0 & y1 & y2 & y3);
    int pos_x = (x0 - vw) | (x1 - vw) | (x2 - vw) | (x3 - vw);
    int pos_y = (y0 - vh) | (y1 - vh) | (y2 - vh) | (y3 - vh);
    if ((m_neg | ~(pos_x & pos_y)) < 0)
        return;
    if (srca <= 0) return;

    int ntris = 0, tx[6][3], ty[6][3];
    int r_lim = vw + 48;
    int b_lim = vh + 24;
    if (((x0 + 48) | (x1 + 48) | (x2 + 48) | (x3 + 48) |
         (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
         (y0 + 24) | (y1 + 24) | (y2 + 24) | (y3 + 24) |
         (b_lim - y0) | (b_lim - y1) | (b_lim - y2) | (b_lim - y3)) < 0)
    {
        if (!HW_ClipFlatQuadOffscreen(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, vw, vh,
                                      &ntris, tx, ty))
            return;
    }

    HW_Worker_EmitFlatQuadDirect(cmd_s_out, qcount_ptr, qcap, x0, y0, x1, y1, x2, y2, x3, y3, srca, shade);
    for (int t = 0; t < ntris; t++)
        HW_Worker_EmitFlatQuadDirect(cmd_s_out, qcount_ptr, qcap,
                                     tx[t][0], ty[t][0], tx[t][1], ty[t][1], tx[t][2], ty[t][2], tx[t][2], ty[t][2],
                                     srca, shade);
}

HW_ALWAYS_INLINE void HW_Worker_EmitTriAsQuads(vdp1_cmd_t *cmd_s_out, int *qcount_ptr, int qcap,
                                            int x0, int y0, int x1, int y1, int x2, int y2,
                                            int srca, int shade, int vw, int vh)
{
    /* VDP1 flawlessly draws triangles if you feed it a degenerate quad */
    HW_Worker_EmitNormalizedQuad(cmd_s_out, qcount_ptr, qcap, x0, y0, x1, y1, x2, y2, x2, y2, srca, shade, vw, vh);
}

static HW_NOINLINE HW_HOT_O2 void HW_Worker_EmitPolyQuads(vdp1_cmd_t *cmd_s_out, int *qcount_ptr, int qcap,
                                                              const int *px, const int *py, int n, int srca, int shade, int vw, int vh)
{
    int k = 1;
    if (n < 3) return;
    while (n - k >= 3)
    {
        HW_Worker_EmitNormalizedQuad(cmd_s_out, qcount_ptr, qcap, px[0], py[0], px[k], py[k], px[k+1], py[k+1], px[k+2], py[k+2], srca, shade, vw, vh);
        k += 2;
    }
    if (n - k == 2)
    {
        HW_Worker_EmitTriAsQuads(cmd_s_out, qcount_ptr, qcap, px[0], py[0], px[k], py[k], px[k+1], py[k+1], srca, shade, vw, vh);
    }
}

typedef struct {
    vdp1_cmd_t             *cmd_s_out;
    saturn_proj_v_t        *proj_v;
    const uint16_t         *flat_cmdcolr_lut;
    fixed_t                 y_floor_w;
    fixed_t                 y_ceil_w;
    fixed_t                 vcos;
    fixed_t                 vsin;
    fixed_t                 cz_origin;
    fixed_t                 cx_origin;
    int                     cam_x16;
    int                     cam_y16;
    int                     local_q;
    int                     cache_gen;
    int                     qcap;
    int                     vw;
    int                     vh;
    int                     vcx;
    int                     vcy;
    uint32_t                solid_summary;
    uint32_t                solid[HW_SOLID_WORDS];
    uint16_t                solid_any;
    uint16_t                floor_srca;
    uint16_t                ceil_srca;
    uint8_t                 emit_floor;
    uint8_t                 emit_ceil;
    uint8_t                 fully_inside;
    uint8_t                 solid_count;
    int                     clip_xy[8];
} hw_flat_emit_ctx_t;

static hw_flat_emit_ctx_t hw_fctx_master;

HW_ALWAYS_INLINE int HW_Slave_IsSpanOccluded(const hw_flat_emit_ctx_t *fctx, int x0, int x1, int vw)
{
    if (fctx->solid_count <= 0)
        return 0;
    if (fctx->solid_count >= vw)
        return 1;

    if (x0 < 0) x0 = 0;
    if (x1 > vw) x1 = vw;
    if (x0 >= x1)
        return 0;

    int w0 = x0 >> 5;
    uint32_t s_any = fctx->solid_any;
    if (!(s_any & hw_bit_lut[w0]))
        return 0;

    int w1 = (x1 - 1) >> 5;
    int b0 = x0 & 31;
    int b1 = (x1 - 1) & 31;

    if (w0 == w1)
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        return ((fctx->solid[w0] & mask) == mask);
    }

    if (!(s_any & hw_bit_lut[w1]))
        return 0;

    uint32_t mask0 = hw_mask_left[b0];
    if ((fctx->solid[w0] & mask0) != mask0)
        return 0;

    /* O(1) Occlusion Check: Static mask table XOR eliminates SH-2 barrel shifter emulation */
    if (w1 > w0 + 1)
    {
        uint32_t req_bits = hw_span_mask_lut[w1] ^ hw_span_mask_lut[w0 + 1];
        if ((fctx->solid_summary & req_bits) != req_bits)
            return 0;
    }

    uint32_t mask1 = hw_mask_right[b1];
    return ((fctx->solid[w1] & mask1) == mask1);
}

/* Cold helper for offscreen-clipped flat quads & generated clip triangles.
 * Takes 4 register arguments (r4=fctx, r5=local_q, r6=w1, r7=size_and_ceil), 0 stack arguments! */
static HW_NOINLINE HW_COLD_OS int HW_EmitFlatQuad_Clipped(
    hw_flat_emit_ctx_t *fctx, int local_q, uint32_t w1, uint32_t size_and_ceil)
{
    int x0 = fctx->clip_xy[0], y0 = fctx->clip_xy[1];
    int x1 = fctx->clip_xy[2], y1 = fctx->clip_xy[3];
    int x2 = fctx->clip_xy[4], y2 = fctx->clip_xy[5];
    int x3 = fctx->clip_xy[6], y3 = fctx->clip_xy[7];
    int vw = fctx->vw, vh = fctx->vh;
    int qcap = fctx->qcap;
    vdp1_cmd_t *cmd_s_out = fctx->cmd_s_out;
    uint16_t base_srca = (size_and_ceil & 0x80000000u) ? fctx->ceil_srca : fctx->floor_srca;
    uint32_t quad_size = size_and_ceil & 0x7FFFFFFFu;
    int ntris = 0, tx[6][3], ty[6][3];

    if (!HW_ClipFlatQuadOffscreen(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, vw, vh,
                                  &ntris, tx, ty))
        return local_q;

    int cw = HW_MulsW_PairAdd(x2 - x0, y3 - y1, x1 - x3, y2 - y0, 0);
    if (cw != 0 && local_q < qcap)
    {
        uint32_t p0 = ((uint32_t)x0 << 16) | (uint16_t)y0;
        uint32_t p1 = ((uint32_t)x1 << 16) | (uint16_t)y1;
        uint32_t p2 = ((uint32_t)x2 << 16) | (uint16_t)y2;
        uint32_t p3 = ((uint32_t)x3 << 16) | (uint16_t)y3;
        int dy = HW_FlatQuadDy(x0, y0, x1, y1, x2, y2, x3, y3);

        vdp1_cmd_t *cmd = &cmd_s_out[local_q++];
        aliased_uint32_t *c32 = (aliased_uint32_t *)cmd;
        c32[0] = (cw < 0) ? 0x00320000u : 0x00220000u;
        c32[1] = w1;
        c32[2] = HW_FlatSizeLOD(base_srca, quad_size, dy);
        if (cw < 0)
        {
            c32[3] = p1; c32[4] = p0; c32[5] = p3; c32[6] = p2;
        }
        else
        {
            c32[3] = p0; c32[4] = p1; c32[5] = p2; c32[6] = p3;
        }
        c32[7] = 0;
    }

    const int *ptx = tx[0], *pty = ty[0];
    for (int t = 0; t < ntris && local_q < qcap; t++, ptx += 3, pty += 3)
    {
        int tx0 = ptx[0], ty0 = pty[0];
        int tx1 = ptx[1], ty1 = pty[1];
        int tx2 = ptx[2], ty2 = pty[2];
        int cw_t = HW_MulsW_PairAdd(tx2 - tx0, ty2 - ty1, tx1 - tx2, ty2 - ty0, 0);
        if (cw_t != 0)
        {
            uint32_t p0 = ((uint32_t)tx0 << 16) | (uint16_t)ty0;
            uint32_t p1 = ((uint32_t)tx1 << 16) | (uint16_t)ty1;
            uint32_t p2 = ((uint32_t)tx2 << 16) | (uint16_t)ty2;
            int dy = HW_FlatTriDy(ty0, ty1, ty2);
            vdp1_cmd_t *cmd = &cmd_s_out[local_q++];
            aliased_uint32_t *c32 = (aliased_uint32_t *)cmd;
            c32[0] = (cw_t < 0) ? 0x00320000u : 0x00220000u;
            c32[1] = w1;
            c32[2] = HW_FlatSizeLOD(base_srca, quad_size, dy);
            if (cw_t < 0)
            {
                c32[3] = p1; c32[4] = p0; c32[5] = p2; c32[6] = p2;
            }
            else
            {
                c32[3] = p0; c32[4] = p1; c32[5] = p2; c32[6] = p2;
            }
            c32[7] = 0;
        }
    }
    return local_q;
}

/* Takes 4 register arguments (r4=fctx, r5=q_ptr, r6=cw, r7=qpack), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 int HW_EmitFlatBands_Multi(
    hw_flat_emit_ctx_t *fctx, const saturn_flat_quad_idx_t *q_ptr, int cw, uint32_t qpack)
{
    const uint32_t pmod = (PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu;
    const saturn_proj_v_t *proj_v = fctx->proj_v;
    const saturn_proj_v_t *pv0 = &proj_v[q_ptr->v[0]];
    const saturn_proj_v_t *pv1 = &proj_v[q_ptr->v[1]];
    const saturn_proj_v_t *pv2 = &proj_v[q_ptr->v[2]];
    const saturn_proj_v_t *pv3 = &proj_v[q_ptr->v[3]];

    int local_q     = (int)(qpack & 0x3FFFu);
    int n_L         = 1 + (int)((qpack >> 14) & 1u);
    int n_R         = 1 + (int)((qpack >> 15) & 1u);
    int quad_rows   = (int)((qpack >> 16) & 0xFFu);
    int base_v0     = (int)((qpack >> 24) & 0x7Fu);
    uint32_t is_ceil_bit = qpack & 0x80000000u;

    int py0, py1, py2, py3;
    fixed_t y_world;
    uint16_t base_srca;
    if (is_ceil_bit)
    {
        py0 = pv0->py_c; py1 = pv1->py_c; py2 = pv2->py_c; py3 = pv3->py_c;
        y_world   = fctx->y_ceil_w;
        base_srca = fctx->ceil_srca;
    }
    else
    {
        py0 = pv0->py_f; py1 = pv1->py_f; py2 = pv2->py_f; py3 = pv3->py_f;
        y_world   = fctx->y_floor_w;
        base_srca = fctx->floor_srca;
    }

    int vw = fctx->vw, vh = fctx->vh;
    int qcap = fctx->qcap;
    vdp1_cmd_t *cmd_s_out = fctx->cmd_s_out;
    const uint16_t *flat_cmdcolr_lut = fctx->flat_cmdcolr_lut;

    int px_L[3], py_L[3], px_R[3], py_R[3];
    fixed_t cz_L[3], cz_R[3];

    px_L[0] = pv0->px; py_L[0] = py0; cz_L[0] = pv0->cz;
    px_R[0] = pv1->px; py_R[0] = py1; cz_R[0] = pv1->cz;
    px_L[2] = pv3->px; py_L[2] = py3; cz_L[2] = pv3->cz;
    px_R[2] = pv2->px; py_R[2] = py2; cz_R[2] = pv2->cz;

    fixed_t cz_L1 = (pv0->cz >> 1) + (pv3->cz >> 1);
    fixed_t cx_L1 = (pv0->cx >> 1) + (pv3->cx >> 1);
    fixed_t cz_R1 = (pv1->cz >> 1) + (pv2->cz >> 1);
    fixed_t cx_R1 = (pv1->cx >> 1) + (pv2->cx >> 1);

    cz_L[1] = cz_L1;
    cz_R[1] = cz_R1;

    int s_L = HW_FastDiv(cz_L1);
    int s_R = HW_FastDiv(cz_R1);
    int pxL, pxR;
    HW_ProjXPair(cx_L1, s_L, cx_R1, s_R, fctx->vcx, &pxL, &pxR);
    pxL = HW_CLAMP_PROJ_WIDE(pxL);
    pxR = HW_CLAMP_PROJ_WIDE(pxR);

    int pyL, pyR;
    HW_ProjYPair(fctx->vcy, y_world, s_L, y_world, s_R, &pyL, &pyR);
    pyL = HW_CLAMP_PROJ_WIDE(pyL);
    pyR = HW_CLAMP_PROJ_WIDE(pyR);

    px_L[1] = pxL; py_L[1] = pyL;
    px_R[1] = pxR; py_R[1] = pyR;

    if (n_L < 2 && py_L[0] >= -4 && py_L[2] >= -4 && py_L[0] <= vh + 4 && py_L[2] <= vh + 4)
    {
        px_L[1] = (px_L[0] + px_L[2]) >> 1;
        py_L[1] = (py_L[0] + py_L[2]) >> 1;
    }
    if (n_R < 2 && py_R[0] >= -4 && py_R[2] >= -4 && py_R[0] <= vh + 4 && py_R[2] <= vh + 4)
    {
        px_R[1] = (px_R[0] + px_R[2]) >> 1;
        py_R[1] = (py_R[0] + py_R[2]) >> 1;
    }

    int r_lim = vw + 48;
    int b_lim = vh + 24;

    for (int s = 0; s < 2; s++)
    {
        int x0 = px_L[s],   y0 = py_L[s];
        int x1 = px_R[s],   y1 = py_R[s];
        int x2 = px_R[s+1], y2 = py_R[s+1];
        int x3 = px_L[s+1], y3 = py_L[s+1];
        int m_neg = (x0 & x1 & x2 & x3) | (y0 & y1 & y2 & y3);
        int pos_x = (x0 - vw) | (x1 - vw) | (x2 - vw) | (x3 - vw);
        int pos_y = (y0 - vh) | (y1 - vh) | (y2 - vh) | (y3 - vh);
        if ((m_neg | ~(pos_x & pos_y)) < 0)
            continue;

        int s_clipped = ((y0 | y1 | y2 | y3 | (vh - y0) | (vh - y1) | (vh - y2) | (vh - y3)) < 0);
        int r_start = (s * quad_rows) >> 1;
        int r_end   = ((s + 1) * quad_rows) >> 1;
        int s_rows  = r_end - r_start;
        if (s_rows < 1) s_rows = 1;
        int v0 = base_v0 + (quad_rows - r_end);
        uint32_t strip_size = ((uint32_t)v0 << 16) | 0x0800u | (uint32_t)s_rows;

        unsigned int cz_s_avg = ((unsigned int)cz_L[s] >> 2) + ((unsigned int)cz_R[s] >> 2) +
                                ((unsigned int)cz_L[s+1] >> 2) + ((unsigned int)cz_R[s+1] >> 2);
        unsigned int cz_s_beg = ((unsigned int)cz_L[s] >> 1) + ((unsigned int)cz_R[s] >> 1);
        unsigned int cz_s_end = ((unsigned int)cz_L[s+1] >> 1) + ((unsigned int)cz_R[s+1] >> 1);
        unsigned int cz_s_min = (cz_s_beg < cz_s_end) ? cz_s_beg : cz_s_end;
        unsigned int cz_s_max = (cz_s_beg > cz_s_end) ? cz_s_beg : cz_s_end;
        unsigned int cz_strip = s_clipped ? ((cz_s_avg >> 1) + (cz_s_max >> 1))
                                          : ((cz_s_min >> 1) + (cz_s_avg >> 1));
        unsigned int zb = cz_strip >> 20;
        if (__builtin_expect(zb > 127u, 0)) zb = 127u;
        uint32_t w1 = (pmod << 16) | ((uint32_t)flat_cmdcolr_lut[zb] & 0xFF00u);

        if (__builtin_expect(((x0 + 48) | (x1 + 48) | (x2 + 48) | (x3 + 48) |
                              (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
                              (y0 + 24) | (y1 + 24) | (y2 + 24) | (y3 + 24) |
                              (b_lim - y0) | (b_lim - y1) | (b_lim - y2) | (b_lim - y3)) < 0, 0))
        {
            fctx->clip_xy[0] = x0; fctx->clip_xy[1] = y0;
            fctx->clip_xy[2] = x1; fctx->clip_xy[3] = y1;
            fctx->clip_xy[4] = x2; fctx->clip_xy[5] = y2;
            fctx->clip_xy[6] = x3; fctx->clip_xy[7] = y3;
            local_q = HW_EmitFlatQuad_Clipped(fctx, local_q, w1, strip_size | is_ceil_bit);
            continue;
        }

        if (cw != 0 && local_q < qcap)
        {
            uint32_t p0 = ((uint32_t)x0 << 16) | (uint16_t)y0;
            uint32_t p1 = ((uint32_t)x1 << 16) | (uint16_t)y1;
            uint32_t p2 = ((uint32_t)x2 << 16) | (uint16_t)y2;
            uint32_t p3 = ((uint32_t)x3 << 16) | (uint16_t)y3;
            int dy = HW_FlatQuadDy(x0, y0, x1, y1, x2, y2, x3, y3);

            vdp1_cmd_t *cmd = &cmd_s_out[local_q++];
            aliased_uint32_t *c32 = (aliased_uint32_t *)cmd;
            c32[0] = (cw < 0) ? 0x00320000u : 0x00220000u;
            c32[1] = w1;
            c32[2] = HW_FlatSizeLOD(base_srca, strip_size, dy);
            if (cw < 0)
            {
                c32[3] = p1; c32[4] = p0; c32[5] = p3; c32[6] = p2;
            }
            else
            {
                c32[3] = p0; c32[4] = p1; c32[5] = p2; c32[6] = p3;
            }
            c32[7] = 0;
        }
    }
    return local_q;
}

/* Takes 4 register arguments (r4=fctx, r5=q_ptr, r6=cw, r7=qpack), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 int HW_EmitFlatBands(
    hw_flat_emit_ctx_t *fctx, const saturn_flat_quad_idx_t *q_ptr, int cw, uint32_t qpack)
{
    const saturn_proj_v_t *proj_v = fctx->proj_v;
    const saturn_proj_v_t *pv0 = &proj_v[q_ptr->v[0]];
    const saturn_proj_v_t *pv1 = &proj_v[q_ptr->v[1]];
    const saturn_proj_v_t *pv2 = &proj_v[q_ptr->v[2]];
    const saturn_proj_v_t *pv3 = &proj_v[q_ptr->v[3]];

    int local_q = (int)(qpack & 0x3FFFu);
    int qcap    = fctx->qcap;
    int vh      = fctx->vh;
    int py0, py1, py2, py3;
    if (qpack & 0x80000000u)
    {
        py0 = pv0->py_c; py1 = pv1->py_c; py2 = pv2->py_c; py3 = pv3->py_c;
    }
    else
    {
        py0 = pv0->py_f; py1 = pv1->py_f; py2 = pv2->py_f; py3 = pv3->py_f;
    }

    int cy0 = (py0 < 0) ? 0 : ((py0 > vh) ? vh : py0);
    int cy1 = (py1 < 0) ? 0 : ((py1 > vh) ? vh : py1);
    int cy2 = (py2 < 0) ? 0 : ((py2 > vh) ? vh : py2);
    int cy3 = (py3 < 0) ? 0 : ((py3 > vh) ? vh : py3);
    int d03 = cy3 - cy0; if (d03 < 0) d03 = -d03;
    int d12 = cy2 - cy1; if (d12 < 0) d12 = -d12;
    fixed_t cz_L_min = (pv0->cz < pv3->cz) ? pv0->cz : pv3->cz;
    fixed_t cz_R_min = (pv1->cz < pv2->cz) ? pv1->cz : pv2->cz;
    uint32_t multi_bits = 0;
    if (__builtin_expect(cz_L_min < (180 << FRACBITS) && d03 > 36, 0))
        multi_bits |= 0x4000u;
    if (__builtin_expect(cz_R_min < (180 << FRACBITS) && d12 > 36, 0))
        multi_bits |= 0x8000u;

    if (__builtin_expect(multi_bits != 0 && local_q + 2 <= qcap, 0))
    {
        return HW_EmitFlatBands_Multi(fctx, q_ptr, cw, qpack | multi_bits);
    }

    if (local_q < qcap)
    {
        const uint32_t pmod = (PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu;
        fixed_t cz_near = (cz_L_min < cz_R_min) ? cz_L_min : cz_R_min;
        fixed_t cz_L_max = (pv0->cz > pv3->cz) ? pv0->cz : pv3->cz;
        fixed_t cz_R_max = (pv1->cz > pv2->cz) ? pv1->cz : pv2->cz;
        fixed_t cz_far = (cz_L_max > cz_R_max) ? cz_L_max : cz_R_max;
        unsigned int cz_avg = ((unsigned int)pv0->cz >> 2) + ((unsigned int)pv1->cz >> 2) + ((unsigned int)pv2->cz >> 2) + ((unsigned int)pv3->cz >> 2);
        int v_clipped = ((py0 | py1 | py2 | py3 | (vh - py0) | (vh - py1) | (vh - py2) | (vh - py3)) < 0);
        unsigned int cz = v_clipped ? ((cz_avg >> 1) + ((unsigned int)cz_far >> 1))
                                    : (((unsigned int)cz_near >> 1) + (cz_avg >> 1));
        unsigned int zb = cz >> 20;
        if (__builtin_expect(zb > 127u, 0)) zb = 127u;
        uint32_t w1 = (pmod << 16) | ((uint32_t)fctx->flat_cmdcolr_lut[zb] & 0xFF00u);

        uint32_t quad_size = ((qpack >> 8) & 0x007F0000u) | 0x0800u | ((qpack >> 16) & 0xFFu);
        int x0 = pv0->px, y0 = py0;
        int x1 = pv1->px, y1 = py1;
        int x2 = pv2->px, y2 = py2;
        int x3 = pv3->px, y3 = py3;

        int r_lim = fctx->vw + 48;
        int b_lim = vh + 24;
        if (__builtin_expect(((x0 + 48) | (x1 + 48) | (x2 + 48) | (x3 + 48) |
                              (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
                              (y0 + 24) | (y1 + 24) | (y2 + 24) | (y3 + 24) |
                              (b_lim - y0) | (b_lim - y1) | (b_lim - y2) | (b_lim - y3)) < 0, 0))
        {
            fctx->clip_xy[0] = x0; fctx->clip_xy[1] = y0;
            fctx->clip_xy[2] = x1; fctx->clip_xy[3] = y1;
            fctx->clip_xy[4] = x2; fctx->clip_xy[5] = y2;
            fctx->clip_xy[6] = x3; fctx->clip_xy[7] = y3;
            return HW_EmitFlatQuad_Clipped(fctx, local_q, w1, quad_size | (qpack & 0x80000000u));
        }

        uint16_t base_srca = (qpack & 0x80000000u) ? fctx->ceil_srca : fctx->floor_srca;
        int dy = HW_FlatQuadDy(x0, y0, x1, y1, x2, y2, x3, y3);
        uint32_t w2 = HW_FlatSizeLOD(base_srca, quad_size, dy);

        uint32_t w0, w3, w4, w5, w6;
        uint32_t x0_hi = (uint32_t)x0 << 16;
        uint32_t x1_hi = (uint32_t)x1 << 16;
        uint32_t x2_hi = (uint32_t)x2 << 16;
        uint32_t x3_hi = (uint32_t)x3 << 16;

        if (cw < 0)
        {
            w0 = 0x00320000u;
            w3 = x1_hi | (uint16_t)y1;
            w4 = x0_hi | (uint16_t)y0;
            w5 = x3_hi | (uint16_t)y3;
            w6 = x2_hi | (uint16_t)y2;
        }
        else
        {
            w0 = 0x00220000u;
            w3 = x0_hi | (uint16_t)y0;
            w4 = x1_hi | (uint16_t)y1;
            w5 = x2_hi | (uint16_t)y2;
            w6 = x3_hi | (uint16_t)y3;
        }

        vdp1_cmd_t *cmd = &fctx->cmd_s_out[local_q++];
        aliased_uint32_t *c32 = (aliased_uint32_t *)cmd;
        c32[0] = w0;
        c32[1] = w1;
        c32[2] = w2;
        c32[3] = w3;
        c32[4] = w4;
        c32[5] = w5;
        c32[6] = w6;
        c32[7] = 0;
    }
    return local_q;
}

/* Takes 3 register arguments (r4=fctx, r5=q_ptr, r6=local_q), 0 stack arguments! */
static HW_NOINLINE HW_COLD_OS int HW_Worker_ClipNearQuad(
    const hw_flat_emit_ctx_t *fctx, const saturn_flat_quad_idx_t *q_ptr, int local_q)
{
    fixed_t raw_cx[4]  __attribute__((aligned(16)));
    fixed_t raw_cz[4]  __attribute__((aligned(16)));
    fixed_t clip_cx[8] __attribute__((aligned(16)));
    fixed_t clip_cz[8] __attribute__((aligned(16)));
    int sx[8], sy_f[8], sy_c[8];
    int s_cache[8];

    const saturn_proj_v_t *proj_v = fctx->proj_v;
    const saturn_proj_v_t *pv0 = &proj_v[q_ptr->v[0]];
    const saturn_proj_v_t *pv1 = &proj_v[q_ptr->v[1]];
    const saturn_proj_v_t *pv2 = &proj_v[q_ptr->v[2]];
    const saturn_proj_v_t *pv3 = &proj_v[q_ptr->v[3]];

    raw_cx[0] = pv0->cx; raw_cz[0] = pv0->cz;
    raw_cx[1] = pv1->cx; raw_cz[1] = pv1->cz;
    raw_cx[2] = pv2->cx; raw_cz[2] = pv2->cz;
    raw_cx[3] = pv3->cx; raw_cz[3] = pv3->cz;

    if (!fctx->fully_inside && HW_OutsideFrustum(raw_cx, raw_cz, 4))
        return local_q;

    fixed_t cz_near = HW_NEAR_Z;
    int n_clip = 0;
    for (int k = 0; k < 4; k++)
    {
        int next = (k + 1 == 4) ? 0 : k + 1;
        fixed_t cza = raw_cz[k],     czb = raw_cz[next];
        fixed_t cxa = raw_cx[k],     cxb = raw_cx[next];

        if (cza >= cz_near)
        {
            clip_cx[n_clip] = cxa;
            clip_cz[n_clip] = cza;
            n_clip++;
            if (czb < cz_near)
            {
                fixed_t t = HW_ClampedFrac((unsigned int)(cza - cz_near), (unsigned int)(cza - czb));
                if (t > FRACUNIT) t = FRACUNIT;

                clip_cx[n_clip] = (int32_t)((uint32_t)cxa + ((uint32_t)FixedMul(t, (cxb >> 1) - (cxa >> 1)) << 1));
                clip_cz[n_clip] = cz_near;
                n_clip++;
            }
        }
        else if (czb >= cz_near)
        {
            fixed_t t = HW_ClampedFrac((unsigned int)(cz_near - cza), (unsigned int)(czb - cza));
            if (t > FRACUNIT) t = FRACUNIT;

            clip_cx[n_clip] = (int32_t)((uint32_t)cxa + ((uint32_t)FixedMul(t, (cxb >> 1) - (cxa >> 1)) << 1));
            clip_cz[n_clip] = cz_near;
            n_clip++;
        }
    }

    if (n_clip < 3 || n_clip > 8)
        return local_q;

    int vw = fctx->vw, vh = fctx->vh;
    int vcx = fctx->vcx, vcy = fctx->vcy;
    int min_x = 9999, max_x = -9999;
    int min_yf = 9999, max_yf = -9999;
    int min_yc = 9999, max_yc = -9999;
    fixed_t min_cz = 0x7FFFFFFF, max_cz = 0;
    for (int k = 0; k < n_clip; k++)
    {
        int s = HW_FastDiv(clip_cz[k]);
        int px = HW_CLAMP_PROJ_WIDE(HW_ProjX(clip_cx[k], s, vcx));
        sx[k] = px;
        s_cache[k] = s;

        if (px < min_x) min_x = px;
        if (px > max_x) max_x = px;
        if (clip_cz[k] < min_cz) min_cz = clip_cz[k];
        if (clip_cz[k] > max_cz) max_cz = clip_cz[k];
    }

    if (max_x <= 0 || min_x >= vw || min_x >= max_x)
        return local_q;

    if (fctx->solid_count > 0 && HW_Slave_IsSpanOccluded(fctx, min_x < 0 ? 0 : min_x, max_x > vw ? vw : max_x, vw))
        return local_q;

    if (fctx->emit_floor)
    {
        fixed_t y_floor_w = fctx->y_floor_w;
        for (int k = 0; k < n_clip; k++)
        {
            int py = HW_CLAMP_PROJ_WIDE(HW_ProjY(y_floor_w, s_cache[k], vcy));
            sy_f[k] = py;
            if (py < min_yf) min_yf = py;
            if (py > max_yf) max_yf = py;
        }
    }
    if (fctx->emit_ceil)
    {
        fixed_t y_ceil_w = fctx->y_ceil_w;
        for (int k = 0; k < n_clip; k++)
        {
            int py = HW_CLAMP_PROJ_WIDE(HW_ProjY(y_ceil_w, s_cache[k], vcy));
            sy_c[k] = py;
            if (py < min_yc) min_yc = py;
            if (py > max_yc) max_yc = py;
        }
    }

    vdp1_cmd_t *cmd_s_out = fctx->cmd_s_out;
    int qcap = fctx->qcap;
    const uint16_t *flat_cmdcolr_lut = fctx->flat_cmdcolr_lut;

    if (fctx->emit_ceil && max_yc > 0 && min_yc < vh)
    {
        unsigned int zb = ((unsigned int)min_cz + (unsigned int)max_cz) >> 21;
        if (__builtin_expect(zb > 127u, 0)) zb = 127u;
        int qcount = local_q;
        HW_Worker_EmitPolyQuads(cmd_s_out, &qcount, qcap, sx, sy_c, n_clip, fctx->ceil_srca, (int)flat_cmdcolr_lut[zb], vw, vh);
        local_q = qcount;
    }

    if (fctx->emit_floor && max_yf > 0 && min_yf < vh)
    {
        unsigned int zb = ((unsigned int)min_cz + (unsigned int)max_cz) >> 21;
        if (__builtin_expect(zb > 127u, 0)) zb = 127u;
        int qcount = local_q;
        HW_Worker_EmitPolyQuads(cmd_s_out, &qcount, qcap, sx, sy_f, n_clip, fctx->floor_srca, (int)flat_cmdcolr_lut[zb], vw, vh);
        local_q = qcount;
    }
    return local_q;
}

/* Cold helper for extreme-distance (>16384 map units) flat vertex transform.
 * Takes 4 register arguments (r4=dx, r5=dy, r6=fctx, r7=p_ptr), 0 stack arguments! */
static HW_NOINLINE HW_COLD_OS void HW_TransformFlatVert_Shifted(
    int dx, int dy, const hw_flat_emit_ctx_t *fctx, saturn_proj_v_t *p_ptr)
{
    fixed_t cz, cx;
    ProjectPillar_2D_Shifted(dx, dy, fctx->vcos, fctx->vsin, fctx->cz_origin, fctx->cx_origin, &cz, &cx);
    p_ptr->cx = cx;
    p_ptr->cz = cz;
}

/* Takes 4 register arguments (r4=is_master, r5=ipc, r6=job, r7=fctx), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 void HW_Worker_ProcessJob(
    int is_master, volatile hw_slave_ipc_t *ipc,
    const volatile hw_flat_job_t *job, hw_flat_emit_ctx_t *fctx)
{
#ifndef SATURN_WIN32
    const volatile aliased_uint32_t *jw = (const volatile aliased_uint32_t *)job;
    const saturn_flat_vert_t * __restrict verts = (const saturn_flat_vert_t *)jw[0];
    const saturn_flat_quad_idx_t * __restrict quads = (const saturn_flat_quad_idx_t *)jw[1];
    uint32_t w2 = jw[2];
    int v_count = (int)(w2 >> 16);
    int q_count = (int)(w2 & 0xFFFFu);
    uint32_t w3 = jw[3];
    int emit_floor   = (int)(w3 >> 24);
    int emit_ceil    = (int)((w3 >> 16) & 0xFFu);
    int fully_inside = (int)((w3 >> 8) & 0xFFu);
    uint32_t sky_flags = w3 & 0xFFu;
    uint32_t w4 = jw[4];
    uint16_t floor_srca = (uint16_t)(w4 >> 16);
    uint16_t ceil_srca  = (uint16_t)(w4 & 0xFFFFu);
    uint32_t w8 = jw[8];
    int sec_light = (int)(int16_t)(w8 >> 16);
    int sub_idx   = (int)(int16_t)(w8 & 0xFFFFu);
#else
    const saturn_flat_vert_t * __restrict verts = job->verts;
    const saturn_flat_quad_idx_t * __restrict quads = job->quads;
    int v_count = (int)job->v_count;
    int q_count = (int)job->q_count;
    int emit_floor   = job->emit_floor;
    int emit_ceil    = job->emit_ceil;
    int fully_inside = job->fully_inside;
    uint32_t sky_flags = job->has_sky;
    uint16_t floor_srca = job->floor_srca;
    uint16_t ceil_srca  = job->ceil_srca;
    int sec_light = (int)job->sec_light;
    int sub_idx   = (int)job->sub_idx;
#endif

#ifndef SATURN_WIN32
    if (!is_master)
    {
        /* Latch frame-constant camera & viewport state ONCE per frame on the first job (bit 1 of sky_flags) */
        if (__builtin_expect((sky_flags & 2u) != 0, 0))
        {
            int gen = ipc->job_done;
            if (__builtin_expect(gen != fctx->cache_gen, 0))
            {
                fctx->cache_gen = gen;
                HW_PURGE_L1_CACHE();
            }
            const fixed_t vx = ipc->viewx;
            const fixed_t vy = ipc->viewy;
            fctx->cam_x16   = (vx >> FRACBITS) * 16;
            fctx->cam_y16   = (vy >> FRACBITS) * 16;
            fctx->vcos      = ipc->viewcos >> 4;
            fctx->vsin      = ipc->viewsin >> 4;
            fctx->cz_origin = ipc->cam_cz_origin;
            fctx->cx_origin = ipc->cam_cx_origin;
            fctx->vw        = (int)ipc->view_w;
            fctx->vh        = (int)ipc->view_h;
            fctx->vcx       = (int)ipc->view_cx;
            fctx->vcy       = (int)ipc->view_cy;
            fctx->cmd_s_out = saturn_cmd_s;
            fctx->proj_v    = hw_proj_verts_slave;
            fctx->qcap      = HW_SLAVE_FLAT_Q_CAP;
            fctx->local_q   = 0;
            fctx->solid_any     = 0;
            fctx->solid_summary = 0;
        }
    }
    else
#endif
    {
        fctx->cam_x16   = hw_cam_x * 16;
        fctx->cam_y16   = hw_cam_y * 16;
        fctx->vcos      = hw_viewcos >> 4;
        fctx->vsin      = hw_viewsin >> 4;
        fctx->cz_origin = cam_cz_origin;
        fctx->cx_origin = cam_cx_origin;
        fctx->vw        = saturn_view_w;
        fctx->vh        = HW_VIEW_H;
        fctx->vcx       = HW_VIEW_CX;
        fctx->vcy       = HW_VIEW_CY;
        fctx->cmd_s_out = saturn_cmd_s;
        fctx->proj_v    = hw_proj_verts_master;
        fctx->qcap      = CMDBUF_S_CAP;
        fctx->local_q   = hw_frame_slave_enabled ? saturn_master_flat_q : saturn_cmd_s_count;
    }

    int start_q = fctx->local_q;
    int local_q = start_q;

    if (sky_flags & 1u)
    {
        if (is_master)
            saturn_hw_sky_visible = 1;
        else
            ipc->sky_visible = 1;
    }

    if (floor_srca == 0 || floor_srca == 0xFFFFu) emit_floor = 0;
    if (ceil_srca == 0 || ceil_srca == 0xFFFFu)   emit_ceil = 0;

    if (!emit_floor && !emit_ceil)
    {
        if ((unsigned)sub_idx < CMDBUF_MAX_VISIT)
        {
            sub_range_t *actual_ranges = saturn_sub_ranges;
            sub_range_t *ranges = (is_master && !hw_frame_slave_enabled)
                ? actual_ranges
                : (sub_range_t *)((uint32_t)actual_ranges | SATURN_WRAM_H_UNCACHED);
            ranges[sub_idx].s_start = (uint16_t)start_q;
            ranges[sub_idx].s_count = 0;
        }
        return;
    }

#ifndef SATURN_WIN32
    fixed_t y_floor_w      = (fixed_t)jw[5];
    const uint8_t *raw_vf  = (const uint8_t *)jw[6];
    fixed_t y_ceil_w       = (fixed_t)jw[7];
    uint32_t w10           = jw[10];
    int solid_count        = (int)(int16_t)(w10 >> 16);
    uint16_t solid_dirty   = (uint16_t)(w10 & 0xFFFFu);
#else
    fixed_t y_floor_w      = job->y_floor_w;
    const uint8_t *raw_vf  = job->vfrac;
    fixed_t y_ceil_w       = job->y_ceil_w;
    int solid_count        = (int)job->solid_count;
    uint16_t solid_dirty   = (uint16_t)job->pad;
#endif

    static const uint8_t hw_zero_vfrac_byte = 0;
    const uint8_t * __restrict vfrac = raw_vf ? raw_vf : &hw_zero_vfrac_byte;
    const int fr_step = raw_vf ? 1 : 0;
    if (sec_light < 0) sec_light = 0; else if (sec_light > 15) sec_light = 15;
    const uint16_t *flat_cmdcolr_lut = flat_zlight_cmdcolr_LUT[sec_light];

    if (v_count > HW_MAX_FLAT_VERTS) v_count = HW_MAX_FLAT_VERTS;

    fctx->flat_cmdcolr_lut = flat_cmdcolr_lut;
    fctx->y_floor_w        = y_floor_w;
    fctx->y_ceil_w         = y_ceil_w;
    fctx->floor_srca       = floor_srca;
    fctx->ceil_srca        = ceil_srca;
    fctx->emit_floor       = (uint8_t)emit_floor;
    fctx->emit_ceil        = (uint8_t)emit_ceil;
    fctx->fully_inside     = (uint8_t)fully_inside;
    fctx->solid_count      = (uint8_t)(solid_count > 255 ? 255 : (solid_count < 0 ? 0 : solid_count));

#ifndef SATURN_WIN32
    if (is_master)
    {
        fctx->solid_any     = solid_dirty;
        fctx->solid_summary = jw[9];
        if (solid_dirty != 0)
        {
            const volatile aliased_uint32_t *js = &jw[11];
            for (int w = 0; w < HW_SOLID_WORDS; w++)
            {
                if (solid_dirty & hw_bit_lut[w])
                    fctx->solid[w] = js[w];
            }
        }
    }
    else if (__builtin_expect(solid_dirty != 0, 0))
    {
        fctx->solid_summary = jw[9];
        fctx->solid_any |= solid_dirty;
        const volatile aliased_uint32_t *js = &jw[11];
        for (int w = 0; w < HW_SOLID_WORDS; w++)
        {
            if (solid_dirty & hw_bit_lut[w])
                fctx->solid[w] = js[w];
        }
    }
#else
    fctx->solid_any     = solid_dirty;
    fctx->solid_summary = job->solid_summary;
    if (solid_dirty != 0)
    {
        const uint32_t *js = (const uint32_t *)job->solid;
        for (int w = 0; w < HW_SOLID_WORDS; w++)
        {
            if (solid_dirty & hw_bit_lut[w])
                fctx->solid[w] = js[w];
        }
    }
#endif

    const int cam_x16      = fctx->cam_x16;
    const int cam_y16      = fctx->cam_y16;
    const fixed_t vcos     = fctx->vcos;
    const fixed_t vsin     = fctx->vsin;
    const fixed_t cz_origin = fctx->cz_origin;
    const fixed_t cx_origin = fctx->cx_origin;
    const int vw           = fctx->vw;
    const int vh           = fctx->vh;
    const int vcx          = fctx->vcx;
    const int vcy          = fctx->vcy;
    saturn_proj_v_t * __restrict proj_v = fctx->proj_v;

    const saturn_flat_vert_t * __restrict v_ptr = verts;
    const uint8_t * __restrict fr_ptr = vfrac;
    saturn_proj_v_t * __restrict p_ptr = proj_v;

    /* Phase 1: Transform & perspective project unique subsector vertices ONCE */
    for (int i = 0; i < v_count; i++, v_ptr++, fr_ptr += fr_step, p_ptr++)
    {
        int qx = (int)v_ptr->x;
        int qy = (int)v_ptr->y;
        int fr = *fr_ptr;
        int dx = (qx * 16) - cam_x16 + ((unsigned)fr >> 4);
        int dy = (qy * 16) - cam_y16 + (fr & 15);

        fixed_t cz, cx;
        if (__builtin_expect((((unsigned int)dx + 262144u) | ((unsigned int)dy + 262144u)) > 524287u, 0))
        {
            HW_TransformFlatVert_Shifted(dx, dy, fctx, p_ptr);
            cx = p_ptr->cx;
            cz = p_ptr->cz;
        }
        else
        {
            ProjectPillar_2D(dx, dy, vcos, vsin, cz_origin, cx_origin, &cz, &cx);
            p_ptr->cx = cx;
            p_ptr->cz = cz;
        }

        if (cz >= HW_NEAR_Z)
        {
            int s = HW_FastDiv(cz);

            if (emit_ceil && emit_floor)
            {
                int px, pyc, pyf;
#ifndef SATURN_WIN32
                int tmp;
                __asm__ volatile(
                    "dmuls.l %4, %5 \n\t"   /* Start M1: cx * s */
                    "mov     %8, %0 \n\t"   /* HIDE STALL 1: px = vcx */
                    "mov     %9, %1 \n\t"   /* HIDE STALL 2: pyc = vcy */
                    "sts     mach, %3 \n\t" /* tmp = M1_hi (0 STALLS) */

                    "dmuls.l %6, %5 \n\t"   /* Start M2: y_ceil_w * s */
                    "add     %3, %0 \n\t"   /* HIDE STALL 1: px = vcx + tmp */
                    "mov     %9, %2 \n\t"   /* HIDE STALL 2: pyf = vcy */
                    "sts     mach, %3 \n\t" /* tmp = M2_hi (0 STALLS) */

                    "dmuls.l %7, %5 \n\t"   /* Start M3: y_floor_w * s */
                    "sub     %3, %1 \n\t"   /* HIDE STALL 1: pyc = vcy - tmp */
                    "sts     mach, %3 \n\t" /* tmp = M3_hi */
                    "sub     %3, %2 \n\t"   /* pyf = vcy - tmp */
                    : "=&r"(px), "=&r"(pyc), "=&r"(pyf), "=&r"(tmp)
                    : "r"(cx), "r"(s), "r"(y_ceil_w), "r"(y_floor_w), "r"(vcx), "r"(vcy)
                    : "mach", "macl"
                );
                int px_cl  = HW_CLAMP_PROJ_WIDE(px);
                int pyc_cl = HW_CLAMP_PROJ_WIDE(pyc);
                int pyf_cl = HW_CLAMP_PROJ_WIDE(pyf);
                aliased_uint32_t *pw = (aliased_uint32_t *)p_ptr;
                pw[2] = ((uint32_t)(uint16_t)px_cl << 16) | (uint16_t)pyc_cl;
                pw[3] = ((uint32_t)(uint16_t)pyf_cl << 16) | 0x0100u;
#else
                px  = vcx + HW_MulHigh(cx, s);
                pyc = vcy - HW_MulHigh(y_ceil_w, s);
                pyf = vcy - HW_MulHigh(y_floor_w, s);
                p_ptr->valid = 1;
                p_ptr->px    = (int16_t)HW_CLAMP_PROJ_WIDE(px);
                p_ptr->py_c  = (int16_t)HW_CLAMP_PROJ_WIDE(pyc);
                p_ptr->py_f  = (int16_t)HW_CLAMP_PROJ_WIDE(pyf);
#endif
            }
            else
            {
                int y_w = emit_ceil ? y_ceil_w : y_floor_w;
                int px, py;
#ifndef SATURN_WIN32
                int tmp;
                __asm__ volatile(
                    "dmuls.l %3, %4 \n\t"   /* Start M1: cx * s */
                    "mov     %6, %0 \n\t"   /* HIDE STALL 1: px = vcx */
                    "mov     %7, %1 \n\t"   /* HIDE STALL 2: py = vcy */
                    "sts     mach, %2 \n\t" /* tmp = M1_hi (0 STALLS) */

                    "dmuls.l %5, %4 \n\t"   /* Start M2: y_w * s */
                    "add     %2, %0 \n\t"   /* HIDE STALL 1: px = vcx + tmp */
                    "sts     mach, %2 \n\t" /* tmp = M2_hi */
                    "sub     %2, %1 \n\t"   /* py = vcy - tmp */
                    : "=&r"(px), "=&r"(py), "=&r"(tmp)
                    : "r"(cx), "r"(s), "r"(y_w), "r"(vcx), "r"(vcy)
                    : "mach", "macl"
                );
                int px_cl = HW_CLAMP_PROJ_WIDE(px);
                int py_cl = HW_CLAMP_PROJ_WIDE(py);
                aliased_uint32_t *pw = (aliased_uint32_t *)p_ptr;
                pw[2] = ((uint32_t)(uint16_t)px_cl << 16) | (uint16_t)py_cl;
                pw[3] = ((uint32_t)(uint16_t)py_cl << 16) | 0x0100u;
#else
                px = vcx + HW_MulHigh(cx, s);
                py = vcy - HW_MulHigh(y_w, s);
                p_ptr->valid = 1;
                p_ptr->px = (int16_t)HW_CLAMP_PROJ_WIDE(px);
                if (emit_ceil)
                    p_ptr->py_c = (int16_t)HW_CLAMP_PROJ_WIDE(py);
                if (emit_floor)
                    p_ptr->py_f = (int16_t)HW_CLAMP_PROJ_WIDE(py);
#endif
            }
        }
        else
        {
#ifndef SATURN_WIN32
            aliased_uint32_t *pw = (aliased_uint32_t *)p_ptr;
            pw[2] = 0;
            pw[3] = 0;
#else
            p_ptr->valid = 0;
            p_ptr->px = 0;
            p_ptr->py_c = 0;
            p_ptr->py_f = 0;
#endif
        }
    }

    const saturn_flat_quad_idx_t * __restrict q_ptr = quads;

    /* Phase 2: Assemble & emit quads using cached projected vertices */
    for (int q = 0; q < q_count; q++, q_ptr++)
    {
        int qv0 = q_ptr->v[0];
        int qv1 = q_ptr->v[1];
        int qv2 = q_ptr->v[2];
        int qv3 = q_ptr->v[3];
        if (__builtin_expect((unsigned)qv0 >= (unsigned)v_count ||
                             (unsigned)qv1 >= (unsigned)v_count ||
                             (unsigned)qv2 >= (unsigned)v_count ||
                             (unsigned)qv3 >= (unsigned)v_count, 0))
            continue;
        const saturn_proj_v_t *pv0 = &proj_v[qv0];
        const saturn_proj_v_t *pv1 = &proj_v[qv1];
        const saturn_proj_v_t *pv2 = &proj_v[qv2];
        const saturn_proj_v_t *pv3 = &proj_v[qv3];

        uint8_t v0_val = pv0->valid;
        uint8_t v1_val = pv1->valid;
        uint8_t v2_val = pv2->valid;
        uint8_t v3_val = pv3->valid;

        if (__builtin_expect(v0_val & v1_val & v2_val & v3_val, 1))
        {
            if (pv0->cz > HW_FAR_Z && pv1->cz > HW_FAR_Z && pv2->cz > HW_FAR_Z && pv3->cz > HW_FAR_Z)
                continue;

            int px0 = pv0->px, px1 = pv1->px, px2 = pv2->px, px3 = pv3->px;

            /* Fast single-branch horizontal screen-edge rejection */
            int m_neg_x = px0 & px1 & px2 & px3;
            int pos_x   = (px0 - vw) | (px1 - vw) | (px2 - vw) | (px3 - vw);
            if ((m_neg_x | ~pos_x) < 0)
                continue;

            int min_x = px0; if (px1 < min_x) min_x = px1; if (px2 < min_x) min_x = px2; if (px3 < min_x) min_x = px3;
            int max_x = px0; if (px1 > max_x) max_x = px1; if (px2 > max_x) max_x = px2; if (px3 > max_x) max_x = px3;

            if (max_x <= 0 || min_x >= vw || min_x >= max_x)
                continue;

            int cl_min_x = (min_x < 0) ? 0 : min_x;
            int cl_max_x = (max_x > vw) ? vw : max_x;
            if (solid_count > 0 && HW_Slave_IsSpanOccluded(fctx, cl_min_x, cl_max_x, vw))
                continue;

            int dx20 = px2 - px0;
            int dx13 = px1 - px3;

            if (dx20 == 0 && dx13 == 0)
                continue;

            int cw_c = 0, cw_f = 0;
            if (emit_ceil)
            {
                int py0_c = pv0->py_c, py1_c = pv1->py_c, py2_c = pv2->py_c, py3_c = pv3->py_c;
                if ((py0_c > 0 || py1_c > 0 || py2_c > 0 || py3_c > 0) &&
                    (py0_c < vh || py1_c < vh || py2_c < vh || py3_c < vh))
                {
                    cw_c = HW_MulsW_PairAdd(dx20, py3_c - py1_c, dx13, py2_c - py0_c, 0);
                }
            }
            if (emit_floor)
            {
                int py0_f = pv0->py_f, py1_f = pv1->py_f, py2_f = pv2->py_f, py3_f = pv3->py_f;
                if ((py0_f < vh || py1_f < vh || py2_f < vh || py3_f < vh) &&
                    (py0_f > 0 || py1_f > 0 || py2_f > 0 || py3_f > 0))
                {
                    cw_f = HW_MulsW_PairAdd(dx20, py3_f - py1_f, dx13, py2_f - py0_f, 0);
                }
            }

            if ((cw_c | cw_f) == 0)
                continue;

            int wy0 = verts[qv0].y, wy1 = verts[qv1].y;
            int wy2 = verts[qv2].y, wy3 = verts[qv3].y;
            int y_bot = (wy0 + wy1) >> 1;
            int y_top = (wy2 + wy3) >> 1;
            int quad_v0 = 0, quad_rows = 64;
            if (wy2 >= wy1 && wy3 >= wy0 && y_top > y_bot)
            {
                int dy = y_top - y_bot;
                if (dy <= 64)
                {
                    int vn = (-y_top) & 63;
                    if (vn + dy <= 64)
                    {
                        quad_v0 = vn;
                        quad_rows = dy;
                    }
                }
            }
            uint32_t qpack_base = ((uint32_t)quad_v0 << 24) | ((uint32_t)quad_rows << 16);

            if (cw_c != 0)
            {
                local_q = HW_EmitFlatBands(fctx, q_ptr, cw_c, qpack_base | 0x80000000u | (uint32_t)local_q);
            }
            if (cw_f != 0)
            {
                local_q = HW_EmitFlatBands(fctx, q_ptr, cw_f, qpack_base | (uint32_t)local_q);
            }
            continue;
        }

        if (!(v0_val | v1_val | v2_val | v3_val))
            continue;

        local_q = HW_Worker_ClipNearQuad(fctx, q_ptr, local_q);
    }

    fctx->local_q = local_q;
    if (is_master)
    {
        if (hw_frame_slave_enabled)
            saturn_master_flat_q = local_q;
        else
            saturn_cmd_s_count = local_q;
    }

    int f_cnt = local_q - start_q;
    if ((unsigned)sub_idx < CMDBUF_MAX_VISIT)
    {
        sub_range_t *actual_ranges = saturn_sub_ranges;
        sub_range_t *ranges = (is_master && !hw_frame_slave_enabled)
            ? actual_ranges
            : (sub_range_t *)((uint32_t)actual_ranges | SATURN_WRAM_H_UNCACHED);
        ranges[sub_idx].s_start = (uint16_t)start_q;
        ranges[sub_idx].s_count = (uint16_t)f_cnt;
#ifndef SATURN_WIN32
        if (!is_master)
        {
            uint16_t dummy_sdram = *(volatile uint16_t *)&ranges[sub_idx].s_count;
            (void)dummy_sdram;
            __asm__ volatile("" : : "r"(dummy_sdram) : "memory");
        }
#endif
    }
}

HW_ALWAYS_INLINE void HW_Master_ProcessJob(volatile hw_slave_ipc_t *ipc, const hw_flat_job_t *job)
{
    HW_Worker_ProcessJob(1, ipc, job, &hw_fctx_master);
}

#ifndef SATURN_WIN32
static HW_NOINLINE HW_HOT_O2 void HW_Slave_FlatWorker_Loop(void)
{
    extern volatile unsigned int saturn_slave_execs;
    saturn_slave_execs++;

    /* Ensure Slave SH-2 L1 Cache is purged & enabled via uncached Area 1 trampoline (0x11 = 0x10 CP | 0x01 CE) */
    HW_PURGE_L1_CACHE();

    volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;
    unsigned int head = 0;

    /* Allocate Slave emit context directly on the dedicated 16 KB Low WRAM stack (0x0021BFF0)
     * so all fctx reads/writes hit Low WRAM L1 cache with zero High WRAM bus traffic! */
    hw_flat_emit_ctx_t slave_fctx __attribute__((aligned(16)));
    slave_fctx.cache_gen = ipc->job_done;
    slave_fctx.local_q   = 0;

    while (1)
    {
        /* Pure lockless SPSC spin-wait with 64-cycle in-register bus yield */
        while ((int)(head - ipc->job_tail) >= 0)
        {
            HW_BUS_YIELD();
        }
        __asm__ volatile("" ::: "memory");

        const volatile hw_flat_job_t *uncached_job = &ipc->jobs[head & (HW_JOB_RING - 1)];
        HW_Worker_ProcessJob(0, ipc, uncached_job, &slave_fctx);
        head++;
        __asm__ volatile("" ::: "memory");
        if ((int)(head - ipc->job_tail) >= 0)
            ipc->quad_count = slave_fctx.local_q;
        ipc->job_head = head;
        uint32_t dummy_done = ipc->job_head;
        (void)dummy_done;
        __asm__ volatile("" : : "r"(dummy_done) : "memory");
    }
}
#endif

void HW_Slave_FlatWorker(void)
{
#ifndef SATURN_WIN32
    /* Switch Slave SH-2 stack pointer (r15) from SGL's tiny default 0x06000FF0 stack
     * (which sits only ~1 KB above the BIOS/SGL SCU Interrupt Callback & Mask tables
     * at 0x06000A00..0x06000AFF) to our dedicated 16 KB stack in WRAM-L (0x0021BFF0). */
    __asm__ volatile(
        "mov   %0, r15 \n\t"
        "jmp   @%1     \n\t"
        "nop           \n\t"
        :
        : "r"(SATURN_WRAM_L_SLAVE_STACK_TOP), "r"(HW_Slave_FlatWorker_Loop)
        : "memory"
    );
    __builtin_unreachable();
#endif
}

static HW_NOINLINE HW_HOT_O2 void HW_SelectFlatLOD(int subnum, int *io_v_first, int *io_v_count, int *io_q_first, int *io_q_count)
{
    if (saturn_baked_flat_lod1_q_count && saturn_baked_flat_lod1_v_count &&
        saturn_baked_flat_lod1_q_first && saturn_baked_flat_lod1_v_first)
    {
        int l_qcount = (int)saturn_baked_flat_lod1_q_count[subnum];
        int l_vcount = (int)saturn_baked_flat_lod1_v_count[subnum];
        if (l_qcount > 0 && l_vcount > 0)
        {
            int cx = 0, cy = 0;
            int min_dist = (384 << 12);
            if (__builtin_expect(saturn_baked_flat_bboxes != NULL, 1))
            {
                const saturn_flat_bbox_t *bbox = &saturn_baked_flat_bboxes[subnum];
                cx = (hw_bbox_nidx_Nx == BOXLEFT) ? bbox->min_x : bbox->max_x;
                cy = (hw_bbox_nidx_Ny == BOXBOTTOM) ? bbox->min_y : bbox->max_y;
                if ((bbox->max_x - bbox->min_x) > 128 || (bbox->max_y - bbox->min_y) > 128)
                    min_dist = (512 << 12);
            }
            int dx = cx - hw_cam_x;
            int dy = cy - hw_cam_y;
            if (dx > 16384) dx = 16384; else if (dx < -16384) dx = -16384;
            if (dy > 16384) dy = 16384; else if (dy < -16384) dy = -16384;
            if (HW_MulsW_PairAdd(dx, hw_bbox_Nx, dy, hw_bbox_Ny, 0) >= min_dist)
            {
                if (*io_q_count > l_qcount)
                    saturn_hw_flat_lod_saved_quads += (unsigned int)(*io_q_count - l_qcount);
                *io_q_first = (int)saturn_baked_flat_lod1_q_first[subnum];
                *io_q_count = l_qcount;
                *io_v_first = (int)saturn_baked_flat_lod1_v_first[subnum];
                *io_v_count = l_vcount;
                saturn_hw_flat_lod1_subs++;
                return;
            }
        }
    }
    *io_v_first = (int)saturn_baked_flat_v_first[subnum];
    *io_q_first = (int)saturn_baked_flat_q_first[subnum];
    saturn_hw_flat_lod0_subs++;
}

static const sector_t *hw_last_flat_sec = NULL;
static uint16_t hw_last_floor_srca = 0;
static uint16_t hw_last_ceil_srca = 0;
static uint8_t  hw_last_emit_floor = 0;
static uint8_t  hw_last_emit_ceil = 0;
static fixed_t  hw_last_y_floor_w = 0;
static fixed_t  hw_last_y_ceil_w = 0;

static void HW_RenderSubsectorBakedFlats(int sub_idx, int subnum, sector_t *sec, int fully_inside);

/* Takes 4 register arguments (r4=sub_idx, r5=subnum, r6=sec, r7=fully_inside), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 void HW_SubmitSlaveFlatJob(int sub_idx, int subnum, sector_t *sec, int fully_inside)
{
#ifndef SATURN_WIN32
    if (!saturn_baked_flat_q_count)
        return;
    int q_count = (int)saturn_baked_flat_q_count[subnum];
    if (q_count <= 0)
        return;
    int v_count = (int)saturn_baked_flat_v_count[subnum];
    if (v_count <= 0)
        return;

    int emit_floor, emit_ceil;
    int floor_srca, ceil_srca;
    fixed_t y_floor_w, y_ceil_w;

    if (sec == hw_last_flat_sec)
    {
        emit_floor = hw_last_emit_floor & ((fully_inside >> 2) & 1);
        emit_ceil  = hw_last_emit_ceil  & ((fully_inside >> 3) & 1);
        fully_inside &= 3;

        if (!emit_ceil && !emit_floor)
            return;
        floor_srca = hw_last_floor_srca;
        ceil_srca  = hw_last_ceil_srca;
        y_floor_w  = hw_last_y_floor_w;
        y_ceil_w   = hw_last_y_ceil_w;
    }
    else
    {
        const fixed_t vz = hw_viewz;
        emit_ceil  = (vz < sec->ceilingheight && ((int16_t)sec->ceilingpic != -1));
        emit_floor = (vz > sec->floorheight && ((int16_t)sec->floorpic != -1));
        if (saturn_vdp2_plane_active && sec == &sectors[saturn_vdp2_plane_sec])
            emit_floor = 0;

        floor_srca = 0;
        ceil_srca  = 0;
        if (emit_floor)
        {
            floor_srca = HW_FastFlatSrca(sec->floorpic);
            if (!floor_srca) emit_floor = 0;
        }
        if (emit_ceil)
        {
            ceil_srca = HW_FastFlatSrca(sec->ceilingpic);
            if (!ceil_srca) emit_ceil = 0;
        }
        y_floor_w = HW_DZQ(sec->floorheight, vz);
        y_ceil_w  = HW_DZQ(sec->ceilingheight, vz);

        hw_last_flat_sec   = sec;
        hw_last_emit_floor = (uint8_t)emit_floor;
        hw_last_emit_ceil  = (uint8_t)emit_ceil;
        hw_last_floor_srca = (uint16_t)floor_srca;
        hw_last_ceil_srca  = (uint16_t)ceil_srca;
        hw_last_y_floor_w  = y_floor_w;
        hw_last_y_ceil_w   = y_ceil_w;
        emit_floor &= (fully_inside >> 2) & 1;
        emit_ceil  &= (fully_inside >> 3) & 1;
        fully_inside &= 3;

        if (!emit_ceil && !emit_floor)
            return;
    }

    volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

    int v_first = 0;
    int q_first = 0;
    HW_SelectFlatLOD(subnum, &v_first, &v_count, &q_first, &q_count);

    const saturn_flat_vert_t *verts = &saturn_baked_flat_verts[v_first];
    const saturn_flat_quad_idx_t *quads = &saturn_baked_flat_quads[q_first];
    const uint8_t *vfrac = saturn_baked_flat_vfrac ? (saturn_baked_flat_vfrac + v_first) : NULL;
    uint8_t sky_flags = (uint8_t)((int16_t)sec->ceilingpic == -1);
    int sc = hw_solid_count;
    int fjob_idx = ++hw_frame_flat_jobs;

    if (__builtin_expect(!saturn_hw_sky_visible && fjob_idx > 36 &&
                         ((fjob_idx & 1) == 0) &&
                         hw_def_flat_count < HW_MAX_DEF_FLAT_JOBS &&
                         saturn_master_flat_q + (q_count << 1) + 8 < CMDBUF_S_CAP, 0))
    {
        aliased_uint32_t *mw = (aliased_uint32_t *)&hw_def_flat_jobs[hw_def_flat_count++];
        uint32_t s_any = hw_solid_any;
        mw[0]  = (uint32_t)verts;
        mw[1]  = (uint32_t)quads;
        mw[2]  = ((uint32_t)(uint16_t)v_count << 16) | (uint16_t)q_count;
        mw[3]  = ((uint32_t)(uint8_t)emit_floor << 24) |
                 ((uint32_t)(uint8_t)emit_ceil << 16) |
                 ((uint32_t)(uint8_t)fully_inside << 8) |
                 sky_flags;
        mw[4]  = ((uint32_t)(uint16_t)floor_srca << 16) | (uint16_t)ceil_srca;
        mw[5]  = (uint32_t)y_floor_w;
        mw[6]  = (uint32_t)vfrac;
        mw[7]  = (uint32_t)y_ceil_w;
        mw[8]  = ((uint32_t)(uint16_t)(int16_t)(sec->lightlevel >> 4) << 16) | (uint16_t)(int16_t)sub_idx;
        mw[9]  = hw_solid_summary;
        mw[10] = ((uint32_t)(uint16_t)(int16_t)sc << 16) | (uint16_t)s_any;
        if (s_any != 0)
        {
            const uint32_t *sm = hw_solid_mask;
            aliased_uint32_t *ms = &mw[11];
            for (int w = 0; w < HW_SOLID_WORDS; w++)
            {
                if (s_any & hw_bit_lut[w])
                    ms[w] = sm[w];
            }
        }
        return;
    }

    unsigned int tail = hw_master_job_tail;
    hw_master_job_tail = tail + 1;

    /* Write directly to uncached IPC ring so Master L1 cache is never polluted by write-only IPC jobs */
    volatile aliased_uint32_t *jw = (volatile aliased_uint32_t *)&ipc->jobs[tail & (HW_JOB_RING - 1)];
    jw[0] = (uint32_t)verts;
    jw[1] = (uint32_t)quads;
    jw[2] = ((uint32_t)(uint16_t)v_count << 16) | (uint16_t)q_count;
    uint32_t dirty = hw_slave_dirty_words;
    if (__builtin_expect(hw_first_flat_job, 0))
    {
        hw_first_flat_job = 0;
        sky_flags |= 2u;
        dirty = hw_solid_any;
    }
    hw_slave_dirty_words = 0;
    jw[3] = ((uint32_t)(uint8_t)emit_floor << 24) |
            ((uint32_t)(uint8_t)emit_ceil << 16) |
            ((uint32_t)(uint8_t)fully_inside << 8) |
            sky_flags;
    jw[4] = ((uint32_t)(uint16_t)floor_srca << 16) | (uint16_t)ceil_srca;
    jw[5] = (uint32_t)y_floor_w;
    jw[6] = (uint32_t)vfrac;
    jw[7] = (uint32_t)y_ceil_w;
    jw[8] = ((uint32_t)(uint16_t)(int16_t)(sec->lightlevel >> 4) << 16) | (uint16_t)(int16_t)sub_idx;
    jw[10] = ((uint32_t)(uint16_t)(int16_t)sc << 16) | (uint16_t)dirty;

    if (__builtin_expect(dirty != 0, 0))
    {
        jw[9] = hw_solid_summary;
        const uint32_t *sm = hw_solid_mask;
        volatile aliased_uint32_t *js = &jw[11];
        for (int w = 0; w < HW_SOLID_WORDS; w++)
        {
            if (dirty & hw_bit_lut[w])
                js[w] = sm[w];
        }
    }

    /* Uncached stores issue strictly in FIFO program order on the SH-2 external bus controller */
    __asm__ volatile("" ::: "memory");
    ipc->job_tail = tail + 1;
#else
    (void)sub_idx; (void)subnum; (void)sec; (void)fully_inside;
#endif
}

static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsectorBakedFlats(int sub_idx, int subnum, sector_t *sec, int fully_inside)
{
    if (!saturn_baked_flat_q_count || saturn_baked_flat_q_count[subnum] <= 0)
        return;
    int v_count = (int)saturn_baked_flat_v_count[subnum];
    if (v_count <= 0)
        return;

    const fixed_t vz = hw_viewz;
    int emit_ceil  = (vz < sec->ceilingheight && ((int16_t)sec->ceilingpic != -1));
    int emit_floor = (vz > sec->floorheight && ((int16_t)sec->floorpic != -1));
    if (saturn_vdp2_plane_active && sec == &sectors[saturn_vdp2_plane_sec]) emit_floor = 0;
    emit_floor &= (fully_inside >> 2) & 1;
    emit_ceil  &= (fully_inside >> 3) & 1;
    fully_inside &= 3;

    if (!emit_ceil && !emit_floor)
        return;

    int floor_srca = 0, ceil_srca = 0;
    if (emit_floor) { floor_srca = HW_FastFlatSrca(sec->floorpic); if (!floor_srca) emit_floor = 0; }
    if (emit_ceil)  { ceil_srca = HW_FastFlatSrca(sec->ceilingpic); if (!ceil_srca) emit_ceil = 0; }
    if (!emit_ceil && !emit_floor) return;

    int v_first = 0;
    int q_first = 0;
    int q_count = (int)saturn_baked_flat_q_count[subnum];
    HW_SelectFlatLOD(subnum, &v_first, &v_count, &q_first, &q_count);

    hw_flat_job_t job __attribute__((aligned(16)));
    job.verts = &saturn_baked_flat_verts[v_first];
    job.quads = &saturn_baked_flat_quads[q_first];
    job.v_count = (uint16_t)v_count;
    job.q_count = (uint16_t)q_count;
    job.emit_floor = (uint8_t)emit_floor;
    job.emit_ceil = (uint8_t)emit_ceil;
    job.fully_inside = (uint8_t)fully_inside;
    job.has_sky = ((int16_t)sec->ceilingpic == -1);
    job.floor_srca = (uint16_t)floor_srca;
    job.ceil_srca = (uint16_t)ceil_srca;
    job.y_floor_w = HW_DZQ(sec->floorheight, vz);
    job.vfrac = saturn_baked_flat_vfrac ? (saturn_baked_flat_vfrac + v_first) : NULL;
    job.y_ceil_w = HW_DZQ(sec->ceilingheight, vz);
    job.sec_light = (int16_t)(sec->lightlevel >> 4);
    job.sub_idx = (int16_t)sub_idx;
    job.solid_count = (int16_t)hw_solid_count;
    job.solid_summary = hw_solid_summary;
    job.pad = (int16_t)(uint16_t)hw_solid_any;
    for (int w = 0; w < HW_SOLID_WORDS; w++) job.solid[w] = hw_solid_mask[w];
#if HW_JOB_PAD_WORDS > 0
    for (int p = 0; p < HW_JOB_PAD_WORDS; p++) job.align_pad[p] = 0;
#endif

    volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;
    HW_Master_ProcessJob(ipc, &job);
}

#define BBOX_OUTSIDE      0
#define BBOX_INTERSECT    1
#define BBOX_FULLY_INSIDE 2

/* Fast 2D Map-Space AABB Frustum Test with Full-Accept Detection.
 * Evaluates exact bounding box boundaries against 2D frustum planes in map space.
 * Returns BBOX_OUTSIDE (0) if guaranteed outside view frustum (culled).
 * Returns BBOX_INTERSECT (1) if intersects or viewer inside.
 * Returns BBOX_FULLY_INSIDE (2) if 100% of bounding box is inside view frustum. */
static HW_NOINLINE HW_HOT_O2 int HW_CheckBBoxStatus(const int16_t *bbox)
{
    /* Pull the box entirely into fast CPU registers instantly */
    int bl = bbox[BOXLEFT];
    int br = bbox[BOXRIGHT];
    int bb = bbox[BOXBOTTOM];
    int bt = bbox[BOXTOP];

    if (bl > br || bb > bt)
        return BBOX_INTERSECT;

    int vx = hw_cam_x;
    int vy = hw_cam_y;

    if (vx >= bl && vx <= br && vy >= bb && vy <= bt)
        return BBOX_INTERSECT;

    /* 1. Near Plane FIRST: Instantly eliminates the entire half of the map behind the player */
    int bxN = bbox[hw_bbox_idx_Nx];
    int byN = bbox[hw_bbox_idx_Ny];
    if (HW_MulsW_PairAdd(bxN, hw_bbox_Nx, byN, hw_bbox_Ny, hw_bbox_Cn) < (HW_NEAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;

    /* 2. Left & Right Frustum Planes */
    int bxL = bbox[hw_bbox_idx_Lx];
    int byL = bbox[hw_bbox_idx_Ly];
    if (HW_MulsW_PairAdd(bxL, hw_bbox_Lx, byL, hw_bbox_Ly, hw_bbox_Cl) < -(64 << 12))
        return BBOX_OUTSIDE;

    int bxR = bbox[hw_bbox_idx_Rx];
    int byR = bbox[hw_bbox_idx_Ry];
    if (HW_MulsW_PairAdd(bxR, hw_bbox_Rx, byR, hw_bbox_Ry, hw_bbox_Cr) < -(64 << 12))
        return BBOX_OUTSIDE;

    /* Far-plane test: evaluated after lateral side planes to cull peripheral nodes first */
    int fxN = bbox[hw_bbox_nidx_Nx];
    int fyN = bbox[hw_bbox_nidx_Ny];
    int32_t d_near_min = HW_MulsW_PairAdd(fxN, hw_bbox_Nx, fyN, hw_bbox_Ny, hw_bbox_Cn);
    if (d_near_min > (HW_FAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;

    int n_bxL = bbox[hw_bbox_nidx_Lx];
    int n_byL = bbox[hw_bbox_nidx_Ly];
    int n_bxR = bbox[hw_bbox_nidx_Rx];
    int n_byR = bbox[hw_bbox_nidx_Ry];

    if (d_near_min >= (16 << 12) &&
        HW_MulsW_PairAdd(n_bxL, hw_bbox_Lx, n_byL, hw_bbox_Ly, hw_bbox_Cl) >= 0 &&
        HW_MulsW_PairAdd(n_bxR, hw_bbox_Rx, n_byR, hw_bbox_Ry, hw_bbox_Cr) >= 0)
    {
        return BBOX_FULLY_INSIDE;
    }

    return BBOX_INTERSECT;
}

HW_ALWAYS_INLINE int HW_CheckBBox(const int16_t *bbox)
{
    return (HW_CheckBBoxStatus(bbox) == BBOX_OUTSIDE);
}

static HW_NOINLINE HW_COLD_OS int HW_ProjectBBoxShift(
    int dx1, int dy1, int dx2, int dy2, fixed_t *out_cz1, fixed_t *out_cx1, fixed_t *out_cz2, fixed_t *out_cx2)
{
    uint32_t b_chk2 = (uint32_t)(dx1 + 32768) | (uint32_t)(dy1 + 32768) |
                      (uint32_t)(dx2 + 32768) | (uint32_t)(dy2 + 32768);
    int shift = (b_chk2 > 65535u) ? 2 : 1;
    fixed_t cz_orig = cam_cz_origin >> shift;
    fixed_t cx_orig = cam_cx_origin >> shift;
    fixed_t limit = 0x7FFFFFFF >> shift;
    fixed_t cz1, cx1, cz2, cx2;

    ProjectPillar_2D(dx1 >> shift, dy1 >> shift, hw_viewcos, hw_viewsin, cz_orig, cx_orig, &cz1, &cx1);
    if (cz1 > limit) cz1 = limit; else if (cz1 < -limit) cz1 = -limit;
    if (cx1 > limit) cx1 = limit; else if (cx1 < -limit) cx1 = -limit;
    cz1 = (fixed_t)((uint32_t)cz1 << shift); cx1 = (fixed_t)((uint32_t)cx1 << shift);
    if (cz1 < HW_NEAR_Z)
        return 0;

    ProjectPillar_2D(dx2 >> shift, dy2 >> shift, hw_viewcos, hw_viewsin, cz_orig, cx_orig, &cz2, &cx2);
    if (cz2 > limit) cz2 = limit; else if (cz2 < -limit) cz2 = -limit;
    if (cx2 > limit) cx2 = limit; else if (cx2 < -limit) cx2 = -limit;
    cz2 = (fixed_t)((uint32_t)cz2 << shift); cx2 = (fixed_t)((uint32_t)cx2 << shift);
    if (cz2 < HW_NEAR_Z)
        return 0;

    *out_cz1 = cz1; *out_cx1 = cx1;
    *out_cz2 = cz2; *out_cx2 = cx2;
    return 1;
}

/* 1D Solid Wall Bounding Box Screen Occlusion Test:
 * Projects all 4 corners of a 2D bounding box to screen X coordinates [min_x, max_x].
 * If any corner crosses the near-plane, the box intersects or is behind the view screen
 * and cannot safely be culled, returning 0 immediately.
 * If every column in [min_x, max_x] is covered by solid 1-sided walls, the entire
 * box (and all geometry/subsectors/nodes inside it) is 100% occluded and skipped.
 * Returns 1 if 100% occluded, 0 if potentially visible. */
static HW_NOINLINE HW_HOT_O2 int HW_CheckBBoxOccluded(const int16_t *bbox)
{
    if (!hw_solid_count) return 0;
    if (hw_solid_count >= saturn_view_w) return 1;

    int bl = bbox[BOXLEFT];
    int br = bbox[BOXRIGHT];
    int bb = bbox[BOXBOTTOM];
    int bt = bbox[BOXTOP];

    /* Fast inside-box check: if camera is inside bounding box, cannot cull */
    if (hw_cam_x >= bl && hw_cam_x <= br && hw_cam_y >= bb && hw_cam_y <= bt)
        return 0;

    /* Ensure the closest corner doesn't pierce the near plane (5-cycle 16x16 MAC) */
    if (HW_MulsW_PairAdd(bbox[hw_bbox_nidx_Nx], hw_bbox_Nx,
                         bbox[hw_bbox_nidx_Ny], hw_bbox_Ny, hw_bbox_Cn) < (HW_NEAR_Z_UNITS << 12))
        return 0;

    int boxpos;
    if (hw_cam_x <= bl)
        boxpos = 0;
    else if (hw_cam_x < br)
        boxpos = 1;
    else
        boxpos = 2;

    if (hw_cam_y >= bt)
        boxpos |= 0;
    else if (hw_cam_y > bb)
        boxpos |= 4;
    else
        boxpos |= 8;

    /* Two-corner silhouette lookup table (Carmack R_CheckBBox) */
    static const int8_t checkcoord[12][4] = {
        { BOXRIGHT, BOXTOP,    BOXLEFT,  BOXBOTTOM }, /* 0: top-left */
        { BOXRIGHT, BOXTOP,    BOXLEFT,  BOXTOP },    /* 1: top-center */
        { BOXRIGHT, BOXBOTTOM, BOXLEFT,  BOXTOP },    /* 2: top-right */
        { 0, 0, 0, 0 },
        { BOXLEFT,  BOXTOP,    BOXLEFT,  BOXBOTTOM }, /* 4: mid-left */
        { 0, 0, 0, 0 },                               /* 5: inside */
        { BOXRIGHT, BOXBOTTOM, BOXRIGHT, BOXTOP },    /* 6: mid-right */
        { 0, 0, 0, 0 },
        { BOXLEFT,  BOXTOP,    BOXRIGHT, BOXBOTTOM }, /* 8: bot-left */
        { BOXLEFT,  BOXBOTTOM, BOXRIGHT, BOXBOTTOM }, /* 9: bot-center */
        { BOXLEFT,  BOXBOTTOM, BOXRIGHT, BOXTOP },    /* 10: bot-right */
        { 0, 0, 0, 0 }
    };

    const int8_t *cc = checkcoord[boxpos];

    /* Project only the 2 extreme silhouette corners instead of 4 */
    int dx1 = bbox[cc[0]] - hw_cam_x;
    int dy1 = bbox[cc[1]] - hw_cam_y;
    int dx2 = bbox[cc[2]] - hw_cam_x;
    int dy2 = bbox[cc[3]] - hw_cam_y;
    fixed_t cz1, cx1, cz2, cx2;

    /* Branchless Bounds Checking: Replaces 8 lazy-eval branches with 1 (0x7FFF = 32767u is bitmask-closed under OR) */
    uint32_t b_chk = (uint32_t)(dx1 + 16384) | (uint32_t)(dy1 + 16384) |
                     (uint32_t)(dx2 + 16384) | (uint32_t)(dy2 + 16384);
    if (__builtin_expect(b_chk <= 32767u, 1))
    {
        ProjectPillar_2D(dx1, dy1, hw_viewcos, hw_viewsin, cam_cz_origin, cam_cx_origin, &cz1, &cx1);
        if (cz1 < HW_NEAR_Z)
            return 0;
        ProjectPillar_2D(dx2, dy2, hw_viewcos, hw_viewsin, cam_cz_origin, cam_cx_origin, &cz2, &cx2);
        if (cz2 < HW_NEAR_Z)
            return 0;
    }
    else
    {
        if (!HW_ProjectBBoxShift(dx1, dy1, dx2, dy2, &cz1, &cx1, &cz2, &cx2))
            return 0;
    }

    int s1 = HW_FastDiv(cz1);
    int px1 = HW_ProjX(cx1, s1, HW_VIEW_CX);

    int s2 = HW_FastDiv(cz2);
    int px2 = HW_ProjX(cx2, s2, HW_VIEW_CX);

    int min_x, max_x;
    if (px1 < px2) { min_x = px1; max_x = px2; }
    else           { min_x = px2; max_x = px1; }

    /* Off-screen span cannot be proven occluded by on-screen solid walls */
    if (max_x <= 0 || min_x >= saturn_view_w) return 0;

    min_x -= 1;
    max_x += 1;
    if (min_x < 0) min_x = 0;
    if (max_x > saturn_view_w) max_x = saturn_view_w;
    int occl = HW_IsSpanOccluded(min_x, max_x);
    return occl;
}

/* Precomputed inverses for du_vis in [1..16] (replaces runtime divisions with 1-cycle multiply-shift) */
static const int32_t saturn_inv_du_vis_lut[17] = {
    0, 65536, 32768, 21845, 16384, 13107, 10923, 9362,
    8192, 7282, 6554, 5958, 5461, 5041, 4681, 4369, 4096
};

/* 1D Vertical Strip Subdivision Emitter (Section 36 / Ezra Dreisbach Architecture)
 * Slices oblique/wide walls into 16-pixel vertical strips.
 * Uses 3D perspective division per strip on close oblique walls (persp_strips=1) and
 * 0-multiply 2D linear screen-space stepping on distant/narrow/perpendicular walls (persp_strips=0). */
/* Cold helper for clipped partial 16-texel strips (du_vis < 16).
 * Takes 4 register arguments (r4=s, r5=u_lo, r6=u_hi, r7=c[9]), 0 stack arguments! */
static HW_NOINLINE HW_COLD_OS int HW_ExtrapolatePartialWallStrip(
    int s, int u_lo, int u_hi, int *c)
{
    int s_px0 = c[0], s_px1 = c[1];
    int s_ya0 = c[2], s_yb0 = c[3];
    int s_ya1 = c[4], s_yb1 = c[5];
    int dx_vis = s_px1 - s_px0;
    if (dx_vis > 2)
    {
        int cl_x0 = (s_px0 < 0) ? 0 : (s_px0 > (saturn_view_w - 1) ? (saturn_view_w - 1) : s_px0);
        int cl_x1 = (s_px1 < 0) ? 0 : (s_px1 > (saturn_view_w - 1) ? (saturn_view_w - 1) : s_px1);
        if (cl_x0 >= cl_x1)
            return 0;

        int du_vis = u_hi - u_lo;
        int du0 = u_lo - (s << 4);
        int du1 = ((s + 1) << 4) - u_hi;
        int dya0 = s_yb0 - s_ya0;
        int dya1 = s_yb1 - s_ya1;
        int inv_du = (du_vis > 0 && du_vis <= 16) ? saturn_inv_du_vis_lut[du_vis] : 4096;

        int s_q_xa = s_px0, s_q_xb = s_px1;
        int s_q_ya0 = s_ya0, s_q_yb0 = s_yb0;
        int s_q_ya1 = s_ya1, s_q_yb1 = s_yb1;

        if (du0 != 0)
        {
            int frac0 = du0 * inv_du;
            int mya0, mya1;
            s_q_xa  -= HW_FixedMul_SH2(dx_vis, frac0);
            HW_FixedMulPair(dya0, frac0, dya1, frac0, &mya0, &mya1);
            s_q_ya0 -= mya0;
            s_q_ya1 -= mya1;
        }
        if (du1 != 0)
        {
            int frac1 = du1 * inv_du;
            int myb0, myb1;
            s_q_xb  += HW_FixedMul_SH2(dx_vis, frac1);
            HW_FixedMulPair(dya0, frac1, dya1, frac1, &myb0, &myb1);
            s_q_yb0 += myb0;
            s_q_yb1 += myb1;
        }

        c[0] = s_q_xa;  c[1] = s_q_xb;
        c[2] = s_q_ya0; c[3] = s_q_yb0;
        c[4] = s_q_ya1; c[5] = s_q_yb1;

        if ((s_q_xa < cl_x0 && cl_x0 > 0) || (s_q_xb > cl_x1 && cl_x1 < saturn_view_w - 1))
        {
            c[6] = 1;
            if (cl_x0 != c[7] || cl_x1 != c[8])
            {
                HW_EmitUserClip(cl_x0, 0, cl_x1, HW_VIEW_H - 1);
                c[7] = cl_x0;
                c[8] = cl_x1;
            }
        }
    }
    return 1;
}

/* Shift-subtract modulo for non-power-of-two texture dimensions without clobbering SH2_DIVU */
static HW_NOINLINE HW_COLD_OS int HW_ModNonPow2(int val, int mod)
{
    unsigned int ustep = (unsigned int)mod;
    int v_mask = val >> 31;
    unsigned int uval = ((unsigned int)val + (unsigned int)v_mask) ^ (unsigned int)v_mask;
    while (ustep <= 0x30000000u && uval >= (ustep << 1))
        ustep <<= 1;
    while (ustep >= (unsigned int)mod)
    {
        if (val >= (int)ustep) val -= (int)ustep;
        else if (val <= -(int)ustep) val += (int)ustep;
        ustep >>= 1;
    }
    if (val < 0) val += mod;
    return val;
}

typedef struct {
    const baked_seg_t       *seg;
    const hw_trans_pillar_t *p0;
    const hw_trans_pillar_t *p1;
    fixed_t                  sec_ceiling;
    fixed_t                  sec_floor;
    fixed_t                  top_world_z;
    fixed_t                  bot_world_z;
    fixed_t                  orig_top_z;
    fixed_t                  orig_bot_z;
    uint32_t                 inv_u_span;
    uint32_t                 cmd_header1;
    int                      u_start;
    int                      u_span;
    int                      dz_top;
    int                      dz_bot;
    int                      cur_tex_id;
    uint16_t                 sub_m_start;
    int16_t                  n_vis;
    int16_t                  last_clip_x0;
    int16_t                  last_clip_x1;
    int16_t                  seg_sx;
    int16_t                  seg_ex;
    int16_t                  top_overlap;
    int16_t                  bot_overlap;
    int16_t                  cur_v;
    int16_t                  rows;
    int16_t                  th;
    int16_t                  tw;
    int16_t                  mono_tw;
    uint16_t                 srca;
    uint16_t                 srca_mono;
    uint16_t                 srca_64;
    uint16_t                 mono_base_srca;
    uint8_t                  is_sky_ceil;
    uint8_t                  was_near_clipped;
    uint8_t                  occl_state;
    uint8_t                  should_subdivide;
    uint8_t                  persp_strips;
    uint8_t                  needs_clip;
    fixed_t                  cur_top_z;
    fixed_t                  t0;
    fixed_t                  t1;
    int16_t                  rem_h;
    int16_t                  wall_h;
    int16_t                  q_xa;
    int16_t                  q_xb;
    int16_t                  ytop_min;
    int16_t                  ybot_max;
} hw_wall_emit_ctx_t;

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Linear(
    hw_wall_emit_ctx_t *ctx, int u_end, int s_min, int s_max)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    const int u_start = ctx->u_start;
    const uint32_t inv_u_span = ctx->inv_u_span;
    const int dz_top = ctx->dz_top;
    const int dz_bot = ctx->dz_bot;
    const int top_overlap = ctx->top_overlap;
    const int bot_overlap = ctx->bot_overlap;
    const int cur_v = ctx->cur_v;
    const int rows = ctx->rows;
    const int th = ctx->th;
    const int tw = ctx->tw;
    const int srca = ctx->srca;
    const int srca_mono = ctx->srca_mono;
    const int srca_64 = ctx->srca_64;
    const uint32_t cmd_header1 = ctx->cmd_header1;
    const int partial_occl = ctx->occl_state;
    const int ytop_min = ctx->ytop_min;
    const int ybot_max = ctx->ybot_max;

    int num_strips = (tw > 0) ? ((tw + 15) >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int strip_mask = num_strips - 1;
    int cur_strip_idx = __builtin_expect((num_strips & strip_mask) == 0, 1)
        ? (s_min & strip_mask)
        : HW_ModNonPow2(s_min, num_strips);

    int strip_srca_step = th << 1;
    int srca_v = srca + (cur_v << 1);
    int strip_srca_base = srca_v + cur_strip_idx * strip_srca_step;
    int loc_quads = 0;
    int loc_strips = 0;
    int lc_x0 = ctx->last_clip_x0;
    int lc_x1 = ctx->last_clip_x1;

    int s_px0 = p0->px;
    int s_ya0, s_ya1, end_ya0, end_ya1;
    HW_ProjYPair(HW_VIEW_CY, dz_top, p0->s, dz_bot, p0->s, &s_ya0, &s_ya1);
    s_ya0 -= top_overlap;
    s_ya1 += bot_overlap;
    HW_ProjYPair(HW_VIEW_CY, dz_top, p1->s, dz_bot, p1->s, &end_ya0, &end_ya1);
    end_ya0 -= top_overlap;
    end_ya1 += bot_overlap;

    int cur_px_fp = 0, cur_ya0_fp = 0, cur_ya1_fp = 0;
    int step_px_fp = 0, step_ya0_fp = 0, step_ya1_fp = 0;
    if (s_min < s_max)
    {
        int first_du = ((s_min + 1) << 4) - u_start;
        int first_t1 = (int)(((uint32_t)first_du * inv_u_span) >> 8);
        int step_t1  = (int)(inv_u_span >> 4);
        int d_px  = p1->px  - s_px0;
        int d_ya0 = end_ya0 - s_ya0;
        int d_ya1 = end_ya1 - s_ya1;
        cur_px_fp   = (s_px0 << 16) + d_px  * first_t1;
        cur_ya0_fp  = (s_ya0 << 16) + d_ya0 * first_t1;
        cur_ya1_fp  = (s_ya1 << 16) + d_ya1 * first_t1;
        step_px_fp  = d_px  * step_t1;
        step_ya0_fp = d_ya0 * step_t1;
        step_ya1_fp = d_ya1 * step_t1;
    }

    int can_coalesce_32 = (tw == 32 && srca_mono != srca);
    int can_coalesce_64 = (tw >= 64 && (srca_64 != srca || srca_mono != srca));
    int srca32_v = srca_mono + (cur_v << 2);
    int step32   = th << 2;

    int srca64_v = srca_64 + (cur_v << 3);

    for (int s = s_min; s <= s_max; s++)
    {
        int u_lo = (s == s_min) ? u_start : (s << 4);
        int coal_strips = 0;
        int emit_srca = strip_srca_base;
        int emit_rows = rows;
        uint32_t emit_size_hi = 2u << 8;

        if (u_lo == (s << 4))
        {
            if (can_coalesce_64 && (cur_strip_idx & 3) == 0 && ((s + 4) << 4) <= u_end)
            {
                coal_strips = 4;
                emit_srca = srca64_v + (cur_strip_idx >> 2) * (th << 3);
                emit_size_hi = 8u << 8;
            }
            else if (can_coalesce_32 && cur_strip_idx == 0 && ((s + 2) << 4) <= u_end)
            {
                coal_strips = 2;
                emit_srca = srca32_v;
                emit_size_hi = 4u << 8;
            }
        }

        int s_end = s;
        int u_hi;
        int s_px1, s_yb0, s_yb1;
        if (coal_strips > 0)
        {
            int c_end = s + coal_strips - 1;
            int c_uhi = (c_end == s_max) ? u_end : ((c_end + 1) << 4);
            int adv = coal_strips - 1;
            int next_px1 = (c_uhi == u_end) ? p1->px : ((cur_px_fp + step_px_fp * adv + 0x8000) >> 16);
            int max_coal_dx = (coal_strips == 4 && srca_64 == srca) ? 24 : 1000;
            if (next_px1 - s_px0 <= max_coal_dx)
            {
                s_end = c_end;
                u_hi  = c_uhi;
                s_px1 = next_px1;
                if (coal_strips == 4 && (s_px1 - s_px0 <= 24 || srca_64 == srca) && srca_mono != srca)
                {
                    emit_srca = srca32_v + (cur_strip_idx >> 2) * step32;
                    emit_size_hi = 4u << 8;
                }
                if (u_hi == u_end)
                {
                    s_yb0 = end_ya0;
                    s_yb1 = end_ya1;
                }
                else
                {
                    cur_px_fp  += step_px_fp  * (adv + 1);
                    cur_ya0_fp += step_ya0_fp * adv;
                    cur_ya1_fp += step_ya1_fp * adv;
                    s_yb0 = (cur_ya0_fp + 0x8000) >> 16; cur_ya0_fp += step_ya0_fp;
                    s_yb1 = (cur_ya1_fp + 0x8000) >> 16; cur_ya1_fp += step_ya1_fp;
                }
            }
            else
            {
                coal_strips = 0;
                emit_srca = strip_srca_base;
                emit_size_hi = 2u << 8;
            }
        }

        if (coal_strips == 0)
        {
            u_hi = (((s + 1) << 4) > u_end) ? u_end : ((s + 1) << 4);
            if (u_hi == u_end)
            {
                s_px1 = p1->px;
                s_yb0 = end_ya0;
                s_yb1 = end_ya1;
            }
            else
            {
                s_px1 = (cur_px_fp  + 0x8000) >> 16; cur_px_fp  += step_px_fp;
                s_yb0 = (cur_ya0_fp + 0x8000) >> 16; cur_ya0_fp += step_ya0_fp;
                s_yb1 = (cur_ya1_fp + 0x8000) >> 16; cur_ya1_fp += step_ya1_fp;
            }
        }

        if (s_px1 > s_px0 && s_px1 > 0 && s_px0 < saturn_view_w &&
            !((s_ya0 >= s_ya1 && s_yb0 >= s_yb1) ||
              (s_ya1 <= ytop_min && s_yb1 <= ytop_min) ||
              (s_ya0 >= ybot_max && s_yb0 >= ybot_max)))
        {
            if (partial_occl)
            {
                int occ_x0 = (s_px0 < 0) ? 0 : s_px0;
                int occ_x1 = (s_px1 > saturn_view_w) ? saturn_view_w : s_px1;
                if (HW_IsSpanOccluded(occ_x0, occ_x1))
                {
                    if (!coal_strips && lc_x0 != -1)
                    {
                        HW_EmitUserClip(0, 0, saturn_view_w - 1, HW_VIEW_H - 1);
                        lc_x0 = -1;
                        lc_x1 = -1;
                    }
                    goto skip_quad_lin;
                }
            }

            int du_vis = u_hi - u_lo;
            int s_q_xa  = s_px0;
            int s_q_xb  = s_px1;
            int s_q_ya0 = s_ya0;
            int s_q_yb0 = s_yb0;
            int s_q_ya1 = s_ya1;
            int s_q_yb1 = s_yb1;
            int need_narrow_clip = 0;

            if (__builtin_expect(du_vis < 16, 0))
            {
                hw_wall_coord_tmp[0] = s_px0; hw_wall_coord_tmp[1] = s_px1;
                hw_wall_coord_tmp[2] = s_ya0; hw_wall_coord_tmp[3] = s_yb0;
                hw_wall_coord_tmp[4] = s_ya1; hw_wall_coord_tmp[5] = s_yb1;
                hw_wall_coord_tmp[6] = 0;
                hw_wall_coord_tmp[7] = lc_x0; hw_wall_coord_tmp[8] = lc_x1;
                if (!HW_ExtrapolatePartialWallStrip(s, u_lo, u_hi, hw_wall_coord_tmp))
                    goto skip_quad_lin;
                s_q_xa  = hw_wall_coord_tmp[0]; s_q_xb  = hw_wall_coord_tmp[1];
                s_q_ya0 = hw_wall_coord_tmp[2]; s_q_yb0 = hw_wall_coord_tmp[3];
                s_q_ya1 = hw_wall_coord_tmp[4]; s_q_yb1 = hw_wall_coord_tmp[5];
                need_narrow_clip = hw_wall_coord_tmp[6];
                lc_x0 = hw_wall_coord_tmp[7]; lc_x1 = hw_wall_coord_tmp[8];
            }

            if (!need_narrow_clip && lc_x0 != -1)
            {
                HW_EmitUserClip(0, 0, saturn_view_w - 1, HW_VIEW_H - 1);
                lc_x0 = -1;
                lc_x1 = -1;
            }

            HW_WALL_XYCLAMP(s_q_xa, s_q_xb, s_q_ya0, s_q_yb0, s_q_ya1, s_q_yb1);

            uint16_t m_cnt = saturn_cmd_m_count;
            if (m_cnt < CMDBUF_M_CAP - 1)
            {
                saturn_cmd_m_count = m_cnt + 1;
                aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                uint32_t s_xa_p = ((uint32_t)s_q_xa << 16);
                cmd32[0] = 0x00020000u;
                cmd32[1] = cmd_header1;
                cmd32[2] = ((uint32_t)emit_srca << 16) | emit_size_hi | ((uint32_t)emit_rows & 0xFFu);
                cmd32[3] = s_xa_p | (uint16_t)s_q_ya0;

                /* Compute remaining vertex words & update counters in registers while 4-stage write buffer drains to SDRAM */
                uint32_t s_xb_p = ((uint32_t)s_q_xb << 16);
                uint32_t w4 = s_xb_p | (uint16_t)s_q_yb0;
                uint32_t w5 = s_xb_p | (uint16_t)s_q_yb1;
                uint32_t w6 = s_xa_p | (uint16_t)s_q_ya1;
                loc_quads++;
                if (!coal_strips) loc_strips++;

                cmd32[4] = w4;
                cmd32[5] = w5;
                cmd32[6] = w6;
                cmd32[7] = 0;
            }
            else
            {
                saturn_v1w_drop++;
                saturn_v1w_captured = 1;
            }
        }

skip_quad_lin:
        s_px0 = s_px1;
        s_ya0 = s_yb0;
        s_ya1 = s_yb1;
        if (coal_strips > 0)
        {
            cur_strip_idx += coal_strips;
            while (cur_strip_idx >= num_strips)
                cur_strip_idx -= num_strips;
            strip_srca_base = srca_v + cur_strip_idx * strip_srca_step;
            s = s_end;
        }
        else
        {
            strip_srca_base += strip_srca_step;
            if (++cur_strip_idx >= num_strips)
            {
                cur_strip_idx = 0;
                strip_srca_base = srca_v;
            }
        }
    }
    ctx->last_clip_x0 = (int16_t)lc_x0;
    ctx->last_clip_x1 = (int16_t)lc_x1;
    saturn_v1w_cells += loc_quads;
    saturn_hw_wall_quads += (unsigned int)loc_quads;
    saturn_hw_wall_strips += (unsigned int)loc_strips;
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Persp(
    hw_wall_emit_ctx_t *ctx, int u_end, int s_min, int s_max)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    const int u_start = ctx->u_start;
    const uint32_t inv_u_span = ctx->inv_u_span;
    const int dz_top = ctx->dz_top;
    const int dz_bot = ctx->dz_bot;
    const int top_overlap = ctx->top_overlap;
    const int bot_overlap = ctx->bot_overlap;
    const int cur_v = ctx->cur_v;
    const int rows = ctx->rows;
    const int th = ctx->th;
    const int tw = ctx->tw;
    const int srca = ctx->srca;
    const uint32_t cmd_header1 = ctx->cmd_header1;
    const int baked_light = ctx->seg->baked_light;
    const int partial_occl = ctx->occl_state;
    const int ytop_min = ctx->ytop_min;
    const int ybot_max = ctx->ybot_max;

    int num_strips = (tw > 0) ? ((tw + 15) >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int strip_mask = num_strips - 1;
    int cur_strip_idx = __builtin_expect((num_strips & strip_mask) == 0, 1)
        ? (s_min & strip_mask)
        : HW_ModNonPow2(s_min, num_strips);

    int strip_srca_step = th << 1;
    int srca_v = srca + (cur_v << 1);
    int strip_srca_base = srca_v + cur_strip_idx * strip_srca_step;
    int loc_quads = 0;
    int loc_strips = 0;
    int lc_x0 = ctx->last_clip_x0;
    int lc_x1 = ctx->last_clip_x1;

    int s_px0 = p0->px;
    int s_ya0, s_ya1;
    HW_ProjYPair(HW_VIEW_CY, dz_top, p0->s, dz_bot, p0->s, &s_ya0, &s_ya1);
    s_ya0 -= top_overlap;
    s_ya1 += bot_overlap;

    int light_s = baked_light >> 4;
    if (light_s < 0) light_s = 0; else if (light_s > 15) light_s = 15;
    const uint16_t *zlight_row = wall_zlight_cmdcolr_LUT[light_s];
    uint32_t cmd_pmod_hi = cmd_header1 & ~0x0000FFFFu;
    int const_light = 1;
    if (light_s < 15)
    {
        unsigned int zb0 = (unsigned int)p0->cz >> 20; if (zb0 > 127u) zb0 = 127u;
        unsigned int zb1 = (unsigned int)p1->cz >> 20; if (zb1 > 127u) zb1 = 127u;
        const_light = (zlight_row[zb0] == zlight_row[zb1]);
    }

    fixed_t s_cz0 = p0->cz;
    fixed_t cur_cx1 = 0, cur_cz1 = 0, step_cx1 = 0, step_cz1 = 0;
    if (s_min < s_max)
    {
        int d_cx = p1->cx - p0->cx;
        int d_cz = p1->cz - p0->cz;
        int first_du = ((s_min + 1) << 4) - u_start;
        fixed_t first_t1 = (fixed_t)(((uint32_t)first_du * inv_u_span) >> 8);
        fixed_t step_t1  = (fixed_t)(inv_u_span >> 4);
        HW_FixedMulPair(d_cx, first_t1, d_cz, first_t1, &cur_cx1, &cur_cz1);
        cur_cx1 += p0->cx;
        cur_cz1 += p0->cz;
        HW_FixedMulPair(d_cx, step_t1, d_cz, step_t1, &step_cx1, &step_cz1);
    }

    for (int s = s_min; s <= s_max; s++)
    {
        int u_lo = (s == s_min) ? u_start : (s << 4);
        int u_hi = (((s + 1) << 4) > u_end) ? u_end : ((s + 1) << 4);

        fixed_t s_cx1, s_cz1;
        int s_scale1, s_px1;
        int s_yb0, s_yb1;

        /* Only calculate Edge 1 (right edge); reuse p1 at final strip */
        if (u_hi == u_end)
        {
            s_cx1    = p1->cx;
            s_cz1    = p1->cz;
            s_scale1 = p1->s;
            s_px1    = p1->px;
        }
        else
        {
            s_cx1 = cur_cx1; cur_cx1 += step_cx1;
            s_cz1 = cur_cz1; cur_cz1 += step_cz1;
            if (s_cz1 < HW_NEAR_Z) s_cz1 = HW_NEAR_Z;
            s_scale1 = HW_FastDiv(s_cz1);
            s_px1    = HW_ProjX(s_cx1, s_scale1, HW_VIEW_CX);
        }
        HW_ProjYPair(HW_VIEW_CY, dz_top, s_scale1, dz_bot, s_scale1, &s_yb0, &s_yb1);
        s_yb0 -= top_overlap;
        s_yb1 += bot_overlap;

        if (s_px1 > s_px0 && s_px1 > 0 && s_px0 < saturn_view_w &&
            !((s_ya0 >= s_ya1 && s_yb0 >= s_yb1) ||
              (s_ya1 <= ytop_min && s_yb1 <= ytop_min) ||
              (s_ya0 >= ybot_max && s_yb0 >= ybot_max)))
        {
            if (partial_occl)
            {
                int occ_x0 = (s_px0 < 0) ? 0 : s_px0;
                int occ_x1 = (s_px1 > saturn_view_w) ? saturn_view_w : s_px1;
                if (HW_IsSpanOccluded(occ_x0, occ_x1))
                {
                    if (lc_x0 != -1)
                    {
                        HW_EmitUserClip(0, 0, saturn_view_w - 1, HW_VIEW_H - 1);
                        lc_x0 = -1;
                        lc_x1 = -1;
                    }
                    goto skip_quad_persp;
                }
            }

            int du_vis = u_hi - u_lo;
            int s_q_xa  = s_px0;
            int s_q_xb  = s_px1;
            int s_q_ya0 = s_ya0;
            int s_q_yb0 = s_yb0;
            int s_q_ya1 = s_ya1;
            int s_q_yb1 = s_yb1;
            int need_narrow_clip = 0;

            if (__builtin_expect(du_vis < 16, 0))
            {
                hw_wall_coord_tmp[0] = s_px0; hw_wall_coord_tmp[1] = s_px1;
                hw_wall_coord_tmp[2] = s_ya0; hw_wall_coord_tmp[3] = s_yb0;
                hw_wall_coord_tmp[4] = s_ya1; hw_wall_coord_tmp[5] = s_yb1;
                hw_wall_coord_tmp[6] = 0;
                hw_wall_coord_tmp[7] = lc_x0; hw_wall_coord_tmp[8] = lc_x1;
                if (!HW_ExtrapolatePartialWallStrip(s, u_lo, u_hi, hw_wall_coord_tmp))
                    goto skip_quad_persp;
                s_q_xa  = hw_wall_coord_tmp[0]; s_q_xb  = hw_wall_coord_tmp[1];
                s_q_ya0 = hw_wall_coord_tmp[2]; s_q_yb0 = hw_wall_coord_tmp[3];
                s_q_ya1 = hw_wall_coord_tmp[4]; s_q_yb1 = hw_wall_coord_tmp[5];
                need_narrow_clip = hw_wall_coord_tmp[6];
                lc_x0 = hw_wall_coord_tmp[7]; lc_x1 = hw_wall_coord_tmp[8];
            }

            if (!need_narrow_clip && lc_x0 != -1)
            {
                HW_EmitUserClip(0, 0, saturn_view_w - 1, HW_VIEW_H - 1);
                lc_x0 = -1;
                lc_x1 = -1;
            }

            HW_WALL_XYCLAMP(s_q_xa, s_q_xb, s_q_ya0, s_q_yb0, s_q_ya1, s_q_yb1);

            uint16_t m_cnt = saturn_cmd_m_count;
            if (m_cnt < CMDBUF_M_CAP - 1)
            {
                saturn_cmd_m_count = m_cnt + 1;
                uint32_t strip_cmd1;
                if (const_light)
                {
                    strip_cmd1 = cmd_header1;
                }
                else
                {
                    unsigned int s_zb = ((unsigned int)s_cz0 + (unsigned int)s_cz1) >> 21;
                    if (__builtin_expect(s_zb > 127u, 0)) s_zb = 127u;
                    strip_cmd1 = cmd_pmod_hi | ((uint32_t)zlight_row[s_zb] & 0xFF00u);
                }

                aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                uint32_t s_xa_p = ((uint32_t)s_q_xa << 16);
                cmd32[0] = 0x00020000u;
                cmd32[1] = strip_cmd1;
                cmd32[2] = ((uint32_t)strip_srca_base << 16) | (2u << 8) | ((uint32_t)rows & 0xFFu);
                cmd32[3] = s_xa_p | (uint16_t)s_q_ya0;

                /* Compute remaining vertex words & update counters in registers while 4-stage write buffer drains to SDRAM */
                uint32_t s_xb_p = ((uint32_t)s_q_xb << 16);
                uint32_t w4 = s_xb_p | (uint16_t)s_q_yb0;
                uint32_t w5 = s_xb_p | (uint16_t)s_q_yb1;
                uint32_t w6 = s_xa_p | (uint16_t)s_q_ya1;
                loc_quads++;
                loc_strips++;

                cmd32[4] = w4;
                cmd32[5] = w5;
                cmd32[6] = w6;
                cmd32[7] = 0;
            }
            else
            {
                saturn_v1w_drop++;
                saturn_v1w_captured = 1;
            }
        }

skip_quad_persp:
        s_cz0 = s_cz1;
        s_px0 = s_px1;
        s_ya0 = s_yb0;
        s_ya1 = s_yb1;
        strip_srca_base += strip_srca_step;
        if (++cur_strip_idx >= num_strips)
        {
            cur_strip_idx = 0;
            strip_srca_base = srca_v;
        }
    }
    ctx->last_clip_x0 = (int16_t)lc_x0;
    ctx->last_clip_x1 = (int16_t)lc_x1;
    saturn_v1w_cells += loc_quads;
    saturn_hw_wall_quads += (unsigned int)loc_quads;
    saturn_hw_wall_strips += (unsigned int)loc_strips;
}

HW_ALWAYS_INLINE void HW_EmitWallStrips(hw_wall_emit_ctx_t *ctx)
{
    int u_span = ctx->u_span;
    if (u_span <= 0) return;
    int u_start = ctx->u_start;
    int u_end = u_start + u_span;
    int s_min = u_start >> 4;
    int s_max = (u_end - 1) >> 4;

    if (__builtin_expect(!ctx->persp_strips, 1))
        HW_EmitWallStrips_Linear(ctx, u_end, s_min, s_max);
    else
        HW_EmitWallStrips_Persp(ctx, u_end, s_min, s_max);
}

HW_ALWAYS_INLINE int HW_NeedPerspectiveStrips(int dx, int u_span, fixed_t cz0, fixed_t cz1, int tw)
{
    if (!saturn_wall_subdivision) return 0;
    if (tw <= 16) return 0;
    /* Distant walls: both ends > 512 units have negligible affine distortion */
    if (cz0 > (512 << FRACBITS) && cz1 > (512 << FRACBITS)) return 0;
    /* Narrow on screen: < 20 pixels has < 0.25px affine sag */
    if (dx < 20) return 0;
    /* Perpendicular walls: < 12.5% depth change across the wall has minimal affine warping */
    fixed_t min_z = (cz0 < cz1) ? cz0 : cz1;
    fixed_t max_z = (cz0 > cz1) ? cz0 : cz1;
    if ((max_z - min_z) < (min_z >> 3)) return 0;
    /* Don't subdivide if average strip is < 6 screen pixels wide */
    if ((dx << 4) < (u_span * 6)) return 0;
    /* Medium-to-far walls (min_z > 256): require >= 25% depth change and >= 8px per strip */
    if (min_z > (256 << FRACBITS) && ((max_z - min_z) < (min_z >> 2) || (dx << 4) < (u_span * 8)))
        return 0;
    return 1;
}

#define HW_MAX_SUBSECTOR_PILLARS 96

static hw_trans_pillar_t hw_p0_clipped;
static hw_trans_pillar_t hw_p1_clipped;

static HW_NOINLINE HW_COLD_OS void HW_TransformPillar_Shifted(
    hw_trans_pillar_t *tp, int pdx, int pdy)
{
    fixed_t cz, cx;
    ProjectPillar_2D_Shifted(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, cam_cz_origin, cam_cx_origin, &cz, &cx);
    int s = 0, px = 0;
    if (cz >= HW_NEAR_Z)
    {
        s = HW_FastDiv(cz);
        px = HW_ProjX(cx, s, saturn_view_cx);
    }
    tp->cx = cx;
    tp->cz = cz;
    tp->s  = s;
    tp->px = px;
}

/* Takes 3 register arguments (r4=tp, r5=raw_p, r6=fr), 0 stack arguments, 0 stack frame on hot path! */
static HW_NOINLINE HW_HOT_O2 void HW_TransformSubsectorPillar(
    hw_trans_pillar_t *tp, const saturn_wl_pillar_t *raw_p, int fr)
{
    int obj_dx = (int)raw_p->x - hw_cam_x;
    int obj_dy = (int)raw_p->z - hw_cam_y;
    int pdx = (obj_dx * 16) + ((unsigned)fr >> 4);
    int pdy = (obj_dy * 16) + (fr & 15);
    if (__builtin_expect((((unsigned int)obj_dx + 16384u) | ((unsigned int)obj_dy + 16384u)) > 32767u, 0))
    {
        HW_TransformPillar_Shifted(tp, pdx, pdy);
        return;
    }
    fixed_t cz, cx;
    ProjectPillar_2D(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, cam_cz_origin, cam_cx_origin, &cz, &cx);
    int s = 0, px = 0;
    if (cz >= HW_NEAR_Z)
    {
        s = HW_FastDiv(cz);
        px = HW_ProjX(cx, s, saturn_view_cx);
    }
    tp->cx = cx;
    tp->cz = cz;
    tp->s  = s;
    tp->px = px;
}

/* Takes 3 register arguments (r4=p0, r5=p1, r6=ctx), 0 stack arguments */
static HW_NOINLINE HW_COLD_OS int HW_ClipWallNearPlane(
    const hw_trans_pillar_t *p0, const hw_trans_pillar_t *p1, hw_wall_emit_ctx_t *ctx)
{
    hw_p0_clipped = *p0;
    hw_p1_clipped = *p1;
    int u_start = ctx->u_start;
    int u_span  = ctx->u_span;
    const int view_cx = saturn_view_cx;
    const int view_w  = saturn_view_w;

    if (hw_p0_clipped.cz < HW_NEAR_Z)
    {
        fixed_t num = HW_NEAR_Z - hw_p0_clipped.cz;
        fixed_t den = hw_p1_clipped.cz - hw_p0_clipped.cz;
        fixed_t t = HW_ClampedFrac((unsigned int)num, (unsigned int)den);
        if (t > FRACUNIT) t = FRACUNIT;

        hw_p0_clipped.cx = (int32_t)((uint32_t)hw_p0_clipped.cx + ((uint32_t)FixedMul(t, (hw_p1_clipped.cx >> 1) - (hw_p0_clipped.cx >> 1)) << 1));
        hw_p0_clipped.cz = HW_NEAR_Z;
        hw_p0_clipped.s  = HW_NEAR_Z_SCALE;
        hw_p0_clipped.px = HW_ProjX(hw_p0_clipped.cx, hw_p0_clipped.s, view_cx);

        int du = (int)(((uint32_t)t * (uint32_t)u_span) >> FRACBITS);
        u_start += du;
        u_span  -= du;
        if (u_span <= 0)
            return 0;
    }
    else if (hw_p1_clipped.cz < HW_NEAR_Z)
    {
        fixed_t num = hw_p0_clipped.cz - HW_NEAR_Z;
        fixed_t den = hw_p0_clipped.cz - hw_p1_clipped.cz;
        fixed_t t = HW_ClampedFrac((unsigned int)num, (unsigned int)den);
        if (t > FRACUNIT) t = FRACUNIT;

        hw_p1_clipped.cx = (int32_t)((uint32_t)hw_p0_clipped.cx + ((uint32_t)FixedMul(t, (hw_p1_clipped.cx >> 1) - (hw_p0_clipped.cx >> 1)) << 1));
        hw_p1_clipped.cz = HW_NEAR_Z;
        hw_p1_clipped.s  = HW_NEAR_Z_SCALE;
        hw_p1_clipped.px = HW_ProjX(hw_p1_clipped.cx, hw_p1_clipped.s, view_cx);

        int du = (int)(((uint32_t)t * (uint32_t)u_span) >> FRACBITS);
        u_span = du;
        if (u_span <= 0)
            return 0;
    }

    int xa = hw_p0_clipped.px;
    int xb = hw_p1_clipped.px;
    if (xb - xa <= 0 || xb <= 0 || xa >= view_w)
        return 0;

    int seg_sx = (xa < 0) ? 0 : xa;
    int seg_ex = (xb > view_w) ? view_w : xb;
    int occl = (hw_solid_count > 0) ? HW_CheckSpanOcclusion(seg_sx, seg_ex) : 0;
    if (occl == 2)
        return 0;

#ifndef SATURN_WIN32
    uint32_t sr_save = HW_StartDivu_Background(256, (unsigned int)u_span);
#endif
    ctx->u_start = u_start;
    ctx->u_span  = u_span;
    ctx->seg_sx  = (int16_t)seg_sx;
    ctx->seg_ex  = (int16_t)seg_ex;
    ctx->occl_state = (uint8_t)occl;
    ctx->was_near_clipped = 1;
#ifndef SATURN_WIN32
    ctx->inv_u_span = HW_FinishDivu_Background(sr_save);
#else
    ctx->inv_u_span = (256u << 16) / (unsigned int)u_span;
#endif
    return 1;
}

/* Cold helpers for narrow monolithic wall UV extrapolation (tw <= 16 && u_span < tw)
 * Takes <= 3 register arguments (0 stack arguments!) */
static HW_NOINLINE HW_COLD_OS int HW_ExtrapolateMonoWallX(
    hw_wall_emit_ctx_t *ctx, int xa, int xb)
{
    const baked_seg_t *seg = ctx->seg;
    int u_start = ctx->u_start;
    int u_span  = ctx->u_span;
    int mono_tw = ctx->mono_tw;
    int view_w  = saturn_view_w;
    int cl_x0 = (xa < 0) ? 0 : (xa > (view_w - 1) ? (view_w - 1) : xa);
    int cl_x1 = (xb < 0) ? 0 : (xb > (view_w - 1) ? (view_w - 1) : xb);
    if (cl_x0 >= cl_x1)
        return 0;

    int inv_u;
    if (u_span == (int)seg->u_span && seg->inv_u_span) inv_u = (int)seg->inv_u_span;
    else if (u_span == 1) inv_u = 65536;
    else if (ctx->was_near_clipped) inv_u = (int)(ctx->inv_u_span >> 8);
    else inv_u = HW_ClampedFrac(1, (unsigned int)u_span);

    int dx = xb - xa;
    int f1 = mono_tw - u_start;
    fixed_t t1 = f1 * inv_u;
    fixed_t t0 = 0;
    int q_xa, q_xb;

    if (u_start == 0)
    {
        int m_dx_t1 = FixedMul(dx, t1);
        q_xa = xa;
        q_xb = (u_span == mono_tw) ? xb : (xa + m_dx_t1);
    }
    else
    {
        int f0 = -u_start;
        t0 = f0 * inv_u;
        int m_dx_t0, m_dx_t1;
        HW_FixedMulPair(dx, t0, dx, t1, &m_dx_t0, &m_dx_t1);
        q_xa = xa + m_dx_t0;
        q_xb = xa + m_dx_t1;
    }

    if (t1 > 98304 || t0 < -32768 ||
        q_xa >= q_xb || q_xa < -100 || q_xb > 420 || (q_xb - q_xa) > 480)
    {
        ctx->q_xa = (int16_t)xa;
        ctx->q_xb = (int16_t)xb;
        ctx->needs_clip = 0;
    }
    else
    {
        ctx->q_xa = (int16_t)q_xa;
        ctx->q_xb = (int16_t)q_xb;
        ctx->t0 = t0;
        ctx->t1 = t1;
        if (cl_x0 != ctx->last_clip_x0 || cl_x1 != ctx->last_clip_x1)
        {
            HW_EmitUserClip(cl_x0, 0, cl_x1, HW_VIEW_H - 1);
            ctx->last_clip_x0 = (int16_t)cl_x0;
            ctx->last_clip_x1 = (int16_t)cl_x1;
        }
    }
    return 1;
}

/* Takes 2 register arguments (r4=ctx, r5=c[6]: s_xa, s_xb, ya0, yb0, ya1, yb1), 0 stack arguments */
static HW_NOINLINE HW_COLD_OS void HW_ExtrapolateMonoWallY(
    const hw_wall_emit_ctx_t *ctx, int *c)
{
    int ya0 = c[2], yb0 = c[3], ya1 = c[4], yb1 = c[5];
    int u_start = ctx->u_start;
    int u_span  = ctx->u_span;
    int mono_tw = ctx->mono_tw;
    fixed_t t0  = ctx->t0;
    fixed_t t1  = ctx->t1;
    int dya0 = yb0 - ya0;
    int dya1 = yb1 - ya1;
    int q_ya0, q_yb0, q_ya1, q_yb1;
    if (u_start == 0)
    {
        int m01, m11;
        HW_FixedMulPair(dya0, t1, dya1, t1, &m01, &m11);
        q_ya0 = ya0;
        q_yb0 = (u_span == mono_tw) ? yb0 : (ya0 + m01);
        q_ya1 = ya1;
        q_yb1 = (u_span == mono_tw) ? yb1 : (ya1 + m11);
    }
    else
    {
        int m00, m01, m10, m11;
        HW_FixedMulPair(dya0, t0, dya0, t1, &m00, &m01);
        HW_FixedMulPair(dya1, t0, dya1, t1, &m10, &m11);
        q_ya0 = ya0 + m00;
        q_yb0 = ya0 + m01;
        q_ya1 = ya1 + m10;
        q_yb1 = ya1 + m11;
    }

    if (q_ya0 >= q_ya1 || q_yb0 >= q_yb1 || q_ya0 < -256 || q_yb0 < -256 || q_ya1 > 400 || q_yb1 > 400)
    {
        c[0] = ctx->p0->px;
        c[1] = ctx->p1->px;
    }
    else
    {
        c[2] = q_ya0;
        c[3] = q_yb0;
        c[4] = q_ya1;
        c[5] = q_yb1;
    }
}

/* Takes 2 register arguments (r4=texnum, r5=ctx), 0 stack arguments, 0 address-taken locals in caller */
static HW_NOINLINE int HW_LoadWallTexCtx(int texnum, hw_wall_emit_ctx_t *ctx)
{
    int tw = 0, th = 0, srca_mono = 0, srca_64 = 0;
    int srca = HW_FastWallSrca(texnum, &tw, &th, &srca_mono, &srca_64);
    ctx->cur_tex_id = texnum;
    ctx->srca       = (uint16_t)srca;
    ctx->srca_mono  = (uint16_t)srca_mono;
    ctx->srca_64    = (uint16_t)srca_64;
    ctx->tw         = (int16_t)tw;
    ctx->th         = (int16_t)th;
    return srca;
}

static HW_NOINLINE HW_COLD_OS void HW_EmitMultiBandBakedWall(hw_wall_emit_ctx_t *ctx)
{
    const baked_seg_t *seg = ctx->seg;
    const int s_A = ctx->p0->s;
    const int s_B = ctx->p1->s;
    const fixed_t vz = hw_viewz;
    const int th = ctx->th;
    const int mono_tw = ctx->mono_tw;
    const int mono_base_srca = ctx->mono_base_srca;
    const uint32_t tw_stride = (uint32_t)(mono_tw >> 3);
    const uint32_t cmd_size_hi = tw_stride << 8;
    const uint32_t cmd_header1 = ctx->cmd_header1;
    const int wall_h = ctx->wall_h;

    fixed_t cur_top_z = ctx->cur_top_z;
    int dz_top = ctx->dz_top;
    int top_overlap = ctx->top_overlap;
    int rem_h = ctx->rem_h;
    int cur_v = ctx->cur_v;

    int ya0, yb0, ya1 = 0, yb1 = 0;
    HW_ProjYPair(HW_VIEW_CY, dz_top, s_A, dz_top, s_B, &ya0, &yb0);
    ya0 -= top_overlap;
    yb0 -= top_overlap;

    if (!ctx->should_subdivide && !ctx->needs_clip && ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, saturn_view_w - 1, HW_VIEW_H - 1);
        ctx->last_clip_x0 = -1;
        ctx->last_clip_x1 = -1;
    }

    while (rem_h > 0)
    {
        int band_h = (th > 0) ? (th - cur_v) : rem_h;
        if (band_h > rem_h)
            band_h = rem_h;
        if (band_h <= 0)
            band_h = rem_h;
        if (band_h > 255)
            band_h = 255;

        fixed_t cur_bot_z = cur_top_z - (band_h << FRACBITS);
        int dz_bot = HW_DZQ(cur_bot_z, vz);

        int bot_overlap = (rem_h <= band_h)
            ? ((seg->flags & SEG_FLAG_UPPER) ? ((wall_h <= 16) ? 0 : 2) : ((wall_h <= 16) ? 0 : 1))
            : 0;

        if (ctx->should_subdivide)
        {
            int rows = band_h;
            if (rows > 255) rows = 255;
            ctx->dz_top = dz_top;
            ctx->dz_bot = dz_bot;
            ctx->top_overlap = (int16_t)top_overlap;
            ctx->bot_overlap = (int16_t)bot_overlap;
            ctx->cur_v = (int16_t)cur_v;
            ctx->rows = (int16_t)rows;
            HW_EmitWallStrips(ctx);
        }
        else
        {
            int strip_srca = mono_base_srca + cur_v * tw_stride;
            HW_ProjYPair(HW_VIEW_CY, dz_bot, s_A, dz_bot, s_B, &ya1, &yb1);
            ya1 += bot_overlap;
            yb1 += bot_overlap;
    
    

            if ((ya0 < ya1 || yb0 < yb1) &&
                !(ya1 <= ctx->ytop_min && yb1 <= ctx->ytop_min) &&
                !(ya0 >= ctx->ybot_max && yb0 >= ctx->ybot_max))
            {
                int s_xa = ctx->q_xa;
                int s_xb = ctx->q_xb;
                int q_ya0 = ya0;
                int q_yb0 = yb0;
                int q_ya1 = ya1;
                int q_yb1 = yb1;

                if (ctx->needs_clip)
                {
                    hw_wall_coord_tmp[0] = s_xa;  hw_wall_coord_tmp[1] = s_xb;
                    hw_wall_coord_tmp[2] = q_ya0; hw_wall_coord_tmp[3] = q_yb0;
                    hw_wall_coord_tmp[4] = q_ya1; hw_wall_coord_tmp[5] = q_yb1;
                    HW_ExtrapolateMonoWallY(ctx, hw_wall_coord_tmp);
                    s_xa  = hw_wall_coord_tmp[0]; s_xb  = hw_wall_coord_tmp[1];
                    q_ya0 = hw_wall_coord_tmp[2]; q_yb0 = hw_wall_coord_tmp[3];
                    q_ya1 = hw_wall_coord_tmp[4]; q_yb1 = hw_wall_coord_tmp[5];
                }

                HW_WALL_XYCLAMP(s_xa, s_xb, q_ya0, q_yb0, q_ya1, q_yb1);
                int rows = band_h;
                if (rows > 255) rows = 255;

                uint16_t m_cnt = saturn_cmd_m_count;
                if (m_cnt < CMDBUF_M_CAP - 1)
                {
                    saturn_cmd_m_count = m_cnt + 1;
                    aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                    uint32_t xa_p = ((uint32_t)s_xa << 16);
                    uint32_t xb_p = ((uint32_t)s_xb << 16);
                    cmd32[0] = 0x00020000u;
                    cmd32[1] = cmd_header1;
                    cmd32[2] = ((uint32_t)strip_srca << 16) | cmd_size_hi | ((uint32_t)rows & 0xFFu);
                    cmd32[3] = xa_p | (uint16_t)q_ya0;

                    uint32_t w4 = xb_p | (uint16_t)q_yb0;
                    uint32_t w5 = xb_p | (uint16_t)q_yb1;
                    uint32_t w6 = xa_p | (uint16_t)q_ya1;
                    saturn_v1w_cells++;
                    saturn_hw_wall_quads++;

                    cmd32[4] = w4;
                    cmd32[5] = w5;
                    cmd32[6] = w6;
                    cmd32[7] = 0;
                }
                else
                {
                    saturn_v1w_drop++;
                    saturn_v1w_captured = 1;
                }
            }
        }

        cur_top_z = cur_bot_z;
        dz_top = dz_bot;
        top_overlap = 0;
        rem_h -= band_h;
        cur_v = (th > 0 && cur_v + band_h < th) ? (cur_v + band_h) : 0;
        ya0 = ya1;
        yb0 = yb1;
    }
}

typedef struct {
    const baked_seg_t       *seg;
    const hw_trans_pillar_t *p0;
    const hw_trans_pillar_t *p1;
    uint32_t                 cmd_header1;
    fixed_t                  cur_top_z;
    int                      dz_top;
    int                      dz_bot;
    uint16_t                 srca;
    uint16_t                 srca_mono;
    uint16_t                 srca_64;
    uint16_t                 mono_base_srca;
    int16_t                  tw;
    int16_t                  th;
    int16_t                  u_start;
    int16_t                  u_span;
    int16_t                  mono_tw;
    int16_t                  top_overlap;
    int16_t                  bot_overlap;
    int16_t                  cur_v;
    int16_t                  rem_h;
    int16_t                  wall_h;
    int16_t                  q_xa;
    int16_t                  q_xb;
    uint8_t                  occl_state;
    uint8_t                  should_subdivide;
    uint8_t                  persp_strips;
    uint8_t                  is_single_band;
    int16_t                  ytop_min;
    int16_t                  ybot_max;
} hw_def_wall_t;

#define HW_MAX_DEF_WALLS 24
static hw_def_wall_t hw_def_walls[HW_MAX_DEF_WALLS];
static int hw_def_wall_count = 0;

static HW_NOINLINE HW_HOT_O2 void HW_DeferOrEmitSubdividedWall(
    hw_wall_emit_ctx_t *ctx, int is_single_band)
{
    if (__builtin_expect(!ctx->was_near_clipped && !ctx->needs_clip && hw_def_wall_count < HW_MAX_DEF_WALLS, 1))
    {
        hw_def_wall_t *dw = &hw_def_walls[hw_def_wall_count++];
        dw->seg              = ctx->seg;
        dw->p0               = ctx->p0;
        dw->p1               = ctx->p1;
        dw->cmd_header1      = ctx->cmd_header1;
        dw->cur_top_z        = ctx->cur_top_z;
        dw->dz_top           = ctx->dz_top;
        dw->dz_bot           = ctx->dz_bot;
        dw->srca             = ctx->srca;
        dw->srca_mono        = ctx->srca_mono;
        dw->srca_64          = ctx->srca_64;
        dw->mono_base_srca   = ctx->mono_base_srca;
        dw->tw               = ctx->tw;
        dw->th               = ctx->th;
        dw->u_start          = (int16_t)ctx->u_start;
        dw->u_span           = (int16_t)ctx->u_span;
        dw->mono_tw          = ctx->mono_tw;
        dw->top_overlap      = ctx->top_overlap;
        dw->bot_overlap      = ctx->bot_overlap;
        dw->cur_v            = ctx->cur_v;
        dw->rem_h            = ctx->rem_h;
        dw->wall_h           = ctx->wall_h;
        dw->q_xa             = ctx->q_xa;
        dw->q_xb             = ctx->q_xb;
        dw->occl_state       = ctx->occl_state;
        dw->should_subdivide = ctx->should_subdivide;
        dw->persp_strips     = ctx->persp_strips;
        dw->is_single_band   = (uint8_t)is_single_band;
        dw->ytop_min         = ctx->ytop_min;
        dw->ybot_max         = ctx->ybot_max;
        return;
    }

    if (!ctx->was_near_clipped && ctx->should_subdivide)
    {
        ctx->inv_u_span = ctx->seg->inv_u_span ? ((uint32_t)ctx->seg->inv_u_span << 8) : (65536u << 8);
    }
    if (__builtin_expect(is_single_band, 1))
    {
        int th = ctx->th;
        int rem_h = ctx->rem_h;
        int rows = (th > 0 && rem_h > th) ? th : rem_h;
        if (rows > 255) rows = 255;
        ctx->rows = (int16_t)rows;
        HW_EmitWallStrips(ctx);
    }
    else
    {
        HW_EmitMultiBandBakedWall(ctx);
    }
}

static HW_NOINLINE HW_HOT_O2 void HW_FlushDeferredSubsectorWalls(hw_wall_emit_ctx_t *ctx)
{
    int cnt = hw_def_wall_count;
    hw_def_wall_count = 0;
    ctx->was_near_clipped = 0;
    ctx->needs_clip = 0;

    for (int k = 0; k < cnt; k++)
    {
        const hw_def_wall_t *dw = &hw_def_walls[k];
        const baked_seg_t *seg = dw->seg;
        ctx->seg              = seg;
        ctx->p0               = dw->p0;
        ctx->p1               = dw->p1;
        ctx->cmd_header1      = dw->cmd_header1;
        ctx->dz_top           = dw->dz_top;
        ctx->dz_bot           = dw->dz_bot;
        ctx->srca             = dw->srca;
        ctx->srca_mono        = dw->srca_mono;
        ctx->srca_64          = dw->srca_64;
        ctx->mono_base_srca   = dw->mono_base_srca;
        ctx->tw               = dw->tw;
        ctx->th               = dw->th;
        ctx->u_start          = dw->u_start;
        ctx->u_span           = dw->u_span;
        ctx->mono_tw          = dw->mono_tw;
        ctx->top_overlap      = dw->top_overlap;
        ctx->bot_overlap      = dw->bot_overlap;
        ctx->cur_v            = dw->cur_v;
        ctx->occl_state       = dw->occl_state;
        ctx->should_subdivide = dw->should_subdivide;
        ctx->persp_strips     = dw->persp_strips;
        ctx->ytop_min         = dw->ytop_min;
        ctx->ybot_max         = dw->ybot_max;
        if (dw->should_subdivide)
        {
            ctx->inv_u_span = seg->inv_u_span ? ((uint32_t)seg->inv_u_span << 8) : (65536u << 8);
        }

        if (__builtin_expect(dw->is_single_band, 1))
        {
            int th = dw->th;
            int rem_h = dw->rem_h;
            int rows = (th > 0 && rem_h > th) ? th : rem_h;
            if (rows > 255) rows = 255;
            ctx->rows = (int16_t)rows;
            HW_EmitWallStrips(ctx);
        }
        else
        {
            ctx->cur_top_z = dw->cur_top_z;
            ctx->rem_h     = dw->rem_h;
            ctx->wall_h    = dw->wall_h;
            ctx->q_xa      = dw->q_xa;
            ctx->q_xb      = dw->q_xb;
            HW_EmitMultiBandBakedWall(ctx);
        }
    }
}
static HW_NOINLINE HW_HOT_O2 void HW_RecordSegPortalAperture(
    const hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg, int s_A, int s_B)
{
    int bsec_idx = seg->back_sec_idx;
    if (ctx->seg_sx >= ctx->seg_ex || bsec_idx < 0 || bsec_idx >= numsectors)
        return;
    const sector_t *bsec = &sectors[bsec_idx];
    hw_aperture_state_t *ap = HW_APERTURE;
    const fixed_t vz = hw_viewz;
    int p_yt = 0;
    int p_yb = HW_VIEW_H;
    int yt_att = ap->cur_sub_yt_attach;
    int yb_att = ap->cur_sub_yb_attach;

    if (!ctx->is_sky_ceil && (int16_t)bsec->ceilingpic != -1)
    {
        fixed_t open_top = (ctx->sec_ceiling < bsec->ceilingheight) ? ctx->sec_ceiling : bsec->ceilingheight;
        if (ctx->sec_ceiling > bsec->ceilingheight || open_top > vz)
        {
            int dzt = HW_DZQ(open_top, vz);
            int yta, ytb;
            HW_ProjYPair(HW_VIEW_CY, dzt, s_A, dzt, s_B, &yta, &ytb);
            int yt_min = (yta < ytb) ? yta : ytb;
            p_yt = yt_min - 1;
            if (ctx->sec_ceiling > bsec->ceilingheight)
            {
                int dzt_hi = HW_DZQ(ctx->sec_ceiling, vz);
                int yha, yhb;
                HW_ProjYPair(HW_VIEW_CY, dzt_hi, s_A, dzt_hi, s_B, &yha, &yhb);
                int w_yt_att = (yha < yhb) ? yha : yhb;
                if (w_yt_att < yt_att) yt_att = w_yt_att;
            }
            if ((unsigned)bsec_idx < HW_MAX_AP_SECTORS)
            {
                int b_yt_att = (yt_min < 0) ? 0 : yt_min;
                if (b_yt_att < ap->sec_yt_attach[bsec_idx])
                    ap->sec_yt_attach[bsec_idx] = (int16_t)b_yt_att;
            }
        }
    }

    fixed_t open_bot = (ctx->sec_floor > bsec->floorheight) ? ctx->sec_floor : bsec->floorheight;
    if (ctx->sec_floor < bsec->floorheight || open_bot < vz)
    {
        int dzb = HW_DZQ(open_bot, vz);
        int yba, ybb;
        HW_ProjYPair(HW_VIEW_CY, dzb, s_A, dzb, s_B, &yba, &ybb);
        int yb_max = (yba > ybb) ? yba : ybb;
        p_yb = yb_max + 1;
        if (ctx->sec_floor < bsec->floorheight)
        {
            int dzb_lo = HW_DZQ(ctx->sec_floor, vz);
            int yla, ylb;
            HW_ProjYPair(HW_VIEW_CY, dzb_lo, s_A, dzb_lo, s_B, &yla, &ylb);
            int w_yb_att = (yla > ylb) ? yla : ylb;
            if (w_yb_att > yb_att) yb_att = w_yb_att;
        }
        if ((unsigned)bsec_idx < HW_MAX_AP_SECTORS)
        {
            int b_yb_att = (yb_max > HW_VIEW_H) ? HW_VIEW_H : yb_max;
            if (b_yb_att > ap->sec_yb_attach[bsec_idx])
                ap->sec_yb_attach[bsec_idx] = (int16_t)b_yb_att;
        }
    }

    if (p_yt > 0 || p_yb < HW_VIEW_H)
        HW_RecordPortalSpan(ctx->seg_sx, ctx->seg_ex, p_yt, p_yb, yt_att, yb_att);
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitVisibleBakedSeg(
    hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg,
    const hw_trans_pillar_t *p0_use, const hw_trans_pillar_t *p1_use)
{
    int tex_id = seg->tex_64;
    if (__builtin_expect(tex_id <= 0, 0))
        return;
    if (__builtin_expect(tex_id != ctx->cur_tex_id, 0))
    {
        if (!HW_LoadWallTexCtx(tex_id, ctx))
        {
            saturn_hw_tex_miss++;
            return;
        }
    }
    else if (__builtin_expect(!ctx->srca, 0))
    {
        saturn_hw_tex_miss++;
        return;
    }

    uint8_t seg_flags = seg->flags;
    ctx->n_vis++;
    int is_solid = 0;
    if (seg_flags & SEG_FLAG_SOLID)
    {
        is_solid = 1;
    }
    else if (seg->back_sec_idx >= 0 && seg->back_sec_idx < numsectors)
    {
        if (!ctx->is_sky_ceil && (int16_t)sectors[seg->back_sec_idx].ceilingpic != -1)
        {
            const sector_t *bsec = &sectors[seg->back_sec_idx];
            fixed_t mbot = (ctx->sec_floor > bsec->floorheight) ? ctx->sec_floor : bsec->floorheight;
            fixed_t mtop = (ctx->sec_ceiling < bsec->ceilingheight) ? ctx->sec_ceiling : bsec->ceilingheight;
            if (mtop <= mbot)
                is_solid = 1; /* Closed door / Raised lift perfect occluder */
        }
    }
    if (is_solid && ctx->seg_sx < ctx->seg_ex)
    {
        HW_MarkSubSolidSpan(ctx->seg_sx, ctx->seg_ex);
    }

    /* Calculate vertical texture coordinate v_top according to DOOM pegging rules */
    int th = ctx->th;
    fixed_t top_world_z = ctx->top_world_z;
    fixed_t bot_world_z = ctx->bot_world_z;
    int wall_h = (top_world_z - bot_world_z) >> FRACBITS;
    if (__builtin_expect(wall_h <= 0, 0))
        wall_h = 1; /* sub-unit riser (moving stairs/donuts): never emit 0 rows to VDP1 */
    int v_off = seg->mid_static_v_offset;
    int v_top;

    if (seg_flags & SEG_PEG_LOWER_UNPEGGED)
    {
        fixed_t peg_ceil = seg->mid_peg_height_ptr ? *seg->mid_peg_height_ptr : top_world_z;
        v_top = ((peg_ceil - top_world_z) >> FRACBITS) + v_off;
    }
    else if (seg_flags & SEG_PEG_BOTTOM)
    {
        v_top = th + v_off - ((top_world_z - ctx->orig_bot_z) >> FRACBITS);
    }
    else
    {
        v_top = v_off + ((ctx->orig_top_z - top_world_z) >> FRACBITS);
    }

    if (__builtin_expect((th & (th - 1)) == 0 && th > 0, 1))
    {
        v_top &= (th - 1);
    }
    else if (th > 0)
    {
        v_top = HW_ModNonPow2(v_top, th);
    }
    else
    {
        v_top = 0;
    }

    int srca      = ctx->srca;
    int srca_mono = ctx->srca_mono;
    int srca_64   = ctx->srca_64;
    int tw        = ctx->tw;
    int u_start   = ctx->u_start;
    int u_span    = ctx->u_span;
    int was_near_clipped = ctx->was_near_clipped;

    if (!was_near_clipped && __builtin_expect((tw & (tw - 1)) == 0 && tw > 0, 1))
    {
        u_start &= (tw - 1);
    }

    int xa = p0_use->px;
    int xb = p1_use->px;
    int dx = xb - xa;
    int view_w = saturn_view_w;
    int persp_strips = HW_NeedPerspectiveStrips(dx, u_span, p0_use->cz, p1_use->cz, tw);

    /* Force perspective subdivision on near-clipped walls to prevent extreme UV stretching */
    if (was_near_clipped && dx >= 24 && tw > 16 && saturn_wall_subdivision)
    {
        persp_strips = 1;
    }

    int mono_base_srca = srca_mono;
    int mono_tw = (tw > 64) ? 64 : tw;
    int should_subdivide = 1;
    int needs_clip = 0;

    if (!was_near_clipped)
    {
        if (tw <= 16)
        {
            if (u_start >= 0 && u_start + u_span <= tw)
            {
                should_subdivide = 0;
                needs_clip = (u_start != 0 || u_span != tw);
            }
        }
        else if (((((u_span ^ 16) | (u_start & 15)) == 0) || (!persp_strips && dx <= 2)) && (unsigned)u_start < (unsigned)tw)
        {
            should_subdivide = 0;
            needs_clip = 0;
            mono_base_srca = srca + (u_start >> 4) * (th << 1);
            mono_tw = 16;
        }
        else if (!persp_strips)
        {
            if (tw == 32 && u_start == 0 && u_span == 32 && srca_mono != srca)
            {
                should_subdivide = 0;
                needs_clip = 0;
                mono_base_srca = srca_mono;
                mono_tw = 32;
            }
            else if (tw >= 64 && ((((u_span ^ 64) | (u_start & 63)) == 0) || (u_span >= 48 && dx <= 16) || (p0_use->cz > (512 << FRACBITS) && p1_use->cz > (512 << FRACBITS) && dx <= 20)) && (unsigned)u_start < (unsigned)tw)
            {
                if (dx <= 24 && srca_mono != srca)
                {
                    should_subdivide = 0;
                    needs_clip = 0;
                    mono_base_srca = srca_mono + (u_start >> 6) * (th << 2);
                    mono_tw = 32;
                }
                else if (srca_64 != srca)
                {
                    should_subdivide = 0;
                    needs_clip = 0;
                    mono_base_srca = srca_64 + (u_start >> 6) * (th << 3);
                    mono_tw = 64;
                }
            }
        }
    }
    else if (!persp_strips && u_start == 0 && u_span == tw)
    {
        if (tw <= 16 || (tw == 32 && srca_mono != srca))
        {
            should_subdivide = 0;
            needs_clip = 0;
        }
        else if (tw == 64 && ((srca_mono != srca && dx <= 24) || srca_64 != srca))
        {
            should_subdivide = 0;
            needs_clip = 0;
            if ((dx <= 24 || srca_64 == srca) && srca_mono != srca)
            {
                mono_base_srca = srca_mono;
                mono_tw = 32;
            }
            else
            {
                mono_base_srca = srca_64;
                mono_tw = 64;
            }
        }
    }

    if (__builtin_expect(saturn_cmd_m_count + ((unsigned)u_span >> 3) + 16 >= CMDBUF_M_CAP, 0))
        return;

    int dist = (__builtin_expect(seg->baked_light < 240, 1))
        ? (((p0_use->cz + p1_use->cz) >> 1) >> FRACBITS)
        : 0;
    int k_shade = HW_FastShade(seg->baked_light, dist);

    int q_xa = xa;
    int q_xb = xb;
    const int s_A = p0_use->s;
    const int s_B = p1_use->s;

    if (!should_subdivide)
    {
        if (__builtin_expect(needs_clip, 0))
        {
            ctx->seg = seg;
            ctx->p0 = p0_use;
            ctx->p1 = p1_use;
            ctx->u_start = u_start;
            ctx->u_span = u_span;
            ctx->mono_tw = (int16_t)mono_tw;
            ctx->needs_clip = 1;
            if (!HW_ExtrapolateMonoWallX(ctx, xa, xb))
                return;
            q_xa = ctx->q_xa;
            q_xb = ctx->q_xb;
            needs_clip = ctx->needs_clip;
        }

        if (!needs_clip && ctx->last_clip_x0 != -1)
        {
            HW_EmitUserClip(0, 0, view_w - 1, HW_VIEW_H - 1);
            ctx->last_clip_x0 = -1;
            ctx->last_clip_x1 = -1;
        }

        if (q_xb <= q_xa)
            return;
    }

    uint32_t pmod = (seg_flags & SEG_FLAG_MID)
        ? (((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu)
        : ((PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu);
    uint32_t cmd_header1 = (pmod << 16) | (uint32_t)k_shade;
    uint32_t tw_stride = (uint32_t)(mono_tw >> 3);
    uint32_t cmd_size_hi = tw_stride << 8;

    const fixed_t vz = hw_viewz;
    int rem_h = wall_h;
    int cur_v = v_top;
    int dz_top = HW_DZQ(top_world_z, vz);
    int top_overlap = (seg_flags & SEG_FLAG_LOWER) ? ((wall_h <= 16) ? 0 : 1) : 1;
    int max_first_h = (th > 0) ? (th - cur_v) : rem_h;
    int is_single_band = (rem_h <= max_first_h) && (rem_h <= 255);
    int dz_bot = HW_DZQ(bot_world_z, vz);
    int bot_overlap = (seg_flags & SEG_FLAG_UPPER)
        ? ((wall_h <= 16) ? 0 : 2)
        : ((wall_h <= 16) ? 0 : 1);

    int ya0, ya1, yb0, yb1;
    HW_ProjYPair(HW_VIEW_CY, dz_top, s_A, dz_bot, s_A, &ya0, &ya1);
    HW_ProjYPair(HW_VIEW_CY, dz_top, s_B, dz_bot, s_B, &yb0, &yb1);

    if (!is_solid && (seg_flags & (SEG_FLAG_UPPER | SEG_FLAG_LOWER)))
    {
        HW_RecordSegPortalAperture(ctx, seg, s_A, s_B);
    }

    ya0 -= top_overlap;
    yb0 -= top_overlap;
    ya1 += bot_overlap;
    yb1 += bot_overlap;

    if ((ya0 >= ya1 && yb0 >= yb1) ||
        (ya1 <= ctx->ytop_min && yb1 <= ctx->ytop_min) ||
        (ya0 >= ctx->ybot_max && yb0 >= ctx->ybot_max))
        return;

    if (__builtin_expect(is_single_band && !should_subdivide, 1))
    {


        int strip_srca = (__builtin_expect(cur_v == 0, 1)) ? mono_base_srca : (mono_base_srca + cur_v * tw_stride);






        if (1)


        {
            int s_xa = q_xa;
            int s_xb = q_xb;
            int q_ya0 = ya0;
            int q_yb0 = yb0;
            int q_ya1 = ya1;
            int q_yb1 = yb1;

            if (__builtin_expect(needs_clip, 0))
            {
                hw_wall_coord_tmp[0] = s_xa;  hw_wall_coord_tmp[1] = s_xb;
                hw_wall_coord_tmp[2] = q_ya0; hw_wall_coord_tmp[3] = q_yb0;
                hw_wall_coord_tmp[4] = q_ya1; hw_wall_coord_tmp[5] = q_yb1;
                HW_ExtrapolateMonoWallY(ctx, hw_wall_coord_tmp);
                s_xa  = hw_wall_coord_tmp[0]; s_xb  = hw_wall_coord_tmp[1];
                q_ya0 = hw_wall_coord_tmp[2]; q_yb0 = hw_wall_coord_tmp[3];
                q_ya1 = hw_wall_coord_tmp[4]; q_yb1 = hw_wall_coord_tmp[5];
            }

            HW_WALL_XYCLAMP(s_xa, s_xb, q_ya0, q_yb0, q_ya1, q_yb1);
            int rows = (th > 0 && rem_h > th) ? th : rem_h;
            if (rows > 255) rows = 255;


            uint16_t m_cnt = saturn_cmd_m_count;
            uint32_t xa_p = ((uint32_t)s_xa << 16);
            uint32_t xb_p = ((uint32_t)s_xb << 16);
            uint32_t w2 = ((uint32_t)strip_srca << 16) | cmd_size_hi | ((uint32_t)rows & 0xFFu);
            uint32_t w3 = xa_p | (uint16_t)q_ya0;
            uint32_t w4 = xb_p | (uint16_t)q_yb0;
            uint32_t w5 = xb_p | (uint16_t)q_yb1;
            uint32_t w6 = xa_p | (uint16_t)q_ya1;

            if (mono_tw == 32 && tw == 32 && srca_64 != srca && !needs_clip && m_cnt > ctx->sub_m_start)
            {
                aliased_uint32_t *prev = (aliased_uint32_t *)&saturn_cmd_m[m_cnt - 1];
                if (prev[1] == cmd_header1 && prev[2] == w2 &&
                    prev[4] == w3 && prev[5] == w6 &&
                    s_xb - (int)(int16_t)(prev[3] >> 16) <= 48)
                {
                    int strip_srca64 = (__builtin_expect(cur_v == 0, 1)) ? srca_64 : (srca_64 + (cur_v << 3));
                    prev[2] = ((uint32_t)strip_srca64 << 16) | (8u << 8) | ((uint32_t)rows & 0xFFu);
                    prev[4] = w4;
                    prev[5] = w5;
                    return;
                }
            }

            if (m_cnt < CMDBUF_M_CAP - 1)
            {
                saturn_cmd_m_count = m_cnt + 1;
                aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                cmd32[0] = 0x00020000u;
                cmd32[1] = cmd_header1;
                cmd32[2] = w2;
                cmd32[3] = w3;

                saturn_v1w_cells++;
                saturn_hw_wall_quads++;

                cmd32[4] = w4;
                cmd32[5] = w5;
                cmd32[6] = w6;
                cmd32[7] = 0;
            }
            else
            {
                saturn_v1w_drop++;
                saturn_v1w_captured = 1;
            }
        }
    }
    else
    {
        ctx->seg = seg;
        ctx->p0 = p0_use;
        ctx->p1 = p1_use;
        ctx->u_start = u_start;
        ctx->u_span = u_span;
        ctx->mono_tw = (int16_t)mono_tw;
        ctx->needs_clip = (uint8_t)needs_clip;
        ctx->cmd_header1 = cmd_header1;
        ctx->dz_top = dz_top;
        ctx->dz_bot = dz_bot;
        ctx->top_overlap = (int16_t)top_overlap;
        ctx->bot_overlap = (int16_t)bot_overlap;
        ctx->cur_v = (int16_t)cur_v;
        ctx->mono_base_srca = (uint16_t)mono_base_srca;
        ctx->should_subdivide = (uint8_t)should_subdivide;
        ctx->persp_strips = (uint8_t)persp_strips;
        ctx->cur_top_z = top_world_z;
        ctx->rem_h = (int16_t)rem_h;
        ctx->wall_h = (int16_t)wall_h;
        ctx->q_xa = (int16_t)q_xa;
        ctx->q_xb = (int16_t)q_xb;
        HW_DeferOrEmitSubdividedWall(ctx, is_single_band);
    }
}

/* Master Blueprint: Zero-Runtime-Math 1D LOD Subsector Wall Renderer (Phase 3)
 * Fast Culling & Pillar Projection Loop (< 1.5 KB): rejects ~72% of segs in registers
 * and dispatches surviving visible segs to HW_EmitVisibleBakedSeg. */
static HW_NOINLINE HW_HOT_O2 int HW_RenderSubsectorBakedWalls(int subnum, sector_t *sec)
{
    static hw_trans_pillar_t local_trans_pillars[HW_MAX_SUBSECTOR_PILLARS];
    static uint32_t pil_transformed[3];
    static hw_wall_emit_ctx_t wctx;

    if (!saturn_baked_walls_active || (unsigned)subnum >= (unsigned)saturn_baked_wall_num_subsectors)
        return 0;

    const saturn_wl_subsector_t *ss = &saturn_baked_wall_subsectors[subnum];
    int num_segs = (int)ss->num_segs;
    if (num_segs <= 0)
        return 1;
    int num_pil = (int)ss->num_pillars;
    if (num_pil <= 0 || num_pil > HW_MAX_SUBSECTOR_PILLARS)
        return 0;

    const saturn_wl_pillar_t *pillars = &saturn_baked_wall_pillars[ss->pillar_start];
    const uint8_t *pfrac = saturn_baked_wall_pillar_frac ? &saturn_baked_wall_pillar_frac[ss->pillar_start] : NULL;
    const baked_seg_t *all_segs = &saturn_baked_wall_segs[ss->seg_start];

    pil_transformed[0] = 0;
    pil_transformed[1] = 0;
    pil_transformed[2] = 0;

    const fixed_t sec_ceiling = sec->ceilingheight;
    const fixed_t sec_floor   = sec->floorheight;
    const int is_sky_ceil     = ((int16_t)sec->ceilingpic == -1);
    const int view_w          = saturn_view_w;
    const int cam_x           = hw_cam_x;
    const int cam_y           = hw_cam_y;

    wctx.sec_ceiling  = sec_ceiling;
    wctx.sec_floor    = sec_floor;
    wctx.is_sky_ceil  = (uint8_t)is_sky_ceil;
    wctx.cur_tex_id   = -1;
    wctx.last_clip_x0 = -1;
    wctx.last_clip_x1 = -1;
    wctx.n_vis        = 0;
    wctx.sub_m_start  = saturn_cmd_m_count;
    wctx.ytop_min     = 0;
    wctx.ybot_max     = (int16_t)HW_VIEW_H;
    hw_def_wall_count = 0;

    uint32_t last_pair_key = 0xFFFFFFFFu;
    int last_pair_state = 0; /* 0 = none, -1 = rejected, 1 = front-facing, 2 = projected & visible */
    uint8_t cached_occl_state = 0;
    int16_t cached_ytop_min = 0;
    int16_t cached_ybot_max = (int16_t)HW_VIEW_H;
    int i;

    for (i = 0; i < num_segs; i++)
    {
        if (hw_solid_count >= view_w)
            break;

        const baked_seg_t *seg = &all_segs[i];
        uint8_t seg_flags = seg->flags;
        if (!seg_flags)
            continue;

        int idx0 = seg->pillar_idx[0];
        int idx1 = seg->pillar_idx[1];
        uint32_t pair_key = ((uint32_t)idx0 << 16) | (uint32_t)idx1;

        if (pair_key == last_pair_key)
        {
            if (last_pair_state < 0)
                continue;
        }
        else
        {
            last_pair_key = pair_key;
            if (idx0 >= num_pil || idx1 >= num_pil)
            {
                last_pair_state = -1;
                continue;
            }

            const saturn_wl_pillar_t *p0_raw = &pillars[idx0];
            const saturn_wl_pillar_t *p1_raw = &pillars[idx1];
            int p0x = (int)p0_raw->x;
            int p0z = (int)p0_raw->z;
            int seg_dx = (int)p1_raw->x - p0x;
            int seg_dy = (int)p1_raw->z - p0z;
            int obj_dx = p0x - cam_x;
            int obj_dy = p0z - cam_y;

            if (HW_CrossBackface(obj_dy, seg_dx, obj_dx, seg_dy) <= 0)
            {
                last_pair_state = -1;
                continue;
            }
            last_pair_state = 1;
        }

        /* Early zero-height and sky-hack rejection BEFORE 3D pillar projection */
        int occlude_only = 0;
        if (!(seg_flags & (SEG_FLAG_LOWER | SEG_FLAG_UPPER | SEG_FLAG_MID)))
        {
            if (sec_ceiling <= sec_floor)
                continue;
            wctx.top_world_z = sec_ceiling;
            wctx.bot_world_z = sec_floor;
            wctx.orig_top_z  = sec_ceiling;
            wctx.orig_bot_z  = sec_floor;
        }
        else
        {
            if (!seg->dynamic_top_z_ptr || !seg->dynamic_bottom_z_ptr)
                continue;

            fixed_t top_world_z = *seg->dynamic_top_z_ptr;
            fixed_t bot_world_z = *seg->dynamic_bottom_z_ptr;
            wctx.orig_top_z = top_world_z;
            wctx.orig_bot_z = bot_world_z;
            if (top_world_z > sec_ceiling)
                top_world_z = sec_ceiling;
            if (bot_world_z < sec_floor)
                bot_world_z = sec_floor;
            if (top_world_z <= bot_world_z)
            {
                if (seg->back_sec_idx >= 0 && seg->back_sec_idx < numsectors)
                {
                    const sector_t *bsec = &sectors[seg->back_sec_idx];
                    fixed_t mbot = (sec_floor > bsec->floorheight) ? sec_floor : bsec->floorheight;
                    fixed_t mtop = (sec_ceiling < bsec->ceilingheight) ? sec_ceiling : bsec->ceilingheight;
                    if (!is_sky_ceil && (int16_t)bsec->ceilingpic != -1 && mtop <= mbot)
                    {
                        occlude_only = 1;
                    }
                    else if (((seg_flags & SEG_FLAG_LOWER) && sec_floor > bsec->floorheight && sec_floor < hw_viewz) ||
                             ((seg_flags & SEG_FLAG_UPPER) && !is_sky_ceil && (int16_t)bsec->ceilingpic != -1 &&
                              sec_ceiling < bsec->ceilingheight && sec_ceiling > hw_viewz))
                    {
                        occlude_only = 2;
                    }
                }
                if (!occlude_only)
                    continue;
            }
            wctx.top_world_z = top_world_z;
            wctx.bot_world_z = bot_world_z;
        }

        const hw_trans_pillar_t *p0_use = &local_trans_pillars[idx0];
        const hw_trans_pillar_t *p1_use = &local_trans_pillars[idx1];
        wctx.u_start = seg->u_start;
        wctx.u_span  = (seg->u_span > 0) ? (int)seg->u_span : 64;
        wctx.was_near_clipped = 0;

        if (last_pair_state == 2)
        {
            int xa = p0_use->px;
            int xb = p1_use->px;
            wctx.seg_sx = (int16_t)((xa < 0) ? 0 : xa);
            wctx.seg_ex = (int16_t)((xb > view_w) ? view_w : xb);
            wctx.occl_state = cached_occl_state;
            wctx.ytop_min = cached_ytop_min;
            wctx.ybot_max = cached_ybot_max;
        }
        else
        {
            if (!(pil_transformed[idx0 >> 5] & hw_bit_lut[idx0 & 31]))
            {
                pil_transformed[idx0 >> 5] |= hw_bit_lut[idx0 & 31];
                HW_TransformSubsectorPillar(&local_trans_pillars[idx0], &pillars[idx0], pfrac ? pfrac[idx0] : 0);
            }

            if (!(pil_transformed[idx1 >> 5] & hw_bit_lut[idx1 & 31]))
            {
                pil_transformed[idx1 >> 5] |= hw_bit_lut[idx1 & 31];
                HW_TransformSubsectorPillar(&local_trans_pillars[idx1], &pillars[idx1], pfrac ? pfrac[idx1] : 0);
            }

            if ((p0_use->cz < HW_NEAR_Z && p1_use->cz < HW_NEAR_Z) ||
                (p0_use->cz > HW_FAR_Z  && p1_use->cz > HW_FAR_Z))
            {
                last_pair_state = -1;
                continue;
            }

            if (__builtin_expect(p0_use->cz >= HW_NEAR_Z && p1_use->cz >= HW_NEAR_Z, 1))
            {
                int xa = p0_use->px;
                int xb = p1_use->px;
                if (xb <= xa || xb <= 0 || xa >= view_w)
                {
                    last_pair_state = -1;
                    continue;
                }
                int seg_sx = (xa < 0) ? 0 : xa;
                int seg_ex = (xb > view_w) ? view_w : xb;
                int occl_state = (hw_solid_count > 0) ? HW_CheckSpanOcclusion(seg_sx, seg_ex) : 0;
                if (occl_state == 2)
                {
                    last_pair_state = -1;
                    continue;
                }
                wctx.seg_sx = (int16_t)seg_sx;
                wctx.seg_ex = (int16_t)seg_ex;
                wctx.occl_state = (uint8_t)occl_state;
                cached_occl_state = (uint8_t)occl_state;
                if (HW_GetSpanAperture(seg_sx, seg_ex, &wctx.ytop_min, &wctx.ybot_max))
                {
                    last_pair_state = -1;
                    continue;
                }
                cached_ytop_min = wctx.ytop_min;
                cached_ybot_max = wctx.ybot_max;
                last_pair_state = 2;
            }
            else
            {
                if (!HW_ClipWallNearPlane(p0_use, p1_use, &wctx))
                    continue;
                p0_use = &hw_p0_clipped;
                p1_use = &hw_p1_clipped;
                if (HW_GetSpanAperture(wctx.seg_sx, wctx.seg_ex, &wctx.ytop_min, &wctx.ybot_max))
                    continue;
            }
        }

        if (__builtin_expect(occlude_only, 0))
        {
            if (occlude_only == 1)
            {
                if (wctx.seg_sx < wctx.seg_ex)
                    HW_MarkSubSolidSpan(wctx.seg_sx, wctx.seg_ex);
            }
            else
            {
                HW_RecordSegPortalAperture(&wctx, seg, p0_use->s, p1_use->s);
            }
            continue;
        }

        HW_EmitVisibleBakedSeg(&wctx, seg, p0_use, p1_use);
    }

    if (hw_def_wall_count > 0)
    {
        HW_FlushDeferredSubsectorWalls(&wctx);
    }
    saturn_hw_seg_iter += (unsigned int)i;
    saturn_hw_seg_vis  += (unsigned int)wctx.n_vis;
    if (wctx.last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, view_w - 1, HW_VIEW_H - 1);
        wctx.last_clip_x0 = -1;
        wctx.last_clip_x1 = -1;
    }
    return 1;
}

/* Walk a subsector */
static HW_NOINLINE HW_HOT_O2 int HW_CheckSubsectorAperture(int subnum, const sector_t *sec)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    ap->cur_sub_yt_attach = 0;
    ap->cur_sub_yb_attach = (int16_t)HW_VIEW_H;
    if (!ap->aperture_active || !saturn_baked_flat_bboxes)
        return 3;

    const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
    if (fb->min_x == fb->max_x && fb->min_y == fb->max_y)
        return 3;

    const fixed_t vx = hw_viewx, vy = hw_viewy;
    const fixed_t cs = hw_viewcos, sn = hw_viewsin;
    fixed_t dx0 = ((fixed_t)fb->min_x << FRACBITS) - vx;
    fixed_t dx1 = ((fixed_t)fb->max_x << FRACBITS) - vx;
    fixed_t dy0 = ((fixed_t)fb->min_y << FRACBITS) - vy;
    fixed_t dy1 = ((fixed_t)fb->max_y << FRACBITS) - vy;

    fixed_t x0c = FixedMul(dx0, cs), x1c = FixedMul(dx1, cs);
    fixed_t y0s = FixedMul(dy0, sn), y1s = FixedMul(dy1, sn);
    fixed_t x0s = FixedMul(dx0, sn), x1s = FixedMul(dx1, sn);
    fixed_t y0c = FixedMul(dy0, cs), y1c = FixedMul(dy1, cs);

    fixed_t cz[4], cx[4];
    cz[0] = x0c + y0s; cx[0] = x0s - y0c;
    cz[1] = x1c + y0s; cx[1] = x1s - y0c;
    cz[2] = x1c + y1s; cx[2] = x1s - y1c;
    cz[3] = x0c + y1s; cx[3] = x0s - y1c;

    const int vw = saturn_view_w;
    const int cx_mid = saturn_view_cx;
    int sx = vw, ex = 0;
    fixed_t min_cz = 0x7FFFFFFF;
    fixed_t max_cz = 0;
    int any_behind = 0;

    for (int k = 0; k < 4; k++)
    {
        if (cz[k] > max_cz) max_cz = cz[k];
        if (cz[k] < HW_NEAR_Z)
        {
            any_behind = 1;
            if (HW_NEAR_Z < min_cz) min_cz = HW_NEAR_Z;
        }
        else
        {
            if (cz[k] < min_cz) min_cz = cz[k];
            int px = HW_ProjX(cx[k], HW_FastDiv(cz[k]), cx_mid);
            if (px < sx) sx = px;
            if (px > ex) ex = px;
        }
    }

    if (max_cz < HW_NEAR_Z)
        return 3;
    if (any_behind)
    {
        sx = 0;
        ex = vw;
    }
    else
    {
        sx -= 2;
        ex += 2;
        if (sx < 0) sx = 0;
        if (ex > vw) ex = vw;
    }
    if (sx >= ex)
        return 3;

    int16_t ytop_min, ybot_max;
    if (HW_GetSpanAperture(sx, ex, &ytop_min, &ybot_max))
        return -1;
    if (ytop_min <= 0 && ybot_max >= HW_VIEW_H)
        return 3;

    int s_max = HW_FastDiv(min_cz);
    int s_min = HW_FastDiv(max_cz);
    const fixed_t vz = hw_viewz;
    int allow_floor = 1;
    int allow_ceil  = 1;
    int secnum = subsectors[subnum].sector;
    int valid_sec = ((unsigned)secnum < HW_MAX_AP_SECTORS);

    if (sec->floorheight < vz)
    {
        int dzf = HW_DZQ(sec->floorheight, vz);
        int yf_min, yf_max;
        HW_ProjYPair(HW_VIEW_CY, dzf, s_min, dzf, s_max, &yf_min, &yf_max);
        if (!any_behind && sec != hw_psec)
        {
            int yb_att = (yf_max > HW_VIEW_H) ? HW_VIEW_H : yf_max;
            if (valid_sec)
            {
                if (yb_att > ap->sec_yb_attach[secnum])
                    ap->sec_yb_attach[secnum] = (int16_t)yb_att;
                yb_att = ap->sec_yb_attach[secnum];
            }
            ap->cur_sub_yb_attach = (int16_t)yb_att;
        }
        else if (valid_sec)
        {
            ap->sec_yb_attach[secnum] = (int16_t)HW_VIEW_H;
        }
        if (yf_min - 2 >= ybot_max || yf_max + 2 <= ytop_min)
            allow_floor = 0;
    }
    else if (!any_behind && sec != hw_psec)
    {
        ap->cur_sub_yb_attach = valid_sec ? ap->sec_yb_attach[secnum] : 0;
    }
    if ((int16_t)sec->ceilingpic != -1 && sec->ceilingheight > vz)
    {
        int dzc = HW_DZQ(sec->ceilingheight, vz);
        int yc_min, yc_max;
        HW_ProjYPair(HW_VIEW_CY, dzc, s_max, dzc, s_min, &yc_min, &yc_max);
        if (!any_behind && sec != hw_psec)
        {
            int yt_att = (yc_min < 0) ? 0 : yc_min;
            if (valid_sec)
            {
                if (yt_att < ap->sec_yt_attach[secnum])
                    ap->sec_yt_attach[secnum] = (int16_t)yt_att;
                yt_att = ap->sec_yt_attach[secnum];
            }
            ap->cur_sub_yt_attach = (int16_t)yt_att;
        }
        else if (valid_sec)
        {
            ap->sec_yt_attach[secnum] = 0;
        }
        if (yc_max + 2 <= ytop_min || yc_min - 2 >= ybot_max)
            allow_ceil = 0;
    }
    else if (!any_behind && sec != hw_psec)
    {
        ap->cur_sub_yt_attach = valid_sec ? ap->sec_yt_attach[secnum] : (int16_t)HW_VIEW_H;
    }

    if (!sec->thinglist && (int16_t)sec->ceilingpic != -1)
    {
        int dzt = HW_DZQ(sec->ceilingheight, vz);
        int dzb = HW_DZQ(sec->floorheight, vz);
        int sub_y_min, sub_y_max;
        HW_ProjYPair(HW_VIEW_CY, dzt, (dzt >= 0) ? s_max : s_min, dzb, (dzb <= 0) ? s_max : s_min, &sub_y_min, &sub_y_max);
        if (sub_y_min - 2 >= ybot_max || sub_y_max + 2 <= ytop_min)
            return -1;
    }

    return allow_floor | (allow_ceil << 1);
}

/* Walk a subsector */
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsector(int subnum, const byte *pvs_row, int fully_inside)
{
    subsector_t *sub;
    sector_t *sec;
    int count, i;
    seg_t *seg;

    if (hw_solid_count >= saturn_view_w || subnum < 0 || subnum >= numsubsectors)
        return;

    /* PVS gate: skip subsectors not potentially visible from viewer's leaf */
    if (pvs_row != NULL && !(pvs_row[subnum >> 3] & hw_bit_lut[subnum & 7]))
        return;

    /* Subsector View Frustum & Occlusion Culling (fallback when hw_node_bboxes is NULL) */
    if (__builtin_expect(!hw_node_bboxes && saturn_baked_flat_bboxes != NULL, 0))
    {
        const saturn_flat_bbox_t *bbox = &saturn_baked_flat_bboxes[subnum];
        if (bbox->min_x != bbox->max_x || bbox->min_y != bbox->max_y)
        {
            int16_t sbox[4];
            sbox[BOXLEFT]   = bbox->min_x;
            sbox[BOXRIGHT]  = bbox->max_x;
            sbox[BOXBOTTOM] = bbox->min_y;
            sbox[BOXTOP]    = bbox->max_y;
            if (!fully_inside)
            {
                int st = HW_CheckBBoxStatus(sbox);
                if (st == BBOX_OUTSIDE)
                    return;
                if (st == BBOX_FULLY_INSIDE)
                    fully_inside = 1;
            }
            if (saturn_hw_bsp_occl != 2 && hw_solid_count > 0 && HW_CheckBBoxOccluded(sbox))
            {
                saturn_hw_occl_culls++;
                return;
            }
        }
    }

    sub = &subsectors[subnum];
    if (sub->sector < 0 || sub->sector >= numsectors)
        return;
    if (saturn_visit_count >= CMDBUF_MAX_VISIT)
        return;
    sec = &sectors[sub->sector];
    int flat_mask = HW_CheckSubsectorAperture(subnum, sec);
    if (flat_mask < 0)
    {
        saturn_hw_occl_culls++;
        return;
    }
    int flat_flags = (fully_inside & 3) | (flat_mask << 2);

    if ((int16_t)sec->ceilingpic == -1)
    {
        saturn_hw_sky_visible = 1;
    }

    extern unsigned char saturn_slave_enabled;
    sub_range_t *ranges = hw_frame_slave_enabled
        ? (sub_range_t *)((uint32_t)saturn_sub_ranges | SATURN_WRAM_H_UNCACHED)
        : saturn_sub_ranges;

    int visit_idx = saturn_visit_count++;
    uint16_t m_start = saturn_cmd_m_count;
    ranges[visit_idx].s_start = 0;
    ranges[visit_idx].s_count = 0;
    ranges[visit_idx].subnum = (uint16_t)subnum;

    int q_count_sub = saturn_baked_flat_q_count ? (int)saturn_baked_flat_q_count[subnum] : 0;
    int has_flats = (q_count_sub > 0) &&
        (((flat_mask & 2) && hw_viewz < sec->ceilingheight && (int16_t)sec->ceilingpic != -1) ||
         ((flat_mask & 1) && hw_viewz > sec->floorheight && (int16_t)sec->floorpic != -1));



    int flats_handled = 0;
    if (saturn_slave_enabled && has_flats)
    {
        flats_handled = 1;
        /* Concurrent execution: Submit to Slave so Slave processes flats while Master computes wall geometry */
        HW_SubmitSlaveFlatJob(visit_idx, subnum, sec, flat_flags);
    }

    /* 1. Render segs (front walls of this subsector) */
    int handled_baked = 0;
    if (__builtin_expect(saturn_baked_walls_active != 0, 1))
    {
        unsigned short tw0 = SAT_FRT();
        handled_baked = HW_RenderSubsectorBakedWalls(subnum, sec);
        unsigned short tw1 = SAT_FRT();
        if (__builtin_expect(tw1 >= tw0, 1))
        {
            saturn_hw_prof_segs += (unsigned int)(tw1 - tw0);
        }
        else
        {
            extern volatile int saturn_frt_vbl_reset;
            extern volatile int saturn_frt_rate;
            unsigned int dt = (unsigned int)(unsigned short)(tw1 - tw0);
            if (saturn_frt_vbl_reset)
            {
                int span = saturn_frt_rate + (int)tw1 - (int)tw0;
                dt = (span > 0 && span <= saturn_frt_rate) ? (unsigned int)span : (unsigned int)tw1;
            }
            saturn_hw_prof_segs += dt;
        }
    }
    if (__builtin_expect(!handled_baked, 0))
    {
        count = (subnum == numsubsectors - 1) ? (numsegs - sub->firstline) : SSEC_NUMLINES(sub);
        if (count <= 0 || count > 1024)
            count = 0;
        if (sub->firstline >= 0 && sub->firstline + count <= numsegs)
        {
            hw_shade_ctx_t seg_shade_ctx;
            HW_InitShadeCtx(&seg_shade_ctx, sec->lightlevel);

            seg = &segs[sub->firstline];
            for (i = 0; i < count; i++, seg++)
            {
                HW_RenderSeg(seg, sec, &seg_shade_ctx);
            }
        }
    }

    /* 2. Render subsector sprites (lazy per-sector frustum-culled bucketing) */
    if (sec->thinglist)
    {
        int secnum = sub->sector;
        if ((unsigned)secnum < 512u)
        {
            uint32_t sec_bit = hw_bit_lut[secnum & 31];
            if (__builtin_expect(!(hw_sec_things_built[secnum >> 5] & sec_bit), 0))
            {
                hw_sec_things_built[secnum >> 5] |= sec_bit;
                HW_BuildSectorThings(sec);
            }
            if (__builtin_expect((hw_sub_has_things[subnum >> 5] & hw_bit_lut[subnum & 31]) != 0, 0))
            {
                HW_RenderSubsectorSprites(sec, subnum);
            }
        }
    }

    ranges[visit_idx].m_start = m_start;
    ranges[visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);

    /* 3. Render pre-baked floor & ceiling quads (records ranges[visit_idx].s_start/s_count inside HW_Worker_ProcessJob) */
    if (__builtin_expect(!flats_handled && has_flats, 0))
    {
        HW_RenderSubsectorBakedFlats(visit_idx, subnum, sec, flat_flags);
    }

    /* 4. Mark solid spans and portal apertures from this subsector */
    if (sub_solid_active || HW_APERTURE->portal_span_count > 0)
    {
        HW_CommitSubsectorOcclusion();
    }
}

typedef struct {
    uint16_t bspnum;       /* must be unsigned: node_t children are uint16_t (NF_SUBSECTOR = 0x8000) */
    int16_t  fully_inside;
    const int16_t *bbox;
} bsp_stack_t;

#define HW_BSP_STACK_MAX 128

/* Front-to-back iterative BSP tree traversal with zero C-call overhead and deferred occlusion */
static HW_NOINLINE HW_HOT_O2 void HW_RenderBSPNode(int root_bspnum, const byte *pvs_row, int root_inside)
{
    static bsp_stack_t bsp_stack[HW_BSP_STACK_MAX];
    int sp = 0;

    bsp_stack[sp].bspnum = root_bspnum;
    bsp_stack[sp].fully_inside = root_inside;
    bsp_stack[sp].bbox = NULL;
    sp++;

    while (sp > 0)
    {
        if (hw_solid_count >= saturn_view_w) {
            saturn_hw_bsp_abort++;
            return;
        }

        sp--;
        int bspnum = bsp_stack[sp].bspnum;
        int fully_inside = bsp_stack[sp].fully_inside;
        const int16_t *bbox = bsp_stack[sp].bbox;

        /* Deferred Frustum & Occlusion Check: Evaluate back-children only AFTER the front 
         * subtree has rendered. If the front walls filled the screen or occlude this box, discard it. */
        if (bbox) {
            if (!fully_inside) {
                int st = HW_CheckBBoxStatus(bbox);
                if (st == BBOX_OUTSIDE)
                    continue;
                if (st == BBOX_FULLY_INSIDE)
                    fully_inside = 1;
            }
            if (saturn_hw_bsp_occl != 2 && hw_solid_count > 0 && HW_CheckBBoxOccluded(bbox)) {
                saturn_hw_occl_culls++;
                continue;
            }
        }

        while (1)
        {
            if (hw_solid_count >= saturn_view_w) {
                saturn_hw_bsp_abort++;
                return;
            }

            /* Leaf check: MARS encoding has bit 15 set */
            if ((int16_t)bspnum < 0) {
                if ((int16_t)bspnum != -1) {
                    int subnum = bspnum & 0x7FFFu;
                    HW_RenderSubsector(subnum, pvs_row, fully_inside);
                }
                break;
            }
            if (bspnum >= numnodes) break;

            node_t *bsp = &nodes[bspnum];

            /* R_PointOnSide is already static inline (2x muls.w). node_t
             * x/y/dx/dy are int16_t map units, not 16.16 - do not mix them
             * with hw_viewx directly. */
            int side = R_PointOnSide(hw_viewx, hw_viewy, bsp);

            const int16_t (*bboxes)[4] = hw_node_bboxes ? hw_node_bboxes[bspnum] : NULL;
            int child_front = (int)bsp->children[side];
            int child_back  = (int)bsp->children[side ^ 1];

            /* -- 1. EVALUATE BACK CHILD (PUSH TO STACK) -- */
            int push_back = 1;
            const int16_t *cbbox_back = bboxes ? bboxes[side ^ 1] : NULL;

            if ((int16_t)child_back == -1) {
                push_back = 0;
            } else if ((int16_t)child_back < 0) {
                int ssec = child_back & 0x7FFFu;
                if (pvs_row && !(pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]))
                    push_back = 0;
            } else if (pvs_row && (unsigned)child_back < (unsigned)numnodes) {
                if (!(HW_APERTURE->node_pvs_vis[child_back >> 5] & hw_bit_lut[child_back & 31]))
                    push_back = 0;
            }

            if (push_back && sp < HW_BSP_STACK_MAX) {
                bsp_stack[sp].bspnum = child_back;
                bsp_stack[sp].fully_inside = fully_inside;
                bsp_stack[sp].bbox = cbbox_back; /* Deferred frustum & occlusion check when popped */
                sp++;
            }

            /* -- 2. EVALUATE FRONT CHILD (TAIL-CALL LOOP) -- */
            if ((int16_t)child_front == -1) {
                break;
            } else if ((int16_t)child_front < 0) {
                int ssec = child_front & 0x7FFFu;
                if (pvs_row && !(pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]))
                    break;
            } else if (pvs_row && (unsigned)child_front < (unsigned)numnodes) {
                if (!(HW_APERTURE->node_pvs_vis[child_front >> 5] & hw_bit_lut[child_front & 31]))
                    break;
            }
            const int16_t *cbbox_front = bboxes ? bboxes[side] : NULL;
            if (cbbox_front &&
                (hw_cam_x < cbbox_front[BOXLEFT] || hw_cam_x > cbbox_front[BOXRIGHT] ||
                 hw_cam_y < cbbox_front[BOXBOTTOM] || hw_cam_y > cbbox_front[BOXTOP]))
            {
                if (!fully_inside) {
                    int st = HW_CheckBBoxStatus(cbbox_front);
                    if (st == BBOX_OUTSIDE)
                        break;
                    if (st == BBOX_FULLY_INSIDE)
                        fully_inside = 1;
                }
                if (saturn_hw_bsp_occl != 2 && hw_solid_count > 0 && HW_CheckBBoxOccluded(cbbox_front)) {
                    saturn_hw_occl_culls++;
                    break;
                }
            }
            bspnum = child_front;
        }
    }
}

/* Main Hardware Renderer entry point */
HW_HOT_O2 void Saturn_HW_RenderPlayerView(int displayplayer)
{
    const byte *pvs_row;
    if ((unsigned)displayplayer >= MAXPLAYERS || !players[displayplayer].mo)
        displayplayer = 0;
    hw_displayplayer = displayplayer;
    player_t *player = &players[displayplayer];

    if (!I_VideoBuffer || numnodes <= 0 || numlines <= 0 || !player->mo)
        return;

    HW_Divlut_Init();

    /* Latch player camera state with 60 Hz inter-tic interpolation.
     * Keyed on gametic, NOT vblank parity: at 30 FPS every frame lands
     * on the same parity, and an odd lock froze prev_view* at the spawn
     * point (camera stuck halfway between spawn and the player, own
     * mobj visible ahead). Midpoint only on the first frame of a new
     * tic that follows a mid-tic frame (true 60 FPS); otherwise cur.
     * interp_key = gametic*2 + (1 if that frame ran no tic). The lerp
     * is prev + (cur-prev)/2: exact cur when prev == cur, and no
     * (cur+prev) overflow past +/-16384 map units. */
    static fixed_t prev_viewx, prev_viewy, prev_viewz;
    static angle_t prev_viewangle;
    static int interp_key;
    int interp_g2 = gametic << 1;

    fixed_t cur_vx = player->mo->x;
    fixed_t cur_vy = player->mo->y;
    fixed_t cur_vz = player->viewz;
    angle_t cur_va = player->mo->angle;

    if (interp_key != interp_g2 - 1)
    {
        interp_key = interp_g2 + ((interp_key >> 1) == gametic);
        prev_viewx = cur_vx;
        prev_viewy = cur_vy;
        prev_viewz = cur_vz;
        prev_viewangle = cur_va;
    }
    else
        interp_key = interp_g2;

    /* Teleport / respawn: never blend across a discontinuity (one frame halfway
     * between source and destination). No legal per-tic move exceeds 128 units. */
#define HW_INTERP_SNAP ((uint32_t)(128 << FRACBITS))
    if (((uint32_t)cur_vx - (uint32_t)prev_viewx + HW_INTERP_SNAP > 2u * HW_INTERP_SNAP) ||
        ((uint32_t)cur_vy - (uint32_t)prev_viewy + HW_INTERP_SNAP > 2u * HW_INTERP_SNAP))
    {
        prev_viewx = cur_vx;
        prev_viewy = cur_vy;
        prev_viewz = cur_vz;
        prev_viewangle = cur_va;
    }
#undef HW_INTERP_SNAP
    hw_viewx = prev_viewx + ((cur_vx - prev_viewx) >> 1);
    hw_viewy = prev_viewy + ((cur_vy - prev_viewy) >> 1);
    hw_viewz = prev_viewz + ((cur_vz - prev_viewz) >> 1);
    hw_viewangle = prev_viewangle + (((int)(cur_va - prev_viewangle)) >> 1);

    hw_cam_x     = hw_viewx >> FRACBITS;
    hw_cam_y     = hw_viewy >> FRACBITS;
    hw_viewsin   = finesine(hw_viewangle >> ANGLETOFINESHIFT);
    hw_viewcos   = finecosine(hw_viewangle >> ANGLETOFINESHIFT);
    {
        fixed_t frac_x = hw_viewx - (hw_cam_x << FRACBITS);
        fixed_t frac_y = hw_viewy - (hw_cam_y << FRACBITS);
        cam_cz_origin = FixedMul(frac_x, hw_viewcos) + FixedMul(frac_y, hw_viewsin);
        cam_cx_origin = FixedMul(frac_x, hw_viewsin) - FixedMul(frac_y, hw_viewcos);
    }

    if (saturn_vdp2_plane_active)
    {
        Saturn_VDP2_Plane_Frame(hw_viewx, hw_viewy, hw_viewz, hw_viewangle, saturn_vdp2_plane_floorz);
    }
    /* Unconditionally fetch true rendering subsector at 60 FPS interpolated coordinates
     * so crossing a doorway between 35Hz game tics never uses a stale PVS row. */
    subsector_t *ssec = R_PointInSubsector(hw_viewx, hw_viewy);
    hw_psec      = (ssec && ssec->sector >= 0 && ssec->sector < numsectors)
                 ? SSEC_SECTOR(ssec)
                 : (numsectors > 29 ? &sectors[29] : (numsectors > 0 ? &sectors[0] : NULL));
    hw_is_sky    = (hw_psec && ((int16_t)hw_psec->ceilingpic == -1));

    /* Setup 2D map-space AABB frustum planes (12-bit fixed point) */
    {
        int vx_m = hw_viewx >> FRACBITS;
        int vy_m = hw_viewy >> FRACBITS;
        hw_bbox_Nx = hw_viewcos >> 4;
        hw_bbox_Ny = hw_viewsin >> 4;
        /* FIX: Apply 1.25x scaling to align with the actual 3D FOV */
        hw_bbox_Lx = (hw_viewsin + hw_viewcos + (hw_viewcos >> 2)) >> 4;
        hw_bbox_Ly = (hw_viewsin - hw_viewcos + (hw_viewsin >> 2)) >> 4;
        hw_bbox_Rx = (hw_viewcos - hw_viewsin + (hw_viewcos >> 2)) >> 4;
        hw_bbox_Ry = (hw_viewsin + hw_viewcos + (hw_viewsin >> 2)) >> 4;

        hw_bbox_Cn = -((vx_m * hw_bbox_Nx) + (vy_m * hw_bbox_Ny));
        hw_bbox_Cl = -((vx_m * hw_bbox_Lx) + (vy_m * hw_bbox_Ly));
        hw_bbox_Cr = -((vx_m * hw_bbox_Rx) + (vy_m * hw_bbox_Ry));

        hw_bbox_idx_Nx = (hw_viewcos > 0) ? BOXRIGHT : BOXLEFT;
        hw_bbox_idx_Ny = (hw_viewsin > 0) ? BOXTOP   : BOXBOTTOM;
        hw_bbox_idx_Lx = (hw_bbox_Lx > 0) ? BOXRIGHT : BOXLEFT;
        hw_bbox_idx_Ly = (hw_bbox_Ly > 0) ? BOXTOP   : BOXBOTTOM;
        hw_bbox_idx_Rx = (hw_bbox_Rx > 0) ? BOXRIGHT : BOXLEFT;
        hw_bbox_idx_Ry = (hw_bbox_Ry > 0) ? BOXTOP   : BOXBOTTOM;

        hw_bbox_nidx_Nx = (hw_bbox_idx_Nx == BOXRIGHT) ? BOXLEFT : BOXRIGHT;
        hw_bbox_nidx_Ny = (hw_bbox_idx_Ny == BOXTOP)   ? BOXBOTTOM : BOXTOP;
        hw_bbox_nidx_Lx = (hw_bbox_idx_Lx == BOXRIGHT) ? BOXLEFT : BOXRIGHT;
        hw_bbox_nidx_Ly = (hw_bbox_idx_Ly == BOXTOP)   ? BOXBOTTOM : BOXTOP;
        hw_bbox_nidx_Rx = (hw_bbox_idx_Rx == BOXRIGHT) ? BOXLEFT : BOXRIGHT;
        hw_bbox_nidx_Ry = (hw_bbox_idx_Ry == BOXTOP)   ? BOXBOTTOM : BOXTOP;
    }

    /* Reset VDP1 quad table and clip buffers */
    Saturn_Vdp1Wall_Reset();
    Saturn_CmdBuf_Reset();
    for (int i = 0; i < HW_SOLID_WORDS; i++)
    {
        hw_solid_mask[i] = 0;
        sub_solid_mask[i] = 0;
    }
    sub_solid_active = 0;
    sub_solid_min_w = HW_SOLID_WORDS;
    sub_solid_max_w = -1;
    hw_first_flat_job = 1;
    hw_solid_count = 0;
    hw_solid_words = 0;
    hw_solid_summary = 0;
    hw_solid_any = 0;
    hw_slave_dirty_words = 0;
    hw_def_flat_count = 0;
    hw_frame_flat_jobs = 0;
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        const int vw = saturn_view_w;
        const int16_t vh = (int16_t)saturn_view_h;
        for (int i = 0; i < vw; i++)
        {
            ap->ytop[i] = 0;
            ap->ybot[i] = vh;
        }
        for (int i = 0; i < HW_SOLID_WORDS; i++)
        {
            ap->ytop_min_w[i] = 0;
            ap->ybot_max_w[i] = vh;
        }
        int n_ap_sec = (numsectors < HW_MAX_AP_SECTORS) ? numsectors : HW_MAX_AP_SECTORS;
        for (int i = 0; i < n_ap_sec; i++)
        {
            ap->sec_yt_attach[i] = vh;
            ap->sec_yb_attach[i] = 0;
        }
        ap->portal_span_count = 0;
        ap->aperture_active = 0;
    }
    {
        int sec_words = ((numsectors > 512 ? 512 : numsectors) + 31) >> 5;
        for (int i = 0; i < sec_words; i++)
            hw_sec_things_built[i] = 0;
        int sub_words = ((numsubsectors > 1024 ? 1024 : numsubsectors) + 31) >> 5;
        for (int i = 0; i < sub_words; i++)
            hw_sub_has_things[i] = 0;
        hw_thing_pool_count = 0;
    }
    hw_vcache_frame++;
    if (hw_vcache_frame == 0)
    {
        if (hw_vcache)
            memset(hw_vcache, 0, HW_VCACHE_SIZE * sizeof(saturn_vcache_t));
        hw_vcache_frame = 1;
    }
    hw_seg_last_line_idx = -1;
    hw_seg_last_side_idx = -1;
    hw_seg_last_sidedef = NULL;
    hw_seg_last_backsec = NULL;
    hw_last_flat_sec = NULL;
    saturn_hw_prof_bsp = 0;
    saturn_hw_prof_segs = 0;
    saturn_hw_prof_flats = 0;
    saturn_hw_prof_bg = 0;
    saturn_hw_seg_count = 0;
    saturn_hw_wall_quads = 0;
    saturn_hw_flat_quads = 0;
    saturn_hw_clamp_count = 0;
    saturn_hw_slave_timeout = 0;
    saturn_hw_prof_slave_wait = 0;
    saturn_hw_bsp_abort = 0;
    saturn_hw_occl_culls = 0;
    saturn_hw_seg_iter = 0;
    saturn_hw_seg_vis = 0;
    saturn_hw_tex_miss = 0;
    saturn_master_flat_q_done = 0;
    saturn_hw_wall_strips = 0;
    saturn_hw_flat_lod0_subs = 0;
    saturn_hw_flat_lod1_subs = 0;
    saturn_hw_flat_lod_saved_quads = 0;

    /* Queue player weapon directly into saturn_cmd_w */
    HW_RenderPlayerWeapon();

    saturn_hw_sky_visible = (hw_psec && ((int16_t)hw_psec->ceilingpic == -1));

    /* Query Potentially Visible Set (PVS) for player's current leaf position */
    extern const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y);
    pvs_row = Saturn_Pvs_GetRow(hw_viewx, hw_viewy);
    if (pvs_row && ((const byte *)ssec != HW_APERTURE->last_pvs_row || numnodes != HW_APERTURE->last_pvs_numnodes))
    {
        HW_UpdateNodePvsVis(pvs_row, ssec);
    }

#ifndef SATURN_WIN32
    extern unsigned char saturn_slave_enabled;
    extern void jo_core_exec_on_slave(void (*callback)(void));
    extern void jo_core_wait_for_slave(void);

    hw_frame_slave_enabled = saturn_slave_enabled;
    if (hw_frame_slave_enabled)
    {
        static int slave_worker_launched = 0;
        volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

        if (!slave_worker_launched)
        {
            slave_worker_launched = 1;
            hw_master_job_tail = 0;
            ipc->job_tail = 0;
            ipc->job_head = 0;
            ipc->job_active = (unsigned int)-1;
            ipc->quad_count = 0;
            ipc->sky_visible = 0;
            __asm__ volatile("" ::: "memory");
            jo_core_exec_on_slave(HW_Slave_FlatWorker);
        }

        ipc->viewx = hw_viewx;
        ipc->viewy = hw_viewy;
        ipc->viewz = hw_viewz;
        ipc->viewcos = hw_viewcos;
        ipc->viewsin = hw_viewsin;
        ipc->cam_cz_origin = cam_cz_origin;
        ipc->cam_cx_origin = cam_cx_origin;
        ipc->psec_ceilingpic = hw_psec ? (int16_t)hw_psec->ceilingpic : -1;
        ipc->view_w  = (int16_t)saturn_view_w;
        ipc->view_cx = (int16_t)saturn_view_cx;
        ipc->view_cy = (int16_t)saturn_view_cy;
        ipc->view_h  = (int16_t)saturn_view_h;
        ipc->quad_count = 0;
        ipc->sky_visible = 0;
        if (hw_slave_level_dirty)
        {
            /* New level: tell the slave to purge stale flat-geometry L1 lines */
            hw_slave_level_dirty = 0;
            ipc->job_done = ipc->job_done + 1;
        }
        saturn_master_flat_q = HW_SLAVE_FLAT_Q_CAP;
        uint32_t dummy_ipc = ipc->sky_visible;
        (void)dummy_ipc;
        __asm__ volatile("" : : "r"(dummy_ipc) : "memory");
    }
#else
    hw_frame_slave_enabled = 0;
#endif

    /* Walk BSP tree and emit visible walls, flats & sprites in exact topological order */
    extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
    extern volatile unsigned int saturn_vbl_count;
    unsigned short tb0;
    unsigned int vb0;
    do {
        vb0 = saturn_vbl_count;
        tb0 = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != vb0, 0));
    HW_RenderBSPNode((int)numnodes - 1, pvs_row, 0);
    saturn_hw_prof_bsp = Saturn_ProfSpan(tb0, vb0);

    static int saturn_hw_sky_was_visible = 0;
    int bg_drawn = 0;

#ifndef SATURN_WIN32
    if (hw_frame_slave_enabled)
    {
        volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

        /* Overlap Master deferred flat overflow & background sky rendering with Slave SH-2 flat queue processing */
        if (hw_def_flat_count > 0)
        {
            for (int dj = 0; dj < hw_def_flat_count; dj++)
            {
                HW_Master_ProcessJob(ipc, &hw_def_flat_jobs[dj]);
            }
            hw_def_flat_count = 0;
        }
        if (ipc->sky_visible)
            saturn_hw_sky_visible = 1;
        if (saturn_hw_sky_visible || saturn_hw_sky_latch > 0)
        {
            if (saturn_hw_sky_visible && !saturn_hw_sky_was_visible)
                Saturn_HW_ForceSkyRepaint();
            saturn_hw_sky_was_visible = saturn_hw_sky_visible;
            unsigned short tbg0;
            unsigned int vbg0;
            do {
                vbg0 = saturn_vbl_count;
                tbg0 = SAT_FRT();
            } while (__builtin_expect(saturn_vbl_count != vbg0, 0));
            HW_FillBackground();
            saturn_hw_prof_bg = Saturn_ProfSpan(tbg0, vbg0);
            bg_drawn = 1;
        }

        unsigned short twait0;
        unsigned int vwait0;
        do {
            vwait0 = saturn_vbl_count;
            twait0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != vwait0, 0));
        int wait_guard = 1000000; /* x 64 cycles: same timeout as before */
        while (1)
        {
            if ((int)(ipc->job_tail - ipc->job_head) <= 0)
                break;
            if (--wait_guard <= 0)
                break;
            /* Yield the bus to the Slave CPU (>= 64 cycles in-register without uncached polls) */
            HW_BUS_YIELD();
        }
        if (wait_guard <= 0)
        {
            saturn_hw_slave_timeout++;
            saturn_slave_enabled = 0; /* Disable Slave to prevent cascading frame hangs */
            *(volatile unsigned char *)((uint32_t)&saturn_slave_enabled | SATURN_WRAM_H_UNCACHED) = 0;
        }
        saturn_hw_prof_slave_wait = Saturn_ProfSpan(twait0, vwait0);
        
        int slave_final = ipc->quad_count;
        if (slave_final > HW_SLAVE_FLAT_Q_CAP)
            slave_final = HW_SLAVE_FLAT_Q_CAP;
        int master_flats = saturn_master_flat_q - HW_SLAVE_FLAT_Q_CAP;
        if (master_flats < 0)
            master_flats = 0;
        saturn_cmd_s_count = (uint16_t)slave_final;
        saturn_hw_flat_quads = (unsigned int)(slave_final + master_flats);
        
        if (ipc->sky_visible)
        {
            saturn_hw_sky_visible = 1;
        }
    }
    else
    {
        saturn_hw_flat_quads = saturn_cmd_s_count;
    }

    /* L1 cache is flash-purged via HW_PURGE_L1_CACHE() at start of Saturn_CmdBuf_CullOccluded() */
#else
    saturn_hw_flat_quads = saturn_cmd_s_count;
#endif
    {
        unsigned short tc0;
        unsigned int vc0;
        do {
            vc0 = saturn_vbl_count;
            tc0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != vc0, 0));
        Saturn_CmdBuf_CullOccluded();
        saturn_hw_prof_flats = Saturn_ProfSpan(tc0, vc0);
    }

    if (!bg_drawn)
    {
        /* Force repaint when sky transitions from invisible to visible */
        if (saturn_hw_sky_visible && !saturn_hw_sky_was_visible)
        {
            Saturn_HW_ForceSkyRepaint();
        }
        saturn_hw_sky_was_visible = saturn_hw_sky_visible;

        /* Fill background sky and floor planes in I_VideoBuffer */
        if (saturn_hw_sky_visible || saturn_hw_sky_latch > 0)
        {
            unsigned short tbg0;
            unsigned int vbg0;
            do {
                vbg0 = saturn_vbl_count;
                tbg0 = SAT_FRT();
            } while (__builtin_expect(saturn_vbl_count != vbg0, 0));
            HW_FillBackground();
            saturn_hw_prof_bg = Saturn_ProfSpan(tbg0, vbg0);
        }
        else
        {
            saturn_hw_prof_bg = 0;
        }
    }

#ifndef SATURN_HOLE_DEBUG
#define SATURN_HOLE_DEBUG 0
#endif
#if SATURN_HOLE_DEBUG
    /* TEMPORARY hole diagnostic: everything below the horizon must be
     * covered by VDP1 floors/walls, so paint the NBG0 background there
     * solid magenta (PLAYPAL 251). Any magenta on screen = missing quad. */
    D_memset(I_VideoBuffer + HW_VIEW_CY * saturn_view_w, 251,
             (size_t)(HW_VIEW_H - HW_VIEW_CY) * saturn_view_w);
    {
        extern int saturn_hw_skydbg;
        if (saturn_hw_skydbg == 2)
        {
            D_memset(I_VideoBuffer, 251, (size_t)HW_VIEW_CY * saturn_view_w);
            hw_last_sky_angle = 0xFFFFFFFFu;   /* repaint sky when toggled off */
        }
    }
#endif
}
