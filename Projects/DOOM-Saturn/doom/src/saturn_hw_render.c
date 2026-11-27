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
#ifndef HW_WALK2_TU             /* level setup and the sky are master-only */
#include "saturn_hw_sky.c"
#endif
#include "saturn_hw_sprites.c"
#include "saturn_hw_flats.c"
#include "saturn_hw_walls.c"
#include "saturn_hw_coarse.c"
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
_Static_assert(CMDBUF_MAX_VISIT == HW_ASM_VISIT_CAP, "sub9");
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
 * [36] &hw_cur_sub_ranges [40] &hw_flat_ring [44] &sectors
 * [48] HW_SubmitSlaveFlatJob [52] HW_RenderSubsectorBakedFlats
 * [56] HW_SubsectorHasThings [60] HW_RenderSubsectorSprites
 * [64] &sub_solid_active [68] &hw_aperture_state [72] commit
 * [76] hw_asm_subw_ctx [80] sizeof(sector_t) */
const void *const hw_asm_sub_ctx[21] = {
    &hw_solid_count, &numsubsectors, &subsectors, &saturn_visit_count,
    hw_sec_fast, (const void *)HW_CheckSubsectorAperture, &saturn_hw_occl_culls,
    &saturn_hw_sky_visible, (const void *)&saturn_cmd_m_count, &hw_cur_sub_ranges,
    &hw_flat_ring, &sectors, (const void *)HW_SUBMIT_FLAT_JOB,
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
/* HW_TINY_SUB=N (2026-11-21 "do less work" study, off by default): a
 * subsector whose whole screen footprint is at most N pixels skips its walls
 * and floor job and draws one flat-colour polygon over its footprint
 * instead (sprites still draw). The harness sweep showed 14-19 of the 40
 * drawn courtyard subsectors cover <= 64-256 px - ~1% of the picture - yet
 * each paid the full per-subsector cost. Footprint = the projected bbox
 * columns x the sector's floor..ceiling at the nearest depth, cut to the
 * aperture window: a superset of everything the subsector draws. No
 * occlusion is recorded for it (farther geometry still renders; the
 * polygon, delivered later as the nearer visit, covers it). */
#ifndef HW_TINY_SUB
#ifdef SATURN_WIN32
#define HW_TINY_SUB 1           /* harness: compiled in, threshold from env HW_TINY_SUB_PX (0 = off) */
#else
#define HW_TINY_SUB 0
#endif
#endif
#if HW_TINY_SUB
#ifdef SATURN_WIN32
static int hw_tiny_sub_px = -1;
#else
static int hw_tiny_sub_px = HW_TINY_SUB;     /* stand-in footprint threshold (px) */
#endif
#endif

/* HW_FINE_CULL (2026-11-27, lossless): an area that passed the aperture test
 * but whose content cannot reach any open pixel. Its content splits into a
 * TOP block (ceiling + upper wall faces: rows [0, top_bot)) and a BOTTOM
 * block (floor + lower wall faces: rows [bot_top, vh)); between them only
 * its two-sided openings, which show OTHER areas. If no open column of its
 * projected x-span has a window reaching into either block, nothing of it
 * can show (and its walls, being outside every open window, record nothing
 * that matters). Not applied to areas with a one-sided wall or a masked
 * middle texture (full height), with things in the sector, or near the
 * camera. Harness census 2026-11-26: e49a 92 of 202 visits ended with zero
 * pixels (44 drew nothing, 48 were painted over by nearer walls/floors). */
#ifndef HW_FLAT_LOD1_ALL
/* every floor on 128-unit cells (2026-11-27, user: "Coarse floors + split"):
 * 5thPlanet FULL detail with HW_WALK2 e42a 63->66, e43a 49->51, e49a
 * 37->40, m6c 63->65 frames/240 fields; picture ~identical. */
#define HW_FLAT_LOD1_ALL 1      /* harness: env HW_FLAT_LOD1_ALL=1 turns it on */
#endif
#ifndef HW_FINE_CULL
#define HW_FINE_CULL 0     /* safe (no wrong culls) but culled only 0-10 of 82-207 visits (2026-11-27): off */
#endif
#if HW_FINE_CULL
unsigned int saturn_hw_fine_culls;     /* per frame */
static int hw_fine_on = 1;             /* harness env HW_FINE_CULL=0 for A/B */
static HW_NOINLINE int HW_FineCull(int subnum, const hw_sec_fast_t *sf)
{
    const hw_aperture_state_t *ap = HW_APERTURE;
    /* (a sky ceiling is not drawn per area - the VDP2 sky shows wherever no
     * quad is; the caller still switches it on for a culled sky area) */
#ifdef SATURN_WIN32
    extern int hw_trace_sub_on;
    if (hw_trace_sub_on)
        printf("[FINEC] sub %d ap %d things %d sky %d\n", subnum, ap->aperture_active, sf->has_things, sf->is_sky);
#endif
    if (!ap->aperture_active || !saturn_baked_flat_bboxes ||
        !saturn_baked_walls_active || (unsigned)subnum >= (unsigned)saturn_baked_wall_num_subsectors)
        return 0;
    {
        int secnum = subsectors[subnum].sector;
        sector_t *sec = &sectors[secnum];
        if (SATURN_V2P_SKIP_FLOOR(sec))
            return 0;                       /* VDP2 plane floor: its punch matters */
        /* a monster / item in THIS area: its sprite can show anywhere */
        if (sf->has_things && HW_SubsectorHasThings(sec, (uint32_t)secnum, subnum))
            return 0;
    }
    /* the area's walls: highest back floor / lowest back ceiling */
    const saturn_wl_subsector_t *ss = &saturn_baked_wall_subsectors[subnum];
    const baked_seg_t *seg = &saturn_baked_wall_segs[ss->seg_start];
    const int32_t dzf = sf->dz_floor, dzc = sf->dz_ceil;
    int32_t maxbf = dzf, minbc = dzc;
    const saturn_wl_pillar_t *pil = &saturn_baked_wall_pillars[ss->pillar_start];
    for (int k = 0; k < (int)ss->num_segs; k++, seg++)
    {
        if (!seg->flags)
            continue;                       /* nothing to draw on this seg */
        int b = seg->back_sec_idx;
        if (b < 0 || (unsigned)b >= HW_MAX_AP_SECTORS || (seg->flags & SEG_FLAG_MID))
        {
            /* full-height content - unless it faces away (never drawn) */
            const saturn_wl_pillar_t *p0 = &pil[seg->pillar_idx[0]], *p1 = &pil[seg->pillar_idx[1]];
            int odx = (int)p0->x - hw_cam_x, ody = (int)p0->z - hw_cam_y;
            if (HW_CrossBackface(ody, (int)p1->x - (int)p0->x, odx, (int)p1->z - (int)p0->z) > 0)
            {
#ifdef SATURN_WIN32
                if (hw_trace_sub_on) printf("[FINEC]   front-facing full-height seg k=%d back %d flags %x\n", k, b, seg->flags);
#endif
                return 0;
            }
            continue;
        }
        const hw_sec_fast_t *bs = &hw_sec_fast[b];
        if (bs->dz_floor > maxbf) maxbf = bs->dz_floor;
        if (bs->dz_ceil < minbc) minbc = bs->dz_ceil;
    }
    /* projected bbox: x-span and the nearest / farthest scales */
    const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
    int dx0 = fb->min_x - hw_cam_x, dx1 = fb->max_x - hw_cam_x;
    int dy0 = fb->min_y - hw_cam_y, dy1 = fb->max_y - hw_cam_y;
    int nx = hw_bbox_Nx, ny = hw_bbox_Ny;
    int32_t cz_off = hw_frust.cz_off12, cx_off = hw_frust.cx_off12;
    int x_cz_min = (nx >= 0) ? dx0 : dx1, y_cz_min = (ny >= 0) ? dy0 : dy1;
    int x_cz_max = (nx >= 0) ? dx1 : dx0, y_cz_max = (ny >= 0) ? dy1 : dy0;
    fixed_t min_cz = HW_MulsW_PairAdd(x_cz_min, nx, y_cz_min, ny, cz_off) << 4;
    fixed_t max_cz = HW_MulsW_PairAdd(x_cz_max, nx, y_cz_max, ny, cz_off) << 4;
    if (min_cz < (128 << FRACBITS))
        return 0;
    int x_cx_min = (ny >= 0) ? dx0 : dx1, x_cx_max = (ny >= 0) ? dx1 : dx0;
    int y_cx_min = (nx >= 0) ? dy1 : dy0, y_cx_max = (nx >= 0) ? dy0 : dy1;
    fixed_t min_cx = HW_MulsW_PairAdd(x_cx_min, ny, y_cx_min, -nx, cx_off) << 4;
    fixed_t max_cx = HW_MulsW_PairAdd(x_cx_max, ny, y_cx_max, -nx, cx_off) << 4;
    int s_near = HW_FastDiv(min_cz), s_far = HW_FastDiv(max_cz);
    const int vcy = HW_VIEW_CY, vh = HW_VIEW_H;
    /* bottom block: floor (seen from above) + lower walls up to maxbf */
    int bot_top = vh;
    if (dzf < 0)
    {
        int y = HW_ProjY(dzf, s_far, vcy);            /* the floor's top row is at its far edge */
        if (y < bot_top) bot_top = y;
    }
    if (maxbf > dzf)
    {
        int y1 = HW_ProjY(maxbf, s_near, vcy), y2 = HW_ProjY(maxbf, s_far, vcy);
        int y = (y1 < y2) ? y1 : y2;
        if (y < bot_top) bot_top = y;
    }
    /* top block: ceiling (seen from below) + upper walls down to minbc */
    int top_bot = 0;
    if (dzc > 0 && !sf->is_sky)
    {
        int y = HW_ProjY(dzc, s_far, vcy);
        if (y > top_bot) top_bot = y;
    }
    if (minbc < dzc)
    {
        int y1 = HW_ProjY(minbc, s_near, vcy), y2 = HW_ProjY(minbc, s_far, vcy);
        int y = (y1 > y2) ? y1 : y2;
        if (y > top_bot) top_bot = y;
    }
    bot_top -= 2;                           /* rounding margin */
    top_bot += 2;
    if (top_bot >= bot_top)
        return 0;                           /* the blocks meet: no gap to see through */
    int xa = HW_ProjX(min_cx, s_near, HW_VIEW_CX), xb = HW_ProjX(min_cx, s_far, HW_VIEW_CX);
    int xc = HW_ProjX(max_cx, s_near, HW_VIEW_CX), xd = HW_ProjX(max_cx, s_far, HW_VIEW_CX);
    int x0 = xa, x1 = xa;
    if (xb < x0) x0 = xb; if (xb > x1) x1 = xb;
    if (xc < x0) x0 = xc; if (xc > x1) x1 = xc;
    if (xd < x0) x0 = xd; if (xd > x1) x1 = xd;
    x0 -= 1; x1 += 2;
    if (x0 < 0) x0 = 0;
    if (x1 > HW_VIEW_W) x1 = HW_VIEW_W;
#ifdef SATURN_WIN32
    extern int hw_trace_sub_on;
    if (hw_trace_sub_on)
        printf("[FINEC] sub %d x %d..%d top_bot %d bot_top %d dzf %d dzc %d maxbf %d minbc %d s %d..%d\n",
               subnum, x0, x1, top_bot, bot_top, dzf >> 16, dzc >> 16, maxbf >> 16, minbc >> 16, s_far, s_near);
#endif
    for (int x = x0; x < x1; x++)
    {
        if (hw_solid_mask[x >> 5] & (1u << (x & 31)))
            continue;
        int t = ap->ytop[x], b = ap->ybot[x];
        if (t >= b)
            continue;
        if (t < top_bot || b > bot_top)
        {
#ifdef SATURN_WIN32
            if (hw_trace_sub_on)
                printf("[FINEC]   visible at col %d window %d..%d\n", x, t, b);
#endif
            return 0;                       /* the window reaches a block: may show */
        }
    }
    return 1;
}
#endif

#ifdef SATURN_WIN32
/* Harness diagnostic (2026-11-26, "most of the pixels in the distance are
 * overdraw"): per visited subsector, the pixels a tight per-column test
 * would still call open at visit time - its projected bbox columns that
 * are not solid, each cut to the column's aperture window and to the
 * sector floor..ceiling at the nearer depth. -1 = near (not tested).
 * Written to sweep\TAG.vis next to the true final pixels. */
int hw_diag_open[8192];
int hw_trace_sub_on;            /* TRACE_SUB=n: the subsector being rendered is n */
uint16_t hw_diag_spr0[1024];    /* per visit: first cmd_m index of its sprites */
int hw_diag_cols[8192];         /* columns the area's bbox spans (before solid/aperture) */
static int HW_DiagOpenArea(int subnum, const hw_sec_fast_t *sf, int *cols_out)
{
    const saturn_flat_bbox_t *fb;
    *cols_out = 0;
    if (!saturn_baked_flat_bboxes)
        return -1;
    fb = &saturn_baked_flat_bboxes[subnum];
    int dx0 = fb->min_x - hw_cam_x, dx1 = fb->max_x - hw_cam_x;
    int dy0 = fb->min_y - hw_cam_y, dy1 = fb->max_y - hw_cam_y;
    int nx = hw_bbox_Nx, ny = hw_bbox_Ny;
    int32_t cz_off = hw_frust.cz_off12, cx_off = hw_frust.cx_off12;
    int x_cz_min = (nx >= 0) ? dx0 : dx1, y_cz_min = (ny >= 0) ? dy0 : dy1;
    int x_cz_max = (nx >= 0) ? dx1 : dx0, y_cz_max = (ny >= 0) ? dy1 : dy0;
    fixed_t min_cz = HW_MulsW_PairAdd(x_cz_min, nx, y_cz_min, ny, cz_off) << 4;
    fixed_t max_cz = HW_MulsW_PairAdd(x_cz_max, nx, y_cz_max, ny, cz_off) << 4;
    if (min_cz < (64 << FRACBITS))
        return -1;
    int x_cx_min = (ny >= 0) ? dx0 : dx1, x_cx_max = (ny >= 0) ? dx1 : dx0;
    int y_cx_min = (nx >= 0) ? dy1 : dy0, y_cx_max = (nx >= 0) ? dy0 : dy1;
    fixed_t min_cx = HW_MulsW_PairAdd(x_cx_min, ny, y_cx_min, -nx, cx_off) << 4;
    fixed_t max_cx = HW_MulsW_PairAdd(x_cx_max, ny, y_cx_max, -nx, cx_off) << 4;
    int s_near = HW_FastDiv(min_cz), s_far = HW_FastDiv(max_cz);
    int xa = HW_ProjX(min_cx, s_near, HW_VIEW_CX), xb = HW_ProjX(min_cx, s_far, HW_VIEW_CX);
    int xc = HW_ProjX(max_cx, s_near, HW_VIEW_CX), xd = HW_ProjX(max_cx, s_far, HW_VIEW_CX);
    int x0 = xa, x1 = xa;
    if (xb < x0) x0 = xb; if (xb > x1) x1 = xb;
    if (xc < x0) x0 = xc; if (xc > x1) x1 = xc;
    if (xd < x0) x0 = xd; if (xd > x1) x1 = xd;
    x0 -= 1; x1 += 1;
    if (x0 < 0) x0 = 0;
    if (x1 > HW_VIEW_W) x1 = HW_VIEW_W;
    int yt = HW_ProjY(sf->dz_ceil, s_near, HW_VIEW_CY), yb = HW_ProjY(sf->dz_floor, s_near, HW_VIEW_CY);
    {
        int yt2 = HW_ProjY(sf->dz_ceil, s_far, HW_VIEW_CY), yb2 = HW_ProjY(sf->dz_floor, s_far, HW_VIEW_CY);
        if (yt2 < yt) yt = yt2;
        if (yb2 > yb) yb = yb2;
    }
    yt -= 1; yb += 1;
    if (yt < 0) yt = 0;
    if (yb > HW_VIEW_H) yb = HW_VIEW_H;
    if (x1 <= x0)
        return 0;
    *cols_out = x1 - x0;
    const hw_aperture_state_t *ap = HW_APERTURE;
    int active = ap->aperture_active, area = 0;
    for (int x = x0; x < x1; x++)
    {
        int t = yt, b = yb;
        if (hw_solid_mask[x >> 5] & (1u << (x & 31)))
            continue;
        if (active)
        {
            if (ap->ytop[x] > t) t = ap->ytop[x];
            if (ap->ybot[x] < b) b = ap->ybot[x];
        }
        if (b > t)
            area += b - t;
    }
    return area;
}
#endif
#if HW_TINY_SUB
unsigned int saturn_hw_tiny_subs;   /* per frame (bench / HUD) */
/* screen rectangle of a subsector; 0 = near / not small */
static HW_NOINLINE int HW_TinyRect(int subnum, const hw_sec_fast_t *sf, int *rx0, int *ry0, int *rx1, int *ry1,
                                   int *zb_out)
{
    const saturn_flat_bbox_t *fb;
    if (!saturn_baked_flat_bboxes)
        return 0;
    fb = &saturn_baked_flat_bboxes[subnum];
    int dx0 = fb->min_x - hw_cam_x, dx1 = fb->max_x - hw_cam_x;
    int dy0 = fb->min_y - hw_cam_y, dy1 = fb->max_y - hw_cam_y;
    int nx = hw_bbox_Nx, ny = hw_bbox_Ny;
    int32_t cz_off = hw_frust.cz_off12, cx_off = hw_frust.cx_off12;
    int x_cz_min = (nx >= 0) ? dx0 : dx1, y_cz_min = (ny >= 0) ? dy0 : dy1;
    int x_cz_max = (nx >= 0) ? dx1 : dx0, y_cz_max = (ny >= 0) ? dy1 : dy0;
    fixed_t min_cz = HW_MulsW_PairAdd(x_cz_min, nx, y_cz_min, ny, cz_off) << 4;
    fixed_t max_cz = HW_MulsW_PairAdd(x_cz_max, nx, y_cz_max, ny, cz_off) << 4;
    if (min_cz < (256 << FRACBITS))
        return 0;                       /* tiny only happens far away */
    int x_cx_min = (ny >= 0) ? dx0 : dx1, x_cx_max = (ny >= 0) ? dx1 : dx0;
    int y_cx_min = (nx >= 0) ? dy1 : dy0, y_cx_max = (nx >= 0) ? dy0 : dy1;
    fixed_t min_cx = HW_MulsW_PairAdd(x_cx_min, ny, y_cx_min, -nx, cx_off) << 4;
    fixed_t max_cx = HW_MulsW_PairAdd(x_cx_max, ny, y_cx_max, -nx, cx_off) << 4;
    int s_near = HW_FastDiv(min_cz);
    int s_far  = HW_FastDiv(max_cz);
    /* x extent: both cx extremes at both depths (conservative) */
    int xa = HW_ProjX(min_cx, s_near, HW_VIEW_CX), xb = HW_ProjX(min_cx, s_far, HW_VIEW_CX);
    int xc = HW_ProjX(max_cx, s_near, HW_VIEW_CX), xd = HW_ProjX(max_cx, s_far, HW_VIEW_CX);
    int x0 = xa, x1 = xa;
    if (xb < x0) x0 = xb; if (xb > x1) x1 = xb;
    if (xc < x0) x0 = xc; if (xc > x1) x1 = xc;
    if (xd < x0) x0 = xd; if (xd > x1) x1 = xd;
    x0 -= 1; x1 += 1;
    if (x0 < 0) x0 = 0;
    if (x1 > HW_VIEW_W) x1 = HW_VIEW_W;
    /* y extent: ceiling and floor at the nearest depth (largest) */
    int yt = HW_ProjY(sf->dz_ceil, s_near, HW_VIEW_CY);
    int yb = HW_ProjY(sf->dz_floor, s_near, HW_VIEW_CY);
    {
        int yt2 = HW_ProjY(sf->dz_ceil, s_far, HW_VIEW_CY);
        int yb2 = HW_ProjY(sf->dz_floor, s_far, HW_VIEW_CY);
        if (yt2 < yt) yt = yt2;
        if (yb2 > yb) yb = yb2;
    }
    yt -= 1; yb += 1;
    if (yt < 0) yt = 0;
    if (yb > HW_VIEW_H) yb = HW_VIEW_H;
    if (x1 <= x0 || yb <= yt)
        return 0;
    /* what can still show: columns not yet solid, each cut to its aperture
     * window (nearer walls cover the rest - they are delivered later) */
    {
        const hw_aperture_state_t *ap = HW_APERTURE;
        int active = ap->aperture_active;
        int area = 0, bx0 = HW_VIEW_W, bx1 = -1, by0 = HW_VIEW_H, by1 = -1, x;
        for (x = x0; x < x1; x++)
        {
            int t = yt, b = yb;
            if (hw_solid_mask[x >> 5] & (1u << (x & 31)))
                continue;
            if (active)
            {
                if (ap->ytop[x] > t) t = ap->ytop[x];
                if (ap->ybot[x] < b) b = ap->ybot[x];
            }
            if (b <= t)
                continue;
            area += b - t;
            if (area > hw_tiny_sub_px)
                return 0;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (t < by0) by0 = t;
            if (b > by1) by1 = b;
        }
        if (bx1 < bx0)
            return 0;               /* nothing open: the AP test let it through conservatively */
        *rx0 = bx0; *ry0 = by0; *rx1 = bx1 + 1; *ry1 = by1;
    }
    {
        unsigned int zb = (unsigned int)((min_cz + max_cz) >> 21);
        *zb_out = (int)(zb > 127u ? 127u : zb);
    }
    return 1;
}

/* the fill polygon: the visible flat's most common colour, wall shading */
static HW_NOINLINE void HW_TinyFill(const sector_t *sec, int x0, int y0, int x1, int y1, int zb)
{
    int pic = ((int16_t)sec->floorpic != -1) ? sec->floorpic : sec->ceilingpic;
    uint16_t m_cnt = saturn_cmd_m_count;
    int light_s, colr;
    if ((int16_t)pic == -1 || m_cnt >= CMDBUF_M_CAP - 1)
        return;
    light_s = ((sec->lightlevel >> 4) & 15) + hw_wall_light_delta;
    if (light_s < 0) light_s = 0;
    if (light_s > 15) light_s = 15;
    colr = (int)(wall_zlight_cmdcolr_LUT[light_s][light_s < 15 ? zb : 0] & 0xFF00u)
         | (Saturn_Vdp1_FlatAvgColor(pic) & 0xFF);
    aliased_uint32_t *c = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
    saturn_cmd_m_count = m_cnt + 1;
    c[0] = 0x00040000u;                              /* polygon */
    c[1] = (0x00C0u << 16) | (uint32_t)colr;         /* no user clip */
    c[2] = 0;
    c[3] = ((uint32_t)x0 << 16) | (uint16_t)y0;
    c[4] = ((uint32_t)(x1 - 1) << 16) | (uint16_t)y0;
    c[5] = ((uint32_t)(x1 - 1) << 16) | (uint16_t)(y1 - 1);
    c[6] = ((uint32_t)x0 << 16) | (uint16_t)(y1 - 1);
    c[7] = 0;
}
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
#if HW_FINE_CULL
#ifdef SATURN_WIN32
    {
        static int env_done = 0, tsub = -1;
        if (!env_done)
        {
            const char *e = getenv("HW_FINE_CULL"); env_done = 1; if (e) hw_fine_on = atoi(e);
            e = getenv("TRACE_SUB"); tsub = e ? atoi(e) : -1;
        }
        hw_trace_sub_on = (subnum == tsub);
    }
#endif
    if (hw_fine_on && HW_FineCull(subnum, sf))
    {
        saturn_hw_fine_culls++;
        if (sf->is_sky && (flat_flags & (int)HW_FLAT_ALLOW_CEIL))
            saturn_hw_sky_visible = 1;      /* as the full visit would (below) */
#ifdef SATURN_WIN32
        if (getenv("FINE_LOG"))
            printf("[FINE] cull sub %d\n", subnum);
#endif
        return;
    }
#endif
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
        static int tsub = -2;
        if (tsub == -2) { const char *e = getenv("TRACE_SUB"); tsub = e ? atoi(e) : -1; }
        hw_trace_sub_on = (subnum == tsub);
        if (hw_trace_sub_on)
            printf("[TSUB] visit %d sub %d flags %x ytop_min %d ybot_max %d att yt %d yb %d dzf %d dzc %d\n",
                   visit_idx, subnum, flat_flags, HW_APERTURE->cur_ytop_min, HW_APERTURE->cur_ybot_max,
                   HW_APERTURE->cur_sub_yt_attach, HW_APERTURE->cur_sub_yb_attach, sf->dz_floor >> FRACBITS, sf->dz_ceil >> FRACBITS);
    }
    if ((unsigned)subnum < 8192u)
        hw_diag_open[subnum] = HW_DiagOpenArea(subnum, sf, &hw_diag_cols[subnum]);
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

