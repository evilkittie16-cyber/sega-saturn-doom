/* saturn_cmdbuf.c - Unified VDP1 Command Buffers & Topological Delivery
 *
 * Implements:
 *   - Fast WRAM-H allocation of cmdbuf M (Master walls/sprites), S (Slave flats),
 *     W (psprites/weapons), and subsector range table.
 *   - Reverse (back-to-front) painter's delivery to VDP1 VRAM slot 4+.
 *   - Zero-copy command streaming.
 */

#include <stddef.h>
#include <stdint.h>
#include <jo/jo.h>
#include "doomdef.h"
#include "i_system.h"
#include "saturn_memmap.h"
#include "saturn_cmdbuf.h"
#include "saturn_vdp1.h"

vdp1_cmd_t  *saturn_cmd_m = NULL;
vdp1_cmd_t  *saturn_cmd_s = NULL;
vdp1_cmd_t  *saturn_cmd_w = NULL;
sub_range_t *saturn_sub_ranges = NULL;

volatile uint16_t saturn_cmd_m_count = 0;
volatile uint16_t saturn_cmd_s_count = 0;
volatile uint16_t saturn_cmd_w_count = 0;
int saturn_visit_count = 0;
uint8_t saturn_cmd_m_needs_ytrim = 0;

uint32_t saturn_vdp1_bus_cmd_bytes = 0;
uint32_t saturn_vdp1_bus_tex_bytes = 0;

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

static void *cmdbuf_alloc_aligned32(int size)
{
    void *raw = jo_malloc_with_behaviour(size + 31, 0);
    if (!raw)
        return NULL;
    return (void *)(((uintptr_t)raw + 31u) & ~(uintptr_t)31u);
}

void Saturn_CmdBuf_Init(void)
{
    if (saturn_cmd_m != NULL)
        return;

    /* Allocate 32-byte aligned in fast 32-bit WRAM-H SDRAM via Jo heap */
    saturn_cmd_m = (vdp1_cmd_t *)cmdbuf_alloc_aligned32(CMDBUF_M_CAP * (int)sizeof(vdp1_cmd_t));
    if (!saturn_cmd_m)
        I_Error("Saturn_CmdBuf_Init: Failed to allocate cmd_m in WRAM-H\n");

    saturn_cmd_s = (vdp1_cmd_t *)cmdbuf_alloc_aligned32(CMDBUF_S_CAP * (int)sizeof(vdp1_cmd_t));
    if (!saturn_cmd_s)
        I_Error("Saturn_CmdBuf_Init: Failed to allocate cmd_s in WRAM-H\n");

    saturn_cmd_w = (vdp1_cmd_t *)cmdbuf_alloc_aligned32(CMDBUF_W_CAP * (int)sizeof(vdp1_cmd_t));
    if (!saturn_cmd_w)
        I_Error("Saturn_CmdBuf_Init: Failed to allocate cmd_w in WRAM-H\n");

    saturn_sub_ranges = (sub_range_t *)cmdbuf_alloc_aligned32(CMDBUF_MAX_VISIT * (int)sizeof(sub_range_t));
    if (!saturn_sub_ranges)
        I_Error("Saturn_CmdBuf_Init: Failed to allocate sub_ranges in WRAM-H\n");
}

void Saturn_CmdBuf_Reset(void)
{
    saturn_cmd_m_count = 0;
    saturn_cmd_s_count = 0;
    saturn_cmd_w_count = 0;
    saturn_visit_count = 0;
    saturn_cmd_m_needs_ytrim = 0;
}

#ifdef __GNUC__
#define CMDBUF_INLINE static inline __attribute__((always_inline))
#define CMDBUF_NOINLINE __attribute__((noinline, optimize("O2")))
#define CMDBUF_HOT_O2 __attribute__((optimize("O2")))
#else
#define CMDBUF_INLINE static inline
#define CMDBUF_NOINLINE
#define CMDBUF_HOT_O2
#endif

