/* saturn_hw_sprites.c — Player Weapon PSprite Renderer, Lazy Per-Sector Thing
 * Bucketing, 4-Gap Shell Sort, Billboards, and Fast Texture SRCA Lookup.
 */

/* Queue player weapon and muzzle flash: weapon first (psprites[0]), muzzle flash second (psprites[1]),
 * ensuring muzzle flash renders on top in VDP1 Painter's algorithm order */
#ifndef _MSC_VER
__attribute__((optimize("Os")))
#endif
static void HW_RenderPlayerWeapon(void)
{
    player_t *p = &players[hw_displayplayer];
    int base = Saturn_Vdp1Wall_Base();
    int ok = 1;
    int i;

    if (!spriteframes || !sprites || !p || !p->mo || !p->mo->subsector)
        return;

    for (i = 0; ok && i < NUMPSPRITES; i++)
    {
        pspdef_t *psp = &p->psprites[i];
        const state_t *st;
        const spritedef_t *sprdef;
        spriteframe_t *sf;
        unsigned lumpv;
        int lump, scache, w, h, lofs, tofs;
        int x1, y0, r0, r1, sh, pw, xA, xB;
        int secnum;

        if (!psp->state || (unsigned)psp->state >= NUMSTATES)
            continue;
        st = &states[psp->state];
        if (st->sprite < 0 || st->sprite >= NUMSPRITES)
            continue;
        sprdef = &sprites[st->sprite];
        if (sprdef->numframes <= 0)
            continue;
        if ((st->frame & FF_FRAMEMASK) >= sprdef->numframes)
            continue;
        sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];
        lumpv = (unsigned short)spritelumps[sf->lump];
        lump = (int)(lumpv & SL_LUMPMASK);
        if (lump < firstsprite || lump >= firstsprite + numsprites)
            continue;
        scache = lump - firstsprite;
        if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
            continue;
        if (w < 1 || h < 1)
            continue;
        x1 = (psp->sx >> FRACBITS) - lofs + ((saturn_view_w - 320) >> 1);
        y0 = 2 + (psp->sy >> FRACBITS) - tofs;
        r0 = 0;
        r1 = h - 1;
        if (y0 + r0 < 0) r0 = 0 - y0;
        if (y0 + r1 >= HW_VIEW_H) r1 = HW_VIEW_H - 1 - y0;
        if (r0 > r1)
            continue;
        secnum = (p->mo && p->mo->subsector) ? p->mo->subsector->sector : -1;
        sh = (secnum >= 0 && secnum < numsectors)
           ? HW_FastLightRow(sectors[secnum].lightlevel, 0) : 0;
        if (st->frame & FF_FULLBRIGHT)
            sh = 0;
        pw = (w + 7) & ~7;
        xA = x1;
        xB = x1 + pw - 1;
        if (xA < -1024 || xA > 1023 || xB < -1024 || xB > 1023
            || y0 + r0 < -1024 || y0 + r0 > 1023
            || y0 + r1 < -1024 || y0 + r1 > 1023)
        {
            ok = 0;
            break;
        }
        if (!Saturn_Vdp1Spr_Add(scache, xA, xB,
                                y0 + r0, y0 + r1,
                                r0, r1 - r0 + 1, sh, 1))
        {
            ok = 0;
            break;
        }
    }
    if (!ok)
        Saturn_Vdp1Wall_Restore(base);
}

typedef struct {
    mobj_t  *mo;
    fixed_t  cz;
    fixed_t  cx;
    uint8_t  next;
    uint8_t  pad[3];
} hw_thing_entry_t; /* 16 bytes (shift-indexed by << 4) */

#define HW_THING_POOL_CAP 64
static uint32_t         hw_sec_things_built[16]; /* Up to 512 sectors */
static uint32_t         hw_sub_has_things[24];   /* Up to 768 subsectors */
static uint8_t          hw_sub_first_thing[768];
static hw_thing_entry_t hw_thing_pool[HW_THING_POOL_CAP];
static int              hw_thing_pool_count = 0;

static const byte      *hw_cur_pvs_row = NULL;
static sub_range_t     *hw_cur_sub_ranges = NULL;

/* Walk sec->thinglist ONCE per sector per frame, frustum-cull in camera space,
 * and bucket surviving visible things by subsector index. */