#if HW_TINY_SUB
    {
        int tx0, ty0, tx1, ty1, tzb;
#ifdef SATURN_WIN32
        if (hw_tiny_sub_px < 0)
        {
            const char *e = getenv("HW_TINY_SUB_PX");
            hw_tiny_sub_px = e ? atoi(e) : 0;
        }
#endif
        if (hw_tiny_sub_px > 0 && HW_TinyRect(subnum, sf, &tx0, &ty0, &tx1, &ty1, &tzb))
        {
            sector_t *sec = &sectors[secnum];
            saturn_hw_tiny_subs++;
            HW_TinyFill(sec, tx0, ty0, tx1, ty1, tzb);
            if (!HW_WALLS_ONLY && __builtin_expect(sf->has_things != 0, 0) &&
                HW_SubsectorHasThings(sec, (uint32_t)secnum, subnum))
                HW_RenderSubsectorSprites(sec, subnum);
#ifndef SATURN_WIN32
            r32[0] = ((uint32_t)m_start << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
#else
            ranges[visit_idx].m_start = m_start;
            ranges[visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);
#endif
            return;
        }
    }
#endif
#if HW_FLAT_LOD1_ALL
    /* 2026-11-27 test (user: "LOD more aggressive up close"): every floor /
     * ceiling uses the 128-unit cells (LOD1), near ones too - fewer floor
     * quads for the slave and VDP1, nothing left out (subsectors without
     * LOD1 data keep their 64-unit cells). Harness: env HW_FLAT_LOD1_ALL=0/1. */
#ifdef SATURN_WIN32
    {
        static int on = -1;
        if (on < 0) { const char *e = getenv("HW_FLAT_LOD1_ALL"); on = e ? atoi(e) : 0; }
        if (on)
            flat_flags |= (int)HW_FLAT_USE_LOD1;
    }
#else
    flat_flags |= (int)HW_FLAT_USE_LOD1;
#endif
#endif
    if (!HW_WALLS_ONLY && (flat_flags & (int)HW_FLAT_ALLOW_BOTH))
    {
        sector_t *sec = &sectors[secnum];
#ifndef SATURN_WIN32
        if (__builtin_expect(hw_flat_ring, 1))
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
        hw_defer_on = (hw_defer_nsub < hw_defer_sub_cap && hw_defer_nrec + ns <= hw_defer_rec_cap);
        hw_defer_need_snap = 0;
    }
#endif
    HW_RenderSubsectorBakedWalls(subnum, sf);
#if HW_DEFER_WALLS
    /* Everything this subsector emits after its walls (the V2P punch, the
     * sprites) is moved behind the walls in pass 2, in this order. */
    int defer_now = hw_defer_on;
    uint16_t spr_m0 = saturn_cmd_m_count;
    hw_defer_on = 0;
#endif
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
#ifdef SATURN_WIN32
    if (visit_idx < 1024)
        hw_diag_spr0[visit_idx] = saturn_cmd_m_count;   /* census: sprites start here */
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

/* HW_BATCH=N (2026-11-21 SH-2 cache study, off by default): the BSP walk
 * queues leaves and every N runs phase A (aperture check + floor job) as one
 * loop over the batch, then phase B (walls, sprites, occlusion commit) per
 * subsector in order. Each phase's code then misses once per batch instead of
 * once per subsector (offline I-cache replay: -5..-8 ms of code misses at the
 * E1M1 courtyard). Phase A tests against the occlusion state at batch start:
 * conservative (it may keep a subsector a fresh test would cull - more floor
 * work, never a missing pixel); walls and sprites in phase B see the exact
 * state, as before. Slave-less frames run unbatched (master floors share the
 * cmd_m range). */
#ifndef HW_BATCH
#define HW_BATCH 0
#endif
#if HW_BATCH
/* 1 = floor jobs after the batch's walls with exact per-subsector occlusion
 * snapshots (measured worse: the slave starts late and the master steals) */
#ifndef HW_BATCH_LATEFLATS
#define HW_BATCH_LATEFLATS 0
#endif
#if HW_ORACLE || HW_SUBMAX || HW_VIS_STATS || HW_SSMAP_DBG
#error "HW_BATCH: oracle / submax / vis-stats / ssmap paths are not batched"
#endif
typedef struct {
    int16_t  subnum, flags;        /* flags < 0: culled */
    int16_t  visit, fully;
    int16_t  yt_att, yb_att;
    int16_t  ytop_min, ybot_max;
    uint8_t  ytop, ybot;
    uint16_t m_start;
    int16_t  res_base, res_n;      /* chunk visit slots reserved after visit */
    int16_t  snap_count;           /* occlusion state just before this */
    uint32_t snap_summary, snap_any; /* subsector's commit (exact) */
    uint32_t snap[HW_SOLID_WORDS];
} hw_batch_slot_t;
static hw_batch_slot_t hw_batch_q[HW_BATCH];
static int hw_batch_n;
static uint32_t hw_batch_sent[HW_SOLID_WORDS];   /* mask the slave last got */
#if HW_FLAT_SPLIT && HW_FLAT_CHUNKS && !defined(SATURN_WIN32)
#define HW_BATCH_SPLIT 1
#else
#define HW_BATCH_SPLIT 0
#endif

/* phase A: aperture check + floor job; returns 0 when culled */
static HW_NOINLINE HW_HOT_O2 int HW_SubPhaseA(int subnum, int fully_inside, hw_batch_slot_t *q)
{
    q->flags = -1;
    if (hw_solid_count >= HW_VIEW_W || (unsigned)subnum >= (unsigned)numsubsectors)
        return 0;
    int secnum = subsectors[subnum].sector;
    if ((unsigned)secnum >= HW_MAX_AP_SECTORS || saturn_visit_count >= CMDBUF_MAX_VISIT)
        return 0;
    hw_sec_fast_t *sf = &hw_sec_fast[secnum];
    int flat_flags = HW_CheckSubsectorAperture(subnum, sf, fully_inside & 3);
    if (flat_flags < 0)
    {
        saturn_hw_occl_culls++;
        return 0;
    }
    if (sf->is_sky)
    {
        if (flat_flags & (int)HW_FLAT_ALLOW_CEIL)
            saturn_hw_sky_visible = 1;
        flat_flags &= ~(int)HW_FLAT_ALLOW_CEIL;
    }
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        q->yt_att   = ap->cur_sub_yt_attach;
        q->yb_att   = ap->cur_sub_yb_attach;
        q->ytop     = ap->cur_sub_ytop;
        q->ybot     = ap->cur_sub_ybot;
        q->ytop_min = ap->cur_ytop_min;
        q->ybot_max = ap->cur_ybot_max;
    }
    int visit_idx = saturn_visit_count++;
    q->subnum  = (int16_t)subnum;
    q->fully   = (int16_t)fully_inside;
    q->flags   = (int16_t)flat_flags;
    q->visit   = (int16_t)visit_idx;
    q->m_start = saturn_cmd_m_count;
    {
        sub_range_t *ranges = hw_cur_sub_ranges;
        aliased_uint32_t *r32 = (aliased_uint32_t *)&ranges[visit_idx];
        r32[1] = 0;
        ranges[visit_idx].subnum = (uint16_t)subnum;
    }
    q->res_base = 0;
    q->res_n = -1;                 /* -1: the floor job must not split */
#if HW_BATCH_SPLIT
    if (!HW_WALLS_ONLY && (flat_flags & (int)HW_FLAT_ALLOW_BOTH))
    {
        /* chunk jobs need visit slots right after this one (delivered
         * before it, so its walls land on top): reserve them now, as
         * empty ranges; unused ones stay empty */
        int n = HW_SplitChunkCount(subnum);
        if (n >= 2 && saturn_visit_count + n < CMDBUF_MAX_VISIT - 32)
        {
            sub_range_t *ranges = hw_cur_sub_ranges;
            q->res_base = (int16_t)saturn_visit_count;
            q->res_n = (int16_t)(n - 1);
            for (int k = 1; k < n; k++)
            {
                int vis = saturn_visit_count++;
                aliased_uint32_t *r32 = (aliased_uint32_t *)&ranges[vis];
                r32[0] = (uint32_t)saturn_cmd_m_count << 16;
                r32[1] = 0;
                ranges[vis].subnum = (uint16_t)subnum;
            }
        }
    }
#endif
    return 1;
}

