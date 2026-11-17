/* saturn_baked_flats.h - Pre-calculated pure VDP1 3D floor & ceiling quads */
#ifndef SATURN_BAKED_FLATS_H
#define SATURN_BAKED_FLATS_H

#include <stdint.h>

typedef struct {
    int16_t x;
    int16_t y;
} saturn_flat_vert_t;

typedef struct {
    uint16_t v[4];
} saturn_flat_quad_idx_t;

typedef struct {
    int16_t x[4];
    int16_t y[4];
} saturn_flat_quad_t;

typedef struct {
    int16_t min_x;
    int16_t max_x;
    int16_t min_y;
    int16_t max_y;
} saturn_flat_bbox_t;

void Saturn_BakedFlats_Init(const char *lumpname);
int  Saturn_BakedFlats_Parse(void *data, unsigned len);
int  Saturn_BakedFlats_GetIndexed(int subnum,
                                 const saturn_flat_vert_t **out_verts, int *out_vcount,
                                 const saturn_flat_quad_idx_t **out_quads, int *out_qcount);
int  Saturn_BakedFlats_Get(int subnum, const saturn_flat_quad_t **out_quads);
int  Saturn_BakedFlats_GetBBox(int subnum, const saturn_flat_bbox_t **out_bbox);
/* Seam fix: per-vertex 1/16-unit fractions, one byte per vertex = (fx << 4) | fy.
 * Exact vertex = (x + fx/16, y + fy/16). Pass the verts pointer returned by
 * Saturn_BakedFlats_GetIndexed; result is indexed with the same vertex index.
 * Never NULL while flats are loaded (old files get an all-zero array). */
const uint8_t *Saturn_BakedFlats_GetVertFrac(const saturn_flat_vert_t *verts);

/* Direct array access for zero-overhead inlined flat rendering & traversal */
extern const saturn_flat_bbox_t     *saturn_baked_flat_bboxes;
extern const uint16_t               *saturn_baked_flat_q_count;
extern const uint16_t               *saturn_baked_flat_v_count;
extern const uint32_t               *saturn_baked_flat_q_first;
extern const uint32_t               *saturn_baked_flat_v_first;
extern const uint16_t               *saturn_baked_flat_lod1_q_count;
extern const uint16_t               *saturn_baked_flat_lod1_v_count;
extern const uint32_t               *saturn_baked_flat_lod1_q_first;
extern const uint32_t               *saturn_baked_flat_lod1_v_first;
extern const saturn_flat_vert_t     *saturn_baked_flat_verts;
extern const saturn_flat_quad_idx_t *saturn_baked_flat_quads;
extern const uint8_t                *saturn_baked_flat_vfrac;
/* Per-quad texture mapping from the QUV1 block (NULL for older .FLT files):
 * bits 15-14 mode (1 normal, 2 transposed, 3 LOD1 tile), bits 13-8 first
 * texture row, bits 6-0 row count. Rows run from the quad's v3-v2 edge to its
 * v0-v1 edge. */
extern const uint16_t               *saturn_baked_flat_quv;
extern const uint8_t                *saturn_baked_flat_subflags; /* bit0: LOD0 uses transposed */

/* CHK1 spatial chunks (tools/bake_flats.py chunk_subsector): a big subsector's
 * LOD0 quads / vertices are contiguous per-chunk ranges (subsector-relative),
 * so the slave can frustum-cull a chunk by its bbox. Record 0 = "none". */
typedef struct {
    int16_t  min_x, max_x, min_y, max_y;
    uint16_t q_off, q_cnt, v_off, v_cnt;   /* v_cnt bit 15: last chunk of the subsector */
} saturn_flat_chunk_t;
#define SATURN_FLAT_CHUNK_LAST 0x8000u
extern const saturn_flat_chunk_t    *saturn_baked_flat_chunks;
extern const uint16_t               *saturn_baked_flat_sub_chunk;
#define SATURN_QUV_NORMAL      1
#define SATURN_QUV_TRANSPOSED  2
#define SATURN_QUV_TILE        3
#define SATURN_SUBFLAG_TRANSPOSED 1
/* QUV bit 7: the piece is not an exact row mapping (cut by a diagonal or
 * trimmed on both axes); drawn with its own warped texture when one is
 * resident (Saturn_Vdp1_GetWarpSrca). Subflag bit 1: LOD0 list has some. */
#define SATURN_QUV_WARP        0x80u
#define SATURN_SUBFLAG_WARP    2
#define SATURN_SUBFLAG_WARP_LOD1 4  /* LOD1 (128-unit) list has QUV_WARP quads */
extern int                           saturn_baked_flat_numsubsectors;

#endif /* SATURN_BAKED_FLATS_H */
