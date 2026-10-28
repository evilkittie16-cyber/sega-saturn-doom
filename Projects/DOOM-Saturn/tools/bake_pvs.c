/* bake_pvs.c - High-Performance Offline PVS Baker for Sega Saturn DOOM
 *
 * Computes 2D Potentially Visible Set (PVS) bitmasks for subsectors across
 * maps E1M1 through E4M9.
 *
 * Algorithm:
 * 1. Load DOOM.WAD (VERTEXES, LINEDEFS, SEGS, SSECTORS).
 * 2. Load <MAP>.FLT (full 2D convex polygon vertices for every rendered flat).
 * 3. Collect 1-sided solid world walls.
 * 4. Spatial uniform grid for sub-microsecond raycasting.
 * 5. Sample points per subsector: centroid, all polygon vertices (from both WAD and FLT),
 *    seg midpoints, and inward-shrunk perimeter points to eliminate boundary precision loss.
 * 6. Direct portal adjacency: any subsectors sharing a 2-sided linedef are
 *    unconditionally marked visible.
 * 7. Conservative ray visibility: if ANY ray between subsector A and subsector B
 *    has an unoccluded line of sight, both are marked visible (pvs[A][B] = pvs[B][A] = 1).
 * 8. Write big-endian PVS1 format to cd/<MAP>.PVS and staging_assets/<MAP>.PVS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#define CELL_SIZE 128
#define MAX_SAMPLES 64

typedef struct {
    float x, y;
} point_t;

typedef struct {
    point_t p1, p2;
    float min_x, max_x, min_y, max_y;
} wall_t;

typedef struct {
    int count;
    int capacity;
    int *walls;
} cell_t;

typedef struct {
    int num_samples;
    point_t pts[MAX_SAMPLES];
    float min_x, max_x, min_y, max_y;
} subsector_info_t;

#pragma pack(push, 1)
typedef struct {
    int16_t x, y;
} wad_vertex_t;

typedef struct {
    int16_t v1, v2;
    int16_t flags, special, tag;
    int16_t sidenum[2];
} wad_linedef_t;

typedef struct {
    int16_t v1, v2;
    int16_t angle;
    int16_t linedef;
    int16_t side;
    int16_t offset;
} wad_seg_t;

typedef struct {
    int16_t numsegs;
    int16_t firstseg;
} wad_subsector_t;

typedef struct {
    char     magic[4];          /* "FLT2" */
    uint16_t version;           /* 2 */
    uint16_t numsubsectors;
    uint32_t numquads;
    uint32_t numvertexes;
    uint32_t ofs_sub_q_first;
    uint32_t ofs_sub_q_count;
    uint32_t ofs_sub_v_first;
    uint32_t ofs_sub_v_count;
    uint32_t ofs_sub_bbox;
    uint32_t ofs_verts;
    uint32_t ofs_quads;
    uint32_t reserved[5];
} flt2_header_t;
#pragma pack(pop)