CMDBUF_INLINE void v1_write_userclip(volatile uint32_t *dst32, int x0, int y0, int x1, int y1)
{
    dst32[0] = 0x00080000u; /* CTRL: user clip (0x0008), LINK (0x0000) */
    dst32[1] = 0;
    dst32[2] = 0;
    dst32[3] = ((uint32_t)(uint16_t)x0 << 16) | (uint16_t)y0;
    dst32[4] = ((uint32_t)(uint16_t)x1 << 16) | (uint16_t)y1;
    dst32[5] = ((uint32_t)(uint16_t)x1 << 16) | (uint16_t)y1;
    dst32[6] = 0;
    dst32[7] = 0;
}

CMDBUF_INLINE void v1_write_localcoord(volatile uint32_t *dst32, int x, int y)
{
    dst32[0] = 0x000A0000u; /* CTRL: local coord (0x000A), LINK (0x0000) */
    dst32[1] = 0;
    dst32[2] = 0;
    dst32[3] = ((uint32_t)(uint16_t)x << 16) | (uint16_t)y;
    dst32[4] = 0;
    dst32[5] = 0;
    dst32[6] = 0;
    dst32[7] = 0;
}

/* 100% Pipeline-Optimized 32-byte Burst Copy
 * Maximizes SDRAM Read Burst, minimizes Write-Buffer turnaround */
CMDBUF_HOT_O2 void Saturn_VDP1_BurstCopy32(uint32_t **dst_ptr, const uint32_t **src_ptr)
{
#ifdef SATURN_WIN32
    memcpy(*dst_ptr, *src_ptr, 32);
    *dst_ptr += 8;
    *src_ptr += 8;
#else
    register uint32_t *dst = *dst_ptr;
    register const uint32_t *src = *src_ptr;

    __asm__ volatile (
        "mov.l  @%0+, r0 \n\t"  "mov.l  @%0+, r1 \n\t"
        "mov.l  @%0+, r2 \n\t"  "mov.l  @%0+, r3 \n\t"
        "mov.l  @%0+, r4 \n\t"  "mov.l  @%0+, r5 \n\t"
        "mov.l  @%0+, r6 \n\t"  "mov.l  @%0+, r7 \n\t"
        
        "mov.l  r0, @%1       \n\t" "mov.l  r1, @(4, %1)  \n\t"
        "mov.l  r2, @(8, %1)  \n\t" "mov.l  r3, @(12, %1) \n\t"
        "mov.l  r4, @(16, %1) \n\t" "mov.l  r5, @(20, %1) \n\t"
        "mov.l  r6, @(24, %1) \n\t" "mov.l  r7, @(28, %1) \n\t"
        "add    #32, %1       \n\t" 
        : "+r" (src), "+r" (dst)
        :: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "memory"
    );
    *src_ptr = src;
    *dst_ptr = dst;
#endif
}

CMDBUF_INLINE uint32_t *copy_quads_32(uint32_t *dst, const uint32_t *src, int count)
{
#ifdef SATURN_WIN32
    if (count > 0)
    {
        memcpy(dst, src, count * 32);
        dst += count * 8;
    }
    return dst;
#else
    if (count > 0)
    {
        __asm__ volatile (
            "1:                   \n\t"
            "mov.l  @%0+, r0      \n\t"  "mov.l  @%0+, r1      \n\t"
            "mov.l  @%0+, r2      \n\t"  "mov.l  @%0+, r3      \n\t"
            "mov.l  @%0+, r4      \n\t"  "mov.l  @%0+, r5      \n\t"
            "mov.l  @%0+, r6      \n\t"  "mov.l  @%0+, r7      \n\t"
            "mov.l  r0, @%1       \n\t"  "mov.l  r1, @(4, %1)  \n\t"
            "mov.l  r2, @(8, %1)  \n\t"  "mov.l  r3, @(12, %1) \n\t"
            "mov.l  r4, @(16, %1) \n\t"  "mov.l  r5, @(20, %1) \n\t"
            "mov.l  r6, @(24, %1) \n\t"  "mov.l  r7, @(28, %1) \n\t"
            "dt     %2            \n\t"
            "bf/s   1b            \n\t"
            "add    #32, %1       \n\t"
            : "+r" (src), "+r" (dst), "+r" (count)
            :: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "t", "memory"
        );
    }
    return dst;
#endif
}

