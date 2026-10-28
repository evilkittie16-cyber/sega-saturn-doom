/* soundst.h — stub for Saturn DOOM. The original d32xr has a full
 * SFX/Music module; Saturn DOOM Phase 1 has no sound. */
#ifndef __SOUNDST__
#define __SOUNDST__

#include "doomtype.h"

typedef struct {
    int         max_voices;
    int         voices;
    int         lump;       /* d32xr's p_enemy.c reads S_sfx[sound].lump */
} sfxinfo_t;

typedef struct {
    int         handle;
    int         lump;
    int         cur;
    int         last;
} musicinfo_t;

extern int      snd_DesiredMusicDevice;
extern int      snd_DesiredSfxDevice;

#endif
