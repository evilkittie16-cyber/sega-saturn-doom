/* i_sound.h — no-op sound. */
#ifndef __I_SOUND__
#define __I_SOUND__

#include "doomtype.h"

void    I_InitSound (void);
void    I_ShutdownSound (void);
void    I_SetSfxVolume (int volume);
void    I_SetMusicVolume (int volume);
void    I_PauseSong (void);
void    I_ResumeSong (void);
void    I_PlaySong (int handle, int looping);
void    I_StopSong (void);
int     I_RegisterSong (void *data);
void    I_UnRegisterSong (int handle);
int     I_QrySongPlaying (void);
int     I_StartSound (void *data, int vol, int sep, int pitch, int priority);
void    I_StopSound (int handle);
int     I_SoundIsPlaying (int handle);
void    I_UpdateSound (void);
void    I_UpdateSoundParams (int handle, int vol, int sep);

#endif