static inline void HW_BatchRestoreAp(const hw_batch_slot_t *q)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    ap->cur_sub_yt_attach = q->yt_att;
    ap->cur_sub_yb_attach = q->yb_att;
    ap->cur_sub_ytop      = q->ytop;
    ap->cur_sub_ybot      = q->ybot;
    ap->cur_ytop_min      = q->ytop_min;
    ap->cur_ybot_max      = q->ybot_max;
}

/* phase F: the floor job (its own loop: the aperture code and the flat
 * submit together are too big to stay cached across a batch) */
static HW_NOINLINE HW_HOT_O2 void HW_SubPhaseFlat(const hw_batch_slot_t *q)
{
    int flat_flags = q->flags;
    if (HW_WALLS_ONLY || !(flat_flags & (int)HW_FLAT_ALLOW_BOTH))
        return;
    int subnum = q->subnum;
    sector_t *sec = &sectors[subsectors[subnum].sector];
    HW_BatchRestoreAp(q);
#if HW_BATCH_SPLIT
    hw_split_res_next = q->res_base;
    hw_split_res_left = q->res_n;
#endif
#ifndef SATURN_WIN32
    if (__builtin_expect(hw_flat_ring, 1))
        HW_SUBMIT_FLAT_JOB(q->visit, subnum, sec, flat_flags);
    else
#endif
        HW_RenderSubsectorBakedFlats(q->visit, subnum, sec, flat_flags);
#if HW_BATCH_SPLIT
    hw_split_res_left = 0;
#endif
}