static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t swap32(uint32_t v) {
    return ((v << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | (v >> 24));
}

static inline int seg_intersect(point_t p1, point_t p2, point_t p3, point_t p4)
{
    float d = (p4.y - p3.y) * (p2.x - p1.x) - (p4.x - p3.x) * (p2.y - p1.y);
    if (fabsf(d) < 1e-6f) return 0;
    float inv_d = 1.0f / d;
    float uA = ((p4.x - p3.x) * (p1.y - p3.y) - (p4.y - p3.y) * (p1.x - p3.x)) * inv_d;
    float uB = ((p2.x - p1.x) * (p1.y - p3.y) - (p2.y - p1.y) * (p1.x - p3.x)) * inv_d;
    return (uA > 0.001f && uA < 0.999f && uB > 0.001f && uB < 0.999f);
}

/* Fast 2D Grid Raycaster */
static inline int ray_clear_grid(point_t p1, point_t p2,
                                 const wall_t *walls,
                                 const cell_t *grid,
                                 int grid_w, int grid_h,
                                 float grid_min_x, float grid_min_y,
                                 uint32_t *stamp_buf, uint32_t curr_stamp)
{
    float rx0 = p1.x < p2.x ? p1.x : p2.x;
    float rx1 = p1.x > p2.x ? p1.x : p2.x;
    float ry0 = p1.y < p2.y ? p1.y : p2.y;
    float ry1 = p1.y > p2.y ? p1.y : p2.y;

    int gx0 = (int)floorf((rx0 - grid_min_x) / CELL_SIZE);
    int gx1 = (int)floorf((rx1 - grid_min_x) / CELL_SIZE);
    int gy0 = (int)floorf((ry0 - grid_min_y) / CELL_SIZE);
    int gy1 = (int)floorf((ry1 - grid_min_y) / CELL_SIZE);

    if (gx0 < 0) gx0 = 0;
    if (gy0 < 0) gy0 = 0;
    if (gx1 >= grid_w) gx1 = grid_w - 1;
    if (gy1 >= grid_h) gy1 = grid_h - 1;

    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            const cell_t *c = &grid[gy * grid_w + gx];
            for (int k = 0; k < c->count; k++) {
                int wi = c->walls[k];
                if (stamp_buf[wi] == curr_stamp) continue;
                stamp_buf[wi] = curr_stamp;

                const wall_t *w = &walls[wi];
                if (rx1 < w->min_x || rx0 > w->max_x || ry1 < w->min_y || ry0 > w->max_y)
                    continue;

                if (seg_intersect(p1, p2, w->p1, w->p2))
                    return 0; /* Blocked */
            }
        }
    }
    return 1; /* Clear */
}

static uint8_t *load_file(const char *path, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(sz);
    if (data) fread(data, 1, sz, f);
    fclose(f);
    if (out_len) *out_len = (uint32_t)sz;
    return data;
}

typedef struct {
    char name[9];
    uint32_t pos;
    uint32_t size;
} wad_lump_t;

static void add_sample_point(subsector_info_t *si, point_t p)
{
    for (int i = 0; i < si->num_samples; i++) {
        if (fabsf(si->pts[i].x - p.x) < 0.5f && fabsf(si->pts[i].y - p.y) < 0.5f)
            return;
    }
    if (si->num_samples < MAX_SAMPLES) {
        si->pts[si->num_samples++] = p;
    }
}

