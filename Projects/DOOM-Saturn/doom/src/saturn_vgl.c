/* saturn_vgl.c — Saturn-native patch graphics for the DOOM UI.
 *
 * The cd\DOOM.WAD is the Ultimate DOOM rerelease, so all UI art
 * (TITLEPIC, STBAR, STTNUM/STGNUM/STYSNUM digits, STKEYS, STF faces, STCFN
 * text font, M_* menu patches) is present in PC patch_t format. The
 * vendored d32xr UI files expect Jaguar jagobj_t lumps that this WAD
 * does not have, so the UI layers (saturn_hud.c / saturn_menu.c) draw
 * through THIS blitter instead.
 *
 * Patch format (verified byte-exact against this WAD by saturn_tex.c's
 * composite_texture, see its post-chain comment): little-endian header
 * { width, height, leftoffset, topoffset } shorts + columnofs[width]
 * int32s; per column a post chain { topdelta, length, pad, pixels[len],
 * pad }* terminated by topdelta 0xFF; pixels at post+3, advance
 * length+4; topdelta is ABSOLUTE (no tall-patch accumulation). Pixel
 * index 0 = transparent (skip, leave the underlying fb pixel).
 *
 * All reads are byte-wise: WAD lumps have no alignment guarantee and
 * SH-2 faults on misaligned word access.
 */
#include "saturn.h"
#include "i_video.h"

/* ---- little-endian readers (byte-wise, alignment-safe) ---- */
static int sat_rd16(const byte *p)
{
    return p[0] | (p[1] << 8);
}

static int sat_rd32(const byte *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((int)p[3] << 24);
}

/* ---- memoized lump cache ----
 * W_CacheLumpNum is a Z_Malloc + full read on EVERY call (MARS path),
 * so each named patch is cached exactly once. Faces and font glyphs
 * are named lazily (dozens of STF and STCFN lumps), hence the table.
 * Lump names are COPIED into the table: callers build names in stack
 * buffers ("STFST%d%d" and "STCFN%03d"). PU_STATIC = the cart-backed
 * 2 MB zone: the whole UI art set (~150 KB) is small next to a level. */
/* 96 slots: the HUD base art (~39) plus the on-demand face/key
 * variants fit with room; the 63-glyph HU font lives OUTSIDE this
 * cache (hu_glyph below) — it used to flood 63 of 128 slots through
 * Saturn_CachePatch. Shrinking here also pays the BSS bill that
 * keeps __bend under the 0x060C0000 ceiling. */
#define SAT_PATCH_CACHE 160

static char         sat_pc_name[SAT_PATCH_CACHE][9];
static const void  *sat_pc_ptr[SAT_PATCH_CACHE];
static int          sat_pc_n = 0;
static int          sat_pc_boot_n = 0;

static void hu_load(void);
static int  hu_loaded;

void Saturn_Vgl_MarkBootDone(void)
{
    if (!hu_loaded)
        hu_load();
    sat_pc_boot_n = sat_pc_n;
}

void Saturn_Vgl_ResetLevelPatches(void)
{
    int i;

    for (i = sat_pc_boot_n; i < sat_pc_n; i++)
    {
        if (sat_pc_ptr[i])
        {
            Z_Free((void *)sat_pc_ptr[i]);
            sat_pc_ptr[i] = 0;
        }
        sat_pc_name[i][0] = 0;
    }
    sat_pc_n = sat_pc_boot_n;
}

