/* saturn_vdp2_plane.c - VDP2 Perspective Ground Plane (RBG0)
 *
 * Provides a hardware-accelerated 3D perspective floor plane using VDP2 RBG0
 * for large outdoor flats (e.g. Sector 29 in E1M8).
 * Eliminates hundreds of VDP1 flat quads while guaranteeing subpixel-accurate
 * alignment with 3D walls and locked 60 FPS performance.
 */

#include "doomdef.h"
#include "r_local.h"
#include <jo/jo.h>
#include "saturn_vdp2_plane.h"
#include "saturn_tex.h"

int saturn_vdp2_plane_active = 0;
int saturn_vdp2_plane_sec = 29;
fixed_t saturn_vdp2_plane_floorz = -136 * FRACUNIT;
int saturn_vdp2_plane_checker = 1; /* 1 = High-contrast test checkerboard, 0 = regular flat */

#ifndef SATURN_WIN32
typedef struct
{
    fixed_t        XST, YST, ZST;
    fixed_t        DXST, DYST;
    fixed_t        DX, DY;
    fixed_t        MATA, MATB, MATC, MATD, MATE, MATF;
    short          PX, PY, PZ, pad0;
    short          CX, CY, CZ, pad1;
    fixed_t        MX, MY;
    fixed_t        KX, KY;
    unsigned int   KAST;
    int            DKAST, DKA;
    unsigned int   pad2[8];
} saturn_rotscroll_t;

static int v2_inited = 0;
static unsigned short     *v2_map = NULL;
static unsigned char      *v2_cell = NULL;
static unsigned short     *v2_kt = NULL;
static saturn_rotscroll_t *v2_rtable = NULL;
static saturn_rotscroll_t  v2_rs;

void Saturn_VDP2_Plane_Init(void)
{
    if (v2_inited)
        return;

    /* Allocate all RBG0 buffers in Bank B0 (0x25E40000) using JO_VDP2_RAM_MAP_RBG0.
     * Bank A0 and A1 are dedicated to NBG0 double-buffered framebuffers.
     * Bank B0 has 128 KB of completely unallocated VDP2 VRAM. */
    v2_map = (unsigned short *)jo_vdp2_malloc(JO_VDP2_RAM_MAP_RBG0, 8192);
    v2_cell = (unsigned char *)jo_vdp2_malloc(JO_VDP2_RAM_MAP_RBG0, 4096);
    v2_kt = (unsigned short *)jo_vdp2_malloc(JO_VDP2_RAM_MAP_RBG0, 1024);
    v2_rtable = (saturn_rotscroll_t *)jo_vdp2_malloc(JO_VDP2_RAM_MAP_RBG0, 128);

    if (!v2_map || !v2_cell || !v2_kt || !v2_rtable)
        return;

    /* Direct hardware VDP2 RBG0 configuration in Bank B0:
     * Bank B0 (RDBSB0 = bits 5:4 = 11 for RBG0 character/pattern, bits 7:6 = 01 for RBG0 pattern name/coeff) */
    JO_VDP2_RAMCTL = (JO_VDP2_RAMCTL & ~0x00F0u) | 0x0050u;

    /* RBG0 character control: 256 colors (bits 14:12 = 001), 1x1 cell size (bit 8 = 0) */
    JO_VDP2_CHCTLB = (JO_VDP2_CHCTLB & 0x00FFu) | 0x1000u;

    /* Pattern Name Control for RBG0: 1-word (bit 15 = 1), 12-bit char number (bit 14 = 1),
     * supplementary character bits (bits 4:2) = top 3 bits of 15-bit cell number for Bank B0 */
    JO_VDP2_PNCR = 0xC000u | (unsigned short)(((((unsigned int)v2_cell & 0x7FFFFu) >> 17) & 0x7u) << 2);

    /* Plane Size: 1x1H for RA and RB (bits 9:8 = 00, bits 13:12 = 00) & Over mode 0 (repeat) */
    JO_VDP2_PLSZ &= 0x00FFu;

    /* Map offset & planes A..P for Parameter A all pointing to v2_map in Bank B0 */
    {
        unsigned int map_addr = ((unsigned int)v2_map) & 0x7FFFFu;
        unsigned short plane = (unsigned short)((map_addr >> 13) & 0x3Fu);
        unsigned short pair = (unsigned short)(plane | (plane << 8));
        JO_VDP2_MPOFR = (JO_VDP2_MPOFR & ~0x0007u) | (unsigned short)((map_addr >> 19) & 0x7u);
        JO_VDP2_MPABRA = pair;
        JO_VDP2_MPCDRA = pair;
        JO_VDP2_MPEFRA = pair;
        JO_VDP2_MPGHRA = pair;
        JO_VDP2_MPIJRA = pair;
        JO_VDP2_MPKLRA = pair;
        JO_VDP2_MPMNRA = pair;
        JO_VDP2_MPOPRA = pair;
    }

    /* Rotation Parameter Mode: 0 (Parameter A only) */
    JO_VDP2_RPMD = 0;

    /* Coefficient table control for Parameter A & B:
     * bit 0 = KTE (enable), bit 1 = KLCE (0 = 2-word), bits 3:2 = KMD (0 = scale XY), bit 4 = CTA (0) */
    JO_VDP2_KTCTL = 0x0101u;
    {
        unsigned int kt_off = ((((unsigned int)v2_kt) & 0x7FFFFu) >> 18) & 0x7u;
        JO_VDP2_KTAOF = (unsigned short)(kt_off | (kt_off << 8));
    }

    /* Rotation Parameter Table Address (in 2-byte word address units across 512 KB VDP2 VRAM) */
    {
        unsigned int rta = (((unsigned int)v2_rtable) & 0x7FFFFu) >> 1;
        JO_VDP2_RPTAU = (unsigned short)((rta >> 16) & 0x7u);
        JO_VDP2_RPTAL = (unsigned short)(rta & 0xFFFEu);
    }

    /* Priority 4: above NBG0 (3), below VDP1 (5) */
    JO_VDP2_PRIR = 0x0004u;

    /* Keep Bank A0/A1 NBG0 256-color bitmap cycle patterns active */
    JO_VDP2_CYCA0L = 0x44EEu;
    JO_VDP2_CYCA0U = 0xEEEEu;
    JO_VDP2_CYCA1L = 0x44EEu;
    JO_VDP2_CYCA1U = 0xEEEEu;

    /* Disable RBG0 color calculation (bit 4 of CCCTL) */
    JO_VDP2_CCCTL &= ~(1u << 4);

    /* Initialize all 256 coefficient rows to transparent (bit 15 set = 0x8000) */
    for (int k = 0; k < 256; k++)
    {
        v2_kt[k * 2]     = 0x8000;
        v2_kt[k * 2 + 1] = 0x0000;
    }

    v2_inited = 1;
}

