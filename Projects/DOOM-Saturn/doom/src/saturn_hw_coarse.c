/* saturn_hw_coarse.c - coarse BSP nodes (HLOD prototype, 2026-11-26).
 *
 * User: "E4M2/5/6/8/9 and other heavy scenes: reduce the load without
 * massively affecting image quality", "dynamic, keeping the load within
 * 30 FPS no matter what". The master's frame cost is per VISITED subsector
 * (aperture test, span occlusion of every wall, occlusion commit), not per
 * quad. A BSP node whose bbox is small on screen (size * focal < T * dist)
 * is drawn "coarse": its subsectors are still walked in BSP order (so the
 * painter's order stays exact, one visit each), but without the aperture
 * test, without span occlusion / aperture clipping of their walls, without
 * recording occlusion, and with the 128-unit (LOD1) floors. Nearer real
 * geometry still culls coarse subtrees (node bbox test), and coarse quads
 * are delivered before nearer visits, so those paint over them.
 *
 * Included from saturn_hw_render.c after saturn_hw_walls.c (unity build).
 * HW_COARSE=0 compiles it out. hw_coarse_t (px, 0 = off) is the threshold
 * the adaptive governor drives.
 */
#ifndef HW_COARSE
#define HW_COARSE 0     /* prototype measured SLOWER (cw_t48/cw_t96, 2026-11-26): off */
#endif

#if HW_COARSE
#ifndef HW_COARSE_T0
#define HW_COARSE_T0 0
#endif
HW_W2_SHARED(volatile int32_t hw_coarse_t, = HW_COARSE_T0);   /* 0 off; node size threshold in px */
unsigned int saturn_hw_coarse_nodes, saturn_hw_coarse_subs;   /* per frame (HUD / bench) */

#ifndef HW_COARSE_MIN_DIST
#define HW_COARSE_MIN_DIST 192     /* map units: never coarse closer than this */
#endif

/* Node test at node entry: the union of both children's bboxes. */
static HW_NOINLINE int HW_CoarseNodeWanted(int bspnum)
{
    int t = hw_coarse_t;
    if (t <= 0 || !hw_node_bboxes)
        return 0;
    const int16_t *a = hw_node_bboxes[bspnum][0];
    const int16_t *b = hw_node_bboxes[bspnum][1];
    int top = a[BOXTOP] > b[BOXTOP] ? a[BOXTOP] : b[BOXTOP];
    int bot = a[BOXBOTTOM] < b[BOXBOTTOM] ? a[BOXBOTTOM] : b[BOXBOTTOM];
    int lft = a[BOXLEFT] < b[BOXLEFT] ? a[BOXLEFT] : b[BOXLEFT];
    int rgt = a[BOXRIGHT] > b[BOXRIGHT] ? a[BOXRIGHT] : b[BOXRIGHT];
    int cx = hw_cam_x, cy = hw_cam_y;
    int dx = (cx < lft) ? lft - cx : (cx > rgt) ? cx - rgt : 0;
    int dy = (cy < bot) ? bot - cy : (cy > top) ? cy - top : 0;
    int dist = dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);    /* ~euclidean */
    if (dist < HW_COARSE_MIN_DIST)
        return 0;
    int size = (rgt - lft) > (top - bot) ? (rgt - lft) : (top - bot);
    return size * 160 < t * dist;
}

