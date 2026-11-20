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

static int pak_read_stream(void *gfs, uint32_t target_sec, uint32_t *p_cur_sec, void *buf, uint32_t bytes)
{
    byte *dst = (byte *)buf;
    uint32_t remaining = bytes;
    int retry;

    if (!buf || bytes == 0)
        return 1;

    if (target_sec != *p_cur_sec)
    {
        if (!__jo_fs_seek_sector(gfs, (int)target_sec))
            return 0;
        *p_cur_sec = target_sec;
    }

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > 2048u) ? 2048u : remaining;

        for (retry = 10; __jo_fs_read_sectors(gfs, 1, (void *)pak_scratch, 2048) <= 0; --retry)
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
    void *gfs;
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
    fid = __jo_fs_get_file_id(fname);
    if (fid < 0)
    {
        dbg_pak_stage = 2;
        return 0;
    }

    gfs = __jo_fs_open(fid);
    if (gfs == JO_NULL)
    {
        dbg_pak_stage = 3;
        return 0;
    }

    /* Read Sector 0 (Header + TOC) into dedicated staging buffer */
    if (!pak_read_stream(gfs, 0, &cur_sec, scratch, 2048))
    {
        __jo_fs_close(gfs);
        dbg_pak_stage = 4;
        return 0;
    }

    /* Validate Magic "MPAK" and Version 1 */
    if (scratch[0] != 'M' || scratch[1] != 'P' || scratch[2] != 'A' || scratch[3] != 'K')
    {
        __jo_fs_close(gfs);
        dbg_pak_stage = 5;
        return 0;
    }
    if ((((unsigned)scratch[4] << 8) | scratch[5]) != 1)
    {
        __jo_fs_close(gfs);
        dbg_pak_stage = 6;
        return 0;
    }

    nsec = (int)(((unsigned)scratch[6] << 8) | scratch[7]);
    if (nsec <= 0 || nsec > SATURN_PAK_MAX_SECS)
    {
        __jo_fs_close(gfs);
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
        __jo_fs_close(gfs);
        dbg_pak_stage = 14;
        return 0;
    }
    if (sec_flat && sec_flat->byte_size > 0)
    {
        dbg_pak_stage = 9;
        if (!pak_read_stream(gfs, sec_flat->sec_offset, &cur_sec, WRAM_L_FLAT_ADDR, sec_flat->byte_size))
        {
            __jo_fs_close(gfs);
            dbg_pak_stage = 10;
            return 0;
        }
        dbg_pak_stage = 11;
        if (!Saturn_BakedFlats_Parse(WRAM_L_FLAT_ADDR, sec_flat->byte_size))
        {
            __jo_fs_close(gfs);
            dbg_pak_stage = 12;
            return 0;
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
    if (sec_tex && sec_tex->byte_size > 16)
    {
        /* Streamed (2026-11-20): header + directory first, Plan places each
         * texture, then every sector of the section is routed straight into
         * the planned buffers - no staging copy of the whole section. */
        uint32_t total = sec_tex->byte_size;
        if (pak_read_stream(gfs, sec_tex->sec_offset, &cur_sec, pak_scratch, 2048u > total ? total : 2048u))
        {
            unsigned count  = ((unsigned)pak_scratch[6] << 8) | pak_scratch[7];
            unsigned dirb   = 16u + count * 24u;
            unsigned dsec   = (dirb + 2047u) / 2048u;
            byte *tmp = (byte *)Z_Malloc((int)(dsec * 2048u + count * sizeof(saturn_tex_place_t) + 16), PU_STATIC);
            saturn_tex_place_t *pl = (saturn_tex_place_t *)(tmp + dsec * 2048u);
            pl = (saturn_tex_place_t *)(((uintptr_t)pl + 3u) & ~3u);
            if (tmp && dirb <= total &&
                pak_read_stream(gfs, sec_tex->sec_offset, &cur_sec, tmp, dirb))
            {
                int n = Saturn_BakedTex_Plan(tmp, dirb, total, pl, (int)count);
                /* route the section: sector s covers bytes [s*2048, s*2048+2048) */
                uint32_t nsec = (total + 2047u) / 2048u;
                uint32_t s0 = 0;
                int k = 0, ok = 1;
                /* first sector holding pixel data: everything before is directory */
                uint32_t first_pix = (n > 0) ? pl[0].ofs : total;
                for (int i = 1; i < n; i++)
                    if (pl[i].ofs < first_pix) first_pix = pl[i].ofs;
                s0 = first_pix / 2048u;
                if (n > 0 && __jo_fs_seek_sector(gfs, (int)(sec_tex->sec_offset + s0)))
                {
                    cur_sec = sec_tex->sec_offset + s0;
                    for (uint32_t s = s0; s < nsec && ok; s++)
                    {
                        int retry;
                        for (retry = 10; __jo_fs_read_sectors(gfs, 1, (void *)pak_scratch, 2048) <= 0; --retry)
                        {
                            if (!retry) { ok = 0; break; }
                            jo_spin_wait(16);
                        }
                        if (!ok) break;
                        cur_sec++;
                        uint32_t b0 = s * 2048u, b1 = b0 + 2048u;
                        /* entries are in ascending offset order (bake_textures.py) */
                        while (k < n && pl[k].ofs + pl[k].sz <= b0) k++;
                        for (int j = k; j < n && pl[j].ofs < b1; j++)
                        {
                            uint32_t lo = pl[j].ofs > b0 ? pl[j].ofs : b0;
                            uint32_t hi = pl[j].ofs + pl[j].sz < b1 ? pl[j].ofs + pl[j].sz : b1;
                            if (lo < hi)
                                D_memcpy(pl[j].dst + (lo - pl[j].ofs), pak_scratch + (lo - b0), hi - lo);
                        }
                    }
                }
                else
                    ok = 0;
                if (ok)
                    Saturn_BakedTex_Finish(pl, n);
            }
            if (tmp)
                Z_Free(tmp);
        }
    }

    /* 6. Stream MSPR rectangular sprite bitmaps into PU_LEVEL */
    /* 6a. Sprite pack bigger than what the zone can spare (retail E4M8,
     * 1.7 MB): start it in the cart arena's free tail after the textures
     * (whole sectors), continue in the zone. */
    if (sec_spr && sec_spr->byte_size > 0)
    {
        extern byte *Saturn_TexArenaFree(unsigned int *size);
        extern int Saturn_BakedSprites_Parse2(byte *blob, unsigned split, byte *blob2, unsigned fsize);
        unsigned int afree = 0;
        byte *atail = Saturn_TexArenaFree(&afree);
        unsigned int split = afree & ~2047u;
        if (atail && split >= 65536u)
        {
            uint32_t total = sec_spr->byte_size;
            if (split >= total)
            {
                if (pak_read_stream(gfs, sec_spr->sec_offset, &cur_sec, atail, total))
                    Saturn_BakedSprites_Parse2(atail, total, atail, total);
            }
            else
            {
                unsigned rest = total - split;
                byte *zpart = (byte *)Z_Malloc((rest + 2047) & ~2047, PU_LEVEL);
                if (zpart &&
                    pak_read_stream(gfs, sec_spr->sec_offset, &cur_sec, atail, split) &&
                    pak_read_stream(gfs, sec_spr->sec_offset + split / 2048u, &cur_sec, zpart, rest))
                {
                    if (!Saturn_BakedSprites_Parse2(atail, split, zpart, total))
                        Z_Free(zpart);
                }
                else if (zpart)
                    Z_Free(zpart);
            }
            sec_spr = NULL;     /* handled */
        }
    }
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

    __jo_fs_close(gfs);
    return 1;
}
