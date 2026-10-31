/* saturn_hw_flats.c — Dual-CPU Master/Slave SH-2 Floor & Ceiling Flat Quad
 * Pipeline, Near-Plane Sutherland-Hodgman Clipper, and Distance Band Splitter.
 */

/* =========================================================================
 * Slave SH-2 Flat Quad Worker Pipeline
 * Offloads floor and ceiling quadrilateral transformation, clipping, and
 * projection to the Slave SH-2 CPU in parallel with Master seg rendering.
 * Located in Low Work RAM (0x00210000) for zero CPU-bus contention.
 * ========================================================================= */
#define HW_SLAVE_BASE_UNCACHED   SATURN_WRAM_L_IPC_UNCACHED

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
    uint8_t  ap_ytop;
    uint8_t  ap_ybot;
    uint16_t light_and_sub;
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
_Static_assert(sizeof(hw_flat_job_t) == 96, "hw_flat_job_t must be 96 bytes ((ring_idx << 6) + (ring_idx << 5))");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_flat_job_t) == ((HW_JOB_WORDS_BEFORE_PAD + 3) & ~3) * 4, "hw_flat_job_t must be 16-byte aligned for IPC burst copy");
static_assert(sizeof(hw_flat_job_t) == 96, "hw_flat_job_t must be 96 bytes ((ring_idx << 6) + (ring_idx << 5))");
#endif

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
#define HW_MAX_DEF_FLAT_JOBS 4
static HW_ALIGN16 hw_flat_job_t hw_def_flat_jobs[HW_MAX_DEF_FLAT_JOBS];
static int hw_def_flat_count = 0;
static int hw_frame_flat_jobs = 0;
static unsigned int hw_master_job_tail = 0;

HW_ALWAYS_INLINE int HW_FlatQuadDy_PreClamped(int x0, int y0, int x1, int y1,
                                              int x2, int y2, int x3, int y3,
                                              int cy0, int cy1, int cy2, int cy3,
                                              int d03, int d12)
{
    int d01 = cy1 - cy0; if (d01 < 0) d01 = -d01;
    int d32 = cy2 - cy3; if (d32 < 0) d32 = -d32;
    int dy = (d03 > d12) ? d03 : d12;
    if (d01 > dy) dy = d01;
    if (d32 > dy) dy = d32;
    if (__builtin_expect(dy > 24, 1))
    {
        if (((x0 == x1 && y0 == y1) | (x1 == x2 && y1 == y2) |
             (x2 == x3 && y2 == y3) | (x3 == x0 && y3 == y0)))
            dy = 24;
    }
    return dy;
}

HW_ALWAYS_INLINE int HW_FlatQuadDy(int x0, int y0, int x1, int y1,
                                   int x2, int y2, int x3, int y3)
{
    const int vh = HW_VIEW_H;
    int cy0 = (y0 < 0) ? 0 : ((y0 > vh) ? vh : y0);
    int cy1 = (y1 < 0) ? 0 : ((y1 > vh) ? vh : y1);
    int cy2 = (y2 < 0) ? 0 : ((y2 > vh) ? vh : y2);
    int cy3 = (y3 < 0) ? 0 : ((y3 > vh) ? vh : y3);
    int d03 = cy3 - cy0; if (d03 < 0) d03 = -d03;
    int d12 = cy2 - cy1; if (d12 < 0) d12 = -d12;
    return HW_FlatQuadDy_PreClamped(x0, y0, x1, y1, x2, y2, x3, y3,
                                    cy0, cy1, cy2, cy3, d03, d12);
}

HW_ALWAYS_INLINE int HW_FlatTriDy(int y0, int y1, int y2)
{
    int d02 = y2 - y0; if (d02 < 0) d02 = -d02;
    int d12 = y2 - y1; if (d12 < 0) d12 = -d12;
    int d01 = y1 - y0; if (d01 < 0) d01 = -d01;
    int dy = (d02 > d12) ? d02 : d12;
    if (d01 > dy) dy = d01;
    if (dy > 24) dy = 24;
    return dy;
}

