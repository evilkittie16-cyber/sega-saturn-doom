/* saturn_hw_walls.c — 2D Map-Space AABB Frustum & 1D Solid-Span Culling,
 * Subsector Pillar Pair Projection, Near-Plane Seg Clipper, 1D Vertical Strip
 * & Monolithic VDP1 Wall Quad Emitters, and Subsector Flat Aperture Evaluator.
 */

#define BBOX_OUTSIDE        0
#define BBOX_INTERSECT_NEAR 1
#define BBOX_INTERSECT      2
#define BBOX_FULLY_INSIDE   3

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
#ifdef __GNUC__
        __asm__("" : "+r"(f));
#endif
        if (HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Lx), f->Lx,
                             HW_BBOX_AT(bbox, f->nidx_Ly), f->Ly, f->Cl) >= 0 &&
            HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Rx), f->Rx,
                             HW_BBOX_AT(bbox, f->nidx_Ry), f->Ry, f->Cr) >= 0)
        {
            return BBOX_FULLY_INSIDE;
        }
    }

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
    if (!near_verified &&
        HW_MulsW_PairAdd(HW_BBOX_AT(bbox, f->nidx_Nx), f->Nx,
                         HW_BBOX_AT(bbox, f->nidx_Ny), f->Ny, f->Cn) < (HW_NEAR_Z_UNITS << 12))
        return 0;

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
    if (!fully_inside)
    {
        int st = HW_CheckBBoxStatus(bbox);
        if (st == BBOX_OUTSIDE)
            return -1;
        if (st == BBOX_INTERSECT_NEAR)
            return 0;
        if (st == BBOX_FULLY_INSIDE)
            fully_inside = 1;
        near_verified = 1;
    }
    if (hw_solid_count > 0 && saturn_hw_bsp_occl != 2 && HW_CheckBBoxOccluded(bbox, near_verified))
    {
        saturn_hw_occl_culls++;
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
    int next_px1 = (c_uhi >= u_end) ? ctx->p1->px : ((cur_px_fp + step_px_fp * adv + 0x8000) >> 16);
    int max_coal_dx = (coal_strips == 4 && srca_64 == srca) ? 52 : 1000;
    if (next_px1 - s_px0 > max_coal_dx)
        return 0;

    if (coal_strips == 4 && (next_px1 - s_px0 <= 28 || srca_64 == srca) && srca_mono != srca)
    {
        emit_srca = srca_mono + (cur_v << 2) + (cur_strip_idx >> 2) * (th << 2);
        emit_size_hi = 4u << 8;
    }
    hw_wall_coord_tmp[0] = (int)(((uint32_t)emit_srca << 16) | emit_size_hi | (cmd_w2_base & 0xFFu));
    hw_wall_coord_tmp[1] = (int)(cmd_w2_cur + ((uint32_t)strip_srca_step << 16) * (uint32_t)adv);
    return coal_strips;
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Linear(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const int u_start = ctx->u_start;
    const int tw = ctx->tw;
    const int th = ctx->th;
    const int srca = ctx->srca;

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
    uint32_t cmd_w2_end  = cmd_w2_base + (uint32_t)num_strips * cmd_w2_step;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;

    int s_px0   = ctx->p0->px;
    int s_ya0   = ctx->ya0;
    int s_ya1   = ctx->ya1;
    int end_px  = ctx->p1->px;
    int end_ya0 = ctx->yb0;
    int end_ya1 = ctx->yb1;

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
            cur_px_fp  = (s_px0 << 16) + step_px_fp;
            cur_ya0_fp = (s_ya0 << 16) + step_ya0_fp;
            cur_ya1_fp = (s_ya1 << 16) + step_ya1_fp;
        }
        else
        {
            int first_du = 16 - (u_start & 15);
            int first_t1 = (int)(((uint32_t)first_du * inv_u_span) >> 8);
            cur_px_fp  = (s_px0 << 16) + d_px  * first_t1;
            cur_ya0_fp = (s_ya0 << 16) + d_ya0 * first_t1;
            cur_ya1_fp = (s_ya1 << 16) + d_ya1 * first_t1;
        }
    }

    int u_len = ctx->u_span;
    int can_coal = (tw == 32 && ctx->srca_mono != srca && u_len >= 32) |
                   (tw >= 64 && (ctx->srca_64 != srca || ctx->srca_mono != srca) && u_len >= 64);

    int wall_needs_clamp = (s_px0 < -500) | (end_px > 500) |
        (((((unsigned int)(s_ya1 - s_ya0) | (unsigned int)(end_ya1 - end_ya0)) > 1022u) |
          HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1)));
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;
    int band_all_vis = (s_px0 >= 0) & (end_px <= HW_VIEW_W) &
                       (s_ya0 < s_ya1) & (end_ya0 < end_ya1) &
                       (s_ya1 > ytop_min) & (end_ya1 > ytop_min) &
                       (s_ya0 < ybot_max) & (end_ya0 < ybot_max);
    int u_end = u_start + u_len;
    int partial_first = (u_start & 15) != 0;
    int partial_last  = (u_end & 15) != 0;
    int has_occl      = (ctx->occl_state != 0);
    int wall_slow     = (!band_all_vis) | has_occl | wall_needs_clamp;
    int first_slow    = wall_slow | partial_first | (ctx->last_clip_x0 != -1);

    /* Pack loop flags into a single 32-bit word:
     * bit 0: can_coal, bit 1: cur_slow, bit 2: band_all_vis, bit 3: wall_slow,
     * bit 4: is_partial_first, bit 5: partial_last, bit 6: has_occl, bit 7: wall_needs_clamp,
     * bits 8..15: l_strips, bits 16..31: l_quads */
    uint32_t lin_flags = (uint32_t)can_coal |
                         ((uint32_t)first_slow << 1) |
                         ((uint32_t)band_all_vis << 2) |
                         ((uint32_t)wall_slow << 3) |
                         ((uint32_t)partial_first << 4) |
                         ((uint32_t)partial_last << 5) |
                         ((uint32_t)has_occl << 6) |
                         ((uint32_t)wall_needs_clamp << 7);

    for (int s = s_min; s <= s_max; s++)
    {
        int coal_strips = 0;
        uint32_t emit_cmd2 = cmd_w2_cur;
        if (__builtin_expect((lin_flags & 1u) != 0, 0))
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
            s_px1 = (cur_px_fp  + 0x8000) >> 16;
            s_yb0 = (cur_ya0_fp + 0x8000) >> 16;
            s_yb1 = (cur_ya1_fp + 0x8000) >> 16;
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
        while (cmd_w2_cur >= cmd_w2_end)
            cmd_w2_cur -= (uint32_t)num_strips * cmd_w2_step;
    }

    unsigned int l_quads  = lin_flags >> 16;
    unsigned int l_strips = (lin_flags >> 8) & 0xFFu;
    saturn_v1w_cells      += (int)l_quads;
    saturn_hw_wall_quads  += l_quads;
    saturn_hw_wall_strips += l_strips;
}

