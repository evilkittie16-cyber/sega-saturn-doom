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
volatile uint16_t saturn_cmd_sw_count = 0;
int saturn_visit_count = 0;
uint8_t saturn_cmd_m_needs_ytrim = 0;

uint32_t saturn_vdp1_bus_cmd_bytes = 0;
uint32_t saturn_vdp1_bus_tex_bytes = 0;
unsigned int saturn_v1_page_flushes = 0;   /* VDP1 texture page flushes (HUD 'Y') */
unsigned int saturn_v1_variant_uploads = 0; /* flat variant pyramids built (transposed / tile) */

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

/* SCU DMA delivery (2026-11-27, user: "do both"). Saturn_CmdBuf_Deliver used
 * to copy every command into VDP1 VRAM with the master CPU (2.0-2.9 ms of
 * master time per frame in the E4 views, B-bus write bound). Now it builds an
 * SCU indirect-mode table ({bytes, dst, src} per contiguous run, in WRAM-H)
 * and starts SCU DMA level 1; the CPU goes on with the next frame's game
 * logic. The frame change waits for the DMA (v1m_vblank), and the next frame
 * waits for it before touching the command buffers or VDP1 VRAM
 * (Saturn_CmdBuf_DmaWait at the start of rendering / 2D / blank lists).
 * Level 1 (Jo's own VDP1 pipeline owns level 0): a table entry moves at most
 * 4 KB, so long runs are split. hw_cmd_dma = 0 (pokeable) or -DHW_CMD_DMA=0
 * restores the CPU copy. */
#ifndef HW_CMD_DMA
#define HW_CMD_DMA 1
#endif
#if HW_CMD_DMA && !defined(SATURN_WIN32)
#define CMD_DMA_LEVEL    1
#define CMD_DMA_BASE     (0x25FE0000u + CMD_DMA_LEVEL * 0x20u)
#define CMD_DMA_REG(o)   (*(volatile uint32_t *)(CMD_DMA_BASE + (o)))
#define CMD_DMA_STATUS   (*(volatile uint32_t *)0x25FE007Cu)
#define CMD_DMA_BUSY_BIT (1u << (4 + CMD_DMA_LEVEL * 4))
#define CMD_DMA_CHUNK    4064u                 /* <= 4096 (level 1/2 count), multiple of 32 */
/* The SCU reads the indirect table wrongly on real hardware unless the table
 * is aligned on a boundary of its size rounded up to a power of 2 (Jo's
 * vdp1_command_pipeline.c, which uses 8 KB at least). The emulators don't
 * model it: the first 32-byte-aligned version froze the hardware on the first
 * 3D frame (user, DOOM-1127C-DMA-RETAIL). Table <= 8 KB, aligned to 8 KB; a
 * frame needing more entries CPU-copies the rest (cmd_dma_add). */
#define CMD_DMA_TAB_ALIGN 8192u
#define CMD_DMA_MAX_ENT  ((int)(CMD_DMA_TAB_ALIGN / 12u))     /* 682 */
volatile int hw_cmd_dma = 1;                   /* runtime switch (pokeable) */
unsigned int saturn_cmd_dma_entries = 0;       /* debug: table entries last frame */
static uint32_t *cmd_dma_tab = NULL;           /* {bytes, dst, src} x CMD_DMA_MAX_ENT */
static int       cmd_dma_n = 0;
static int       cmd_dma_on = 0;
static uint32_t  cmd_dma_next_src = 0;

int Saturn_CmdBuf_DmaBusy(void)
{
    return (CMD_DMA_STATUS & CMD_DMA_BUSY_BIT) != 0;
}

void Saturn_CmdBuf_DmaWait(void)
{
    unsigned int guard = 4000000u;
    while ((CMD_DMA_STATUS & CMD_DMA_BUSY_BIT) && --guard)
        ;
    if (!guard)
    {
        /* hung (never seen once the table was aligned): stop the level and
         * go back to the CPU copy instead of freezing the frame change */
        CMD_DMA_REG(0x10) = 0;
        hw_cmd_dma = 0;
    }
}