/* Takes 3 register arguments (r4=base_srca, r5=quad_size, r6=dy), 0 stack arguments, 0 callee-saved reg pushes */
static HW_NOINLINE uint32_t HW_FlatSizeLOD(uint16_t base_srca, uint32_t quad_size, int dy)
{
    int v0   = (int)(quad_size >> 16);
    int rows = (int)(quad_size & 0xFFu);

    if (dy > 26)
    {
        uint32_t srca = (uint32_t)base_srca + (uint32_t)(v0 << 3);
        return (srca << 16) | 0x0800u | (uint32_t)rows;
    }
    if (dy > 11)
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
    int clip_far = clip_p - (dir * (swap_xy ? 64 : 24));

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

    const int y_top = -2;
    int m_top = (y0 < y_top) | ((y1 < y_top) << 1) | ((y2 < y_top) << 2) | ((y3 < y_top) << 3);
    if (m_top == 15) return 0;
    if (m_top != 0)
        HW_ClipQuadPlane(&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3, y_top, m_top, 1, 0, &ntris, tx, ty, vw, vh);

    const int y_bot = vh + 2;
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

    const int x_left = -4;
    int m_left = (x0 < x_left) | ((x1 < x_left) << 1) | ((x2 < x_left) << 2) | ((x3 < x_left) << 3);
    if (m_left == 15 && ntris == 0) return 0;
    if (m_left != 0 && m_left != 15)
        HW_ClipQuadPlane(&y0, &x0, &y1, &x1, &y2, &x2, &y3, &x3, x_left, m_left, 1, 1, &ntris, tx, ty, vw, vh);

    const int x_right = vw + 4;
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
    int r_lim = vw + 16;
    int b_lim = vh + 8;
    if (((x0 + 16) | (x1 + 16) | (x2 + 16) | (x3 + 16) |
         (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
         (y0 + 8) | (y1 + 8) | (y2 + 8) | (y3 + 8) |
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
    uint8_t                 ap_ytop;
    uint8_t                 ap_ybot;
    int                     clip_xy[8];
} hw_flat_emit_ctx_t;

static HW_ALIGN16 hw_flat_emit_ctx_t hw_fctx_master;
#ifndef SATURN_WIN32
static HW_ALIGN16 hw_flat_emit_ctx_t hw_fctx_slave;
#endif

static HW_NOINLINE HW_HOT_O2 int HW_Slave_IsSpanOccluded(const hw_flat_emit_ctx_t *fctx, int x0, int x1, int vw)
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
        uint32_t w2 = HW_FlatSizeLOD(base_srca, quad_size, dy);
        c32[0] = (cw < 0) ? 0x00320000u : 0x00220000u;
        c32[1] = w1;
        c32[2] = w2;
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

    int r_lim = vw + 16;
    int b_lim = vh + 8;

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

        if (__builtin_expect(((x0 + 16) | (x1 + 16) | (x2 + 16) | (x3 + 16) |
                              (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
                              (y0 + 8) | (y1 + 8) | (y2 + 8) | (y3 + 8) |
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
            uint32_t w2 = HW_FlatSizeLOD(base_srca, strip_size, dy);
            c32[0] = (cw < 0) ? 0x00320000u : 0x00220000u;
            c32[1] = w1;
            c32[2] = w2;
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
    if (__builtin_expect(!(fctx->fully_inside & HW_FLAT_USE_LOD1), 1))
    {
        if (__builtin_expect(cz_L_min < (112 << FRACBITS) && d03 > 40, 0))
            multi_bits |= 0x4000u;
        if (__builtin_expect(cz_R_min < (112 << FRACBITS) && d12 > 40, 0))
            multi_bits |= 0x8000u;
    }

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

        int r_lim = fctx->vw + 16;
        int b_lim = vh + 8;
        if (__builtin_expect(((x0 + 16) | (x1 + 16) | (x2 + 16) | (x3 + 16) |
                              (r_lim - x0) | (r_lim - x1) | (r_lim - x2) | (r_lim - x3) |
                              (y0 + 8) | (y1 + 8) | (y2 + 8) | (y3 + 8) |
                              (b_lim - y0) | (b_lim - y1) | (b_lim - y2) | (b_lim - y3)) < 0, 0))
        {
            fctx->clip_xy[0] = x0; fctx->clip_xy[1] = y0;
            fctx->clip_xy[2] = x1; fctx->clip_xy[3] = y1;
            fctx->clip_xy[4] = x2; fctx->clip_xy[5] = y2;
            fctx->clip_xy[6] = x3; fctx->clip_xy[7] = y3;
            return HW_EmitFlatQuad_Clipped(fctx, local_q, w1, quad_size | (qpack & 0x80000000u));
        }

        uint16_t base_srca = (qpack & 0x80000000u) ? fctx->ceil_srca : fctx->floor_srca;
        int dy = HW_FlatQuadDy_PreClamped(x0, y0, x1, y1, x2, y2, x3, y3,
                                          cy0, cy1, cy2, cy3, d03, d12);
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

    if (fctx->emit_ceil && max_yc > (int)fctx->ap_ytop && min_yc < (int)fctx->ap_ybot)
    {
        unsigned int zb = ((unsigned int)min_cz + (unsigned int)max_cz) >> 21;
        if (__builtin_expect(zb > 127u, 0)) zb = 127u;
        int qcount = local_q;
        HW_Worker_EmitPolyQuads(cmd_s_out, &qcount, qcap, sx, sy_c, n_clip, fctx->ceil_srca, (int)flat_cmdcolr_lut[zb], vw, vh);
        local_q = qcount;
    }

    if (fctx->emit_floor && max_yf > (int)fctx->ap_ytop && min_yf < (int)fctx->ap_ybot)
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
    int ap_ytop   = (int)(w8 >> 24);
    int ap_ybot   = (int)((w8 >> 16) & 0xFFu);
    int sec_light = (int)((w8 >> 12) & 0x0Fu);
    int sub_idx   = (int)(w8 & 0x0FFFu);
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
    int ap_ytop   = (int)job->ap_ytop;
    int ap_ybot   = (int)job->ap_ybot;
    int sec_light = (int)((job->light_and_sub >> 12) & 0x0Fu);
    int sub_idx   = (int)(job->light_and_sub & 0x0FFFu);
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
            fctx->qcap      = HW_SLAVE_FLAT_Q_CAP;
            fctx->local_q   = 0;
            fctx->solid_any     = 0;
            fctx->solid_summary = 0;
            fctx->proj_v        = hw_proj_verts_slave;
        }
    }
    else
#endif
    {
        fctx->cam_x16   = hw_cam_x * 16;
        fctx->cam_y16   = hw_cam_y * 16;
        fctx->vcos      = hw_viewcos >> 4;
        fctx->vsin      = hw_viewsin >> 4;
        fctx->cz_origin = hw_frust.cz_origin;
        fctx->cx_origin = hw_frust.cx_origin;
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
    fctx->ap_ytop          = (uint8_t)ap_ytop;
    fctx->ap_ybot          = (uint8_t)ap_ybot;

#ifndef SATURN_WIN32
    if (solid_count <= 0)
    {
        fctx->solid_any     = 0;
        fctx->solid_summary = 0;
    }
    else
    {
        if (is_master)
        {
            fctx->solid_any     = solid_dirty;
            fctx->solid_summary = jw[9];
        }
        else if (__builtin_expect(solid_dirty != 0, 0))
        {
            fctx->solid_summary = jw[9];
            fctx->solid_any |= solid_dirty;
        }
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
#else
    fctx->solid_any     = (solid_count <= 0) ? 0 : solid_dirty;
    fctx->solid_summary = (solid_count <= 0) ? 0 : job->solid_summary;
    if (solid_count > 0 && solid_dirty != 0)
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
        int s;
        if (__builtin_expect((((unsigned int)dx + 22400u) | ((unsigned int)dy + 22400u)) <= 44800u, 1))
        {
            ProjectPillar_2D_16(dx, dy, vcos, vsin, cz_origin, cx_origin, &cz, &cx);
            p_ptr->cx = cx;
            p_ptr->cz = cz;
            if (cz < HW_NEAR_Z)
                goto vert_behind;
            s = (int)SATURN_OCLUT[(unsigned int)cz >> FRACBITS];
        }
        else
        {
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
            if (cz < HW_NEAR_Z)
                goto vert_behind;
            s = HW_FastDiv(cz);
        }

        {

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
                if (__builtin_expect((((unsigned int)(px + 8192) | (unsigned int)(pyc + 8192) | (unsigned int)(pyf + 8192)) > 16383u), 0))
                {
                    px  = HW_CLAMP_PROJ_WIDE(px);
                    pyc = HW_CLAMP_PROJ_WIDE(pyc);
                    pyf = HW_CLAMP_PROJ_WIDE(pyf);
                }
                aliased_uint32_t *pw = (aliased_uint32_t *)p_ptr;
                pw[2] = ((uint32_t)(uint16_t)px << 16) | (uint16_t)pyc;
                pw[3] = ((uint32_t)(uint16_t)pyf << 16) | 0x0100u;
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
                if (__builtin_expect((((unsigned int)(px + 8192) | (unsigned int)(py + 8192)) > 16383u), 0))
                {
                    px = HW_CLAMP_PROJ_WIDE(px);
                    py = HW_CLAMP_PROJ_WIDE(py);
                }
                aliased_uint32_t *pw = (aliased_uint32_t *)p_ptr;
                pw[2] = ((uint32_t)(uint16_t)px << 16) | (uint16_t)py;
                pw[3] = ((uint32_t)(uint16_t)py << 16) | 0x0100u;
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
            continue;
        }
vert_behind:
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

            int min_x = px0; if (px1 < min_x) min_x = px1; if (px2 < min_x) min_x = px2; if (px3 < min_x) min_x = px3;
            int max_x = px0; if (px1 > max_x) max_x = px1; if (px2 > max_x) max_x = px2; if (px3 > max_x) max_x = px3;

            if (max_x <= 0 || min_x >= vw || min_x >= max_x)
                continue;

            int cl_min_x = (min_x < 0) ? 0 : min_x;
            int cl_max_x = (max_x > vw) ? vw : max_x;
            if ((fctx->solid_any & hw_bit_lut[cl_min_x >> 5]) &&
                HW_Slave_IsSpanOccluded(fctx, cl_min_x, cl_max_x, vw))
                continue;

            int dx20 = px2 - px0;
            int dx13 = px1 - px3;

            if (dx20 == 0 && dx13 == 0)
                continue;

            int cw_c = 0, cw_f = 0;
            if (emit_ceil)
            {
                int py0_c = pv0->py_c, py1_c = pv1->py_c, py2_c = pv2->py_c, py3_c = pv3->py_c;
                if ((((ap_ytop - py0_c) | (ap_ytop - py1_c) | (ap_ytop - py2_c) | (ap_ytop - py3_c)) &
                     ((py0_c - ap_ybot) | (py1_c - ap_ybot) | (py2_c - ap_ybot) | (py3_c - ap_ybot))) < 0)
                {
                    cw_c = HW_MulsW_PairAdd(dx20, py3_c - py1_c, dx13, py2_c - py0_c, 0);
                }
            }
            if (emit_floor)
            {
                int py0_f = pv0->py_f, py1_f = pv1->py_f, py2_f = pv2->py_f, py3_f = pv3->py_f;
                if ((((ap_ytop - py0_f) | (ap_ytop - py1_f) | (ap_ytop - py2_f) | (ap_ytop - py3_f)) &
                     ((py0_f - ap_ybot) | (py1_f - ap_ybot) | (py2_f - ap_ybot) | (py3_f - ap_ybot))) < 0)
                {
                    cw_f = HW_MulsW_PairAdd(dx20, py3_f - py1_f, dx13, py2_f - py0_f, 0);
                }
            }

            if ((cw_c | cw_f) == 0)
                continue;

            uint32_t qpack_base = 64u << 16;
            if (__builtin_expect(!(fully_inside & HW_FLAT_USE_LOD1), 1))
            {
                int wy0 = verts[qv0].y, wy1 = verts[qv1].y;
                int wy2 = verts[qv2].y, wy3 = verts[qv3].y;
                int y_bot = (wy0 + wy1) >> 1;
                int y_top = (wy2 + wy3) >> 1;
                if (wy2 >= wy1 && wy3 >= wy0 && y_top > y_bot)
                {
                    int dy = y_top - y_bot;
                    if (dy <= 64)
                    {
                        int vn = (-y_top) & 63;
                        if (vn + dy <= 64)
                            qpack_base = ((uint32_t)vn << 24) | ((uint32_t)dy << 16);
                    }
                }
            }

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

    hw_fctx_slave.cache_gen = ipc->job_done;
    hw_fctx_slave.local_q   = 0;

    while (1)
    {
        /* Pure lockless SPSC spin-wait with 64-cycle in-register bus yield */
        while ((int)(head - ipc->job_tail) >= 0)
        {
            HW_BUS_YIELD();
        }
        __asm__ volatile("" ::: "memory");

        const volatile hw_flat_job_t *uncached_job = &ipc->jobs[head & (HW_JOB_RING - 1)];
        HW_Worker_ProcessJob(0, ipc, uncached_job, &hw_fctx_slave);
        head++;
        __asm__ volatile("" ::: "memory");
        if ((int)(head - ipc->job_tail) >= 0)
            ipc->quad_count = hw_fctx_slave.local_q;
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
     * to our dedicated 16 KB Slave Stack in Low Work RAM (0x0021BFF0) before creating
     * any C stack frame. */
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

typedef struct
{
    const sector_t *sec;
    uint32_t        srca_pack;
    uint16_t        floor_srca;
    uint16_t        ceil_srca;
    uint16_t        light_bits;
    uint8_t         emit_floor;
    uint8_t         emit_ceil;
    uint8_t         is_sky;
    uint8_t         pad;
    fixed_t         y_floor_w;
    fixed_t         y_ceil_w;
} hw_flat_sec_cache_t;

static hw_flat_sec_cache_t hw_flat_sec_cache = { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void HW_RenderSubsectorBakedFlats(int sub_idx, int subnum, sector_t *sec, int fully_inside);

static HW_NOINLINE HW_HOT_O2 void HW_CacheFlatSectorInfo(const sector_t *sec)
{
    const fixed_t vz = hw_viewz;
    int is_sky     = ((int16_t)sec->ceilingpic == -1);
    int emit_ceil  = (vz < sec->ceilingheight && !is_sky);
    int emit_floor = (vz > sec->floorheight && ((int16_t)sec->floorpic != -1));
    if (saturn_vdp2_plane_active && sec == &sectors[saturn_vdp2_plane_sec])
        emit_floor = 0;

    int floor_srca = 0;
    int ceil_srca  = 0;
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

    hw_flat_sec_cache_t *fc = &hw_flat_sec_cache;
    fc->sec        = sec;
    fc->srca_pack  = ((uint32_t)(uint16_t)floor_srca << 16) | (uint16_t)ceil_srca;
    fc->floor_srca = (uint16_t)floor_srca;
    fc->ceil_srca  = (uint16_t)ceil_srca;
    fc->light_bits = (uint16_t)(((sec->lightlevel >> 4) & 15) << 12);
    fc->emit_floor = (uint8_t)emit_floor;
    fc->emit_ceil  = (uint8_t)emit_ceil;
    fc->is_sky     = (uint8_t)is_sky;
    fc->y_floor_w  = HW_DZQ(sec->floorheight, vz);
    fc->y_ceil_w   = HW_DZQ(sec->ceilingheight, vz);
}

#ifndef SATURN_WIN32
static HW_NOINLINE HW_COLD_OS void HW_DeferMasterFlatJob(
    uint32_t v_first, uint32_t q_first, uint32_t vq_pack, uint32_t w3_pack, uint32_t sub_idx)
{
    const hw_flat_sec_cache_t *fc = &hw_flat_sec_cache;
    aliased_uint32_t *mw = (aliased_uint32_t *)&hw_def_flat_jobs[hw_def_flat_count++];
    uint32_t s_any = hw_solid_any;
    mw[0]  = (uint32_t)&saturn_baked_flat_verts[v_first];
    mw[1]  = (uint32_t)&saturn_baked_flat_quads[q_first];
    mw[2]  = vq_pack;
    mw[3]  = w3_pack;
    mw[4]  = fc->srca_pack;
    mw[5]  = (uint32_t)fc->y_floor_w;
    mw[6]  = (uint32_t)(saturn_baked_flat_vfrac ? (saturn_baked_flat_vfrac + v_first) : NULL);
    mw[7]  = (uint32_t)fc->y_ceil_w;
    mw[8]  = ((uint32_t)HW_APERTURE->cur_sub_ytop << 24) |
             ((uint32_t)HW_APERTURE->cur_sub_ybot << 16) |
             (uint32_t)fc->light_bits | (sub_idx & 0x0FFFu);
    mw[9]  = hw_solid_summary;
    mw[10] = ((uint32_t)(uint16_t)(int16_t)hw_solid_count << 16) | (uint16_t)s_any;
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
}
#endif

/* Takes 4 register arguments (r4=sub_idx, r5=subnum, r6=sec, r7=fully_inside), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 void HW_SubmitSlaveFlatJob(int sub_idx, int subnum, sector_t *sec, int fully_inside)
{
#ifndef SATURN_WIN32
    if (!saturn_baked_flat_q_count)
        return;
    const hw_sub_flat_info_t *fi = &HW_SUB_FLAT_INFO[subnum];
    int q_count = (int)fi->q_count;
    int v_count = (int)fi->v_count;
    if (q_count <= 0 || v_count <= 0)
        return;

    hw_flat_sec_cache_t *fc = &hw_flat_sec_cache;
    if (__builtin_expect(sec != fc->sec, 0))
        HW_CacheFlatSectorInfo(sec);

    uint32_t u_flags = (uint32_t)fully_inside;
    uint32_t allow_sky  = (u_flags >> 3) & 1u;
    uint32_t emit_floor = (uint32_t)fc->emit_floor & ((u_flags >> 2) & 1u);
    uint32_t emit_ceil  = (uint32_t)fc->emit_ceil  & allow_sky;
    if (!(emit_ceil | emit_floor))
        return;

    uint32_t v_first, q_first;
    int l_qcount = (int)fi->lod1_q_count;
    int l_vcount = (int)fi->lod1_v_count;
    if ((u_flags & 16u) && l_qcount > 0 && l_vcount > 0)
    {
        if (q_count > l_qcount)
            saturn_hw_flat_lod_saved_quads += (unsigned int)(q_count - l_qcount);
        q_first = fi->lod1_q_first;
        q_count = l_qcount;
        v_first = fi->lod1_v_first;
        v_count = l_vcount;
        saturn_hw_flat_lod1_subs++;
    }
    else
    {
        v_first = fi->v_first;
        q_first = fi->q_first;
        saturn_hw_flat_lod0_subs++;
    }

    uint32_t sky_flags = (uint32_t)fc->is_sky & allow_sky;
    uint32_t w3_pack = (emit_floor << 24) | (emit_ceil << 16) | ((u_flags & 3u) << 8) | sky_flags;
    uint32_t vq_pack = ((uint32_t)(uint16_t)v_count << 16) | (uint16_t)q_count;
    int fjob_idx = ++hw_frame_flat_jobs;

    if (__builtin_expect(!saturn_hw_sky_visible && fjob_idx > 36 &&
                         ((fjob_idx & 1) == 0) &&
                         hw_def_flat_count < HW_MAX_DEF_FLAT_JOBS &&
                         saturn_master_flat_q + (q_count << 1) + 8 < CMDBUF_S_CAP, 0))
    {
        HW_DeferMasterFlatJob(v_first, q_first, vq_pack, w3_pack, (uint32_t)sub_idx);
        return;
    }

    unsigned int tail = hw_master_job_tail;
    hw_master_job_tail = tail + 1;
    unsigned int ring_idx = tail & (HW_JOB_RING - 1);
    volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

    /* Write directly to uncached IPC ring so Master L1 cache is never polluted by write-only IPC jobs */
    volatile aliased_uint32_t *jw = (volatile aliased_uint32_t *)((uintptr_t)ipc->jobs + (ring_idx << 6) + (ring_idx << 5));
    jw[0] = (uint32_t)&saturn_baked_flat_verts[v_first];
    jw[1] = (uint32_t)&saturn_baked_flat_quads[q_first];
    jw[2] = vq_pack;
    uint32_t dirty = hw_slave_dirty_words;
    if (__builtin_expect(hw_first_flat_job, 0))
    {
        hw_first_flat_job = 0;
        w3_pack |= 2u;
        dirty = hw_solid_any;
    }
    hw_slave_dirty_words = 0;
    jw[3] = w3_pack;
    jw[4] = fc->srca_pack;
    jw[5] = (uint32_t)fc->y_floor_w;
    jw[6] = (uint32_t)(saturn_baked_flat_vfrac ? (saturn_baked_flat_vfrac + v_first) : NULL);
    jw[7] = (uint32_t)fc->y_ceil_w;
    jw[8] = ((uint32_t)HW_APERTURE->cur_sub_ytop << 24) |
            ((uint32_t)HW_APERTURE->cur_sub_ybot << 16) |
            (uint32_t)fc->light_bits | ((uint32_t)sub_idx & 0x0FFFu);
    jw[10] = ((uint32_t)(uint16_t)(int16_t)hw_solid_count << 16) | (uint16_t)dirty;

    if (__builtin_expect(dirty != 0, 0))
    {
        jw[9] = hw_solid_summary;
        const uint32_t *sm = hw_solid_mask;
        volatile aliased_uint32_t *js = &jw[11];
        uint32_t d = dirty;
        for (int w = 0; d != 0; w++, d >>= 1)
        {
            if (d & 1u)
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
    if (!saturn_baked_flat_q_count)
        return;
    const hw_sub_flat_info_t *fi = &HW_SUB_FLAT_INFO[subnum];
    int q_count = (int)fi->q_count;
    int v_count = (int)fi->v_count;
    if (q_count <= 0 || v_count <= 0)
        return;

    const fixed_t vz = hw_viewz;
    int allow_sky  = (fully_inside >> 3) & 1;
    int use_lod1   = (fully_inside >> 4) & 1;
    int emit_ceil  = (vz < sec->ceilingheight && ((int16_t)sec->ceilingpic != -1));
    int emit_floor = (vz > sec->floorheight && ((int16_t)sec->floorpic != -1));
    if (saturn_vdp2_plane_active && sec == &sectors[saturn_vdp2_plane_sec]) emit_floor = 0;
    emit_floor &= (fully_inside >> 2) & 1;
    emit_ceil  &= allow_sky;
    fully_inside &= 3;

    if (!emit_ceil && !emit_floor)
        return;

    int floor_srca = 0, ceil_srca = 0;
    if (emit_floor) { floor_srca = HW_FastFlatSrca(sec->floorpic); if (!floor_srca) emit_floor = 0; }
    if (emit_ceil)  { ceil_srca = HW_FastFlatSrca(sec->ceilingpic); if (!ceil_srca) emit_ceil = 0; }
    if (!emit_ceil && !emit_floor) return;

    int v_first, q_first;
    int l_qcount = (int)fi->lod1_q_count;
    int l_vcount = (int)fi->lod1_v_count;
    if (use_lod1 && l_qcount > 0 && l_vcount > 0)
    {
        if (q_count > l_qcount)
            saturn_hw_flat_lod_saved_quads += (unsigned int)(q_count - l_qcount);
        q_first = (int)fi->lod1_q_first;
        q_count = l_qcount;
        v_first = (int)fi->lod1_v_first;
        v_count = l_vcount;
        saturn_hw_flat_lod1_subs++;
    }
    else
    {
        v_first = (int)fi->v_first;
        q_first = (int)fi->q_first;
        saturn_hw_flat_lod0_subs++;
    }

    hw_flat_job_t job __attribute__((aligned(16)));
    job.verts = &saturn_baked_flat_verts[v_first];
    job.quads = &saturn_baked_flat_quads[q_first];
    job.v_count = (uint16_t)v_count;
    job.q_count = (uint16_t)q_count;
    job.emit_floor = (uint8_t)emit_floor;
    job.emit_ceil = (uint8_t)emit_ceil;
    job.fully_inside = (uint8_t)fully_inside;
    job.has_sky = (uint8_t)(((int16_t)sec->ceilingpic == -1) && allow_sky);
    job.floor_srca = (uint16_t)floor_srca;
    job.ceil_srca = (uint16_t)ceil_srca;
    job.y_floor_w = HW_DZQ(sec->floorheight, vz);
    job.vfrac = saturn_baked_flat_vfrac ? (saturn_baked_flat_vfrac + v_first) : NULL;
    job.y_ceil_w = HW_DZQ(sec->ceilingheight, vz);
    job.ap_ytop = HW_APERTURE->cur_sub_ytop;
    job.ap_ybot = HW_APERTURE->cur_sub_ybot;
    job.light_and_sub = (uint16_t)((((sec->lightlevel >> 4) & 15) << 12) | (sub_idx & 0x0FFFu));
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

