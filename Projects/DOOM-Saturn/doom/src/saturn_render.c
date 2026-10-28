/* saturn_render.c â€” Saturn-native textured raycaster for DOOM.
 *
 * Why: d32xr's renderer is built around the 32X DSP + the Jaguar GPU,
 * with column drawers that hand off work to a co-processor. The Saturn
 * has neither â€” it has VDP1 quads + a single SH-2 doing CPU drawing.
 * Rather than rewrite d32xr's full BSP/seg command pipeline to work
 * Saturn-style, we write a small raycaster that consumes the same
 * load-time BSP/sector/line/vertex tables p_setup.c already populates
 * and emits 8bpp pixels into I_VideoBuffer.
 *
 * 2026-08-31 â€” full textured pass, structured like vanilla's column
 * renderer (Game Engine Black Book 5.12): for every screen column the
 * ray's line crossings are visited NEAR-TO-FAR, each crossing drawing
 * its upper/lower wall spans and advancing two clip boundaries
 * (vanilla's ceilingclip/floorclip, here painted_top/painted_bot):
 *
 *   - upper wall (back ceiling lower)  -> sidedef toptexture
 *   - lower wall (back floor higher)   -> sidedef bottomtexture
 *   - nonzero midtexture on two-sided  -> solid grate, column ends
 *   - one-sided / ML_BLOCKING          -> midtexture, column ends
 *   - opening between them             -> ray continues, flats come
 *                                         from the sector beyond
 *
 * This is what makes doors, windows and steps render correctly: a
 * closed door is an upper wall whose bottom rides the door sector's
 * ceiling, a step is a lower wall, and the sky/ceiling/floor visible
 * through an opening belongs to the sector PAST the opening â€” the old
 * single-hit renderer painted the whole column with the nearest line's
 * sector.
 *
 * Textures: vertical columns, perspective-correct u per column (u is
 * constant along a column â€” the core vanilla insight), linear v with
 * wrap. Flats: per-row world distance (one reciprocal-table lookup per
 * row) + per-column world direction (2 mults per pixel), texel =
 * flat[(wy&63)<<6 | (wx&63)] â€” vanilla's 64x64 span mapping. Lighting:
 * the PC COLORMAP lightmap, shade = ((255-light)>>4) + (distance>>6),
 * clamped 0..33; the sky ignores lightmaps like vanilla's.
 *
 * Everything degrades to the old flat-colour renderer when the WAD
 * doesn't provide PNAMES/TEXTURE1/COLORMAP (weird WADs), and texture
 * number 0 ("-") draws nothing, exactly like vanilla's rw_midtexture
 * checks.
 *
 * Performance: camera-space line cache (see camline_t) keeps every
 * per-column product inside 32 bits; a per-line tan-range window
 * (endpoints near-clipped at z>=8 half-units, ratio compared against
 * the column's fixed-point tan) rejects most lines with two compares
 * before any cross-product math runs.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "saturn.h"     /* SATURN_CART_REC_OFF (r92) */
#include "r_local.h"
#include "p_local.h"   /* P_PointOnLineSide, sector height thresholds */
#include "i_video.h"
#include "saturn_tex.h"
#include "saturn_segs.h"   /* round 11b probe struct + viswall hooks */
#include "saturn_cache.h"  /* saturn_oc_purge â€” Phase 4 dual-SH-2 split */

/* Forward from g_game.c (which we don't include to avoid pulling in
 * the d32xr renderer-side callers). */
extern byte *I_VideoBuffer;

/* FULL-WIDTH RENDER (2026-09-05, user request: more rendered columns):
 * the frame is back to 320 columns — vanilla width, no display stretch.
 * (2026-09-01..09-05 ran a HALF-WIDTH 160-column frame stretched 2:1 at
 * display; comments dated before today describe that path.) The column
 * ray (cl_x, cl_z) = (HALF_W-x, PROJ_X) with HALF_W = 160, PROJ_X = 160
 * is exactly vanilla's pinhole (ManagedDoom focal = 160), so col_tan /
 * t / u / t_u are the ORIGINAL 320-wide values, column for column.
 * Cost: every per-column loop (walk, sweep, solid/flat runs, fb stride,
 * copy-back) does 2x the half-width work. FOV stays 90 degrees. */
#define SCREEN_W  DOOM_SCREENWIDTH          /* 320 */
#define SCREEN_H  DOOM_SCREENHEIGHT        /* 200 */
#define HALF_W    (SCREEN_W / 2)
#define HALF_H    (SCREEN_H / 2)

/* 320x180 render window (2026-09-01 user request, "same as the 32X"):
 * the projection is UNTOUCHED (horizon stays at row 100) â€” the walk
 * simply clamps every span to rows [VIEW_TOP, VIEW_BOT), cropping 10
 * rows off the top and bottom of the old view. ~10% fewer painted
 * pixels; the letterbox bands are the fb clear color (the global
 * memset below keeps them clean). */
/* 2026-09-05 UI: VIEW_BOT 190 -> 168 made rows 168-199 the classic
 * 32-row status-bar strip (saturn_hud.c draws the STBAR patch there). */
/* 2026-09-06 full-screen layout (user request): the fb is the full
 * 224-line window (DOOM_FBHEIGHT). The view spans rows 0..191 - window
 * starts at the very top - and the STBAR strip occupies 192..223,
 * CONNECTED to the view's bottom edge and flush against the physical
 * bottom of the screen. The projection stays untouched (horizon at
 * row 100); the walk clamps every span to [0, 192). ~21% more painted
 * pixels than the 10..168 crop - the price of the full-screen look. */
#define VIEW_TOP  0
#define VIEW_BOT  192

/* Set to 1 to bring back the diagnostic strip + minimap overlays
 * (rows 0-20 bars, top-down line map) that isolated the original
 * camera-space/missing-texture bugs. They draw over the scene, so
 * they stay off in normal builds. */
#define SATURN_RENDER_DIAG  0

/* Projection focal length. Vanilla DOOM projects BOTH axes with focal
 * centerX = 160 (ManagedDoom ThreeDRenderer.cs:263 keeps the formula:
 * planeYSlope[y] = (windowWidth/2) / dy) â€” the 90-degree horizontal FOV
 * across 320 columns sets the pinhole distance, and the vertical axis
 * shares it. Our first cuts used SCREEN_H (200), which stretched
 * everything vertically 25%.
 *
 * Full-width render (2026-09-05): PROJ and PROJ_X are both 160 again —
 * PROJ is the VERTICAL focal (y = (z-eye)*PROJ/t_u with t_u a depth)
 * and PROJ_X the column ray's plane distance; with the 320-wide frame
 * they coincide, exactly as in vanilla. col_clz[x] = PROJ_X keeps t_u
 * scale-invariant (see the SCREEN_W note above). */
#define PROJ      160
#define PROJ_X    160

/* Max line crossings walked per column (safety valve; real views use
 * 1-4). */
#define MAX_CROSSINGS 16

/* Near plane for the per-line window, in half-units. Also guards the
 * window's tan computation from dividing by ~0. */
#define NEAR_Z 8

/* Player camera state. We reuse the d32xr `players[0]` so the renderer
 * reads position/angle straight from gameplay. */
typedef struct
{
    fixed_t viewx, viewy;        /* world position        */
    angle_t viewangle;           /* facing angle          */
    fixed_t viewz;               /* eye height above floor */
    fixed_t viewcos, viewsin;    /* precomputed trig       */
} camera_t;

static camera_t cam;

/* Surface base colours: one 16-entry PLAYPAL ramp per surface family.
 * Only used as the FALLBACK renderer now â€” when textures/flats are
 * available the real art draws instead. Within each block the ramp
 * runs LIGHT -> DARK with rising index.
 *   0x10 pink  0x40 brown  0x80 tan  0xC0 pale blue  (0x20 = dark red
 *   all-miss background, 0x00 = black minimap lines). */
#define COL_CEILING  0x10
#define COL_FLOOR    0x40
#define COL_WALL_TOP 0x80
#define COL_WALL_LOW 0xC0

/*----- helpers -----*/

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* Camera-space line cache, rebuilt once per frame.
 *
 * Why this exists: the first-cut raycaster tested every ray against
 * world-space segments with raw fixed-point cross products â€”
 * (viewcos ~2^16) x (wall delta ~2^24) ~ 2^40 and
 * (vertex-camera ~2^25) x (delta ~2^25) ~ 2^50 â€” both WAY past the
 * 32-bit int the SH-2 multiplies in. Every product wrapped, every
 * sign test saw garbage, and every ray missed every wall (the
 * uniform dark-red frame). Doing the products in 64-bit works but
 * costs ~600k __muldi3 calls per frame â€” seconds per frame on the
 * 28.6 MHz SH-2.
 *
 * Instead, rotate every line's endpoints into camera space ONCE per
 * frame (8 FixedMuls per line â€” a few thousand 64-bit mults total),
 * and store them at HALF-MAP-UNIT granularity (>> (FRACBITS-1)):
 * values top out around 2^13, so every per-column x per-line product
 * below (coords 2^13 x direction 2^16, or coord x coord) stays
 * comfortably inside 32 bits. Half-unit precision is invisible for
 * wall columns; the win is that the 192k inner-loop tests per frame
 * are plain 32-bit multiplies. */
typedef struct
{
    int x1, z1;      /* lateral / forward of endpoint 1 (half-units) */
    int x2, z2;      /* endpoint 2 */
} camline_t;

static camline_t *camlines     = NULL;
static int       camlines_cap = 0;

/* Hot-table placement (2026-09-01): every table the flat kernel reads
 * PER PIXEL (col_wxs/wys) or the scan reads PER CANDIDATE (camlines,
 * win_tan, active[]) used to live in the cart Z zone â€” each access
 * paid A-bus latency, ~200k extra-read cycles per frame. The top 64 KB
 * of WRAM-L is reserved for them (Saturn_RtblAlloc, saturn_tex.c);
 * Z_Malloc stays as the fallback when the reservation overflows on a
 * bigger map. Never freed â€” same lifetime as the old PU_STATIC blocks. */
extern void *Saturn_RtblAlloc(int size);
/* Round-41 WILD-WRITE FENCE HUNT: the crash spot kills the slave's
 * entry AND the vblank chain (round-40 verdict: kind-4 dead-chain fire
 * with E=0/SP-min unsampled) — the signature of a runaway write into
 * SGL's interrupt/protocol area, which is linked INTERLEAVED with the
 * game's BSS (SlaveCommand 0x060482d0 sits inside the game's BSS
 * range). Prime suspect: a per-frame hot table running past its end.
 * So: every rtbl_z table (and the segs-side blocks registered from
 * saturn_segs.c) gets a 4-word canary fence past its tail. The verdict
 * check names the first tripped fence; the fence values identify the
 * overflowing writer's word pattern. Registration order = the id:
 *   1 camlines, 2 line_len, 3 win_tan0, 4 win_tan1, 5 act_head,
 *   6 deact_head, 7 act_next, 8 deact_next, 9/10 active_v,
 *   11/12 in_active_v, 13 col_clx, 14 col_clz, then saturn_segs.c's
 *   registrations (15 segwalls, 16 spr_vis, 17 spr_ctop, 18 spr_cbot,
 *   19 seg_mmid_guard) — VERIFY against the live map at decode time. */
#define RTBL_FENCE_MAX 48
static unsigned int *rtbl_fence[RTBL_FENCE_MAX];
static int           rtbl_fence_n = 0;
volatile unsigned int saturn_rtbl_trip_val  = 0;  /* first bad word */
volatile unsigned int saturn_rtbl_trip_word = 0;  /* its index 0..3 */

void saturn_rtbl_fence_register(unsigned int *f)
{
    if (rtbl_fence_n < RTBL_FENCE_MAX)
        rtbl_fence[rtbl_fence_n++] = f;
}

void saturn_rtbl_fence_arm(void)
{
    int i;

    for (i = 0; i < rtbl_fence_n; i++)
    {
        rtbl_fence[i][0] = 0x5E61F001u;
        rtbl_fence[i][1] = 0x5E61F002u;
        rtbl_fence[i][2] = 0x5E61F003u;
        rtbl_fence[i][3] = 0x5E61F004u;
    }
}

/* 0 = all fences clean; 1..n = the first tripped fence's id. */
int saturn_rtbl_fence_check(void)
{
    int i, w;

    for (i = 0; i < rtbl_fence_n; i++)
        for (w = 0; w < 4; w++)
            if (rtbl_fence[i][w] != 0x5E61F001u + (unsigned int)w)
            {
                saturn_rtbl_trip_val  = rtbl_fence[i][w];
                saturn_rtbl_trip_word = (unsigned int)w;
                return i + 1;
            }
    return 0;
}

static void *rtbl_z(int bytes)
{
    void *p = Saturn_RtblAlloc(bytes + 16);
    int   fb = (bytes + 3) & ~3;

    if (!p)
        p = Z_Malloc(bytes + 16, PU_STATIC);
    if (p && rtbl_fence_n < RTBL_FENCE_MAX)
        saturn_rtbl_fence_register((unsigned int *)((char *)p + fb));
    return p;
}

/* Per-line, per-frame:
 *   line_len[i]  world length in whole map units (texture u scale)
 *   win_tan0/1   the line's on-screen angular span, expressed as the
 *                16.16 fixed tan of its (near-clipped) endpoints'
 *                camera-space rays. A column whose own fixed tan
 *                falls outside [win_tan0, win_tan1] cannot cross the
 *                line, and is rejected with two compares.
 *   win_tan0 > win_tan1 marks an empty window (behind the near plane
 *   or degenerate). */
static int *line_len;
/* Per-frame work counters (definitions near the profiler block; the
 * HUD's L/X/Y/Z rows print these) plus FRT sub-windows. */
extern int saturn_cnt_scan;
extern int saturn_cnt_solve;
extern int saturn_cnt_cross;
extern int saturn_cnt_span;
extern int saturn_cnt_fruns;   /* defined near the other counters */
extern int saturn_cnt_frows;
extern int saturn_cnt_fpx;    /* total flat pixels; defined below */
extern int saturn_cnt_femit;  /* round 8b: emitted span rows */
extern int saturn_frt_ploop;  /* paint_flat_run kernel loop >>8 */
extern int saturn_frt_fire;
extern int saturn_frt_scan;
extern int saturn_frt_walk;
extern int saturn_frt_total;
extern int saturn_frt_stage;
extern int saturn_frt_paint;
extern int saturn_frt_emit;
extern int saturn_frt_flush;
extern int saturn_frt_merge;
extern int saturn_frt_pre;    /* preload walk (Saturn_TexData sweep)   */
extern int saturn_frt_cl;     /* per-frame camline rebuild            */
extern int saturn_frt_tic;    /* game-logic tic body (d_main.c loop)  */

/* Sub-vblank resolution: the SH-2 on-chip Free-Running Timer's TCNT
 * (16-bit). MEASURED rate (HUD, 2026-09-01): Y=4246 units over a
 * 78-vblank render = ~13.9k ticks/vblank = phi/32 (~895 kHz, 36 SH-2
 * cycles/tick), NOT the phi/8 this file assumed before. All the
 * sub-window stamps must wrap-protect with uint16 arithmetic. */
/* Flat kernel-loop accumulator (paint_flat_run only; reset + published
 * by render_columns like the other frt windows). */
static unsigned int frt_ploop_acc = 0;

#define SAT_FRT() \
    (*(volatile unsigned short *)0xFFFFFE12u)

static int *win_tan0;
static int *win_tan1;

/* Active-list tan sweep (Phase 2). The per-crossing scan formerly
 * tested ALL numlines windows per crossing attempt; instead each line
 * ACTIVATES at the first column whose tan <= win_tan1+4096 and
 * DEACTIVATES at the first column whose tan < win_tan0-4096 (exactly
 * the scan's window test, walked along the strictly decreasing
 * col_tan). Per-column event buckets (linked lists via *_next) are
 * built once per frame with a division-free estimate+adjust against
 * the monotone col_tan -- the SH-2 has no hardware divide. The scan
 * then runs its UNTOUCHED window+straddle test over active[] only
 * (typically 10-100 lines vs 486; sim-measured 5.7x fewer candidates
 * on E1M1). active[] is kept ASCENDING by line index so the equal-t
 * tie-break (lowest index wins) stays identical to the baseline scan;
 * the retained window test makes the list a safe superset (lines
 * whose act/deact columns coincide may linger -- they reject like any
 * other non-candidate). Deactivations fire before activations each
 * column, matching the validated sim. */
static int *act_head;    /* [SCREEN_W] first line to activate at col x */
static int *act_next;    /* [numlines] bucket linked list */
static int *deact_head;  /* [SCREEN_W] first line to deactivate at x */
static int *deact_next;  /* [numlines] */
/* The evolving active-list state is PER SH-2 (Phase 4): each core
 * fires the same per-column buckets into its own copy. The buckets
 * themselves are built once on the master and only read afterwards. */
static int *active_v[2];        /* [numlines] ascending line indices */
static signed char *in_active_v[2];  /* [numlines] 1 == in active[] */
static int active_count_v[2];

#define ACTIVE(c)       active_v[c]
#define IN_ACTIVE(c)    in_active_v[c]
#define ACTIVE_COUNT(c) active_count_v[c]

/* Per-column crossing-candidate cache (2026-09-01): the scan builds
 * it once per column, sorted by t; each crossing pass advances a
 * pointer instead of re-solving the active list. Per-cpu â€” both
 * halves run the column loop under the split build. Zone-alloc'd
 * (BSS rides the 0x060C0000 ceiling). */
#define CAND_CAP 256
static int *cand_i_v[2], *cand_t_v[2], *cand_un_v[2], *cand_den_v[2];
static int   cand_n_v[2], cand_pos_v[2];

#define CAND_I(c)   cand_i_v[c]
#define CAND_T(c)   cand_t_v[c]
#define CAND_U(c)   cand_un_v[c]
#define CAND_D(c)   cand_den_v[c]
#define CAND_N(c)   cand_n_v[c]
#define CAND_POS(c) cand_pos_v[c]

/* line_len[] only depends on the level's lines/vertexes, not the
 * camera â€” rebuilt once per level (invalidated from P_SetupLevel). */
static int line_len_built;

/* Preload-walk warming gate: after one pass with zero new composites
 * and zero flat fills, every reachable texture/flat is cached and the
 * ~1900-lookup walk is skipped (re-run only after any new composite or
 * flat refill, and after a level change). */
static int preload_warm = 0;

void Saturn_RenderInvalidateLevel(void)
{
    line_len_built = 0;
    preload_warm   = 0;
}

/* Per-column precomputed values:
 *   col_clx/clz  the RAW projection-plane ray direction (lateral axis
 *                is LEFT; see the camline build): the ray through
 *                screen column x passes through (HALF_W-x, PROJ) on a
 *                virtual plane PROJ units ahead of the eye â€” vanilla's
 *                pinhole model. Plain ints, NOT 16.16 and NOT unit
 *                length; cast_ray's solve returns t = mu*65536 with
 *                mu = hit-depth/PROJ and recovers the depth from it.
 *   col_tan      16.16 fixed tan of the ray (cl_x/cl_z = -(x-HALF_W)/
 *                PROJ); monotone across the screen, so tan-window
 *                tests are exact
 *   col_bam      the same ray offset as a BAM (atan of the tan above),
 *                only the sky needs it
 *   col_wxs/wys  per-frame WORLD-space ray step for the flats: at
 *                perpendicular depth d the hit sits at view +
 *                d*forward + (tan*d)*lateral-left, so the step is
 *                fhat + tan*lhat (length up to ~1.42 â€” it carries the
 *                1/cos edge correction a plain unit vector drops)
 *   col_sky      sky texture column for this ray (BAM >> 22 wraps the
 *                256-wide sky 4x per full turn â€” vanilla pans it once
 *                per 90-degree FOV)
 * Zone-allocated, NOT static: BSS already ends within bytes of the
 * 0x060C0000 user-RAM ceiling, and these ~7 KB as arrays there pushed
 * __bend 0x630 past it into reserved SGL territory (check game.map). */
static fixed_t *col_clx;
static fixed_t *col_clz;
static fixed_t *col_wxs;
static fixed_t *col_wys;
static int     *col_tan;
static int     *col_bam;
static byte    *col_sky;

/* rowinv[y] = 65536 / |y - HALF_H| â€” turns the flat distance formula
 * d = (eye-h)*PROJ/|y-HALF_H| into one multiply per row. Built once.
 * Zone-alloc'd: BSS rides the 0x060C0000 user-RAM ceiling (see the
 * col_* comment). */
static int *rowinv;
static int rowinv_built;

/* col_clx/clz/col_tan never depend on the camera (pure column
 * geometry), so they're built once. The 16.16 tan needs a 64-bit
 * divide: at the screen edges sine(45deg) = 46341 and 46341<<16
 * overflows a 32-bit int. */
static int colgeom_built;

#if SATURN_RENDER_DIAG
/*----- diagnostic overlays (see SATURN_RENDER_DIAG) -----*/

#define COL_ANG   0x10
#define COL_POS   0x40
#define COL_NEG   0xC0
#define COL_INFO  0x80
#define COL_MAP   0x00

static void diag_bar(int y, int x0, int w, byte col)
{
    byte *fb;
    int   x;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (x0 > SCREEN_W) return;
    if (w > SCREEN_W - x0) w = SCREEN_W - x0;
    if (w <= 0) return;
    fb = I_VideoBuffer + y * SCREEN_W + x0;
    for (x = 0; x < w; x++)
        fb[x] = col;
}

static void diag_strip(int ndeg, int nhits)
{
    int ux, w;

    memset(I_VideoBuffer, 0x00, SCREEN_W * 20);

    /* angle: full 32-bit turn spread across the screen */
    w = (int)(((unsigned long long)cam.viewangle * SCREEN_W) >> 32);
    diag_bar(1, 0, w, COL_ANG);
    diag_bar(2, 0, w, COL_ANG);

    /* viewx / viewy: signed, centered on x=160, 64 map units per px */
    ux = (int)(cam.viewx >> (FRACBITS + 6));
    if (ux > 159)  ux = 159;
    if (ux < -160) ux = -160;
    if (ux >= 0)
    {
        diag_bar(4, HALF_W, ux, COL_POS);
        diag_bar(5, HALF_W, ux, COL_POS);
    }
    else
    {
        diag_bar(4, HALF_W + ux, -ux, COL_NEG);
        diag_bar(5, HALF_W + ux, -ux, COL_NEG);
    }
    ux = (int)(cam.viewy >> (FRACBITS + 6));
    if (ux > 159)  ux = 159;
    if (ux < -160) ux = -160;
    if (ux >= 0)
    {
        diag_bar(7, HALF_W, ux, COL_POS);
        diag_bar(8, HALF_W, ux, COL_POS);
    }
    else
    {
        diag_bar(7, HALF_W + ux, -ux, COL_NEG);
        diag_bar(8, HALF_W + ux, -ux, COL_NEG);
    }

    /* numlines (nominal: E1M1 486 -> 61 px) */
    w = numlines >> 3;
    diag_bar(10, 0, w, COL_INFO);
    diag_bar(11, 0, w, COL_INFO);

    /* degenerate camlines as a fraction of numlines */
    if (numlines > 0)
        w = ndeg * SCREEN_W / numlines;
    else
        w = 0;
    diag_bar(13, 0, w, COL_ANG);
    diag_bar(14, 0, w, COL_ANG);

    /* columns that hit a wall this frame (0..320) */
    diag_bar(16, 0, nhits, COL_INFO);
    diag_bar(17, 0, nhits, COL_INFO);
}

static void diag_plot(int x, int y, byte col)
{
    if (x < 0 || x >= SCREEN_W || y < 20 || y >= SCREEN_H)
        return;
    I_VideoBuffer[y * SCREEN_W + x] = col;
}

static void diag_minimap(void)
{
    int w, h, i;

    w = worldbbox[BOXRIGHT]  - worldbbox[BOXLEFT];
    h = worldbbox[BOXTOP]    - worldbbox[BOXBOTTOM];
    if (w <= 0 || h <= 0 || numlines <= 0)
        return;

    for (i = 0; i < numlines; i++)
    {
        line_t *ld = &lines[i];
        int x0, y0, x1, y1, dx, dy, steps, s;

        if (ld->v1 < 0 || ld->v2 < 0
            || ld->v1 >= numvertexes || ld->v2 >= numvertexes)
            continue;

        x0 = 1 + ((int)(vertexes[ld->v1].x - worldbbox[BOXLEFT])
                  * (SCREEN_W - 2)) / w;
        y0 = 21 + ((int)(vertexes[ld->v1].y - worldbbox[BOXBOTTOM])
                  * (SCREEN_H - 22)) / h;
        x1 = 1 + ((int)(vertexes[ld->v2].x - worldbbox[BOXLEFT])
                  * (SCREEN_W - 2)) / w;
        y1 = 21 + ((int)(vertexes[ld->v2].y - worldbbox[BOXBOTTOM])
                  * (SCREEN_H - 22)) / h;

        dx = x1 - x0;
        dy = y1 - y0;
        steps = (dx < 0 ? -dx : dx);
        if ((dy < 0 ? -dy : dy) > steps)
            steps = (dy < 0 ? -dy : dy);
        if (steps == 0)
        {
            diag_plot(x0, y0, COL_MAP);
            continue;
        }
        for (s = 0; s <= steps; s++)
            diag_plot(x0 + dx * s / steps, y0 + dy * s / steps, COL_MAP);
    }

    /* Camera: pink 3x3 dot + heading tick (in map space). */
    {
        int cx, cy, hx, hy;
        fixed_t fa = cam.viewangle >> ANGLETOFINESHIFT;
        cx = 1 + (((int)(cam.viewx >> FRACBITS) - worldbbox[BOXLEFT])
                  * (SCREEN_W - 2)) / w;
        cy = 21 + (((int)(cam.viewy >> FRACBITS) - worldbbox[BOXBOTTOM])
                  * (SCREEN_H - 22)) / h;
        hx = (finecosine(fa) * 14) >> FRACBITS;
        hy = (finesine  (fa) * 14) >> FRACBITS;
        for (i = -1; i <= 1; i++)
        {
            diag_plot(cx + i, cy - 1, COL_ANG);
            diag_plot(cx + i, cy,     COL_ANG);
            diag_plot(cx + i, cy + 1, COL_ANG);
        }
        for (i = 1; i <= 14; i++)
            diag_plot(cx + (hx * i) / 14, cy + (hy * i) / 14, COL_ANG);
    }
}
#endif /* SATURN_RENDER_DIAG */

/* Fallback flat shading (no-flat / no-colormap renderer), rebuilt once
 * per frame and measured against the PLAYER's sector â€” the old flat
 * colour look. Only the distance ramp survives here: sh = d>>6 reaches
 * full dark at ~1024 units (the first cut's >>4 darkened every E1M1
 * wall past 240 units into its block's near-black tail). */
/* Zone-alloc'd: BSS rides the 0x060C0000 user-RAM ceiling (see the
 * col_* comment). */
static byte *floorshade;
static byte *ceilshade;
static int  flatshades_built;

static void build_flat_shades(int eye_u, int floor_u, int ceil_u)
{
    int y, d, sh;

    if (eye_u - floor_u < 1) floor_u = eye_u - 1;
    if (ceil_u - eye_u < 1)  ceil_u  = eye_u + 1;

    for (y = 0; y < SCREEN_H; y++)
    {
        if (y > HALF_H)
            d = (eye_u - floor_u) * PROJ / (y - HALF_H);
        else
            d = 0x7fffffff;
        sh = d >> 6;
        if (sh > 15) sh = 15;
        floorshade[y] = COL_FLOOR + sh;

        if (y < HALF_H)
            d = (ceil_u - eye_u) * PROJ / (HALF_H - y);
        else
            d = 0x7fffffff;
        sh = d >> 6;
        if (sh > 15) sh = 15;
        ceilshade[y] = COL_CEILING + sh;
    }
}

/*----- per-frame state used by the span painters -----*/
static int   eye_u;      /* eye height, whole units                  */
static int   viewx_u;    /* camera position, whole units             */
static int   viewy_u;
static int   cmap_shades;   /* COLORMAP rows available                */
static int   skytexnum = -1;

/* Vanilla zLight brighten ramp (ManagedDoom InitLighting): the flat
 * COLORMAP row is `start - 1280/(d+16)` â€” distance only BRIGHTENS a
 * plane toward the sector's own light (start); it never darkens past
 * it. Indexed by d>>3, entry taken at the bucket midpoint. The old
 * `+ (d >> 6)` per-row ramp was the inverse (faded every distant floor
 * to black) and the painter is shared, so the seg diff never saw it. */
static byte  zbright[256];
static int   zbright_built;

static int zbright_at(int d)
{
    int di = d >> 3;

    if (!zbright_built)
    {
        int i;

        for (i = 0; i < 256; i++)
            zbright[i] = (byte)(1280 / ((i << 3) + 20));
        zbright_built = 1;
    }
    return di > 255 ? 255 : di;
}

/*----- span painters -----
 *
 * Flats/sky are now drawn RUN-MERGED, ROW-MAJOR (see the fr_* block):
 * per column the flat spans are staged in order and merged with the
 * previous column's when their (sector, side, rows) sequence matches,
 * then painted per-run with the EXACT per-pixel expressions the old
 * column-major painters used â€” only the write iteration order differs
 * (rows outer, columns inner, contiguous fb writes instead of one
 * byte per 320-byte stride). wall spans stay column-major, but pending
 * runs are flushed BEFORE each wall span so per-column paint order
 * (flat-before-wall, and every overwrite relationship) is preserved.
 *
 * Validated 2026-08-31: sim_opt.py (rowmajor=1) renders pixel-identical
 * to sim_texed.py's baseline on 6 poses (spawn, 90/180/270 degree turns,
 * two off-spawn positions). A further 16.16 incremental-stepping
 * variant of the per-pixel wx/wy loop was ALSO validated there and
 * REJECTED: it drifts up to one flat texel (10-15% of flat pixels on
 * off-spawn poses) because the baseline's per-column FixedMul
 * truncation can't be reproduced by stepping. */

/* Max staged flat spans per column: 2 per crossing (ceil/floor) plus
 * the 2 fallback rows; MAX_CROSSINGS=16 ceilings it around 34. Overflow
 * never happens with the current walk, but emit has a safety valve. */
/* Phase 3: shared projection divide â€” see the reciprocal comment in
 * the crossing walk. Exact divide when the reciprocal wasn't built
 * (t_u < 32); otherwise a 64-bit multiply-by-reciprocal (SH-2 32x64
 * mul ~50-60 cycles vs ~250 for __sdivsi3). */
static int saturn_ydiv(int n, int yrec_on, int yrec, int t_u)
{
    if (!yrec_on)
        return n / t_u;
    return (int)(((long long)n * (long long)yrec) >> 20);
}

