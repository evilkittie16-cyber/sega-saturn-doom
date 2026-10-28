/* st_main.h — minimal status-bar types d32xr's gameplay code references.
 * The full st_main.c renderer doesn't yet exist for Saturn; we just
 * expose the data the gameplay code mutates. */
#ifndef __ST_MAIN_H_SATURN__
#define __ST_MAIN_H_SATURN__

#include "doomdef.h"

#define NUMMICROS     6
#define NUMCARDS      6
#define NUMSPCLFACES  6
#define NUMSBOBJ      32

#define KEYX          0
#define BLUKEYY       0
#define YELKEYY       0
#define REDKEYY       0

typedef enum {
    sbf_facelft = 1,
    sbf_facergt,
    sbf_ouch,
    sbf_gotgat,
    sbf_mowdown
} sbf_e;

typedef enum {
    f_faceleft = 0,
    f_faceright,
    f_hurtbad,
    f_gotgat,
    f_mowdown
} stbarface_e;

typedef struct {
    stbarface_e   specialFace;
    boolean       tryopen[6];
    boolean       forcedraw;
    boolean       gotgibbed;
    VINT          msgTics;
} stbar_t;

extern stbar_t  *stbar;
extern VINT      stbar_tics;

#endif