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
extern int                           saturn_baked_flat_numsubsectors;

#endif /* SATURN_BAKED_FLATS_H */
