#ifndef _UNISTD_H_
#define _UNISTD_H_

#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <direct.h>
#include <stdlib.h>

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef access
#define access _access
#endif

#define open(path, flags, ...) _open((path), (flags) | _O_BINARY, ##__VA_ARGS__)

#endif /* _UNISTD_H_ */
