/* saturn_baked_segs.c - Zero-Runtime-Math Baked Walls & Seg Loader for Sega Saturn
 *
 * Streams CD-baked <=64-unit pre-subdivided segs (.WL / MWAL) into WRAM-L (SATURN_WRAM_L_SEG_ADDR)
 * and executes single-pass pointer swizzling (< 0.2 ms) to bind dynamic sector height pointers.
 */

#include <stddef.h>
#include <string.h>

#include "doomdef.h"
#include "r_local.h"
#include "saturn_baked_segs.h"
#include "saturn_leaf.h"
#include "saturn_memmap.h"

#define WRAM_L_SEG_CAP   SATURN_WRAM_L_SEG_CAP

/* Master Blueprint MWAL state */
int saturn_baked_walls_active = 0;
int saturn_baked_segs_active  = 0;
volatile int saturn_wall_parse_dbg = 0;
extern volatile int saturn_wall_dbg;

static byte                         *cur_wl_blob           = NULL;
static const saturn_wl_header_t     *cur_wl_hdr            = NULL;
const saturn_wl_subsector_t         *saturn_baked_wall_subsectors = NULL;
const saturn_wl_pillar_t            *saturn_baked_wall_pillars    = NULL;
const uint8_t                       *saturn_baked_wall_pillar_frac = NULL;
baked_seg_t                         *saturn_baked_wall_segs       = NULL;
int                                  saturn_baked_wall_num_subsectors = 0;
static int                           cur_wl_num_segs       = 0;

#define cur_wl_subsectors     saturn_baked_wall_subsectors
#define cur_wl_pillars        saturn_baked_wall_pillars
#define cur_wl_pillar_frac    saturn_baked_wall_pillar_frac
#define cur_swizzled_segs     saturn_baked_wall_segs
#define cur_wl_num_subsectors saturn_baked_wall_num_subsectors

#ifdef SATURN_WIN32
#include <stdlib.h> /* For _byteswap macros */
#endif

/* Legacy SEGS header */
typedef struct {
    char     magic[4];          /* "SEGS" */
    uint16_t version;           /* 1 */
    uint16_t numvertexes;
    uint32_t numsegs;
    uint32_t numsubsectors;
    uint32_t ofs_vertexes;
    uint32_t ofs_segs;
    uint32_t ofs_ssect_first;
    uint32_t reserved;
} saturn_seg_header_t;

/* Legacy parser fallback */
static int Saturn_BakedSegs_ParseLegacy(void *data, unsigned len)
{
    const saturn_seg_header_t *hdr;
    const uint16_t *ssect_first;
    int nss, i;

    saturn_baked_segs_active = 0;
    if (!data || len < sizeof(saturn_seg_header_t))
        return 0;

/* Performance Fix: Clear the uncached bit (0x20000000) to ensure 1-cycle L1 cache hits */
#ifndef SATURN_WIN32
    data = (void *)((uint32_t)data & ~0x20000000u); 
#endif
    
    hdr = (const saturn_seg_header_t *)data;

#ifdef SATURN_WIN32
    /* PC Endian Fix: Byte-swap the header so legacy fallback works on Windows */
    {
        saturn_seg_header_t *h = (saturn_seg_header_t *)data;
        h->version = _byteswap_ushort(h->version);
        h->numvertexes = _byteswap_ushort(h->numvertexes);
        h->numsegs = _byteswap_ulong(h->numsegs);
        h->numsubsectors = _byteswap_ulong(h->numsubsectors);
        h->ofs_vertexes = _byteswap_ulong(h->ofs_vertexes);
        h->ofs_segs = _byteswap_ulong(h->ofs_segs);
        h->ofs_ssect_first = _byteswap_ulong(h->ofs_ssect_first);
        
        uint16_t *sf = (uint16_t *)((byte *)data + h->ofs_ssect_first);
        for (i = 0; i < (int)h->numsubsectors; i++) {
            sf[i] = _byteswap_ushort(sf[i]);
        }
    }
#endif

    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'E' ||
        hdr->magic[2] != 'G' || hdr->magic[3] != 'S' ||
        hdr->version != 1)
        return 0;

    nss = (int)hdr->numsubsectors;
    if (nss != numsubsectors)
        return 0;

    numvertexes = (int)hdr->numvertexes;
    vertexes    = (mapvertex_t *)((byte *)data + hdr->ofs_vertexes);

    numsegs     = (int)hdr->numsegs;
    segs        = (seg_t *)((byte *)data + hdr->ofs_segs);

    ssect_first = (const uint16_t *)((byte *)data + hdr->ofs_ssect_first);
    
    /* Bug Fix: i < nss prevents fatal off-by-one heap corruption */
    for (i = 0; i < nss; i++) {
        subsectors[i].firstline = (int)ssect_first[i];
    }

    saturn_baked_segs_active = 1;
    return 1;
}

