/* saturn_hw_walls.c â€” 2D Map-Space AABB Frustum & 1D Solid-Span Culling,
 * Subsector Pillar Pair Projection, Near-Plane Seg Clipper, 1D Vertical Strip
 * & Monolithic VDP1 Wall Quad Emitters, and Subsector Flat Aperture Evaluator.
 */

/* Live light adjustment for the subsector being drawn (hw_sec_fast_t
 * light_delta, set in HW_RenderSubsectorBakedWalls). baked_light holds the
 * level-load sector light + fake contrast; adding the delta makes glowing /
 * flickering / strobing sectors and light-change linedefs reach walls. */
static int hw_wall_light_delta = 0;

HW_ALWAYS_INLINE int HW_SegLight16(const baked_seg_t *seg)
{
    int l = (int)(seg->baked_light >> 4) + hw_wall_light_delta;
    return (l < 0) ? 0 : ((l > 15) ? 15 : l);
}

#define BBOX_OUTSIDE        0
#define BBOX_INTERSECT_NEAR 1
#define BBOX_INTERSECT      2
#define BBOX_FULLY_INSIDE   3

#ifndef SATURN_WIN32
/* 0-stall, 3-register SH-2 inline assembly 2D frustum plane evaluator:
 * Computes (HW_BBOX_AT(bbox, d_ix)*px + HW_BBOX_AT(bbox, d_iy)*py + C) using only r0 + 2 scratch
 * registers, filling both muls.w latency slots with the next indexed loads (0 pipeline stalls, 0 callee-saved spills). */
#define HW_FRUST_DOT(bbox_ptr, f_ptr, d_ix, d_iy, d_px, d_py, d_c)                     \
    ({                                                                                 \
        int32_t _res, _tmp, _r0;                                                       \
        __asm__(                                                                       \
            "mov.b @(" #d_ix ",%3), %2 \n\t"                                           \
            "mov.w @(%2,%4), %1        \n\t"                                           \
            "mov.w @(" #d_px ",%3), %2 \n\t"                                           \
            "muls.w %1, %2             \n\t"                                           \
            "mov.b @(" #d_iy ",%3), %2 \n\t"                                           \
            "mov.w @(%2,%4), %1        \n\t"                                           \
            "mov.w @(" #d_py ",%3), %2 \n\t"                                           \
            "sts   macl, %0            \n\t"                                           \
            "muls.w %1, %2             \n\t"                                           \
            "mov.l @(" #d_c ",%3), %1  \n\t"                                           \
            "add   %1, %0              \n\t"                                           \
            "sts   macl, %1            \n\t"                                           \
            "add   %1, %0              \n\t"                                           \
            : "=&r"(_res), "=&r"(_tmp), "=&z"(_r0)                                     \
            : "r"(f_ptr), "r"(bbox_ptr)                                                \
            : "macl"                                                                   \
        );                                                                             \
        _res;                                                                          \
    })
#endif

/* Fast 2D Map-Space AABB Frustum Test with Full-Accept & Near-Intersect Detection.
 * Evaluates exact bounding box boundaries against 2D frustum planes in map space.
 * Returns BBOX_OUTSIDE (0) if guaranteed outside view frustum (culled).
 * Returns BBOX_INTERSECT_NEAR (1) if viewer is inside box or box pierces near plane (cannot be 1D-occlusion culled).
 * Returns BBOX_INTERSECT (2) if intersects frustum completely in front of near plane.
 * Returns BBOX_FULLY_INSIDE (3) if 100% of bounding box is inside view frustum and in front of near plane. */
static HW_NOINLINE HW_HOT_O2 int HW_CheckBBoxStatus(const int16_t *bbox)
{
    int bl = bbox[BOXLEFT];
    int br = bbox[BOXRIGHT];
    int bb = bbox[BOXBOTTOM];
    int bt = bbox[BOXTOP];

    if (bl > br || bb > bt)
        return BBOX_INTERSECT_NEAR;

    int vx = hw_cam_x;
    int vy = hw_cam_y;

    if (vx >= bl && vx <= br && vy >= bb && vy <= bt)
        return BBOX_INTERSECT_NEAR;

    const hw_frust_t *f = &hw_frust;

#ifndef SATURN_WIN32
    /* 1. Near Plane FIRST: Instantly eliminates the entire half of the map behind the player */
    if (HW_FRUST_DOT(bbox, f, 0, 1, 12, 14, 24) < (HW_NEAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;

    /* 2. Left & Right Frustum Planes */
    if (HW_FRUST_DOT(bbox, f, 2, 3, 16, 18, 28) < -(16 << 12))
        return BBOX_OUTSIDE;

    if (HW_FRUST_DOT(bbox, f, 4, 5, 20, 22, 32) < -(16 << 12))
        return BBOX_OUTSIDE;

    /* Far-plane test: evaluated after lateral side planes to cull peripheral nodes first */
    int32_t d_near_min = HW_FRUST_DOT(bbox, f, 6, 7, 12, 14, 24);
    if (d_near_min > (HW_FAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;
    if (d_near_min < (HW_NEAR_Z_UNITS << 12))
        return BBOX_INTERSECT_NEAR;

    if (d_near_min >= (16 << 12) &&
        HW_FRUST_DOT(bbox, f, 8, 9, 16, 18, 28) >= 0 &&
        HW_FRUST_DOT(bbox, f, 10, 11, 20, 22, 32) >= 0)
    {
        return BBOX_FULLY_INSIDE;
    }
#else
    /* 1. Near Plane FIRST: Instantly eliminates the entire half of the map behind the player */
    if (HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->idx_Nx), f->Nx,
                         HW_BBOX_AT(bbox, f->idx_Ny), f->Ny, f->Cn) < (HW_NEAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;

    /* 2. Left & Right Frustum Planes */
    if (HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->idx_Lx), f->Lx,
                         HW_BBOX_AT(bbox, f->idx_Ly), f->Ly, f->Cl) < -(16 << 12))
        return BBOX_OUTSIDE;

    if (HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->idx_Rx), f->Rx,
                         HW_BBOX_AT(bbox, f->idx_Ry), f->Ry, f->Cr) < -(16 << 12))
        return BBOX_OUTSIDE;

    /* Far-plane test: evaluated after lateral side planes to cull peripheral nodes first */
    int32_t d_near_min = HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Nx), f->Nx,
                                          HW_BBOX_AT(bbox, f->nidx_Ny), f->Ny, f->Cn);
    if (d_near_min > (HW_FAR_Z_UNITS << 12))
        return BBOX_OUTSIDE;
    if (d_near_min < (HW_NEAR_Z_UNITS << 12))
        return BBOX_INTERSECT_NEAR;

    if (d_near_min >= (16 << 12))
    {
        if (HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Lx), f->Lx,
                             HW_BBOX_AT(bbox, f->nidx_Ly), f->Ly, f->Cl) >= 0 &&
            HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Rx), f->Rx,
                             HW_BBOX_AT(bbox, f->nidx_Ry), f->Ry, f->Cr) >= 0)
        {
            return BBOX_FULLY_INSIDE;
        }
    }
#endif

    return BBOX_INTERSECT;
}

/* 1D Solid Wall Bounding Box Screen Occlusion Test:
 * Projects the 2 silhouette corners of a 2D bounding box to screen X coordinates [min_x, max_x].
 * If any corner crosses the near-plane, the box intersects or is behind the view screen
 * and cannot safely be culled, returning 0 immediately.
 * If every column in [min_x, max_x] is covered by solid 1-sided walls, the entire
 * box (and all geometry/subsectors/nodes inside it) is 100% occluded and skipped.
 * Returns 1 if 100% occluded, 0 if potentially visible. */
static HW_NOINLINE HW_HOT_O2 int HW_CheckBBoxOccluded(const int16_t *bbox, int near_verified)
{
    if (!hw_solid_count) return 0;
    if (hw_solid_count >= saturn_view_w) return 1;

    const hw_frust_t *f = &hw_frust;

    /* Ensure the closest corner doesn't pierce the near plane (5-cycle 16x16 MAC).
     * Skipped when HW_CullBSPBBox already verified d_near_min >= (HW_NEAR_Z_UNITS << 12). */
#ifndef SATURN_WIN32
    if (!near_verified && HW_FRUST_DOT(bbox, f, 6, 7, 12, 14, 24) < (HW_NEAR_Z_UNITS << 12))
        return 0;
#else
    if (!near_verified &&
        HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Nx), f->Nx,
                         HW_BBOX_AT(bbox, f->nidx_Ny), f->Ny, f->Cn) < (HW_NEAR_Z_UNITS << 12))
        return 0;
#endif

    int bl = bbox[BOXLEFT];
    int br = bbox[BOXRIGHT];
    int bb = bbox[BOXBOTTOM];
    int bt = bbox[BOXTOP];
    int cam_x = hw_cam_x, cam_y = hw_cam_y;

    int boxpos = (cam_x <= bl) ? 0 : ((cam_x < br) ? 1 : 2);
    if (cam_y < bt)
        boxpos |= (cam_y > bb) ? 4 : 8;
    if (__builtin_expect(boxpos == 5, 0))
        return 0;

    /* Two-corner silhouette lookup table (Carmack R_CheckBBox), pre-scaled by 2 for byte indexing */
    static const int8_t checkcoord[12][4] = {
        { BOXRIGHT * 2, BOXTOP * 2,    BOXLEFT * 2,  BOXBOTTOM * 2 }, /* 0: top-left */
        { BOXRIGHT * 2, BOXTOP * 2,    BOXLEFT * 2,  BOXTOP * 2 },    /* 1: top-center */
        { BOXRIGHT * 2, BOXBOTTOM * 2, BOXLEFT * 2,  BOXTOP * 2 },    /* 2: top-right */
        { 0, 0, 0, 0 },
        { BOXLEFT * 2,  BOXTOP * 2,    BOXLEFT * 2,  BOXBOTTOM * 2 }, /* 4: mid-left */
        { 0, 0, 0, 0 },                                               /* 5: inside */
        { BOXRIGHT * 2, BOXBOTTOM * 2, BOXRIGHT * 2, BOXTOP * 2 },    /* 6: mid-right */
        { 0, 0, 0, 0 },
        { BOXLEFT * 2,  BOXTOP * 2,    BOXRIGHT * 2, BOXBOTTOM * 2 }, /* 8: bot-left */
        { BOXLEFT * 2,  BOXBOTTOM * 2, BOXRIGHT * 2, BOXBOTTOM * 2 }, /* 9: bot-center */
        { BOXLEFT * 2,  BOXBOTTOM * 2, BOXRIGHT * 2, BOXTOP * 2 },    /* 10: bot-right */
        { 0, 0, 0, 0 }
    };

    const int8_t *cc = checkcoord[boxpos];

    /* Project only the 2 extreme silhouette corners using 1-cycle 16x16->32 muls.w */
    int dx1 = (int)HW_BBOX_AT(bbox, cc[0]) - cam_x;
    int dy1 = (int)HW_BBOX_AT(bbox, cc[1]) - cam_y;
    int dx2 = (int)HW_BBOX_AT(bbox, cc[2]) - cam_x;
    int dy2 = (int)HW_BBOX_AT(bbox, cc[3]) - cam_y;
    int nx = f->Nx, ny = f->Ny;
    int neg_nx = -nx;
    int32_t cz_off = f->cz_off12;
    int32_t cx_off = f->cx_off12;

    fixed_t cz1 = HW_MulsW_PairAdd(dx1, nx, dy1, ny, cz_off) << 4;
    if (cz1 < HW_NEAR_Z)
        return 0;
    fixed_t cz2 = HW_MulsW_PairAdd(dx2, nx, dy2, ny, cz_off) << 4;
    if (cz2 < HW_NEAR_Z)
        return 0;
    fixed_t cx1 = HW_MulsW_PairAdd(dx1, ny, dy1, neg_nx, cx_off) << 4;
    fixed_t cx2 = HW_MulsW_PairAdd(dx2, ny, dy2, neg_nx, cx_off) << 4;

    int s1 = HW_FastDiv(cz1);
    int s2 = HW_FastDiv(cz2);
    int px1, px2;
    HW_ProjXPair(cx1, s1, cx2, s2, HW_VIEW_CX, &px1, &px2);

    int min_x, max_x;
    if (px1 < px2) { min_x = px1; max_x = px2; }
    else           { min_x = px2; max_x = px1; }

    if (max_x < -1 || min_x > HW_VIEW_W + 1) return 1;
    if (max_x <= 0 || min_x >= HW_VIEW_W) return 0;

    min_x -= 1;
    max_x += 1;
    if (min_x < 0) min_x = 0;
    if (max_x > HW_VIEW_W) max_x = HW_VIEW_W;
    return HW_IsClippedSpanOccluded(min_x, max_x);
}

/* Combined BSP Node Bounding Box Frustum + 1D Occlusion Cull:
 * Returns -1 if culled, 0 if visible (not fully inside), 1 if visible (fully inside). */
static HW_NOINLINE HW_HOT_O2 int HW_CullBSPBBox(const int16_t *bbox, int fully_inside)
{
    int near_verified = 0;
    HW_VS(2);
    if (!fully_inside)
    {
        int st = HW_CheckBBoxStatus(bbox);
        if (st == BBOX_OUTSIDE)
        {
            HW_VS(3);
            return -1;
        }
        if (st == BBOX_INTERSECT_NEAR)
            return 0;
        if (st == BBOX_FULLY_INSIDE)
            fully_inside = 1;
        near_verified = 1;
    }
    if (hw_solid_count > 0 && saturn_hw_bsp_occl != 2 && HW_CheckBBoxOccluded(bbox, near_verified))
    {
        saturn_hw_occl_culls++;
        HW_VS(4);
        return -1;
    }
    return fully_inside;
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
    /* Bytes 0..7: hot uint8_t fields -> 1-insn mov.b @(0..7, r8), r0 */
    uint8_t                  portal_recorded;
    uint8_t                  was_near_clipped;
    uint8_t                  occl_state;
    uint8_t                  is_sky_ceil;
    uint8_t                  n_vis;
    uint8_t                  top_overlap;
    uint8_t                  bot_overlap;
    uint8_t                  needs_near_clip;
    /* Bytes 8..31: 12 hot int16_t/uint16_t fields -> 1-insn mov.w @(8..30, r8), r0 */
    int16_t                  seg_sx;
    int16_t                  seg_ex;
    int16_t                  ytop_min;
    int16_t                  ybot_max;
    int16_t                  ya0;
    int16_t                  ya1;
    int16_t                  yb0;
    int16_t                  yb1;
    uint16_t                 srca;
    int16_t                  th;
    int16_t                  wall_h;
    int16_t                  cur_v;
    /* Bytes 32..63: 8 hot 32-bit fields -> 1-insn mov.l @(32..60, r8), Rn */
    fixed_t                  sec_ceiling;
    fixed_t                  sec_floor;
    const hw_trans_pillar_t *p0;
    const hw_trans_pillar_t *p1;
    const baked_seg_t       *seg;
    int                      dz_top;
    int                      dz_bot;
    int                      cur_tex_id;
    /* Bytes 64..123: quad emission context fields (hot strip fields within @(0..56, r8+64)) */
    uint8_t                  should_subdivide;
    uint8_t                  needs_clip;
    uint8_t                  persp_strips;
    uint8_t                  pad_b67;
    int16_t                  tw;
    uint16_t                 srca_mono;
    uint16_t                 srca_64;
    uint16_t                 sub_m_start;
    int16_t                  last_clip_x0;
    int16_t                  last_clip_x1;
    int16_t                  mono_tw;
    uint16_t                 mono_base_srca;
    int16_t                  rem_h;
    int16_t                  q_xa;
    int16_t                  q_xb;
    int16_t                  rows;
    int                      u_start;
    int                      u_span;
    uint32_t                 inv_u_span;
    uint32_t                 cmd_header1;
    const baked_seg_t       *seg_end;
    fixed_t                  t0;
    fixed_t                  t1;
    uint16_t                 portal_first;
    uint16_t                 portal_count;
} hw_wall_emit_ctx_t;

static hw_wall_emit_ctx_t hw_wctx;
static uint16_t hw_wctx_tex_frame = 0;
static const saturn_wl_pillar_t *hw_cur_pillars = NULL;

/* DEFERRED WALL EMISSION (2026-11-15, the 60 FPS work).
 *
 * 5thPlanet showed the master's render code running at CPI ~2.5-3.6
 * (cache-resident code is ~1.3): the BSP walk ran visibility, occlusion,
 * texture lookup, quad emission and sprites per subsector, ~30 functions
 * that together are far bigger than the 4 KB cache, so every subsector
 * refetched all of them. Now the walk (pass 1) only decides visibility and
 * occlusion and RECORDS each visible wall (hw_wall_rec_t: everything the
 * emitter reads, the projected pillars copied by value because
 * local_trans_pillars is per-subsector scratch). Pass 2
 * (HW_EmitDeferredWalls, right after the walk, while the slave is still on
 * flats) emits all wall quads and sprites in visit order with a much
 * smaller code footprint.
 *
 * Output is unchanged: each visit's m range still holds its walls then its
 * sprites (Deliver reads per-visit ranges, so where they sit in cmd_m does
 * not matter), and the strip-level / sprite occlusion tests run against a
 * snapshot of the solid mask as it was at that subsector. A subsector that
 * does not fit the record pool, or needs a snapshot when the snapshot pool
 * is full, is emitted immediately like before.
 *
 * RESULT (5thPlanet, E1M1 start, 2026-11-15): neutral on the master alone.
 * render 22.04 -> 22.28 ms/frame. Pass 2's emitters did get cache-resident
 * (HW_EmitWallStrips_Linear_General CPI 2.43 -> 1.78, HW_EmitBakedWallQuads
 * 3.07 -> 1.83), but recording + the pass-1 bookkeeping cost as much, and
 * the walk's own functions stayed at CPI ~2.5-3.7 (HighWRAM fetch stalls
 * only -5%). Kept, OFF by default, as the base for running pass 2 on the
 * half-idle slave (it needs no texture uploads: pass 1 touches them).
 * Verified output-identical to the immediate path (DOOM-Windows -pos,
 * settled frames, 5 spots incl. E1M1 nukage pit + computer room). */
#ifndef HW_DEFER_WALLS
#define HW_DEFER_WALLS 0
#endif
#ifndef HW_DEFER_REC_CAP
#define HW_DEFER_REC_CAP  320
#endif
#define HW_DEFER_SUB_CAP  CMDBUF_MAX_VISIT
#ifndef HW_DEFER_SNAP_CAP
#define HW_DEFER_SNAP_CAP 96
#endif
#ifndef HW_DEFER_ALWAYS_SNAP
#define HW_DEFER_ALWAYS_SNAP 0
#endif

typedef struct {
    const baked_seg_t *seg;
    hw_trans_pillar_t  p0, p1;
    int                dz_top, dz_bot;
    int                u_start, u_span;
    uint32_t           inv_u_span;
    int16_t            ya0, ya1, yb0, yb1;
    int16_t            seg_sx, seg_ex, ytop_min, ybot_max;
    int16_t            wall_h, cur_v;
    uint8_t            top_overlap, bot_overlap, was_near_clipped, occl_state;
} hw_wall_rec_t;

typedef struct {
    uint16_t  spr_start;    /* sprite quads emitted in pass 1 (cmd_m range), */
    uint16_t  spr_count;    /* moved after the walls in pass 2 */
    uint16_t  visit_idx;
    uint16_t  subnum;
    uint16_t  rec_first;
    uint16_t  rec_count;
    int16_t   light_delta;
    int16_t   snap;         /* hw_defer_snap index, -1 = none needed */
} hw_wall_sub_t;

typedef struct {
    uint32_t mask[HW_SOLID_WORDS];
    uint32_t summary, any;      /* hw_solid_summary / hw_solid_any (occl queries read them) */
    int      count;
} hw_solid_snap_t;

#if HW_DEFER_WALLS && !defined(SATURN_WIN32)
/* Low WRAM (saturn_memmap.h): the High WRAM BSS has no room for ~37 KB
 * (2026-11-17: also without HW_SLAVE_WALLS - the BSS copy overran the
 * 0x060C0000 ceiling by 38 KB) */
#define HW_DEFER_REC_BYTES  ((uint32_t)sizeof(hw_wall_rec_t) * HW_DEFER_REC_CAP)
#define HW_DEFER_SUB_BYTES  ((uint32_t)sizeof(hw_wall_sub_t) * HW_DEFER_SUB_CAP)
#define HW_DEFER_SNAP_BYTES ((uint32_t)sizeof(hw_solid_snap_t) * HW_DEFER_SNAP_CAP)
_Static_assert(((sizeof(hw_wall_rec_t) * HW_DEFER_REC_CAP + sizeof(hw_wall_sub_t) * HW_DEFER_SUB_CAP + 15u) & ~15u)
               + sizeof(hw_solid_snap_t) * HW_DEFER_SNAP_CAP <= SATURN_WRAM_L_DEFER_SIZE,
               "deferred-wall queue must fit SATURN_WRAM_L_DEFER");
#define hw_defer_rec  ((hw_wall_rec_t *)SATURN_WRAM_L_DEFER_ADDR)
#define hw_defer_sub  ((hw_wall_sub_t *)(SATURN_WRAM_L_DEFER_ADDR + HW_DEFER_REC_BYTES))
#define hw_defer_snap ((hw_solid_snap_t *)(SATURN_WRAM_L_DEFER_ADDR + \
                       ((HW_DEFER_REC_BYTES + HW_DEFER_SUB_BYTES + 15u) & ~15u)))
#else
static hw_wall_rec_t   hw_defer_rec[HW_DEFER_REC_CAP];
static hw_wall_sub_t   hw_defer_sub[HW_DEFER_SUB_CAP];
static hw_solid_snap_t hw_defer_snap[HW_DEFER_SNAP_CAP];
#endif
static int     hw_defer_nrec = 0, hw_defer_nsub = 0, hw_defer_nsnap = 0;
static int     hw_defer_on = 0;         /* this subsector records instead of emitting */
static int     hw_defer_tex_seen = -1;  /* last wall texture touched in pass 1 */
static uint8_t hw_defer_need_snap = 0;  /* a record needs strip-level occlusion */