static void sat_name_copy(char *dst, const char *src)
{
    int i;

    for (i = 0; i < 8 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int sat_name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void *Saturn_CachePatch(const char *name)
{
    int i, lump;

    for (i = 0; i < sat_pc_n; i++)
        if (sat_name_eq(sat_pc_name[i], name))
            return (void *)sat_pc_ptr[i];

    lump = W_CheckNumForName(name);
    if (lump < 0)
        return 0;                   /* missing art: caller skips the draw */

    if (sat_pc_n < SAT_PATCH_CACHE)
    {
        sat_name_copy(sat_pc_name[sat_pc_n], name);
        sat_pc_ptr[sat_pc_n] = W_CacheLumpNum(lump, PU_STATIC);
        return (void *)sat_pc_ptr[sat_pc_n++];
    }

    /* Cache full: still correct (just re-reads) — do NOT fall back to
     * a non-PU_STATIC tag, the MARS W_CacheLumpNum I_Errors on it. */
    return W_CacheLumpNum(lump, PU_STATIC);
}

/* ---- big-patch cache: the sliced variant of W_CacheLumpNum ----
 * A whole-lump cache read runs as one sustained burst: N back-to-back
 * (GFS sector + masked cart write) chunks with no vblank between them.
 * Past ~20-28 consecutive chunks that wedges the console (the moving-
 * point r63-69 wedge): on Kronos every TITLEPIC boot (57 KB = 29
 * chunks) verdict-parked, while a boot with the read skipped ran to the
 * level, and slicing only the final memcpy still parked (the burst is
 * the pread's chunk train, not the copy — 2026-09-05 A/B/C pairs).
 * Small lumps stay on the normal path (PLAYPAL/COLORMAP/FONT boot
 * copies and the 37 KB directory read stay under the threshold).
 *
 * Mechanism: 16 KB slices read DIRECTLY into the zone through
 * saturn_iwad_pread — byte-identical to saturn_copy_to_cart (the fill
 * drain's real-HW-proven sync path) — with one vblank between slices,
 * so no A-bus write sequence ever survives a vblank. The offset is the
 * on-disc offset (COLD bias stripped): on-disc bytes are always
 * correct, whether or not the placement is resident in cart DRAM. */
#define SAT_BIG_SLICE   16384u

/* Diagnostic (2026-09-06 title stripes): the raw filepos the big patch
 * saw (cart offset for hot lumps, disc offset | COLD_BIAS for cold),
 * its length, and the RESOLVED on-disc offset the CD train would use —
 * the title screen prints all three. */
unsigned int saturn_bigpatch_base;
unsigned int saturn_bigpatch_len;
unsigned int saturn_bigpatch_cd;

void *Saturn_CachePatchBig(const char *name)
{
    extern volatile unsigned int saturn_vbl_count;
    extern int  Saturn_LumpResident(int lump,
                                    const unsigned char **cart_ptr_out,
                                    unsigned int *cd_off_out);
    extern int  saturn_iwad_pread(unsigned int pos, void *dest,
                                  unsigned int nbytes);

    int                 lump = W_CheckNumForName(name);
    wadfile_t          *wad  = &wadfile[0];  /* the rerelease DOOM.WAD */
    lumpinfo_t         *l;
    int                 len, v0;
    unsigned            base;
    byte               *mem;
    const unsigned char *csrc;
    unsigned            cd_off = 0;
    volatile unsigned int *vblp = &saturn_vbl_count;

    if (lump < 0)
        return 0;
    if (lump < wad->firstlump || lump >= wad->firstlump + wad->numlumps)
        return 0;                   /* not an IWAD lump: caller skips */
#ifdef SATURN_WIN32
    return W_CacheLumpNum(lump, PU_STATIC);
#endif
    l = &wad->lumpinfo[lump - wad->firstlump];
    len = l->size;                  /* BIGLONG is the identity on Saturn */
    base = (unsigned)l->filepos;    /* cart offset (HOT) or |COLD_BIAS */
    saturn_bigpatch_base = base;
    saturn_bigpatch_len  = (unsigned)len;

    if ((unsigned)len <= SAT_BIG_SLICE)
        return W_CacheLumpNum(lump, PU_STATIC);

    mem = (byte *)Z_Malloc(len + 1, PU_STATIC);
    v0  = (int)*vblp;

    /* SOURCE DISPATCH (2026-09-06 title stripes). The hybrid loader
     * rewrites filepos to CART offsets for hot lumps, so the old
     * "strip the bias and read it as a disc offset" logic read the
     * cart offset 0x054654 as a CD position and cached boot-directory
     * garbage (the 147-wide stripe block). Route through the same
     * hot/cold logic I_ReadCDFile uses: filled hot lumps slice-copy
     * from cart DRAM (no CD access at all); everything else streams
     * from the true on-disc offset. */
    if (Saturn_LumpResident(lump, &csrc, &cd_off))
    {
        /* HOT + filled: cart -> zone slice copy, one vblank between
         * 16 KB slices (the same A-bus pacing as the CD train). */
        unsigned got = 0;

        saturn_bigpatch_cd = cd_off;
        while ((unsigned)len - got >= SAT_BIG_SLICE)
        {
            D_memcpy(mem + got, csrc + got, SAT_BIG_SLICE);
            got += SAT_BIG_SLICE;
            while (*vblp == (unsigned)v0)
                ;
            v0 = (int)*vblp;
        }
        if (got < (unsigned)len)
            D_memcpy(mem + got, csrc + got, (unsigned)len - got);
        mem[len] = 0;
        return mem;
    }
    saturn_bigpatch_cd = cd_off;

    /* COLD / hot-unfilled: SEQUENTIAL SLICE TRAIN from the on-disc
     * offset. Every slice is addressed cd_off + got (got counts bytes
     * into the DESTINATION, so cd_off+got is always the matching disc
     * position): after the partial head sector cd_off+got is exactly
     * sector-aligned, so the stream is already at each slice's first
     * sector — pread seeks nothing and re-opens nothing. (The first
     * cut wrote s0*2048 + got and DROPPED the 892-byte within of
     * TITLEPIC: every slice landed 892 bytes early — convicted by the
     * M/T readout matching disc bytes at the shifted positions — and
     * each slice sat BEHIND the stream, re-opening the handle every
     * time. Backward seeks after a mid-session re-open are the second
     * title bug — see the PRIMER note in i_wad.c's saturn_iwad_pread.)
     * The vblank between slices bounds every A-bus write train to 8
     * consecutive cart chunks (the ~20-28-chunk wedge threshold). */
    {
        unsigned int within = cd_off % 2048u;
        unsigned int got    = 0;

        /* Partial head sector: exactly one sector, no overshoot. */
        if (within)
        {
            unsigned int take = 2048u - within;

            if (take > (unsigned)len)
                take = (unsigned)len;
            if (saturn_iwad_pread(cd_off, mem, take) != (int)take)
                return 0;
            got = take;
            while (*vblp == (unsigned)v0)
                ;
            v0 = (int)*vblp;
        }

        /* Sector-aligned 16 KB slices: 8 whole sectors each, and the
         * stream ends exactly at the next slice's first sector. */
        while ((unsigned)len - got >= SAT_BIG_SLICE)
        {
            if (saturn_iwad_pread(cd_off + got, mem + got,
                                  SAT_BIG_SLICE) != (int)SAT_BIG_SLICE)
                return 0;
            got += SAT_BIG_SLICE;
            while (*vblp == (unsigned)v0)
                ;
            v0 = (int)*vblp;
        }

        /* Sector-aligned tail: at most one partial sector. */
        if (got < (unsigned)len)
        {
            unsigned int take = (unsigned)len - got;

            if (saturn_iwad_pread(cd_off + got, mem + got,
                                  take) != (int)take)
                return 0;
        }
    }
    mem[len] = 0;
    return mem;
}

/* ---- the blitter ----
 * Draw patch at (x, y) into I_VideoBuffer, clipping rows to
 * [clip_top, clip_bot) and columns to [0, 320). x/y may be negative
 * (the clip handles it). No offset application: UI callers position
 * patches explicitly, matching linuxdoom's status-bar/menu drawers
 * which pass raw x/y.
 *
 * Per-column bound: columnofs is monotonic, so columnofs[col+1] ends
 * this column's data. The LAST column has no next offset — the chain
 * there is bounded only by its 0xFF terminator (reliable in id WADs;
 * saturn_tex.c leans on the same terminator). */
void Saturn_DrawPatch(int x, int y, const void *patch,
                      int clip_top, int clip_bot)
{
    const byte *p   = (const byte *)patch;
    int         w   = sat_rd16(p);
    int         h   = sat_rd16(p + 2);
    const byte *ofs = p + 8;
    int         col, guard;
    byte       *fb  = I_VideoBuffer;

    if (w < 1 || h < 1)
        return;

    for (col = 0; col < w; col++)
    {
        int         scx  = x + col;
        int         next = (col + 1 < w) ? sat_rd32(ofs + 4 * (col + 1)) : -1;
        const byte *post = p + sat_rd32(ofs + 4 * col);
        const byte *pend = (next >= 0) ? p + next : 0;

        if (scx < 0 || scx >= DOOM_SCREENWIDTH)
            continue;

        /* Posts partition the column's rows (len >= 1 each), so a chain
         * can never hold more than h posts — bounds the un-terminated
         * last column. */
        for (guard = h; guard > 0; guard--)
        {
            int top, len, k;
            const byte *px;

            if (pend && post + 3 > pend)
                break;
            top = post[0];
            len = post[1];
            if (top == 0xFF)
                break;
            if (len < 1 || (pend && post + 3 + len > pend))
                break;

            px = post + 3;
            for (k = 0; k < len; k++)
            {
                int sy = y + top + k;

                if (sy < clip_top || sy >= clip_bot)
                    continue;
                fb[sy * DOOM_SCREENWIDTH + scx] = px[k] ? px[k] : 247;
            }
            post += 4 + len;
        }
    }
}

/* Convenience: draw with the full-screen clip (menu/title/pause).
 * 2026-09-06: the fb is the full 224-line window (DOOM_FBHEIGHT) — the
 * status bar draws at rows 192..223, past the classic 200-row art
 * space. */
void Saturn_DrawPatchFS(int x, int y, const void *patch)
{
    Saturn_DrawPatch(x, y, patch, 0, DOOM_FBHEIGHT);
}

/* Nearest-neighbour downscaled blit: destination (dx, dy) samples the
 * source at (dx*den/num, dy*den/num). Same post-chain walk and bounds
 * as Saturn_DrawPatch — only the destination coordinate mapping
 * changes. Column/row collapsing (two source columns mapping to one
 * destination column) just overwrites, which is the look we want. */
void Saturn_DrawPatchScale(int x, int y, const void *patch,
                           int clip_top, int clip_bot, int num, int den)
{
    const byte *p   = (const byte *)patch;
    int         w   = sat_rd16(p);
    int         h   = sat_rd16(p + 2);
    const byte *ofs = p + 8;
    int         col, guard;
    byte       *fb  = I_VideoBuffer;

    if (w < 1 || h < 1 || num < 1 || den < 1)
        return;

    for (col = 0; col < w; col++)
    {
        int scx = (num == 3 && den == 4) ? (x + ((col * 3) >> 2)) : (x + (col * num) / den);
        int next = (col + 1 < w) ? sat_rd32(ofs + 4 * (col + 1)) : -1;
        const byte *post = p + sat_rd32(ofs + 4 * col);
        const byte *pend = (next >= 0) ? p + next : 0;

        if (scx < 0 || scx >= DOOM_SCREENWIDTH)
            continue;

        for (guard = h; guard > 0; guard--)
        {
            int top, len, k;
            const byte *px;

            if (pend && post + 3 > pend)
                break;
            top = post[0];
            len = post[1];
            if (top == 0xFF)
                break;
            if (len < 1 || (pend && post + 3 + len > pend))
                break;

            px = post + 3;
            if (num == 3 && den == 4)
            {
                for (k = 0; k < len; k++)
                {
                    int sy = y + (((top + k) * 3) >> 2);
                    if (sy >= clip_top && sy < clip_bot)
                        fb[sy * DOOM_SCREENWIDTH + scx] = px[k] ? px[k] : 247;
                }
            }
            else
            {
                for (k = 0; k < len; k++)
                {
                    int sy = y + ((top + k) * num) / den;
                    if (sy >= clip_top && sy < clip_bot)
                        fb[sy * DOOM_SCREENWIDTH + scx] = px[k] ? px[k] : 247;
                }
            }
            post += 4 + len;
        }
    }
}

/* Structure view: identical walk, but every non-zero texel writes the
 * forced index instead of the patch's palette index. */
void Saturn_DrawPatchSolid(int x, int y, const void *patch,
                           int clip_top, int clip_bot, byte idx)
{
    const byte *p   = (const byte *)patch;
    int         w   = sat_rd16(p);
    int         h   = sat_rd16(p + 2);
    const byte *ofs = p + 8;
    int         col, guard;
    byte       *fb  = I_VideoBuffer;

    if (w < 1 || h < 1)
        return;

    for (col = 0; col < w; col++)
    {
        int         scx  = x + col;
        int         next = (col + 1 < w) ? sat_rd32(ofs + 4 * (col + 1)) : -1;
        const byte *post = p + sat_rd32(ofs + 4 * col);
        const byte *pend = (next >= 0) ? p + next : 0;

        if (scx < 0 || scx >= DOOM_SCREENWIDTH)
            continue;

        for (guard = h; guard > 0; guard--)
        {
            int top, len, k;
            const byte *px;

            if (pend && post + 3 > pend)
                break;
            top = post[0];
            len = post[1];
            if (top == 0xFF)
                break;
            if (len < 1 || (pend && post + 3 + len > pend))
                break;

            px = post + 3;
            for (k = 0; k < len; k++)
            {
                int sy = y + top + k;

                if (sy < clip_top || sy >= clip_bot)
                    continue;
                if (px[k])
                    fb[sy * DOOM_SCREENWIDTH + scx] = idx;
            }
            post += 4 + len;
        }
    }
}

/* ---- text (the WAD's red HU font: STCFN033..STCFN095) ----
 * Uppercase, digits, punctuation. Unmapped characters are skipped.
 * Advance = glyph width + 1 (one blank column of spacing, as the PC
 * hu_lib does). */
/* ---- the HU font, outside the name cache ----
 * STCFN033-095 ('!'..'_'), lowercase folds onto uppercase. Direct-
 * indexed: per-frame message draws must not pay a 96-entry linear
 * name scan per glyph. Loaded on first use (lumps land in the zone
 * PU_STATIC and stay). */
static const void *hu_glyph[1 + 95 - 33];
static int         hu_loaded;

static void hu_load(void)
{
    int c;

    for (c = 33; c <= 95; c++)
    {
        char name[9] = "STCFN000";
        int  lump;

        name[5] = (char)('0' + (c / 100) % 10);
        name[6] = (char)('0' + (c / 10) % 10);
        name[7] = (char)('0' + c % 10);

        lump = W_CheckNumForName(name);
        hu_glyph[c - 33] = lump >= 0
                         ? W_CacheLumpNum(lump, PU_STATIC) : 0;
    }
    hu_loaded = 1;
}

void Saturn_Print(int x, int y, const char *s)
{
    unsigned char c;

    if (!hu_loaded)
        hu_load();

    for (; (c = (unsigned char)*s) != 0; s++)
    {
        const void *glyph;

        if (c == ' ')
        {
            x += 4;
            continue;
        }
        if (c >= 'a' && c <= 'z')   /* the font is uppercase-only */
            c -= 32;
        if (c < 33 || c > 95)       /* the font covers ! (33) .. _ (95) */
            continue;

        glyph = hu_glyph[c - 33];
        if (!glyph)
            continue;

        Saturn_DrawPatchFS(x, y, glyph);
        x += sat_rd16((const byte *)glyph) + 1;
        if (x >= DOOM_SCREENWIDTH)
            break;
    }
}

int Saturn_PrintWidth(const char *s)
{
    int            width = 0;
    unsigned char  c;

    if (!hu_loaded)
        hu_load();

    for (; (c = (unsigned char)*s) != 0; s++)
    {
        const byte *glyph;

        if (c == ' ')
        {
            width += 4;
            continue;
        }
        if (c >= 'a' && c <= 'z')   /* the font is uppercase-only */
            c -= 32;
        if (c < 33 || c > 95)
            continue;
        glyph = (const byte *)hu_glyph[c - 33];
        if (!glyph)
            continue;
        width += sat_rd16(glyph) + 1;
    }
    return width;
}

/* ---- solid fill (boxes behind menu text) ---- */
void Saturn_ClearRect(int x, int y, int w, int h, byte idx)
{
    int c0, c1, r0, r1, r, c;

    if (w < 1 || h < 1)
        return;
    c0 = x < 0 ? 0 : x;
    c1 = x + w > DOOM_SCREENWIDTH ? DOOM_SCREENWIDTH : x + w;
    r0 = y < 0 ? 0 : y;
    r1 = y + h > DOOM_FBHEIGHT ? DOOM_FBHEIGHT : y + h;
    if (c0 >= c1 || r0 >= r1)
        return;
    for (r = r0; r < r1; r++)
    {
        byte *d = I_VideoBuffer + r * DOOM_SCREENWIDTH + c0;

        for (c = c0; c < c1; c++)
            *d++ = idx;
    }
}