/* Walls of a coarse subsector: backface + frustum only, no occlusion. */
static HW_NOINLINE void HW_CoarseLeafWalls(int subnum, const hw_sec_fast_t *sf)
{
    if (!saturn_baked_walls_active || (unsigned)subnum >= (unsigned)saturn_baked_wall_num_subsectors)
        return;
    const saturn_wl_subsector_t *ss = &saturn_baked_wall_subsectors[subnum];
    int num_segs = (int)ss->num_segs, num_pil = (int)ss->num_pillars;
    uint8_t pil_gen = (uint8_t)(hw_cur_pil_gen + 1u);
    if (pil_gen == 0)
    {
        for (int i = 0; i < (int)sizeof(hw_pil_gen); i++)
            hw_pil_gen[i] = 0;
        pil_gen = 1u;
    }
    hw_cur_pil_gen = pil_gen;
    if (num_segs <= 0 || num_pil <= 0 || num_pil > HW_MAX_SUBSECTOR_PILLARS)
        return;
    hw_wall_emit_ctx_t *ctx = &hw_wctx;
    const saturn_wl_pillar_t *pillars = &saturn_baked_wall_pillars[ss->pillar_start];
    hw_cur_pillars = pillars;
    hw_cur_pfrac = saturn_baked_wall_pillar_frac ? &saturn_baked_wall_pillar_frac[ss->pillar_start] : hw_zero_pfrac;
    const baked_seg_t *seg = &saturn_baked_wall_segs[ss->seg_start];
    const baked_seg_t *seg_end = seg + num_segs;
    ctx->portal_first = ss->portal_first;
    ctx->portal_count = 0;
    ctx->sec_ceiling  = sf->dz_ceil;
    ctx->sec_floor    = sf->dz_floor;
    ctx->is_sky_ceil  = sf->is_sky;
    ctx->seg_end      = seg_end;
    hw_wall_light_delta = sf->light_delta;
    if (hw_wctx_tex_frame != hw_render_frame)
    {
        hw_wctx_tex_frame = hw_render_frame;
        ctx->cur_tex_id = -1;
    }
    ctx->last_clip_x0 = -1;
    ctx->last_clip_x1 = -1;
    ctx->n_vis        = 0;
    ctx->sub_m_start  = saturn_cmd_m_count;
    const int cam_x = hw_cam_x, cam_y = hw_cam_y;
    for (; seg < seg_end; seg++)
    {
        if (!seg->flags)
            continue;
        int i0 = seg->pillar_idx[0], i1 = seg->pillar_idx[1];
        const saturn_wl_pillar_t *p0 = &pillars[i0], *p1 = &pillars[i1];
        int obj_dx = (int)p0->x - cam_x, obj_dy = (int)p0->z - cam_y;
        if (HW_CrossBackface(obj_dy, (int)p1->x - (int)p0->x, obj_dx, (int)p1->z - (int)p0->z) <= 0)
            continue;
        if (hw_pil_gen[i0] != pil_gen)
        {
            hw_pil_gen[i0] = pil_gen;
            HW_TransformSubsectorPillar(i0, obj_dx, obj_dy);
        }
        if (hw_pil_gen[i1] != pil_gen)
        {
            hw_pil_gen[i1] = pil_gen;
            HW_TransformSubsectorPillar(i1, (int)p1->x - cam_x, (int)p1->z - cam_y);
        }
        const hw_trans_pillar_t *tp0 = &local_trans_pillars[i0], *tp1 = &local_trans_pillars[i1];
        if (tp0->cz < HW_NEAR_Z || tp1->cz < HW_NEAR_Z || tp0->cz > HW_FAR_Z || tp1->cz > HW_FAR_Z)
            continue;                       /* coarse nodes are not near: no near-plane clip */
        int xa = tp0->px, xb = tp1->px;
        if (xb <= xa || xb <= 0 || xa >= HW_VIEW_W)
            continue;
        ctx->p0 = tp0;
        ctx->p1 = tp1;
        ctx->seg_sx = (int16_t)(xa < 0 ? 0 : xa);
        ctx->seg_ex = (int16_t)(xb > HW_VIEW_W ? HW_VIEW_W : xb);
        ctx->occl_state = 0;
        ctx->needs_near_clip = 0;
        ctx->ytop_min = 0;
        ctx->ybot_max = (int16_t)HW_VIEW_H;
        ctx->portal_recorded = 1;           /* no portal aperture recording */
        if (!(seg->flags & (SEG_FLAG_LOWER | SEG_FLAG_UPPER | SEG_FLAG_MID)))
            HW_MarkSubSolidSpan(ctx->seg_sx, ctx->seg_ex);   /* one-sided: covers its columns */
        HW_EmitVisibleBakedSeg(seg);
    }
    if (ctx->last_clip_x0 != -1)
    {
        HW_EmitUserClip(0, 0, HW_VIEW_W - 1, HW_VIEW_H - 1);
        ctx->last_clip_x0 = -1;
        ctx->last_clip_x1 = -1;
    }
}

/* One subsector of a coarse node. */
static HW_NOINLINE void HW_CoarseLeaf(int subnum, int fully_inside)
{
    if (hw_solid_count >= HW_VIEW_W || (unsigned)subnum >= (unsigned)numsubsectors)
        return;
    int secnum = subsectors[subnum].sector;
    if ((unsigned)secnum >= HW_MAX_AP_SECTORS || saturn_visit_count >= CMDBUF_MAX_VISIT)
        return;
    hw_sec_fast_t *sf = &hw_sec_fast[secnum];
    /* the normal subsector test (frustum, far plane, solid columns, portal
     * aperture of nearer real geometry): ~15% of a full visit's cost */
    int flat_flags = HW_CheckSubsectorAperture(subnum, sf, fully_inside & 3);
    if (flat_flags < 0)
        return;
    saturn_hw_coarse_subs++;
    flat_flags |= (int)HW_FLAT_USE_LOD1;
    if (sf->is_sky)
        flat_flags &= ~(int)HW_FLAT_ALLOW_CEIL;
    int visit_idx = saturn_visit_count++;
    uint16_t m_start = saturn_cmd_m_count;
    sub_range_t *ranges = hw_cur_sub_ranges;
    aliased_uint32_t *r32 = (aliased_uint32_t *)&ranges[visit_idx];
    r32[1] = 0;
    ranges[visit_idx].subnum = (uint16_t)subnum;
    if (!HW_WALLS_ONLY)
    {
        sector_t *sec = &sectors[secnum];
#ifndef SATURN_WIN32
        if (hw_flat_ring)
            HW_SUBMIT_FLAT_JOB(visit_idx, subnum, sec, flat_flags);
        else
#endif
            HW_RenderSubsectorBakedFlats(visit_idx, subnum, sec, flat_flags);
    }
    HW_CoarseLeafWalls(subnum, sf);
    if (!HW_WALLS_ONLY && sf->has_things != 0)
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
    if (sub_solid_active)
        HW_CommitSubsectorOcclusion();      /* solid spans only (no portals recorded) */
}
#endif /* HW_COARSE */
