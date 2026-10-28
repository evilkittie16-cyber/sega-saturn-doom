/* doomstat.c — definitions of the globals in doomstat.h that d32xr's
 * header files declare but don't define. Most of these are now defined
 * by d32xr's doomdef.h or p_local.h; we only carry the few that remain. */
#include "doomdef.h"
#include "p_local.h"

int   gametic    = 0;
int   basetic    = 0;
int   killcount  = 0;
int   itemcount  = 0;
int   secretcount = 0;
int   paused     = 0;
int   sendpause  = 0;
int   berserk    = 0;
int   autorun    = 0;
int   viewactive = 0;
int   menuactive = 0;
int   nodrawers  = 0;
int   timeinmenu = 0;
int   totalleveltimes = 0;
int   totalgametime  = 0;
int   mouse_accelfactor   = 0;