#define FLATSPAN_CAP 40

typedef struct
{
    sector_t      *sec;
    int            y0, y1;  /* clamped screen rows          */
    unsigned char  isfloor;
} fspan_t;

typedef struct
{
    sector_t      *sec;
    int            x0, x1; /* columns [x0, x1)             */
    int            y0, y1; /* clamped screen rows          */
    unsigned char  isfloor;
} frun_t;

/* Phase 4: flat staging/pending state exists PER SH-2 â€” both CPUs run
 * render_columns concurrently over disjoint column halves, and the run
 * merge machinery is per-column-stream mutable state. */
static fspan_t *fr_col_spans_v[2];       /* staged spans, current column */
static int      fr_col_count_v[2];
static frun_t  *fr_pending_v[2];         /* runs awaiting row-major paint */
static int      fr_pending_count_v[2];
static short    *texcols_v[2];           /* per-core hoisted sky texcols  */

/* Phase 4: the executing core is passed down the painter chain as a
 * plain ARGUMENT -- the slave is identified at its bootstrap, never
 * through shared memory (a single fr_cpu BSS variable would be one
 * location written by two CPUs). Everything below the per-frame build
 * only READS the shared tables (camlines, win_tan, col_*, sweep
 * buckets, line_len, shades); every mutable run-merge structure is
 * indexed by the core id. */
#define FR_COL_SPANS(c)     fr_col_spans_v[c]
#define FR_COL_COUNT(c)     fr_col_count_v[c]
#define FR_PENDING(c)       fr_pending_v[c]
#define FR_PENDING_COUNT(c) fr_pending_count_v[c]

/* fr_merge_column / fr_flush_pending now declared in saturn_segs.h
 * (non-static — the seg renderer calls them across the file boundary). */

/* Sky rows [r->y0, r->y1) across columns [r->x0, r->x1). The sky needs
 * the run's per-column texture index â€” hoisted out of the row loop into
 * texcols so the % w costs one divide per COLUMN (per run), not per
 * pixel like the old painter (it was a modulo per (row, column)). */
#ifdef SATURN_SEG_SHOW
/* DIAGNOSTIC ROUND 20 (2026-09-02): per-reason fallback pixel counters.
 * The pure-blue sky-region fills survived every code-read explanation â€”
 * why=4 (ti.data NULL after TexInfo success) is provably impossible from
 * saturn_tex.c, the sky/flat fallbacks paint white/salmon, and real SKY1
 * texels are greys. So the counters below attribute every fallback pixel
 * AND sample the sky composite itself: a composite buffer that reads
 * back as solid 0xC8 (stomped WRAM / alias region) is the remaining
 * candidate for the blue. Reset on the master each frame; both cores
 * increment (racy adds are fine â€” we only need "fired" and magnitude).
 * Published on the w/i HUD rows (i_video.c, SEG_SHOW build). */
volatile int saturn_why_px[6];   /* wall-span fallback pixels per why */
volatile int saturn_sky_fb_px;   /* sky fallback pixels (either branch) */
volatile int saturn_flat_fb_px;  /* flat fallback pixels (both isfloor) */
volatile int saturn_sky_null;    /* master sky TexData returned NULL */
volatile int saturn_sky_bad;     /* sky runs whose sampled texels ~ 0xC8 */
volatile int saturn_sky_runs;    /* sky runs that painted the real path */
#endif

static void paint_sky_run(const frun_t *r, int cpu)
{
    short     *texcols = texcols_v[cpu];
    byte      *sky;
    int        w, h, rowmask, y, x;
    byte     *fb;

    if (r->y1 <= r->y0)
        return;

    /* skytexnum is resolved once per frame in the master's setup pass
     * (BEFORE the two cores are dispatched) â€” the slave never writes
     * shared render state. */
    if (skytexnum < 0)
    {
        /* 2026-09-03 round 6, the motion stripe: the slave must not
         * paint the fallback — see the NoLoad note in paint_flat_run. */
        if (cpu == 1)
            return;
        fb = I_VideoBuffer + r->y0 * SCREEN_W + r->x0;
        for (y = r->y0; y < r->y1; y++)
        {
#ifdef SATURN_SEG_SHOW
            for (x = r->x0; x < r->x1; x++)
                fb[x - r->x0] = 0x04;   /* diagnostic: white = sky fallback */
            saturn_sky_fb_px += (r->y1 - r->y0) * (r->x1 - r->x0);
#else
            for (x = r->x0; x < r->x1; x++)
                fb[x - r->x0] = ceilshade[y];
#endif
            fb += SCREEN_W;
        }
        return;
    }
#ifdef SATURN_SEG_SHOW
    /* DIAGNOSTIC: a sky fallback identifies itself as PURE WHITE
     * (PLAYPAL 0x04) instead of the salmon ceilshade ramp â€” the salmon
     * blocks are otherwise indistinguishable from a ceiling-flat
     * fallback fill. */
#define SKY_FALLBACK_COL 0x04
#else
#define SKY_FALLBACK_COL ceilshade[y]   /* production: unchanged ramp */
#endif
    /* Phase 4: the slave must never run the lazy composite path
     * (W_CacheLumpNum on the slave is banned â€” it races the master's
     * GFS handle). Cached pixels or a one-frame fallback fill. */
    sky = (cpu == 1) ? Saturn_TexDataNoLoad(skytexnum)
                     : Saturn_TexData(skytexnum);
    w = Saturn_TexWidth(skytexnum);
    h = Saturn_TexHeight(skytexnum);
    if (!sky || w < 1 || h < 1)
    {
        /* 2026-09-03 round 6, the motion stripe: the slave must not
         * paint the fallback — see the NoLoad note in paint_flat_run. */
        if (cpu == 1)
            return;
#ifdef SATURN_SEG_SHOW
        if (cpu == 0 && !sky) saturn_sky_null++;
#endif
        fb = I_VideoBuffer + r->y0 * SCREEN_W + r->x0;
        for (y = r->y0; y < r->y1; y++)
        {
            for (x = r->x0; x < r->x1; x++)
                fb[x - r->x0] = SKY_FALLBACK_COL;
            fb += SCREEN_W;
        }
#ifdef SATURN_SEG_SHOW
        saturn_sky_fb_px += (r->y1 - r->y0) * (r->x1 - r->x0);
#endif
        return;
    }

    /* Sky rows are flat pixels too: same kernel window as paint_flat_run
     * (master-gated, see the frt_ploop_acc note at the publish site). */
    {
        unsigned int tq_ = SAT_FRT();

#ifdef SATURN_SEG_SHOW
        saturn_sky_runs++;
        /* Diagnostic: sample the composite across the run â€” a buffer
         * stomped to a constant (e.g. 0xC8) reads back uniform. */
        {
            int hits = 0;
            for (x = 0; x < 32; x++)
                if (sky[((r->y0 + 3) % h) * w + ((x * 97) % w)] == 0xC8)
                    hits++;
            if (hits > 24)
                saturn_sky_bad++;
        }
#endif

        for (x = r->x0; x < r->x1; x++)
            texcols[x - r->x0] = (short)(col_sky[x] % w);
        rowmask = ((h & (h - 1)) == 0) ? h - 1 : 0;
        fb = I_VideoBuffer + r->y0 * SCREEN_W + r->x0;
        for (y = r->y0; y < r->y1; y++)
        {
            int texrow = rowmask ? (y & rowmask) : (y % h);
            const byte *src = sky + texrow * w;

            for (x = 0; x < r->x1 - r->x0; x++)
                fb[x] = src[texcols[x]];
            fb += SCREEN_W;
        }
        if (cpu == 0)
            frt_ploop_acc += (unsigned short)(SAT_FRT() - tq_);
    }
}

#if SATURN_GREY_TEX
/* BISECT: one 64x64 grey pattern in WRAM .rodata replaces every wall
 * texture AND flat. Same span math, same COLORMAP per pixel â€” only the
 * texel SOURCE moves (fast cached WRAM-H instead of random cart reads).
 * J drops a lot -> cart texel fetches own the frame. */
static const byte saturn_grey_tex[64 * 64] = { [0 ... 64 * 64 - 1] = 0x58 };

static void saturn_grey_override(stexinfo_t *ti)
{
    ti->data  = (byte *)saturn_grey_tex;
    ti->w     = 64;
    ti->h     = 64;
    ti->pow2  = 1;
    ti->wmask = 63;
}
#define GREY_TEX(ti)   saturn_grey_override(ti)
#define GREY_FLAT()    saturn_grey_tex
#else
#define GREY_TEX(ti)   ((void)0)
#define GREY_FLAT()    ((void)0)
#endif

/* Row-major paint of one merged flat run, [x0,x1) columns by [y0,y1)
 * rows. Per-pixel math is byte-for-byte the expressions of the old
 * paint_flat_span; only the loop order changed (per row: distance from
 * rowinv and shade are row-constants, then contiguous x sweep). */
/* Hand-scheduled SH-2 pixel kernels (saturn_draw.s) â€” adapted from
 * d32xr's 32X drawers, bit-identical to the C loops below. */
extern void saturn_wall_col_asm(byte *fb, const byte *cmaprow,
                                const byte *tex, int count,
                                int v0, int vstep, int vh,
                                int w, int utex);
extern void saturn_flat_row_asm(byte *fb, int count,
                                const fixed_t *wxs, const fixed_t *wys,
                                int viewx_u, int viewy_u, int d,
                                const byte *flat, const byte *cmaprow);
/* Stepping flat kernel (2026-09-02): u/v seed + per-pixel ADD, no
 * per-pixel dmuls/loads. Sample-then-add â€” pixel 0 bit-exact vs the
 * old kernel, later pixels <=1 texel drift (truncation carry). */
extern void saturn_flat_step_asm(byte *fb, int count, int u_fx, int v_fx,
                                 int ustep, int vstep, const byte *flat,
                                 const byte *cmaprow);

/* BISECT rounds 3-4 (2026-09-02): emit-vs-paint probe for the hole band
 * cols [23,35). fd_emit = fr_emit_flat calls at those columns; fd_paint
 * = paint_flat_run runs covering any of them with valid ys; fd_bad =
 * runs covering them but y1<=y0 (corrupted coords); fd_stage = spans
 * staged into pending at those columns; fd_flush = pending runs at the
 * frame-end flush on cpu1. Reset on cpu0's frame start. */
int saturn_fd_emit  = 0;
int saturn_fd_paint = 0;
int saturn_fd_bad   = 0;
int saturn_fd_stage = 0;
int saturn_fd_flush = 0;
/* BISECT round 20: every cpu1 painted run overlapping [20,40) is
 * recorded here (walk order), then drawn at walk end as black
 * vertical strokes so a capture shows exactly what painted where. */
int saturn_pr_n = 0;
int saturn_pr_x0[24], saturn_pr_x1[24], saturn_pr_y0[24],
    saturn_pr_y1[24], saturn_pr_fl[24];
/* Round 27i (2026-09-02): fork probe â€” did ANY cpu1 painted run cover
 * part of the hole band [23,35)? cov counts such runs; last_x/last_y
 * hold (x0<<8)|x1 / (y0<<8)|y1 of the most recent one. cov == 0 means
 * the staged spans never reached paint_flat_run (loss upstream in the
 * merge); cov > 0 with the band still clear means paint_flat_run got
 * the runs and failed to write pixels. */
int saturn_pr_cov   = 0;
volatile int saturn_pr_last_x = 0;
volatile int saturn_pr_last_y = 0;
/* BISECT round 9: per-column ground truth at band col 28.
 *   fd28_span = (y0<<8)|y1 of the LAST valid fr_emit_flat at col 28
 *   (0 = none this frame); fd28_floor = its isfloor bit.
 *   dbg_s_clip = the SLAVE's final clipbounds[28] after the final fill
 *   ((ce<<8)|fl; fl<=ce = the band was closed, open = nearpaint skip). */
volatile int saturn_fd28_span  = 0;
volatile int saturn_fd28_floor = 0;
volatile int saturn_fd28_cnt   = 0;   /* Round 27c: spans staged at col 28 this frame */
volatile int saturn_fd60_span  = 0;   /* same record inside the black slab (col 60) */
volatile int saturn_fd60_floor = 0;
volatile int saturn_fd60_cnt   = 0;
volatile int saturn_dbg_s_clip = 0;
/* Round 27j (2026-09-03): pending-run fate probes. The 27i fork probe
 * proved NO cpu1 painted run covers the hole band [23,35), yet the
 * staging records (fd28) prove valid spans are staged at col 28 every
 * frame, and fd_stage==0 proves every band-column merge takes the
 * EXTEND path â€” so a pending run covering the band must exist. These
 * record pending[0] at the two exits that could paint it: the
 * mismatch path's paint loop (wherever the mismatch lands, when
 * pending[0] covers part of the band) and fr_flush_pending (same
 * condition). 0 = that exit never saw a band-covering run this frame. */
volatile int saturn_dbg_fpx = 0, saturn_dbg_fpy = 0, saturn_dbg_fpn = 0;

/* Row-index LUT moved up here (2026-09-03, round 8): paint_flat_run's
 * hoisted light path indexes it directly — see Saturn_LightRow below
 * for the curve it implements and the luminance remap it encodes. */
static const byte d32xr_row_lut[32] = {
      0,  0,  0,  0,  0,  0,  1,  1,  1,  2,  3,  3,  4,  5,  5,  6,
      7,  8,  9, 10, 11, 11, 12, 13, 14, 15, 17, 19, 22, 24, 27, 30
};

/* ---- OCRAM reciprocal divide LUT (2026-09-08, SATURN_DIVLUT) --------
 * Ported from the SCUDSP-rig campaign's V3ocr leg (w37, real-HW
 * verdict): 1080-1350 k-div/s from the SH7604's on-chip 2KB scratchpad
 * vs DIVU 490-540 and gcc 337-360. Table: lut[d] = ceil(2^32/d) for
 * d = 2..511, computed+installed by Saturn_Divlut_Init (d_main.c) at
 * boot; 510 u32 = 2040 B fits the split-mode scratchpad exactly.
 * Consume: a/d == mulhi(a, lut[d]) via dmulu.l + sts mach (2 instr).
 * EXACTNESS (proven by sim/sim_divlut.py, 332855 divisions, 0 fails):
 * exact for every a with a*d < 2^32. Callers must uphold that bound —
 * there is no per-call check (one dmulu per row would eat the win). */
#ifdef SATURN_DIVLUT
#ifdef SATURN_DIVLUT_NOOCRAM
/* Kronos diagnostic variant: WRAM .bss table (d_main.c), no CCR
 * writes, no OCRAM access. */
extern volatile unsigned int sat_divlut_table[512];
#define SATURN_OCLUT sat_divlut_table
#else
#define SATURN_OCLUT ((volatile unsigned int *)0xC0000000u)
#endif

/* Defined in d_main.c: ok = boot install verified, on = live A/B
 * toggle (I_ReadControls, default on when ok). */
extern volatile int saturn_divlut_ok;
extern volatile int saturn_divlut_on;

static inline unsigned int Saturn_MulHi(unsigned int a, unsigned int b)
{
    unsigned int hi;

    __asm__ __volatile__(
        "dmulu.l %2, %1\n"
        "sts mach, %0"
        : "=&r"(hi) : "r"(a), "r"(b));
    return hi;
}

/* Census (snapshot+reset once per frame in i_video.c's perf rows):
 * lhits = LightRow/light_div LUT divides, dfallback = the DIVU/C
 * fallbacks they would have taken. */
volatile unsigned int sat_divlut_lhits     = 0;
volatile unsigned int sat_divlut_dfallback = 0;
#endif /* SATURN_DIVLUT */

/* Round 8 (2026-09-03, the 32X-level push): 32/32 non-negative divide,
 * DIVU when d_main's boot probe proved the unit, else the C operator.
 * Used by the hoisted flat-light path — per-ROW in the fade band, so
 * the call must stay cheap (the DIVU turn-around is the floor). */
static int saturn_light_div(int num, int den)
{
    extern volatile int saturn_divu_probe;
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 5)
    /* LUT path (2026-09-08): exact iff num*den < 2^32 and 2 <= den
     * <= 511. All three call sites pass num = lt_dl*350 <= 89250, so
     * num*511 <= 45.6M < 2^32 — the bound sim_divlut.py proved is
     * upheld by construction and the guard is range-only. 2026-09-08
     * real-HW 1fps hunt: the old guard ALSO computed 0xFFFFFFFFu/den —
     * a full __udivsi3 per lookup, making the "fast" path a divide
     * PLUS a multiply. Range-only guard + the on-check (the A+B+C
     * toggle must reach this site too; it previously did not). */
    if (saturn_divlut_on && den >= 2 && den <= 511)
    {
        unsigned int q = Saturn_MulHi((unsigned int)num,
                                      SATURN_OCLUT[den]);

        sat_divlut_lhits += 1u;
        return (int)q;
    }
    sat_divlut_dfallback += 1u;
#endif
    if (saturn_divu_probe)
    {
        volatile int *const dvsr  = (volatile int *)0xFFFFFF00u;
        volatile int *const dvdnt = (volatile int *)0xFFFFFF04u;

        *dvsr  = den;
        *dvdnt = num;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          ::: "memory");
        return *dvdnt;
    }
    return num / den;
}

byte Saturn_LightRow(int lightlevel, int dist);   /* defined below */

static void paint_flat_run(const frun_t *r, int cpu)
{
    byte    *cmap = Saturn_Colormap();
    byte    *flat;
    int      h_u, K, sh_base;
    int      dh0 = 0;   /* signed height-above-eye: <0 = far-side plane */
    int      y, x;

    if (r->y1 <= r->y0)
    {
        if (r->x0 < 35 && r->x1 > 23)
        {
            int mxx;

            saturn_fd_bad++;
#ifdef SATURN_FLAT_DIAG
            /* BISECT round 15: chain flag â€” degenerate run. Row 7.
             * Round-31: gated like the other probes â€” this painted
             * 0xFC (= PLAYPAL magenta) in the production build. */
            for (mxx = r->x0; mxx < r->x1; mxx++)
                if (mxx >= 23 && mxx < 35)
                    I_VideoBuffer[7 * SCREEN_W + mxx] = 0xFC;
#endif
        }
        return;
    }

#ifdef SATURN_FLAT_DIAG
    /* BISECT round 2 (2026-09-02): every flat run paints a SOLID colour
     * â€” floor runs white (0x04), ceiling runs magenta (0xFC). If the
     * missing bands stay clear, the staged runs never paint; if they
     * light up somewhere else, the run coordinates are wrong. */
    {
        byte *dfb = I_VideoBuffer + r->y0 * SCREEN_W + r->x0;
        byte  dcol = r->isfloor ? 0x04 : 0xC0;  /* floor white, ceiling
                                                   pale blue â€” 0xFC is
                                                   the HUD digit colour */
        int   dyy, dxx;

        if (r->x0 < 35 && r->x1 > 23)
        {
            int mxx;

            saturn_fd_paint++;
            /* BISECT round 15: chain flag â€” a run PAINTED over these
             * columns. Row 6 (top bar), full width. */
            for (mxx = r->x0; mxx < r->x1; mxx++)
                if (mxx >= 0 && mxx < SCREEN_W)
                    I_VideoBuffer[6 * SCREEN_W + mxx] = 0xFC;
        }
        /* BISECT round 20: record every cpu1 painted run overlapping
         * [20,40) â€” drawn as black strokes at walk end. */
        if (cpu == 1 && r->x0 < 40 && r->x1 > 20 && saturn_pr_n < 24)
        {
            saturn_pr_x0[saturn_pr_n] = r->x0;
            saturn_pr_x1[saturn_pr_n] = r->x1;
            saturn_pr_y0[saturn_pr_n] = r->y0;
            saturn_pr_y1[saturn_pr_n] = r->y1;
            saturn_pr_fl[saturn_pr_n] = r->isfloor;
            saturn_pr_n++;
        }
        /* Round 27i: band-coverage fork probe (see declaration). */
        if (cpu == 1 && r->x1 > 23 && r->x0 < 35)
        {
            saturn_pr_cov++;
            saturn_pr_last_x = ((unsigned int)r->x0 << 8)
                             | (unsigned int)r->x1;
            saturn_pr_last_y = ((unsigned int)r->y0 << 8)
                             | (unsigned int)r->y1;
        }
        /* BISECT round 9: fb-marker for painted runs â€” rows 16/17
         * (ceiling/floor run) spanning [x0,x1), black. */
        {
            byte *mr = I_VideoBuffer + (r->isfloor ? 17 : 16) * SCREEN_W;
            int   mx;

            for (mx = r->x0; mx < r->x1; mx++)
                if (mx >= 0 && mx < SCREEN_W)
                    mr[mx] = 0x00;
        }
        for (dyy = r->y0; dyy < r->y1; dyy++, dfb += SCREEN_W)
            for (dxx = 0; dxx < r->x1 - r->x0; dxx++)
                dfb[dxx] = dcol;
        /* BISECT round 17: outline every painted run in red so one
         * capture shows each run's exact extent. */
        {
            byte *tb = I_VideoBuffer + r->y0 * SCREEN_W;
            byte *lb = I_VideoBuffer + (r->y1 - 1) * SCREEN_W;
            int   oxx;

            for (oxx = r->x0; oxx < r->x1; oxx++)
                if (oxx >= 0 && oxx < SCREEN_W)
                {
                    tb[oxx] = 0xB0;
                    lb[oxx] = 0xB0;
                }
        }
        return;
    }
#endif

    if (cpu == 0)
    {
        saturn_cnt_fruns++;
        saturn_cnt_frows += r->y1 - r->y0;
        saturn_cnt_fpx += (r->y1 - r->y0) * (r->x1 - r->x0);
    }

    /* F_SKY1 ceilings become sky runs. */
    if (!r->isfloor && *(int8_t *)&r->sec->ceilingpic == -1)
    {
        paint_sky_run(r, cpu);
        return;
    }

    flat = (cpu == 1)
         ? Saturn_FlatDataNoLoad(r->isfloor ? r->sec->floorpic
                                            : r->sec->ceilingpic)
         : Saturn_FlatData(r->isfloor ? r->sec->floorpic
                                      : r->sec->ceilingpic);
#if SATURN_GREY_TEX
    flat = GREY_FLAT();
#endif
    if (!flat || !cmap)
    {
        /* 2026-09-03 round 6, the MOTION STRIPE root cause: on a
         * first-sight flat the slave's NoLoad fetch returns NULL for
         * one frame (the preload walk only re-arms the FOLLOWING
         * frame), and this fallback then painted ceilshade/floorshade
         * (the pink/salmon 0x10+sh ramp) for that frame. In motion a
         * grazing 1px column sliver of newly-revealed geometry hits
         * the miss every frame, painting a 1px full-band pink stripe
         * that wanders with the view and vanishes when static (no
         * first-sights). All video sightings sat inside the slave's
         * half [0, split_x). The wall painter's own comment documents
         * the intended behaviour ("draws nothing for that span this
         * frame"), so the slave now SKIPS on a miss: the master
         * composites the texture and everyone paints it from the next
         * frame; the 1-frame hole holds the previous frame's content,
         * invisible. The MASTER keeps its ramp: its fetch composites
         * synchronously, so a master miss is a genuine failure worth
         * seeing in colour. Same skip in paint_sky_run and the
         * paint_wall_span why-fallback. */
        if (cpu == 1)
            return;
        /* Fallback: the old flat-colour ramp â€” per-row constant. */
        byte *fb = I_VideoBuffer + r->y0 * SCREEN_W + r->x0;

        for (y = r->y0; y < r->y1; y++)
        {
            byte col = r->isfloor ? floorshade[y] : ceilshade[y];

            for (x = 0; x < r->x1 - r->x0; x++)
                fb[x] = col;
            fb += SCREEN_W;
        }
#ifdef SATURN_SEG_SHOW
        saturn_flat_fb_px += (r->y1 - r->y0) * (r->x1 - r->x0);
#endif
        return;
    }

    h_u = (int)((r->isfloor ? r->sec->floorheight
                            : r->sec->ceilingheight) >> FRACBITS);
    /* Round 26 (2026-09-02): planes on the FAR side of the horizon are
     * real, visible surfaces â€” a FLOOR above the eye (raised platform
     * seen through an opening) projects ABOVE the horizon, a CEILING
     * below the eye (lower room seen from above) BELOW it. The
     * reference renderer uses the absolute height difference and no
     * horizon-side filter at all (ManagedDoom DrawFloorColumn:
     * height = Fixed.Abs(floorHeight - viewZ)). The old code clamped
     * K<0 to 0 (garbage d=0 texel scale) AND the row loop below
     * skipped every far-side row, so those runs staged, merged and
     * then painted NOTHING â€” the clear slabs/bands through openings
     * (upper band rows ~15-64 = staged isfloor=1 span [15,64) from the
     * W/HUD evidence, lower band = a below-eye ceiling run). */
    {
        int dh = (r->isfloor ? eye_u - h_u : h_u - eye_u);

        dh0 = dh;
        K = dh < 0 ? -dh : dh;
    }
    K *= PROJ;
    if (K > 30000) K = 30000;     /* keeps K*rowinv inside 32 bits */
    (void)sh_base;  /* flats now shade through Saturn_LightRow below */
    {
        int spanw = r->x1 - r->x0;
        /* Round 8 hoisted light state (see the derivation below):
         * per-RUN constants of the d32xr plane-lighting curve. */
        int lt_Dnear = 0, lt_Dfar = 0x7FFFFFFF;
        int lt_sh_const = 0, lt_ri_near = 0, lt_ri_far = 0;
        int lt_dl = 0;
        /* Stepping-kernel setup (2026-09-02): the per-column direction
         * DIFFERENCE at the run's first column is a run constant â€” only
         * d changes per row. u/v step in .16 fixed point per pixel;
         * the texel index uses bits [21:16] so 64-wrap and int32
         * overflow both reduce correctly (2^22 | 2^32). */
        int wxs_d = (r->x0 + 1 < SCREEN_W)
                  ? col_wxs[r->x0 + 1] - col_wxs[r->x0] : 0;
        int wys_d = (r->x0 + 1 < SCREEN_W)
                  ? col_wys[r->x0 + 1] - col_wys[r->x0] : 0;
        /* Round 8 (32X-level push): HOIST the light curve out of the row
         * loop. Saturn_LightRow re-derived lightmin/dl and re-ran the
         * DIVU prologue for every row of every run — with ~3800 rows per
         * frame the prologue alone rivalled the stepping kernel. All of
         * dl/seglight/lightmin are run constants; only d changes per
         * row. L(d) = (dl*350)/d - dl/4 is monotone non-increasing in
         * d, so the two clamp edges are exact threshold distances via
         *   floor(a/b) >= A  <=>  b <= floor(a/A)
         *   floor(a/b) <= B  <=>  b >= floor(a/(B+1)) + 1
         * Inside the band L lies strictly in (lightmin, seglight), so
         * Saturn_LightRow's clamps and its ri>31 clamp are provably
         * no-ops there (kept below for safety). The threshold divides
         * go through saturn_light_div: a C '/' here would be the
         * ~200-cycle shift-subtract, twice per run. */
        {
            int ll  = r->sec->lightlevel;
            int lmn;

            if (ll < 0)  ll = 0;
            if (ll > 255) ll = 255;
            lmn = ll - (255 - ll - ll / 2) * 2;
            if (lmn < 0)        lmn = 0;
            if (lmn > ll)       lmn = ll;
            lt_dl = ll - lmn;
            if (lt_dl == 0)
            {
                /* Constant row (already-saturated sector light). */
                lt_sh_const = d32xr_row_lut[(255 - ll) >> 3];
            }
            else
            {
                int A   = ll + (lt_dl >> 2);
                int B   = lmn + (lt_dl >> 2);

                lt_Dnear   = saturn_light_div(lt_dl * 350, A);
                lt_Dfar    = saturn_light_div(lt_dl * 350, B + 1) + 1;
                lt_ri_near = ((255 - ll) >> 3);
                lt_ri_far  = ((255 - lmn) >> 3);
                if (lt_ri_near > 31) lt_ri_near = 31;
                if (lt_ri_far > 31)  lt_ri_far = 31;
            }
        }
        /* Kernel-loop-only window: everything from here to the loop end
         * is the per-row/per-pixel drawing; paint-flat-run total minus
         * this = per-run entry overhead (colormap/flat fetch, setup). */
        unsigned int tq_ = SAT_FRT();

        for (y = r->y0; y < r->y1; y++)
        {
            int dy  = y - HALF_H;
            int ady = dy < 0 ? -dy : dy;
            int d, sh;
            byte *fb;

            /* Only the rows on the plane's visible side of the horizon
             * hit it. Round 26: the visible side depends on which side
             * of the eye the plane is on â€” a floor above the eye paints
             * rows ABOVE the horizon (dy < 0), a ceiling below the eye
             * rows BELOW it (dy > 0); the horizon row itself stays
             * unpainted in all four cases (d diverges there). */
            if (r->isfloor ? (dh0 < 0 ? dy >= 0 : dy <= 0)
                          : (dh0 < 0 ? dy <= 0 : dy >= 0))
                continue;

            d = (K * rowinv[ady]) >> 16;
            if (d > 20000)
                d = 20000;      /* horizon rows: keeps wxs*d in 32 bits */
            /* d32xr plane lighting (r_phase7.c:406-428), round-8 hoist:
             * thresholds precomputed above; bit-exact with the full
             * Saturn_LightRow (near/far clamp edges are exact, and the
             * in-band raw L is provably inside [0,255]). */
            if (lt_dl == 0)
                sh = lt_sh_const;
            else if (d <= lt_Dnear)
                sh = lt_ri_near;
            else if (d >= lt_Dfar)
                sh = lt_ri_far;
            else
            {
                int L_ = saturn_light_div(lt_dl * 350, d) - (lt_dl >> 2);

                sh = d32xr_row_lut[(255 - L_) >> 3];
            }
            if (sh < 0) sh = 0;
            if (sh > 31) sh = 31;
#ifdef SATURN_SH_SPLIT
            if (r->x0 < SCREEN_W / 2)
                sh = 0;         /* left half fullbright (round 22) */
#endif

            fb = I_VideoBuffer + y * SCREEN_W + r->x0;
#if SATURN_PAINT_SOLID
            /* BISECT: skip the per-pixel world math and cart fetches;
             * keep the row loop and span machinery. */
            {
                byte col = (byte)(sh * 8 + (y & 7));
                for (x = 0; x < spanw; x++)
                    fb[x] = col;
                continue;
            }
#endif
#if !defined(SATURN_NO_ASM_DRAW) && !defined(SATURN_NO_ASM_FLAT) \
    && !defined(SATURN_SEG_SHOW)
            /* spanw > 0 guard: the kernel's dt-loop terminates on count
             * wrapping to -1, so a degenerate span must stay in C (the
             * C loop no-ops on it). SEG_SHOW round 21: the C loop runs
             * instead â€” flats paint a high-palette encoding of the raw
             * texel below (asm can't). */
            if (spanw > 0)
            {
                int u_fx = ((viewx_u
                             + ((col_wxs[r->x0] * d) >> 16)) & 63) << 16;
                int v_fx = ((viewy_u
                             + ((col_wys[r->x0] * d) >> 16)) & 63) << 16;

                saturn_flat_step_asm(fb, spanw, u_fx, v_fx,
                                     wxs_d * d, wys_d * d, flat,
                                     cmap + (sh << 8));
            }
#else
            /* C reference of the stepping kernel (sim-anchored). */
            {
                int u_fx = ((viewx_u + ((col_wxs[r->x0] * d) >> 16)) & 63)
                           << 16;
                int v_fx = ((viewy_u + ((col_wys[r->x0] * d) >> 16)) & 63)
                           << 16;
                int ustep = wxs_d * d, vstep = wys_d * d;

                for (x = 0; x < spanw; x++)
                {
                    byte ftx = flat[(((v_fx >> 16) & 63) << 6)
                                    | ((u_fx >> 16) & 63)];
#ifdef SATURN_SEG_SHOW
                    /* Round 21: flat pixels land in the HIGH palette
                     * block ((texel>>1)+0x80) â€” unmistakable against
                     * raw-texel walls (low block) and the grey sky.
                     * Shows the flat texel itself AND flat ownership. */
                    fb[x] = (byte)((ftx >> 1) + 0x80);
#else
                    fb[x] = cmap[(sh << 8) + ftx];
#endif
                    u_fx += ustep;
                    v_fx += vstep;
                }
            }
#endif
        }
        /* Master-gated: paint_flat_run runs on BOTH cores through the
         * seg renderer's flush machinery, and this static is shared â€”
         * racing += would drop increments. The stage rows are
         * master-half-only anyway (T likewise). */
        if (cpu == 0)
            frt_ploop_acc += (unsigned short)(SAT_FRT() - tq_);
    }
}

