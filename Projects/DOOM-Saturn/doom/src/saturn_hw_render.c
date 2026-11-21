/* saturn_hw_render.c — Top-Level Sega Saturn Hardware Renderer Orchestrator
 *
 * Implements Ezra Dreisbach's Lobotomy-style VDP1 distorted quad architecture
 * with dual-SH-2 SMP flat rasterization, hardware DIVU pipelining, and VDP2
 * background planes.
 *
 * Subsystem modules (included in unity-build order to preserve static linkage,
 * SH-2 literal-pool addressing, and zero-stall inline assembly scheduling):
 *   - saturn_hw_internal.h : Hardware defs, HW_DBG_* switches, viewport state
 *   - saturn_hw_occl.c     : 1D solid-wall bitmask, WRAM-L scratch, 2D aperture, PVS
 *   - saturn_hw_math.h     : Frustum planes, OCLUT, SH-2 MAC/DIVU/projection ASM
 *   - saturn_hw_sky.c      : Level asset preloader, BBox expansion, VDP2 sky
 *   - saturn_hw_sprites.c  : Player weapon psprites, lazy sector thing sort/draw
 *   - saturn_hw_flats.c    : Slave SH-2 flat quad pipeline, near-clipper, bands
 *   - saturn_hw_walls.c    : AABB frustum/occl cull, wall projection & strip emission
 */

#include "saturn_hw_internal.h"
#include "saturn_hw_occl.c"
#include "saturn_hw_math.h"
#include "saturn_hw_sky.c"
#include "saturn_hw_sprites.c"
#include "saturn_hw_flats.c"
#include "saturn_hw_walls.c"
#if HW_ORACLE && HW_DEFER_WALLS
#error "HW_ORACLE keeps its list in the deferred-wall region"
#endif

/* SH-2 cache-policy experiment (2026-11-21, off by default). CCR bits OD (2)
 * and ID (1) are REPLACEMENT disables (SH7604 HW manual 8.2 / 8.4.5): lines
 * already in the cache keep hitting, a miss is read straight from memory and
 * evicts nothing. HW_CCR_WALK = the CCR value held during the master's BSP
 * walk + emission (e.g. 0x05 = CE|OD: data reads stop evicting code;
 * 0x03 = CE|ID: code misses stop evicting data). Written from the
 * cache-through alias like _hw_purge_cache. */
#ifndef HW_CCR_WALK
#define HW_CCR_WALK 0
#endif
#if HW_CCR_WALK && !defined(SATURN_WIN32)
static HW_NOINLINE void hw_ccr_write(unsigned int v)
{
    (*(volatile unsigned char *)0xFFFFFE92u) = (unsigned char)v;
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;" ::: "memory");
}
#define HW_CCR_SET(v) ((void (*)(unsigned int))((uintptr_t)hw_ccr_write | SATURN_WRAM_H_UNCACHED))(v)
#else
#define HW_CCR_SET(v) ((void)0)
#endif

static unsigned int hw_prof_ap_acc = 0;
static unsigned int hw_prof_fsub_acc = 0;
static unsigned int hw_prof_spr_acc = 0;
static unsigned int hw_prof_occl_acc = 0;

/* Hitch diagnostics (2026-11-14): slowest subsector of the frame and how
 * its time splits (aperture / flat submit / walls / sprites / occlusion),
 * timed with the vblank-extended Saturn_ProfSpan so a multi-vblank stall
 * inside one subsector is measured correctly (the 16-bit FRT sub-timers
 * wrap every ~18 ms). Latched into the HUD hitch row by d_main.c. */
extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
extern volatile unsigned int saturn_vbl_count;
unsigned int saturn_hw_submax_tot = 0, saturn_hw_submax_ap = 0, saturn_hw_submax_fl = 0,
             saturn_hw_submax_wall = 0, saturn_hw_submax_spr = 0, saturn_hw_submax_occ = 0;
int          saturn_hw_submax_num = -1;
static unsigned int hw_submax_tot = 0, hw_submax_parts[5] = { 0, 0, 0, 0, 0 };
static int          hw_submax_num = -1;

/* Slowest-subsector latch (HUD row 2 "U n/total A F G P O"). It cost 5
 * out-of-line Saturn_ProfSpan calls + timer reads per subsector (~150 per
 * heavy frame) in EVERY build, so it is now on only in profiling builds or
 * with -DHW_SUBMAX=1 (2026-11-16, walk rewrite stage A). Off: HUD shows U0. */
#ifndef HW_SUBMAX
#define HW_SUBMAX HW_PROF_SUBTIMERS
#endif
#if HW_SUBMAX
#define HW_SUBPROF_MARK(slot)                                           \
    do {                                                                \
        unsigned int   v_ = saturn_vbl_count;                           \
        unsigned short f_ = SAT_FRT();                                  \
        sp_parts[slot] = Saturn_ProfSpan(sp_f, sp_v);                   \
        sp_f = f_; sp_v = v_;                                           \
    } while (0)
#else
#define HW_SUBPROF_MARK(slot) do { } while (0)
#endif

#if HW_ASM_SUBBODY
/* Stage E2 (saturn_hw_asm.s): Body + HW_RenderSubsectorBakedWalls. */
extern void hw_asm_subsector(int subnum, int fully_inside);
_Static_assert(sizeof(subsector_t) == 4, "sub1");
_Static_assert(offsetof(subsector_t, sector) == 2, "sub2");
_Static_assert(sizeof(hw_sec_fast_t) == 16, "sub3");
_Static_assert(offsetof(hw_sec_fast_t, has_things) == 13, "sub4");
_Static_assert(offsetof(hw_sec_fast_t, light_delta) == 14, "sub5");
_Static_assert(offsetof(sub_range_t, s_start) == 4, "sub6");
_Static_assert(offsetof(sub_range_t, subnum) == 8, "sub7");
_Static_assert(HW_MAX_AP_SECTORS == 512, "sub8");
_Static_assert(CMDBUF_MAX_VISIT == 384, "sub9");
_Static_assert(HW_FLAT_ALLOW_CEIL == 8, "sub10");
_Static_assert(sizeof(saturn_wl_subsector_t) == 16, "sub11");
_Static_assert(offsetof(saturn_wl_subsector_t, portal_first) == 8, "sub12");
_Static_assert(sizeof(saturn_wl_pillar_t) == 4, "sub13");
_Static_assert(sizeof(baked_seg_t) == 16, "sub14");
_Static_assert(HW_MAX_SUBSECTOR_PILLARS == 96, "sub15");
_Static_assert(offsetof(hw_wall_emit_ctx_t, is_sky_ceil) == 3, "sub16");
_Static_assert(offsetof(hw_wall_emit_ctx_t, n_vis) == 4, "sub17");
_Static_assert(offsetof(hw_wall_emit_ctx_t, ytop_min) == 12, "sub18");
_Static_assert(offsetof(hw_wall_emit_ctx_t, ybot_max) == 14, "sub19");
_Static_assert(offsetof(hw_wall_emit_ctx_t, sec_ceiling) == 32, "sub20");
_Static_assert(offsetof(hw_wall_emit_ctx_t, sec_floor) == 36, "sub21");
_Static_assert(offsetof(hw_wall_emit_ctx_t, cur_tex_id) == 60, "sub22");
_Static_assert(offsetof(hw_wall_emit_ctx_t, sub_m_start) == 74, "sub23");
_Static_assert(offsetof(hw_wall_emit_ctx_t, last_clip_x0) == 76, "sub24");
_Static_assert(offsetof(hw_wall_emit_ctx_t, last_clip_x1) == 78, "sub25");
_Static_assert(offsetof(hw_wall_emit_ctx_t, seg_end) == 108, "sub26");
_Static_assert(offsetof(hw_wall_emit_ctx_t, portal_first) == 120, "sub27");
_Static_assert(offsetof(hw_wall_emit_ctx_t, portal_count) == 122, "sub28");
#define HW_RenderSubsector(subnum, fully_inside) hw_asm_subsector((subnum), (fully_inside))
#else
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsector_Body(int subnum, int fully_inside,
                                                         unsigned int *sp_parts);
#endif