CMDBUF_INLINE uint32_t *deliver_range_32(uint32_t *dst, const uint32_t * const limit, const uint32_t *src, uint32_t count)
{
    uint32_t max_q = (dst < limit) ? (uint32_t)((limit - dst) >> 3) : 0u;
    if (count > max_q)
        count = max_q;
    return copy_quads_32(dst, src, (int)count);
}

CMDBUF_INLINE uint32_t cmdbuf_inv_dx(int dx, const unsigned int *oclut)
{
    if (dx <= 1)
        return 65536u;
    if (dx > 2047)
        dx = 2047;
#ifndef SATURN_WIN32
    uint32_t res;
    uint32_t val = oclut[dx];
    uint32_t mult = 26843546u;
    __asm__ volatile(
        "dmulu.l %1, %2\n\t"
        "sts mach, %0\n\t"
        : "=r"(res)
        : "r"(val), "r"(mult)
        : "mach", "macl"
    );
    return res;
#else
    return (uint32_t)(((uint64_t)oclut[dx] * 26843546ULL) >> 32);
#endif
}

#define CMDBUF_VW 320
#define CMDBUF_VH 168

#ifndef SATURN_WIN32
#define CMD_XA(c32) ((int)((const int16_t *)(c32))[6])
#define CMD_YA(c32) ((int)((const int16_t *)(c32))[7])
#define CMD_XB(c32) ((int)((const int16_t *)(c32))[8])
#define CMD_YB(c32) ((int)((const int16_t *)(c32))[9])
#define CMD_XC(c32) ((int)((const int16_t *)(c32))[10])
#define CMD_YC(c32) ((int)((const int16_t *)(c32))[11])
#define CMD_XD(c32) ((int)((const int16_t *)(c32))[12])
#define CMD_YD(c32) ((int)((const int16_t *)(c32))[13])
#else
#define CMD_XA(c32) ((int)(int16_t)((c32)[3] >> 16))
#define CMD_YA(c32) ((int)(int16_t)((c32)[3] & 0xFFFFu))
#define CMD_XB(c32) ((int)(int16_t)((c32)[4] >> 16))
#define CMD_YB(c32) ((int)(int16_t)((c32)[4] & 0xFFFFu))
#define CMD_XC(c32) ((int)(int16_t)((c32)[5] >> 16))
#define CMD_YC(c32) ((int)(int16_t)((c32)[5] & 0xFFFFu))
#define CMD_XD(c32) ((int)(int16_t)((c32)[6] >> 16))
#define CMD_YD(c32) ((int)(int16_t)((c32)[6] & 0xFFFFu))
#endif

