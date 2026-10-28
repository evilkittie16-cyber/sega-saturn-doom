/* saturn_baked_flats.c - Pre-calculated pure VDP1 3D floor & ceiling quads
 *
 * Streams CD-baked pure 4-vertex quadrilaterals for floor and ceiling polygons
 * per level (cd/<MAP>.FLT) into High WRAM via jo_fs_read_file.
 */

#include <stddef.h>
#include <string.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "r_local.h"
#include "saturn_baked_flats.h"
#include "saturn_leaf.h"
#include "saturn_memmap.h"

#define WRAM_L_FLAT_ADDR  ((byte *)SATURN_WRAM_L_FLAT_ADDR)
#define WRAM_L_FLAT_CAP   SATURN_WRAM_L_FLAT_CAP

typedef struct {
    char     magic[4];          /* "FLT2" */
    uint16_t version;           /* 2 */
    uint16_t numsubsectors;
    uint32_t numquads;
    uint32_t numvertexes;
    uint32_t ofs_sub_q_first;
    uint32_t ofs_sub_q_count;
    uint32_t ofs_sub_v_first;
    uint32_t ofs_sub_v_count;
    uint32_t ofs_sub_bbox;
    uint32_t ofs_verts;
    uint32_t ofs_quads;
    uint32_t reserved[5];       /* [0] = ofs_vert_frac (seam fix), 0 if absent */
} saturn_flt2_header_t;

typedef struct {
    char     magic[4];          /* "FLAT" */
    uint16_t version;           /* 1 */
    uint16_t numsubsectors;
    uint32_t numquads;
    uint32_t ofs_first;
    uint32_t ofs_count;
    uint32_t ofs_bbox;
    uint32_t ofs_quads;
    uint32_t reserved;
} saturn_flt_header_t;

static byte                         *cur_blob          = NULL;
static const saturn_flt2_header_t   *cur_hdr2          = NULL;
const uint32_t                      *saturn_baked_flat_q_first   = NULL;
const uint16_t                      *saturn_baked_flat_q_count   = NULL;
const uint32_t                      *saturn_baked_flat_v_first   = NULL;
const uint16_t                      *saturn_baked_flat_v_count   = NULL;
#ifndef SATURN_WIN32
const uint16_t                      *saturn_baked_flat_lod1_q_count __attribute__((section(".data"))) = NULL;
const uint16_t                      *saturn_baked_flat_lod1_v_count __attribute__((section(".data"))) = NULL;
const uint32_t                      *saturn_baked_flat_lod1_q_first __attribute__((section(".data"))) = NULL;
const uint32_t                      *saturn_baked_flat_lod1_v_first __attribute__((section(".data"))) = NULL;
#else
const uint16_t                      *saturn_baked_flat_lod1_q_count = NULL;
const uint16_t                      *saturn_baked_flat_lod1_v_count = NULL;
const uint32_t                      *saturn_baked_flat_lod1_q_first = NULL;
const uint32_t                      *saturn_baked_flat_lod1_v_first = NULL;
#endif
const saturn_flat_bbox_t            *saturn_baked_flat_bboxes    = NULL;
const saturn_flat_vert_t            *saturn_baked_flat_verts     = NULL;
const saturn_flat_quad_idx_t        *saturn_baked_flat_quads     = NULL;
const uint8_t                       *saturn_baked_flat_vfrac     = NULL;
int                                  saturn_baked_flat_numsubsectors = 0;

#define cur_sub_q_first saturn_baked_flat_q_first
#define cur_sub_q_count saturn_baked_flat_q_count
#define cur_sub_v_first saturn_baked_flat_v_first
#define cur_sub_v_count saturn_baked_flat_v_count
#define cur_bbox        saturn_baked_flat_bboxes
#define cur_verts       saturn_baked_flat_verts
#define cur_quads_idx   saturn_baked_flat_quads
#define cur_vert_frac   saturn_baked_flat_vfrac
#define cur_numsubsectors saturn_baked_flat_numsubsectors

/* Version 1 fallback support if loaded */
static const saturn_flt_header_t    *cur_header1       = NULL;
static const uint16_t               *cur_first1        = NULL;
static const uint16_t               *cur_count1        = NULL;
static const saturn_flat_quad_t     *cur_quads1        = NULL;

volatile int dbg_parse_err = 0;

/* Geometry window bookkeeping (see saturn_memmap.h): walls are packed right
 * after the flats; the texture tier starts above saturn_wram_l_geom_end. */
uint8_t *saturn_wram_l_seg_addr = (uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_FLAT_OFF + SATURN_WRAM_L_FLAT_CAP);
uint8_t *saturn_wram_l_geom_end = (uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_FLAT_OFF + SATURN_WRAM_L_FLAT_CAP);

