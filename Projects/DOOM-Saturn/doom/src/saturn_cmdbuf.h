/* saturn_cmdbuf.h - Unified VDP1 Command Buffers & Topological Range Table
 *
 * Implements:
 *   - Final 32-byte VDP1 hardware packet definition (vdp1_cmd_t).
 *   - Fast WRAM-H command buffers:
 *       cmd_m: Master walls and subsector sprites (1,024 slots)
 *       cmd_s: Slave floor & ceiling flats (768 slots)
 *       cmd_w: Dedicated weapon/psprite overlay buffer (4 slots)
 *   - Topological subsector range table for reverse painter's delivery.
 */

#ifndef __SATURN_CMDBUF_H__
#define __SATURN_CMDBUF_H__

#include <stdint.h>
#include "saturn_memmap.h"

/* VDP1 Command Packet: exactly 32 bytes (big-endian) */
typedef struct {
    uint16_t ctrl;   /* 0x00: END(15) | JP(14:12) | ZP(11:8) | DIR(5:4) | COMM(3:0) */
    uint16_t link;   /* 0x02: Jump target / 8 */
    uint16_t pmod;   /* 0x04: MON(15) HSS(12) PCLP(11) CLIP(10) CMOD(9) MESH(8) ECD(7) SPD(6) CM(5:3) CC(2:0) */
    uint16_t colr;   /* 0x06: Colour bank: (k_shade << 8) */
    uint16_t srca;   /* 0x08: Texture address / 8 */
    uint16_t size;   /* 0x0A: (width / 8) << 8 | height */
    int16_t  xa;     /* 0x0C */
    int16_t  ya;     /* 0x0E */
    int16_t  xb;     /* 0x10 */
    int16_t  yb;     /* 0x12 */
    int16_t  xc;     /* 0x14 */
    int16_t  yc;     /* 0x16 */
    int16_t  xd;     /* 0x18 */
    int16_t  yd;     /* 0x1A */
    uint16_t grda;   /* 0x1C: Gouraud table address / 8 (0) */
    uint16_t dmmy;   /* 0x1E: Reserved / 0 */
} __attribute__((aligned(32))) vdp1_cmd_t;

#if defined(__GNUC__) && !defined(__cplusplus)
_Static_assert(sizeof(vdp1_cmd_t) == 32, "vdp1_cmd_t must be exactly 32 bytes");
#endif

/* Command Control Codes (CTRL COMM bits 3:0) */
#define CMD_NORMAL_SPRITE    0x0000u
#define CMD_SCALED_SPRITE    0x0001u
#define CMD_DISTORTED        0x0002u
#define CMD_POLYGON          0x0004u
#define CMD_POLYLINE         0x0005u
#define CMD_LINE             0x0006u
#define CMD_USERCLIP         0x0008u
#define CMD_SYSTEMCLIP       0x0009u
#define CMD_LOCALCOORD       0x000Au
#define CMD_END              0x8000u

/* PMOD flags */
#define PMOD_HSS             (1u << 12)
#define PMOD_PCLP            (1u << 11)
#define PMOD_CLIP_IN         (2u << 9)   /* V1_PMOD_WINDOW_IN */
#define PMOD_WALL            (PMOD_HSS | 0x0080u | 0x0040u | (4u << 3)) /* HSS | PCLP=0 (hw pre-clip enabled) | ECD | SPD | CM=4 (8bpp 256 colors) */

/* Command Buffer Capacities */
#define CMDBUF_M_CAP         1280
#define CMDBUF_S_CAP         1024
#define HW_SLAVE_FLAT_Q_CAP  768   /* Slave flat quad budget; remaining 256 for Master stealing */
#define CMDBUF_W_CAP         4
#define CMDBUF_MAX_VISIT     384

#if HW_SLAVE_FLAT_Q_CAP >= CMDBUF_S_CAP
#error "HW_SLAVE_FLAT_Q_CAP must be strictly less than CMDBUF_S_CAP!"
#endif

/* Subsector Range Bookkeeping (recorded in front-to-back traversal order) */
typedef struct {
    uint16_t m_start;   /* Master walls/sprites start index in cmd_m */
    uint16_t m_count;   /* Number of commands emitted by Master */
    uint16_t s_start;   /* Slave flats start index in cmd_s */
    uint16_t s_count;   /* Number of commands emitted by Slave */
    uint16_t subnum;    /* Subsector number for debugging / profiling */
    int16_t  cz;        /* Camera space depth (for Painter's delivery sort) */
    uint32_t dummy;     /* Pad to 16 bytes for single-cycle SH-2 index shift */
} __attribute__((aligned(16))) sub_range_t;

#if defined(__GNUC__) && !defined(__cplusplus)
_Static_assert(sizeof(sub_range_t) == 16, "sub_range_t must be exactly 16 bytes for bit-shift indexing");
#elif defined(_MSC_VER)
static_assert(sizeof(sub_range_t) == 16, "sub_range_t must be exactly 16 bytes for bit-shift indexing");
#endif

/* Global Command Buffers and Range Table */
extern vdp1_cmd_t  *saturn_cmd_m;
extern vdp1_cmd_t  *saturn_cmd_s;
extern vdp1_cmd_t  *saturn_cmd_w;
extern sub_range_t *saturn_sub_ranges;
extern volatile uint16_t saturn_cmd_m_count;
extern volatile uint16_t saturn_cmd_s_count;
extern volatile uint16_t saturn_cmd_w_count;
extern int saturn_visit_count;

void Saturn_CmdBuf_Init(void);
void Saturn_CmdBuf_Reset(void);
void Saturn_CmdBuf_CullOccluded(void);
unsigned int Saturn_CmdBuf_ComputeDDA(void);
void Saturn_CmdBuf_Deliver(void);
void Saturn_VDP1_BurstCopy32(uint32_t **dst_ptr, const uint32_t **src_ptr);

#endif /* __SATURN_CMDBUF_H__ */
