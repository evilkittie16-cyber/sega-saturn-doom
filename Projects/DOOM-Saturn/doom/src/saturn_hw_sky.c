/* saturn_hw_sky.c — Level Texture/Flat/Sprite Preloader, BSP Flat Bounding Box
 * Expansion, and VDP2 NBG0 Cylindrical Sky Background Rasterizer.
 */

#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
static void Saturn_ExpandNodeBBoxesWithFlats(void)
{
    if (!hw_node_bboxes || !saturn_baked_flat_bboxes || numnodes <= 0)
        return;

    int16_t (*node_flat)[4] = NULL;
    int16_t *stack          = NULL;
    uint8_t *node_state     = NULL;
    int used_vbuf_scratch   = 0;

    if (saturn_cmd_m && (size_t)numnodes * 11u <= (size_t)CMDBUF_M_CAP * sizeof(vdp1_cmd_t))
    {
        uint8_t *base = (uint8_t *)saturn_cmd_m;
        node_flat  = (int16_t (*)[4])base;
        stack      = (int16_t *)(base + (size_t)numnodes * 8u);
        node_state = (uint8_t *)(base + (size_t)numnodes * 10u);
        used_vbuf_scratch = 1;
    }
    else
    {
        node_flat  = (int16_t (*)[4])Z_Malloc(numnodes * 4 * sizeof(int16_t), PU_LEVEL);
        node_state = (uint8_t *)Z_Malloc(numnodes * sizeof(uint8_t), PU_LEVEL);
        stack      = (int16_t *)Z_Malloc(numnodes * sizeof(int16_t), PU_LEVEL);
        if (!node_flat || !node_state || !stack)
        {
            if (stack) Z_Free(stack);
            if (node_state) Z_Free(node_state);
            if (node_flat) Z_Free(node_flat);
            return;
        }
    }

    D_memset(node_state, 0, (size_t)numnodes);

    /* Strict O(N) iterative post-order traversal:
     * Guarantees single-pass resolution regardless of whether the node builder
     * ordered BSP nodes bottom-up or top-down, with cycle protection (state 3). */
    for (int root = numnodes - 1; root >= 0; root--)
    {
        if (node_state[root])
            continue;

        int sp = 0;
        stack[sp++] = (int16_t)root;

        while (sp > 0)
        {
            int i = (int)stack[sp - 1];
            if (node_state[i] == 0)
            {
                node_state[i] = 3; /* Mark children being pushed */
                int c0 = (int)nodes[i].children[0];
                if ((int16_t)c0 >= 0 && c0 < numnodes && node_state[c0] == 0 && sp < numnodes)
                    stack[sp++] = (int16_t)c0;
                int c1 = (int)nodes[i].children[1];
                if ((int16_t)c1 >= 0 && c1 < numnodes && node_state[c1] == 0 && sp < numnodes)
                    stack[sp++] = (int16_t)c1;
                continue;
            }

            sp--;
            if (node_state[i] != 3)
                continue;

            int c0 = (int)nodes[i].children[0];
            int has0 = 0;
            int16_t b0[4];

            if ((int16_t)c0 < 0)
            {
                if ((int16_t)c0 != -1)
                {
                    int subnum = c0 & 0x7FFFu;
                    if (subnum >= 0 && subnum < numsubsectors)
                    {
                        const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
                        if (fb->min_x != fb->max_x || fb->min_y != fb->max_y)
                        {
                            b0[BOXTOP]    = fb->max_y;
                            b0[BOXBOTTOM] = fb->min_y;
                            b0[BOXLEFT]   = fb->min_x;
                            b0[BOXRIGHT]  = fb->max_x;
                            has0 = 1;
                        }
                    }
                }
            }
            else if (c0 >= 0 && c0 < numnodes && node_state[c0] == 2)
            {
                b0[BOXTOP]    = node_flat[c0][BOXTOP];
                b0[BOXBOTTOM] = node_flat[c0][BOXBOTTOM];
                b0[BOXLEFT]   = node_flat[c0][BOXLEFT];
                b0[BOXRIGHT]  = node_flat[c0][BOXRIGHT];
                has0 = 1;
            }

            int c1 = (int)nodes[i].children[1];
            int has1 = 0;
            int16_t b1[4];

            if ((int16_t)c1 < 0)
            {
                if ((int16_t)c1 != -1)
                {
                    int subnum = c1 & 0x7FFFu;
                    if (subnum >= 0 && subnum < numsubsectors)
                    {
                        const saturn_flat_bbox_t *fb = &saturn_baked_flat_bboxes[subnum];
                        if (fb->min_x != fb->max_x || fb->min_y != fb->max_y)
                        {
                            b1[BOXTOP]    = fb->max_y;
                            b1[BOXBOTTOM] = fb->min_y;
                            b1[BOXLEFT]   = fb->min_x;
                            b1[BOXRIGHT]  = fb->max_x;
                            has1 = 1;
                        }
                    }
                }
            }
            else if (c1 >= 0 && c1 < numnodes && node_state[c1] == 2)
            {
                b1[BOXTOP]    = node_flat[c1][BOXTOP];
                b1[BOXBOTTOM] = node_flat[c1][BOXBOTTOM];
                b1[BOXLEFT]   = node_flat[c1][BOXLEFT];
                b1[BOXRIGHT]  = node_flat[c1][BOXRIGHT];
                has1 = 1;
            }

            if (has0)
            {
                int16_t *orig = hw_node_bboxes[i][0];
                if (b0[BOXTOP] > orig[BOXTOP])       orig[BOXTOP] = b0[BOXTOP];
                if (b0[BOXBOTTOM] < orig[BOXBOTTOM]) orig[BOXBOTTOM] = b0[BOXBOTTOM];
                if (b0[BOXLEFT] < orig[BOXLEFT])     orig[BOXLEFT] = b0[BOXLEFT];
                if (b0[BOXRIGHT] > orig[BOXRIGHT])   orig[BOXRIGHT] = b0[BOXRIGHT];
            }
            if ((int16_t)c0 < 0 && (int16_t)c0 != -1)
            {
                int subnum = c0 & 0x7FFFu;
                int16_t *orig = hw_node_bboxes[i][0];
                if (subnum >= 0 && subnum < numsubsectors &&
                    orig[BOXLEFT] <= orig[BOXRIGHT] && orig[BOXBOTTOM] <= orig[BOXTOP])
                {
                    saturn_flat_bbox_t *fb = (saturn_flat_bbox_t *)&saturn_baked_flat_bboxes[subnum];
                    fb->max_y = orig[BOXTOP];
                    fb->min_y = orig[BOXBOTTOM];
                    fb->min_x = orig[BOXLEFT];
                    fb->max_x = orig[BOXRIGHT];
                }
            }
            if (has1)
            {
                int16_t *orig = hw_node_bboxes[i][1];
                if (b1[BOXTOP] > orig[BOXTOP])       orig[BOXTOP] = b1[BOXTOP];
                if (b1[BOXBOTTOM] < orig[BOXBOTTOM]) orig[BOXBOTTOM] = b1[BOXBOTTOM];
                if (b1[BOXLEFT] < orig[BOXLEFT])     orig[BOXLEFT] = b1[BOXLEFT];
                if (b1[BOXRIGHT] > orig[BOXRIGHT])   orig[BOXRIGHT] = b1[BOXRIGHT];
            }
            if ((int16_t)c1 < 0 && (int16_t)c1 != -1)
            {
                int subnum = c1 & 0x7FFFu;
                int16_t *orig = hw_node_bboxes[i][1];
                if (subnum >= 0 && subnum < numsubsectors &&
                    orig[BOXLEFT] <= orig[BOXRIGHT] && orig[BOXBOTTOM] <= orig[BOXTOP])
                {
                    saturn_flat_bbox_t *fb = (saturn_flat_bbox_t *)&saturn_baked_flat_bboxes[subnum];
                    fb->max_y = orig[BOXTOP];
                    fb->min_y = orig[BOXBOTTOM];
                    fb->min_x = orig[BOXLEFT];
                    fb->max_x = orig[BOXRIGHT];
                }
            }

            if (has0 && has1)
            {
                node_flat[i][BOXTOP]    = (b0[BOXTOP] > b1[BOXTOP]) ? b0[BOXTOP] : b1[BOXTOP];
                node_flat[i][BOXBOTTOM] = (b0[BOXBOTTOM] < b1[BOXBOTTOM]) ? b0[BOXBOTTOM] : b1[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = (b0[BOXLEFT] < b1[BOXLEFT]) ? b0[BOXLEFT] : b1[BOXLEFT];
                node_flat[i][BOXRIGHT]  = (b0[BOXRIGHT] > b1[BOXRIGHT]) ? b0[BOXRIGHT] : b1[BOXRIGHT];
                node_state[i] = 2;
            }
            else if (has0)
            {
                node_flat[i][BOXTOP]    = b0[BOXTOP];
                node_flat[i][BOXBOTTOM] = b0[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = b0[BOXLEFT];
                node_flat[i][BOXRIGHT]  = b0[BOXRIGHT];
                node_state[i] = 2;
            }
            else if (has1)
            {
                node_flat[i][BOXTOP]    = b1[BOXTOP];
                node_flat[i][BOXBOTTOM] = b1[BOXBOTTOM];
                node_flat[i][BOXLEFT]   = b1[BOXLEFT];
                node_flat[i][BOXRIGHT]  = b1[BOXRIGHT];
                node_state[i] = 2;
            }
            else
            {
                node_state[i] = 1;
            }
        }
    }

    if (!used_vbuf_scratch)
    {
        Z_Free(stack);
        Z_Free(node_state);
        Z_Free(node_flat);
    }

#ifdef SATURN_WIN32
    printf("Saturn_ExpandNodeBBoxesWithFlats: expanded %d node bboxes with baked flat bounds (O(N) post-order)\n", numnodes);
    fflush(stdout);
#endif
}

/* Set on level init; the next frame bumps ipc->job_done so the slave purges its L1 */
static unsigned char hw_slave_level_dirty = 1;

#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
void Saturn_HW_RenderInit(void)
{
    Saturn_CmdBuf_Init();
    Saturn_Vdp1_ClearPages();

    int s;
    for (s = 0; s < numsectors; s++)
    {
        int f;
        if ((f = sectors[s].floorpic) >= 0 && f < numflats)
        {
            Saturn_FlatData(f);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && f >= a->basepic && f <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_FlatData(p);
                    }
                }
            }
        }
        if ((f = sectors[s].ceilingpic) >= 0 && f < numflats)
        {
            Saturn_FlatData(f);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && f >= a->basepic && f <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_FlatData(p);
                    }
                }
            }
        }
    }
    for (s = 0; s < numsides; s++)
    {
        int k, tex_arr[3];
        tex_arr[0] = sides[s].toptexture;
        tex_arr[1] = sides[s].midtexture;
        tex_arr[2] = sides[s].bottomtexture;
        for (k = 0; k < 3; k++)
        {
            int t = tex_arr[k];
            if (t > 0 && t < numtextures)
            {
                Saturn_TexData(t);
                if (anims && lastanim)
                {
                    const anim_t *a;
                    for (a = anims; a < lastanim; a++)
                    {
                        if (a->istexture && t >= a->basepic && t <= a->picnum)
                        {
                            int p;
                            for (p = a->basepic; p <= a->picnum; p++)
                                Saturn_TexData(p);
                        }
                    }
                }
                if (switchlist)
                {
                    int sw;
                    for (sw = 0; sw < MAXSWITCHES * 2 && switchlist[sw]; sw++)
                    {
                        if (switchlist[sw] == t)
                        {
                            int opp = switchlist[sw ^ 1];
                            if (opp > 0 && opp < numtextures)
                                Saturn_TexData(opp);
                        }
                    }
                }
            }
        }
    }
    /* Preload all sprites for things present in the level so rendering never hits CD */
    if (firstsprite > 0 && numsprites > 0)
    {
        uint8_t spr_used[NUMSPRITES];
        memset(spr_used, 0, sizeof(spr_used));

        /* Weapon sprites */
        spr_used[SPR_PUNG] = 1;
        spr_used[SPR_PISG] = 1;
        spr_used[SPR_PISF] = 1;
        spr_used[SPR_SHTG] = 1;
        spr_used[SPR_SHTF] = 1;

        for (s = 0; s < numsectors; s++)
        {
            mobj_t *mo = (mobj_t *)sectors[s].thinglist;
            while (mo)
            {
                if (mo->state >= 0 && mo->state < NUMSTATES)
                {
                    int sp = states[mo->state].sprite;
                    if (sp >= 0 && sp < NUMSPRITES)
                        spr_used[sp] = 1;
                }
                if (mo->type >= 0 && mo->type < NUMMOBJTYPES)
                {
                    const mobjinfo_t *mi = &mobjinfo[mo->type];
                    /* S_NULL (0) maps to SPR_TROO: skip it, otherwise every mobj with an
                     * unused state slot drags the full imp sprite set into hram. */
#define HW_PRELOAD_STATE(st) do { if ((st) > S_NULL && (st) < NUMSTATES) spr_used[states[(st)].sprite] = 1; } while (0)
                    HW_PRELOAD_STATE(mi->spawnstate);
                    HW_PRELOAD_STATE(mi->seestate);
                    HW_PRELOAD_STATE(mi->painstate);
                    HW_PRELOAD_STATE(mi->meleestate);
                    HW_PRELOAD_STATE(mi->missilestate);
                    HW_PRELOAD_STATE(mi->deathstate);
                    HW_PRELOAD_STATE(mi->xdeathstate);
#undef HW_PRELOAD_STATE
                }
                mo = (mobj_t *)mo->snext;
            }
        }

        for (s = 0; s < NUMSPRITES; s++)
        {
            if (!spr_used[s])
                continue;
            const spritedef_t *sprdef = &sprites[s];
            for (int f = 0; f < sprdef->numframes; f++)
            {
                spriteframe_t *sf = &spriteframes[sprdef->firstframe + f];
                int l_idx = sf->lump;
                int count = (spritelumps[l_idx] & SL_SINGLESIDED) ? 1 : 8;
                for (int r = 0; r < count; r++)
                {
                    int lump = (int)(spritelumps[l_idx + r] & SL_LUMPMASK);
                    if (lump >= firstsprite && lump < firstsprite + numsprites)
                    {
                        Saturn_SpriteData(lump - firstsprite);
                    }
                }
            }
        }
    }

    /* Preload SKY1 composite into fast WRAM-L (0x00220000) so background fill never hits CD or slow bus */
    int skytex = Saturn_SkyTexNum();
    if (skytex >= 0)
    {
        const uint8_t *sky = (const uint8_t *)Saturn_TexData(skytex);
        if (sky)
        {
            int w = Saturn_TexWidth(skytex);
            int h = Saturn_TexHeight(skytex);
            int sz = w * h;
            if (sz > 32768) sz = 32768;
            D_memcpy(HW_FAST_SKY_ADDR, sky, sz);
        }
    }
    hw_last_sky_angle = 0xFFFFFFFFu;
    hw_last_sky_cy    = -1;

    /* New level: reset VDP1 arena pages and preload all level flats (and animated frames) to both pages */
    Saturn_Vdp1_ClearPages();
    for (s = 0; s < numsectors; s++)
    {
        if (sectors[s].floorpic >= 0)
        {
            Saturn_Vdp1_PreloadFlat(sectors[s].floorpic);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && sectors[s].floorpic >= a->basepic && sectors[s].floorpic <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_Vdp1_PreloadFlat(p);
                    }
                }
            }
        }
        if (sectors[s].ceilingpic != -1)
        {
            Saturn_Vdp1_PreloadFlat(sectors[s].ceilingpic);
            if (anims && lastanim)
            {
                const anim_t *a;
                for (a = anims; a < lastanim; a++)
                {
                    if (!a->istexture && sectors[s].ceilingpic >= a->basepic && sectors[s].ceilingpic <= a->picnum)
                    {
                        int p;
                        for (p = a->basepic; p <= a->picnum; p++)
                            Saturn_Vdp1_PreloadFlat(p);
                    }
                }
            }
        }
    }
    Saturn_Vdp1_LockPreloadedFlats();
    saturn_hw_sky_latch = 0;
    saturn_hw_sky_prime = 0;
    saturn_view_latch   = 2;
    if (I_VideoBuffer)
        D_memset(I_VideoBuffer, 0xF7, 320 * 168);

    hw_render_frame = 1;
    hw_slave_level_dirty = 1;
    HW_APERTURE->last_pvs_ssec = NULL;
    HW_APERTURE->last_pvs_numnodes = 0;
    HW_APERTURE->dirty_words = 0xFFFFFFFFu;
    if (numsubsectors > 0 && numsubsectors <= HW_MAX_AP_SUBSECTORS &&
        segs && lines && sides && sectors && vertexes)
    {
        hw_portal_seg_t *portal_segs = HW_PORTAL_SEGS;
        int p_total = 0;
        for (int subnum = 0; subnum < numsubsectors; subnum++)
        {
            const subsector_t *sub = &subsectors[subnum];
            int fsec_idx = sub->sector;
            int first = sub->firstline;
            int count = (subnum == numsubsectors - 1) ? (numsegs - first) : SSEC_NUMLINES(sub);
            int sub_first = p_total;
            int sub_cnt = 0;
            if ((unsigned)fsec_idx < (unsigned)numsectors &&
                count > 0 && count <= 1024 && first >= 0 && first + count <= numsegs)
            {
                int is_fsky = ((int16_t)sectors[fsec_idx].ceilingpic == -1);
                for (int j = 0; j < count; j++)
                {
                    const seg_t *sg = &segs[first + j];
                    int line_idx = SEG_UNPACK_LINEDEF(sg);
                    int side_idx = SEG_UNPACK_SIDE(sg);
                    if ((unsigned)line_idx >= (unsigned)numlines || (unsigned)side_idx > 1)
                        continue;
                    const line_t *ld = &lines[line_idx];
                    int s0 = ld->sidenum[side_idx];
                    int s1 = ld->sidenum[side_idx ^ 1];
                    if ((unsigned)s0 >= (unsigned)numsides || (unsigned)s1 >= (unsigned)numsides)
                        continue;
                    int bsec_idx = sides[s1].sector;
                    if (bsec_idx == fsec_idx || (unsigned)bsec_idx >= (unsigned)numsectors)
                        continue;
                    int is_bsky = ((int16_t)sectors[bsec_idx].ceilingpic == -1);
                    int has_upper = (!is_fsky || !is_bsky) && (sides[s0].toptexture > 0);
                    int has_lower = (sides[s0].bottomtexture > 0);
                    if (has_upper || has_lower)
                        continue;
                    int v1_idx = SEG_UNPACK_V1(sg);
                    int v2_idx = SEG_UNPACK_V2(sg);
                    if ((unsigned)v1_idx >= (unsigned)numvertexes ||
                        (unsigned)v2_idx >= (unsigned)numvertexes || v1_idx == v2_idx)
                        continue;
                    if (p_total < HW_MAX_PORTAL_SEGS && sub_cnt < 255)
                    {
                        int16_t vx0 = vertexes[v1_idx].x;
                        int16_t vy0 = vertexes[v1_idx].y;
                        int16_t vx1 = vertexes[v2_idx].x;
                        int16_t vy1 = vertexes[v2_idx].y;
                        uint8_t chain = 0;
                        if (sub_cnt > 0)
                        {
                            hw_portal_seg_t *prev = &portal_segs[p_total - 1];
                            if (prev->x1 == vx0 && prev->y1 == vy0)
                            {
                                if (prev->back_sec_idx == bsec_idx)
                                {
                                    int dx0 = (int)prev->dx;
                                    int dy0 = (int)prev->dy;
                                    int dx1 = (int)vx1 - (int)vx0;
                                    int dy1 = (int)vy1 - (int)vy0;
                                    if (dx0 * dy1 == dy0 * dx1 && (dx0 * dx1 + dy0 * dy1) >= 0)
                                    {
                                        prev->x1 = vx1;
                                        prev->y1 = vy1;
                                        prev->dx = (int16_t)((int)vx1 - (int)prev->x0);
                                        prev->dy = (int16_t)((int)vy1 - (int)prev->y0);
                                        continue;
                                    }
                                }
                                chain = 1;
                            }
                        }
                        portal_segs[p_total].x0 = vx0;
                        portal_segs[p_total].y0 = vy0;
                        portal_segs[p_total].x1 = vx1;
                        portal_segs[p_total].y1 = vy1;
                        portal_segs[p_total].dx = (int16_t)((int)vx1 - (int)vx0);
                        portal_segs[p_total].dy = (int16_t)((int)vy1 - (int)vy0);
                        portal_segs[p_total].back_sec_idx = (int16_t)bsec_idx;
                        portal_segs[p_total].chain_prev = chain;
                        portal_segs[p_total].pad = 0;
                        p_total++;
                        sub_cnt++;
                    }
                }
            }
            if (saturn_baked_wall_subsectors && (unsigned)subnum < (unsigned)saturn_baked_wall_num_subsectors)
            {
                saturn_wl_subsector_t *wss = (saturn_wl_subsector_t *)&saturn_baked_wall_subsectors[subnum];
                wss->portal_first = (uint16_t)sub_first;
                wss->portal_count = (uint16_t)sub_cnt;
                wss->max_bsec0    = -1;
                wss->max_bsec1    = -1;

                int nsegs = (int)wss->num_segs;
                if (saturn_baked_wall_segs && saturn_baked_wall_pillars && nsegs > 0)
                {
                    baked_seg_t *segs_base = &saturn_baked_wall_segs[wss->seg_start];
                    const saturn_wl_pillar_t *pillars = &saturn_baked_wall_pillars[wss->pillar_start];
                    /* 0. Remove bogus duplicate SEG_FLAG_MID segs on 2-sided door/portal linedefs,
                     *    and bind back_sec_idx on genuine 2-sided midtextures. */
                    int clean_n = 0;
                    for (int i = 0; i < nsegs; i++)
                    {
                        baked_seg_t s = segs_base[i];
                        s.flags &= (uint8_t)~SEG_FLAG_PORTAL_DONE;
                        if (s.flags & SEG_FLAG_MID)
                        {
                            uint8_t p0 = s.pillar_idx[0];
                            uint8_t p1 = s.pillar_idx[1];
                            int drop = 0;
                            for (int j = 0; j < nsegs; j++)
                            {
                                if (i != j &&
                                    segs_base[j].pillar_idx[0] == p0 &&
                                    segs_base[j].pillar_idx[1] == p1 &&
                                    (segs_base[j].flags & (SEG_FLAG_UPPER | SEG_FLAG_LOWER)) &&
                                    segs_base[j].back_sec_idx >= 0)
                                {
                                    if (s.tex_64 == segs_base[j].tex_64 || s.tex_64 == 0)
                                    {
                                        drop = 1;
                                        break;
                                    }
                                    s.back_sec_idx = segs_base[j].back_sec_idx;
                                }
                            }
                            if (drop)
                                continue;
                        }
                        segs_base[clean_n++] = s;
                    }
                    nsegs = clean_n;

                    /* 1. Coalesce adjacent colinear 32-unit step segs (tw == 32 && th <= 16) into 64-unit segs */
                    int dst = 0;
                    for (int i = 0; i < nsegs; i++)
                    {
                        baked_seg_t s0 = segs_base[i];
                        if (i + 1 < nsegs && s0.u_start == 0 && s0.u_span == 32 && s0.tex_64 > 0)
                        {
                            baked_seg_t s1 = segs_base[i + 1];
                            if (s1.u_start == 0 && s1.u_span == 32 &&
                                s0.flags == s1.flags &&
                                s0.tex_64 == s1.tex_64 &&
                                s0.pillar_idx[1] == s1.pillar_idx[0] &&
                                s0.mid_static_v_offset == s1.mid_static_v_offset &&
                                s0.back_sec_idx == s1.back_sec_idx &&
                                s0.baked_light == s1.baked_light &&
                                Saturn_TexWidth(s0.tex_64) == 32 &&
                                Saturn_TexHeight(s0.tex_64) <= 16)
                            {
                                int p0 = s0.pillar_idx[0], p1 = s0.pillar_idx[1], p2 = s1.pillar_idx[1];
                                int dx0 = (int)pillars[p1].x - (int)pillars[p0].x;
                                int dy0 = (int)pillars[p1].z - (int)pillars[p0].z;
                                int dx1 = (int)pillars[p2].x - (int)pillars[p1].x;
                                int dy1 = (int)pillars[p2].z - (int)pillars[p1].z;
                                int cross = dx0 * dy1 - dy0 * dx1;
                                if (cross >= -64 && cross <= 64 && (dx0 * dx1 + dy0 * dy1) > 0)
                                {
                                    s0.pillar_idx[1] = (uint8_t)p2;
                                    s0.u_span        = 64;
                                    s0.inv_u_span    = 1024;
                                    i++;
                                }
                            }
                        }
                        segs_base[dst++] = s0;
                    }
                    nsegs = dst;
                    wss->num_segs = (uint16_t)nsegs;

                    /* 2. Group segs sharing the exact same (pillar_idx[0], pillar_idx[1]) so pair_key cache hits */
                    for (int i = 0; i < nsegs; i++)
                    {
                        uint8_t p0 = segs_base[i].pillar_idx[0];
                        uint8_t p1 = segs_base[i].pillar_idx[1];
                        for (int j = i + 1; j < nsegs; j++)
                        {
                            if (segs_base[j].pillar_idx[0] == p0 && segs_base[j].pillar_idx[1] == p1)
                            {
                                if (j > i + 1)
                                {
                                    baked_seg_t tmp = segs_base[j];
                                    for (int k = j; k > i + 1; k--)
                                        segs_base[k] = segs_base[k - 1];
                                    segs_base[i + 1] = tmp;
                                }
                                i++;
                            }
                        }
                    }

                    /* 3. Mark SEG_FLAG_PORTAL_DONE on 2-sided sub-segs covered by an earlier wider 2-sided seg on the same line */
                    for (int j = 1; j < nsegs; j++)
                    {
                        uint8_t fj = segs_base[j].flags;
                        if (!(fj & (SEG_FLAG_UPPER | SEG_FLAG_LOWER)))
                            continue;
                        int bsec_idx = segs_base[j].back_sec_idx;
                        if ((unsigned)bsec_idx >= (unsigned)numsectors)
                            continue;
                        int xj0 = (int)pillars[segs_base[j].pillar_idx[0]].x;
                        int zj0 = (int)pillars[segs_base[j].pillar_idx[0]].z;
                        int xj1 = (int)pillars[segs_base[j].pillar_idx[1]].x;
                        int zj1 = (int)pillars[segs_base[j].pillar_idx[1]].z;
                        for (int i = 0; i < j; i++)
                        {
                            uint8_t fi = segs_base[i].flags;
                            if (!(fi & (SEG_FLAG_UPPER | SEG_FLAG_LOWER)) || segs_base[i].back_sec_idx != bsec_idx)
                                continue;
                            if (segs_base[i].pillar_idx[0] == segs_base[j].pillar_idx[0] &&
                                segs_base[i].pillar_idx[1] == segs_base[j].pillar_idx[1])
                                break; /* Handled by last_pair_key */
                            if (i + 1 < nsegs &&
                                segs_base[i + 1].pillar_idx[0] == segs_base[i].pillar_idx[0] &&
                                segs_base[i + 1].pillar_idx[1] == segs_base[i].pillar_idx[1])
                                continue;
                            int xi0 = (int)pillars[segs_base[i].pillar_idx[0]].x;
                            int zi0 = (int)pillars[segs_base[i].pillar_idx[0]].z;
                            int dxi = (int)pillars[segs_base[i].pillar_idx[1]].x - xi0;
                            int dzi = (int)pillars[segs_base[i].pillar_idx[1]].z - zi0;
                            int len2 = dxi * dxi + dzi * dzi;
                            if (len2 <= 0)
                                continue;
                            int rx0 = xj0 - xi0, rz0 = zj0 - zi0;
                            int rx1 = xj1 - xi0, rz1 = zj1 - zi0;
                            int c0 = rx0 * dzi - rz0 * dxi;
                            int c1 = rx1 * dzi - rz1 * dxi;
                            int dot0 = rx0 * dxi + rz0 * dzi;
                            int dot1 = rx1 * dxi + rz1 * dzi;
                            if (c0 >= -64 && c0 <= 64 && c1 >= -64 && c1 <= 64 &&
                                dot0 >= 0 && dot1 <= len2 && dot1 > dot0)
                            {
                                segs_base[j].flags |= SEG_FLAG_PORTAL_DONE;
                                break;
                            }
                        }
                    }

                    /* 4. Record max floor & min ceiling backsectors for non-solid/non-mid subsectors */
                }
                if ((unsigned)fsec_idx < (unsigned)numsectors)
                {
                    int is_fsky = ((int16_t)sectors[fsec_idx].ceilingpic == -1);
                    int can_cull = 1;
                    int max_floor_bsec = fsec_idx;
                    int min_ceil_bsec  = is_fsky ? -1 : fsec_idx;
                    if (saturn_baked_wall_segs && nsegs > 0)
                    {
                        const baked_seg_t *segs_base = &saturn_baked_wall_segs[wss->seg_start];
                        for (int i = 0; i < nsegs; i++)
                        {
                            uint8_t fl = segs_base[i].flags & (uint8_t)~SEG_FLAG_PORTAL_DONE;
                            if (!fl)
                                continue;
                            if ((fl & (SEG_FLAG_SOLID | SEG_FLAG_MID)) ||
                                (is_fsky && (fl & SEG_FLAG_UPPER)))
                            {
                                can_cull = 0;
                                break;
                            }
                            int bsec_idx = segs_base[i].back_sec_idx;
                            if ((unsigned)bsec_idx >= (unsigned)numsectors ||
                                is_fsky != ((int16_t)sectors[bsec_idx].ceilingpic == -1))
                            {
                                can_cull = 0;
                                break;
                            }
                            if (sectors[bsec_idx].floorheight > sectors[max_floor_bsec].floorheight)
                                max_floor_bsec = bsec_idx;
                            if (!is_fsky && sectors[bsec_idx].ceilingheight < sectors[min_ceil_bsec].ceilingheight)
                                min_ceil_bsec = bsec_idx;
                        }
                    }
                    if (can_cull && portal_segs && sub_cnt > 0)
                    {
                        for (int i = 0; i < sub_cnt; i++)
                        {
                            int bsec_idx = portal_segs[sub_first + i].back_sec_idx;
                            if ((unsigned)bsec_idx >= (unsigned)numsectors ||
                                is_fsky != ((int16_t)sectors[bsec_idx].ceilingpic == -1))
                            {
                                can_cull = 0;
                                break;
                            }
                            if (sectors[bsec_idx].floorheight > sectors[max_floor_bsec].floorheight)
                                max_floor_bsec = bsec_idx;
                            if (!is_fsky && sectors[bsec_idx].ceilingheight < sectors[min_ceil_bsec].ceilingheight)
                                min_ceil_bsec = bsec_idx;
                        }
                    }
                    if (can_cull)
                    {
                        wss->max_bsec0 = (int16_t)max_floor_bsec;
                        wss->max_bsec1 = (int16_t)min_ceil_bsec;
                    }
                }
            }
            hw_sub_flat_info_t *fi = &HW_SUB_FLAT_INFO[subnum];
            fi->q_first      = saturn_baked_flat_q_first      ? saturn_baked_flat_q_first[subnum]      : 0;
            fi->v_first      = saturn_baked_flat_v_first      ? saturn_baked_flat_v_first[subnum]      : 0;
            fi->q_count      = saturn_baked_flat_q_count      ? saturn_baked_flat_q_count[subnum]      : 0;
            fi->v_count      = saturn_baked_flat_v_count      ? saturn_baked_flat_v_count[subnum]      : 0;
            fi->lod1_q_first = saturn_baked_flat_lod1_q_first ? saturn_baked_flat_lod1_q_first[subnum] : 0;
            fi->lod1_v_first = saturn_baked_flat_lod1_v_first ? saturn_baked_flat_lod1_v_first[subnum] : 0;
            fi->lod1_q_count = saturn_baked_flat_lod1_q_count ? saturn_baked_flat_lod1_q_count[subnum] : 0;
            fi->lod1_v_count = saturn_baked_flat_lod1_v_count ? saturn_baked_flat_lod1_v_count[subnum] : 0;
        }
    }

    /* Expand BSP node bboxes so subsector flats are never culled prematurely */
    Saturn_ExpandNodeBBoxesWithFlats();

    /* VDP2 RBG0 ground plane deactivated: large outdoor sectors render via Large LOD VDP1 quads */
    saturn_vdp2_plane_active = 0;
}