/* Stage one flat span for the current column. */
void fr_emit_flat(int x, int y0, int y1, sector_t *sec, int isfloor,
                  int cpu)
{
#ifdef SATURN_SEG_DIFF
    /* Flat probe: which floor band did each pass EMIT at the probe
     * column (pairs with the blue seg-hole regions in the diff). The
     * probe globals live just below paint_wall_span â€” declare here. */
    extern volatile int saturn_diff_probe_col, saturn_diff_ray_pass;
    extern volatile int saturn_probe_seg_floor, saturn_probe_ray_floor;
#endif
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    if (y1 <= y0)
        return;
    if (cpu == 0)
        saturn_cnt_femit += y1 - y0;   /* round 8b: staging volume */
    if (x >= 23 && x < 35)
    {
        saturn_fd_emit++;
#ifdef SATURN_FLAT_DIAG
        /* BISECT round 9: fb-marker probe â€” rows 12/14 (ceiling/floor
         * emit) inside the view; magenta stands out on the pale-blue/
         * white diag fills. No cross-core counter reads involved. */
        I_VideoBuffer[(isfloor ? 14 : 12) * SCREEN_W + x] = 0xFC;
#endif
    }
    /* Round 27c (2026-09-02): LIVE staging record, unconditionally â€”
     * the fd28 record above sat inside SATURN_FLAT_DIAG, which the
     * production Makefile does NOT define, so the HUD's W/A rows read
     * values left by a long-dead diagnostic build (the pcnt=897 /
     * en=82 "constants" were exactly that). Record the LAST span
     * staged at col 28 (the red-bar pose) and col 60 (the black-slab
     * pose) plus a per-frame stage count. */
    if (x == 28)
    {
        saturn_fd28_span  = (y0 << 8) | y1;
        saturn_fd28_floor = isfloor ? 1 : 0;
        saturn_fd28_cnt++;
    }
    else if (x == 60)
    {
        saturn_fd60_span  = (y0 << 8) | y1;
        saturn_fd60_floor = isfloor ? 1 : 0;
        saturn_fd60_cnt++;
    }
#ifdef SATURN_SEG_DIFF
    /* Accumulate painted floor ROWS, not the last band: the seg pass
     * emits floor in multiple ranges per column and last-writer hid
     * all but one (r read 0 while the ray painted rows). */
    if (x == saturn_diff_probe_col && isfloor)
    {
        if (saturn_diff_ray_pass)
            saturn_probe_ray_floor += y1 - y0;
        else
            saturn_probe_seg_floor += y1 - y0;
    }
#endif
    (void)x;
    if (FR_COL_COUNT(cpu) >= FLATSPAN_CAP)
    {
        /* Safety valve (unreachable with the current walk): merge +
         * flush everything, then paint this span as a 1-column run. */
        frun_t r;

        fr_merge_column(x, cpu);
        fr_flush_pending(cpu);
        r.sec = sec; r.x0 = x; r.x1 = x + 1;
        r.y0 = y0;   r.y1 = y1;
        r.isfloor = (unsigned char)(isfloor != 0);
        paint_flat_run(&r, cpu);
        return;
    }
    /* BISECT bypass removed (2026-09-02): direct paint proved the
     * emission math is right; staging is back under test with
     * -DSATURN_FLAT_DIAG solid-colour flat runs. */
    FR_COL_SPANS(cpu)[FR_COL_COUNT(cpu)].sec = sec;
    FR_COL_SPANS(cpu)[FR_COL_COUNT(cpu)].y0 = y0;
    FR_COL_SPANS(cpu)[FR_COL_COUNT(cpu)].y1 = y1;
    FR_COL_SPANS(cpu)[FR_COL_COUNT(cpu)].isfloor = (unsigned char)(isfloor != 0);
    FR_COL_COUNT(cpu)++;
}

/* fr_flush_for_wall removed (2026-09-01): its only callers were the
 * per-wall-span flushes, now deferred â€” see the call sites. */

/* 2026-09-03 photos #158/#159: P only wraps the flat EMIT (staging)
 * calls, so the flat paints that fire from the MERGE paths —
 * fr_merge_column's mismatch repaint and fr_flush_pending — were
 * landing unmeasured in K (glue). This counts the vblanks spanned by
 * those paint loops, master side only (cpu==0): the slave's writes
 * would race the master's cached HUD read. */
int saturn_frt_vmerge = 0;

/* Round 8b (2026-09-03 23:52 ISO, the 32X-level push): S (the macc
 * remainder) read 3 vbl of GLUE — ~640 cycles per (piece,column) pair
 * at s=337. This accumulates (master side only, >>8 like ploop) the
 * MATCH/BOOKKEEPING phase of fr_merge_column — entry to just before
 * its paint loops — i.e. the span-sequence compare + pending refill
 * the walk pays TWICE per pair (per-seg flush + open-band flush).
 * S - q = the rest of the pair glue (DIVU setup, finetangent,
 * FixedMuls, clip math). Prints on the q row. (The OLD frt_merge_acc
 * at the raycaster's call site is dead in the seg production build —
 * this one lives inside fr_merge_column itself.) */
static unsigned int frt_mbook_acc = 0;
int saturn_frt_mbook = 0;

/* Round 8b: total flat SPAN rows EMITTED per frame (master side, one
 * entry per (piece,column) band) — the pre-merge staging volume.
 * F counts PAINTED run px; F - j = the volume the merge machinery's
 * extends dedup already removed, and j vs the ~16K half-screen area
 * measures the geometric nesting overdraw (every portal piece
 * re-emits its full floor/ceiling band per column). Prints on the
 * j row. */
int saturn_cnt_femit = 0;

/* =====================================================================
 * 2026-09-03 round 7, the RED SCREEN: "sometimes the whole screen goes
 * red" = the slave-hang watchdog (dispatch block below) fired — the
 * slave took >60 vbl (~1 s) to reach its done flag. saturn_slave_heart
 * localizes where: the slave publishes its running (piece,column) pair
 * count through the P2 uncached mirror every column of the wall walk
 * (+1000 per column of the open-band fill, so the two passes are
 * distinguishable); the watchdog snapshots it when it paints red, and
 * the M row prints the delta vs dispatch:
 *   0         = the slave never painted a column (stuck before/at
 *               dispatch, or in pre-walk glue)
 *   small N   = stuck at wall-walk pair N
 *   ~N*1000   = stuck N columns into the open-band fill
 *   huge/growing across reds = alive but crawling (perf cliff)
 * The C row carries (red events << 8) | stall vblanks (capped 255), the
 * Y row a build stamp so a burned disc is identifiable on camera.
 * ===================================================================== */
/* Round-34 CACHE-LINE FIX: the published heart is its OWN 16-byte
 * cache line (aligned + padded, exactly one SH-2 OC line), touched
 * ONLY through SATURN_HEART_MIRROR by both CPUs — never through the
 * cache. The old saturn_slave_heart sat at 0x0607194C, the same line
 * as saturn_heart_pairs (0x06071948 — the slave increments it CACHED)
 * and the dbg counters (0x06071940/44 — the master writes those
 * cached): every eviction of that line in either CPU's OC wrote the
 * STALE heart slot back over the slave's uncached mirror writes.
 * Proof (2026-09-04 flight record): the mirror read 0x18900000 at
 * dispatch AND twice at the watchdog fire, then 0x00005F26 on a
 * fourth read microseconds later — a stale cached copy leaking
 * through, not a live value. [0] = pair/fill heart, [1] = slave-entry
 * marker (0x5217 once the slave callback runs, reset by the master at
 * dispatch) so the verdict can split "never dispatched" from
 * "dispatched, died before the first heart". */
volatile unsigned int saturn_heart_pub[4]
    __attribute__((aligned(16))) = {0, 0, 0, 0};
int                  saturn_heart_pairs = 0;
static unsigned int  saturn_heart_pre   = 0;
/* Round-36: slave self-bail deadline (vblank count). Set by the slave
 * callback before its render half; saturn_segs.c's wall walk and final
 * fill bail out (saturn_seg_bailed) once the uncached vblank count
 * passes it. 0 = no deadline (the master's half never bails — its
 * check is also cpu==1-gated). */
volatile unsigned int saturn_slave_bail_vbl = 0;
/* Round-40: MASTER-side bail deadlines. The 10:42 round-39 run froze
 * SILENTLY on a normal frame (bkram untouched — the watchdog never
 * fired): the wedge hit the MASTER's own path and nothing guarded it —
 * the red watchdog only watches the slave's done flag inside the join
 * poll, and the master's build/half run BEFORE that poll.
 *   saturn_master_bail_vbl — the cpu-0 render half's deadline (vbl+60)
 *   saturn_build_bail_vbl  — the pre-dispatch BSP build's (vbl+30)
 *   saturn_bail_arm_vbl/_frt — vblank + FRT latched at arm time: if
 *       the FRT advances ~0xC000 ticks while the vblank count does not
 *       move, the VBLANK CHAIN itself is dead (the round-37 frame-1
 *       freeze mode) and every vbl-based guard is blind — the walk
 *       bails on this instead (saturn_seg_bailed = 4). */
volatile unsigned int saturn_master_bail_vbl = 0;
volatile unsigned int saturn_build_bail_vbl  = 0;
volatile unsigned int saturn_bail_arm_vbl    = 0;
volatile unsigned int saturn_bail_arm_frt    = 0;
/* Round-45: anything the SLAVE reads must reach WRAM. A cached master
 * write sits dirty in this core's OC (caches don't snoop), and if a
 * hot master loop keeps touching that line the write-back never
 * happens. Publish-through-mirror every value the slave's bail logic
 * consumes. */
#define SAT_PUBLISH(var)                                                  \
    (*(volatile unsigned int *)                                           \
         ((unsigned int)&(var) + 0x20000000u) = (var))
/* Round-48: the PREVIOUS job's exit state, captured at each dispatch
 * before the heart mirror is wiped (see the dispatch site). Stamped
 * into the verdict record: when E=0 says "slave never entered", these
 * split "wedged inside its last job" from "wedged in SGL's
 * between-jobs loop". */
volatile unsigned int saturn_prev_pub0 = 0;
volatile unsigned int saturn_prev_pub2 = 0;
volatile unsigned int saturn_prev_pub3 = 0;
/* Round-50: the ISR-side freeze reporter's inputs. The rounds-47/48/49
 * silent freeze wedges the MASTER main loop below every check-call
 * granularity (no red screen, no record — the reporter lives on the
 * frozen loop), so the VBLANK ISR now does the reporting:
 *   saturn_master_beat — incremented at every check site (build node,
 *       walk pair, join-poll iteration, D_DoomLoop head). Any wedge
 *       inside a single call stops it; the ISR fires when it sits
 *       still 120 vbl (2 s — nothing legit stops the loop that long).
 *   saturn_master_site — last coarse landmark, stamped at the frame
 *       skeleton (see the arm/dispatch/join sites) and 0x40|site
 *       inside saturn_segs.c's fine checks. Printed on the verdict.
 *   saturn_freeze_watch — armed at the first build arm, so the boot's
 *       long silent phases (CD load) cannot false-fire. */
volatile unsigned int saturn_master_beat  = 0;
volatile unsigned int saturn_master_site  = 0;
volatile unsigned int saturn_frame_open   = 0;
volatile int          saturn_freeze_watch = 0;
/* ROUND-56: the terminal crawl paints no verdict because the loop keeps
 * BEATING (no net-3 S row at 03/60) and frames keep completing < 120
 * vbl (no net-2 magenta) — a fast loop whose PICTURE barely moves.
 * These four split the between-frames region, which every r50-55 net
 * was blind to:
 *   saturn_iter_count  — ++ at D_DoomLoop loop top (census row H)
 *   saturn_tic_count   — ++ per game tic (census row T)
 *   saturn_sim_site    — last between-frames phase stamp (0x1 loop top,
 *       0x2/0x3 fill drain, 0x4/0x5 display cap, 0x6/0x7 tics, 0x8 after
 *       tics, 0x9 G_DoLoadLevel) — the yellow gap verdict prints it.
 *   saturn_frame_done_vbl — vbl count at the last frame-complete (the
 *       gap net's zero; 0 = no frame completed yet, net suppressed). */
volatile unsigned int saturn_iter_count      = 0;
volatile unsigned int saturn_tic_count       = 0;
volatile unsigned int saturn_sim_site        = 0;
volatile unsigned int saturn_frame_done_vbl  = 0;
/* ROUND-57: set by the display-cap wait in D_DoomLoop when ~3 vbl of
 * FRT pass with saturn_vbl_count unchanged — the vblank ISR is DEAD.
 * Everything downstream must then self-skip the vblank-dependent waits
 * (the flip wait) and the main thread paints the verdict itself. */
volatile unsigned int saturn_vbl_dead        = 0;
/* Round-51: per-iteration FRT-acc bail nets for the legs the walk's
 * check sites never cover (see saturn_segs.h for the contract).
 * saturn_frame_open is held 1 from the frame arm to I_FinishUpdate's
 * exit; the ISR's net-2 fires MAGENTA if a frame stays open > 120 vbl
 * even while the heartbeat continues (a stuck-but-beating loop — the
 * round-42 re-post poll — can never trip net 1). Round-50 defined the
 * flag but never SET it; the arm site now does. */
volatile unsigned int saturn_spr_frt_acc  = 0;
volatile unsigned int saturn_flu_frt_acc  = 0;
volatile unsigned int saturn_blit_frt_acc = 0;
volatile unsigned int saturn_pre_frt_acc  = 0;
unsigned int saturn_spr_frt_last  = 0;
unsigned int saturn_flu_frt_last  = 0;
unsigned int saturn_blit_frt_last = 0;
unsigned int saturn_pre_frt_last  = 0;
/* Round-52: the freeze is a CRAWL (S=41 build nodes then S=42 walk
 * pairs, frames stretching to seconds, heartbeat still beating). Two
 * live counters split the two candidate diseases, painted on the
 * verdict every vbl: saturn_load_count climbs iff the game is
 * RELOADING the level every frame (demo-restart storm — L climbing
 * with an identical picture); saturn_frame_count climbs iff frames
 * are being rendered at all. Round-51's record already proved the
 * walk acc stays ~2 vbl at a bail — these make the crawl's TIME
 * distribution visible live. */
volatile unsigned int saturn_load_count  = 0;
volatile unsigned int saturn_frame_count = 0;
/* Defined in saturn_segs.c: set when the slave's walk/fill hit the
 * self-bail deadline above (read by the red-screen verdict S row). */
extern volatile int   saturn_seg_bailed;
int saturn_dbg_red_events   = 0;
int saturn_dbg_red_stallvbl = 0;
int saturn_dbg_red_heart    = -1;
/* Round-39 stack-overflow probe: the slave's DEEPEST SP, sampled at
 * every heart publish (saturn_segs.c, per wall pair / fill column)
 * through the P2 mirror. If SGL's resident program runs the callback
 * on a small stack and the crash spot's walk is the deepest yet, the
 * wild writes clobbering master statics are the stack floor being
 * pushed through — the red screen's Q row prints it next to P (SP at
 * entry). 0xFFFFFFFF = no sample yet this frame. */
volatile unsigned int saturn_slave_sp_min = 0xFFFFFFFFu;
#define SATURN_HEART_MIRROR ((volatile unsigned int *)           \
    ((unsigned int)&saturn_heart_pub[0] + 0x20000000u))

/* Merged-run queue: merge_column(x) either extends pending runs whose
 * (sector, side, rows) sequence matches the current column's staged
 * spans exactly (extending every run by one column), or paints
 * everything pending row-major and starts fresh runs at this column. */
void fr_merge_column(int x, int cpu)
{
    int n = FR_COL_COUNT(cpu);
    int j;
    unsigned int tqm_;

    if (cpu == 0)
        tqm_ = SAT_FRT();

    if (n > 0 && FR_PENDING_COUNT(cpu) == n)
    {
        int match = 1;

        for (j = 0; j < n; j++)
            if (/* Round 27k (2026-09-03) ROOT CAUSE of the salmon bar:
                 * the extend path assumed columns arrive in increasing
                 * order â€” true for the old raycaster, FALSE for the seg
                 * renderer, which walks walls in BSP/painter's order. A
                 * seg covering [35,80] queues runs anchored at x0=35;
                 * when a nearer seg then walks [22,34] with identical
                 * (sec,rows) spans, every band-column merge took the
                 * EXTEND path and grew those right-anchored runs
                 * rightward â€” the band columns [23,35) were consumed
                 * into runs that paint [35,..) and the band stayed
                 * unpainted clear (fd_stage==0, cov==0, no pending
                 * stroke over the band: all confirmed 27i/27j, e0 read
                 * pending[0].x0 == 35 at col 28). Extend only when the
                 * pending runs are CONTIGUOUS with this column; any
                 * other case falls to the mismatch path below, which
                 * paints the old runs and queues fresh ones here. */
                FR_PENDING(cpu)[j].x1         != x
                || FR_PENDING(cpu)[j].sec       != FR_COL_SPANS(cpu)[j].sec
                || FR_PENDING(cpu)[j].y0     != FR_COL_SPANS(cpu)[j].y0
                || FR_PENDING(cpu)[j].y1     != FR_COL_SPANS(cpu)[j].y1
                || FR_PENDING(cpu)[j].isfloor != FR_COL_SPANS(cpu)[j].isfloor)
            {
                match = 0;
                break;
            }
        if (cpu == 0)
            frt_mbook_acc += (unsigned short)(SAT_FRT() - tqm_);
        if (match)
        {
            for (j = 0; j < n; j++)
                FR_PENDING(cpu)[j].x1++;
#ifdef SATURN_FLAT_DIAG
            /* BISECT round 15: chain flag â€” pending EXTENDED at this
             * column. Row 5 (top bar, above VIEW_TOP), full width. */
            if (cpu == 1)
                I_VideoBuffer[5 * SCREEN_W + x] = 0xFC;
            /* BISECT round 18: what does pending[0] look like at band
             * col 28? (x0, y0, y1 of run 0; run count.) Read one frame
             * stale by the HUD print â€” fine for pose structure. */
            if (cpu == 1 && x == 28)
            {
                extern volatile int saturn_dbg_e0x0, saturn_dbg_e0y0,
                                    saturn_dbg_e0y1, saturn_dbg_en;

                saturn_dbg_e0x0 = FR_PENDING(cpu)[0].x0;
                saturn_dbg_e0y0 = FR_PENDING(cpu)[0].y0;
                saturn_dbg_e0y1 = FR_PENDING(cpu)[0].y1;
                saturn_dbg_en   = FR_PENDING_COUNT(cpu);
            }
            /* BISECT round 20: same snapshot at col 33 â€” the last
             * extend column before the col-34 mismatch paints the
             * queue. Comparing E/F/G/H (col 28) with these shows
             * whether the run survived the band intact. */
            if (cpu == 1 && x == 33)
            {
                extern volatile int saturn_dbg_f0x0, saturn_dbg_f0y0,
                                    saturn_dbg_f0y1, saturn_dbg_fn;

                saturn_dbg_f0x0 = FR_PENDING(cpu)[0].x0;
                saturn_dbg_f0y0 = FR_PENDING(cpu)[0].y0;
                saturn_dbg_f0y1 = FR_PENDING(cpu)[0].y1;
                saturn_dbg_fn   = FR_PENDING_COUNT(cpu);
            }
#endif
            return;
        }
    }
#ifdef SATURN_FLAT_DIAG
    /* BISECT round 20: what does the mismatch path inherit at col 34
     * (the first mismatch after the band)? Snapshot BEFORE painting.
     * Round 27j: generalized â€” the mismatch may land at ANY column
     * after the band (fd_stage==0 says the extends chain unbroken
     * through [23,35)), so snapshot whenever the mismatch path is
     * about to paint a pending[0] that covers part of the band. 0 =
     * no band-covering run ever reached this paint loop this frame. */
    if (cpu == 1 && FR_PENDING_COUNT(cpu) > 0
        && FR_PENDING(cpu)[0].x0 <= 34 && FR_PENDING(cpu)[0].x1 > 23)
    {
        extern volatile int saturn_dbg_m0x0, saturn_dbg_m0x1,
                            saturn_dbg_m0y0, saturn_dbg_m0y1,
                            saturn_dbg_mn;

        saturn_dbg_m0x0 = FR_PENDING(cpu)[0].x0;
        saturn_dbg_m0x1 = FR_PENDING(cpu)[0].x1;
        saturn_dbg_m0y0 = FR_PENDING(cpu)[0].y0;
        saturn_dbg_m0y1 = FR_PENDING(cpu)[0].y1;
        saturn_dbg_mn   = FR_PENDING_COUNT(cpu);
    }
#endif
    if (cpu == 0)
    {
        unsigned int tvb_ = saturn_vbl_count;

        for (j = 0; j < FR_PENDING_COUNT(cpu); j++)
            paint_flat_run(&FR_PENDING(cpu)[j], cpu);
        saturn_frt_vmerge += (int)(saturn_vbl_count - tvb_);
    }
    else
    {
        for (j = 0; j < FR_PENDING_COUNT(cpu); j++)
            paint_flat_run(&FR_PENDING(cpu)[j], cpu);
    }
    FR_PENDING_COUNT(cpu) = 0;
#ifdef SATURN_FLAT_DIAG
    /* BISECT round 16: the MISMATCH path painted the pending runs at
     * this column â€” row 2 (top bar). */
    if (cpu == 1 && FR_PENDING_COUNT(cpu) > 0)
        I_VideoBuffer[2 * SCREEN_W + (x & (SCREEN_W - 1))] = 0xFC;
#endif
    for (j = 0; j < n; j++)
    {
        FR_PENDING(cpu)[j].x0      = x;
        FR_PENDING(cpu)[j].x1      = x + 1;
        FR_PENDING(cpu)[j].sec     = FR_COL_SPANS(cpu)[j].sec;
        FR_PENDING(cpu)[j].y0      = FR_COL_SPANS(cpu)[j].y0;
        FR_PENDING(cpu)[j].y1      = FR_COL_SPANS(cpu)[j].y1;
        FR_PENDING(cpu)[j].isfloor = FR_COL_SPANS(cpu)[j].isfloor;
    }
    FR_PENDING_COUNT(cpu) = n;
#ifdef SATURN_FLAT_DIAG
    /* BISECT round 15: chain flag â€” fresh runs QUEUED at this
     * column. Row 4 (top bar), full width. */
    if (cpu == 1)
        I_VideoBuffer[4 * SCREEN_W + x] = 0xFC;
#endif
    /* BISECT round 4: runs staged into pending at the hole columns. */
    if (cpu == 1 && x >= 23 && x < 35)
        saturn_fd_stage += n;
}

/* Paint and clear pending runs. (Column staging is NOT cleared here â€”
 * it survives until the next column overwrites it, like the sim's
 * merge machinery; redundant re-paints are value-identical.) */
void fr_flush_pending(int cpu)
{
    int j, n = FR_PENDING_COUNT(cpu);

#ifdef SATURN_FLAT_DIAG
    /* Round 27j: last exit for a band-covering pending run â€” record
     * pending[0] if a flush (walk end or safety valve) is about to
     * paint one. 0 = no flush ever saw a band-covering run. */
    if (cpu == 1 && n > 0
        && FR_PENDING(cpu)[0].x0 <= 34 && FR_PENDING(cpu)[0].x1 > 23)
    {
        saturn_dbg_fpx = (FR_PENDING(cpu)[0].x0 << 8)
                       | FR_PENDING(cpu)[0].x1;
        saturn_dbg_fpy = (FR_PENDING(cpu)[0].y0 << 8)
                       | FR_PENDING(cpu)[0].y1;
        saturn_dbg_fpn = n;
    }
#endif
    FR_PENDING_COUNT(cpu) = 0;
    if (cpu == 0)
    {
        unsigned int tvb_ = saturn_vbl_count;

        for (j = 0; j < n; j++)
        {
            SAT_FRT_NET(saturn_flu_frt_acc, saturn_flu_frt_last, 6);
            paint_flat_run(&FR_PENDING(cpu)[j], cpu);
        }
        saturn_frt_vmerge += (int)(saturn_vbl_count - tvb_);
    }
    else
    {
        for (j = 0; j < n; j++)
            paint_flat_run(&FR_PENDING(cpu)[j], cpu);
    }
}

/* Seg-renderer staging hooks (saturn_segs.c): cold-start this core's
 * run-merge state at frame entry, then merge+clear the staged spans
 * after each viswall's columns so later segs never append onto spans
 * that are already merged. */
void saturn_seg_reset_staging(int cpu)
{
    FR_COL_COUNT(cpu)     = 0;
    FR_PENDING_COUNT(cpu) = 0;
    if (cpu == 0)
    {
        /* Master-side per-frame reset (2026-09-03 photos #161/#162):
         * D row (vmerge) and the flat-paint census counters. The old
         * raycaster's reset lives in render_columns (dead in the seg
         * production build), so these accumulated forever. The slave
         * must NOT touch them — it would race the master's cache. */
        saturn_frt_vmerge = 0;
        saturn_cnt_fruns  = 0;
        saturn_cnt_frows  = 0;
        saturn_cnt_fpx    = 0;
        saturn_cnt_femit  = 0;
    }
    if (cpu == 1)
    {
        saturn_pr_n = 0;   /* BISECT round 20: painted-run recorder */
        saturn_pr_cov = 0; /* Round 27i: band-coverage fork probe */
        /* Round 27j: the pending-fate snapshots are per-frame â€” zero
         * them at frame entry so a 0 read means "never fired", not
         * "stale from a previous frame". */
        {
            extern volatile int saturn_dbg_e0x0, saturn_dbg_e0y0,
                                saturn_dbg_e0y1, saturn_dbg_en,
                                saturn_dbg_m0x0, saturn_dbg_m0x1,
                                saturn_dbg_m0y0, saturn_dbg_m0y1,
                                saturn_dbg_mn;

            saturn_dbg_e0x0 = 0; saturn_dbg_e0y0 = 0;
            saturn_dbg_e0y1 = 0; saturn_dbg_en   = 0;
            saturn_dbg_m0x0 = 0; saturn_dbg_m0x1 = 0;
            saturn_dbg_m0y0 = 0; saturn_dbg_m0y1 = 0;
            saturn_dbg_mn   = 0;
            saturn_dbg_fpx  = 0; saturn_dbg_fpy  = 0;
            saturn_dbg_fpn  = 0;
            saturn_pr_last_x = 0; saturn_pr_last_y = 0;
        }
    }
}

void saturn_seg_flush_column(int x, int cpu)
{
    if (FR_COL_COUNT(cpu) > 0)
        fr_merge_column(x, cpu);
    FR_COL_COUNT(cpu) = 0;
}

volatile unsigned int saturn_dbg_p0x = 0, saturn_dbg_p0y = 0,
                      saturn_dbg_pcnt = 0;
/* BISECT round 18: pending[0] geometry snapshot (extend path, col 28). */
volatile int saturn_dbg_e0x0 = 0, saturn_dbg_e0y0 = 0,
             saturn_dbg_e0y1 = 0, saturn_dbg_en = 0;
/* BISECT round 20: pending[0] at col 33 (last extend col before the
 * col-34 mismatch) and pending[0] as the mismatch path is about to
 * paint it at col 34. */
volatile int saturn_dbg_f0x0 = 0, saturn_dbg_f0y0 = 0,
             saturn_dbg_f0y1 = 0, saturn_dbg_fn = 0;
volatile int saturn_dbg_m0x0 = 0, saturn_dbg_m0x1 = 0,
             saturn_dbg_m0y0 = 0, saturn_dbg_m0y1 = 0,
             saturn_dbg_mn    = 0;

