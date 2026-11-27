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

/* Compact rewrite stage C2: HW_CheckSpanOcclusion / HW_MarkSubSolidSpan in
 * saturn_hw_asm.s (hw_asm_spanocc / hw_asm_marksub); every caller is
 * routed to them below. */
#ifndef HW_ASM_OCC
#define HW_ASM_OCC (HW_ASM && !HW_ORACLE)
#endif
#if HW_ASM_OCC
extern int  hw_asm_spanocc(int x0, int x1);
extern void hw_asm_marksub(int x0, int x1);
_Static_assert(HW_VIEW_W == 320, "hw_asm_spanocc: 320 columns");
const void *const hw_asm_occ_ctx[11] = {
    &hw_solid_count, hw_solid_mask, hw_mask_left, hw_mask_right,
    hw_span_mask_lut, &hw_solid_any, &hw_solid_summary,
    sub_solid_mask, &sub_solid_active, &sub_solid_min_w, &sub_solid_max_w
};
#if HW_ASM_CHECK
#define HW_CheckSpanOcclusion HW_CheckSpanOcclusion_C
#define HW_MarkSubSolidSpan   HW_MarkSubSolidSpan_C
#endif
#endif

#if !HW_ASM_OCC || HW_ASM_CHECK
/* 3-state span occlusion query: *   0 = 100% open (zero solid bits in [x0, x1]; sub-strips need no occlusion test)
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
    HW_ORACLE_SKIP();
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
#endif /* !HW_ASM_OCC || HW_ASM_CHECK */

#if HW_ASM_OCC
#undef HW_CheckSpanOcclusion
#undef HW_MarkSubSolidSpan
#if HW_ASM_CHECK
/* spanocc: [0] calls [1] mismatches [2] x0 [3] x1 [4] asm [5] C
 * marksub: [0] calls [1] mismatches [2] x0 [3] x1 */
static volatile uint32_t hw_asm_chk_spanocc[8];
static volatile uint32_t hw_asm_chk_marksub[8];
static HW_NOINLINE int HW_SpanOccCheck(int x0, int x1)
{
    int a = hw_asm_spanocc(x0, x1);
    int c = HW_CheckSpanOcclusion_C(x0, x1);
    hw_asm_chk_spanocc[0]++;
    if (a != c && hw_asm_chk_spanocc[1]++ == 0)
    {
        hw_asm_chk_spanocc[2] = (uint32_t)x0; hw_asm_chk_spanocc[3] = (uint32_t)x1;
        hw_asm_chk_spanocc[4] = (uint32_t)a;  hw_asm_chk_spanocc[5] = (uint32_t)c;
    }
    return a;
}
static HW_NOINLINE void HW_MarkSubCheck(int x0, int x1)
{
    uint32_t m0[HW_SOLID_WORDS], m1[HW_SOLID_WORDS];
    int act0 = sub_solid_active, mn0 = sub_solid_min_w, mx0 = sub_solid_max_w;
    memcpy(m0, sub_solid_mask, sizeof(m0));
    hw_asm_marksub(x0, x1);
    memcpy(m1, sub_solid_mask, sizeof(m1));
    int act1 = sub_solid_active, mn1 = sub_solid_min_w, mx1 = sub_solid_max_w;
    memcpy(sub_solid_mask, m0, sizeof(m0));
    sub_solid_active = act0; sub_solid_min_w = mn0; sub_solid_max_w = mx0;
    HW_MarkSubSolidSpan_C(x0, x1);
    hw_asm_chk_marksub[0]++;
    if ((memcmp(m1, sub_solid_mask, sizeof(m1)) != 0 || act1 != sub_solid_active ||
         mn1 != sub_solid_min_w || mx1 != sub_solid_max_w) && hw_asm_chk_marksub[1]++ == 0)
    {
        hw_asm_chk_marksub[2] = (uint32_t)x0; hw_asm_chk_marksub[3] = (uint32_t)x1;
    }
}
#define HW_CheckSpanOcclusion HW_SpanOccCheck
#define HW_MarkSubSolidSpan   HW_MarkSubCheck
#else
#define HW_CheckSpanOcclusion hw_asm_spanocc
#define HW_MarkSubSolidSpan   hw_asm_marksub
#endif
#endif
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
    int16_t dx;
    int16_t dy;
    int16_t back_sec_idx;
    uint8_t chain_prev;
    uint8_t pad;
} hw_portal_seg_t; /* 12 bytes */

#define HW_NODE_PVS_WORDS     32
#define HW_MAX_PORTAL_SPANS   32
/* 2026-11-21: 256 -> 512 sectors and a separate whole-level subsector limit.
 * Ultimate DOOM has maps with up to 323 sectors (E2M7) and 956 subsectors
 * (E4M9); at the old limits those maps skipped the per-level renderer init
 * and indexed past several per-sector / per-subsector tables.
 * HW_MAX_AP_SUBSECTORS stays the size of the fast static flat-info table
 * (and the WRAM-L scratch layout); bigger maps use a PU_LEVEL table. */
#define HW_MAX_AP_SECTORS     512
#ifndef HW_MAX_AP_SUBSECTORS
#define HW_MAX_AP_SUBSECTORS  608
#endif
#define HW_MAX_SUBSECTORS     1024
#define HW_MAX_PORTAL_SEGS    336

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

/* Per-frame 16-byte sector fast table (indexed by secnum << 4, 1 SH-2 L1 cache line):
 * Replaces non-power-of-two sector_t (36-byte multiply) + HW_DZQ(z, hw_viewz) +
 * separate sec_yt_attach / sec_yb_attach arrays with a single L1 cache line! */
typedef struct {
    int32_t  dz_ceil;    /* sec->ceilingheight - hw_viewz */
    int32_t  dz_floor;   /* sec->floorheight   - hw_viewz */
    int16_t  yt_attach;  /* init to vh */
    int16_t  yb_attach;  /* init to 0 */
    uint8_t  is_sky;     /* (int16_t)sec->ceilingpic == -1 */
    uint8_t  has_things; /* sec->thinglist != NULL */
    int16_t  light_delta;/* (lightlevel >> 4) now minus at level load: walls add
                          * it to their baked light so glow/flicker/strobe and
                          * light-change triggers reach them like flats/sprites */
} HW_ALIGN16 hw_sec_fast_t;