/* Queue `count` commands from WRAM-H `src` to VRAM `dst` (dst is always the
 * end of the previous run, so a run whose source continues the last entry's
 * source just grows that entry). Returns the new VRAM write position. */
static uint32_t *cmd_dma_add(uint32_t *dst, const uint32_t *src, int count)
{
    uint32_t bytes = (uint32_t)count << 5;
    uint32_t s = (uint32_t)(uintptr_t)src, d = (uint32_t)(uintptr_t)dst;
    /* SCU DMA reads only WRAM-H (not Low WRAM): anything else is CPU-copied */
    const int dma_ok = (s & 0x07000000u) == 0x06000000u;
    while (bytes)
    {
        uint32_t take;
        if (!dma_ok)
        {
            const uint32_t *ps = (const uint32_t *)(uintptr_t)s;
            uint32_t *pd = (uint32_t *)(uintptr_t)d;
            for (uint32_t k = 0; k < (bytes >> 2); k++)
                pd[k] = ps[k];
            d += bytes;
            break;
        }
        if (cmd_dma_n > 0 && s == cmd_dma_next_src && cmd_dma_tab[(cmd_dma_n - 1) * 3] < CMD_DMA_CHUNK)
        {
            uint32_t room = CMD_DMA_CHUNK - cmd_dma_tab[(cmd_dma_n - 1) * 3];
            take = (bytes < room) ? bytes : room;
            cmd_dma_tab[(cmd_dma_n - 1) * 3] += take;
        }
        else if (cmd_dma_n < CMD_DMA_MAX_ENT)
        {
            uint32_t *e = &cmd_dma_tab[cmd_dma_n * 3];
            take = (bytes < CMD_DMA_CHUNK) ? bytes : CMD_DMA_CHUNK;
            e[0] = take;
            e[1] = d & 0x07FFFFFFu;
            e[2] = s & 0x07FFFFFFu;
            cmd_dma_n++;
        }
        else
        {
            /* table full (never seen): the CPU copies the rest; its VRAM
             * area is disjoint from the DMA's, so order does not matter */
            const uint32_t *ps = (const uint32_t *)(uintptr_t)s;
            uint32_t *pd = (uint32_t *)(uintptr_t)d;
            for (uint32_t k = 0; k < (bytes >> 2); k++)
                pd[k] = ps[k];
            d += bytes;
            break;
        }
        s += take;
        d += take;
        bytes -= take;
        cmd_dma_next_src = s;
    }
    return (uint32_t *)(uintptr_t)d;
}

static void cmd_dma_kick(void)
{
    saturn_cmd_dma_entries = (unsigned int)cmd_dma_n;
    if (cmd_dma_n <= 0)
        return;
    cmd_dma_tab[(cmd_dma_n - 1) * 3 + 2] |= 0x80000000u;   /* last entry */
    /* the table and the commands are in WRAM-H, written through the
     * write-through cache: memory is current when the SCU reads it */
    CMD_DMA_REG(0x10) = 0;                       /* DxEN off */
    CMD_DMA_REG(0x04) = (uint32_t)(uintptr_t)cmd_dma_tab & 0x07FFFFFFu;   /* table */
    CMD_DMA_REG(0x0C) = 0x101u;                  /* read +4, write +2 (B-bus) */
    CMD_DMA_REG(0x14) = 0x01000007u;             /* indirect, start by DxEN go */
    CMD_DMA_REG(0x10) = 0x101u;                  /* enable + go */
}
#else
int  Saturn_CmdBuf_DmaBusy(void) { return 0; }
void Saturn_CmdBuf_DmaWait(void) { }
#endif

void Saturn_CmdBuf_Init(void)
{
    if (saturn_cmd_m != NULL)
        return;

    /* Allocate 32-byte aligned in fast 32-bit WRAM-H SDRAM via Jo heap */
    saturn_cmd_m = (vdp1_cmd_t *)cmdbuf_alloc_aligned32((CMDBUF_M_CAP + CMDBUF_SW_CAP) * (int)sizeof(vdp1_cmd_t));
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

#if HW_CMD_DMA && !defined(SATURN_WIN32)
    /* no room: the CPU copy stays (cmd_dma_tab NULL) */
    {
        void *raw = jo_malloc_with_behaviour((int)(CMD_DMA_TAB_ALIGN * 2u - 1u), 0);
        cmd_dma_tab = raw ? (uint32_t *)(((uintptr_t)raw + CMD_DMA_TAB_ALIGN - 1u) & ~(uintptr_t)(CMD_DMA_TAB_ALIGN - 1u))
                          : NULL;
    }
#endif
}

