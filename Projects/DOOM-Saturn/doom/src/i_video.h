/* i_video.h — Saturn video I/O for DOOM. */
#ifndef __I_VIDEO__
#define __I_VIDEO__

#include "doomtype.h"

#define DOOM_SCREENWIDTH    320
#define DOOM_SCREENHEIGHT   200
#define SATURN_SCREEN_W     352
#define SATURN_SCREEN_H     224
#define SATURN_BORDER_X     ((SATURN_SCREEN_W - DOOM_SCREENWIDTH) / 2) /* 16 */
/* The NBG0 display window is 352x224 (Saturn 352x224): the 320-wide DOOM
 * render window and status bar are centered with 16-pixel black borders
 * on left and right (columns 16..335). DOOM_SCREENHEIGHT stays the classic
 * 200-row DOS art/render space. */
#define DOOM_FBHEIGHT       224

/* d32xr writes into these. The video shim copies them to VDP1 each frame. */
extern byte    *I_VideoBuffer;
extern byte    *I_VideoBuffer2;     /* double-buffer */

/* Initialization: called from jo_main() before D_DoomMain. */
void    I_InitVideo (void);

/* Per-frame: submit the back buffer to VDP1. */
void    I_FinishUpdate (void);

/* Page flip if double-buffered. */
void    I_WaitVBL (int count);

/* Boot diagnostic: render TITLEPIC to the framebuffer and hold for
 * 3 seconds, so the user can confirm VDP1 sprite + palette + WAD
 * CD streaming all work before the game loop takes over. */
void    Saturn_ShowTitle (void);

/* VBlank tick counter. Incremented once per VBlank IRQ by the callback
 * registered in I_InitVideo. Read by I_WaitVBL and I_Error to spin for
 * a known number of vblanks. */
extern volatile unsigned int saturn_vbl_count;

#endif