/* Batched floor jobs go out after the batch's walls, each with the exact
 * occlusion state phase B saw just before that subsector's commit (the
 * slave culls hidden floor quads with it). Swaps that state in, keeps the
 * per-job dirty-word delta right, and restores the live state after. */
static HW_NOINLINE void HW_BatchFlats(int n)
{
    uint32_t live[HW_SOLID_WORDS];
    int live_count = hw_solid_count;
    uint32_t live_summary = hw_solid_summary, live_any = hw_solid_any;
    int i, w;
    for (w = 0; w < HW_SOLID_WORDS; w++)
        live[w] = hw_solid_mask[w];
    for (i = 0; i < n; i++)
    {
        const hw_batch_slot_t *q = &hw_batch_q[i];
        if (q->flags < 0 || !(q->flags & (int)HW_FLAT_ALLOW_BOTH))
            continue;
        uint32_t dirty = 0;
        for (w = 0; w < HW_SOLID_WORDS; w++)
        {
            hw_solid_mask[w] = q->snap[w];
            if (q->snap[w] != hw_batch_sent[w])
                dirty |= 1u << w;
        }
        hw_solid_count = q->snap_count;
        hw_solid_summary = q->snap_summary;
        hw_solid_any = q->snap_any;
        hw_slave_dirty_words = dirty;
        HW_SubPhaseFlat(q);
        for (w = 0; w < HW_SOLID_WORDS; w++)
            hw_batch_sent[w] = q->snap[w];
    }
    uint32_t dirty = 0;
    for (w = 0; w < HW_SOLID_WORDS; w++)
    {
        hw_solid_mask[w] = live[w];
        if (live[w] != hw_batch_sent[w])
            dirty |= 1u << w;
    }
    hw_solid_count = live_count;
    hw_solid_summary = live_summary;
    hw_solid_any = live_any;
    hw_slave_dirty_words = dirty;
}

