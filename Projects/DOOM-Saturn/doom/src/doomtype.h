/* doomtype.h — pass-through. d32xr's doomdef.h pulls in its own
 * doomtype.h. We don't redefine the types here; we just re-export
 * them so the rest of the Saturn code uses d32xr's typedefs. */
#ifndef __DOOMTYPE_SAT__
#define __DOOMTYPE_SAT__

/* The d32xr doomdef.h is included by d32xr's own .c files, but our
 * Saturn-tuned .c files in doom/src/ also need the types. We include
 * the d32xr doomdef.h here, but only if it hasn't been included yet.
 * To avoid the heavy doomdef.h pull-in for trivial callers, we
 * also define the lightweight aliases inline. */
#include <stdint.h>

/* These match d32xr's doomtype.h. Keep them in sync. */
typedef signed char    sb8;
typedef unsigned char  ub8;
typedef signed short   sb16;
typedef unsigned short ub16;
typedef signed long    sb32;
typedef unsigned long  ub32;

/* The full type set is provided by including "doomdef.h" (which pulls
 * in d32xr's doomdef.h). */

#endif
