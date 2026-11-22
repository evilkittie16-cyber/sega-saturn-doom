/* saturn_vdp2_plane.c - VDP2 Perspective Ground Plane (RBG0)
 *
 * Provides a hardware-accelerated 3D perspective floor plane using VDP2 RBG0
 * for large outdoor flats (e.g. Sector 29 in E1M8).
 * Eliminates hundreds of VDP1 flat quads while guaranteeing subpixel-accurate
 * alignment with 3D walls and locked 60 FPS performance.
 */

#include <string.h>
#include "doomdef.h"
#include "r_local.h"
#include <jo/jo.h>
#include "saturn_vdp2_plane.h"
#include "saturn_tex.h"
#include "p_local.h"    /* p_spec.h anims[] (animated flats can't use the static plane bitmap), P_GetSectorTag */

int saturn_vdp2_plane_active = 0;
int saturn_vdp2_plane_sec = 29;
fixed_t saturn_vdp2_plane_floorz = -136 * FRACUNIT;
int saturn_vdp2_plane_checker = 1; /* 1 = High-contrast test checkerboard, 0 = regular flat */
/* Full RAMCTL value (the register is write-only: never read-modify-write it).
 * i_video.c writes it at every flip; the plane adds its RBG0 bank roles. */
unsigned short saturn_vdp2_ramctl = SATURN_VDP2_RAMCTL_BASE;
volatile unsigned char saturn_vdp2_rbg0_on = 0;
unsigned char saturn_vdp2_plane_secmask[SATURN_V2P_MAX_SECTORS];

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
/* what each slot's coefficient table / light banks were last built for */
static struct { fixed_t camz_rel; int16_t cy, max_y, light; } v2_slot_key[SATURN_V2P_SLOTS];
static uint32_t v2_slot_gen[SATURN_V2P_SLOTS];      /* bumped on every rebuild */
static uint32_t v2_inv[256];                        /* 2^32 / n (n >= 1) */
static unsigned short     *v2_kts[SATURN_V2P_SLOTS];
static saturn_rotscroll_t *v2_rtables[SATURN_V2P_SLOTS];
static unsigned short     *v2_kt = NULL;        /* slot being written */
static saturn_rotscroll_t *v2_rtable = NULL;    /* slot shown at init */
static saturn_rotscroll_t  v2_rs;
volatile int saturn_v2p_last = -1, saturn_v2p_pending = -1, saturn_v2p_drawing = -1, saturn_v2p_shown = -1;

/* Per-scanline lighting (HW_VDP2_PLANE_LIGHT): PC Doom lights a floor per
 * screen row (row -> distance -> zlight). RBG0 reads the same 8 shaded CRAM
 * banks as the VDP1 flats through CRAOFB.R0CAOS (bits 2-0), so each frame's
 * slot holds a bank per display line, taken from the VDP1 flats' own table
 * (flat_zlight_cmdcolr_LUT[light][depth >> 20], bank = bits 10-8). At every
 * vblank the shown slot's banks are laid out per raster line into a DMA
 * buffer that SCU DMA level 2 copies into CRAOFA/CRAOFB at each HBlank-IN
 * (start factor 2), so line N+1 is scanned with its own bank. */
#ifndef HW_VDP2_PLANE_LIGHT
#define HW_VDP2_PLANE_LIGHT 1
#endif
#if HW_VDP2_PLANE_LIGHT
#include "saturn_light_lut.h"
#define V2_LINES     263            /* NTSC raster lines per field */
static uint8_t  v2_bank[SATURN_V2P_SLOTS][256];
/* two fields by absolute raster line: a start anywhere in the first field
 * leaves a full field (+) of entries before the next vblank re-aims it */