/* Walk a subsector (takes 2 register arguments: r4=subnum, r5=fully_inside) */
#if HW_ASM_SUBBODY
#elif HW_SUBMAX
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsector(int subnum, int fully_inside)
{
    unsigned int parts[5] = { 0, 0, 0, 0, 0 };
    HW_RenderSubsector_Body(subnum, fully_inside, parts);
    unsigned int tot = parts[0] + parts[1] + parts[2] + parts[3] + parts[4];
    if (tot > hw_submax_tot)
    {
        hw_submax_tot = tot;
        hw_submax_num = subnum;
        for (int i = 0; i < 5; i++)
            hw_submax_parts[i] = parts[i];
    }
}
#else
#define HW_RenderSubsector(subnum, fully_inside) HW_RenderSubsector_Body((subnum), (fully_inside), NULL)
#endif

#if HW_ASM_SUBBODY
#if HW_ASM_CHECK
#define HW_SUB_COMMIT_FN HW_CommitSubsectorOcclusion   /* keeps the commit check */
#else
#define HW_SUB_COMMIT_FN hw_asm_commit
#endif
/* Wall half: [0] &saturn_baked_walls_active [4] &saturn_baked_wall_num_subsectors
 * [8] &saturn_baked_wall_subsectors [12] &hw_wctx [16] &hw_wall_light_delta
 * [20] &hw_solid_count [24] RecordSubsectorUnbakedPortals [28] &hw_prof_segs_acc
 * [32] HW_ProfDelta_Wrap [36] &hw_cur_pil_gen [40] hw_pil_gen
 * [44] &saturn_baked_wall_pillars [48] &hw_cur_pillars [52] &hw_cur_pfrac
 * [56] &saturn_baked_wall_pillar_frac [60] hw_zero_pfrac [64] &saturn_baked_wall_segs
 * [68] &hw_wctx_tex_frame [72] &hw_render_frame [76] &saturn_cmd_m_count
 * [80] &saturn_view_h [84] hw_asm_walk [88] hw_asm_walk_ctx [92] &saturn_hw_seg_iter
 * [96] &saturn_hw_seg_vis [100] HW_EmitUserClip */
const void *const hw_asm_subw_ctx[26] = {
    &saturn_baked_walls_active, &saturn_baked_wall_num_subsectors, &saturn_baked_wall_subsectors,
    &hw_wctx, &hw_wall_light_delta, &hw_solid_count,
    (const void *)HW_RecordSubsectorUnbakedPortals, &hw_prof_segs_acc,
    (const void *)HW_ProfDelta_Wrap, &hw_cur_pil_gen, hw_pil_gen,
    &saturn_baked_wall_pillars, &hw_cur_pillars, &hw_cur_pfrac,
    &saturn_baked_wall_pillar_frac, hw_zero_pfrac, &saturn_baked_wall_segs,
    &hw_wctx_tex_frame, &hw_render_frame, (const void *)&saturn_cmd_m_count,
    &saturn_view_h, (const void *)hw_asm_walk, hw_asm_walk_ctx, &saturn_hw_seg_iter,
    &saturn_hw_seg_vis, (const void *)HW_EmitUserClip
};
/* Body half: [0] &hw_solid_count [4] &numsubsectors [8] &subsectors
 * [12] &saturn_visit_count [16] hw_sec_fast [20] HW_CheckSubsectorAperture
 * [24] &saturn_hw_occl_culls [28] &saturn_hw_sky_visible [32] &saturn_cmd_m_count
 * [36] &hw_cur_sub_ranges [40] &saturn_slave_enabled [44] &sectors
 * [48] HW_SubmitSlaveFlatJob [52] HW_RenderSubsectorBakedFlats
 * [56] HW_SubsectorHasThings [60] HW_RenderSubsectorSprites
 * [64] &sub_solid_active [68] &hw_aperture_state [72] commit
 * [76] hw_asm_subw_ctx [80] sizeof(sector_t) */
const void *const hw_asm_sub_ctx[21] = {
    &hw_solid_count, &numsubsectors, &subsectors, &saturn_visit_count,
    hw_sec_fast, (const void *)HW_CheckSubsectorAperture, &saturn_hw_occl_culls,
    &saturn_hw_sky_visible, (const void *)&saturn_cmd_m_count, &hw_cur_sub_ranges,
    &saturn_slave_enabled, &sectors, (const void *)HW_SUBMIT_FLAT_JOB,
    (const void *)HW_RenderSubsectorBakedFlats, (const void *)HW_SubsectorHasThings,
    (const void *)HW_RenderSubsectorSprites, &sub_solid_active, &hw_aperture_state,
    (const void *)HW_SUB_COMMIT_FN, hw_asm_subw_ctx, (const void *)(uintptr_t)sizeof(sector_t)
};
#else
#if HW_ASM
const void *const hw_asm_sub_ctx[1] = { 0 };   /* asm stays linked, unused */
#endif
#ifndef HW_SSMAP_DBG
#define HW_SSMAP_DBG 0
#endif
#if HW_SSMAP_DBG
volatile uint8_t hw_ssmap_dbg[256];
volatile uint8_t hw_ssmap_last[256];   /* copy of the last complete frame */
#endif
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsector_Body(int subnum, int fully_inside,
                                                         unsigned int *sp_parts)
{
    if (hw_solid_count >= HW_VIEW_W || (unsigned)subnum >= (unsigned)numsubsectors)
        return;

    int secnum = subsectors[subnum].sector;
    if ((unsigned)secnum >= HW_MAX_AP_SECTORS || saturn_visit_count >= CMDBUF_MAX_VISIT)
        return;
    hw_sec_fast_t *sf = &hw_sec_fast[secnum];
#if HW_SUBMAX
    unsigned int   sp_v = saturn_vbl_count;
    unsigned short sp_f = SAT_FRT();
#else
    (void)sp_parts;
#endif
#if HW_PROF_SUBTIMERS
    unsigned short ts0 = SAT_FRT();
#endif
    HW_VS(5);
    int flat_flags;
#if HW_ORACLE
    if (hw_oracle_on)
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        const hw_oracle_t *o = hw_oracle_cur;
        if (o->flags == -2)
        {
            int vh = HW_VIEW_H;
            flat_flags = (fully_inside & 3) | (int)HW_FLAT_ALLOW_BOTH;
            ap->cur_sub_yt_attach = 0;
            ap->cur_sub_yb_attach = (int16_t)vh;
            ap->cur_sub_ytop = 0;
            ap->cur_sub_ybot = (uint8_t)vh;
            ap->cur_ytop_min = 0;
            ap->cur_ybot_max = (int16_t)vh;
        }
        else
        {
            flat_flags = o->flags;
            ap->cur_sub_yt_attach = o->yt_att;
            ap->cur_sub_yb_attach = o->yb_att;
            ap->cur_sub_ytop = o->ytop;
            ap->cur_sub_ybot = o->ybot;
            ap->cur_ytop_min = o->ytop_min;
            ap->cur_ybot_max = o->ybot_max;
        }
    }
    else
#endif
    flat_flags = HW_CheckSubsectorAperture(subnum, sf, fully_inside & 3);
    HW_SUBPROF_MARK(0);
#if HW_PROF_SUBTIMERS
    unsigned short ts1 = SAT_FRT();
    hw_prof_ap_acc += HW_ProfDelta(ts0, ts1);
#endif
#if HW_ORACLE
    if (!hw_oracle_on && hw_oracle_rec_n < HW_ORACLE_CAP && (flat_flags >= 0 || HW_ORACLE == 2))
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        hw_oracle_t *o = &hw_oracle_list[hw_oracle_rec_n++];
        o->subnum = (uint16_t)subnum;
        o->fully = (uint8_t)fully_inside;
        o->flags = (int16_t)((flat_flags >= 0) ? flat_flags : -2);
        o->yt_att = ap->cur_sub_yt_attach;
        o->yb_att = ap->cur_sub_yb_attach;
        o->ytop = ap->cur_sub_ytop;
        o->ybot = ap->cur_sub_ybot;
        o->ytop_min = ap->cur_ytop_min;
        o->ybot_max = ap->cur_ybot_max;
    }