/* phase B: walls, sprites, occlusion commit (in BSP order) */
static HW_NOINLINE HW_HOT_O2 void HW_SubPhaseB(hw_batch_slot_t *q, int batched)
{
    int subnum = q->subnum;
    int secnum = subsectors[subnum].sector;
    hw_sec_fast_t *sf = &hw_sec_fast[secnum];
    int visit_idx = q->visit;
    HW_BatchRestoreAp(q);
    uint16_t m_start = batched ? saturn_cmd_m_count : q->m_start;
    sub_range_t *ranges = hw_cur_sub_ranges;
    aliased_uint32_t *r32 = (aliased_uint32_t *)&ranges[visit_idx];
#if HW_DEFER_WALLS
    int defer_rec0 = hw_defer_nrec;
    {
        int ns = (saturn_baked_walls_active && (unsigned)subnum < (unsigned)saturn_baked_wall_num_subsectors)
               ? (int)saturn_baked_wall_subsectors[subnum].num_segs : 0;
        hw_defer_on = (hw_defer_nsub < hw_defer_sub_cap && hw_defer_nrec + ns <= hw_defer_rec_cap);
        hw_defer_need_snap = 0;
    }
#endif
    HW_RenderSubsectorBakedWalls(subnum, sf);
#if HW_DEFER_WALLS
    int defer_now = hw_defer_on;    /* punch + sprites follow the walls in pass 2 */
    uint16_t spr_m0 = saturn_cmd_m_count;
    hw_defer_on = 0;
#endif
#if HW_V2P_PUNCH
    if (__builtin_expect(SATURN_V2P_SKIP_FLOOR(&sectors[secnum]), 0) && HW_V2PlaneWanted(subnum))
        HW_V2PlanePunch(subnum, sf);
#endif
    if (!HW_WALLS_ONLY && __builtin_expect(sf->has_things != 0, 0))
    {
        sector_t *sec = &sectors[secnum];
        if (HW_SubsectorHasThings(sec, (uint32_t)secnum, subnum))
            HW_RenderSubsectorSprites(sec, subnum);
    }
#ifndef SATURN_WIN32
    r32[0] = ((uint32_t)m_start << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
#else
    ranges[visit_idx].m_start = m_start;
    ranges[visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);
#endif
#if HW_DEFER_WALLS
    if (defer_now)
        HW_DeferCloseSub(visit_idx, subnum, defer_rec0, spr_m0,
                         (int)(saturn_cmd_m_count - spr_m0), sf->light_delta);
#endif
    if (HW_BATCH_LATEFLATS && batched)
    {
        /* exact occlusion before this commit, for its floor job */
        for (int w = 0; w < HW_SOLID_WORDS; w++)
            q->snap[w] = hw_solid_mask[w];
        q->snap_count = (int16_t)hw_solid_count;
        q->snap_summary = hw_solid_summary;
        q->snap_any = hw_solid_any;
    }
    if (sub_solid_active || HW_APERTURE->portal_span_count > 0)
        HW_CommitSubsectorOcclusion();
}

static HW_NOINLINE void HW_BatchFlush(void)
{
    int i, n = hw_batch_n;
    hw_batch_n = 0;
    for (i = 0; i < n; i++)
        if (hw_batch_q[i].subnum >= 0 && !HW_SubPhaseA(hw_batch_q[i].subnum, hw_batch_q[i].fully, &hw_batch_q[i]))
            hw_batch_q[i].flags = -1;
#if HW_BATCH_LATEFLATS
    for (i = 0; i < n; i++)
        if (hw_batch_q[i].flags >= 0)
            HW_SubPhaseB(&hw_batch_q[i], 1);
    HW_BatchFlats(n);
#else
    for (i = 0; i < n; i++)
        if (hw_batch_q[i].flags >= 0)
            HW_SubPhaseFlat(&hw_batch_q[i]);
    for (i = 0; i < n; i++)
        if (hw_batch_q[i].flags >= 0)
            HW_SubPhaseB(&hw_batch_q[i], 1);
#endif
}

/* BSP leaf: queue (the walk keeps going with the batch-start state) */
static HW_NOINLINE void HW_BatchLeaf(int subnum, int fully_inside, unsigned int *unused)
{
    (void)unused;
#ifndef SATURN_WIN32
    if (__builtin_expect(!hw_flat_ring, 0))
    {
        hw_batch_slot_t q;
        HW_BatchFlush();
        if (HW_SubPhaseA(subnum, fully_inside, &q))
        {
            HW_SubPhaseFlat(&q);
            HW_SubPhaseB(&q, 0);
        }
        return;
    }
#endif
    hw_batch_q[hw_batch_n].subnum = (int16_t)subnum;
    hw_batch_q[hw_batch_n].fully  = (int16_t)fully_inside;
    if (++hw_batch_n >= HW_BATCH)
        HW_BatchFlush();
}
#undef HW_RenderSubsector
#define HW_RenderSubsector(subnum, fully_inside) HW_BatchLeaf((subnum), (fully_inside), NULL)
#define HW_LEAF_FN HW_BatchLeaf
#else
#define HW_LEAF_FN HW_RenderSubsector_Body
#define HW_BatchFlush() ((void)0)
#endif /* HW_BATCH */
#endif /* HW_ASM_SUBBODY */
#ifndef HW_LEAF_FN
#define HW_LEAF_FN HW_RenderSubsector_Body
#define HW_BatchFlush() ((void)0)
#endif

typedef struct {
    uint16_t bspnum;       /* must be unsigned: node_t children are uint16_t (NF_SUBSECTOR = 0x8000) */
    int16_t  fully_inside;
    const int16_t *bbox;
} bsp_stack_t;

#define HW_BSP_STACK_MAX 64

#include "saturn_hw_tiny.c"     /* skip areas tiny on screen (2026-11-26) */

/* BSP node far plane in map units << 12 (adaptive detail; default the
 * HW_FAR_Z limit). Read cache-through by hw_asm_cullbox so it can change
 * between frames (and be poked by the 5thPlanet harness). */
HW_W2_SHARED(volatile int32_t hw_far_cull, = HW_FAR_Z_UNITS << 12);

/* Adaptive detail (2026-11-20, user: "try adaptive detail to keep the FPS
 * high"). After each rendered 3D frame: frames slower than 30 FPS step the
 * detail level up, a run of comfortably fast frames steps it back down.
 * Levels first cap the BSP draw distance (the E1M1 courtyard spends ~8 ms
 * on areas seen through far window openings: 5thPlanet p2 25.8 -> 30.0 FPS
 * at 1536 units with 69 px changed, m1b 22.5 -> 30.0 at 1280 with 386 px;
 * the other 10 tour views neither change nor gain), then coarsen the wall
 * strip LOD (m1a, where distance does not help: +0.5..0.7 FPS).
 * hw_ad_mode (cache-through, pokeable): 1 adaptive, 0 off (full detail). */
/* 2026-11-23 (user: "tune the LOD to get a minimum of 30 FPS everywhere,
 * reduce the work in big scenes at a distance, keep close detail"): AUTO by
 * default again. The far plane now also culls single subsectors (it only
 * culled BSP nodes before, so it barely changed E4 views), the steps are
 * finer at long range, and a step that does not pay is taken back (see
 * Saturn_HW_AdaptiveDetail) - dense near scenes keep their far detail. */
#ifndef HW_AD_DEFAULT
#define HW_AD_DEFAULT 1
#endif
HW_W2_SHARED(volatile int32_t hw_ad_mode, = HW_AD_DEFAULT);   /* 1 AUTO, 0 FULL (pause menu DETAIL) */
HW_W2_SHARED(int saturn_hw_detail_level, = 0);                 /* HUD */
#ifndef HW_WALK2_TU
unsigned int saturn_hw_ad_avg = 0, saturn_hw_ad_last = 0;   /* debug: frame ticks (avg, last) */
static const int16_t hw_ad_far[]   = { 16384, 3072, 2304, 1792, 1536, 1280, 1280 };
static const uint8_t hw_ad_lodx[]  = {     1,    1,    1,    1,    1,    1,    2 };
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
#if HW_TINY
        if (HW_TINY_T0 < 0 && hw_tiny_t != 0)
        {
            hw_tiny_t = 0;          /* DETAIL FULL: draw everything (a fixed HW_TINY_T0 stays) */
            HW_TinyReset();
        }
#endif
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
#if HW_TINY
    HW_TinyGovern(avg, slow, fast);
#endif
    /* A climb that does not pay is taken back (2026-11-23): dense NEAR
     * geometry (retail E4M2 / E4M9: 100-200 subsectors within 1000 units)
     * does not get faster when the far plane comes in, it only loses its
     * distant detail. The levels climb as before; 20 frames after the top
     * level is reached while still slow, the average must be >= 6% below
     * the one that started the climb, else it drops straight back to full
     * detail and no climb is tried for 3 s (doubling to 20 s while it keeps
     * failing). Judging every single step instead stopped at the first
     * one (3072, no gain in E4M3) although 1536 gives +70% there. */
    static unsigned int climb_avg = 0;
    static int top_wait = 0, up_block = 0, fail_block = 180;
    if (up_block > 0)
        up_block--;
    if (top_wait > 0 && --top_wait == 0 && level == HW_AD_LEVELS - 1 && frame_ticks > slow)
    {
        if (frame_ticks > climb_avg - climb_avg / 16u)
        {
            HW_AdaptiveApply(level = 0);
            up_block = fail_block;
            if (fail_block < 1200)
                fail_block *= 2;
            slow_run = fast_run = 0;
            return;
        }
        fail_block = 180;                   /* the climb paid */
    }
    if (frame_ticks > slow)
    {
        fast_run = 0;
        if (++slow_run >= 6 && level < HW_AD_LEVELS - 1 && !up_block)
        {
            if (level == 0)
                climb_avg = frame_ticks;
            if (level + 1 == HW_AD_LEVELS - 1)
                top_wait = 20;
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
#endif /* !HW_WALK2_TU */
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
/* Not const: in HW_WALK2 split frames the master's leaf entry (index 16) is
 * HW_W2_LeafPoll (serves the slave's requests between subsectors). */
#define HW_BSP_CTX_LEAF 16     /* index of the leaf entry (byte offset 64) */
const void *hw_asm_bsp_ctx[21] = {
    &hw_frust, &hw_cam_x, &hw_cam_y, &hw_solid_count,                    /*  0 */
    &saturn_hw_bsp_occl, &saturn_hw_occl_culls, saturn_master_oclut,     /* 16 */
    hw_solid_mask, hw_mask_left, hw_mask_right, hw_span_mask_lut,        /* 28 */
    &hw_solid_any, &hw_solid_summary, &saturn_view_w,                    /* 44 */
#if HW_ASM_SUBBODY
    &nodes, hw_bsp_stack, (const void *)hw_asm_subsector,                /* 56 */
#else
    &nodes, hw_bsp_stack, (const void *)HW_LEAF_FN,                      /* 56 */
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
#ifdef SATURN_WIN32
static int hw_visit_budget = -1;     /* -1 = read HW_VISIT_BUDGET once; 0 = off */
/* vdp1_emu.c's per-subsector pixel census (true final pixels): kept for
 * checking the tiny-area estimate (saturn_hw_tiny.c) against */
int hw_census_px[8192];
uint8_t hw_census_present[8192];
void HW_TinyCensus(const int *px, const uint8_t *present, int n, unsigned gen)
{
    (void)gen;
    if (n > 8192) n = 8192;
    memcpy(hw_census_px, px, (size_t)n * sizeof(int));
    memcpy(hw_census_present, present, (size_t)n);
}
#endif
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
#ifdef SATURN_WIN32
    if (hw_visit_budget < 0)
    {
        const char *e = getenv("HW_VISIT_BUDGET");
        hw_visit_budget = e ? atoi(e) : 0;
    }
#endif
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
#if HW_COARSE
    /* coarse subtree (saturn_hw_coarse.c): entries pushed at or above
     * coarse_d0 belong to the coarse node entered at that stack depth */
    int coarse_d0 = -1;
#endif

    while (sp_ptr > stack_base)
    {
        if (hw_solid_count >= HW_VIEW_W) {
            saturn_hw_bsp_abort++;
            return;
        }
#ifdef SATURN_WIN32
        /* frame-deadline preview (2026-11-26): stop after N visited
         * subsectors (harness env HW_VISIT_BUDGET; Saturn cost fit
         * ~15.6 ms + 0.45 ms per visit: 38 = 30 FPS, 76 = 20 FPS) */
        if (hw_visit_budget > 0 && (int)saturn_visit_count >= hw_visit_budget) {
            saturn_hw_bsp_abort++;
            return;
        }
#endif

        --sp_ptr;
#if HW_COARSE
        if (coarse_d0 >= 0 && (int)(sp_ptr - stack_base) < coarse_d0)
            coarse_d0 = -1;             /* left the coarse subtree */
#endif
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
#if HW_COARSE
                    if (coarse_d0 >= 0)
                        HW_CoarseLeaf(bspnum & 0x7FFFu, fully_inside);
                    else
#endif
#if HW_TINY
                    if (hw_tiny_t > 0)
                        HW_TinyLeaf(bspnum & 0x7FFFu, fully_inside);
                    else
#endif
                    HW_RenderSubsector(bspnum & 0x7FFFu, fully_inside);
                }
                break;
            }
#if HW_COARSE
            if (coarse_d0 < 0 && HW_CoarseNodeWanted(bspnum))
            {
                coarse_d0 = (int)(sp_ptr - stack_base);
                saturn_hw_coarse_nodes++;
            }
#endif

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

/* =========================================================================
 * HW_WALK2: two-CPU screen split (2026-11-24, saturn_hw_walk2.c/.h)
 * ========================================================================= */
#if HW_WALK2 && !defined(SATURN_WIN32)
extern int  Saturn_Vdp1_SprSrca(int scache, int *pw, int *h);    /* saturn_vdp1.c */
extern int  Saturn_Vdp1_FlatAvgPeek(int flatnum);
extern v1_tag_t *saturn_v1_spr_tags;
extern byte *Saturn_SpritePatchPeek(int s, int *w, int *h, int *lofs, int *tofs);   /* saturn_tex.c */
#ifndef HW_WALK2_TU
/* ---- master side ---- */
hw_w2_mail_t hw_w2_mail __attribute__((aligned(16)));
/* Split control (cache-through, pokeable): 0 = off, 1 = fixed column
 * hw_w2_fix_x (the slave renders [x, 320); 0 = the slave renders all),
 * 2 = automatic column, every frame split, 3 = dynamic: split only the
 * frames where that measured cheaper (automatic column). */
#ifndef HW_W2_DEFAULT
#define HW_W2_DEFAULT 3
#endif
volatile int32_t hw_w2_mode  = HW_W2_DEFAULT;
#ifndef HW_W2_FIX_X
#define HW_W2_FIX_X 160
#endif
volatile int32_t hw_w2_fix_x = HW_W2_FIX_X;
volatile uint32_t hw_w2_stat[8];     /* [0] split frames [1] rpcs [2] wait timeouts [3] last x [4] slave busy [5] master walk [6] wait ticks */
static int  hw_w2_split = 0;         /* this frame is split */
static int  hw_w2_x = 160;           /* this frame's column: master [0, x), slave [x, 320) */
static int  hw_w2_auto_x = 160;      /* automatic mode: the balance point so far */
static int  hw_w2_worker_up = 0;     /* the slave worker loop runs (it starts split walks) */
static uint32_t hw_w2_seq = 0;
static unsigned short hw_w2_t0;      /* master walk start (FRT) */
/* this frame's split for the delivery (saturn_cmdbuf.c): -1 = not split,
 * else the column; the slave's list lengths (from 0 in each buffer) */
int saturn_w2_split_x = -1;
int hw_w2_out_m = 0, hw_w2_out_s = 0, hw_w2_out_v = 0;
#if defined(SATURN_BENCH)
static uint32_t hw_sbusy0;
#endif

/* Serve one pending slave request (runs between the master's own
 * subsectors and while it waits; never inside a texture lookup). */
static HW_NOINLINE void HW_W2_Serve(void)
{
    volatile hw_w2_mail_t *m = HW_W2_MAIL;
    uint32_t req = m->rpc_req;
    if (req == m->rpc_ack)
        return;
    int fn = m->rpc_fn, a0 = m->rpc_a[0], a1 = m->rpc_a[1];
    int r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    switch (fn)
    {
    case HW_W2_RPC_TEX:
    {
        extern int saturn_v1_wall_half_req;
        int keep = saturn_v1_wall_half_req;
        saturn_v1_wall_half_req = a1;
        r0 = Saturn_Vdp1_GetTexSrca(a0, &r1, &r2);
        saturn_v1_wall_half_req = keep;
        break;
    }
    case HW_W2_RPC_FLAT:     r0 = Saturn_Vdp1_GetFlatSrca(a0); break;
    case HW_W2_RPC_FLATVAR:  r0 = Saturn_Vdp1_GetFlatSrcaVariant(a0, a1); break;
    case HW_W2_RPC_FLATAVG:  r0 = Saturn_Vdp1_FlatAvgColor(a0); break;
    case HW_W2_RPC_SPR:      r0 = Saturn_Vdp1_SprSrca(a0, &r1, &r2); break;
    case HW_W2_RPC_SPRPATCH: r0 = (int)(uintptr_t)Saturn_SpritePatch(a0, &r1, &r2, &r3, &r4); break;
    default: break;
    }
    m->rpc_r[0] = r0; m->rpc_r[1] = r1; m->rpc_r[2] = r2;
    m->rpc_r[3] = r3; m->rpc_r[4] = r4;
    m->rpc_ack = req;
    hw_w2_stat[1]++;
}

/* The master's BSP leaf in split frames (hw_asm_bsp_ctx[56]) */
static HW_NOINLINE void HW_W2_LeafPoll(int subnum, int fully_inside)
{
    HW_W2_Serve();
#if HW_TINY
    if (hw_tiny_t > 0)
        HW_TinyLeaf(subnum, fully_inside);
    else
        HW_TINY_REAL_LEAF(subnum, fully_inside);
#elif HW_ASM_SUBBODY
    hw_asm_subsector(subnum, fully_inside);
#else
    HW_LEAF_FN(subnum, fully_inside, NULL);
#endif
}

/* Mode 3 (dynamic, 2026-11-26): each frame is either a normal frame (the
 * master walks everything, the slave does floors) or a split frame,
 * whichever measured cheaper. The split loses where floors dominate (the
 * slave is already busy: E3M6 m6c 63 -> 43 frames) and wins where the
 * master's walk dominates (E4M2 e42a 63 -> 68). Cost = the master's time
 * from the walk start to the end of its wait for the slave. The mode not in
 * use is re-measured with one probe frame every hw_w2_every[] frames; a
 * probe that loses doubles that interval (fewer slow frames), one that
 * wins switches and resets it. */
#ifndef HW_W2_PROBE_MIN
#define HW_W2_PROBE_MIN 24
#endif
#ifndef HW_W2_PROBE_MAX
#define HW_W2_PROBE_MAX 192
#endif
static int hw_w2_dyn = 0;                       /* mode 3: 1 = split frames */
static int hw_w2_probe = 0;                     /* this frame probes the other choice */
static unsigned int hw_w2_cost[2];              /* recent cost per choice (FRT ticks) */
static int hw_w2_age[2];                        /* frames since that choice ran */
static int hw_w2_every[2] = { HW_W2_PROBE_MIN, HW_W2_PROBE_MIN };
static int hw_w2_timed = 0;                     /* this frame is timed (mode 3) */
static unsigned short hw_w2_f0;
static unsigned int hw_w2_v0;

/* This frame's split column. Automatic: move the column 32 px toward the
 * CPU that finished early, by last frame's walk times. */
static void HW_W2_Decide(void)
{
    hw_w2_split = 0;
    hw_w2_timed = 0;
    saturn_w2_split_x = -1;
    hw_w_x0 = 0;
    hw_w_x1 = HW_VIEW_W;
    if (!hw_w2_worker_up || !saturn_slave_enabled || !hw_w2_inited || HW_ORACLE || HW_BATCH)
        return;
    int mode = hw_w2_mode, x;
    if (mode == 1)
        x = hw_w2_fix_x;
    else if (mode == 2)
        x = hw_w2_auto_x;
    else if (mode == 3)
    {
        int use = hw_w2_dyn;
        hw_w2_probe = 0;
        if (hw_w2_age[use ^ 1] >= hw_w2_every[use ^ 1])
        {
            use ^= 1;
            hw_w2_probe = 1;
        }
        hw_w2_timed = 1;
        do {
            hw_w2_v0 = saturn_vbl_count;
            hw_w2_f0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != hw_w2_v0, 0));
        if (!use)
            return;
        x = hw_w2_auto_x;
    }
    else
        return;
    x &= ~31;
    if (x < 0) x = 0;
    if (x > HW_VIEW_W - 32) x = HW_VIEW_W - 32;
    hw_w2_split = 1;
    hw_w2_x = x;
    saturn_w2_split_x = x;
    hw_w_x1 = (int16_t)x;
}

static unsigned short hw_w2_f0s;        /* split walk start (FRT, vbl) */
static unsigned int hw_w2_v0s;
static void HW_W2_Kick(const byte *pvs_row)
{
    if (!hw_w2_split)
        return;
    volatile hw_w2_mail_t *m = HW_W2_MAIL;
    m->x0 = hw_w2_x;
    m->x1 = HW_VIEW_W;
    m->viewx = hw_viewx; m->viewy = hw_viewy; m->viewz = hw_viewz;
    m->viewangle = hw_viewangle;
    m->viewsin = hw_viewsin; m->viewcos = hw_viewcos;
    m->cam_x = hw_cam_x; m->cam_y = hw_cam_y;
    m->cz_origin = hw_frust.cz_origin; m->cx_origin = hw_frust.cx_origin;
    m->displayplayer = hw_displayplayer;
    m->pvs_row = pvs_row;
    m->wall_tags = saturn_v1_wall_tags ? (void *)((uintptr_t)saturn_v1_wall_tags | 0x20000000u) : NULL;
    m->flat_tags = saturn_v1_flat_tags ? (void *)((uintptr_t)saturn_v1_flat_tags | 0x20000000u) : NULL;
    m->spr_tags  = saturn_v1_spr_tags  ? (void *)((uintptr_t)saturn_v1_spr_tags  | 0x20000000u) : NULL;
    m->cmd_m = saturn_cmd_m;
    m->cmd_s = saturn_cmd_s;
    m->ranges = saturn_sub_ranges;
    __asm__ volatile("" ::: "memory");
    m->seq = ++hw_w2_seq;               /* the slave worker loop starts hw_w2_entry */
    hw_asm_bsp_ctx[HW_BSP_CTX_LEAF] = (const void *)HW_W2_LeafPoll;
    hw_w2_t0 = SAT_FRT();
    do {
        hw_w2_v0s = saturn_vbl_count;
        hw_w2_f0s = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != hw_w2_v0s, 0));
}

/* After the master's own walk: serve the slave until it is done, then take
 * its results (its lists are the bottom of the master's buffers). */
static unsigned int hw_w2_mwalk;
static unsigned short hw_w2_fe;         /* the master's walk end (FRT, vbl) */
static unsigned int hw_w2_ve;
static void HW_W2_EndWalk(void)
{
    if (!hw_w2_split)
        return;
#if HW_ASM_SUBBODY
    hw_asm_bsp_ctx[HW_BSP_CTX_LEAF] = (const void *)hw_asm_subsector;
#else
    hw_asm_bsp_ctx[HW_BSP_CTX_LEAF] = (const void *)HW_LEAF_FN;
#endif
    hw_w2_mwalk = (unsigned short)(SAT_FRT() - hw_w2_t0);
    do {
        hw_w2_ve = saturn_vbl_count;
        hw_w2_fe = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != hw_w2_ve, 0));
}