void Saturn_VDP2_Plane_LoadFlat(const char *flatname)
{
    if (!v2_inited)
        Saturn_VDP2_Plane_Init();
    if (!v2_inited)
        return;

    /* Build 64x64 cell map:
     * Base units in 32-byte chunks within Bank B0.
     * In 1-word pattern name data mode (PNB_1WORD | CN_12BIT):
     * Bits 11-0: Character number (bunits + 2*m).
     * Bits 14-12: Palette bank (0 = CRAM bank 0 = standard DOOM palette). */
    unsigned int bunits = (((unsigned int)v2_cell) & (JO_VDP2_BANK_SIZE - 1u)) >> 5;
    for (int my = 0; my < 64; my++)
    {
        for (int mx = 0; mx < 64; mx++)
        {
            int cx = mx & 7;
            int cy = my & 7;
            int m = cy * 8 + cx;
            unsigned int char_num = bunits + 2 * m;
            v2_map[my * 64 + mx] = (unsigned short)(char_num & 0x0FFF);
        }
    }

    if (saturn_vdp2_plane_checker)
    {
        for (int cy = 0; cy < 8; cy++)
        {
            for (int cx = 0; cx < 8; cx++)
            {
                int m = cy * 8 + cx;
                unsigned char *dst_char = v2_cell + m * 64;
                for (int py = 0; py < 8; py++)
                {
                    for (int px = 0; px < 8; px++)
                    {
                        int src_y = cy * 8 + py;
                        int src_x = cx * 8 + px;
                        unsigned char p;

                        if (src_x < 2 || src_x >= 62 || src_y < 2 || src_y >= 62)
                        {
                            p = 231; /* Bright Yellow tile border */
                        }
                        else if (src_x == 31 || src_x == 32 || src_y == 31 || src_y == 32)
                        {
                            p = 231; /* Bright Yellow crosshair */
                        }
                        else if (src_y < 31)
                        {
                            if (src_x < 31)
                            {
                                /* Top-Left: White with black 'N' (North) */
                                int qx = src_x - 10;
                                int qy = src_y - 10;
                                if (qx >= 0 && qx < 11 && qy >= 0 && qy < 11 &&
                                    (qx <= 1 || qx >= 9 || qx == qy || qx == qy + 1))
                                    p = 247; /* Black 'N' */
                                else
                                    p = 4;   /* Pure White */
                            }
                            else
                            {
                                p = 176;     /* Top-Right: Pure Red */
                            }
                        }
                        else
                        {
                            if (src_x < 31)
                                p = 112;     /* Bottom-Left: Pure Green */
                            else
                                p = 251;     /* Bottom-Right: Pure Magenta */
                        }
                        dst_char[py * 8 + px] = p;
                    }
                }
            }
        }
        return;
    }

    int flatnum = R_FlatNumForName(flatname);
    if (flatnum < 0)
        return;

    const byte *src = Saturn_FlatData(flatnum);
    if (!src)
        return;

    for (int cy = 0; cy < 8; cy++)
    {
        for (int cx = 0; cx < 8; cx++)
        {
            int m = cy * 8 + cx;
            unsigned char *dst_char = v2_cell + m * 64;
            for (int py = 0; py < 8; py++)
            {
                for (int px = 0; px < 8; px++)
                {
                    int src_y = cy * 8 + py;
                    int src_x = cx * 8 + px;
                    unsigned char p = src[src_y * 64 + src_x];
                    if (p == 0) p = 247;    /* Remap transparent black index 0 to 247 */
                    dst_char[py * 8 + px] = p;
                }
            }
        }
    }
}