#endif
#if HW_SSMAP_DBG
    /* debug (-DHW_SSMAP_DBG=1): [0..127] leaves reached, [128..255] leaves
     * rendered, bitmaps; cleared at frame start */
    hw_ssmap_dbg[subnum >> 3] |= (uint8_t)(1u << (subnum & 7));
    if (flat_flags >= 0)
        hw_ssmap_dbg[128 + (subnum >> 3)] |= (uint8_t)(1u << (subnum & 7));
#endif
    if (flat_flags < 0)
    {
        saturn_hw_occl_culls++;
        HW_VS(6);
        return;
    }
    HW_VS(7);
#if HW_VIS_STATS
    hw_vis_ssmap[subnum >> 3] |= (uint8_t)(1u << (subnum & 7));
#endif

    if (sf->is_sky)
    {
        if (flat_flags & (int)HW_FLAT_ALLOW_CEIL)
            saturn_hw_sky_visible = 1;
        flat_flags &= ~(int)HW_FLAT_ALLOW_CEIL;
    }

    int visit_idx = saturn_visit_count++;
    uint16_t m_start = saturn_cmd_m_count;
    sub_range_t *ranges = hw_cur_sub_ranges;
    aliased_uint32_t *r32 = (aliased_uint32_t *)&ranges[visit_idx];
    r32[1] = 0;
    ranges[visit_idx].subnum = (uint16_t)subnum;
#ifdef SATURN_WIN32
    {
        extern int hw_dbg_visit_flags[1024];   /* harness -sweep SWEEP_DUMP */
        extern int hw_dbg_trace;
        if (visit_idx < 1024)
            hw_dbg_visit_flags[visit_idx] = flat_flags;
        if (hw_dbg_trace)
            printf("[VISDBG] v%d ss %d sec %d solid %d ap %d ytop %d ybot %d dzf %d dzc %d bbox %d..%d %d..%d\n",
                   visit_idx, subnum, secnum, hw_solid_count, HW_APERTURE->aperture_active,
                   HW_APERTURE->cur_sub_ytop, HW_APERTURE->cur_sub_ybot,
                   sf->dz_floor >> FRACBITS, sf->dz_ceil >> FRACBITS,
                   saturn_baked_flat_bboxes ? saturn_baked_flat_bboxes[subnum].min_x : 0,
                   saturn_baked_flat_bboxes ? saturn_baked_flat_bboxes[subnum].max_x : 0,
                   saturn_baked_flat_bboxes ? saturn_baked_flat_bboxes[subnum].min_y : 0,
                   saturn_baked_flat_bboxes ? saturn_baked_flat_bboxes[subnum].max_y : 0);
    }
#endif

    if (!HW_WALLS_ONLY && (flat_flags & (int)HW_FLAT_ALLOW_BOTH))
    {
        sector_t *sec = &sectors[secnum];
#ifndef SATURN_WIN32
        if (__builtin_expect(saturn_slave_enabled, 1))
        {
            /* Concurrent execution: Submit to Slave so Slave processes flats while Master computes wall geometry */
            HW_SUBMIT_FLAT_JOB(visit_idx, subnum, sec, flat_flags);
        }
        else
#endif
        {
            HW_RenderSubsectorBakedFlats(visit_idx, subnum, sec, flat_flags);
        }
    }
    HW_SUBPROF_MARK(1);
#if HW_PROF_SUBTIMERS
    hw_prof_fsub_acc += HW_ProfDelta(ts1, SAT_FRT());
#endif

    /* 1. Render segs (front walls of this subsector). Deferred mode
     * (saturn_hw_walls.c): record the visible walls, emit them in pass 2. */
#if HW_DEFER_WALLS
    int defer_rec0 = hw_defer_nrec;
    {
        int ns = (saturn_baked_walls_active && (unsigned)subnum < (unsigned)saturn_baked_wall_num_subsectors)
               ? (int)saturn_baked_wall_subsectors[subnum].num_segs : 0;
        hw_defer_on = (hw_defer_nsub < HW_DEFER_SUB_CAP && hw_defer_nrec + ns <= HW_DEFER_REC_CAP);
        hw_defer_need_snap = 0;
    }
#endif
    HW_RenderSubsectorBakedWalls(subnum, sf);
#if HW_V2P_PUNCH
    /* plane floor: erase farther geometry below this subsector's portal
     * floor lines (before its sprites; saturn_hw_walls.c) */
    if (__builtin_expect(SATURN_V2P_SKIP_FLOOR(&sectors[secnum]), 0) && HW_V2PlaneWanted(subnum))
        HW_V2PlanePunch(subnum, sf);
#endif
    HW_SUBPROF_MARK(2);

    /* 2. Render subsector sprites (lazy per-sector frustum-culled bucketing) */
#if HW_PROF_SUBTIMERS
    unsigned short ts2 = SAT_FRT();
#endif
#if HW_DEFER_WALLS
    int defer_now = hw_defer_on;
    uint16_t spr_m0 = saturn_cmd_m_count;
    hw_defer_on = 0;
#endif
    if (!HW_WALLS_ONLY && __builtin_expect(sf->has_things != 0, 0))
    {
        sector_t *sec = &sectors[secnum];
        if (HW_SubsectorHasThings(sec, (uint32_t)secnum, subnum))
            HW_RenderSubsectorSprites(sec, subnum);
    }
    HW_SUBPROF_MARK(3);
#if HW_PROF_SUBTIMERS
    unsigned short ts3 = SAT_FRT();
    hw_prof_spr_acc += HW_ProfDelta(ts2, ts3);
#endif

#if HW_VIS_STATS
    {
        int out_m = (saturn_cmd_m_count != m_start)
#if HW_DEFER_WALLS
                    || (hw_defer_nrec != defer_rec0)
#endif
                    ;
        if (!out_m)
            HW_VS((flat_flags & (int)HW_FLAT_ALLOW_BOTH) ? 9 : 8);
    }
#endif
#ifndef SATURN_WIN32
    r32[0] = ((uint32_t)m_start << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
#else
    ranges[visit_idx].m_start = m_start;
    ranges[visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);
#endif
#if HW_DEFER_WALLS
    /* After the range write (an immediate flush rewrites it) and before
     * the occlusion commit (the snapshot must not include this subsector). */
    if (defer_now)
        HW_DeferCloseSub(visit_idx, subnum, defer_rec0, spr_m0,
                         (int)(saturn_cmd_m_count - spr_m0), sf->light_delta);
#endif

    /* 3. Mark solid spans and portal apertures from this subsector */
    if (sub_solid_active || HW_APERTURE->portal_span_count > 0)
    {
        HW_CommitSubsectorOcclusion();
#if HW_PROF_SUBTIMERS
        hw_prof_occl_acc += HW_ProfDelta(ts3, SAT_FRT());
#endif
    }
    HW_SUBPROF_MARK(4);
}
#endif /* HW_ASM_SUBBODY */

typedef struct {
    uint16_t bspnum;       /* must be unsigned: node_t children are uint16_t (NF_SUBSECTOR = 0x8000) */
    int16_t  fully_inside;
    const int16_t *bbox;
} bsp_stack_t;

#define HW_BSP_STACK_MAX 64

/* BSP node far plane in map units << 12 (adaptive detail; default the
 * HW_FAR_Z limit). Read cache-through by hw_asm_cullbox so it can change
 * between frames (and be poked by the 5thPlanet harness). */
volatile int32_t hw_far_cull = HW_FAR_Z_UNITS << 12;

/* Adaptive detail (2026-11-20, user: "try adaptive detail to keep the FPS
 * high"). After each rendered 3D frame: frames slower than 30 FPS step the
 * detail level up, a run of comfortably fast frames steps it back down.
 * Levels first cap the BSP draw distance (the E1M1 courtyard spends ~8 ms
 * on areas seen through far window openings: 5thPlanet p2 25.8 -> 30.0 FPS
 * at 1536 units with 69 px changed, m1b 22.5 -> 30.0 at 1280 with 386 px;
 * the other 10 tour views neither change nor gain), then coarsen the wall
 * strip LOD (m1a, where distance does not help: +0.5..0.7 FPS).
 * hw_ad_mode (cache-through, pokeable): 1 adaptive, 0 off (full detail). */
volatile int32_t hw_ad_mode = 0;   /* default DETAIL: FULL (user, 2026-11-20: the distance cut
                                    * visibly removes far geometry, e.g. E1M8 over the low fence) */