static void HW_W2_Wait(void)
{
    if (!hw_w2_split)
        return;
    volatile hw_w2_mail_t *m = HW_W2_MAIL;
    unsigned short tw0 = SAT_FRT();
    unsigned int mwalk = hw_w2_mwalk;
    int guard = 2000000;
    while (m->done != hw_w2_seq)
    {
        HW_W2_Serve();
        HW_BUS_YIELD();
        if (--guard <= 0)
            break;
    }
    unsigned int wait = (unsigned short)(SAT_FRT() - tw0);
    hw_w2_stat[0]++;
    hw_w2_stat[3] = (uint32_t)hw_w2_x;
    hw_w2_stat[5] = mwalk;
    hw_w2_stat[6] = wait;
    if (guard <= 0)
    {
        /* the slave never finished: no split frames (and no slave) again */
        hw_w2_stat[2]++;
        hw_w2_mode = 0;
        saturn_slave_enabled = 0;
        *(volatile unsigned char *)((uint32_t)&saturn_slave_enabled | SATURN_WRAM_H_UNCACHED) = 0;
        hw_w2_out_m = hw_w2_out_s = hw_w2_out_v = 0;   /* deliver the master's half alone */
        return;
    }
    hw_w2_out_m = m->m_count;
    hw_w2_out_s = m->s_count;
    hw_w2_out_v = m->visit_count;
    if (m->needs_ytrim)
        saturn_cmd_m_needs_ytrim = 1;
    if (m->sky_visible)
        saturn_hw_sky_visible = 1;
    saturn_hw_wall_quads += m->wall_quads;
    saturn_hw_wall_strips += m->wall_strips;
    hw_w2_stat[4] = m->busy;
    /* automatic column: the slave renders [x, 320), then the master's
     * floor jobs from the ring. Comparing walk times alone pushed the column
     * to the right edge (the slave's half carries its floors, the master's
     * does not; and a 16-bit walk time wraps past 73 ms). Balance on what
     * the frame pays instead: the master's time after its own walk (waiting
     * for / helping the slave). Waiting more than 1/16 of the walk: the
     * slave has too much, move the column right; otherwise left. */
    {
        unsigned int post = Saturn_ProfSpan(hw_w2_fe, hw_w2_ve);
        unsigned int walk = Saturn_ProfSpan(hw_w2_f0s, hw_w2_v0s) - post;
        if (post > (walk >> 4))
        {
            if (hw_w2_auto_x < HW_VIEW_W - 32)
                hw_w2_auto_x += 32;     /* slave slower: give it fewer columns */
        }
        else if (hw_w2_auto_x > 32)
            hw_w2_auto_x -= 32;
        hw_w2_stat[5] = walk;
        hw_w2_stat[6] = post;
    }
}

/* Mode 3: book this frame's cost (after HW_W2_Wait, both kinds of frame). */
static void HW_W2_FrameEnd(void)
{
    if (!hw_w2_timed)
        return;
    hw_w2_timed = 0;
    unsigned int c = Saturn_ProfSpan(hw_w2_f0, hw_w2_v0);
    int m = hw_w2_split;                /* the choice this frame ran */
    hw_w2_age[m] = 0;
    if (hw_w2_age[m ^ 1] < 0x7FFF)
        hw_w2_age[m ^ 1]++;
    if (hw_w2_probe)
    {
        hw_w2_probe = 0;
        /* against the current choice's recent cost (same view, a frame
         * apart); switch only on a clear win (> 1/16) */
        if (c + (c >> 4) < hw_w2_cost[m ^ 1])
        {
            hw_w2_dyn = m;
            hw_w2_every[m ^ 1] = HW_W2_PROBE_MIN;
        }
        else if (hw_w2_every[m] < HW_W2_PROBE_MAX)
            hw_w2_every[m] <<= 1;
        hw_w2_cost[m] = c;
    }
    else
        hw_w2_cost[m] = hw_w2_cost[m] ? (hw_w2_cost[m] + c) >> 1 : c;
    hw_w2_stat[7] = (uint32_t)hw_w2_dyn | ((uint32_t)hw_w2_every[0] << 8) |
                    ((uint32_t)hw_w2_every[1] << 20);
}
#endif /* !HW_WALK2_TU */
#endif /* HW_WALK2 */
#if !HW_WALK2 || defined(SATURN_WIN32)
#define HW_W2_Decide()  ((void)0)
#define HW_W2_Kick(p)   ((void)(p))
#define HW_W2_Wait()    ((void)0)
#define HW_W2_FrameEnd() ((void)0)
#define HW_W2_EndWalk() ((void)0)
#define hw_w2_split     0
static int hw_w2_worker_up;
#if defined(SATURN_BENCH)
static uint32_t hw_sbusy0;
#endif
#endif

#if defined(SATURN_BENCH) && !defined(HW_WALK2_TU)
extern volatile uint32_t saturn_slave_busy_ticks;
extern uint32_t saturn_hw_prof_slave_busy;
#endif

/* Per-walker frame: setup (occlusion, aperture, sector cache, strip of
 * columns) and the BSP walk + emission. The master runs them inside
 * Saturn_HW_RenderPlayerView; the slave copy (HW_WALK2_TU) from hw_w2_entry. */