/* Sector light (>> 4) captured at level load (Saturn_HW_RenderInit), the
 * value bake_walls.py baked into every seg's baked_light. */
HW_W2_SHARED(uint8_t hw_sec_light0[HW_MAX_AP_SECTORS], );

HW_ALWAYS_INLINE int HW_SecLight16(int lightlevel)
{
    if ((unsigned)lightlevel > 255u)
        lightlevel = (lightlevel < 0) ? 0 : 255;
    return lightlevel >> 4;
}

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
} hw_aperture_state_t;

typedef struct {
    hw_sub_flat_info_t sub_flat_info[HW_MAX_AP_SUBSECTORS];
    hw_portal_seg_t    portal_segs[HW_MAX_PORTAL_SEGS];
    int32_t            sky_col_bam[HW_MAX_SCREEN_W];
} hw_wram_l_scratch_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(hw_wram_l_scratch_t) <= 32768,
               "hw_wram_l_scratch_t exceeds 32 KB Low Work RAM budget");
_Static_assert(sizeof(hw_sub_flat_info_t) == 16,
               "hw_sub_flat_info_t must be 16 bytes (1 SH-2 cache line)");
_Static_assert(sizeof(hw_sec_fast_t) == 16,
               "hw_sec_fast_t must be 16 bytes (1 SH-2 cache line)");
#elif defined(_MSC_VER)
static_assert(sizeof(hw_wram_l_scratch_t) <= 32768,
              "hw_wram_l_scratch_t exceeds 32 KB Low Work RAM budget");
static_assert(sizeof(hw_sub_flat_info_t) == 16,
              "hw_sub_flat_info_t must be 16 bytes (1 SH-2 cache line)");
static_assert(sizeof(hw_sec_fast_t) == 16,
              "hw_sec_fast_t must be 16 bytes (1 SH-2 cache line)");
#endif

static HW_ALIGN16 hw_aperture_state_t hw_aperture_state;
static HW_ALIGN16 hw_ap_word_t hw_ap_words[HW_SOLID_WORDS];
static HW_ALIGN16 hw_sec_fast_t hw_sec_fast[HW_MAX_AP_SECTORS];
/* Per-sector far plane for subsector culling, map units << 12 (2026-11-23,
 * per frame in Saturn_HW_RenderPlayerView): the adaptive-detail far plane,
 * or nearer for dark sectors whose walls and flats are bank 7 (COLORMAP 31,
 * black) beyond a distance - see HW_DARK_CUT. */
static int32_t hw_sec_far[HW_MAX_AP_SECTORS];
/* Height-aware culling (2026-11-20): per sector the highest point anything
 * in it can be drawn, in world units (HW_SEC_ZTOP_CEIL = use the ceiling:
 * one-sided/upper/masked walls, moving sectors). Built per level in
 * saturn_hw_sky.c; hw_sec_dz_top = that minus viewz per frame (dz_ceil when
 * the sector holds things). Used for sky sectors, whose ceiling is not
 * drawn. Jo heap (BSS is at its 0x060C0000 ceiling). */
#define HW_SEC_ZTOP_CEIL 0x7FFF
/* (HW_WALK2: built per level / per frame by the master; the slave copy
 * reads the master's) */
HW_W2_SHARED(int16_t *hw_sec_ztop, );
HW_W2_SHARED(int32_t *hw_sec_dz_top, );            /* published (non-NULL) only while the lists are valid */
HW_W2_SHARED(int32_t *hw_sec_dz_mem, );
HW_W2_SHARED(const uint16_t *hw_sec_nb_first, );   /* PU_LEVEL: neighbour list offsets, [s]..[s+1] */
HW_W2_SHARED(const uint16_t *hw_sec_nb, );         /* PU_LEVEL: two-sided neighbour sectors */
extern uint32_t hw_wram_h_sub_flat_info[HW_MAX_AP_SUBSECTORS * 4];
/* Per-subsector flat info: the static High-WRAM table, or a PU_LEVEL table
 * for maps with more than HW_MAX_AP_SUBSECTORS subsectors (then
 * hw_sub_flat_big = 1 and flat jobs go through the C submit path, since
 * hw_asm_flatsub indexes the static table). Set in Saturn_HW_RenderPlayerView
 * when the level's baked flats change. */
HW_W2_SHARED(hw_sub_flat_info_t *hw_sub_flat_info_p, = (hw_sub_flat_info_t *)hw_wram_h_sub_flat_info);
HW_W2_SHARED(int hw_sub_flat_big, = 0);
extern uint32_t hw_wram_h_portal_segs[HW_MAX_PORTAL_SEGS * 3];
#define HW_WRAM_L_SCRATCH ((hw_wram_l_scratch_t *)SATURN_WRAM_L_PILLARS_ADDR)
#define HW_APERTURE       (&hw_aperture_state)
#define HW_SUB_FLAT_INFO  hw_sub_flat_info_p

/* Point HW_SUB_FLAT_INFO at a table that fits this level (once per level
 * load, keyed on saturn_level_gen; the PU_LEVEL table dies with the level). */
static void HW_SubFlatInfoEnsure(void)
{
    extern unsigned int saturn_level_gen;
    static unsigned int gen = 0xFFFFFFFFu;
    if (gen == saturn_level_gen)
        return;
    gen = saturn_level_gen;
    if (numsubsectors > HW_MAX_AP_SUBSECTORS)
    {
        uintptr_t p = (uintptr_t)Z_Malloc(numsubsectors * (int)sizeof(hw_sub_flat_info_t) + 15, PU_LEVEL);
        hw_sub_flat_info_p = (hw_sub_flat_info_t *)((p + 15u) & ~(uintptr_t)15u);
        hw_sub_flat_big = 1;
    }
    else
    {
        hw_sub_flat_info_p = (hw_sub_flat_info_t *)hw_wram_h_sub_flat_info;
        hw_sub_flat_big = 0;
    }
}
#define HW_PORTAL_SEGS    ((hw_portal_seg_t *)hw_wram_h_portal_segs)
#define HW_SKY_COL_BAM    (HW_WRAM_L_SCRATCH->sky_col_bam)