/* Master Blueprint MWAL Parser & Pointer Swizzler (Phase 2 Step 2.3) */
int Saturn_BakedWalls_Parse(void *data, unsigned len)
{
    const saturn_wl_header_t *hdr;
    const packed_seg_disc_t *disc_segs;
    baked_seg_t *dst_segs;
    uint32_t num_segs, i;

    saturn_baked_walls_active = 0;
    cur_wl_blob           = NULL;
    cur_wl_hdr            = NULL;
    cur_wl_subsectors     = NULL;
    cur_wl_pillars        = NULL;
    cur_wl_pillar_frac    = NULL;
    cur_swizzled_segs     = NULL;
    cur_wl_num_subsectors = 0;
    cur_wl_num_segs       = 0;

    saturn_wall_parse_dbg = 1;
    if (!data || len < sizeof(saturn_wl_header_t))
    {
        saturn_wall_parse_dbg = -1;
        return 0;
    }

#ifndef SATURN_WIN32
    /* CRITICAL FIX: Purge SH-2 L1 Cache from uncached Area 1 trampoline to guarantee visibility of CD-DMA'd data */
    extern void _hw_purge_cache(void);
    ((void (*)(void))((uint32_t)_hw_purge_cache | 0x20000000u))();

    /* Access through fast SH-2 cached mirror for 1-cycle hits */
    data = (void *)((uint32_t)data & ~0x20000000u);
#endif
    hdr = (const saturn_wl_header_t *)data;
    saturn_wram_l_geom_end = (uint8_t *)data + len;   /* blob is resident even if parse fails */

    if (hdr->magic[0] != 'M' || hdr->magic[1] != 'W' ||
        hdr->magic[2] != 'A' || hdr->magic[3] != 'L')
    {
        saturn_wall_parse_dbg = -2;
        return 0;
    }

#ifdef SATURN_WIN32
    /* In-place byteswapping for PC testing (external pointers read these natively) */
    {
        saturn_wl_header_t *h = (saturn_wl_header_t *)data;
        h->version = _byteswap_ushort(h->version);
        h->num_subsectors = _byteswap_ushort(h->num_subsectors);
        h->total_size = _byteswap_ulong(h->total_size);
        h->ofs_subsectors = _byteswap_ulong(h->ofs_subsectors);
        h->ofs_pillars = _byteswap_ulong(h->ofs_pillars);
        h->ofs_segs = _byteswap_ulong(h->ofs_segs);
        h->reserved[0] = _byteswap_ulong(h->reserved[0]);
        h->reserved[1] = _byteswap_ulong(h->reserved[1]);

        saturn_wl_subsector_t *subs = (saturn_wl_subsector_t *)((byte *)data + h->ofs_subsectors);
        for (int i = 0; i < (int)h->num_subsectors; i++) {
            subs[i].num_pillars = _byteswap_ushort(subs[i].num_pillars);
            subs[i].pillar_start = _byteswap_ushort(subs[i].pillar_start);
            subs[i].num_segs = _byteswap_ushort(subs[i].num_segs);
            subs[i].seg_start = _byteswap_ushort(subs[i].seg_start);
            subs[i].num_dsp_batches = _byteswap_ushort(subs[i].num_dsp_batches);
            subs[i].max_batch_size = _byteswap_ushort(subs[i].max_batch_size);
            subs[i].reserved = _byteswap_ulong(subs[i].reserved);
        }

        uint32_t of_frac = h->reserved[0];
        uint32_t pil_end = (of_frac > h->ofs_pillars && of_frac < h->ofs_segs) ? of_frac : h->ofs_segs;
        uint32_t num_pils = (pil_end - h->ofs_pillars) / sizeof(saturn_wl_pillar_t);
        saturn_wl_pillar_t *pils = (saturn_wl_pillar_t *)((byte *)data + h->ofs_pillars);
        for (uint32_t i = 0; i < num_pils; i++) {
            pils[i].x = (int16_t)_byteswap_ushort((uint16_t)pils[i].x);
            pils[i].z = (int16_t)_byteswap_ushort((uint16_t)pils[i].z);
        }

        uint32_t nsegs = (h->total_size - h->ofs_segs) / sizeof(packed_seg_disc_t);
        packed_seg_disc_t *psegs = (packed_seg_disc_t *)((byte *)data + h->ofs_segs);
        for (uint32_t i = 0; i < nsegs; i++) {
            for (int k = 0; k < 5; k++)
                psegs[i].pillar_idx[k] = _byteswap_ushort(psegs[i].pillar_idx[k]);
            psegs[i].tex_64 = _byteswap_ushort(psegs[i].tex_64);
            psegs[i].tex_32 = _byteswap_ushort(psegs[i].tex_32);
            psegs[i].tex_16 = _byteswap_ushort(psegs[i].tex_16);
            psegs[i].mid_static_v_offset = (int16_t)_byteswap_ushort((uint16_t)psegs[i].mid_static_v_offset);
            psegs[i].peg_sec = _byteswap_ushort(psegs[i].peg_sec);
            psegs[i].top_sec = _byteswap_ushort(psegs[i].top_sec);
            psegs[i].bot_sec = _byteswap_ushort(psegs[i].bot_sec);
            psegs[i].u_start = _byteswap_ushort(psegs[i].u_start);
            psegs[i].u_span = _byteswap_ushort(psegs[i].u_span);
            psegs[i].reserved = _byteswap_ushort(psegs[i].reserved);
        }
    }
#endif

    if (hdr->version != 1)
    {
        saturn_wall_parse_dbg = -3;
        return 0;
    }

    if (hdr->ofs_subsectors >= len || hdr->ofs_pillars >= len || hdr->ofs_segs >= len)
    {
        saturn_wall_parse_dbg = -4;
        return 0;
    }

    saturn_wall_parse_dbg = 2;

    cur_wl_blob           = (byte *)data;
    cur_wl_hdr            = hdr;
    cur_wl_num_subsectors = (int)hdr->num_subsectors;
    cur_wl_subsectors     = (const saturn_wl_subsector_t *)((const byte *)data + hdr->ofs_subsectors);
    cur_wl_pillars        = (const saturn_wl_pillar_t *)((const byte *)data + hdr->ofs_pillars);

    disc_segs = (const packed_seg_disc_t *)((const byte *)data + hdr->ofs_segs);
    num_segs = (hdr->total_size - hdr->ofs_segs) / sizeof(packed_seg_disc_t);
    cur_wl_num_segs = (int)num_segs;

    /* Swizzled segs reside directly in fast WRAM-L right after the disc blob */
    dst_segs = (baked_seg_t *)(((uint32_t)data + hdr->total_size + 15) & ~15);
    cur_swizzled_segs = dst_segs;

    /* Seam fix: 1/16-unit pillar fractions live between pillars and segs */
    {
        uint32_t of = hdr->reserved[0];
        uint8_t *zend = (uint8_t *)(dst_segs + num_segs);
        if (of > hdr->ofs_pillars && of < hdr->ofs_segs)
            cur_wl_pillar_frac = (const uint8_t *)data + of;
        else
        {
            uint32_t np = (hdr->ofs_segs - hdr->ofs_pillars) / sizeof(saturn_wl_pillar_t);
            
            /* CRITICAL FIX: Bounds check BEFORE memset to prevent memory overwrite */
            if ((uint32_t)((zend + np) - (uint8_t *)data) > WRAM_L_SEG_CAP)
            {
                saturn_wall_parse_dbg = -5;
                cur_swizzled_segs = NULL;
                return 0;
            }
            
            memset(zend, 0, np);
            cur_wl_pillar_frac = zend;
            zend += np;
        }
        
        if ((uint32_t)(zend - (uint8_t *)data) > WRAM_L_SEG_CAP)
        {
            saturn_wall_parse_dbg = -5;
            cur_swizzled_segs = NULL;
            return 0;
        }
        saturn_wram_l_geom_end = zend;   /* texture tier starts above this */
    }

    /* Swizzle 16-bit disc sector indices to live 32-bit RAM pointers (Trap 1 Resolution) */
    for (i = 0; i < num_segs; i++)
    {
        dst_segs[i].pillar_idx[0] = disc_segs[i].pillar_idx[0];
        dst_segs[i].pillar_idx[1] = disc_segs[i].pillar_idx[4]; /* Endpoint 1 (swizzled from disc p4) */

        dst_segs[i].baked_light = disc_segs[i].baked_light;
        dst_segs[i].flags       = disc_segs[i].flags;

        dst_segs[i].tex_64 = disc_segs[i].tex_64;
        dst_segs[i].pad0   = 0;

        dst_segs[i].mid_static_v_offset = disc_segs[i].mid_static_v_offset;
        dst_segs[i].u_start             = disc_segs[i].u_start;
        uint32_t us = disc_segs[i].u_span;
        dst_segs[i].u_span              = (uint16_t)us;
        
        /* PERFORMANCE FIX: Fast lookup/shift to avoid slow SH-2 division stall */
        dst_segs[i].inv_u_span = (us == 64) ? 1024 : 
                                 (us == 32) ? 2048 : 
                                 (us == 16) ? 4096 : 
                                 (us == 8)  ? 8192 : 
                                 (us > 0)   ? (uint16_t)(65536u / us) : 0;
                                 
        dst_segs[i].back_sec_idx        = -1;

        if (sectors && numsectors > 0)
        {
            if (disc_segs[i].flags & SEG_FLAG_UPPER)
            {
                uint16_t tsec = disc_segs[i].top_sec;
                uint16_t bsec = disc_segs[i].bot_sec;

                /* Standard DOOM sky-hack: suppress upper wall if both front and back have sky ceilings */
                if (tsec < (uint16_t)numsectors && bsec < (uint16_t)numsectors &&
                    (int8_t)sectors[tsec].ceilingpic == -1 &&
                    (int8_t)sectors[bsec].ceilingpic == -1)
                {
                    dst_segs[i].dynamic_top_z_ptr    = NULL;
                    dst_segs[i].dynamic_bottom_z_ptr = NULL;
                    dst_segs[i].back_sec_idx         = -1;
                    dst_segs[i].flags                = 0;
                    continue;
                }

                dst_segs[i].dynamic_top_z_ptr = (tsec < (uint16_t)numsectors)
                    ? &sectors[tsec].ceilingheight
                    : &sectors[0].ceilingheight;

                dst_segs[i].dynamic_bottom_z_ptr = (bsec < (uint16_t)numsectors)
                    ? &sectors[bsec].ceilingheight
                    : &sectors[0].ceilingheight;

                dst_segs[i].back_sec_idx = (bsec < (uint16_t)numsectors) ? (int16_t)bsec : -1;
            }
            else if (disc_segs[i].flags & SEG_FLAG_LOWER)
            {
                dst_segs[i].dynamic_top_z_ptr = (disc_segs[i].top_sec < (uint16_t)numsectors)
                    ? &sectors[disc_segs[i].top_sec].floorheight
                    : &sectors[0].floorheight;

                dst_segs[i].dynamic_bottom_z_ptr = (disc_segs[i].bot_sec < (uint16_t)numsectors)
                    ? &sectors[disc_segs[i].bot_sec].floorheight
                    : &sectors[0].floorheight;

                dst_segs[i].back_sec_idx = (disc_segs[i].top_sec < (uint16_t)numsectors) ? (int16_t)disc_segs[i].top_sec : -1;
            }
            else
            {
                dst_segs[i].dynamic_top_z_ptr = (disc_segs[i].top_sec < (uint16_t)numsectors)
                    ? &sectors[disc_segs[i].top_sec].ceilingheight
                    : &sectors[0].ceilingheight;

                dst_segs[i].dynamic_bottom_z_ptr = (disc_segs[i].bot_sec < (uint16_t)numsectors)
                    ? &sectors[disc_segs[i].bot_sec].floorheight
                    : &sectors[0].floorheight;

                dst_segs[i].back_sec_idx = (disc_segs[i].flags & SEG_FLAG_MID)
                    ? ((disc_segs[i].bot_sec < (uint16_t)numsectors) ? (int16_t)disc_segs[i].bot_sec : -1)
                    : -1;
            }

            if (disc_segs[i].peg_sec < (uint16_t)numsectors)
            {
                dst_segs[i].mid_peg_height_ptr = (disc_segs[i].flags & SEG_PEG_LOWER_UNPEGGED)
                    ? &sectors[disc_segs[i].peg_sec].ceilingheight
                    : ((disc_segs[i].flags & SEG_PEG_BOTTOM)
                        ? &sectors[disc_segs[i].peg_sec].floorheight
                        : &sectors[disc_segs[i].peg_sec].ceilingheight);
            }
            else
            {
                dst_segs[i].mid_peg_height_ptr = &sectors[0].ceilingheight;
            }
        }
        else
        {
            dst_segs[i].dynamic_top_z_ptr    = NULL;
            dst_segs[i].dynamic_bottom_z_ptr = NULL;
            dst_segs[i].mid_peg_height_ptr   = NULL;
        }
    }

    saturn_baked_walls_active = 1;
    saturn_wall_parse_dbg = 100 + (int)num_segs;
    return 1;
}