static HW_NOINLINE void HW_FrameSetup(const byte *pvs_row, const subsector_t *ssec);
static HW_NOINLINE void HW_FrameWalk(const byte *pvs_row);

#ifndef HW_WALK2_TU
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
    /* New level (2026-11-22): the zone allocates the same way every load,
     * so `nodes` can land at the same address and the frame counters
     * restart at 1 - per-frame/per-camera caches keyed on them could
     * survive into the next map (old subsector/PVS row, old texture SRCA,
     * old plane-punch spans). Drop them on every level load. */
    {
        extern unsigned int saturn_level_gen;
        static unsigned int hw_cache_gen = 0xFFFFFFFFu;
        if (hw_cache_gen != saturn_level_gen)
        {
            hw_cache_gen      = saturn_level_gen;
            last_ssec         = NULL;
            last_pvs_row      = NULL;
            hw_wctx_tex_frame = (uint16_t)(hw_render_frame + 0x8000);
            hw_v2p_frame      = 0xFFFFFFFFu;
#if HW_TINY
            HW_TinyReset();     /* subsector numbers of the old map */
#endif
        }
    }
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

    /* Two-CPU screen split (HW_WALK2): pick this frame's split column; the
    * master renders [hw_w_x0, hw_w_x1), the slave copy the rest. */
    HW_W2_Decide();
    HW_FrameSetup(pvs_row, ssec);
    HW_W2_Kick(pvs_row);            /* split frame: start the slave walk */
    HW_FrameWalk(pvs_row);
    HW_W2_EndWalk();                /* split frame: the master's walk is done */


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
            HW_W2_SERVE();          /* a split walk's request (HW_WALK2) */
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
            *(volatile uint32_t *)((uint32_t)&saturn_slave_busy_ticks | 0x20000000u) - hw_sbusy0;
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
    HW_W2_Wait();               /* split frame: the slave's walk outputs (it is done by now) */
    HW_W2_FrameEnd();           /* mode 3: this frame's cost -> split or not next frame */
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
#if HW_TINY
    HW_TinyMeasure();           /* mark this frame's tiny visits (cache purged above) */
#endif

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
#endif /* !HW_WALK2_TU */

static HW_NOINLINE void HW_FrameSetup(const byte *pvs_row, const subsector_t *ssec)
{
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

#ifdef HW_WALK2_TU
    /* the slave copy: its own lists only (the VDP1 frame state, texture
     * evictions and the per-level tables are the master's) */
    hw_w2_cmd_m_count = 0;
    hw_w2_cmd_s_count = 0;
    hw_w2_visit_count = 0;
    hw_w2_needs_ytrim = 0;
    hw_w2_v1w_cells = hw_w2_v1w_drop = hw_w2_v1w_captured = 0;
#else
    /* Reset VDP1 quad table and clip buffers */
    Saturn_Vdp1Wall_Reset();
    Saturn_CmdBuf_Reset();
#if HW_WALK2 && !defined(SATURN_WIN32)
    if (hw_w2_split)
    {
        /* the slave's lists take the bottom of each buffer (saturn_hw_walk2.h) */
        saturn_cmd_m_count = HW_W2_M_CAP;
        saturn_visit_count = HW_W2_VISIT_CAP;
    }
#endif
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
#endif /* !HW_WALK2_TU */
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
#ifndef HW_WALK2_TU
    Saturn_Vdp1_WarpNewFrame();
    HW_WarpService();       /* textures the flat workers asked for last frame */
#endif
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
        /* Dark-sector distance cut (2026-11-23): saturn_light_lut.h shades
         * every wall and flat of light index L <= 7 (light < 128) in
         * bank 7 = COLORMAP 31 beyond these depths (both zlight
         * LUTs, bucket = depth / 16, checked over all 128 buckets), so its
         * subsectors past them only ever drew black: cull them there.
         * Sectors with things keep the normal far plane (fullbright sprites).
         * Measured on 9 retail worst views: +0..1 frame per 240 fields, so
         * OFF by default (a culled dark wall also stops occluding what is
         * behind it). -DHW_DARK_CUT=1 to try. Depths in map units. */
#ifndef HW_DARK_CUT
#define HW_DARK_CUT 0
#endif
        static const uint16_t hw_dark_far[8] = { 32, 32, 48, 64, 80, 112, 160, 320 };
        const int32_t far_all = hw_far_cull;
        int32_t *s_far = hw_sec_far;
        for (int i = 0; i < n_ap_sec; i++, s_in++, s_out++, s_far++)
        {
            s_out->dz_ceil    = s_in->ceilingheight - vz;
            s_out->dz_floor   = s_in->floorheight - vz;
            s_out->yt_attach  = vh;
            s_out->yb_attach  = 0;
            s_out->is_sky     = (uint8_t)((int16_t)s_in->ceilingpic == -1);
            s_out->has_things = (uint8_t)(s_in->thinglist != NULL);
            int l16 = HW_SecLight16(s_in->lightlevel);
            s_out->light_delta = (int16_t)(l16 - (int)hw_sec_light0[i]);
            int32_t f = far_all;
            /* walls carry DOOM's fake contrast (bake_walls.py: axis-aligned
             * +-16), so they can sit one index above the sector: use L + 1 */
            if (HW_DARK_CUT && l16 < 7 && !s_out->has_things)
            {
                int32_t d = (int32_t)hw_dark_far[l16 + 1] << 12;
                if (d < f)
                    f = d;
            }
            *s_far = f;
        }
#ifndef HW_WALK2_TU     /* shared table: the master fills it before the slave starts */
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
#endif
        hw_psec_fast =(hw_psec && (unsigned)(hw_psec - sectors) < HW_MAX_AP_SECTORS)
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
#ifndef HW_WALK2_TU
    hw_ad_rendered = 1;             /* adaptive detail: a 3D frame */
#endif
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
#if HW_TINY_SUB
    saturn_hw_tiny_subs = 0;
#endif
    saturn_hw_slave_timeout = 0;
    saturn_hw_prof_slave_wait = 0;
#if defined(SATURN_BENCH) && !defined(HW_WALK2_TU)
    hw_sbusy0 = *(volatile uint32_t *)((uint32_t)&saturn_slave_busy_ticks | 0x20000000u);
    saturn_hw_prof_slave_busy = 0;
#endif
    saturn_hw_bsp_abort = 0;
    saturn_hw_occl_culls = 0;
#if HW_FINE_CULL && !defined(HW_WALK2_TU)
    saturn_hw_fine_culls = 0;
#endif
#if HW_COARSE
    saturn_hw_coarse_nodes = 0;
    saturn_hw_coarse_subs = 0;
#endif
    saturn_hw_seg_iter = 0;
    saturn_hw_seg_vis = 0;
    saturn_hw_tex_miss = 0;
    saturn_hw_wall_strips = 0;
    saturn_hw_flat_lod0_subs = 0;
    saturn_hw_flat_lod1_subs = 0;
    saturn_hw_flat_lod_saved_quads = 0;

#ifndef HW_WALK2_TU
    /* Queue player weapon directly into saturn_cmd_w */
    if (!HW_WALLS_ONLY)
        HW_RenderPlayerWeapon();

    /* raster-hole backing polygons only when the sky shows (saturn_hw_flats.c) */
    hw_hole_backing_on = saturn_hw_sky_visible;     /* last frame's value */
#endif
    saturn_hw_sky_visible = (hw_psec && ((int16_t)hw_psec->ceilingpic == -1));

    /* Apply Potentially Visible Set (PVS) for player's current leaf position.
     * Note: Keyed on `ssec` (unique subsector pointer), NOT `pvs_row`, because
     * Saturn_Pvs_GetRow() on Saturn returns a static cache buffer address.
     * (Writes the shared nodes[].encbbox: master only; the slave copy walks
     * the master's result.) */
    hw_cur_pvs_row = pvs_row;
#ifndef HW_WALK2_TU
    const void *ssec_key = (pvs_row && ssec) ? (const void *)ssec : (const void *)1;
    if (__builtin_expect(ssec_key != HW_APERTURE->last_pvs_ssec ||
                         numnodes != HW_APERTURE->last_pvs_numnodes, 0))
    {
        HW_UpdateNodePvsVis(pvs_row, ssec_key);
    }
#else
    (void)ssec;
#endif

#ifdef HW_WALK2_TU
    hw_frame_slave_enabled = 0;     /* the slave copy renders its own flats */
    hw_cur_sub_ranges = saturn_sub_ranges;
#elif !defined(SATURN_WIN32)
    extern unsigned char saturn_slave_enabled;
    extern void jo_core_exec_on_slave(void (*callback)(void));
    extern void jo_core_wait_for_slave(void);

    /* split frame (HW_WALK2): the master's floor jobs still go to the slave's
     * ring; the slave takes them after its own walk (its own floors are at
     * the bottom of cmd_s, the ring's follow: saturn_hw_flats.c worker loop) */
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
            hw_w2_worker_up = 1;
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
        /* View mode change (168 <-> 192 rows): the slave's flat emitters
         * read saturn_view_h / saturn_view_cy through its cache
         * (HW_FlatQuadDy, HW_SliceFlatQuadV) and could keep the old values
         * in a hot line - make it purge, same as a new level (2026-11-22). */
        {
            static int hw_slave_vh = -1, hw_slave_vcy = -1;
            if (hw_slave_vh != saturn_view_h || hw_slave_vcy != saturn_view_cy)
            {
                hw_slave_vh  = saturn_view_h;
                hw_slave_vcy = saturn_view_cy;
                hw_slave_level_dirty = 1;
            }
        }
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
    hw_flat_ring = (unsigned char)(hw_frame_slave_enabled != 0);
    HW_SlaveWalls_BeginFrame(hw_frame_slave_enabled);   /* HW_SLAVE_WALLS */

#if defined(HW_STRIP_X0) || defined(HW_STRIP_X1)
    /* Measurement only (2026-11-23, two-CPU split study): render just the
     * columns [HW_STRIP_X0, HW_STRIP_X1). */
#ifndef HW_STRIP_X0
#define HW_STRIP_X0 0
#endif
#ifndef HW_STRIP_X1
#define HW_STRIP_X1 320
#endif
    _Static_assert((HW_STRIP_X0 & 31) == 0 && (HW_STRIP_X1 & 31) == 0, "HW_STRIP_X0/X1: word edges");
    hw_w_x0 = HW_STRIP_X0;
    hw_w_x1 = HW_STRIP_X1;
#endif
    /* This walker's columns [hw_w_x0, hw_w_x1) (HW_WALK2 split, or the
     * HW_STRIP measurement): the rest start out solid, as if the other
     * CPU's walls covered them. Edges are multiples of 32 (whole occlusion
     * words: no aperture rescan needed). */
    if (hw_w_x0 > 0 || hw_w_x1 < HW_VIEW_W)
    {
        hw_aperture_state_t *ap = HW_APERTURE;
        const int16_t vh = (int16_t)saturn_view_h;
        for (int w = 0; w < (HW_VIEW_W >> 5); w++)
        {
            if ((w << 5) >= hw_w_x0 && (w << 5) < hw_w_x1)
                continue;
            uint32_t wbit = hw_bit_lut[w];
            hw_solid_mask[w] = 0xFFFFFFFFu;
            hw_solid_any |= wbit;
            hw_slave_dirty_words |= wbit;
            hw_solid_words++;
            hw_solid_summary |= wbit;
            hw_ap_words[w].yt_min = hw_ap_words[w].yt_max = vh;
            hw_ap_words[w].yb_min = hw_ap_words[w].yb_max = 0;
            ap->dirty_words |= wbit;
            ap->aperture_active = 1;
        }
        hw_solid_count = (hw_solid_words >= (HW_VIEW_W >> 5)) ? HW_VIEW_W : 1;
    }
}

