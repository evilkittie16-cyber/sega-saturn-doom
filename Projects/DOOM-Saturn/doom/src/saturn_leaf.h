/*
 * saturn_leaf.h — retail Saturn Doom convex-polygon leaf data (M2).
 *
 * The LEAF blob (cd\<MAP>.LEAF, built by gen_leafs.py, validated by
 * sim_leaf.py) replaces the SSECTORS seg range as the wall producer's
 * per-leaf edge list: each subsector is a CLOSED convex polygon whose
 * edges are (u16 vertexIdx, u16 segIdx); segIdx == 0xFFFF = closure /
 * miniseg edge (partition-line boundary, no wall).  seg_add_line
 * consumes the real edges unchanged; the closure edges are what the
 * M3 z-buffer flat fill needs for the leaf's screen range.
 *
 * Fail-open contract, same as the PVS blob (Saturn_PvsLevelLoad): any
 * load/validation failure leaves leaf_active() == 0 and the plain
 * seg walk runs.  Blob memory is PU_LEVEL zone, dropped here at
 * level start (the previous level's block died with the last
 * Z_FreeTags(PU_LEVEL) — no frame may dereference it).
 */

#ifndef SATURN_LEAF_H
#define SATURN_LEAF_H

#include "doomdef.h"

/* Called from P_SetupLevel: NULL drops the previous level's pointers,
 * the map name loads cd\<lumpname>.LEAF (E1M1 -> "E1M1.LEAF"); any
 * failure = feature off = the plain seg walk. */
void Saturn_LeafLevelLoad(const char *lumpname);
int  Saturn_Leaf_Parse(byte *blob, int fsize);

/* One-shot synchronous GFS read of a bare 8.3 cd\ file (i_wad.c).
 * Returns bytes read, 0 on any failure. Main thread only. */
int  Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname);

#ifdef SATURN_LEAF_RENDERER
/* 1 while a validated leaf blob covers the loaded map. */
int  leaf_active(void);

/* Edge run for subsector ss: sets *edges to the run start (u16 pairs,
 * big-endian blob read in place by the BE target) and returns the
 * record count. Only valid when leaf_active(). */
int  leaf_edges(int ss, const unsigned short **edges);

/* Extra-vertex table (corners the WAD vertex table lacks): a vertex
 * index >= numvertexes refers to extra[idx - numvertexes] (i16 x, i16
 * y). M3's flat fill uses these; NULL when nExtra == 0. */
const short *leaf_extra_verts(void);
#endif

#endif /* SATURN_LEAF_H */