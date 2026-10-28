/* saturn_leaf.c — retail Saturn Doom convex-polygon leaf data loader (M2).
 *
 * Loads and validates cd\<MAP>.LEAF (gen_leafs.py output, big-endian,
 * SH-2 reads it in place with zero conversion) and exposes the per-
 * subsector edge runs to the seg renderer's leaf branch.  Mirrors the
 * PVS blob loader's fail-open contract (Saturn_PvsLevelLoad): every
 * validation failure frees the blob and clears the active flag, and
 * the renderer falls back to the plain SSECTORS seg walk.  The blob
 * itself lives in the PU_LEVEL zone exactly like the map structs, so
 * its lifetime is the level's.
 *
 * Blob layout (all big-endian):
 *   Header 40B: "LEAF" | u8 ver=1 | u8 flags | u16 numLeaves
 *               | u32 ofsDir | u32 ofsEdges | u32 nEdges
 *               | u32 ofsExtra | u32 nExtra
 *               | u32 segsLumpBytes | u32 ssectorsLumpBytes
 *               | u32 reserved
 *   Dir:   (numLeaves+1) * u32 firstEdge
 *   Edges: nEdges * 4B: u16 vert, u16 seg (0xFFFF = closure/miniseg)
 *   ExtraVerts: nExtra * 4B: i16 x, i16 y
 *             (vert >= numvertexes -> extra[vert - numvertexes])
 */

#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "r_local.h"
#include "saturn_leaf.h"

/* Buffer cap: E1M1's blob is 8244 bytes (239 leaves, 1691 edges, 120
 * extra verts); 16 KB leaves room for denser maps while staying a
 * one-GFS-shot read.  Files larger than the cap fail open. */
#define LEAF_BLOB_CAP   32768
/* Per-leaf edge budget (generator hard-fails above this too). */
#define LEAF_MAX_EDGES  32

/* Fixed Endian Macro Naming: These parse Big-Endian, not Little-Endian */
#define BE16(p)  ((unsigned)((p)[0]) << 8 | (unsigned)(p)[1])
#define BE32(p)  ((unsigned)((p)[0]) << 24 | (unsigned)((p)[1]) << 16 \
                | (unsigned)((p)[2]) << 8  | (unsigned)(p)[3])

/* M2 leaf state: pointers into the blob + the active flag. 8 bytes of
 * pointers + one int — .bss cost rebated 1:1 against the pool after
 * the first link's __bend measurement (ritual). */
static byte               *leaf_blob;
static const unsigned int *leaf_dir;      /* numLeaves+1 entries */
static const unsigned char *leaf_edgebase;/* 4B records */
static const unsigned char *leaf_extrabase;/* 4B records */
static unsigned short      leaf_nleaves;
static unsigned int        leaf_nextra;
static int                 leaf_ok;

int Saturn_Leaf_Parse(byte *blob, int fsize)
{
#ifdef SATURN_LEAF_RENDERER
    unsigned n, nEdges, nExtra, ofsDir, ofsEdges, ofsExtra, need, i;

    if (!blob || fsize < 40)
        return 0;

    /* Big-endian file: read bytes, never native loads beyond u8. */
    if (blob[0] != 'L' || blob[1] != 'E' || blob[2] != 'A'
        || blob[3] != 'F' || blob[4] != 1 || blob[5] != 0) 
    {
        return 0;
    }

    n        = BE16(blob + 6);
    ofsDir   = BE32(blob + 8);
    ofsEdges = BE32(blob + 12);
    nEdges   = BE32(blob + 16);
    ofsExtra = BE32(blob + 20);
    nExtra   = BE32(blob + 24);

    /* Security fix: Prevent 32-bit integer overflow wrapping */
    if (nEdges >= (LEAF_BLOB_CAP / 4u) || nExtra >= (LEAF_BLOB_CAP / 4u) || n >= (LEAF_BLOB_CAP / 4u))
        return 0;

    if (n != (unsigned)numsubsectors
        || ofsDir   != 40u
        || ofsEdges != 40u + 4u * (n + 1u)
        || ofsExtra != ofsEdges + 4u * nEdges
        || BE32(blob + 28) != (unsigned)numsegs * 12u
        || BE32(blob + 32) != (unsigned)numsubsectors * 4u)
    {
        return 0;
    }

    need = ofsExtra + 4u * nExtra;
    if (need > (unsigned)fsize)
        return 0;

    /* Directory: (n+1) ascending firstEdge offsets, last == nEdges,
     * every leaf's run inside the edge table with <= budget records. */
    {
        const byte *dirb = blob + ofsDir;
        unsigned prev = BE32(dirb);

        if (prev != 0u)
            return 0;
        for (i = 0; i < n; i++)
        {
            unsigned e0 = prev;
            unsigned e1 = BE32(dirb + 4u * (i + 1u));
            if (e1 < e0 || e1 - e0 > LEAF_MAX_EDGES)
                return 0;
            prev = e1;
        }
        if (prev != nEdges)
            return 0;
    }

    /* Every edge: vert in range (WAD table + extra table), seg is
     * a closure marker or a valid seg index. */
    for (i = 0; i < nEdges; i++)
    {
        const byte *er = blob + ofsEdges + 4u * i;
        unsigned vert = BE16(er);
        unsigned seg  = BE16(er + 2);

        if (vert >= (unsigned)numvertexes + nExtra
            || (seg != 0xFFFFu && seg >= (unsigned)numsegs))
        {
            return 0;
        }
    }

#ifdef SATURN_WIN32
    /* Portability Fix: In-place byte swap for Little-Endian hosts (x86 PC).
     * This allows the renderer to use native zero-overhead pointer indexing safely. */
    unsigned int *dir32 = (unsigned int *)(blob + ofsDir);
    for (i = 0; i <= n; i++) {
        unsigned int v = dir32[i];
        dir32[i] = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | 
                   ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
    }
    
    unsigned short *edge16 = (unsigned short *)(blob + ofsEdges);
    for (i = 0; i < nEdges * 2; i++) {
        unsigned short v = edge16[i];
        edge16[i] = (v >> 8) | (v << 8);
    }
    
    unsigned short *extra16 = (unsigned short *)(blob + ofsExtra);
    for (i = 0; i < nExtra * 2; i++) {
        unsigned short v = extra16[i];
        extra16[i] = (v >> 8) | (v << 8);
    }
#endif

    leaf_blob      = blob;
    leaf_dir       = (const unsigned int *)(blob + ofsDir);
    leaf_edgebase  = blob + ofsEdges;
    leaf_extrabase = blob + ofsExtra;
    leaf_nleaves   = (unsigned short)n;
    leaf_nextra    = nExtra;
    leaf_ok        = 1;
    return 1;
#else
    (void)blob;
    (void)fsize;
    return 0;
#endif
}

