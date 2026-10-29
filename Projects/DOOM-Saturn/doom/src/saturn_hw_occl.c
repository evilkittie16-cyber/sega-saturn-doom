/* saturn_hw_occl.c — 1D Solid-Wall Span Occlusion Bitmask, Low Work RAM
 * Scratch Layout (hw_wram_l_scratch_t), 2D Vertical Portal Aperture, and
 * PVS BSP Node Pruning.
 */

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

/* hw_mask_right[0..31] is identical to hw_span_mask_lut[1..32]; aliasing saves
 * 128 bytes of .rodata (8 L1 cache lines) and 1 literal-pool base register. */
#define hw_mask_right (hw_span_mask_lut + 1)

static uint32_t hw_solid_summary = 0; /* Bitmask of fully solid words (0xFFFFFFFFu) */
static uint32_t hw_solid_any = 0;     /* Bitmask of words with ANY solid pixels (!= 0u) */
static uint32_t hw_slave_dirty_words = 0; /* Words modified in hw_solid_mask since last Slave flat job */

static HW_NOINLINE HW_HOT_O2 int HW_IsSpanOccluded(int x0, int x1)
{
    if (hw_solid_count == 0)
        return 0;
    if (hw_solid_count >= HW_VIEW_W)
        return 1;
    if (x0 < 0) x0 = 0;
    if (x1 > HW_VIEW_W) x1 = HW_VIEW_W;
    if (x0 >= x1)
        return 0;

    int x1_m1 = x1 - 1;
    int w0 = (int)((unsigned int)x0 >> 5);
    int b0 = x0 & 31;
    int b1 = x1_m1 & 31;

    if (__builtin_expect((unsigned int)(x0 ^ x1_m1) < 32u, 1))
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        return ((hw_solid_mask[w0] & mask) == mask);
    }

    int w1 = (int)((unsigned int)x1_m1 >> 5);
    uint32_t all_words = hw_span_mask_lut[w1 + 1] ^ hw_span_mask_lut[w0];
    if ((hw_solid_any & all_words) != all_words)
        return 0;

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

HW_ALWAYS_INLINE int HW_IsClippedSpanOccluded(int x0, int x1)
{
    int x1_m1 = x1 - 1;
    if (__builtin_expect((unsigned int)(x0 ^ x1_m1) < 32u, 1))
    {
        uint32_t mask = hw_mask_left[x0 & 31] & hw_mask_right[x1_m1 & 31];
        return ((hw_solid_mask[(unsigned int)x0 >> 5] & mask) == mask);
    }
    return HW_IsSpanOccluded(x0, x1);
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
    if (hw_solid_count >= HW_VIEW_W)
        return 2;
    if (x0 < 0) x0 = 0;
    if (x1 > HW_VIEW_W) x1 = HW_VIEW_W;
    if (x0 >= x1)
        return 0;

    int x1_m1 = x1 - 1;
    int w0 = (int)((unsigned int)x0 >> 5);
    int b0 = x0 & 31;
    int b1 = x1_m1 & 31;

    if (__builtin_expect((unsigned int)(x0 ^ x1_m1) < 32u, 1))
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        uint32_t hit  = hw_solid_mask[w0] & mask;
        if (hit == 0)
            return 0;
        return (hit == mask) ? 2 : 1;
    }

    int w1 = (int)((unsigned int)x1_m1 >> 5);
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
    int x1_m1 = x1 - 1;
    int w0 = (int)((unsigned int)x0 >> 5);
    int b0 = x0 & 31;
    int b1 = x1_m1 & 31;
    if (__builtin_expect((unsigned int)(x0 ^ x1_m1) < 32u, 1))
    {
        if (!sub_solid_active)
        {
            sub_solid_active = 1;
            sub_solid_min_w  = w0;
            sub_solid_max_w  = w0;
        }
        else
        {
            if (w0 < sub_solid_min_w) sub_solid_min_w = w0;
            if (w0 > sub_solid_max_w) sub_solid_max_w = w0;
        }
        sub_solid_mask[w0] |= (hw_mask_left[b0] & hw_mask_right[b1]);
        return;
    }
    int w1 = (int)((unsigned int)x1_m1 >> 5);
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
    sub_solid_mask[w0] |= hw_mask_left[b0];
    for (int w = w0 + 1; w < w1; w++)
    {
        sub_solid_mask[w] = 0xFFFFFFFFu;
    }
    sub_solid_mask[w1] |= hw_mask_right[b1];
}