/* Cold helper for clipped/clamped 16-texel strips (du_vis < 16, UserClip reset, or XY DDA overflow).
 * Takes 4 register arguments (r4=s, r5=u_lo_and_hi, r6=strip_cmd1, r7=strip_cmd2), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 int HW_EmitStripQuad_Slow(
    int s, uint32_t u_lo_and_hi, uint32_t strip_cmd1, uint32_t strip_cmd2)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    int u_lo = (int)(uint16_t)u_lo_and_hi;
    int u_hi = (int)(u_lo_and_hi >> 16);
    int s_q_xa  = hw_wall_coord_tmp[0];
    int s_q_xb  = hw_wall_coord_tmp[1];
    int s_q_ya0 = hw_wall_coord_tmp[2];
    int s_q_yb0 = hw_wall_coord_tmp[3];
    int s_q_ya1 = hw_wall_coord_tmp[4];
    int s_q_yb1 = hw_wall_coord_tmp[5];
    int lc_x0   = ctx->last_clip_x0;
    int lc_x1   = ctx->last_clip_x1;
    int need_narrow_clip = 0;
    int du_vis = u_hi - u_lo;

    if (du_vis < 16)
    {
        int dx_vis = s_q_xb - s_q_xa;
        if (dx_vis > 2)
        {
            int cl_x0 = (s_q_xa < 0) ? 0 : (s_q_xa > (HW_VIEW_W - 1) ? (HW_VIEW_W - 1) : s_q_xa);
            int cl_x1 = (s_q_xb < 0) ? 0 : (s_q_xb > (HW_VIEW_W - 1) ? (HW_VIEW_W - 1) : s_q_xb);
            if (cl_x0 >= cl_x1)
                return 0;

            int du0 = u_lo - (s << 4);
            int du1 = ((s + 1) << 4) - u_hi;
            int dya0 = s_q_yb0 - s_q_ya0;
            int dya1 = s_q_yb1 - s_q_ya1;
            int inv_du = (du_vis > 0) ? saturn_inv_du_vis_lut[du_vis] : 4096;

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

            if ((s_q_xa < cl_x0 && cl_x0 > 0) || (s_q_xb > cl_x1 && cl_x1 < HW_VIEW_W - 1))
            {
                need_narrow_clip = 1;
                if (cl_x0 != lc_x0 || cl_x1 != lc_x1)
                {
                    HW_EmitUserClip(cl_x0, 0, cl_x1, HW_VIEW_H - 1);
                    lc_x0 = cl_x0;
                    lc_x1 = cl_x1;
                }
            }
        }
    }

    if (!need_narrow_clip && lc_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        lc_x0 = -1;
        lc_x1 = -1;
    }
    ctx->last_clip_x0 = (int16_t)lc_x0;
    ctx->last_clip_x1 = (int16_t)lc_x1;

    HW_WALL_XYCLAMP(s_q_xa, s_q_xb, s_q_ya0, s_q_yb0, s_q_ya1, s_q_yb1);

    uint16_t m_cnt = saturn_cmd_m_count;
    if (m_cnt < CMDBUF_M_CAP - 1)
    {
        saturn_cmd_m_count = m_cnt + 1;
        aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
        uint32_t s_xa_p = ((uint32_t)s_q_xa << 16);
        uint32_t s_xb_p = ((uint32_t)s_q_xb << 16);
        cmd32[0] = 0x00020000u;
        cmd32[1] = strip_cmd1;
        cmd32[2] = strip_cmd2;
        cmd32[3] = s_xa_p | (uint16_t)s_q_ya0;
        cmd32[4] = s_xb_p | (uint16_t)s_q_yb0;
        cmd32[5] = s_xb_p | (uint16_t)s_q_yb1;
        cmd32[6] = s_xa_p | (uint16_t)s_q_ya1;
        cmd32[7] = 0;
        return 1;
    }
    saturn_v1w_drop++;
    saturn_v1w_captured = 1;
    return 0;
}

static HW_NOINLINE HW_HOT_O2 int HW_IsStripOccluded_Slow(int s_px0, int s_px1, int coal_strips)
{
    int occ_x0 = (s_px0 < 0) ? 0 : s_px0;
    int occ_x1 = (s_px1 > HW_VIEW_W) ? HW_VIEW_W : s_px1;
    if (HW_CheckSpanOcclusion(occ_x0, occ_x1) == 2)
    {
        if (!coal_strips && hw_wctx.last_clip_x0 != -1)
        {
            HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
            hw_wctx.last_clip_x0 = -1;
            hw_wctx.last_clip_x1 = -1;
        }
        return 1;
    }
    return 0;
}

static HW_NOINLINE HW_COLD_OS int HW_TryCoalesceLinearStrip(
    int s, int s_px0, uint32_t cmd_w2_cur)
{
    const hw_wall_emit_ctx_t *ctx = &hw_wctx;
    int u_start = ctx->u_start;
    if ((u_start & 15) != 0 && s == (u_start >> 4))
        return 0;

    int cur_px_fp = hw_wall_coord_tmp[0];
    int step_px_fp = hw_wall_coord_tmp[1];
    uint32_t cmd_w2_base = (uint32_t)hw_wall_coord_tmp[2];
    int tw = ctx->tw;
    int th = ctx->th;
    int srca = ctx->srca;
    int srca_mono = ctx->srca_mono;
    int srca_64 = ctx->srca_64;
    int cur_v = ctx->cur_v;
    int u_end = u_start + ctx->u_span;
    int strip_srca_step = th << 1;
    int num_strips = (tw > 0) ? ((tw + 15) >> 4) : 1;
    int strip_mask = num_strips - 1;
    int cur_strip_idx = __builtin_expect((num_strips & strip_mask) == 0, 1)
        ? (s & strip_mask)
        : HW_ModNonPow2(s, num_strips);
    int coal_strips = 0;
    int emit_srca = 0;
    uint32_t emit_size_hi = 2u << 8;

    if ((s << 4) < u_start)
        return 0;

    if (tw >= 64 && (srca_64 != srca || srca_mono != srca) && (cur_strip_idx & 3) == 0 && ((s + 4) << 4) <= u_end)
    {
        coal_strips = 4;
        emit_srca = srca_64 + (cur_v << 3) + (cur_strip_idx >> 2) * (th << 3);
        emit_size_hi = 8u << 8;
    }
    else if (tw == 32 && srca_mono != srca && cur_strip_idx == 0 && ((s + 2) << 4) <= u_end)
    {
        coal_strips = 2;
        emit_srca = srca_mono + (cur_v << 2);
        emit_size_hi = 4u << 8;
    }
    else
    {
        return 0;
    }

    int adv = coal_strips - 1;
    int c_uhi = (s + coal_strips) << 4;
    int next_px1 = (c_uhi >= u_end) ? ctx->p1->px : ((cur_px_fp + step_px_fp * adv) >> 16);
    int max_coal_dx = (coal_strips == 4 && srca_64 == srca) ? 52 : 1000;
    if (next_px1 - s_px0 > max_coal_dx)
        return 0;

    if (coal_strips == 4 && (next_px1 - s_px0 <= 20 || srca_64 == srca) && srca_mono != srca)
    {
        emit_srca = srca_mono + (cur_v << 2) + (cur_strip_idx >> 2) * (th << 2);
        emit_size_hi = 4u << 8;
    }
    hw_wall_coord_tmp[0] = (int)(((uint32_t)emit_srca << 16) | emit_size_hi | (cmd_w2_base & 0xFFu));
    hw_wall_coord_tmp[1] = (int)(cmd_w2_cur + ((uint32_t)strip_srca_step << 16) * (uint32_t)adv);
    return coal_strips;
}

/* Wrap a strip's packed (SRCA << 16 | size) word back into the texture
 * (2026-11-16 hang fix). The old test `cur >= base + wrap` overflows 32 bits
 * when the texture sits high in VDP1 VRAM (SRCA >= ~0xF000 for wide
 * textures): `base + wrap` wraps to a small value, the test is always true,
 * and the `while` form spun forever (benchmark tour, E1M1; the fast paths'
 * `if` form instead stepped to wrong texture columns). Comparing the
 * offset from base cannot overflow. */
#define HW_W2_WRAP(cur, base, wrap)                                     \
    do {                                                                \
        uint32_t w_ = (wrap);                                           \
        if (w_ != 0u)                                                   \
            while ((uint32_t)((cur) - (base)) >= w_)                    \
                (cur) -= w_;                                            \
    } while (0)

/* Compact rewrite stage B2: HW_EmitWallStrips_Linear_General in assembly
 * (saturn_hw_asm.s hw_asm_lingen, same arithmetic; the rare per-strip cases
 * call the C helpers through this table). */
#ifndef HW_ASM_LINGEN
#define HW_ASM_LINGEN (HW_ASM && !HW_VIS_STATS)
#endif
#if HW_ASM_LINGEN
extern void hw_asm_lingen(hw_wall_emit_ctx_t *ctx, int s_min, int s_max);
_Static_assert(offsetof(hw_wall_emit_ctx_t, occl_state) == 2 && offsetof(hw_wall_emit_ctx_t, ytop_min) == 12 &&
               offsetof(hw_wall_emit_ctx_t, ybot_max) == 14 && offsetof(hw_wall_emit_ctx_t, yb0) == 20 &&
               offsetof(hw_wall_emit_ctx_t, yb1) == 22 && offsetof(hw_wall_emit_ctx_t, p1) == 44 &&
               offsetof(hw_wall_emit_ctx_t, rows) == 90 && offsetof(hw_wall_emit_ctx_t, inv_u_span) == 100 &&
               offsetof(hw_wall_emit_ctx_t, cmd_header1) == 104 && offsetof(hw_wall_emit_ctx_t, last_clip_x0) == 76 &&
               offsetof(hw_wall_emit_ctx_t, tw) == 68 && offsetof(hw_wall_emit_ctx_t, u_start) == 92 &&
               CMDBUF_M_CAP == 1280 && sizeof(vdp1_cmd_t) == 32, "hw_asm_lingen: layout");
const void *const hw_asm_lingen_ctx[12] = {
    &saturn_cmd_m, (const void *)&saturn_cmd_m_count, &saturn_v1w_cells, &saturn_hw_wall_quads,
    &saturn_hw_wall_strips, &saturn_v1w_drop, &saturn_v1w_captured, hw_wall_coord_tmp,
    (const void *)HW_TryCoalesceLinearStrip, (const void *)HW_IsStripOccluded_Slow,
    (const void *)HW_EmitStripQuad_Slow, (const void *)HW_ModNonPow2
};
#endif

#if !HW_ASM_LINGEN || HW_ASM_CHECK
static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Linear_General(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const int u_start = ctx->u_start;
    const int u_end   = u_start + ctx->u_span;
    const int tw = ctx->tw;
    const int th = ctx->th;
    const int srca = ctx->srca;

    int s_px0   = ctx->p0->px;
    int s_ya0   = ctx->ya0;
    int s_ya1   = ctx->ya1;
    int end_px  = ctx->p1->px;
    int end_ya0 = ctx->yb0;
    int end_ya1 = ctx->yb1;

    int can_coal = (tw == 32 && ctx->srca_mono != srca && (((u_start + 31) & ~31) + 32 <= u_end)) |
                   (tw >= 64 && ((tw & 63) == 0) && (ctx->srca_64 != srca || ctx->srca_mono != srca) && (((u_start + 63) & ~63) + 64 <= u_end));
    int wall_needs_clamp =
        ((((unsigned int)(s_px0 + 500) | (unsigned int)(500 - end_px) |
           (unsigned int)(s_ya1 - s_ya0) | (unsigned int)(end_ya1 - end_ya0)) > 1022u) ||
         HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1));
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;
    int band_all_vis = (s_px0 >= 0) & (end_px <= HW_VIEW_W) &
                       (s_ya0 < s_ya1) & (end_ya0 < end_ya1) &
                       (s_ya1 > ytop_min) & (end_ya1 > ytop_min) &
                       (s_ya0 < ybot_max) & (end_ya0 < ybot_max);
    int partial_first = (u_start & 15) != 0;
    int partial_last  = (u_end & 15) != 0;
    int has_occl      = (ctx->occl_state != 0);
    int wall_slow     = (!band_all_vis) | has_occl | wall_needs_clamp;
    int first_slow    = wall_slow | partial_first | (ctx->last_clip_x0 != -1);

#if HW_VIS_STATS
    /* [34] linear-general walls [35] can_coal [36] wall_slow [37] partial
     * first [38] partial last [39] none of these (simple fast wall) */
    HW_VS(34);
    if (can_coal) HW_VS(35);
    if (wall_slow) HW_VS(36);
    if (partial_first) HW_VS(37);
    if (partial_last) HW_VS(38);
    if (!can_coal && !wall_slow && !partial_first && !partial_last && ctx->last_clip_x0 == -1) HW_VS(39);
#endif
    uint32_t lin_flags = (uint32_t)can_coal |
                         ((uint32_t)first_slow << 1) |
                         ((uint32_t)band_all_vis << 2) |
                         ((uint32_t)wall_slow << 3) |
                         ((uint32_t)partial_first << 4) |
                         ((uint32_t)partial_last << 5) |
                         ((uint32_t)has_occl << 6) |
                         ((uint32_t)wall_needs_clamp << 7);

    int num_strips = (tw > 0) ? ((tw + 15) >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int strip_mask = num_strips - 1;
    int cur_strip_idx = __builtin_expect((num_strips & strip_mask) == 0, 1)
        ? (s_min & strip_mask)
        : HW_ModNonPow2(s_min, num_strips);

    uint32_t size_lo = (2u << 8) | ((uint32_t)ctx->rows & 0xFFu);
    uint32_t srca_v = (uint32_t)(srca + (ctx->cur_v << 1));
    uint32_t cmd_w2_base = (srca_v << 16) | size_lo;
    uint32_t cmd_w2_step = (uint32_t)th << 17;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;

    int cur_px_fp = 0, step_px_fp = 0;
    int cur_ya0_fp = 0, step_ya0_fp = 0;
    int cur_ya1_fp = 0, step_ya1_fp = 0;

    if (s_min < s_max)
    {
        uint32_t inv_u_span = ctx->inv_u_span;
        int step_t1  = (int)(inv_u_span >> 4);
        int d_px  = end_px  - s_px0;
        int d_ya0 = end_ya0 - s_ya0;
        int d_ya1 = end_ya1 - s_ya1;
        step_px_fp  = d_px  * step_t1;
        step_ya0_fp = d_ya0 * step_t1;
        step_ya1_fp = d_ya1 * step_t1;
        if (__builtin_expect((u_start & 15) == 0, 1))
        {
            cur_px_fp  = (s_px0 << 16) + 0x8000 + step_px_fp;
            cur_ya0_fp = (s_ya0 << 16) + 0x8000 + step_ya0_fp;
            cur_ya1_fp = (s_ya1 << 16) + 0x8000 + step_ya1_fp;
        }
        else
        {
            int first_du = 16 - (u_start & 15);
            int first_t1 = (int)(((uint32_t)first_du * inv_u_span) >> 8);
            cur_px_fp  = (s_px0 << 16) + 0x8000 + d_px  * first_t1;
            cur_ya0_fp = (s_ya0 << 16) + 0x8000 + d_ya0 * first_t1;
            cur_ya1_fp = (s_ya1 << 16) + 0x8000 + d_ya1 * first_t1;
        }
    }

    for (int s = s_min; s <= s_max; s++)
    {
        int coal_strips = 0;
        uint32_t emit_cmd2 = cmd_w2_cur;
        if (__builtin_expect((lin_flags & 1u) != 0 && ((s & 1) == 0) && (s + 1 <= s_max), 0))
        {
            hw_wall_coord_tmp[0] = cur_px_fp;
            hw_wall_coord_tmp[1] = step_px_fp;
            hw_wall_coord_tmp[2] = (int)cmd_w2_base;
            coal_strips = HW_TryCoalesceLinearStrip(s, s_px0, cmd_w2_cur);
            if (coal_strips > 0)
            {
                int adv = coal_strips - 1;
                emit_cmd2   = (uint32_t)hw_wall_coord_tmp[0];
                cmd_w2_cur  = (uint32_t)hw_wall_coord_tmp[1];
                cur_px_fp  += step_px_fp  * adv;
                cur_ya0_fp += step_ya0_fp * adv;
                cur_ya1_fp += step_ya1_fp * adv;
                s          += adv;
            }
        }

        int s_px1, s_yb0, s_yb1;
        if (__builtin_expect(s < s_max, 1))
        {
            s_px1 = cur_px_fp  >> 16;
            s_yb0 = cur_ya0_fp >> 16;
            s_yb1 = cur_ya1_fp >> 16;
            cur_px_fp  += step_px_fp;
            cur_ya0_fp += step_ya0_fp;
            cur_ya1_fp += step_ya1_fp;
        }
        else
        {
            s_px1 = ctx->p1->px;
            s_yb0 = ctx->yb0;
            s_yb1 = ctx->yb1;
            lin_flags |= (lin_flags >> 4) & 2u; /* if partial_last (bit 5), set cur_slow (bit 1) */
        }

        if (s_px1 > s_px0)
        {
            if (__builtin_expect((lin_flags & 2u) != 0, 0))
            {
                int is_partial_first = ((lin_flags & 16u) != 0);
                lin_flags &= ~16u;
                if (!(lin_flags & 4u))
                {
                    int yt_m = ctx->ytop_min;
                    int yb_m = ctx->ybot_max;
                    if (s_px1 <= 0 || s_px0 >= HW_VIEW_W ||
                        (s_ya0 >= s_ya1 && s_yb0 >= s_yb1) ||
                        (s_ya1 <= yt_m && s_yb1 <= yt_m) ||
                        (s_ya0 >= yb_m && s_yb0 >= yb_m))
                        goto next_strip_lin;
                }

                if (__builtin_expect((lin_flags & 64u) != 0, 0))
                {
                    if (HW_IsStripOccluded_Slow(s_px0, s_px1, coal_strips))
                        goto next_strip_lin;
                }

                int is_partial_last = (s == s_max) && ((lin_flags & 32u) != 0);
                if ((!coal_strips && (is_partial_first || is_partial_last)) || (lin_flags & 128u) || ctx->last_clip_x0 != -1)
                {
                    int u_lo = is_partial_first ? ctx->u_start : ((s - (coal_strips ? (coal_strips - 1) : 0)) << 4);
                    int u_hi = (s == s_max) ? (ctx->u_start + ctx->u_span) : ((s + 1) << 4);
                    hw_wall_coord_tmp[0] = s_px0; hw_wall_coord_tmp[1] = s_px1;
                    hw_wall_coord_tmp[2] = s_ya0; hw_wall_coord_tmp[3] = s_yb0;
                    hw_wall_coord_tmp[4] = s_ya1; hw_wall_coord_tmp[5] = s_yb1;
                    if (HW_EmitStripQuad_Slow(
                            s, (uint32_t)(uint16_t)u_lo | ((uint32_t)(uint16_t)u_hi << 16),
                            ctx->cmd_header1, emit_cmd2))
                    {
                        lin_flags += coal_strips ? (1u << 16) : 0x00010100u;
                    }
                    goto next_strip_lin;
                }
            }

            /* Fast path: full 16-texel strip (or 32/64-texel coalesced quad), no narrow UserClip, coordinates in range */
            uint16_t m_cnt = saturn_cmd_m_count;
            if (__builtin_expect(m_cnt < CMDBUF_M_CAP - 1, 1))
            {
                saturn_cmd_m_count = m_cnt + 1;
                aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                uint32_t s_xa_p = ((uint32_t)s_px0 << 16);
                uint32_t s_xb_p = ((uint32_t)s_px1 << 16);
                cmd32[0] = 0x00020000u;
                cmd32[1] = ctx->cmd_header1;
                cmd32[2] = emit_cmd2;
                cmd32[3] = s_xa_p | (uint16_t)s_ya0;
                lin_flags += coal_strips ? (1u << 16) : 0x00010100u;
                cmd32[4] = s_xb_p | (uint16_t)s_yb0;
                cmd32[5] = s_xb_p | (uint16_t)s_yb1;
                cmd32[6] = s_xa_p | (uint16_t)s_ya1;
                cmd32[7] = 0;
            }
            else
            {
                saturn_v1w_drop++;
                saturn_v1w_captured = 1;
            }
        }

next_strip_lin:
        if (__builtin_expect((lin_flags & 2u) != 0, 0))
        {
            lin_flags = (lin_flags & ~18u) | ((lin_flags >> 2) & 2u);
            if (ctx->last_clip_x0 != -1)
                lin_flags |= 2u;
        }
        s_px0 = s_px1;
        s_ya0 = s_yb0;
        s_ya1 = s_yb1;
        cmd_w2_cur += cmd_w2_step;
        HW_W2_WRAP(cmd_w2_cur, cmd_w2_base, (uint32_t)num_strips * cmd_w2_step);
    }

    unsigned int l_quads  = lin_flags >> 16;
    unsigned int l_strips = (lin_flags >> 8) & 0xFFu;
    saturn_v1w_cells      += (int)l_quads;
    saturn_hw_wall_quads  += l_quads;
    saturn_hw_wall_strips += l_strips;
}
#endif /* !HW_ASM_LINGEN || HW_ASM_CHECK */

#if HW_ASM_LINGEN && HW_ASM_CHECK
/* Self-check: run the asm, hash its output (commands + counters + clip
 * state), roll back, run C, compare. [0] calls [1] mismatches
 * [2] s_min [3] s_max [4] asm quads [5] C quads [6] asm hash [7] C hash */