static HW_NOINLINE void HW_FrameWalk(const byte *pvs_row)
{
#if HW_TINY && HW_ASM_BSP && !HW_ASM_CHECK
    /* tiny-area skip (saturn_hw_tiny.c): the asm walk's leaf, in the master
     * and in the HW_WALK2 slave copy. A split frame's master leaf is
     * HW_W2_LeafPoll (set by HW_W2_Kick; it applies the skip itself). */
    {
        const void *cur = hw_asm_bsp_ctx[HW_BSP_CTX_LEAF];
        if (cur == HW_TINY_REAL_LEAF_PTR || cur == (const void *)HW_TinyLeaf)
            hw_asm_bsp_ctx[HW_BSP_CTX_LEAF] = (hw_tiny_t > 0) ? (const void *)HW_TinyLeaf
                                                              : HW_TINY_REAL_LEAF_PTR;
    }
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
    HW_BatchFlush();                /* HW_BATCH: the last partial batch */
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

/* =========================================================================
 * HW_WALK2: the slave renderer copy (this TU compiled as saturn_hw_walk2.c)
 * ========================================================================= */
#if HW_WALK2 && !defined(SATURN_WIN32) && defined(HW_WALK2_TU)
vdp1_cmd_t        *hw_w2_cmd_m, *hw_w2_cmd_s;   /* = the master's buffers (bottom part) */
sub_range_t       *hw_w2_sub_ranges;
volatile uint16_t  hw_w2_cmd_m_count, hw_w2_cmd_s_count;
int                hw_w2_visit_count;
uint8_t            hw_w2_needs_ytrim;
int                hw_w2_v1w_cells, hw_w2_v1w_drop, hw_w2_v1w_captured;
unsigned char      hw_w2_no_slave = 0;
v1_tag_t          *hw_w2_wall_tags, *hw_w2_flat_tags;   /* cache-through aliases */
static v1_tag_t   *hw_w2_spr_tags;
int                hw_w2_half_req = 0, hw_w2_late_copy_armed = 0;
static uint32_t    hw_w2_rpcs;

/* Lazy buffers of this copy (hw_fsc store) come from a pool the master
 * allocates in hw_w2_init: the slave never calls the allocator. */
#define HW_W2_POOL 2048
static uint8_t *hw_w2_pool;
static int      hw_w2_pool_left;
byte *hw_w2_AllocHigh(int length)
{
    length = (length + 15) & ~15;
    if (!hw_w2_pool || length > hw_w2_pool_left)
        return NULL;
    byte *p = hw_w2_pool;
    hw_w2_pool += length;
    hw_w2_pool_left -= length;
    return p;
}

/* One request to the master; results in HW_W2_MAIL->rpc_r[] */
static HW_NOINLINE int hw_w2_rpc(int fn, int a0, int a1)
{
    volatile hw_w2_mail_t *m = HW_W2_MAIL;
    m->rpc_fn = fn;
    m->rpc_a[0] = a0;
    m->rpc_a[1] = a1;
    uint32_t req = m->rpc_req + 1;
    __asm__ volatile("" ::: "memory");
    m->rpc_req = req;
    while (m->rpc_ack != req)
        HW_BUS_YIELD();
    hw_w2_rpcs++;
    return m->rpc_r[0];
}

int hw_w2_GetTexSrca(int texnum, int *tw, int *th)
{
    int s = hw_w2_rpc(HW_W2_RPC_TEX, texnum, hw_w2_half_req);
    *tw = HW_W2_MAIL->rpc_r[1];
    *th = HW_W2_MAIL->rpc_r[2];
    return s;
}

int hw_w2_GetFlatSrca(int flatnum)
{
    return hw_w2_rpc(HW_W2_RPC_FLAT, flatnum, 0);
}

int hw_w2_GetFlatSrcaVariant(int flatnum, int variant)
{
    if (flatnum < 0 || flatnum >= (int)numflats || (variant != 1 && variant != 2))
        return 0;
    int f = flattranslation ? (int)flattranslation[flatnum] : flatnum;
    if (f >= 0 && f < (int)numflats && hw_w2_flat_tags)
    {
        const volatile v1_tag_t *t = &hw_w2_flat_tags[f];
        unsigned s = t->srca;
        if (s != 0 && s != V1_TAG_BAD)
        {
            unsigned v = (variant == 1) ? t->srca_mono : t->srca_64;
            if (v == V1_TAG_BAD)
                return 0;
            if (v != 0)
                return (int)v;
        }
    }
    return hw_w2_rpc(HW_W2_RPC_FLATVAR, flatnum, variant);
}

int hw_w2_FlatAvgColor(int flatnum)
{
    int c = Saturn_Vdp1_FlatAvgPeek(flatnum);
    return (c >= 0) ? c : hw_w2_rpc(HW_W2_RPC_FLATAVG, flatnum, 0);
}

byte *hw_w2_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs)
{
    byte *p = Saturn_SpritePatchPeek(s, w, h, lofs, tofs);
    if (p)
        return p;
    p = (byte *)(uintptr_t)hw_w2_rpc(HW_W2_RPC_SPRPATCH, s, 0);
    if (p)
    {
        volatile hw_w2_mail_t *m = HW_W2_MAIL;
        *w = m->rpc_r[1]; *h = m->rpc_r[2]; *lofs = m->rpc_r[3]; *tofs = m->rpc_r[4];
    }
    return p;
}

/* == Saturn_Vdp1Spr_Add's world-sprite branch, into this copy's cmd_m */
int hw_w2_SprAdd(int scache, int xA, int xB, int yt, int yb, int srcrow, int rows, int shade, int mark)
{
    int pw, h, srca;
    (void)mark;
    if (scache < 0 || scache >= (int)numsprites || !hw_w2_spr_tags)
        return 0;
    {
        const volatile v1_tag_t *t = &hw_w2_spr_tags[scache];
        unsigned s = t->srca;
        if (s == V1_TAG_BAD)
            return 0;
        if (s != 0)
        {
            srca = (int)s;
            pw = (int)t->w8 << 3;
            h = (int)t->h;
        }
        else
        {
            srca = hw_w2_rpc(HW_W2_RPC_SPR, scache, 0);
            pw = HW_W2_MAIL->rpc_r[1];
            h = HW_W2_MAIL->rpc_r[2];
        }
    }
    if (srca == 0)
        return 0;
    if (srcrow < 0)
        srcrow = 0;
    if (srcrow + rows > h)
        rows = h - srcrow;
    if (rows <= 0)
        return 0;
    srca += (srcrow * pw) / 8;
    int k = saturn_shade_bank[(shade < 0) ? 0 : ((shade > 31) ? 31 : shade)];
    uint32_t pmod = ((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu;
    uint32_t colr = ((uint32_t)k << 8) & 0xFFFFu;
    uint32_t size = (((uint32_t)pw / 8u) << 8) | ((uint32_t)rows & 0xFFu);
    if (saturn_cmd_m_count < CMDBUF_M_CAP)
    {
        uint32_t *cmd32 = (uint32_t *)&saturn_cmd_m[saturn_cmd_m_count];
        cmd32[0] = 0x00020000u;
        cmd32[1] = (pmod << 16) | colr;
        cmd32[2] = ((uint32_t)srca << 16) | size;
        cmd32[3] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yt;
        cmd32[4] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yt;
        cmd32[5] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yb;
        cmd32[6] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yb;
        cmd32[7] = 0;
        saturn_cmd_m_count++;
        saturn_v1w_cells++;
        return 1;
    }
    saturn_v1w_drop++;
    return 0;
}

/* Slave CPU, from its worker loop (saturn_hw_flats.c): one split frame. */
void hw_w2_entry(void)
{
    HW_PURGE_L1_CACHE();        /* the game and the master changed memory since last frame */
    volatile hw_w2_mail_t *m = HW_W2_MAIL;
    unsigned short t0 = SAT_FRT();
    hw_viewx = m->viewx;
    hw_viewy = m->viewy;
    hw_viewz = m->viewz;
    hw_viewangle = m->viewangle;
    hw_viewsin = m->viewsin;
    hw_viewcos = m->viewcos;
    hw_cam_x = m->cam_x;
    hw_cam_y = m->cam_y;
    hw_frust.cz_origin = m->cz_origin;
    hw_frust.cx_origin = m->cx_origin;
    hw_displayplayer = m->displayplayer;
    hw_w_x0 = (int16_t)m->x0;
    hw_w_x1 = (int16_t)m->x1;
    hw_w2_wall_tags = (v1_tag_t *)m->wall_tags;
    hw_w2_flat_tags = (v1_tag_t *)m->flat_tags;
    hw_w2_spr_tags  = (v1_tag_t *)m->spr_tags;
    hw_w2_cmd_m = (vdp1_cmd_t *)m->cmd_m;
    hw_w2_cmd_s = (vdp1_cmd_t *)m->cmd_s;
    hw_w2_sub_ranges = (sub_range_t *)m->ranges;
    hw_w2_rpcs = 0;
    const byte *pvs_row = m->pvs_row;
    HW_FrameSetup(pvs_row, NULL);
    HW_FrameWalk(pvs_row);
    m->m_count = hw_w2_cmd_m_count;
    m->s_count = hw_w2_cmd_s_count;
    m->visit_count = hw_w2_visit_count;
    m->flat_quads = hw_w2_cmd_s_count;
    m->needs_ytrim = hw_w2_needs_ytrim;
    m->sky_visible = saturn_hw_sky_visible;
    m->wall_quads = saturn_hw_wall_quads;
    m->wall_strips = saturn_hw_wall_strips;
    m->rpcs = hw_w2_rpcs;
    m->busy = (unsigned short)(SAT_FRT() - t0);
}

/* Master CPU, at level start (Saturn_HW_RenderInit): this copy's pool. */
#undef I_AllocHigh
extern byte *I_AllocHigh(int length);
void hw_w2_init(void)
{
    static uint8_t *pool0;
    if (!pool0)
        pool0 = (uint8_t *)I_AllocHigh(HW_W2_POOL);
    if (!hw_w2_pool)            /* first level: hand out the pool (kept for good) */
    {
        hw_w2_pool = pool0;
        hw_w2_pool_left = pool0 ? HW_W2_POOL : 0;
    }
}
#endif