typedef struct {
    int16_t sx;
    int16_t ex;
    uint8_t ytop;
    uint8_t ybot;
    uint8_t yt_att;
    uint8_t yb_att;
} hw_portal_span_t; /* 8 bytes (1 << 3) */

typedef struct {
    int16_t yt_min;
    int16_t yb_max;
    int16_t yt_max;
    int16_t yb_min;
} hw_ap_word_t; /* 8 bytes (1 << 3) */

typedef struct {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    int16_t dx;
    int16_t dy;
    int16_t back_sec_idx;
    uint8_t chain_prev;
    uint8_t pad;
} hw_portal_seg_t;

#define HW_NODE_PVS_WORDS     32
#define HW_MAX_PORTAL_SPANS   32
#define HW_MAX_AP_SECTORS     256
#define HW_MAX_AP_SUBSECTORS  1024
#define HW_MAX_PORTAL_SEGS    510

/* Subsector Flat Evaluation & LOD Bitmask Flags */
#define HW_FLAT_INSIDE_MASK   0x03u
#define HW_FLAT_ALLOW_FLOOR   0x04u
#define HW_FLAT_ALLOW_CEIL    0x08u
#define HW_FLAT_ALLOW_BOTH    (HW_FLAT_ALLOW_FLOOR | HW_FLAT_ALLOW_CEIL)
#define HW_FLAT_USE_LOD1      0x10u
#define HW_FLAT_CHECK_AP      0x100u
#define HW_FLAT_NEAR_OR_PSEC  0x200u
#define HW_FLAT_AP_OR_NEAR    (HW_FLAT_CHECK_AP | HW_FLAT_NEAR_OR_PSEC)

typedef struct {
    uint16_t q_first;
    uint16_t v_first;
    uint16_t q_count;
    uint16_t v_count;
    uint16_t lod1_q_first;
    uint16_t lod1_v_first;
    uint16_t lod1_q_count;
    uint16_t lod1_v_count;
} HW_ALIGN16 hw_sub_flat_info_t;

/* Unified Low Work RAM Scratch Layout at SATURN_WRAM_L_PILLARS_ADDR (32 KB capacity).
 * All tables placed into a single C struct so the compiler enforces non-overlapping
 * offsets and bounds-checks total size via _Static_assert. */
typedef struct {
    hw_sub_flat_info_t sub_flat_info[HW_MAX_AP_SUBSECTORS]; /* 16,384 B (0x0000..0x3FFF) */
    hw_portal_seg_t    portal_segs[HW_MAX_PORTAL_SEGS];     /*  8,160 B (0x4000..0x5FDF) */
    int32_t            sky_col_bam[HW_MAX_SCREEN_W];        /*  1,408 B (0x5FE0..0x655F) */
} hw_wram_l_scratch_t;

typedef struct {
    /* Bytes 0..23: hot scalars at offset 0 for 1-cycle @(disp, rAp) access on SH-2 */
    uint8_t          cur_sub_ytop;
    uint8_t          cur_sub_ybot;
    uint8_t          aperture_active;
    uint8_t          portal_span_count;
    int16_t          cur_sub_yt_attach;
    int16_t          cur_sub_yb_attach;
    uint32_t         dirty_words;
    const void      *last_pvs_ssec;
    int              last_pvs_numnodes;
    int16_t          cur_ytop_min;
    int16_t          cur_ybot_max;
    /* Arrays */
    hw_portal_span_t portal_spans[HW_MAX_PORTAL_SPANS];
    uint32_t         node_pvs_vis[HW_NODE_PVS_WORDS];
    uint8_t          ytop[HW_MAX_SCREEN_W];
    uint8_t          ybot[HW_MAX_SCREEN_W];
    int16_t          sec_yt_attach[HW_MAX_AP_SECTORS];
    int16_t          sec_yb_attach[HW_MAX_AP_SECTORS];
} hw_aperture_state_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(hw_sub_flat_info_t) == 16,
               "hw_sub_flat_info_t must be 16 bytes (1 SH-2 cache line)");