void Saturn_CmdBuf_Reset(void)
{
    saturn_cmd_m_count = 0;
    saturn_cmd_s_count = 0;
    saturn_cmd_w_count = 0;
    saturn_cmd_sw_count = 0;
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

/* Manual frame-change mode (saturn_vdp1.c, SATURN_V1_MANUAL) has no
 * automatic erase: clear the 3D view with one transparent polygon at the
 * head of every list (Jo Engine's jo_vdp1_make_clear approach). Polygon =
 * CTRL 0x0004; PMOD 0x00C0 (no end-code / transparency checks, replace);
 * COLR 0x0000 = the transparent sprite pixel (same value as EWDR's erase).
 * Coordinates are local (the list's local-coord command centres the view).
 *
 * OPAQUE BLACK WHEN NO SKY (2026-11-15, hardware photo: a white/dotted
 * mark in the E1M1 start-room ceiling that moves with the view). VDP1
 * leaves occasional single undrawn pixels inside skewed ceiling quads
 * (5thPlanet framebuffer: lone 0x0000 pixels on a diagonal through two
 * quads). Those keep the clear value, and with a transparent clear the
 * VDP2 sky picture behind the view (NBG0, which still holds the last sky
 * even indoors) shows through as bright dots. When no sky is visible this
 * frame nothing should show NBG0, so the clear is opaque palette black
 * (bank 0x300 = the walls' and flats' bank, index 0 = black): holes go
 * dark. With sky visible the clear must stay transparent (the sky IS the
 * undrawn area). */
#define V1_CLEAR_OPAQUE_COLR 0x0300u
CMDBUF_INLINE void v1_write_clear(volatile uint32_t *dst32, int vw, int vh, uint32_t colr)
{
    dst32[0] = 0x00040000u;                                  /* CTRL: polygon, LINK 0 */
    dst32[1] = 0x00C00000u | colr;                           /* PMOD 0x00C0, COLR     */
    dst32[2] = 0;                                            /* SRCA, SIZE (unused)   */
    dst32[3] = 0;                                            /* A = (0, 0)            */
    dst32[4] = ((uint32_t)(uint16_t)(vw - 1) << 16);         /* B = (vw-1, 0)         */
    dst32[5] = ((uint32_t)(uint16_t)(vw - 1) << 16) | (uint16_t)(vh - 1); /* C */
    dst32[6] = (uint32_t)(uint16_t)(vh - 1);                 /* D = (0, vh-1)         */
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
#if HW_CMD_DMA
    if (cmd_dma_on)
        return (count > 0) ? cmd_dma_add(dst, src, count) : dst;
#endif
    /* Word 7 (CMDGRDA + dummy) is NOT copied: VDP1 reads the gouraud table
     * address only when PMOD selects gouraud, and no command this renderer
     * emits does (walls, flats, sprites, punch polygons: CC != 4). The copy
     * is bound by the B-bus write rate to VDP1 VRAM, so 7 of 8 words is
     * 12.5% less delivery time. */
    if (count > 0)
    {
        __asm__ volatile (
            "1:                   \n\t"
            "mov.l  @%0+, r0      \n\t"  "mov.l  @%0+, r1      \n\t"
            "mov.l  @%0+, r2      \n\t"  "mov.l  @%0+, r3      \n\t"
            "mov.l  @%0+, r4      \n\t"  "mov.l  @%0+, r5      \n\t"
            "mov.l  @%0+, r6      \n\t"  "add    #4, %0        \n\t"
            "mov.l  r0, @%1       \n\t"  "mov.l  r1, @(4, %1)  \n\t"
            "mov.l  r2, @(8, %1)  \n\t"  "mov.l  r3, @(12, %1) \n\t"
            "mov.l  r4, @(16, %1) \n\t"  "mov.l  r5, @(20, %1) \n\t"
            "mov.l  r6, @(24, %1) \n\t"
            "dt     %2            \n\t"
            "bf/s   1b            \n\t"
            "add    #32, %1       \n\t"
            : "+r" (src), "+r" (dst), "+r" (count)
            :: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "t", "memory"
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

/* ~65536/dx. 2026-11-21: this used to read a reciprocal table at
 * SATURN_WRAM_L_DIVLUT_ADDR (Low WRAM 0x238000) that NOTHING fills — zero
 * in emulators (trim silently a no-op) but leftover RAM on real hardware,
 * where it could shrink or wipe near tall walls. Now `oclut` is the real
 * table (saturn_master_oclut, 10485760/d; *26843546>>32 = 65536/d), and
 * dx > 2047 divides instead of clamping (the clamp over-trimmed walls
 * taller than 2047 px). */
CMDBUF_INLINE uint32_t cmdbuf_inv_dx(int dx, const unsigned int *oclut)
{
    if (dx <= 1)
        return 65536u;
    if (__builtin_expect(dx > 2047, 0))
        return 65536u / (uint32_t)dx;
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

    extern unsigned int saturn_master_oclut[];   /* HW_Divlut_Init (saturn_hw_math.h) */
    const unsigned int *oclut = saturn_master_oclut;
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
    if (!(c32[1] & 0x10000000u) && tw > inner) inner = tw;
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

/* HW_WALK2 split frame (saturn_hw_render.c): -1 = not split, else the
 * column: the master's lists start at HW_W2_M_CAP / HW_W2_S_CAP /
 * HW_W2_VISIT_CAP, the slave's (hw_w2_out_*) at 0 in the same buffers. */
extern int saturn_w2_split_x;
extern int hw_w2_out_m, hw_w2_out_s, hw_w2_out_v;
#include "saturn_hw_walk2.h"
#if !HW_WALK2 || defined(SATURN_WIN32)
int saturn_w2_split_x = -1;
int hw_w2_out_m, hw_w2_out_s, hw_w2_out_v;
#endif

CMDBUF_HOT_O2 void Saturn_CmdBuf_CullOccluded(void)
{
    extern int saturn_view_w;
    extern int saturn_view_h;
    uint32_t m_count = saturn_cmd_m_count;
    const int split = (saturn_w2_split_x >= 0);
#ifndef SATURN_WIN32
    if (split)
    {
        /* the slave wrote the bottom of the lists: purge before reading them */
        extern void _hw_purge_cache(void);
        ((void (*)(void))((uintptr_t)_hw_purge_cache | SATURN_WRAM_H_UNCACHED))();
    }
#endif
    if (__builtin_expect(saturn_cmd_m_needs_ytrim != 0, 0) &&
        m_count > 0 && saturn_view_w <= CMDBUF_VW && saturn_view_h <= CMDBUF_VH)
    {
        /* Trim extreme off-screen vertical wall coordinates before purging L1 cache
         * (split frame: the slave's part [0, out_m), then the master's) */
        uint32_t *c32 = (uint32_t *)saturn_cmd_m;
        uint32_t *m_end = c32 + (m_count << 3);
#if HW_WALK2 && !defined(SATURN_WIN32)
        if (split)
        {
            uint32_t *s_end = c32 + ((uint32_t)hw_w2_out_m << 3);
            for (; c32 != s_end; c32 += 8)
            {
                uint32_t ctrl = c32[0] >> 16;
                if ((ctrl & 0x802Fu) != 0x0002u)
                    continue;
                int yLt = CMD_YA(c32), yRt = CMD_YB(c32);
                int yLb = CMD_YD(c32), yRb = CMD_YC(c32);
                if ((yLt < -8 && yRt < -8) || (yLb > CMDBUF_VH + 8 && yRb > CMDBUF_VH + 8))
                    Saturn_CmdBuf_TrimWallY(c32);
            }
            c32 = (uint32_t *)saturn_cmd_m + ((uint32_t)HW_W2_M_CAP << 3);
        }
#endif
        for (; c32 < m_end; c32 += 8)
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

#ifndef SATURN_WIN32
    if (saturn_visit_count > 0 || saturn_cmd_s_count > 0)
    {
        /* Flash-purge Master SH-2 L1 cache via uncached Area 1 trampoline (12 cycles)
         * so Saturn_CmdBuf_Deliver reads fresh saturn_sub_ranges and saturn_cmd_s written by Slave. */
        extern void _hw_purge_cache(void);
        ((void (*)(void))((uintptr_t)_hw_purge_cache | SATURN_WRAM_H_UNCACHED))();
    }
#endif
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
    if (saturn_cmd_sw_count > 0)
    {
        const uint32_t *c32 = (const uint32_t *)&saturn_cmd_m[CMDBUF_M_CAP];
        const uint32_t *m_end = c32 + ((uint32_t)saturn_cmd_sw_count << 3);
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

#ifdef SATURN_WIN32
/* Harness only (2026-11-26, per-area pixel census): each delivered 3D
 * command carries its subsector in word 7 (CMDGRDA, which vdp1_emu.c does
 * not read) as 0xA5A50000 | subnum; the first command carries the
 * marker 0x5A5A0000. The rasteriser reads them from the command it draws,
 * whatever frame that is. */
/* low 16 bits: subnum (13 bits) | class << 13 (0 wall, 1 floor/ceiling, 2 sprite) */
static void od_mark(uint32_t *from, const uint32_t *to, unsigned subnum, unsigned cls)
{
    for (; from < to; from += 8)
        from[7] = 0xA5A50000u | (subnum & 0x1FFFu) | (cls << 13);
}
extern uint16_t hw_diag_spr0[1024];
#endif

CMDBUF_HOT_O2 void Saturn_CmdBuf_Deliver(void)
{
    /* Table to fill: SATURN_V1_USER0 in 1-cycle mode; in manual mode the
     * double-buffered table VDP1 is not drawing (saturn_vdp1.c). */
    extern unsigned int saturn_v1_write_table, saturn_v1_write_limit;
    uint32_t *vram32 = (uint32_t *)(SATURN_VDP1_VRAM + saturn_v1_write_table);
    const uint32_t * const vram_start = vram32;

#if HW_CMD_DMA && !defined(SATURN_WIN32)
    Saturn_CmdBuf_DmaWait();                    /* previous list (normally long done) */
    cmd_dma_n = 0;
    cmd_dma_next_src = 0;
    cmd_dma_on = (cmd_dma_tab != NULL) && hw_cmd_dma;
#endif

    /* 1. Slot 0 (V1_USER0): Set user clip to full 3D viewport */
    extern int saturn_view_w;
    extern int saturn_view_h;
    const int vw = (saturn_view_w > 0) ? saturn_view_w : 320;
    const int vh = (saturn_view_h > 0) ? saturn_view_h : 168;
    int x_offset = (352 - vw) >> 1;
    int clip_y1 = vh - 1;
    v1_write_userclip((volatile uint32_t *)vram32, x_offset, 0, (vw - 1) + x_offset, clip_y1);
#ifdef SATURN_WIN32
    vram32[7] = 0x5A5A0000u;
#endif
    vram32 += 8;

    /* 2. Slot 1: Set local coord to (x_offset, 0) to center viewport in 352-wide screen */
    v1_write_localcoord((volatile uint32_t *)vram32, x_offset, 0);
    vram32 += 8;

    /* 2b. Manual frame-change mode: no hardware erase, clear the view first */
    extern int saturn_v1_manual;
    if (saturn_v1_manual)
    {
        extern int saturn_hw_sky_visible;
        extern int saturn_vdp2_plane_active;   /* RBG0 floor behind VDP1 (test feature) */
        v1_write_clear((volatile uint32_t *)vram32, vw, vh,
                       (saturn_hw_sky_visible || saturn_vdp2_plane_active) ? 0u : V1_CLEAR_OPAQUE_COLR);
        vram32 += 8;
    }

    const sub_range_t *ranges = saturn_sub_ranges;
    const uint32_t *cmd_s_base = (const uint32_t *)saturn_cmd_s;
    const uint32_t *cmd_m = (const uint32_t *)saturn_cmd_m;

    /* Strict Back-To-Front Topological BSP Order:
     * DOOM's BSP tree mathematical traversal front-to-back guarantees that
     * iterating in reverse (saturn_visit_count - 1 down to 0) provides the
     * exact back-to-front topological order. Never re-sort subsectors by bbox
     * centers, which breaks BSP plane partitioning for concave/large sectors! */
    const uint32_t * const vram_limit = (const uint32_t *)(SATURN_VDP1_VRAM + saturn_v1_write_limit);
    /* unchecked fast path only when every quad surely fits before the limit */
    const uint32_t fit_slots = (uint32_t)(vram_limit - (const uint32_t *)vram32) >> 3;
    /* Flat count: saturn_hw_flat_quads, not saturn_cmd_s_count - with
     * HW_FLAT_STEAL the master's stolen flat jobs live in cmd_s[768..)
     * and are referenced by ranges[].s_* but are not in
     * saturn_cmd_s_count (saturn_hw_render.c), so the old sum could pass
     * while the copy ran past the table limit (2026-11-22). */
    extern unsigned int saturn_hw_flat_quads;
    uint32_t flat_q = (uint32_t)saturn_hw_flat_quads;
    if (flat_q < (uint32_t)saturn_cmd_s_count)
        flat_q = (uint32_t)saturn_cmd_s_count;
#if HW_WALK2 && !defined(SATURN_WIN32)
    if (saturn_w2_split_x >= 0)
    {
        /* HW_WALK2 split frame: the master's half [0, x) and the slave's
         * half [x, vw), each behind a user clip of its columns (every
         * drawing command uses the user clip; the walls' own clips stay
         * inside their walker's columns). Each half far-to-near; the halves
         * never overlap, so their order does not matter. */
        extern int saturn_v1w_slotdrop;
        const int sx = saturn_w2_split_x;
        int i;
        saturn_v1w_slotdrop = 0;
        if (sx > 0 && vram32 + 8 <= vram_limit)
        {
            v1_write_userclip((volatile uint32_t *)vram32, x_offset, 0, x_offset + sx - 1, clip_y1);
            vram32 += 8;
            for (i = saturn_visit_count - 1; i >= HW_W2_VISIT_CAP; i--)
            {
                const sub_range_t *r = &ranges[i];
                if (r->s_count > 0)
                    vram32 = deliver_range_32(vram32, vram_limit, cmd_s_base + ((uint32_t)r->s_start << 3), r->s_count);
                if (r->m_count > 0)
                    vram32 = deliver_range_32(vram32, vram_limit, cmd_m + ((uint32_t)r->m_start << 3), r->m_count);
            }
        }
        if (vram32 + 8 <= vram_limit)
        {
            v1_write_userclip((volatile uint32_t *)vram32, x_offset + sx, 0, x_offset + vw - 1, clip_y1);
            vram32 += 8;
            for (i = hw_w2_out_v - 1; i >= 0; i--)
            {
                const sub_range_t *r = &ranges[i];
                if (r->s_count > 0)
                    vram32 = deliver_range_32(vram32, vram_limit, cmd_s_base + ((uint32_t)r->s_start << 3), r->s_count);
                if (r->m_count > 0)
                    vram32 = deliver_range_32(vram32, vram_limit, cmd_m + ((uint32_t)r->m_start << 3), r->m_count);
            }
        }
        if (vram32 + 8 <= vram_limit)      /* full view again for the weapon */
        {
            v1_write_userclip((volatile uint32_t *)vram32, x_offset, 0, (vw - 1) + x_offset, clip_y1);
            vram32 += 8;
        }
    }
    else
#endif
    if (__builtin_expect((uint32_t)saturn_cmd_m_count + (uint32_t)saturn_cmd_sw_count +
                         flat_q + (uint32_t)saturn_cmd_w_count <= fit_slots, 1))
    {
        extern int saturn_v1w_slotdrop;
        saturn_v1w_slotdrop = 0;
        for (int i = saturn_visit_count - 1; i >= 0; i--)
        {
            const sub_range_t *r = &ranges[i];
            int sc = r->s_count;
#ifdef SATURN_WIN32
            uint32_t *od0 = vram32;
#endif
            if (sc > 0)
                vram32 = copy_quads_32(vram32, cmd_s_base + ((uint32_t)r->s_start << 3), sc);
#ifdef SATURN_WIN32
            uint32_t *od1 = vram32;
            od_mark(od0, od1, r->subnum, 1);
#endif
            int mc = r->m_count;
            if (mc > 0)
                vram32 = copy_quads_32(vram32, cmd_m + ((uint32_t)r->m_start << 3), mc);
#ifdef SATURN_WIN32
            {
                int nw = mc;                /* walls first, then this visit's sprites */
                if (i < 1024 && hw_diag_spr0[i] >= r->m_start && hw_diag_spr0[i] < r->m_start + mc)
                    nw = hw_diag_spr0[i] - r->m_start;
                od_mark(od1, od1 + nw * 8, r->subnum, 0);
                od_mark(od1 + nw * 8, vram32, r->subnum, 2);
            }
#endif
        }
    }
    else
    {
        /* Over the table (2026-11-21): the list is delivered far-to-near,
         * so filling until the limit used to silently drop the NEAREST
         * subsectors and the weapon (a missing near half of the view).
         * Keep the weapon, then keep visits from the near end (visit 0 =
         * nearest) while they fit, and drop the FARTHEST ones instead.
         * The dropped command count goes to saturn_v1w_slotdrop (HUD X). */
        extern int saturn_v1w_slotdrop;
        uint32_t avail = fit_slots;
        uint32_t used = 0, dropped = 0;
        int i, keep;
        avail = (avail > (uint32_t)saturn_cmd_w_count) ? avail - (uint32_t)saturn_cmd_w_count : 0u;
        for (keep = 0; keep < saturn_visit_count; keep++)
        {
            uint32_t n = (uint32_t)ranges[keep].s_count + (uint32_t)ranges[keep].m_count;
            if (used + n > avail)
                break;
            used += n;
        }
        for (i = keep; i < saturn_visit_count; i++)
            dropped += (uint32_t)ranges[i].s_count + (uint32_t)ranges[i].m_count;
        saturn_v1w_slotdrop = (int)dropped;
        for (i = keep - 1; i >= 0; i--)
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
    vram32[0] = 0x80000000u; /* CTRL: END (0x8000), LINK (0x0000); VDP1 reads nothing else of it */
    vram32 += 8;

#if HW_CMD_DMA && !defined(SATURN_WIN32)
    /* the body goes in the background; the frame change waits for it */
    if (cmd_dma_on)
        cmd_dma_kick();
    cmd_dma_on = 0;
#endif

    saturn_vdp1_bus_cmd_bytes = (uint32_t)((uintptr_t)vram32 - (uintptr_t)vram_start);
}

/* A list that only clears the 3D view (manual frame-change mode, frames
 * with no 3D: automap, intermission). */
void Saturn_CmdBuf_DeliverBlank(void)
{
    extern unsigned int saturn_v1_write_table;
    volatile uint32_t *vram32 = (volatile uint32_t *)(SATURN_VDP1_VRAM + saturn_v1_write_table);
    extern int saturn_view_w;
    extern int saturn_view_h;
    const int vw = (saturn_view_w > 0) ? saturn_view_w : 320;
    const int vh = (saturn_view_h > 0) ? saturn_view_h : 168;
    const int x_offset = (352 - vw) >> 1;

    Saturn_CmdBuf_DmaWait();
    v1_write_userclip(vram32, x_offset, 0, (vw - 1) + x_offset, vh - 1);
    v1_write_localcoord(vram32 + 8, x_offset, 0);
    v1_write_clear(vram32 + 16, vw, vh, 0u);   /* no 3D: let NBG0 show */
    vram32[24] = 0x80000000u;       /* END */
    for (int i = 25; i < 32; i++)
        vram32[i] = 0;
}