static volatile uint32_t hw_asm_chk_lingen[8];
static uint32_t HW_LinGenHash(uint16_t m0, hw_wall_emit_ctx_t *ctx)
{
    uint32_t h = 2166136261u;
    const uint32_t *p = (const uint32_t *)&saturn_cmd_m[m0];
    const uint32_t *e = (const uint32_t *)&saturn_cmd_m[saturn_cmd_m_count];
    for (; p < e; p++) h = (h ^ *p) * 16777619u;
    h = (h ^ (uint32_t)saturn_cmd_m_count) * 16777619u;
    h = (h ^ (uint32_t)saturn_v1w_cells) * 16777619u;
    h = (h ^ saturn_hw_wall_quads) * 16777619u;
    h = (h ^ saturn_hw_wall_strips) * 16777619u;
    h = (h ^ (uint32_t)saturn_v1w_drop) * 16777619u;
    h = (h ^ (uint32_t)saturn_v1w_captured) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)ctx->last_clip_x0) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)ctx->last_clip_x1) * 16777619u;
    return h;
}
static HW_NOINLINE void HW_LinGenCheck(hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    uint16_t m0 = saturn_cmd_m_count;
    int16_t lc0 = ctx->last_clip_x0, lc1 = ctx->last_clip_x1;
    int cells0 = saturn_v1w_cells, drop0 = saturn_v1w_drop, cap0 = saturn_v1w_captured;
    unsigned int q0 = saturn_hw_wall_quads, st0 = saturn_hw_wall_strips;
    hw_asm_lingen(ctx, s_min, s_max);
    uint32_t ha = HW_LinGenHash(m0, ctx);
    unsigned int qa = saturn_hw_wall_quads - q0;
    saturn_cmd_m_count = m0;
    ctx->last_clip_x0 = lc0; ctx->last_clip_x1 = lc1;
    saturn_v1w_cells = cells0; saturn_v1w_drop = drop0; saturn_v1w_captured = cap0;
    saturn_hw_wall_quads = q0; saturn_hw_wall_strips = st0;
    HW_EmitWallStrips_Linear_General(ctx, s_min, s_max);
    uint32_t hc = HW_LinGenHash(m0, ctx);
    hw_asm_chk_lingen[0]++;
    if (ha != hc && hw_asm_chk_lingen[1]++ == 0)
    {
        hw_asm_chk_lingen[2] = (uint32_t)s_min;
        hw_asm_chk_lingen[3] = (uint32_t)s_max;
        hw_asm_chk_lingen[4] = qa;
        hw_asm_chk_lingen[5] = saturn_hw_wall_quads - q0;
        hw_asm_chk_lingen[6] = ha;
        hw_asm_chk_lingen[7] = hc;
    }
}
#define HW_LINGEN_CALL HW_LinGenCheck
#elif HW_ASM_LINGEN
#define HW_LINGEN_CALL hw_asm_lingen
#else
#define HW_LINGEN_CALL HW_EmitWallStrips_Linear_General
#endif

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Linear(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const int u_start = ctx->u_start;
    const int u_len   = ctx->u_span;
    const int u_end   = u_start + u_len;
    const int tw      = ctx->tw;
    const int srca    = ctx->srca;

    int s_px0   = ctx->p0->px;
    int s_ya0   = ctx->ya0;
    int s_ya1   = ctx->ya1;
    int end_px  = ctx->p1->px;
    int end_ya0 = ctx->yb0;
    int end_ya1 = ctx->yb1;
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;
    uint16_t m_start = saturn_cmd_m_count;

    if (__builtin_expect(
            ((u_start | u_len) & 15) != 0 ||
            ((tw & (tw - 1)) != 0) ||
            ctx->occl_state != 0 ||
            ctx->last_clip_x0 != -1 ||
            s_px0 < 0 || end_px > HW_VIEW_W ||
            s_ya1 <= ytop_min || end_ya1 <= ytop_min ||
            s_ya0 >= ybot_max || end_ya0 >= ybot_max ||
            (((unsigned int)(s_ya1 - s_ya0 - 1) | (unsigned int)(end_ya1 - end_ya0 - 1)) > 1021u) ||
            HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1) ||
            (tw == 32 && ctx->srca_mono != srca && (((u_start + 31) & ~31) + 32 <= u_end)) ||
            (tw >= 64 && (ctx->srca_64 != srca || ctx->srca_mono != srca) && (((u_start + 63) & ~63) + 64 <= u_end)) ||
            (int)m_start + (s_max - s_min + 1) >= CMDBUF_M_CAP - 1, 0))
    {
        HW_LINGEN_CALL(ctx, s_min, s_max);
        return;
    }

    /* Zero-Call Leaf Fast Path: 100% in registers, 7-cycle SH-2 swap.w+xtrct vertex packing,
     * shared edge reuse (w_left_top = w_right_top), and direct cmd32 pointer stepping! */
    int num_strips = (tw > 0) ? (tw >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int cur_strip_idx = s_min & (num_strips - 1);
    int th = ctx->th;

    uint32_t size_lo     = (2u << 8) | ((uint32_t)ctx->rows & 0xFFu);
    uint32_t srca_v      = (uint32_t)(srca + (ctx->cur_v << 1));
    uint32_t cmd_w2_base = (srca_v << 16) | size_lo;
    uint32_t cmd_w2_step = (uint32_t)th << 17;
    uint32_t cmd_w2_wrap = (uint32_t)num_strips * cmd_w2_step;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;
    uint32_t cmd_header1 = ctx->cmd_header1;
    uint32_t w_left_top  = ((uint32_t)s_px0 << 16) | (uint16_t)s_ya0;
    uint32_t w_left_bot  = ((uint32_t)s_px0 << 16) | (uint16_t)s_ya1;
    aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_start];

    int rem = s_max - s_min;
    if (rem == 1)
    {
        int s_px1 = (s_px0 + end_px + 1) >> 1;
        int ya0_m = (s_ya0 + end_ya0 + 1) >> 1;
        int ya1_m = (s_ya1 + end_ya1 + 1) >> 1;
        uint32_t s_xb_p = (uint32_t)s_px1 << 16;
        uint32_t w_right_top = s_xb_p | (uint16_t)ya0_m;
        uint32_t w_right_bot = s_xb_p | (uint16_t)ya1_m;
        if (s_px1 > s_px0)
        {
            cmd32[0] = 0x00020000u;
            cmd32[1] = cmd_header1;
            cmd32[2] = cmd_w2_cur;
            cmd32[3] = w_left_top;
            cmd32[4] = w_right_top;
            cmd32[5] = w_right_bot;
            cmd32[6] = w_left_bot;
            cmd32[7] = 0;
            cmd32 += 8;
        }
        s_px0      = s_px1;
        w_left_top = w_right_top;
        w_left_bot = w_right_bot;
        cmd_w2_cur += cmd_w2_step;
        if (__builtin_expect(cmd_w2_cur - cmd_w2_base >= cmd_w2_wrap, 0))   /* overflow-safe (see HW_W2_WRAP) */
            cmd_w2_cur -= cmd_w2_wrap;
    }
    else if (rem > 0)
    {
        int step_px_fp, step_ya0_fp, step_ya1_fp;
        if (__builtin_expect(rem == 3, 1))
        {
            step_px_fp  = (end_px  - s_px0) << 14;
            step_ya0_fp = (end_ya0 - s_ya0) << 14;
            step_ya1_fp = (end_ya1 - s_ya1) << 14;
        }
        else
        {
            int step_t1 = (int)(ctx->inv_u_span >> 4);
            step_px_fp  = (end_px  - s_px0) * step_t1;
            step_ya0_fp = (end_ya0 - s_ya0) * step_t1;
            step_ya1_fp = (end_ya1 - s_ya1) * step_t1;
        }
        int cur_px_fp   = (s_px0 << 16) + 0x8000 + step_px_fp;
        int cur_ya0_fp  = (s_ya0 << 16) + 0x8000 + step_ya0_fp;
        int cur_ya1_fp  = (s_ya1 << 16) + 0x8000 + step_ya1_fp;

        do {
            int s_px1;
            uint32_t w_right_top, w_right_bot;
            HW_PackLinearStripRight(cur_px_fp, cur_ya0_fp, cur_ya1_fp, &s_px1, &w_right_top, &w_right_bot);
            cur_px_fp  += step_px_fp;
            cur_ya0_fp += step_ya0_fp;
            cur_ya1_fp += step_ya1_fp;
            if (s_px1 > s_px0)
            {
                cmd32[0] = 0x00020000u;
                cmd32[1] = cmd_header1;
                cmd32[2] = cmd_w2_cur;
                cmd32[3] = w_left_top;
                cmd32[4] = w_right_top;
                cmd32[5] = w_right_bot;
                cmd32[6] = w_left_bot;
                cmd32[7] = 0;
                cmd32 += 8;
            }
            s_px0      = s_px1;
            w_left_top = w_right_top;
            w_left_bot = w_right_bot;
            cmd_w2_cur += cmd_w2_step;
            if (__builtin_expect(cmd_w2_cur - cmd_w2_base >= cmd_w2_wrap, 0))   /* overflow-safe */
                cmd_w2_cur -= cmd_w2_wrap;
        } while (--rem > 0);
    }

    if (ctx->p1->px > s_px0)
    {
        uint32_t end_xb_p = (uint32_t)ctx->p1->px << 16;
        cmd32[0] = 0x00020000u;
        cmd32[1] = cmd_header1;
        cmd32[2] = cmd_w2_cur;
        cmd32[3] = w_left_top;
        cmd32[4] = end_xb_p | (uint16_t)ctx->yb0;
        cmd32[5] = end_xb_p | (uint16_t)ctx->yb1;
        cmd32[6] = w_left_bot;
        cmd32[7] = 0;
        cmd32 += 8;
    }

    uint16_t m_end = (uint16_t)((vdp1_cmd_t *)cmd32 - saturn_cmd_m);
    unsigned int l_quads = (unsigned int)(m_end - m_start);
    saturn_cmd_m_count    = m_end;
    saturn_v1w_cells      += (int)l_quads;
    saturn_hw_wall_quads  += l_quads;
    saturn_hw_wall_strips += l_quads;
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Persp_General(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    const int u_start = ctx->u_start;
    const int u_end   = u_start + ctx->u_span;
    const int tw = ctx->tw;
    const int th = ctx->th;

    int s_px0     = p0->px;
    int s_ya0     = ctx->ya0;
    int s_ya1     = ctx->ya1;
    int end_px    = p1->px;
    int end_ya0   = ctx->yb0;
    int end_ya1   = ctx->yb1;
    fixed_t s_cz0 = p0->cz;

    int light_s = HW_SegLight16(ctx->seg);
    int const_light = 1;
    if (light_s < 15)
    {
        const uint16_t *zlight_row = wall_zlight_cmdcolr_LUT[light_s];
        unsigned int zb0 = (unsigned int)p0->cz >> 20; if (zb0 > 127u) zb0 = 127u;
        unsigned int zb1 = (unsigned int)p1->cz >> 20; if (zb1 > 127u) zb1 = 127u;
        const_light = (zlight_row[zb0] == zlight_row[zb1]);
    }

    int wall_needs_clamp =
        ((((unsigned int)(s_px0 + 500) | (unsigned int)(500 - end_px) |
           (unsigned int)(s_ya1 - s_ya0) | (unsigned int)(end_ya1 - end_ya0)) > 1022u) ||
         HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1));
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;
    int band_all_vis = (s_px0 >= 0) & (end_px <= HW_VIEW_W) &
                       (s_ya0 < s_ya1) & (end_ya0 < end_ya1) &
                       (s_ya1 > ytop_min) & (end_ya1 > ytop_min) &
                       (s_ya0 < ybot_max) & (end_ya0 < ybot_max);
    int partial_first = (u_start & 15) != 0;
    int partial_last  = (u_end & 15) != 0;
    int has_occl      = (ctx->occl_state != 0);
    int wall_slow     = (!const_light) | (!band_all_vis) | has_occl | wall_needs_clamp;
    int first_slow    = wall_slow | partial_first | (ctx->last_clip_x0 != -1);
    int can_coal      = (tw >= 64) & ((tw & 63) == 0) & ((ctx->srca_64 != ctx->srca) | (ctx->srca_mono != ctx->srca)) & (((u_start + 63) & ~63) + 64 <= u_end);

    uint32_t persp_flags = (uint32_t)const_light |
                           ((uint32_t)first_slow << 1) |
                           ((uint32_t)band_all_vis << 2) |
                           ((uint32_t)wall_slow << 3) |
                           ((uint32_t)partial_first << 4) |
                           ((uint32_t)partial_last << 5) |
                           ((uint32_t)has_occl << 6) |
                           ((uint32_t)wall_needs_clamp << 7) |
                           ((uint32_t)can_coal << 8);

    int num_strips = (tw > 0) ? ((tw + 15) >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int strip_mask = num_strips - 1;
    int cur_strip_idx = __builtin_expect((num_strips & strip_mask) == 0, 1)
        ? (s_min & strip_mask)
        : HW_ModNonPow2(s_min, num_strips);

    uint32_t size_lo = (2u << 8) | ((uint32_t)ctx->rows & 0xFFu);
    uint32_t srca_v = (uint32_t)(ctx->srca + (ctx->cur_v << 1));
    uint32_t cmd_w2_base = (srca_v << 16) | size_lo;
    uint32_t cmd_w2_step = (uint32_t)th << 17;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;

    int vcy_top   = HW_VIEW_CY - ctx->top_overlap;
    int vcy_bot   = HW_VIEW_CY + ctx->bot_overlap;
    int dz_top    = ctx->dz_top;
    int dz_bot    = ctx->dz_bot;

    fixed_t cur_cx1 = 0, cur_cz1 = 0, step_cx1 = 0, step_cz1 = 0;
    if (s_min < s_max)
    {
        int d_cx = p1->cx - p0->cx;
        int d_cz = p1->cz - p0->cz;
        int u_len = ctx->u_span;
        if (__builtin_expect(((u_start & 15) == 0) && (u_len == 64 || u_len == 32), 1))
        {
            int sh = (u_len == 64) ? 2 : 1;
            step_cx1 = d_cx >> sh;
            step_cz1 = d_cz >> sh;
            cur_cx1  = p0->cx + step_cx1;
            cur_cz1  = p0->cz + step_cz1;
        }
        else
        {
            uint32_t inv_u_span = ctx->inv_u_span;
            int first_du = ((s_min + 1) << 4) - u_start;
            fixed_t first_t1 = (fixed_t)(((uint32_t)first_du * inv_u_span) >> 8);
            fixed_t step_t1  = (fixed_t)(inv_u_span >> 4);
            HW_FixedMulPair(d_cx, first_t1, d_cz, first_t1, &cur_cx1, &cur_cz1);
            cur_cx1 += p0->cx;
            cur_cz1 += p0->cz;
            HW_FixedMulPair(d_cx, step_t1, d_cz, step_t1, &step_cx1, &step_cz1);
        }
    }

    for (int s = s_min; s <= s_max; s++)
    {
        int coal_strips = 0;
        uint32_t emit_cmd2 = cmd_w2_cur;
        if (__builtin_expect((persp_flags & 256u) != 0, 0) &&
            ((cur_strip_idx & 3) == 0) && !(persp_flags & 16u) &&
            (((s + 4) << 4) <= u_end) &&
            s_px0 >= 0 && s_cz0 >= (192 << FRACBITS))
        {
            fixed_t end_cz3 = (s + 3 < s_max) ? (cur_cz1 + step_cz1 * 3) : ctx->p1->cz;
            if (end_cz3 >= (192 << FRACBITS))
            {
                coal_strips = 4;
                int blk = cur_strip_idx >> 2;
                if (ctx->srca_64 != ctx->srca)
                {
                    int emit_srca = (int)ctx->srca_64 + ((int)ctx->cur_v << 3) + blk * (th << 3);
                    emit_cmd2 = ((uint32_t)emit_srca << 16) | (8u << 8) | ((uint32_t)ctx->rows & 0xFFu);
                }
                else
                {
                    int emit_srca = (int)ctx->srca_mono + ((int)ctx->cur_v << 2) + blk * (th << 2);
                    emit_cmd2 = ((uint32_t)emit_srca << 16) | (4u << 8) | ((uint32_t)ctx->rows & 0xFFu);
                }
                cmd_w2_cur += cmd_w2_step * 3u;
                cur_strip_idx += 3;
                cur_cx1 += step_cx1 * 3;
                cur_cz1 += step_cz1 * 3;
                s += 3;
            }
        }

        fixed_t s_cz1;
        int s_px1, s_yb0, s_yb1;

        if (__builtin_expect(s < s_max, 1))
        {
            fixed_t s_cx1 = cur_cx1;
            s_cz1 = cur_cz1;
            cur_cx1 += step_cx1;
            cur_cz1 += step_cz1;
            if (__builtin_expect(s_cz1 < HW_NEAR_Z, 0)) s_cz1 = HW_NEAR_Z;
            int s_scale1 = HW_FastDiv(s_cz1);
            HW_ProjPerspStrip_SH2(s_cx1, dz_top, dz_bot, s_scale1, vcy_top, vcy_bot, &s_px1, &s_yb0, &s_yb1);
        }
        else
        {
            s_cz1 = ctx->p1->cz;
            s_px1 = ctx->p1->px;
            s_yb0 = ctx->yb0;
            s_yb1 = ctx->yb1;
            persp_flags |= (persp_flags >> 4) & 2u; /* if partial_last (bit 5), set cur_slow (bit 1) */
        }

        if (s_px1 > s_px0)
        {
            uint32_t strip_cmd1 = ctx->cmd_header1;
            if (__builtin_expect((persp_flags & 2u) != 0, 0))
            {
                int is_partial_first = ((persp_flags & 16u) != 0);
                persp_flags &= ~16u;
                if (!(persp_flags & 4u))
                {
                    int yt_m = ctx->ytop_min;
                    int yb_m = ctx->ybot_max;
                    if (s_px1 <= 0 || s_px0 >= HW_VIEW_W ||
                        (s_ya0 >= s_ya1 && s_yb0 >= s_yb1) ||
                        (s_ya1 <= yt_m && s_yb1 <= yt_m) ||
                        (s_ya0 >= yb_m && s_yb0 >= yb_m))
                        goto next_strip_persp;
                }

                if (__builtin_expect((persp_flags & 64u) != 0, 0))
                {
                    if (HW_IsStripOccluded_Slow(s_px0, s_px1, coal_strips))
                        goto next_strip_persp;
                }

                if (!(persp_flags & 1u))
                {
                    unsigned int s_zb = ((unsigned int)s_cz0 + (unsigned int)s_cz1) >> 21;
                    if (__builtin_expect(s_zb > 127u, 0)) s_zb = 127u;
                    strip_cmd1 = (strip_cmd1 & ~0x0000FFFFu) |
                                 ((uint32_t)wall_zlight_cmdcolr_LUT[light_s][s_zb] & 0xFF00u);
                }

                int is_partial_last = (s == s_max) && ((persp_flags & 32u) != 0);
                if ((!coal_strips && (is_partial_first || is_partial_last)) || (persp_flags & 128u) || ctx->last_clip_x0 != -1)
                {
                    int u_lo = is_partial_first ? ctx->u_start : ((s - (coal_strips ? 3 : 0)) << 4);
                    int u_hi = (s == s_max) ? (ctx->u_start + ctx->u_span) : ((s + 1) << 4);
                    hw_wall_coord_tmp[0] = s_px0; hw_wall_coord_tmp[1] = s_px1;
                    hw_wall_coord_tmp[2] = s_ya0; hw_wall_coord_tmp[3] = s_yb0;
                    hw_wall_coord_tmp[4] = s_ya1; hw_wall_coord_tmp[5] = s_yb1;
                    if (HW_EmitStripQuad_Slow(
                            s, (uint32_t)(uint16_t)u_lo | ((uint32_t)(uint16_t)u_hi << 16),
                            strip_cmd1, emit_cmd2))
                    {
                        persp_flags += (1u << 16);
                    }
                    goto next_strip_persp;
                }
            }

            /* Fast path: full 16-texel strip (or 64-texel coalesced block), no narrow UserClip, coordinates in range */
            uint16_t m_cnt = saturn_cmd_m_count;
            if (__builtin_expect(m_cnt < CMDBUF_M_CAP - 1, 1))
            {
                saturn_cmd_m_count = m_cnt + 1;
                aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_cnt];
                uint32_t s_xa_p = ((uint32_t)s_px0 << 16);
                uint32_t s_xb_p = ((uint32_t)s_px1 << 16);
                cmd32[0] = 0x00020000u;
                cmd32[1] = strip_cmd1;
                cmd32[2] = emit_cmd2;
                cmd32[3] = s_xa_p | (uint16_t)s_ya0;
                persp_flags += (1u << 16);
                cmd32[4] = s_xb_p | (uint16_t)s_yb0;
                cmd32[5] = s_xb_p | (uint16_t)s_yb1;
                cmd32[6] = s_xa_p | (uint16_t)s_ya1;
                cmd32[7] = 0;
            }
            else
            {
                saturn_v1w_drop++;
                saturn_v1w_captured = 1;
            }
        }

next_strip_persp:
        if (__builtin_expect((persp_flags & 2u) != 0, 0))
        {
            persp_flags = (persp_flags & ~18u) | ((persp_flags >> 2) & 2u);
            if (ctx->last_clip_x0 != -1)
                persp_flags |= 2u;
        }
        s_px0 = s_px1;
        s_ya0 = s_yb0;
        s_ya1 = s_yb1;
        s_cz0 = s_cz1;
        cur_strip_idx++;
        if (cur_strip_idx >= num_strips)
            cur_strip_idx -= num_strips;
        cmd_w2_cur += cmd_w2_step;
        HW_W2_WRAP(cmd_w2_cur, cmd_w2_base, (uint32_t)num_strips * cmd_w2_step);
    }

    unsigned int l_quads = persp_flags >> 16;
    saturn_v1w_cells      += (int)l_quads;
    saturn_hw_wall_quads  += l_quads;
    saturn_hw_wall_strips += l_quads;
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Persp(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    const int u_start = ctx->u_start;
    const int u_len   = ctx->u_span;
    const int u_end   = u_start + u_len;
    const int tw      = ctx->tw;

    int s_px0   = p0->px;
    int s_ya0   = ctx->ya0;
    int s_ya1   = ctx->ya1;
    int end_px  = p1->px;
    int end_ya0 = ctx->yb0;
    int end_ya1 = ctx->yb1;
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;

    int light_s = HW_SegLight16(ctx->seg);
    int const_light = 1;
    if (light_s < 15)
    {
        const uint16_t *zlight_row = wall_zlight_cmdcolr_LUT[light_s];
        unsigned int zb0 = (unsigned int)p0->cz >> 20; if (zb0 > 127u) zb0 = 127u;
        unsigned int zb1 = (unsigned int)p1->cz >> 20; if (zb1 > 127u) zb1 = 127u;
        const_light = (zlight_row[zb0] == zlight_row[zb1]);
    }

    uint16_t m_start = saturn_cmd_m_count;
    if (__builtin_expect(
            !const_light ||
            ((u_start | u_len) & 15) != 0 ||
            ((tw & (tw - 1)) != 0) ||
            ctx->occl_state != 0 ||
            ctx->last_clip_x0 != -1 ||
            s_px0 < 0 || end_px > HW_VIEW_W ||
            s_ya1 <= ytop_min || end_ya1 <= ytop_min ||
            s_ya0 >= ybot_max || end_ya0 >= ybot_max ||
            (((unsigned int)(s_ya1 - s_ya0 - 1) | (unsigned int)(end_ya1 - end_ya0 - 1)) > 1021u) ||
            HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1) ||
            (tw >= 64 && (ctx->srca_64 != ctx->srca || ctx->srca_mono != ctx->srca) && (((u_start + 63) & ~63) + 64 <= u_end)) ||
            (int)m_start + (s_max - s_min + 1) >= CMDBUF_M_CAP - 1, 0))
    {
        HW_EmitWallStrips_Persp_General(ctx, s_min, s_max);
        return;
    }

    /* Zero-Call Leaf Fast Path for perspective strips:
     * Direct SATURN_OCLUT lookup, 0-stall 3x dmuls.l projection, shared edge reuse, and direct cmd32 pointer stepping! */
    int num_strips = (tw > 0) ? (tw >> 4) : 1;
    if (num_strips <= 0) num_strips = 1;
    int cur_strip_idx = s_min & (num_strips - 1);
    int th = ctx->th;

    uint32_t size_lo     = (2u << 8) | ((uint32_t)ctx->rows & 0xFFu);
    uint32_t srca_v      = (uint32_t)(ctx->srca + (ctx->cur_v << 1));
    uint32_t cmd_w2_base = (srca_v << 16) | size_lo;
    uint32_t cmd_w2_step = (uint32_t)th << 17;
    uint32_t cmd_w2_wrap = (uint32_t)num_strips * cmd_w2_step;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;
    uint32_t cmd_header1 = ctx->cmd_header1;
    uint32_t w_left_top  = ((uint32_t)s_px0 << 16) | (uint16_t)s_ya0;
    uint32_t w_left_bot  = ((uint32_t)s_px0 << 16) | (uint16_t)s_ya1;
    aliased_uint32_t *cmd32 = (aliased_uint32_t *)&saturn_cmd_m[m_start];

    int rem = s_max - s_min;
    if (rem > 0)
    {
        int d_cx = p1->cx - p0->cx;
        int d_cz = p1->cz - p0->cz;
        fixed_t step_cx1, step_cz1;
        if (__builtin_expect(rem == 3, 1))
        {
            step_cx1 = d_cx >> 2;
            step_cz1 = d_cz >> 2;
        }
        else if (rem == 1)
        {
            step_cx1 = d_cx >> 1;
            step_cz1 = d_cz >> 1;
        }
        else
        {
            int step_t1 = (int)(ctx->inv_u_span >> 4);
            HW_FixedMulPair(d_cx, step_t1, d_cz, step_t1, &step_cx1, &step_cz1);
        }
        fixed_t cur_cx1  = p0->cx + step_cx1;
        fixed_t cur_cz1  = p0->cz + step_cz1;
        int vcy_top      = HW_VIEW_CY - ctx->top_overlap;
        int vcy_bot      = HW_VIEW_CY + ctx->bot_overlap;
        int dz_top       = ctx->dz_top;
        int dz_bot       = ctx->dz_bot;

        do {
            fixed_t s_cx1 = cur_cx1;
            fixed_t s_cz1 = cur_cz1;
            cur_cx1 += step_cx1;
            cur_cz1 += step_cz1;
            if (__builtin_expect(s_cz1 < HW_NEAR_Z, 0)) s_cz1 = HW_NEAR_Z;
            int s_scale1 = HW_FastDiv(s_cz1);
            int s_px1;
            uint32_t w_right_top, w_right_bot;
            HW_ProjPackPerspStripRight(s_cx1, dz_top, dz_bot, s_scale1, vcy_top, vcy_bot, &s_px1, &w_right_top, &w_right_bot);
            if (s_px1 > s_px0)
            {
                cmd32[0] = 0x00020000u;
                cmd32[1] = cmd_header1;
                cmd32[2] = cmd_w2_cur;
                cmd32[3] = w_left_top;
                cmd32[4] = w_right_top;
                cmd32[5] = w_right_bot;
                cmd32[6] = w_left_bot;
                cmd32[7] = 0;
                cmd32 += 8;
            }
            s_px0      = s_px1;
            w_left_top = w_right_top;
            w_left_bot = w_right_bot;
            cmd_w2_cur += cmd_w2_step;
            if (__builtin_expect(cmd_w2_cur - cmd_w2_base >= cmd_w2_wrap, 0))   /* overflow-safe */
                cmd_w2_cur -= cmd_w2_wrap;
        } while (--rem > 0);
    }

    if (ctx->p1->px > s_px0)
    {
        uint32_t end_xb_p = (uint32_t)ctx->p1->px << 16;
        cmd32[0] = 0x00020000u;
        cmd32[1] = cmd_header1;
        cmd32[2] = cmd_w2_cur;
        cmd32[3] = w_left_top;
        cmd32[4] = end_xb_p | (uint16_t)ctx->yb0;
        cmd32[5] = end_xb_p | (uint16_t)ctx->yb1;
        cmd32[6] = w_left_bot;
        cmd32[7] = 0;
        cmd32 += 8;
    }

    uint16_t m_end = (uint16_t)((vdp1_cmd_t *)cmd32 - saturn_cmd_m);
    unsigned int l_quads = (unsigned int)(m_end - m_start);
    saturn_cmd_m_count    = m_end;
    saturn_v1w_cells      += (int)l_quads;
    saturn_hw_wall_quads  += l_quads;
    saturn_hw_wall_strips += l_quads;
}