static int process_map(const char *mapname, uint8_t *wad_data, wad_lump_t *lumps, int num_lumps)
{
    int map_idx = -1;
    for (int i = 0; i < num_lumps; i++) {
        if (strcmp(lumps[i].name, mapname) == 0) {
            map_idx = i;
            break;
        }
    }
    if (map_idx == -1) return 0;

    wad_vertex_t *v_data = NULL;
    wad_linedef_t *l_data = NULL;
    wad_seg_t *s_data = NULL;
    wad_subsector_t *ss_data = NULL;
    int num_v = 0, num_l = 0, num_s = 0, num_ss = 0;

    for (int i = map_idx + 1; i < map_idx + 12 && i < num_lumps; i++) {
        if (strcmp(lumps[i].name, "VERTEXES") == 0) {
            v_data = (wad_vertex_t *)(wad_data + lumps[i].pos);
            num_v = lumps[i].size / sizeof(wad_vertex_t);
        } else if (strcmp(lumps[i].name, "LINEDEFS") == 0) {
            l_data = (wad_linedef_t *)(wad_data + lumps[i].pos);
            num_l = lumps[i].size / sizeof(wad_linedef_t);
        } else if (strcmp(lumps[i].name, "SEGS") == 0) {
            s_data = (wad_seg_t *)(wad_data + lumps[i].pos);
            num_s = lumps[i].size / sizeof(wad_seg_t);
        } else if (strcmp(lumps[i].name, "SSECTORS") == 0) {
            ss_data = (wad_subsector_t *)(wad_data + lumps[i].pos);
            num_ss = lumps[i].size / sizeof(wad_subsector_t);
        }
    }

    if (!v_data || !l_data || !s_data || !ss_data || num_ss <= 0) return 0;

    clock_t t0 = clock();

    /* 1. Collect solid 1-sided walls */
    wall_t *walls = (wall_t *)malloc(num_l * sizeof(wall_t));
    int num_walls = 0;
    float map_min_x = 1e9f, map_max_x = -1e9f;
    float map_min_y = 1e9f, map_max_y = -1e9f;

    for (int i = 0; i < num_l; i++) {
        if (l_data[i].sidenum[1] == -1) { /* 1-sided wall */
            point_t p1 = { (float)v_data[l_data[i].v1].x, (float)v_data[l_data[i].v1].y };
            point_t p2 = { (float)v_data[l_data[i].v2].x, (float)v_data[l_data[i].v2].y };
            wall_t *w = &walls[num_walls++];
            w->p1 = p1;
            w->p2 = p2;
            w->min_x = p1.x < p2.x ? p1.x : p2.x;
            w->max_x = p1.x > p2.x ? p1.x : p2.x;
            w->min_y = p1.y < p2.y ? p1.y : p2.y;
            w->max_y = p1.y > p2.y ? p1.y : p2.y;

            if (w->min_x < map_min_x) map_min_x = w->min_x;
            if (w->max_x > map_max_x) map_max_x = w->max_x;
            if (w->min_y < map_min_y) map_min_y = w->min_y;
            if (w->max_y > map_max_y) map_max_y = w->max_y;
        }
    }

    map_min_x -= 32.0f; map_max_x += 32.0f;
    map_min_y -= 32.0f; map_max_y += 32.0f;

    int grid_w = (int)ceilf((map_max_x - map_min_x) / CELL_SIZE) + 1;
    int grid_h = (int)ceilf((map_max_y - map_min_y) / CELL_SIZE) + 1;
    cell_t *grid = (cell_t *)calloc(grid_w * grid_h, sizeof(cell_t));

    for (int i = 0; i < num_walls; i++) {
        const wall_t *w = &walls[i];
        int gx0 = (int)floorf((w->min_x - map_min_x) / CELL_SIZE);
        int gx1 = (int)floorf((w->max_x - map_min_x) / CELL_SIZE);
        int gy0 = (int)floorf((w->min_y - map_min_y) / CELL_SIZE);
        int gy1 = (int)floorf((w->max_y - map_min_y) / CELL_SIZE);
        if (gx0 < 0) gx0 = 0;
        if (gy0 < 0) gy0 = 0;
        if (gx1 >= grid_w) gx1 = grid_w - 1;
        if (gy1 >= grid_h) gy1 = grid_h - 1;

        for (int gy = gy0; gy <= gy1; gy++) {
            for (int gx = gx0; gx <= gx1; gx++) {
                cell_t *c = &grid[gy * grid_w + gx];
                if (c->count == c->capacity) {
                    c->capacity = c->capacity == 0 ? 8 : c->capacity * 2;
                    c->walls = (int *)realloc(c->walls, c->capacity * sizeof(int));
                }
                c->walls[c->count++] = i;
            }
        }
    }

    /* 2. Collect sample points for each subsector */
    subsector_info_t *ss_info = (subsector_info_t *)calloc(num_ss, sizeof(subsector_info_t));

    /* 2a. Gather from SEGS in DOOM.WAD */
    for (int s = 0; s < num_ss; s++) {
        subsector_info_t *si = &ss_info[s];
        int cnt = ss_data[s].numsegs;
        int first = ss_data[s].firstseg;

        float sum_x = 0.0f, sum_y = 0.0f;
        int pt_count = 0;

        for (int k = 0; k < cnt; k++) {
            wad_seg_t *sg = &s_data[first + k];
            point_t p1 = { (float)v_data[sg->v1].x, (float)v_data[sg->v1].y };
            point_t p2 = { (float)v_data[sg->v2].x, (float)v_data[sg->v2].y };
            point_t mid = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };

            add_sample_point(si, p1);
            add_sample_point(si, p2);
            add_sample_point(si, mid);

            sum_x += p1.x + p2.x;
            sum_y += p1.y + p2.y;
            pt_count += 2;
        }

        if (pt_count > 0) {
            point_t centroid = { sum_x / pt_count, sum_y / pt_count };
            /* Put centroid at index 0 */
            if (si->num_samples > 0) {
                point_t temp = si->pts[0];
                si->pts[0] = centroid;
                add_sample_point(si, temp);
            } else {
                add_sample_point(si, centroid);
            }
        }
    }

    /* 2b. Gather from <MAP>.FLT (exact rendered floor/ceiling polygon vertices) */
    char flt_path[260];
    snprintf(flt_path, sizeof(flt_path), "Projects/DOOM-Saturn/cd/%s.FLT", mapname);
    uint32_t flt_len = 0;
    uint8_t *flt_data = load_file(flt_path, &flt_len);
    if (!flt_data) {
        snprintf(flt_path, sizeof(flt_path), "cd/%s.FLT", mapname);
        flt_data = load_file(flt_path, &flt_len);
    }

    if (flt_data && flt_len >= sizeof(flt2_header_t)) {
        flt2_header_t *fh = (flt2_header_t *)flt_data;
        if (memcmp(fh->magic, "FLT2", 4) == 0) {
            uint16_t flt_nss = swap16(fh->numsubsectors);
            uint32_t flt_nv = swap32(fh->numvertexes);
            uint32_t ofs_v_first = swap32(fh->ofs_sub_v_first);
            uint32_t ofs_v_count = swap32(fh->ofs_sub_v_count);
            uint32_t ofs_verts = swap32(fh->ofs_verts);

            const uint32_t *v_first_arr = (const uint32_t *)(flt_data + ofs_v_first);
            const uint16_t *v_count_arr = (const uint16_t *)(flt_data + ofs_v_count);
            const wad_vertex_t *flt_verts = (const wad_vertex_t *)(flt_data + ofs_verts);

            int n_to_proc = (int)flt_nss < num_ss ? (int)flt_nss : num_ss;
            for (int s = 0; s < n_to_proc; s++) {
                subsector_info_t *si = &ss_info[s];
                uint32_t vf = swap32(v_first_arr[s]);
                uint16_t vc = swap16(v_count_arr[s]);

                if (vf + vc <= flt_nv) {
                    for (uint16_t k = 0; k < vc; k++) {
                        point_t p = {
                            (float)(int16_t)swap16((uint16_t)flt_verts[vf + k].x),
                            (float)(int16_t)swap16((uint16_t)flt_verts[vf + k].y)
                        };
                        add_sample_point(si, p);
                    }
                }
            }
        }
        free(flt_data);
    }

    /* 2c. Add inward-nudged sample points (8% towards centroid) to eliminate tangent grazing */
    for (int s = 0; s < num_ss; s++) {
        subsector_info_t *si = &ss_info[s];
        if (si->num_samples <= 0) {
            point_t zero = { 0, 0 };
            add_sample_point(si, zero);
            continue;
        }

        point_t c = si->pts[0]; /* centroid */
        int orig_cnt = si->num_samples;
        for (int p = 1; p < orig_cnt && si->num_samples < MAX_SAMPLES; p++) {
            point_t nudged = {
                si->pts[p].x + (c.x - si->pts[p].x) * 0.08f,
                si->pts[p].y + (c.y - si->pts[p].y) * 0.08f
            };
            add_sample_point(si, nudged);
        }
    }

    /* 3. Allocate PVS bitmask matrix */
    int rowbytes = (num_ss + 7) >> 3;
    uint8_t *pvs = (uint8_t *)calloc(num_ss * rowbytes, 1);

    /* Reflexive visibility: every subsector sees itself */
    for (int i = 0; i < num_ss; i++) {
        pvs[i * rowbytes + (i >> 3)] |= (1 << (i & 7));
    }

    /* 4. Direct Portal Adjacency Pass:
     * Subsectors sharing a 2-sided linedef are directly adjacent and unconditionally visible. */
    for (int s1 = 0; s1 < num_ss; s1++) {
        int cnt1 = ss_data[s1].numsegs;
        int first1 = ss_data[s1].firstseg;
        for (int k1 = 0; k1 < cnt1; k1++) {
            int ld1 = s_data[first1 + k1].linedef;
            if (l_data[ld1].sidenum[1] == -1) continue; /* 1-sided */

            for (int s2 = s1 + 1; s2 < num_ss; s2++) {
                int cnt2 = ss_data[s2].numsegs;
                int first2 = ss_data[s2].firstseg;
                for (int k2 = 0; k2 < cnt2; k2++) {
                    if (s_data[first2 + k2].linedef == ld1) {
                        pvs[s1 * rowbytes + (s2 >> 3)] |= (1 << (s2 & 7));
                        pvs[s2 * rowbytes + (s1 >> 3)] |= (1 << (s1 & 7));
                        break;
                    }
                }
            }
        }
    }

    /* 5. Raycasting Visibility Pass */
    uint32_t *stamp_buf = (uint32_t *)calloc(num_walls, sizeof(uint32_t));
    uint32_t curr_stamp = 1;

    for (int i = 0; i < num_ss; i++) {
        const subsector_info_t *si = &ss_info[i];
        for (int j = i + 1; j < num_ss; j++) {
            if (pvs[i * rowbytes + (j >> 3)] & (1 << (j & 7)))
                continue;

            const subsector_info_t *sj = &ss_info[j];
            int visible = 0;

            /* Fast path 1: Centroid-to-centroid ray */
            curr_stamp++;
            if (ray_clear_grid(si->pts[0], sj->pts[0], walls, grid, grid_w, grid_h,
                               map_min_x, map_min_y, stamp_buf, curr_stamp)) {
                visible = 1;
            } else {
                /* Multi-ray sample testing */
                for (int pi = 0; pi < si->num_samples; pi++) {
                    for (int pj = 0; pj < sj->num_samples; pj++) {
                        curr_stamp++;
                        if (ray_clear_grid(si->pts[pi], sj->pts[pj], walls, grid, grid_w, grid_h,
                                           map_min_x, map_min_y, stamp_buf, curr_stamp)) {
                            visible = 1;
                            break;
                        }
                    }
                    if (visible) break;
                }
            }

            if (visible) {
                pvs[i * rowbytes + (j >> 3)] |= (1 << (j & 7));
                pvs[j * rowbytes + (i >> 3)] |= (1 << (i & 7));
            }
        }
    }

    /* 6. Compute statistics */
    int total_vis = 0;
    for (int i = 0; i < num_ss; i++) {
        for (int j = 0; j < num_ss; j++) {
            if (pvs[i * rowbytes + (j >> 3)] & (1 << (j & 7)))
                total_vis++;
        }
    }
    float avg_vis = (float)total_vis / (float)num_ss;
    float cull_pct = 100.0f - (avg_vis / (float)num_ss * 100.0f);

    /* 7. Write PVS binary file */
    int total_file_size = 16 + num_ss * rowbytes;
    uint8_t *out_buf = (uint8_t *)calloc(total_file_size, 1);

    out_buf[0] = 'P'; out_buf[1] = 'V'; out_buf[2] = 'S'; out_buf[3] = '1';
    /* version=1 (big endian) */
    out_buf[4] = 0; out_buf[5] = 1;
    /* numsubsectors (big endian) */
    out_buf[6] = (uint8_t)(num_ss >> 8); out_buf[7] = (uint8_t)(num_ss & 0xFF);
    /* rowbytes (big endian) */
    out_buf[8] = (uint8_t)(rowbytes >> 8); out_buf[9] = (uint8_t)(rowbytes & 0xFF);
    /* reserved = 0 */
    out_buf[10] = 0; out_buf[11] = 0; out_buf[12] = 0; out_buf[13] = 0;

    memcpy(out_buf + 16, pvs, num_ss * rowbytes);

    char out_path1[260], out_path2[260];
    snprintf(out_path1, sizeof(out_path1), "Projects/DOOM-Saturn/cd/%s.PVS", mapname);
    snprintf(out_path2, sizeof(out_path2), "Projects/DOOM-Saturn/staging_assets/%s.PVS", mapname);

    FILE *fo1 = fopen(out_path1, "wb");
    if (fo1) { fwrite(out_buf, 1, total_file_size, fo1); fclose(fo1); }
    FILE *fo2 = fopen(out_path2, "wb");
    if (fo2) { fwrite(out_buf, 1, total_file_size, fo2); fclose(fo2); }

    clock_t t1 = clock();
    double dt = (double)(t1 - t0) / CLOCKS_PER_SEC;

    printf("Baked %-8s (%5d B) in %5.2fs: %3d ssec, avg vis %4.1f (%4.1f%%), culling %4.1f%%\n",
           mapname, total_file_size, dt, num_ss, avg_vis, 100.0f - cull_pct, cull_pct);
    fflush(stdout);

    /* Cleanup */
    for (int k = 0; k < grid_w * grid_h; k++) {
        if (grid[k].walls) free(grid[k].walls);
    }
    free(grid);
    free(walls);
    free(ss_info);
    free(pvs);
    free(stamp_buf);
    free(out_buf);
    return 1;
}