int saturn_hw_detail_level = 0;                 /* HUD */
unsigned int saturn_hw_ad_avg = 0, saturn_hw_ad_last = 0;   /* debug: frame ticks (avg, last) */
static const int16_t hw_ad_far[]   = { 16384, 2048, 1536, 1280, 1280, 1280 };
static const uint8_t hw_ad_lodx[]  = {     1,    1,    1,    1,    2,    3 };
#define HW_AD_LEVELS ((int)(sizeof(hw_ad_far) / sizeof(hw_ad_far[0])))
static int hw_ad_rendered = 0;                  /* a 3D frame was drawn since the last call */

static void HW_AdaptiveApply(int level)
{
    static int32_t base[16];
    static int have_base = 0, lodx_now = 1;
    if (!have_base)
    {
        for (int i = 0; i < 16; i++)
            base[i] = hw_wall_lod[i];
        have_base = 1;
    }
    hw_far_cull = (int32_t)hw_ad_far[level] << 12;
    if (hw_ad_lodx[level] != lodx_now)
    {
        static const uint8_t dx_idx[] = { HW_LOD_M16_DX, HW_LOD_M16_FAR_DX, HW_LOD_M32_DX, HW_LOD_H64_DX,
                                          HW_LOD_H64_NF_DX, HW_LOD_C4_NF_DX, HW_LOD_C4_DG_DX,
                                          HW_LOD_MERGE_DX, HW_LOD_C8_DX };
        lodx_now = hw_ad_lodx[level];
        for (unsigned int k = 0; k < sizeof(dx_idx); k++)
        {
            int32_t v = base[dx_idx[k]];
            hw_wall_lod[dx_idx[k]] = (v > 0) ? v * lodx_now : v;   /* <= 0: rule off, keep */
        }
    }
    saturn_hw_detail_level = level;
}

/* d_main.c, after each displayed frame: frame_ticks = whole-frame FRT
 * span, rate = FRT ticks per field. */
void Saturn_HW_AdaptiveDetail(unsigned int frame_ticks, unsigned int rate)
{
    static int level = 0, slow_run = 0, fast_run = 0;
    /* backoff: stepping down then straight back up (a view on the edge of
     * 30 FPS) doubles the fast run needed for the next step down, so far
     * detail does not pop in and out every couple of seconds */
    static int down_need = 45, since_down = 1 << 20, since_up = 0;
    if (!hw_ad_rendered || rate == 0)
        return;                                 /* menus / 2D frames: leave it */
    hw_ad_rendered = 0;
    if (since_down < (1 << 20)) since_down++;
    if (++since_up > 1800)                      /* ~1 min without stepping up */
    {
        down_need = 45;
        since_up = 0;
    }
#ifndef SATURN_WIN32
    if (!*(const volatile int32_t *)((const char *)&hw_ad_mode + 0x20000000u))
#else
    if (!hw_ad_mode)    /* no SH-2 cache-through alias on the PC (crashed the harness) */
#endif
    {
        if (level != 0)
            HW_AdaptiveApply(level = 0);
        slow_run = fast_run = 0;
        return;
    }
    const unsigned int slow = 2u * rate + rate / 16u;   /* > ~34.4 ms: under 30 FPS */
    const unsigned int fast = 2u * rate - rate / 8u;    /* < ~31.3 ms (backoff handles a view on the edge) */
    /* decide on a running average (1/8 per frame), not single frames:
     * texture-upload spikes while turning would otherwise pin the level at
     * the top although the average frame has headroom */
    static unsigned int avg = 0;
    saturn_hw_ad_last = frame_ticks;
    if (frame_ticks > 6u * rate)
        frame_ticks = 6u * rate;                /* a load hitch must not swamp it */
    avg = avg ? avg + (unsigned int)(((int)frame_ticks - (int)avg) >> 3) : frame_ticks;
    frame_ticks = avg;
    saturn_hw_ad_avg = avg;
    if (frame_ticks > slow)
    {
        fast_run = 0;
        if (++slow_run >= 6 && level < HW_AD_LEVELS - 1)   /* ~ the average's lag */
        {
            HW_AdaptiveApply(++level);
            slow_run = 0;
            since_up = 0;
            if (since_down < 120 && down_need < 600)
                down_need *= 2;                 /* bounced: wait longer next time */
        }
    }
    else if (frame_ticks < fast)
    {
        slow_run = 0;
        if (++fast_run >= down_need && level > 0)   /* >= ~1.5 s of headroom */
        {
            HW_AdaptiveApply(--level);
            fast_run = 0;
            since_down = 0;
        }
    }
    else
        slow_run = 0;           /* in between: neither counts (keeps fast_run) */
}
#if HW_ASM_BSP
/* Stage E1 (saturn_hw_asm.s): hw_asm_bsp(root, inside) walks the tree,
 * hw_asm_cullbox(bbox, fully_inside) = HW_CullBSPBBox. Table: */
extern void hw_asm_bsp(int root_bspnum, int root_inside);
extern int  hw_asm_cullbox(const int16_t *bbox, int fully_inside);
static bsp_stack_t hw_bsp_stack[HW_BSP_STACK_MAX];
_Static_assert(sizeof(bsp_stack_t) == 8, "hw_asm_bsp: 8-byte stack entries");
_Static_assert(sizeof(node_t) == 16, "hw_asm_bsp: 16-byte nodes");
_Static_assert(HW_VIEW_W == 320 && HW_VIEW_CX == 160, "hw_asm_cullbox: 320 columns");
_Static_assert(HW_NEAR_Z_UNITS == 4 && HW_FAR_Z_UNITS == 16384, "hw_asm_cullbox: near/far");
const void *const hw_asm_bsp_ctx[21] = {
    &hw_frust, &hw_cam_x, &hw_cam_y, &hw_solid_count,                    /*  0 */
    &saturn_hw_bsp_occl, &saturn_hw_occl_culls, saturn_master_oclut,     /* 16 */
    hw_solid_mask, hw_mask_left, hw_mask_right, hw_span_mask_lut,        /* 28 */
    &hw_solid_any, &hw_solid_summary, &saturn_view_w,                    /* 44 */
#if HW_ASM_SUBBODY
    &nodes, hw_bsp_stack, (const void *)hw_asm_subsector,                /* 56 */
#else
    &nodes, hw_bsp_stack, (const void *)HW_RenderSubsector_Body,         /* 56 */
#endif
    &hw_node_bboxes, &saturn_baked_flat_bboxes, &saturn_hw_bsp_abort,    /* 68 */
    (const void *)((const char *)&hw_far_cull + 0x20000000u)             /* 80: cache-through */
};
#if HW_ASM_CHECK
/* cullbox: [0] calls [1] mismatches [2] bbox [3] fully [4] asm [5] C */
static volatile uint32_t hw_asm_chk_cullbox[8];
static HW_NOINLINE int HW_CullBoxCheck(const int16_t *bbox, int fully_inside)
{
    unsigned int culls0 = saturn_hw_occl_culls;
    int a = hw_asm_cullbox(bbox, fully_inside);
    unsigned int culls_a = saturn_hw_occl_culls;
    saturn_hw_occl_culls = culls0;
    int c = HW_CullBSPBBox(bbox, fully_inside);
    hw_asm_chk_cullbox[0]++;
    if ((a != c || culls_a != saturn_hw_occl_culls) && hw_asm_chk_cullbox[1]++ == 0)
    {
        hw_asm_chk_cullbox[2] = (uint32_t)bbox; hw_asm_chk_cullbox[3] = (uint32_t)fully_inside;
        hw_asm_chk_cullbox[4] = (uint32_t)a;    hw_asm_chk_cullbox[5] = (uint32_t)c;
    }
    return a;
}
#define HW_CullBSPBBox HW_CullBoxCheck
#else
/* Normal build: the asm walk. The root-leaf case (one-subsector map)
 * stays C so the asm needs no numnodes (VINT width). */
