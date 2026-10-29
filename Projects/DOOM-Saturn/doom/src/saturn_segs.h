/* saturn_segs.h — Offline PVS1 loader & leaf lookup interface. */

#ifndef SATURN_SEGS_H
#define SATURN_SEGS_H

#include "doomdef.h"

typedef struct
{
    int px, rows, mintu, maxtu, why, utex, v0;
    int tex, sh, light;
    int calls, band, vstep;
    int pad[3];
} saturn_mm_dbg_t;

extern volatile saturn_mm_dbg_t saturn_mm_dbg[2];
extern volatile int saturn_mm_zfull[3];
extern volatile int saturn_mm_zcol;
extern volatile int saturn_mm_ztex;
extern volatile int saturn_mm_fill_sec;
extern volatile int saturn_mm_fill_light;

/* Per-level PVS1 blob loader & per-frame viewer leaf row lookup. */
void Saturn_PvsLevelLoad(const char *lumpname);
int  Saturn_Pvs_Parse(byte *blob, unsigned fsize);
int  Saturn_Pvs_BlobRead(void *buf, unsigned cap);
const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y);

extern volatile unsigned int saturn_spr_frt_acc;
extern volatile unsigned int saturn_flu_frt_acc;
extern volatile unsigned int saturn_blit_frt_acc;
extern volatile unsigned int saturn_pre_frt_acc;
extern unsigned int saturn_spr_frt_last;
extern unsigned int saturn_flu_frt_last;
extern unsigned int saturn_blit_frt_last;
extern unsigned int saturn_pre_frt_last;

#define SAT_FRT_NET_ARMED 0
#define SAT_FRT_NET(acc_, last_, kind_) do { } while (0)

#endif /* SATURN_SEGS_H */