void saturn_seg_flush_pending(int cpu)
{
    /* BISECT round 4: pending runs left at the frame-end flush. */
    if (cpu == 1)
    {
        int k, my, n = FR_PENDING_COUNT(cpu);

        saturn_fd_flush += n;
        /* BISECT round 16: pre-flush pending geometry (read one frame
         * stale by the HUD print â€” the pose is static, fine). */
        saturn_dbg_pcnt = (unsigned int)n;
        if (n > 0)
        {
            saturn_dbg_p0x = ((unsigned int)FR_PENDING(cpu)[0].x0 << 16)
                           | (unsigned int)FR_PENDING(cpu)[0].x1;
            saturn_dbg_p0y = ((unsigned int)FR_PENDING(cpu)[0].y0 << 16)
                           | (unsigned int)FR_PENDING(cpu)[0].y1;
        }
#ifdef SATURN_FLAT_DIAG
        /* BISECT round 15: chain flag â€” walk-end pending count, drawn
         * as that many band columns lit on row 8; each pending run's
         * column coverage drawn black on row 9. */
        for (k = 0; k < n && k < 12; k++)
            I_VideoBuffer[8 * SCREEN_W + 23 + k] = 0xFC;
        for (k = 0; k < n; k++)
        {
            frun_t *pr = &FR_PENDING(cpu)[k];
            int    mx;

            for (mx = pr->x0; mx < pr->x1; mx++)
                if (mx >= 0 && mx < SCREEN_W)
                    I_VideoBuffer[9 * SCREEN_W + mx] = 0xB0;
        }
        /* BISECT round 20: pending queue drawn IN THE SCENE as
         * VERTICAL strokes â€” one at x0, one at x1-1, spanning the
         * run's rows [y0,y1) in bright magenta. Row-extent and
         * position in one read, no HUD-row calibration needed. */
        for (k = 0; k < n && k < 6; k++)
        {
            frun_t *pr = &FR_PENDING(cpu)[k];
            int    sx[2], s;

            sx[0] = pr->x0; sx[1] = pr->x1 - 1;
            for (s = 0; s < 2; s++)
                if (sx[s] >= 0 && sx[s] < SCREEN_W)
                    for (my = pr->y0; my < pr->y1; my++)
                        if (my >= 0 && my < SCREEN_H)
                            I_VideoBuffer[my * SCREEN_W + sx[s]] = 0xFB;
        }
        /* BISECT round 20: every recorded cpu1 painted run overlapping
         * [20,40) drawn as BLACK vertical strokes at x0/x1-1 over its
         * rows. Where a stroke pair stands, that run painted. */
        for (k = 0; k < saturn_pr_n; k++)
        {
            int sx[2], s;

            sx[0] = saturn_pr_x0[k]; sx[1] = saturn_pr_x1[k] - 1;
            for (s = 0; s < 2; s++)
                if (sx[s] >= 0 && sx[s] < SCREEN_W)
                    for (my = saturn_pr_y0[k]; my < saturn_pr_y1[k]; my++)
                        if (my >= 0 && my < SCREEN_H)
                            I_VideoBuffer[my * SCREEN_W + sx[s]] = 0x00;
        }
#endif
    }
    fr_flush_pending(cpu);
}

/* Wall texture column, rows [y0..y1).
 *
 * utex_raw: the along-line u in texels BEFORE the width wrap â€” the
 * caller computes it once per crossing (texoff + hit u scaled by the
 * line length, mirrored for the back side); upper/lower/mid spans of
 * the same crossing share it, each applying its own texture-width
 * wrap. (The old code repeated the FixedMul + modulo in every span.)
 *
 * v: linear in the row (walls are vertical planes): the world height
 * at row y is eye + (HALF_H-y)*t_u/PROJ, so v steps by t_u/PROJ
 * texels per row from the peg height (top-pegged: the sector ceiling;
 * lower walls peg to the top edge of the lower wall = the higher
 * floor). Wrap handles tall textures and negative pegs. Power-of-two
 * textures (all id WAD wall art) use masks instead of divides.
 * Shading: COLORMAP at the wall's distance + sector light. */
/* Exports for the seg renderer (saturn_segs.c). The raycaster keeps
 * these static-by-convention; the BSP/seg port drives them per column. */
#ifdef SATURN_SEG_DIFF
/* Diff probe (2026-09-02): the surviving artifact â€” the solid strip at
 * render cols 22-35 â€” flickers RED/BLUE between frames at a fixed pose:
 * the seg pass (slave half owns col 28) sometimes paints something
 * wrong, sometimes nothing. paint_wall_span records what each pass
 * feeds at the probe column; 99999 = this pass painted nothing. */
volatile int saturn_diff_probe_col = 104;   /* right wall, master half */
volatile int saturn_diff_ray_pass  = 0;
volatile int saturn_probe_seg_tex  = 99999;
volatile int saturn_probe_ray_tex  = 99999;
volatile int saturn_probe_seg_u    = 0;
volatile int saturn_probe_ray_u    = 0;
volatile int saturn_probe_seg_y    = 99999;   /* (y0<<8)|y1, 99999 = none */
volatile int saturn_probe_ray_y    = 99999;
volatile int saturn_probe_seg_stex = 99999;   /* span texnum at probe col */
volatile int saturn_probe_seg_tu   = 0;       /* depth/shade input */
volatile int saturn_probe_ray_tu   = 0;
volatile int saturn_probe_seg_peg  = 0;       /* peg_u fed to the painter */
volatile int saturn_probe_ray_peg  = 0;
volatile int saturn_probe_seg_floor = 99999;  /* floor band (y0<<8)|y1 */
volatile int saturn_probe_ray_floor = 99999;
/* FIRST-painted-span probes: both passes paint the same nearest wall
 * first, so first-span values are same-wall comparable (last-writer
 * probes aliased to different walls â€” see i_video.c probe comment). */
volatile int saturn_probe_seg_painted = 0;
volatile int saturn_probe_ray_painted = 0;
/* Build-time values from seg_store_wall_range for the wall covering
 * the probe col: B = t_peg_abs as early_prep computed it, E = its
 * texture. Compared against the paint-time seg probes (S/D) this
 * splits "early_prep computed a garbage peg" from "the peg is lost
 * between build and paint". */
volatile int saturn_probe_build_peg = 0;
volatile int saturn_probe_build_tex = 0;
/* Hole census (2026-09-02): the seg pass's FINAL clip state, saved by
 * saturn_segs_render_half (master half only â€” the slave's would need a
 * shared copy), plus the first column the diff found fully blue. A
 * blue column = the seg left the 0x20 clear in every row: nothing
 * painted AND the post-pass skipped (fl <= ce). The saved clip value
 * shows WHO closed the band; 99999 = no fully-blue column this frame. */
volatile unsigned short saturn_dbg_clip[SCREEN_W + 1];
volatile unsigned short saturn_dbg_lastsec[SCREEN_W + 1];
volatile int saturn_probe_hole_x    = 99999;
volatile int saturn_probe_hole_clip = 0;
volatile int saturn_probe_hole_sec  = 0;
#endif

/* d32xr (32X) MARS lighting (r_phase6.c:472-524, r_phase7.c:406-428).
 * A surface holds the RAW sector light (lightmax) out to ~167 map
 * units, fades to lightmin = seglight - (255-seglight-seglight/2)*2
 * by ~320 units, then HOLDS there â€” nothing like the vanilla
 * scaleLight ramp this port previously ran (which dropped the same
 * wall 5-7 COLORMAP rows darker at mid range and is why the scene
 * read as "far too dark" against the 32X look).
 * The continuous curve, folded out of d32xr's fixed-point
 *   light = FixedMul(scale, lightcoef) - lightsub
 *   scale = stretchX/d (stretchX = 219<<16 on the MARS 320x224 view),
 * is  L = (dl*350)/d - dl/4  clamped to [lightmin, lightmax]
 * (dl = lightmax - lightmin); verified equal to the r_phase6
 * simulation at d = 167/200/250/320.
 * Row index = (255-L)>>3 (HWLIGHT, r_local.h:48) is then remapped
 * onto the PC COLORMAP by mean luminance: decoding the 32X shading
 * table (content/files/colormap8_new + playpals) shows its rows run
 * ~30% brighter than the PC COLORMAP row of the same index, so the
 * identity mapping would re-darken everything. LUT from the decoded
 * 32X table vs cd\DOOM.WAD COLORMAP, 5-bit-display mean lum match.
 * (The LUT itself moved above paint_flat_run, round 8.) */

byte Saturn_LightRow(int lightlevel, int dist)
{
    /* d_main.c boot-probed SH-2 divide unit flag (HUD 'V' row) */
    extern volatile int saturn_divu_probe;
    int seglight, lightmin, dl, L, ri;

    if (lightlevel < 0)   lightlevel = 0;
    if (lightlevel > 255) lightlevel = 255;
    if (dist < 1) dist = 1;

    seglight = lightlevel;
    lightmin = seglight - (255 - seglight - seglight / 2) * 2;
    if (lightmin < 0)        lightmin = 0;
    if (lightmin > seglight) lightmin = seglight;

    dl = seglight - lightmin;
    if (dl == 0)
        L = seglight;
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 5)
    else if (saturn_divlut_on && dist <= 511)
    {
        /* OCRAM recip-LUT path (2026-09-08, the rig's V3ocr port): the
         * whale's divide without the DIVU turn-around. Exactness: the
         * dividend dl*350 <= 89250 and dist <= 511, so
         * (dl*350)*dist <= 45.6M < 2^32 — the bound sim_divlut.py
         * proved, upheld unconditionally at this site. dist == 1
         * divides by definition. */
        unsigned int q;

        if (dist > 1)
            q = Saturn_MulHi((unsigned int)(dl * 350), SATURN_OCLUT[dist]);
        else
            q = (unsigned int)(dl * 350);
        L = (int)q - (dl >> 2);
        sat_divlut_lhits += 1u;
    }
#endif
    else if (saturn_divu_probe)
    {
        /* 2026-09-03 photos #163/#164: F=71K flat px/frame at R=190 runs
         * and D~9-11 vbl — the flat paint runs ~64 cycles per PAINTED
         * pixel against a ~12-instruction stepping kernel, so per-ROW
         * overhead dominates. The whale was this plain `/` (d32xr
         * shift-subtract __sdivsi3, ~200+ cycles) executed once per row
         * of every run — and once per wall span (the paint_wall_span
         * call below). The SH-2 divide unit (gated on the boot probe,
         * same pattern as FixedDiv / seg_divsi) does the 32/32 signed
         * divide in ~30 cycles. Both cores have their own unit, so
         * master and slave both take this path. Operands are
         * non-negative (dl > 0 here, dist >= 1). */
        volatile int *const dvsr  = (volatile int *)0xFFFFFF00u;
        volatile int *const dvdnt = (volatile int *)0xFFFFFF04u;

#ifdef SATURN_DIVLUT
        if (saturn_divlut_on)
            sat_divlut_dfallback += 1u;  /* dist > 511: past the LUT */
#endif
        *dvsr  = dist;
        *dvdnt = dl * 350;                   /* start the divide */
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          ::: "memory");
        L = *dvdnt - (dl >> 2);
    }
    else
    {
#ifdef SATURN_DIVLUT
        if (saturn_divlut_on)
            sat_divlut_dfallback += 1u;
#endif
        L = (dl * 350) / dist - (dl >> 2);
    }
    if (L < lightmin)   L = lightmin;
    if (L > seglight)   L = seglight;

    ri = (255 - L) >> 3;
    if (ri > 31) ri = 31;
    return d32xr_row_lut[ri];
}

void paint_wall_span(int x, int y0, int y1,
                     int texnum, side_t *side, int nside,
                     int utex_raw, int peg_u, int t_u,
                     byte fallback_col, int cpu)
{
    byte *fb = I_VideoBuffer + x;
    stexinfo_t ti;
    byte *tex, *cmap;
    int   w, h;
    int   utex, v0, vstep, rowoff;
    int   vh;
    int   sh;
    int   y;

    if (y1 <= y0)
        return;

#ifdef SATURN_SEG_DIFF
    /* Only spans that actually paint rows record, and only the FIRST
     * one: both passes paint the same nearest wall first, so first-span
     * probes compare the same wall. Last-writer probes aliased to
     * whichever span happened to paint last (seg: a lower wall pegged
     * near z0; ray: an upper wall pegged to the ceiling) and read as
     * phantom peg/tex mismatches. */
    if (x == saturn_diff_probe_col)
    {
        if (saturn_diff_ray_pass)
        {
            if (!saturn_probe_ray_painted)
            {
                saturn_probe_ray_tex     = texnum;
                saturn_probe_ray_y       = (y0 << 8) | y1;
                saturn_probe_ray_u       = utex_raw;
                saturn_probe_ray_tu      = t_u;
                saturn_probe_ray_peg     = peg_u;
                saturn_probe_ray_painted = 1;
            }
        }
        else if (!saturn_probe_seg_painted)
        {
            saturn_probe_seg_stex    = texnum;
            saturn_probe_seg_y       = (y0 << 8) | y1;
            saturn_probe_seg_u       = utex_raw;
            saturn_probe_seg_tu      = t_u;
            saturn_probe_seg_peg     = peg_u;
            saturn_probe_seg_painted = 1;
        }
    }
#endif
    saturn_cnt_span++;
    {
        int why = 0;

        if (texnum <= 0)                  why = 1;
        else if (!Saturn_TexActive())     why = 2;
        else if (!(cpu == 1 ? Saturn_TexInfoNoLoad(texnum, &ti)
                            : Saturn_TexInfo(texnum, &ti)))
                                          why = 3;
        else if (!ti.data)                why = 4;
        else if (ti.w < 1 || ti.h < 1)    why = 5;

        if (why)
        {
            byte col = fallback_col;

#ifdef SATURN_SEG_SHOW
            /* Fallback-reason colors (2026-09-02): the red strips
             * SURVIVED the segwall preload, so make the fallback path
             * identify itself by color â€” green = texnum<=0, magenta =
             * cache miss (slave NoLoad / master load fail), cyan =
             * no composite data, yellow = bad texture dims. An
             * unchanged dark-red strip means the span is NOT a
             * fallback: a real texture drew it. */
            {
                static byte whycol[6];
                static int  why_inited = 0;

                if (!why_inited)
                {
                    byte *pal = (byte *)W_CacheLumpName("PLAYPAL",
                                                        PU_STATIC);
                    int i, bg = 0, bm = 0, bc = 0, by = 0;
                    int vbg = -1000, vbm = -1000, vbc = -1000, vby = -1000;

                    if (pal)
                    {
                        for (i = 0; i < 256; i++)
                        {
                            int r = pal[i * 3];
                            int g = pal[i * 3 + 1];
                            int b = pal[i * 3 + 2];
                            int v;

                            v = g * 2 - r - b;         /* green   */
                            if (v > vbg) { vbg = v; bg = i; }
                            v = r + b - g * 2;         /* magenta */
                            if (v > vbm) { vbm = v; bm = i; }
                            v = g + b - r * 2;         /* cyan    */
                            if (v > vbc) { vbc = v; bc = i; }
                            v = r + g - b * 2;         /* yellow  */
                            if (v > vby) { vby = v; by = i; }
                        }
                    }
                    whycol[1] = (byte)bg;
                    whycol[2] = (byte)bm;
                    whycol[3] = (byte)bm;
                    whycol[4] = (byte)bc;
                    whycol[5] = (byte)by;
                    why_inited = 1;
                }
                col = whycol[why];
            }
#endif
            /* 2026-09-03 round 6, the motion stripe: the slave paints
             * NOTHING on a miss (the why=3 NoLoad case is a 1-frame
             * window until the master composites; the fallback ramp
             * here was the pink-sliver source — see paint_flat_run).
             * The master keeps the colour: its misses are genuine
             * failures worth seeing. */
            if (cpu == 1)
                return;
            fb += y0 * SCREEN_W;
            for (y = y0; y < y1; y++)
            {
                *fb = col;
                fb += SCREEN_W;
            }
#ifdef SATURN_SEG_SHOW
            saturn_why_px[why] += (y1 - y0);
#endif
            return;
        }
    }
    GREY_TEX(&ti);
    tex = ti.data;
    w = ti.w;
    h = ti.h;
    vh = h << FRACBITS;

    /* u: the caller's raw offset, wrapped by this texture's width. */
    if (ti.pow2)
        utex = utex_raw & ti.wmask;
    else
    {
        utex = utex_raw % w;
        if (utex < 0)
            utex += w;
    }

    /* rowoffset: low byte in rowoffset, high nibble packed into
     * textureoffset bits 12-15 (see P_LoadSidedefs). */
    rowoff = side->rowoffset | (((side->textureoffset >> 12) & 0x0f) << 8);

    /* v: texel row at the span's top row. The world height at row y is
     * eye + (HALF_H-y)*t_u/PROJ, so v steps t_u/PROJ texels per row
     * from the peg (top-pegged: near ceiling / lower-wall top edge).
     * ((t_u<<8)/PROJ)<<8 keeps the intermediate inside 32 bits; the
     * modulo folds vstep below one texture wrap so the per-row step
     * needs a single conditional subtract. t_u >= 1 here (clamped at
     * the crossing), so both v0 and vstep land in [0, vh) and
     * v0 + vstep < 2*vh â€” the per-row wrap is one conditional
     * subtract, and row = v0>>16 never needs the multi-step wrap the
     * old code also ran per pixel. */
    {
        int htop = eye_u + ((HALF_H - y0) * t_u) / PROJ;
        v0 = (peg_u + rowoff - htop) << FRACBITS;
        if (ti.pow2)
            v0 &= vh - 1;
        else
        {
            v0 %= vh;
            if (v0 < 0)
                v0 += vh;
        }
        vstep = ((t_u << 8) / PROJ) << 8;
        if (ti.pow2)
            vstep &= vh - 1;
        else
            vstep %= vh;
    }

    /* Shade: d32xr (32X) MARS seg lighting (see Saturn_LightRow above).
     * The port's ancestor holds the raw sector light out to ~167 units
     * and fades to its lightmin by ~320; the vanilla scaleLight model
     * previously here put mid-range walls 5-7 COLORMAP rows darker and
     * the scene read far too dark. t_u = perpendicular map-unit depth,
     * the same distance input the old `j` used. */
    cmap = Saturn_Colormap();
    {
        sector_t *ws = &sectors[side->sector];
        sh = Saturn_LightRow(ws->lightlevel, t_u);
    }
#ifdef SATURN_SH_SPLIT
    /* Round 22 attribution: the LEFT half (slave, cols 0-79) paints
     * FULLBRIGHT â€” sh = 0 reads COLORMAP row 0, which the round-21
     * identity probe proved byte-exact. A clean left half against a
     * garbage right half convicts the shade lookup (sh values / cmap
     * rows 1-31); garbage in BOTH halves convicts the texel/asm side.
     * Diagnostic only â€” remove -DSATURN_SH_SPLIT for production. */
    if (x < SCREEN_W / 2)
        sh = 0;
#endif

    fb += y0 * SCREEN_W;
#if SATURN_PAINT_SOLID
    /* BISECT: keep the span setup (divides, wraps, shading) â€” skip the
     * per-pixel texel fetch + COLORMAP lookup + per-pixel wrap math. */
    {
        byte col = (byte)(sh * 8 + (utex & 7));

        for (y = y0; y < y1; y++)
        {
            *fb = col;
            fb += SCREEN_W;
        }
    }
    return;
#endif
#if !defined(SATURN_NO_ASM_DRAW) && !defined(SATURN_NO_ASM_WALL) \
    && !defined(SATURN_SEG_SHOW)
    /* Hand-scheduled SH-2 kernel (saturn_draw.s), bit-identical to the
     * C loop below. Pow2 textures only (row*w = row << wshift); NPO2
     * textures and the no-COLORMAP case fall through to C.
     * SEG_SHOW (2026-09-02 round 21): the C loop runs instead â€” it
     * paints the RAW TEXEL below, which the asm path can't express. */
    if (ti.pow2 && cmap)
    {
        /* arg8 is the texture WIDTH (not a shift count): SH-2 has no
         * dynamic shift (shad/shld are SH-3+), so the kernel multiplies
         * row*w via dmuls.l instead of row<<log2w. */
        saturn_wall_col_asm(fb, cmap + (sh << 8), tex, y1 - y0,
                            v0, vstep, vh, w, utex);
        return;
    }
#endif
    for (y = y0; y < y1; y++)
    {
        int  row = v0 >> FRACBITS;
        byte texel;
        texel = tex[row * w + utex];
#ifdef SATURN_SEG_SHOW
        /* Round 21: paint the RAW TEXEL â€” no COLORMAP. If a wrong
         * wall band survives raw, the texel fetch/composite is the
         * liar; if it turns into texture art, COLORMAP was. */
        *fb = texel;
#else
        *fb = cmap ? cmap[(sh << 8) + texel] : texel;
#endif
        fb += SCREEN_W;
        v0 += vstep;
        if (v0 >= vh) v0 -= vh;
    }
}

/* Round 10 (2026-09-04): vanilla masked mid textures (grates, fences).
 * paint_wall_span's contract is a SOLID wall face: vertically tiled
 * (wrap) and fully opaque — right for one-sided walls and doors, wrong
 * for a mid texture on a two-sided line: vanilla draws those ONCE from
 * the peg (R_DrawMaskedColumn — no vertical repeat, transparent posts
 * skipped) and leaves the opening open so the geometry behind shows
 * through. The seg walk defers masked grates to the open-band fill
 * (which runs after every nearer wall and the fill's own lastsec
 * paint) and calls this instead. Texel 0 = hole: our composites
 * zero-fill, so a post-absent texel reads 0 — a genuinely palette-0
 * texel is sacrificed to the mask, the same conflation the sprite
 * pass already lives with. A texture miss paints NOTHING (round-6
 * doctrine: a 1-frame hole holds the previous frame's content; a
 * colour ramp would bleed through the holes every sighting). */
/* Round 11 probe (2026-09-04): the FAR-VANTAGE masked-wall census.
 * The grate renders correct up close, but the far back wall (same
 * texture) reads bright / banded / hole-less on HW — and geometry
 * says its screen height is RIGHT, so vstep is right and t_u's
 * lighting/scale role can't both be broken. The remaining far-vs-near
 * variables are the values reaching THIS painter: t_u (drives both
 * the shade and how far v0 sits from the texture top), utex (could
 * land the whole wall in transparent columns -> grate paints
 * nothing -> the fill's flats show through), or an early return.
 * Per-core (no cross-core races): px = texels written, rows =
 * eligible rows (v0 in [0,vh)), min/max t_u seen this frame, why =
 * OR of early-return reasons, utex/v0tex = the LAST call's fetch
 * column and start v (texels, signed). HUD rows s/S/B/I/i/E while
 * the probe is live. */
volatile saturn_mm_dbg_t saturn_mm_dbg[2]
    __attribute__((aligned(16)));
/* Round 11c composite forensics: PC truth for tex 4/5/6 is 297/58/74
 * per-mille zeros. zcol/ztex = the LAST masked call's sampled column
 * and its zero count — if zfull matches the PC truth but the wall still
 * reads banded, the composite is fine and the fill/shade owns the bug;
 * if zfull is fat, the engine's composite is the corruption. */
volatile int saturn_mm_zfull[3] = { -1, -1, -1 };
volatile int saturn_mm_zcol = -1;
volatile int saturn_mm_ztex = -1;

void paint_wall_span_masked(int x, int y0, int y1, int texnum,
                            side_t *side, int nside, int utex_raw,
                            int peg_u, int t_u, int cpu)
{
    byte *fb = I_VideoBuffer + x;
    stexinfo_t ti;
    byte *tex, *cmap;
    int   w, h;
    int   utex, v0, vstep, rowoff;
    int   vh;
    int   sh;
    int   y;

    if (y1 <= y0)
        return;
    saturn_cnt_span++;
    {
        int why = 0;

        if (texnum <= 0)                  why = 1;
        else if (!Saturn_TexActive())     why = 2;
        else if (!(cpu == 1 ? Saturn_TexInfoNoLoad(texnum, &ti)
                            : Saturn_TexInfo(texnum, &ti)))
                                          why = 3;
        else if (!ti.data)                why = 4;
        else if (ti.w < 1 || ti.h < 1)    why = 5;

        if (why)
        {
            saturn_mm_dbg[cpu].why |= why;
            return;       /* holes hold the previous frame's content */
        }
    }
    GREY_TEX(&ti);
    tex = ti.data;
    w = ti.w;
    h = ti.h;
    vh = h << FRACBITS;

    /* u: the caller's raw offset, wrapped by this texture's width. */
    if (ti.pow2)
        utex = utex_raw & ti.wmask;
    else
    {
        utex = utex_raw % w;
        if (utex < 0)
            utex += w;
    }

    /* rowoffset: low byte in rowoffset, high nibble packed into
     * textureoffset bits 12-15 (see P_LoadSidedefs). */
    rowoff = side->rowoffset | (((side->textureoffset >> 12) & 0x0f) << 8);

    /* v: SIGNED and never wrapped — vanilla masked columns run once
     * from the peg: rows above the texture's top (v0 < 0) are
     * transparent, the walk stops at the texture's bottom (v0 >= vh).
     * Same v0/vstep formulas as paint_wall_span otherwise. */
    {
        int htop = eye_u + ((HALF_H - y0) * t_u) / PROJ;
        v0 = (peg_u + rowoff - htop) << FRACBITS;
        vstep = ((t_u << 8) / PROJ) << 8;
    }

    cmap = Saturn_Colormap();
    {
        sector_t *ws = &sectors[side->sector];
        sh = Saturn_LightRow(ws->lightlevel, t_u);
    }

    /* round 11b probe: stamp this call's feed values */
    saturn_mm_dbg[cpu].utex = utex;
    saturn_mm_dbg[cpu].v0   = v0 >> FRACBITS;
    saturn_mm_dbg[cpu].tex  = texnum;
    saturn_mm_dbg[cpu].sh   = sh;
    saturn_mm_dbg[cpu].light =
        sectors[side->sector].lightlevel;
    saturn_mm_dbg[cpu].calls++;
    saturn_mm_dbg[cpu].band  = y1 - y0;
    saturn_mm_dbg[cpu].vstep = vstep >> 8;   /* texels/row * 256 */
    if (t_u < saturn_mm_dbg[cpu].mintu) saturn_mm_dbg[cpu].mintu = t_u;
    if (t_u > saturn_mm_dbg[cpu].maxtu) saturn_mm_dbg[cpu].maxtu = t_u;

    /* round 11c composite forensics. zfull: the ENGINE composite's zero
     * per-mille for tex 4..6 (PC truth 297/58/74). zcol: zeros in THIS
     * call's sampled column — the exact skip rate the painter sees.
     * Dump columns 146/150: rows 0..31 of the sampled column, raw
     * palette (unshaded) — the photo shows the engine's composite slice
     * directly: X-grate art = composite fine; mostly black = broken. */
    if (texnum >= 4 && texnum <= 6)
    {
        if (saturn_mm_zfull[texnum - 4] < 0)
        {
            int n = w * h, k, z = 0;

            for (k = 0; k < n; k++)
                if (!tex[k]) z++;
            saturn_mm_zfull[texnum - 4] = (z * 1000) / n;
        }
        {
            int k, zc = 0;

            for (k = 0; k < h; k++)
                if (!tex[k * w + utex]) zc++;
            saturn_mm_zcol = zc;
            saturn_mm_ztex = texnum;
        }
#ifdef SATURN_PERF_ROWS
        /* round-11c on-screen composite slice (rows 0..31 at fb cols
         * 146/150). Round-31: this was still live in the clean build and
         * IS the "upper-right wall stripes" — two pose-dependent brown
         * stripes from fb row 0 down to the wall top (the dump ignores
         * every clip; visible only when the sampled column holds solid
         * texels, i.e. "looking at the texture with the transparency").
         * Diagnostic-only, behind the HUD flag like the other probes. */
        if (cpu == 0)
        {
            int dx = (texnum == 4) ? 146 : 150;
            int k;

            for (k = 0; k < 32; k++)
                I_VideoBuffer[k * SCREEN_W + dx] =
                    tex[k * w + utex];
        }
#endif
    }

    fb += y0 * SCREEN_W;
    for (y = y0; y < y1; y++)
    {
        if (v0 >= vh)
            break;                    /* ONCE: no vertical tiling */
        if (v0 >= 0)
        {
            int  row = v0 >> FRACBITS;
            byte texel = tex[row * w + utex];

            saturn_mm_dbg[cpu].rows++;
            if (texel)                /* 0 = masked hole: skip */
            {
                *fb = cmap[(sh << 8) + texel];
                saturn_mm_dbg[cpu].px++;
            }
            /* round-10h (2026-09-04): probes over — RESTORED the real
             * shaded lookup. Verdict trail: 10b put magenta exactly on
             * the speckle (this painter wrote it), 10d raw texels were
             * coherent art (fetch/v/u fine), 10f fixed row 8 was
             * coherent (cmap pointer + buffer fine), and the Y114
             * canary + cd\DOOM.WAD parse proved the WRAM-L colormap is
             * BYTE-EXACT to the source (row 0's 7 non-identity bytes —
             * 168/190/191/208/224/240/247 — are Ultimate-DOOM source
             * data mapping each index to a same-colour twin; visually
             * a no-op). With buffer, pointer, fetch and write all
             * proven, real sh is the last variable: coherent art here
             * closes the speckle (the t_um clamp fixed it); a returning
             * speckle convicts the index and the next build saturates
             * it with a HUD census. */
        }
        fb += SCREEN_W;
        v0 += vstep;
    }
}

/* Phase 4: fire the sweep event chains belonging to ONE column into
 * ONE core's active list â€” deactivations first, then activations
 * (order matters only for the coinciding-column case, where the line
 * stays active and the scan's window test rejects it â€” see the
 * validated sim). Extracted from the column loop so each core can
 * replay the columns before its x0 (below) purely in sweep terms. */
static void fire_sweep_column(int x, int cpu)
{
    int k = deact_head[x];
    while (k >= 0)
    {
        if (IN_ACTIVE(cpu)[k])
        {
            int pos = 0, kk;
            while (ACTIVE(cpu)[pos] != k) pos++;
            for (kk = pos; kk < ACTIVE_COUNT(cpu)-1; kk++)
                ACTIVE(cpu)[kk] = ACTIVE(cpu)[kk+1];
            ACTIVE_COUNT(cpu)--;
            IN_ACTIVE(cpu)[k] = 0;
        }
        k = deact_next[k];
    }
    k = act_head[x];
    while (k >= 0)
    {
        if (!IN_ACTIVE(cpu)[k])
        {
            /* insert keeping ACTIVE(cpu)[] ascending by line index
             * so the equal-t tie-break (lowest index wins) matches
             * the baseline scan exactly */
            int lo = 0, hi = ACTIVE_COUNT(cpu), pos, kk;
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;
                if (ACTIVE(cpu)[mid] < k) lo = mid + 1;
                else hi = mid;
            }
            for (kk = ACTIVE_COUNT(cpu); kk > lo; kk--)
                ACTIVE(cpu)[kk] = ACTIVE(cpu)[kk-1];
            ACTIVE(cpu)[lo] = k;
            ACTIVE_COUNT(cpu)++;
            IN_ACTIVE(cpu)[k] = 1;
        }
        k = act_next[k];
    }
}

/* The per-column ray walk, for columns [x0, x1). Runs on EITHER SH-2:
 * everything below reads only the master-built shared tables
 * (camlines, win_tan, the sweep buckets, col_*, rowinv, line_len) and
 * touches mutable state exclusively through the `cpu`-indexed arrays.
 * Each core cold-starts its OWN active list + run-merge state, then
 * replays the sweep buckets for the columns before its x0 without
 * painting them, so its list is in exactly the state the single-
 * threaded baseline had at its first column. All fr_ and painter
 * calls are threaded with `cpu` (never a shared variable â€” see the
 * Phase 4 comment above the fr_col_spans_v block). No zone
 * allocation, no WAD I/O, no SGL calls here â€” the slave's copy must
 * be able to run from the SGL slave bootstrap. */
