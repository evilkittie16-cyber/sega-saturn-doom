/* saturn_segs.c — Offline Potentially Visible Set (PVS1) loader & leaf lookup.
 *
 * The legacy software seg/column wall renderer was retired in favor of the
 * VDP1/VDP2 hardware renderer (saturn_hw_render.c). This module retains the
 * PVS1 binary parser (Saturn_Pvs_Parse), CD fallback loader (Saturn_PvsLevelLoad),
 * per-frame viewer leaf row lookup (Saturn_Pvs_GetRow), and HUD telemetry symbols.
 */

#include "doomdef.h"
#include "r_local.h"
#include "saturn_segs.h"

extern int Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname);

#ifdef SATURN_PVS
/* Level state: the blob's PVS row base + geometry.
 * The PVS blob itself lives in PU_LEVEL zone memory (freed on level teardown). */
static const byte    *pvs_blob     = NULL;
static unsigned short pvs_nssec    = 0;
static unsigned short pvs_rowbytes = 0;

/* Locate the viewer's subsector via standard BSP point-on-side walk. */
static int seg_pvs_locate(fixed_t x, fixed_t y)
{
    int nodenum = (int)numnodes - 1;

    while ((int16_t)nodenum >= 0)
    {
        node_t *bsp = &nodes[nodenum];
        nodenum = (int)bsp->children[R_PointOnSide(x, y, bsp)];
    }
    return (nodenum == -1) ? 0 : (nodenum & ~NF_SUBSECTOR);
}

static byte        pvs_row_cache[128] __attribute__((aligned(4)));
static int         pvs_cached_leaf = -1;
static const byte *pvs_cached_blob = NULL;

const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y)
{
    if (pvs_blob != NULL)
    {
        int leaf = seg_pvs_locate(x, y);
        if (leaf >= 0 && leaf < pvs_nssec)
        {
            const byte *src = pvs_blob + (unsigned)leaf * pvs_rowbytes;
            if (pvs_rowbytes <= sizeof(pvs_row_cache))
            {
                if (leaf != pvs_cached_leaf || pvs_blob != pvs_cached_blob)
                {
                    D_memcpy(pvs_row_cache, src, pvs_rowbytes);
                    pvs_cached_leaf = leaf;
                    pvs_cached_blob = pvs_blob;
                }
                return pvs_row_cache;
            }
            return src;
        }
    }
    return NULL;
}
#else
const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y)
{
    (void)x;
    (void)y;
    return NULL;
}
#endif /* SATURN_PVS */

int Saturn_Pvs_Parse(byte *blob, unsigned fsize)
{
#ifdef SATURN_PVS
    unsigned nssec, rowbytes, need;

    if (!blob || fsize <= 16)
        return 0;

    /* Header: 'PVS1' magic, u16 ver @4, u16 nssec @6, u16 rowbytes @8, u32 res @10 */
    if (blob[0] != 'P' || blob[1] != 'V' || blob[2] != 'S' || blob[3] != '1'
        || (((unsigned)blob[4] << 8) | blob[5]) != 1)
        return 0;

    nssec    = ((unsigned)blob[6] << 8) | blob[7];
    rowbytes = ((unsigned)blob[8] << 8) | blob[9];

    if (nssec != (unsigned)numsubsectors || rowbytes != ((nssec + 7) >> 3))
        return 0;

    need = 16u + nssec * rowbytes;
    if (need > (unsigned)fsize)
        return 0;

    pvs_blob     = blob + 16;
    pvs_nssec    = (unsigned short)nssec;
    pvs_rowbytes = (unsigned short)rowbytes;
    return 1;
#else
    (void)blob;
    (void)fsize;
    return 0;
#endif
}

void Saturn_PvsLevelLoad(const char *lumpname)
{
#ifdef SATURN_PVS
    pvs_blob     = NULL;
    pvs_nssec    = 0;
    pvs_rowbytes = 0;

    if (lumpname == NULL || netgame)
        return;

    {
        char  fname[16];
        int   k;
        byte *blob;
        int   fsize;

        for (k = 0; k < 8 && lumpname[k]; k++)
        {
            char c = lumpname[k];
            if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
            fname[k] = c;
        }
        fname[k++] = '.';
        fname[k++] = 'P';
        fname[k++] = 'V';
        fname[k++] = 'S';
        fname[k]   = '\0';

        /* Buffer is 128 KB in PU_LEVEL zone memory (max map PVS is E4M9 at 115 KB) */
        blob = (byte *)Z_Malloc(131072, PU_LEVEL);
        if (blob == NULL)
            return;
        fsize = Saturn_Leaf_BlobRead(blob, 131072, fname);
        if (fsize <= 16 || !Saturn_Pvs_Parse(blob, (unsigned)fsize))
        {
            Z_Free(blob);
            return;
        }
    }
#else
    (void)lumpname;
#endif
}

void Saturn_Ocram_State_Init(void)
{
}

/* --- Legacy HUD Telemetry Counters (referenced by i_video.c / saturn_vdp1.c) --- */
int                  segwalls_count      = 0;
int                  saturn_seg_caphits  = 0;
int                  saturn_v1w_skipcols = 0;
int                  saturn_v1w_skipdie  = 0;
int                  saturn_v1w_commits  = 0;
int                  saturn_v1w_kills    = 0;
int                  saturn_v1w_skipmid  = 0;
int                  saturn_v1w_rj[20];
int                  saturn_mmid_skip    = 0;
int                  saturn_mmid_paint   = 0;
int                  saturn_sw1_built    = 0;
int                  saturn_sw1_ok       = 0;
int                  saturn_sw1_paint    = 0;
volatile int         saturn_mm_fill_sec   = -1;
volatile int         saturn_mm_fill_light = -1;
volatile int         saturn_ls_bad        = 0;
volatile int         saturn_dbg_w28       = 0;
volatile int         saturn_dbg_w60       = 0;
int                  saturn_dbg_pairs     = 0;
volatile int         saturn_seg_bailed    = 0;
volatile unsigned int saturn_half_frt_acc  = 0;
volatile unsigned int saturn_build_frt_acc = 0;
int                  saturn_frt_macc     = 0;
int                  saturn_frt_paintmax = 0;
int                  saturn_frt_spr      = 0;
int                  saturn_frt_psite    = 0;
int                  saturn_frt_gapmax   = 0;
int                  saturn_frt_vwalls   = 0;
int                  saturn_frt_vfill    = 0;
int                  saturn_frt_vspr     = 0;
int                  saturn_frt_rcomp    = 0;
int                  saturn_frt_vwalk    = 0;
int                  saturn_frt_vfire    = 0;
