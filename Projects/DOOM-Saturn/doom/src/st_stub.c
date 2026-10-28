/* st_stub.c — minimal status-bar stub. d32xr's p_user.c and p_inter.c
 * mutate stbar[].specialFace, stbar[p].tryopen[card], etc. The full
 * status-bar renderer (st_main.c) is dropped for now (Saturn doesn't
 * yet have HUD draw). The data still has to live in memory so the
 * writes don't fault. */

#include "st_main.h"
#include <string.h>

stbar_t   *stbar;
VINT       stbar_tics;

void ST_Init(void)        { }
void ST_Ticker(void)      { }
void ST_Drawer(void)      { }
void ST_ForceDraw(void)   { }
void ST_InitEveryLevel(void)
{
    int  i;
    if (!stbar)
    {
        /* size_of_stbar_t comes from st_main.h. MAXPLAYERS = 4. */
        stbar = (stbar_t *)Z_Malloc(sizeof(stbar_t) * 4, 1);
        memset(stbar, 0, sizeof(stbar_t) * 4);
    }
    for (i = 0; i < 4; i++) stbar[i].specialFace = 0;
}
void ST_Num(int x, int y, int num)   { (void)x; (void)y; (void)num; }
void ST_DrawValue(int x,int y,int v) { (void)x; (void)y; (void)v; }
void ST_DrawMicroValue(int x,int y,int v) { (void)x; (void)y; (void)v; }
void ST_EraseBlock(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }