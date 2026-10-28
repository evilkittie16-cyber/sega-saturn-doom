/* m_misc.h — misc utilities. */
#ifndef __M_MISC__
#define __M_MISC__

#include "doomtype.h"

typedef struct {
    const char  *name;
    void       **location;
    int          defaultvalue;
} default_t;

int     M_DrawText (int x, int y, const char *string, int scalex, int scaley);
void    M_WriteFile (const char *name, void *source, int length);
int     M_ReadFile  (const char *name, unsigned char **buffer);
int     M_FileLength (const char *file);
void    M_LoadDefaults (void);
void    M_SaveDefaults (void);
void    M_DefaultValueChanged (const char *name);
void    M_FreeDefaults (void);
default_t *M_GetDefault (const char *name);
int     M_ScreenShot (void);

#endif
