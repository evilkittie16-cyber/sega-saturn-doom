/* m_argv.c — command-line. The Saturn has no shell, so M_InitArgv is
 * called with a synthesized list ("DOOM" + "-iwad" + "/CD/DOOM1.WAD" or
 * whatever D_DoomMain chooses). */
#include <string.h>
#include <ctype.h>
#include "m_argv.h"

/* Case-insensitive compare; newlib on the SH-elf toolchain has no
 * strcasecmp, so provide a local one. */
static int strcasecmp_local(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int      myargc = 1;
static char    *myargv_default = "doom";
static char    *myargv_list[32];
static char     myargv_storage[32][32];
static int      myargv_initted = 0;

int M_CheckParm(char *check)
{
    int i;
    for (i = 1; i < myargc; i++)
    {
        if (!strcasecmp_local(check, myargv_list[i]))
        {
            return i;
        }
    }
    return 0;
}

int M_GetArgc(void) { return myargc; }
char **M_GetArgv(void) { return myargv_list; }

int M_InitArgv(int argc, char **argv)
{
    int i;
    myargc = (argc < 32) ? argc : 32;
    if (myargc < 1) myargc = 1;
    for (i = 0; i < myargc; i++)
    {
        myargv_list[i] = argv[i];
    }
    myargv_initted = 1;
    return myargc;
}

/* Called by D_DoomMain when there is no command line. The original id code
 * passed a real argc/argv from the shell; we synthesize one. */
int M_FakeArgv(const char *extra_args)
{
    myargc = 1;
    myargv_list[0] = myargv_default;
    if (extra_args)
    {
        /* tokenize on space; each token becomes an argv[i+1] */
        const char *p = extra_args;
        while (*p && myargc < 32)
        {
            /* skip whitespace */
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            /* copy a token into storage */
            {
                int j = 0;
                while (*p && *p != ' ' && *p != '\t' && j < 30)
                {
                    myargv_storage[myargc][j++] = *p++;
                }
                myargv_storage[myargc][j] = 0;
                myargv_list[myargc] = myargv_storage[myargc];
                myargc++;
            }
        }
    }
    myargv_initted = 1;
    return myargc;
}