static void render_columns(int x0, int x1, sector_t *player_sec, int cpu)
{
    int            x;
    unsigned int   frt_fire_acc = 0, frt_scan_acc = 0, frt_walk_acc = 0;
    unsigned int   frt_all_acc  = 0;
    unsigned int   frt_stage_acc = 0, frt_paint_acc = 0;
    unsigned int   frt_emit_acc = 0, frt_flush_acc = 0, frt_merge_acc = 0;
    unsigned int   t0, td, t_all;
    /* Sub-window stamp for one staging/paint call: time expr, add to
     * acc. Leaves t0 (the enclosing walk stamp) untouched. */
#define FRT_SUB(acc, expr) \
    do { unsigned int tq_ = SAT_FRT(); expr; \
         acc += (unsigned short)(SAT_FRT() - tq_); } while (0)

    memset(IN_ACTIVE(cpu), 0, numlines);
    ACTIVE_COUNT(cpu) = 0;
    FR_COL_COUNT(cpu) = 0;
    FR_PENDING_COUNT(cpu) = 0;

    /* round 11b: mm-probe reset moved to saturn_segs_render_half (this
     * function only runs in the SATURN_SEG_DIFF diagnostic path). */
    if (cpu == 0)
    {
        saturn_cnt_scan  = 0;
        saturn_cnt_solve = 0;
        saturn_cnt_cross = 0;
        saturn_cnt_span  = 0;
        saturn_cnt_fruns = 0;
        saturn_cnt_frows = 0;
        saturn_cnt_fpx = 0;
        frt_ploop_acc = 0;
        saturn_frt_fire = 0;
        saturn_frt_scan = 0;
        saturn_frt_walk = 0;
        saturn_frt_total = 0;
        saturn_frt_stage = 0;
        saturn_frt_paint = 0;
        saturn_frt_emit = 0;
        saturn_frt_flush = 0;
        saturn_frt_merge = 0;
    }
    t_all = SAT_FRT();
    for (x = 0; x < x0; x++)
        fire_sweep_column(x, cpu);   /* sweep-only: don't paint */

    for (x = x0; x < x1; x++)
    {
        fixed_t   cl_x   = col_clx[x];
        fixed_t   cl_z   = col_clz[x];
        int       ctan   = col_tan[x];
        fixed_t   t_start = 0;
        int       painted_top = VIEW_TOP;
        int       painted_bot = VIEW_BOT;
        sector_t *cur_sec = player_sec;
        int       covered = 0;
        int       pass;

        FR_COL_COUNT(cpu) = 0;  /* fresh staging per column */

        /* Fire this column's sweep events. */
        t0 = SAT_FRT();
        fire_sweep_column(x, cpu);
        frt_fire_acc += (unsigned short)(SAT_FRT() - t0);

        for (pass = 0; pass < MAX_CROSSINGS; pass++)
        {
            fixed_t best_t = 0x7fffffff;
            fixed_t best_u = 0;
            int     best_i = -1;
            int     best_d = 0, best_un = 0;   /* winner's fraction (cache) */
            line_t *line;
            side_t *side;
            sector_t *nsec, *fsec;
            int     nside, t_u, two_sided, utex_raw;
            int     yrec_on = 0, yrec = 0;
            int     y_n_ceil, y_n_floor, y_f_ceil, y_f_floor;
            int     i;

            /* Nearest crossing beyond t_start. The candidate source is
             * the sweep's active list (superset of every line the old
             * all-lines scan would reach, see the Phase 2 comment
             * above); the window test below is RETAINED so a stray
             * list member rejects exactly as it did in the baseline.
             * Candidates ascend by line index, preserving the equal-t
             * tie-break. The window test makes this two compares for
             * most lines; the straddle test and the solve only run for
             * genuine candidates. */
            t0 = SAT_FRT();
            {
                const int nact = ACTIVE_COUNT(cpu);
                int ni;

                if (pass == 0)
                {
                /* CANDIDATE CACHE (2026-09-01): a column's ray is
                 * fixed, so the straddle-passing candidate set â€” and
                 * each candidate's t â€” is identical on every crossing
                 * pass. The old loop re-ran the window test, the four
                 * straddle products, and the normalization for every
                 * candidate on EVERY pass (and paid FixedDiv for each
                 * running best). Build the cache once per column,
                 * insertion-sort it by t ascending (STABLE: equal t
                 * keeps active-list order, which is exactly the old
                 * first-encountered-minimum tie-break), then each pass
                 * advances a pointer past entries with t <= t_start.
                 * The entry the pointer stops on is the same one the
                 * old per-pass scan selected: minimum t, earliest in
                 * active order, same FixedDiv inputs â€” bit-identical
                 * crossings. The old per-pass skips (t <= t_start,
                 * t >= best_t, behind-camera, u out of range, den==0)
                 * all happen at build time or in the pointer advance,
                 * with the same values. */
                    int nc = 0;
                    int *ci = CAND_I(cpu), *ct = CAND_T(cpu);
                    int *cu = CAND_U(cpu), *cd = CAND_D(cpu);
                    for (ni = 0; ni < nact; ni++)
                    {
                        camline_t *cl;
                        int sdx, sdy, den, t_num, u_num;
                        int s1, s2;
                        fixed_t t;

                        i = ACTIVE(cpu)[ni];
                        cl = &camlines[i];

                        saturn_cnt_scan++;
                        if (ctan < win_tan0[i] - 4096
                            || ctan > win_tan1[i] + 4096)
                            continue;

                        s1 = cl->z1 * cl_x - cl->x1 * cl_z;
                        s2 = cl->z2 * cl_x - cl->x2 * cl_z;
                        if (!((s1 ^ s2) < 0))
                            continue;
                        saturn_cnt_solve++;

                        /* Solve  mu*D = A + u*S  in camera space (D =
                         * the raw projection-plane direction, A = line
                         * start, S = the segment):
                         *   mu = (A x S)/(D x S)
                         *   u = (cl_z*A.x - cl_x*A.z) / (D x S)
                         * FixedDiv returns mu*65536 as t. */
                        sdx = cl->x2 - cl->x1;
                        sdy = cl->z2 - cl->z1;
                        den = cl_x * sdy - cl_z * sdx;
                        if (den == 0)
                            continue;
                        t_num = cl->x1 * sdy - cl->z1 * sdx;
                        u_num = cl_z * cl->x1 - cl_x * cl->z1;
                        if (den < 0)
                        {
                            den = -den; t_num = -t_num; u_num = -u_num;
                        }
                        if (t_num < 0)                 /* behind camera */
                            continue;
                        if (u_num < 0 || u_num > den)  /* outside seg */
                            continue;

                        t = FixedDiv(t_num, den);

                        if (nc < CAND_CAP)
                        {
                            ci[nc] = i;
                            ct[nc] = t;
                            cu[nc] = u_num;
                            cd[nc] = den;
                            nc++;
                        }
                        /* Over-cap candidates drop. A ray paints at
                         * most MAX_CROSSINGS (16) crossings per column
                         * and E1M1's worst column holds ~40 straddlers
                         * â€” 256 is unreachable; the sim asserts
                         * nc <= CAND_CAP on every pose/column. */
                    }

                    /* Stable insertion sort by t (strict > keeps equal
                     * t in active order). */
                    {
                        int k;
                        for (k = 1; k < nc; k++)
                        {
                            int ki = ci[k], kt = ct[k];
                            int ku = cu[k], kd = cd[k];
                            int m = k - 1;
                            while (m >= 0 && ct[m] > kt)
                            {
                                ci[m + 1] = ci[m]; ct[m + 1] = ct[m];
                                cu[m + 1] = cu[m]; cd[m + 1] = cd[m];
                                m--;
                            }
                            ci[m + 1] = ki; ct[m + 1] = kt;
                            cu[m + 1] = ku; cd[m + 1] = kd;
                        }
                    }
                    CAND_N(cpu) = nc;
                    CAND_POS(cpu) = 0;
                }

                /* Pass selection: the first cache entry with
                 * t > t_start is the pass's minimum (sorted order);
                 * every earlier winner now has t == t_start, so the
                 * pointer skip reproduces the old t <= t_start reject
                 * â€” including equal-t lines losing to the first of
                 * their t group, and the old t >= best_t reject being
                 * unnecessary (the pointer stops at the true minimum). */
                {
                    int pos = CAND_POS(cpu);
                    const int nc = CAND_N(cpu);
                    while (pos < nc && CAND_T(cpu)[pos] <= t_start)
                        pos++;
                    CAND_POS(cpu) = pos;
                    if (pos < nc)
                    {
                        best_t  = CAND_T(cpu)[pos];
                        best_i  = CAND_I(cpu)[pos];
                        best_un = CAND_U(cpu)[pos];
                        best_d  = CAND_D(cpu)[pos];
                    }
                }
            }

            if (best_i < 0)
            {
                frt_scan_acc += (unsigned short)(SAT_FRT() - t0);
                break;          /* ray escaped: caller fills the rest */
            }
            /* The pass's u â€” one divide for the winner only. Same
             * FixedDiv(best_un, best_d) the old code computed for the
             * candidate that won. */
            best_u = FixedDiv(best_un, best_d);
            frt_scan_acc += (unsigned short)(SAT_FRT() - t0);

            t0 = SAT_FRT();
            line = &lines[best_i];
            saturn_cnt_cross++;

            /* Perpendicular distance in WHOLE map units (the fisheye
             * fix: ray distance * cos(column offset)). The camline
             * cache stores HALF-units (>>FRACBITS-1 of 16.16), so with
             * the RAW projection-plane direction (HALF_W-x, PROJ_X)
             * the solve's t = mu*65536 where mu = hit depth/PROJ_X:
             * FixedMul by PROJ_X (col_clz) recovers the hit depth in
             * HALF-units, and >>1 converts half- to whole map units.
             * Half-width: (cl_x, cl_z) both scaled by 1/2 keeps t and
             * t_u bit-identical to the 320-wide build. The old <<1 (a
             * LEFT shift where a right one belonged) made every
             * distance 4x too large â€” "everything very far away".
             * FixedMul's 16x16 partial products keep best_t*PROJ_X
             * inside 32 bits (best_t tops out near 24.6M). */
            t_u = (int)(FixedMul(best_t, cl_z) >> 1);
            if (t_u < 1)
                t_u = 1;
            if (t_u > 30000)
                t_u = 30000;

            /* Phase 3: ONE reciprocal per crossing replaces the 2-4
             * projection divides below (each ~250-cycle __sdivsi3 on
             * the SH-2, the single biggest remaining cost pool).
             *   c_div(n, t_u) -> (n * rec) >> 20,  rec = 2^20 / t_u
             * error <= 0.03 rows from rec truncation plus <= 1 row
             * when the arithmetic-shift floor differs from the
             * old divide's trunc-toward-zero (negative n only); the
             * sim showed 0 pixel diffs on all 6 validation poses at
             * the 1-row gate. Exact divide kept when t_u < 32 (recip
             * too coarse). The 64-bit intermediate keeps tall-sector
             * numerators (up to ~2000 units * PROJ * rec) safe. */
            {
                yrec_on = (t_u >= 32);
                if (yrec_on)
                    yrec = (1 << 20) / t_u;
            }

            /* Which side faces the camera -> the near sector whose
             * floor/ceiling bound this column. */
            nside = P_PointOnLineSide(cam.viewx, cam.viewy, line);
            if (line->sidenum[nside] < 0)
                nside = 1 - nside;
            if (line->sidenum[nside] < 0)
            {
                frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
                break;           /* no sidedef at all: give up */
            }
            side = &sides[line->sidenum[nside]];
            nsec = &sectors[side->sector];

            /* Along-line texture offset in texels, computed ONCE per
             * crossing â€” upper/lower/mid spans of this crossing all
             * share the same side/u/line length (they differ only in
             * texture width). Mirrored for the back side. */
            utex_raw = (side->textureoffset & 0xfff)
                     + (int)(FixedMul((nside == 0) ? best_u : 0x10000 - best_u,
                                       line_len[best_i] << FRACBITS) >> FRACBITS);

            two_sided = (line->sidenum[0] >= 0 && line->sidenum[1] >= 0
                         && !(line->flags & ML_BLOCKING));

            /* Screen rows where the near sector's surfaces project:
             *   y(z) = HALF_H - (z - eye_u) * PROJ / t_u
             * MULTIPLY BEFORE DIVIDING (the first cut divided first
             * and truncated to zero past 200 units). */
            y_n_ceil  = HALF_H - saturn_ydiv(((int)(nsec->ceilingheight >> FRACBITS) - eye_u) * PROJ, yrec_on, yrec, t_u);
            y_n_floor = HALF_H + saturn_ydiv((eye_u - (int)(nsec->floorheight >> FRACBITS)) * PROJ, yrec_on, yrec, t_u);

            if (!two_sided)
            {
                /* Solid wall: near ceiling flat above, midtexture,
                 * near floor flat below. PC maps put the texture on
                 * midtexture for solid lines; tolerate the odd map
                 * that only fills top or bottom. */
                int midtex = side->midtexture;
                int y0, y1;

                if (!midtex) midtex = side->toptexture;
                if (!midtex) midtex = side->bottomtexture;

                FRT_SUB(frt_emit_acc,
                        fr_emit_flat(x, painted_top,
                                     imin(imax(y_n_ceil, 0), painted_bot),
                                     nsec, 0, cpu));
                y0 = imax(imax(y_n_ceil, 0), painted_top);
                y1 = imin(imin(y_n_floor, SCREEN_H), painted_bot);
                /* DEFERRED (2026-09-01): no flush at the wall span. Wall pixels and flat pixels are disjoint (flats stage only the open band, walls paint inside the covered band), so paint order between them is irrelevant â€” and skipping the flush lets pending runs extend across columns instead of restarting at ~1 column wide (HUD: 3539 runs x 1.1 px = one kernel CALL per pixel, 58 of 74 vbl). Pending paints at the per-column merge or the frame-end flush. */
                FRT_SUB(frt_paint_acc,
                        paint_wall_span(x, y0, y1, midtex, side, nside,
                                        utex_raw,
                                        (int)(nsec->ceilingheight >> FRACBITS),
                                        t_u, COL_WALL_TOP + imin(t_u >> 6, 15),
                                        cpu));
                FRT_SUB(frt_emit_acc,
                        fr_emit_flat(x, imax(imin(y_n_floor, VIEW_BOT), painted_top),
                                     painted_bot, nsec, 1, cpu));
                covered = 1;
                frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
                break;
            }

            fsec = &sectors[sides[line->sidenum[1 - nside]].sector];
            y_f_ceil  = HALF_H - saturn_ydiv(((int)(fsec->ceilingheight >> FRACBITS) - eye_u) * PROJ, yrec_on, yrec, t_u);
            y_f_floor = HALF_H + saturn_ydiv((eye_u - (int)(fsec->floorheight >> FRACBITS)) * PROJ, yrec_on, yrec, t_u);

            /* --- ceiling half: flat up to the near ceiling, then the
             * upper wall down to the far ceiling (a door face, the
             * wall under a lowered ceiling). Texture 0 ("-") means no
             * wall: the span stays open. --- */
            FRT_SUB(frt_emit_acc,
                    fr_emit_flat(x, painted_top,
                                 imin(imax(y_n_ceil, 0), painted_bot),
                                 nsec, 0, cpu));
            if (fsec->ceilingheight < nsec->ceilingheight
                && side->toptexture != 0)
            {
                /* DEFERRED (2026-09-01): no flush at the wall span. Wall pixels and flat pixels are disjoint (flats stage only the open band, walls paint inside the covered band), so paint order between them is irrelevant â€” and skipping the flush lets pending runs extend across columns instead of restarting at ~1 column wide (HUD: 3539 runs x 1.1 px = one kernel CALL per pixel, 58 of 74 vbl). Pending paints at the per-column merge or the frame-end flush. */
                FRT_SUB(frt_paint_acc,
                        paint_wall_span(x,
                                        imax(imax(y_n_ceil, 0), painted_top),
                                        imin(imin(y_f_ceil, SCREEN_H), painted_bot),
                                        side->toptexture, side, nside,
                                        utex_raw,
                                        (int)(nsec->ceilingheight >> FRACBITS),
                                        t_u, COL_WALL_LOW + imin(t_u >> 6, 15),
                                        cpu));
                painted_top = imin(imax(y_f_ceil, painted_top), painted_bot);
            }
            else
            {
                painted_top = imin(imax(y_n_ceil, painted_top), painted_bot);
            }

            /* --- floor half: near floor flat, lower wall up the
             * raised far floor (steps, ledges). --- */
            FRT_SUB(frt_emit_acc,
                    fr_emit_flat(x, imax(imin(y_n_floor, SCREEN_H), painted_top),
                                 painted_bot, nsec, 1, cpu));
            if (fsec->floorheight > nsec->floorheight
                && side->bottomtexture != 0)
            {
                /* DEFERRED (2026-09-01): no flush at the wall span. Wall pixels and flat pixels are disjoint (flats stage only the open band, walls paint inside the covered band), so paint order between them is irrelevant â€” and skipping the flush lets pending runs extend across columns instead of restarting at ~1 column wide (HUD: 3539 runs x 1.1 px = one kernel CALL per pixel, 58 of 74 vbl). Pending paints at the per-column merge or the frame-end flush. */
                FRT_SUB(frt_paint_acc,
                        paint_wall_span(x,
                                        imax(imax(y_f_floor, 0), painted_top),
                                        imin(imin(y_n_floor, SCREEN_H), painted_bot),
                                        side->bottomtexture, side, nside,
                                        utex_raw,
                                        (int)(fsec->floorheight >> FRACBITS),
                                        t_u, COL_WALL_LOW + imin(t_u >> 6, 15),
                                        cpu));
                painted_bot = imax(imin(y_f_floor, painted_bot), painted_top);
            }
            else
            {
                painted_bot = imax(imin(y_n_floor, painted_bot), painted_top);
            }

            /* --- middle: a nonzero midtexture is a solid grate; an
             * opening that closed (upper met lower) ends the column;
             * otherwise the ray walks on into the far sector. --- */
            if (side->midtexture != 0)
            {
                /* DEFERRED (2026-09-01): no flush at the wall span. Wall pixels and flat pixels are disjoint (flats stage only the open band, walls paint inside the covered band), so paint order between them is irrelevant â€” and skipping the flush lets pending runs extend across columns instead of restarting at ~1 column wide (HUD: 3539 runs x 1.1 px = one kernel CALL per pixel, 58 of 74 vbl). Pending paints at the per-column merge or the frame-end flush. */
                FRT_SUB(frt_paint_acc,
                        paint_wall_span(x, painted_top, painted_bot,
                                        side->midtexture, side, nside,
                                        utex_raw,
                                        (int)(nsec->ceilingheight >> FRACBITS),
                                        t_u, COL_WALL_TOP + imin(t_u >> 6, 15),
                                        cpu));
                covered = 1;
                frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
                break;
            }
            if (painted_top >= painted_bot)
            {
                covered = 1;
                frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
                break;
            }
            cur_sec = fsec;
            t_start = best_t;
            frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
        }

        /* Whatever the walk left open belongs to the sector the ray
         * ended in (player sector if it never crossed anything). */
        t0 = SAT_FRT();
        if (!covered)
        {
            FRT_SUB(frt_emit_acc,
                    fr_emit_flat(x, painted_top, imin(painted_bot, HALF_H),
                                 cur_sec, 0, cpu));
            FRT_SUB(frt_emit_acc,
                    fr_emit_flat(x, imax(painted_top, HALF_H), painted_bot,
                                 cur_sec, 1, cpu));
        }

        FRT_SUB(frt_merge_acc, fr_merge_column(x, cpu));
        frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
        /* Whole-iteration window (T): column body + all sub-stamps.
         * One delta per iteration â€” always far below the TCNT wrap. */
        frt_all_acc += (unsigned short)(SAT_FRT() - t_all);
        t_all = SAT_FRT();
    }

    t0 = SAT_FRT();
    fr_flush_pending(cpu);         /* paint this core's remaining runs */
    frt_walk_acc += (unsigned short)(SAT_FRT() - t0);
    frt_all_acc += (unsigned short)(SAT_FRT() - t_all);
    if (cpu == 0)
    {
        /* >>8 to fit 16M cycles / 8 FRT ticks into the 5-digit HUD row
         * while keeping ~0.4% resolution. */
        saturn_frt_fire  = (int)(frt_fire_acc >> 8);
        saturn_frt_scan  = (int)(frt_scan_acc >> 8);
        saturn_frt_walk  = (int)(frt_walk_acc >> 8);
        saturn_frt_total = (int)(frt_all_acc >> 8);
        saturn_frt_stage = (int)(frt_stage_acc >> 8);
        saturn_frt_paint = (int)(frt_paint_acc >> 8);
        saturn_frt_emit   = (int)(frt_emit_acc >> 8);
        saturn_frt_flush  = (int)(frt_flush_acc >> 8);
        saturn_frt_merge  = (int)(frt_merge_acc >> 8);
        saturn_frt_mbook  = (int)(frt_mbook_acc >> 8);
        saturn_frt_ploop  = (int)(frt_ploop_acc >> 8);
    }
}

/*----- Phase 4: dual-SH-2 column split driver -----
 *
 * SGL's slave machinery (jo_engine/slave.c): jo_core_exec_on_slave()
 * = slSlaveFunc(bootstrap, cb), jo_core_wait_for_slave() spins on the
 * done flag through the UNCACHED 0x2xxxxxxx mirror then slCashPurge.
 * The callback type is argument-less, so the player sector rides a
 * shared variable that is written BEFORE dispatch (through the
 * master's purged OC) and only READ on the slave. */

static sector_t *render_cb_player_sec;

/* Per-frame render ticket. The master writes 1 BEFORE the dispatch
 * purge (so the write-back flush lands it in RAM); the slave consumes
 * it through the UNCACHED work-RAM-H mirror (+0x20000000, the same
 * trick jo_engine/slave.c uses for __jo_slave_done_flag â€” BSS byte
 * offset, not element count). SGL's slave program is free to re-enter
 * a registered function; without the ticket a stale re-execution would
 * repaint columns 0..159 with PREVIOUS-frame camera data while the
 * master is already building the next frame â€” left-half pixels from
 * the past mixed into the new view (a visible "warp" that isn't real
 * movement). */
static volatile int saturn_slave_ticket;

/* Round-38: retransmit budget for the CURRENT dispatch (reset each
 * frame at the dispatch site); 8 re-posts then the red screen rules. */
static int                   sat_dispatch_retries;

/* Phase-4 profiler extension (read by the i_video.c HUD):
 * setup  = vblanks spent before dispatch (preload walk + per-frame
 *          camera tables), join = dispatch..join (master half + slave
 *          wait), slave = the left half's own vblanks (uncached vbl
 *          counter reads, since the slave's OC may be stale there),
 * active = sum of both cores' active-line list lengths. */
volatile int saturn_prof_setup  = 0;
volatile int saturn_prof_join   = 0;
volatile int saturn_prof_slave  = 0;
volatile int saturn_prof_active = 0;

/* FRT-honest half timings (2026-09-02, >>8, 1 unit = 0.198 ms):
 *   saturn_frt_slavehalf = the SLAVE's own render_half(0,HALF_W) body
 *     (the slave's FRT â€” T is the MASTER half only; if the slave's
 *     FRT is dead under SGL this reads a stuck ~255 and the vbl-based
 *     saturn_prof_slave below is the fallback).
 *   saturn_frt_join = the master's jo_core_wait_for_slave block â€”
 *     slave excess over the master half + join overhead. This is the
 *     prime suspect for the ~8 ms of G_Display outside T+segs_build. */
volatile int saturn_frt_slavehalf = 0;
/* One-time slave-side FRT calibration: TCNT ticks across 2 vblanks,
 * measured on the slave (master reference: 21536/vbl => ~43072). */
volatile int saturn_slave_frt_cal = 0;
volatile int saturn_frt_join      = 0;
/* Master's own half, master-FRT >>8 â€” the trustworthy side of the
 * balance equation (the slave's FRT is unreliable under Kronos; its
 * body is judged in vblanks instead). */
volatile int saturn_frt_masterhalf = 0;
/* Adaptive column split (2026-09-03): the slave renders [0, split),
 * the master [split, SCREEN_W). Spawn-view capture showed the slave's half
 * at ~18.8 ms vs the master's ~10.9 ms for the same 80 columns â€” the
 * flat-heavy ground sits on the slave's side, so the join tail (j=7.9
 * ms) was workload imbalance, not SGL latency. The controller below
 * walks the split toward balance using j (the slave's excess over the
 * master's half, in master-FRT >>8 units) â€” no slave-clock arithmetic
 * anywhere. */
volatile int saturn_split_x = HALF_W;

/* 2026-09-03 photo #150: the pre window costs a real 3 vbl (s = 65,393
 * ticks) but the named legs (K camline, G segs_build, P preload) read
 * 0/garbled in the capture. prea = pre_f0 .. camline-block start
 * (camera pulls + one-time allocs); preb = camline-block end .. segb
 * block (colgeom one-time, col_wxs/sky, clears, eye/rowinv/colormap/
 * flatshades, dirty check). prea + K + preb + G + P + tail must sum
 * to s — whichever leg reads fat IS the 50 ms. */
volatile int saturn_frt_prea = 0;
volatile int saturn_frt_preb = 0;
static unsigned short preb_tf;

/* Slave-cache experiment: CCR (0xFFFFFF92, bit 0 = operand-cache
 * enable) as seen at slave-callback entry, after our enable attempt,
 * and on the master at dispatch. If the slave's OC is OFF, its column
 * loop runs fully uncached (~20x slower than the master's, which is
 * exactly the L>>master-half symptom). */
volatile int saturn_prof_sccr0 = -1;
volatile int saturn_prof_sccr1 = -1;
volatile int saturn_prof_mccr  = -1;
volatile int saturn_prof_pre   = 0;  /* prologue (entryâ†’preload), vblanks */

/* Real-HW render-window decomposition (2026-09-03, first console
 * capture): D read 174 ms while the master's own half was ~51 ms and
 * the join ~3 ms â€” ~120 ms of Saturn_R_RenderPlayerView is master-
 * serial work OUTSIDE the column loop (prologue + camline build +
 * segs_build + preload_textures before the dispatch, plus whatever
 * follows the join). These two windows name it. Both spans can exceed
 * the 16-bit FRT fold (65536 ticks = 50.7 ms), so each is rebuilt
 * with the vblank-count reconstruction (d_main.c's D pattern) and
 * printed >>8 like T/M (1 unit = 0.198 ms):
 *   saturn_frt_disp_pre  = function entry -> slave dispatch
 *   saturn_frt_disp_post = split controller -> dispatch-section exit
 * First-frame values also include the M1/M2 one-shot photo-holds. */
volatile int saturn_frt_disp_pre  = 0;
volatile int saturn_frt_disp_post = 0;

/* 2026-09-03 photos #152/#153: FRT deltas are phase garbage on real
 * HW (~55 ticks/vbl), so the join window is measured in VBLANKS now:
 * master-half return -> slave-done, 16.7 ms units. The old FRT join
 * (saturn_frt_join) read 16 and 1 on consecutive frames and drove the
 * split controller to the floor; this replaces it as the controller
 * AND the j row's input. */
volatile int saturn_frt_joinvbl   = 0;
static unsigned int master_tv_after;

/* FRT-honest span: the 16-bit FRT delta rebuilt against the
 * boot-calibrated ticks/vblank clock, returned in >>8 units
 * (21536 ticks/vbl => 0.198 ms/unit). Valid for spans up to ~2 hours
 * (the vbl diff is 32-bit). The rate is MEASURED on this machine at
 * boot (Saturn_FrtCalibrate) — the 21536 constant was calibrated on
 * Kronos only, and the 2026-09-03 capture's rows were self-
 * contradictory in exactly the way a wrong rate produces. */
extern volatile int saturn_frt_rate;

static int saturn_frt_span8(unsigned short f0, unsigned int v0,
                            unsigned short f1, unsigned int v1)
{
    unsigned int rate     = (unsigned int)saturn_frt_rate;
    unsigned int expected = (v1 - v0) * rate;
    unsigned int fd       = (unsigned int)(unsigned short)(f1 - f0);
    int          diff     = (int)expected - (int)fd;
    int          wraps    = (diff + 32768) >> 16;

    if (wraps < 0)
        wraps = 0;
    return (int)((fd + (unsigned int)wraps * 65536u) >> 8);
}

/* Per-frame work counters (the old L/X/Y/Z CCR rows are repurposed).
 * scan  = active-list line visits in the crossing scan
 * solve = lines that got past the window+straddle tests to the solve
 * cross = crossings painted (one per pass that found a wall)
 * span  = wall spans painted (upper/lower/mid each count)
 * Reset by render_columns on the master each frame; the HUD prints
 * them via i_video.c. */
int saturn_cnt_scan  = 0;
int saturn_cnt_solve = 0;
int saturn_cnt_cross = 0;
int saturn_cnt_span  = 0;
/* Flat-paint census (2026-09-01): paint_flat_run calls and total rows
 * painted per frame. 320 columns Ã— ~200 rows = ~64000 unique flat
 * pixels; rows far above that = the merge/flush machinery repaints
 * the same ground (overdraw through fr_flush_for_wall per crossing).
 * Reset with the other counters each frame. */
int saturn_cnt_fruns = 0;
int saturn_cnt_frows = 0;
/* Total flat PIXELS painted (rows x span width, summed over runs).
 * frows counts run HEIGHTS only; comparing fpx against frows decides
 * whether runs merge across columns (wide runs: fpx >> frows) or stay
 * 1-column (fpx ~= frows, and per-run/per-row overhead dominates). */
int saturn_cnt_fpx = 0;

/* FRT sub-windows, accumulated per frame in TCNT ticks (printed >>8):
 *  frt_fire = fire_sweep_column totals (sweep event chains)
 *  frt_scan = the active-list crossing scan per pass
 *  frt_walk = solve + span staging + painting per crossing
 * Sum + per-column overhead â‰ˆ J. */
int saturn_frt_fire = 0;
int saturn_frt_scan = 0;
int saturn_frt_walk = 0;
int saturn_frt_total = 0;
/* Walk-window decomposition (2026-09-01): the flat ASM kernel sim'd at
 * ~25 steps/px, so 70k flat rows should cost ~4 vbl â€” yet the walk
 * window is 61 of the 74-vblank frame. Stage = every fr_* staging call
 * (emit/flush/merge/pending), paint = every paint_wall_span call;
 * walk - stage - paint = per-crossing solve/setup glue. */