static void HW_RenderBSPNode(int root_bspnum, const byte *pvs_row, int root_inside)
{
    (void)pvs_row;
    if (__builtin_expect((unsigned)root_bspnum >= (unsigned)numnodes, 0))
    {
        if ((int16_t)root_bspnum < 0 && (int16_t)root_bspnum != -1)
            HW_RenderSubsector(root_bspnum & 0x7FFFu, root_inside);
        return;
    }
    hw_asm_bsp(root_bspnum, root_inside);
}
#endif
#elif HW_ASM
const void *const hw_asm_bsp_ctx[1] = { 0 };   /* asm stays linked, unused */
#endif

#if !HW_ASM_BSP || HW_ASM_CHECK
/* Front-to-back iterative BSP tree traversal with zero C-call overhead and deferred occlusion */
static HW_NOINLINE HW_HOT_O2 void HW_RenderBSPNode(int root_bspnum, const byte *pvs_row, int root_inside)
{
    (void)pvs_row;
    if (__builtin_expect((unsigned)root_bspnum >= (unsigned)numnodes, 0))
    {
        if ((int16_t)root_bspnum < 0 && (int16_t)root_bspnum != -1)
            HW_RenderSubsector(root_bspnum & 0x7FFFu, root_inside);
        return;
    }
    static bsp_stack_t bsp_stack[HW_BSP_STACK_MAX];
    const node_t * const local_nodes = nodes;
    const int16_t (* const local_bboxes)[2][4] = hw_node_bboxes;
    const int cam_x = hw_cam_x;
    const int cam_y = hw_cam_y;
    bsp_stack_t * const stack_base = bsp_stack;
    bsp_stack_t * const stack_end  = bsp_stack + HW_BSP_STACK_MAX;
    bsp_stack_t *sp_ptr = stack_base;

    sp_ptr->bspnum = (uint16_t)root_bspnum;
    sp_ptr->fully_inside = (int16_t)root_inside;
    sp_ptr->bbox = NULL;
    sp_ptr++;

    while (sp_ptr > stack_base)
    {
        if (hw_solid_count >= HW_VIEW_W) {
            saturn_hw_bsp_abort++;
            return;
        }

        --sp_ptr;
        int bspnum = sp_ptr->bspnum;
        int fully_inside = sp_ptr->fully_inside;
        const int16_t *bbox = sp_ptr->bbox;

        /* Deferred Frustum & Occlusion Check: Evaluate back-children only AFTER the front 
         * subtree has rendered. If the front walls filled the screen or occlude this box, discard it. */
        if (bbox && (!fully_inside || (hw_solid_count > 0 && saturn_hw_bsp_occl != 2))) {
            fully_inside = HW_CullBSPBBox(bbox, fully_inside);
            if (fully_inside < 0)
                continue;
        }

        while (1)
        {
            /* Leaf check: MARS encoding has bit 15 set */
            if ((int16_t)bspnum < 0) {
                if ((int16_t)bspnum != -1) {
                    HW_RenderSubsector(bspnum & 0x7FFFu, fully_inside);
                }
                break;
            }

            int side, c0, c1;
#ifndef SATURN_WIN32
            const int16_t *np = (const int16_t *)&local_nodes[bspnum];
            int t0;
            __asm__(
                "mov.w @%4+, %0 \n\t"   /* %0 = bsp->x */
                "mov.w @%4+, %1 \n\t"   /* %1 = bsp->y */
                "sub   %5, %0   \n\t"   /* %0 = bsp->x - cam_x */
                "sub   %6, %1   \n\t"   /* %1 = bsp->y - cam_y */
                "mov.w @%4+, %2 \n\t"   /* %2 = bsp->dx */
                "muls.w %1, %2  \n\t"   /* macl = (bsp->y - cam_y) * bsp->dx */
                "mov.w @%4+, %3 \n\t"   /* %3 = bsp->dy */
                "add   #4, %4   \n\t"   /* skip children[0..1], point at encbbox[0] */
                "sts   macl, %1 \n\t"   /* %1 = (bsp->y - cam_y) * bsp->dx */
                "muls.w %0, %3  \n\t"   /* macl = (bsp->x - cam_x) * bsp->dy */
                "mov.w @%4+, %2 \n\t"   /* %2 = c0 = (int16_t)bsp->encbbox[0] (fills muls.w latency!) */
                "mov.w @%4+, %3 \n\t"   /* %3 = c1 = (int16_t)bsp->encbbox[1] (fills muls.w latency!) */
                "sts   macl, %0 \n\t"   /* %0 = (bsp->x - cam_x) * bsp->dy */
                "cmp/ge %1, %0  \n\t"   /* T = ((bsp->x - cam_x)*dy >= (bsp->y - cam_y)*dx) */
                "movt  %0       \n\t"   /* %0 = side (0 or 1) */
                : "=&r"(side), "=&r"(t0), "=&r"(c0), "=&r"(c1), "+r"(np)
                : "r"(cam_x), "r"(cam_y)
                : "macl", "t"
            );
#else
            const node_t *bsp = &local_nodes[bspnum];
            side = (HW_MulsW_PairAdd(cam_y - (int)bsp->y, (int)bsp->dx,
                                     (int)bsp->x - cam_x, (int)bsp->dy, 0) >= 0);
            c0 = (int)(int16_t)bsp->encbbox[0];
            c1 = (int)(int16_t)bsp->encbbox[1];
#endif
            int child_front = side ? c1 : c0;
            int child_back  = side ? c0 : c1;

            /* -- 1. EVALUATE BACK CHILD (PUSH TO STACK) -- */
            if (child_back != -1 && sp_ptr < stack_end) {
                const int16_t *cbbox_back = NULL;
                if (local_bboxes && (child_back >= 0 || !saturn_baked_flat_bboxes))
                    cbbox_back = local_bboxes[bspnum][side ^ 1];
                sp_ptr->bspnum = (uint16_t)child_back;
                sp_ptr->fully_inside = (int16_t)fully_inside;
                sp_ptr->bbox = cbbox_back; /* Deferred frustum & occlusion check when popped */
                sp_ptr++;
            }

            /* -- 2. EVALUATE FRONT CHILD (TAIL-CALL LOOP) -- */
            if (child_front == -1)
                break;
            if (child_front >= 0 && local_bboxes &&
                (!fully_inside || (hw_solid_count > 0 && saturn_hw_bsp_occl != 2)))
            {
                fully_inside = HW_CullBSPBBox(local_bboxes[bspnum][side], fully_inside);
                if (fully_inside < 0)
                    break;
            }
            bspnum = child_front;
        }
    }
}
#endif /* !HW_ASM_BSP || HW_ASM_CHECK */

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
#if SATURN_BENCH && !defined(SATURN_WIN32)
    {
        static int32_t lod_seq_seen = 0;
        const volatile int32_t *in =
            (const volatile int32_t *)((uintptr_t)hw_wall_lod_in | 0x20000000u);   /* cache-through */
        int32_t seq = in[16];
        if (seq != lod_seq_seen)
        {
            lod_seq_seen = seq;
            for (int i = 0; i < 16; i++)
                hw_wall_lod[i] = in[i];
        }
    }
#endif
#ifdef SATURN_WIN32
    {
        /* DOOM-Windows strip-LOD tuning: HW_WALL_LOD=v0,v1,... overrides
         * hw_wall_lod[] (scored against the DOS reference pane). */
        static int lod_env_done = 0;
        if (!lod_env_done)
        {
            const char *e = getenv("HW_WALL_LOD");
            int i = 0;
            lod_env_done = 1;
            while (e && *e && i < 16)
            {
                hw_wall_lod[i++] = (int32_t)strtol(e, (char **)&e, 10);
                if (*e == ',')
                    e++;
            }
        }
    }
#endif

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

    /* Teleport / respawn / vertical step discontinuity: never blend across a discontinuity
     * (one frame halfway between source and destination). No legal per-tic move exceeds 128 XY or 16 Z units. */
#define HW_INTERP_SNAP   ((uint32_t)(128 << FRACBITS))
#define HW_INTERP_Z_SNAP ((uint32_t)(16 << FRACBITS))
    if (((uint32_t)cur_vx - (uint32_t)prev_viewx + HW_INTERP_SNAP > 2u * HW_INTERP_SNAP) ||
        ((uint32_t)cur_vy - (uint32_t)prev_viewy + HW_INTERP_SNAP > 2u * HW_INTERP_SNAP) ||
        ((uint32_t)cur_vz - (uint32_t)prev_viewz + HW_INTERP_Z_SNAP > 2u * HW_INTERP_Z_SNAP))
    {
        prev_viewx = cur_vx;
        prev_viewy = cur_vy;
        prev_viewz = cur_vz;
        prev_viewangle = cur_va;
    }
