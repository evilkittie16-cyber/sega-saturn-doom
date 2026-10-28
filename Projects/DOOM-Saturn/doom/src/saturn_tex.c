/* saturn_tex.c — PC-WAD texture pipeline for the Saturn software
 * renderer.
 *
 * THE root cause of "the 3D rendering doesn't work" (found 2026-08-31):
 * d32xr's R_InitData is never called in the Saturn boot path (only the
 * empty R_InitTables stub), so numtextures/numflats stayed 0 and every
 * texturetranslation/flattextures lookup returned nothing — sidedef
 * texture numbers resolved to 0 and sector flats to -1. The renderer
 * had nothing to draw but flat colours.
 *
 * Why we can't just call d32xr's R_InitData/R_InitTextures verbatim:
 *   - R_InitData looks up the JAGUAR lump "PLAYPALS" (PC WADs ship
 *     "PLAYPAL") and d32xr colormaps that don't exist -> I_Error.
 *   - R_InitTextures requires T_START/T_END "replacement texture"
 *     markers that PC WADs don't have; with both missing it calls
 *     W_GetNameForNum(-2) -> I_Error (w_wad.c:521).
 *   - Its decal model reads PC mappatch_t.patch — a PNAMES index — as
 *     a TEXTURE NUMBER (Jaguar heritage), which both mis-composites
 *     and can overrun the decals[] allocation.
 *
 * Instead this file parses the PC structures directly:
 *   TEXTURE1  : count, int32 offset directory, then per record
 *              name[8], int masked, short width/height, 4-byte
 *              columndirectory, short patchcount, mappatch_t[patchcount]
 *              (mappatch = 5 x int16: originx, originy, patch,
 *              stepdir, colormap — `patch` is a PNAMES index on PC).
 *   PNAMES    : int32 count, then count x 8-byte patch NAMES, each
 *              resolved to a lump number with W_CheckNumForName at
 *              init time (4 + 8*count == lump size; this WAD's table
 *              has 351 entries and every one names a real lump —
 *              WALL00_3, DOOR2_1, ...). Reading the names as int32
 *              lump numbers was the "W_CacheLumpNum: <garbage>
 *              >= numlumps" red screen: 0x4C4C4157 is ASCII "WALL".
 *   patches   : int16 width/height/leftoffset/topoffset, int32
 *              columnofs[width], then per column a post chain of
 *              posts {topdelta, length, pad, pixels[length], pad}:
 *              pixels start 3 bytes in, the chain advances
 *              length+4 bytes per post (byte-verified against the
 *              id WAD), terminated by topdelta 0xFF. post[4]+len+5
 *              walks (the original code) drift the chain into pixel
 *              data — the wall-texture "stretch" bug.
 *   flats     : raw 64x64 8bpp, no header.
 *   COLORMAP  : 34 x 256 lightmap, index = shade*256 + texel.
 *
 * Endianness: every multi-byte field is read with LITTLESHORT/
 * LITTLELONG exactly like p_setup.c's map parsing — the same
 * disc->memory path, and the one that demonstrably decodes PC data
 * correctly on this build (this Makefile defines MARS, which turns
 * those macros into the byte-swapping branch).
 *
 * Everything is cached and composited lazily: only the textures E1M1's
 * sidedefs actually reference ever get built, so the one-time cost is
 * spread over the first frames. Since the dual-SH-2 split, the CACHE
 * lives in WRAM-L (see hram_alloc below) — the Z zone stayed on the
 * A-Bus cartridge as pure storage, and per-pixel reads by BOTH SH-2s
 * now hit the fast internal SCU bus instead of the cart.
 */
#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "r_local.h"
#include "saturn_tex.h"
#include "saturn_memmap.h"

/*----- PC binary field offsets (bytes) -----*/
#define TX1_COUNTDIR   4
#define MT_NAME        0
#define MT_MASKED      8
#define MT_WIDTH      12
#define MT_HEIGHT     14
#define MT_PCOUNT     20
#define MT_PATCHES    22
#define MP_OX          0
#define MP_OY          2
#define MP_PATCH      4
#define MP_SIZE       10

#define PT_WIDTH       0
#define PT_HEIGHT      2
#define PT_COLDEFS     8

static byte  *tex1_raw;
static int32_t *tex1_dir;
static int    numtextures1;
static byte  *tex2_raw;
static int32_t *tex2_dir;
static int    numtextures2;
static int    *tex_rec_off;
static byte  **tex_cache;
static byte   *tex_holes;

static int32_t *pnames;
static int     num_pnames;
static byte  **patch_cache;

static byte  **flat_cache;
static byte  *colormap;
static int    colormap_shades;
static int    skytexnum = -1;

static int tex_active;

typedef struct
{
    short w, h;
    short lo, to;
} sprhdr_t;

static byte   **sprite_cache;
static byte   **sprite_raw;
static sprhdr_t *sprite_hdr;

static byte   *sprite_botz;
static byte   *sprite_gguard;

int saturn_tex_comp_count = 0;
int saturn_tex_dirty = 1;

