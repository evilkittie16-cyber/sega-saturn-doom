/* saturn_pak.c - Consolidated level asset package loader for Sega Saturn
 *
 * Reads cd/<MAP>.PAK in a single sequential CD-ROM pass.
 * Unpacks flats, segs, PVS, leaf, textures, and sprites directly into their
 * target memory tiers (WRAM-L, Cart RAM, and Z Zone) with ZERO disc seeking.
 */

#include <stddef.h>
#include <string.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "r_local.h"
#include "saturn_baked_flats.h"
#include "saturn_baked_segs.h"
#include "saturn_leaf.h"
#include "saturn_tex.h"
#include "saturn_pak.h"
#include "saturn_memmap.h"

#define WRAM_L_FLAT_ADDR  ((byte *)SATURN_WRAM_L_FLAT_UNCACHED)
#define WRAM_L_SEG_ADDR   ((byte *)SATURN_WRAM_L_SEG_ADDR)

extern int Saturn_Pvs_Parse(byte *blob, unsigned fsize);
volatile int dbg_pak_stage = 0;
volatile int saturn_wall_dbg = 0;
static byte pak_scratch[2048] __attribute__((aligned(16)));

static int pak_read_stream(GfsHn gfs, uint32_t target_sec, uint32_t *p_cur_sec, void *buf, uint32_t bytes)
{
    byte *dst = (byte *)buf;
    uint32_t remaining = bytes;
    int retry;

    if (!buf || bytes == 0)
        return 1;

    if (target_sec > *p_cur_sec)
    {
        Sint32 delta = (Sint32)(target_sec - *p_cur_sec);
        int seek_ok = 0;
        for (retry = 5; retry > 0; --retry)
        {
            if (GFS_Seek(gfs, delta, GFS_SEEK_CUR) >= 0)
            {
                seek_ok = 1;
                break;
            }
            jo_spin_wait(16);
        }
        if (!seek_ok)
        {
            /* If relative seek is refused, walk forward with sequential reads */
            while (*p_cur_sec < target_sec)
            {
                for (retry = 10; GFS_Fread(gfs, 1, (void *)pak_scratch, 2048) <= 0; --retry)
                {
                    if (!retry)
                        return 0;
                    jo_spin_wait(16);
                }
                (*p_cur_sec)++;
            }
        }
        else
        {
            *p_cur_sec = target_sec;
        }
    }

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > 2048u) ? 2048u : remaining;

        for (retry = 10; GFS_Fread(gfs, 1, (void *)pak_scratch, 2048) <= 0; --retry)
        {
            if (!retry)
                return 0;
            jo_spin_wait(16);
        }

        D_memcpy(dst, pak_scratch, chunk);
        dst += chunk;
        remaining -= chunk;
        (*p_cur_sec)++;
    }
    return 1;
}