#undef HW_INTERP_Z_SNAP
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
        fixed_t cz_orig = FixedMul(frac_x, hw_viewcos) + FixedMul(frac_y, hw_viewsin);
        fixed_t cx_orig = FixedMul(frac_x, hw_viewsin) - FixedMul(frac_y, hw_viewcos);
        hw_frust.cz_origin = cz_orig;
        hw_frust.cx_origin = cx_orig;
    }

    if (saturn_vdp2_plane_active)
    {
        Saturn_VDP2_Plane_Frame(hw_viewx, hw_viewy, hw_viewz, hw_viewangle, saturn_vdp2_plane_floorz);
    }
    /* Fetch true rendering subsector and PVS row at 60 FPS interpolated coordinates
     * (cached when camera XY and level nodes have not changed). */
    extern const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y);
    static fixed_t last_ssec_vx = (fixed_t)0x80000000u;
    static fixed_t last_ssec_vy = (fixed_t)0x80000000u;
    static const node_t *last_ssec_nodes = NULL;
    static subsector_t *last_ssec = NULL;
    static const byte *last_pvs_row = NULL;
    subsector_t *ssec;
    if (hw_viewx == last_ssec_vx && hw_viewy == last_ssec_vy && nodes == last_ssec_nodes && last_ssec != NULL)
    {
        ssec = last_ssec;
        pvs_row = last_pvs_row;
    }
    else
    {
        last_ssec_vx = hw_viewx;
        last_ssec_vy = hw_viewy;
        last_ssec_nodes = nodes;
        ssec = R_PointInSubsector(hw_viewx, hw_viewy);
        pvs_row = Saturn_Pvs_GetRow(hw_viewx, hw_viewy);
        last_ssec = ssec;
        last_pvs_row = pvs_row;
    }
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
        /* Apply 1.125x scaling to align with the 3D FOV (1.078x) */
        hw_bbox_Lx = (hw_viewsin + hw_viewcos + (hw_viewcos >> 3)) >> 4;
        hw_bbox_Ly = (hw_viewsin - hw_viewcos + (hw_viewsin >> 3)) >> 4;
        hw_bbox_Rx = (hw_viewcos - hw_viewsin + (hw_viewcos >> 3)) >> 4;
        hw_bbox_Ry = (hw_viewsin + hw_viewcos + (hw_viewsin >> 3)) >> 4;

        hw_bbox_Cn = -((vx_m * hw_bbox_Nx) + (vy_m * hw_bbox_Ny));
        hw_bbox_Cl = -((vx_m * hw_bbox_Lx) + (vy_m * hw_bbox_Ly));
        hw_bbox_Cr = -((vx_m * hw_bbox_Rx) + (vy_m * hw_bbox_Ry));
        hw_frust.cz_off12 = -(hw_frust.cz_origin >> 4);
        hw_frust.cx_off12 = -(hw_frust.cx_origin >> 4);

        hw_bbox_idx_Nx = (hw_viewcos > 0) ? (BOXRIGHT * 2) : (BOXLEFT * 2);
        hw_bbox_idx_Ny = (hw_viewsin > 0) ? (BOXTOP * 2)   : (BOXBOTTOM * 2);
        hw_bbox_idx_Lx = (hw_bbox_Lx > 0) ? (BOXRIGHT * 2) : (BOXLEFT * 2);
        hw_bbox_idx_Ly = (hw_bbox_Ly > 0) ? (BOXTOP * 2)   : (BOXBOTTOM * 2);
        hw_bbox_idx_Rx = (hw_bbox_Rx > 0) ? (BOXRIGHT * 2) : (BOXLEFT * 2);
        hw_bbox_idx_Ry = (hw_bbox_Ry > 0) ? (BOXTOP * 2)   : (BOXBOTTOM * 2);

        hw_bbox_nidx_Nx = (hw_bbox_idx_Nx == (BOXRIGHT * 2)) ? (BOXLEFT * 2)   : (BOXRIGHT * 2);
        hw_bbox_nidx_Ny = (hw_bbox_idx_Ny == (BOXTOP * 2))   ? (BOXBOTTOM * 2) : (BOXTOP * 2);
        hw_bbox_nidx_Lx = (hw_bbox_idx_Lx == (BOXRIGHT * 2)) ? (BOXLEFT * 2)   : (BOXRIGHT * 2);
        hw_bbox_nidx_Ly = (hw_bbox_idx_Ly == (BOXTOP * 2))   ? (BOXBOTTOM * 2) : (BOXTOP * 2);
        hw_bbox_nidx_Rx = (hw_bbox_idx_Rx == (BOXRIGHT * 2)) ? (BOXLEFT * 2)   : (BOXRIGHT * 2);
        hw_bbox_nidx_Ry = (hw_bbox_idx_Ry == (BOXTOP * 2))   ? (BOXBOTTOM * 2) : (BOXTOP * 2);
    }

    /* Reset VDP1 quad table and clip buffers */
    Saturn_Vdp1Wall_Reset();
    Saturn_CmdBuf_Reset();
    {
        static const uint16_t *last_flat_q_ptr = NULL;
        if (__builtin_expect(saturn_baked_flat_q_count != last_flat_q_ptr, 0))
        {
            last_flat_q_ptr = saturn_baked_flat_q_count;
            HW_APERTURE->last_pvs_numnodes = -1;
            HW_SubFlatInfoEnsure();
            if (saturn_baked_flat_q_count && numsubsectors > 0)
            {
                for (int s = 0; s < numsubsectors; s++)
                {
                    hw_sub_flat_info_t *fi = &HW_SUB_FLAT_INFO[s];
                    fi->q_first      = saturn_baked_flat_q_first      ? saturn_baked_flat_q_first[s]      : 0;
                    fi->v_first      = saturn_baked_flat_v_first      ? saturn_baked_flat_v_first[s]      : 0;
                    fi->q_count      = saturn_baked_flat_q_count      ? saturn_baked_flat_q_count[s]      : 0;
                    fi->v_count      = saturn_baked_flat_v_count      ? saturn_baked_flat_v_count[s]      : 0;
                    fi->lod1_q_first = saturn_baked_flat_lod1_q_first ? saturn_baked_flat_lod1_q_first[s] : 0;
                    fi->lod1_v_first = saturn_baked_flat_lod1_v_first ? saturn_baked_flat_lod1_v_first[s] : 0;
                    fi->lod1_q_count = saturn_baked_flat_lod1_q_count ? saturn_baked_flat_lod1_q_count[s] : 0;
                    fi->lod1_v_count = saturn_baked_flat_lod1_v_count ? saturn_baked_flat_lod1_v_count[s] : 0;
                }
            }
        }
    }
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
    Saturn_Vdp1_WarpNewFrame();
    HW_WarpService();       /* textures the flat workers asked for last frame */
    {
        static int16_t last_ap_vh = -1;
        hw_aperture_state_t *ap = HW_APERTURE;
        const int vw = saturn_view_w;
        const int16_t vh = (int16_t)saturn_view_h;
        const uint32_t vh_pair = ((uint32_t)(uint16_t)vh << 16) | (uint16_t)vh;
        uint32_t dirty = ap->dirty_words;
        if (__builtin_expect(vh != last_ap_vh, 0))
        {
            dirty = 0xFFFFFFFFu;
            last_ap_vh = vh;
        }
        if (dirty != 0)
        {
            ap->dirty_words = 0;
            int n_words = (vw + 31) >> 5;
            uint32_t yb4 = (uint32_t)(uint8_t)vh * 0x01010101u;
            for (int w = 0; w < n_words; w++)
            {
                if (!(dirty & hw_bit_lut[w]))
                    continue;
#if HW_AP_HALF
                int x_w0 = w << 4;      /* column pairs */
                aliased_uint32_t *yt32 = (aliased_uint32_t *)&ap->ytop[x_w0];
                aliased_uint32_t *yb32 = (aliased_uint32_t *)&ap->ybot[x_w0];
                yt32[0] = 0; yt32[1] = 0; yt32[2] = 0; yt32[3] = 0;
                yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
#else
                int x_w0 = w << 5;
                aliased_uint32_t *yt32 = (aliased_uint32_t *)&ap->ytop[x_w0];
                aliased_uint32_t *yb32 = (aliased_uint32_t *)&ap->ybot[x_w0];
                yt32[0] = 0; yt32[1] = 0; yt32[2] = 0; yt32[3] = 0;
                yt32[4] = 0; yt32[5] = 0; yt32[6] = 0; yt32[7] = 0;
                yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
                yb32[4] = yb4; yb32[5] = yb4; yb32[6] = yb4; yb32[7] = yb4;
#endif
                hw_ap_words[w].yt_min = 0;
                hw_ap_words[w].yb_max = vh;
                hw_ap_words[w].yt_max = 0;
                hw_ap_words[w].yb_min = vh;
            }
        }
        int n_ap_sec = (numsectors < HW_MAX_AP_SECTORS) ? numsectors : HW_MAX_AP_SECTORS;
        const fixed_t vz = hw_viewz;
        const sector_t *s_in = sectors;
        hw_sec_fast_t *s_out = hw_sec_fast;
        for (int i = 0; i < n_ap_sec; i++, s_in++, s_out++)
        {
            s_out->dz_ceil    = s_in->ceilingheight - vz;
            s_out->dz_floor   = s_in->floorheight - vz;
            s_out->yt_attach  = vh;
            s_out->yb_attach  = 0;
            s_out->is_sky     = (uint8_t)((int16_t)s_in->ceilingpic == -1);
            s_out->has_things = (uint8_t)(s_in->thinglist != NULL);
            s_out->light_delta = (int16_t)(HW_SecLight16(s_in->lightlevel) - (int)hw_sec_light0[i]);
        }
        if (hw_sec_nb)
        {
            /* sky sectors: highest drawable point from the current heights
             * (lower-wall tops = higher neighbour floors; an upper wall to a
             * lower non-sky ceiling, a one-sided/masked wall or things in the
             * sector reach the ceiling) */
            for (int i = 0; i < n_ap_sec; i++)
            {
                const hw_sec_fast_t *sf = &hw_sec_fast[i];
                if (!sf->is_sky)
                    continue;
                fixed_t top = sf->dz_ceil;
                if (hw_sec_ztop[i] != HW_SEC_ZTOP_CEIL && !sf->has_things)
                {
                    top = sf->dz_floor;
                    for (int k = hw_sec_nb_first[i], e = hw_sec_nb_first[i + 1]; k < e; k++)
                    {
                        const hw_sec_fast_t *bf = &hw_sec_fast[hw_sec_nb[k]];
                        if (!bf->is_sky && bf->dz_ceil < sf->dz_ceil)
                        {
                            top = sf->dz_ceil;
                            break;
                        }
                        if (bf->dz_floor > top)
                            top = bf->dz_floor;
                    }
                    if (top > sf->dz_ceil)
                        top = sf->dz_ceil;
                }
                hw_sec_dz_top[i] = top;
            }
        }
        hw_psec_fast = (hw_psec && (unsigned)(hw_psec - sectors) < HW_MAX_AP_SECTORS)
                     ? &hw_sec_fast[hw_psec - sectors]
                     : NULL;
        ap->portal_span_count = 0;
        ap->aperture_active = 0;
    }
    {
        int sec_words = ((numsectors > 512 ? 512 : numsectors) + 31) >> 5;
        for (int i = 0; i < sec_words; i++)
            hw_sec_things_built[i] = 0;
        int sub_words = ((numsubsectors > HW_MAX_SUBSECTORS ? HW_MAX_SUBSECTORS : numsubsectors) + 31) >> 5;
        for (int i = 0; i < sub_words; i++)
            hw_sub_has_things[i] = 0;
        hw_thing_pool_count = 0;
    }