CMDBUF_NOINLINE void Saturn_CmdBuf_TrimWallY(uint32_t *c32)
{
    int xL  = CMD_XA(c32), xR  = CMD_XB(c32);
    int yLt = CMD_YA(c32), yRt = CMD_YB(c32);
    int yLb = CMD_YD(c32), yRb = CMD_YC(c32);
    uint32_t w2 = c32[2];
    int rows = (int)(w2 & 0xFFu);
    int stride = (int)((w2 >> 8) & 0x3Fu);
    int srca = (int)(w2 >> 16);
    int dya = yLb - yLt;
    int dyb = yRb - yRt;
    if (rows <= 4 || dya <= 6 || dyb <= 6)
        return;

    const unsigned int *oclut = (const unsigned int *)SATURN_WRAM_L_DIVLUT_ADDR;
    int modified = 0;

    if (yLt < -8 && yRt < -8)
    {
        int k_a = (int)((((uint32_t)(-1 - yLt) * (uint32_t)rows) * cmdbuf_inv_dx(dya, oclut)) >> 16);
        int k_b = (int)((((uint32_t)(-1 - yRt) * (uint32_t)rows) * cmdbuf_inv_dx(dyb, oclut)) >> 16);
        int k = (k_a < k_b) ? k_a : k_b;
        if (k >= rows) k = rows - 1;
        if (k > 0)
        {
            uint32_t inv_r = cmdbuf_inv_dx(rows, oclut);
            yLt += (int)((((uint32_t)k * (uint32_t)dya) * inv_r + 32768u) >> 16);
            yRt += (int)((((uint32_t)k * (uint32_t)dyb) * inv_r + 32768u) >> 16);
            srca += k * stride;
            rows -= k;
            dya = yLb - yLt;
            dyb = yRb - yRt;
            modified = 1;
        }
    }

    if (yLb > CMDBUF_VH + 8 && yRb > CMDBUF_VH + 8 && rows > 4 && dya > 6 && dyb > 6)
    {
        int k_a = (int)((((uint32_t)(yLb - (CMDBUF_VH + 1)) * (uint32_t)rows) * cmdbuf_inv_dx(dya, oclut)) >> 16);
        int k_b = (int)((((uint32_t)(yRb - (CMDBUF_VH + 1)) * (uint32_t)rows) * cmdbuf_inv_dx(dyb, oclut)) >> 16);
        int k = (k_a < k_b) ? k_a : k_b;
        if (k >= rows) k = rows - 1;
        if (k > 0)
        {
            uint32_t inv_r = cmdbuf_inv_dx(rows, oclut);
            yLb -= (int)((((uint32_t)k * (uint32_t)dya) * inv_r + 32768u) >> 16);
            yRb -= (int)((((uint32_t)k * (uint32_t)dyb) * inv_r + 32768u) >> 16);
            rows -= k;
            modified = 1;
        }
    }

    if (modified)
    {
        c32[2] = ((uint32_t)srca << 16) | ((uint32_t)stride << 8) | ((uint32_t)rows & 0xFFu);
        c32[3] = ((uint32_t)(uint16_t)xL << 16) | (uint16_t)yLt;
        c32[4] = ((uint32_t)(uint16_t)xR << 16) | (uint16_t)yRt;
        c32[5] = ((uint32_t)(uint16_t)xR << 16) | (uint16_t)yRb;
        c32[6] = ((uint32_t)(uint16_t)xL << 16) | (uint16_t)yLb;
    }
}

unsigned int saturn_hw_dda_steps = 0;

static inline uint32_t cmdbuf_quad_dda(const uint32_t *c32)
{
    if (((c32[0] >> 16) & 0x800Fu) != 0x0002u)
        return 0u;
    uint32_t w2 = c32[2];
    int rows = (int)(w2 & 0xFFu);
    int tw = (int)((w2 >> 5) & 0x1F8u);
    int xA = CMD_XA(c32), yA = CMD_YA(c32);
    int xB = CMD_XB(c32), yB = CMD_YB(c32);
    int xC = CMD_XC(c32), yC = CMD_YC(c32);
    int xD = CMD_XD(c32), yD = CMD_YD(c32);
    int dx_ab = xB - xA; if (dx_ab < 0) dx_ab = -dx_ab;
    int dy_ab = yB - yA; if (dy_ab < 0) dy_ab = -dy_ab;
    int w_ab = (dx_ab > dy_ab ? dx_ab : dy_ab) + 1;
    int dx_dc = xC - xD; if (dx_dc < 0) dx_dc = -dx_dc;
    int dy_dc = yC - yD; if (dy_dc < 0) dy_dc = -dy_dc;
    int w_dc = (dx_dc > dy_dc ? dx_dc : dy_dc) + 1;
    int inner = (w_ab + w_dc) >> 1;
    if (tw > inner) inner = tw;
    int dx_ad = xD - xA; if (dx_ad < 0) dx_ad = -dx_ad;
    int dy_ad = yD - yA; if (dy_ad < 0) dy_ad = -dy_ad;
    int dx_bc = xC - xB; if (dx_bc < 0) dx_bc = -dx_bc;
    int dy_bc = yC - yB; if (dy_bc < 0) dy_bc = -dy_bc;
    int l_ad = (dx_ad > dy_ad ? dx_ad : dy_ad) + 1;
    int l_bc = (dx_bc > dy_bc ? dx_bc : dy_bc) + 1;
    int outer = l_ad > l_bc ? l_ad : l_bc;
    if (rows > outer) outer = rows;
    return (uint32_t)inner * (uint32_t)outer;
}

