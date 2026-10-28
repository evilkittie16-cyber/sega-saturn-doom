/* s_sound.c — Saturn stub. d32xr's full s_sound.c needs its music
 * player (music.h) and sample mixer; Saturn DOOM Phase 1 has no audio.
 * Provide empty functions matching the symbols other modules call:
 *
 *   S_Init, S_StartSound, S_StopSound, S_StartSong, S_StopSong,
 *   S_RestartSounds, S_SetSoundDriver.
 *
 * The globals sfxvolume, musicvolume, finalquad, soundtics, etc. are
 * declared `extern VINT` in d32xr's doomdef.h. We do not redefine them
 * here; d32xr's w_wad.c will pull in the definitions from its s_sound.c
 * block. For now Saturn has no audio mixer so these are unused. */

#include "doomdef.h"

void S_Init(void) { }

void S_StartSound(void *origin, int sfx_id)
{
    (void)origin; (void)sfx_id;
}

void S_StopSound(void *origin)
{
    (void)origin;
}

void S_StartSong(int musiclump, int looping, int cdtrack)
{
    (void)musiclump; (void)looping; (void)cdtrack;
}

void S_StopSong(void) { }

void S_RestartSounds(void) { }

void S_SetSoundDriver(int newdrv) { (void)newdrv; }