_Static_assert(sizeof(hw_wram_l_scratch_t) <= SATURN_WRAM_L_PILLARS_SIZE,
               "hw_wram_l_scratch_t must fit inside 32 KB SATURN_WRAM_L_PILLARS_ADDR region");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_sub_flat_info_t) == 16,
              "hw_sub_flat_info_t must be 16 bytes (1 SH-2 cache line)");
static_assert(sizeof(hw_wram_l_scratch_t) <= SATURN_WRAM_L_PILLARS_SIZE,
              "hw_wram_l_scratch_t must fit inside 32 KB SATURN_WRAM_L_PILLARS_ADDR region");
#endif

static HW_ALIGN16 hw_aperture_state_t hw_aperture_state;
static HW_ALIGN16 hw_ap_word_t hw_ap_words[HW_SOLID_WORDS];
#define HW_APERTURE       (&hw_aperture_state)
#define HW_WRAM_L_SCRATCH ((hw_wram_l_scratch_t *)SATURN_WRAM_L_PILLARS_ADDR)
#define HW_SUB_FLAT_INFO  (HW_WRAM_L_SCRATCH->sub_flat_info)
#define HW_PORTAL_SEGS    (HW_WRAM_L_SCRATCH->portal_segs)
#define HW_SKY_COL_BAM    (HW_WRAM_L_SCRATCH->sky_col_bam)

static HW_NOINLINE HW_HOT_O2 void HW_RecordPortalSpan(int sx_ex, int ytop, int ybot, int yt_yb_att)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int sx = sx_ex >> 16;
    int ex = (int16_t)sx_ex;
    int yt_att = yt_yb_att >> 16;
    int yb_att = (int16_t)yt_yb_att;
    if (ytop < 0) ytop = 0;
    else if (ytop > HW_VIEW_H) ytop = HW_VIEW_H;
    if (ybot < 0) ybot = 0;
    else if (ybot > HW_VIEW_H) ybot = HW_VIEW_H;
    if (yt_att < 0) yt_att = 0;
    else if (yt_att > HW_VIEW_H) yt_att = HW_VIEW_H;
    if (yb_att < 0) yb_att = 0;
    else if (yb_att > HW_VIEW_H) yb_att = HW_VIEW_H;
    if (yt_att > ytop + 1) ytop = 0;
    if (yb_att < ybot - 1) ybot = HW_VIEW_H;
    if (sx >= ex || (ytop <= 0 && ybot >= HW_VIEW_H))
        return;
    int idx = ap->portal_span_count;
    hw_portal_span_t *spans = ap->portal_spans;
    if (idx > 0)
    {
        hw_portal_span_t *prev = &spans[idx - 1];
        if (prev->sx == sx && prev->ex == ex)
        {
            if (ytop > prev->ytop) prev->ytop = (uint8_t)ytop;
            if (ybot < prev->ybot) prev->ybot = (uint8_t)ybot;
            if (yt_att < prev->yt_att) prev->yt_att = (uint8_t)yt_att;
            if (yb_att > prev->yb_att) prev->yb_att = (uint8_t)yb_att;
            return;
        }
    }
    if (idx < HW_MAX_PORTAL_SPANS)
    {
        hw_portal_span_t *cur = &spans[idx];
        cur->sx     = (int16_t)sx;
        cur->ex     = (int16_t)ex;
        cur->ytop   = (uint8_t)ytop;
        cur->ybot   = (uint8_t)ybot;
        cur->yt_att = (uint8_t)yt_att;
        cur->yb_att = (uint8_t)yb_att;
        ap->portal_span_count = (uint8_t)(idx + 1);
    }
}