int Saturn_Pak_LevelLoad(const char *lumpname)
{
    char fname[16];
    int fid, k, i, nsec;
    GfsHn gfs;
    uint32_t cur_sec = 0;
    byte *scratch = pak_scratch;
    saturn_pak_section_t secs[SATURN_PAK_MAX_SECS];
    const saturn_pak_section_t *sec_flat = NULL;
    const saturn_pak_section_t *sec_wall = NULL;
    const saturn_pak_section_t *sec_segs = NULL;
    const saturn_pak_section_t *sec_pvs  = NULL;
    const saturn_pak_section_t *sec_leaf = NULL;
    const saturn_pak_section_t *sec_tex  = NULL;
    const saturn_pak_section_t *sec_spr  = NULL;

    if (!lumpname)
        return 0;

    for (k = 0; k < 8 && lumpname[k]; k++)
    {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        fname[k] = c;
    }
    fname[k++] = '.';
    fname[k++] = 'P';
    fname[k++] = 'A';
    fname[k++] = 'K';
    fname[k]   = '\0';

    dbg_pak_stage = 1;
    fid = GFS_NameToId((Sint8 *)fname);
    if (fid < 0)
    {
        dbg_pak_stage = 2;
        return 0;
    }

    gfs = GFS_Open(fid);
    if (gfs == JO_NULL)
    {
        dbg_pak_stage = 3;
        return 0;
    }

    GFS_SetTmode(gfs, GFS_TMODE_CPU);
    GFS_SetReadPara(gfs, 8);
    GFS_SetTransPara(gfs, 8);

    /* Read Sector 0 (Header + TOC) into dedicated staging buffer */
    if (!pak_read_stream(gfs, 0, &cur_sec, scratch, 2048))
    {
        GFS_Close(gfs);
        dbg_pak_stage = 4;
        return 0;
    }

    /* Validate Magic "MPAK" and Version 1 */
    if (scratch[0] != 'M' || scratch[1] != 'P' || scratch[2] != 'A' || scratch[3] != 'K')
    {
        GFS_Close(gfs);
        dbg_pak_stage = 5;
        return 0;
    }
    if ((((unsigned)scratch[4] << 8) | scratch[5]) != 1)
    {
        GFS_Close(gfs);
        dbg_pak_stage = 6;
        return 0;
    }

    nsec = (int)(((unsigned)scratch[6] << 8) | scratch[7]);
    if (nsec <= 0 || nsec > SATURN_PAK_MAX_SECS)
    {
        GFS_Close(gfs);
        dbg_pak_stage = 7;
        return 0;
    }

    /* Copy TOC records to stack so scratch can be safely overwritten */
    for (i = 0; i < nsec; i++)
    {
        const byte *e = scratch + 16 + i * 16;
        secs[i].name[0] = (char)e[0];
        secs[i].name[1] = (char)e[1];
        secs[i].name[2] = (char)e[2];
        secs[i].name[3] = (char)e[3];
        secs[i].sec_offset = ((uint32_t)e[4] << 24) | ((uint32_t)e[5] << 16)
                           | ((uint32_t)e[6] << 8)  | (uint32_t)e[7];
        secs[i].byte_size  = ((uint32_t)e[8] << 24) | ((uint32_t)e[9] << 16)
                           | ((uint32_t)e[10] << 8) | (uint32_t)e[11];

        if (memcmp(secs[i].name, "FLAT", 4) == 0) sec_flat = &secs[i];
        else if (memcmp(secs[i].name, "WALL", 4) == 0) sec_wall = &secs[i];
        else if (memcmp(secs[i].name, "SEGS", 4) == 0) sec_segs = &secs[i];
        else if (memcmp(secs[i].name, "PVS ", 4) == 0) sec_pvs  = &secs[i];
        else if (memcmp(secs[i].name, "LEAF", 4) == 0) sec_leaf = &secs[i];
        else if (memcmp(secs[i].name, "MTEX", 4) == 0) sec_tex  = &secs[i];
        else if (memcmp(secs[i].name, "MSPR", 4) == 0) sec_spr  = &secs[i];
    }

    /* 1. Stream FLAT quads directly into WRAM-L (SATURN_WRAM_L_FLAT_ADDR) */
    if (sec_flat && sec_flat->byte_size > SATURN_WRAM_L_FLAT_CAP)
    {
        /* Would overrun the SEG region (E1M8 did before the cap was raised) */
        GFS_Close(gfs);
        dbg_pak_stage = 14;
        return 0;
    }
    if (sec_flat && sec_flat->byte_size > 0)
    {
        dbg_pak_stage = 9;
        if (!pak_read_stream(gfs, sec_flat->sec_offset, &cur_sec, WRAM_L_FLAT_ADDR, sec_flat->byte_size))
        {
            GFS_Close(gfs);
            dbg_pak_stage = 10;
            return 0; /* Fall back to loose files if PAK flat load fails */
        }
        dbg_pak_stage = 11;
        if (!Saturn_BakedFlats_Parse(WRAM_L_FLAT_ADDR, sec_flat->byte_size))
        {
            GFS_Close(gfs);
            dbg_pak_stage = 12;
            return 0; /* Fall back to loose files if PAK flat load fails */
        }
        dbg_pak_stage = 13;
    }
    else
    {
        dbg_pak_stage = sec_flat ? 8 : 7;
    }

    /* 2. Stream WALL (Master Blueprint MWAL) or SEGS directly into WRAM-L (SATURN_WRAM_L_SEG_ADDR) */
    if (sec_wall && sec_wall->byte_size > SATURN_WRAM_L_SEG_CAP)
    {
        saturn_wall_dbg = -4;
    }
    else if (sec_wall && sec_wall->byte_size > 0)
    {
        saturn_wall_dbg = 1;
        if (pak_read_stream(gfs, sec_wall->sec_offset, &cur_sec, WRAM_L_SEG_ADDR, sec_wall->byte_size))
        {
            int r;
            saturn_wall_dbg = 2;
            r = Saturn_BakedSegs_Parse(WRAM_L_SEG_ADDR, sec_wall->byte_size);
            saturn_wall_dbg = r ? 3 : -3;
        }
        else
        {
            saturn_wall_dbg = -2;
        }
    }
    else if (sec_segs && sec_segs->byte_size > 0 && sec_segs->byte_size <= SATURN_WRAM_L_SEG_CAP)
    {
        saturn_wall_dbg = 10;
        if (pak_read_stream(gfs, sec_segs->sec_offset, &cur_sec, WRAM_L_SEG_ADDR, sec_segs->byte_size))
        {
            Saturn_BakedSegs_Parse(WRAM_L_SEG_ADDR, sec_segs->byte_size);
            saturn_wall_dbg = 11;
        }
        else
        {
            saturn_wall_dbg = -10;
        }
    }
    else
    {
        saturn_wall_dbg = -1;
    }

    /* 3. Stream PVS bitmasks into PU_LEVEL */
    if (sec_pvs && sec_pvs->byte_size > 0 && !netgame)
    {
        unsigned alloc_sz = (sec_pvs->byte_size + 2047) & ~2047;
        byte *blob = (byte *)Z_Malloc(alloc_sz, PU_LEVEL);
        if (blob)
        {
            if (pak_read_stream(gfs, sec_pvs->sec_offset, &cur_sec, blob, sec_pvs->byte_size))
            {
                if (!Saturn_Pvs_Parse(blob, sec_pvs->byte_size))
                    Z_Free(blob);
            }
            else
            {
                Z_Free(blob);
            }
        }
    }

    /* 4. Stream LEAF data into PU_LEVEL */
    if (sec_leaf && sec_leaf->byte_size > 0)
    {
        unsigned alloc_sz = (sec_leaf->byte_size + 2047) & ~2047;
        byte *blob = (byte *)Z_Malloc(alloc_sz, PU_LEVEL);
        if (blob)
        {
            if (pak_read_stream(gfs, sec_leaf->sec_offset, &cur_sec, blob, sec_leaf->byte_size))
            {
                if (!Saturn_Leaf_Parse(blob, (int)sec_leaf->byte_size))
                    Z_Free(blob);
            }
            else
            {
                Z_Free(blob);
            }
        }
    }

    /* 5. Stream MTEX composited textures into high-end PU_STATIC staging, copy to WRAM-L/Cart, and free */
    if (sec_tex && sec_tex->byte_size > 0)
    {
        extern void *Z_MallocHigh(int size, int tag);
        unsigned alloc_sz = (sec_tex->byte_size + 2047) & ~2047;
        byte *blob = (byte *)Z_MallocHigh(alloc_sz, PU_STATIC);
        if (blob)
        {
            if (pak_read_stream(gfs, sec_tex->sec_offset, &cur_sec, blob, sec_tex->byte_size))
                Saturn_BakedTextures_Parse(blob, sec_tex->byte_size);
            Z_Free(blob);
        }
    }

    /* 6. Stream MSPR rectangular sprite bitmaps into PU_LEVEL */
    if (sec_spr && sec_spr->byte_size > 0)
    {
        unsigned alloc_sz = (sec_spr->byte_size + 2047) & ~2047;
        byte *blob = (byte *)Z_Malloc(alloc_sz, PU_LEVEL);
        if (blob)
        {
            if (pak_read_stream(gfs, sec_spr->sec_offset, &cur_sec, blob, sec_spr->byte_size))
            {
                if (!Saturn_BakedSprites_Parse(blob, sec_spr->byte_size))
                    Z_Free(blob);
            }
            else
            {
                Z_Free(blob);
            }
        }
    }

    GFS_Close(gfs);
    return 1;
}