static HW_NOINLINE HW_HOT_O2 void HW_BuildSectorThings(sector_t *sec)
{
    mobj_t *mo = (mobj_t *)sec->thinglist;
    const mobj_t *player_mo = players[hw_displayplayer].mo;
    const byte *pvs_row = hw_cur_pvs_row;
    const fixed_t vx   = hw_viewx;
    const fixed_t vy   = hw_viewy;
    const fixed_t vcos = hw_viewcos;
    const fixed_t vsin = hw_viewsin;

    while (mo)
    {
        const subsector_t *msub_ptr = mo->subsector;
        if (mo != player_mo && msub_ptr &&
            !(mo->flags & MF_NOSECTOR) && mo->state && (unsigned)mo->state < NUMSTATES)
        {
            int msub = (int)(msub_ptr - subsectors);
            if ((unsigned)msub < (unsigned)numsubsectors && (unsigned)msub < 768u &&
                (!pvs_row || (pvs_row[msub >> 3] & hw_bit_lut[msub & 7])))
            {
                fixed_t dx = mo->x - vx;
                fixed_t dy = mo->y - vy;
                fixed_t cz1, cz2;
                HW_FixedMulPair(dx, vcos, dy, vsin, &cz1, &cz2);
                fixed_t cz = cz1 + cz2;

                if (cz >= (12 << FRACBITS) && cz <= HW_FAR_Z)
                {
                    fixed_t cx1, cx2;
                    HW_FixedMulPair(dx, vsin, dy, vcos, &cx1, &cx2);
                    fixed_t cx = cx1 - cx2;
                    fixed_t max_cx = cz + (cz >> 2) + (128 << FRACBITS);

                    if (cx >= -max_cx && cx <= max_cx && hw_thing_pool_count < HW_THING_POOL_CAP)
                    {
                        uint32_t sbit = hw_bit_lut[msub & 31];
                        uint8_t next_idx = 0xFFu;
                        if (hw_sub_has_things[msub >> 5] & sbit)
                            next_idx = hw_sub_first_thing[msub];
                        else
                            hw_sub_has_things[msub >> 5] |= sbit;

                        int idx = hw_thing_pool_count++;
                        hw_thing_pool[idx].mo   = mo;
                        hw_thing_pool[idx].cz   = cz;
                        hw_thing_pool[idx].cx   = cx;
                        hw_thing_pool[idx].next = next_idx;
                        hw_sub_first_thing[msub] = (uint8_t)idx;
                    }
                }
            }
        }
        mo = (mobj_t *)mo->snext;
    }
}

HW_ALWAYS_INLINE int HW_SubsectorHasThings(sector_t *sec, uint32_t secnum, int subnum)
{
    if (!sec->thinglist || secnum >= 512u)
        return 0;
    uint32_t sec_word = secnum >> 5;
    uint32_t sec_bit  = hw_bit_lut[secnum & 31];
    if (__builtin_expect(!(hw_sec_things_built[sec_word] & sec_bit), 0))
    {
        hw_sec_things_built[sec_word] |= sec_bit;
        HW_BuildSectorThings(sec);
    }
    return (hw_sub_has_things[(unsigned int)subnum >> 5] & hw_bit_lut[subnum & 31]) != 0;
}

