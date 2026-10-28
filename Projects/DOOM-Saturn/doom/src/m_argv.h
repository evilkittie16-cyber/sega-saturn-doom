/* m_argv.h — command-line parsing. */
#ifndef __M_ARGV__
#define __M_ARGV__

int     M_CheckParm (char *check);
int     M_InitArgv (int argc, char **argv);
int     M_GetArgc (void);
char  **M_GetArgv (void);

#endif