int saturn_frt_stage = 0;
int saturn_frt_paint = 0;
/* Second-level split of the staging total (2026-09-01): staging was
 * 96% of the walk window. emit/flush/merge/pending each get their own
 * accumulator to find the dominant fr_* call. */
int saturn_frt_emit   = 0;
int saturn_frt_flush  = 0;
int saturn_frt_merge  = 0;
int saturn_frt_ploop  = 0;  /* paint_flat_run's row/kernel loop only */

/* The seg renderer's stage publish (saturn_segs.c) owns the HUD stage
 * rows now: L = the flat KERNEL window (paint_flat_run + paint_sky_run
 * row loops, master-gated into frt_ploop_acc). The raycaster's
 * render_columns used to reset/publish this static but is dead code
 * under SATURN_SEG_RENDERER, so saturn_segs.c resets/publishes it
 * through these two accessors. */
unsigned int saturn_frt_ploop_kernel(void) { return frt_ploop_acc; }
void saturn_frt_ploop_reset(void)          { frt_ploop_acc = 0; }
int saturn_frt_pre  = 0;
int saturn_frt_cl   = 0;
/* Saturn seg-BSP build + texture preload window (>>8, 1 unit = 0.198
 * ms): the master-only saturn_segs_build + preload_textures pair that
 * runs between the camline build and the dual-core column split. This
 * plus frt_cl is the ~10 ms of G_Display the column-loop rows never
 * see (D 26.6 ms vs T 16.2 ms at the E1M1 benchmark, 2026-09-02). */
int saturn_frt_segb = 0;
/* 2026-09-03 photo #146 split: segs_build alone (w row) vs the
 * poll-accumulated whole pre window (s row). The window sits at the
 * 16-bit FRT fold, so single-delta reads mis-fold; the accumulator
 * sums sub-deltas each provably < one fold. pre_last stashes the tail
 * leg's start sample for the disp_pre site. */
int            saturn_frt_segb2    = 0;
int            saturn_frt_pre_acc  = 0;
unsigned short saturn_frt_pre_last = 0;
/* saturn_frt_tic is defined in d_main.c (owned by the main loop). */

extern volatile unsigned int saturn_vbl_count;

static unsigned int saturn_prof_p0;
static unsigned int saturn_prof_p1;

/* Round-33 red-screen verdict, drawn by the watchdog poll. Text goes
 * through Saturn_StageText (direct NBG0-page glyphs, index-0 on the
 * red fill) because the VDP1 boot font is invisible once NBG0 direct
 * display is live. Layout (2x2 glyphs, 20 px pitch, 3 lines):
 *   STALL            < 8 chars max per line at 160 fb cols
 *   V=nnnnn          vblanks since dispatch (climbs while stalled)
 *   H=hhhhhh         slave-heart delta, hex (see saturn_slave_heart:
 *                    0 = never painted; small N = wall pair N;
 *                    ~N*1000 = open-band fill column N) */
static void saturn_red_screen_report(unsigned int vbls, int liveh, int ent,
                                     int kind, unsigned int frt_acc)
{
    extern void Saturn_PaintNow(unsigned short);
    extern void Saturn_StageText(int, int, const char *);
    extern void Saturn_Udec(char *, unsigned int);
    extern void Saturn_Uhex(char *, unsigned int);
    /* Round-33d: NO Saturn_Format here — its varargs call is the thing
     * that garbles digits and destabilizes the target (08-30 MAP
     * screen, round-33c boot stamp). Hand-built strings only.
     * Round-34: E = slave-entry marker (0x5217 = the callback ran).
     * Round-36: 6 rows — S = the slave's phase code (pub[3]: A000
     * armed / A001 entered / A002 ticket / A003 purged+rendering /
     * A004 half done / DEAD self-bailed), P = the slave's SP at
     * entry (pub[2]). Every row is PADDED to 12 chars: the refresh
     * redraws over the old glyphs, and a shorter value used to leave
     * stale digits standing (the H=20000->"00000" aliasing).
     * Round-39: E=0 persisted through EIGHT re-posts — the slave is
     * not merely missing one dispatch, it stopped servicing entirely.
     * Two new rows interrogate SGL's resident slave program AT the
     * crash point (LIBCPK globals linked BY NAME — the 2026-09-01
     * probe hardcoded 0x06043940/44/48 and today's map has them at
     * 0x060482d0/44/48; the addresses move with the link layout.
     * Read UNCACHED — the caches don't snoop each other):
     *   T = (SlaveCommand << 16) | SlaveState
     *   R = SlaveParam (the posted callback pointer — a sane 0x060xxxxx
     *         value means the post LANDED)
     *   Q = the slave's deepest SP this frame (stack-overflow probe,
     *         saturn_slave_sp_min; compare with P = SP at entry).
     * Split: post lost (R sane but cb never entered) vs resident
     * program desynced (T shows a stuck state machine) vs slave CPU
     * dead (T frozen at an idle/last value, R = last cb pointer). */
    unsigned int sglcmd;
    unsigned int sglstate;
    unsigned int sglparam;
    unsigned int spmin;
    char l1[16], l2[16], l3[16], l4[16], l5[16], l6[16], l7[16], l8[16];
    int  n, k, g;
    int  fw;
    unsigned int fv;

    /* Round-41: the fence hunt — name the overflowing table. g != 0 is
     * shown in place of the R row (the record keeps the raw param). */
    g  = saturn_rtbl_fence_check();
    fw = (int)saturn_rtbl_trip_word;
    fv = (unsigned int)saturn_rtbl_trip_val;

    {
        extern volatile unsigned int SlaveCommand;
        extern volatile unsigned int SlaveState;
        extern volatile unsigned int SlaveParam;
        volatile unsigned int *sglp =
            (volatile unsigned int *)
            ((unsigned int)&SlaveCommand + 0x20000000u);

        sglcmd   = sglp[0];
        sglstate = sglp[1];
        sglparam = sglp[2];
    }

    {
        volatile unsigned int *spp =
            (volatile unsigned int *)
            ((unsigned int)&saturn_slave_sp_min + 0x20000000u);

        spmin = *spp;
    }
    l1[0] = 'S'; l1[1] = 'T'; l1[2] = 'A'; l1[3] = 'L'; l1[4] = 'L';
    if (kind > 0 && kind < 10)
    {
        /* Round-41: the STALL row self-identifies the bail kind
         * (2 master half, 3 build, 4 dead chain; 1 slave self-bail). */
        l1[5] = 'K';
        Saturn_Udec(l1 + 6, (unsigned int)kind);
    }
    else
        l1[5] = 0;
    l2[0] = 'V';
    l2[1] = '=';
    Saturn_Udec(l2 + 2, vbls);
    l3[0] = 'H';
    l3[1] = '=';
    Saturn_Uhex(l3 + 2, (unsigned int)liveh);
    l4[0] = 'E';
    l4[1] = '=';
    Saturn_Uhex(l4 + 2, (unsigned int)ent);
    l5[0] = 'S';
    l5[1] = '=';
    Saturn_Uhex(l5 + 2, SATURN_HEART_MIRROR[3]);
    /* Round-42: F = FRT ticks per stall-vbl (frt_acc / vbls). The two
     * dead-chain thresholds disagree by ~250x about the Kronos FRT
     * rate (0xC000 seg check vs 1292160 join accumulator); F measures
     * it live: ~85 = real-HW-like (kind 4 = a REAL ~10 s chain death),
     * ~21536 = the seg check fires after just 2.3 vbl (SPURIOUS). */
    l6[0] = 'F';
    l6[1] = '=';
    Saturn_Udec(l6 + 2, frt_acc / (vbls ? vbls : 1u));
    l7[0] = 'T';
    l7[1] = '=';
    Saturn_Uhex(l7 + 2, (sglcmd << 16) | (sglstate & 0xFFFFu));
    if (g > 0)
    {
        /* Round-41: G=<fence id> W=<word 0..3> — the table whose canary
         * tripped and which of its 4 words (the bad VALUE is in the
         * record, recw[28]; 8 visible chars per row cap). */
        l8[0] = 'G';
        l8[1] = '=';
        Saturn_Udec(l8 + 2, (unsigned int)g);
        n = 0;
        while (l8[n]) n++;
        l8[n] = 'W';
        l8[n + 1] = '=';
        l8[n + 2] = (char)('0' + fw);
        l8[n + 3] = 0;
    }
    else
    {
        l8[0] = 'R';
        l8[1] = '=';
        Saturn_Uhex(l8 + 2, sglparam);
    }
    n = 0;
    while (l2[n]) n++;
    for (k = n; k < 12; k++) l2[k] = ' ';
    l2[12] = 0;
    n = 0;
    while (l3[n]) n++;
    for (k = n; k < 12; k++) l3[k] = ' ';
    l3[12] = 0;
    n = 0;
    while (l4[n]) n++;
    for (k = n; k < 12; k++) l4[k] = ' ';
    l4[12] = 0;
    n = 0;
    while (l5[n]) n++;
    for (k = n; k < 12; k++) l5[k] = ' ';
    l5[12] = 0;
    n = 0;
    while (l6[n]) n++;
    for (k = n; k < 12; k++) l6[k] = ' ';
    l6[12] = 0;
    n = 0;
    while (l7[n]) n++;
    for (k = n; k < 12; k++) l7[k] = ' ';
    l7[12] = 0;
    n = 0;
    while (l8[n]) n++;
    for (k = n; k < 12; k++) l8[k] = ' ';
    l8[12] = 0;
    Saturn_PaintNow(0x001Fu);
    /* 20px pitch (round-39): 8 rows, 8+7*20+16 = 164 < 180. The old
     * 28px pitch (round-36b) could only carry 6 rows. Round-41: the
     * STALL row self-identifies the bail kind ("STALLK4"). */
    n = 0;
    while (l1[n]) n++;
    for (k = n; k < 12; k++) l1[k] = ' ';
    l1[12] = 0;
    Saturn_StageText(8, 8, l1);
    Saturn_StageText(8, 28, l2);
    Saturn_StageText(8, 48, l3);
    Saturn_StageText(8, 68, l4);
    Saturn_StageText(8, 88, l5);
    Saturn_StageText(8, 108, l6);
    Saturn_StageText(8, 128, l7);
    Saturn_StageText(8, 148, l8);
}

/* Seg renderer dispatch (2026-09-01): with SATURN_SEG_RENDERER both
 * cores run the BSP/seg wall painter (saturn_segs.c) instead of the
 * per-column raycaster â€” same column split, same staging discipline. */
#ifdef SATURN_SEG_RENDERER
void saturn_segs_render_half(int x0, int x1, int cpu);   /* saturn_segs.c */
#ifdef SATURN_SEG_DIFF
volatile int saturn_seg_diff_count = 0;   /* seg-vs-ray mismatches/frame */
#endif
static void render_half(int x0, int x1, sector_t *ps, int cpu)
{
    (void)ps;
    saturn_segs_render_half(x0, x1, cpu);
}
#else
#define render_half render_columns
#endif

static void saturn_render_slave_cb(void)
{
    /* Round-34: slave-entry marker — FIRST instruction of the job, so
     * the red-screen verdict can tell "dispatch never reached the
     * slave" (E=0) from "slave entered, then died" (E=5217). Written
     * through the uncached mirror only (cache-line rule, see
     * saturn_heart_pub). */
    SATURN_HEART_MIRROR[1] = 0x5217u;
    {
    volatile int *tick =
        (volatile int *)((unsigned int)&saturn_slave_ticket + 0x20000000);
    volatile unsigned int *vbl =
        (volatile unsigned int *)((unsigned int)&saturn_vbl_count + 0x20000000);
    unsigned int sv0, sv1;
    extern void slCashPurge(void);   /* SGL's own purge (SL_DEF.H) */

    /* Round-36 live instrumentation (readout S/P rows): pub[2] = the
     * slave's SP at entry (where SGL parked its stack), pub[3] = phase
     * code. A000 = armed, cb never ran; 5217 in E with A001 in S = the
     * cb ran but SGL re-entry was rejected; A002 = ticket accepted;
     * A003 = purged, rendering; A004 = half done; DEAD = self-bailed. */
    {
        unsigned int spv;

        __asm__ __volatile__ ("mov r15,%0" : "=r" (spv));
        SATURN_HEART_MIRROR[2] = spv;
    }
    SATURN_HEART_MIRROR[3] = 0xA001u;

    if (*tick != 1)
    {
        /* ROUND-59 THE FREEZE FIX: the reject path returned WITHOUT a
         * purge, and that no-purge loop is the ~9-min demo freeze. The
         * frozen state file shows both cores wedged in it: the slave
         * storming here (heart 5217/A001, PC at this entry, ticket 0 =
         * SGL re-running a completed job), the master inside SGL's
         * SlaveControl join. The storm sustains the deadlock BY ITSELF:
         * SGL's resident loop polls its command/State words CACHED, so
         * the stale "job posted" line never invalidates (it keeps
         * re-running the job) and this core's dirty "done" State lines
         * never reach RAM (the master's join never sees completion).
         * The accept path purges at entry (below) and at exit (tail) —
         * the reject path purged NEVER. Purging here write-backs the
         * handshake lines (master unblocks) and invalidates the stale
         * command line (storm ends on the next resident-loop poll). */
        slCashPurge();
        return;             /* stale SGL re-entry: nothing to do */
    }
    *tick = 0;
    SATURN_HEART_MIRROR[3] = 0xA002u;

    /* (The CCR read-probe that stood here is GONE, 2026-09-03: real HW
     * wedges on CCR writes, and the reads were never real-HW-proven â€”
     * the first frame froze before any marker with no red watchdog, so
     * even reads are off the table. saturn_prof_sccr0/1 stay at their
     * initialised values for the C row.) */

    /* The slave's OC may hold STALE lines for the tables the master
     * just rewrote (camlines, win_tan, sweep buckets, zone data) and
     * for its fb half from the previous frame â€” purge FIRST so every
     * read below sees the master's data.
     * ROUND-36 THE FIX: saturn_oc_purge is a NO-OP (any CCR write
     * wedges real HW), so NOTHING ever invalidated the slave's cache
     * â€” SGL's slCashPurge only ever ran on the MASTER. The slave has
     * rendered thousands of good frames because its stale lines get
     * evicted naturally ~always; at the demo's ~9-min crash spot a
     * stale line for a table the master just SHRANK (segwalls, clip
     * data) survives the whole half, the walk runs past the new table
     * end, and the slave is lost mid-frame (wild reads/writes, no done
     * flag -> red screen; the round-34/35 E-row garbage in pub[1] was
     * such a wild write). slCashPurge is SGL's OWN purge â€” the one
     * cache-control write proven safe on real silicon: the master has
     * run it per frame since the 09-03 join fix. On the slave it
     * write-backs its dirty lines (previous fb half) and invalidates
     * every stale line before the walk reads a single table byte. */
    slCashPurge();
    SATURN_HEART_MIRROR[3] = 0xA003u;
    sv0 = *vbl;
    /* Round-36 self-bail deadline: if the slave's half is still running
     * 90 vblanks in, the walk loops (the very hang this hunts) — bail
     * out of the half instead of stalling the master's join forever.
     * One glitchy half-frame beats a dead console.
     * Round-42: 90 -> 180 — the round-41 fire caught the slave ENTERED
     * and making real progress (heart 0x11666 = ~71 fill columns)
     * before the 90-vbl deadline killed it; if vblank starvation
     * doubles the half's wall time at the crash spot, 90 fires on a
     * healthy-but-slow half. 180 still bounds the join (the master's
     * own bail paints at 60 and re-posts forever now). */
    saturn_slave_bail_vbl = sv0 + 180u;
    {
        unsigned int tf = SAT_FRT();

        render_half(0, saturn_split_x + 1, render_cb_player_sec, 1);
        /* +1 overlap (2026-09-03 round 5): paint the split column on
         * BOTH cores. The walks are identical (same segwalls, same
         * clip math), so both write the same bytes — a boundary gap
         * gets filled and any write-timing overlap renders harmlessly.
         * This is the test for the motion-only 1px stripe: gone =
         * boundary confirmed; persists = boundary exonerated. */
        saturn_frt_slavehalf =
            (int)((unsigned short)(SAT_FRT() - tf) >> 8);
    }
    saturn_slave_bail_vbl = 0u;
    if (saturn_seg_bailed)
        SATURN_HEART_MIRROR[3] = 0xDEADu;   /* walk looped; half bailed */
    else
        SATURN_HEART_MIRROR[3] = 0xA004u;   /* half done cleanly */
    sv1 = *vbl;
    saturn_prof_slave = (int)(sv1 - sv0);

    /* One-time slave-FRT calibration: count this core's TCNT ticks
     * across exactly 2 vblanks. The master calibrated at 21536/vbl, so
     * a healthy reading here is ~43072 (both FRTs at reset-default
     * prescale); 21536/865xx/... = the slave's FRT runs at a different
     * clock and every >>8 unit from this core is scaled wrong. */
    if (!saturn_slave_frt_cal)
    {
        unsigned int f0 = SAT_FRT();
        unsigned int v0 = *vbl;

        while (*vbl == v0)
            ;
        /* Wait ONE more transition (the old `== v0+2` form returned
         * immediately: vbl was already v0+1, which != v0+2). Exactly
         * 2 vblanks now. */
        while (*vbl == v0 + 1)
            ;
        saturn_slave_frt_cal = (int)(unsigned short)(SAT_FRT() - f0);
    }

    /* Push this core's fb writes out of the slave's operand cache â€”
     * the framebuffer is write-back cached on BOTH cores. Round-36:
     * the no-op purge here becomes the REAL purge — the slave's dirty
     * fb/staging lines reach WRAM NOW, not on some eviction whim. */
    slCashPurge();
    }
}

extern void jo_core_exec_on_slave(void (*callback)(void));
extern void jo_core_wait_for_slave(void);
/* Round-60 direct-mailbox dispatch + degrade (saturn_slave.c). */
extern unsigned saturn_slave_direct_ok(void);
extern unsigned saturn_slave_direct_post(void *cb);
extern volatile unsigned int saturn_slave_master_only;

/*----- public entry -----*/

void Saturn_R_RenderPlayerView(int displayplayer)
{
    sector_t      *player_sec;
    int            floor_u, ceil_u;
    int            i, x;
    unsigned short pre_f0;
    unsigned int   pre_v0;

    (void)displayplayer;

    if (!I_VideoBuffer) return;
    if (numnodes <= 0 || numlines <= 0) return;

    /* PRE window start (see saturn_frt_disp_pre): everything this function
     * does before the slave dispatch â€” camera tables, camline build,
     * segs_build, preload_textures. Reconstructed at the dispatch. */
    pre_f0 = SAT_FRT();
    pre_v0 = saturn_vbl_count;

#ifdef SATURN_SEG_SHOW
    /* DIAGNOSTIC (2026-09-02): dark-red clear â€” every pixel still dark
     * red at display time was NEVER painted this frame (a hole), vs
     * black (a real shade-31 paint) or any fallback colour. Pairs with
     * the white sky-fallback fill and the wall fallback whycol codes. */
    memset(I_VideoBuffer, 0x20, SCREEN_W * DOOM_FBHEIGHT);
    /* Round-20 counters: reset before any painter of this frame runs. */
    saturn_why_px[0] = saturn_why_px[1] = saturn_why_px[2] = 0;
    saturn_why_px[3] = saturn_why_px[4] = saturn_why_px[5] = 0;
    saturn_sky_fb_px = saturn_flat_fb_px = 0;
    saturn_sky_null = saturn_sky_bad = saturn_sky_runs = 0;
#endif

    /* Profile: the prologue (camera tables + camline build) sits BELOW
     * the preload stamp at p0 â€” stamp the window start separately so
     * no part of the render window is untimed. saturn_prof_p0 already
     * holds this frame's start; the p0 stamp below turns it into the
     * prologue duration P. */
    saturn_prof_p0 = saturn_vbl_count;

    /* Pull the player's position from the d32xr players[0] struct. */
    /* Eye height in whole map units (VIEWHEIGHT is 41 units above the
     * floor plus bob) â€” every projection below is measured against
     * it. */
    eye_u = 0;

    /* Pull the player's position from the d32xr players[0] struct. */
    cam.viewx = players[0].mo->x;
    cam.viewy = players[0].mo->y;
    cam.viewz = players[0].viewz;
    cam.viewangle = players[0].mo->angle;

    player_sec = (players[0].mo->subsector)
               ? SSEC_SECTOR(players[0].mo->subsector)
               : &sectors[0];

    /* Build this frame's camera-space line cache (see camline_t),
     * plus per-line lengths and screen windows. One-time zone
     * allocation; PU_STATIC so level reloads that
     * Z_FreeTags(PU_LEVEL) can't pull it out from under us. */
    if (camlines_cap < numlines)
    {
        camlines  = (camline_t *)rtbl_z(numlines * sizeof(camline_t));
        line_len  = (int *)rtbl_z(numlines * sizeof(int));
        win_tan0  = (int *)rtbl_z(numlines * sizeof(int));
        win_tan1  = (int *)rtbl_z(numlines * sizeof(int));
        act_head   = (int *)rtbl_z(SCREEN_W * sizeof(int));
        deact_head = (int *)rtbl_z(SCREEN_W * sizeof(int));
        act_next   = (int *)rtbl_z(numlines * sizeof(int));
        deact_next = (int *)rtbl_z(numlines * sizeof(int));
        active_v[0] = (int *)rtbl_z(2 * numlines * sizeof(int));
        active_v[1] = active_v[0] + numlines;
        in_active_v[0] = (signed char *)rtbl_z(2 * numlines);
        in_active_v[1] = in_active_v[0] + numlines;
        camlines_cap = numlines;
    }
    if (!col_clx)
    {
        col_clx = (fixed_t *)rtbl_z(SCREEN_W * sizeof(fixed_t));
        col_clz = (fixed_t *)rtbl_z(SCREEN_W * sizeof(fixed_t));
        col_wxs = (fixed_t *)rtbl_z(SCREEN_W * sizeof(fixed_t));
        col_wys = (fixed_t *)rtbl_z(SCREEN_W * sizeof(fixed_t));
        col_tan = (int *)rtbl_z(SCREEN_W * sizeof(int));
        col_bam = (int *)rtbl_z(SCREEN_W * sizeof(int));
        col_sky = (byte *)rtbl_z(SCREEN_W);
        fr_col_spans_v[0] = fr_col_spans_v[1] = (fspan_t *)rtbl_z(
            2 * FLATSPAN_CAP * sizeof(fspan_t));
        fr_col_spans_v[1] += FLATSPAN_CAP;
        fr_pending_v[0] = fr_pending_v[1] = (frun_t *)rtbl_z(
            2 * FLATSPAN_CAP * sizeof(frun_t));
        fr_pending_v[1] += FLATSPAN_CAP;
        texcols_v[0] = texcols_v[1] = (short *)rtbl_z(
            2 * SCREEN_W * sizeof(short));
        texcols_v[1] += SCREEN_W;
        cand_i_v[0] = (int *)Z_Malloc(2 * CAND_CAP * sizeof(int), PU_STATIC);
        cand_i_v[1] = cand_i_v[0] + CAND_CAP;
        cand_t_v[0] = (int *)Z_Malloc(2 * CAND_CAP * sizeof(int), PU_STATIC);
        cand_t_v[1] = cand_t_v[0] + CAND_CAP;
        cand_un_v[0] = (int *)Z_Malloc(2 * CAND_CAP * sizeof(int), PU_STATIC);
        cand_un_v[1] = cand_un_v[0] + CAND_CAP;
        cand_den_v[0] = (int *)Z_Malloc(2 * CAND_CAP * sizeof(int), PU_STATIC);
        cand_den_v[1] = cand_den_v[0] + CAND_CAP;
        rowinv = (int *)rtbl_z((SCREEN_H + 1) * sizeof(int));
        floorshade = (byte *)Z_Malloc(2 * SCREEN_H, PU_STATIC);
        ceilshade = floorshade + SCREEN_H;
        fr_col_count_v[0] = fr_col_count_v[1] = 0;
        fr_pending_count_v[0] = fr_pending_count_v[1] = 0;
    }
    if (!line_len_built)
    {
        /* World length in whole map units, for the wall u mapping.
         * P_LoadVertexes stores RAW int16 map units (no FRACBITS
         * shift), so the raw differences are already unit-scaled.
         * Camera-independent â€” computed once per level (see
         * line_len_built / Saturn_RenderInvalidateLevel from
         * P_SetupLevel). */
        for (i = 0; i < numlines; i++)
        {
            line_t *ld = &lines[i];

            if (ld->v1 < 0 || ld->v2 < 0
                || ld->v1 >= numvertexes || ld->v2 >= numvertexes)
            {
                line_len[i] = 0;
                continue;
            }
            line_len[i] = P_AproxDistance(vertexes[ld->v2].x - vertexes[ld->v1].x,
                                           vertexes[ld->v2].y - vertexes[ld->v1].y);
        }
        line_len_built = 1;
    }
    {
        fixed_t dsin = finesine  (cam.viewangle >> ANGLETOFINESHIFT);
        /* prea close: pre_f0 .. here (camera pulls + one-time allocs) */
        saturn_frt_prea =
            (int)((unsigned short)(SAT_FRT() - pre_f0) >> 8);
        fixed_t dcos = finecosine(cam.viewangle >> ANGLETOFINESHIFT);
        unsigned int frt_cl_acc = 0;   /* per-iteration FRT, >>8 at store */
        /* Rotate the camera position with the same transform as the
         * endpoints, then subtract per endpoint so the camera really
         * is at the origin (rotate-then-subtract == subtract-then-
         * rotate). The first cut omitted this and the camera sat at
         * world (0,0), outside E1M1 in the void. */
        fixed_t cam_r = FixedMul(dcos, cam.viewy) - FixedMul(dsin, cam.viewx);
        fixed_t cam_f = FixedMul(dcos, cam.viewx) + FixedMul(dsin, cam.viewy);

        for (i = 0; i < numlines; i++)
        {
            line_t *ld = &lines[i];
            unsigned int tf = SAT_FRT();   /* camline-build FRT window */

            if (ld->v1 < 0 || ld->v2 < 0
                || ld->v1 >= numvertexes || ld->v2 >= numvertexes)
            {
                /* Degenerate entry: never straddles. */
                camlines[i].x1 = camlines[i].z1 = 0;
                camlines[i].x2 = camlines[i].z2 = 0;
                win_tan0[i] = 1; win_tan1[i] = 0;
                frt_cl_acc += (unsigned short)(SAT_FRT() - tf);
                continue;
            }

            /* World length (u mapping) is per-level â€” see the
             * line_len_built block above the allocation section. */
            {
                fixed_t rx = (fixed_t)vertexes[ld->v1].x << FRACBITS;
                fixed_t ry = (fixed_t)vertexes[ld->v1].y << FRACBITS;
                fixed_t r  = FixedMul(dcos, ry) - FixedMul(dsin, rx);
                fixed_t f  = FixedMul(dcos, rx) + FixedMul(dsin, ry);
                camlines[i].x1 = (r - cam_r) >> (FRACBITS - 1);
                camlines[i].z1 = (f - cam_f) >> (FRACBITS - 1);

                rx = (fixed_t)vertexes[ld->v2].x << FRACBITS;
                ry = (fixed_t)vertexes[ld->v2].y << FRACBITS;
                r  = FixedMul(dcos, ry) - FixedMul(dsin, rx);
                f  = FixedMul(dcos, rx) + FixedMul(dsin, ry);
                camlines[i].x2 = (r - cam_r) >> (FRACBITS - 1);
                camlines[i].z2 = (f - cam_f) >> (FRACBITS - 1);
            }

            /* Screen window: near-clip endpoints to z >= NEAR_Z and
             * compare the endpoints' tan against each column's. Both
             * endpoints behind the near plane = empty window. */
            {
                int xa = camlines[i].x1, za = camlines[i].z1;
                int xb = camlines[i].x2, zb = camlines[i].z2;
                int ta, tb;

                if (za < NEAR_Z)
                {
                    if (zb < NEAR_Z)
                    {
                        win_tan0[i] = 1; win_tan1[i] = 0;
                        continue;
                    }
                    xa += (xb - xa) * (NEAR_Z - za) / (zb - za);
                    za = NEAR_Z;
                }
                else if (zb < NEAR_Z)
                {
                    xb += (xa - xb) * (NEAR_Z - zb) / (za - zb);
                    zb = NEAR_Z;
                }
                ta = (xa << 16) / za;
                tb = (xb << 16) / zb;
                if (ta > tb) { int tmp = ta; ta = tb; tb = tmp; }
                /* Screen-range capping: the screen's columns span
                 * col_tan -65127..+65536. A line whose whole window
                 * lies beyond that range (Â± the reject test's 4096
                 * slack) can never be crossed by any column's ray â€”
                 * mark it degenerate so every column skips it BEFORE
                 * the window reads. Along a segment with both endpoints
                 * z >= NEAR_Z, z is linear so the crossing tan stays
                 * inside the endpoint tans, making the window
                 * conservative. */
                if (tb < -73728 || ta > 73728)
                {
                    win_tan0[i] = 1; win_tan1[i] = 0;
                    continue;
                }
                win_tan0[i] = ta;
                win_tan1[i] = tb;
            }
            frt_cl_acc += (unsigned short)(SAT_FRT() - tf);
        }
        saturn_frt_cl = (int)(frt_cl_acc >> 8);
        preb_tf = SAT_FRT();   /* preb open: camline end .. segb block */
    }

    /* Static per-column geometry (camera-independent). Vanilla's
     * pinhole model: the ray through screen column x passes through
     * the point (HALF_W-x, PROJ_X) on a virtual plane PROJ_X units
     * ahead of the eye, so the RAW direction is (HALF_W-x, PROJ_X) and
     * its tan is -(x-HALF_W)/PROJ_X. NOT a linear angle sweep: the old
     * (x-HALF_W)*ANG90/160 construction mapped the screen onto a
     * 180-DEGREE panorama (edge rays at 89.4 degrees instead of
     * atan(159/160) = 44.8), which shrank everything at screen center
     * 2x ("everything very far away") and fired the edge columns
     * nearly SIDEWAYS, hitting the side walls at near-zero
     * perpendicular distance ("walls warp toward the camera"). The
     * lateral axis is LEFT (see the camline build), hence the negated
     * x offset. col_bam is atan of the tan as a BAM â€” only the sky
     * needs it; found by binary-searching the fine tables once.
     * Half-width: (HALF_W-x, PROJ_X) = (80-x, 80) is the old even-
     * column direction (160-2x, 160) scaled by 1/2 â€” same tan. */
    if (!colgeom_built)
    {
        for (x = 0; x < SCREEN_W; x++)
        {
            int adx = x - HALF_W;
            int aadx = adx < 0 ? -adx : adx;
            int lo = 0, hi = FINEANGLES / 4, ix;

            col_clx[x] = HALF_W - x;        /* LEFT-lateral: right col < 0 */
            col_clz[x] = PROJ_X;
            col_tan[x] = (int)(((int64_t)(HALF_W - x) << 16) / PROJ_X);

            /* smallest fine index whose tan reaches |x-HALF_W|/PROJ_X */
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;

                if ((int64_t)finesine(mid) * PROJ_X
                    >= (int64_t)finecosine(mid) * aadx)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            ix = lo;
            col_bam[x] = (adx >= 0) ? (ix << ANGLETOFINESHIFT)
                                    : -(ix << ANGLETOFINESHIFT);
        }
        colgeom_built = 1;
    }

    /* Per-frame column data. The flats need the ray in WORLD axes: at
     * perpendicular depth d the hit sits at view + d*forward +
     * (tan*d)*lateral-left, so the per-column world step is
     * fhat + tan*lhat â€” its length runs up to ~1.42 at the screen
     * edges, carrying the 1/cos correction the old unit-vector step
     * dropped (which sampled floor texels up to 29% too close to the
     * camera at the edges). The sky column is (viewangle - col_bam)
     * >> 22: a full turn is 1024 texels, so the 256-wide sky pans
     * exactly once across the 90-degree FOV (PrBoom/ManagedDoom
     * shift). */
    {
        fixed_t vsin = finesine  (cam.viewangle >> ANGLETOFINESHIFT);
        fixed_t vcos = finecosine(cam.viewangle >> ANGLETOFINESHIFT);

        for (x = 0; x < SCREEN_W; x++)
        {
            int ctan = col_tan[x];

            col_wxs[x] = vcos - FixedMul(vsin, ctan);
            col_wys[x] = vsin + FixedMul(vcos, ctan);
            col_sky[x] = (byte)(((cam.viewangle
                                  - (unsigned)col_bam[x]) >> 22) & 0xff);
        }
    }

    /* Clear the screen with a mid-tone so missing regions don't read
     * as pure black. (2026-09-06: the fb is the full 224-line window.) */
    memset(I_VideoBuffer, 0x20, SCREEN_W * DOOM_FBHEIGHT);
    /* Letterbox: the bar strip 192..223 reads as frame border until the
     * HUD paints it - black, not the mid-tone scene clear. */
    memset(I_VideoBuffer, 0, VIEW_TOP * SCREEN_W);
    memset(I_VideoBuffer + VIEW_BOT * SCREEN_W, 0,
           (DOOM_FBHEIGHT - VIEW_BOT) * SCREEN_W);

    /* Tan-event sweep build (per frame, division-free; col_tan above
     * is already built). Resolve each line's activation/deactivation
     * column by BINARY SEARCH against the strictly monotone col_tan
     * table (col_tan[x] ~ (HALF_W-x)*65536/PROJ) â€” the boundary is
     * unique, so the search is bit-identical to the old walk, and
     * costs <=9 probes per line. xa/xb == SCREEN_W means the event
     * never fires (windows that stay outside the screen's tan range
     * even after the 4096 slack; the capping pass above already
     * empties most of them but cannot be exact at the edges). Bucket
     * insertion order is irrelevant: the scan iterates the
     * index-ordered active[] list. */
#if !defined(SATURN_SEG_RENDERER) || defined(SATURN_SEG_DIFF)
    {
        for (i = 0; i < SCREEN_W; i++)
        {
            act_head[i] = -1;
            deact_head[i] = -1;
        }
        for (i = 0; i < numlines; i++)
        {
            int ta_hi = win_tan1[i] + 4096;
            int ta_lo = win_tan0[i] - 4096;
            int xa, xb, lo, hi;

            /* col_tan is affine in x (strictly monotone), so the old
             * estimate-then-adjust walks converge to the UNIQUE first
             * index where col_tan[x] <= ta_hi (resp. < ta_lo) â€” a
             * binary search lands on the same index bit-identically,
             * in <=9 probes instead of up to 320 walk steps per line. */
            lo = 0;
            hi = SCREEN_W;
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;
                if (col_tan[mid] > ta_hi) lo = mid + 1; else hi = mid;
            }
            xa = lo;

            lo = 0;
            hi = SCREEN_W;
            while (lo < hi)
            {
                int mid = (lo + hi) >> 1;
                if (col_tan[mid] >= ta_lo) lo = mid + 1; else hi = mid;
            }
            xb = lo;

            if (xa < SCREEN_W) { act_next[i] = act_head[xa]; act_head[xa] = i; }
            if (xb < SCREEN_W) { deact_next[i] = deact_head[xb]; deact_head[xb] = i; }
        }
        /* Each core cold-starts its OWN active list at dispatch (see
         * render_columns) â€” no shared active[] state to reset here. */
    }