CMDBUF_HOT_O2 void Saturn_CmdBuf_CullOccluded(void)
{
#ifndef SATURN_WIN32
    if (saturn_visit_count > 0 || saturn_cmd_s_count > 0)
    {
        /* Flash-purge Master SH-2 L1 cache via uncached Area 1 trampoline (12 cycles)
         * so Saturn_CmdBuf_Deliver reads fresh saturn_sub_ranges and saturn_cmd_s written by Slave. */
        extern void _hw_purge_cache(void);
        ((void (*)(void))((uintptr_t)_hw_purge_cache | SATURN_WRAM_H_UNCACHED))();
    }
#endif

    extern int saturn_view_w;
    extern int saturn_view_h;
    uint32_t m_count = saturn_cmd_m_count;
    if (__builtin_expect(saturn_cmd_m_needs_ytrim != 0, 0) &&
        m_count > 0 && saturn_view_w <= CMDBUF_VW && saturn_view_h <= CMDBUF_VH)
    {
        /* Trim extreme off-screen vertical wall coordinates */
        uint32_t *c32 = (uint32_t *)saturn_cmd_m;
        uint32_t *m_end = c32 + (m_count << 3);
        for (; c32 != m_end; c32 += 8)
        {
            uint32_t ctrl = c32[0] >> 16;
            if ((ctrl & 0x802Fu) != 0x0002u)
                continue;
            int yLt = CMD_YA(c32), yRt = CMD_YB(c32);
            int yLb = CMD_YD(c32), yRb = CMD_YC(c32);
            if (__builtin_expect((yLt < -8 && yRt < -8) || (yLb > CMDBUF_VH + 8 && yRb > CMDBUF_VH + 8), 0))
            {
                Saturn_CmdBuf_TrimWallY(c32);
            }
        }
    }
}

unsigned int Saturn_CmdBuf_ComputeDDA(void)
{
    uint32_t dda_total = 0;
    uint32_t m_count = saturn_cmd_m_count;
    if (m_count > 0)
    {
        const uint32_t *c32 = (const uint32_t *)saturn_cmd_m;
        const uint32_t *m_end = c32 + (m_count << 3);
        for (; c32 != m_end; c32 += 8)
            dda_total += cmdbuf_quad_dda(c32);
    }
    if (saturn_visit_count > 0)
    {
        const sub_range_t *ranges = saturn_sub_ranges;
        const uint32_t *cmd_s_base = (const uint32_t *)saturn_cmd_s;
        for (int i = 0; i < saturn_visit_count; i++)
        {
            uint16_t sc = ranges[i].s_count;
            if (sc > 0)
            {
                const uint32_t *s32 = cmd_s_base + ((uint32_t)ranges[i].s_start << 3);
                const uint32_t *s_end = s32 + ((uint32_t)sc << 3);
                for (; s32 != s_end; s32 += 8)
                    dda_total += cmdbuf_quad_dda(s32);
            }
        }
    }
    if (saturn_cmd_w_count > 0)
    {
        const uint32_t *w32 = (const uint32_t *)saturn_cmd_w;
        const uint32_t *w_end = w32 + ((uint32_t)saturn_cmd_w_count << 3);
        for (; w32 != w_end; w32 += 8)
            dda_total += cmdbuf_quad_dda(w32);
    }
    saturn_hw_dda_steps = dda_total;
    return dda_total;
}

