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

/* Walk a subsector (takes 2 register arguments: r4=subnum, r5=fully_inside) */
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsector(int subnum, int fully_inside)
{
    if (hw_solid_count >= HW_VIEW_W || (unsigned)subnum >= (unsigned)numsubsectors)
        return;

    subsector_t *sub = &subsectors[subnum];
    int secnum = sub->sector;
    if ((unsigned)secnum >= (unsigned)numsectors || saturn_visit_count >= CMDBUF_MAX_VISIT)
        return;
    sector_t *sec = &sectors[secnum];
    int flat_flags = HW_CheckSubsectorAperture(subnum, sec, (secnum << 8) | (fully_inside & 3));
    if (flat_flags < 0)
    {
        saturn_hw_occl_culls++;
        return;
    }

    if ((int16_t)sec->ceilingpic == -1)
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

    if (flat_flags & (int)HW_FLAT_ALLOW_BOTH)
    {
#ifndef SATURN_WIN32
        if (__builtin_expect(saturn_slave_enabled, 1))
        {
            /* Concurrent execution: Submit to Slave so Slave processes flats while Master computes wall geometry */
            HW_SubmitSlaveFlatJob(visit_idx, subnum, sec, flat_flags);
        }
        else
#endif
        {
            HW_RenderSubsectorBakedFlats(visit_idx, subnum, sec, flat_flags);
        }
    }

    /* 1. Render segs (front walls of this subsector) */
    HW_RenderSubsectorBakedWalls(subnum, sec);

    /* 2. Render subsector sprites (lazy per-sector frustum-culled bucketing) */
    if (__builtin_expect(HW_SubsectorHasThings(sec, (uint32_t)secnum, subnum), 0))
    {
        HW_RenderSubsectorSprites(sec, subnum);
    }

#ifndef SATURN_WIN32
    r32[0] = ((uint32_t)m_start << 16) | (uint16_t)(saturn_cmd_m_count - m_start);
#else
    ranges[visit_idx].m_start = m_start;
    ranges[visit_idx].m_count = (uint16_t)(saturn_cmd_m_count - m_start);
#endif

    /* 3. Mark solid spans and portal apertures from this subsector */
    if (sub_solid_active || HW_APERTURE->portal_span_count > 0)
    {
        HW_CommitSubsectorOcclusion();
    }
}

typedef struct {
    uint16_t bspnum;       /* must be unsigned: node_t children are uint16_t (NF_SUBSECTOR = 0x8000) */
    int16_t  fully_inside;
    const int16_t *bbox;
} bsp_stack_t;

#define HW_BSP_STACK_MAX 64

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

            const node_t *bsp = &local_nodes[bspnum];

            /* Fast 2x muls.w point-on-side using integer camera coordinates (cam_x, cam_y). */
            int side = (HW_MulsW_PairAdd(cam_y - (int)bsp->y, (int)bsp->dx,
                                         (int)bsp->x - cam_x, (int)bsp->dy, 0) >= 0);
#ifdef __GNUC__
            __asm__("" : "+r"(side));
#endif

            /* Read PVS-pruned children directly from node_t.encbbox (same 16B L1 cache line as x,y,dx,dy!) */
            int c0 = (int)(int16_t)bsp->encbbox[0];
            int c1 = (int)(int16_t)bsp->encbbox[1];
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
            if (saturn_baked_flat_q_count && numsubsectors > 0 && numsubsectors <= HW_MAX_AP_SUBSECTORS)
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
                int x_w0 = w << 5;
                aliased_uint32_t *yt32 = (aliased_uint32_t *)&ap->ytop[x_w0];
                aliased_uint32_t *yb32 = (aliased_uint32_t *)&ap->ybot[x_w0];
                yt32[0] = 0; yt32[1] = 0; yt32[2] = 0; yt32[3] = 0;
                yt32[4] = 0; yt32[5] = 0; yt32[6] = 0; yt32[7] = 0;
                yb32[0] = yb4; yb32[1] = yb4; yb32[2] = yb4; yb32[3] = yb4;
                yb32[4] = yb4; yb32[5] = yb4; yb32[6] = yb4; yb32[7] = yb4;
                hw_ap_words[w].yt_min = 0;
                hw_ap_words[w].yb_max = vh;
                hw_ap_words[w].yt_max = 0;
                hw_ap_words[w].yb_min = vh;
            }
        }
        int n_ap_sec = (numsectors < HW_MAX_AP_SECTORS) ? numsectors : HW_MAX_AP_SECTORS;
        int n_ap_pairs = (n_ap_sec + 1) >> 1;
        aliased_uint32_t *syt32 = (aliased_uint32_t *)ap->sec_yt_attach;
        aliased_uint32_t *syb32 = (aliased_uint32_t *)ap->sec_yb_attach;
        for (int i = 0; i < n_ap_pairs; i++)
        {
            syt32[i] = vh_pair;
            syb32[i] = 0;
        }
        ap->portal_span_count = 0;
        ap->aperture_active = 0;
    }
    {
        int sec_words = ((numsectors > 512 ? 512 : numsectors) + 31) >> 5;
        for (int i = 0; i < sec_words; i++)
            hw_sec_things_built[i] = 0;
        int sub_words = ((numsubsectors > 768 ? 768 : numsubsectors) + 31) >> 5;
        for (int i = 0; i < sub_words; i++)
            hw_sub_has_things[i] = 0;
        hw_thing_pool_count = 0;
    }
    hw_render_frame++;
    if (hw_render_frame == 0)
        hw_render_frame = 1;
    hw_flat_sec_cache.sec = NULL;
    hw_prof_segs_acc = 0;
    saturn_hw_prof_flats = 0;
    saturn_hw_prof_bg = 0;
    saturn_hw_wall_quads = 0;
    saturn_hw_flat_quads = 0;
    saturn_hw_slave_timeout = 0;
    saturn_hw_prof_slave_wait = 0;
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
    HW_RenderPlayerWeapon();

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

    /* Walk BSP tree and emit visible walls, flats & sprites in exact topological order */
    extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
    extern volatile unsigned int saturn_vbl_count;
    unsigned short tb0;
    unsigned int vb0;
    do {
        vb0 = saturn_vbl_count;
        tb0 = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != vb0, 0));
    HW_RenderBSPNode((int)numnodes - 1, pvs_row, 0);
    saturn_hw_prof_segs = hw_prof_segs_acc;
    saturn_hw_prof_bsp = Saturn_ProfSpan(tb0, vb0);

    static int saturn_hw_sky_was_visible = 0;
    int bg_drawn = 0;

#ifndef SATURN_WIN32
    if (hw_frame_slave_enabled)
    {
        volatile hw_slave_ipc_t *ipc = (volatile hw_slave_ipc_t *)HW_SLAVE_BASE_UNCACHED;

        /* Overlap Master deferred flat overflow & background sky rendering with Slave SH-2 flat queue processing */
        if (hw_def_flat_count > 0)
        {
            for (int dj = 0; dj < hw_def_flat_count; dj++)
            {
                HW_Master_ProcessJob(ipc, &hw_def_flat_jobs[dj]);
            }
            hw_def_flat_count = 0;
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

        unsigned short twait0;
        unsigned int vwait0;
        do {
            vwait0 = saturn_vbl_count;
            twait0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != vwait0, 0));
        int wait_guard = 1000000; /* x 64 cycles: same timeout as before */
        while (1)
        {
            if ((int)(ipc->job_tail - ipc->job_head) <= 0)
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
        saturn_hw_prof_slave_wait = Saturn_ProfSpan(twait0, vwait0);
        
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