void Saturn_HW_ForceSkyRepaint(void)
{
    hw_last_sky_angle = 0xFFFFFFFFu;
    hw_last_sky_cy    = -1;
    hw_last_sky_max_y = 0;
    hw_sky_dirty      = 1;
}

/* Background fill in I_VideoBuffer: Paints the sky texture across rows 0..95
 * when visible in skybox openings. VDP2 saturn_vdp2_copy_back transfers this
 * region to NBG0 bitmap plane, where it displays behind VDP1 3D walls/floors. */
static HW_HOT_O2 void HW_FillBackground(void)
{
    if (!saturn_hw_sky_visible && saturn_hw_sky_prime <= 0)
        return;

    int skytex = Saturn_SkyTexNum();
    if (skytex < 0)
        return;
    const uint8_t *sky = HW_FAST_SKY_ADDR;
    int w = Saturn_TexWidth(skytex);
    int h = Saturn_TexHeight(skytex);
    if (w <= 0 || h <= 0)
        return;
    /* Prevent OOB reads if custom sky exceeds 32 KB buffer */
    if (w * h > 32768)
    {
        h = 32768 / w;
        if (h <= 0)
            return;
    }

#ifndef SATURN_WIN32
    int max_sky_y = (hw_is_sky || saturn_view_latch > 0) ? HW_VIEW_H : 108;
    if (max_sky_y > HW_VIEW_H) max_sky_y = HW_VIEW_H;
#else
    int max_sky_y = HW_VIEW_H;
#endif

    /* When camera has not rotated or changed pitch/height, I_VideoBuffer rows 0..max_sky_y
     * already contain the exact sky raster from the previous frame.
     * Note: saturn_vdp2_copy_back() in i_video.c owns decrementing saturn_hw_sky_latch
     * (2 -> 1 -> 0) across two frames so both double-buffered VDP2 NBG0 pages receive
     * the updated I_VideoBuffer sky. */
    if (!hw_sky_dirty && hw_viewangle == hw_last_sky_angle &&
        HW_VIEW_CY == hw_last_sky_cy && max_sky_y <= hw_last_sky_max_y)
        return;

    hw_last_sky_angle   = hw_viewangle;
    hw_last_sky_cy      = HW_VIEW_CY;
    hw_last_sky_max_y   = max_sky_y;
    hw_sky_dirty        = 0;
    saturn_hw_sky_latch = 2;

    /* 1-time angle table initialization (stored in Low WRAM scratch via HW_SKY_COL_BAM) */
    int32_t *hw_col_bam = HW_SKY_COL_BAM;
    static int last_bam_w = 0;
    if (__builtin_expect(last_bam_w != saturn_view_w, 0))
    {
        int cx = saturn_view_w >> 1;
        int x;
        for (x = 0; x < saturn_view_w; x++)
        {
            int adx = x - cx;
            int aadx = adx < 0 ? -adx : adx;
            int lo = 0, hi = FINEANGLES / 4, ix;
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;
                if ((int32_t)finesine(mid) * cx >= (int32_t)finecosine(mid) * aadx)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            ix = lo;
            hw_col_bam[x] = (adx >= 0) ? (ix << ANGLETOFINESHIFT)
                                       : -(ix << ANGLETOFINESHIFT);
        }
        last_bam_w = saturn_view_w;
    }

    int16_t col_sky[HW_MAX_SCREEN_W];
    int x;
    if ((w & (w - 1)) == 0)
    {
        int wmask = w - 1;
        for (x = 0; x < saturn_view_w; x++)
            col_sky[x] = (int16_t)(((hw_viewangle - (unsigned)hw_col_bam[x]) >> 22) & wmask);
    }
    else
    {
        uint32_t uw = (uint32_t)w;
        for (x = 0; x < saturn_view_w; x++)
        {
            uint32_t val = (hw_viewangle - (unsigned)hw_col_bam[x]) >> 22;
            col_sky[x] = (int16_t)((val * uw) >> 10);
        }
    }

    int y;
    /* Vanilla placement: skytexturemid = 100, so texture row 100 sits on
     * the horizon (centre row) and the top of the view shows row 100 - CY.
     * Fill all the way down to HW_VIEW_H so low-ceiling outdoor sectors and
     * distant outdoor walls/floors that sit below the horizon don't leave black gaps. */
    int row_ofs = 100 - HW_VIEW_CY;
    int prev_texrow = -1;

    /* 16-byte aligned line buffer so 32-byte unrolled copies map cleanly to SH-2 16-byte L1 cache lines */
    HW_ALIGN16 uint32_t cached_sky_row[HW_MAX_SCREEN_W / 4];

    for (y = 0; y < max_sky_y; y++)
    {
        int texrow = y + row_ofs;
        if (texrow < 0) texrow = 0;
        if (texrow >= h) texrow = h - 1;
        uint8_t *dst = I_VideoBuffer + y * saturn_view_w;

        if (texrow == prev_texrow)
        {
            /* Fast 32-byte cache-line unrolled duplicate of identical clamped sky row from L1 CACHE */
            aliased_uint32_t *d32 = (aliased_uint32_t *)dst;
            const aliased_uint32_t *s32 = (const aliased_uint32_t *)cached_sky_row;
            int count = saturn_view_w >> 5;
            while (count--)
            {
                uint32_t r0 = *s32++, r1 = *s32++, r2 = *s32++, r3 = *s32++;
                *d32++ = r0; *d32++ = r1; *d32++ = r2; *d32++ = r3;

                uint32_t r4 = *s32++, r5 = *s32++, r6 = *s32++, r7 = *s32++;
                *d32++ = r4; *d32++ = r5; *d32++ = r6; *d32++ = r7;
            }
            continue;
        }
        prev_texrow = texrow;

        const uint8_t *src = sky + texrow * w;
        aliased_uint32_t *d32 = (aliased_uint32_t *)dst;
        const int16_t *csky = col_sky;
        int n_words = saturn_view_w >> 2;
        if (texrow == 0 || texrow == h - 1)
        {
            aliased_uint32_t *c32 = (aliased_uint32_t *)cached_sky_row;
            for (x = 0; x < n_words; x += 2, csky += 8)
            {
#ifdef SATURN_WIN32
                uint32_t r0 = ((uint32_t)src[csky[0]]) | ((uint32_t)src[csky[1]] << 8) |
                              ((uint32_t)src[csky[2]] << 16) | ((uint32_t)src[csky[3]] << 24);
                uint32_t r1 = ((uint32_t)src[csky[4]]) | ((uint32_t)src[csky[5]] << 8) |
                              ((uint32_t)src[csky[6]] << 16) | ((uint32_t)src[csky[7]] << 24);
#else
                uint32_t r0 = ((((uint32_t)src[csky[0]] << 8) | (uint32_t)src[csky[1]]) << 16) |
                              ((uint32_t)src[csky[2]] << 8) | (uint32_t)src[csky[3]];
                uint32_t r1 = ((((uint32_t)src[csky[4]] << 8) | (uint32_t)src[csky[5]]) << 16) |
                              ((uint32_t)src[csky[6]] << 8) | (uint32_t)src[csky[7]];
#endif
                d32[x + 0] = r0; d32[x + 1] = r1;
                c32[x + 0] = r0; c32[x + 1] = r1;
            }
        }
        else
        {
            for (x = 0; x < n_words; x += 2, csky += 8)
            {
#ifdef SATURN_WIN32
                uint32_t r0 = ((uint32_t)src[csky[0]]) | ((uint32_t)src[csky[1]] << 8) |
                              ((uint32_t)src[csky[2]] << 16) | ((uint32_t)src[csky[3]] << 24);
                uint32_t r1 = ((uint32_t)src[csky[4]]) | ((uint32_t)src[csky[5]] << 8) |
                              ((uint32_t)src[csky[6]] << 16) | ((uint32_t)src[csky[7]] << 24);
#else
                uint32_t r0 = ((((uint32_t)src[csky[0]] << 8) | (uint32_t)src[csky[1]]) << 16) |
                              ((uint32_t)src[csky[2]] << 8) | (uint32_t)src[csky[3]];
                uint32_t r1 = ((((uint32_t)src[csky[4]] << 8) | (uint32_t)src[csky[5]]) << 16) |
                              ((uint32_t)src[csky[6]] << 8) | (uint32_t)src[csky[7]];
#endif
                d32[x + 0] = r0; d32[x + 1] = r1;
            }
        }
    }
}