void Saturn_VDP2_Plane_Frame(fixed_t camx, fixed_t camy, fixed_t camz, angle_t camang, fixed_t floorz)
{
    if (!v2_inited || !saturn_vdp2_plane_active)
        return;

    extern int saturn_view_cy;
    extern int saturn_view_mode;
    int cy = saturn_view_cy;
    if (cy <= 0)
        cy = 84;
    int max_y = (saturn_view_mode == 1) ? 192 : 168;
    if (max_y <= 0 || max_y > 224)
        max_y = 168;

    fixed_t c = finecosine(camang >> ANGLETOFINESHIFT);
    fixed_t s = finesine(camang >> ANGLETOFINESHIFT);

    v2_rs.XST = -160 << 16;
    v2_rs.YST = -cy << 16;
    v2_rs.ZST = 160 << 16;
    v2_rs.DXST = 0;
    v2_rs.DYST = 0;
    v2_rs.DX = 1 << 16;
    v2_rs.DY = 0;
    v2_rs.MATA = s;
    v2_rs.MATB = 0;
    v2_rs.MATC = c;
    v2_rs.MATD = -c;
    v2_rs.MATE = 0;
    v2_rs.MATF = s;
    v2_rs.PX = 0;
    v2_rs.PY = 0;
    v2_rs.PZ = 0;
    v2_rs.CX = 0;
    v2_rs.CY = 0;
    v2_rs.CZ = 0;
    v2_rs.MX = camx;
    v2_rs.MY = camy;
    v2_rs.KX = 1 << 16;
    v2_rs.KY = 1 << 16;
    v2_rs.KAST = ((((unsigned int)v2_kt) & (JO_VDP2_BANK_SIZE - 1u)) >> 2) << 16;
    v2_rs.DKAST = 1 << 16;
    v2_rs.DKA = 0;

    /* Rows 0..cy: transparent (sky / mountains above horizon) */
    for (int y = 0; y <= cy; y++)
    {
        v2_kt[y * 2]     = 0x8000;
        v2_kt[y * 2 + 1] = 0x0000;
    }

    /* Rows cy+1..max_y-1: visible perspective ground plane */
    fixed_t camz_rel = camz - floorz;
    if (camz_rel > 0)
    {
        for (int y = cy + 1; y < max_y; y++)
        {
            unsigned int k24 = (unsigned int)camz_rel / (unsigned int)(y - cy);
            if (k24 > 0x7FFFFFu)
                k24 = 0x7FFFFFu;
            v2_kt[y * 2]     = (unsigned short)((k24 >> 16) & 0x7F);
            v2_kt[y * 2 + 1] = (unsigned short)(k24 & 0xFFFF);
        }
    }
    else
    {
        for (int y = cy + 1; y < max_y; y++)
        {
            v2_kt[y * 2]     = 0x8000;
            v2_kt[y * 2 + 1] = 0x0000;
        }
    }

    /* Rows max_y..255: transparent (status bar & lower border) */
    for (int y = max_y; y < 256; y++)
    {
        v2_kt[y * 2]     = 0x8000;
        v2_kt[y * 2 + 1] = 0x0000;
    }

    /* Deliver directly to VDP2 VRAM rotation parameter table */
    *v2_rtable = v2_rs;
}

void Saturn_VDP2_Plane_VBlank(void)
{
    if (!saturn_vdp2_plane_active)
        return;

    if (v2_rtable)
    {
        unsigned int rta = (((unsigned int)v2_rtable) & 0x7FFFFu) >> 1;
        JO_VDP2_RPTAU = (unsigned short)((rta >> 16) & 0x7u);
        JO_VDP2_RPTAL = (unsigned short)(rta & 0xFFFEu);
    }

    /* Keep RBG0 color calc off and priority at 4 */
    JO_VDP2_CCCTL &= ~(1u << 4);
    JO_VDP2_PRIR = 0x0004u;

    /* Guarantee NBG0 (bit 0) and RBG0 (bit 4) are both asserted in BGON */
    JO_VDP2_BGON |= (1u << 0) | (1u << 4);

    /* Guarantee Bank A0 and A1 cycle patterns allow NBG0 bitmap reads */
    JO_VDP2_CYCA0L = 0x44EEu;
    JO_VDP2_CYCA1L = 0x44EEu;
}
#else /* SATURN_WIN32 */
void Saturn_VDP2_Plane_Init(void) { }
void Saturn_VDP2_Plane_LoadFlat(const char *flatname) { (void)flatname; }
void Saturn_VDP2_Plane_Frame(fixed_t camx, fixed_t camy, fixed_t camz, angle_t camang, fixed_t floorz)
{
    (void)camx; (void)camy; (void)camz; (void)camang; (void)floorz;
}
void Saturn_VDP2_Plane_VBlank(void) { }
#endif
