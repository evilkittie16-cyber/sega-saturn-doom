/* saturn_baked_segs.h - Zero-Runtime-Math Baked Walls & Seg Loader for Sega Saturn
 *
 * Implements:
 *   - Offline .WL (MWAL) binary parser
 *   - Subsector 2D floorplan pillars
 *   - Load-time pointer swizzler (Trap 1 resolution)
 *   - 1D Wall LOD (Law 1 & Law 2)
 */

#ifndef SATURN_BAKED_SEGS_H
#define SATURN_BAKED_SEGS_H

#include <stdint.h>
#include "r_local.h"
#include "saturn_memmap.h"

#define WRAM_L_SEG_ADDR   ((byte *)SATURN_WRAM_L_SEG_ADDR)

/* Wall Type & Pegging Flags (matching bake_walls.py) */
#define SEG_FLAG_SOLID    0x01
#define SEG_FLAG_UPPER    0x02
#define SEG_FLAG_LOWER    0x04
#define SEG_FLAG_MID      0x08
#define SEG_PEG_BOTTOM         0x10  /* Bottom-anchored: v_bot = th + rowoff, v_top = v_bot - wall_h */
#define SEG_PEG_LOWER_UNPEGGED 0x20  /* Lower unpegged: v_top = (front.ceil - back.floor) + rowoff */
#define SEG_FLAG_PORTAL_DONE   0x80  /* Portal aperture already recorded by earlier seg on same line */

/* Disc Header: 32 bytes (Big-endian) */
typedef struct {
    char     magic[4];          /* "MWAL" */
    uint16_t version;           /* 1 */
    uint16_t num_subsectors;
    uint32_t total_size;
    uint32_t ofs_subsectors;
    uint32_t ofs_pillars;
    uint32_t ofs_segs;
    uint32_t reserved[2];       /* [0] = ofs_pillar_frac (seam fix), 0 if absent */
} saturn_wl_header_t;

/* Subsector Header: 16 bytes (Big-endian) */
typedef struct {
    uint16_t num_pillars;
    uint16_t pillar_start;
    uint16_t num_segs;
    uint16_t seg_start;
    union {
        uint16_t num_dsp_batches;   /* Disc: SCU-DSP batch count */
        uint16_t portal_first;      /* Runtime: first unbaked portal seg index */
    };
    union {
        uint16_t max_batch_size;    /* Disc: Max pillars per batch */
        uint16_t portal_count;      /* Runtime: unbaked portal seg count */
    };
    union {
        uint32_t reserved;          /* Alignment padding to 16 bytes */
        struct {
            int16_t  max_bsec0;     /* Runtime: highest backsector 0 if lower-only sky subsector (-1 if none) */
            int16_t  max_bsec1;     /* Runtime: highest backsector 1 if lower-only sky subsector */
        };
    };
} saturn_wl_subsector_t;

/* 2D Floorplan Pillar: 4 bytes (int16 x, int16 z) */
typedef struct {
    int16_t x;
    int16_t z;
} saturn_wl_pillar_t;

/* Disc Seg: Exactly 32 bytes on disc */
typedef struct {
    uint16_t pillar_idx[5];        /* [0]=0, [1]=16, [2]=32, [3]=48, [4]=64 */
    uint8_t  baked_light;          /* Pre-baked sector light + directional fake contrast */
    uint8_t  flags;                /* SEG_FLAG_* */
    uint16_t tex_64;               /* VDP1 texture index for 64-wide strip */
    uint16_t tex_32;               /* VDP1 texture index for 32-wide strip */
    uint16_t tex_16;               /* VDP1 texture index for 16-wide strip */
    int16_t  mid_static_v_offset;  /* Pre-calculated vertical texture offset */
    uint16_t peg_sec;              /* Sector index for pegging height */
    uint16_t top_sec;              /* Sector index for top ceiling height */
    uint16_t bot_sec;              /* Sector index for bottom floor height */
    uint16_t u_start;              /* Texture U start */
    uint16_t u_span;               /* Physical segment width (<= 64 units) */
    uint16_t reserved;             /* Padding for 32-byte alignment */
} __attribute__((aligned(4))) packed_seg_disc_t;

/* Runtime In-Memory Format (16 bytes = 1 single SH-2 L1 cache line) */
typedef struct {
    uint8_t  pillar_idx[2];        /* 2B: [0]=start pillar, [1]=end pillar */
    uint8_t  baked_light;          /* 1B: Sector light + fake contrast */
    uint8_t  flags;                /* 1B: SEG_FLAG_* */
    uint16_t tex_64;               /* 2B: VDP1 texture index for 64-wide */
    int16_t  mid_static_v_offset;  /* 2B: Pre-calculated vertical texture offset */
    uint16_t u_start;              /* 2B: Texture U start */
    uint16_t u_span;               /* 2B: Segment width */
    uint16_t inv_u_span;           /* 2B: Precalculated 16.16 reciprocal (65536 / u_span) */
    int16_t  back_sec_idx;         /* 2B: Back sector index for 2-sided portal, or -1 if 1-sided */
} __attribute__((aligned(16))) baked_seg_t;

/* Engine status flags */
extern int saturn_baked_segs_active;   /* Legacy SEGS active */
extern int saturn_baked_walls_active;  /* Master Blueprint MWAL active */

/* Initialization & Parsing API */
void Saturn_BakedSegs_Init(const char *lumpname);
int  Saturn_BakedSegs_Parse(void *data, unsigned len);
int  Saturn_BakedWalls_Parse(void *data, unsigned len);

/* Accessor API for Saturn Hardware Renderer */
int  Saturn_BakedWalls_Active(void);
int  Saturn_BakedWalls_NumSubsectors(void);
const saturn_wl_subsector_t *Saturn_BakedWalls_GetSubsector(int ssec_idx);
const saturn_wl_pillar_t    *Saturn_BakedWalls_GetPillars(void);
const baked_seg_t           *Saturn_BakedWalls_GetSegs(void);
/* Seam fix: per-pillar 1/16-unit fractions ((fx << 4) | fz), parallel to
 * Saturn_BakedWalls_GetPillars(). Header reserved[0] = offset (0 = absent;
 * old files get an all-zero array so the renderer never branches). */
const uint8_t               *Saturn_BakedWalls_GetPillarFrac(void);

/* Direct array access for zero-overhead inlined wall rendering & traversal */
extern const saturn_wl_subsector_t *saturn_baked_wall_subsectors;
extern const saturn_wl_pillar_t    *saturn_baked_wall_pillars;
extern const uint8_t               *saturn_baked_wall_pillar_frac;
extern baked_seg_t                 *saturn_baked_wall_segs;
extern int                          saturn_baked_wall_num_subsectors;
extern int                          saturn_baked_wall_num_pillars;
extern int                          saturn_baked_wall_num_segs;

#endif /* SATURN_BAKED_SEGS_H */