static uint32_t v2_light_dma[2 * V2_LINES] __attribute__((aligned(16)));
static uint8_t  v2_dma_on = 0;      /* SCU DMA level 2 armed by this module */
static int      v2_dma_slot = -1;   /* slot / generation v2_light_dma was built from */
static uint32_t v2_dma_gen = 0;
#define SCU_D2R   (*(volatile uint32_t *)0x25FE0040u)
#define SCU_D2W   (*(volatile uint32_t *)0x25FE0044u)
#define SCU_D2C   (*(volatile uint32_t *)0x25FE0048u)
#define SCU_D2AD  (*(volatile uint32_t *)0x25FE004Cu)
#define SCU_D2EN  (*(volatile uint32_t *)0x25FE0050u)
#define SCU_D2MD  (*(volatile uint32_t *)0x25FE0054u)
#define V2_VCNT   (*(volatile uint16_t *)0x25F8000Au)
#define V2_TVSTAT (*(volatile uint16_t *)0x25F80004u)   /* readable status: bit 2 HBLANK */
#endif
#if defined(HW_VDP2_PLANE_TEST) && HW_VDP2_PLANE_TEST
/* sync test log, one entry per vblank: angle (>> 16) of the newest built
 * frame (old behaviour), of the shown plane slot, of the shown VDP1 list */
static uint32_t v2_slot_ang[SATURN_V2P_SLOTS];
volatile uint32_t saturn_v2p_list_ang[3];          /* pending, drawing, shown */
volatile uint32_t saturn_v2p_frame_ang;            /* camera angle of the frame being built */
volatile uint16_t saturn_v2p_log[64][4];
volatile uint32_t saturn_v2p_log_n;
#endif

/* VRAM layout (hardware rule, VDP2 manual §3.3 / RAMCTL RDBS: in rotation
 * mode each bank serves RBG0 in ONE role -- 00 unused, 01 coefficient table,
 * 10 pattern name table, 11 character / bitmap -- and RBG0 data in a bank
 * with another role is simply not fetched; 2026-11-17 hardware test: the old
 * map + cells + tables all in B0 with B0 = "coefficient" showed no floor).
 *   A0, A1  NBG0 double-buffered framebuffers (RDBS 00)
 *   B0      RBG0 bitmap, 512x256 8bpp = the whole bank (RDBS 11)
 *   B1      tail: rotation parameter + coefficient tables (RDBS 01; Jo's own
 *           plane keeps its parameter table in its coefficient bank too)
 * VDP2 registers are write-only: every register here is written whole. */
#define V2_BITMAP      ((volatile uint32_t *)0x25E40000u)           /* B0 */
#define V2_TABLES      ((unsigned char *)0x25E7E000u)               /* B1 + 0x1E000 */
#define V2_RAMCTL_RBG0 (0x0030u | 0x0040u)                          /* B0 = bitmap, B1 = coefficient */

