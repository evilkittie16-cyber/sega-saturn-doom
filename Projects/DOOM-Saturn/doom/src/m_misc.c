/* m_misc.c — misc helpers.
 *
 * The original id Misc file was a grab-bag: config file read/write, error
 * dialogs, screenshots, etc. On Saturn we strip it down to:
 *   - stubbed file I/O (no /home, no /etc)
 *   - a small default table so D_DoomMain can compile
 *   - the doom_strdup helper used by the cheat table
 */
#include <string.h>
#include <stdint.h>
#include "m_misc.h"
#include "doomdef.h"

/* The original had a long defaults table; we keep an empty one for now. */
default_t        defaults[1]  = { {NULL, NULL, 0} };
default_t       *def_default  = defaults;

int M_DrawText(int x, int y, const char *string, int scalex, int scaley)
{
    (void)x; (void)y; (void)string; (void)scalex; (void)scaley;
    return 0;
}

void M_WriteFile(const char *name, void *source, int length)
{
    /* No filesystem persistence on Saturn. The original saved the config to
     * ~/.doomrc; we have no .doomrc. */
    (void)name; (void)source; (void)length;
}

int M_ReadFile(const char *name, unsigned char **buffer)
{
    (void)name; (void)buffer;
    return -1;
}

int M_FileLength(const char *file)
{
    (void)file;
    return 0;
}

void M_LoadDefaults(void)
{
    /* nothing */
}

void M_SaveDefaults(void)
{
    /* nothing */
}

void M_DefaultValueChanged(const char *name)
{
    (void)name;
}

void M_FreeDefaults(void)
{
    /* nothing */
}

default_t *M_GetDefault(const char *name)
{
    (void)name;
    return NULL;
}

int M_ScreenShot(void)
{
    return -1;
}