/* HW_AP_HALF (2026-11-17, the 32X idea): the per-column window limits
 * ytop[]/ybot[] are kept per PAIR of screen columns (index = x >> 1, 16 per
 * 32-column word). Conservative both ways: a portal span narrows only pairs
 * entirely inside it, and lookups skip a pair only when both of its columns
 * are solid, so nothing visible is culled; a few hidden pieces may be drawn.
 * The solid-column bitmask and the per-word summaries stay full resolution. */
#ifndef HW_AP_HALF
#define HW_AP_HALF 0
#endif
#if HW_AP_HALF
#define HW_AP_PAIR_SOLID(sm, h) ((((sm) >> (((h) & 15) << 1)) & 3u) == 3u)
#endif
/* Compact rewrite stage C1: the whole commit below in saturn_hw_asm.s
 * (hw_asm_commit; table layout documented there). */
#ifndef HW_ASM_COMMIT
#define HW_ASM_COMMIT (HW_ASM && HW_ASM_RESCAN && HW_DBG_OCCLUSION && HW_DBG_APERTURE && !HW_AP_HALF)
#endif

/* HW_AP_LINEAR (2026-11-26 prototype, C paths only - the harness): a portal
 * span narrows each column to the wall edge's height AT THAT COLUMN (a
 * flat wall's top/bottom edge projects to a straight screen line), not to
 * the most conservative end value for the whole span. The span struct keeps
 * the value at sx; hw_ap_lin_yt/yb hold the value at ex - 1. The harness
 * sweep showed ~30-50% of the walked subsectors in dense E4 views ending
 * 100% overdrawn, covered by nearer steps/ledges whose spans were recorded
 * with the near-end height only. */
#ifndef HW_AP_LINEAR
#ifdef SATURN_WIN32
#define HW_AP_LINEAR 0          /* harness prototype measured NO gain (2026-11-26): off */
#else
#define HW_AP_LINEAR 0          /* Saturn: hw_asm_portal / hw_asm_commit own the span layout */
#endif
#endif
#if HW_AP_LINEAR
static uint8_t hw_ap_lin_yt[HW_MAX_PORTAL_SPANS], hw_ap_lin_yb[HW_MAX_PORTAL_SPANS];
static int hw_ap_lin_yt_e = 0, hw_ap_lin_yb_e = 0;   /* the next span's ex-1 values (set by the caller) */
static int hw_ap_linear_on = -1;                     /* harness env HW_AP_LINEAR=0 turns it off */
static inline int HW_ApLinearOn(void)
{
#ifdef SATURN_WIN32
    if (hw_ap_linear_on < 0) { const char *e = getenv("HW_AP_LINEAR"); hw_ap_linear_on = e ? atoi(e) : 1; }
    return hw_ap_linear_on;
#else
    return 1;
#endif
}
#endif

/* unused when hw_asm_portal (saturn_hw_walls.c) records spans itself */
static HW_NOINLINE HW_HOT_O2 __attribute__((unused)) void HW_RecordPortalSpan(int sx_ex, int ytop, int ybot, int yt_yb_att)
{
#if HW_AP_LINEAR
    int lin = HW_ApLinearOn();
    int ytop_e = lin ? hw_ap_lin_yt_e : ytop, ybot_e = lin ? hw_ap_lin_yb_e : ybot;
    hw_ap_lin_yt_e = 0;
    hw_ap_lin_yb_e = HW_VIEW_H;
    if (ytop_e < 0) ytop_e = 0;
    else if (ytop_e > HW_VIEW_H) ytop_e = HW_VIEW_H;
    if (ybot_e < 0) ybot_e = 0;
    else if (ybot_e > HW_VIEW_H) ybot_e = HW_VIEW_H;
#endif
#if HW_AP_1D
    (void)sx_ex; (void)ytop; (void)ybot; (void)yt_yb_att;
    return;
#endif
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
#if HW_AP_LINEAR
    /* per column the edge can still pass the attach test at one end */
#ifdef SATURN_WIN32
    {
        extern int hw_trace_sub_on;
        if (hw_trace_sub_on)
            printf("[SPAN] rec x %d..%d yt %d/%d yb %d/%d att yt %d yb %d\n", sx, ex, ytop, ytop_e, ybot, ybot_e, yt_att, yb_att);
    }
#endif
    if (yt_att > (ytop > ytop_e ? ytop : ytop_e) + 1) ytop = ytop_e = 0;
    if (yb_att < (ybot < ybot_e ? ybot : ybot_e) - 1) ybot = ybot_e = HW_VIEW_H;
    if (sx >= ex || (ytop <= 0 && ytop_e <= 0 && ybot >= HW_VIEW_H && ybot_e >= HW_VIEW_H))
        return;
#else
    if (yt_att > ytop + 1) ytop = 0;
    if (yb_att < ybot - 1) ybot = HW_VIEW_H;
    if (sx >= ex || (ytop <= 0 && ybot >= HW_VIEW_H))
        return;
#endif
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
#if HW_AP_LINEAR
            if (ytop_e > hw_ap_lin_yt[idx - 1]) hw_ap_lin_yt[idx - 1] = (uint8_t)ytop_e;
            if (ybot_e < hw_ap_lin_yb[idx - 1]) hw_ap_lin_yb[idx - 1] = (uint8_t)ybot_e;
#endif
            return;
        }
    }
    if (idx < HW_MAX_PORTAL_SPANS)
    {
        hw_portal_span_t *cur = &spans[idx];
#if HW_AP_LINEAR
        hw_ap_lin_yt[idx] = (uint8_t)ytop_e;
        hw_ap_lin_yb[idx] = (uint8_t)ybot_e;
#endif
        cur->sx     = (int16_t)sx;
        cur->ex     = (int16_t)ex;
        cur->ytop   = (uint8_t)ytop;
        cur->ybot   = (uint8_t)ybot;
        cur->yt_att = (uint8_t)yt_att;
        cur->yb_att = (uint8_t)yb_att;
        ap->portal_span_count = (uint8_t)(idx + 1);
    }
}

