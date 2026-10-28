/* saturn_pak.h - Consolidated level asset package loader for Sega Saturn
 *
 * Reads cd/<MAP>.PAK into memory in a single sequential CD-ROM pass.
 */
#ifndef SATURN_PAK_H
#define SATURN_PAK_H

#include <stdint.h>

#define SATURN_PAK_MAGIC    0x4D50414B  /* "MPAK" */
#define SATURN_PAK_VERSION  1
#define SATURN_PAK_MAX_SECS 16

typedef struct {
    char     name[4];        /* "FLAT", "SEGS", "PVS ", "LEAF", "MTEX", "MSPR" */
    uint32_t sec_offset;     /* 2048-byte sector offset from start of file */
    uint32_t byte_size;      /* exact size in bytes */
    uint32_t reserved;
} saturn_pak_section_t;

/* Loads and initializes all level assets from cd/<lumpname>.PAK.
 * Returns 1 on success, 0 if .PAK not found or invalid (caller can fall back). */
int Saturn_Pak_LevelLoad(const char *lumpname);

#endif /* SATURN_PAK_H */