static HW_NOINLINE HW_HOT_O2 int HW_GetSpanAperture(int sx, int ex)
{
    const hw_aperture_state_t *ap = HW_APERTURE;
    int ex_m1 = ex - 1;
    int w0 = (int)((unsigned int)sx >> 5);
    int b0 = sx & 31;
    int b1 = ex_m1 & 31;
    if (__builtin_expect((unsigned int)(sx ^ ex_m1) < 32u, 1))
    {
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        if ((hw_solid_mask[w0] & mask) == mask)
            return -1;
        const hw_ap_word_t *aw = &hw_ap_words[w0];
        int yt_min = aw->yt_min;
        int yb_max = aw->yb_max;
        return (yt_min >= yb_max) ? -1 : ((yt_min << 16) | (uint16_t)yb_max);
    }
    int w1 = (int)((unsigned int)ex_m1 >> 5);
    if (b0 != 0 && (hw_solid_mask[w0] & hw_mask_left[b0]) == hw_mask_left[b0])
        w0++;
    if (b1 != 31 && (hw_solid_mask[w1] & hw_mask_right[b1]) == hw_mask_right[b1])
        w1--;
    if (w0 <= w1 && !((ap->dirty_words | hw_solid_summary) & (hw_span_mask_lut[w1 + 1] ^ hw_span_mask_lut[w0])))
        return (int)HW_VIEW_H;
    int yt_min = HW_VIEW_H;
    int yb_max = 0;
    for (int w = w0; w <= w1; w++)
    {
        if (hw_solid_mask[w] == 0xFFFFFFFFu)
            continue;
        const hw_ap_word_t *aw = &hw_ap_words[w];
        if (aw->yt_min < yt_min) yt_min = aw->yt_min;
        if (aw->yb_max > yb_max) yb_max = aw->yb_max;
    }
    return (yt_min >= yb_max) ? -1 : ((yt_min << 16) | (uint16_t)yb_max);
}

static HW_NOINLINE HW_HOT_O2 uint32_t HW_CommitSubSolidMask(void)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    const int vw = HW_VIEW_W;
    const int16_t vh = (int16_t)saturn_view_h;
    const int req_words = (HW_VIEW_W + 31) >> 5;
    uint32_t touched_words = 0;
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
            hw_ap_word_t *aw = &hw_ap_words[i];
            if (new_v == 0xFFFFFFFFu)
            {
                hw_solid_words++;
                hw_solid_summary |= wbit;
                aw->yt_min = vh;
                aw->yb_max = 0;
                aw->yt_max = vh;
                aw->yb_min = 0;
                ap->dirty_words |= wbit;
            }
            else if ((ap->dirty_words & wbit) &&
                     (((const aliased_uint32_t *)aw)[0] != ((const aliased_uint32_t *)aw)[1]))
            {
                touched_words |= wbit;
            }
        }
    }
    sub_solid_active = 0;
    hw_solid_count = (hw_solid_words >= req_words) ? vw : 1;
    return touched_words;
}

static HW_NOINLINE HW_HOT_O2 void HW_RescanApertureWords(uint32_t touched_words)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    const int vw = HW_VIEW_W;
    const int vh = saturn_view_h;
    const int req_words = (HW_VIEW_W + 31) >> 5;
    ap->dirty_words |= touched_words;
    const uint8_t * __restrict ytop = ap->ytop;
    const uint8_t * __restrict ybot = ap->ybot;

    for (int w = 0; w < req_words; w++)
    {
        uint32_t wbit = hw_bit_lut[w];
        if (!(touched_words & wbit))
            continue;
        hw_ap_word_t *aw = &hw_ap_words[w];
        uint32_t sm = hw_solid_mask[w];
        if (sm == 0xFFFFFFFFu)
        {
            aw->yt_min = (int16_t)vh;
            aw->yb_max = 0;
            aw->yt_max = (int16_t)vh;
            aw->yb_min = 0;
            continue;
        }
        int x_start = w << 5;
        int x_end = x_start + 32;
        if (x_end > vw) x_end = vw;

        int min_yt = vh, max_yt = 0;
        int min_yb = vh, max_yb = 0;
        if (sm == 0)
        {
            for (int x = x_start; x < x_end; x++)
            {
                int yt = ytop[x];
                int yb = ybot[x];
                if (yt < min_yt) min_yt = yt;
                if (yt > max_yt) max_yt = yt;
                if (yb < min_yb) min_yb = yb;
                if (yb > max_yb) max_yb = yb;
            }
        }
        else
        {
            uint32_t m = sm;
            for (int x = x_start; x < x_end; x++, m >>= 1)
            {
                if (m & 1u)
                    continue;
                int yt = ytop[x];
                int yb = ybot[x];
                if (yt < min_yt) min_yt = yt;
                if (yt > max_yt) max_yt = yt;
                if (yb < min_yb) min_yb = yb;
                if (yb > max_yb) max_yb = yb;
            }
        }
        if (min_yt > max_yb)
        {
            min_yt = max_yt = vh;
            min_yb = max_yb = 0;
        }
        aw->yt_min = (int16_t)min_yt;
        aw->yb_max = (int16_t)max_yb;
        aw->yt_max = (int16_t)max_yt;
        aw->yb_min = (int16_t)min_yb;
    }
}