#if HW_ASM_SPANAP
/* saturn_hw_asm.s; layout documented there. */
extern int hw_asm_spanap(int sx, int ex, const void *const *ctx);
static const void *const hw_asm_spanap_ctx[8] = {
    hw_solid_mask, hw_ap_words, hw_aperture_state.ytop,
    &hw_aperture_state.dirty_words, &hw_solid_summary,
    hw_mask_left, hw_mask_right, &saturn_view_h
};
#if HW_ASM_CHECK
/* Check build: run the C reference too. [0] calls, [1] mismatches,
 * [2..5] first mismatch sx, ex, C result, asm result. */
static int HW_GetSpanAperture_C(int sx, int ex);
static volatile uint32_t hw_asm_chk_spanap[8];
#endif
HW_ALWAYS_INLINE int HW_GetSpanAperture(int sx, int ex)
{
    int r = hw_asm_spanap(sx, ex, hw_asm_spanap_ctx);
#if HW_ASM_CHECK
    int c = HW_GetSpanAperture_C(sx, ex);
    hw_asm_chk_spanap[0]++;
    if (r != c && hw_asm_chk_spanap[1]++ == 0)
    {
        hw_asm_chk_spanap[2] = (uint32_t)sx;
        hw_asm_chk_spanap[3] = (uint32_t)ex;
        hw_asm_chk_spanap[4] = (uint32_t)c;
        hw_asm_chk_spanap[5] = (uint32_t)r;
    }
#endif
    return r;
}
#define HW_SPANAP_C_NAME HW_GetSpanAperture_C
#else
#define HW_SPANAP_C_NAME HW_GetSpanAperture
#endif
#if !HW_ASM_SPANAP || HW_ASM_CHECK
static HW_NOINLINE HW_HOT_O2 int HW_SPANAP_C_NAME(int sx, int ex)
{
    const hw_aperture_state_t *ap = HW_APERTURE;
    int ex_m1 = ex - 1;
    int w0 = (int)((unsigned int)sx >> 5);
    int b0 = sx & 31;
    int b1 = ex_m1 & 31;
    if (__builtin_expect((unsigned int)(sx ^ ex_m1) < 32u, 1))
    {
        uint32_t sm = hw_solid_mask[w0];
        uint32_t mask = hw_mask_left[b0] & hw_mask_right[b1];
        if ((sm & mask) == mask)
            return -1;
        const hw_ap_word_t *aw = &hw_ap_words[w0];
        int yt_min = aw->yt_min;
        int yb_max = aw->yb_max;
        if (__builtin_expect((aw->yt_max - yt_min) > 4 || (yb_max - aw->yb_min) > 4, 0))
        {
            const uint8_t *ytop = ap->ytop;
            const uint8_t *ybot = ap->ybot;
            yt_min = HW_VIEW_H;
            yb_max = 0;
#if HW_AP_HALF
            for (int h = sx >> 1; h <= (ex_m1 >> 1); h++)
            {
                if (HW_AP_PAIR_SOLID(sm, h))
                    continue;
                int yt = ytop[h];
                int yb = ybot[h];
                if (yt < yt_min) yt_min = yt;
                if (yb > yb_max) yb_max = yb;
            }
#else
            uint32_t m = sm >> b0;
            for (int x = sx; x <= ex_m1; x++, m >>= 1)
            {
                if (m & 1u)
                    continue;
                int yt = ytop[x];
                int yb = ybot[x];
                if (yt < yt_min) yt_min = yt;
                if (yb > yb_max) yb_max = yb;
            }
#endif
        }
        return (yt_min >= yb_max) ? -1 : ((yt_min << 16) | (uint16_t)yb_max);
    }
    int w1 = (int)((unsigned int)ex_m1 >> 5);
    if (b0 != 0 && (hw_solid_mask[w0] & hw_mask_left[b0]) == hw_mask_left[b0])
    {
        w0++;
        b0 = 0;
        sx = w0 << 5;
    }
    if (b1 != 31 && (hw_solid_mask[w1] & hw_mask_right[b1]) == hw_mask_right[b1])
    {
        w1--;
        b1 = 31;
        ex_m1 = (w1 << 5) + 31;
    }
    if (w0 > w1)
        return -1;
    if (!((ap->dirty_words | hw_solid_summary) & (hw_span_mask_lut[w1 + 1] ^ hw_span_mask_lut[w0])))
        return (int)HW_VIEW_H;
    int yt_min = HW_VIEW_H;
    int yb_max = 0;
    int wf0 = (b0 == 0) ? w0 : (w0 + 1);
    int wf1 = (b1 == 31) ? w1 : (w1 - 1);
    for (int w = wf0; w <= wf1; w++)
    {
        if (hw_solid_mask[w] == 0xFFFFFFFFu)
            continue;
        const hw_ap_word_t *aw = &hw_ap_words[w];
        if (aw->yt_min < yt_min) yt_min = aw->yt_min;
        if (aw->yb_max > yb_max) yb_max = aw->yb_max;
    }
    if (b0 != 0 && hw_solid_mask[w0] != 0xFFFFFFFFu)
    {
        const hw_ap_word_t *aw0 = &hw_ap_words[w0];
        if (aw0->yt_min < yt_min || aw0->yb_max > yb_max)
        {
            if ((aw0->yt_max - aw0->yt_min) <= 4 && (aw0->yb_max - aw0->yb_min) <= 4)
            {
                if (aw0->yt_min < yt_min) yt_min = aw0->yt_min;
                if (aw0->yb_max > yb_max) yb_max = aw0->yb_max;
            }
            else
            {
                const uint8_t *ytop = ap->ytop;
                const uint8_t *ybot = ap->ybot;
                int x_end = (w0 << 5) + 31;
#if HW_AP_HALF
                uint32_t sm0 = hw_solid_mask[w0];
                for (int h = sx >> 1; h <= (x_end >> 1); h++)
                {
                    if (HW_AP_PAIR_SOLID(sm0, h))
                        continue;
                    int yt = ytop[h];
                    int yb = ybot[h];
                    if (yt < yt_min) yt_min = yt;
                    if (yb > yb_max) yb_max = yb;
                }
#else
                uint32_t m = hw_solid_mask[w0] >> b0;
                for (int x = sx; x <= x_end; x++, m >>= 1)
                {
                    if (m & 1u)
                        continue;
                    int yt = ytop[x];
                    int yb = ybot[x];
                    if (yt < yt_min) yt_min = yt;
                    if (yb > yb_max) yb_max = yb;
                }
#endif
            }
        }
    }
    if (b1 != 31 && hw_solid_mask[w1] != 0xFFFFFFFFu)
    {
        const hw_ap_word_t *aw1 = &hw_ap_words[w1];
        if (aw1->yt_min < yt_min || aw1->yb_max > yb_max)
        {
            if ((aw1->yt_max - aw1->yt_min) <= 4 && (aw1->yb_max - aw1->yb_min) <= 4)
            {
                if (aw1->yt_min < yt_min) yt_min = aw1->yt_min;
                if (aw1->yb_max > yb_max) yb_max = aw1->yb_max;
            }
            else
            {
                const uint8_t *ytop = ap->ytop;
                const uint8_t *ybot = ap->ybot;
                int x_start = w1 << 5;
#if HW_AP_HALF
                uint32_t sm1 = hw_solid_mask[w1];
                for (int h = x_start >> 1; h <= (ex_m1 >> 1); h++)
                {
                    if (HW_AP_PAIR_SOLID(sm1, h))
                        continue;
                    int yt = ytop[h];
                    int yb = ybot[h];
                    if (yt < yt_min) yt_min = yt;
                    if (yb > yb_max) yb_max = yb;
                }
#else
                uint32_t m = hw_solid_mask[w1];
                for (int x = x_start; x <= ex_m1; x++, m >>= 1)
                {
                    if (m & 1u)
                        continue;
                    int yt = ytop[x];
                    int yb = ybot[x];
                    if (yt < yt_min) yt_min = yt;
                    if (yb > yb_max) yb_max = yb;
                }
#endif
            }
        }
    }
    return (yt_min >= yb_max) ? -1 : ((yt_min << 16) | (uint16_t)yb_max);
}
#endif /* !HW_ASM_SPANAP || HW_ASM_CHECK */