/* Render pre-bucketed visible mobjs (monsters, items, pickups) in subsector */
static HW_NOINLINE HW_HOT_O2 void HW_RenderSubsectorSprites(sector_t *sec, int subnum)
{
    static uint8_t thing_list[32];
    static uint8_t spr_order[32];
    static int     spr_dims[4];
    int n_things = 0;
    for (uint8_t tidx = hw_sub_first_thing[subnum]; tidx != 0xFFu && n_things < 32; tidx = hw_thing_pool[tidx].next)
        thing_list[n_things++] = tidx;
    if (n_things <= 0)
        return;

    typedef struct {
        fixed_t cz;
        int16_t scache;
        int16_t xA, xB, yt, yb;
        int16_t h;
        int16_t shade;
        int16_t pad;
    } hw_sec_spr_t;

    static hw_sec_spr_t spr_queue[32];
    int          spr_count = 0;
    int          i;
    const fixed_t vx = hw_viewx;
    const fixed_t vy = hw_viewy;
    const fixed_t vz = hw_viewz;

    for (int ti = n_things - 1; ti >= 0; ti--)
    {
        const hw_thing_entry_t *te = &hw_thing_pool[thing_list[ti]];
        const mobj_t *mo = te->mo;
        fixed_t cz = te->cz;
        fixed_t cx = te->cx;

        const state_t *st = &states[mo->state];
        if (st->sprite >= 0 && st->sprite < NUMSPRITES)
        {
            const spritedef_t *sprdef = &sprites[st->sprite];
            if (sprdef->numframes > 0 && (st->frame & FF_FRAMEMASK) < sprdef->numframes)
            {
                spriteframe_t *sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];
                const VINT *sl = &spritelumps[sf->lump];
                unsigned short lumpv = (unsigned short)sl[0];
                int flip = 0;

                if (!(lumpv & SL_SINGLESIDED))
                {
                    angle_t ang = R_PointToAngle(vx, vy, mo->x, mo->y);
                    int rot = (int)(((ang - mo->angle + (unsigned)(ANG45 / 2) * 9) >> 29) & 7);
                    lumpv = (unsigned short)sl[rot];
                }
                flip = (lumpv & SL_FLIPPED) ? 1 : 0;
                int lump = (int)(lumpv & SL_LUMPMASK);

                if (lump >= firstsprite && lump < firstsprite + numsprites)
                {
                    int scache = lump - firstsprite;

                    if (Saturn_SpritePatch(scache, &spr_dims[0], &spr_dims[1], &spr_dims[2], &spr_dims[3]))
                    {
                        int w = spr_dims[0];
                        int h = spr_dims[1];
                        int lofs = spr_dims[2];
                        int tofs = spr_dims[3];
                        if (w >= 1 && h >= 1)
                        {
                        int s, cx_screen, xl, x2t, pw, wpx, xA, xB, yt, yb;
                        fixed_t y_world;
                        int shade;

                        s = HW_FastDiv(cz);
                        cx_screen = HW_ProjX(cx, s, HW_VIEW_CX);
                        pw  = (w + 7) & ~7;
                        wpx = (pw * s) >> FRACBITS;
                        xl  = cx_screen - ((lofs * s) >> FRACBITS);
                        x2t = xl + ((w * s) >> FRACBITS) - 1;
                        xA  = flip ? x2t : xl;
                        xB  = flip ? xA - wpx + 1 : xl + wpx - 1;

                        y_world = mo->z + ((fixed_t)tofs << FRACBITS) - vz;
                        yt = HW_ProjY(y_world, s, HW_VIEW_CY);
                        yb = yt + ((h * s) >> FRACBITS);

                        if (xB - xA > 1023)
                        {
                            if (xA < -(1023 - HW_VIEW_W)) xA = xB - 1023;
                            else xB = xA + 1023;
                        }
                        else if (xA - xB > 1023)
                        {
                            if (xB < -(1023 - HW_VIEW_W)) xB = xA - 1023;
                            else xA = xB + 1023;
                        }
                        if (yb - yt > 1023)
                        {
                            if (yt < -(1023 - HW_VIEW_H)) yt = yb - 1023;
                            else yb = yt + 1023;
                        }

                        if (xA < -1024) xA = -1024; else if (xA > 1023) xA = 1023;
                        if (xB < -1024) xB = -1024; else if (xB > 1023) xB = 1023;
                        if (yt < -1024) yt = -1024; else if (yt > 1023) yt = 1023;
                        if (yb < -1024) yb = -1024; else if (yb > 1023) yb = 1023;

                        int diff = xA - xB;
                        int sign = diff >> 31;
                        int x_min = xB + (diff & sign);
                        int x_max = xA - (diff & sign);

                        if (x_max > 0 && x_min < HW_VIEW_W && x_min != x_max &&
                            yb > 0 && yt < HW_VIEW_H && yt < yb)
                        {
                            int cull_x0 = x_min < 0 ? 0 : x_min;
                            int cull_x1 = x_max > HW_VIEW_W ? HW_VIEW_W : x_max;
                            if (hw_solid_count == 0 || !HW_IsSpanOccluded(cull_x0, cull_x1))
                            {
                                shade = (st->frame & FF_FULLBRIGHT)
                                      ? 0 : HW_FastLightRow(sec->lightlevel, cz >> FRACBITS);
                                if (spr_count < 32)
                                {
                                    spr_queue[spr_count].cz     = cz;
                                    spr_queue[spr_count].scache = (int16_t)scache;
                                    spr_queue[spr_count].xA     = (int16_t)xA;
                                    spr_queue[spr_count].xB     = (int16_t)xB;
                                    spr_queue[spr_count].yt     = (int16_t)yt;
                                    spr_queue[spr_count].yb     = (int16_t)yb;
                                    spr_queue[spr_count].h      = (int16_t)h;
                                    spr_queue[spr_count].shade  = (int16_t)shade;
                                    spr_count++;
                                }
                            }
                        }
                        }
                    }
                }
            }
        }
    }

    /* Depth sort buffered sector sprites descending by cz (far first, near last).
     * NOTE ON TOPOLOGICAL DELIVERY: While Saturn_CmdBuf_Deliver() reverses the order
     * of SUBSECTORS (back-to-front), commands within each subsector are copied linearly
     * forward into VRAM (walls first, then sprites) and executed forward by VDP1.
     * Therefore, within a single subsector, the farthest sprite must be queued first
     * (at index 0) so it is drawn underneath nearer sprites in the same subsector.
     * Uses a 4-gap Shell Sort (O(N^1.25)) to prevent O(N^2) stalls with dense sprite clusters. */
    for (i = 0; i < spr_count; i++)
        spr_order[i] = (uint8_t)i;

    if (spr_count > 1)
    {
        static const int gaps[4] = { 40, 13, 4, 1 };
        int g = (spr_count <= 4) ? 3 : ((spr_count <= 13) ? 2 : ((spr_count <= 40) ? 1 : 0));
        for (; g < 4; g++)
        {
            int gap = gaps[g];
            for (i = gap; i < spr_count; i++)
            {
                uint8_t key_idx = spr_order[i];
                fixed_t key_cz = spr_queue[key_idx].cz;
                int j = i;
                while (j >= gap)
                {
                    uint8_t prev_idx = spr_order[j - gap];
                    fixed_t prev_cz = spr_queue[prev_idx].cz;

                    /* Swap if out of order by depth, OR if depths are equal but original sequence order is inverted */
                    if (prev_cz < key_cz || (prev_cz == key_cz && prev_idx > key_idx))
                    {
                        spr_order[j] = prev_idx;
                        j -= gap;
                    }
                    else
                    {
                        break;
                    }
                }
                spr_order[j] = key_idx;
            }
        }
    }

    for (i = 0; i < spr_count; i++)
    {
        int idx = (int)spr_order[i];
        Saturn_Vdp1Spr_Add(spr_queue[idx].scache,
                           spr_queue[idx].xA, spr_queue[idx].xB,
                           spr_queue[idx].yt, spr_queue[idx].yb,
                           0, spr_queue[idx].h,
                           spr_queue[idx].shade, 0);
    }
}