int Saturn_BakedFlats_Parse(void *data, unsigned len)
{
    cur_blob          = NULL;
    cur_hdr2          = NULL;
    cur_sub_q_first   = NULL;
    cur_sub_q_count   = NULL;
    cur_sub_v_first   = NULL;
    cur_sub_v_count   = NULL;
    saturn_baked_flat_lod1_q_first = NULL;
    saturn_baked_flat_lod1_q_count = NULL;
    saturn_baked_flat_lod1_v_first = NULL;
    saturn_baked_flat_lod1_v_count = NULL;
    cur_bbox          = NULL;
    cur_verts         = NULL;
    cur_quads_idx     = NULL;
    cur_header1       = NULL;
    cur_first1        = NULL;
    cur_count1        = NULL;
    cur_quads1        = NULL;
    cur_numsubsectors = 0;

    /* Until parsed: assume a full-cap blob so walls never land on flat bytes */
    saturn_wram_l_seg_addr = (uint8_t *)(SATURN_WRAM_L_BASE + SATURN_WRAM_L_FLAT_OFF + SATURN_WRAM_L_FLAT_CAP);
    saturn_wram_l_geom_end = saturn_wram_l_seg_addr;

    if (!data || len < 32)
    {
        dbg_parse_err = 1;
        return 0;
    }

    /* Access through fast SH-2 cached mirror for 1-cycle L1 cache hits */
#ifndef SATURN_WIN32
    /* CRITICAL FIX: Purge SH-2 L1 Cache from uncached Area 1 trampoline so fresh CD data in DRAM is visible */
    extern void _hw_purge_cache(void);
    ((void (*)(void))((uint32_t)_hw_purge_cache | 0x20000000u))();

    cur_blob = (byte *)((uint32_t)data & ~0x20000000u);
#else
    cur_blob = (byte *)data;
#endif

    /* Check FLT2 (version 2 indexed) */
    if (len < 64)
    {
        dbg_parse_err = 2;
        return 0;
    }

    if (cur_blob[0] != 'F' || cur_blob[1] != 'L' ||
        cur_blob[2] != 'T' || cur_blob[3] != '2')
    {
        dbg_parse_err = 100 + (int)cur_blob[0];
        return 0;
    }

#ifdef SATURN_WIN32
    {
        saturn_flt2_header_t *h = (saturn_flt2_header_t *)cur_blob;
        h->version = _byteswap_ushort(h->version);
        h->numsubsectors = _byteswap_ushort(h->numsubsectors);
        h->numquads = _byteswap_ulong(h->numquads);
        h->numvertexes = _byteswap_ulong(h->numvertexes);
        h->ofs_sub_q_first = _byteswap_ulong(h->ofs_sub_q_first);
        h->ofs_sub_q_count = _byteswap_ulong(h->ofs_sub_q_count);
        h->ofs_sub_v_first = _byteswap_ulong(h->ofs_sub_v_first);
        h->ofs_sub_v_count = _byteswap_ulong(h->ofs_sub_v_count);
        h->ofs_sub_bbox = _byteswap_ulong(h->ofs_sub_bbox);
        h->ofs_verts = _byteswap_ulong(h->ofs_verts);
        h->ofs_quads = _byteswap_ulong(h->ofs_quads);
        for (int r = 0; r < 5; r++)
            h->reserved[r] = _byteswap_ulong(h->reserved[r]);

        int nsub = h->numsubsectors;
        uint32_t *q_first = (uint32_t *)(cur_blob + h->ofs_sub_q_first);
        uint16_t *q_count = (uint16_t *)(cur_blob + h->ofs_sub_q_count);
        uint32_t *v_first = (uint32_t *)(cur_blob + h->ofs_sub_v_first);
        uint16_t *v_count = (uint16_t *)(cur_blob + h->ofs_sub_v_count);
        saturn_flat_bbox_t *bbox = (saturn_flat_bbox_t *)(cur_blob + h->ofs_sub_bbox);

        for (int i = 0; i < nsub; i++) {
            q_first[i] = _byteswap_ulong(q_first[i]);
            q_count[i] = _byteswap_ushort(q_count[i]);
            v_first[i] = _byteswap_ulong(v_first[i]);
            v_count[i] = _byteswap_ushort(v_count[i]);
            bbox[i].min_x = (int16_t)_byteswap_ushort((uint16_t)bbox[i].min_x);
            bbox[i].max_x = (int16_t)_byteswap_ushort((uint16_t)bbox[i].max_x);
            bbox[i].min_y = (int16_t)_byteswap_ushort((uint16_t)bbox[i].min_y);
            bbox[i].max_y = (int16_t)_byteswap_ushort((uint16_t)bbox[i].max_y);
        }

        if (h->reserved[1] >= 64 && h->reserved[1] < len) {
            uint32_t *l_q_first = (uint32_t *)(cur_blob + h->reserved[1]);
            uint16_t *l_q_count = (uint16_t *)(cur_blob + h->reserved[2]);
            uint32_t *l_v_first = (uint32_t *)(cur_blob + h->reserved[3]);
            uint16_t *l_v_count = (uint16_t *)(cur_blob + h->reserved[4]);
            for (int i = 0; i < nsub; i++) {
                l_q_first[i] = _byteswap_ulong(l_q_first[i]);
                l_q_count[i] = _byteswap_ushort(l_q_count[i]);
                l_v_first[i] = _byteswap_ulong(l_v_first[i]);
                l_v_count[i] = _byteswap_ushort(l_v_count[i]);
            }
        }

        saturn_flat_vert_t *verts = (saturn_flat_vert_t *)(cur_blob + h->ofs_verts);
        for (uint32_t i = 0; i < h->numvertexes; i++) {
            verts[i].x = (int16_t)_byteswap_ushort((uint16_t)verts[i].x);
            verts[i].y = (int16_t)_byteswap_ushort((uint16_t)verts[i].y);
        }

        saturn_flat_quad_idx_t *quads = (saturn_flat_quad_idx_t *)(cur_blob + h->ofs_quads);
        for (uint32_t i = 0; i < h->numquads; i++) {
            quads[i].v[0] = _byteswap_ushort(quads[i].v[0]);
            quads[i].v[1] = _byteswap_ushort(quads[i].v[1]);
            quads[i].v[2] = _byteswap_ushort(quads[i].v[2]);
            quads[i].v[3] = _byteswap_ushort(quads[i].v[3]);
        }
    }
#endif

    cur_hdr2 = (const saturn_flt2_header_t *)cur_blob;
    if (cur_hdr2->version != 2)
    {
        dbg_parse_err = 4;
        return 0;
    }

    cur_numsubsectors = (int)cur_hdr2->numsubsectors;
    cur_sub_q_first   = (const uint32_t *)(cur_blob + cur_hdr2->ofs_sub_q_first);
    cur_sub_q_count   = (const uint16_t *)(cur_blob + cur_hdr2->ofs_sub_q_count);
    cur_sub_v_first   = (const uint32_t *)(cur_blob + cur_hdr2->ofs_sub_v_first);
    cur_sub_v_count   = (const uint16_t *)(cur_blob + cur_hdr2->ofs_sub_v_count);
    cur_bbox          = (const saturn_flat_bbox_t *)(cur_blob + cur_hdr2->ofs_sub_bbox);
    cur_verts         = (const saturn_flat_vert_t *)(cur_blob + cur_hdr2->ofs_verts);
    cur_quads_idx     = (const saturn_flat_quad_idx_t *)(cur_blob + cur_hdr2->ofs_quads);

    if (cur_hdr2->reserved[1] >= 64 && cur_hdr2->reserved[1] < len)
    {
        saturn_baked_flat_lod1_q_first = (const uint32_t *)(cur_blob + cur_hdr2->reserved[1]);
        saturn_baked_flat_lod1_q_count = (const uint16_t *)(cur_blob + cur_hdr2->reserved[2]);
        saturn_baked_flat_lod1_v_first = (const uint32_t *)(cur_blob + cur_hdr2->reserved[3]);
        saturn_baked_flat_lod1_v_count = (const uint16_t *)(cur_blob + cur_hdr2->reserved[4]);
    }

    /* Seam fix: 1/16-unit vertex fractions. Files baked before the fix have
     * none; give them an all-zero array right after the blob (same region)
     * so the renderer's vertex loops stay branch-free. */
    {
        uint32_t nv = cur_hdr2->numvertexes;
        uint32_t of = cur_hdr2->reserved[0];
        uint32_t end = len;
        if (of >= 64 && of + nv <= len)
            cur_vert_frac = cur_blob + of;
        else
        {
            uint32_t zo = (len + 3u) & ~3u;
            if (zo + nv > WRAM_L_FLAT_CAP)
            {
                dbg_parse_err = 5;
                cur_blob = NULL;
                return 0;
            }
            memset(cur_blob + zo, 0, nv);
            cur_vert_frac = cur_blob + zo;
            end = zo + nv;
        }
        /* Walls go right after the flats (16-byte aligned) */
        saturn_wram_l_seg_addr = (uint8_t *)(((uint32_t)cur_blob + end + 15u) & ~15u);
        saturn_wram_l_geom_end = saturn_wram_l_seg_addr;
    }
    dbg_parse_err     = 99;
    return 1;

    /* Fallback: Version 1 unindexed */
    if (cur_blob[0] == 'F' && cur_blob[1] == 'L' &&
        cur_blob[2] == 'A' && cur_blob[3] == 'T')
    {
        cur_header1 = (const saturn_flt_header_t *)cur_blob;
        if (cur_header1->version == 1)
        {
            cur_numsubsectors = (int)cur_header1->numsubsectors;
            cur_first1        = (const uint16_t *)(cur_blob + cur_header1->ofs_first);
            cur_count1        = (const uint16_t *)(cur_blob + cur_header1->ofs_count);
            cur_bbox          = (const saturn_flat_bbox_t *)(cur_blob + cur_header1->ofs_bbox);
            cur_quads1        = (const saturn_flat_quad_t *)(cur_blob + cur_header1->ofs_quads);
            return 1;
        }
    }

    cur_blob = NULL;
    return 0;
}