void Saturn_VDP2_Plane_Init(void)
{
    if (v2_inited)
        return;

    for (int s = 0; s < SATURN_V2P_SLOTS; s++)
    {
        v2_rtables[s] = (saturn_rotscroll_t *)(V2_TABLES + s * 128);
        v2_kts[s] = (unsigned short *)(V2_TABLES + SATURN_V2P_SLOTS * 128 + s * 1024);
    }
    v2_kt = v2_kts[0];
    v2_rtable = v2_rtables[0];
    /* ceil(2^32 / n): (c * inv) >> 32 == c / n exactly while c * (inv * n - 2^32)
     * < 2^32, i.e. for eye heights up to ~256 units above the plane (1 LSB of
     * the 16.16 coefficient above that) */
    for (int n = 1; n < 256; n++)
        v2_inv[n] = 0xFFFFFFFFu / (unsigned int)n + 1u;
    for (int s = 0; s < SATURN_V2P_SLOTS; s++)
        v2_slot_key[s].cy = -1;             /* force a full first build */

    saturn_vdp2_ramctl = SATURN_VDP2_RAMCTL_BASE | V2_RAMCTL_RBG0;
    JO_VDP2_RAMCTL = saturn_vdp2_ramctl;

    /* RBG0: 256 colours (R0CHCN = 001, bits 14:12), bitmap (R0BMEN, bit 9),
     * 512x256 (R0BMSZ = 0, bit 10); NBG2/NBG3 fields (low byte) unused = 0 */
    JO_VDP2_CHCTLB = 0x1200u;
    JO_VDP2_BMPNB  = 0x0000u;          /* RBG0 bitmap palette number 0 */
    /* Over mode 0 (RAOVR = 00: the bitmap repeats); plane sizes unused */
    JO_VDP2_PLSZ   = 0x0000u;
    /* RBG0 bitmap start: map offset (MPOFR RAMP) x 0x20000 = B0 */
    JO_VDP2_MPOFR  = (unsigned short)(((0x25E40000u & 0x7FFFFu) >> 17) & 0x7u);

    /* Rotation Parameter Mode: 0 (Parameter A only) */
    JO_VDP2_RPMD = 0;

    /* Coefficient table, parameter A only: RAKTE (bit 0), longword data
     * (bit 1 = 0), KMD = 0 (scale X and Y), no line colour */
    JO_VDP2_KTCTL = 0x0001u;
    /* KTAOS selects the 256 KB half (longword tables); KAST indexes 4-byte
     * entries from there (see v2_kast()) */
    JO_VDP2_KTAOF = (unsigned short)((((unsigned int)V2_TABLES & 0x7FFFFu) >> 18) & 0x7u);

    /* Rotation Parameter Table Address (in 2-byte word address units across 512 KB VDP2 VRAM) */
    {
        unsigned int rta = (((unsigned int)v2_rtable) & 0x7FFFFu) >> 1;
        JO_VDP2_RPTAU = (unsigned short)((rta >> 16) & 0x7u);
        JO_VDP2_RPTAL = (unsigned short)(rta & 0xFFFEu);
    }

    /* Priority 4: above NBG0 (3), below VDP1 (5); RBG1 0 */
    JO_VDP2_PRIR = 0x0004u;

    /* Keep Bank A0/A1 NBG0 256-color bitmap cycle patterns active (B0/B1
     * cycle patterns are ignored while the banks carry RBG0 data) */
    JO_VDP2_CYCA0L = 0x44EEu;
    JO_VDP2_CYCA0U = 0xEEEEu;
    JO_VDP2_CYCA1L = 0x44EEu;
    JO_VDP2_CYCA1U = 0xEEEEu;

    /* No colour calculation on any screen (the game uses none) */
    JO_VDP2_CCCTL = 0x0000u;

    /* Initialize all 256 coefficient rows of every slot to transparent (bit 15 set = 0x8000) */
    for (int s = 0; s < SATURN_V2P_SLOTS; s++)
        for (int k = 0; k < 256; k++)
        {
            v2_kts[s][k * 2]     = 0x8000;
            v2_kts[s][k * 2 + 1] = 0x0000;
        }

    v2_inited = 1;
}

static void v2_load_flatnum(int flatnum);

void Saturn_VDP2_Plane_LoadFlat(const char *flatname)
{
    v2_load_flatnum(saturn_vdp2_plane_checker ? -1 : R_FlatNumForName(flatname));
}

/* Level load: pick the plane's sector group (see saturn_vdp2_plane.h). */
#ifndef HW_VDP2_PLANE_MIN_PCT
#define HW_VDP2_PLANE_MIN_PCT 15       /* group's share of the level's LOD0 floor quads */
#endif
#ifndef HW_VDP2_PLANE_MIN_QUADS
#define HW_VDP2_PLANE_MIN_QUADS 150
#endif
extern const uint16_t *saturn_baked_flat_q_count;

static int v2_flat_animated(int pic)
{
    for (const anim_t *a = anims; a < lastanim; a++)
        if (!a->istexture && pic >= a->basepic && pic <= a->picnum)
            return 1;
    return 0;
}

static int v2_sector_eligible(sector_t *s)
{
    static const uint8_t light_fx[] = { 1, 2, 3, 4, 8, 12, 13, 17 };
    if ((int16_t)s->floorpic < 0 || v2_flat_animated(s->floorpic))
        return 0;
    for (unsigned i = 0; i < sizeof(light_fx); i++)
        if (s->special == light_fx[i])
            return 0;                   /* light changes: one per-row light table */
    return P_GetSectorTag(s) <= 0;      /* tagged: floor / light can be moved by a trigger */
}