HW_ALWAYS_INLINE void HW_EmitWallStrips(hw_wall_emit_ctx_t *ctx)
{
    int u_span = ctx->u_span;
    if (u_span <= 0) return;
    int u_start = ctx->u_start;
    int u_end = u_start + u_span;
    int s_min = u_start >> 4;
    int s_max = (u_end - 1) >> 4;

    /* Walk rewrite stage B1 (2026-11-16): the _Linear / _Persp fast paths are
     * specializations of the _General loops (same arithmetic, stricter gate)
     * that add ~1.9 KB to the hot path. Default: General only, to test
     * whether less code beats the faster special case.
     * MEASURED (5thPlanet E1M1 turning, heavy views): master 20.52 -> 20.65
     * ms, i.e. no gain - the General loop is slower per strip and that
     * cancels the code saved. Fast paths stay on (default 1). */
#ifndef HW_STRIP_FASTPATH
#define HW_STRIP_FASTPATH 1
#endif
#if HW_STRIP_FASTPATH
    if (__builtin_expect(!ctx->persp_strips, 1))
        HW_EmitWallStrips_Linear(ctx, s_min, s_max);
    else
        HW_EmitWallStrips_Persp(ctx, s_min, s_max);
#else
    if (__builtin_expect(!ctx->persp_strips, 1))
        HW_LINGEN_CALL(ctx, s_min, s_max);
    else
        HW_EmitWallStrips_Persp_General(ctx, s_min, s_max);
#endif
}

HW_ALWAYS_INLINE int HW_NeedPerspectiveStrips(int dx, int u_span, fixed_t cz0, fixed_t cz1, int tw)
{
    if (!saturn_wall_subdivision) return 0;
    if (tw <= 16 || dx < 16) return 0;
    fixed_t min_z = (cz0 < cz1) ? cz0 : cz1;
    /* Distant walls: both ends > 448 units have negligible affine distortion */
    if (min_z > (448 << FRACBITS)) return 0;
    if (dx < 20 && min_z > (128 << FRACBITS)) return 0;
    /* Perpendicular walls: < 12.5% depth change across the wall has minimal affine warping */
    fixed_t max_z = (cz0 > cz1) ? cz0 : cz1;
    fixed_t dz = max_z - min_z;
    if (dz < (min_z >> 3)) return 0;
    if (min_z > (288 << FRACBITS) && (dx < 32 || dz < (min_z >> 2))) return 0;
    /* Don't subdivide into perspective strips if average strip is < 5 screen pixels wide */
    if ((dx << 4) < (u_span * 5)) return 0;
    return 1;
}

#define HW_MAX_SUBSECTOR_PILLARS 96

static hw_trans_pillar_t local_trans_pillars[HW_MAX_SUBSECTOR_PILLARS];
static uint8_t hw_pil_gen[HW_MAX_SUBSECTOR_PILLARS];
static const uint8_t hw_zero_pfrac[HW_MAX_SUBSECTOR_PILLARS] = {0};
static uint8_t hw_cur_pil_gen = 0;
static const uint8_t *hw_cur_pfrac = hw_zero_pfrac;
static hw_trans_pillar_t hw_p0_clipped;
static hw_trans_pillar_t hw_p1_clipped;

static HW_NOINLINE HW_COLD_OS void HW_TransformPillar_Shifted(
    hw_trans_pillar_t *tp, int pdx, int pdy)
{
    fixed_t cz, cx;
    if ((((unsigned int)pdx + 32767u) | ((unsigned int)pdy + 32767u)) <= 65534u)
        ProjectPillar_2D_16(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, hw_frust.cz_origin, hw_frust.cx_origin, &cz, &cx);
    else if ((((unsigned int)(pdx >> 4) + 16384u) | ((unsigned int)(pdy >> 4) + 16384u)) <= 32767u)
        ProjectPillar_2D(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, hw_frust.cz_origin, hw_frust.cx_origin, &cz, &cx);
    else
        ProjectPillar_2D_Shifted(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, hw_frust.cz_origin, hw_frust.cx_origin, &cz, &cx);
    int s = 0, px = 0;
    if (cz >= HW_NEAR_Z)
    {
        s = HW_FastDiv(cz);
        px = HW_ProjX(cx, s, HW_VIEW_CX);
    }
    tp->cx = cx;
    tp->cz = cz;
    tp->s  = s;
    tp->px = px;
}

/* Pure zero-push leaf function: takes 3 register arguments (r4=idx, r5=dx, r6=dy), 0 stack arguments.
 * For |dx|, |dy| <= 2047, pdx/pdy fit in int16_t for 1-cycle muls.w and cz >> 16 <= 2897 < 4096,
 * allowing direct SATURN_OCLUT indexing with zero jsr calls and zero register saves! */