#define SATURN_HRAM_BASE   (SATURN_WRAM_L_BASE + 0x00080000u)
#define SATURN_HRAM_SIZE   0x00080000u
#define SATURN_RTBL_SIZE   0x10000u
#define SATURN_HWRAM_WANT  49152u

#ifndef SATURN_T0_OFF
#define SATURN_T0_OFF 1
#endif
#ifndef SATURN_T1_OFF
#define SATURN_T1_OFF 0
#endif

int saturn_tex_flat_miss = 0;
int saturn_tex_fill_bytes = 0;
int saturn_tex_comp_bytes = 0;

static const unsigned char saturn_cm_row0_exc[7] = { 168, 190, 191, 208, 224, 240, 247 };

static int saturn_cm_row0_ok(int i)
{
    int k;
    if (colormap[i] == (byte)i) return 1;
    for (k = 0; k < 7; k++)
        if (i == saturn_cm_row0_exc[k]) return 1;
    return 0;
}

volatile int saturn_cm_bad = 0;
volatile int saturn_cm_bad_boot_index = 255;
volatile int saturn_cm_bad_imin  = 255;
volatile int saturn_cm_bad_imax  = 0;
volatile int saturn_cm_bad_idist = 0;
volatile int saturn_cm_bad_val   = 0;
static unsigned char saturn_cm_badmap[32];

static byte         *hw_base;
static unsigned int  hw_used;
static unsigned int  hw_used_boot;
static unsigned int  hw_size;
static unsigned int  hram_used;
static int           hram_ok;

#ifndef SATURN_WIN32
extern void *jo_malloc_with_behaviour(unsigned int n, const int behaviour);
#endif

#ifdef SATURN_WIN32
static byte s_win32_hram[4 * 1024 * 1024];
static unsigned int s_win32_hram_used = 0;
static unsigned int s_win32_hram_used_boot = 0;
#endif

static void *hram_alloc(int size)
{
#ifdef SATURN_WIN32
    int adv;
    void *p;
    if (size <= 0) return NULL;
    adv = (size + 7) & ~7;
    if (s_win32_hram_used + (unsigned int)adv <= sizeof(s_win32_hram)) {
        p = &s_win32_hram[s_win32_hram_used];
        s_win32_hram_used += (unsigned int)adv;
        return p;
    }
    return NULL;
#else
    void *p;
    int   adv;

    if (size <= 0) return NULL;
    adv = (size + 7) & ~7; /* Ensure 8-byte alignment */

    if (hw_base && (int)hw_used + adv > 0 && hw_used + (unsigned int)adv <= hw_size)
    {
        p = hw_base + hw_used;
        hw_used += (unsigned int)adv;
        return p;
    }

    if (hram_ok)
    {
        const unsigned int fb_lo = SATURN_WRAM_L_BASE + SATURN_WRAM_L_SHADOW_FB_OFF - SATURN_HRAM_BASE;
        const unsigned int fb_hi = fb_lo + SATURN_WRAM_L_SHADOW_FB_SIZE;
        if (hram_used < fb_hi && hram_used + (unsigned int)adv > fb_lo)
            hram_used = fb_hi;
    }
    
    if (hram_ok && (int)hram_used + adv <= (int)(SATURN_HRAM_SIZE - SATURN_RTBL_SIZE))
    {
        p = (void *)(SATURN_HRAM_BASE + hram_used);
        hram_used += (unsigned int)adv;
        return p;
    }

    return NULL;
#endif
}

void *Saturn_HramAlloc(int size) { return hram_alloc(size); }

static unsigned int rtbl_used;

void *Saturn_RtblAlloc(int size)
{
    void *p;
    int   adv;
    if (size <= 0 || !hram_ok) return NULL;
    adv = (size + 7) & ~7;
    if (rtbl_used + (unsigned int)adv > SATURN_RTBL_SIZE) return NULL;
    p = (void *)(SATURN_HRAM_BASE + SATURN_HRAM_SIZE - SATURN_RTBL_SIZE + rtbl_used);
    rtbl_used += (unsigned int)adv;
    return p;
}

static void hram_probe(void)
{
    volatile byte *lo = (volatile byte *)SATURN_HRAM_BASE;
    volatile byte *hi = (volatile byte *)(SATURN_HRAM_BASE + SATURN_HRAM_SIZE - 8);
    unsigned int   want = SATURN_HWRAM_WANT;

    hram_used = 0;
#if !SATURN_T1_OFF
    lo[0] = 0x5C; lo[1] = 0xC5;
    hi[0] = 0xC3; hi[1] = 0x3C;
    hram_ok = (lo[0] == 0x5C && lo[1] == 0xC5 && hi[0] == 0xC3 && hi[1] == 0x3C);
#endif

#if !SATURN_T0_OFF
    while (want >= 8192u && hw_base == 0)
    {
        void *ptr = jo_malloc_with_behaviour(want + 8, 0); 
        if (ptr) {
            hw_base = (byte *)(((uintptr_t)ptr + 7) & ~7); /* Force boundary alignment safety */
            hw_size = want;
        } else {
            want >>= 1;
        }
    }
#endif
    (void)want;
}