int Saturn_BakedSegs_Parse(void *data, unsigned len)
{
    if (!data || len < 4)
        return 0;

#ifndef SATURN_WIN32
    const char *magic = (const char *)((uint32_t)data & ~0x20000000u);
#else
    const char *magic = (const char *)data;
#endif
    if (magic[0] == 'M' && magic[1] == 'W' && magic[2] == 'A' && magic[3] == 'L')
    {
        return Saturn_BakedWalls_Parse(data, len);
    }
    return Saturn_BakedSegs_ParseLegacy(data, len);
}

void Saturn_BakedSegs_Init(const char *lumpname)
{
    char fname[16];
    int len = 0, k;

    saturn_baked_walls_active = 0;
    saturn_baked_segs_active  = 0;

    if (!lumpname)
        return;

    for (k = 0; k < 8 && lumpname[k] != 0; k++)
    {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z')
            c -= ('a' - 'A');
        fname[k] = c;
    }

    /* 1. Try <MAP>.WL (Master Blueprint format) */
    fname[k]   = '.';
    fname[k+1] = 'W';
    fname[k+2] = 'L';
    fname[k+3] = 0;

    saturn_wall_dbg = 50;
    /* FIX: Added void* cast to prevent compiler warnings */
    len = Saturn_Leaf_BlobRead((void *)WRAM_L_SEG_ADDR, WRAM_L_SEG_CAP, fname);
    if (len >= 32 && Saturn_BakedWalls_Parse((void *)WRAM_L_SEG_ADDR, len))
    {
        saturn_wall_dbg = 51;
        return;
    }
    saturn_wall_dbg = (len < 32) ? -50 : -51;

    /* 2. Fallback to <MAP>.SEG (Legacy format) */
    fname[k+1] = 'S';
    fname[k+2] = 'E';
    fname[k+3] = 'G';
    fname[k+4] = 0;

    len = Saturn_Leaf_BlobRead((void *)WRAM_L_SEG_ADDR, WRAM_L_SEG_CAP, fname);
    if (len >= 32)
    {
        saturn_wall_dbg = 52;
        Saturn_BakedSegs_ParseLegacy((void *)WRAM_L_SEG_ADDR, len);
    }
    else
    {
        saturn_wall_dbg = -52;
    }
}

int Saturn_BakedWalls_Active(void)
{
    return saturn_baked_walls_active;
}

int Saturn_BakedWalls_NumSubsectors(void)
{
    return cur_wl_num_subsectors;
}

const saturn_wl_subsector_t *Saturn_BakedWalls_GetSubsector(int ssec_idx)
{
    if (!saturn_baked_walls_active || ssec_idx < 0 || ssec_idx >= cur_wl_num_subsectors)
        return NULL;
    return &cur_wl_subsectors[ssec_idx];
}

const saturn_wl_pillar_t *Saturn_BakedWalls_GetPillars(void)
{
    return cur_wl_pillars;
}

const baked_seg_t *Saturn_BakedWalls_GetSegs(void)
{
    return cur_swizzled_segs;
}

const uint8_t *Saturn_BakedWalls_GetPillarFrac(void)
{
    return cur_wl_pillar_frac;
}