static HW_NOINLINE HW_COLD_OS int HW_CommitPortalClosedWord(int w, uint32_t newly_closed)
{
    uint32_t sm = hw_solid_mask[w];
    uint32_t new_sm = sm | newly_closed;
    if (new_sm == sm)
        return 0;
    uint32_t wbit = hw_bit_lut[w];
    const int vw = HW_VIEW_W;
    const int req_words = (HW_VIEW_W + 31) >> 5;
    hw_solid_mask[w] = new_sm;
    hw_solid_any |= wbit;
    hw_slave_dirty_words |= wbit;
    if (new_sm == 0xFFFFFFFFu)
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        hw_ap_word_t *aw = &hw_ap_words[w];
        int16_t vh = (int16_t)saturn_view_h;
        hw_solid_words++;
        hw_solid_summary |= wbit;
        aw->yt_min = aw->yt_max = vh;
        aw->yb_min = aw->yb_max = 0;
        ap->dirty_words |= wbit;
        ap->aperture_active = 1;
        hw_solid_count = (hw_solid_words >= req_words) ? vw : 1;
        return 1;
    }
    hw_solid_count = (hw_solid_words >= req_words) ? vw : 1;
    return 0;
}

static HW_NOINLINE HW_HOT_O2 int HW_UpdatePortalWordPartial(
    int w_and_flags, const hw_portal_span_t *ps)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int w      = (int)((unsigned int)w_and_flags >> 8);
    int do_yt  = w_and_flags & 1;
    int do_yb  = (w_and_flags >> 1) & 1;
    int vw     = HW_VIEW_W;
    int x_w0   = w << 5;
    int x_w1   = x_w0 + 32;
    if (x_w1 > vw) x_w1 = vw;
    int sx     = ps->sx;
    int ex     = ps->ex;
    int x_s    = (sx > x_w0) ? sx : x_w0;
    int x_e    = (ex < x_w1) ? ex : x_w1;

    int new_yt = ps->ytop;
    int new_yb = ps->ybot;
    int yt_att = ps->yt_att;
    int yb_att = ps->yb_att;
    int touch_ret = 0;

    if (w_and_flags & 4)
    {
        hw_ap_word_t *aw = &hw_ap_words[w];
        int full_yt = do_yt && (yt_att <= aw->yt_min + 2) && (new_yt >= aw->yt_max);
        int full_yb = do_yb && (yb_att >= aw->yb_max - 2) && (new_yb <= aw->yb_min);
        if (full_yt || full_yb)
        {
            int next_yt_max = full_yt ? new_yt : aw->yt_max;
            int next_yb_min = full_yb ? new_yb : aw->yb_min;
            if (next_yt_max < next_yb_min)
            {
                uint32_t wbit = hw_bit_lut[w];
                int is_full_word = (x_s == x_w0 && x_e == x_w1);
                if (full_yt)
                {
                    uint8_t * __restrict ytop = ap->ytop;
                    if (is_full_word)
                    {
                        uint32_t yt4 = (uint32_t)(uint8_t)new_yt * 0x01010101u;
                        aliased_uint32_t *yt32 = (aliased_uint32_t *)&ytop[x_w0];
                        yt32[0] = yt4; yt32[1] = yt4; yt32[2] = yt4; yt32[3] = yt4;
                        yt32[4] = yt4; yt32[5] = yt4; yt32[6] = yt4; yt32[7] = yt4;
                        aw->yt_min = aw->yt_max = (int16_t)new_yt;
                    }
                    else
                    {
                        for (int x = x_s; x < x_e; x++)
                            ytop[x] = (uint8_t)new_yt;
                        if (aw->yt_min == aw->yt_max)
                            aw->yt_max = (int16_t)new_yt;
                        else
                            touch_ret = 1;
                    }
                    ap->dirty_words |= wbit;
                    ap->aperture_active = 1;
                    do_yt = 0;
                }
                if (full_yb)
                {
                    uint8_t * __restrict ybot = ap->ybot;
                    if (is_full_word)
                    {
                        uint32_t yb4 = (uint32_t)(uint8_t)new_yb * 0x01010101u;
                        aliased_uint32_t *yb32 = (aliased_uint32_t *)&ybot[x_w0];
                        yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
                        yb32[4] = yb4; yb32[5] = yb4; yb32[6] = yb4; yb32[7] = yb4;
                        aw->yb_min = aw->yb_max = (int16_t)new_yb;
                    }
                    else
                    {
                        for (int x = x_s; x < x_e; x++)
                            ybot[x] = (uint8_t)new_yb;
                        if (aw->yb_min == aw->yb_max)
                            aw->yb_min = (int16_t)new_yb;
                        else
                            touch_ret = 1;
                    }
                    ap->dirty_words |= wbit;
                    ap->aperture_active = 1;
                    do_yb = 0;
                }
                if (!do_yt && !do_yb)
                    return touch_ret;
            }
        }
    }

    uint8_t * __restrict ytop = ap->ytop;
    uint8_t * __restrict ybot = ap->ybot;
    uint32_t newly_closed = 0;
    int w_mod = touch_ret;

    int yt_req = (yt_att > 2) ? (yt_att - 2) : 0;
    unsigned int yt_span = (new_yt > yt_req) ? (unsigned int)(new_yt - yt_req) : 0u;
    int yb_base = new_yb + 1;
    int yb_req  = yb_att + 2;
    unsigned int yb_span = (yb_req >= yb_base) ? (unsigned int)(yb_req - new_yb) : 0u;

    if (!do_yb)
    {
        if (yt_span > 0u)
        {
            for (int x = x_s; x < x_e; x++)
            {
                int yt = ytop[x];
                if ((unsigned int)(yt - yt_req) < yt_span)
                {
                    ytop[x] = (uint8_t)new_yt;
                    w_mod = 1;
                    if (__builtin_expect(new_yt >= (int)ybot[x], 0))
                        newly_closed |= hw_bit_lut[x & 31];
                }
            }
        }
    }
    else if (!do_yt)
    {
        if (yb_span > 0u)
        {
            for (int x = x_s; x < x_e; x++)
            {
                int yb = ybot[x];
                if ((unsigned int)(yb - yb_base) < yb_span)
                {
                    ybot[x] = (uint8_t)new_yb;
                    w_mod = 1;
                    if (__builtin_expect((int)ytop[x] >= new_yb, 0))
                        newly_closed |= hw_bit_lut[x & 31];
                }
            }
        }
    }
    else
    {
        for (int x = x_s; x < x_e; x++)
        {
            int yt = ytop[x];
            int yb = ybot[x];
            int mod = 0;
            if ((unsigned int)(yt - yt_req) < yt_span)  { yt = new_yt; ytop[x] = (uint8_t)new_yt; mod = 1; }
            if ((unsigned int)(yb - yb_base) < yb_span) { yb = new_yb; ybot[x] = (uint8_t)new_yb; mod = 1; }
            if (mod)
            {
                w_mod = 1;
                if (__builtin_expect(yt >= yb, 0))
                    newly_closed |= hw_bit_lut[x & 31];
            }
        }
    }
    if (__builtin_expect(newly_closed != 0, 0))
    {
        if (HW_CommitPortalClosedWord(w, newly_closed))
            return 2;
    }
    return w_mod;
}