#if HW_DEFER_WALLS
    hw_defer_nsub = 0;
    hw_defer_nrec = 0;
    hw_defer_nsnap = 0;
    hw_defer_on = 0;
    hw_defer_tex_seen = -1;
#endif
    hw_render_frame++;
    if (hw_render_frame == 0)
        hw_render_frame = 1;
    hw_ad_rendered = 1;             /* adaptive detail: a 3D frame */
#if HW_SSMAP_DBG
    for (int i = 0; i < 256; i++) { hw_ssmap_last[i] = hw_ssmap_dbg[i]; hw_ssmap_dbg[i] = 0; }
#endif
    hw_fc = &hw_fsc_none;       /* per-frame sector cache: nothing active, */
    if (++hw_fsc_gen == 0)      /* every slot invalid */
        hw_fsc_gen = 1;
    hw_submax_tot = 0;
    hw_submax_num = -1;
    hw_prof_segs_acc = 0;
    hw_prof_ap_acc = 0;
    hw_prof_fsub_acc = 0;
    hw_prof_spr_acc = 0;
    hw_prof_occl_acc = 0;
    hw_prof_wseg_acc = 0;
    hw_prof_wquad_acc = 0;
    hw_prof_sportal_acc = 0;
    hw_prof_unb_acc = 0;
    saturn_hw_prof_mflat = 0;
    saturn_hw_prof_flats = 0;
    saturn_hw_prof_bg = 0;
    saturn_hw_wall_quads = 0;
    saturn_hw_flat_quads = 0;
    saturn_hw_slave_timeout = 0;
    saturn_hw_prof_slave_wait = 0;
#ifdef SATURN_BENCH
    extern volatile uint32_t saturn_slave_busy_ticks;
    extern uint32_t saturn_hw_prof_slave_busy;
    uint32_t sbusy0 = *(volatile uint32_t *)((uint32_t)&saturn_slave_busy_ticks | 0x20000000u);
    saturn_hw_prof_slave_busy = 0;
#endif
    saturn_hw_bsp_abort = 0;
    saturn_hw_occl_culls = 0;
    saturn_hw_seg_iter = 0;
    saturn_hw_seg_vis = 0;
    saturn_hw_tex_miss = 0;
    saturn_hw_wall_strips = 0;
    saturn_hw_flat_lod0_subs = 0;
    saturn_hw_flat_lod1_subs = 0;
    saturn_hw_flat_lod_saved_quads = 0;

    /* Queue player weapon directly into saturn_cmd_w */
    if (!HW_WALLS_ONLY)
        HW_RenderPlayerWeapon();

    /* raster-hole backing polygons only when the sky shows (saturn_hw_flats.c) */
    hw_hole_backing_on = saturn_hw_sky_visible;     /* last frame's value */
    saturn_hw_sky_visible = (hw_psec && ((int16_t)hw_psec->ceilingpic == -1));

    /* Apply Potentially Visible Set (PVS) for player's current leaf position.
     * Note: Keyed on `ssec` (unique subsector pointer), NOT `pvs_row`, because
     * Saturn_Pvs_GetRow() on Saturn returns a static cache buffer address. */
    hw_cur_pvs_row = pvs_row;
    const void *ssec_key = (pvs_row && ssec) ? (const void *)ssec : (const void *)1;
    if (__builtin_expect(ssec_key != HW_APERTURE->last_pvs_ssec ||
                         numnodes != HW_APERTURE->last_pvs_numnodes, 0))
    {
        HW_UpdateNodePvsVis(pvs_row, ssec_key);
    }

#ifndef SATURN_WIN32
    extern unsigned char saturn_slave_enabled;
    extern void jo_core_exec_on_slave(void (*callback)(void));
    extern void jo_core_wait_for_slave(void);

    hw_frame_slave_enabled = saturn_slave_enabled;
    hw_cur_sub_ranges = hw_frame_slave_enabled
        ? (sub_range_t *)((uint32_t)saturn_sub_ranges | SATURN_WRAM_H_UNCACHED)
        : saturn_sub_ranges;
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

#if HW_FLAT_STEAL
        HW_StealBegin(ipc);