#endif  /* !SATURN_SEG_RENDERER || SATURN_SEG_DIFF */

    eye_u   = (int)(cam.viewz >> FRACBITS);
    viewx_u = (int)(cam.viewx >> FRACBITS);
    viewy_u = (int)(cam.viewy >> FRACBITS);
    floor_u = (int)(players[0].mo->floorz   >> FRACBITS);
    ceil_u  = (int)(players[0].mo->ceilingz >> FRACBITS);

    if (!rowinv_built)
    {
        for (i = 1; i <= SCREEN_H; i++)
            rowinv[i] = 65536 / i;
        rowinv[0] = 65536;
        rowinv_built = 1;
    }
    {
        byte *cm = Saturn_Colormap();
        cmap_shades = cm ? 34 : 1;
    }
    if (skytexnum < 0)
        skytexnum = Saturn_SkyTexNum();

    /* The flat-shade ramps (400 software divides per frame) exist ONLY
     * for the fallback paths: no-columnmap sky, no-flat columns, and
     * the colour-ramp wall fallback. With the real WAD all three are
     * unreachable, so skip the rebuild â€” but always fill them once so
     * a late-detecting fallback never reads zeros. */
    if (!flatshades_built
        || !Saturn_Colormap()
        || !Saturn_FlatActive()
        || skytexnum < 0)
    {
        build_flat_shades(eye_u, floor_u, ceil_u);
        flatshades_built = 1;
    }

#if SATURN_RENDER_DIAG
    int ndeg  = 0;
    int nhits = 0;
#endif

    /* Phase 4: preload every texture and flat the level's lines can
     * reach BEFORE the slave is ever fired. The lazy composite used to
     * trigger only from a crossing's own fetch â€” the master crosses
     * what its RIGHT half sees, so a texture visible only on the slave
     * half could hold a fallback colour nearly forever (the pink
     * patches that "load after a while as you move closer"). This walk
     * is a bounds check per line once everything is cached; the first
     * frame that first-sees textures pays the composites up front,
     * on the master, outside the parallel render window. The slave's
     * NoLoad fetches then always hit the cache. */
    {
        extern volatile int saturn_prof_pre;

        /* The entry stamp above measured the prologue (camera tables +
         * camline build); record it as P and restart the clock for the
         * preload walk. */
        saturn_prof_pre = (int)(saturn_vbl_count - saturn_prof_p0);
        saturn_prof_p0  = saturn_vbl_count;
    }
    /* New pixels composited since the last pass (first sight of a new
     * room's textures) re-arm the preload walk, so the FOLLOWING frame
     * absorbs the warm-up off the render path instead of every future
     * first-sight composite firing inside the column loop. */
    {
        extern int saturn_tex_dirty;   /* saturn_tex.c */

        if (saturn_tex_dirty)
        {
            preload_warm    = 0;
            saturn_tex_dirty = 0;
        }
    }
    if (!preload_warm)
    {
        unsigned int frt_pre_acc = 0;
        int          comp0;
        extern int   saturn_tex_flat_miss;
        extern int   saturn_tex_comp_count;

        comp0 = saturn_tex_comp_count;
        /* M row becomes per-frame this frame: a persisting nonzero
         * value means the flat tier is STILL missing every frame. */
        saturn_tex_flat_miss = 0;
        for (i = 0; i < numlines; i++)
        {
            line_t *ld = &lines[i];
            int     s;
            unsigned int tf = SAT_FRT();    /* preload FRT window */

            for (s = 0; s < 2; s++)
            {
                side_t   *sd;
                sector_t *ssec;

                if (ld->sidenum[s] < 0 || ld->sidenum[s] >= numsides)
                    continue;
                sd = &sides[ld->sidenum[s]];
                if (sd->midtexture)
                    (void)Saturn_TexData(sd->midtexture);
                if (sd->toptexture)
                    (void)Saturn_TexData(sd->toptexture);
                if (sd->bottomtexture)
                    (void)Saturn_TexData(sd->bottomtexture);
                ssec = &sectors[sd->sector];
                (void)Saturn_FlatData(ssec->floorpic);
                (void)Saturn_FlatData(ssec->ceilingpic);
            }
            frt_pre_acc += (unsigned short)(SAT_FRT() - tf);
        }
        if (skytexnum >= 0)
            (void)Saturn_TexData(skytexnum);
        saturn_frt_pre = (int)(frt_pre_acc >> 8);
        /* Clean pass (nothing composited, no flat missed): everything is
         * cached â€” skip the ~1900-lookup walk until new pixels appear. */
        if (saturn_tex_comp_count == comp0 && saturn_tex_flat_miss == 0)
            preload_warm = 1;
    }
    else
    {
        /* Warm: no walk ran this frame — zero the row so H reads the
         * truth (0) instead of the last cold pass's stale cost. */
        saturn_frt_pre = 0;
    }

        /* Phase 4 dual-SH-2 column split: every per-frame table above was
     * written through the master's write-back operand cache, so purge
     * the OC BEFORE the slave touches anything. The slave renders
     * columns [0, HALF_W) while the master walks [HALF_W, SCREEN_W) â€”
     * disjoint 160-byte row segments (whole 16-byte cache lines), no
     * false sharing. The join's slCashPurge refreshes the master's own
     * OC so I_FinishUpdate sees the slave's pixels.
     *
     * BISECT (SATURN_NO_SLAVE=1): single-core render, no exec/join â€”
     * separates "the slave is slow" from "the master is slow". The L/J
     * rows read ~0 in this mode. */
#ifdef SATURN_SEG_DIFF
    /* Diff probe: sentinel reset each frame BEFORE the seg build so the
     * wall-emission probe (written inside saturn_segs_build) survives â€”
     * the reset used to run after the build and wiped it every frame
     * (X stuck at 99999 was self-inflicted, not a culling bug). */
    saturn_probe_seg_tex  = 99999;
    saturn_probe_seg_u    = 0;
    saturn_probe_seg_y    = 99999;
    saturn_probe_seg_stex = 99999;
    saturn_probe_seg_tu   = 0;
    saturn_probe_seg_peg  = 0;
    saturn_probe_seg_floor = 0;   /* accumulated painted floor ROWS now */
    saturn_probe_seg_painted = 0; /* first-span probe: rearm */
    saturn_probe_build_peg  = 0;  /* build-time peg probe: rearm */
    saturn_probe_build_tex  = 0;
#endif
#ifdef SATURN_SEG_RENDERER
    /* BSP traversal + viswall build on the master, BEFORE the purge:
     * the slave only reads the finished segwall array. FRT-timed
     * (saturn_frt_segb) â€” vbl-based probes read 0 here and this window
     * is a prime 20-fps suspect (master-only serial work the dual-core
     * column split cannot hide). */
    {
        /* 2026-09-03: timed SEPARATELY (segb2 = segs_build, segb =
         * preload_textures) and the whole pre window is
         * poll-accumulated from 16-bit sub-deltas (pre_acc) — the
         * window sits at the 16-bit FRT fold, so single-delta reads
         * mis-fold (photo #144/#146) and vbl-based probes read 0. */
        unsigned short tf, f1;
        unsigned int   acc;

        /* preb close: camline-block end .. here (clears + col tables +
         * eye/rowinv/colormap/flatshades + dirty check) */
        saturn_frt_preb =
            (int)((unsigned short)(SAT_FRT() - preb_tf) >> 8);
        tf  = SAT_FRT();
        acc = (unsigned int)(unsigned short)(tf - pre_f0);

        /* Round-40: arm the BUILD watchdog (deadline vbl+30; normal
         * build <1 vbl) + the dead-chain detector's clocks. */
        saturn_bail_arm_vbl    = saturn_vbl_count;
        saturn_bail_arm_frt    = SAT_FRT();
        SAT_PUBLISH(saturn_bail_arm_vbl);   /* round-45: slave reads */
        SAT_PUBLISH(saturn_bail_arm_frt);
        saturn_build_bail_vbl  = saturn_bail_arm_vbl + 30u;
        saturn_frt_bail_reset();   /* round-49: FRT-acc bails re-zeroed */
        saturn_master_site = 1;    /* round-50: frame starts here */
        saturn_freeze_watch = 1;
        saturn_frame_open = 1;     /* round-51: net-2 clock starts */
        saturn_frame_count++;      /* round-52: frames rendered */
        saturn_rtbl_fence_arm();   /* round-41: fresh canaries */
        saturn_segs_build(player_sec);
        saturn_master_site = 2;    /* build returned */
        f1 = SAT_FRT();
        saturn_frt_segb2 = (int)((unsigned short)(f1 - tf) >> 8);
        acc += (unsigned int)(unsigned short)(f1 - tf);

        tf = f1;
        saturn_segs_preload_textures();   /* red-strip fix: stage the
                                           * frame's wall textures before
                                           * the slave's NoLoad fetches
                                           * run */
        saturn_master_site = 3;    /* preload returned */
        f1 = SAT_FRT();
        saturn_frt_segb = (int)((unsigned short)(f1 - tf) >> 8);
        acc += (unsigned int)(unsigned short)(f1 - tf);

        saturn_frt_pre_acc  = (int)acc;
        saturn_frt_pre_last = f1;
    }
#endif
#if SATURN_NO_SLAVE
    render_cb_player_sec = player_sec;
    {
        extern volatile int saturn_prof_setup;

        saturn_prof_p1    = saturn_vbl_count;
        saturn_prof_setup = (int)(saturn_prof_p1 - saturn_prof_p0);
    }
    saturn_oc_purge();
    render_half(0, SCREEN_W, player_sec, 0);   /* bisect over: full width */
