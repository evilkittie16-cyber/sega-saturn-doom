/* i_system.h — Saturn DOOM's i_system. The signatures must match
 * d32xr's doomdef.h so the linker can resolve symbols cleanly. */
#ifndef __I_SYSTEM__
#define __I_SYSTEM__

#include <jo/jo.h>
#include "doomtype.h"

/* Match d32xr's prototypes exactly. */
void    I_Init (void);
void    I_InitSound (void);
void    I_Shutdown (void);
int     I_GetTime (void);
void    I_Error (char *error, ...) __attribute__((noreturn));
void    I_Quit (void);
void    I_InitTimer (void);
byte   *I_AllocLow (int length);
byte   *I_AllocHigh (int length);

/* Saturn-side: translate Jo Engine's gamepad bitmask to DOOM's BT_*
 * ticbuttons word and stash it in players[consoleplayer]. Matches
 * d32xr's I_ReadControls signature so the MiniLoop call sites compile. */
int     I_ReadControls (void);

/* Saturn-specific extras not in d32xr. */
unsigned int   I_GetTicks (void);

/* Boot / error text (i_system.c). saturn_boot_font is the VDP1-sprite
 * font loaded from cd/FONT.TGA — the proven-visible text channel
 * under SGL. */
extern jo_font *saturn_boot_font;
void    Saturn_BootFontInit(void);   /* idempotent jo_font_load        */
void    Saturn_BootText(int x, int y, const char *str);  /* one line    */
void    Saturn_SetupTag(const char *str); /* BootText + NBG0 glyph stamp */
void    Saturn_BootMsg(const char *msg);  /* draw + commit one frame    */
/* Boot-flight recorder (2026-09-09): stamp boot milestones into the
 * cart record slot — the only memory that survives the SYSTEM-RESET
 * boot wedge on real HW. Saturn_BootRec_PaintPrev shows the previous
 * boot's last milestone on the wedge screen (see i_system.c). */
void    Saturn_BootRec(int n);
void    Saturn_BootRec_PaintPrev(void);
void    Saturn_PumpFrame(void);          /* slSynch() frame commit      */
/* Paint NBG1 a solid stage-marker color (guaranteed-visible channel —
 * see i_system.c). The boot sequence paints one color per stage so a
 * single observation localizes a failure even if no text/sprite shows. */
void    Saturn_StageColor(jo_color color);
void    Saturn_PaintNow(jo_color color);
/* Cart GSUB breadcrumb (i_wad.c) — survives reboot; the next boot's
 * I_WadBase consumes it into saturn_init_site = 0x50+sub. Subs 21-29
 * mirror the level-init path (decode in i_wad.c / g_game.c). */
void    Saturn_GsubStamp(unsigned int sub);
/* Crash-proof printf (local formatter — the SGL library's vsnprintf
 * dies on real varargs in this -nodefaultlibs build). Handles
 * %d %i %u %x %X %s %c %p %%, 0-pad width, 'l' length.
 * WARNING: the numeric specs are BROKEN on target (emit nothing or
 * garbage, and a numeric call destabilizes the boot — 2026-09-04).
 * For numbers use Saturn_Udec / Saturn_Uhex instead. */
int     Saturn_Format(char *out, int outsz, const char *fmt, ...);
/* Varargs-free numeric writers (safe on target). */
void    Saturn_Udec(char *out, unsigned int v);   /* decimal, no pad  */
void    Saturn_Udec0(char *out, unsigned int v, int wide);  /* r68: zero-
                                                             * padded fixed
                                                             * width — the
                                                             * ISR's diag
                                                             * rows use it
                                                             * so every
                                                             * cell is
                                                             * rewritten
                                                             * each paint */
void    Saturn_Uhex(char *out, unsigned int v);   /* hex, upper, no pad */

#endif