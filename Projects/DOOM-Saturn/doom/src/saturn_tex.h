/* saturn_tex.h — PC-WAD texture/flat/colormap pipeline for the Saturn
 * software renderer (see saturn_tex.c). */
#ifndef SATURN_TEX_H
#define SATURN_TEX_H

#include "doomdef.h"

/* One-time init, called from D_DoomMain AFTER the WAD is loaded and
 * Z_Init has run, and BEFORE P_Init / P_SetupLevel: P_LoadSidedefs
 * resolves texture NAMES to numbers via R_TextureNumForName, and
 * P_InitSwitchList does the same for every switch pair. */
void Saturn_R_InitData(void);

/* Nonzero once TEXTURE1 parsed and textures[] is valid. */
int Saturn_TexActive(void);

/* Round 10: nonzero if texture `texnum`'s composite contains a zero
 * texel (a masked hole). Uncached/unusable textures answer nonzero —
 * conservative, sends the seg walk down the masked-mid path. */
int Saturn_TexHasHoles(int texnum);

/* Composited 8bpp pixels for texture `texnum` (a raw TEXTURE1 index,
 * NOT passed through texturetranslation). Composites lazily on first
 * use and caches for the life of the zone. NULL if unavailable. */
byte *Saturn_TexData(int texnum);
/* Phase 4: slave-SH-2-safe twins — same answers, but they NEVER run
 * the lazy composite / W_CacheLumpNum path (no lump I/O from the
 * slave). Callers on the slave core pass through these; an uncached
 * texture/flat comes back NULL and the span falls back for that
 * frame. */
byte *Saturn_TexDataNoLoad(int texnum);
int   Saturn_TexWidth(int texnum);
int   Saturn_TexHeight(int texnum);

/* Everything a span painter needs for `texnum` in ONE lookup (the old
 * three accessors each re-ran bounds checks + the translation chain).
 * Fills the struct and returns 1, or returns 0 if texnum is unusable
 * (inactive tex, out of range, lazy composite failed).
 *   wmask/hmask are the row/texel wrap masks, valid ONLY when
 *   is_pow2 is set (power-of-two width AND height — the usual case
 *   for id WAD art; everything else can still use the % fallback). */
typedef struct
{
    byte *data;                 /* composited pixels, w*h bytes       */
    int   w, h;
    int   wmask, hmask;         /* w-1 / h-1 when power of two        */
    int   pow2;                 /* w and h both powers of two         */
} stexinfo_t;

int Saturn_TexInfo(int texnum, stexinfo_t *out);
/* Slave-safe twin of Saturn_TexInfo: cached textures only. */
int   Saturn_TexInfoNoLoad(int texnum, stexinfo_t *out);

/* Raw 64x64 8bpp pixels for flat `flatnum` (raw index, NOT through
 * flattranslation). Stable zone copy, cached. NULL if unavailable. */
byte *Saturn_FlatData(int flatnum);
/* Slave-safe twin of Saturn_FlatData: cached flats only. */
byte *Saturn_FlatDataNoLoad(int flatnum);
int   Saturn_FlatActive(void);

/* PC COLORMAP lump: shades*256 byte table, index = shade*256 + texel
 * (shade 0 = brightest, 33 = black). NULL if the lump is missing. */
byte *Saturn_Colormap(void);

/* Round-10f: rescan COLORMAP row 0's identity ramp and ACCUMULATE any
 * mismatch into saturn_cm_bad_live (nonzero at any time = the cached
 * colormap buffer was stomped in play). Master-side, once per frame.
 * Round-10g disambiguators: saturn_cm_bad is the BOOT-time probe
 * count (nonzero = the load itself was broken); index/count carry the
 * LATEST bad poll's first mismatched byte index and bad-byte count —
 * the writer's pattern. */
extern volatile int saturn_cm_bad;
extern volatile int saturn_cm_bad_live;
extern volatile int saturn_cm_bad_index;
extern volatile int saturn_cm_bad_count;
/* Round-10h writer fingerprint: boot probe's bad index, the min/max
 * distinct bad indices ever seen (255/0 = nothing yet), how many
 * DISTINCT bytes have ever gone bad, and the latest first-bad byte's
 * VALUE. Bitmap accessor for the flight recorder. */
extern volatile int saturn_cm_bad_boot_index;
extern volatile int saturn_cm_bad_imin;
extern volatile int saturn_cm_bad_imax;
extern volatile int saturn_cm_bad_idist;
extern volatile int saturn_cm_bad_val;
const unsigned char *Saturn_ColormapBadmap(void);
void Saturn_ColormapPoll(void);

/* Texture number of SKY1 (composited like any wall texture), or -1. */
int Saturn_SkyTexNum(void);

/*---- Sprites (task #16) ----
 * `s` is an index into the S_START window (lump - firstsprite).
 * Saturn_SpritePatch returns the stable RAW patch copy and fills the
 * parsed header (NULL = unusable sprite). Saturn_SpriteData (master
 * only, lazy) returns the ROW-major w*h pixel buffer with 0 as
 * transparent; the slave must use Saturn_SpriteDataNoLoad — the master
 * preloads every collected sprite before dispatch. */
byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs);
byte *Saturn_SpriteData(int s);
byte *Saturn_SpriteDataNoLoad(int s);

/* Sprite-float hunt vitals (round-33f): conversion-time facts about
 * sprite `s`'s cache. botz = all-zero bottom rows of the converted
 * buffer; gguard = columns whose post chain died on the pend guard;
 * plen = the lump length the conversion used. Slave-safe reads. */
int Saturn_SpriteBotz(int s);
int Saturn_SpriteGuard(int s);
int Saturn_SpritePlen(int s);

/* Bump-allocate from the render-table reservation at the top of
 * WRAM-L (see saturn_tex.c). NULL when the reservation is exhausted
 * or WRAM-L failed its probe — callers must fall back to the cart
 * Z zone. */
void *Saturn_RtblAlloc(int size);

/* Level-transition hook, called at the end of Z_FreeTags: nulls every
 * pixel-cache entry that points inside the Z zone (its PU_LEVEL copy
 * was just freed; the next use re-caches on demand). WRAM-L/WRAM-H
 * entries survive — see saturn_tex.c. */
void Saturn_CacheZoneReset(void);

/* Pre-baked texture and sprite loaders (E1M1-E1M9) */
void Saturn_BakedTextures_Init(const char *lumpname);
void Saturn_BakedSprites_Init(const char *lumpname);
int  Saturn_BakedTextures_Parse(byte *blob, unsigned fsize);
int  Saturn_BakedSprites_Parse(byte *blob, unsigned fsize);

#endif