#if !HW_ASM_COMMIT || HW_ASM_CHECK
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
#endif

#if HW_ASM_RESCAN
/* saturn_hw_asm.s; bit-identical to the C below (HW_AP_HALF=0 only). */
extern void hw_asm_rescan(uint32_t touched, const uint32_t *solid_mask,
                          hw_ap_word_t *ap_words, const uint8_t *ytop);
_Static_assert(offsetof(hw_aperture_state_t, ybot) - offsetof(hw_aperture_state_t, ytop) == 352,
               "hw_asm_rescan assumes ybot = ytop + 352");
_Static_assert(HW_VIEW_W == 320 && HW_AP_HALF == 0, "hw_asm_rescan assumes 10x32 full-res columns");
#if HW_ASM_CHECK
/* Check build: C reference into a scratch copy. [0] calls, [1] mismatching
 * calls, [2] first touched mask, [3] first bad word, [4..5] C / asm words. */
static void HW_RescanApertureWords_C(uint32_t touched_words, hw_ap_word_t *out);
static volatile uint32_t hw_asm_chk_rescan[8];
#endif
HW_ALWAYS_INLINE void HW_RescanApertureWords(uint32_t touched_words)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    ap->dirty_words |= touched_words;
#if HW_ASM_CHECK
    hw_ap_word_t ref[HW_SOLID_WORDS];
    memcpy(ref, hw_ap_words, sizeof(ref));
    HW_RescanApertureWords_C(touched_words, ref);
#endif
    hw_asm_rescan(touched_words, hw_solid_mask, hw_ap_words, ap->ytop);
#if HW_ASM_CHECK
    hw_asm_chk_rescan[0]++;
    for (int w = 0; w < HW_SOLID_WORDS; w++)
    {
        if (memcmp(&ref[w], &hw_ap_words[w], sizeof(hw_ap_word_t)) != 0)
        {
            if (hw_asm_chk_rescan[1]++ == 0)
            {
                const uint32_t *c = (const uint32_t *)&ref[w];
                const uint32_t *a = (const uint32_t *)&hw_ap_words[w];
                hw_asm_chk_rescan[2] = touched_words;
                hw_asm_chk_rescan[3] = (uint32_t)w;
                hw_asm_chk_rescan[4] = c[0];
                hw_asm_chk_rescan[5] = c[1];
                hw_asm_chk_rescan[6] = a[0];
                hw_asm_chk_rescan[7] = a[1];
            }
            break;
        }
    }