CMDBUF_HOT_O2 void Saturn_CmdBuf_Deliver(void)
{
    uint32_t *vram32 = (uint32_t *)(SATURN_VDP1_VRAM + SATURN_V1_USER0);
    const uint32_t * const vram_start = vram32;

    /* 1. Slot 0 (V1_USER0): Set user clip to full 3D viewport */
    extern int saturn_view_w;
    extern int saturn_view_h;
    const int vw = (saturn_view_w > 0) ? saturn_view_w : 320;
    const int vh = (saturn_view_h > 0) ? saturn_view_h : 168;
    int x_offset = (352 - vw) >> 1;
    int clip_y1 = vh - 1;
    v1_write_userclip((volatile uint32_t *)vram32, x_offset, 0, (vw - 1) + x_offset, clip_y1);
    vram32 += 8;

    /* 2. Slot 1: Set local coord to (x_offset, 0) to center viewport in 352-wide screen */
    v1_write_localcoord((volatile uint32_t *)vram32, x_offset, 0);
    vram32 += 8;

    const sub_range_t *ranges = saturn_sub_ranges;
    const uint32_t *cmd_s_base = (const uint32_t *)saturn_cmd_s;
    const uint32_t *cmd_m = (const uint32_t *)saturn_cmd_m;

    /* Strict Back-To-Front Topological BSP Order:
     * DOOM's BSP tree mathematical traversal front-to-back guarantees that
     * iterating in reverse (saturn_visit_count - 1 down to 0) provides the
     * exact back-to-front topological order. Never re-sort subsectors by bbox
     * centers, which breaks BSP plane partitioning for concave/large sectors! */
    const uint32_t * const vram_limit = (const uint32_t *)(SATURN_VDP1_VRAM + 0xFFE0u);
    if (__builtin_expect((uint32_t)saturn_cmd_m_count + (uint32_t)saturn_cmd_s_count + (uint32_t)saturn_cmd_w_count <= 2040u, 1))
    {
        for (int i = saturn_visit_count - 1; i >= 0; i--)
        {
            const sub_range_t *r = &ranges[i];
            int sc = r->s_count;
            if (sc > 0)
                vram32 = copy_quads_32(vram32, cmd_s_base + ((uint32_t)r->s_start << 3), sc);
            int mc = r->m_count;
            if (mc > 0)
                vram32 = copy_quads_32(vram32, cmd_m + ((uint32_t)r->m_start << 3), mc);
        }
    }
    else
    {
        for (int i = saturn_visit_count - 1; i >= 0; i--)
        {
            const sub_range_t *r = &ranges[i];
            if (r->s_count > 0)
                vram32 = deliver_range_32(vram32, vram_limit, cmd_s_base + ((uint32_t)r->s_start << 3), r->s_count);
            if (r->m_count > 0)
                vram32 = deliver_range_32(vram32, vram_limit, cmd_m + ((uint32_t)r->m_start << 3), r->m_count);
        }
    }

    /* 3. Deliver weapon / psprite quads last (on top of all 3D geometry) */
    if (saturn_cmd_w_count > 0)
    {
        uint16_t w_cnt = saturn_cmd_w_count;
        if (vram32 + w_cnt * 8 > vram_limit)
            w_cnt = (vram_limit > vram32) ? (uint16_t)((vram_limit - vram32) >> 3) : 0;
        if (w_cnt > 0)
            vram32 = copy_quads_32(vram32, (const uint32_t *)saturn_cmd_w, (int)w_cnt);
    }

    /* 4. Terminate VDP1 command list with END packet */
    vram32[0] = 0x80000000u; /* CTRL: END (0x8000), LINK (0x0000) */
    vram32[1] = 0;
    vram32[2] = 0;
    vram32[3] = 0;
    vram32[4] = 0;
    vram32[5] = 0;
    vram32[6] = 0;
    vram32[7] = 0;
    vram32 += 8;

    saturn_vdp1_bus_cmd_bytes = (uint32_t)((uintptr_t)vram32 - (uintptr_t)vram_start);
}