void Saturn_BakedFlats_Init(const char *lumpname)
{
    char fname[16];
    int len = 0, k;

    /* Parse(NULL, 0) resets every cur_* pointer and the geometry-window
     * bookkeeping (same reset as before, without duplicating it here). */
    (void)Saturn_BakedFlats_Parse(NULL, 0);

    extern volatile int dbg_pak_stage;
    if (dbg_pak_stage == 0)
        dbg_pak_stage = 20;

    if (!lumpname)
        return;

    for (k = 0; k < 8 && lumpname[k] != 0; k++)
    {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z')
            c -= ('a' - 'A');
        fname[k] = c;
    }
    fname[k++] = '.';
    fname[k++] = 'F';
    fname[k++] = 'L';
    fname[k++] = 'T';
    fname[k]   = 0;

    len = Saturn_Leaf_BlobRead(WRAM_L_FLAT_ADDR, WRAM_L_FLAT_CAP, fname);
    if (len < 32)
    {
        if (dbg_pak_stage >= 20)
            dbg_pak_stage = 21;
        return;
    }

    if (dbg_pak_stage >= 20) dbg_pak_stage = 22;
    if (!Saturn_BakedFlats_Parse(WRAM_L_FLAT_ADDR, len))
    {
        if (dbg_pak_stage >= 20)
            dbg_pak_stage = 23;
        return;
    }
    if (dbg_pak_stage >= 20) dbg_pak_stage = 24;
}