#endif
}
#define HW_RESCAN_C_SIG static void HW_RescanApertureWords_C(uint32_t touched_words, hw_ap_word_t *out_words)
#else
#define HW_RESCAN_C_SIG static HW_NOINLINE HW_HOT_O2 void HW_RescanApertureWords(uint32_t touched_words)
#endif
#if !HW_ASM_RESCAN || HW_ASM_CHECK
HW_RESCAN_C_SIG
{
    hw_aperture_state_t *ap = HW_APERTURE;
    const int vw = HW_VIEW_W;
    const int vh = saturn_view_h;
    const int req_words = (HW_VIEW_W + 31) >> 5;
#if HW_ASM_RESCAN
    hw_ap_word_t *aw_base = out_words;   /* check build: scratch copy */
#else
    hw_ap_word_t *aw_base = hw_ap_words;
    ap->dirty_words |= touched_words;
#endif
    const uint8_t * __restrict ytop = ap->ytop;
    const uint8_t * __restrict ybot = ap->ybot;

    for (int w = 0; w < req_words; w++)
    {
        uint32_t wbit = hw_bit_lut[w];
        if (!(touched_words & wbit))
            continue;
        hw_ap_word_t *aw = &aw_base[w];
        uint32_t sm = hw_solid_mask[w];
        if (sm == 0xFFFFFFFFu)
        {
            aw->yt_min = (int16_t)vh;
            aw->yb_max = 0;
            aw->yt_max = (int16_t)vh;
            aw->yb_min = 0;
            continue;
        }
        int min_yt = vh, max_yt = 0;
        int min_yb = vh, max_yb = 0;
#if HW_AP_HALF
        int h_start = w << 4;
        int h_end = h_start + 16;
        if (h_end > ((vw + 1) >> 1)) h_end = (vw + 1) >> 1;
        for (int h = h_start; h < h_end; h++)
        {
            if (sm != 0 && HW_AP_PAIR_SOLID(sm, h))
                continue;
            int yt = ytop[h];
            int yb = ybot[h];
            if (yt < min_yt) min_yt = yt;
            if (yt > max_yt) max_yt = yt;
            if (yb < min_yb) min_yb = yb;
            if (yb > max_yb) max_yb = yb;
        }
#else
        int x_start = w << 5;
        int x_end = x_start + 32;
        if (x_end > vw) x_end = vw;
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
#endif
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
#endif /* !HW_ASM_RESCAN || HW_ASM_CHECK */

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

#if !HW_ASM_COMMIT || HW_ASM_CHECK
static HW_NOINLINE HW_HOT_O2 int HW_UpdatePortalWordCols(
    int w_and_flags, const hw_portal_span_t *ps, int touch_ret)
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

    uint8_t * __restrict ytop = ap->ytop;
    uint8_t * __restrict ybot = ap->ybot;
    uint32_t newly_closed = 0;
    int w_mod = touch_ret;

    int yt_req = (yt_att > 2) ? (yt_att - 2) : 0;
    unsigned int yt_span = (new_yt > yt_req) ? (unsigned int)(new_yt - yt_req) : 0u;
    int yb_base = new_yb + 1;
    int yb_req  = yb_att + 2;
    unsigned int yb_span = (yb_req >= yb_base) ? (unsigned int)(yb_req - new_yb) : 0u;

#if HW_AP_LINEAR
    {
        /* per-column edge: ytop rounds down, ybot up (never over-narrow) */
        int idx  = (int)(ps - ap->portal_spans);
        int ytS  = ps->ytop, ytE = hw_ap_lin_yt[idx];
        int ybS  = ps->ybot, ybE = hw_ap_lin_yb[idx];
        int den  = ps->ex - 1 - ps->sx;
#ifdef SATURN_WIN32
        extern int hw_trace_sub_on;
        if (hw_trace_sub_on)
            printf("[SPANC] word %d x %d..%d do_yt %d do_yb %d yt_req %d yb_req %d | col %d: ytop %d ybot %d edge yt %d yb %d\n",
                   w, x_s, x_e, do_yt, do_yb, yt_req, yb_req, x_s, ytop[x_s], ybot[x_s], ytS, ybS);
#endif
        for (int x = x_s; x < x_e; x++)
        {
            int nyt = ytS, nyb = ybS;
            if (den > 0)
            {
                int num = x - ps->sx;
                int dt = (ytE - ytS) * num, db = (ybE - ybS) * num;
                nyt = ytS + ((dt >= 0) ? dt / den : -((-dt + den - 1) / den));     /* floor */
                nyb = ybS + ((db >= 0) ? (db + den - 1) / den : -((-db) / den));   /* ceil */
            }
            int yt = ytop[x], yb = ybot[x], mod = 0;
            if (do_yt && nyt > yt_req && yt >= yt_req && yt < nyt)
            {
                yt = nyt; ytop[x] = (uint8_t)nyt; mod = 1;
            }
            if (do_yb && yb_req >= nyb + 1 && yb >= nyb + 1 && yb <= yb_req)
            {
                yb = nyb; ybot[x] = (uint8_t)nyb; mod = 1;
            }
            if (mod)
            {
                w_mod = 1;
                if (__builtin_expect(yt >= yb, 0))
                    newly_closed |= hw_bit_lut[x & 31];
            }
        }
        (void)yt_span; (void)yb_span; (void)yb_base; (void)new_yt; (void)new_yb;
    }
#elif HW_AP_HALF
    /* pairs fully inside [x_s, x_e) only; a closed pair closes both columns */
    {
        int h_s = (x_s + 1) >> 1;
        int h_e = x_e >> 1;
        for (int h = h_s; h < h_e; h++)
        {
            int yt = ytop[h];
            int yb = ybot[h];
            int mod = 0;
            if (do_yt && (unsigned int)(yt - yt_req) < yt_span)  { yt = new_yt; ytop[h] = (uint8_t)new_yt; mod = 1; }
            if (do_yb && (unsigned int)(yb - yb_base) < yb_span) { yb = new_yb; ybot[h] = (uint8_t)new_yb; mod = 1; }
            if (mod)
            {
                w_mod = 1;
                if (__builtin_expect(yt >= yb, 0))
                    newly_closed |= 3u << ((h & 15) << 1);
            }
        }
    }
#else
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
#endif
    if (__builtin_expect(newly_closed != 0, 0))
    {
        if (HW_CommitPortalClosedWord(w, newly_closed))
            return 2;
        return 1;
    }
#if HW_AP_LINEAR
    if (0)      /* per-column heights: let the caller rescan the word summary */
#else
    if (w_mod && !touch_ret && !(w_and_flags & 8))
#endif
    {
        hw_ap_word_t *aw = &hw_ap_words[w];
        int is_partial_clean = (w_and_flags & 4) && (x_s > x_w0 || x_e < x_w1);
        int ok_yt = !do_yt || (yt_req > aw->yt_min) || (is_partial_clean && aw->yt_min == aw->yt_max);
        int ok_yb = !do_yb || (yb_req < aw->yb_max) || (is_partial_clean && aw->yb_min == aw->yb_max);
        if (ok_yt && ok_yb)
        {
            if (do_yt && new_yt > aw->yt_max) aw->yt_max = (int16_t)new_yt;
            if (do_yb && new_yb < aw->yb_min) aw->yb_min = (int16_t)new_yb;
            ap->dirty_words |= hw_bit_lut[w];
            ap->aperture_active = 1;
            return 0;
        }
    }
    return w_mod;
}

static HW_NOINLINE HW_HOT_O2 int HW_UpdatePortalWordPartial(
    int w_and_flags, const hw_portal_span_t *ps)
{
    int touch_ret = 0;
#if HW_AP_LINEAR
    if (0)      /* the whole-word fast path assumes one height per span */
#else
    if (w_and_flags & 4)
#endif
    {
        int w      = (int)((unsigned int)w_and_flags >> 8);
        int do_yt  = w_and_flags & 1;
        int do_yb  = (w_and_flags >> 1) & 1;
        int new_yt = ps->ytop;
        int new_yb = ps->ybot;
        int yt_att = ps->yt_att;
        int yb_att = ps->yb_att;
        hw_ap_word_t *aw = &hw_ap_words[w];
        int full_yt = do_yt && (yt_att <= aw->yt_min + 2) && (new_yt >= aw->yt_max);
        int full_yb = do_yb && (yb_att >= aw->yb_max - 2) && (new_yb <= aw->yb_min);
        if (full_yt || full_yb)
        {
            int next_yt_max = full_yt ? new_yt : aw->yt_max;
            int next_yb_min = full_yb ? new_yb : aw->yb_min;
            if (next_yt_max < next_yb_min)
            {
                hw_aperture_state_t *ap = HW_APERTURE;
                int vw   = HW_VIEW_W;
                int x_w0 = w << 5;
                int x_w1 = x_w0 + 32;
                if (x_w1 > vw) x_w1 = vw;
                int sx   = ps->sx;
                int ex   = ps->ex;
                int x_s  = (sx > x_w0) ? sx : x_w0;
                int x_e  = (ex < x_w1) ? ex : x_w1;
                uint32_t wbit = hw_bit_lut[w];
                int is_full_word = (x_s == x_w0 && x_e == x_w1);
                if (full_yt)
                {
                    uint8_t * __restrict ytop = ap->ytop;
                    if (is_full_word)
                    {
                        uint32_t yt4 = (uint32_t)(uint8_t)new_yt * 0x01010101u;
#if HW_AP_HALF
                        aliased_uint32_t *yt32 = (aliased_uint32_t *)&ytop[x_w0 >> 1];
                        yt32[0] = yt4; yt32[1] = yt4; yt32[2] = yt4; yt32[3] = yt4;
#else
                        aliased_uint32_t *yt32 = (aliased_uint32_t *)&ytop[x_w0];
                        yt32[0] = yt4; yt32[1] = yt4; yt32[2] = yt4; yt32[3] = yt4;
                        yt32[4] = yt4; yt32[5] = yt4; yt32[6] = yt4; yt32[7] = yt4;
#endif
                        aw->yt_min = aw->yt_max = (int16_t)new_yt;
                    }
                    else
                    {
#if HW_AP_HALF
                        for (int h = (x_s + 1) >> 1; h < (x_e >> 1); h++)
                            ytop[h] = (uint8_t)new_yt;
#else
                        for (int x = x_s; x < x_e; x++)
                            ytop[x] = (uint8_t)new_yt;
#endif
                        if (aw->yt_min == aw->yt_max)
                            aw->yt_max = (int16_t)new_yt;
                        else
                            touch_ret = 1;
                    }
                    ap->dirty_words |= wbit;
                    ap->aperture_active = 1;
                    w_and_flags &= ~1;
                }
                if (full_yb)
                {
                    uint8_t * __restrict ybot = ap->ybot;
                    if (is_full_word)
                    {
                        uint32_t yb4 = (uint32_t)(uint8_t)new_yb * 0x01010101u;
#if HW_AP_HALF
                        aliased_uint32_t *yb32 = (aliased_uint32_t *)&ybot[x_w0 >> 1];
                        yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
#else
                        aliased_uint32_t *yb32 = (aliased_uint32_t *)&ybot[x_w0];
                        yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
                        yb32[4] = yb4; yb32[5] = yb4; yb32[6] = yb4; yb32[7] = yb4;
#endif
                        aw->yb_min = aw->yb_max = (int16_t)new_yb;
                    }
                    else
                    {
#if HW_AP_HALF
                        for (int h = (x_s + 1) >> 1; h < (x_e >> 1); h++)
                            ybot[h] = (uint8_t)new_yb;
#else
                        for (int x = x_s; x < x_e; x++)
                            ybot[x] = (uint8_t)new_yb;
#endif
                        if (aw->yb_min == aw->yb_max)
                            aw->yb_min = (int16_t)new_yb;
                        else
                            touch_ret = 1;
                    }
                    ap->dirty_words |= wbit;
                    ap->aperture_active = 1;
                    w_and_flags &= ~2;
                }
                if (!(w_and_flags & 3))
                    return touch_ret;
            }
        }
    }
    return HW_UpdatePortalWordCols(w_and_flags, ps, touch_ret);
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
#if HW_AP_LINEAR
    {
        /* filters only (the columns test exactly): the more narrowing end */
        int idx = (int)(ps - HW_APERTURE->portal_spans);
        if (hw_ap_lin_yt[idx] > new_yt) new_yt = hw_ap_lin_yt[idx];
        if (hw_ap_lin_yb[idx] < new_yb) new_yb = hw_ap_lin_yb[idx];
    }
#endif
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
        int do_yt = check_yt && (is_touched || ((new_yt > aw->yt_min) && (ps->yt_att <= aw->yt_max + 2)));
        int do_yb = check_yb && (is_touched || ((new_yb < aw->yb_max) && (ps->yb_att >= aw->yb_min - 2)));
        if (!do_yt && !do_yb)
            continue;

        int fast_ok = (sm == 0 && !is_touched) ? 4 : (is_touched ? 8 : 0);
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
#endif

#if HW_ASM && !HW_ASM_COMMIT
const void *const hw_asm_commit_ctx[15] = { 0 };   /* asm stays linked, unused (e.g. -DHW_DBG_OCCLUSION=0) */
#endif
#if HW_ASM_COMMIT
extern void hw_asm_commit(void);
_Static_assert(offsetof(hw_aperture_state_t, aperture_active) == 2 &&
               offsetof(hw_aperture_state_t, portal_span_count) == 3 &&
               offsetof(hw_aperture_state_t, dirty_words) == 8 &&
               offsetof(hw_aperture_state_t, portal_spans) == 24 &&
               offsetof(hw_aperture_state_t, ytop) == 408 &&
               offsetof(hw_aperture_state_t, ybot) == 760 &&
               sizeof(hw_portal_span_t) == 8 && sizeof(hw_ap_word_t) == 8 &&
               offsetof(hw_ap_word_t, yb_max) == 2 && offsetof(hw_ap_word_t, yt_max) == 4 &&
               offsetof(hw_ap_word_t, yb_min) == 6 && HW_VIEW_W == 320 && HW_MAX_PORTAL_SPANS == 32,
               "hw_asm_commit: layout");
const void *const hw_asm_commit_ctx[15] = {
    &hw_aperture_state, hw_solid_mask, hw_ap_words, sub_solid_mask,
    &sub_solid_active, &sub_solid_min_w, &sub_solid_max_w, &hw_solid_any,
    &hw_slave_dirty_words, &hw_solid_summary, &hw_solid_words, &hw_solid_count,
    &saturn_view_h, hw_bit_lut, (const void *)HW_CommitPortalClosedWord
};
#if HW_ASM_CHECK
static void HW_CommitSubsectorOcclusion_C(void);
/* [0] calls [1] mismatches [2] first differing byte offset in the snapshot */
static volatile uint32_t hw_asm_chk_commit[8];
typedef struct {
    hw_aperture_state_t ap;
    hw_ap_word_t aw[HW_SOLID_WORDS];
    uint32_t sm[HW_SOLID_WORDS], sub[HW_SOLID_WORDS];
    int act, mn, mx, words, count;
    uint32_t any, sdirty, summ;
} hw_occl_snap_t;
/* heap, not BSS: 3 x ~1.3 KB would push static data past 0x060C0000 */
extern byte *I_AllocHigh(int length);
static hw_occl_snap_t *hw_occl_snap;
static void HW_OcclSave(hw_occl_snap_t *s)
{
    memcpy(&s->ap, &hw_aperture_state, sizeof(s->ap));
    memcpy(s->aw, hw_ap_words, sizeof(s->aw));
    memcpy(s->sm, hw_solid_mask, sizeof(s->sm));
    memcpy(s->sub, sub_solid_mask, sizeof(s->sub));
    s->act = sub_solid_active; s->mn = sub_solid_min_w; s->mx = sub_solid_max_w;
    s->words = hw_solid_words; s->count = hw_solid_count;
    s->any = hw_solid_any; s->sdirty = hw_slave_dirty_words; s->summ = hw_solid_summary;
}
static void HW_OcclLoad(const hw_occl_snap_t *s)
{
    memcpy(&hw_aperture_state, &s->ap, sizeof(s->ap));
    memcpy(hw_ap_words, s->aw, sizeof(s->aw));
    memcpy(hw_solid_mask, s->sm, sizeof(s->sm));
    memcpy(sub_solid_mask, s->sub, sizeof(s->sub));
    sub_solid_active = s->act; sub_solid_min_w = s->mn; sub_solid_max_w = s->mx;
    hw_solid_words = s->words; hw_solid_count = s->count;
    hw_solid_any = s->any; hw_slave_dirty_words = s->sdirty; hw_solid_summary = s->summ;
}
#endif
static HW_NOINLINE void HW_CommitSubsectorOcclusion(void)
{
#if HW_ASM_CHECK
    if (!hw_occl_snap)
    {
        hw_occl_snap = (hw_occl_snap_t *)I_AllocHigh((int)(3 * sizeof(hw_occl_snap_t)));
        memset(hw_occl_snap, 0, 3 * sizeof(hw_occl_snap_t));
    }
    HW_OcclSave(&hw_occl_snap[0]);
    hw_asm_commit();
    HW_OcclSave(&hw_occl_snap[1]);
    HW_OcclLoad(&hw_occl_snap[0]);
    HW_CommitSubsectorOcclusion_C();
    HW_OcclSave(&hw_occl_snap[2]);
    hw_asm_chk_commit[0]++;
    {
        const uint8_t *a = (const uint8_t *)&hw_occl_snap[1];
        const uint8_t *c = (const uint8_t *)&hw_occl_snap[2];
        for (uint32_t i = 0; i < sizeof(hw_occl_snap_t); i++)
            if (a[i] != c[i])
            {
                if (hw_asm_chk_commit[1]++ == 0)
                {
                    hw_asm_chk_commit[2] = i;
                    hw_asm_chk_commit[3] = a[i];
                    hw_asm_chk_commit[4] = c[i];
                }
                break;
            }
    }
#else
    hw_asm_commit();
#endif
}
#define HW_COMMIT_C_SIG static void HW_CommitSubsectorOcclusion_C(void)
#else
#define HW_COMMIT_C_SIG static HW_NOINLINE HW_HOT_O2 void HW_CommitSubsectorOcclusion(void)
#endif

#if !HW_ASM_COMMIT || HW_ASM_CHECK
HW_COMMIT_C_SIG
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
#endif /* !HW_ASM_COMMIT || HW_ASM_CHECK */

static HW_NOINLINE HW_COLD_OS void HW_UpdateNodePvsVis(const byte *pvs_row, const void *ssec_key)
{
#if !HW_DBG_PVS
    pvs_row = NULL;
#endif
    hw_aperture_state_t *ap = HW_APERTURE;
    uint32_t *vis = ap->node_pvs_vis;
    node_t *local_nodes = nodes;
    int n_nodes = numnodes;
    /* node_pvs_vis holds HW_NODE_PVS_WORDS*32 nodes: never write past it on
     * a bigger map (2026-11-22); nodes beyond count as visible below. */
    int n_words = (n_nodes + 31) >> 5;
    if (n_words > HW_NODE_PVS_WORDS)
        n_words = HW_NODE_PVS_WORDS;
    const unsigned vis_cap = (unsigned)n_words << 5;
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
            v0 = ((unsigned)c0 >= vis_cap) || (vis[c0 >> 5] & hw_bit_lut[c0 & 31]) != 0;
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
            v1 = ((unsigned)c1 >= vis_cap) || (vis[c1 >> 5] & hw_bit_lut[c1 & 31]) != 0;
        }
        local_nodes[i].encbbox[1] = v1 ? (uint16_t)c1 : 0xFFFFu;

        if ((v0 | v1) && (unsigned)i < vis_cap)
            vis[i >> 5] |= hw_bit_lut[i & 31];
    }
    ap->last_pvs_ssec = ssec_key;
    ap->last_pvs_numnodes = n_nodes;
}
