/* saturn_vgl.h — Saturn-native patch graphics (saturn_vgl.c). */
#ifndef __SATURN_VGL__
#define __SATURN_VGL__

#include "doomtype.h"

/* Cache a WAD patch by name (PU_STATIC, memoized). NULL if the WAD
 * doesn't have it. The name may be a stack buffer — it is copied. */
void *Saturn_CachePatch(const char *name);

/* Big-patch variant: lumps > 8 KB stream through the shared CD scratch
 * and copy into the cart-backed zone in vblank-gap slices — the normal
 * path's single whole-lump memcpy into cart is a banned sustained A-bus
 * burst past ~50 KB (wedges the console; see saturn_vgl.c). */
void *Saturn_CachePatchBig(const char *name);

/* Diagnostic: the raw filepos the last CachePatchBig call saw (cart
 * offset for hot lumps, disc offset | COLD_BIAS for cold), its length,
 * and the resolved on-disc offset (the CD train's base). */
extern unsigned int saturn_bigpatch_base;
extern unsigned int saturn_bigpatch_len;
extern unsigned int saturn_bigpatch_cd;

/* Blit a PC patch_t into I_VideoBuffer at (x, y), clipping rows to
 * [clip_top, clip_bot) and columns to [0, 320). Index-0 = transparent.
 * No offset application — pass the final position. */
void Saturn_DrawPatch(int x, int y, const void *patch,
                      int clip_top, int clip_bot);
void Saturn_DrawPatchFS(int x, int y, const void *patch);

/* Nearest-neighbour downscaled blit: destination pixel (dx, dy) samples
 * the source at (dx*den/num, dy*den/num). Used for the HUD's tall
 * numbers at 3/4 scale (num=3, den=4) — the rerelease bar's digits are
 * ~3/4 of the DOS STTNUM size, which is what lets "100%" fit inside
 * its bar panel. */
void Saturn_DrawPatchScale(int x, int y, const void *patch,
                           int clip_top, int clip_bot, int num, int den);

/* Same blit with every non-zero texel forced to `idx` (transparency
 * unchanged) — the title screen's structure view: white where patch
 * data exists, whatever the palette is doing. */
void Saturn_DrawPatchSolid(int x, int y, const void *patch,
                           int clip_top, int clip_bot, byte idx);

/* The WAD's red HU font (STCFN lumps), uppercase/digits/punctuation. */
void Saturn_Print(int x, int y, const char *s);
int  Saturn_PrintWidth(const char *s);

/* Solid-index rectangle fill. */
void Saturn_ClearRect(int x, int y, int w, int h, byte idx);

#endif