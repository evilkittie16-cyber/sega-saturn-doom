/* saturn_hw_math.h — 2D Frustum State, Reciprocal Scale LUT (SATURN_OCLUT),
 * SH-2 Inline Assembly Fixed-Point & Projection Math, Hardware DIVU Helpers,
 * and VDP1 11-Bit Coordinate Clampers.
 */

#ifndef SATURN_HW_MATH_H
#define SATURN_HW_MATH_H


/* 2D Map-space AABB Frustum Plane Constants (12-bit fixed point, computed once per frame).
 * Byte indices idx_* / nidx_* are placed at offsets 0..11 (<= 15) and pre-scaled by 2
 * so SH-2 loads each bbox corner in 2 instructions: mov.b @(disp, rFrust), r0; mov.w @(r0, rBbox), rX */
typedef struct {
    int8_t  idx_Nx, idx_Ny;
    int8_t  idx_Lx, idx_Ly;
    int8_t  idx_Rx, idx_Ry;
    int8_t  nidx_Nx, nidx_Ny;
    int8_t  nidx_Lx, nidx_Ly;
    int8_t  nidx_Rx, nidx_Ry;
    int16_t Nx, Ny;
    int16_t Lx, Ly;
    int16_t Rx, Ry;
    int32_t Cn, Cl, Cr;
    int32_t cz_off12, cx_off12;
    fixed_t cz_origin, cx_origin;
} hw_frust_t;
static hw_frust_t hw_frust;

#define HW_BBOX_AT(bbox, b_off) (*(const int16_t *)((const char *)(bbox) + (b_off)))

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

