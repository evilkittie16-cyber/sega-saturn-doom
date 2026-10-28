#ifndef _DOS_REF_COMPAT_H_
#define _DOS_REF_COMPAT_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define alloca      _alloca
#define strupr      wad_strupr

#endif /* _DOS_REF_COMPAT_H_ */
