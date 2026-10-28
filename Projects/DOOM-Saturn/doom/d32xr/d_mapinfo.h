/* d_mapinfo.h — DEH/BEX mapinfo types for d32xr. The fields match the
 * d32xr's g_game.c usage: it Z_Mallocs sizeof(dmapinfo_t) + strings,
 * sets lumpName = sizeof(dmapinfo_t), and packs three strings at the
 * tail. A_BossDeath reads the `specials` bitmask.
 *
 * Saturn DOOM doesn't run DEH, so we just set sensible defaults in
 * G_MapInfoForLumpName (called from g_game.c). */
#ifndef __D_MAPINFO__
#define __D_MAPINFO__

#include "doomtype.h"

/* Map special bit flags. These gate which monster types the boss-death
 * code handles per-map (e.g. MAP08: BARON; E1M8: BARON, etc.). */
#define MI_BARON_SPECIAL      0x001
#define MI_CYBER_SPECIAL      0x002
#define MI_SPIDER_SPECIAL     0x004
#define MI_FATSO_SPECIAL      0x008
#define MI_BABY_SPECIAL       0x010
#define MI_CYBER_SPECIAL2     0x020
#define MI_SPIDER_SPECIAL2    0x040

typedef struct {
    /* Game-wide info (not per-map, but kept here so doomdef.h's
     * "extern dgameinfo_t gameinfo" still compiles). */
    char    dummy;
} dgameinfo_t;

/* Per-map info. The trailing strings are allocated past the end of
 * this struct, and `lumpName`/`name`/`sky` are offsets into that
 * trailing buffer. The two "mapNumber" fields below are referenced
 * by g_game.c. */
typedef struct {
    int     mapNumber;     /* DOOM2 map number, 1..32 */
    int     specials;      /* MI_*_SPECIAL bits */
    int     lumpName;      /* offset within struct to "MAP01" string */
    int     name;          /* offset within struct to display name */
    int     sky;           /* offset within struct to sky lump name */
    /* followed by: lumpName string, then name string, then sky string */
} dmapinfo_t;

extern dgameinfo_t  gameinfo;
extern dmapinfo_t  *gamemapinfo;
extern dmapinfo_t **gamemaplist;
/* gamemapcount is declared in doomdef.h (d32xr) — don't redeclare here. */

dmapinfo_t *G_MapInfoForLumpName(const char *lumpName);

#endif