static HW_NOINLINE HW_HOT_O2 uint32_t HW_CommitPortalSpan(
    const hw_portal_span_t *ps, uint32_t touched_words)
{
    int sx = ps->sx;
    int ex = ps->ex;
    if (sx < 0) sx = 0;
    if (ex > HW_VIEW_W) ex = HW_VIEW_W;
    if (sx >= ex)
        return touched_words;

    int new_yt = ps->ytop;
    int new_yb = ps->ybot;
    int yt_att = ps->yt_att;
    int yb_att = ps->yb_att;
    int check_yt = (new_yt > 0 && yt_att <= new_yt + 1);
    int check_yb = (new_yb < saturn_view_h && yb_att >= new_yb - 1);
    if (!check_yt && !check_yb)
        return touched_words;

    int w0 = (int)((unsigned int)sx >> 5);
    int w1 = (int)((unsigned int)(ex - 1) >> 5);
    for (int w = w0; w <= w1; w++)
    {
        uint32_t sm = hw_solid_mask[w];
        if (sm == 0xFFFFFFFFu)
            continue;
        hw_ap_word_t *aw = &hw_ap_words[w];
        uint32_t wbit = hw_bit_lut[w];
        int is_touched = (touched_words & wbit) != 0;
        int do_yt = check_yt && (is_touched || ((ps->ytop > aw->yt_min) && (ps->yt_att <= aw->yt_max + 2)));
        int do_yb = check_yb && (is_touched || ((ps->ybot < aw->yb_max) && (ps->yb_att >= aw->yb_min - 2)));
        if (!do_yt && !do_yb)
            continue;

        int fast_ok = (sm == 0 && !is_touched) ? 4 : 0;
        int res = HW_UpdatePortalWordPartial((w << 8) | fast_ok | do_yt | (do_yb << 1), ps);
        if (res == 2)
        {
            touched_words &= ~wbit;
        }
        else if (res == 1)
        {
            touched_words |= wbit;
            HW_APERTURE->aperture_active = 1;
        }
    }
    return touched_words;
}