#endif
        ipc->viewx = hw_viewx;
        ipc->viewy = hw_viewy;
        ipc->viewz = hw_viewz;
        ipc->viewcos = hw_viewcos;
        ipc->viewsin = hw_viewsin;
        ipc->cam_cz_origin = hw_frust.cz_origin;
        ipc->cam_cx_origin = hw_frust.cx_origin;
        ipc->psec_ceilingpic = hw_psec ? (int16_t)hw_psec->ceilingpic : -1;
        ipc->view_w  = (int16_t)HW_VIEW_W;
        ipc->view_cx = (int16_t)HW_VIEW_CX;
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
    hw_cur_sub_ranges = saturn_sub_ranges;
#endif
    HW_SlaveWalls_BeginFrame(hw_frame_slave_enabled);   /* HW_SLAVE_WALLS */

    /* Walk BSP tree and emit visible walls, flats & sprites in exact topological order */
    extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
    extern volatile unsigned int saturn_vbl_count;
    unsigned short tb0;
    unsigned int vb0;
    do {
        vb0 = saturn_vbl_count;
        tb0 = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != vb0, 0));
#if HW_VIS_STATS
    HW_VS(0);
    for (int s = 0; s < (int)sizeof(hw_vis_ssmap); s++)
        hw_vis_ssmap[s] = 0;
    {
        unsigned int n = 0;
        for (int s = 0; s < numsubsectors; s++)
            n += (!pvs_row || (pvs_row[s >> 3] & hw_bit_lut[s & 7])) ? 1u : 0u;
        hw_vis_stats[1] += n;
    }
#endif
#if HW_ORACLE
    if (!hw_oracle_on && ++hw_oracle_frame >= HW_ORACLE_START && hw_oracle_rec_n > 0)
    {
        hw_oracle_n = hw_oracle_rec_n;
        hw_oracle_on = 1;
    }
    if (hw_oracle_on)
    {
        for (int i = 0; i < hw_oracle_n; i++)
        {
            hw_oracle_cur = &hw_oracle_list[i];
            HW_RenderSubsector(hw_oracle_cur->subnum, hw_oracle_cur->fully);
        }
    }
    else
    {
        hw_oracle_rec_n = 0;
        HW_RenderBSPNode((int)numnodes - 1, pvs_row, 0);
    }
#else
    HW_CCR_SET(HW_CCR_WALK);
    HW_RenderBSPNode((int)numnodes - 1, pvs_row, 0);
#endif
    HW_EmitDeferredWalls();         /* pass 2: wall quads + sprites */
    HW_CCR_SET(0x01u);
    saturn_hw_prof_segs = hw_prof_segs_acc;
    saturn_hw_prof_ap = hw_prof_ap_acc;
    saturn_hw_prof_fsub = hw_prof_fsub_acc;
    saturn_hw_prof_spr = hw_prof_spr_acc;
    saturn_hw_prof_occl = hw_prof_occl_acc;
    saturn_hw_prof_wseg = hw_prof_wseg_acc;
    saturn_hw_prof_wquad = hw_prof_wquad_acc;
    saturn_hw_prof_sportal = hw_prof_sportal_acc;
    saturn_hw_prof_unb = hw_prof_unb_acc;
    saturn_hw_prof_bsp = Saturn_ProfSpan(tb0, vb0);
    saturn_hw_submax_tot  = hw_submax_tot;
    saturn_hw_submax_num  = hw_submax_num;
    saturn_hw_submax_ap   = hw_submax_parts[0];
    saturn_hw_submax_fl   = hw_submax_parts[1];
    saturn_hw_submax_wall = hw_submax_parts[2];
    saturn_hw_submax_spr  = hw_submax_parts[3];
    saturn_hw_submax_occ  = hw_submax_parts[4];

    static int saturn_hw_sky_was_visible = 0;
    int bg_drawn = 0;
#ifndef SATURN_WIN32
    HW_SkyHwBegin();   /* VDP2 sky: per-frame scroll value, no fill */
#endif

#ifndef SATURN_WIN32
    if (hw_frame_slave_enabled)
    {
        volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

        /* Overlap Master deferred flat overflow & background sky rendering with Slave SH-2 flat queue processing */
        if (hw_def_flat_count > 0)
        {
            unsigned short tmf0 = SAT_FRT();
            for (int dj = 0; dj < hw_def_flat_count; dj++)
            {
                HW_Master_ProcessJob(ipc, &hw_def_flat_jobs[dj]);
            }
            hw_def_flat_count = 0;
            saturn_hw_prof_mflat = HW_ProfDelta(tmf0, SAT_FRT());
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

#if HW_FLAT_STEAL
        HW_StealFlatJobs(ipc);   /* take floor jobs from the back instead of spinning */
#endif
        unsigned short twait0;
        unsigned int vwait0;
        do {
            vwait0 = saturn_vbl_count;
            twait0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != vwait0, 0));
        int wait_guard = 1000000; /* x 64 cycles: same timeout as before */
        while (1)
        {
            /* job_head is the slave's 32-bit store; with the ring in Low
             * WRAM (16-bit bus) a read can catch it half-written at a
             * 0xFFFF->0x10000 carry and end the wait while jobs remain
             * (2026-11-21). Only trust two equal reads. */
            uint32_t h1 = ipc->job_head, h2 = ipc->job_head;
            if (h1 == h2 && (int)(ipc->job_tail - h1) <= 0)
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
#if HW_FLAT_STEAL
        if (wait_guard > 0)
            HW_StealFinish(ipc);
#endif
        saturn_hw_prof_slave_wait = Saturn_ProfSpan(twait0, vwait0);
#ifdef SATURN_BENCH
        saturn_hw_prof_slave_busy =
            *(volatile uint32_t *)((uint32_t)&saturn_slave_busy_ticks | 0x20000000u) - sbusy0;
#endif

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
    HW_SlaveWalls_Finish();     /* walls neither CPU took yet (HW_SLAVE_WALLS) */
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

static HW_NOINLINE HW_COLD_OS uint32_t HW_SliceFlatQuadV(
    int y0, int y1, int y2, int y3, uint32_t quad_size)
{
    const int y_top = -2;
    const int y_bot = HW_VIEW_H + 8;
    const int vcy   = HW_VIEW_CY;
    int m_top = (y0 < y_top) | ((y1 < y_top) << 1) | ((y2 < y_top) << 2) | ((y3 < y_top) << 3);
    int m_bot = (y0 > y_bot) | ((y1 > y_bot) << 1) | ((y2 > y_bot) << 2) | ((y3 > y_bot) << 3);

    int m_v = 0, d_clip = 0, d_in = 0, d_out = 0;
    if ((m_top == 3 || m_top == 12) && m_bot == 0)
    {
        m_v = m_top;
        d_clip = vcy - y_top;
        int y_01 = (y0 + y1) >> 1;
        int y_23 = (y2 + y3) >> 1;
        d_out = vcy - ((m_v == 3) ? y_01 : y_23);
        d_in  = vcy - ((m_v == 3) ? y_23 : y_01);
    }
    else if ((m_bot == 3 || m_bot == 12) && m_top == 0)
    {
        m_v = m_bot;
        d_clip = y_bot - vcy;
        int y_01 = (y0 + y1) >> 1;
        int y_23 = (y2 + y3) >> 1;
        d_out = ((m_v == 3) ? y_01 : y_23) - vcy;
        d_in  = ((m_v == 3) ? y_23 : y_01) - vcy;
    }

    if (m_v != 0 && d_in > 0 && d_clip > d_in && d_out > d_clip)
    {
        unsigned int num = (unsigned int)(d_clip - d_in) * (unsigned int)d_out;
        unsigned int den = (unsigned int)(d_out - d_in) * (unsigned int)d_clip;
        fixed_t t_keep = HW_ClampedFrac(num, den);
        int base_v0   = (int)((quad_size >> 16) & 0x7Fu);
        int quad_rows = (int)(quad_size & 0xFFu);
        int new_rows  = ((quad_rows * (t_keep >> 8)) + 128) >> 8;
        if (new_rows < 1) new_rows = 1;
        else if (new_rows > quad_rows) new_rows = quad_rows;
        if (m_v == 12)
            base_v0 += (quad_rows - new_rows);
        quad_size = (quad_size & ~0x007F00FFu) | ((uint32_t)base_v0 << 16) | (uint32_t)new_rows;
    }
    return quad_size;
}