static HW_NOINLINE HW_COLD_OS int HW_FastFlatSrca_Miss(int flatnum)
{
    /* v1_tex_get() applies flattranslation itself: pass the UNtranslated flatnum,
     * otherwise animated flats resolve to T(T(x)) and populate the wrong tag slot. */
    return Saturn_Vdp1_GetFlatSrca(flatnum);
}

HW_ALWAYS_INLINE int HW_FastFlatSrca(int flatnum)
{
    if (__builtin_expect((unsigned)flatnum >= (unsigned)numflats, 0))
        return 0;
    int f = flattranslation ? flattranslation[flatnum] : flatnum;
    if (__builtin_expect((unsigned)f >= (unsigned)numflats, 0))
        f = flatnum;
    extern v1_tag_t *saturn_v1_flat_tags;
    if (__builtin_expect(saturn_v1_flat_tags != NULL, 1))
    {
        unsigned short s = saturn_v1_flat_tags[f].srca;
        if (__builtin_expect(s != 0 && s != 0xFFFFu, 1))
            return (int)s;
    }
    return HW_FastFlatSrca_Miss(flatnum);
}

static HW_NOINLINE int HW_FastWallSrca(int texnum, int *tw, int *th, int *srca_mono, int *srca_64)
{
    if ((unsigned)texnum >= (unsigned)numtextures || texnum <= 0)
    {
        if (srca_mono) *srca_mono = 0;
        if (srca_64)   *srca_64   = 0;
        return 0;
    }
    int t = texturetranslation ? texturetranslation[texnum] : texnum;
    if ((unsigned)t >= (unsigned)numtextures)
        t = texnum;
    extern v1_tag_t *saturn_v1_wall_tags;
    if (saturn_v1_wall_tags)
    {
        unsigned short s = saturn_v1_wall_tags[t].srca;
        if (s != 0 && s != 0xFFFFu)
        {
            *tw = (int)saturn_v1_wall_tags[t].w8 << 3;
            *th = (int)saturn_v1_wall_tags[t].h;
            if (srca_mono) *srca_mono = (int)saturn_v1_wall_tags[t].srca_mono;
            if (srca_64)   *srca_64   = (int)saturn_v1_wall_tags[t].srca_64;
            return (int)s;
        }
    }
    int _tw = 0, _th = 0;
    int s = Saturn_Vdp1_GetTexSrca(texnum, &_tw, &_th); /* translates internally */
    *tw = _tw;
    *th = _th;
    if (srca_mono)
    {
        if (saturn_v1_wall_tags && saturn_v1_wall_tags[t].srca_mono)
            *srca_mono = (int)saturn_v1_wall_tags[t].srca_mono;
        else
            *srca_mono = s;
    }
    if (srca_64)
    {
        if (saturn_v1_wall_tags && saturn_v1_wall_tags[t].srca_64)
            *srca_64 = (int)saturn_v1_wall_tags[t].srca_64;
        else
            *srca_64 = s;
    }
    return s;
}