static HW_NOINLINE HW_HOT_O2 void HW_CommitSubsectorOcclusion(void)
{
#if !HW_DBG_OCCLUSION
    sub_solid_active = 0;
    HW_APERTURE->portal_span_count = 0;
    return;
#endif
    hw_aperture_state_t *ap = HW_APERTURE;
    uint32_t touched_words = sub_solid_active ? HW_CommitSubSolidMask() : 0;

    int p_count = ap->portal_span_count;
#if HW_DBG_APERTURE
    if (p_count > 0)
    {
        ap->portal_span_count = 0;
        const hw_portal_span_t *ps = ap->portal_spans;
        const hw_portal_span_t *ps_end = ps + p_count;
        for (; ps < ps_end; ps++)
            touched_words = HW_CommitPortalSpan(ps, touched_words);
    }
#else
    ap->portal_span_count = 0;
#endif

    if (touched_words != 0)
        HW_RescanApertureWords(touched_words);
}

static HW_NOINLINE HW_COLD_OS void HW_UpdateNodePvsVis(const byte *pvs_row, const void *ssec_key)
{
#if !HW_DBG_PVS
    pvs_row = NULL;
#endif
    hw_aperture_state_t *ap = HW_APERTURE;
    uint32_t *vis = ap->node_pvs_vis;
    node_t *local_nodes = nodes;
    int n_nodes = numnodes;
    int n_words = (n_nodes + 31) >> 5;
    for (int w = 0; w < n_words; w++)
        vis[w] = 0;

    for (int i = 0; i < n_nodes; i++)
    {
        int c0 = (int)local_nodes[i].children[0];
        int v0 = 0;
        if (!pvs_row)
        {
            v0 = ((int16_t)c0 != -1);
        }
        else if ((int16_t)c0 < 0)
        {
            if ((int16_t)c0 != -1)
            {
                int ssec = c0 & 0x7FFFu;
                v0 = (pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]) != 0;
            }
        }
        else if ((unsigned)c0 < (unsigned)n_nodes)
        {
            v0 = (vis[c0 >> 5] & hw_bit_lut[c0 & 31]) != 0;
        }
        local_nodes[i].encbbox[0] = v0 ? (uint16_t)c0 : 0xFFFFu;

        int c1 = (int)local_nodes[i].children[1];
        int v1 = 0;
        if (!pvs_row)
        {
            v1 = ((int16_t)c1 != -1);
        }
        else if ((int16_t)c1 < 0)
        {
            if ((int16_t)c1 != -1)
            {
                int ssec = c1 & 0x7FFFu;
                v1 = (pvs_row[ssec >> 3] & hw_bit_lut[ssec & 7]) != 0;
            }
        }
        else if ((unsigned)c1 < (unsigned)n_nodes)
        {
            v1 = (vis[c1 >> 5] & hw_bit_lut[c1 & 31]) != 0;
        }
        local_nodes[i].encbbox[1] = v1 ? (uint16_t)c1 : 0xFFFFu;

        if (v0 | v1)
            vis[i >> 5] |= hw_bit_lut[i & 31];
    }
    ap->last_pvs_ssec = ssec_key;
    ap->last_pvs_numnodes = n_nodes;
}
