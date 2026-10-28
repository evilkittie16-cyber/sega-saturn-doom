/* m_cheat.h — cheat code table. */
#ifndef __M_CHEAT__
#define __M_CHEAT__

#include "doomtype.h"

struct cheat_s {
    char    *cheat;
    void    (*func)(struct player_s *p, char c);
};

void    M_CheatInit (void);
int     M_CheatAllowed (void);
int     M_ParseCheat (char *buf);

extern struct cheat_s cheat[];
extern int            numcheats;

#endif