static HW_NOINLINE HW_HOT_O2 void HW_EmitWallStrips_Persp(
    hw_wall_emit_ctx_t *ctx, int s_min, int s_max)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    const int u_start = ctx->u_start;
    const int tw = ctx->tw;
    const int th = ctx->th;

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
    uint32_t cmd_w2_end  = cmd_w2_base + (uint32_t)num_strips * cmd_w2_step;
    uint32_t cmd_w2_cur  = cmd_w2_base + (uint32_t)cur_strip_idx * cmd_w2_step;

    int s_px0   = p0->px;
    int s_ya0   = ctx->ya0;
    int s_ya1   = ctx->ya1;
    fixed_t s_cz0 = p0->cz;
    int end_px  = p1->px;
    int end_ya0 = ctx->yb0;
    int end_ya1 = ctx->yb1;

    int light_s = ctx->seg->baked_light >> 4;
    int const_light = 1;
    if (light_s < 15)
    {
        const uint16_t *zlight_row = wall_zlight_cmdcolr_LUT[light_s];
        unsigned int zb0 = (unsigned int)p0->cz >> 20; if (zb0 > 127u) zb0 = 127u;
        unsigned int zb1 = (unsigned int)p1->cz >> 20; if (zb1 > 127u) zb1 = 127u;
        const_light = (zlight_row[zb0] == zlight_row[zb1]);
    }

    fixed_t cur_cx1 = 0, cur_cz1 = 0, step_cx1 = 0, step_cz1 = 0;
    if (s_min < s_max)
    {
        uint32_t inv_u_span = ctx->inv_u_span;
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

    int wall_needs_clamp = (s_px0 < -500) | (end_px > 500) |
        (((((unsigned int)(s_ya1 - s_ya0) | (unsigned int)(end_ya1 - end_ya0)) > 1022u) |
          HW_ANY_OUT1024(s_ya0, end_ya0, s_ya1, end_ya1)));
    int ytop_min = ctx->ytop_min;
    int ybot_max = ctx->ybot_max;
    int band_all_vis = (s_px0 >= 0) & (end_px <= HW_VIEW_W) &
                       (s_ya0 < s_ya1) & (end_ya0 < end_ya1) &
                       (s_ya1 > ytop_min) & (end_ya1 > ytop_min) &
                       (s_ya0 < ybot_max) & (end_ya0 < ybot_max);
    int u_end = u_start + ctx->u_span;
    int partial_first = (u_start & 15) != 0;
    int partial_last  = (u_end & 15) != 0;
    int has_occl      = (ctx->occl_state != 0);
    int wall_slow     = (!const_light) | (!band_all_vis) | has_occl | wall_needs_clamp;
    int first_slow    = wall_slow | partial_first | (ctx->last_clip_x0 != -1);
    int can_coal      = (tw >= 64) & ((tw & (tw - 1)) == 0) & ((ctx->srca_64 != ctx->srca) | (ctx->srca_mono != ctx->srca)) & (ctx->u_span >= 64);

    /* Pack loop flags into a single 32-bit word:
     * bit 0: const_light, bit 1: cur_slow, bit 2: band_all_vis, bit 3: wall_slow,
     * bit 4: is_partial_first, bit 5: partial_last, bit 6: has_occl, bit 7: wall_needs_clamp,
     * bit 8: can_coal, bits 16..31: l_quads */
    uint32_t persp_flags = (uint32_t)const_light |
                           ((uint32_t)first_slow << 1) |
                           ((uint32_t)band_all_vis << 2) |
                           ((uint32_t)wall_slow << 3) |
                           ((uint32_t)partial_first << 4) |
                           ((uint32_t)partial_last << 5) |
                           ((uint32_t)has_occl << 6) |
                           ((uint32_t)wall_needs_clamp << 7) |
                           ((uint32_t)can_coal << 8);

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
            if (s_cz1 < HW_NEAR_Z) s_cz1 = HW_NEAR_Z;
            int s_scale1 = HW_FastDiv(s_cz1);
            s_px1 = HW_ProjX(s_cx1, s_scale1, HW_VIEW_CX);
            HW_ProjYPair(HW_VIEW_CY, ctx->dz_top, s_scale1, ctx->dz_bot, s_scale1, &s_yb0, &s_yb1);
            s_yb0 -= ctx->top_overlap;
            s_yb1 += ctx->bot_overlap;
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
                                 ((uint32_t)wall_zlight_cmdcolr_LUT[ctx->seg->baked_light >> 4][s_zb] & 0xFF00u);
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
        while (cmd_w2_cur >= cmd_w2_end)
            cmd_w2_cur -= (uint32_t)num_strips * cmd_w2_step;
    }

    unsigned int l_quads = persp_flags >> 16;
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

    if (__builtin_expect(!ctx->persp_strips, 1))
        HW_EmitWallStrips_Linear(ctx, s_min, s_max);
    else
        HW_EmitWallStrips_Persp(ctx, s_min, s_max);
}