int Saturn_BakedFlats_GetIndexed(int subnum,
                                 const saturn_flat_vert_t **out_verts, int *out_vcount,
                                 const saturn_flat_quad_idx_t **out_quads, int *out_qcount)
{
    if (!cur_blob || !cur_hdr2 || subnum < 0 || subnum >= cur_numsubsectors)
    {
        if (out_verts)  *out_verts = NULL;
        if (out_vcount) *out_vcount = 0;
        if (out_quads)  *out_quads = NULL;
        if (out_qcount) *out_qcount = 0;
        return 0;
    }

    int q_cnt = (int)cur_sub_q_count[subnum];
    int v_cnt = (int)cur_sub_v_count[subnum];
    if (q_cnt <= 0 || v_cnt <= 0)
    {
        if (out_verts)  *out_verts = NULL;
        if (out_vcount) *out_vcount = 0;
        if (out_quads)  *out_quads = NULL;
        if (out_qcount) *out_qcount = 0;
        return 0;
    }

    if (out_verts)  *out_verts = &cur_verts[cur_sub_v_first[subnum]];
    if (out_vcount) *out_vcount = v_cnt;
    if (out_quads)  *out_quads = &cur_quads_idx[cur_sub_q_first[subnum]];
    if (out_qcount) *out_qcount = q_cnt;
    return q_cnt;
}

const uint8_t *Saturn_BakedFlats_GetVertFrac(const saturn_flat_vert_t *verts)
{
    return cur_vert_frac + (verts - cur_verts);
}

int Saturn_BakedFlats_Get(int subnum, const saturn_flat_quad_t **out_quads)
{
    if (!cur_blob || !cur_header1 || subnum < 0 || subnum >= cur_numsubsectors)
    {
        *out_quads = NULL;
        return 0;
    }
    *out_quads = &cur_quads1[cur_first1[subnum]];
    return (int)cur_count1[subnum];
}

int Saturn_BakedFlats_GetBBox(int subnum, const saturn_flat_bbox_t **out_bbox)
{
    if (!cur_blob || subnum < 0 || subnum >= cur_numsubsectors || !cur_bbox)
    {
        *out_bbox = NULL;
        return 0;
    }
    *out_bbox = &cur_bbox[subnum];
    return 1;
}