/* Fast Unaligned Little-Endian Readers (Bypasses SH-2 Exception Handlers) */
static inline int rd16(const byte *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline int rd32(const byte *p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

/* Highly optimized 32-bit chunk replacement for pixel 0 -> 247 */
static void ReplaceZeroPixels(byte *data, int size)
{
    int i = 0;
    
    /* 32-bit aligned block processing (Significantly faster on SH-2) */
    if (((uintptr_t)data & 3) == 0) 
    {
        uint32_t *data32 = (uint32_t *)data;
        int size32 = size / 4;
        for (int j = 0; j < size32; j++) 
        {
            uint32_t val = data32[j];
            /* SWAR check: if any of the 4 bytes is zero */
            if ((val - 0x01010101u) & ~val & 0x80808080u) 
            {
                byte *p = (byte *)&data32[j];
                if (p[0] == 0) p[0] = 247;
                if (p[1] == 0) p[1] = 247;
                if (p[2] == 0) p[2] = 247;
                if (p[3] == 0) p[3] = 247;
            }
        }
        i = size32 * 4;
    }

    /* Process remaining unaligned tail */
    for (; i < size; i++) 
    {
        if (data[i] == 0) data[i] = 247;
    }
}

void Saturn_R_InitData(void)
{
    int lump, i;
    hram_probe();

    numtextures1 = 0; numtextures2 = 0;
    tex1_raw = NULL; tex2_raw = NULL;

    lump = W_CheckNumForName("TEXTURE1");
    int lump2 = W_CheckNumForName("TEXTURE2");
    int len1 = 0, len2 = 0;

    if (lump >= 0) {
        len1 = W_LumpLength(lump);
        tex1_raw = (byte *)W_CacheLumpNum(lump, PU_STATIC);
        if (tex1_raw && len1 > 4) {
            numtextures1 = rd32(tex1_raw);
            if (numtextures1 < 0) numtextures1 = 0;
            if (numtextures1 > 4096) numtextures1 = 4096;
        }
    }

    if (lump2 >= 0) {
        len2 = W_LumpLength(lump2);
        tex2_raw = (byte *)W_CacheLumpNum(lump2, PU_STATIC);
        if (tex2_raw && len2 > 4) {
            numtextures2 = rd32(tex2_raw);
            if (numtextures2 < 0) numtextures2 = 0;
            if (numtextures2 > 4096) numtextures2 = 4096;
        }
    }

    numtextures = numtextures1 + numtextures2;
    if (numtextures > 0)
    {
        textures = (texture_t *)Z_Malloc(numtextures * sizeof(texture_t), PU_STATIC);
        D_memset(textures, 0, numtextures * sizeof(texture_t));
        tex_rec_off = (int *)Z_Malloc(numtextures * sizeof(int), PU_STATIC);
        tex_cache = (byte **)Z_Malloc(numtextures * sizeof(byte *), PU_STATIC);
        D_memset(tex_cache, 0, numtextures * sizeof(byte *));
        tex_holes = (byte *)Z_Malloc(numtextures, PU_STATIC);
        D_memset(tex_holes, 0, numtextures);

        if (numtextures1 > 0)
        {
            tex1_dir = (int32_t *)(tex1_raw + TX1_COUNTDIR);
            for (i = 0; i < numtextures1; i++) {
                texture_t *tx = &textures[i];
                int off = rd32((byte *)&tex1_dir[i]);
                if (off < MT_PATCHES || off + MT_PATCHES > len1) off = MT_PATCHES;
                tex_rec_off[i] = off;
                byte *mt = tex1_raw + off;
                D_memcpy(tx->name, mt + MT_NAME, 8);
                for (int j = 0; j < 8; j++) {
                    if (tx->name[j] >= 'a' && tx->name[j] <= 'z') tx->name[j] -= ('a' - 'A');
                }
                tx->width = rd16(mt + MT_WIDTH); tx->height = rd16(mt + MT_HEIGHT);
                if (tx->width < 1) tx->width = 1; if (tx->width > 512) tx->width = 512;
                if (tx->height < 1) tx->height = 1; if (tx->height > 512) tx->height = 512;
                tx->lumpnum = -1; tx->decals = 0;
            }
        }

        if (numtextures2 > 0)
        {
            tex2_dir = (int32_t *)(tex2_raw + TX1_COUNTDIR);
            for (i = 0; i < numtextures2; i++) {
                int tidx = numtextures1 + i;
                texture_t *tx = &textures[tidx];
                int off = rd32((byte *)&tex2_dir[i]);
                if (off < MT_PATCHES || off + MT_PATCHES > len2) off = MT_PATCHES;
                tex_rec_off[tidx] = off;
                byte *mt = tex2_raw + off;
                D_memcpy(tx->name, mt + MT_NAME, 8);
                for (int j = 0; j < 8; j++) {
                    if (tx->name[j] >= 'a' && tx->name[j] <= 'z') tx->name[j] -= ('a' - 'A');
                }
                tx->width = rd16(mt + MT_WIDTH); tx->height = rd16(mt + MT_HEIGHT);
                if (tx->width < 1) tx->width = 1; if (tx->width > 512) tx->width = 512;
                if (tx->height < 1) tx->height = 1; if (tx->height > 512) tx->height = 512;
                tx->lumpnum = -1; tx->decals = 0;
            }
        }

        texturetranslation = (uint8_t *)Z_Malloc((numtextures + 1) * sizeof(uint8_t), PU_STATIC);
        for (i = 0; i < numtextures; i++) texturetranslation[i] = i;
    }
    numdecals = 0; decals = NULL; texmips = false;

    lump = W_CheckNumForName("PNAMES");
    if (lump >= 0) {
        byte *raw = (byte *)W_CacheLumpNum(lump, PU_STATIC);
        int n, len = W_LumpLength(lump);
        if (raw && len > 4) {
            n = rd32(raw);
            if (n < 0) n = 0;
            if (n > 1024) n = 1024;
            if (4 + 8 * n > len) n = (len - 4) / 8;
            num_pnames = n;
            pnames = (int32_t *)Z_Malloc(num_pnames * sizeof(int32_t), PU_STATIC);
            for (i = 0; i < num_pnames; i++) {
                char nm[9];
                D_memcpy(nm, raw + 4 + 8 * i, 8); nm[8] = 0;
                pnames[i] = W_CheckNumForName(nm);
            }
            patch_cache = (byte **)Z_Malloc(num_pnames * sizeof(byte *), PU_STATIC);
            D_memset(patch_cache, 0, num_pnames * sizeof(byte *));
        }
    }

    R_InitFlats();
    if (numflats > 0) {
        flat_cache = (byte **)Z_Malloc(numflats * sizeof(byte *), PU_STATIC);
        D_memset(flat_cache, 0, numflats * sizeof(byte *));
    }

    lump = W_CheckNumForName("COLORMAP");
    if (lump >= 0) {
        int len = W_LumpLength(lump);
        colormap = (byte *)hram_alloc(len);
        if (colormap) W_ReadLump(lump, colormap);
        else colormap = (byte *)W_CacheLumpNum(lump, PU_STATIC);
        if (colormap) {
            colormap_shades = len / 256;
            if (colormap_shades < 1) colormap_shades = 1;
            if (colormap_shades > 34) colormap_shades = 34;
        }
        for (i = 0; i < 256; i++) {
            if (!saturn_cm_row0_ok(i)) {
                saturn_cm_bad++;
                saturn_cm_bad_boot_index = i;
            }
        }
    }

    skytexnum = R_CheckTextureNumForName("SKY1");

    sprites = (spritedef_t *)Z_Malloc(sizeof(*sprites) * NUMSPRITES, PU_STATIC);
    D_memset(sprites, 0, sizeof(*sprites) * NUMSPRITES);
    lump = W_GetNumForName("S_START");
    firstsprite = lump + 1;
    numsprites = W_GetNumForName("S_END") - firstsprite;
    if (numsprites < 0) numsprites = 0;
    if (numsprites > 0) R_InitSpriteDefs((const char **)sprnames);

    sprite_cache = (byte **)Z_Malloc(numsprites * sizeof(byte *), PU_STATIC);
    sprite_raw = (byte **)Z_Malloc(numsprites * sizeof(byte *), PU_STATIC);
    sprite_hdr = (sprhdr_t *)Z_Malloc(numsprites * sizeof(sprhdr_t), PU_STATIC);
    sprite_botz = (byte *)Z_Malloc(numsprites, PU_STATIC);
    sprite_gguard = (byte *)Z_Malloc(numsprites, PU_STATIC);
    
    D_memset(sprite_cache, 0, numsprites * sizeof(byte *));
    D_memset(sprite_raw, 0, numsprites * sizeof(byte *));
    D_memset(sprite_hdr, 0, numsprites * sizeof(sprhdr_t));
    D_memset(sprite_botz, 0, numsprites);
    D_memset(sprite_gguard, 0, numsprites);

    hw_used_boot = hw_used;
#ifdef SATURN_WIN32
    s_win32_hram_used_boot = s_win32_hram_used;
#endif
    tex_active = (numtextures > 0);
}

int Saturn_TexActive(void) { return tex_active; }

static byte *get_patch(int pindex, int *plen)
{
    if (pindex < 0 || pindex >= num_pnames) return NULL;
    if (patch_cache[pindex]) {
        if (plen) *plen = W_LumpLength(pnames[pindex]);
        return patch_cache[pindex];
    }
    int lump = pnames[pindex];
    if (lump < 0) return NULL;
    byte *raw = (byte *)W_CacheLumpNum(lump, PU_LEVEL);
    saturn_tex_fill_bytes += W_LumpLength(lump);
    patch_cache[pindex] = raw;
    if (plen) *plen = W_LumpLength(lump);
    return raw;
}

static void composite_texture(int texnum)
{
    if (texnum < 0 || texnum >= numtextures) return;
    
    /* Memory Leak Fix: Do not double-allocate if already in cache */
    if (tex_cache[texnum]) return;

    texture_t *tx = &textures[texnum];
    byte *mt = (texnum < numtextures1) ? (tex1_raw + tex_rec_off[texnum]) : (tex2_raw + tex_rec_off[texnum]);
    
    int w = tx->width, h = tx->height;
    byte *dst;
    int pcount, j;

    saturn_tex_comp_count++;
    saturn_tex_comp_bytes += w * h;
    saturn_tex_dirty = 1;

    dst = (byte *)hram_alloc(w * h);
    if (!dst) dst = (byte *)Z_Malloc(w * h, PU_LEVEL);
    D_memset(dst, 0, w * h);
    tex_cache[texnum] = dst;

    pcount = rd16(mt + MT_PCOUNT);
    if (pcount < 0) pcount = 0;
    if (pcount > 256) pcount = 256;

    for (j = 0; j < pcount; j++)
    {
        byte *mp = mt + MT_PATCHES + MP_SIZE * j;
        int ox = rd16(mp + MP_OX);
        int oy = rd16(mp + MP_OY);
        int pindex = rd16(mp + MP_PATCH);
        int plen = 0;

        byte *patch = get_patch(pindex, &plen);
        if (!patch || plen < PT_COLDEFS) continue;

        int pw = rd16(patch + PT_WIDTH);
        int ph = rd16(patch + PT_HEIGHT);
        if (pw < 1 || pw > 1024 || ph < 1) continue;

        for (int c = 0; c < pw; c++)
        {
            int texcol = ox + c;
            if (texcol < 0 || texcol >= w) continue;
            
            /* Buffer Over-Read Fix: Clamp invalid column arrays */
            if (PT_COLDEFS + 4 * c + 3 >= plen) break;
            
            int coloff = rd32(patch + PT_COLDEFS + 4 * c);
            if (coloff < 0 || coloff >= plen) continue;
            
            byte *post = patch + coloff;
            byte *patch_end = patch + plen;

            while (post + 3 <= patch_end)
            {
                int top = post[0];
                int len = post[1];
                if (top == 0xFF) break;

                /* Bound check to prevent overflow / out of bounds read */
                if (len < 1 || len > (patch_end - (post + 3))) break;

                byte *px = post + 3;
                
                /* Multiplication elimination (strength reduction) */
                int ty = oy + top;
                int offset = ty * w + texcol;
                for (int k = 0; k < len; k++)
                {
                    if (ty >= 0 && ty < h)
                        dst[offset] = px[k] ? px[k] : 247;
                    ty++;
                    offset += w;
                }
                post += 4 + len;
            }
        }
    }

    /* Fast 32-bit hole detection (replaces byte-wise loop) */
    {
        int n = w * h;
        int n32 = n / 4;
        uint32_t *dst32 = (uint32_t *)dst;
        int has_holes = 0;

        for (int k = 0; k < n32; k++)
        {
            uint32_t val = dst32[k];
            if ((val - 0x01010101u) & ~val & 0x80808080u)
            {
                has_holes = 1;
                break;
            }
        }
        if (!has_holes) 
        {
            for (int k = n32 * 4; k < n; k++) 
            {
                if (dst[k] == 0) {
                    has_holes = 1;
                    break;
                }
            }
        }
        tex_holes[texnum] = has_holes;
    }
}

int Saturn_TexHasHoles(int texnum)
{
    if (!tex_active || texnum < 0 || texnum >= numtextures || !tex_holes || !tex_cache[texnum])
        return 1;
    return tex_holes[texnum];
}

volatile int saturn_cm_bad_live  = 0;
volatile int saturn_cm_bad_index = 255;
volatile int saturn_cm_bad_count = 0;

void Saturn_ColormapPoll(void)
{
    if (!colormap) return;
    int count = 0, first = 255;
    for (int i = 0; i < 256; i++) {
        if (!saturn_cm_row0_ok(i)) {
            count++;
            if (first == 255) first = i;
            if (!(saturn_cm_badmap[i >> 3] & (1 << (i & 7)))) {
                saturn_cm_badmap[i >> 3] |= (1 << (i & 7));
                saturn_cm_bad_idist++;
            }
        }
    }
    if (count) {
        saturn_cm_bad_live += count;
        saturn_cm_bad_index = first;
        saturn_cm_bad_count = count;
        saturn_cm_bad_val   = colormap[first];
        saturn_cm_bad_imin = 255; saturn_cm_bad_imax = 0;
        for (int i = 0; i < 256; i++) {
            if (saturn_cm_badmap[i >> 3] & (1 << (i & 7))) {
                if (i < saturn_cm_bad_imin) saturn_cm_bad_imin = i;
                if (i > saturn_cm_bad_imax) saturn_cm_bad_imax = i;
            }
        }
    }
}

const unsigned char *Saturn_ColormapBadmap(void) { return saturn_cm_badmap; }

byte *Saturn_TexData(int texnum)
{
    if (!tex_active || texnum < 0 || texnum >= numtextures) return NULL;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return NULL;
    if (!tex_cache[t]) composite_texture(t);
    return tex_cache[t];
}

byte *Saturn_TexDataNoLoad(int texnum)
{
    if (!tex_active || texnum < 0 || texnum >= numtextures) return NULL;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return NULL;
    return tex_cache[t];
}

static int saturn_tex_info_cached(int t, stexinfo_t *out)
{
    texture_t *tx = &textures[t];
    int w = tx->width, h = tx->height;
    out->data  = tex_cache[t];
    out->w     = w; out->h     = h;
    out->pow2  = ((w & (w - 1)) == 0) && ((h & (h - 1)) == 0);
    out->wmask = out->pow2 ? w - 1 : 0; out->hmask = out->pow2 ? h - 1 : 0;
    return 1;
}

int Saturn_TexInfo(int texnum, stexinfo_t *out)
{
    if (!tex_active || !out || texnum < 0 || texnum >= numtextures) return 0;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return 0;
    if (!tex_cache[t]) composite_texture(t);
    if (!tex_cache[t]) return 0;
    return saturn_tex_info_cached(t, out);
}

int Saturn_TexInfoNoLoad(int texnum, stexinfo_t *out)
{
    if (!tex_active || !out || texnum < 0 || texnum >= numtextures) return 0;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return 0;
    if (!tex_cache[t]) return 0;
    return saturn_tex_info_cached(t, out);
}

int Saturn_TexWidth(int texnum)
{
    if (!tex_active || texnum < 0 || texnum >= numtextures) return 1;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return 1;
    return textures[t].width;
}

int Saturn_TexHeight(int texnum)
{
    if (!tex_active || texnum < 0 || texnum >= numtextures) return 1;
    int t = texturetranslation[texnum];
    if (t < 0 || t >= numtextures) return 1;
    return textures[t].height;
}

byte *Saturn_FlatData(int flatnum)
{
    extern flattex_t *flatpixels;
    if (flatnum < 0 || flatnum >= numflats) return NULL;
    int f = flattranslation[flatnum];
    if (f < 0 || f >= numflats) f = flatnum;
    
    if (!flat_cache[f])
    {
        if (flatpixels && flatpixels[f].data[0]) {
            flat_cache[f] = (byte *)flatpixels[f].data[0];
            return flat_cache[f];
        }

        saturn_tex_flat_miss++;
        saturn_tex_dirty = 1;
        int lump = firstflat + f;
        int len = W_LumpLength(lump);
        saturn_tex_fill_bytes += len;
        
        flat_cache[f] = (byte *)hram_alloc(len);
        if (flat_cache[f]) W_ReadLump(lump, flat_cache[f]);
        else flat_cache[f] = (byte *)W_CacheLumpNum(lump, PU_LEVEL);

        if (flat_cache[f] && len > 0) 
            ReplaceZeroPixels(flat_cache[f], len);
    }
    return flat_cache[f];
}

byte *Saturn_FlatDataNoLoad(int flatnum)
{
    extern flattex_t *flatpixels;
    if (flatnum < 0 || flatnum >= numflats) return NULL;
    int f = flattranslation[flatnum];
    if (f < 0 || f >= numflats) f = flatnum;
    if (!flat_cache[f] && flatpixels && flatpixels[f].data[0])
        flat_cache[f] = (byte *)flatpixels[f].data[0];
    return flat_cache[f];
}

int Saturn_FlatActive(void) { return numflats > 0; }
byte *Saturn_Colormap(void) { return colormap; }
int Saturn_SkyTexNum(void) { return skytexnum; }

void Saturn_SetSkyTexture(const char *name)
{
    if (!name || !*name) name = "SKY1";
    skytexnum = R_CheckTextureNumForName((char *)name);
    if (skytexnum < 0) skytexnum = R_CheckTextureNumForName("SKY1");
}

byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs)
{
    if (s < 0 || s >= numsprites || !sprite_hdr) return NULL;
    sprhdr_t *hd = &sprite_hdr[s];
    if (sprite_cache && sprite_cache[s] && hd->w > 0) {
        *w = hd->w; *h = hd->h; *lofs = hd->lo; *tofs = hd->to;
        return sprite_cache[s];
    }
    if (!sprite_raw || !sprite_raw[s]) {
        int lump = firstsprite + s;
        int len = W_LumpLength(lump);
        byte *raw = (byte *)W_CacheLumpNum(lump, PU_LEVEL);
        if (!raw || len < PT_COLDEFS) return NULL;
        saturn_tex_fill_bytes += len;
        sprite_raw[s] = raw;
        hd->w = rd16(raw + PT_WIDTH); hd->h = rd16(raw + PT_HEIGHT);
        hd->lo = rd16(raw + 4); hd->to = rd16(raw + 6);
        if (hd->w < 1 || hd->w > 1024 || hd->h < 1 || hd->h > 1024) hd->w = hd->h = 0; 
    }
    if (hd->w < 1) return NULL;
    *w = hd->w; *h = hd->h; *lofs = hd->lo; *tofs = hd->to;
    return sprite_raw[s];
}

byte *Saturn_SpriteData(int s)
{
    if (!sprite_cache || s < 0 || s >= numsprites) return NULL;
    if (sprite_cache[s]) return sprite_cache[s];

    int w, h, lo, to;
    byte *raw = Saturn_SpritePatch(s, &w, &h, &lo, &to);
    if (!raw || w < 1 || h < 1) return NULL;
    int plen = W_LumpLength(firstsprite + s);

    byte *dst = (byte *)hram_alloc(w * h);
    if (!dst) dst = (byte *)Z_Malloc(w * h, PU_LEVEL);
    D_memset(dst, 0, w * h);

    for (int c = 0; c < w; c++)
    {
        if (PT_COLDEFS + 4 * c + 3 >= plen) break;
        int coloff = rd32(raw + PT_COLDEFS + 4 * c);
        if (coloff < PT_COLDEFS || coloff >= plen) continue;
        
        byte *post = raw + coloff;
        byte *pend = raw + plen;
        while (post + 3 <= pend)
        {
            int top = post[0];
            int len = post[1];
            if (top == 0xFF) break;
            if (len < 1 || len > (pend - (post + 3))) {
                sprite_gguard[s]++;
                break;
            }
            
            int ty = top;
            int offset = ty * w + c;
            for (int k = 0; k < len; k++)
            {
                if (ty >= 0 && ty < h)
                    dst[offset] = post[3 + k] ? post[3 + k] : 247;
                ty++;
                offset += w;
            }
            post += 4 + len;
        }
    }

    int botz = 0;
    for (int r = h - 1; r >= 0; r--) {
        int any = 0;
        for (int c2 = 0; c2 < w; c2++) {
            if (dst[r * w + c2]) { any = 1; break; }
        }
        if (!any) botz++;
        else break;
    }
    sprite_botz[s] = (byte)botz;

    sprite_cache[s] = dst;
    saturn_tex_dirty = 1;
    return dst;
}

byte *Saturn_SpriteDataNoLoad(int s)
{
    if (!sprite_cache || s < 0 || s >= numsprites) return NULL;
    return sprite_cache[s];
}

int Saturn_SpriteBotz(int s) { return (!sprite_botz || s < 0 || s >= numsprites) ? -1 : sprite_botz[s]; }
int Saturn_SpriteGuard(int s) { return (!sprite_gguard || s < 0 || s >= numsprites) ? -1 : sprite_gguard[s]; }
int Saturn_SpritePlen(int s) { return (s < 0 || s >= numsprites) ? -1 : W_LumpLength(firstsprite + s); }

void Saturn_CacheZoneReset(void)
{
    extern memzone_t *mainzone;
    if (!mainzone) return;
#ifdef SATURN_WIN32
    s_win32_hram_used = s_win32_hram_used_boot;
#else
    hw_used = hw_used_boot;
#endif
    hram_used = 0;
    if (tex_cache)
        D_memset(tex_cache, 0, numtextures * sizeof(byte *));
    if (flat_cache)
        D_memset(flat_cache, 0, numflats * sizeof(byte *));
    if (patch_cache)
        D_memset(patch_cache, 0, num_pnames * sizeof(byte *));
    if (sprite_cache)
        D_memset(sprite_cache, 0, numsprites * sizeof(byte *));
    if (sprite_raw)
        D_memset(sprite_raw, 0, numsprites * sizeof(byte *));
}

extern int Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname);