#else
    render_cb_player_sec = player_sec;
    /* Arm the ticket through the UNCACHED mirror: the old comment said
     * "before the purge flushes it", but the purge is a real-HW no-op
     * (CCR writes wedge the bus) — a cached write here could sit dirty
     * in the master's OC for many frames while the slave's uncached
     * read kept seeing 0 (slave skips its half, left side stale). */
    {
        volatile int *tickp =
            (volatile int *)((unsigned int)&saturn_slave_ticket
                             + 0x20000000u);

        *tickp = 1;
    }
    /* BISECT round 5: reset the fd probes HERE (before the dispatch) â€”
     * the old reset inside reset_staging(0) ran AFTER the slave had
     * already started incrementing, wiping its early counts (the hole
     * wall is walked early in the slave's BSP order). */
    saturn_fd_emit  = 0;
    saturn_fd_paint = 0;
    saturn_fd_bad   = 0;
    saturn_fd_stage = 0;
    saturn_fd_flush = 0;
    saturn_fd28_cnt = 0;
    saturn_fd60_cnt = 0;
    {
        extern volatile int saturn_prof_setup;

        saturn_prof_p1    = saturn_vbl_count;    /* dispatch point */
        saturn_prof_setup = (int)(saturn_prof_p1 - saturn_prof_p0);
    }
    saturn_oc_purge();
    /* FIRST-FRAME DISPATCH MARKERS (2026-09-03 real-HW bring-up): the
     * console froze on static dark cyan (no red watchdog) = a master-
     * side first-frame hang. Three one-shot paints bracket the dispatch:
     *   M1 (24,24,0) dark yellow  before the slave launch
     *   M2 (0,24,0)  green        master's own half starting
     *   M3 (0,0,24)  dark blue    master's half done, joining
     * A freeze on one of these (IN-LOOP flash already played) names the
     * window; no marker + red = slave-side instead. All CCR accesses
     * removed: this machine wedges on CCR WRITES and the reads were
     * never real-HW-proven â€” nothing functional needs the register. */
    {
        static int m1 = 0;

        if (!m1)
        {
            extern void Saturn_StageColor(unsigned short color);

            Saturn_StageColor(0x8318u);   /* RGB555 (24,24,0) */
            m1 = 1;
        }
    }
    /* PRE window close: entry -> dispatch, rebuilt against the vblank
     * clock (the camline build + segs_build + preload_textures span
     * can exceed the 16-bit FRT fold on real hardware). */
    {
        /* Tail leg of the poll-accumulated pre window (ticket arm +
         * probe resets + purge) — all trivial, < one fold. */
        unsigned short f2 = SAT_FRT();

        saturn_frt_pre_acc =
            (int)((unsigned int)saturn_frt_pre_acc +
                  (unsigned int)(unsigned short)
                      (f2 - saturn_frt_pre_last));
    }
    saturn_frt_disp_pre = saturn_frt_span8(pre_f0, pre_v0,
                                      SAT_FRT(), saturn_vbl_count);
    /* Round-34: per-frame heart protocol — reset BOTH mirror words and
     * the slave's pair counter at dispatch, THEN baseline. All through
     * the uncached mirror (cache-line rule, saturn_heart_pub). Before
     * this, saturn_heart_pairs accumulated since boot and the baseline
     * could be a stale OC write-back, making the H decode meaningless. */
    /* Round-48: shadow the PREVIOUS job's exit state before wiping it
     * — when the next dispatch draws E=0/S=A000 (the slave never
     * entered), these say where the slave actually was: A004 = its
     * last job FINISHED (wedge is in SGL's between-jobs loop, the
     * stale-command-line theory), A003 = it never finished the last
     * half (wedged mid-walk), DEAD = bailed and never came back. */
    saturn_prev_pub0 = SATURN_HEART_MIRROR[0];
    saturn_prev_pub2 = SATURN_HEART_MIRROR[2];
    saturn_prev_pub3 = SATURN_HEART_MIRROR[3];
    SATURN_HEART_MIRROR[0] = 0u;
    SATURN_HEART_MIRROR[1] = 0u;
    SATURN_HEART_MIRROR[2] = 0u;   /* P row: SP at entry */
    SATURN_HEART_MIRROR[3] = 0xA000u;   /* S row: armed, cb not yet run */
    saturn_heart_pairs     = 0;
    saturn_heart_pre = *SATURN_HEART_MIRROR;   /* watchdog M-row baseline */
    {   /* Round-39: fresh SP-min window per frame (mirror-only, see
         * saturn_slave_sp_min — the slave samples it per heart). */
        volatile unsigned int *spp =
            (volatile unsigned int *)
            ((unsigned int)&saturn_slave_sp_min + 0x20000000u);

        *spp = 0xFFFFFFFFu;
    }
    sat_dispatch_retries = 0;                  /* round-38 retransmit budget */
    /* Round-38 dispatch: back to the per-frame SGL post (the round-37
     * permanent-slave-main takeover HARD-HUNG frame 1 on Kronos — the
     * master froze at the M3 marker with the vblank chain dead, twice:
     * SGL's slave program evidently needs the posted function to
     * RETURN between jobs). The dispatch race is instead handled by
     * RETRANSMISSION in the watchdog poll below: if the slave has not
     * ENTERED the callback (E still 0) within ~20 vbl, the master
     * re-posts the dispatch. A lost mailbox post now recovers in ~1/3 s
     * instead of stalling into the red screen.
     * ROUND-60 THE FREEZE FIX: slSlaveFunc's SlaveControl post waits
     * for the slave to ACK the PREVIOUS job finished — the r58/r59
     * state files caught the master wedged there forever after the
     * slave crashed mid-job. Once the boot experiment has proven the
     * direct mailbox recipe, dispatch through it instead: uncached
     * pointer writes, no SGL join, and every wait below is bounded by
     * our own poll. */
    /* ROUND-62 (2026-09-04): MASTER-ONLY. The file6.yss forensics
     * (direct mailbox retired, dispatch through SGL's own slSlaveFunc,
     * still froze at vbl 27995) caught SGL's own internals using the
     * SAME mailbox: SlaveCommand held SlaveControl (the post function
     * itself, 0x060457E0) as a queued job with SlaveParam = 0 — SGL's
     * internal sync posts SlaveControl(0) and the slave launched it,
     * deadlocking the slave while the master spun in SlaveControl's
     * wait loop. Jo's dispatch is an intruder in SGL's slave protocol;
     * every coexistence scheme (slSlaveFunc join, direct mailbox post,
     * raw SMPC/SETSINT takeover) has ended in a terminal freeze. No
     * game dispatches at all any more: the render path paints the full
     * width on the master, and the slave keeps only SGL's own internal
     * work. */
    if (saturn_slave_master_only == 0u)
        jo_core_exec_on_slave(saturn_render_slave_cb);
    {
        static int m2 = 0;

        if (!m2)
        {
            extern void Saturn_StageColor(unsigned short color);

            Saturn_StageColor(0x8300u);   /* RGB555 (0,24,0) */
            m2 = 1;
        }
    }
    {
        unsigned int   tv = saturn_vbl_count;

        /* Round-40: arm the MASTER half's bail (vbl+60; normal half
         * 1-2 vbl) and re-latch the dead-chain clocks at dispatch. */
        saturn_bail_arm_vbl   = tv;
        saturn_bail_arm_frt   = SAT_FRT();
        SAT_PUBLISH(saturn_bail_arm_vbl);   /* round-45: slave reads */
        SAT_PUBLISH(saturn_bail_arm_frt);
        saturn_master_bail_vbl = tv + 60u;
        saturn_frt_bail_reset();   /* round-49: FRT-acc bails re-zeroed */
        saturn_master_site = 4;    /* dispatch armed, master half next */
        /* Round-60: degraded (slave dead) = master-only, full width. */
        render_half(saturn_slave_master_only ? 0 : saturn_split_x,
                    SCREEN_W, player_sec, 0);
        /* 2026-09-03 photos #152/#153: the FRT is dead on real HW
         * (~55 ticks/vbl — every 16-bit-delta row reads phase
         * garbage), so the packed low byte is now the SPLIT POSITION
         * instead of a dead FRT delta: high = master-half vblanks,
         * low = saturn_split_x (24..136 fits a byte). The split
         * controller below consumes only the high byte. */
        saturn_frt_masterhalf =
            (int)((((saturn_vbl_count - tv) & 0xFFu) << 8) |
                  ((unsigned int)saturn_split_x & 0xFFu));
        /* Vbl-honest join wait: the join block below measures
         * vblanks from HERE (master half returned) to slave-done. */
        master_tv_after = saturn_vbl_count;
    }
    {
        static int m3 = 0;

        if (!m3)
        {
            extern void Saturn_StageColor(unsigned short color);
            extern void Saturn_StageTextHi(int, int, const char *);

            Saturn_StageColor(0xE000u);   /* RGB555 (0,0,24) */
            /* r96 TEXT PINPOINT LADDER (A): the master half RETURNED.
             * Painted through Saturn_StageText — the direct-framebuffer
             * glyph channel that produced the readable STALL/V/H/E/S/P
             * photos on real HW — because the W4/SWV DEAD path goes
             * through Saturn_BootMsg's VDP1 font, which is INVISIBLE
             * over the NBG0 game framebuffer (the round-34 lesson): the
             * 04:28 photo could never have shown W4 no matter where
             * the freeze was. Letters advance A->B->C->D in the frame;
             * a live poll readout (B/E/S rows) runs inside the join. */
            Saturn_StageTextHi(8, 8, "A");
            /* r98: master-side disc-ID stamp — the ISR beacon in
             * i_video.c only paints while the VBLANK chain is alive;
             * with a dead chain (the r95 'creeping colours' state)
             * this is the ONLY disc identifier on the frozen screen,
             * and its presence proves the master reached m3 on the
             * r98 build (kills the stale-disc ambiguity). The ISR
             * overwrites the same cells with identical glyphs when
             * the chain IS alive — idempotent. */
            Saturn_StageTextHi(8, 160, "r104");
            m3 = 1;
        }
    }
    {
        /* W4 = master half returned; W5 = slave join returned. With W3
         * present but W4 missing, the hang is in the master's seg
         * half; W4 present but W5 missing points at the slave half or
         * the join itself. First 4 frames only, like R1/R2/R3. */
        static int half_dbg_marks = 0;

        if (half_dbg_marks < 4)
        {
            extern void Saturn_BootMsg(const char *);
            extern void Saturn_StageTextHi(int, int, const char *);

            Saturn_BootMsg("W4");
            /* r96 (B): about to enter the join block. Same letter every
             * one of the first 4 frames — a healthy frame 2+ overwrites
             * the game picture anyway, only a freeze keeps it visible. */
            Saturn_StageTextHi(8, 28, "B");
            half_dbg_marks++;
        }
    }
    {
        /* Join window by FRT: dispatch..join-return minus the master's
         * own half (T) = slave excess + SGL join overhead. If this is
         * fat, the split is unbalanced and columns should be rebalanced
         * (or the slave's cart fetches/OC fixed) before any kernel
         * micro-work. */
        static int join_dbg_marks = 0;
        unsigned int tf = SAT_FRT();

        {
            /* SLAVE-HANG WATCHDOG (2026-09-03 real-HW bring-up): the
             * console froze on the IN-LOOP dark-cyan marker while Kronos
             * sailed through. Candidates: the slave never finishing, a
             * hang in the master's own half, or the new CCR purge on
             * real silicon. Poll the done flag (same uncached mirror
             * jo_core_wait_for_slave spins on) with a ~1 s timeout; on
             * timeout paint SOLID RED â€” an unmistakable "the SLAVE
             * never finished" signature (dark cyan without red would
             * instead mean the MASTER's half hung). Keep polling so a
             * very late finish still resumes the frame. */
            extern volatile unsigned char __jo_slave_done_flag;
            extern unsigned int           saturn_cart_base_addr;
            extern unsigned int           saturn_cart_size;
            extern int                    segwalls_count;
            extern int                    saturn_mmid_paint;
            extern int                    saturn_mmid_skip;
            volatile unsigned char *donep =
                (volatile unsigned char *)(&__jo_slave_done_flag
                                           + 0x20000000u);
            unsigned int v0 = saturn_vbl_count;
            int painted_red = 0;
            static unsigned int pr_last;   /* live-readout scheduler */
            /* Round-40: FRT-accumulated stall clock — independent of
             * the vblank chain, so a dead chain inside the JOIN still
             * fires the verdict (the walk/fill bail checks carry their
             * own). 60 Kronos-vbl budget ≈ 1 s (real-HW FRT is ~400x
             * slower per tick — a several-minute last resort there). */
            unsigned int frt_last = SAT_FRT();
            unsigned int frt_acc  = 0;
            unsigned int last_post = v0;   /* round-42: re-post clock */
            /* r95c: raw spin budget — the FRT fallback above needs
             * ~6.5 min of real-HW ticks to reach 1292160, so a join
             * stall with a DEAD vblank chain sat dark-blue (the m3
             * marker) for minutes with no verdict (the 04:28 photo).
             * The poll body is ~tens of cycles; 60M spins is a few
             * seconds of genuine polling and cannot fire on a healthy
             * slave (a real half completes in < 4 vbl). */
            unsigned int join_spins = 0;
            /* r96: live-join-readout scheduler (redrawn every ~1M
             * spins — ~50-100 ms, at the same rate the r95c spin
             * trigger calibrates the poll body). */
            unsigned int live_last = 0;

            saturn_master_site = 9;    /* round-50: in the join poll */
            {
                /* r96 (C): the join poll LOOP is about to be entered —
                 * anything past this letter is inside the loop. */
                static int fb_c = 0;

                if (fb_c < 4)
                {
                    extern void Saturn_StageTextHi(int, int, const char *);

                    Saturn_StageTextHi(8, 48, "C");
                    fb_c++;
                }
            }
            /* Round-60 degrade exit: once the slave is declared dead,
             * stop polling — the frame finishes with a stale left half
             * and every later frame renders master-only. */
            while (!*donep && saturn_slave_master_only == 0u)
            {
                saturn_master_beat++;   /* round-50: join poll heartbeat */
                join_spins++;
                frt_acc += (unsigned short)(SAT_FRT() - frt_last);
                frt_last = SAT_FRT();
                /* r96 LIVE JOIN READOUT: three glyph rows redrawn every
                 * ~1M spins (~50-100 ms) while the poll runs, so a real
                 * HW freeze PHOTOGRAPHS its own state:
                 *   B:<join_spins>>20> — the MASTER's beat: a cycling
                 *     digit proves the master is alive IN this poll (a
                 *     frozen beat = wedged entering the loop body or a
                 *     hard crash between C and here).
                 *   E=<pub[1]>  — 0 = the slave NEVER ENTERED its cb
                 *     (dispatch lost); 0x5217 = it entered.
                 *   S=<pub[3]>  — slave phase A000 armed / A001 entered
                 *     / A002 ticket / A003 purged+rendering / A004 half
                 *     done / DEAD self-bailed.
                 * Rows sit at y=108/128/148 — the red report's F/T/R
                 * band — but are suspended once painted_red so the
                 * verdict report owns the screen after it fires. */
                if (!painted_red &&
                    (unsigned int)(join_spins - live_last) >= (1u << 20))
                {
                    extern void Saturn_Uhex(char *, unsigned int);
                    extern void Saturn_StageTextHi(int, int, const char *);
                    char lr[10];
                    int  n;

                    live_last = join_spins;
                    lr[0] = 'B'; lr[1] = ':';
                    Saturn_Uhex(lr + 2, join_spins >> 20);
                    n = 0; while (lr[n]) n++;
                    for (; n < 8; n++) lr[n] = ' ';
                    lr[8] = 0;
                    Saturn_StageTextHi(8, 108, lr);
                    lr[0] = 'E'; lr[1] = '=';
                    Saturn_Uhex(lr + 2, SATURN_HEART_MIRROR[1]);
                    n = 0; while (lr[n]) n++;
                    for (; n < 8; n++) lr[n] = ' ';
                    lr[8] = 0;
                    Saturn_StageTextHi(8, 128, lr);
                    lr[0] = 'S'; lr[1] = '=';
                    Saturn_Uhex(lr + 2, SATURN_HEART_MIRROR[3]);
                    n = 0; while (lr[n]) n++;
                    for (; n < 8; n++) lr[n] = ' ';
                    lr[8] = 0;
                    Saturn_StageTextHi(8, 148, lr);
                }
                /* Round-38 DISPATCH RETRANSMIT: the round-36 stall's
                 * signature was E=0/S=A000 with V climbing — the master
                 * dispatched (it only dispatches after a clean join) but
                 * the slave callback NEVER ENTERED. slSlaveFunc's post
                 * travels through SGL's slave-side mailbox, which the
                 * slave polls through its CACHED address space, and the
                 * slave's operand cache is never purged outside a
                 * running callback — a stale clean "no job" line hides
                 * the post indefinitely (round-37 tried to route around
                 * this with a permanent uncached-polling slave main;
                 * that takeover hard-hung frame 1, so the fix is to
                 * simply RE-POST: a lost dispatch now recovers ~1/3 s
                 * late instead of stalling into the red screen).
                 * Retransmit only while E is still 0 — if the callback
                 * ENTERED (0x5217), the half is merely slow and the
                 * self-bail deadline governs. Capped: a slave that never
                 * enters anything still reaches the red screen. */
                /* Round-42: re-post FOREVER while E==0 (every 32 vbl),
                 * INCLUDING after the red paints — the old
                 * !painted_red + 8-post cap gave up at ~2.7 s, but the
                 * round-41 run proved the slave can come back at this
                 * spot (E=0x5217 fire recovered via bail-lurch). A
                 * slave that revives after the paint now still gets
                 * the job: the frame completes, donep sets, the
                 * recovery unpaints — the stall becomes a hitch
                 * instead of a parked red screen. */
                if (SATURN_HEART_MIRROR[1] == 0u
                    && (unsigned int)(saturn_vbl_count - last_post) > 32u
                    && sat_dispatch_retries < 1000)
                {
                    last_post = (unsigned int)saturn_vbl_count;
                    SATURN_HEART_MIRROR[0] = 0u;
                    SATURN_HEART_MIRROR[1] = 0u;
                    SATURN_HEART_MIRROR[2] = 0u;
                    SATURN_HEART_MIRROR[3] = 0xA000u;
                    saturn_heart_pairs     = 0;
                    saturn_heart_pre = *SATURN_HEART_MIRROR;
                    {
                        volatile int *tickp =
                            (volatile int *)((unsigned int)
                                             &saturn_slave_ticket
                                             + 0x20000000u);

                        *tickp = 1;
                    }
                    /* Round-60: re-post through the direct mailbox when
                     * it's proven — SlaveControl's join must never be
                     * entered from inside this watchdog either. */
                    if (saturn_slave_direct_ok() != 0u)
                        saturn_slave_direct_post(saturn_render_slave_cb);
                    else
                        jo_core_exec_on_slave(saturn_render_slave_cb);
                    sat_dispatch_retries++;
                    /* Round-60 DEGRADE LADDER: the slave has never
                     * ENTERED any of ~12 re-posts over ~6 s — it is
                     * dead (the r59 state files: crashed mid-job, SGL
                     * join can never complete). Master-only from the
                     * next frame on; the loop condition exits the
                     * poll. One degraded frame-rate beats a dead
                     * console. */
                    if (sat_dispatch_retries >= 12
                        && saturn_slave_master_only == 0u)
                    {
                        extern void Saturn_BootMsg(const char *);
                        extern void Saturn_StageTextHi(int, int,
                                                       const char *);

                        saturn_slave_master_only = 1u;
                        Saturn_BootMsg("SLV DEAD");
                        /* r96: the BootMsg font is invisible over the
                         * NBG0 game framebuffer on real HW — stamp the
                         * degrade verdict in the framebuffer channel. */
                        Saturn_StageTextHi(8, 88, "SLVDEAD");
                    }
                }
                /* Round-60: slave ENTERED but never finished in 600 vbl
                 * (its own self-bail is 180) — it crashed mid-job. */
                if (SATURN_HEART_MIRROR[1] != 0u
                    && (unsigned int)(saturn_vbl_count - v0) > 600u
                    && saturn_slave_master_only == 0u)
                {
                    extern void Saturn_BootMsg(const char *);
                    extern void Saturn_StageTextHi(int, int, const char *);

                    saturn_slave_master_only = 1u;
                    Saturn_BootMsg("SLV DEAD");
                    Saturn_StageTextHi(8, 88, "SLVDEAD");
                }
                if (!painted_red &&
                    ((unsigned int)(saturn_vbl_count - v0) > 60u
                     || saturn_seg_bailed >= 2
                     || join_spins > 60000000u   /* r95c: dead-clock join */
                     || frt_acc > 1292160u))   /* ~60 Kronos vbl of FRT
                                                * with no vbl advance =
                                                * dead chain in the JOIN */
                {
                    extern void Saturn_StageColor(unsigned short color);

                    /* RED FIRST (2026-09-04 round 12): paint before the
                     * recorder save — the old order built recw[] and ran
                     * Saturn_BupSave BEFORE painting, so a BUP write that
                     * wedged left a frozen NORMAL scene, indistinguishable
                     * from a master-half hang. Red on the frozen screen
                     * now = "the slave stalled AND the watchdog fired";
                     * the save running after can no longer hide it.
                     * Round-40: RED = slave join stall (unchanged);
                     * MAGENTA = a MASTER-side bail (saturn_seg_bailed:
                     * 2 = the master's render half blew its deadline,
                     * 3 = the pre-dispatch BSP build did, 4 = the
                     * vblank chain is dead — FRT advanced, vbl frozen).
                     * The 10:42 run froze SILENTLY because only the
                     * slave's done flag was watched. */
                    if (saturn_seg_bailed == 0)
                        saturn_seg_bailed = 4;   /* FRT-only fire */
                    Saturn_StageColor(saturn_seg_bailed >= 2
                                          ? 0x7C1Fu   /* solid MAGENTA */
                                          : 0x001Fu); /* solid RED */

                    /* Round-33: FIRST live readout, BEFORE the recorder
                     * save — a BUP write that wedges must not hide the
                     * verdict (that exact hazard is why the red paint
                     * moved ahead of the save). The master is alive in
                     * this poll, so put the stall data where the pilot
                     * can read it off the screen: V = stall vblanks,
                     * H = slave-heart delta vs dispatch. */
                    pr_last = (unsigned int)saturn_vbl_count;
                    saturn_red_screen_report(
                        (unsigned int)(saturn_vbl_count - v0),
                        (int)(*SATURN_HEART_MIRROR - saturn_heart_pre),
                        (int)SATURN_HEART_MIRROR[1],
                        (int)saturn_seg_bailed, frt_acc);

                    /* Round 7: snapshot WHERE the slave is. This delta
                     * (columns of progress since dispatch) prints on
                     * the M row — decode table at saturn_slave_heart. */
                    saturn_dbg_red_heart =
                        (int)(*SATURN_HEART_MIRROR - saturn_heart_pre);

                    /* Round-10e FLIGHT RECORDER (2026-09-04): the red
                     * screen can be PERMANENT (slave never finishes ->
                     * this poll never exits) and a reset kills all WRAM
                     * state — the C/M rows die with it. Round-11c: the
                     * record's PRIMARY home is now the internal backup
                     * RAM (battery-backed, survives the SAROO boot menu
                     * that wipes cart DRAM); Saturn_BupSave in i_wad.c.
                     * The cart's top 512 bytes stay as a secondary copy
                     * for boots where BUP is dead, written through the
                     * cart's CACHED mirror then OC-purged. i_video.c
                     * consumes the record on the next boot (BUP first,
                     * cart fallback) and seeds the C/M/X rows with it. */
                    {
                        /* Round-39: widened 21 -> 26. recw[21..23] = SGL
                         * resident-slave protocol words (SlaveCommand /
                         * SlaveState / SlaveParam, 0x06043940/44/48 read
                         * uncached at fire), recw[24] = ABSOLUTE
                         * saturn_vbl_count (the round-36 record suggested
                         * the counter itself may get rewritten mid-run —
                         * this watches it live), recw[25] = the slave's
                         * deepest SP this frame (stack-overflow probe).
                         * NOTE: the round-36 disk decodes were ambiguous
                         * because the then-running build's field map
                         * differed from this one — recw[6] on disk read
                         * as (segwalls<<16)|split under the CURRENT map.
                         * From now on recw[0]='REC1' + these round-39
                         * words identify the layout unambiguously. */
                        extern volatile unsigned int SlaveCommand;
                        extern volatile unsigned int SlaveState;
                        extern volatile unsigned int SlaveParam;
                        volatile unsigned int *sglp =
                            (volatile unsigned int *)
                            ((unsigned int)&SlaveCommand + 0x20000000u);
                        volatile unsigned int *spp =
                            (volatile unsigned int *)
                            ((unsigned int)&saturn_slave_sp_min
                             + 0x20000000u);
                        unsigned int recw[54];

                        recw[0] = 0x52454331u;           /* 'REC1' */
                        recw[1] = (unsigned int)saturn_dbg_red_heart;
                        recw[2] = (unsigned int)saturn_heart_pre;
                        recw[3] = (unsigned int)*SATURN_HEART_MIRROR;
                        recw[4] = ((unsigned int)saturn_mmid_paint << 16)
                                | ((unsigned int)saturn_mmid_skip
                                   & 0xFFFFu);
                        recw[5] = ((unsigned int)segwalls_count << 16)
                                | ((unsigned int)saturn_split_x
                                   & 0xFFFFu);
                        recw[6] = (unsigned int)saturn_vbl_count;
                        recw[7] = (unsigned int)(saturn_vbl_count - v0);
                        /* Round-36: latch the readout's E/S words — E =
                         * pub[1] (0x5217 once the cb ran this dispatch),
                         * S = pub[3] phase code (A000 armed .. A004 done
                         * / DEAD self-bail). recw[1..5] all zero with
                         * E=0 = the slave NEVER ENTERED (dispatch race),
                         * not a mid-render hang. */
                        recw[19] = (unsigned int)SATURN_HEART_MIRROR[1];
                        recw[20] = (unsigned int)SATURN_HEART_MIRROR[3];
                        recw[21] = sglp[0];
                        recw[22] = sglp[1];
                        recw[23] = sglp[2];
                        recw[24] = (unsigned int)saturn_vbl_count;
                        recw[25] = *spp;
                        recw[26] = (unsigned int)saturn_seg_bailed;
                        /* Round-41 fence hunt: [27] = first tripped
                         * fence id (0 = clean), [28] = its bad word
                         * value, [29] = which of the 4 fence words. */
                        recw[27] = (unsigned int)
                            saturn_rtbl_fence_check();
                        recw[28] = saturn_rtbl_trip_val;
                        recw[29] = saturn_rtbl_trip_word;
                        /* Round-42: [30] = the join-poll FRT stall
                         * accumulator (with recw[7]=vbl delta gives the
                         * true FRT rate -> calibrates both dead-chain
                         * thresholds). [31] = 0xC0FE<<16 anchor (the
                         * disk tail has read +2 words off for three
                         * builds straight; finding this magic pins the
                         * shift) | low half = CD-fill state at the fire
                         * (the round-33 precedent: a BUP write during
                         * an async GFS batch killed the whole CD
                         * block). */
                        recw[30] = frt_acc;
                        {
                            extern int Saturn_CdFillBusy(void);
                            extern volatile unsigned int
                                saturn_dc_trip_frt;
                            extern volatile unsigned int
                                saturn_dc_site;
                            extern volatile unsigned int
                                saturn_dc_seen_vbl;
                            extern volatile unsigned int
                                saturn_dc_seen_arm;

                            recw[31] = (0xC0FEu << 16)
                                     | ((unsigned int)Saturn_CdFillBusy()
                                        & 0xFFFFu);
                            /* Round-43: the seg-side dead-chain trip's
                             * own FRT delta — spurious-threshold vs
                             * real chain death. */
                            recw[32] = saturn_dc_trip_frt;
                            recw[33] = 0xC0FB0000u;   /* tail anchor 2 */
                            /* Round-46 view stamps — decode by
                             * pattern, immune to the disk shift. */
                            recw[34] = (0xDC01u << 16)
                                     | ((unsigned int)saturn_dc_site
                                        & 0xFFFFu);
                            recw[35] = 0xD3000000u
                                     | (saturn_dc_seen_vbl & 0xFFFFFFu);
                            recw[36] = 0xD4000000u
                                     | ((unsigned int)saturn_vbl_count
                                        & 0xFFFFFFu);
                            recw[37] = 0xD5000000u
                                     | (saturn_dc_seen_arm & 0xFFFFFFu);
                            recw[38] = 0xD6000000u
                                     | ((unsigned int)saturn_seg_bailed
                                        & 0xFFFFFFu);
                            recw[39] = 0xC0FA0000u;   /* tail anchor 3 */
                            /* Round-48: previous job's exit shadow. */
                            recw[40] = 0xD9000000u
                                     | (saturn_prev_pub3 & 0xFFFFFFu);
                            recw[41] = 0xDA000000u
                                     | (saturn_prev_pub0 & 0xFFFFFFu);
                            recw[42] = 0xDB000000u
                                     | (saturn_prev_pub2 & 0xFFFFFFu);
                            recw[43] = 0xC0F90000u;   /* tail anchor 4 */
                            /* Round-49: the FRT-accumulator bails' own
                             * totals at the fire — how much FRT time
                             * the wedged leg really consumed before the
                             * master painted (kind 2/3 = confirmed
                             * FRT-covered freeze; kinds 0/4 with these
                             * ~0 = froze outside the guarded legs). */
                            extern volatile unsigned int saturn_half_frt_acc;
                            extern volatile unsigned int saturn_build_frt_acc;

                            recw[44] = 0xDD000000u
                                     | (saturn_half_frt_acc & 0xFFFFFFu);
                            recw[45] = 0xDE000000u
                                     | (saturn_build_frt_acc & 0xFFFFFFu);
                            recw[46] = 0xC0F80000u;   /* tail anchor 5 */
                            /* Round-51 legs: sprite-pass / flush-run FRT
                             * accs at the fire (kind 5/6 = confirmed the
                             * wedged leg; kind 7/8 fire in i_video.c —
                             * its accs ride recw[47]/[48] too). */
                            recw[47] = 0xDF000000u
                                     | (saturn_spr_frt_acc & 0xFFFFFFu);
                            recw[48] = 0xE1000000u
                                     | (saturn_flu_frt_acc & 0xFFFFFFu);
                            recw[49] = 0xC0F70000u;   /* tail anchor 6 */
                            /* Round-52: preload acc + the crawl counters
                             * (L/T rows) and segwalls at the fire. */
                            recw[50] = 0xE2000000u
                                     | (saturn_pre_frt_acc & 0xFFFFFFu);
                            recw[51] = 0xE3000000u
                                     | ((saturn_load_count & 0xFFu) << 16)
                                     | (saturn_frame_count & 0xFFFFu);
                            recw[52] = 0xE4000000u
                                     | ((unsigned int)segwalls_count
                                        & 0xFFFFFFu);
                            recw[53] = 0xC0F60000u;   /* tail anchor 7 */
                        }
                        /* Round-10g: latch the colormap canary state —
                         * a red screen during the stomp hunt still
                         * delivers the verdict after a reset.
                         * recw[8] = (index << 16) | count of the LATEST
                         * bad poll; recw[9] = (boot probe << 16) |
                         * (live total & 0xFFFF). */
                        {
                            extern volatile int saturn_cm_bad;
                            extern volatile int saturn_cm_bad_live;
                            extern volatile int saturn_cm_bad_index;
                            extern volatile int saturn_cm_bad_count;

                            recw[8] = ((unsigned int)
                                       (saturn_cm_bad_index & 0xFF) << 16)
                                    | ((unsigned int)saturn_cm_bad_count
                                       & 0xFFFFu);
                            recw[9] = ((unsigned int)
                                       (saturn_cm_bad & 0xFF) << 16)
                                    | ((unsigned int)saturn_cm_bad_live
                                       & 0xFFFFu);
                            /* Round-10h: the distinct-bad-index bitmap,
                             * 32 bytes = recw[10..17], plus imin/imax/
                             * idist/val in recw[18]. */
                            {
                                const unsigned char *bm =
                                    Saturn_ColormapBadmap();
                                int k;

                                for (k = 0; k < 8; k++)
                                    recw[10 + k] =
                                        ((unsigned int)bm[k * 4] << 24)
                                        | ((unsigned int)bm[k * 4 + 1]
                                           << 16)
                                        | ((unsigned int)bm[k * 4 + 2]
                                           << 8)
                                        | (unsigned int)bm[k * 4 + 3];
                                recw[18] = ((unsigned int)
                                            (saturn_cm_bad_imin & 0xFF)
                                            << 24)
                                         | ((unsigned int)
                                            (saturn_cm_bad_imax & 0xFF)
                                            << 16)
                                         | ((unsigned int)
                                            (saturn_cm_bad_idist & 0xFF)
                                            << 8)
                                         | ((unsigned int)
                                            saturn_cm_bad_val & 0xFFu);
                            }
                        }
                        {
                            extern int Saturn_BupSave(const unsigned int *,
                                                      int);

                            /* Round-33b: BUP_Write is a CD-block SH-1
                             * operation — issuing it while an async
                             * GFS_NwFread batch is mid-flight took the
                             * whole CD block down (2026-09-04 07:52
                             * Kronos: stripes + dead window).
                             * Round-36: the batch is ABORTED first
                             * (NwStop + fresh handle) instead of
                             * skipping the save — mid-demo the batch is
                             * ALWAYS in flight, so the gated save never
                             * ran and the round-35 stall left a stale
                             * boot-era record in backup RAM. Aborted
                             * batch + save = the recorder actually
                             * captures mid-game stalls. */
                            {
                                extern void Saturn_CdFillStallAbort(void);

                                Saturn_CdFillStallAbort();
                            }
                            Saturn_BupSave(recw, 54);
                        }
                        if (saturn_cart_base_addr != 0u)
                        {
                            volatile unsigned int *rec =
                                (volatile unsigned int *)
                                (saturn_cart_base_addr + SATURN_CART_REC_OFF);
                            int k;

                            for (k = 0; k < 19; k++)
                                rec[k] = recw[k];
                            saturn_oc_purge();
                        }
                    }

                    painted_red = 1;      /* red painted above, before
                                           * the recorder save */
                }
                else if (painted_red)
                {
                    /* Wedge persisting: keep rec[7] (stall length)
                     * climbing once per vbl so a post-reset read
                     * separates "wedged right after firing" (small
                     * value) from "alive but catastrophically slow for
                     * seconds" (hundreds/thousands). */
                    static unsigned int        rec_vbl;
                    volatile unsigned int     *rec =
                        (volatile unsigned int *)
                        (saturn_cart_base_addr + SATURN_CART_REC_OFF);

                    if (saturn_cart_base_addr != 0u
                        && rec_vbl != (unsigned int)saturn_vbl_count)
                    {
                        rec_vbl = (unsigned int)saturn_vbl_count;
                        rec[7] = (unsigned int)(saturn_vbl_count - v0);
                        saturn_oc_purge();
                    }

                    /* Round-33: LIVE readout refresh (every 32 vbl).
                     *   V climbing, H frozen  = truly wedged at H
                     *   V climbing, H growing = alive but crawling
                     * H decode (saturn_slave_heart): 0 = never painted
                     * a column; small N = wall-walk pair N; ~N*1000 =
                     * open-band fill column N. */
                    if ((unsigned int)(saturn_vbl_count - pr_last) >= 32u)
                    {
                        pr_last = (unsigned int)saturn_vbl_count;
                        saturn_red_screen_report(
                            (unsigned int)(saturn_vbl_count - v0),
                            (int)(*SATURN_HEART_MIRROR
                                  - saturn_heart_pre),
                            (int)SATURN_HEART_MIRROR[1],
                            (int)saturn_seg_bailed, frt_acc);
                    }
                }
            }
            /* ROUND-63 (2026-09-04): gate the join-wait on a dispatch
             * actually having happened. file7.yss (round-62 freeze,
             * frame 1, vbl 10083): with saturn_slave_master_only == 1
             * from boot there are NO dispatches, __jo_slave_done_flag
             * never sets, and this call — which was OUTSIDE every
             * master_only gate — spun on it forever (state file: flag
             * 0x00, heart S=0xA000 armed / E=0, execs=0, site=9, the
             * master 4497 vbl into one render call, PC at this call's
             * return site). The wait exists to consume + reset the
             * done flag after a REAL dispatch's join; with no dispatch
             * there is nothing to wait for. */
            if (saturn_slave_master_only == 0u)
                jo_core_wait_for_slave();   /* flag set: proper reset + purge */
            saturn_master_site = 0xA;   /* round-50: join returned */
            {
                /* r96 (D): the JOIN RETURNED — the poll, the re-posts
                 * and the wait all completed. With C present but D
                 * missing on a frozen screen, the wedge is inside the
                 * poll window itself (the B/E/S rows say which). */
                static int fb_d = 0;

                if (fb_d < 4)
                {
                    extern void Saturn_StageTextHi(int, int, const char *);

                    Saturn_StageTextHi(8, 68, "D");
                    fb_d++;
                }
            }
            if (painted_red)
            {
                /* Transient stall recovered: unpaint the red so a
                 * one-off >1 s slave stall doesn't leave the screen
                 * permanently red. RED THAT COMES BACK = the stall
                 * keeps happening; red that clears was transient. */
                extern void Saturn_StageColor(unsigned short color);

                Saturn_StageColor(0x0000u);   /* back to black letterbox */
                saturn_seg_bailed = 0;   /* round-40: verdict consumed —
                                          * a fresh frame starts clean */
                /* Round 7 census for the HUD C row: how many stalls,
                 * and how long the last one ran (vblanks, capped). */
                saturn_dbg_red_events++;
                saturn_dbg_red_stallvbl =
                    (int)(saturn_vbl_count - v0) > 255
                        ? 255 : (int)(saturn_vbl_count - v0);
            }
        }
        saturn_frt_joinvbl =
            (int)(saturn_vbl_count - master_tv_after);

        /* Adaptive split controller (runs AFTER the join — the slave is
         * done, so saturn_split_x is stable for next frame's dispatch).
         * 2026-09-03 photos #152/#153: FRT deltas are phase garbage on
         * real HW, so the controller now reads only VBLANK clocks.
         * joinvbl >= 2 = the slave's half ran >= 2 vbl past the
         * master's: shrink its range [0, split). Otherwise the master
         * half (M high byte, in vbl) decides: more than 1 vbl past the
         * slave's (saturn_prof_slave, also vbl) = master is the long
         * pole, grow. One-vbl hysteresis on both sides keeps the
         * 1-vbl-quantized readings from oscillating. */
        if (saturn_frt_joinvbl >= 2)
        {
            int step = saturn_frt_joinvbl >> 1;

            if (step > 4)
                step = 4;
            saturn_split_x -= step;
        }
        else if ((((unsigned int)saturn_frt_masterhalf >> 8) & 0xFFu) >
                 (unsigned int)saturn_prof_slave + 1u)
        {
            /* MASTER-long-pole growth (vbl-honest, 2026-09-03): the
             * master's half ran >1 vbl past the slave's whole range.
             * Overshoot is harmless — the joinvbl branch above claws
             * it back within a few frames. */
            saturn_split_x += 2;
        }
        /* Real-HW bounds (2026-09-03): the console pinned the split at
         * the old 48 floor with j still 16 â€” the slave is per-column
         * slower than the master there, so the floor moved to 24. The
         * ceiling moved out symmetrically. */
        if (saturn_split_x < 24)
            saturn_split_x = 24;
        if (saturn_split_x > 136)
            saturn_split_x = 136;
        /* SINGLE-CORE PROBE (2026-09-03 round 6) REVERTED same evening:
         * the video proved the booted disc was the pre-pin overlap
         * build (A wandered 82-94), so the probe never ran — and the
         * stripe was root-caused first: the slave's fallback paints on
         * NoLoad first-sight misses (the pink ceilshade ramp; see
         * paint_flat_run). The slave now skips on a miss instead, so
         * the real dual-core split is back. Re-pin here (saturn_split_x
         * = SCREEN_W - 1) only if the stripe ever returns. */

        /* POST window: join-return -> here (anything the dual-CPU path
         * does after the join before this section ends). */
        saturn_frt_disp_post = saturn_frt_span8(pre_f0, pre_v0,
                                           SAT_FRT(), saturn_vbl_count);

        if (join_dbg_marks < 4)
        {
            extern void Saturn_BootMsg(const char *);

            Saturn_BootMsg("W5");
            join_dbg_marks++;
        }
    }
#endif

#ifdef SATURN_SEG_DIFF
    /* Seg-vs-raycaster diff (2026-09-01): the old verified raycaster is
     * still in the tree, so render every frame TWICE â€” seg renderer via
     * the normal dispatch, then snapshot the fb, re-clear, and re-render
     * the full view with render_columns on the master. Every pixel the
     * two disagree on is painted the reddest/greenest/bluest PLAYPAL
     * index by mismatch class; the HUD 'Z' row counts them. Red = a
     * seg-vs-raycaster divergence at that exact spot â€” no screenshot
     * timing needed, just walk around and look for red.
     * Costs a full single-core raycast per frame (~3 fps) â€” diagnostic
     * build only. */
    {
        static byte *segdiff_snap;
        static byte  segdiff_red, segdiff_green, segdiff_blue;
        static byte *segdiff_pal;
        static int   segdiff_inited = 0;
        int          x, y, n = 0;
        static unsigned short hole_blue[SCREEN_W];   /* blue px per column */

        if (!segdiff_inited)
        {
            byte *pal;

            segdiff_snap = (byte *)Z_Malloc(SCREEN_W * DOOM_FBHEIGHT, PU_STATIC);
            pal          = (byte *)W_CacheLumpName("PLAYPAL", PU_STATIC);
            segdiff_pal  = pal;
            if (pal)
            {
                int i, bred = 0, bgreen = 0, bblue = 0;
                int vred = -1000, vgreen = -1000, vblue = -1000;

                for (i = 0; i < 256; i++)
                {
                    int r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
                    int v;

                    v = r - (g + b) / 2;
                    if (v > vred) { vred = v; bred = i; }
                    v = g * 2 - r - b;
                    if (v > vgreen) { vgreen = v; bgreen = i; }
                    v = b * 2 - r - g;
                    if (v > vblue) { vblue = v; bblue = i; }
                }
                segdiff_red   = (byte)bred;
                segdiff_green = (byte)bgreen;
                segdiff_blue  = (byte)bblue;
            }
            segdiff_inited = 1;
        }

        memcpy(segdiff_snap, I_VideoBuffer, SCREEN_W * DOOM_FBHEIGHT);

#ifdef SATURN_SEG_SHOW
        /* Seg-only view (2026-09-02): skip the raycaster + red paint and
         * blit the seg pass back â€” shows what the seg renderer actually
         * drew, for eyeball comparison against the known-good raycaster.
         * Z reads 0 in this build. Diagnostic only. */
        memcpy(I_VideoBuffer, segdiff_snap, SCREEN_W * DOOM_FBHEIGHT);
        saturn_seg_diff_count = 0;
#else
        /* Same base the seg pass started from. */
        memset(I_VideoBuffer, 0x20, SCREEN_W * DOOM_FBHEIGHT);
        memset(I_VideoBuffer, 0, VIEW_TOP * SCREEN_W);
        memset(I_VideoBuffer + VIEW_BOT * SCREEN_W, 0,
               (DOOM_FBHEIGHT - VIEW_BOT) * SCREEN_W);

        /* Ray-side probe: sentinel reset, then tag the pass so
         * paint_wall_span records into the ray slots. */
        saturn_probe_ray_tex = 99999;
        saturn_probe_ray_u   = 0;
        saturn_probe_ray_y   = 99999;
        saturn_probe_ray_tu  = 0;
        saturn_probe_ray_peg = 0;
        saturn_probe_ray_floor = 0;   /* accumulated painted floor ROWS */
        saturn_probe_ray_painted = 0; /* first-span probe: rearm */
        saturn_diff_ray_pass = 1;
        render_columns(0, SCREEN_W, player_sec, 0);
        saturn_diff_ray_pass = 0;

        /* Three-way decode (2026-09-02): RED = both drew but disagree
         * (texel/uv math divergence), GREEN = seg-only (seg paints where
         * the raycaster cleared â€” overpaint / clip leak), BLUE =
         * raycaster-only (seg left a hole â€” clip too tight / wall
         * dropped). Background in the band is the 0x20 clear. */
        memset(hole_blue, 0, sizeof(hole_blue));
        for (y = VIEW_TOP; y < VIEW_BOT; y++)
        {
            byte *a = I_VideoBuffer + y * SCREEN_W;
            byte *b = segdiff_snap + y * SCREEN_W;

            for (x = 0; x < SCREEN_W; x++)
                if (b[x] == 0x20)
                    hole_blue[x]++;
                if (a[x] != b[x])
                {
                    /* Tolerance gate (2026-09-02): adjacent colormap rows
                     * of the SAME texel differ by tiny RGB steps, so raw
                     * inequality counts shading-rounding noise. Only
                     * divergence past TOL paints and counts. */
                    const byte *pa = segdiff_pal + a[x] * 3;
                    const byte *pb = segdiff_pal + b[x] * 3;
                    int dr = pa[0] - pb[0];
                    int dg = pa[1] - pb[1];
                    int db = pa[2] - pb[2];

                    if (dr < 0) dr = -dr;
                    if (dg < 0) dg = -dg;
                    if (db < 0) db = -db;
                    if (dr + dg + db <= 16)
                        continue;
                    if (b[x] == 0x20)      a[x] = segdiff_blue;
                    else if (a[x] == 0x20) a[x] = segdiff_green;
                    else                   a[x] = segdiff_red;
                    n++;
                }
        }
        saturn_seg_diff_count = n;

        /* Hole census decode: first column whose seg frame kept the 0x20
         * clear for most of the view band â€” nothing painted it at all.
         * D row = that column's FINAL seg clip ((ce<<8)|fl: a closed
         * band here proves a wall narrowed the clip without painting),
         * H row = (col<<8)|owning sector. 99999 = no full column hole. */
        {
            int hx = 99999, hc = 0, hs = 0;
            int thresh = (VIEW_BOT - VIEW_TOP) * 2 / 3;

            for (x = 0; x < SCREEN_W; x++)
                if (hole_blue[x] >= thresh)
                {
                    hx = x;
                    hc = saturn_dbg_clip[x];
                    hs = saturn_dbg_lastsec[x];
                    break;
                }
            saturn_probe_hole_x    = hx;
            saturn_probe_hole_clip = hc;
            saturn_probe_hole_sec  = hs;
        }
#endif
    }
#endif

    {
        extern volatile int saturn_prof_join;
        extern volatile int saturn_prof_active;

        saturn_prof_join   = (int)(saturn_vbl_count - saturn_prof_p1);
        saturn_prof_active = active_count_v[0] + active_count_v[1];
    }

#if SATURN_RENDER_DIAG
    diag_strip(ndeg, nhits);
    diag_minimap();
#endif
}