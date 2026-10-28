/* i_sound.c — stubbed. Saturn DOOM Phase 1 has no sound. The Saturn's
 * SCSP/M68K sound CPU could host a DOOM sound driver, but that is a
 * follow-up project on its own. */
#include "i_sound.h"

void I_InitSound(void)        { }
void I_ShutdownSound(void)    { }
void I_SetSfxVolume(int v)    { (void)v; }
void I_SetMusicVolume(int v)  { (void)v; }
void I_PauseSong(void)        { }
void I_ResumeSong(void)       { }
void I_PlaySong(int h, int l) { (void)h; (void)l; }
void I_StopSong(void)         { }
int  I_RegisterSong(void *d)  { (void)d; return 0; }
void I_UnRegisterSong(int h)  { (void)h; }
int  I_QrySongPlaying(void)   { return 0; }
int  I_StartSound(void *d, int v, int s, int p, int pr) { (void)d; (void)v; (void)s; (void)p; (void)pr; return 0; }
void I_StopSound(int h)       { (void)h; }
int  I_SoundIsPlaying(int h)  { (void)h; return 0; }
void I_UpdateSound(void)      { }
void I_UpdateSoundParams(int h, int v, int s) { (void)h; (void)v; (void)s; }
