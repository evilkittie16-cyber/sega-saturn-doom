/* m_cheat.c — cheat code table. On Saturn we keep the data table but the
 * actual cheat handlers (in p_inter) are no-ops; this compiles. */
#include "m_cheat.h"

struct cheat_s cheat[1];
int            numcheats = 0;

void M_CheatInit(void)        { numcheats = 0; }
int  M_CheatAllowed(void)     { return 0; }
int  M_ParseCheat(char *buf)  { (void)buf; return 0; }