void Saturn_LeafLevelLoad(const char *lumpname)
{
#ifdef SATURN_LEAF_RENDERER
    byte   *temp_blob;
    int     fsize;

    /* Drop the PREVIOUS level's pointers first thing — the blob's
     * zone block died with the last Z_FreeTags(PU_LEVEL) and no frame
     * may dereference it (same ritual as Saturn_PvsLevelLoad(NULL)). */
    leaf_blob      = NULL;
    leaf_dir       = NULL;
    leaf_edgebase  = NULL;
    leaf_extrabase = NULL;
    leaf_nleaves   = 0;
    leaf_nextra    = 0;
    leaf_ok        = 0;

    if (lumpname == NULL)
        return;
    {
        char fname[16];                 /* 8.3 name: <lump>.LEAF */
        int  k;

        for (k = 0; k < 8 && lumpname[k] != 0; k++)
        {
            char c = lumpname[k];
            if (c >= 'a' && c <= 'z')
                c -= ('a' - 'A');
            fname[k] = c;
        }
        fname[k++] = '.';
        fname[k++] = 'L';
        fname[k++] = 'E';
        fname[k++] = 'A';
        fname[k++] = 'F';
        fname[k] = 0;

        /* Fix: Memory Waste - Allocate a temporary blob to PU_CACHE to load the file */
        temp_blob = (byte *)Z_Malloc(LEAF_BLOB_CAP, PU_CACHE);
        if (temp_blob == NULL)
            return;
            
        fsize = Saturn_Leaf_BlobRead(temp_blob, LEAF_BLOB_CAP, fname);
        
        /* Validate header and allocate exact size in PU_LEVEL */
        if (fsize >= 40 &&
            temp_blob[0] == 'L' && temp_blob[1] == 'E' && temp_blob[2] == 'A' &&
            temp_blob[3] == 'F' && temp_blob[4] == 1 && temp_blob[5] == 0)
        {
            unsigned ofsExtra = BE32(temp_blob + 20);
            unsigned nExtra   = BE32(temp_blob + 24);
            unsigned exact_size = ofsExtra + 4u * nExtra;

            if (exact_size >= 40 && exact_size <= (unsigned)fsize && exact_size <= LEAF_BLOB_CAP)
            {
                byte *perm_blob = (byte *)Z_Malloc(exact_size, PU_LEVEL);
                if (perm_blob)
                {
                    memcpy(perm_blob, temp_blob, exact_size);
                    if (!Saturn_Leaf_Parse(perm_blob, exact_size))
                    {
                        Z_Free(perm_blob);
                        leaf_blob = NULL;
                        leaf_ok = 0;
                    }
                }
            }
        }
        
        Z_Free(temp_blob); /* Free the massive 32KB block back to the system */
    }
#else
    (void)lumpname;
#endif
}

#ifdef SATURN_LEAF_RENDERER

int leaf_active(void)
{
    return leaf_ok;
}

int leaf_edges(int ss, const unsigned short **edges)
{
    unsigned a, b;

    /* Casting signed to unsigned naturally wraps negative numbers to a huge int.
     * __builtin_expect prevents branch prediction misses in the hot path. */
    if (__builtin_expect(!leaf_ok || (unsigned)ss >= leaf_nleaves, 0))
    {
        *edges = NULL;
        return 0;
    }

    /* Hot-Path Fix: The SH-2 is native Big-Endian. Since the .LEAF data 
     * is already Big-Endian and Z_Malloc guarantees 4-byte alignment, we 
     * read the indices directly via native pointer indirection instead of 
     * relying on the slow byte-by-byte shifting of the macro. */
    a = leaf_dir[ss];
    b = leaf_dir[ss + 1];

    *edges = (const unsigned short *)(leaf_edgebase + (a << 2));
    return (int)(b - a);
}

const short *leaf_extra_verts(void)
{
    return (const short *)leaf_extrabase;
}

#endif /* SATURN_LEAF_RENDERER */