static HW_NOINLINE HW_HOT_O2 void HW_TransformSubsectorPillar(int idx, int dx, int dy)
{
    hw_trans_pillar_t *tp = &local_trans_pillars[idx];
    int fr = hw_cur_pfrac[idx];
    int pdx = (dx << 4) | (fr >> 4);
    int pdy = (dy << 4) | (fr & 15);
    if (__builtin_expect((((unsigned int)dx + 1400u) | ((unsigned int)dy + 1400u)) > 2800u, 0))
    {
        HW_TransformPillar_Shifted(tp, pdx, pdy);
        return;
    }
    fixed_t cz, cx;
    ProjectPillar_2D_16_Frust(pdx, pdy, &cz, &cx);
    int s = 0, px = 0;
    if (cz >= HW_NEAR_Z)
    {
        s = (int)saturn_master_oclut[(unsigned int)cz >> FRACBITS];
        px = HW_ProjX(cx, s, HW_VIEW_CX);
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
    const int view_cx = HW_VIEW_CX;
    const int view_w  = HW_VIEW_W;

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
    int view_w  = HW_VIEW_W;
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

static HW_NOINLINE HW_COLD_OS int HW_LoadWallTexCtx_Miss(int texnum, hw_wall_emit_ctx_t *ctx)
{
    int tw = 0, th = 0, srca_mono = 0, srca_64 = 0;
    int srca = HW_FastWallSrca(texnum, &tw, &th, &srca_mono, &srca_64);
    ctx->srca       = (uint16_t)srca;
    ctx->srca_mono  = (uint16_t)srca_mono;
    ctx->srca_64    = (uint16_t)srca_64;
    ctx->tw         = (int16_t)tw;
    ctx->th         = (int16_t)th;
    return srca;
}

/* Outlined tag lookup (called only on texture changes, ~40% of visible segs) */
#ifdef HW_SLAVE_TU
/* Slave copy: lookup only. The tag is read cache-through (the master may
 * have uploaded it this frame); nothing is allocated, copied or touched -
 * pass 1 already uploaded every wall texture, and a texture still missing
 * just skips the wall (counted as a texture miss), as an upload refusal
 * does on the master. */
static HW_NOINLINE HW_HOT_O2 int HW_LoadWallTexCtx(int texnum, hw_wall_emit_ctx_t *ctx)
{
    ctx->cur_tex_id = texnum;
    ctx->srca = 0;
    if ((unsigned)texnum >= (unsigned)numtextures)
        return 0;
    int t = texturetranslation ? texturetranslation[texnum] : texnum;
    if ((unsigned)t >= (unsigned)numtextures)
        t = texnum;
    extern v1_tag_t *saturn_v1_wall_tags;
    if (!saturn_v1_wall_tags)
        return 0;
    const volatile v1_tag_t *tag =
        (const volatile v1_tag_t *)((uintptr_t)&saturn_v1_wall_tags[t] | 0x20000000u);   /* cache-through */
    unsigned short s = tag->srca;
    if (s == 0 || s == 0xFFFFu)
        return 0;
    ctx->srca      = s;
    ctx->srca_mono = tag->srca_mono;
    ctx->srca_64   = tag->srca_64;
    ctx->tw        = (int16_t)((int)tag->w8 << 3);
    ctx->th        = (int16_t)tag->h;
    return (int)s;
}
#else
static HW_NOINLINE HW_HOT_O2 int HW_LoadWallTexCtx(int texnum, hw_wall_emit_ctx_t *ctx)
{
    ctx->cur_tex_id = texnum;
    if (__builtin_expect((unsigned)texnum < (unsigned)numtextures, 1))
    {
        int t = texturetranslation ? texturetranslation[texnum] : texnum;
        if (__builtin_expect((unsigned)t >= (unsigned)numtextures, 0))
            t = texnum;
        extern v1_tag_t *saturn_v1_wall_tags;
        if (__builtin_expect(saturn_v1_wall_tags != NULL, 1))
        {
            const v1_tag_t *tag = &saturn_v1_wall_tags[t];
            unsigned short s = tag->srca;
            if (__builtin_expect(s != 0 && s != 0xFFFFu, 1))
            {
                /* no optional copies yet and VRAM has room this frame:
                 * make them now (saturn_vdp1.c, one wall per frame) */
                extern int saturn_v1_late_copy_armed;
                if (__builtin_expect(saturn_v1_late_copy_armed && tag->srca_mono == s, 0))
                    Saturn_Vdp1_WallLateCopy(texnum);
                ctx->srca      = s;
                ctx->srca_mono = tag->srca_mono;
                ctx->srca_64   = tag->srca_64;
                ctx->tw        = (int16_t)((int)tag->w8 << 3);
                ctx->th        = (int16_t)tag->h;
                return (int)s;
            }
        }
    }
    return HW_LoadWallTexCtx_Miss(texnum, ctx);
}
#endif /* HW_SLAVE_TU */

static HW_NOINLINE HW_HOT_O2 void HW_EmitMultiBandBakedWall(hw_wall_emit_ctx_t *ctx)
{
    const baked_seg_t *seg = ctx->seg;
    const int s_A = ctx->p0->s;
    const int s_B = ctx->p1->s;
    const int th = ctx->th;
    const int mono_tw = ctx->mono_tw;
    const int mono_base_srca = ctx->mono_base_srca;
    const uint32_t tw_stride = (uint32_t)(mono_tw >> 3);
    const uint32_t cmd_size_hi = tw_stride << 8;
    const uint32_t cmd_header1 = ctx->cmd_header1;
    const int wall_h = ctx->wall_h;

    int dz_top = ctx->dz_top;
    int top_overlap = ctx->top_overlap;
    int rem_h = ctx->rem_h;
    int cur_v = ctx->cur_v;

    int ya0 = ctx->ya0;
    int yb0 = ctx->yb0;
    int ya1 = 0, yb1 = 0;

    if (!ctx->should_subdivide && !ctx->needs_clip && ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
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

        int dz_bot = dz_top - (band_h << FRACBITS);

        int bot_overlap = (rem_h <= band_h)
            ? ((seg->flags & SEG_FLAG_UPPER) ? ((wall_h <= 16) ? 0 : 2) : 1)
            : 0;

        int raw_ya1, raw_yb1;
        HW_ProjYPair(HW_VIEW_CY, dz_bot, s_A, dz_bot, s_B, &raw_ya1, &raw_yb1);
        ya1 = raw_ya1 + bot_overlap;
        yb1 = raw_yb1 + bot_overlap;

        if (ctx->should_subdivide)
        {
            int rows = band_h;
            if (rows > 255) rows = 255;
            ctx->dz_top = dz_top;
            ctx->dz_bot = dz_bot;
            ctx->top_overlap = (uint8_t)top_overlap;
            ctx->bot_overlap = (uint8_t)bot_overlap;
            ctx->ya0 = (int16_t)ya0;
            ctx->ya1 = (int16_t)ya1;
            ctx->yb0 = (int16_t)yb0;
            ctx->yb1 = (int16_t)yb1;
            ctx->cur_v = (int16_t)cur_v;
            ctx->rows = (int16_t)rows;
            HW_EmitWallStrips(ctx);
        }
        else
        {
            int strip_srca = mono_base_srca + cur_v * tw_stride;

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

        dz_top = dz_bot;
        top_overlap = 0;
        rem_h -= band_h;
        cur_v = (th > 0 && cur_v + band_h < th) ? (cur_v + band_h) : 0;
        ya0 = raw_ya1;
        yb0 = raw_yb1;
    }
}
static HW_NOINLINE HW_COLD_OS void HW_ClipPortalScales(
    int xa, int xb, int s_A, int s_B)
{
    int dx = xb - xa;
    int ds = s_B - s_A;
    int orig_sA = s_A;
    if (xa < 0)
    {
        int f = HW_ClampedFrac((unsigned int)(-xa), (unsigned int)dx);
        s_A = orig_sA + FixedMul(ds, f);
    }
    if (xb > HW_VIEW_W)
    {
        int f = HW_ClampedFrac((unsigned int)(HW_VIEW_W - xa), (unsigned int)dx);
        s_B = orig_sA + FixedMul(ds, f);
    }
    hw_wall_coord_tmp[0] = s_A;
    hw_wall_coord_tmp[1] = s_B;
}

static unsigned int hw_prof_segs_acc = 0;
static unsigned int hw_prof_wseg_acc = 0;
static unsigned int hw_prof_wquad_acc = 0;
static unsigned int hw_prof_sportal_acc = 0;
static unsigned int hw_prof_unb_acc = 0;

/* Takes 2 register arguments (r4=ctx, r5=bsec_idx), 0 stack arguments!
 * Uses 16-byte hw_sec_fast_t (1 SH-2 cache line, indexed by bsec_idx << 4) with pre-subtracted dz_ceil/dz_floor. */
static HW_NOINLINE HW_HOT_O2 void HW_RecordSegPortalAperture(
    hw_wall_emit_ctx_t *ctx, int bsec_idx)
{
    ctx->portal_recorded = 1;
#if HW_AP_1D
    return;     /* D32XR-style walk: solid columns only, no vertical aperture */
#endif
    HW_ORACLE_SKIP();
    if (ctx->seg_sx >= ctx->seg_ex || (unsigned)bsec_idx >= HW_MAX_AP_SECTORS)
        return;

#if HW_PROF_SUBTIMERS
    unsigned short tsp0 = SAT_FRT();
#endif
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    int xa  = p0->px;
    int xb  = p1->px;
    int s_A = p0->s;
    int s_B = p1->s;
    if (__builtin_expect((xa < 0 || xb > HW_VIEW_W) && xb > xa, 0))
    {
        HW_ClipPortalScales(xa, xb, s_A, s_B);
        s_A = hw_wall_coord_tmp[0];
        s_B = hw_wall_coord_tmp[1];
    }

    int s_min = (s_A < s_B) ? s_A : s_B;
    int s_max = (s_A > s_B) ? s_A : s_B;
    hw_sec_fast_t *bsec_f = &hw_sec_fast[bsec_idx];
    int32_t b_dz_ceil  = bsec_f->dz_ceil;
    int32_t b_dz_floor = bsec_f->dz_floor;
    int32_t f_dz_ceil  = ctx->sec_ceiling;
    int32_t f_dz_floor = ctx->sec_floor;
    hw_aperture_state_t *ap = HW_APERTURE;
    const int vcy = HW_VIEW_CY;
    const int vh  = HW_VIEW_H;
    int p_yt = 0;
    int p_yb = vh;
    int yt_att = ap->cur_sub_yt_attach;
    int yb_att = ap->cur_sub_yb_attach;

    if (!ctx->is_sky_ceil || (!bsec_f->is_sky && f_dz_ceil > b_dz_ceil))
    {
        int32_t dzt = (f_dz_ceil < b_dz_ceil) ? f_dz_ceil : b_dz_ceil;
        if (f_dz_ceil > b_dz_ceil || dzt > 0)
        {
            int yt_min = HW_ProjY(dzt, (dzt >= 0) ? s_max : s_min, vcy);

            if (!bsec_f->is_sky)
            {
                int b_yt_att = (yt_min < 0) ? 0 : yt_min;
                if (b_yt_att < bsec_f->yt_attach)
                    bsec_f->yt_attach = (int16_t)b_yt_att;
            }
            int yt_pad = (s_max < 8192) ? 0 : 1;
            if (yt_min - yt_pad > ctx->ytop_min)
            {
                p_yt = yt_min - yt_pad;
                if (f_dz_ceil > b_dz_ceil)
                {
                    int w_yt_att = HW_ProjY(f_dz_ceil, (f_dz_ceil >= 0) ? s_max : s_min, vcy);
                    if (w_yt_att < yt_att) yt_att = w_yt_att;
                }
            }
        }
    }

    int32_t dzb = (f_dz_floor > b_dz_floor) ? f_dz_floor : b_dz_floor;
    if (f_dz_floor < b_dz_floor || dzb < 0)
    {
        int yb_max = HW_ProjY(dzb, (dzb <= 0) ? s_max : s_min, vcy);

        int b_yb_att = (yb_max > vh) ? vh : yb_max;
        if (b_yb_att > bsec_f->yb_attach)
            bsec_f->yb_attach = (int16_t)b_yb_att;

        int yb_pad = (s_max < 8192) ? 0 : 1;
        if (yb_max + yb_pad < ctx->ybot_max)
        {
            p_yb = yb_max + yb_pad;
            if (f_dz_floor < b_dz_floor)
            {
                int w_yb_att = HW_ProjY(f_dz_floor, (f_dz_floor <= 0) ? s_max : s_min, vcy);
                if (w_yb_att > yb_att) yb_att = w_yb_att;
            }
        }
    }

    if (p_yt > 0 || p_yb < vh)
        HW_RecordPortalSpan(((int)ctx->seg_sx << 16) | (uint16_t)ctx->seg_ex, p_yt, p_yb, (yt_att << 16) | (uint16_t)yb_att);
#if HW_PROF_SUBTIMERS
    hw_prof_sportal_acc += HW_ProfDelta(tsp0, SAT_FRT());
#endif
}

#if defined(__GNUC__) && !defined(__clang__)
#define HW_HOT_O2_NOTHREAD __attribute__((optimize("O2,no-thread-jumps")))
#else
#define HW_HOT_O2_NOTHREAD HW_HOT_O2
#endif
/* HW_WALL_FUSE (2026-11-17, I-cache work): fold the single-caller wall chain
 * EmitWallFromCtx -> EmitBakedWallQuads -> EmitMonolithicBakedSeg into
 * HW_EmitVisibleBakedSeg (no calls, values stay in registers). */
#ifndef HW_WALL_FUSE
#define HW_WALL_FUSE 1      /* 5thPlanet: master -0.3..-0.6 ms, pixel-identical */
#endif
#ifndef HW_WALL_TINY
#define HW_WALL_TINY 0
#endif
#if HW_WALL_FUSE && defined(__GNUC__)
#define HW_WFUSE    inline __attribute__((always_inline))
#define HW_WFUSE_O2 HW_HOT_O2_NOTHREAD
#else
#define HW_WFUSE    HW_NOINLINE
#define HW_WFUSE_O2 HW_HOT_O2
#endif

static HW_NOINLINE HW_HOT_O2 void HW_EmitSubdividedBakedSeg(
    hw_wall_emit_ctx_t *ctx, int persp_strips)
{
    int u_span = ctx->u_span;
    if (__builtin_expect(saturn_cmd_m_count + ((unsigned)u_span >> 3) + 16 >= CMDBUF_M_CAP, 0))
        return;

    const baked_seg_t *s_ptr = ctx->seg;
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    int light_s = HW_SegLight16(s_ptr);
    unsigned int zb = (__builtin_expect(light_s < 15, 1))
        ? (((uint32_t)p0->cz + (uint32_t)p1->cz) >> 21)
        : 0u;
    if (__builtin_expect(zb > 127u, 0)) zb = 127u;
    int k_shade = (int)(wall_zlight_cmdcolr_LUT[light_s][zb] & 0xFF00u);

    uint32_t pmod = (s_ptr->flags & SEG_FLAG_MID)
        ? (((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu)
        : ((PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu);
    ctx->cmd_header1  = (pmod << 16) | (uint32_t)k_shade;
    ctx->persp_strips = (uint8_t)persp_strips;

    if (!ctx->was_near_clipped)
        ctx->inv_u_span = s_ptr->inv_u_span ? ((uint32_t)s_ptr->inv_u_span << 8) : (65536u << 8);

    int rem_h = ctx->wall_h;
    int cur_v = ctx->cur_v;
    int th    = ctx->th;
    int max_first_h = (th > 0) ? (th - cur_v) : rem_h;
    if (__builtin_expect((rem_h <= max_first_h) && (rem_h <= 255), 1))
    {
        int rows = (th > 0 && rem_h > th) ? th : rem_h;
        if (rows > 255) rows = 255;
        ctx->rows = (int16_t)rows;
        HW_EmitWallStrips(ctx);
    }
    else
    {
        ctx->mono_tw          = (ctx->tw > 64) ? 64 : ctx->tw;
        ctx->needs_clip       = 0;
        ctx->mono_base_srca   = ctx->srca_mono;
        ctx->should_subdivide = 1;
        ctx->rem_h            = (int16_t)rem_h;
        ctx->q_xa             = (int16_t)p0->px;
        ctx->q_xb             = (int16_t)p1->px;
        HW_EmitMultiBandBakedWall(ctx);
    }
}

static HW_NOINLINE HW_COLD_OS int HW_ClipSegNearPlane(
    hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg)
{
    ctx->u_start = seg->u_start;
    ctx->u_span  = (seg->u_span > 0) ? (int)seg->u_span : 64;
    if (!HW_ClipWallNearPlane(&local_trans_pillars[seg->pillar_idx[0]],
                              &local_trans_pillars[seg->pillar_idx[1]], ctx))
        return 0;
    if (HW_APERTURE->aperture_active)
    {
        int ap_pack = HW_GetSpanAperture(ctx->seg_sx, ctx->seg_ex);
        if (ap_pack < 0)
            return 0;
        ctx->ytop_min = (int16_t)(ap_pack >> 16);
        ctx->ybot_max = (int16_t)ap_pack;
    }
    else
    {
        ctx->ytop_min = 0;
        ctx->ybot_max = (int16_t)HW_VIEW_H;
    }
    return 1;
}

static HW_WFUSE HW_WFUSE_O2 void HW_EmitMonolithicBakedSeg(
    hw_wall_emit_ctx_t *ctx, int mono_tw, int mono_base_srca, int needs_clip)
{
    if (__builtin_expect(saturn_cmd_m_count + 16 >= CMDBUF_M_CAP, 0))
        return;

    int q_xa = ctx->p0->px;
    int q_xb = ctx->p1->px;

    if (__builtin_expect(needs_clip, 0))
    {
        ctx->mono_tw = (int16_t)mono_tw;
        ctx->needs_clip = 1;
        if (!HW_ExtrapolateMonoWallX(ctx, q_xa, q_xb))
            return;
        q_xa = ctx->q_xa;
        q_xb = ctx->q_xb;
        needs_clip = ctx->needs_clip;
    }

    if (!needs_clip && ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        ctx->last_clip_x0 = -1;
        ctx->last_clip_x1 = -1;
    }

    if (q_xb <= q_xa)
        return;

    const baked_seg_t *s_ptr = ctx->seg;
    int light_s = HW_SegLight16(s_ptr);
    unsigned int zb = (__builtin_expect(light_s < 15, 1))
        ? (((uint32_t)ctx->p0->cz + (uint32_t)ctx->p1->cz) >> 21)
        : 0u;
    if (__builtin_expect(zb > 127u, 0)) zb = 127u;
    int k_shade = (int)(wall_zlight_cmdcolr_LUT[light_s][zb] & 0xFF00u);

    uint32_t pmod = (s_ptr->flags & SEG_FLAG_MID)
        ? (((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu)
        : ((PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu);
    uint32_t cmd_header1 = (pmod << 16) | (uint32_t)k_shade;

    int th    = ctx->th;
    int rem_h = ctx->wall_h;
    int cur_v = ctx->cur_v;
    int max_first_h = (th > 0) ? (th - cur_v) : rem_h;

    if (__builtin_expect((rem_h <= max_first_h) && (rem_h <= 255), 1))
    {
        uint32_t tw_stride = (uint32_t)(mono_tw >> 3);
        int strip_srca = (__builtin_expect(cur_v == 0, 1)) ? mono_base_srca : (mono_base_srca + cur_v * (int)tw_stride);
        int rows = (th > 0 && rem_h > th) ? th : rem_h;
        if (rows > 255) rows = 255;
        uint32_t w2 = ((uint32_t)strip_srca << 16) | (tw_stride << 8) | ((uint32_t)rows & 0xFFu);

        int s_xa = q_xa;
        int s_xb = q_xb;
        int q_ya0 = ctx->ya0;
        int q_yb0 = ctx->yb0;
        int q_ya1 = ctx->ya1;
        int q_yb1 = ctx->yb1;

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

        uint16_t m_cnt = saturn_cmd_m_count;
        uint32_t xa_p = ((uint32_t)s_xa << 16);
        uint32_t xb_p = ((uint32_t)s_xb << 16);
        uint32_t w3 = xa_p | (uint16_t)q_ya0;
        uint32_t w4 = xb_p | (uint16_t)q_yb0;
        uint32_t w5 = xb_p | (uint16_t)q_yb1;
        uint32_t w6 = xa_p | (uint16_t)q_ya1;

        if (!needs_clip && m_cnt > ctx->sub_m_start)
        {
            aliased_uint32_t *prev = (aliased_uint32_t *)&saturn_cmd_m[m_cnt - 1];
            if (prev[4] == w3 && prev[5] == w6 &&
                prev[1] == cmd_header1 && prev[2] == w2)
            {
                int merged_dx = s_xb - (int)(int16_t)(prev[3] >> 16);
                if (((w2 & 0x3F00u) == 0x0400u) && ctx->tw == 32 && ctx->srca_64 != ctx->srca &&
                    merged_dx <= 48 &&
                    (unsigned int)(q_ya0 - q_yb0 + 3) <= 6u)
                {
                    int srca_64 = ctx->srca_64;
                    int cv = ctx->cur_v;
                    int strip_srca64 = (__builtin_expect(cv == 0, 1)) ? srca_64 : (srca_64 + (cv << 3));
                    prev[2] = ((uint32_t)strip_srca64 << 16) | (8u << 8) | (w2 & 0xFFu);
                    prev[4] = w4;
                    prev[5] = w5;
                    return;
                }
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
    else
    {
        ctx->cmd_header1      = cmd_header1;
        ctx->persp_strips     = 0;
        ctx->mono_tw          = (int16_t)mono_tw;
        ctx->needs_clip       = (uint8_t)needs_clip;
        ctx->mono_base_srca   = (uint16_t)mono_base_srca;
        ctx->should_subdivide = 0;
        ctx->rem_h            = (int16_t)rem_h;
        ctx->q_xa             = (int16_t)q_xa;
        ctx->q_xb             = (int16_t)q_xb;
        HW_EmitMultiBandBakedWall(ctx);
    }
}

static HW_WFUSE HW_HOT_O2_NOTHREAD void HW_EmitBakedWallQuads(hw_wall_emit_ctx_t *ctx)
{
#if HW_PROF_SUBTIMERS
    unsigned short twq0 = SAT_FRT();
#endif
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    int dx = p1->px - p0->px;

    if (__builtin_expect(ctx->was_near_clipped, 0))
    {
        HW_VS(12);
        HW_EmitSubdividedBakedSeg(ctx, (dx >= 24 && saturn_wall_subdivision));
#if HW_PROF_SUBTIMERS
        hw_prof_wquad_acc += HW_ProfDelta(twq0, SAT_FRT());
#endif
        return;
    }

    const baked_seg_t *s_ptr = ctx->seg;
    int tw      = ctx->tw;
    int u_start = s_ptr->u_start;
    int u_span  = (int)s_ptr->u_span;
    ctx->u_start = u_start;
    ctx->u_span  = u_span;

    int mono_base_srca = 0;
    int mono_tw = 0;
    int needs_clip = 0;

    if (tw <= 16)
    {
        if (u_start == 0 && u_span == tw)
        {
            mono_tw = tw;
            mono_base_srca = ctx->srca_mono;
        }
        else if (u_start >= 0 && u_start + u_span <= tw)
        {
            mono_tw = tw;
            mono_base_srca = ctx->srca_mono;
            needs_clip = 1;
        }
    }
    else if ((((u_span ^ 16) | (u_start & 15)) == 0) && (unsigned)u_start < (unsigned)tw)
    {
        mono_base_srca = (int)ctx->srca + (int)(((unsigned)u_start >> 4) * (unsigned)((int)ctx->th << 1));
        mono_tw = 16;
    }
    else
    {
        if ((ctx->wall_h > 24 || (p0->cz < (128 << FRACBITS) && p1->cz < (128 << FRACBITS))) &&
            ((unsigned int)(u_start ^ (u_start + u_span - 1)) > 15u) &&
            HW_NeedPerspectiveStrips(dx, u_span, p0->cz, p1->cz, tw))
        {
            HW_VS(13);
            if (p0->cz >= (512 << FRACBITS) && p1->cz >= (512 << FRACBITS)) HW_VS(17);
            HW_EmitSubdividedBakedSeg(ctx, 1);
#if HW_PROF_SUBTIMERS
            hw_prof_wquad_acc += HW_ProfDelta(twq0, SAT_FRT());
#endif
            return;
        }

        int srca      = ctx->srca;
        int srca_mono = ctx->srca_mono;
        int srca_64   = ctx->srca_64;
        int th        = ctx->th;
        if (((ctx->wall_h <= 16 && dx <= 16 && u_span <= 16) || (dx <= 6 && p0->cz > (512 << FRACBITS))) &&
            (unsigned)u_start < (unsigned)tw)
        {
            mono_base_srca = srca + (int)(((unsigned)u_start >> 4) * (unsigned)(th << 1));
            mono_tw = 16;
        }
        else if (tw == 32 && u_start == 0)
        {
            if ((u_span == 32 || (dx <= 28 && (unsigned int)(ctx->ya0 - ctx->yb0 + 3) <= 6u)) && srca_mono != srca)
            {
                mono_base_srca = srca_mono;
                mono_tw = 32;
            }
            else if (u_span == 64 && srca_64 != srca &&
                     (unsigned int)(ctx->ya0 - ctx->yb0 + 3) <= 6u)
            {
                mono_base_srca = srca_64;
                mono_tw = 64;
            }
        }
        else if (tw >= 64 && (((u_span ^ 64) | (u_start & 63)) == 0) && (unsigned)u_start < (unsigned)tw)
        {
            if ((dx <= 20 || (srca_64 == srca && dx <= 56)) && srca_mono != srca)
            {
                mono_base_srca = srca_mono + (int)(((unsigned)u_start >> 6) * (unsigned)(th << 2));
                mono_tw = 32;
            }
            else if (srca_64 != srca)
            {
                mono_base_srca = srca_64 + (int)(((unsigned)u_start >> 6) * (unsigned)(th << 3));
                mono_tw = 64;
            }
        }
    }

#if HW_VIS_STATS
    /* wall-path census: [14] linear strips [15] mono [16] linear, min cz >= 512
     * [18] linear, min cz >= 256 [19] sum dx of linear >= 512 [20] linear >= 512
     * with u range inside one 64-texel period [21] ... inside one 32 period
     * [22] linear >= 512 with tw >= 128 [23] linear >= 512 with tw == 64 */
    if (!mono_tw)
    {
        fixed_t czmin = (p0->cz < p1->cz) ? p0->cz : p1->cz;
        int u_end = u_start + u_span - 1;
        HW_VS(14);
        if (czmin >= (256 << FRACBITS)) HW_VS(18);
        if (czmin >= (512 << FRACBITS))
        {
            HW_VS(16);
            hw_vis_stats[19] += (uint32_t)dx;
            if ((u_start >> 6) == (u_end >> 6)) HW_VS(20);
            if ((u_start >> 5) == (u_end >> 5)) HW_VS(21);
            if (tw >= 128) HW_VS(22);
            else if (tw == 64) HW_VS(23);
        }
    }
    else
        HW_VS(15);
#endif
    if (!mono_tw)
        HW_EmitSubdividedBakedSeg(ctx, 0);
    else
        HW_EmitMonolithicBakedSeg(ctx, mono_tw, mono_base_srca, needs_clip);
#if HW_PROF_SUBTIMERS
    hw_prof_wquad_acc += HW_ProfDelta(twq0, SAT_FRT());
#endif
}

static HW_NOINLINE HW_COLD_OS void HW_OccludeZeroHeightSeg(
    const baked_seg_t *seg, int bsec_idx, int is_solid)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    uint8_t seg_flags = seg->flags;
    if ((seg_flags & SEG_FLAG_UPPER) && (seg + 1 < ctx->seg_end))
    {
        const baked_seg_t *next_seg = seg + 1;
        if (next_seg->pillar_idx[0] == seg->pillar_idx[0] &&
            next_seg->pillar_idx[1] == seg->pillar_idx[1] &&
            (next_seg->flags & SEG_FLAG_LOWER))
            return;
    }

    const hw_sec_fast_t *bsec_f = &hw_sec_fast[bsec_idx];
    int occlude_only = 0;
    if (is_solid)
    {
        occlude_only = 1;
    }
    else if (!ctx->portal_recorded &&
             ((!ctx->is_sky_ceil && (ctx->sec_ceiling != bsec_f->dz_ceil || ctx->sec_ceiling > 0)) ||
              (ctx->sec_floor != bsec_f->dz_floor || ctx->sec_floor < 0)))
    {
        occlude_only = 2;
    }
    if (!occlude_only)
        return;

    ctx->was_near_clipped = 0;
    if (ctx->needs_near_clip)
    {
        if (!HW_ClipSegNearPlane(ctx, seg))
            return;
        ctx->p0 = &hw_p0_clipped;
        ctx->p1 = &hw_p1_clipped;
    }

    if (is_solid)
    {
        ctx->portal_recorded = 1;
        if (ctx->seg_sx < ctx->seg_ex)
            HW_MarkSubSolidSpan(ctx->seg_sx, ctx->seg_ex);
    }
    else
    {
        HW_RecordSegPortalAperture(ctx, bsec_idx);
    }
}

static HW_WFUSE HW_HOT_O2_NOTHREAD void HW_EmitWallFromCtx(
    hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg);

static HW_NOINLINE HW_HOT_O2_NOTHREAD void HW_EmitVisibleBakedSeg(
    const baked_seg_t *seg)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    uint8_t seg_flags   = seg->flags;
    fixed_t sec_ceiling = ctx->sec_ceiling;
    fixed_t sec_floor   = ctx->sec_floor;
    fixed_t top_world_z = sec_ceiling;
    fixed_t bot_world_z = sec_floor;
    int is_solid = 1;
    int bsec_idx = -1;
    int v_base;

    if (__builtin_expect((seg_flags & SEG_FLAG_STATIC) != 0, 1))
    {
        if (seg_flags & (SEG_FLAG_LOWER | SEG_FLAG_UPPER))
        {
            bsec_idx = seg->back_sec_idx;
            const hw_sec_fast_t *bsec_f = &hw_sec_fast[bsec_idx];
            is_solid = 0;
            if (seg_flags & SEG_FLAG_UPPER)
                bot_world_z = bsec_f->dz_ceil;
            else
                top_world_z = bsec_f->dz_floor;
        }
        v_base = seg->mid_static_v_offset;
    }
    else if (seg_flags & (SEG_FLAG_LOWER | SEG_FLAG_UPPER))
    {
        bsec_idx = seg->back_sec_idx;
        if (__builtin_expect((unsigned)bsec_idx >= HW_MAX_AP_SECTORS, 0))
            return;
        const hw_sec_fast_t *bsec_f = &hw_sec_fast[bsec_idx];
        fixed_t bceil  = bsec_f->dz_ceil;
        fixed_t bfloor = bsec_f->dz_floor;
        fixed_t mbot   = (sec_floor > bfloor) ? sec_floor : bfloor;
        fixed_t mtop   = (sec_ceiling < bceil) ? sec_ceiling : bceil;
        is_solid = (!ctx->is_sky_ceil && !bsec_f->is_sky && mtop <= mbot);

        top_world_z = (seg_flags & SEG_FLAG_UPPER) ? sec_ceiling : bfloor;
        bot_world_z = (seg_flags & SEG_FLAG_UPPER) ? bceil       : sec_floor;
        fixed_t orig_top_z = top_world_z;
        fixed_t orig_bot_z = bot_world_z;
        if (top_world_z > sec_ceiling)
            top_world_z = sec_ceiling;
        if (bot_world_z < sec_floor)
            bot_world_z = sec_floor;

        if (__builtin_expect(top_world_z <= bot_world_z, 0))
        {
            if (!ctx->portal_recorded && !(seg_flags & SEG_FLAG_PORTAL_DONE))
                HW_OccludeZeroHeightSeg(seg, bsec_idx, is_solid);
            return;
        }

        int v_off = seg->mid_static_v_offset;
        if (seg_flags & SEG_PEG_LOWER_UNPEGGED)
            v_base = ((sec_ceiling - top_world_z) >> FRACBITS) + v_off;
        else if (seg_flags & SEG_PEG_BOTTOM)
            v_base = v_off - ((top_world_z - orig_bot_z) >> FRACBITS);
        else
            v_base = v_off + ((orig_top_z - top_world_z) >> FRACBITS);
    }
    else
    {
        if (seg_flags & SEG_FLAG_MID)
        {
            is_solid = 0;
            bsec_idx = seg->back_sec_idx;
            if ((unsigned)bsec_idx < HW_MAX_AP_SECTORS)
            {
                const hw_sec_fast_t *bsec_f = &hw_sec_fast[bsec_idx];
                if (bsec_f->dz_ceil < top_world_z)
                    top_world_z = bsec_f->dz_ceil;
                if (bsec_f->dz_floor > bot_world_z)
                    bot_world_z = bsec_f->dz_floor;
            }
        }
        if (__builtin_expect(top_world_z <= bot_world_z, 0))
            return;
        int v_off = seg->mid_static_v_offset;
        v_base = (seg_flags & SEG_PEG_BOTTOM)
            ? (v_off - ((top_world_z - bot_world_z) >> FRACBITS))
            : v_off;
    }

    int wall_h = (top_world_z - bot_world_z) >> FRACBITS;
    if (__builtin_expect(wall_h <= 0, 0))
        wall_h = 1;
    ctx->dz_top           = top_world_z;
    ctx->dz_bot           = bot_world_z;
    ctx->wall_h           = (int16_t)wall_h;
    ctx->cur_v            = (int16_t)v_base;
    ctx->was_near_clipped = 0;

    if (__builtin_expect(ctx->needs_near_clip != 0, 0))
    {
        if (!HW_ClipSegNearPlane(ctx, seg))
            return;
        ctx->p0 = &hw_p0_clipped;
        ctx->p1 = &hw_p1_clipped;
    }

    if (!ctx->portal_recorded && !(seg_flags & SEG_FLAG_PORTAL_DONE))
    {
        if (is_solid)
        {
            ctx->portal_recorded = 1;
            if (ctx->seg_sx < ctx->seg_ex)
                HW_MarkSubSolidSpan(ctx->seg_sx, ctx->seg_ex);
        }
        else if (bsec_idx >= 0)
        {
            HW_RecordSegPortalAperture(ctx, bsec_idx);
        }
    }

    int tex_id = seg->tex_64;
    if (__builtin_expect(tex_id <= 0, 0))
        return;

    ctx->n_vis++;

    int ya0, ya1, yb0, yb1;
    HW_ProjYQuad(HW_VIEW_CY, ctx->dz_top, ctx->dz_bot, ctx->p0->s, ctx->p1->s, &ya0, &ya1, &yb0, &yb1);

    if ((ya1 <= ctx->ytop_min && yb1 <= ctx->ytop_min) ||
        (ya0 >= ctx->ybot_max && yb0 >= ctx->ybot_max))
        return;

    int top_overlap = 1;
    int bot_overlap = 1;
    if (__builtin_expect(ctx->wall_h > 16, 1))
        bot_overlap = (seg_flags & SEG_FLAG_UPPER) ? 2 : 1;
    else if (seg_flags & SEG_FLAG_LOWER)
        top_overlap = 0;
    else if (seg_flags & SEG_FLAG_UPPER)
        bot_overlap = 0;
    ya0 -= top_overlap;
    yb0 -= top_overlap;
    ya1 += bot_overlap;
    yb1 += bot_overlap;

    if (ya0 >= ya1 && yb0 >= yb1)
        return;

    if (__builtin_expect((ya0 < -8 && yb0 < -8) || (ya1 > HW_VIEW_H + 8 && yb1 > HW_VIEW_H + 8), 0))
        saturn_cmd_m_needs_ytrim = 1;

#if HW_DEFER_WALLS
    if (hw_defer_on)
    {
        /* Pass 1: record; HW_EmitDeferredWalls emits it (capacity checked
         * per subsector in HW_RenderSubsector_Body).
         * Still make sure the texture is uploaded HERE, in walk order: the
         * cache builds a texture's mono/64 variants only while the arena has
         * room, so uploading every wall after every flat (pass 2) left walls
         * without variants -> up to ~40% more wall quads (tour9 E1M1
         * Zigzag 164 -> 227). Eviction only runs at the frame boundary, so
         * the upload is still valid in pass 2. */
        if (tex_id != hw_defer_tex_seen)
        {
            int ttw, tth, tmono, t64;
            hw_defer_tex_seen = tex_id;
#if HW_SLAVE_WALLS
            /* the optional wall copies HW_LoadWallTexCtx makes in pass 2 on
             * the master: make them here, the slave's pass 2 can't */
            int ts = HW_FastWallSrca(tex_id, &ttw, &tth, &tmono, &t64);
            extern int saturn_v1_late_copy_armed;
            if (__builtin_expect(saturn_v1_late_copy_armed && ts != 0 && ts != 0xFFFF && tmono == ts, 0))
                Saturn_Vdp1_WallLateCopy(tex_id);
#else
            (void)HW_FastWallSrca(tex_id, &ttw, &tth, &tmono, &t64);
#endif
        }
        hw_wall_rec_t *r = &hw_defer_rec[hw_defer_nrec++];
        r->seg              = seg;
        r->p0               = *ctx->p0;
        r->p1               = *ctx->p1;
        r->dz_top           = ctx->dz_top;
        r->dz_bot           = ctx->dz_bot;
        r->u_start          = ctx->u_start;
        r->u_span           = ctx->u_span;
        r->inv_u_span       = ctx->inv_u_span;
        r->ya0              = (int16_t)ya0;
        r->ya1              = (int16_t)ya1;
        r->yb0              = (int16_t)yb0;
        r->yb1              = (int16_t)yb1;
        r->seg_sx           = ctx->seg_sx;
        r->seg_ex           = ctx->seg_ex;
        r->ytop_min         = ctx->ytop_min;
        r->ybot_max         = ctx->ybot_max;
        r->wall_h           = ctx->wall_h;
        r->cur_v            = ctx->cur_v;
        r->top_overlap      = (uint8_t)top_overlap;
        r->bot_overlap      = (uint8_t)bot_overlap;
        r->was_near_clipped = ctx->was_near_clipped;
        r->occl_state       = ctx->occl_state;
        if (ctx->occl_state)
            hw_defer_need_snap = 1;
        return;
    }
#endif

    ctx->seg         = seg;
    ctx->ya0         = (int16_t)ya0;
    ctx->ya1         = (int16_t)ya1;
    ctx->yb0         = (int16_t)yb0;
    ctx->yb1         = (int16_t)yb1;
    ctx->top_overlap = (uint8_t)top_overlap;
    ctx->bot_overlap = (uint8_t)bot_overlap;
#if HW_VIS_STATS
    /* size census (2026-11-17, distance-LOD study): [24] walls emitted
     * [25] width <= 2 px [26] width <= 4 [27] height <= 3 px (before the
     * overlap pad) [28] w<=4 || h<=3 ("tiny") [29] quads, all walls
     * [30] quads from tiny walls [31] tiny with both ends cz >= 1024
     * [32] tiny with both ends cz >= 512 [33] tiny, cz < 512 */
    uint16_t census_m0 = saturn_cmd_m_count;
    int c_w = ctx->p1->px - ctx->p0->px;
    int c_h0 = (ya1 - bot_overlap) - (ya0 + top_overlap);
    int c_h1 = (yb1 - bot_overlap) - (yb0 + top_overlap);
    int c_h = (c_h0 > c_h1) ? c_h0 : c_h1;
    int c_tiny = (c_w <= 4 || c_h <= 3);
    HW_VS(24);
    if (c_w <= 2) HW_VS(25);
    if (c_w <= 4) HW_VS(26);
    if (c_h <= 3) HW_VS(27);
    if (c_tiny)
    {
        HW_VS(28);
        if (ctx->p0->cz >= (1024 << FRACBITS) && ctx->p1->cz >= (1024 << FRACBITS)) HW_VS(31);
        if (ctx->p0->cz >= (512 << FRACBITS) && ctx->p1->cz >= (512 << FRACBITS)) HW_VS(32);
        else HW_VS(33);
    }
#endif
#if HW_WALL_TINY
    /* distance LOD study: far walls only a few pixels big are not drawn
     * (occlusion above still counts them) - measurement upper bound */
    {
        int t_w = ctx->p1->px - ctx->p0->px;
        int t_h0 = (ya1 - bot_overlap) - (ya0 + top_overlap);
        int t_h1 = (yb1 - bot_overlap) - (yb0 + top_overlap);
        if ((t_w <= 4 || (t_h0 <= 3 && t_h1 <= 3)) &&
            ctx->p0->cz >= (512 << FRACBITS) && ctx->p1->cz >= (512 << FRACBITS))
            return;
    }
#endif
    HW_EmitWallFromCtx(ctx, seg);
#if HW_VIS_STATS
    {
        uint32_t nq = (uint32_t)(uint16_t)(saturn_cmd_m_count - census_m0);
        hw_vis_stats[29] += nq;
        if (c_tiny) hw_vis_stats[30] += nq;
    }
#endif
}

/* Texture + quad emission for the wall described by hw_wctx (immediate
 * path above, or pass 2 from a hw_wall_rec_t). */
static HW_WFUSE HW_HOT_O2_NOTHREAD void HW_EmitWallFromCtx(
    hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg)
{
    uint8_t seg_flags = seg->flags;
    int tex_id = seg->tex_64;
    int ya0 = ctx->ya0, ya1 = ctx->ya1, yb0 = ctx->yb0, yb1 = ctx->yb1;

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

    if (__builtin_expect(!(seg_flags & SEG_FLAG_STATIC), 0))
    {
        /* Calculate vertical texture coordinate v_top according to DOOM pegging rules */
        int th = ctx->th;
        int v_top = ctx->cur_v;
        if ((seg_flags & (SEG_PEG_LOWER_UNPEGGED | SEG_PEG_BOTTOM)) == SEG_PEG_BOTTOM)
            v_top += th;

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
        if (th >= 64 && (v_top + ctx->wall_h) > th &&
            (ya1 - ya0) <= 32 && (yb1 - yb0) <= 32)
        {
            if (ctx->wall_h > th)
                ctx->wall_h = (int16_t)th;
            v_top = th - ctx->wall_h;
        }
        ctx->cur_v = (int16_t)v_top;
    }

    HW_EmitBakedWallQuads(ctx);
}

static HW_NOINLINE HW_HOT_O2 void HW_ProjectPortalEndpoint(hw_trans_pillar_t *tp, int dx, int dy)
{
    fixed_t cz, cx;
    ProjectMapVertex_2D_Frust(dx, dy, &cz, &cx);
    tp->cz = cz;
    tp->cx = cx;
    if (cz >= HW_NEAR_Z)
    {
        int s = HW_FastDiv(cz);
        tp->s  = s;
        tp->px = HW_ProjX(cx, s, HW_VIEW_CX);
    }
    else
    {
        tp->s  = 0;
        tp->px = 0;
    }
}

static HW_NOINLINE HW_HOT_O2 void HW_RecordSubsectorUnbakedPortals(hw_wall_emit_ctx_t *ctx)
{
    static hw_trans_pillar_t p_tp0, p_tp1;
#if HW_AP_1D
    ctx->portal_recorded = 0;
    return;
#endif
    const hw_aperture_state_t *ap = HW_APERTURE;
    int count = (int)ctx->portal_count;
    const hw_portal_seg_t *p = &HW_PORTAL_SEGS[ctx->portal_first];
    const int cam_x = hw_cam_x;
    const int cam_y = hw_cam_y;
    int prev_projected = 0;
    int i;

    for (i = 0; i < count; i++, p++)
    {
#ifndef SATURN_WIN32
        const int16_t *p_xy = &p->x0;
        int cross, obj_dx0, obj_dy0, seg_dx, seg_dy;
        __asm__(
            "mov.w\t@%5+, %1\n\t"   /* obj_dx0 = p->x0 */
            "mov.w\t@%5+, %2\n\t"   /* obj_dy0 = p->y0 */
            "mov.w\t@%5+, %3\n\t"   /* seg_dx  = p->dx */
            "mov.w\t@%5+, %4\n\t"   /* seg_dy  = p->dy */
            "sub\t%7, %2\n\t"       /* obj_dy0 = p->y0 - cam_y */
            "muls.w\t%2, %3\n\t"    /* MACL    = obj_dy0 * seg_dx */
            "sub\t%6, %1\n\t"       /* [slot] obj_dx0 = p->x0 - cam_x */
            "sts\tmacl, %0\n\t"     /* cross   = obj_dy0 * seg_dx (0 stall!) */
            "muls.w\t%1, %4\n\t"    /* MACL    = obj_dx0 * seg_dy */
            "sts\tmacl, %5\n\t"     /* p_xy    = obj_dx0 * seg_dy */
            "sub\t%5, %0"           /* cross   = obj_dy0 * seg_dx - obj_dx0 * seg_dy */
            : "=&r"(cross), "=&r"(obj_dx0), "=&r"(obj_dy0), "=&r"(seg_dx), "=&r"(seg_dy), "+r"(p_xy)
            : "r"(cam_x), "r"(cam_y)
            : "macl"
        );
        if (cross <= 0)
        {
            prev_projected = 0;
            continue;
        }
#else
        int seg_dx = (int)p->dx;
        int seg_dy = (int)p->dy;
        int obj_dx0 = (int)p->x0 - cam_x;
        int obj_dy0 = (int)p->y0 - cam_y;
        if (HW_CrossBackface(obj_dy0, seg_dx, obj_dx0, seg_dy) <= 0)
        {
            prev_projected = 0;
            continue;
        }
#endif

        if (prev_projected && p->chain_prev)
        {
            p_tp0 = p_tp1;
        }
        else
        {
            HW_ProjectPortalEndpoint(&p_tp0, obj_dx0, obj_dy0);
        }
        HW_ProjectPortalEndpoint(&p_tp1, obj_dx0 + seg_dx, obj_dy0 + seg_dy);
        prev_projected = 1;

        fixed_t cz0 = p_tp0.cz;
        fixed_t cz1 = p_tp1.cz;
        if ((cz0 < HW_NEAR_Z && cz1 < HW_NEAR_Z) ||
            (cz0 > HW_FAR_Z  && cz1 > HW_FAR_Z))
            continue;

        if (__builtin_expect(cz0 >= HW_NEAR_Z && cz1 >= HW_NEAR_Z, 1))
        {
            int xa = p_tp0.px;
            int xb = p_tp1.px;
            if (xb <= xa || xb <= 0 || xa >= HW_VIEW_W)
                continue;
            int seg_sx = (xa < 0) ? 0 : xa;
            int seg_ex = (xb > HW_VIEW_W) ? HW_VIEW_W : xb;
            int ex_m1  = seg_ex - 1;
            if (__builtin_expect((unsigned int)(seg_sx ^ ex_m1) < 32u, 1))
            {
                int w0 = (int)((unsigned int)seg_sx >> 5);
                uint32_t mask = hw_mask_left[seg_sx & 31] & hw_mask_right[ex_m1 & 31];
                if ((hw_solid_mask[w0] & mask) == mask)
                    continue;
                int16_t yt = hw_ap_words[w0].yt_min;
                int16_t yb = hw_ap_words[w0].yb_max;
                if (yt >= yb)
                    continue;
                ctx->ytop_min = yt;
                ctx->ybot_max = yb;
            }
            else
            {
                if (hw_solid_count > 0 && HW_CheckSpanOcclusion(seg_sx, seg_ex) == 2)
                    continue;
                if (ap->aperture_active)
                {
                    int ap_pack = HW_GetSpanAperture(seg_sx, seg_ex);
                    if (ap_pack < 0)
                        continue;
                    ctx->ytop_min = (int16_t)(ap_pack >> 16);
                    ctx->ybot_max = (int16_t)ap_pack;
                }
                else
                {
                    ctx->ytop_min = 0;
                    ctx->ybot_max = (int16_t)HW_VIEW_H;
                }
            }
            ctx->seg_sx = (int16_t)seg_sx;
            ctx->seg_ex = (int16_t)seg_ex;
            ctx->p0     = &p_tp0;
            ctx->p1     = &p_tp1;
        }
        else
        {
            ctx->u_start = 0;
            ctx->u_span  = 64;
            if (!HW_ClipWallNearPlane(&p_tp0, &p_tp1, ctx))
                continue;
            if (ap->aperture_active)
            {
                int ap_pack = HW_GetSpanAperture(ctx->seg_sx, ctx->seg_ex);
                if (ap_pack < 0)
                    continue;
                ctx->ytop_min = (int16_t)(ap_pack >> 16);
                ctx->ybot_max = (int16_t)ap_pack;
            }
            else
            {
                ctx->ytop_min = 0;
                ctx->ybot_max = (int16_t)HW_VIEW_H;
            }
            ctx->p0 = &hw_p0_clipped;
            ctx->p1 = &hw_p1_clipped;
        }
        ctx->portal_recorded = 0;
        HW_RecordSegPortalAperture(ctx, (int)p->back_sec_idx);
    }
}

/* Outlined Pillar Pair Projection + 1D Frustum + Occlusion + Aperture Check
 * Takes 3 register arguments (r4=pair_key, r5=obj_dx, r6=obj_dy), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 int HW_ProjectAndCullPillarPair(
    uint32_t pair_key, int obj_dx, int obj_dy)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    ctx->portal_recorded = 0;
    uint8_t pil_gen = hw_cur_pil_gen;
    int idx0 = (int)(pair_key >> 8);
    int idx1 = (int)(pair_key & 0xFFu);

    if (hw_pil_gen[idx0] != pil_gen)
    {
        hw_pil_gen[idx0] = pil_gen;
        HW_TransformSubsectorPillar(idx0, obj_dx, obj_dy);
    }

    if (hw_pil_gen[idx1] != pil_gen)
    {
        hw_pil_gen[idx1] = pil_gen;
        const int16_t *p1_xy = &hw_cur_pillars[idx1].x;
        int p1x = (int)*p1_xy++;
        int p1z = (int)*p1_xy;
        HW_TransformSubsectorPillar(idx1, p1x - hw_cam_x, p1z - hw_cam_y);
    }

    const hw_trans_pillar_t *tp0 = &local_trans_pillars[idx0];
    const hw_trans_pillar_t *tp1 = &local_trans_pillars[idx1];
    if ((tp0->cz < HW_NEAR_Z && tp1->cz < HW_NEAR_Z) ||
        (tp0->cz > HW_FAR_Z  && tp1->cz > HW_FAR_Z))
    {
        return -1;
    }

    ctx->p0 = tp0;
    ctx->p1 = tp1;

    if (__builtin_expect(tp0->cz >= HW_NEAR_Z && tp1->cz >= HW_NEAR_Z, 1))
    {
        int xa = tp0->px;
        int xb = tp1->px;
        if (xb <= xa || xb <= 0 || xa >= HW_VIEW_W)
            return -1;

        int seg_sx = (xa < 0) ? 0 : xa;
        int seg_ex = (xb > HW_VIEW_W) ? HW_VIEW_W : xb;
        int ex_m1  = seg_ex - 1;
        int occl_state;
        if (__builtin_expect((unsigned int)(seg_sx ^ ex_m1) < 32u, 1))
        {
            int w0 = (int)((unsigned int)seg_sx >> 5);
            uint32_t sm = hw_solid_mask[w0];
            if (sm == 0)
            {
                occl_state = 0;
            }
            else
            {
                uint32_t mask = hw_mask_left[seg_sx & 31] & hw_mask_right[ex_m1 & 31];
                uint32_t hit  = sm & mask;
                if (hit == mask)
                    return -1;
                occl_state = (hit != 0);
            }
            const hw_ap_word_t *aw = &hw_ap_words[w0];
            int16_t yt = aw->yt_min;
            int16_t yb = aw->yb_max;
            if (__builtin_expect((aw->yt_max - yt) > 8 || (yb - aw->yb_min) > 8, 0))
            {
                int ap_pack = HW_GetSpanAperture(seg_sx, seg_ex);
                if (ap_pack < 0)
                    return -1;
                yt = (int16_t)(ap_pack >> 16);
                yb = (int16_t)ap_pack;
            }
            else if (yt >= yb)
            {
                return -1;
            }
            ctx->ytop_min = yt;
            ctx->ybot_max = yb;
        }
        else
        {
            occl_state = (hw_solid_count > 0) ? HW_CheckSpanOcclusion(seg_sx, seg_ex) : 0;
            if (occl_state == 2)
                return -1;
            if (HW_APERTURE->aperture_active)
            {
                int ap_pack = HW_GetSpanAperture(seg_sx, seg_ex);
                if (ap_pack < 0)
                    return -1;
                ctx->ytop_min = (int16_t)(ap_pack >> 16);
                ctx->ybot_max = (int16_t)ap_pack;
            }
            else
            {
                ctx->ytop_min = 0;
                ctx->ybot_max = (int16_t)HW_VIEW_H;
            }
        }
        ctx->seg_sx          = (int16_t)seg_sx;
        ctx->seg_ex          = (int16_t)seg_ex;
        ctx->occl_state      = (uint8_t)occl_state;
        ctx->needs_near_clip = 0;
        return 3;
    }
    ctx->needs_near_clip = 1;
    return 2;
}

/* Zero-spill inner seg loop: exactly 7 live variables (seg, seg_end, pillars, cam_x, cam_y, last_pair_key, last_pair_state)
 * fit 100% in SH-2 callee-saved registers r8..r14 with 0 stack spills! */
static HW_NOINLINE HW_HOT_O2 void HW_WalkSubsectorBakedSegs(
    const baked_seg_t *seg, const baked_seg_t *seg_end, const saturn_wl_pillar_t *pillars)
{
    const int cam_x = hw_cam_x;
    const int cam_y = hw_cam_y;
    uint32_t last_pair_key = 0xFFFFFFFFu;
    int last_pair_state = 0; /* 0 = none, -1 = rejected, >= 0 = projected & visible */

    for (; seg < seg_end; seg++)
    {
#ifndef SATURN_WIN32
        uint32_t seg_w0 = *(const aliased_uint32_t *)seg;
        if (!(seg_w0 & 0xFFu))
            continue;
        uint32_t pair_key = seg_w0 >> 16;
#else
        if (!seg->flags)
            continue;
        uint32_t pair_key = ((uint32_t)seg->pillar_idx[0] << 8) | (uint32_t)seg->pillar_idx[1];
#endif

        /* 1. Backface Cull + Pillar Projection + 1D Frustum + Occlusion + Aperture
         *    (Evaluated ONCE per pillar pair BEFORE touching sectors[seg->back_sec_idx]!) */
        if (pair_key == last_pair_key)
        {
            if (last_pair_state < 0)
                continue;
        }
        else
        {
#ifndef SATURN_WIN32
            int cross, obj_dx, obj_dy, seg_dy;
            __asm__(
                "mov\t%6, %0\n\t"       /* %0 = pair_key */
                "extu.b\t%0, %3\n\t"    /* %3 = pair_key & 0xFF */
                "shlr8\t%0\n\t"         /* %0 = pair_key >> 8 */
                "shll2\t%3\n\t"         /* %3 = (pair_key & 0xFF) * 4 */
                "shll2\t%0\n\t"         /* %0 = (pair_key >> 8) * 4 */
                "add\t%5, %3\n\t"       /* %3 = &pillars[p1] */
                "add\t%5, %0\n\t"       /* %0 = &pillars[p0] */
                "mov.w\t@%0+, %1\n\t"   /* obj_dx = p0->x */
                "mov.w\t@%0+, %2\n\t"   /* obj_dy = p0->z */
                "mov.w\t@%3+, %0\n\t"   /* cross  = p1->x */
                "mov.w\t@%3+, %3\n\t"   /* seg_dy = p1->z */
                "sub\t%1, %0\n\t"       /* seg_dx = p1->x - p0->x */
                "sub\t%2, %3\n\t"       /* seg_dy = p1->z - p0->z */
                "sub\t%8, %2\n\t"       /* obj_dy = p0->z - cam_y */
                "muls.w\t%2, %0\n\t"    /* MACL   = obj_dy * seg_dx */
                "sub\t%7, %1\n\t"       /* [slot] obj_dx = p0->x - cam_x */
                "sts\tmacl, %0\n\t"     /* cross  = obj_dy * seg_dx (0 stall!) */
                "muls.w\t%1, %3\n\t"    /* MACL   = obj_dx * seg_dy */
                "mov\t%6, %4\n\t"       /* [slot] last_pair_key = pair_key (0 stall!) */
                "sts\tmacl, %3\n\t"     /* seg_dy = obj_dx * seg_dy */
                "sub\t%3, %0"           /* cross  = obj_dy * seg_dx - obj_dx * seg_dy */
                : "=&r"(cross), "=&r"(obj_dx), "=&r"(obj_dy), "=&r"(seg_dy), "+r"(last_pair_key)
                : "r"(pillars), "r"(pair_key), "r"(cam_x), "r"(cam_y)
                : "macl"
            );
            if (cross <= 0)
            {
                seg += seg->baked_light & 15u;
                last_pair_state = -1;
                continue;
            }
#else
            last_pair_key = pair_key;
            const saturn_wl_pillar_t *p0_raw = &pillars[pair_key >> 8];
            const saturn_wl_pillar_t *p1_raw = &pillars[pair_key & 0xFFu];
            int p0x = (int)p0_raw->x;
            int p0z = (int)p0_raw->z;
            int seg_dx = (int)p1_raw->x - p0x;
            int seg_dy = (int)p1_raw->z - p0z;
            int obj_dx = p0x - cam_x;
            int obj_dy = p0z - cam_y;

            if (HW_CrossBackface(obj_dy, seg_dx, obj_dx, seg_dy) <= 0)
            {
                seg += seg->baked_light & 15u;
                last_pair_state = -1;
                continue;
            }
#endif

            last_pair_state = HW_ProjectAndCullPillarPair(pair_key, obj_dx, obj_dy);
            if (last_pair_state < 0)
                continue;
        }

        HW_EmitVisibleBakedSeg(seg);
    }
}

#ifndef HW_ASM_WALK
#define HW_ASM_WALK (HW_ASM && HW_ASM_SPANAP && !HW_DEFER_WALLS && !HW_VIS_STATS && !HW_WALL_TINY)
#endif
#if HW_ASM_WALK
/* Compact rewrite stage A: seg walk + pillar projection/cull + the front of
 * HW_EmitVisibleBakedSeg in saturn_hw_asm.s (hw_asm_walk); layout there. */
extern void hw_asm_walk(const baked_seg_t *seg, const baked_seg_t *seg_end,
                        const saturn_wl_pillar_t *pillars, const void *const *ctx);
_Static_assert(!HW_DEFER_WALLS && !HW_VIS_STATS && !HW_WALL_TINY, "hw_asm_walk: default build only");
_Static_assert(offsetof(hw_wall_emit_ctx_t, needs_near_clip) == 7 &&
               offsetof(hw_wall_emit_ctx_t, seg_sx) == 8 &&
               offsetof(hw_wall_emit_ctx_t, ya0) == 16 &&
               offsetof(hw_wall_emit_ctx_t, wall_h) == 28 &&
               offsetof(hw_wall_emit_ctx_t, cur_v) == 30 &&
               offsetof(hw_wall_emit_ctx_t, sec_ceiling) == 32 &&
               offsetof(hw_wall_emit_ctx_t, p0) == 40 &&
               offsetof(hw_wall_emit_ctx_t, seg) == 48 &&
               offsetof(hw_wall_emit_ctx_t, dz_top) == 52 &&
               offsetof(hw_wall_emit_ctx_t, dz_bot) == 56, "hw_asm_walk: hw_wctx layout");
_Static_assert(sizeof(baked_seg_t) == 16 && offsetof(baked_seg_t, flags) == 3 &&
               offsetof(baked_seg_t, tex_64) == 4 && offsetof(baked_seg_t, mid_static_v_offset) == 6 &&
               offsetof(baked_seg_t, back_sec_idx) == 14, "hw_asm_walk: baked_seg_t layout");
_Static_assert(sizeof(hw_trans_pillar_t) == 16 && offsetof(hw_trans_pillar_t, cz) == 4 &&
               offsetof(hw_trans_pillar_t, s) == 8 && offsetof(hw_trans_pillar_t, px) == 12,
               "hw_asm_walk: hw_trans_pillar_t layout");
_Static_assert(offsetof(hw_frust_t, cz_origin) == 44 && offsetof(hw_frust_t, cx_origin) == 48,
               "hw_asm_walk: hw_frust layout");
_Static_assert(offsetof(hw_sec_fast_t, dz_floor) == 4 && offsetof(hw_sec_fast_t, is_sky) == 12,
               "hw_asm_walk: hw_sec_fast_t layout");
_Static_assert(HW_FAR_Z == (1 << 30) && HW_VIEW_W == 320, "hw_asm_walk: constants");

static HW_NOINLINE HW_HOT_O2_NOTHREAD void HW_EmitWallFromCtx_Call(hw_wall_emit_ctx_t *ctx, const baked_seg_t *seg)
{
    HW_EmitWallFromCtx(ctx, seg);
}

/* Stage B1: one-band unclipped mono walls emitted inside hw_asm_walk
 * (layout in saturn_hw_asm.s at .Le_*); everything else -> C emitter. */
#ifndef HW_ASM_EMIT
#define HW_ASM_EMIT 1
#endif
_Static_assert(offsetof(hw_wall_emit_ctx_t, srca) == 24 && offsetof(hw_wall_emit_ctx_t, th) == 26 &&
               offsetof(hw_wall_emit_ctx_t, cur_tex_id) == 60 && offsetof(hw_wall_emit_ctx_t, tw) == 68 &&
               offsetof(hw_wall_emit_ctx_t, srca_mono) == 70 && offsetof(hw_wall_emit_ctx_t, srca_64) == 72 &&
               offsetof(hw_wall_emit_ctx_t, sub_m_start) == 74 && offsetof(hw_wall_emit_ctx_t, last_clip_x0) == 76 &&
               offsetof(hw_wall_emit_ctx_t, u_start) == 92 && offsetof(hw_wall_emit_ctx_t, u_span) == 96,
               "hw_asm_walk emitter: hw_wctx layout");
_Static_assert(offsetof(hw_wall_emit_ctx_t, should_subdivide) == 64 && offsetof(hw_wall_emit_ctx_t, needs_clip) == 65 &&
               offsetof(hw_wall_emit_ctx_t, persp_strips) == 66 && offsetof(hw_wall_emit_ctx_t, mono_tw) == 80 &&
               offsetof(hw_wall_emit_ctx_t, mono_base_srca) == 82 && offsetof(hw_wall_emit_ctx_t, rem_h) == 84 &&
               offsetof(hw_wall_emit_ctx_t, q_xa) == 86 && offsetof(hw_wall_emit_ctx_t, q_xb) == 88 &&
               offsetof(hw_wall_emit_ctx_t, cmd_header1) == 104, "hw_asm_walk emitter: multi-band fields");
_Static_assert(offsetof(baked_seg_t, baked_light) == 2 && offsetof(baked_seg_t, u_start) == 8 &&
               offsetof(baked_seg_t, u_span) == 10 && sizeof(vdp1_cmd_t) == 32 && CMDBUF_M_CAP == 1280 &&
               SEG_FLAG_STATIC == 0x40 && SEG_FLAG_MID == 0x08,
               "hw_asm_walk emitter: layout/constants");
#if HW_ASM_EMIT
#ifndef HW_ASM_EMSTAT
#define HW_ASM_EMSTAT 0
#endif
#if HW_ASM_EMSTAT
/* Full-C fallbacks by reason (the other exits call the C helpers directly
 * and are not counted): [3] tw<=16 partial (clipped mono), [7] user clip
 * reset. [1] [2] [4] [5] [6] [8] [9] are no longer fallbacks. */
volatile uint32_t hw_asm_emstat[10];
#endif
static const void *const hw_asm_em_ctx[19] = {
    &saturn_cmd_m, (const void *)&saturn_cmd_m_count, &hw_wall_light_delta, wall_zlight_cmdcolr_LUT,
    (const void *)(uintptr_t)((((PMOD_WALL | PMOD_CLIP_IN) & 0xFFFFu)) << 16),
    (const void *)(uintptr_t)(((((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu)) << 16),
    &saturn_v1w_cells, &saturn_hw_wall_quads, &saturn_v1w_drop, &saturn_v1w_captured,
    &saturn_hw_tex_miss, (const void *)HW_LoadWallTexCtx,
#if HW_ASM_EMSTAT
    (const void *)hw_asm_emstat,
#else
    NULL,
#endif
    &saturn_wall_subdivision, (const void *)HW_ModNonPow2, (const void *)HW_WallXYClamp_Slow,
    hw_wall_coord_tmp, (const void *)HW_EmitSubdividedBakedSeg, (const void *)HW_EmitMultiBandBakedWall
};
#endif

static const void *const hw_asm_walk_ctx[30] = {
    &hw_wctx, &hw_cam_x, &hw_cam_y, local_trans_pillars,
    hw_pil_gen, &hw_cur_pil_gen, &hw_cur_pfrac, &hw_frust,
    saturn_master_oclut, hw_solid_mask, hw_mask_left, hw_mask_right,
    hw_ap_words, &hw_solid_count, &hw_aperture_state, &saturn_view_h,
    &saturn_view_cy, hw_sec_fast, &saturn_cmd_m_needs_ytrim, hw_asm_spanap_ctx,
    (const void *)HW_TransformPillar_Shifted, (const void *)HW_CheckSpanOcclusion,
    (const void *)HW_OccludeZeroHeightSeg, (const void *)HW_ClipSegNearPlane,
    (const void *)HW_MarkSubSolidSpan, (const void *)HW_RecordSegPortalAperture,
    (const void *)HW_EmitWallFromCtx_Call, &hw_p0_clipped, &hw_p1_clipped,
#if HW_ASM_EMIT
    hw_asm_em_ctx
#else
    NULL
#endif
};
#endif /* HW_ASM_WALK */

/* Master Blueprint: Zero-Runtime-Math 1D LOD Subsector Wall Renderer (Phase 3)
 * Fast Culling & Pillar Projection Loop (< 400 bytes, zero register spills):
 * rejects ~72% of segs in registers and dispatches surviving visible segs to HW_EmitVisibleBakedSeg. */
static HW_NOINLINE HW_HOT_O2 int HW_RenderSubsectorBakedWalls(int subnum, const hw_sec_fast_t *sf)
{
    if (!saturn_baked_walls_active || (unsigned)subnum >= (unsigned)saturn_baked_wall_num_subsectors)
        return 0;

    unsigned short tw0 = SAT_FRT();
    const saturn_wl_subsector_t *ss = &saturn_baked_wall_subsectors[subnum];
    int num_segs = (int)ss->num_segs;
    hw_wctx.portal_first = ss->portal_first;
    hw_wctx.portal_count = ss->portal_count;
    hw_wctx.sec_ceiling  = sf->dz_ceil;
    hw_wctx.sec_floor    = sf->dz_floor;
    hw_wctx.is_sky_ceil  = sf->is_sky;
    hw_wall_light_delta  = sf->light_delta;
    if (num_segs <= 0)
    {
        if (hw_wctx.portal_count > 0 && hw_solid_count < HW_VIEW_W)
        {
#if HW_PROF_SUBTIMERS
            unsigned short tu0 = SAT_FRT();
#endif
            HW_RecordSubsectorUnbakedPortals(&hw_wctx);
#if HW_PROF_SUBTIMERS
            hw_prof_unb_acc += HW_ProfDelta(tu0, SAT_FRT());
#endif
        }
        hw_prof_segs_acc += HW_ProfDelta(tw0, SAT_FRT());
        return 1;
    }
    int num_pil = (int)ss->num_pillars;
    if (num_pil <= 0 || num_pil > HW_MAX_SUBSECTOR_PILLARS)
        return 0;

    uint8_t pil_gen = (uint8_t)(hw_cur_pil_gen + 1u);
    if (__builtin_expect(pil_gen == 0, 0))
    {
        aliased_uint32_t *pg32 = (aliased_uint32_t *)hw_pil_gen;
        for (int i = 0; i < (int)(sizeof(hw_pil_gen) / sizeof(uint32_t)); i++)
            pg32[i] = 0;
        pil_gen = 1u;
    }
    hw_cur_pil_gen = pil_gen;

    const saturn_wl_pillar_t *pillars = &saturn_baked_wall_pillars[ss->pillar_start];
    hw_cur_pillars = pillars;
    hw_cur_pfrac = saturn_baked_wall_pillar_frac ? &saturn_baked_wall_pillar_frac[ss->pillar_start] : hw_zero_pfrac;
    const baked_seg_t *seg = &saturn_baked_wall_segs[ss->seg_start];
    const baked_seg_t *seg_end = seg + num_segs;
    hw_wctx.seg_end = seg_end;

    if (__builtin_expect(hw_wctx_tex_frame != hw_render_frame, 0))
    {
        hw_wctx_tex_frame = hw_render_frame;
        hw_wctx.cur_tex_id = -1;
    }
    hw_wctx.last_clip_x0 = -1;
    hw_wctx.last_clip_x1 = -1;
    hw_wctx.n_vis        = 0;
    hw_wctx.sub_m_start  = saturn_cmd_m_count;
    hw_wctx.ytop_min     = 0;
    hw_wctx.ybot_max     = (int16_t)HW_VIEW_H;

#if HW_ASM_WALK
    hw_asm_walk(seg, seg_end, pillars, hw_asm_walk_ctx);
#else
    HW_WalkSubsectorBakedSegs(seg, seg_end, pillars);
#endif
#if HW_PROF_SUBTIMERS
    unsigned short tw1 = SAT_FRT();
    hw_prof_wseg_acc += HW_ProfDelta(tw0, tw1);
#endif

    if (hw_wctx.portal_count > 0 && hw_solid_count < HW_VIEW_W)
    {
        HW_RecordSubsectorUnbakedPortals(&hw_wctx);
#if HW_PROF_SUBTIMERS
        hw_prof_unb_acc += HW_ProfDelta(tw1, SAT_FRT());
#endif
    }

    saturn_hw_seg_iter += (unsigned int)num_segs;
    saturn_hw_seg_vis  += (unsigned int)hw_wctx.n_vis;
    if (hw_wctx.last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        hw_wctx.last_clip_x0 = -1;
        hw_wctx.last_clip_x1 = -1;
    }
    hw_prof_segs_acc += HW_ProfDelta(tw0, SAT_FRT());
    return 1;
}

#if HW_DEFER_WALLS
/* One recorded wall -> hw_wctx -> quads (both CPUs' pass 2). */
HW_ALWAYS_INLINE void HW_EmitDeferredRec(hw_wall_emit_ctx_t *ctx, const hw_wall_rec_t *r)
{
        ctx->p0               = &r->p0;
        ctx->p1               = &r->p1;
        ctx->dz_top           = r->dz_top;
        ctx->dz_bot           = r->dz_bot;
        ctx->u_start          = r->u_start;
        ctx->u_span           = r->u_span;
        ctx->inv_u_span       = r->inv_u_span;
        ctx->ya0              = r->ya0;
        ctx->ya1              = r->ya1;
        ctx->yb0              = r->yb0;
        ctx->yb1              = r->yb1;
        ctx->seg_sx           = r->seg_sx;
        ctx->seg_ex           = r->seg_ex;
        ctx->ytop_min         = r->ytop_min;
        ctx->ybot_max         = r->ybot_max;
        ctx->wall_h           = r->wall_h;
        ctx->cur_v            = r->cur_v;
        ctx->top_overlap      = r->top_overlap;
        ctx->bot_overlap      = r->bot_overlap;
        ctx->was_near_clipped = r->was_near_clipped;
        ctx->occl_state       = r->occl_state;
        ctx->seg              = r->seg;
        HW_EmitWallFromCtx(ctx, r->seg);
}

#ifndef HW_SLAVE_TU
/* Emit one deferred subsector: its walls, then its sprites, into a fresh
 * m range. The solid mask must already be the subsector's (snapshot
 * restored, or the live mask when emitted straight from pass 1). */
static HW_NOINLINE HW_HOT_O2 void HW_EmitDeferredSub(const hw_wall_sub_t *h)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    uint16_t m_start = saturn_cmd_m_count;

    hw_wall_light_delta = h->light_delta;
    ctx->sub_m_start  = m_start;
    ctx->last_clip_x0 = -1;
    ctx->last_clip_x1 = -1;

    const hw_wall_rec_t *r  = &hw_defer_rec[h->rec_first];
    const hw_wall_rec_t *re = r + h->rec_count;
    for (; r < re; r++)
        HW_EmitDeferredRec(ctx, r);
    if (ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        ctx->last_clip_x0 = -1;
        ctx->last_clip_x1 = -1;
    }
    /* Sprites stay in pass 1 (their occlusion test and texture upload
     * order are unchanged); move their quads after this subsector's walls. */
    if (h->spr_count)
    {
        uint32_t dst = saturn_cmd_m_count;
        if (dst + h->spr_count <= CMDBUF_M_CAP)
        {
            const uint32_t *s32 = (const uint32_t *)&saturn_cmd_m[h->spr_start];
            uint32_t *d32 = (uint32_t *)&saturn_cmd_m[dst];
            for (int k = 0; k < (int)h->spr_count * 8; k++)
                d32[k] = s32[k];
            saturn_cmd_m_count = (uint16_t)(dst + h->spr_count);
        }
    }

    sub_range_t *ranges = hw_cur_sub_ranges;
#ifndef SATURN_WIN32
    ((aliased_uint32_t *)&ranges[h->visit_idx])[0] =
        ((uint32_t)m_start << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
#else
    ranges[h->visit_idx].m_start = m_start;
    ranges[h->visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);
#endif
}

/* Restore item i's occlusion state and emit it (master). */
static void HW_EmitDeferredIdx(int i)
{
    const hw_wall_sub_t *h = &hw_defer_sub[i];
    if (h->snap >= 0)
    {
        const hw_solid_snap_t *sn = &hw_defer_snap[h->snap];
        for (int w = 0; w < HW_SOLID_WORDS; w++)
            hw_solid_mask[w] = sn->mask[w];
        hw_solid_summary = sn->summary;
        hw_solid_any     = sn->any;
        hw_solid_count   = sn->count;
    }
    else
    {
        hw_solid_count = 0;     /* no occlusion test needed / nothing solid yet */
    }
    HW_EmitDeferredSub(h);
}

/* Emit items [lo, hi), keeping the live occlusion state (the walk's). */
static void HW_EmitDeferredRange(int lo, int hi)
{
    if (lo >= hi)
        return;
    uint32_t live_mask[HW_SOLID_WORDS];
    int      live_count   = hw_solid_count;
    uint32_t live_summary = hw_solid_summary;
    uint32_t live_any     = hw_solid_any;
    for (int w = 0; w < HW_SOLID_WORDS; w++)
        live_mask[w] = hw_solid_mask[w];
    for (int i = lo; i < hi; i++)
        HW_EmitDeferredIdx(i);
    for (int w = 0; w < HW_SOLID_WORDS; w++)
        hw_solid_mask[w] = live_mask[w];
    hw_solid_summary = live_summary;
    hw_solid_any     = live_any;
    hw_solid_count   = live_count;
}

#if HW_SLAVE_WALLS && !defined(SATURN_WIN32)
/* ---- pass 2 shared with the slave (master side) ---- */
static int hw_sw_on    = 0;     /* the slave takes deferred walls this frame */
static int hw_sw_m_low = 0;     /* lowest item the master emitted from the back */
extern unsigned char saturn_slave_enabled;
#define HW_WALL_IPC() (&((volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED)->walls)

/* Frame start, before the walk (hw_cur_sub_ranges already set). */
static void HW_SlaveWalls_BeginFrame(int enabled)
{
    saturn_cmd_sw_count = 0;
    hw_sw_on = enabled;
    if (!enabled)
        return;
    volatile hw_wall_ipc_t *w = HW_WALL_IPC();
    w->ready     = 0;
    w->walk_done = 0;
    w->m_claim   = 0x7FFF;
    w->s_claim   = -1;
    w->s_done    = 0;
    w->s_stopped = 0;
    w->s_cmds    = 0;
    w->s_quads   = 0;
    w->s_strips  = 0;
    w->s_miss    = 0;
    w->s_subs    = 0;
    w->subs   = (uint32_t)(uintptr_t)hw_defer_sub;
    w->recs   = (uint32_t)(uintptr_t)hw_defer_rec;
    w->snaps  = (uint32_t)(uintptr_t)hw_defer_snap;
    w->ranges = (uint32_t)(uintptr_t)hw_cur_sub_ranges;
    w->cmd_m  = (uint32_t)(uintptr_t)saturn_cmd_m;
    w->gen    = (int16_t)(w->gen + 1);  /* last: the slave starts on this */
    int dummy = w->gen;
    (void)dummy;
    __asm__ volatile("" ::: "memory");
}

/* After the slave's flat queue drained, before Saturn_CmdBuf_CullOccluded:
 * wait for the slave to stop taking walls, emit what neither CPU took. */
static void HW_SlaveWalls_Finish(void)
{
    if (!hw_sw_on)
        return;
    volatile hw_wall_ipc_t *w = HW_WALL_IPC();
    int guard = 1000000;    /* x 64 cycles, like the flat wait */
    while (!w->s_stopped && saturn_slave_enabled && --guard > 0)
        HW_BUS_YIELD();
    int s_done = w->s_done;
    if (!w->s_stopped)
    {
        /* slave gone (timeout): take everything it hadn't finished */
        saturn_hw_slave_timeout++;
        if (s_done < 0) s_done = 0;
    }
    HW_EmitDeferredRange(s_done, hw_sw_m_low);
    saturn_cmd_sw_count    = (uint16_t)(w->s_stopped ? w->s_cmds : 0);
    saturn_hw_wall_quads  += (unsigned int)w->s_quads;
    saturn_hw_wall_strips += (unsigned int)w->s_strips;
    saturn_hw_tex_miss    += (unsigned int)w->s_miss;
    hw_sw_on = 0;
    hw_defer_nsub = 0;
    hw_defer_nrec = 0;
    hw_defer_nsnap = 0;
}
#else
static void HW_SlaveWalls_BeginFrame(int enabled) { (void)enabled; }
static void HW_SlaveWalls_Finish(void) { }
#endif

/* Pass 1, end of a subsector that recorded (rec_first.. = its records):
 * queue it for pass 2, or emit it right now if it needs an occlusion
 * snapshot and the snapshot pool is full. */
static void HW_DeferCloseSub(int visit_idx, int subnum, int rec_first,
                             int spr_start, int spr_count, int16_t light_delta)
{
    int rec_count = hw_defer_nrec - rec_first;
    if (rec_count <= 0)
        return;                 /* no walls: the sprites are already in place */
    hw_wall_sub_t *h = &hw_defer_sub[hw_defer_nsub];
    h->spr_start   = (uint16_t)spr_start;
    h->spr_count   = (uint16_t)spr_count;
    h->visit_idx   = (uint16_t)visit_idx;
    h->subnum      = (uint16_t)subnum;
    h->rec_first   = (uint16_t)rec_first;
    h->rec_count   = (uint16_t)rec_count;
    h->light_delta = light_delta;
    h->snap        = -1;
    if ((hw_defer_need_snap || HW_DEFER_ALWAYS_SNAP) && hw_solid_count > 0)
    {
        if (hw_defer_nsnap >= HW_DEFER_SNAP_CAP)
        {
            HW_EmitDeferredSub(h);          /* live mask is this subsector's */
            hw_defer_nrec = rec_first;
            return;
        }
        hw_solid_snap_t *sn = &hw_defer_snap[hw_defer_nsnap];
        for (int w = 0; w < HW_SOLID_WORDS; w++)
            sn->mask[w] = hw_solid_mask[w];
        sn->summary = hw_solid_summary;
        sn->any     = hw_solid_any;
        sn->count   = hw_solid_count;
        h->snap = (int16_t)hw_defer_nsnap++;
    }
    hw_defer_nsub++;
#if HW_SLAVE_WALLS && !defined(SATURN_WIN32)
    if (hw_sw_on)
    {
        /* Mark the visit's m range pending: pass 2 (slave, or master in
         * Finish) rewrites it with the real walls+sprites range. Until then
         * HW_StealPick (HW_FLAT_STEAL) must not take it as an empty m range
         * for a stolen floor job, or one side's write would drop the
         * other's quads. */
        ((volatile sub_range_t *)((uint32_t)hw_cur_sub_ranges | SATURN_WRAM_H_UNCACHED))[visit_idx].m_count = HW_RANGE_PENDING;
        HW_WALL_IPC()->ready = hw_defer_nsub;   /* records + header written above */
    }
#endif
}

/* Pass 2: emit every queued subsector in visit order. */
static HW_NOINLINE HW_HOT_O2 void HW_EmitDeferredWalls(void)
{
#ifdef HW_DEFER_SKIP_EMIT   /* measurement only: pass 2 costs the master nothing (walls missing) */
    int nsub = 0;
#else
    int nsub = hw_defer_nsub;
#endif
#if HW_SLAVE_WALLS && !defined(SATURN_WIN32)
    if (hw_sw_on)
    {
        /* The slave has been taking items from the front since the walk
         * began; take them from the back until we meet it. What's left
         * between us is emitted by HW_SlaveWalls_Finish. */
        volatile hw_wall_ipc_t *w = HW_WALL_IPC();
        w->ready     = nsub;
        w->walk_done = 1;
        int i = nsub - 1;
        if (nsub > 0)
        {
            uint32_t live_mask[HW_SOLID_WORDS];
            int      live_count   = hw_solid_count;
            uint32_t live_summary = hw_solid_summary;
            uint32_t live_any     = hw_solid_any;
            for (int k = 0; k < HW_SOLID_WORDS; k++)
                live_mask[k] = hw_solid_mask[k];
            for (; i >= 0; i--)
            {
                w->m_claim = i;
                int rb = w->m_claim;            /* the claim is on the bus before we look */
                (void)rb;
                __asm__ volatile("" ::: "memory");
                if (w->s_claim >= i)
                    break;
                HW_EmitDeferredIdx(i);
            }
            for (int k = 0; k < HW_SOLID_WORDS; k++)
                hw_solid_mask[k] = live_mask[k];
            hw_solid_summary = live_summary;
            hw_solid_any     = live_any;
            hw_solid_count   = live_count;
        }
        hw_sw_m_low = i + 1;
        return;                 /* the queue stays valid until Finish */
    }
#endif
    HW_EmitDeferredRange(0, nsub);
    hw_defer_nsub = 0;
    hw_defer_nrec = 0;
    hw_defer_nsnap = 0;
}
#endif /* !HW_SLAVE_TU */

#if defined(HW_SLAVE_TU) && !defined(SATURN_WIN32)
/* ---- pass 2 shared with the master (slave side, saturn_hw_slave_walls.c).
 * Everything the master writes during the walk (queue headers, records,
 * snapshots, the pass-1 sprite quads, ranges) is read cache-through. */
static int hw_sw_gen    = 0x7FFFFFFF;  /* last frame generation seen (never a 16-bit value) */
static int hw_sw_next   = 0;           /* next item from the front */
static int hw_sw_active = 0;           /* still taking items this frame */
static hw_wall_rec_t   hw_sw_rec;      /* private copies (not on the small stack) */
static hw_wall_sub_t   hw_sw_sub;
static hw_solid_snap_t hw_sw_snap;

/* SH-2 cache-through alias (bit 29), valid for High and Low WRAM alike;
 * SATURN_WRAM_H_UNCACHED (0x26000000) is right for High WRAM only. */
#define HW_SW_THRU 0x20000000u

static void HW_SW_Read32(void *dst, uint32_t src, int bytes)
{
    const volatile uint32_t *s = (const volatile uint32_t *)(src | HW_SW_THRU);
    uint32_t *d = (uint32_t *)dst;
    for (bytes >>= 2; bytes > 0; bytes--)
        *d++ = *s++;
}

static void HW_SW_Read16(void *dst, uint32_t src, int bytes)
{
    const volatile uint16_t *s = (const volatile uint16_t *)(src | HW_SW_THRU);
    uint16_t *d = (uint16_t *)dst;
    for (bytes >>= 1; bytes > 0; bytes--)
        *d++ = *s++;
}

static HW_NOINLINE HW_HOT_O2 void HW_SlaveEmitSub(volatile hw_wall_ipc_t *w, int j)
{
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    hw_wall_sub_t *h = &hw_sw_sub;
    HW_SW_Read16(h, w->subs + (uint32_t)j * sizeof(hw_wall_sub_t), (int)sizeof(hw_wall_sub_t));
    if (h->snap >= 0)
    {
        HW_SW_Read32(&hw_sw_snap, w->snaps + (uint32_t)h->snap * sizeof(hw_solid_snap_t),
                     (int)sizeof(hw_solid_snap_t));
        for (int k = 0; k < HW_SOLID_WORDS; k++)
            hw_solid_mask[k] = hw_sw_snap.mask[k];
        hw_solid_summary = hw_sw_snap.summary;
        hw_solid_any     = hw_sw_snap.any;
        hw_solid_count   = hw_sw_snap.count;
    }
    else
    {
        hw_solid_count = 0;
    }

    const uint16_t m_start = saturn_cmd_m_count;    /* this CPU's region */
    hw_wall_light_delta = h->light_delta;
    ctx->sub_m_start  = m_start;
    ctx->last_clip_x0 = -1;
    ctx->last_clip_x1 = -1;
    uint32_t rsrc = w->recs + (uint32_t)h->rec_first * sizeof(hw_wall_rec_t);
    for (int k = 0; k < (int)h->rec_count; k++, rsrc += sizeof(hw_wall_rec_t))
    {
        HW_SW_Read32(&hw_sw_rec, rsrc, (int)sizeof(hw_wall_rec_t));
        HW_EmitDeferredRec(ctx, &hw_sw_rec);
    }
    if (ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        ctx->last_clip_x0 = -1;
        ctx->last_clip_x1 = -1;
    }
    if (h->spr_count)
    {
        uint32_t dst = saturn_cmd_m_count;
        if (dst + h->spr_count <= CMDBUF_M_CAP)
        {
            HW_SW_Read32(&saturn_cmd_m[dst], w->cmd_m + (uint32_t)h->spr_start * sizeof(vdp1_cmd_t),
                         (int)h->spr_count * (int)sizeof(vdp1_cmd_t));
            saturn_cmd_m_count = (uint16_t)(dst + h->spr_count);
        }
    }

    /* Saturn_CmdBuf_CullOccluded trims only the master's quads: trim ours */
    if (HW_VIEW_W <= 320 && HW_VIEW_H <= 168)
    {
        uint32_t *c32 = (uint32_t *)&saturn_cmd_m[m_start];
        uint32_t *c_end = (uint32_t *)&saturn_cmd_m[saturn_cmd_m_count];
        for (; c32 != c_end; c32 += 8)
        {
            if (((c32[0] >> 16) & 0x802Fu) != 0x0002u)
                continue;
            int yLt = (int16_t)c32[3], yRt = (int16_t)c32[4];
            int yRb = (int16_t)c32[5], yLb = (int16_t)c32[6];
            if ((yLt < -8 && yRt < -8) || (yLb > 168 + 8 && yRb > 168 + 8))
                Saturn_CmdBuf_TrimWallY(c32);
        }
    }

    sub_range_t *ranges = (sub_range_t *)(uintptr_t)w->ranges;     /* cache-through alias */
    ((volatile uint32_t *)&ranges[h->visit_idx])[0] =
        ((uint32_t)(HW_SW_M_BASE + m_start) << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
}

/* Slave idle loop (HW_Slave_FlatWorker_Loop): 1 = emitted one item. */
int HW_SlaveWallsService(void)
{
    volatile hw_wall_ipc_t *w = &((volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED)->walls;
    /* While active the generation can't change (the master waits for
     * s_stopped before its next frame), so an active idle spin costs one
     * bus read (ready); gen is polled only between frames. */
    if (!hw_sw_active)
    {
        int g = w->gen;
        if (g == hw_sw_gen)
            return 0;

        /* new frame: drop every cached line (view size, translations,
         * texture tags may all have changed), reset this CPU's region */
        hw_sw_gen    = g;
        hw_sw_next   = 0;
        hw_sw_active = 1;
        HW_PURGE_L1_CACHE();
        saturn_cmd_m        = (vdp1_cmd_t *)(uintptr_t)w->cmd_m + HW_SW_M_BASE;
        saturn_cmd_m_count  = 0;
        saturn_hw_wall_quads  = 0;
        saturn_hw_wall_strips = 0;
        saturn_hw_tex_miss    = 0;
        saturn_v1w_cells    = 0;
        saturn_v1w_drop     = 0;
        saturn_v1w_captured = 0;
        hw_wctx.cur_tex_id  = -1;
    }
    int j = hw_sw_next;
    if (j >= w->ready)
    {
        if (!w->walk_done)
            return 0;           /* walk still running: wait for more */
        if (j >= w->ready)
            goto stop;          /* queue complete (ready is final once walk_done) */
    }
    if (saturn_cmd_m_count > CMDBUF_M_CAP - 128)
        goto stop;              /* region nearly full: the master takes the rest */
    w->s_claim = j;
    {
        int rb = w->s_claim;    /* the claim is on the bus before we look */
        (void)rb;
        __asm__ volatile("" ::: "memory");
    }
    if (w->m_claim <= j)
        goto stop;              /* met the master */
    HW_SlaveEmitSub(w, j);
    hw_sw_next = j + 1;
    w->s_done  = j + 1;
    return 1;
stop:
    w->s_done   = j;
    w->s_cmds   = saturn_cmd_m_count;
    w->s_quads  = (int)saturn_hw_wall_quads;
    w->s_strips = (int)saturn_hw_wall_strips;
    w->s_miss   = (int)saturn_hw_tex_miss;
    w->s_subs   = j;
    w->s_stopped = 1;
    {
        int rb = w->s_stopped;
        (void)rb;
    }
    hw_sw_active = 0;
    return 0;
}
#endif /* HW_SLAVE_TU */
#else
static void HW_EmitDeferredWalls(void) { }
static void HW_SlaveWalls_BeginFrame(int enabled) { (void)enabled; }
static void HW_SlaveWalls_Finish(void) { }
#endif

static HW_NOINLINE HW_COLD_OS int HW_CheckNearSubsectorFrustum(const saturn_flat_bbox_t *fb)
{
    int dx0 = (int)fb->min_x - hw_cam_x;
    int dx1 = (int)fb->max_x - hw_cam_x;
    int dy0 = (int)fb->min_y - hw_cam_y;
    int dy1 = (int)fb->max_y - hw_cam_y;
    if (!(dx0 > 0 || dx1 < 0 || dy0 > 0 || dy1 < 0))
        return 1;
    int lx = hw_bbox_Lx, ly = hw_bbox_Ly;
    int xl_max = (lx >= 0) ? (int)fb->max_x : (int)fb->min_x;
    int yl_max = (ly >= 0) ? (int)fb->max_y : (int)fb->min_y;
    if (HW_MulsW_PairAdd(xl_max, lx, yl_max, ly, hw_bbox_Cl) < -(16 << 12))
        return 0;
    int rx = hw_bbox_Rx, ry = hw_bbox_Ry;
    int xr_max = (rx >= 0) ? (int)fb->max_x : (int)fb->min_x;
    int yr_max = (ry >= 0) ? (int)fb->max_y : (int)fb->min_y;
    if (HW_MulsW_PairAdd(xr_max, rx, yr_max, ry, hw_bbox_Cr) < -(16 << 12))
        return 0;
    return 1;
}

static const hw_sec_fast_t *hw_psec_fast = NULL;

static HW_NOINLINE HW_HOT_O2 int HW_EvalSubsectorFlatAperture(
    hw_sec_fast_t *sf, uint32_t flags, int s_max, int s_min)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    const int vcy = HW_VIEW_CY;
    const int vh  = HW_VIEW_H;
    int32_t dzf   = sf->dz_floor;
    int32_t dzc   = sf->dz_ceil;
    int is_sky    = sf->is_sky;

    if (__builtin_expect(!(flags & HW_FLAT_AP_OR_NEAR), 1))
    {
        if (dzf < 0)
        {
            int yf_max = HW_ProjY(dzf, s_max, vcy);
            int yb_att = (yf_max > vh) ? vh : yf_max;
            if (yb_att > sf->yb_attach)
                sf->yb_attach = (int16_t)yb_att;
            else
                yb_att = sf->yb_attach;
            ap->cur_sub_yb_attach = (int16_t)yb_att;
        }
        else
        {
            ap->cur_sub_yb_attach = sf->yb_attach;
        }

        if (!is_sky && dzc > 0)
        {
            int yc_min = HW_ProjY(dzc, s_max, vcy);
            int yt_att = (yc_min < 0) ? 0 : yc_min;
            if (yt_att < sf->yt_attach)
                sf->yt_attach = (int16_t)yt_att;
            else
                yt_att = sf->yt_attach;
            ap->cur_sub_yt_attach = (int16_t)yt_att;
        }
        else
        {
            ap->cur_sub_yt_attach = is_sky ? 0 : sf->yt_attach;
        }
        return (int)((flags & (HW_FLAT_USE_LOD1 | HW_FLAT_INSIDE_MASK)) | HW_FLAT_ALLOW_BOTH);
    }

    int check_ap     = (int)((flags & HW_FLAT_CHECK_AP) != 0);
    int near_or_psec = (int)((flags & HW_FLAT_NEAR_OR_PSEC) != 0);
    int ytop_min     = ap->cur_ytop_min;
    int ybot_max     = ap->cur_ybot_max;
    int pad          = (s_max < 8192) ? 1 : 2;

    int allow_floor = 1;
    int allow_ceil  = 1;
    int yf_max = vh;
    int yc_min = 0;

    if (dzf < 0)
    {
        if (!near_or_psec)
        {
            yf_max = HW_ProjY(dzf, s_max, vcy);
            int yb_att = (yf_max > vh) ? vh : yf_max;
            if (yb_att > sf->yb_attach)
                sf->yb_attach = (int16_t)yb_att;
            else
                yb_att = sf->yb_attach;
            ap->cur_sub_yb_attach = (int16_t)yb_att;
            if (check_ap)
            {
                int yf_min = HW_ProjY(dzf, s_min, vcy);
                if (yf_min >= ybot_max || yf_max + pad <= ytop_min)
                    allow_floor = 0;
            }
        }
        else
        {
            sf->yb_attach = (int16_t)vh;
            int yf_min = HW_ProjY(dzf, s_min, vcy);
            if (yf_min >= ybot_max)
                allow_floor = 0;
            if (check_ap)
            {
                yf_max = HW_ProjY(dzf, s_max, vcy);
                if (yf_max + pad <= ytop_min)
                    allow_floor = 0;
            }
        }
    }
    else if (!near_or_psec)
    {
        ap->cur_sub_yb_attach = sf->yb_attach;
    }

    if (!is_sky)
    {
        if (dzc > 0)
        {
            if (!near_or_psec)
            {
                yc_min = HW_ProjY(dzc, s_max, vcy);
                int yt_att = (yc_min < 0) ? 0 : yc_min;
                if (yt_att < sf->yt_attach)
                    sf->yt_attach = (int16_t)yt_att;
                else
                    yt_att = sf->yt_attach;
                ap->cur_sub_yt_attach = (int16_t)yt_att;
                if (check_ap)
                {
                    int yc_max = HW_ProjY(dzc, s_min, vcy);
                    if (yc_max <= ytop_min || yc_min - pad >= ybot_max)
                        allow_ceil = 0;
                }
            }
            else
            {
                sf->yt_attach = 0;
                int yc_max = HW_ProjY(dzc, s_min, vcy);
                if (yc_max <= ytop_min)
                    allow_ceil = 0;
                if (check_ap)
                {
                    yc_min = HW_ProjY(dzc, s_max, vcy);
                    if (yc_min - pad >= ybot_max)
                        allow_ceil = 0;
                }
            }
        }
        else if (!near_or_psec)
        {
            ap->cur_sub_yt_attach = sf->yt_attach;
        }
    }
    else if (!near_or_psec)
    {
        ap->cur_sub_yt_attach = 0;
    }

    if (check_ap)
    {
        int sub_y_max = (dzf < 0)
            ? yf_max
            : HW_ProjY(dzf, (dzf <= 0) ? s_max : s_min, vcy);
        if (sub_y_max + pad <= ytop_min)
            return -1;
        if (!is_sky)
        {
            int sub_y_min = (dzc > 0)
                ? yc_min
                : HW_ProjY(dzc, (dzc >= 0) ? s_max : s_min, vcy);
            if (sub_y_min - pad >= ybot_max)
                return -1;
        }
    }

    return (int)((flags & (HW_FLAT_USE_LOD1 | HW_FLAT_INSIDE_MASK)) | ((uint32_t)(allow_floor | (allow_ceil << 1)) << 2));
}

static HW_NOINLINE HW_HOT_O2 int HW_EvalSubsectorSpanSlow(int sx, int ex)
{
    if (hw_solid_count > 0 && HW_CheckSpanOcclusion(sx, ex) == 2)
        return -1;
    if (!HW_APERTURE->aperture_active)
        return HW_VIEW_H;
    if (ex - sx > 4)
    {
        sx += 2;
        ex -= 2;
    }
    return HW_GetSpanAperture(sx, ex);
}

static HW_NOINLINE HW_COLD_OS int HW_CheckPortalSubsectorStepCull(
    int subnum, const hw_sec_fast_t *sf, int s_min)
{
    if (!saturn_baked_wall_subsectors)
        return 0;
    const saturn_wl_subsector_t *wss = &saturn_baked_wall_subsectors[subnum];
    if (wss->max_bsec0 < 0)
        return 0;
    const hw_aperture_state_t *ap = HW_APERTURE;
    int32_t dz_f = hw_sec_fast[wss->max_bsec0].dz_floor;
    int floor_ok = (dz_f <= sf->dz_floor) ||
                   (dz_f <= 0 && HW_ProjY(dz_f, s_min, HW_VIEW_CY) >= ap->cur_ybot_max);
    if (!floor_ok)
        return 0;
    if (wss->max_bsec1 >= 0)
    {
        int32_t dz_c = hw_sec_fast[wss->max_bsec1].dz_ceil;
        int ceil_ok = (dz_c >= sf->dz_ceil) ||
                      (dz_c >= 0 && HW_ProjY(dz_c, s_min, HW_VIEW_CY) <= ap->cur_ytop_min);
        if (!ceil_ok)
            return 0;
    }
    int secnum = subsectors[subnum].sector;
    if (sf->has_things && HW_SubsectorHasThings(&sectors[secnum], (uint32_t)secnum, subnum))
        return 0;
    if (sf->is_sky)
        saturn_hw_sky_visible = 1;
    return 1;
}

#if HW_ASM_SUBAP
/* Front half in saturn_hw_asm.s (hw_asm_subap); layout documented there. */
typedef struct {
    int32_t max_cz, s_max, s_min, ytop_min, ybot_max, sx, ex, flags;
} hw_subap_out_t;
extern int hw_asm_subap(const saturn_flat_bbox_t *fb, int fully_inside,
                        hw_subap_out_t *o, const void *const *ctx);
_Static_assert(offsetof(hw_frust_t, Nx) == 12 && offsetof(hw_frust_t, Ny) == 14 &&
               offsetof(hw_frust_t, cz_off12) == 36 && offsetof(hw_frust_t, cx_off12) == 40,
               "hw_asm_subap hw_frust offsets");
_Static_assert(offsetof(hw_aperture_state_t, aperture_active) == 2 &&
               offsetof(hw_aperture_state_t, cur_ytop_min) == 20, "hw_asm_subap ap offsets");
#ifndef HW_LOD1_MIN_A   /* same defaults as in HW_CheckSubsectorAperture_C */
#define HW_LOD1_MIN_A 192
#define HW_LOD1_MAX_A 240
#define HW_LOD1_MIN_B 132
#define HW_LOD1_MAX_B 336
#endif
_Static_assert(HW_NEAR_Z_UNITS == 4 && HW_FAR_Z_UNITS == 16384 && HW_VIEW_CX == 160 &&
               HW_DBG_FLAT_LOD && HW_LOD1_MIN_A == 192 && HW_LOD1_MAX_A == 240 &&
               HW_LOD1_MIN_B == 132 && HW_LOD1_MAX_B == 336,
               "hw_asm_subap constants");
static const void *const hw_asm_subap_ctx[11] = {
    &hw_cam_x, &hw_cam_y, &hw_frust, saturn_master_oclut, &hw_solid_count,
    &hw_aperture_state, hw_solid_mask, hw_mask_left, hw_mask_right,
    hw_ap_words, &saturn_view_h
};

static HW_NOINLINE HW_HOT_O2 int HW_CheckSubsectorAperture_A(int subnum, hw_sec_fast_t *sf, int fully_inside)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    const saturn_flat_bbox_t *fb = saturn_baked_flat_bboxes ? &saturn_baked_flat_bboxes[subnum] : NULL;
    hw_subap_out_t o;
    int r = hw_asm_subap(fb, fully_inside, &o, hw_asm_subap_ctx);
    if (r <= 0)
    {
        if (r < 0)
            return -1;
        int vh = HW_VIEW_H;
        ap->cur_sub_yt_attach = 0;
        ap->cur_sub_yb_attach = (int16_t)vh;
        ap->cur_sub_ytop      = 0;
        ap->cur_sub_ybot      = (uint8_t)vh;
        return fully_inside | (int)HW_FLAT_ALLOW_BOTH;
    }
    if (__builtin_expect(r >= 2, 0))
    {
        if (r == 2)
        {
            if (!HW_CheckNearSubsectorFrustum(fb))
                return -1;
        }
        else
        {
            int ap_pack = HW_EvalSubsectorSpanSlow(o.sx, o.ex);
            if (ap_pack < 0)
                return -1;
            o.ytop_min = ap_pack >> 16;
            o.ybot_max = (int16_t)ap_pack;
        }
    }
    int ytop_min = o.ytop_min;
    int ybot_max = o.ybot_max;
    int s_max = o.s_max;
    int s_min = o.s_min;
    int any_behind = (o.flags >> 8) & 1;
    uint32_t keep = (uint32_t)o.flags & (HW_FLAT_USE_LOD1 | HW_FLAT_INSIDE_MASK);

    /* == the tail of HW_CheckSubsectorAperture_C below */
    int vh = HW_VIEW_H;
    ap->cur_sub_yt_attach = 0;
    ap->cur_sub_yb_attach = (int16_t)vh;
    ap->cur_sub_ytop      = 0;
    ap->cur_sub_ybot      = (uint8_t)vh;
    ap->cur_ytop_min      = (int16_t)ytop_min;
    ap->cur_ybot_max      = (int16_t)ybot_max;
    int check_ap = 0;
    if (ytop_min > 0 || ybot_max < vh)
    {
        int pad = (s_max < 8192) ? 1 : 2;
        check_ap = 1;
        ap->cur_sub_ytop = (ytop_min > pad) ? (uint8_t)(ytop_min - pad) : 0;
        ap->cur_sub_ybot = (ybot_max + pad < vh) ? (uint8_t)(ybot_max + pad) : (uint8_t)vh;
    }

    int near_or_psec = (any_behind || sf == hw_psec_fast);
    if (__builtin_expect((check_ap || near_or_psec) && !s_min, 0))
        s_min = HW_FastDiv(o.max_cz);

    uint32_t packed = (near_or_psec ? HW_FLAT_NEAR_OR_PSEC : 0u) |
                      (check_ap     ? HW_FLAT_CHECK_AP     : 0u) | keep;
    int flat_flags = HW_EvalSubsectorFlatAperture(sf, packed, s_max, s_min);
    if (__builtin_expect(flat_flags >= 0 && check_ap && !near_or_psec &&
                         !(flat_flags & (int)HW_FLAT_ALLOW_FLOOR) &&
                         (sf->is_sky || !(flat_flags & (int)HW_FLAT_ALLOW_CEIL)), 0))
    {
        if (HW_CheckPortalSubsectorStepCull(subnum, sf, s_min))
            return -1;
    }
    return flat_flags;
}

#if HW_ASM_CHECK
/* Check build: C reference first, then asm (the shared flat/attach updates
 * are monotone, so the second call sees the same inputs). [0] calls,
 * [1] mismatches, [2..5] first mismatch subnum, C result, asm result,
 * aperture-header diff. */
static int HW_CheckSubsectorAperture_C(int subnum, hw_sec_fast_t *sf, int fully_inside);
static volatile uint32_t hw_asm_chk_subap[8];
static HW_NOINLINE int HW_CheckSubsectorAperture(int subnum, hw_sec_fast_t *sf, int fully_inside)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int c = HW_CheckSubsectorAperture_C(subnum, sf, fully_inside);
    uint32_t h0[6];
    memcpy(h0, ap, 24);
    int a = HW_CheckSubsectorAperture_A(subnum, sf, fully_inside);
    uint32_t h1[6];
    memcpy(h1, ap, 24);
    /* cur_sub_ytop/ybot (+0,+1), attaches (+4..7), cur_ytop_min/ybot_max (+20..23) */
    uint32_t hd = (c >= 0) ? (((h0[0] ^ h1[0]) & 0xFFFF0000u) | (h0[1] ^ h1[1]) | (h0[5] ^ h1[5])) : 0;
    hw_asm_chk_subap[0]++;
    if ((a != c || hd) && hw_asm_chk_subap[1]++ == 0)
    {
        hw_asm_chk_subap[2] = (uint32_t)subnum;
        hw_asm_chk_subap[3] = (uint32_t)c;
        hw_asm_chk_subap[4] = (uint32_t)a;
        hw_asm_chk_subap[5] = hd;
    }
    return a;
}
#else
#define HW_CheckSubsectorAperture HW_CheckSubsectorAperture_A
#endif
#define HW_SUBAP_C_NAME HW_CheckSubsectorAperture_C
#else
#define HW_SUBAP_C_NAME HW_CheckSubsectorAperture
#endif /* HW_ASM_SUBAP */

#if !HW_ASM_SUBAP || HW_ASM_CHECK
/* Walk a subsector: returns flat_flags = (fully_inside & 3) | ((allow_floor | (allow_ceil << 1) | use_lod1) << 2), or -1 if culled */
static HW_NOINLINE HW_HOT_O2 int HW_SUBAP_C_NAME(int subnum, hw_sec_fast_t *sf, int fully_inside)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int default_flags = fully_inside | (int)HW_FLAT_ALLOW_BOTH;
    if (__builtin_expect(!saturn_baked_flat_bboxes, 0))
    {
        int vh = HW_VIEW_H;
        ap->cur_sub_yt_attach = 0;
        ap->cur_sub_yb_attach = (int16_t)vh;
        ap->cur_sub_ytop      = 0;
        ap->cur_sub_ybot      = (uint8_t)vh;
        return default_flags;
    }

    const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
    int b_min_x = fb->min_x, b_max_x = fb->max_x;
    int b_min_y = fb->min_y, b_max_y = fb->max_y;
    if (__builtin_expect(b_min_x == b_max_x && b_min_y == b_max_y, 0))
    {
        int vh = HW_VIEW_H;
        ap->cur_sub_yt_attach = 0;
        ap->cur_sub_yb_attach = (int16_t)vh;
        ap->cur_sub_ytop      = 0;
        ap->cur_sub_ybot      = (uint8_t)vh;
        return default_flags;
    }

    int cam_x = hw_cam_x, cam_y = hw_cam_y;
    int dx0 = b_min_x - cam_x;
    int dx1 = b_max_x - cam_x;
    int dy0 = b_min_y - cam_y;
    int dy1 = b_max_y - cam_y;
    int nx = hw_bbox_Nx, ny = hw_bbox_Ny;
    int32_t cz_off = hw_frust.cz_off12;
    int32_t cx_off = hw_frust.cx_off12;

    int x_cz_max = (nx >= 0) ? dx1 : dx0;
    int y_cz_max = (ny >= 0) ? dy1 : dy0;
    fixed_t max_cz = HW_MulsW_PairAdd(x_cz_max, nx, y_cz_max, ny, cz_off) << 4;
    if (__builtin_expect(max_cz < HW_NEAR_Z, 0))
    {
        if (dx0 > 0 || dx1 < 0 || dy0 > 0 || dy1 < 0)
            return -1;
        int vh = HW_VIEW_H;
        ap->cur_sub_yt_attach = 0;
        ap->cur_sub_yb_attach = (int16_t)vh;
        ap->cur_sub_ytop      = 0;
        ap->cur_sub_ybot      = (uint8_t)vh;
        return default_flags;
    }

    int x_cz_min = (nx >= 0) ? dx0 : dx1;
    int y_cz_min = (ny >= 0) ? dy0 : dy1;
    int32_t raw_min_cz = HW_MulsW_PairAdd(x_cz_min, nx, y_cz_min, ny, 0);
    if (raw_min_cz > (HW_FAR_Z_UNITS << 12))
        return -1;
    fixed_t true_min_cz = (raw_min_cz + cz_off) << 4;
    fixed_t min_cz = true_min_cz;

    int x_cx_min = (ny >= 0) ? dx0 : dx1;
    int x_cx_max = (ny >= 0) ? dx1 : dx0;
    int y_cx_min = (nx >= 0) ? dy1 : dy0;
    int y_cx_max = (nx >= 0) ? dy0 : dy1;
    int neg_nx = -nx;
    fixed_t min_cx = HW_MulsW_PairAdd(x_cx_min, ny, y_cx_min, neg_nx, cx_off) << 4;
    fixed_t max_cx = HW_MulsW_PairAdd(x_cx_max, ny, y_cx_max, neg_nx, cx_off) << 4;

    fixed_t cz_fmin = 0;
    if (min_cx > 0)
        cz_fmin = min_cx - (min_cx >> 3);
    else if (max_cx < 0)
        cz_fmin = (-max_cx) - ((-max_cx) >> 3);
    if (cz_fmin > max_cz)
        return -1;
    if (cz_fmin > min_cz)
        min_cz = cz_fmin;
#if HW_DBG_FLAT_LOD
    /* LOD1 (128-unit flat cells) thresholds, camera-depth units; overridable
     * for the 2026-11-16 E1M8 experiment (the slave transforms ~4000 LOD0
     * flat vertices/frame in the arena). */
#ifndef HW_LOD1_MIN_A
#define HW_LOD1_MIN_A 192    /* x0.75 of the 11-16 values (2026-11-17 LOD sweep) */
#define HW_LOD1_MAX_A 240
#define HW_LOD1_MIN_B 132
#define HW_LOD1_MAX_B 336
#endif
    int use_lod1 = ((true_min_cz >= (HW_LOD1_MIN_A << FRACBITS) && max_cz >= (HW_LOD1_MAX_A << FRACBITS)) ||
                    (true_min_cz >= (HW_LOD1_MIN_B << FRACBITS) && max_cz >= (HW_LOD1_MAX_B << FRACBITS)))
                   ? (int)HW_FLAT_USE_LOD1 : 0;
#else
    int use_lod1 = 0;
#endif

    int any_behind = 0;
    int s_min = 0, s_max = 0;
    int ytop_min = 0;
    int ybot_max = HW_VIEW_H;

    if (__builtin_expect(true_min_cz < HW_NEAR_Z, 0))
    {
        any_behind = 1;
        min_cz = HW_NEAR_Z;
        if (!fully_inside && !HW_CheckNearSubsectorFrustum(fb))
            return -1;
        s_max = HW_NEAR_Z_SCALE;
    }
    else if (fully_inside && ((hw_solid_count | ap->aperture_active) == 0))
    {
        /* Fast path for front subsectors already inside frustum before first solid/portal wall */
        s_max = HW_FastDiv(min_cz);
    }
    else
    {
        int sx, ex;
        fixed_t cz_left  = HW_MulsW_PairAdd(x_cx_min, nx, y_cx_min, ny, cz_off) << 4;
        fixed_t cz_right = (true_min_cz + max_cz) - cz_left;
        fixed_t cx_near  = HW_MulsW_PairAdd(x_cz_min, ny, y_cz_min, neg_nx, cx_off) << 4;

        s_max = HW_FastDiv(true_min_cz);
        int s_left  = HW_FastDiv(cz_left);
        int s_right = HW_FastDiv(cz_right);
        int px_near = HW_ProjX(cx_near, s_max, HW_VIEW_CX);
        HW_ProjXPair(min_cx, s_left, max_cx, s_right, HW_VIEW_CX, &sx, &ex);
        if (px_near < sx) sx = px_near;
        if (px_near > ex) ex = px_near;
        if (min_cx >= 0 || max_cx <= 0)
        {
            s_min = HW_FastDiv(max_cz);
            fixed_t cx_far = (min_cx + max_cx) - cx_near;
            int px_far = HW_ProjX(cx_far, s_min, HW_VIEW_CX);
            if (px_far < sx) sx = px_far;
            if (px_far > ex) ex = px_far;
        }
        if (cz_fmin > true_min_cz)
            s_max = HW_FastDiv(min_cz);
        if (!fully_inside && raw_min_cz >= (16 << 12) && sx >= 0 && ex <= HW_VIEW_W)
            fully_inside = 1;

        sx -= 1;
        ex += 1;
        if (sx < 0) sx = 0;
        if (ex > HW_VIEW_W) ex = HW_VIEW_W;
        int ex_m1 = ex - 1;
        if (sx > ex_m1)
            return -1;

        if (__builtin_expect((unsigned int)(sx ^ ex_m1) < 32u, 1))
        {
            int w0 = (int)((unsigned int)sx >> 5);
            uint32_t sm = hw_solid_mask[w0];
            if (sm != 0)
            {
                uint32_t mask = hw_mask_left[sx & 31] & hw_mask_right[ex_m1 & 31];
                if ((sm & mask) == mask)
                    return -1;
            }
            if (ap->aperture_active)
            {
                const hw_ap_word_t *aw = &hw_ap_words[w0];
                ytop_min = aw->yt_min;
                ybot_max = aw->yb_max;
                if (__builtin_expect((aw->yt_max - ytop_min) > 4 || (ybot_max - aw->yb_min) > 4, 0))
                {
                    int ap_pack = HW_EvalSubsectorSpanSlow(sx, ex);
                    if (ap_pack < 0)
                        return -1;
                    ytop_min = ap_pack >> 16;
                    ybot_max = (int16_t)ap_pack;
                }
                else if (ytop_min >= ybot_max)
                {
                    return -1;
                }
            }
        }
        else if ((hw_solid_count | ap->aperture_active) != 0)
        {
            int ap_pack = HW_EvalSubsectorSpanSlow(sx, ex);
            if (ap_pack < 0)
                return -1;
            ytop_min = ap_pack >> 16;
            ybot_max = (int16_t)ap_pack;
        }
    }

    int vh = HW_VIEW_H;
    ap->cur_sub_yt_attach = 0;
    ap->cur_sub_yb_attach = (int16_t)vh;
    ap->cur_sub_ytop      = 0;
    ap->cur_sub_ybot      = (uint8_t)vh;
    ap->cur_ytop_min      = (int16_t)ytop_min;
    ap->cur_ybot_max      = (int16_t)ybot_max;
    int check_ap = 0;
    if (ytop_min > 0 || ybot_max < vh)
    {
        int pad = (s_max < 8192) ? 1 : 2;
        check_ap = 1;
        ap->cur_sub_ytop = (ytop_min > pad) ? (uint8_t)(ytop_min - pad) : 0;
        ap->cur_sub_ybot = (ybot_max + pad < vh) ? (uint8_t)(ybot_max + pad) : (uint8_t)vh;
    }

    int near_or_psec = (any_behind || sf == hw_psec_fast);
    if (__builtin_expect((check_ap || near_or_psec) && !s_min, 0))
        s_min = HW_FastDiv(max_cz);

    uint32_t packed = (near_or_psec ? HW_FLAT_NEAR_OR_PSEC : 0u) |
                      (check_ap     ? HW_FLAT_CHECK_AP     : 0u) |
                      (uint32_t)use_lod1 |
                      ((uint32_t)fully_inside & HW_FLAT_INSIDE_MASK);
    int flat_flags = HW_EvalSubsectorFlatAperture(sf, packed, s_max, s_min);
    if (__builtin_expect(flat_flags >= 0 && check_ap && !near_or_psec &&
                         !(flat_flags & (int)HW_FLAT_ALLOW_FLOOR) &&
                         (sf->is_sky || !(flat_flags & (int)HW_FLAT_ALLOW_CEIL)), 0))
    {
        if (HW_CheckPortalSubsectorStepCull(subnum, sf, s_min))
            return -1;
    }
    return flat_flags;
}
#endif /* !HW_ASM_SUBAP || HW_ASM_CHECK */