HW_ALWAYS_INLINE int HW_NeedPerspectiveStrips(int dx, int u_span, fixed_t cz0, fixed_t cz1, int tw)
{
    if (!saturn_wall_subdivision) return 0;
    if (tw <= 16) return 0;
    /* Distant walls: both ends > 512 units have negligible affine distortion */
    if (cz0 > (512 << FRACBITS) && cz1 > (512 << FRACBITS)) return 0;
    /* Narrow on screen: < 16 pixels has negligible affine sag */
    if (dx < 16) return 0;
    /* Perpendicular walls: < 12.5% depth change across the wall has minimal affine warping */
    fixed_t min_z = (cz0 < cz1) ? cz0 : cz1;
    fixed_t max_z = (cz0 > cz1) ? cz0 : cz1;
    fixed_t dz = max_z - min_z;
    if (dz < (min_z >> 3)) return 0;
    if (min_z > (288 << FRACBITS) && (dx < 32 || dz < (min_z >> 2))) return 0;
    /* Don't subdivide if average strip is < 4 screen pixels wide */
    if ((dx << 4) < (u_span * 4)) return 0;
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
 * For |dx|, |dy| <= 1400, cz >> 16 is mathematically guaranteed <= 1401*sqrt(2) + 1 = 1982 < 2048,
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
    ProjectPillar_2D_16(pdx, pdy, hw_bbox_Nx, hw_bbox_Ny, hw_frust.cz_origin, hw_frust.cx_origin, &cz, &cx);
    int s = 0, px = 0;
    if (cz >= HW_NEAR_Z)
    {
        s = (int)SATURN_OCLUT[(unsigned int)cz >> FRACBITS];
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

/* Takes 2 register arguments (r4=ctx, r5=bsec_idx), 0 stack arguments! */
static HW_NOINLINE HW_HOT_O2 void HW_RecordSegPortalAperture(
    hw_wall_emit_ctx_t *ctx, int bsec_idx)
{
    ctx->portal_recorded = 1;
    if (ctx->seg_sx >= ctx->seg_ex || (unsigned)bsec_idx >= (unsigned)numsectors)
        return;

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
    const sector_t *bsec = &sectors[bsec_idx];
    hw_aperture_state_t *ap = HW_APERTURE;
    const fixed_t vz = hw_viewz;
    const int vcy = HW_VIEW_CY;
    const int vh  = HW_VIEW_H;
    int p_yt = 0;
    int p_yb = vh;
    int yt_att = ap->cur_sub_yt_attach;
    int yb_att = ap->cur_sub_yb_attach;

    if (!ctx->is_sky_ceil || ((int16_t)bsec->ceilingpic != -1 && ctx->sec_ceiling > bsec->ceilingheight))
    {
        fixed_t open_top = (ctx->sec_ceiling < bsec->ceilingheight) ? ctx->sec_ceiling : bsec->ceilingheight;
        if (ctx->sec_ceiling > bsec->ceilingheight || open_top > vz)
        {
            int dzt = HW_DZQ(open_top, vz);
            int yt_min = HW_ProjY(dzt, (dzt >= 0) ? s_max : s_min, vcy);

            if ((int16_t)bsec->ceilingpic != -1 && (unsigned)bsec_idx < HW_MAX_AP_SECTORS)
            {
                int b_yt_att = (yt_min < 0) ? 0 : yt_min;
                if (b_yt_att < ap->sec_yt_attach[bsec_idx])
                    ap->sec_yt_attach[bsec_idx] = (int16_t)b_yt_att;
            }
            int yt_pad = (s_max < 8192) ? 0 : 1;
            if (yt_min - yt_pad > ctx->ytop_min)
            {
                p_yt = yt_min - yt_pad;
                if (ctx->sec_ceiling > bsec->ceilingheight)
                {
                    int dzt_hi = HW_DZQ(ctx->sec_ceiling, vz);
                    int w_yt_att = HW_ProjY(dzt_hi, (dzt_hi >= 0) ? s_max : s_min, vcy);
                    if (w_yt_att < yt_att) yt_att = w_yt_att;
                }
            }
        }
    }

    fixed_t open_bot = (ctx->sec_floor > bsec->floorheight) ? ctx->sec_floor : bsec->floorheight;
    if (ctx->sec_floor < bsec->floorheight || open_bot < vz)
    {
        int dzb = HW_DZQ(open_bot, vz);
        int yb_max = HW_ProjY(dzb, (dzb <= 0) ? s_max : s_min, vcy);

        if ((unsigned)bsec_idx < HW_MAX_AP_SECTORS)
        {
            int b_yb_att = (yb_max > vh) ? vh : yb_max;
            if (b_yb_att > ap->sec_yb_attach[bsec_idx])
                ap->sec_yb_attach[bsec_idx] = (int16_t)b_yb_att;
        }
        int yb_pad = (s_max < 8192) ? 0 : 1;
        if (yb_max + yb_pad < ctx->ybot_max)
        {
            p_yb = yb_max + yb_pad;
            if (ctx->sec_floor < bsec->floorheight)
            {
                int dzb_lo = HW_DZQ(ctx->sec_floor, vz);
                int w_yb_att = HW_ProjY(dzb_lo, (dzb_lo <= 0) ? s_max : s_min, vcy);
                if (w_yb_att > yb_att) yb_att = w_yb_att;
            }
        }
    }

    if (p_yt > 0 || p_yb < vh)
        HW_RecordPortalSpan(((int)ctx->seg_sx << 16) | (uint16_t)ctx->seg_ex, p_yt, p_yb, (yt_att << 16) | (uint16_t)yb_att);
}

static unsigned int hw_prof_segs_acc = 0;

#if defined(__GNUC__) && !defined(__clang__)
#define HW_HOT_O2_NOTHREAD __attribute__((optimize("O2,no-thread-jumps")))
#else
#define HW_HOT_O2_NOTHREAD HW_HOT_O2
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
    unsigned int zb = (__builtin_expect(s_ptr->baked_light < 240, 1))
        ? (((uint32_t)p0->cz + (uint32_t)p1->cz) >> 21)
        : 0u;
    if (__builtin_expect(zb > 127u, 0)) zb = 127u;
    int k_shade = (int)(wall_zlight_cmdcolr_LUT[s_ptr->baked_light >> 4][zb] & 0xFF00u);

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

static HW_NOINLINE HW_HOT_O2 void HW_EmitMonolithicBakedSeg(
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
    unsigned int zb = (__builtin_expect(s_ptr->baked_light < 240, 1))
        ? (((uint32_t)ctx->p0->cz + (uint32_t)ctx->p1->cz) >> 21)
        : 0u;
    if (__builtin_expect(zb > 127u, 0)) zb = 127u;
    int k_shade = (int)(wall_zlight_cmdcolr_LUT[s_ptr->baked_light >> 4][zb] & 0xFF00u);

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

static HW_NOINLINE HW_HOT_O2_NOTHREAD void HW_EmitBakedWallQuads(hw_wall_emit_ctx_t *ctx)
{
    const hw_trans_pillar_t *p0 = ctx->p0;
    const hw_trans_pillar_t *p1 = ctx->p1;
    int dx = p1->px - p0->px;

    if (__builtin_expect(ctx->was_near_clipped, 0))
    {
        HW_EmitSubdividedBakedSeg(ctx, (dx >= 24 && saturn_wall_subdivision));
        return;
    }

    const baked_seg_t *s_ptr = ctx->seg;
    int tw      = ctx->tw;
    int u_start = s_ptr->u_start;
    int u_span  = (s_ptr->u_span > 0) ? (int)s_ptr->u_span : 64;

    if (__builtin_expect((tw & (tw - 1)) == 0 && tw > 0, 1))
        u_start &= (tw - 1);
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
            HW_EmitSubdividedBakedSeg(ctx, 1);
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
            if ((dx <= 28 || (srca_64 == srca && dx <= 56)) && srca_mono != srca)
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

    if (!mono_tw)
        HW_EmitSubdividedBakedSeg(ctx, 0);
    else
        HW_EmitMonolithicBakedSeg(ctx, mono_tw, mono_base_srca, needs_clip);
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

    const sector_t *bsec = &sectors[bsec_idx];
    int occlude_only = 0;
    if (is_solid)
    {
        occlude_only = 1;
    }
    else if (!ctx->portal_recorded &&
             ((!ctx->is_sky_ceil && (ctx->sec_ceiling != bsec->ceilingheight || ctx->sec_ceiling > hw_viewz)) ||
              (ctx->sec_floor != bsec->floorheight || ctx->sec_floor < hw_viewz)))
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

    if (seg_flags & (SEG_FLAG_LOWER | SEG_FLAG_UPPER))
    {
        bsec_idx = seg->back_sec_idx;
        if (__builtin_expect((unsigned)bsec_idx >= (unsigned)numsectors, 0))
            return;
        const sector_t *bsec = &sectors[bsec_idx];
        fixed_t bceil  = bsec->ceilingheight;
        fixed_t bfloor = bsec->floorheight;
        fixed_t mbot   = (sec_floor > bfloor) ? sec_floor : bfloor;
        fixed_t mtop   = (sec_ceiling < bceil) ? sec_ceiling : bceil;
        is_solid = (!ctx->is_sky_ceil && (int16_t)bsec->ceilingpic != -1 && mtop <= mbot);

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
            if ((unsigned)bsec_idx < (unsigned)numsectors)
            {
                const sector_t *bsec = &sectors[bsec_idx];
                if (bsec->ceilingheight < top_world_z)
                    top_world_z = bsec->ceilingheight;
                if (bsec->floorheight > bot_world_z)
                    bot_world_z = bsec->floorheight;
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
    const fixed_t vz      = hw_viewz;
    ctx->dz_top           = HW_DZQ(top_world_z, vz);
    ctx->dz_bot           = HW_DZQ(bot_world_z, vz);
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

    const int s_A = ctx->p0->s;
    const int s_B = ctx->p1->s;
    const int vcy = HW_VIEW_CY;
    int dz_top = ctx->dz_top;
    int dz_bot = ctx->dz_bot;
    int ya0, ya1, yb0, yb1;
    HW_ProjYPair(vcy, dz_top, s_A, dz_bot, s_A, &ya0, &ya1);
    HW_ProjYPair(vcy, dz_top, s_B, dz_bot, s_B, &yb0, &yb1);

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

    ctx->seg         = seg;
    ctx->ya0         = (int16_t)ya0;
    ctx->ya1         = (int16_t)ya1;
    ctx->yb0         = (int16_t)yb0;
    ctx->yb1         = (int16_t)yb1;
    ctx->top_overlap = (uint8_t)top_overlap;
    ctx->bot_overlap = (uint8_t)bot_overlap;

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

    HW_EmitBakedWallQuads(ctx);
}

static HW_NOINLINE HW_HOT_O2 void HW_ProjectPortalEndpoint(hw_trans_pillar_t *tp, int dx, int dy)
{
    fixed_t cz, cx;
    ProjectMapVertex_2D(dx, dy, hw_bbox_Nx, hw_bbox_Ny, hw_frust.cz_origin, hw_frust.cx_origin, &cz, &cx);
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
    const hw_aperture_state_t *ap = HW_APERTURE;
    int count = (int)ctx->portal_count;
    const hw_portal_seg_t *p = &HW_PORTAL_SEGS[ctx->portal_first];
    const int cam_x = hw_cam_x;
    const int cam_y = hw_cam_y;
    int prev_projected = 0;
    int i;

    for (i = 0; i < count; i++, p++)
    {
        int seg_dx = (int)p->dx;
        int seg_dy = (int)p->dy;
        int obj_dx0 = (int)p->x0 - cam_x;
        int obj_dy0 = (int)p->y0 - cam_y;
        if (HW_CrossBackface(obj_dy0, seg_dx, obj_dx0, seg_dy) <= 0)
        {
            prev_projected = 0;
            continue;
        }

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
        const saturn_wl_pillar_t *p1_raw = &hw_cur_pillars[idx1];
        HW_TransformSubsectorPillar(idx1, (int)p1_raw->x - hw_cam_x, (int)p1_raw->z - hw_cam_y);
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
                last_pair_state = -1;
                continue;
            }

            last_pair_state = HW_ProjectAndCullPillarPair(pair_key, obj_dx, obj_dy);
            if (last_pair_state < 0)
                continue;
        }

        HW_EmitVisibleBakedSeg(seg);
    }
}

/* Master Blueprint: Zero-Runtime-Math 1D LOD Subsector Wall Renderer (Phase 3)
 * Fast Culling & Pillar Projection Loop (< 400 bytes, zero register spills):
 * rejects ~72% of segs in registers and dispatches surviving visible segs to HW_EmitVisibleBakedSeg. */
static HW_NOINLINE HW_HOT_O2 int HW_RenderSubsectorBakedWalls(int subnum, sector_t *sec)
{
    if (!saturn_baked_walls_active || (unsigned)subnum >= (unsigned)saturn_baked_wall_num_subsectors)
        return 0;

    unsigned short tw0 = SAT_FRT();
    const saturn_wl_subsector_t *ss = &saturn_baked_wall_subsectors[subnum];
    int num_segs = (int)ss->num_segs;
    hw_wctx.portal_first = ss->portal_first;
    hw_wctx.portal_count = ss->portal_count;
    hw_wctx.sec_ceiling  = sec->ceilingheight;
    hw_wctx.sec_floor    = sec->floorheight;
    hw_wctx.is_sky_ceil  = (uint8_t)((int16_t)sec->ceilingpic == -1);
    if (num_segs <= 0)
    {
        if (hw_wctx.portal_count > 0 && hw_solid_count < HW_VIEW_W)
            HW_RecordSubsectorUnbakedPortals(&hw_wctx);
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

    HW_WalkSubsectorBakedSegs(seg, seg_end, pillars);

    if (hw_wctx.portal_count > 0 && hw_solid_count < HW_VIEW_W)
    {
        HW_RecordSubsectorUnbakedPortals(&hw_wctx);
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

static HW_NOINLINE HW_HOT_O2 int HW_EvalSubsectorFlatAperture(
    const sector_t *sec, uint32_t secnum_and_flags, int s_max, int s_min)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    uint32_t secnum  = secnum_and_flags >> 16;
    int valid_sec    = (secnum < HW_MAX_AP_SECTORS);
    const fixed_t vz = hw_viewz;
    const int vcy    = HW_VIEW_CY;
    const int vh     = HW_VIEW_H;

    if (__builtin_expect(!(secnum_and_flags & HW_FLAT_AP_OR_NEAR), 1))
    {
        if (sec->floorheight < vz)
        {
            int yf_max = HW_ProjY(HW_DZQ(sec->floorheight, vz), s_max, vcy);
            int yb_att = (yf_max > vh) ? vh : yf_max;
            if (valid_sec)
            {
                if (yb_att > ap->sec_yb_attach[secnum])
                    ap->sec_yb_attach[secnum] = (int16_t)yb_att;
                yb_att = ap->sec_yb_attach[secnum];
            }
            ap->cur_sub_yb_attach = (int16_t)yb_att;
        }
        else
        {
            ap->cur_sub_yb_attach = valid_sec ? ap->sec_yb_attach[secnum] : 0;
        }

        if ((int16_t)sec->ceilingpic != -1 && sec->ceilingheight > vz)
        {
            int yc_min = HW_ProjY(HW_DZQ(sec->ceilingheight, vz), s_max, vcy);
            int yt_att = (yc_min < 0) ? 0 : yc_min;
            if (valid_sec)
            {
                if (yt_att < ap->sec_yt_attach[secnum])
                    ap->sec_yt_attach[secnum] = (int16_t)yt_att;
                yt_att = ap->sec_yt_attach[secnum];
            }
            ap->cur_sub_yt_attach = (int16_t)yt_att;
        }
        else
        {
            ap->cur_sub_yt_attach = ((int16_t)sec->ceilingpic == -1) ? 0 : (valid_sec ? ap->sec_yt_attach[secnum] : (int16_t)vh);
        }
        return (int)((secnum_and_flags & (HW_FLAT_USE_LOD1 | HW_FLAT_INSIDE_MASK)) | HW_FLAT_ALLOW_BOTH);
    }

    int check_ap     = (int)((secnum_and_flags & HW_FLAT_CHECK_AP) != 0);
    int near_or_psec = (int)((secnum_and_flags & HW_FLAT_NEAR_OR_PSEC) != 0);
    int ytop_min     = ap->cur_ytop_min;
    int ybot_max     = ap->cur_ybot_max;
    int pad          = (s_max < 8192) ? 1 : 2;

    int allow_floor = 1;
    int allow_ceil  = 1;
    int yf_max = vh;
    int yc_min = 0;

    if (sec->floorheight < vz)
    {
        int dzf = HW_DZQ(sec->floorheight, vz);
        if (!near_or_psec)
        {
            yf_max = HW_ProjY(dzf, s_max, vcy);
            int yb_att = (yf_max > vh) ? vh : yf_max;
            if (valid_sec)
            {
                if (yb_att > ap->sec_yb_attach[secnum])
                    ap->sec_yb_attach[secnum] = (int16_t)yb_att;
                yb_att = ap->sec_yb_attach[secnum];
            }
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
            if (valid_sec)
                ap->sec_yb_attach[secnum] = (int16_t)vh;
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
        ap->cur_sub_yb_attach = valid_sec ? ap->sec_yb_attach[secnum] : 0;
    }

    if ((int16_t)sec->ceilingpic != -1)
    {
        if (sec->ceilingheight > vz)
        {
            int dzc = HW_DZQ(sec->ceilingheight, vz);
            if (!near_or_psec)
            {
                yc_min = HW_ProjY(dzc, s_max, vcy);
                int yt_att = (yc_min < 0) ? 0 : yc_min;
                if (valid_sec)
                {
                    if (yt_att < ap->sec_yt_attach[secnum])
                        ap->sec_yt_attach[secnum] = (int16_t)yt_att;
                    yt_att = ap->sec_yt_attach[secnum];
                }
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
                if (valid_sec)
                    ap->sec_yt_attach[secnum] = 0;
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
            ap->cur_sub_yt_attach = valid_sec ? ap->sec_yt_attach[secnum] : (int16_t)vh;
        }
    }
    else if (!near_or_psec)
    {
        ap->cur_sub_yt_attach = 0;
    }

    if (check_ap)
    {
        int sub_y_max = (sec->floorheight < vz)
            ? yf_max
            : HW_ProjY(HW_DZQ(sec->floorheight, vz), (sec->floorheight <= vz) ? s_max : s_min, vcy);
        if (sub_y_max + pad <= ytop_min)
            return -1;
        if ((int16_t)sec->ceilingpic != -1)
        {
            int sub_y_min = (sec->ceilingheight > vz)
                ? yc_min
                : HW_ProjY(HW_DZQ(sec->ceilingheight, vz), (sec->ceilingheight >= vz) ? s_max : s_min, vcy);
            if (sub_y_min - pad >= ybot_max)
                return -1;
        }
    }

    return (int)((secnum_and_flags & (HW_FLAT_USE_LOD1 | HW_FLAT_INSIDE_MASK)) | ((uint32_t)(allow_floor | (allow_ceil << 1)) << 2));
}

/* Walk a subsector: returns flat_flags = (fully_inside & 3) | ((allow_floor | (allow_ceil << 1) | use_lod1) << 2), or -1 if culled */
static HW_NOINLINE HW_HOT_O2 int HW_CheckSubsectorAperture(int subnum, sector_t *sec, int secnum_and_inside)
{
    hw_aperture_state_t *ap = HW_APERTURE;
    int fully_inside  = secnum_and_inside & (int)HW_FLAT_INSIDE_MASK;
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
    int use_lod1 = (min_cz >= (160 << FRACBITS)) ? (int)HW_FLAT_USE_LOD1 : 0;
#else
    int use_lod1 = 0;
#endif

    int sx = 0, ex = HW_VIEW_W;
    int any_behind = 0;
    int s_min = 0, s_max = 0;

    int vh = HW_VIEW_H;
    int ytop_min = 0;
    int ybot_max = vh;
    if (__builtin_expect(true_min_cz < HW_NEAR_Z, 0))
    {
        any_behind = 1;
        min_cz = HW_NEAR_Z;
        if (!fully_inside && !HW_CheckNearSubsectorFrustum(fb))
            return -1;
        s_max = HW_NEAR_Z_SCALE;
    }
    else
    {
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
        int ap_sx = sx;
        int ap_ex = ex;
        if (ex - sx > 4)
        {
            ap_sx = sx + 2;
            ap_ex = ex - 2;
        }
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
                    int ap_pack = HW_GetSpanAperture(ap_sx, ap_ex);
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
        else if (ap->aperture_active)
        {
            if (hw_solid_count > 0 && HW_CheckSpanOcclusion(sx, ex) == 2)
                return -1;
            int ap_pack = HW_GetSpanAperture(ap_sx, ap_ex);
            if (ap_pack < 0)
                return -1;
            ytop_min = ap_pack >> 16;
            ybot_max = (int16_t)ap_pack;
        }
        else if (hw_solid_count > 0 && HW_CheckSpanOcclusion(sx, ex) == 2)
        {
            return -1;
        }
    }

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

    int near_or_psec = (any_behind || sec == hw_psec);
    if (__builtin_expect((check_ap || near_or_psec) && !s_min, 0))
        s_min = HW_FastDiv(max_cz);

    uint32_t packed = ((uint32_t)(secnum_and_inside & ~0xFF) << 8) |
                      (near_or_psec ? HW_FLAT_NEAR_OR_PSEC : 0u) |
                      (check_ap     ? HW_FLAT_CHECK_AP     : 0u) |
                      (uint32_t)use_lod1 |
                      ((uint32_t)fully_inside & HW_FLAT_INSIDE_MASK);
    int flat_flags = HW_EvalSubsectorFlatAperture(sec, packed, s_max, s_min);
    if (flat_flags >= 0 && check_ap && !near_or_psec &&
        !(flat_flags & (int)HW_FLAT_ALLOW_FLOOR) &&
        ((int16_t)sec->ceilingpic == -1 || !(flat_flags & (int)HW_FLAT_ALLOW_CEIL)) &&
        saturn_baked_wall_subsectors)
    {
        const saturn_wl_subsector_t *wss = &saturn_baked_wall_subsectors[subnum];
        if (wss->max_bsec0 >= 0)
        {
            fixed_t z_floor = sectors[wss->max_bsec0].floorheight;
            int floor_ok = (z_floor <= sec->floorheight) ||
                           (z_floor <= hw_viewz && HW_ProjY(HW_DZQ(z_floor, hw_viewz), s_min, HW_VIEW_CY) >= ybot_max);
            if (floor_ok)
            {
                int ceil_ok = 1;
                if (wss->max_bsec1 >= 0)
                {
                    fixed_t z_ceil = sectors[wss->max_bsec1].ceilingheight;
                    ceil_ok = (z_ceil >= sec->ceilingheight) ||
                              (z_ceil >= hw_viewz && HW_ProjY(HW_DZQ(z_ceil, hw_viewz), s_min, HW_VIEW_CY) <= ytop_min);
                }
                if (ceil_ok && !HW_SubsectorHasThings(sec, (uint32_t)(secnum_and_inside >> 8), subnum))
                {
                    if ((int16_t)sec->ceilingpic == -1)
                        saturn_hw_sky_visible = 1;
                    return -1;
                }
            }
        }
    }
    return flat_flags;
}