HW_ALWAYS_INLINE int32_t HW_MulsW_PairAdd(int a, int b, int c, int d, int32_t e)
{
#ifndef SATURN_WIN32
    if (__builtin_constant_p(e) && e == 0)
    {
        int32_t res, tmp;
        __asm__(
            "muls.w %2, %3 \n\t"
            "sts    macl, %0 \n\t"
            "muls.w %4, %5 \n\t"
            "sts    macl, %1 \n\t"
            "add    %1, %0 \n\t"
            : "=&r"(res), "=r"(tmp)
            : "r"(a), "r"(b), "r"(c), "r"(d)
            : "macl"
        );
        return res;
    }
    else
    {
        int32_t res = e, tmp;
        __asm__(
            "muls.w %2, %3 \n\t"
            "sts    macl, %1 \n\t"
            "muls.w %4, %5 \n\t"
            "add    %1, %0 \n\t"
            "sts    macl, %1 \n\t"
            "add    %1, %0 \n\t"
            : "+r"(res), "=&r"(tmp)
            : "r"(a), "r"(b), "r"(c), "r"(d)
            : "macl"
        );
        return res;
    }
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
static int          hw_last_sky_max_y = 0;
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
        : "=&r"(res), "=r"(hi)
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
        : "=&r"(*out1), "=r"(*out2), "=&r"(hi1), "=&r"(hi2)
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
    int px = view_cx, tmp;
    __asm__(
        "dmuls.l %2, %3 \n\t"
        "sts     mach, %1 \n\t" /* tmp = hi(cx * s) */
        "add     %1, %0 \n\t"   /* px = view_cx + tmp */
        : "+r"(px), "=r"(tmp)
        : "r"(cx), "r"(s)
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
    int r1 = view_cx, r2, tmp;
    __asm__(
        "dmuls.l %3, %4 \n\t"   /* Start m1 = cx1 * s1 */
        "sts     mach, %2 \n\t" /* tmp = hi(m1) */
        "dmuls.l %5, %6 \n\t"   /* Start m2 = cx2 * s2 */
        "mov     %0, %1 \n\t"   /* px2 = view_cx (hides 1st cycle latency of m2!) */
        "add     %2, %0 \n\t"   /* px1 = view_cx + tmp (hides 2nd cycle latency of m2!) */
        "sts     mach, %2 \n\t" /* tmp = hi(m2) */
        "add     %2, %1 \n\t"   /* px2 = view_cx + tmp */
        : "+r"(r1), "=r"(r2), "=&r"(tmp)
        : "r"(cx1), "r"(s1), "r"(cx2), "r"(s2)
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
    int py = view_cy, tmp;
    __asm__(
        "dmuls.l %2, %3 \n\t"
        "sts     mach, %1 \n\t" /* tmp = hi(dz * s) */
        "sub     %1, %0 \n\t"   /* py = view_cy - tmp */
        : "+r"(py), "=r"(tmp)
        : "r"(dz), "r"(s)
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
    int cx = cx_orig, cz = cz_orig, tmp;
    __asm__(
        "dmuls.l %3, %6 \n\t"   /* MAC = dx * vsin */
        "sts     macl, %2 \n\t" /* tmp = dx * vsin */
        "dmuls.l %4, %5 \n\t"   /* MAC = dy * vcos */
        "sub     %2, %0 \n\t"   /* cx = cx_orig - dx * vsin */
        "sts     macl, %2 \n\t" /* tmp = dy * vcos */
        "dmuls.l %3, %5 \n\t"   /* MAC = dx * vcos */
        "add     %2, %0 \n\t"   /* cx = cx_orig - dx*vsin + dy*vcos */
        "sts     macl, %2 \n\t" /* tmp = dx * vcos */
        "dmuls.l %4, %6 \n\t"   /* MAC = dy * vsin */
        "sub     %2, %1 \n\t"   /* cz = cz_orig - dx*vcos */
        "neg     %0, %0 \n\t"   /* cx = dx*vsin - dy*vcos - cx_orig */
        "sts     macl, %2 \n\t" /* tmp = dy * vsin */
        "sub     %2, %1 \n\t"   /* cz = cz_orig - dx*vcos - dy*vsin */
        "neg     %1, %1 \n\t"   /* cz = dx*vcos + dy*vsin - cz_orig */
        : "+r"(cx), "+r"(cz), "=&r"(tmp)
        : "r"(dx), "r"(dy), "r"(vcos), "r"(vsin)
        : "mach", "macl"
    );
    *out_cz = cz;
    *out_cx = cx;
#else
    *out_cz = (fixed_t)(((int64_t)dx * vcos + (int64_t)dy * vsin) - cz_orig);
    *out_cx = (fixed_t)(((int64_t)dx * vsin - (int64_t)dy * vcos) - cx_orig);
#endif
}

/* 1-cycle 16x16->32 muls.w variant when dx, dy fit in signed int16_t [-32768, 32767] */
HW_ALWAYS_INLINE void ProjectPillar_2D_16(int dx, int dy, int vcos, int vsin,
                                          fixed_t cz_orig, fixed_t cx_orig,
                                          fixed_t *out_cz, fixed_t *out_cx)
{
#ifndef SATURN_WIN32
    int cx = cx_orig, cz = cz_orig, tmp;
    __asm__(
        "muls.w  %3, %6 \n\t"   /* MAC = dx * vsin */
        "sts     macl, %2 \n\t" /* tmp = dx * vsin */
        "muls.w  %4, %5 \n\t"   /* MAC = dy * vcos */
        "sub     %2, %0 \n\t"   /* cx = cx_orig - dx * vsin */
        "sts     macl, %2 \n\t" /* tmp = dy * vcos */
        "muls.w  %3, %5 \n\t"   /* MAC = dx * vcos */
        "add     %2, %0 \n\t"   /* cx = cx_orig - dx*vsin + dy*vcos */
        "sts     macl, %2 \n\t" /* tmp = dx * vcos */
        "muls.w  %4, %6 \n\t"   /* MAC = dy * vsin */
        "sub     %2, %1 \n\t"   /* cz = cz_orig - dx*vcos */
        "sts     macl, %2 \n\t" /* tmp = dy * vsin */
        "sub     %2, %1 \n\t"   /* cz = cz_orig - dx*vcos - dy*vsin */
        "neg     %0, %0 \n\t"   /* cx = dx*vsin - dy*vcos - cx_orig */
        "neg     %1, %1 \n\t"   /* cz = dx*vcos + dy*vsin - cz_orig */
        : "+r"(cx), "+r"(cz), "=&r"(tmp)
        : "r"(dx), "r"(dy), "r"(vcos), "r"(vsin)
        : "macl"
    );
    *out_cz = cz;
    *out_cx = cx;
#else
    *out_cz = (fixed_t)(((int32_t)(int16_t)dx * (int16_t)vcos + (int32_t)(int16_t)dy * (int16_t)vsin) - cz_orig);
    *out_cx = (fixed_t)(((int32_t)(int16_t)dx * (int16_t)vsin - (int32_t)(int16_t)dy * (int16_t)vcos) - cx_orig);
#endif
}

/* 1-cycle 16x16->32 muls.w projection for integer map-space offsets (obj_dx, obj_dy in [-16384, 16384]) */
HW_ALWAYS_INLINE void ProjectMapVertex_2D(int obj_dx, int obj_dy, int vcos, int vsin,
                                          fixed_t cz_orig, fixed_t cx_orig,
                                          fixed_t *out_cz, fixed_t *out_cx)
{
#ifndef SATURN_WIN32
    int cz, cx, tmp;
    __asm__(
        "muls.w  %3, %6 \n\t"   /* MAC = obj_dx * vsin */
        "sts     macl, %0 \n\t"
        "muls.w  %4, %5 \n\t"   /* MAC = obj_dy * vcos */
        "sts     macl, %2 \n\t"
        "muls.w  %3, %5 \n\t"   /* MAC = obj_dx * vcos */
        "sub     %2, %0 \n\t"   /* cx = obj_dx*vsin - obj_dy*vcos */
        "sts     macl, %2 \n\t"
        "muls.w  %4, %6 \n\t"   /* MAC = obj_dy * vsin */
        "shll2   %0     \n\t"
        "sts     macl, %1 \n\t"
        "shll2   %0     \n\t"   /* cx <<= 4 */
        "add     %2, %1 \n\t"   /* cz = obj_dx*vcos + obj_dy*vsin */
        "sub     %8, %0 \n\t"   /* cx -= cx_orig */
        "shll2   %1     \n\t"
        "shll2   %1     \n\t"   /* cz <<= 4 */
        "sub     %7, %1 \n\t"   /* cz -= cz_orig */
        : "=&r"(cx), "=&r"(cz), "=&r"(tmp)
        : "r"(obj_dx), "r"(obj_dy), "r"(vcos), "r"(vsin), "r"(cz_orig), "r"(cx_orig)
        : "macl"
    );
    *out_cz = cz;
    *out_cx = cx;
#else
    *out_cz = (fixed_t)((((int32_t)(int16_t)obj_dx * (int16_t)vcos + (int32_t)(int16_t)obj_dy * (int16_t)vsin) << 4) - cz_orig);
    *out_cx = (fixed_t)((((int32_t)(int16_t)obj_dx * (int16_t)vsin - (int32_t)(int16_t)obj_dy * (int16_t)vcos) << 4) - cx_orig);
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


/* Two independent Y projections cy - hi32(dz * s), absorbing cy - hi32 into dmuls.l latency slots (0 stalls) */
HW_ALWAYS_INLINE void HW_ProjYPair(int cy, int dz1, int s1, int dz2, int s2,
                                   int *out1, int *out2)
{
#ifndef SATURN_WIN32
    int r1 = cy, r2, tmp;
    __asm__(
        "dmuls.l %3, %4 \n\t"   /* Start m1 = dz1 * s1 */
        "sts     mach, %2 \n\t" /* tmp = mach(m1) */
        "dmuls.l %5, %6 \n\t"   /* Start m2 = dz2 * s2 */
        "mov     %0, %1 \n\t"   /* %1 = cy (hides 1st cycle latency of m2) */
        "sub     %2, %0 \n\t"   /* %0 = cy - tmp (hides 2nd cycle latency of m2!) */
        "sts     mach, %2 \n\t" /* tmp = mach(m2) (0 STALLS!) */
        "sub     %2, %1 \n\t"   /* %1 = cy - tmp */
        : "+r"(r1), "=r"(r2), "=&r"(tmp)
        : "r"(dz1), "r"(s1), "r"(dz2), "r"(s2)
        : "mach", "macl"
    );
    *out1 = r1;
    *out2 = r2;
#else
    *out1 = cy - (int)(((int64_t)dz1 * (int64_t)s1) >> 32);
    *out2 = cy - (int)(((int64_t)dz2 * (int64_t)s2) >> 32);
#endif
}

/* Fast 16x16->32 Cross Product for Baked Wall Backface Culling:
 * For any four int16_t values in [-32767, 32767], |a*b - c*d| <= 2 * 32767^2 = 2,147,352,578 < INT32_MAX,
 * so 32-bit signed overflow is mathematically impossible and uses 2-register muls.w instead of 7-register dmuls.l. */
HW_ALWAYS_INLINE int HW_CrossBackface(int obj_dy, int seg_dx, int obj_dx, int seg_dy)
{
    return HW_MulsW_PairAdd(obj_dy, seg_dx, -obj_dx, seg_dy, 0);
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

static HW_NOINLINE HW_COLD_OS int HW_FastDiv_Slow(unsigned int dist)
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

HW_ALWAYS_INLINE int HW_WallLightLevel(int lightlevel, int dist)
{
    if (__builtin_expect((unsigned)lightlevel > 255u, 0))
        lightlevel = (lightlevel < 0) ? 0 : 255;
    int s = lightlevel >> 4;
    unsigned int zb = (unsigned int)dist >> 4;
    if (__builtin_expect(zb > 127, 0)) zb = 127;
    return (int)wall_zlight_level_LUT[s][zb];
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

#endif /* SATURN_HW_MATH_H */