int Saturn_BakedTextures_Parse(byte *blob, unsigned fsize)
{
    if (!blob || fsize <= 16) return 0;
    if (blob[0] != 'M' || blob[1] != 'T' || blob[2] != 'E' || blob[3] != 'X' || (((unsigned)blob[4] << 8) | blob[5]) != 1) return 0;
    
    unsigned count = ((unsigned)blob[6] << 8) | blob[7];
    hram_used = 0;
    uint32_t ge = ((uint32_t)saturn_wram_l_geom_end + 7u) & ~7u;
    if (ge > SATURN_HRAM_BASE) hram_used = ge - SATURN_HRAM_BASE;
    if (tex_cache) D_memset(tex_cache, 0, numtextures * sizeof(byte *));

    for (unsigned i = 0; i < count; i++)
    {
        const byte *ent = blob + 16 + i * 24;
        unsigned int texnum = ((unsigned int)ent[0] << 8) | ent[1];
        unsigned int w      = ((unsigned int)ent[2] << 8) | ent[3];
        unsigned int h      = ((unsigned int)ent[4] << 8) | ent[5];
        unsigned int holes  = ent[6];
        unsigned int ofs    = ((unsigned int)ent[8] << 24) | ((unsigned int)ent[9] << 16) | ((unsigned int)ent[10] << 8) | (unsigned int)ent[11];
        unsigned int sz     = ((unsigned int)ent[12] << 24) | ((unsigned int)ent[13] << 16) | ((unsigned int)ent[14] << 8) | (unsigned int)ent[15];

        if (texnum < (unsigned int)numtextures && ofs + sz <= (unsigned int)fsize)
        {
            textures[texnum].width  = (short)w;
            textures[texnum].height = (short)h;
            if (tex_holes) tex_holes[texnum] = (byte)holes;

            byte *fast_dst = (byte *)hram_alloc((int)sz);
            if (fast_dst) {
                D_memcpy(fast_dst, blob + ofs, sz);
                tex_cache[texnum] = fast_dst;
            } else {
                byte *cart_dst = (byte *)Z_Malloc(sz, PU_LEVEL);
                if (cart_dst) {
                    D_memcpy(cart_dst, blob + ofs, sz);
                    tex_cache[texnum] = cart_dst;
                }
            }
            if (!holes && tex_cache[texnum]) {
                ReplaceZeroPixels(tex_cache[texnum], (int)sz);
            }
        }
    }
    saturn_tex_dirty = 1;
    return 1;
}

