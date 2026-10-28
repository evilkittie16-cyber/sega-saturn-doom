/* lzss.h — stub for d32xr's LZSS decompressor. d32xr's w_wad.c uses
 * LZSS-compressed PWADs; retail IWADs (DOOM.WAD, DOOM2.WAD) are NOT
 * LZSS-compressed, so for the IWAD path the lzss_state_t API is never
 * actually invoked. We provide just enough of the API to compile. */
#ifndef __LZSS_H_SATURN__
#define __LZSS_H_SATURN__

#include "doomdef.h"

typedef struct {
    unsigned char  *src;
    unsigned char  *dst;
    int             size;
} lzss_state_t;

static inline void lzss_setup(lzss_state_t *s, unsigned char *src, unsigned char *dst, int size)
{
    s->src = src;
    s->dst = dst;
    s->size = size;
}

/* Stub: IWAD lumps are not LZSS-compressed. Real implementation is
 * provided when Saturn DOOM supports LZSS-compressed PWADs (none of
 * the retail IWADs use it). */
static inline int lzss_read_all(lzss_state_t *s)
{
    /* copy src to dst for the size, no decompression */
    int  i;
    for (i = 0; i < s->size; i++) s->dst[i] = s->src[i];
    return s->size;
}

#endif