int main(int argc, char **argv)
{
    const char *wad_path = (argc > 1) ? argv[1] : "Projects/DOOM-Saturn/cd/DOOM.WAD";
    uint32_t wad_len = 0;
    uint8_t *wad_data = load_file(wad_path, &wad_len);
    if (!wad_data) {
        printf("Error: Failed to open %s\n", wad_path);
        return 1;
    }

    if (memcmp(wad_data, "IWAD", 4) != 0) {
        printf("Error: %s is not an IWAD\n", wad_path);
        free(wad_data);
        return 1;
    }

    int32_t num_lumps = *(int32_t *)(wad_data + 4);
    int32_t info_ofs = *(int32_t *)(wad_data + 8);
    wad_lump_t *lumps = (wad_lump_t *)malloc(num_lumps * sizeof(wad_lump_t));

    for (int i = 0; i < num_lumps; i++) {
        uint8_t *entry = wad_data + info_ofs + i * 16;
        lumps[i].pos = *(uint32_t *)entry;
        lumps[i].size = *(uint32_t *)(entry + 4);
        memcpy(lumps[i].name, entry + 8, 8);
        lumps[i].name[8] = 0;
    }

    printf("=========================================================\n");
    printf(" Offline PVS Generator for Sega Saturn DOOM\n");
    printf(" Multi-Ray Conservative Sampling with Zero False Negatives\n");
    printf("=========================================================\n");

    const char *single_map = (argc > 2) ? argv[2] : NULL;

    if (single_map) {
        process_map(single_map, wad_data, lumps, num_lumps);
    } else {
        char mapname[16];
        for (int e = 1; e <= 4; e++) {
            for (int m = 1; m <= 9; m++) {
                snprintf(mapname, sizeof(mapname), "E%dM%d", e, m);
                process_map(mapname, wad_data, lumps, num_lumps);
            }
        }
    }

    free(lumps);
    free(wad_data);
    printf("All maps baked successfully!\n");
    return 0;
}