#if HW_VDP2_PLANE
/* Floors that move although their own sector is untagged (2026-11-22):
 *  - stair steps: EV_BuildStairs (specials 7, 8, 100, 127) raises the
 *    chain of same-floorpic sectors behind the tagged start sector,
 *    following two-sided lines whose FRONT is the previous step;
 *  - donut rings: EV_DoDonut (special 9) raises the sector around the
 *    tagged pillar.
 * The plane draws its floor group at a fixed height, so such a sector in
 * the group lost its top surface once it rose. ok[] bit 7 marks movers
 * during the flood; they end up ineligible. */
static void v2_exclude_untagged_movers(uint8_t *ok)
{
    int any = 0;
    for (int l = 0; l < numlines; l++)
    {
        int sp = lines[l].special;
        int stairs = (sp == 7 || sp == 8 || sp == 100 || sp == 127);
        if (!stairs && sp != 9)
            continue;
        int tag = P_GetLineTag(&lines[l]);
        if (tag <= 0)
            continue;
        for (int m = 0; m < numlines; m++)
        {
            int sn0 = lines[m].sidenum[0], sn1 = lines[m].sidenum[1];
            if ((unsigned)sn0 >= (unsigned)numsides || (unsigned)sn1 >= (unsigned)numsides)
                continue;
            int f = sides[sn0].sector, b = sides[sn1].sector;
            if ((unsigned)f >= (unsigned)numsectors || (unsigned)b >= (unsigned)numsectors)
                continue;
            if (stairs)
            {
                /* first step: front = tagged start, back has its floorpic */
                if (P_GetSectorTag(&sectors[f]) == tag && sectors[b].floorpic == sectors[f].floorpic)
                    ok[b] |= 0x80u, any = 1;
            }
            else
            {
                /* donut: any two-sided neighbour of the tagged pillar */
                if (P_GetSectorTag(&sectors[f]) == tag)
                    ok[b] |= 0x80u, any = 1;
                if (P_GetSectorTag(&sectors[b]) == tag)
                    ok[f] |= 0x80u, any = 1;
            }
        }
    }
    /* later steps: front is a marked step, back continues the floorpic */
    for (int pass = 0; any && pass < numsectors; pass++)
    {
        int changed = 0;
        for (int m = 0; m < numlines; m++)
        {
            int sn0 = lines[m].sidenum[0], sn1 = lines[m].sidenum[1];
            if ((unsigned)sn0 >= (unsigned)numsides || (unsigned)sn1 >= (unsigned)numsides)
                continue;
            int f = sides[sn0].sector, b = sides[sn1].sector;
            if ((unsigned)f >= (unsigned)numsectors || (unsigned)b >= (unsigned)numsectors)
                continue;
            if ((ok[f] & 0x80u) && !(ok[b] & 0x80u) && sectors[b].floorpic == sectors[f].floorpic)
            {
                ok[b] |= 0x80u;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    for (int i = 0; i < numsectors; i++)
        if (ok[i] & 0x80u)
            ok[i] = 0;
}
#endif

void Saturn_VDP2_Plane_SelectForLevel(void)
{
    static uint16_t secq[SATURN_V2P_MAX_SECTORS];
    static uint8_t  ok[SATURN_V2P_MAX_SECTORS];
    saturn_vdp2_plane_active = 0;
    memset(saturn_vdp2_plane_secmask, 0, sizeof(saturn_vdp2_plane_secmask));
    for (int s = 0; s < SATURN_V2P_SLOTS; s++)
        v2_slot_key[s].cy = -1;             /* new level: rebuild every slot */
#if HW_VDP2_PLANE
    if (numsectors <= 0 || numsectors > SATURN_V2P_MAX_SECTORS || !saturn_baked_flat_q_count)
        return;
    memset(secq, 0, sizeof(secq));
    unsigned int total = 0;
    for (int ss = 0; ss < numsubsectors; ss++)
    {
        int sn = subsectors[ss].sector;
        if ((unsigned)sn < (unsigned)numsectors)
        {
            unsigned q = saturn_baked_flat_q_count[ss];
            secq[sn] = (uint16_t)((secq[sn] + q > 0xFFFFu) ? 0xFFFFu : secq[sn] + q);
            total += q;
        }
    }
    for (int i = 0; i < numsectors; i++)
        ok[i] = (uint8_t)v2_sector_eligible(&sectors[i]);
    v2_exclude_untagged_movers(ok);
    /* group = same floor height, flat and light level */
    unsigned int best_q = 0;
    int best = -1;
    for (int i = 0; i < numsectors; i++)
    {
        if (!ok[i])
            continue;
        const sector_t *a = &sectors[i];
        int first = 1;
        unsigned int q = 0;
        for (int j = 0; j < numsectors && first; j++)
        {
            const sector_t *b = &sectors[j];
            if (ok[j] && b->floorheight == a->floorheight && b->floorpic == a->floorpic &&
                b->lightlevel == a->lightlevel)
            {
                if (j < i)
                    first = 0;          /* group already counted from its first sector */
                else
                    q += secq[j];
            }
        }
        if (first && q > best_q)
        {
            best_q = q;
            best = i;
        }
    }
    if (best < 0 || best_q < HW_VDP2_PLANE_MIN_QUADS || best_q * 100u < total * HW_VDP2_PLANE_MIN_PCT)
        return;
    const sector_t *a = &sectors[best];
    for (int j = 0; j < numsectors; j++)
    {
        const sector_t *b = &sectors[j];
        if (ok[j] && b->floorheight == a->floorheight && b->floorpic == a->floorpic &&
            b->lightlevel == a->lightlevel)
            saturn_vdp2_plane_secmask[j] = 1;
    }
    saturn_vdp2_plane_sec    = best;
    saturn_vdp2_plane_floorz = a->floorheight;
#if defined(HW_VDP2_PLANE_TEST) && HW_VDP2_PLANE_TEST == 2
    saturn_vdp2_plane_checker = 1;      /* alignment test texture */
#else
    saturn_vdp2_plane_checker = 0;
#endif
    v2_load_flatnum(saturn_vdp2_plane_checker ? -1 : a->floorpic);
    saturn_vdp2_plane_active = v2_inited;
#endif
}

static void v2_load_flatnum(int flatnum)
{
    if (!v2_inited)
        Saturn_VDP2_Plane_Init();
    if (!v2_inited)
        return;

    /* One 64x64 tile (the flat, or the alignment-test checker), then tiled
     * 8 x 4 into the 512x256 bitmap: texel (x, y) = world (x, y) mod 512/256,
     * which keeps 64-unit flat alignment. Palette index 0 is transparent in
     * an 8bpp bitmap, so it is remapped to the black 247. */
    /* The tile is built straight into the bitmap's top-left 64x64 and
     * replicated from VRAM (level load only; a 4 KB static staging buffer
     * cost static space under the 0x060C0000 ceiling). */
    const byte *src = NULL;
    if (flatnum >= 0 && !(src = Saturn_FlatData(flatnum)))
        return;
    for (int y = 0; y < 64; y++)
    {
        volatile uint32_t *dst = V2_BITMAP + y * (512 / 4);
        for (int x4 = 0; x4 < 64; x4 += 4)
        {
            uint32_t w4 = 0;
            for (int x = x4; x < x4 + 4; x++)
            {
                unsigned char p;
                if (src)
                    p = src[y * 64 + x];
                else if (x < 2 || x >= 62 || y < 2 || y >= 62 || x == 31 || x == 32 || y == 31 || y == 32)
                    p = 231;                                 /* yellow border + crosshair */
                else if (y < 31)
                {
                    int qx = x - 10, qy = y - 10;            /* white quadrant with a black 'N' */
                    p = (x >= 31) ? 176 : ((qx >= 0 && qx < 11 && qy >= 0 && qy < 11 &&
                                            (qx <= 1 || qx >= 9 || qx == qy || qx == qy + 1)) ? 247 : 4);
                }
                else
                    p = (x < 31) ? 112 : 251;                /* green / magenta */
                w4 = (w4 << 8) | ((p == 0) ? 247u : p);      /* big-endian: first texel high */
            }
            dst[x4 >> 2] = w4;
        }
    }
    for (int y = 0; y < 256; y++)
    {
        const volatile uint32_t *row = V2_BITMAP + (y & 63) * (512 / 4);
        volatile uint32_t *dst = V2_BITMAP + y * (512 / 4);
        for (int x = (y < 64) ? 16 : 0; x < 512 / 4; x++)
            dst[x] = row[x & 15];
    }
}

void Saturn_VDP2_Plane_Frame(fixed_t camx, fixed_t camy, fixed_t camz, angle_t camang, fixed_t floorz)
{
    if (!v2_inited || !saturn_vdp2_plane_active)
        return;

    /* Build into a slot no VDP1 frame in flight (shown / drawing / pending)
     * still refers to; it goes on screen with this frame's VDP1 list. */
    {
        int w = 0;
        int sh, dr, pe;
        /* Snapshot the three slots atomically (2026-11-21): the vblank
         * callback rotates them (shown <- drawing <- pending); reading them
         * one by one across a rotation could pick the slot now on screen
         * and overwrite its tables (plane swims for a field). After a
         * consistent snapshot a later rotation only moves slots within
         * that set, so the chosen one stays free. */
#ifndef SATURN_WIN32
        {
            unsigned int sr, sr_off;
            __asm__ volatile("stc sr,%0" : "=r"(sr));
            sr_off = sr | 0xF0u;
            __asm__ volatile("ldc %0,sr" : : "r"(sr_off) : "memory");
            sh = saturn_v2p_shown; dr = saturn_v2p_drawing; pe = saturn_v2p_pending;
            __asm__ volatile("ldc %0,sr" : : "r"(sr) : "memory");
        }
#else
        sh = saturn_v2p_shown; dr = saturn_v2p_drawing; pe = saturn_v2p_pending;
#endif
        while (w < SATURN_V2P_SLOTS - 1 && (w == sh || w == dr || w == pe))
            w++;
        v2_kt = v2_kts[w];
        v2_rtable = v2_rtables[w];
        saturn_v2p_last = w;
#if defined(HW_VDP2_PLANE_TEST) && HW_VDP2_PLANE_TEST
        v2_slot_ang[w] = camang;
        saturn_v2p_frame_ang = camang;
#endif
    }

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

    /* RBG0 screen x 0 is display x 0; the VDP1 view is drawn with local
     * origin x = 16 (320 view centred in the 352 screen), so the shared
     * projection centre is display x 160 + 16 (measured: vanishing point
     * 176.0, rows within 0.3 px of the VDP1 projection). */
    v2_rs.XST = -(160 + 16) << 16;
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
    /* longword coefficients: KAST = entry index (4 bytes) from the 256 KB half KTAOS selects */
    v2_rs.KAST = ((((unsigned int)v2_kt) & 0x3FFFFu) >> 2) << 16;
    v2_rs.DKAST = 1 << 16;
    v2_rs.DKA = 0;

    /* Coefficient table + per-row light banks depend only on (eye height
     * above the plane, horizon, view height, light): rebuild a slot's copy
     * only when that key changed since the slot was last built (2026-11-17
     * tour: rebuilding all 256 rows with a divide each, every frame, cost
     * ~1 ms of master time on master-bound maps). */
    fixed_t camz_rel = camz - floorz;
    int li = sectors[saturn_vdp2_plane_sec].lightlevel >> 4;
    if (li < 0) li = 0; else if (li > 15) li = 15;
    int w = saturn_v2p_last;
    if (v2_slot_key[w].camz_rel != camz_rel || v2_slot_key[w].cy != cy ||
        v2_slot_key[w].max_y != max_y || v2_slot_key[w].light != li)
    {
        int full = (v2_slot_key[w].cy != cy || v2_slot_key[w].max_y != max_y);
        v2_slot_key[w].camz_rel = camz_rel;
        v2_slot_key[w].cy = (int16_t)cy;
        v2_slot_key[w].max_y = (int16_t)max_y;
        v2_slot_key[w].light = (int16_t)li;
        v2_slot_gen[w]++;
        if (full)
        {
            /* rows 0..cy (sky side) and max_y..255 (status bar): transparent */
            for (int y = 0; y <= cy; y++)
            {
                v2_kt[y * 2]     = 0x8000;
                v2_kt[y * 2 + 1] = 0x0000;
            }
            for (int y = max_y; y < 256; y++)
            {
                v2_kt[y * 2]     = 0x8000;
                v2_kt[y * 2 + 1] = 0x0000;
            }
        }
#if HW_VDP2_PLANE_LIGHT
        memset(v2_bank[w], 0, sizeof(v2_bank[w]));   /* rows off the plane: bank 0 */
#endif
        /* Rows cy+1..max_y-1: visible perspective ground plane,
         * k = camz_rel / (y - cy) by reciprocal (v2_inv[n] = 2^32 / n) */
        for (int y = cy + 1; y < max_y; y++)
        {
            if (camz_rel <= 0)
            {
                v2_kt[y * 2]     = 0x8000;      /* eye at/below the plane: not seen */
                v2_kt[y * 2 + 1] = 0x0000;
                continue;
            }
            unsigned int k24 = (unsigned int)(((uint64_t)(uint32_t)camz_rel * v2_inv[y - cy]) >> 32);
            if (k24 > 0x7FFFFFu)
                k24 = 0x7FFFFFu;
            v2_kt[y * 2]     = (unsigned short)((k24 >> 16) & 0x7F);
            v2_kt[y * 2 + 1] = (unsigned short)(k24 & 0xFFFF);
#if HW_VDP2_PLANE_LIGHT
            {
                /* row depth = k * focal (160), as the VDP1 flats' cz; same LUT */
                unsigned int zb = (k24 * 160u) >> 20;
                if (zb > 127u) zb = 127u;
                v2_bank[w][y] = (uint8_t)((flat_zlight_cmdcolr_LUT[li][zb] >> 8) & 7u);
            }
#endif
        }
    }

    /* Deliver directly to VDP2 VRAM rotation parameter table */
    *v2_rtable = v2_rs;
}

void Saturn_VDP2_Plane_VBlank(void)
{
#if HW_VDP2_PLANE_LIGHT
    /* The per-scanline DMA must stop whenever the plane is not on screen.
     * It is only re-aimed here, once per field; a channel left enabled
     * without that walks past v2_light_dma at every HBlank-IN and copies
     * whatever memory follows into CRAOFA/CRAOFB (hardware 2026-11-17: NBG0
     * striped on the pause menu, and over the whole screen after resuming,
     * when the pause had deactivated the plane and this hook returned early). */
    if (!saturn_vdp2_plane_active || !saturn_vdp2_rbg0_on)
    {
        if (v2_dma_on)
        {
            SCU_D2EN = 0;
            v2_dma_on = 0;
        }
    }
#endif
    if (!saturn_vdp2_plane_active)
        return;

    /* Show the slot of the VDP1 frame on screen (set at its frame change in
     * saturn_vdp1.c); before manual mode starts, the latest built slot. */
    extern int saturn_v1_manual;
    int s = saturn_v1_manual ? saturn_v2p_shown : saturn_v2p_last;
    saturn_rotscroll_t *rt = (s >= 0 && s < SATURN_V2P_SLOTS) ? v2_rtables[s] : NULL;
#if defined(HW_VDP2_PLANE_TEST) && HW_VDP2_PLANE_TEST
    if (saturn_v2p_last >= 0 && s >= 0)
    {
        uint32_t i = saturn_v2p_log_n++ & 63u;
        saturn_v2p_log[i][0] = (uint16_t)(v2_slot_ang[saturn_v2p_last] >> 16);
        saturn_v2p_log[i][1] = (uint16_t)(v2_slot_ang[s] >> 16);
        saturn_v2p_log[i][2] = (uint16_t)(saturn_v2p_list_ang[2] >> 16);
        saturn_v2p_log[i][3] = (uint16_t)s;
    }
#endif
    if (rt)
    {
        unsigned int rta = (((unsigned int)rt) & 0x7FFFFu) >> 1;
        JO_VDP2_RPTAU = (unsigned short)((rta >> 16) & 0x7u);
        JO_VDP2_RPTAL = (unsigned short)(rta & 0xFFFEu);
    }

#if HW_VDP2_PLANE_LIGHT
    if (saturn_vdp2_rbg0_on && s >= 0 && s < SATURN_V2P_SLOTS)
    {
        /* Buffer indexed by ABSOLUTE raster line (entry k = line k mod 263,
         * two fields long), so only the start pointer depends on where the
         * raster is -- read right before the enable to keep the race short.
         * VDP2 registers are write-only (a read returns bus junk), so the
         * other fields are written as the constants the game uses: CRAOFA = 0
         * (all NBG offsets 0, i_video.c clears NBG0's) and CRAOFB.SPCAOS = 0
         * (sprite palette offset 0; VDP1 picks banks with its colour bank). */
        /* rebuild the buffer only when the shown slot or its banks changed */
        if (s != v2_dma_slot || v2_slot_gen[s] != v2_dma_gen)
        {
            /* entry k = bank[k mod V2_LINES] for lines < 224, else 0. The
             * zero lines never change: write them once; per rebuild only the
             * 224 bank lines of both fields (was 526 entries with a modulo
             * each, ~0.55 ms of master time per rendered frame). */
            static int zeroed = 0;
            if (!zeroed)
            {
                for (int k = 224; k < V2_LINES; k++)
                {
                    v2_light_dma[k] = 0u;
                    v2_light_dma[V2_LINES + k] = 0u;
                }
                zeroed = 1;
            }
            const uint8_t *bank = v2_bank[s];
            uint32_t *d0 = (uint32_t *)&v2_light_dma[0];
            uint32_t *d1 = (uint32_t *)&v2_light_dma[V2_LINES];
            for (int k = 0; k < 224; k++)
            {
                uint32_t b = bank[k];
                d0[k] = b;
                d1[k] = b;
            }
            v2_dma_slot = s;
            v2_dma_gen = v2_slot_gen[s];
        }
        SCU_D2EN = 0;
        SCU_D2W  = 0x05F800E4u;                 /* CRAOFA, CRAOFB */
        SCU_D2C  = 4;                           /* one long per HBlank-IN */
        SCU_D2AD = 0x101u;                      /* read +4, write +2 */
        SCU_D2MD = 0x00010002u;                 /* direct, RUP (read address advances), factor 2 = HBlank-IN */
        {
            /* Raster position (VCNT reads live; past the vsync start it
             * counts on toward 0x1FF = the last line, Kronos/Mednafen;
             * 5thPlanet linear). The next HBlank-IN ends this line and feeds
             * line+1 -- or, if this line's HBLANK has already begun
             * (TVSTAT bit 2), it ends the next line and feeds line+2. */
            unsigned int tv = V2_TVSTAT;
            unsigned int vc = V2_VCNT & 0x1FFu;
            unsigned int line = (vc >= V2_LINES) ? (vc + V2_LINES - 0x200u) % V2_LINES : vc;
            unsigned int first = line + 1u + ((tv & 0x0004u) ? 1u : 0u);
            SCU_D2R  = (uint32_t)&v2_light_dma[first % V2_LINES] & 0x07FFFFFFu;
            SCU_D2EN = 0x100u;                  /* enable (started by the factor) */
        }
        v2_dma_on = 1;
    }
#endif

    /* Whole-register writes only (VDP2 registers are write-only): bank
     * roles, no colour calc, RBG0 priority 4. BGON (whether RBG0 shows at
     * all) is the i_video.c flip's: only under 3D pages. */
    JO_VDP2_RAMCTL = saturn_vdp2_ramctl;
    JO_VDP2_CCCTL = 0x0000u;
    JO_VDP2_PRIR = 0x0004u;

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
void Saturn_VDP2_Plane_SelectForLevel(void)
{
    saturn_vdp2_plane_active = 0;
    memset(saturn_vdp2_plane_secmask, 0, sizeof(saturn_vdp2_plane_secmask));
}
#endif