void Saturn_BakedTextures_Init(const char *lumpname)
{
    if (!lumpname || !tex_active) return;
    char fname[16];
    int k, fsize;
    
    for (k = 0; k < 8 && lumpname[k]; k++) {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        fname[k] = c;
    }
    fname[k++] = '.'; fname[k++] = 'T'; fname[k++] = 'E'; fname[k++] = 'X'; fname[k] = '\0';

    fsize = Saturn_Leaf_BlobRead(NULL, 0, fname);
    if (fsize < 16) return;

    extern void *Z_MallocHigh(int size, int tag);
    unsigned alloc_sz = (unsigned)((fsize + 2047) & ~2047);
    byte *temp_blob = (byte *)Z_MallocHigh(alloc_sz, PU_STATIC);
    if (!temp_blob) return;

    if (Saturn_Leaf_BlobRead(temp_blob, alloc_sz, fname) == fsize) {
        Saturn_BakedTextures_Parse(temp_blob, (unsigned)fsize);
    }
    Z_Free(temp_blob);
}

int Saturn_BakedSprites_Parse(byte *blob, unsigned fsize)
{
    if (!blob || fsize <= 16) return 0;
    if (blob[0] != 'M' || blob[1] != 'S' || blob[2] != 'P' || blob[3] != 'R' || (((unsigned)blob[4] << 8) | blob[5]) != 1) return 0;
    
    unsigned count = ((unsigned)blob[6] << 8) | blob[7];
    D_memset(sprite_cache, 0, numsprites * sizeof(byte *));

    for (unsigned i = 0; i < count; i++)
    {
        const byte *ent = blob + 16 + i * 32;
        unsigned int sidx = ((unsigned int)ent[0] << 8) | ent[1];
        unsigned int w    = ((unsigned int)ent[2] << 8) | ent[3];
        unsigned int h    = ((unsigned int)ent[4] << 8) | ent[5];
        short lo          = (short)(((unsigned int)ent[6] << 8) | ent[7]);
        short to          = (short)(((unsigned int)ent[8] << 8) | ent[9]);
        unsigned int ofs  = ((unsigned int)ent[12] << 24) | ((unsigned int)ent[13] << 16) | ((unsigned int)ent[14] << 8)  | (unsigned int)ent[15];
        unsigned int sz   = ((unsigned int)ent[16] << 24) | ((unsigned int)ent[17] << 16) | ((unsigned int)ent[18] << 8)  | (unsigned int)ent[19];

        if (sidx < (unsigned int)numsprites && ofs + sz <= (unsigned int)fsize)
        {
            sprite_hdr[sidx].w  = (short)w;
            sprite_hdr[sidx].h  = (short)h;
            sprite_hdr[sidx].lo = lo;
            sprite_hdr[sidx].to = to;
            sprite_cache[sidx]  = blob + ofs;
        }
    }
    saturn_tex_dirty = 1;
    return 1;
}

void Saturn_BakedSprites_Init(const char *lumpname)
{
    if (!lumpname || numsprites <= 0 || !sprite_cache) return;
    char fname[16];
    int k, fsize;
    
    for (k = 0; k < 8 && lumpname[k]; k++) {
        char c = lumpname[k];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        fname[k] = c;
    }
    fname[k++] = '.'; fname[k++] = 'S'; fname[k++] = 'P'; fname[k++] = 'R'; fname[k] = '\0';

    fsize = Saturn_Leaf_BlobRead(NULL, 0, fname);
    if (fsize < 16) return;

    unsigned alloc_sz = (unsigned)((fsize + 2047) & ~2047);
    byte *perm_blob = (byte *)Z_Malloc(alloc_sz, PU_LEVEL);
    if (!perm_blob) return;

    if (Saturn_Leaf_BlobRead(perm_blob, alloc_sz, fname) == fsize) {
        if (!Saturn_BakedSprites_Parse(perm_blob, (unsigned)fsize))
            Z_Free(perm_blob);
    } else {
        Z_Free(perm_blob);
    }
}
