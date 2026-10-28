/* saturn_segs.c â€” 32X-style BSP/seg wall renderer (2026-09-01).
 *
 * Ported from d32xr's r_phase1.c (BSP traversal + viswall build),
 * r_phase2.c (R_PointToDist / R_ScaleFromGlobalAngle / wall prep) and
 * r_phase6.c (per-column seg math), replacing the per-column raycaster
 * (crossing scan + walk) with the classic vanilla pipeline:
 *
 *   1. saturn_segs_build (master, once per frame before the dispatch
 *      purge): R_BSP walks the node tree front-to-back, clips every seg
 *      against solidsegs, and fills segwalls[] with per-piece geometry
 *      AND all per-seg prep (scalefrac/scalestep/distance/offset/
 *      centerangle, action bits, texture bands). The slave only reads
 *      the finished array.
 *   2. saturn_segs_render_half (each core over its own column range):
 *      one fused loop per viswall per column â€” scale2 = scalefrac +
 *      (x-start)*scalestep (the 32X's linear interpolation), open-band
 *      flats through the existing run-merge staging (fr_emit_flat;
 *      sky is auto-detected by the painter), wall bands through
 *      paint_wall_span, then the clipbounds narrowing with the same
 *      fallbacks as R_SegLoop.
 *
 * Projection equivalence (why the d32xr math lands on our pixels):
 * R_ScaleFromGlobalAngle with stretchX = PROJ<<16 gives
 *   scale2 = (PROJ<<16) * sin(ray-seg) / (d_perp * cos(ray-view))
 * and the wall hit's forward depth is t_u = d_perp*cos(ray-view)/
 * sin(ray-seg), so scale2 = (PROJ<<16)/t_u â€” the raycaster's
 * y = HALF_H - z_rel*PROJ/t_u becomes cy - (scale2*h)>>FRACBITS.
 * The angle->column tables are built exactly like d32xr's
 * R_InitMathTables with viewportWidth = SCREEN_W (320), centerX = 160 â€”
 * identical rays to the raycaster's PROJ_X = 160 column build.
 *
 * Deviations from d32xr (all deliberate, raycaster parity):
 *   - no texturetranslation/mips/decals in the painter: raw texture
 *     numbers go straight to paint_wall_span (lazy-cache painter);
 *     texturetranslation is read in early prep ONLY for the vanilla
 *     peg math's texture heights.
 *   - pegs are vanilla's (r_segs.c): DONTPEGBOTTOM bottoms anchor at
 *     the front ceiling, pegged tops at back ceiling + texheight,
 *     solid walls DONTPEGBOTTOM at front floor + texheight, everything
 *     else at the front ceiling.
 *   - single-sided lines paint mid||top||bottom texture (d32xr paints
 *     mid only); a zero-texture wall paints the fallback ramp.
 *   - a two-sided MIDTEXTURE covers the whole open band and CLOSES the
 *     clip for that column (the raycaster's covered=1), so closed
 *     doors occlude exactly as they do today. d32xr instead leaves the
 *     clip open and blends mid textures last.
 *   - no sprites, no SIL bits, no seglight packing (the painter reads
 *     the sidedef's own sector).
 *
 * NOT bit-exact with the raycaster (linear scalestep interpolation,
 * d32xr-style) â€” value-checked visually. The raycaster remains intact
 * behind the SATURN_SEG_RENDERER build flag.
 */

#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "r_local.h"
#include "i_video.h"
#include "saturn_segs.h"
#include "saturn_leaf.h"
#include "saturn_vdp1.h"

/* saturn_tex.h drags stexinfo_t plumbing; only the loader entry point is
 * needed here (see the red-strip fix at saturn_segs_preload_textures). */
extern byte *Saturn_TexData(int texnum);
/* Round 10: masked-mid gate (saturn_tex.c). */
extern int   Saturn_TexHasHoles(int texnum);
/* VDP1 quad capture: cached texture width in texels (saturn_tex.c);
 * 0 = texture unusable (no repetition split, checker fallback). */
extern int   Saturn_TexWidth(int texnum);
extern int   Saturn_TexHeight(int texnum);
/* d32xr seg lighting curve (saturn_render.c) — the quad gouraud rate
 * uses the SAME light the paint path applies (COLORMAP row 0..31). */
extern byte  Saturn_LightRow(int lightlevel, int dist);
/* WRAM bump allocator (saturn_tex.c) — spr_vis[] at the d32xr size. */
extern void *Saturn_HramAlloc(int size);

/* Must match saturn_render.c (full-width render since 2026-09-05;
 * 2026-09-06 full-screen layout: view rows 0..191, STBAR strip
 * 192..223 in the 224-line fb) */
#define SCREEN_W  DOOM_SCREENWIDTH          /* 320 */
#define SCREEN_H  DOOM_SCREENHEIGHT         /* 200 */
#define HALF_H    (SCREEN_H / 2)
#define VIEW_TOP  0
#define VIEW_BOT  192
#define PROJ      160
#define PROJ_X    160     /* horizontal focal (saturn_render.c) */

#define COL_WALL_TOP 0x80
#define COL_WALL_LOW 0xC0

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

#define SEGW_MAX   165      /* d32xr MAXWALLCMDS */
#define MAXSEGS    32

/* action bits â€” d32xr r_local.h values we consume */
#define AC_ADDFLOOR      0x001
#define AC_ADDCEILING    0x002
#define AC_TOPTEXTURE    0x004
#define AC_BOTTOMTEXTURE 0x008
#define AC_NEWCEILING    0x010
#define AC_NEWFLOOR      0x020
#define AC_TOPSIL        0x040
#define AC_BOTTOMSIL     0x080
#define AC_SOLIDSIL      0x100
#define AC_ADDSKY        0x200
#define AC_MIDTEXTURE    0x800

/* d32xr's cliprange_t on MARS is VINT = short (doomdef.h:54-55), i.e.
 * 4 bytes per range. seg_clip_wall_segment's insert/crunch paths move
 * ranges as SINGLE INTS ((int*)start = *(int*)next, rotate loops with
 * cnt = next - start) â€” correct only while a range IS one int. The
 * first port used int fields (8-byte ranges), so every insert/crunch
 * copied just the 'first' field and left 'last' stale, corrupting the
 * occlusion list: slivers of far walls painting through near geometry,
 * stray one-row wall bands (the mid-air vertical strip + white line).
 * Values are in [-2, 162] so shorts are always wide enough. */
typedef struct
{
    short first, last;
} /* aligned(4): the insert/crunch paths move ranges as single ints
   * ((int*)start = *(int*)next). Without it a {short,short} struct only
   * promises 2-byte alignment, and solidsegs[] on the stack can land at
   * 2 mod 4 â€” an unaligned int access is an SH-2 address error (the R1
   * freeze of the first diff build). d32xr's cliprange_t carries the
   * same attribute for the same reason. */ __attribute__((aligned(4)))
segcliprange_t;

typedef struct
{
    seg_t  *seg;
    int     start, stop;        /* inclusive column range */
    int     actionbits;
    int     angle1;             /* BAM angle of v1 from the eye */
    /* late prep (16.16 / BAM) */
    int     scalefrac;
    int     scalestep;
    int     distance;           /* perpendicular distance, 16.16 */
    int     offset;             /* u at start col, 16.16 texels */
    int     centerangle;        /* BAM */
    /* early prep â€” all heights 16.16 relative to viewz */
    int     t_tex, b_tex, m_tex;
    int     t_top, t_bot;       /* upper/solid band */
    int     b_top, b_bot;       /* lower band */
    int     floorheight, ceilingheight;   /* front-sector flat bands */
    int     floornew, ceilingnew;         /* clip narrowing planes */
    int     t_peg_abs, b_peg_abs;         /* ABS world-z pegs (painter) */
    int     t_peg_top;                    /* ABS peg for the TOP-texture /
                                           * solid band paint. Kept separate
                                           * from t_peg_abs so masked-mid
                                           * walls (which share t_peg_abs)
                                           * keep their proven behaviour.
                                           * Vanilla r_segs.c: DONTPEGTOP ->
                                           * front ceiling (the default);
                                           * pegged -> back ceiling +
                                           * texheight; single-sided
                                           * DONTPEGBOTTOM -> front floor +
                                           * texheight. */
    short   fsec_i, side_i;
    /* back-sector index: the open-band owner for the end-of-frame flat
     * fill (see saturn_segs_render_half's post-pass). Garbage (= the
     * seg_emptysector delta) for solid walls, but those close the clip,
     * so the post-pass never reads it there. */
    short   bsec_i;
} segwall_t;

static segwall_t *segwalls;
int               segwalls_count;         /* non-static: HUD 'u' row */
int               saturn_seg_caphits;     /* seg_store_wall_range quota drops
                                           * (see 1711) — reset per frame   */
int               saturn_v1w_skipcols;    /* paint-skip (2026-09-07): columns
                                           * whose TOP-texture paint_wall_span
                                           * was skipped because the wall's
                                           * quads provably cover it */
int               saturn_v1w_skipdie;     /* preskip said commit, the walk
                                           * gate killed it — divergence */
int               saturn_v1w_commits;     /* walls that reached commit */
int               saturn_v1w_kills;       /* pieces skipped INSIDE commit
                                            (2026-09-08): safe-range
                                            corners + degenerate u. The
                                            S row showed 85 commits but
                                            only 21 cells stored — the
                                            32-cell table was never full
                                            (21 < 32), so the loss is
                                            upstream: these two continues
                                            plus Add's texture refuses
                                            (saturn_v1w_refuse, HUD B). */
/* saturn_v1w_cells lives in saturn_vdp1.c (incremented by Add) */
extern int        saturn_v1w_cells;
/* P5 Phase 2b (2026-09-10): -DSATURN_V1W_ALL relocates the VDP1 capture
 * into build (saturn_v1w_capture_pass — master, single-threaded, BEFORE
 * the halves dispatch). The walk-side capture machinery compiles out:
 * admission ladder, preskip, per-column gate/commit, per-wall map
 * unmark, and the skipdie tripwire (nothing left to disagree with).
 * The paint sites read the capture's FULL-FRAME map on BOTH cores (the
 * marks are complete before dispatch — the old cpu == 0 gate guarded
 * against a slave reading a half-written map, which can no longer
 * happen), and render_half stops resetting seg_mmid (capture owns it,
 * full width, pre-dispatch). Rollback = drop the define. */
#if defined(SATURN_VDP1_WALL) && !defined(SATURN_V1W_ALL)
#define SATURN_V1W_WALK_CAPTURE 1
#else
#define SATURN_V1W_WALK_CAPTURE 0
#endif
/* Dud commits (2026-09-08): commits that returned ZERO cells through
 * saturn_v1w_commit's silent entry return (xa>=xb || window<=0) — the
 * only uncounted exit (real-HW census: S=5515 commits=85 cells=21 with
 * B=0 refuse/kills). Zero-height upper/lower faces pass the v1w_alive
 * gates (which never check the face's world height) and evaporate here.
 * Pure CPU waste, not a coverage hole (preskip shares the same math). */
int               saturn_v1w_duds;
/* Stage 2a (2026-09-08): masked-mid software paints skipped because the
 * flush prescan converted their columns to an AddMid quad — the mid
 * counterpart of saturn_v1w_skipcols. Reset per frame with the rest. */
int               saturn_v1w_skipmid;
/* Stage 2a verify (temporary): master-half columns the mid prescan saw
 * with a recorded grate, and AddMid pieces it committed this frame. */
int               saturn_v1w_midcols;
int               saturn_v1w_midquads;
/* Reject census (2026-09-08, corrected S=515 read): commits=2/cells=3
 * per frame with B=0 (nothing killed) and j=40 — the capture is SMALL,
 * not lossy. One counter per wall/face rejection reason, first failing
 * test wins, to name the dominant blocker:
 *  0 no AC_TOPTEXTURE / AC_BOTTOMTEXTURE — BENIGN (faceless two-sided
 *       openings; single-sided walls DO set TOPTEXTURE, see :1497)
 *  1 t_tex / b_tex <= 0
 *  2 top face: no AC_SOLIDSIL | AC_NEWCEILING (not solid-occluding)
 *  3 AC_MIDTEXTURE (door/grate body)
 *  4 zero face window (equal heights) — BENIGN (nothing to draw)
 *  5 v1w_spr_ymax == NULL (sprite-ybuffer machinery absent)
 *  6 preskip: run < V1W_MIN_RUN columns
 *  7 gate: face span off-screen or < 8 px tall
 *  8 gate: outside the open clip band (vt < ce || vb > fl)
 *  9 gate: nearer masked-mid grate recorded (P5 Phase 3 re-scope:
 *       only a grate that stayed SOFTWARE vetoes; a quad'd grate's
 *       columns draw farther walls as quads under it)
 * 10 gate: nearer sprite / weapon rect covers the face rows
 * 11 preskip: v-bands don't cover the window (VBANDS cap / texh < 8)
 * 12 preskip: CanAdd fail (table headroom / texture / cached size)
 * 13 preskip: degenerate u piece (ub == ua)
 * --- stage 2a, masked-mid prescan (saturn_v1w_mid_prescan) ---
 * 14 gate_m: t_u clamp active (flush's ttm outside [1,30000])
 * 15 gate_m: texh < 8 (VDP1 texture cache minimum)
 * 16 gate_m: quad off-view or < 8 px tall
 * 17 gate_m: texture rect not inside the recorded band [y0, y1)
 * 18 gate_m: nearer sprite / weapon rect covers the rows
 * 19 prescan: run proof failed (degenerate u / safe-range corner) */
int               saturn_v1w_rj[20];
/* 2026-09-08 teal-wedge bisect: RETIRED — the teal/white/black wedge
 * chronology was boot flakiness (lost F1 / dead Kronos process) + the
 * #if 0 that amputated saturn_open_iwad's candidate loop; the BYPASS=1
 * control booted E1M1 clean in Kronos. Stage-3a restored. */
#define SATURN_3A_BYPASS 0
/* Preskip v2 (2026-09-08): per-column paint-skip map. preskip marks the
 * columns of every sub-run it PROVED commit will add (the walk's own
 * gate per column, then the band/piece/CanAdd/safe-corner checks per
 * run — verbatim commit math); the paint sites skip exactly those
 * columns and the committed quads repaint them on the VDP1. A run that
 * fails any run-level check is NOT marked (software paints it, the
 * quad still overdraws — harmless). v1w_skip semantics: 0 = nothing
 * proven (paint all), 1 = the WHOLE face proven (map is all-ones over
 * [xs,xe]), 2 = partial (map marks the proven runs). Unmarking is a full
 * clear — the bitmap is 80 bytes, so 160 bytes/wall across ~53 walls is
 * ~8KB of memset per frame (~0.03ms); the span-queue version of this
 * cost 264 bytes of BSS the map file said we did not have. */
#define V1W_MAP_W      320
#define V1W_MAP_BYTES  ((V1W_MAP_W + 7) / 8)    /* BITMAP — .bss is tight:
                                            the byte map overflowed the
                                            0x060C0000 wall by 0x180 */
#define V1W_MAP_SPANS  32       /* preskip run-scan stack arrays only */
/* face 0 = top wall, 1 = bottom wall, 2 = masked midtexture (stage 2a:
 * the flush's mid prescan marks the columns whose grate paint it
 * converted to an AddMid quad) */
static byte       v1w_skimap[3][V1W_MAP_BYTES];
/* column x <-> bit (MSB-first within each byte) */
#define V1W_MAP_TEST(f, x) \
    (v1w_skimap[f][x >> 3] & (byte)(0x80u >> (x & 7)))
/* set/clear the bits of column range [a, b] */
static void v1w_map_bits(int face, int a, int b, int set)
{
    int ba = a >> 3, bb = b >> 3, i;
    byte ma = (byte)(0xFFu >> (a & 7));
    byte mb = (byte)(0xFFu << (7 - (b & 7)));

    if (ba == bb)
    {
        if (set) v1w_skimap[face][ba] |= ma & mb;
        else     v1w_skimap[face][ba] &= (byte)~(ma & mb);
        return;
    }
    if (set) v1w_skimap[face][ba] |= ma;
    else     v1w_skimap[face][ba] &= (byte)~ma;
    for (i = ba + 1; i < bb; i++)
        v1w_skimap[face][i] = set ? 0xFFu : 0u;
    if (set) v1w_skimap[face][bb] |= mb;
    else     v1w_skimap[face][bb] &= (byte)~mb;
}
/* Retract one face's marked columns. Called per wall on the master,
 * BEFORE the setup ladder — walls rejected by the ladder (or after the
 * quad table went full) never call preskip, and a stale mark would
 * skip a paint no quad covers. */
static void v1w_map_unmark(int face)
{
    memset(v1w_skimap[face], 0, V1W_MAP_BYTES);
}
/* BSP traversal timing (2026-09-08 STAGE 1): the post-VDP1 frame is
 * walk + command assembly, so the walk needs its own number instead of
 * hiding inside T (whose frt_all_acc folds across vbls — phase junk).
 * saturn_frt_bsp = seg_render_bsp_node window, TCNT ticks >>8
 * (1 unit = 0.198 ms, ~84 units/vbl); saturn_bsp_vbls = vblanks the
 * walk spanned — if >0 the window crossed a vblank edge and the tick
 * delta only holds the wrapped tail (use vbls as coarse truth). */
int               saturn_frt_bsp;
int               saturn_bsp_vbls;
/* Occlusion guard (2026-09-07): VDP1 quads composite ABOVE every NBG0
 * pixel, so anything NEARER that paints into the framebuffer over a
 * captured wall's face must veto the capture: world sprites, the
 * player weapon, the nearest masked-mid grate. spr_vis[] is fully
 * collected and sorted by the time saturn_segs_build returns
 * (seg_collect_sector_sprites runs at the BSP leaves), so the guard is
 * three per-column arrays filled once per frame on the master:
 *   v1w_spr_ymax[x] = LARGEST sprite scale covering column x (same
 *     PROJ-based 1/depth units as the walk's scalefrac; 0 = none),
 *   v1w_spr_top/bot[x] = the UNION of all sprites' screen rows there.
 * Kill test = ymax > wall scale AND union rows overlap the face rows.
 * The sprite clipper's tie-break (seg_spr_behind) is ignored: a
 * straddling wall/sprite pair always vetoes — conservative, a lost
 * capture only costs the software paint it falls back to. Union rows
 * can over-kill (the max-scale sprite may sit outside the face rows a
 * nearer smaller one covers) — same trade. */
static int   *v1w_spr_ymax;
static short *v1w_spr_top;
static short *v1w_spr_bot;

/* Round 10 (2026-09-04): vanilla masked mid textures. A mid texture
 * whose composite has transparent texels (or that doesn't cover the
 * open band) is NOT the solid grate the walk used to paint — vanilla
 * draws those ONCE from the peg with the opening left open
 * (R_RenderMaskedSegRange). The walk can't paint it there: everything
 * behind is still unrendered. So the piece is RECORDED per column and
 * painted at the end of the open-band fill (after every wall and the
 * fill's own flats), holes skipped. The walk is near->far, so the
 * first record at a column is the nearest grate; a second one counts
 * (saturn_mmid_skip) and is dropped — vanilla would paint both, we
 * paint one (documented divergence, the common fence case). */
typedef struct
{
    segwall_t *w;                   /* NULL = no deferred grate here   */
    side_t    *side;
    byte       nside;
    byte       y0, y1;              /* open band this grate covers     */
} seg_mmid_t;

static seg_mmid_t seg_mmid[SCREEN_W + 1];   /* [+1]: the shared reset loop
                         * runs x <= SCREEN_W inclusive (the clip arrays'
                         * bound); the walk/fill only touch [0, SCREEN_W) */
static unsigned int seg_mmid_guard[4];      /* round-41 canary fence */

/* Per-column render state (2026-09-06 OCRAM feature): 1926 B, parked in
 * the SH-2's 2KB on-chip RAM (cache data array, TW mode,
 * 0xC0000000-0xC00007FF) when the boot probe (-DSATURN_OCRAM_PROBE,
 * d_main.c) verified the write; otherwise this static WRAM twin.
 * Master-only render path (saturn_slave.c) — the OCRAM is per-SH2, so
 * there is no coherency surface. On-chip = 0 wait states and zero
 * operand-cache pressure for the wall walk + open-band fill. */
typedef struct {
    unsigned short clipbounds[SCREEN_W + 1];    /* 642 B */
    unsigned short lastsec[SCREEN_W + 1];       /* 642 B */
    byte nearlo[SCREEN_W + 1];                  /* 321 B */
    byte nearhi[SCREEN_W + 1];                  /* 321 B */
} sat_colstate_t;

static sat_colstate_t sat_colstate_wram;
static sat_colstate_t *sat_colstate = &sat_colstate_wram;

void Saturn_Ocram_State_Init(void)
{
#if defined(SATURN_OCRAM_PROBE)
    {
        extern volatile int saturn_oc_ok;

        if (saturn_oc_ok)
            sat_colstate = (sat_colstate_t *)0xC0000000u;
    }
#endif
}

/* 2026-09-06 RETIRED here: the reciprocal-division LUT (divtab) for the
 * masked-midtexture path. The math was verified exhaustively
 * (sim/make_divtab.py: 336k values, max |dt_u| 0.07%), but the SH-2 has
 * no shad/shld and no CLZ, so gcc -m2 compiles every variable shift in
 * the normalize+shift into a libgcc __lshrsi3/__ashlsi3 CALL LOOP —
 * the three calls ate the entire ~40-70-cycle software-divide saving.
 * The pipelined DIVU idiom (see the masked pass below) replaces it with
 * zero memory and bit-exact results. The generator + tables stay in
 * sim/make_divtab.py + saturn_divtab.h for any future site whose shifts
 * can be made constant. */

int               saturn_mmid_skip = 0;   /* non-static: HUD diag    */
int               saturn_mmid_paint = 0;  /* non-static: HUD diag    */
/* 2026-09-06 SW1COMP face probe (user shot: E1M1 lift switch face dark).
 * The face is linedef 136's front lower texture on the sector-36 lift
 * box (64x64 at x 256..320, y -3136..-3072), seen from the north. The
 * sim (sim_segw) builds and paints it at every nearby pose, so the
 * disc gets counters instead: how many segwalls carry the SW1COMP
 * bottom texture (master walk), whether the preload's TexData got
 * pixels for it, and the master's own paint count at the
 * AC_BOTTOMTEXTURE site. P row = (built<<9)|(ok<<8)|paint. */
int               saturn_sw1_built = 0;   /* non-static: HUD diag    */
int               saturn_sw1_ok    = 0;   /* TexData non-NULL        */
int               saturn_sw1_paint = 0;   /* master-half paint count */
static int        saturn_sw1_tex   = -1;
/* round 11b fill-side capture (mmid_flush): raw walk-end lastsec and
 * the light of the sector the fill actually used for that column */
volatile int      saturn_mm_fill_sec   = -1;
volatile int      saturn_mm_fill_light = -1;

/* camera, set once per frame by saturn_segs_build (master) and read by
 * both cores after the dispatch purge */
static fixed_t  seg_viewx, seg_viewy, seg_viewz;
static angle_t  seg_viewangle;
static fixed_t  seg_viewcos, seg_viewsin;   /* sprite collection */
/* player sector: default open-band owner for columns no wall covers */
static short    seg_viewsec_i;

/* final-fill safety net census (2026-09-02): open-band fills whose
 * lastsec owner was out of [0,numsectors) and got folded back to the
 * view sector. Nonzero = something still writes wild bsec_i values.
 * Production-safe (one increment per bad column). */
volatile int     saturn_ls_bad = 0;

/* BISECT round 13 probes (slave-drawn into the fb via Saturn_HudNum):
 * the seg that last CLOSED clip rows at col 28 without painting them,
 * and the seg that last painted the col-28 wall span. */
volatile int saturn_dbg_close_i    = -1;
volatile int saturn_dbg_close_ab   = 0;
volatile int saturn_dbg_close_ttex = 0;
volatile int saturn_dbg_close_tt   = 0;
volatile int saturn_dbg_close_tb   = 0;
volatile int saturn_dbg_near_i     = -1;
volatile int saturn_dbg_near_span  = 0;
/* round 14: seg 7's clip state at col 28 on entry, and its scale2. */
volatile int saturn_dbg_clip_before = 0;
volatile int saturn_dbg_scale2_28   = 0;
/* Round 27c (2026-09-02): per-frame counters â€” how many segwall pieces
 * PROCESSED the probe columns (28 = red-bar pose, 60 = black-slab
 * pose). 0 pieces at a column = nothing narrowed, nothing painted,
 * the fill's lastsec flats own it. Reset with the clipbounds. */
volatile int saturn_dbg_w28 = 0;
volatile int saturn_dbg_w60 = 0;
/* 2026-09-03 photos #158/#159: w28/w60 sampled only 3/5 pieces at two
 * columns of one pose — too sparse to bound the pair count. This is
 * the TOTAL (piece, column) pairs the master walked this frame; with
 * K (glue vbl) it names the true per-pair cost. Master-side only. */
int saturn_dbg_pairs = 0;

/*----- things/sprites (2026-09-02, task #16) -----
 * The one level-rendering feature the port never had: d32xr's
 * R_SpritePrep is stubbed empty in d32xr_stubs.c, so no thing ever
 * drew. This is the R_DrawVisSprite / R_ClipVisSprite pipeline
 * (r_phase8.c) collapsed into this file's data flow:
 *
 *   seg_collect_sector_sprites (MASTER, from seg_subsector during the
 *   BSP walk, d32xr's R_Subsector validcount scheme): for each visited
 *   sector, project every thing on its thinglist with the vanilla/
 *   ManagedDoom math (tz = forward, tx = lateral right,
 *   scale = (PROJ<<16)/tz, rotation via R_PointToAngle), keep <=
 *   spr_vis_max, then sort far-to-near once after the walk
 *   (seg_sort_sprites at the end of saturn_segs_build).
 *
 *   seg_draw_sprites (EACH CORE over its column range, after the
 *   wall/flat flush): per sprite, clip per column against every
 *   segwall piece NEARER than the sprite (the same scale-at-ends +
 *   R_SegBehindPoint depth test d32xr uses): solid/mid pieces cull the
 *   column, two-sided pieces tighten the opening with their
 *   ceilingnew/floornew planes; then paint the patch columns straight
 *   into I_VideoBuffer (0 texel = transparent), cmap-shaded like the
 *   walls (FF_FULLBRIGHT -> shade 0).
 *
 * Sprites draw AFTER every wall row is on screen, so they sit in
 * front of the flats of their own sector but are clipped by nearer
 * geometry through the per-column opening. d32xr draws masked
 * (mid-texture) segs OVER sprites â€” this renderer CLOSES the clip at
 * mid textures instead (doors occlude everything farther), so sprites
 * behind a closed door are culled too: consistent with the walls. */
/* d32xr's MAXVISSPRITES is MAXWALLCMDS (= our SEGW_MAX, 165). The
 * array lives in WRAM at that size (the zone has no headroom for the
 * extra ~5 KB); the zone fallback keeps the historical 48-entry size
 * (the pre-2026-09-06 collection cap). */
#define SPRVIS_MAX        48
#define SPRVIS_MAX_HRAM   165

typedef struct
{
    int     dist;               /* forward distance tz, 16.16 (sort) */
    int     yscale;             /* screen px per map unit, 16.16     */
    int     ustep;              /* texels per screen px, 16.16
                                 * (= FixedDiv(FRACUNIT, scale_h),
                                 * texel-X only)                     */
    int     vstep;              /* texel-Y per screen row, 16.16
                                 * (= FixedDiv(FRACUNIT, yscale)) —
                                 * round-32: the v-loop was reusing
                                 * ustep (the PROJ_X=80 horizontal
                                 * reciprocal, 2x too large at the
                                 * half-width split), so every sprite
                                 * rendered HALF height, anchored at
                                 * its top = floating above the ground */
    int     gzt;                /* world z of the patch TOP, fixed   */
    int     xl;                 /* screen x of the sprite's LEFT edge
                                 * (unclamped, may be negative)      */
    int     gx, gy;             /* thing's map position (behind test) */
    short   x1, x2;             /* clipped inclusive column range    */
    short   scache;             /* index into Saturn_SpriteData[]    */
    short   w, h;               /* patch size                        */
    short   sh;                 /* COLORMAP shade row                */
    byte    flip;
} sprvis_t;

static sprvis_t *spr_vis;
static int       spr_vis_count;
static int       spr_vis_max = SPRVIS_MAX;
static short   (*spr_ctop)[SCREEN_W + 1];   /* per-cpu clip planes   */
static short   (*spr_cbot)[SCREEN_W + 1];

#ifdef SATURN_SPRCLIP_DIAG
/* Round-33b sprite-clip probe (diagnostic builds only). For the
 * FARTHEST sprite that reaches each core's half, seg_draw_sprites
 * records which wall piece (if any) last lowered obot at the sprite's
 * centre column, the clip row it set, and where the UNCLIPPED
 * projection puts the sprite's base. cpu 0 paints four glyph rows at
 * y=8/28/48/68 (free in-game; the X/V/W census owns 148/168/208):
 *   M J999 Y999 B999   master half: J = segwalls index of the
 *                      bottom-clipping wall (999 = none), Y = the row
 *                      it set (999 = none), B = unclipped base row
 *   S J999 Y999 B999   slave half, same
 *   F99999 N999        F = that wall's floornew (int16, eye-relative
 *                      whole units), N = walls passing the depth test
 *   G99999 D99999      G = (gzt - viewz)>>16 (int16), D = dist>>16
 * A white marker line is also painted at B-1 across the watched
 * sprite's columns so a screenshot shows projection-vs-clip directly.
 * Watch rule (round-33c): the data describes the FARTHEST sprite of
 * the half that is still VISIBLE at its own centre column — an
 * occluded sprite's clip numbers say nothing about the float.
 * Sentinels: J999 = no wall clipped the base; Y999 = no BOTTOMSIL row;
 * Y998 = a SOLIDSIL/MIDTEXTURE wall's full block; B999 = invalid.
 * The capture variables are non-static: d_main.c republishes the four
 * glyph rows every main-loop iteration (StageTextHi writes the VDP2
 * display pages DIRECTLY, so any glyph painted from inside the render
 * is erased by the next I_FinishUpdate copy_back — the X/V/W census
 * only survives because it is painted after the flip). */
volatile int sprdiag_seen[2];
volatile int sprdiag_cx[2];
volatile int sd_j[2], sd_yy[2], sd_base[2], sd_dist[2];
volatile int sd_n[2], sd_fl[2], sd_gz[2];
volatile int sd_gap[2];
/* round-33f cache vitals of the watched (most-erased) sprite:
 * K = all-zero bottom rows of its converted pixel buffer,
 * G = its columns whose post chain died on the pend guard,
 * P = the lump length the conversion bounded its chains with.
 * K>0 = cache truncated/clobbered; G=w = plen short; both 0 with a
 * float = the bug is NOT in the sprite cache. */
volatile int sd_botz[2], sd_guard[2], sd_plen[2];
volatile int sd_span[2], sd_top[2], sd_hi[2], sd_lo[2];
/* vanish hunt (round-34): watch 2 (max-span) commits the erasing wall
 * too — J = segwalls[] index, C = clip class (1 SOLID/MID, 2 TOPSIL,
 * 3 BOTTOMSIL), S = that wall's per-column scale at the sprite's
 * centre column, X = the sprite's own yscale (both 16.16>>6). S < X
 * at a fully-erased sprite = the occluder was FARTHER than the thing
 * it erased (over-occlusion). */
volatile int sd_sc[2], sd_ys[2], sd_cl[2];
volatile int sd_espan[2], sd_eok[2];
/* corpse hunt (round-34b): watch 3 commits ONLY sprites the clip +
 * paint left with ZERO pixels (base still on screen) — watch 2 fires
 * on live sprites too and can mask the vanished thing with a bigger
 * live one. sd_espan = max span among erased sprites this frame,
 * sd_eok = an erased commit happened (d_main prints the row with an
 * 'e' prefix so the two watches can never be confused). */
/* round-33g: the sd_* vitals cross cores (cpu1 = the slave half, where
 * the E1M1 floaters live). Cached stores sit in the WRITING core's OC
 * and the purge is a no-op, so every sd_* access in the sprite pass
 * goes through the UNCACHED WRAM mirror — ~12 uncached 32-bit ops per
 * frame, negligible next to the fill. d_main reads them uncached too. */
#define SD_UNC(p) (*(volatile int *)(0x20000000u | (unsigned)(p)))
#endif

extern degenmobj_t mobjhead;
extern byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs, int *tofs);
extern byte *Saturn_SpriteData(int s);
extern byte *Saturn_SpriteDataNoLoad(int s);
extern int Saturn_SpriteBotz(int s);
extern int Saturn_SpriteGuard(int s);
extern int Saturn_SpritePlen(int s);
extern byte *Saturn_Colormap(void);
extern byte  Saturn_LightRow(int lightlevel, int dist);

/* R_SegBehindPoint (r_phase8.c verbatim): is the sprite's map point on
 * the back side of this wall's seg? Coordinates are WHOLE map units. */
static int seg_spr_behind(segwall_t *w, fixed_t gx, fixed_t gy)
{
    seg_t       *seg = w->seg;
    mapvertex_t *v1 = &vertexes[SEG_UNPACK_V1(seg)];
    mapvertex_t *v2 = &vertexes[SEG_UNPACK_V2(seg)];
    int x1 = v1->x, y1 = v1->y;
    int sdx = v2->x - x1, sdy = v2->y - y1;
    int dx = (gx >> FRACBITS) - x1;
    int dy = (gy >> FRACBITS) - y1;

    dx *= sdy;
    sdx *= dy;
    return (sdx < dx);
}

/*----- collection (master only, once per frame) -----
 * d32xr model (r_phase1.c R_Subsector + validcount): sprites are
 * collected ONLY for sectors whose subsectors the BSP walk actually
 * visits, deduped per frame through the level's validcount[] array
 * (p_setup.c allocates numlines+1 entries — always > numsectors).
 * The previous implementation walked the GLOBAL mobj list and stopped
 * at SPRVIS_MAX collected entries in list order — on a busy UV E1M5
 * the first 48 things that projected into the frustum (most of them
 * behind walls, from anywhere in the level) filled the array, and
 * every visible monster listed after them was never drawn: the
 * skill- and angle-dependent sprite vanish (2026-09-06). */
static void seg_add_mobj_sprite(mobj_t *mo)
{
        sprvis_t *sv = &spr_vis[spr_vis_count];
        const state_t *st;
        const spritedef_t *sprdef;
        spriteframe_t *sf;
        fixed_t dx, dy, tz, scale, scale_h, tx, gzt;
        unsigned lumpv;
        int lump, scache, flip, w, h, lofs, tofs;
        int x1, x2, rot;

        if (spr_vis_count >= spr_vis_max)
            return;
        if (mo == players[0].mo)
            return;
        if (!mo->state || (unsigned)mo->state >= NUMSTATES)
            return;

        st = &states[mo->state];
        if (st->sprite < 0 || st->sprite >= NUMSPRITES)
            return;
        sprdef = &sprites[st->sprite];
        if (sprdef->numframes <= 0)
            return;
        if ((st->frame & FF_FRAMEMASK) >= sprdef->numframes)
            return;
        sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];

        /*---- project the thing's origin ----*/
        dx = mo->x - seg_viewx;
        dy = mo->y - seg_viewy;
        {
            fixed_t gxt = FixedMul(dx, seg_viewcos);
            fixed_t gyt = -FixedMul(dy, seg_viewsin);

            tz = gxt - gyt;                 /* forward distance */
        }
        if (tz < (4 << FRACBITS))
            return;
        /* Full-width render (2026-09-05): PROJ and PROJ_X coincide at
         * 160 again, as in vanilla/d32xr. (In the half-width era PROJ
         * was the VERTICAL focal and the HORIZONTAL scale had to be
         * PROJ_X = 80; using PROJ there drew every sprite 2x too wide.) */
        scale = FixedDiv(PROJ << FRACBITS, tz);
        scale_h = FixedDiv(PROJ_X << FRACBITS, tz);
        {
            fixed_t gxt = -FixedMul(dx, seg_viewsin);
            fixed_t gyt = FixedMul(dy, seg_viewcos);

            tx = -(gyt + gxt);              /* lateral right */
        }
        if (tx > (tz << 2) || tx < -(tz << 2))
            return;

        /*---- pick the patch (ManagedDoom's rotation formula) ----
         * spriteframes[i].lump is an OFFSET into spritelumps[]: entry
         * [0] of a single-rotation frame carries SL_SINGLESIDED; a
         * rotated frame packs 8 consecutive entries (d32xr r_data.c
         * pack loop). VINT is a short, so the flag bits make it
         * negative â€” read through unsigned short first. */
        {
            VINT *sl = &spritelumps[sf->lump];

            lumpv = (unsigned short)sl[0];
            if (!(lumpv & SL_SINGLESIDED))
            {
                angle_t ang = R_PointToAngle(seg_viewx, seg_viewy,
                                             mo->x, mo->y);

                rot = (int)(((ang - mo->angle
                              + (unsigned)(ANG45 / 2) * 9) >> 29) & 7);
                lumpv = (unsigned short)sl[rot];
            }
        }
        flip = (lumpv & SL_FLIPPED) ? 1 : 0;
        lump = (int)(lumpv & SL_LUMPMASK);
        if (lump < firstsprite || lump >= firstsprite + numsprites)
            return;
        scache = lump - firstsprite;
        if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
            return;
        if (w < 1 || h < 1)
            return;

        /*---- screen edges (ManagedDoom ProjectSprite) ----
         * HORIZONTAL: scale_h (PROJ_X plane distance). Vertical rows
         * keep sv->yscale (PROJ). */
        tx -= lofs << FRACBITS;
        x1 = (((SCREEN_W / 2) << FRACBITS) + FixedMul(tx, scale_h))
             >> FRACBITS;
        if (x1 > SCREEN_W)
            return;
        tx += w << FRACBITS;
        x2 = ((((SCREEN_W / 2) << FRACBITS) + FixedMul(tx, scale_h))
              >> FRACBITS) - 1;
        if (x2 < 0)
            return;

        sv->x1 = x1 < 0 ? 0 : x1;
        sv->x2 = x2 >= SCREEN_W ? SCREEN_W - 1 : x2;
        if (sv->x1 > sv->x2)
            return;

        sv->xl = x1;
        sv->gx = mo->x;
        sv->gy = mo->y;
        sv->scache = (short)scache;
        sv->w = (short)w;
        sv->h = (short)h;
        sv->flip = (byte)flip;
        sv->ustep = FixedDiv(FRACUNIT, scale_h);   /* texel-x stepping */
        sv->vstep = FixedDiv(FRACUNIT, scale);     /* texel-y stepping:
                                                    * vanilla dc_iscale,
                                                    * vertical focal PROJ */
        sv->yscale = scale;
        sv->gzt = mo->z + (tofs << FRACBITS);
        sv->dist = tz;

        /* COLORMAP row: d32xr (32X) lighting, same model as the walls
         * and flats now use (Saturn_LightRow in saturn_render.c) â€”
         * raw sector light held out to ~167 units, fading to lightmin
         * by ~320. FF_FULLBRIGHT things (explosions, muzzles) ignore
         * it. */
        {
            sector_t *sec = &sectors[mo->subsector->sector];
            int      dmap = tz >> FRACBITS;
            int      sh;

            sh = Saturn_LightRow(sec->lightlevel, dmap);

            if (st->frame & FF_FULLBRIGHT)
                sh = 0;
            sv->sh = (short)sh;
#ifdef SATURN_SH_SPLIT
            if (sv->x1 < SCREEN_W / 2)
                sv->sh = 0;     /* left half fullbright (round 22) */
#endif
        }
        spr_vis_count++;
}

/* Per-frame dedup stamp + sector thinglist walk (d32xr R_Subsector's
 * validcount scheme). Called from seg_subsector during the BSP walk. */
static void seg_collect_sector_sprites(int secnum)
{
    mobj_t   *mo;
    sector_t *sec = &sectors[secnum];

    if (!spriteframes || !sprites || !validcount)
        return;
    if (validcount[secnum + 1] == validcount[0])
        return;                 /* sector already processed this frame */
    validcount[secnum + 1] = validcount[0];

    for (mo = (mobj_t *)sec->thinglist; mo; mo = mo->snext)
        seg_add_mobj_sprite(mo);
}

/*----- player weapon (psprite) -----
 * d32xr draws the on-screen gun through R_PrepPSprite (r_phase3.c) +
 * R_DrawPSprites (r_phase8.c) — neither file is compiled here, so the
 * player has never seen their weapon or its muzzle flash (firing
 * logic in p_pspr.c/p_shoot.c has always run; only the view was
 * missing). This is the screen-space replacement, called from
 * Saturn_G_Display after the world render: psprites sit at yscale =
 * FRACUNIT, so all the FixedDiv projection collapses to
 *   x1 = (psp->sx >> FRACBITS) - patch.leftoffset
 *        (centerXFrac - 160*weaponXScale cancels at width 320)
 *   top = 2 + (psp->sy >> FRACBITS) - patch.topoffset
 *        (texmid = BASEYCENTER(100) + topoffset - weaponYpos(6) - sy;
 *         centerYFrac = 96 in the 192-row window).
 * PSprites are always rotation 0, never flipped, and are NOT clipped
 * by walls — they paint over everything below the view's top edge. */
/* Screen rect actually painted this frame (union over weapon + flash),
 * published for the NEXT frame's VDP1 capture guard: the weapon draws
 * after the wall walk, so the capture must treat it as nearer
 * everywhere. saturn_gun_x1 < saturn_gun_x0 = nothing published. */
int saturn_gun_x0 = 9999, saturn_gun_x1 = -1;
int saturn_gun_y0 = 9999, saturn_gun_y1 = -1;
void Saturn_PSprite_Draw(void)
{
    player_t *p = &players[consoleplayer];
    byte     *cmap = Saturn_Colormap();
    int       i, x, r;

    if (!spriteframes || !sprites || !p || !p->mo)
        return;

    saturn_gun_x0 = 9999;                   /* fresh union each frame */
    saturn_gun_x1 = -1;
    saturn_gun_y0 = 9999;
    saturn_gun_y1 = -1;

    for (i = 0; i < NUMPSPRITES; i++)       /* weapon, then flash */
    {
        pspdef_t           *psp = &p->psprites[i];
        const state_t      *st;
        const spritedef_t  *sprdef;
        spriteframe_t      *sf;
        unsigned            lumpv;
        int                 lump, scache, w, h, lofs, tofs;
        int                 x1, x2, xs, xe, y0, r0, r1, sh;
        byte               *pix, *fb;

        if (!psp->state || (unsigned)psp->state >= NUMSTATES)                    /* S_NULL = inactive */
            continue;
        st = &states[psp->state];
        if (st->sprite < 0 || st->sprite >= NUMSPRITES)
            continue;
        sprdef = &sprites[st->sprite];
        if (sprdef->numframes <= 0)
            continue;
        if ((st->frame & FF_FRAMEMASK) >= sprdef->numframes)
            continue;
        sf = &spriteframes[sprdef->firstframe + (st->frame & FF_FRAMEMASK)];

        lumpv = (unsigned short)spritelumps[sf->lump];   /* 1-D table:
                                    * rot-0 entry carries the SL_ flags */
        lump = (int)(lumpv & SL_LUMPMASK);
        if (lump < firstsprite || lump >= firstsprite + numsprites)
            continue;
        scache = lump - firstsprite;
        if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
            continue;
        if (w < 1 || h < 1)
            continue;
        pix = Saturn_SpriteData(scache);    /* master: lazy convert OK */
        if (!pix)
            continue;

        x1 = (psp->sx >> FRACBITS) - lofs;
        x2 = x1 + w - 1;
        xs = x1 < 0 ? 0 : x1;
        xe = x2 >= SCREEN_W ? SCREEN_W - 1 : x2;
        if (xs > xe)
            continue;

        y0 = 2 + (psp->sy >> FRACBITS) - tofs;
        r0 = 0;
        r1 = h - 1;
        if (y0 + r0 < VIEW_TOP) r0 = VIEW_TOP - y0;
        if (y0 + r1 >= VIEW_BOT) r1 = VIEW_BOT - 1 - y0;
        if (r0 > r1)
            continue;

        /* Publish the painted rect for the next frame's capture guard
         * (pad 4 px each way absorbs the weapon bob between frames). */
        {
            int gx0 = xs - 4;
            int gx1 = xe + 4;
            int gy0 = y0 + r0 - 4;
            int gy1 = y0 + r1 + 4;
            if (saturn_gun_x1 < saturn_gun_x0)
            {
                saturn_gun_x0 = gx0;
                saturn_gun_x1 = gx1;
                saturn_gun_y0 = gy0;
                saturn_gun_y1 = gy1;
            }
            else
            {
                if (gx0 < saturn_gun_x0) saturn_gun_x0 = gx0;
                if (gx1 > saturn_gun_x1) saturn_gun_x1 = gx1;
                if (gy0 < saturn_gun_y0) saturn_gun_y0 = gy0;
                if (gy1 > saturn_gun_y1) saturn_gun_y1 = gy1;
            }
        }

        /* stage 3a (2026-09-08): the prep pass drew this psprite as a
         * VDP1 quad (head of the quad table = drawn last/on top). The
         * software paint below would land on NBG0 UNDER the VDP1
         * layer — skip it, but keep the rect publication above.
         * BUILD C: gated out with the whole stage-3a surface. */
#if !SATURN_3A_BYPASS
        if (Saturn_Vdp1Quad_PspMarked())
            continue;
#endif

        sh = Saturn_LightRow(
            sectors[p->mo->subsector->sector].lightlevel, 0);
        if (st->frame & FF_FULLBRIGHT)
            sh = 0;                         /* muzzle flash */

        for (x = xs; x <= xe; x++)
        {
            fb = I_VideoBuffer + (y0 + r0) * SCREEN_W + x;
            for (r = r0; r <= r1; r++, fb += SCREEN_W)
            {
                byte t = pix[r * w + (x - x1)];

                if (t)
                    fb[0] = cmap ? cmap[(sh << 8) + t] : t;
            }
        }
    }
}

/* 2026-09-08 psprite-invisibility forensics (TEMPORARY, rev 2): the
 * weapon is absent from every Kronos snap of BOTH the control and the
 * stage-3a build while the P row proves the psprite quads never land in
 * the v1q table — yet every static link (WEAPONTOP 32, PISGA0 tofs -106
 * -> y0=140, VIEW_BOT 192, TEXS class 3968/4096) checks out. Rev 1's
 * packing was useless on the 5-digit census cell (reasons 1/2/7/9 all
 * displayed "0", indistinguishable from a stale EDSR probe). Rev 2
 * packs EVERY outcome as a distinct nonzero 5-digit code — pure locals
 * + existing tables (the BSS ceiling forbids new statics).
 *   9xxxx = weapon slot refuses (bug lives here)
 *   8xxxx = flash slot ROLLS THE PAIR BACK (prep reasons 4-6)
 *   81000-83999 = flash merely SKIPS (weapon still converts)
 *   79999 = pair stored (PspMarked) — contradicts the P row, shouldn't
 *           display while P < 3
 *   69999 = no player/sprite tables
 *   else  = weapon WOULD PAINT: y0*1000+sy truncated to 5 digits
 *           (y0=140,sy=32 -> "40032"; y0 = v/1000 or v/1000+100)
 * Weapon/flash slot codes, reason r in the thousands digit + detail:
 *   r1 =91001/81001 no state          r2 =92xxx st->sprite & 0x3FF
 *   r3 =93xxx numframes-lownibble<<4 | frame&0xF
 *   r4 =94xxx lump-window: v=lump-firstsprite; v>=0 ? v&0x3FF
 *          : 0x400+(-v&0x1FF)  (>=0x400 digit means NEGATIVE delta)
 *   r5 =95xxx scache & 0x3FF (SpritePatch failed)
 *   r6 =96xxx w & 0x3FF (w<1 after SpritePatch)
 *   r7 =97xxx xA & 0x3FF (VDP1 coords sanity)
 *   r8 =98xxx Saturn_Vdp1Wall_Count() (Spr_Add/table refuse)
 *   r9 =99001/89001 Saturn_SpriteData NULL
 * Prep semantics: r1-r3 are per-slot SKIPS (other slot still converts);
 * r4-r9 set ok=0 and ROLL THE PAIR BACK. r4-r6 are prep refusals too —
 * on the WEAPON slot they both refuse prep AND the software paint. */
static int saturn_gun_chain(pspdef_t *pp, int base, int *ok)
{
    const state_t      *st;
    const spritedef_t  *sprdef;
    spriteframe_t      *sf;
    unsigned            lumpv;
    int                 lump, scache, w, h, lofs, tofs;
    int                 sy, y0, r0, r1;

    *ok = 0;
    if (!pp->state)
        return base + 1000;
    st = &states[pp->state];
    if (st->sprite < 0 || st->sprite >= NUMSPRITES)
        return base + 2000 + (st->sprite & 0x3FF);
    sprdef = &sprites[st->sprite];
    if (sprdef->numframes <= 0
        || (st->frame & FF_FRAMEMASK) >= sprdef->numframes)
        return base + 3000 + ((sprdef->numframes & 0xF) << 4)
               + (st->frame & FF_FRAMEMASK & 0xF);
    sf = &spriteframes[sprdef->firstframe
                       + (st->frame & FF_FRAMEMASK)];
    lumpv = (unsigned short)spritelumps[sf->lump];
    lump = (int)(lumpv & SL_LUMPMASK);
    if (lump < firstsprite || lump >= firstsprite + numsprites)
    {
        int v = lump - firstsprite;

        return base + 4000 + (v >= 0 ? (v & 0x3FF)
                              : 0x400 + ((-v) & 0x1FF));
    }
    scache = lump - firstsprite;
    if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
        return base + 5000 + (scache & 0x3FF);
    if (w < 1 || h < 1)
        return base + 6000 + (w & 0x3FF);
    sy = pp->sy >> FRACBITS;
    y0 = 2 + sy - tofs;
    r0 = 0;
    r1 = h - 1;
    if (y0 + r0 < VIEW_TOP) r0 = VIEW_TOP - y0;
    if (y0 + r1 >= VIEW_BOT) r1 = VIEW_BOT - 1 - y0;
    if (r0 > r1)
        return base + 7000 + (y0 & 0x3FF);
    /* prep's coords sanity + Spr_Add preconditions */
    {
        extern int   Saturn_Vdp1Wall_Count(void);
        extern int   Saturn_Vdp1Wall_Cap(void);
        extern byte *Saturn_SpriteData(int s);
        int          pw, xA, xB;
        byte        *pix;

        pw = (w + 7) & ~7;
        xA = (pp->sx >> FRACBITS) - lofs;
        xB = xA + pw - 1;
        if (xA < -500 || xA > 1023 || xB < -500 || xB > 1023
            || y0 + r0 < -500 || y0 + r0 > 1023
            || y0 + r1 + 1 < -500 || y0 + r1 + 1 > 1023)
            return base + 7000 + (xA & 0x3FF);
        if (Saturn_Vdp1Wall_Count() >= Saturn_Vdp1Wall_Cap()
            || w > 128 || h > 255 || pw * h > 128 * 128)
            return base + 8000 + Saturn_Vdp1Wall_Count();
        pix = Saturn_SpriteData(scache);
        if (!pix)
            return base + 9001;
    }
    *ok = 1;
    return sy + y0 * 1000;
}

/* Display-time verdict for the h row — TEMPORARY rev-7 probe: the E row
 * (y88, slot3/user0 vblank samples) is overprinted by the D row painted
 * at the same y, so the walk evidence is invisible. Repack it here.
 * Encoding (5 digits): slot3-code × 10000 + user0-code × 1000 + EDSR&3.
 * Code per CTRL word: 1 = our value (slot3 0x000A / user0 0x0002 —
 * table intact), 2 = 0x8000 (END — SGL rebuild won / walk terminator),
 * 3 = other non-zero (foreign writer), 9 = zero (callback never ran or
 * VRAM cleared). 11000+EDSR = fully healthy (our table + draw done). */
int Saturn_Gun_Debug(void)
{
    extern int saturn_v1w_slot3_at_vbl;
    extern int saturn_v1w_user0_at_vbl;
    extern int saturn_v1w_edsr;
    int        s = saturn_v1w_slot3_at_vbl & 0xFFFF;
    int        u = saturn_v1w_user0_at_vbl & 0xFFFF;
    int        sc, uc;

    if (s == 0)          sc = 9;
    else if (s == 0x000A) sc = 1;
    else if (s == 0x8000) sc = 2;
    else                 sc = 3;
    if (u == 0)          uc = 9;
    else if (u == 0x0002) uc = 1;
    else if (u == 0x8000) uc = 2;
    else                 uc = 3;
    return sc * 10000 + uc * 1000 + (saturn_v1w_edsr & 3);
}

/* far-to-near paint order: descending tz, insertion sort */
static void seg_sort_sprites(void)
{
    int i, j;

    for (i = 1; i < spr_vis_count; i++)
    {
        sprvis_t key = spr_vis[i];

        for (j = i - 1; j >= 0 && spr_vis[j].dist < key.dist; j--)
            spr_vis[j + 1] = spr_vis[j];
        spr_vis[j + 1] = key;
    }
}

/*----- per-core draw, after every wall/flat row is painted -----*/
static void seg_draw_sprites(int x0, int x1c, int cpu)
{
    short *otop = spr_ctop[cpu];
    short *obot = spr_cbot[cpu];
    byte  *cmap = Saturn_Colormap();
    int    i, x, j;

#ifdef SATURN_SPRCLIP_DIAG
    sprdiag_seen[cpu] = 0;
    sd_gap[cpu] = -1;
    SD_UNC(&sd_span[cpu]) = -1;
    SD_UNC(&sd_espan[cpu]) = -1;
    SD_UNC(&sd_eok[cpu]) = 0;
#endif

    for (i = 0; i < spr_vis_count; i++)
    {
        sprvis_t *sv = &spr_vis[i];
        int xs = sv->x1 > x0 ? sv->x1 : x0;
        int xe = sv->x2 < x1c - 1 ? sv->x2 : x1c - 1;
        byte *pix, *fb;
        fixed_t row0f;
        int y, ytop, ybot;
#ifdef SATURN_SPRCLIP_DIAG
        int sdw = 0;    /* this half's watched (most-erased) sprite */
        int lj = -1, lyy = -1, lfl = 0, lgz = 0, ln = 0, lb = -1, scx;
        int lcl = 0, lsc = 0;   /* vanish hunt: occluder class + scale */
        int pa = 999, pz = -1;  /* round-33g paint extent (hi/lo row) */
        int painted = 0;        /* corpse hunt: any texel written? */
#endif

        if (cpu == 0)   /* round-51: net 5 — sprite-pass FRT acc */
            SAT_FRT_NET(saturn_spr_frt_acc, saturn_spr_frt_last, 5);

        if (xs > xe)
            continue;

#ifdef SATURN_SPRCLIP_DIAG
        scx = (xs + xe) >> 1;
#endif

        /* slave-safe: cached pixels only (master preloaded them) */
        pix = (cpu == 1) ? Saturn_SpriteDataNoLoad(sv->scache)
                         : Saturn_SpriteData(sv->scache);
        if (!pix)
            continue;

        for (x = xs; x <= xe; x++)
        {
            otop[x] = VIEW_TOP;
            obot[x] = VIEW_BOT;
        }

        /*---- clip against every wall piece NEARER than the sprite ----*/
        for (j = 0; j < segwalls_count; j++)
        {
            segwall_t *w = &segwalls[j];
            int s1, s2, lo, hi, r1, r2, p0, pe;

            if (w->start > xe || w->stop < xs)
                continue;

            /* round-34c — d32xr SPLITS long segs into <=80-column
             * pieces (R_StoreWallRange, maxlen = centerX/2) and
             * R_ClipVisSprite depth-tests EACH piece with its OWN
             * endpoint scales. Our whole-span test let a long oblique
             * wall's NEAR end qualify the ENTIRE span as "nearer than
             * the sprite"; its AC_SOLIDSIL then full-closed the
             * sprite's columns even where the wall recedes far behind
             * the thing — the angle-dependent erase that survived the
             * MIDTEXTURE fix (demo corridor + platform-corpse vanish;
             * the original conviction frame's J000 S05694 wall 29
             * units from the camera was exactly such a near end).
             * Walk 80-col pieces exactly like d32xr. */
            for (p0 = w->start; p0 <= w->stop; p0 = pe + 1)
            {
                pe = p0 + 79;
                if (pe > w->stop)
                    pe = w->stop;

                s1 = w->scalefrac + (p0 - w->start) * w->scalestep;
                s2 = w->scalefrac + (pe - w->start) * w->scalestep;
                lo = imin(s1, s2);
                hi = imax(s1, s2);

                /* entirely farther: cannot occlude (per piece) */
                if (hi < sv->yscale)
                    continue;
                /* straddling the sprite's depth, but the seg runs
                 * behind the sprite's map point: d32xr's tie-break */
                if (lo <= sv->yscale && seg_spr_behind(w, sv->gx, sv->gy))
                    continue;

#ifdef SATURN_SPRCLIP_DIAG
                ln++;
#endif

                r1 = p0 < xs ? xs : p0;
                r2 = pe > xe ? xe : pe;
                if (r1 > r2)
                    continue;

            /* Only a genuine SOLID silhouette may close the whole
             * column (d32xr r_phase8.c R_ClipVisSprite: silhouette =
             * TOPSIL/BOTTOMSIL/SOLIDSIL only; silhouette == 0 ->
             * continue). AC_MIDTEXTURE on an open two-sided line (door
             * tracks, grates) draws a masked middle texture but must
             * NOT clip sprites — clipping there erased everything
             * standing behind an open door frame, angle-dependent
             * (the demo sergeant vanish). */
            if (w->actionbits & AC_SOLIDSIL)
            {
                for (x = r1; x <= r2; x++)
                {
#ifdef SATURN_SPRCLIP_DIAG
                    if (x == scx)
                    {
                        lj  = j;
                        lyy = -2;    /* solid/mid full block */
                        lfl = w->floornew;
                        lgz = sv->gzt - seg_viewz;
                        lcl = 1;
                        lsc = w->scalefrac + (scx - w->start) * w->scalestep;
                    }
#endif
                    otop[x] = (short)(VIEW_BOT + 1);
                    obot[x] = (short)(VIEW_TOP - 1);
                }
            }
            else
            {
                for (x = r1; x <= r2; x++)
                {
                    int sc = w->scalefrac
                           + (x - w->start) * w->scalestep;

                    /* Only genuine silhouette edges may clip sprites
                     * (d32xr r_phase8.c R_ClipVisSprite consumes
                     * TOPSIL/BOTTOMSIL/SOLIDSIL; vanilla r_things.c
                     * R_DrawSprite consumes ds->silhouette). The
                     * flat-clip NEW* planes of plain two-sided
                     * openings belong to geometry that is often
                     * FARTHER than the thing at the thing's own
                     * columns — clipping there floats standing
                     * sprites. */
                    if (w->actionbits & AC_TOPSIL)
                    {
                        int yy = HALF_H
                               - (FixedMul(sc, w->ceilingnew) >> FRACBITS);

                        if (yy > otop[x])
                        {
                            otop[x] = (short)yy;
#ifdef SATURN_SPRCLIP_DIAG
                            if (x == scx)
                            {
                                lj  = j;
                                lyy = yy;
                                lfl = w->floornew;
                                lgz = sv->gzt - seg_viewz;
                                lcl = 2;
                                lsc = sc;
                            }
#endif
                        }
                    }
                    if (w->actionbits & AC_BOTTOMSIL)
                    {
                        int yy = HALF_H
                               - (FixedMul(sc, w->floornew) >> FRACBITS);

                        if (yy < obot[x])
                        {
                            obot[x] = (short)yy;
#ifdef SATURN_SPRCLIP_DIAG
                            if (x == scx)
                            {
                                lj  = j;
                                lyy = yy;
                                lfl = w->floornew;
                                lgz = sv->gzt - seg_viewz;
                                lcl = 3;
                                lsc = sc;
                            }
#endif
                        }
                    }
                }
            }
            }   /* end 80-col piece (body above sits in the piece loop;
                 * its indentation is the pre-piece depth) */
        }

        /*---- paint the columns ----*/
        row0f = (HALF_H << FRACBITS)
              - FixedMul(sv->gzt - seg_viewz, sv->yscale);
        /* keep the texel math in int range for pathological
         * depth/height combinations (see the v-loop below) */
        if (row0f > (200 << FRACBITS))       row0f = 200 << FRACBITS;
        else if (row0f < -(200 << FRACBITS)) row0f = -(200 << FRACBITS);

#ifdef SATURN_SPRCLIP_DIAG
        /* commit the VISIBLE sprite with the LARGEST clip gap at its
         * own centre column (base minus final obot) — the most-erased
         * sprite is exactly the floater the probe is hunting. gap <= 0
         * (feet intact) never wins, so a clean frame reports the last
         * commit's stale values — the white/red per-sprite ticks carry
         * the ground truth in that case. */
        lb = (row0f >> FRACBITS)
           + ((sv->h * sv->yscale) >> FRACBITS);
        {
            /* lb <= VIEW_BOT added (round-33f): a near sprite whose
             * base is below the screen committed gap = base - 189 and
             * MASKED the real floater's record (M row showed a junk
             * J999 Y999 B303 in the floater screenshot). Only a sprite
             * whose base is ON screen can be a watched floater. */
            int vis = (obot[scx] >= otop[scx] && otop[scx] <= VIEW_BOT
                       && obot[scx] >= VIEW_TOP && lb > VIEW_TOP
                       && lb <= VIEW_BOT);
            int gap = lb - obot[scx];

            /* dist gate: tz < 1 unit is impossible (collect guard) —
             * a committed record with D=0 means NO commit ever happened
             * (BSS zeros), not a real sprite. Require a real erased
             * span and a sane depth so degenerate sprites never win. */
            if (vis && gap >= 2 && sv->dist > (16 << FRACBITS)
                && gap > sd_gap[cpu])
            {
                sd_gap[cpu]  = gap;
                sd_j[cpu]    = lj;
                sd_yy[cpu]   = lyy;
                sd_fl[cpu]   = lfl;
                sd_gz[cpu]   = lgz;
                sd_n[cpu]    = ln;
                sd_base[cpu] = lb;
                sd_dist[cpu] = sv->dist >> FRACBITS;
                SD_UNC(&sd_botz[cpu])  = Saturn_SpriteBotz(sv->scache);
                SD_UNC(&sd_guard[cpu]) = Saturn_SpriteGuard(sv->scache);
                if (cpu == 0)   /* W_LumpLength: master-half only */
                    SD_UNC(&sd_plen[cpu]) = Saturn_SpritePlen(sv->scache);
                sdw = 1;
            }
        }
        /* round-33g SECOND watch: the max-gap commit above can NEVER
         * fire on an uncut floater (obot[scx] == VIEW_BOT -> gap
         * negative), so K/G/P only ever reported clip victims — the
         * loaded bug3.yss savestate showed K000 G000 WITH floaters on
         * screen. Watch the sprite with the largest projected span
         * whose base is on screen instead; in the floater scene that
         * IS the floater (it spans the SLAVE half, so this runs per
         * cpu — the E1M1 zombies are only ever seen by cpu 1). */
        if (lb > VIEW_TOP && lb <= VIEW_BOT
            && sv->dist > (16 << FRACBITS))
        {
            int span = lb - (row0f >> FRACBITS);

            /* once watch 3 caught a fully-ERASED sprite this frame it
             * owns the row — a bigger live sprite must not mask it. */
            if (span > SD_UNC(&sd_span[cpu]) && !SD_UNC(&sd_eok[cpu]))
            {
                SD_UNC(&sd_span[cpu]) = span;
                SD_UNC(&sd_top[cpu])  = row0f >> FRACBITS;
                SD_UNC(&sd_base[cpu]) = lb;
                SD_UNC(&sd_dist[cpu]) = sv->dist >> FRACBITS;
                SD_UNC(&sd_botz[cpu])  = Saturn_SpriteBotz(sv->scache);
                SD_UNC(&sd_guard[cpu]) = Saturn_SpriteGuard(sv->scache);
                if (cpu == 0)   /* W_LumpLength: master-half only */
                    SD_UNC(&sd_plen[cpu]) = Saturn_SpritePlen(sv->scache);
                /* vanish hunt: name the last wall that clipped this
                 * sprite's centre column, its class, and its scale
                 * there vs the sprite's own (see sd_sc/sd_ys above). */
                SD_UNC(&sd_j[cpu])   = lj;
                SD_UNC(&sd_yy[cpu])  = lyy;
                SD_UNC(&sd_fl[cpu])  = lfl;
                SD_UNC(&sd_n[cpu])   = ln;
                SD_UNC(&sd_sc[cpu])  = lsc;
                SD_UNC(&sd_ys[cpu])  = sv->yscale;
                SD_UNC(&sd_cl[cpu])  = lcl;
                sdw = 1;
            }
        }
#endif

        for (x = xs; x <= xe; x++)
        {
            int texcol, c;
            fixed_t vv;

            ytop = otop[x];
            ybot = obot[x];
            if (ybot < ytop || ytop > VIEW_BOT || ybot < VIEW_TOP)
                continue;
            if (ytop < VIEW_TOP) ytop = VIEW_TOP;
            if (ybot > VIEW_BOT) ybot = VIEW_BOT;

            /* texel column: c pixels from the sprite's left edge */
            c = x - sv->xl;
            if (c < 0) c = 0;
            texcol = (int)(((fixed_t)c * (fixed_t)sv->ustep) >> FRACBITS);
            if (sv->flip)
                texcol = sv->w - 1 - texcol;
            if (texcol < 0) texcol = 0;
            else if (texcol >= sv->w) texcol = sv->w - 1;

            /* texel row (16.16) at the clip top — 64-bit intermediate:
             * a far sprite's vstep times a 390-row clip span
             * overflows int before the >>16 */
            {
                int64_t v64 = (((int64_t)ytop << 16) - row0f)
                            * (int64_t)sv->vstep >> 16;

                if (v64 >= (int64_t)sv->h << FRACBITS)
                    continue;       /* patch entirely below the clip */
                if (v64 < 0)
                {
                    /* round-33g THE FLOATING-SPRITE BUG: patch top BELOW
                     * the clip top. The old -(1<<24) clamp corrupted the
                     * anchor — it pulled the texel counter up to -256
                     * texels, so texel 0 painted 256/vstep rows EARLY and
                     * the whole sprite shifted UP by
                     *   (row0f - ytop) - 256/vstep
                     * rows while staying complete (displacement grows with
                     * distance: fires when (row0f-ytop)*tz/160 > 256).
                     * Keep the anchor exact instead: skip whole rows, then
                     * start with vv in [0, vstep) — texel 0 lands exactly
                     * on the first painted row. */
                    int64_t adv = -v64 / (int64_t)sv->vstep + 1;

                    if (adv > (int64_t)(ybot - ytop))
                        continue;   /* patch starts below the clip bottom */
                    ytop += (int)adv;
                    v64 += adv * (int64_t)sv->vstep;
                }
                vv = (fixed_t)v64;
            }

            fb = I_VideoBuffer + ytop * SCREEN_W + x;
#ifdef SATURN_SPRCLIP_DIAG
            painted = 1;
#endif
            for (y = ytop; y <= ybot; y++, vv += sv->vstep)
            {
                int rr = vv >> FRACBITS;
                byte t;

                if (rr < 0)
                {
                    fb += SCREEN_W;
                    continue;           /* above the patch top */
                }
                if (rr >= sv->h)
                    break;              /* below the patch bottom */
                t = pix[rr * sv->w + texcol];
                if (t)
                {
                    fb[0] = cmap ? cmap[(sv->sh << 8) + t] : t;
#ifdef SATURN_SPRCLIP_DIAG
                    if (y < pa) pa = y;
                    if (y > pz) pz = y;
#endif
                }
                fb += SCREEN_W;
            }

#ifdef SATURN_SPRCLIP_DIAG
            /* white marker at the UNCLIPPED base row: if the sprite's
             * feet float ABOVE this line, the clip loop cut them; if
             * the line itself floats above the floor, projection.
             * (Painted into I_VideoBuffer — survives the flip, unlike
             * StageText glyphs, which d_main republishes instead.) */
            if (sdw)
            {
                int my = sd_base[cpu] - 1;

                if (my >= VIEW_TOP && my <= VIEW_BOT)
                    I_VideoBuffer[my * SCREEN_W + x] = 4;
            }
#endif
        }

#ifdef SATURN_SPRCLIP_DIAG
        /* round-34b watch 3: this sprite's columns were on screen but
         * the clip + paint left it with ZERO texels (base still on
         * screen) — a fully-ERASED sprite, exactly the corpse-vanish
         * signature. The biggest such sprite wins the row (the vanished
         * thing is near = big; distant legit-occluded things are tiny).
         * J/C/Y/S name the last wall that clipped its centre column —
         * J=999/C=0 means the centre column was NEVER clipped and the
         * erase came from another column or the texel-anchor skips. */
        if (!painted && lb > VIEW_TOP && lb <= VIEW_BOT
            && sv->dist > (16 << FRACBITS))
        {
            int span = lb - (row0f >> FRACBITS);

            if (span > SD_UNC(&sd_espan[cpu]))
            {
                SD_UNC(&sd_espan[cpu]) = span;
                SD_UNC(&sd_top[cpu])  = row0f >> FRACBITS;
                SD_UNC(&sd_base[cpu]) = lb;
                SD_UNC(&sd_dist[cpu]) = sv->dist >> FRACBITS;
                SD_UNC(&sd_j[cpu])   = lj;
                SD_UNC(&sd_yy[cpu])  = lyy;
                SD_UNC(&sd_fl[cpu])  = lfl;
                SD_UNC(&sd_n[cpu])   = ln;
                SD_UNC(&sd_sc[cpu])  = lsc;
                SD_UNC(&sd_ys[cpu])  = sv->yscale;
                SD_UNC(&sd_cl[cpu])  = lcl;
                SD_UNC(&sd_eok[cpu]) = 1;
                sdw = 1;    /* its base marker + A/Z extent show too */
            }
        }
#endif

#ifdef SATURN_SPRCLIP_DIAG
        /* round-33g: rows the watched sprite's paint ACTUALLY wrote.
         * A == T && Z == B-1 = paint matches the projection; A < T =
         * paint started ABOVE the projection (anchor bug); Z < B-1 =
         * paint stopped early (cache/truncation). */
        if (sdw)
        {
            SD_UNC(&sd_hi[cpu]) = pa;
            SD_UNC(&sd_lo[cpu]) = pz;
        }
        /* round-33e FULL-SPAN bars for EVERY sprite (single-pixel ticks
         * at the centre column kept landing on the wrong sprite —
         * legitimate behind-a-railing occlusions match the floater's
         * signature). WHITE bar = unclipped projection base row-1
         * across the sprite's whole span; RED bar = the final clip row
         * (obot) per column. Reading a sprite's bracket:
         *   white bar AT the visible feet          -> projection correct
         *   white bar BELOW the visible feet       -> clip erased them
         *   white bar but NO sprite pixels at all  -> fully occluded
         *     (legit, e.g. behind a railing) — ignore those sprites.
         * Painted AFTER the sprite's columns so its own texture cannot
         * cover them; a later NEARER sprite may (sprites draw far to
         * near — the nearest sprite's bars always survive). */
        {
            int tb = (row0f >> FRACBITS)
                   + ((sv->h * sv->yscale) >> FRACBITS);

            if (tb - 1 >= VIEW_TOP && tb - 1 <= VIEW_BOT)
                for (x = xs; x <= xe; x++)
                    I_VideoBuffer[(tb - 1) * SCREEN_W + x] = 4;
            for (x = xs; x <= xe; x++)
                if (obot[x] >= VIEW_TOP && obot[x] <= VIEW_BOT)
                    I_VideoBuffer[obot[x] * SCREEN_W + x] = 176;
            /* round-33f GREEN bar = projected patch TOP row. With the
             * white bottom bar this bracket yields yscale = span/h on
             * the photo alone: head ABOVE the green bar = projection
             * displaced; head AT the green bar with feet ABOVE the
             * white bar = the paint/cache lost the bottom texels
             * (see the K/G/P vitals row). */
            if (row0f >= 0 && (row0f >> FRACBITS) <= VIEW_BOT)
                for (x = xs; x <= xe; x++)
                    I_VideoBuffer[(row0f >> FRACBITS) * SCREEN_W + x] = 112;
        }
#endif
    }
}

/* angle -> column tables (self-contained: d32xr's R_InitMathTables is
 * not called by the Saturn boot and its work-buffer API is Jaguar
 * specific). Built once into the zone. */
static short    *seg_viewangletox;      /* [FINEANGLES/2], post-cleanup.
                                         * d32xr MARS stores these as VINT
                                         * (= short); an int8_t was a bug â€”
                                         * columns 128..160 wrapped negative
                                         * (-128..-96), the packed
                                         * (x1<<16)|x2 return then produced
                                         * last up to 65439, and the clip
                                         * scans walked past the final
                                         * solidsegs sentinel into stack
                                         * garbage = the W2 freeze. */
static uint16_t *seg_xtoviewangle;      /* [SCREEN_W+1] */
static unsigned  seg_clipangle;
static int       seg_inited;

/* zone-carved constants (kept out of razor-thin BSS) */
static sector_t *seg_emptysector;
static int      *seg_checkcoord;

/*----- math (r_phase2.c ports) -----*/

/* SH-2 divide unit, signed 32/32 form (DVDNT at 0xFFFFFF04 â€” d32xr's
 * r_phase2.c uses this for scalestep). ~250-cycle __sdivsi3 -> ~16
 * cycles. Gated on the boot probe (d_main.c, HUD 'V' row). */
static int seg_divsi(int num, int den)
{
    extern volatile int saturn_divu_probe;
    if (den == 0)
        return 0;
    if (saturn_divu_probe)
    {
        volatile int *const dvsr  = (volatile int *)0xFFFFFF00u;
        volatile int *const dvdnt = (volatile int *)0xFFFFFF04u;
        *dvsr  = den;
        *dvdnt = num;                    /* start the divide */
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          ::: "memory");
        return *dvdnt;
    }
    return num / den;
}

/* R_PointToDist: distance to a point in the projection */
static fixed_t seg_point_to_dist(fixed_t x, fixed_t y)
{
    angle_t angle;
    fixed_t dx, dy, temp;

    dx = D_abs(x - seg_viewx);
    dy = D_abs(y - seg_viewy);

    if (dy > dx)
    {
        temp = dx;
        dx = dy;
        dy = temp;
    }

    angle = (tantoangle[(unsigned)FixedDiv(dy, dx) >> DBITS] + ANG90)
            >> ANGLETOFINESHIFT;

    return FixedDiv(dx, finesine(angle));
}

/* R_ScaleFromGlobalAngle with stretchX = PROJ<<16 (our PROJ) */
static fixed_t seg_scale_from_angle(fixed_t rw_distance, angle_t visangle,
                                    angle_t normalangle)
{
    angle_t anglea, angleb;
    fixed_t num, den;
    int     sinea, sineb;

    visangle += ANG90;

    anglea = visangle - seg_viewangle;
    sinea  = finesine(anglea >> ANGLETOFINESHIFT);
    angleb = visangle - normalangle;
    sineb  = finesine(angleb >> ANGLETOFINESHIFT);

    num = FixedMul(PROJ << FRACBITS, sineb);
    den = FixedMul(rw_distance, sinea);

    return FixedDiv(num, den);
}

/* R_SetupCalc: texture offset along the wall + center angle */
static void seg_setup_calc(segwall_t *w, fixed_t hyp, angle_t normalangle,
                           angle_t angle1)
{
    angle_t offsetangle;
    fixed_t sineval, rw_offset;

    offsetangle = normalangle - angle1;

    if (offsetangle > ANG180)
        offsetangle = 0 - offsetangle;
    if (offsetangle > ANG90)
        offsetangle = ANG90;

    sineval   = finesine(offsetangle >> ANGLETOFINESHIFT);
    rw_offset = FixedMul(hyp, sineval);

    if (normalangle - angle1 < ANG180)
        rw_offset = -rw_offset;

    w->offset      += rw_offset;
    w->centerangle  = ANG90 + seg_viewangle - normalangle;
}

/* P_SegOffset: distance from the linedef's start vertex to the seg's
 * first vertex, in map units (integer sqrt) */
static unsigned seg_p_seg_offset(seg_t *seg)
{
    int side = SEG_UNPACK_SIDE(seg);
    mapvertex_t *sv = &vertexes[lines[SEG_UNPACK_LINEDEF(seg)].v1];
    mapvertex_t *ev = &vertexes[lines[SEG_UNPACK_LINEDEF(seg)].v2];
    mapvertex_t *v1 = &vertexes[SEG_UNPACK_V1(seg)];
    unsigned dx, dy;
    unsigned g, c;
    unsigned n;

    if (side)
    {
        dx = D_abs(ev->x - v1->x);
        dy = D_abs(ev->y - v1->y);
    }
    else
    {
        dx = D_abs(sv->x - v1->x);
        dy = D_abs(sv->y - v1->y);
    }

    if (dx == 0)
        return dy;
    if (dy == 0)
        return dx;

    n = dx * dx + dy * dy;
    g = c = 0x8000;
    for (;;)
    {
        if (g * g > n) g ^= c;
        c >>= 1;
        if (c == 0) return g;
        g |= c;
    }
}

/*----- wall prep -----*/

/* R_WallEarlyPrep, simplified to what our painter consumes */
static void seg_wall_early_prep(segwall_t *w, line_t *li, side_t *si,
                                sector_t *fsec, sector_t *bsec_in)
{
    sector_t *bsec = bsec_in ? bsec_in : seg_emptysector;
    fixed_t   f_floor = fsec->floorheight   - seg_viewz;
    fixed_t   f_ceil  = fsec->ceilingheight - seg_viewz;
    fixed_t   b_floor = bsec->floorheight   - seg_viewz;
    fixed_t   b_ceil  = bsec->ceilingheight - seg_viewz;
    int       f_cpic  = *(int8_t *)&fsec->ceilingpic == -1 ? -1
                        : fsec->ceilingpic;
    int       b_cpic  = *(int8_t *)&bsec->ceilingpic == -1 ? -1
                        : bsec->ceilingpic;
    int       f_light = fsec->lightlevel, b_light = bsec->lightlevel;
    short     skyhack;
    int       ab = 0;

    li->flags |= ML_MAPPED;     /* automap seen-mark */

    w->floorheight   = f_floor;
    w->floornew      = f_floor;
    w->ceilingheight = f_ceil;
    w->ceilingnew    = f_ceil;
    w->t_top         = f_ceil;
    w->t_tex = w->b_tex = w->m_tex = 0;
    /* ABS world-z pegs, in WHOLE MAP UNITS â€” paint_wall_span computes
     * v0 = (peg_u + rowoff - htop) << FRACBITS with htop in map units
     * (eye_u = viewz>>FRACBITS), and the raycaster feeds pegs through
     * >>FRACBITS. These were stored as raw fixed_t: every seg wall's
     * texture was v-shifted by (ceilingheight mod texheight) rows â€”
     * correct under 128-high ceilings with 128-tall textures, red-noise
     * everywhere else (the widespread wall red). */
    w->t_peg_abs = fsec->ceilingheight >> FRACBITS;
    w->b_peg_abs = bsec->floorheight >> FRACBITS;
    w->t_peg_top = w->t_peg_abs;
    w->offset    = (si->textureoffset & 0xfff) << 4 >> 4;  /* 12.4 sign-ext */

    skyhack = (f_cpic == -1 && b_cpic == -1);

    if (f_floor < 0 &&
        (fsec->floorpic   != bsec->floorpic   ||
         f_floor          != b_floor          ||
         f_light          != b_light          ||
         b_ceil           == b_floor))
        ab |= (AC_ADDFLOOR | AC_NEWFLOOR);

    if (!skyhack &&
        (f_ceil > 0 || f_cpic == -1) &&
        (f_cpic != b_cpic ||
         f_ceil  != b_ceil  ||
         f_light != b_light ||
         b_ceil  == b_floor))
        ab |= (f_cpic == -1 ? AC_ADDSKY : AC_ADDCEILING) | AC_NEWCEILING;

    if (bsec == seg_emptysector)
    {
        /* single-sided: paint mid||top||bottom (raycaster parity) */
        w->t_tex = si->midtexture ? si->midtexture
                 : (si->toptexture ? si->toptexture : si->bottomtexture);
        w->t_bot = f_floor;
        /* vanilla r_segs.c:629 solid seg: DONTPEGBOTTOM anchors the
         * texture's BOTTOM edge at the front floor */
        if (li->flags & ML_DONTPEGBOTTOM)
            w->t_peg_top = (fsec->floorheight >> FRACBITS)
                         + textures[texturetranslation[w->t_tex]].height;
        ab |= AC_SOLIDSIL | AC_TOPTEXTURE;
    }
    else
    {
        if (si->midtexture)
        {
            w->m_tex = si->midtexture;
            ab |= AC_MIDTEXTURE;
        }

        if (b_floor > f_floor && si->bottomtexture)
        {
            w->b_tex = si->bottomtexture;
            w->b_top  = b_floor;
            w->b_bot  = f_floor;
            w->floornew = b_floor;
            /* vanilla r_segs.c:594 lower wall: DONTPEGBOTTOM anchors row 0
             * at the FRONT ceiling (worldtop), not the back floor. E1M1's
             * SW1COMP lift step (ld136, flags 0x1C) was painted from the
             * back floor -> showed texture rows 0..48 (dark COMP04_5
             * board) instead of rows ~80..128 (grey panel + red switch). */
            if (li->flags & ML_DONTPEGBOTTOM)
                w->b_peg_abs = fsec->ceilingheight >> FRACBITS;
            ab |= AC_BOTTOMTEXTURE | AC_NEWFLOOR;
        }

        if (b_ceil < f_ceil && !skyhack && si->toptexture)
        {
            w->t_tex = si->toptexture;
            w->t_bot    = b_ceil;
            w->ceilingnew = b_ceil;
            /* vanilla r_segs.c:585-596 upper wall: pegged (no DONTPEGTOP)
             * anchors the texture's BOTTOM edge at backceiling+texheight */
            if (!(li->flags & ML_DONTPEGTOP))
                w->t_peg_top = (bsec->ceilingheight >> FRACBITS)
                             + textures[texturetranslation[w->t_tex]].height;
            ab |= AC_NEWCEILING | AC_TOPTEXTURE;
        }

        /* Sprite-clipper silhouette bits (d32xr r_phase1.c:349-367).
         * seg_draw_sprites may clip things ONLY against walls with a
         * genuine solid edge: a closed opening (SOLIDSIL), a step face
         * at/above eye or a floor drop seen from above (BOTTOMSIL), a
         * ceiling step (TOPSIL). Plain two-sided openings — continuous
         * floor/ceiling differing only in pic or light — set
         * NEWFLOOR/NEWCEILING for the FLAT clip but carry no
         * silhouette and must never reach the sprite clipper: their
         * floor/ceiling plane at columns where the wall is FARTHER
         * than the thing projects ABOVE the thing's base and cuts its
         * feet loose (the distance-dependent floating-sprite bug:
         * grounded far away, floating at mid range, grounded again up
         * close as the piece turns entirely-farther/behind). */
        if (b_floor >= f_ceil || b_ceil <= f_floor)
            ab |= AC_SOLIDSIL;
        else
        {
            if ((b_floor >= 0 && b_floor > f_floor) ||
                (f_floor < 0 && f_floor > b_floor))
                ab |= AC_BOTTOMSIL;
            if (!skyhack &&
                ((b_ceil <= 0 && b_ceil < f_ceil) ||
                 (f_ceil > 0 && b_ceil > f_ceil)))
                ab |= AC_TOPSIL;
        }
    }

    w->actionbits = ab;

    /* SW1COMP face probe: the walk just built a wall carrying the
     * switch texture as its lower face. */
    if (w->b_tex == saturn_sw1_tex && saturn_sw1_tex > 0)
        saturn_sw1_built++;
}

/* R_WallLatePrep: scale/offset/centerangle for one viswall piece */
static void seg_wall_late_prep(segwall_t *w)
{
    seg_t   *seg = w->seg;
    int      nv1 = SEG_UNPACK_V1(seg);
    int      nv2 = SEG_UNPACK_V2(seg);
    fixed_t  x1 = vertexes[nv1].x << FRACBITS;
    fixed_t  y1 = vertexes[nv1].y << FRACBITS;
    fixed_t  x2 = vertexes[nv2].x << FRACBITS;
    fixed_t  y2 = vertexes[nv2].y << FRACBITS;
    angle_t  angle1 = (angle_t)w->angle1;
    fixed_t  hyp = seg_point_to_dist(x1, y1);
    angle_t  normalangle = R_PointToAngle(x1, y1, x2, y2) + ANG90;
    angle_t  offsetangle = normalangle - angle1;
    fixed_t  rw_distance, scalefrac, scale2;

    if ((int)offsetangle < 0)
        offsetangle = 0 - offsetangle;
    if (offsetangle > ANG90)
        offsetangle = ANG90;

    rw_distance = FixedMul(hyp, finesine((ANG90 - offsetangle)
                                         >> ANGLETOFINESHIFT));
    w->distance = rw_distance;
    /* Round 27 (2026-09-02): the HUD odd rows twice showed segwalls[7]
     * with a NEGATIVE distance (0xDFFFFF00 = -128 units) and
     * scalefrac>>16 = 0 (clamped to 1) â€” a degenerate seg (vertex
     * degenerate with the view point drives seg_point_to_dist's
     * tantoangle index out of range) whose garbage scale collapses its
     * projections onto the horizon row: the painter then narrows the
     * clip to a dead band down the wall's whole column range while
     * painting nothing there â€” the full-height clear bar family
     * (pose-dependent, Software WARP renders those columns fine). A
     * visible wall always has positive distance, so kill the piece
     * outright: an empty [start,stop) makes the painter skip both its
     * pixels and its clip narrowing, and its ADD-flat staging with
     * them. */
    if (rw_distance <= 0)
    {
        w->stop      = w->start;
        w->scalestep = 0;
        w->scalefrac = 1;
        return;
    }
    w->offset   = ((fixed_t)w->offset + seg_p_seg_offset(seg)) << FRACBITS;

    scalefrac = scale2 = seg_scale_from_angle(rw_distance,
        seg_viewangle + ((unsigned)seg_xtoviewangle[w->start] << FRACBITS),
        normalangle);
    if (scalefrac < 1)
        scalefrac = 1;

    if (w->stop > w->start)
    {
        scale2 = seg_scale_from_angle(rw_distance,
            seg_viewangle + ((unsigned)seg_xtoviewangle[w->stop] << FRACBITS),
            normalangle);
        if (scale2 < 1)
            scale2 = 1;
        w->scalestep = seg_divsi(scale2 - scalefrac, w->stop - w->start);
    }
    else
        w->scalestep = 0;

    w->scalefrac = scalefrac;

    if (w->actionbits & (AC_TOPTEXTURE | AC_BOTTOMTEXTURE | AC_MIDTEXTURE))
        seg_setup_calc(w, hyp, normalangle, angle1);
}

/*----- BSP traversal (r_phase1.c ports) -----*/

typedef struct
{
    segcliprange_t *newend;
    segcliprange_t *solidsegs;
    seg_t          *curline;
    sector_t       *curfsector, *curbsector;
    side_t         *curside;
    line_t         *curldef;
    angle_t         lineangle1;
    int             splitspans;
    int             lastv1, lastv2;
    angle_t         lastangle1, lastangle2;
    segwall_t      *lastwall;
#ifdef SATURN_PVS
    /* P4: this frame's PVS row (viewer's leaf) or NULL = ungated walk.
     * Lives in the stack-local segbsp_t — zero .bss. */
    const byte     *pvs_row;
    int             pvs_skip;
#endif
} segbsp_t;

static int seg_clip_to_view_edges(angle_t angle1, angle_t angle2)
{
    angle_t span, tspan;
    int x1, x2;
    angle_t doubleclipangle;

    span = angle1 - angle2;
    if (span >= ANG180)
        return 0;

    angle1 -= seg_viewangle;
    angle2 -= seg_viewangle;
    doubleclipangle = seg_clipangle * 2;

    tspan = angle1 + seg_clipangle;
    if (tspan > doubleclipangle)
    {
        tspan -= doubleclipangle;
        if (tspan >= span)
            return -1;
        angle1 = seg_clipangle;
    }

    tspan = seg_clipangle - angle2;
    if (tspan > doubleclipangle)
    {
        tspan -= doubleclipangle;
        if (tspan >= span)
            return -1;
        angle2 = 0 - seg_clipangle;
    }

    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    x1 = seg_viewangletox[angle1];
    x2 = seg_viewangletox[angle2];

    if (x1 == x2)
        return -1;

    return (x1 << 16) | x2;
}

static boolean seg_check_bbox(segbsp_t *rbsp, const int16_t *bspcoord_)
{
    int      boxx, boxy, boxpos;
    fixed_t  x1, y1, x2, y2;
    angle_t  angle1, angle2;
    segcliprange_t *start;
    int      sx1, sx2;
    fixed_t  bspcoord[4];

    bspcoord[0] = bspcoord_[0] << FRACBITS;
    bspcoord[1] = bspcoord_[1] << FRACBITS;
    bspcoord[2] = bspcoord_[2] << FRACBITS;
    bspcoord[3] = bspcoord_[3] << FRACBITS;

    boxx = 2;
    boxx -= (seg_viewx < bspcoord[BOXRIGHT]);
    boxx -= (seg_viewx <= bspcoord[BOXLEFT]);

    boxy = 2;
    boxy -= (seg_viewy > bspcoord[BOXBOTTOM]);
    boxy -= (seg_viewy >= bspcoord[BOXTOP]);

    boxpos = (boxy << 2) + boxx;
    if (boxpos == 5)
        return true;

    x1 = bspcoord[seg_checkcoord[boxpos * 4 + 0]];
    y1 = bspcoord[seg_checkcoord[boxpos * 4 + 1]];
    x2 = bspcoord[seg_checkcoord[boxpos * 4 + 2]];
    y2 = bspcoord[seg_checkcoord[boxpos * 4 + 3]];

    angle1 = R_PointToAngle(seg_viewx, seg_viewy, x1, y1);
    angle2 = R_PointToAngle(seg_viewx, seg_viewy, x2, y2);

    sx1 = seg_clip_to_view_edges(angle1, angle2);
    if (sx1 == 0)
        return true;
    if (sx1 == -1)
        return false;
    sx2 = (sx1 & 0xffff) - 1;
    sx1 = sx1 >> 16;

    start = rbsp->solidsegs;
    while (start->last < sx2)
        ++start;

    if (sx1 >= start->first && sx2 <= start->last)
        return false;

    return true;
}

static void seg_store_wall_range(segbsp_t *rbsp, int start, int stop)
{
    const int maxlen = SCREEN_W / 4;    /* d32xr centerX/2 */
    int newstop, split;
    int numwalls = rbsp->lastwall - segwalls;
    int len = stop - start + 1;

    if (start > stop)
        return;

#ifdef SATURN_SEG_NULLWALL
    /* Measurement build, A/B/C protocol (2026-09-02): how much of the D
     * row is wall MATH vs wall PIXELS vs traversal? Inert unless the
     * Makefile passes -DSATURN_SEG_NULLWALL=n.
     *   C (=2): return here â€” D_C = traversal only (BSP walk + clip).
     *   B (=1): fall through â€” prep math runs, but the wall is never
     *           stored, so phase 2 emits zero wall pixels: D_B = C + math.
     * Wall-math cost = D_B - D_C; wall-pixel cost = D_A - D_B.
     * The image is garbage in both â€” measurement builds only. */
#if SATURN_SEG_NULLWALL == 2
    return;
#endif
#endif

    if (rbsp->splitspans <= 0 || len < maxlen)
    {
        split   = len;
        newstop = stop;
    }
    else
    {
        split   = 0;
        newstop = start + maxlen - 1;
    }

    do
    {
        segwall_t *rw;

        if (numwalls == SEGW_MAX)
        {
            /* Round 25: d32xr drops silently here (r_phase1.c:415). The
             * symptom of a hit is "distant walls are not rendered until
             * you move close" â€” the near-to-far walk fills the quota
             * with nearer pieces and the far vista is dropped. Counted
             * on the HUD 'u' row. */
            ++saturn_seg_caphits;
            return;
        }

        rw = rbsp->lastwall;
        rw->seg        = rbsp->curline;
        rw->start      = start;
        rw->stop       = newstop;
        rw->angle1     = (int)rbsp->lineangle1;
#ifndef SATURN_SEG_NULLWALL
        ++rbsp->lastwall;       /* B variant (NULLWALL=1): math above runs
                                 * (early/late prep), but the wall is never
                                 * counted, so phase 2 emits no wall pixels. */
#endif

        seg_wall_early_prep(rw, rbsp->curldef, rbsp->curside,
                            rbsp->curfsector, rbsp->curbsector);
        /* Phase 2 resolves flats (sectors[w->fsec_i]) and wall-u offsets
         * (sides[w->side_i]) from these indices â€” they were never stored,
         * so every segwall read stale zone bytes: one constant bogus
         * sector painted every floor/ceiling band (grass indoors, sky
         * where ceilings belong) and garbage utex offsets (the solid
         * red-brown strips). */
        rw->fsec_i = (short)(rbsp->curfsector - sectors);
        rw->side_i = (short)(rbsp->curside - sides);
        /* Owner of whatever the clip still leaves open after this wall:
         * a two-sided wall narrows the clip to its own opening, so the
         * remainder lies in its BACK sector (solid walls close the clip
         * outright â€” the value is then never read). */
        rw->bsec_i = (rbsp->curbsector == seg_emptysector)
                         ? rw->fsec_i
                         : (short)(rbsp->curbsector - sectors);
#ifdef SATURN_SEG_DIFF
        /* Build-time probe for the wall covering the probe col: peg +
         * texture as early_prep computed them, BEFORE late_prep/render.
         * Compared against paint_wall_span's first-span probes (S/D
         * rows) this splits "early_prep computed a garbage peg" from
         * "the peg is lost between build and paint". (Was keyed to col
         * 28; the paint-side probes moved to col 104.) */
        if (start <= 104 && newstop >= 104)
        {
            extern volatile int saturn_probe_build_peg;
            extern volatile int saturn_probe_build_tex;

            saturn_probe_build_peg = rw->t_peg_abs;
            saturn_probe_build_tex =
                rw->t_tex ? rw->t_tex
                          : (rw->m_tex ? rw->m_tex : rw->b_tex);
        }
#endif
        seg_wall_late_prep(rw);

        numwalls++;

        start  = newstop + 1;
        newstop += maxlen;
        if (newstop > stop)
            newstop = stop;
    } while (start <= stop);

    rbsp->splitspans -= split;
}

static void seg_clip_wall_segment(segbsp_t *rbsp, int first, int last,
                                  boolean solid)
{
    segcliprange_t *start, *next;

    start = rbsp->solidsegs;
    while (start->last < first - 1)
        ++start;

    if (first < start->first)
    {
        if (last < start->first - 1)
        {
            seg_store_wall_range(rbsp, first, last);

            if (solid)
            {
                next = rbsp->newend;
                ++rbsp->newend;

                if (next != start)
                {
                    int *s = (int *)start;
                    int n = *s++;
                    int cnt = next - start;

                    next = start;
                    do
                    {
                        int nn = *s;
                        *s++ = n, n = nn;
                    } while (--cnt);
                }

                next->first = first;
                next->last  = last;
            }
            return;
        }

        seg_store_wall_range(rbsp, first, start->first - 1);

        if (solid)
            start->first = first;
    }

    if (last <= start->last)
        return;

    next = start;
    while (last >= (next + 1)->first - 1)
    {
        seg_store_wall_range(rbsp, next->last + 1, (next + 1)->first - 1);
        ++next;

        if (last <= next->last)
        {
            last = next->last;
            goto crunch;
        }
    }

    seg_store_wall_range(rbsp, next->last + 1, last);
crunch:
    if (solid)
    {
        start->last = last;

        if (next == start)
            return;

        if (next != rbsp->newend)
        {
            int cnt = rbsp->newend - next;
            do
            {
                start++, next++;
                *((int *)start) = *((int *)next);
            } while (--cnt);
        }

        rbsp->newend = start + 1;
    }
}

static void seg_add_line(segbsp_t *rbsp, seg_t *line)
{
    angle_t  angle1, angle2;
    int      x1, x2;
    sector_t *backsector;
    line_t   *ldef;
    side_t   *sidedef;
    boolean   solid;
    int       side;
    int       nv1 = SEG_UNPACK_V1(line);
    int       nv2 = SEG_UNPACK_V2(line);
    fixed_t   v1x = vertexes[nv1].x << FRACBITS;
    fixed_t   v1y = vertexes[nv1].y << FRACBITS;
    fixed_t   v2x = vertexes[nv2].x << FRACBITS;
    fixed_t   v2y = vertexes[nv2].y << FRACBITS;

    if (nv1 == rbsp->lastv2)
        angle1 = rbsp->lastangle2;
    else if (nv1 == rbsp->lastv1)
        angle1 = rbsp->lastangle1;
    else
        angle1 = R_PointToAngle(seg_viewx, seg_viewy, v1x, v1y);
    if (nv2 == rbsp->lastv1)
        angle2 = rbsp->lastangle1;
    else if (nv2 == rbsp->lastv2)
        angle2 = rbsp->lastangle2;
    else
        angle2 = R_PointToAngle(seg_viewx, seg_viewy, v2x, v2y);

    rbsp->lastv1     = nv1;
    rbsp->lastv2     = nv2;
    rbsp->lastangle1 = angle1;
    rbsp->lastangle2 = angle2;

    x1 = seg_clip_to_view_edges(angle1, angle2);
    if (x1 <= 0)
        return;
    x2 = (x1 & 0xffff) - 1;
    x1 = x1 >> 16;

    side     = SEG_UNPACK_SIDE(line);
    ldef     = &lines[SEG_UNPACK_LINEDEF(line)];
    backsector = ldef->sidenum[1] >= 0
               ? LD_SIDESECTOR(ldef, side ^ 1) : NULL;
    sidedef  = &sides[ldef->sidenum[side]];
    solid    = false;

    if (!backsector)
        solid = true;
    else if (*(int8_t *)&backsector->ceilingpic == -1 &&
             *(int8_t *)&rbsp->curfsector->ceilingpic == -1)
        solid = false;      /* both skies: stay open to prevent HOM */
    else if (backsector->ceilingheight <= rbsp->curfsector->floorheight ||
             backsector->floorheight   >= rbsp->curfsector->ceilingheight)
        solid = true;
    else if (backsector->ceilingheight == rbsp->curfsector->ceilingheight &&
             backsector->floorheight   == rbsp->curfsector->floorheight)
    {
        /* reject empty trigger lines */
        if (sidedef->midtexture == 0 &&
            *(int16_t *)&backsector->floorpic
                == *(int16_t *)&rbsp->curfsector->floorpic &&
            *(int8_t *)&backsector->lightlevel
                == *(int8_t *)&rbsp->curfsector->lightlevel)
            return;
    }

    rbsp->curline    = line;
    rbsp->curside    = sidedef;
    rbsp->curldef    = ldef;
    /* Round 27g (2026-09-02): one-sided lines store NULL here, but the
     * bsec_i consumer (seg_wall_early_prep's caller) only tests
     * seg_emptysector â€” raw NULL fell through and every one-sided
     * closer wrote bsec_i = (short)(NULL - sectors): a constant bogus
     * owner (lastsec read 63351 in every pose), which the frame-end
     * open-band fill then painted as the stable salmon/black bands.
     * Normalise here, matching seg_wall_early_prep's own guard. */
    rbsp->curbsector = backsector ? backsector : seg_emptysector;
    rbsp->lineangle1 = angle1;
    seg_clip_wall_segment(rbsp, x1, x2, solid);
}

/*----- P4: PVS-gated traversal (E1M1 blob, see Saturn_PvsLevelLoad) -----*/

#ifdef SATURN_PVS
/* Level state: the blob's PVS row base + geometry. Deliberately the
 * ONLY .bss this feature adds (8 bytes): the 39 KB blob itself lives in
 * the zone (PU_LEVEL, freed by the standard level teardown) and the
 * per-frame row pointer lives in the stack-local segbsp_t. */
static const byte *pvs_blob;         /* row base; NULL = feature off    */
static unsigned short pvs_nssec;     /* == numsubsectors, validated     */
static unsigned short pvs_rowbytes;  /* ceil(nssec/8)                   */

/* Locate the viewer's subsector: the same MARS walk seg_render_bsp_node
 * uses ((int16_t)node < 0 leaves, R_PointOnSide conv-A children pick),
 * minus bbox work. Returns the ssec index, or -1 on any surprise
 * (fail-open: the frame renders ungated). */
static int seg_pvs_locate(fixed_t x, fixed_t y)
{
    int nodenum = (int)numnodes - 1;

    while ((int16_t)nodenum >= 0)
    {
        node_t *bsp = &nodes[nodenum];

        nodenum = (int)bsp->children[R_PointOnSide(x, y, bsp)];
    }
    /* -1 = the degenerate single-subsector map encoding (root is the
     * leaf); anything else strips the NF_SUBSECTOR flag bit. */
    return (nodenum == -1) ? 0 : (nodenum & ~NF_SUBSECTOR);
}

static byte pvs_row_cache[128] __attribute__((aligned(4)));
static int  pvs_cached_leaf = -1;
static const byte *pvs_cached_blob = NULL;

const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y)
{
    if (pvs_blob != NULL)
    {
        int leaf = seg_pvs_locate(x, y);
        if (leaf >= 0 && leaf < pvs_nssec)
        {
            const byte *src = pvs_blob + (unsigned)leaf * pvs_rowbytes;
            if (pvs_rowbytes <= sizeof(pvs_row_cache))
            {
                if (leaf != pvs_cached_leaf || pvs_blob != pvs_cached_blob)
                {
                    D_memcpy(pvs_row_cache, src, pvs_rowbytes);
                    pvs_cached_leaf = leaf;
                    pvs_cached_blob = pvs_blob;
                }
                return pvs_row_cache;
            }
            return src;
        }
    }
    return NULL;
}
#else
const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y)
{
    (void)x; (void)y;
    return NULL;
}
#endif /* SATURN_PVS */

static void seg_subsector(segbsp_t *rbsp, int num)
{
#ifdef SATURN_PVS
    /* P4 gate: skip subsectors the blob's glVis rows mark not
     * potentially-visible from the viewer's leaf. Pure early-return —
     * visit order, solidsegs clipping and every downstream consumer see
     * the same sequence minus occluded leaves (PVS is a conservative
     * superset of line-of-sight, so nothing visible is ever skipped;
     * sprite collection below is gated too, which is correct for the
     * same reason). Rows are LSB-first — NOT the MSB-first v1w_skimap
     * idiom. */
    if (rbsp->pvs_row != NULL
        && !(rbsp->pvs_row[num >> 3] & (1u << (num & 7))))
    {
        rbsp->pvs_skip++;
        return;
    }
#endif
#ifdef SATURN_LEAF_RENDERER
    /* M2: the leaf blob replaces the SSECTORS seg range as the edge
     * source — same per-edge chain (seg_add_line below), same set of
     * real segs (the blob emits each exactly once, in perimeter
     * order), plus 0xFFFF closure/miniseg edges the wall path skips.
     * The closure records are what M3's z-buffer flat fill needs for
     * the leaf's screen range. */
    if (leaf_active())
    {
        subsector_t            *sub = &subsectors[num];
        const unsigned short   *e;
        int                     n, i;

        rbsp->curfsector = &sectors[sub->sector];
#ifndef SATURN_SPRITES_OFF
        seg_collect_sector_sprites(sub->sector);
#endif
        n = leaf_edges(num, &e);
        for (i = 0; i < n; i++)
        {
            unsigned si = e[1];

            e += 2;
            if (si != 0xFFFF)
                seg_add_line(rbsp, &segs[si]);
        }
        return;
    }
#endif
    {
    subsector_t *sub = &subsectors[num];
    seg_t       *line;
    int          count;

    rbsp->curfsector = &sectors[sub->sector];
#ifndef SATURN_SPRITES_OFF
    seg_collect_sector_sprites(sub->sector);
#endif
    line  = &segs[sub->firstline];
    count = SSEC_NUMLINES(sub);
    while (--count >= 0)
        seg_add_line(rbsp, line++);
    }
}

/* Round-43: FRT ticks elapsed in the frozen-vblank window when the
 * kind-4 dead-chain check trips (set at either trip site below; read
 * by the verdict record). Decides spurious-threshold vs real chain
 * death: trip/85 = seconds if the real-HW-like rate holds, trip/21536
 * = seconds at the join-poll accumulator's implied Kronos rate. */
volatile unsigned int saturn_dc_trip_frt = 0;
/* Round-46: which call site fired (1 = BSP build walk, 2 = SAT_BAIL_
 * CHECK macro), plus the firing core's live mirror view of the vbl
 * counter and the arm value — the record compares these against the
 * master's own cached count to split "publish broken" (master count
 * advanced, slave view frozen) from "ISR starved" (both frozen). */
volatile unsigned int saturn_dc_site     = 0;
volatile unsigned int saturn_dc_seen_vbl = 0;
volatile unsigned int saturn_dc_seen_arm = 0;

/* Round-49: the one honest clock when the vblank chain dies. */
static unsigned short saturn_frt16(void)
{
    return *(volatile unsigned short *)0xFFFFFE12u;
}

/* Round-44: shared kind-4 evaluator, forward-declared for the BSP
 * build check above; defined next to the mirror pointers below. */
static int saturn_dc_check(int site);
/* Round-49: FRT-accumulator bail evaluators (defined below, next to
 * the mirror block); forward-declared for the BSP build check. */
static int saturn_build_frt_over(void);

static void seg_decode_bbox(int16_t *bbox, const int16_t *outerbbox,
                             uint16_t encbbox)
{
    uint16_t l;

    l = outerbbox[BOXTOP] - outerbbox[BOXBOTTOM];
    bbox[BOXBOTTOM] = outerbbox[BOXBOTTOM] + ((l * (encbbox & 0x0f)) >> 4);
    encbbox >>= 4;
    bbox[BOXTOP] = outerbbox[BOXTOP] - ((l * (encbbox & 0x0f)) >> 4);
    encbbox >>= 4;

    l = outerbbox[BOXRIGHT] - outerbbox[BOXLEFT];
    bbox[BOXLEFT] = outerbbox[BOXLEFT] + ((l * (encbbox & 0x0f)) >> 4);
    encbbox >>= 4;
    bbox[BOXRIGHT] = outerbbox[BOXRIGHT] - ((l * (encbbox & 0xf)) >> 4);
}

/* Round-40 forward externs (the round-40 extern block sits further
 * down, below this function; the BSP recursion needs them here). */
extern volatile unsigned int saturn_build_bail_vbl;
extern volatile unsigned int saturn_bail_arm_vbl;
extern volatile unsigned int saturn_bail_arm_frt;
extern volatile unsigned int saturn_vbl_count;
extern volatile int          saturn_seg_bailed;
extern volatile unsigned int saturn_master_beat;   /* round-50 ISR probe */
extern volatile unsigned int saturn_master_site;

static void seg_render_bsp_node(segbsp_t *rbsp, int bspnum, int16_t *outerbbox)
{
    node_t *bsp;
    int     i, side;
    int16_t bbox[2][4];

    /* Round-40: the build walk is a RECURSION with no depth bound — a
     * corrupt node chain here is an unbounded master-stack dive (the
     * silent freeze this round guards against). One compare per node;
     * a bail unwinds by returning at every level. */
    if (saturn_build_bail_vbl != 0u
        && (saturn_vbl_count > saturn_build_bail_vbl
            || saturn_build_frt_over()))  /* round-49: FRT clock */
    {
        saturn_seg_bailed = 3;
        return;
    }
    if (saturn_dc_check(1))  /* round-44: mirror-read kind-4 evaluator */
        return;

    if ((int16_t)bspnum < 0)    /* MARS leaf encoding */
    {
        seg_subsector(rbsp, bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
        return;
    }

    bsp = &nodes[bspnum];

    side = R_PointOnSide(seg_viewx, seg_viewy, bsp);
    for (i = 0; i < 2; i++)
        seg_decode_bbox(bbox[i], outerbbox, bsp->encbbox[i]);

    seg_render_bsp_node(rbsp, bsp->children[side], bbox[side]);

    side ^= 1;
    if (seg_check_bbox(rbsp, bbox[side]))
        seg_render_bsp_node(rbsp, bsp->children[side], bbox[side]);
}

/*----- tables -----*/

static void seg_build_tables(void)
{
    /* d32xr R_InitMathTables, viewportWidth = SCREEN_W, centerX =
     * SCREEN_W/2, FIELDOFVIEW = 2048 fine angles. tempviewangletox
     * needs 16 KB â€” zone-alloc'd and deliberately leaked (the zone is
     * 2 MB of cart; the table work runs once per power-on). */
    int    *temp = (int *)Z_Malloc(sizeof(int) * (FINEANGLES / 2),
                                   PU_STATIC);
    fixed_t focalLength;
    fixed_t centerXFrac = (SCREEN_W / 2) * FRACUNIT;
    int     i, x;
    byte   *blob;

    focalLength = FixedDiv(centerXFrac,
                           finetangent(FINEANGLES / 4 + FIELDOFVIEW / 2));

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        fixed_t t;

        if (finetangent(i) > FRACUNIT * 2)
            t = -1;
        else if (finetangent(i) < -FRACUNIT * 2)
            t = SCREEN_W + 1;
        else
        {
            t = FixedMul(finetangent(i), focalLength);
            t = (centerXFrac - t + FRACUNIT - 1) >> FRACBITS;
            if (t < -1)
                t = -1;
            else if (t > SCREEN_W + 1)
                t = SCREEN_W + 1;
        }
        temp[i] = t;
    }

    for (i = 0; i <= SCREEN_W; i++)
    {
        for (x = 0; temp[x] > i; x++)
            ;
        seg_xtoviewangle[i] = (uint16_t)(((x << ANGLETOFINESHIFT) - ANG90)
                                         >> FRACBITS);
    }

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        if (temp[i] == -1)
            temp[i] = 0;
        else if (temp[i] == SCREEN_W + 1)
            temp[i] = SCREEN_W;
        seg_viewangletox[i] = (short)temp[i];
    }

    seg_clipangle = (unsigned)seg_xtoviewangle[0] << FRACBITS;

    /* d32xr's checkcoord, into the zone */
    {
        static const int cc[12][4] =
        {
            { 3, 0, 2, 1 },
            { 3, 0, 2, 0 },
            { 3, 1, 2, 0 },
            { 0, 0, 0, 0 },
            { 2, 0, 2, 1 },
            { 0, 0, 0, 0 },
            { 3, 1, 3, 0 },
            { 0, 0, 0, 0 },
            { 2, 0, 3, 1 },
            { 2, 1, 3, 1 },
            { 2, 1, 3, 0 },
            { 0, 0, 0, 0 }
        };

        blob = (byte *)Z_Malloc(12 * 4 * sizeof(int) + sizeof(sector_t),
                                PU_STATIC);
        seg_checkcoord   = (int *)blob;
        memcpy(seg_checkcoord, cc, sizeof(cc));
        blob += 12 * 4 * sizeof(int);
        seg_emptysector = (sector_t *)blob;
        memset(seg_emptysector, 0, sizeof(sector_t));
        seg_emptysector->floorpic    = 0xfe;
        seg_emptysector->ceilingpic  = 0xfe;
        seg_emptysector->lightlevel  = 0xfe;
    }
}

/*----- public entry points -----*/

#ifdef SATURN_VDP1_WALL
#ifdef SATURN_V1W_ALL
/* P5 Phase 2b: forward decl — the capture pass is defined after
 * seg_column_body (it calls the shared per-column helper) but fires
 * at the end of this build. */
static void saturn_v1w_capture_pass(void);
#endif
#endif

void saturn_segs_build(sector_t *player_sec)
{
    segcliprange_t solidsegs[MAXSEGS];
    segbsp_t       rbsp;
    static int     seg_dbg_marks = 0;

    seg_viewsec_i = (short)(player_sec - sectors);

#ifdef SATURN_VDP1_WALL
    /* VDP1 wall prototype: fresh quad table each frame (runs on the
     * master before both render halves dispatch). */
    Saturn_Vdp1Wall_Reset();
    saturn_v1w_captured = 0;
#endif

    /* Freeze localizers for the first 4 frames (same pattern as the
     * R1/R2/R3 markers in D_DoomLoop): W1 = build entered, W2 = tables
     * built, W3 = BSP walk returned. The last marker on screen names
     * the stage that never finished. */
#define SEGS_DBG(msg)                                   \
    do                                                  \
    {                                                   \
        if (seg_dbg_marks < 4)                          \
        {                                               \
            extern void Saturn_BootMsg(const char *);   \
                                                    \
            Saturn_BootMsg(msg);                        \
            seg_dbg_marks++;                            \
        }                                               \
    } while (0)

    SEGS_DBG("W1");

    if (!seg_inited)
    {
        extern void saturn_rtbl_fence_register(unsigned int *);

        unsigned int sz;

        seg_viewangletox = (short *)Z_Malloc(FINEANGLES / 2 * sizeof(short),
                                             PU_STATIC);
        seg_xtoviewangle = (uint16_t *)Z_Malloc(
            (SCREEN_W + 1) * sizeof(uint16_t), PU_STATIC);
        /* Round-41: each block padded +16 so a 4-word canary fence
         * sits past its tail (ids 15..19 follow the rtbl_z tables —
         * see saturn_render.c's fence map). */
        sz = (unsigned int)(SEGW_MAX * sizeof(segwall_t));
        segwalls = (segwall_t *)Z_Malloc((int)(sz + 16u), PU_STATIC);
        saturn_rtbl_fence_register((unsigned int *)
                                   ((char *)segwalls
                                    + ((sz + 3u) & ~3u)));
        /* sprite tables (task #16) â€” same zone treatment: BSS is too
         * tight for a 48-entry vissprite array + 2x2 per-cpu clip
         * planes */
        /* Sprite array at the d32xr ceiling (165 = MAXVISSPRITES) in
         * WRAM — the zone has no headroom for the extra ~5 KB (it sits
         * 80 bytes under its limit). Zone fallback = the historical
         * 48-entry size. */
        sz = (unsigned int)(SPRVIS_MAX_HRAM * sizeof(sprvis_t));
        spr_vis = (sprvis_t *)Saturn_HramAlloc((int)sz);
        if (spr_vis)
        {
            spr_vis_max = SPRVIS_MAX_HRAM;
        }
        else
        {
            sz = (unsigned int)(SPRVIS_MAX * sizeof(sprvis_t));
            spr_vis = (sprvis_t *)Z_Malloc((int)(sz + 16u), PU_STATIC);
            spr_vis_max = SPRVIS_MAX;
        }
        saturn_rtbl_fence_register((unsigned int *)
                                   ((char *)spr_vis
                                    + ((sz + 3u) & ~3u)));
        sz = (unsigned int)(2 * (SCREEN_W + 1) * sizeof(short));
        spr_ctop = (short (*)[SCREEN_W + 1])Z_Malloc((int)(sz + 16u),
                                                     PU_STATIC);
        saturn_rtbl_fence_register((unsigned int *)
                                   ((char *)spr_ctop
                                    + ((sz + 3u) & ~3u)));
        spr_cbot = (short (*)[SCREEN_W + 1])Z_Malloc((int)(sz + 16u),
                                                     PU_STATIC);
        saturn_rtbl_fence_register((unsigned int *)
                                   ((char *)spr_cbot
                                    + ((sz + 3u) & ~3u)));
        saturn_rtbl_fence_register(seg_mmid_guard);
        /* Occlusion guard arrays (2026-09-07): same HRAM treatment as
         * spr_vis[] — the capture gate and preskip read them, so an
         * allocation failure disables VDP1 capture entirely (the
         * software paint stays exact either way). */
        sz = (unsigned int)(SCREEN_W * (sizeof(int) + 2 * sizeof(short)));
        v1w_spr_ymax = (int *)Saturn_HramAlloc((int)sz);
        if (!v1w_spr_ymax)
            v1w_spr_ymax = (int *)Z_Malloc((int)(sz + 16u), PU_STATIC);
        if (v1w_spr_ymax)
        {
            v1w_spr_top = (short *)(v1w_spr_ymax + SCREEN_W);
            v1w_spr_bot = v1w_spr_top + SCREEN_W;
            saturn_rtbl_fence_register((unsigned int *)
                ((char *)v1w_spr_ymax + ((sz + 3u) & ~3u)));
        }
        seg_build_tables();
        seg_inited = 1;
    }

    SEGS_DBG("W2");

    /* camera (same fields saturn_render.c's prologue reads) */
    seg_viewx     = players[0].mo->x;
    seg_viewy     = players[0].mo->y;
    seg_viewz     = players[0].viewz;
    seg_viewangle = players[0].mo->angle;
    seg_viewcos   = finecosine(seg_viewangle >> ANGLETOFINESHIFT);
    seg_viewsin   = finesine(seg_viewangle >> ANGLETOFINESHIFT);

    segwalls_count    = 0;
    saturn_seg_caphits = 0;
    saturn_v1w_skipcols = 0;
    saturn_v1w_skipdie = 0;
    saturn_v1w_commits = 0;
    saturn_v1w_kills = 0;
    saturn_v1w_duds = 0;
    saturn_v1w_skipmid = 0;
    saturn_v1w_midcols = 0;
    saturn_v1w_midquads = 0;
    /* 2026-09-08 CENSUS v3: CUMULATIVE — the per-frame memset is gone.
     * The attract demo's view changes every second, so a snapshot HUD
     * frame only saw one frame's rejections; accumulating over the
     * session makes ANY readable frame show the dominant blocker.
     * (Counts saturate harmlessly: they only ever select a max.) */
    /* memset(saturn_v1w_rj, 0, sizeof(saturn_v1w_rj)); */

    /* SW1COMP face probe: fresh counters each frame, texture number
     * resolved once textures[] exists (R_TextureNumForName = -1/0 on a
     * miss, both fail the b_tex == test against a real texnum). */
    if (saturn_sw1_tex < 0)
        saturn_sw1_tex = R_TextureNumForName("SW1COMP");
    saturn_sw1_built = 0;
    saturn_sw1_ok    = 0;
    saturn_sw1_paint = 0;

#ifndef SATURN_SPRITES_OFF
    spr_vis_count = 0;
    if (validcount)
        validcount[0]++;    /* per-frame collection stamp (d32xr) */
#endif

    solidsegs[0].first = -2;
    solidsegs[0].last  = -1;
    solidsegs[1].first = SCREEN_W;
    solidsegs[1].last  = SCREEN_W + 1;
    rbsp.solidsegs  = solidsegs;
    rbsp.newend     = &solidsegs[2];
    rbsp.splitspans = SCREEN_W + SCREEN_W / 2;
    rbsp.lastv1     = -1;
    rbsp.lastv2     = -1;
    rbsp.lastwall   = segwalls;

#ifdef SATURN_PVS
    /* P4: fetch this frame's PVS row (locate cost is inside the D-row
     * FRT window below — honest). Any surprise = NULL = ungated walk. */
    rbsp.pvs_skip = 0;
    rbsp.pvs_row  = NULL;
    if (pvs_blob != NULL)
    {
        int leaf = seg_pvs_locate(seg_viewx, seg_viewy);

        if (leaf >= 0 && leaf < pvs_nssec)
            rbsp.pvs_row = pvs_blob + (unsigned)leaf * pvs_rowbytes;
    }
#endif

    {
        /* D row (2026-09-08 STAGE 1): traversal-only window — walk +
         * clip + viswall build, everything downstream (paint, merge,
         * flats) excluded. Sub-vbl delta; saturn_bsp_vbls flags a
         * vblank crossing (then the tick delta is the wrapped tail).
         * (Raw TCNT read — SEG_SAT_FRT() is defined further down.) */
        unsigned short tb0 = *(volatile unsigned short *)0xFFFFFE12u;
        unsigned int   vb0 = saturn_vbl_count;

        seg_render_bsp_node(&rbsp, numnodes - 1, worldbbox);

        saturn_frt_bsp  = (int)((unsigned short)
                            (*(volatile unsigned short *)0xFFFFFE12u - tb0)
                            >> 8);
        saturn_bsp_vbls = (int)(saturn_vbl_count - vb0);
    }

    SEGS_DBG("W3");

    segwalls_count = rbsp.lastwall - segwalls;

    /* things/sprites: collection happened at the BSP leaves
     * (seg_collect_sector_sprites); sort far-to-near while the master
     * still owns the bus, so both cores see the finished spr_vis[]
     * after the dispatch purge (same contract as segwalls[]).
     * DIAGNOSTIC: build with -DSATURN_SPRITES_OFF to render the exact
     * pre-sprite wall image (spr_vis_count stays 0, so the preload and
     * draw passes no-op too) â€” isolates sprite-pass damage from
     * pre-existing seg-renderer wall bugs. */
#ifndef SATURN_SPRITES_OFF
    seg_sort_sprites();
#endif

#ifdef SATURN_VDP1_WALL
    /* Occlusion guard fill (2026-09-07, master, once per frame, after
     * the BSP walk collected spr_vis[] and seg_sort_sprites ordered it
     * — the render halves only READ these): per column, the max
     * sprite scale and the union row span. Depth clamps mirror the
     * draw path's row0f clamp (FixedMul wrap protection); the result
     * rows are clamped to a sane screen-adjacent range so shorts hold
     * them. The player weapon rect (published by LAST frame's
     * Saturn_PSprite_Draw — it draws after this render half) is folded
     * in with its own pass: it is never wall-clipped, paints over
     * everything below the view top, so its scale is "infinite". */
    if (v1w_spr_ymax)
    {
        int x, si;

        for (x = 0; x < SCREEN_W; x++)
        {
            v1w_spr_ymax[x] = 0;
            v1w_spr_top[x]  = 32767;
            v1w_spr_bot[x]  = -32768;
        }
#ifndef SATURN_SPRITES_OFF
        for (si = 0; si < spr_vis_count; si++)
        {
            sprvis_t *sv = &spr_vis[si];
            fixed_t   dz;
            int       s_top, s_bot, sx0, sx1;

            dz = sv->gzt - seg_viewz;
            if (dz > (400 << FRACBITS))
                dz = 400 << FRACBITS;
            else if (dz < -(400 << FRACBITS))
                dz = -(400 << FRACBITS);
            s_top = HALF_H - (FixedMul(dz, sv->yscale) >> FRACBITS);
            s_bot = s_top + ((sv->h * sv->yscale) >> FRACBITS) + 1;
            if (s_top < -500) s_top = -500;
            if (s_bot > 1100) s_bot = 1100;
            sx0 = sv->x1 < 0 ? 0 : sv->x1;
            sx1 = sv->x2 >= SCREEN_W ? SCREEN_W - 1 : sv->x2;
            for (x = sx0; x <= sx1; x++)
            {
                if (sv->yscale > v1w_spr_ymax[x])
                    v1w_spr_ymax[x] = sv->yscale;
                if (s_top < v1w_spr_top[x])
                    v1w_spr_top[x] = (short)s_top;
                if (s_bot > v1w_spr_bot[x])
                    v1w_spr_bot[x] = (short)s_bot;
            }
        }
#endif
        if (saturn_gun_x1 >= saturn_gun_x0)
        {
            int gx0 = saturn_gun_x0 < 0 ? 0 : saturn_gun_x0;
            int gx1 = saturn_gun_x1 >= SCREEN_W ? SCREEN_W - 1
                                                : saturn_gun_x1;
            int gy0 = saturn_gun_y0 < -500 ? -500 : saturn_gun_y0;
            int gy1 = saturn_gun_y1 > 1100 ? 1100 : saturn_gun_y1;

            for (x = gx0; x <= gx1; x++)
            {
                if (v1w_spr_ymax[x] < 0x7FFFFFFF)
                    v1w_spr_ymax[x] = 0x7FFFFFFF;
                if (gy0 < v1w_spr_top[x]) v1w_spr_top[x] = (short)gy0;
                if (gy1 > v1w_spr_bot[x]) v1w_spr_bot[x] = (short)gy1;
            }
        }
    }

#ifdef SATURN_V1W_ALL
    /* P5 Phase 2b: the capture pass runs HERE — after the occlusion
     * guard fill (sprite veto data ready), before preload/v1s_prep
     * and the halves dispatch. Full-frame skip map + seg_mmid are
     * complete by the time either core starts painting. */
    saturn_v1w_capture_pass();
#endif
#endif
}

/*----- P4: per-level PVS blob load (called from P_SetupLevel) -----
 *
 * lumpname == NULL (call at P_SetupLevel entry): drop any previous
 * level's row pointer — closes the stale-pointer window between the
 * previous level's Z_FreeTags teardown and this level's map-struct
 * load. Otherwise: E1M1 only, one-shot synchronous GFS read of
 * cd\E1M1.BLOB into a PU_LEVEL zone block, header + count validation
 * against the just-loaded map. ANY failure leaves the state nulled =
 * the plain BSP walk (fail-open; boot-to-title never depends on this).
 *
 * .bss cost = the three statics above (8 bytes) — the blob itself is
 * zone memory and the per-frame row pointer lives in segbsp_t. */
extern int Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname);

int Saturn_Pvs_Parse(byte *blob, unsigned fsize)
{
#ifdef SATURN_PVS
    unsigned nssec, rowbytes, need;

    if (!blob || fsize <= 16)
        return 0;

    /* Header: 'PVS1' magic, u16 ver @4, u16 nssec @6, u16 rowbytes @8, u32 res @10 */
    if (blob[0] != 'P' || blob[1] != 'V' || blob[2] != 'S' || blob[3] != '1'
        || (((unsigned)blob[4] << 8) | blob[5]) != 1)
        return 0;

    nssec    = ((unsigned)blob[6] << 8) | blob[7];
    rowbytes = ((unsigned)blob[8] << 8) | blob[9];

    if (nssec != (unsigned)numsubsectors || rowbytes != ((nssec + 7) >> 3))
        return 0;

    need = 16u + nssec * rowbytes;
    if (need > (unsigned)fsize)
        return 0;

    pvs_blob     = blob + 16;
    pvs_nssec    = (unsigned short)nssec;
    pvs_rowbytes = (unsigned short)rowbytes;
    return 1;
#else
    (void)blob;
    (void)fsize;
    return 0;
#endif
}

void Saturn_PvsLevelLoad(const char *lumpname)
{
#ifdef SATURN_PVS
    pvs_blob     = NULL;
    pvs_nssec    = 0;
    pvs_rowbytes = 0;

    if (lumpname == NULL)
        return;
    if (netgame)                /* rows are keyed on players[0] only */
        return;
    {
        char     fname[16];
        int      k;
        byte    *blob;
        int      fsize;

        for (k = 0; k < 8 && lumpname[k]; k++)
        {
            char c = lumpname[k];
            if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
            fname[k] = c;
        }
        fname[k++] = '.';
        fname[k++] = 'P';
        fname[k++] = 'V';
        fname[k++] = 'S';
        fname[k]   = '\0';

        /* Buffer is 128 KB in PU_LEVEL zone memory (max map PVS is E4M9 at 115 KB) */
        blob = (byte *)Z_Malloc(131072, PU_LEVEL);
        if (blob == NULL)
            return;
        fsize = Saturn_Leaf_BlobRead(blob, 131072, fname);
        if (fsize <= 16 || !Saturn_Pvs_Parse(blob, (unsigned)fsize))
        {
            Z_Free(blob);
            return;
        }
    }
#else
    (void)lumpname;
#endif
}

#ifdef SATURN_VDP1_WALL
#ifndef SATURN_SPRITES_OFF
/*----- stage 3a (2026-09-08): fully-visible sprites -> VDP1 quads -----
 * Master-only, once per frame, after the BSP walk has collected and
 * seg_sort_sprites has ordered spr_vis[] (saturn_segs_build) and the
 * sprite pixels are converted (preload walk above); runs BEFORE the
 * dispatch purge so both halves see the skip decision. The quads land
 * at the HEAD of the near-to-far table (Reset already emptied it in
 * the build), i.e. they are written LAST by the command writer and
 * draw ON TOP of every wall quad — correct because the conversion
 * criterion below guarantees every wall piece at the sprite's columns
 * is entirely FARTHER (nearer pieces keep the sprite in software), so
 * the only quads sharing its pixels are behind it. Sprite-vs-sprite
 * order comes from the iteration: near-to-far (reverse of the sorted
 * array), and the psprite/flash pair is added first of all (absolute
 * nearest, flash after weapon in software = flash added before it
 * here, so the flash draws on top).
 *
 * Conversion criterion (conservative, bit-exact by construction): the
 * software sprite pass clips each sprite per column against nearer
 * wall pieces (otop/obot); a VDP1 quad cannot reproduce a per-column
 * clip, and the VDP1 layer sits ABOVE the NBG0 software paint — so
 * ANY wall piece that is nearer (or straddles) at the sprite's
 * on-screen columns and is not behind the thing's map point keeps the
 * sprite in software, even a silhouette-less open band (its top/
 * bottom faces may become quads that would cover the sprite). This
 * is saturn_v1s_occluded — the same piece depth test as the draw
 * path's clip loop, with the silhouette test replaced by "any".
 *
 * P5 Phase 3 (2026-09-10, verdict C): masked-mid segs are segwall
 * entries too, so a sprite BEHIND a grate — quad'd or software —
 * already fails this test (the grate piece is nearer and not behind
 * the thing's map point) and keeps the software paint, under the
 * VDP1 layer where the grate's opaque texels cover it and its
 * PMOD-transparent holes reveal it. No per-column seg_mmid scan is
 * needed here; the piece test is a superset (80-col endpoint scales
 * are exact maxima for a monotone scalestep). Reason-18 in gate_m is
 * the mirror direction (a NEARER sprite keeps the grate software).
 *
 * Skip mechanism (zero new state): a converted sprite sets
 * sv->x1 = 1, sv->x2 = 0 — both halves' seg_draw_sprites compute
 * xs > xe and skip it, exactly like an off-screen sprite. Collection
 * refills x1/x2 next frame. The v1w_spr_* occlusion-guard fill ran
 * in the build with the ORIGINAL x1/x2, so the wall gate's r18 veto
 * still sees converted sprites (conservative and still correct).
 *
 * Preskip safety: the wall preskip's CanAdd sees the sprite cells in
 * v1q_count, so a table crowded by sprites simply keeps more walls in
 * software (paint intact, no holes).
 *
 * Texture: v1_spr_get uploads the composited patch padded to a
 * multiple of 8 texels into the sprite VRAM classes (saturn_vdp1.c);
 * shade bank = the wall scheme ((sh+2)>>2, bank 0 = fullbright).
 *
 * NO NEW .BSS (2026-09-08): ANY .bss growth shifts the SGL lib-common
 * work cells and wedges the boot (the +28B and -4096 experiments), and
 * every saturn_v1w_rj slot is spoken for (gate reasons 7-10, drop 13,
 * gate_m 14-19). Verification signal = the existing P row (v1q table
 * size): walls alone sit at ~3, sprite quads push it to ~20-30. The
 * psprite skip flag lives in the quad's own flags word (bit 1), so it
 * costs nothing either. */

static int saturn_v1s_occluded(sprvis_t *sv, int xs, int xe)
{
    int j;

    for (j = 0; j < segwalls_count; j++)
    {
        segwall_t *w = &segwalls[j];
        int p0, pe;

        if (w->start > xe || w->stop < xs)
            continue;
        /* 80-column pieces, endpoint scales — R_ClipVisSprite's
         * per-piece depth test (the draw path's clip loop) */
        for (p0 = w->start; p0 <= w->stop; p0 = pe + 1)
        {
            int s1, s2, lo, hi, r1, r2;

            pe = p0 + 79;
            if (pe > w->stop)
                pe = w->stop;
            s1 = w->scalefrac + (p0 - w->start) * w->scalestep;
            s2 = w->scalefrac + (pe - w->start) * w->scalestep;
            lo = imin(s1, s2);
            hi = imax(s1, s2);
            if (hi < sv->yscale)
                continue;       /* entirely farther: cannot occlude  */
            if (lo <= sv->yscale
                && seg_spr_behind(w, sv->gx, sv->gy))
                continue;       /* straddling, but behind the thing  */
            r1 = p0 < xs ? xs : p0;
            r2 = pe > xe ? xe : pe;
            if (r1 <= r2)
                return 1;       /* nearer and not behind: occluded   */
        }
    }
    return 0;
}

static void saturn_v1s_prep(void)
{
    int i;

    /*---- player weapon + flash: quads at the head of the table ----
     * All-or-nothing across the pair (a software flash paints into
     * NBG0 UNDER the weapon quad, so converting one and not the other
     * would hide the flash); any failure rolls both back. Count DOWN:
     * the table draws higher indices first, so flash (added first,
     * lower index) lands ON TOP of the weapon — the same order the
     * software paint produces (weapon, then flash over it). */
    {
        player_t *p = &players[consoleplayer];
        int base = Saturn_Vdp1Wall_Base();
        int ok   = 1;

        if (!spriteframes || !sprites || !p || !p->mo)
            ok = 0;
        for (i = NUMPSPRITES - 1; ok && i >= 0; i--)
        {
            pspdef_t           *psp = &p->psprites[i];
            const state_t      *st;
            const spritedef_t  *sprdef;
            spriteframe_t      *sf;
            unsigned            lumpv;
            int lump, scache, w, h, lofs, tofs;
            int x1, y0, r0, r1, sh, pw, xA, xB;

            if (!psp->state || (unsigned)psp->state >= NUMSTATES)        /* S_NULL = inactive slot     */
                continue;
            st = &states[psp->state];
            if (st->sprite < 0 || st->sprite >= NUMSPRITES)
                continue;
            sprdef = &sprites[st->sprite];
            if (sprdef->numframes <= 0)
                continue;
            if ((st->frame & FF_FRAMEMASK) >= sprdef->numframes)
                continue;
            sf = &spriteframes[sprdef->firstframe
                               + (st->frame & FF_FRAMEMASK)];
            lumpv = (unsigned short)spritelumps[sf->lump];
            lump = (int)(lumpv & SL_LUMPMASK);
            if (lump < firstsprite || lump >= firstsprite + numsprites)
                continue;
            scache = lump - firstsprite;
            if (!Saturn_SpritePatch(scache, &w, &h, &lofs, &tofs))
                continue;
            if (w < 1 || h < 1)
                continue;
            x1 = (psp->sx >> FRACBITS) - lofs;
            y0 = 2 + (psp->sy >> FRACBITS) - tofs;
            r0 = 0;
            r1 = h - 1;
            if (y0 + r0 < VIEW_TOP) r0 = VIEW_TOP - y0;
            if (y0 + r1 >= VIEW_BOT) r1 = VIEW_BOT - 1 - y0;
            if (r0 > r1)
                continue;
            sh = Saturn_LightRow(
                sectors[p->mo->subsector->sector].lightlevel, 0);
            if (st->frame & FF_FULLBRIGHT)
                sh = 0;
            pw = (w + 7) & ~7;
            xA = x1;
            xB = x1 + pw - 1;   /* unflipped: pad extends right     */
            if (xA < -500 || xA > 1023 || xB < -500 || xB > 1023
                || y0 + r0 < -500 || y0 + r0 > 1023
                || y0 + r1 + 1 < -500 || y0 + r1 + 1 > 1023)
            {
                ok = 0;         /* off the VDP1's safe coordinates  */
                break;
            }
            if (!Saturn_Vdp1Spr_Add(scache, xA, xB,
                                    y0 + r0, y0 + r1 + 1,
                                    r0, r1 - r0 + 1, sh, 1))
            {
                ok = 0;         /* texture/class/table refuse       */
                break;
            }
        }
        if (!ok)
            Saturn_Vdp1Wall_Restore(base);
        /* mark flag lives in the quads' flags word; the software pass
         * asks Saturn_Vdp1Quad_PspMarked() (nothing to reset here —
         * the table is rebuilt from zero every frame) */
    }

    /*---- world sprites, near-to-far (reverse of the sorted array) ----*/
    for (i = spr_vis_count - 1; i >= 0; i--)
    {
        sprvis_t *sv = &spr_vis[i];
        fixed_t   scale_h, dz, row0f;
        int64_t   v64;
        int xs, xe, r0, r1, yt, yb, pw, wpx, xA, xB, x2t;

        if (sv->x1 > sv->x2)
            continue;
        xs = sv->x1 < 0 ? 0 : sv->x1;
        xe = sv->x2 >= SCREEN_W ? SCREEN_W - 1 : sv->x2;
        if (xs > xe)
            continue;
        if (saturn_v1s_occluded(sv, xs, xe))
            continue;               /* nearer wall piece: software   */
        if (sv->w > 128 || sv->h > 255)
            continue;               /* over the texture classes      */
        scale_h = FixedDiv(PROJ_X << FRACBITS, sv->dist);
        /* row0f: the draw path's own projection, with its clamps */
        dz = sv->gzt - seg_viewz;
        if (dz > (400 << FRACBITS))
            dz = 400 << FRACBITS;
        else if (dz < -(400 << FRACBITS))
            dz = -(400 << FRACBITS);
        row0f = (HALF_H << FRACBITS) - FixedMul(dz, sv->yscale);
        if (row0f > (200 << FRACBITS))
            row0f = 200 << FRACBITS;
        else if (row0f < -(200 << FRACBITS))
            row0f = -(200 << FRACBITS);
        /* visible texel rows: first at VIEW_TOP, one past the last at
         * VIEW_BOT+1 (same 64-bit intermediate as the draw path)    */
        v64 = (((int64_t)VIEW_TOP << 16) - row0f)
            * (int64_t)sv->vstep >> 16;
        r0 = v64 < 0 ? 0 : (int)(v64 >> 16);
        v64 = (((int64_t)(VIEW_BOT + 1) << 16) - row0f)
            * (int64_t)sv->vstep >> 16;
        r1 = (int)(v64 >> 16);
        if (r1 > sv->h - 1)
            r1 = sv->h - 1;
        if (r0 > r1)
            continue;               /* fully above/below the view    */
        yt = (int)((row0f + (int64_t)r0 * sv->vstep) >> 16);
        yb = (int)((row0f + (int64_t)(r1 + 1) * sv->vstep) >> 16);
        /* padded width in screen pixels; the quad extends over the
         * pad columns (transparent). xA = the texel-0 edge.         */
        pw  = (sv->w + 7) & ~7;
        wpx = (pw * scale_h) >> FRACBITS;
        x2t = sv->xl + ((sv->w * scale_h) >> FRACBITS) - 1;
        xA = sv->flip ? x2t : sv->xl;
        xB = sv->flip ? xA - wpx + 1 : xA + wpx - 1;
        if (xA < -500 || xA > 1023 || xB < -500 || xB > 1023
            || yt < -500 || yt > 1023 || yb < -500 || yb > 1023)
            continue;               /* near/tall: outside the VDP1's
                                       safe coordinate range         */
        if (Saturn_Vdp1Spr_Add(sv->scache, xA, xB, yt, yb,
                               r0, r1 - r0 + 1, sv->sh, 0))
        {
            sv->x1 = 1;             /* both halves skip the software */
            sv->x2 = 0;             /* paint (their own xs > xe test)*/
        }
    }
}
#endif /* !SATURN_SPRITES_OFF */
#endif /* SATURN_VDP1_WALL */

/* SH-2 divide unit registers (d32xr 32x.h:143-146; the SH7604 core in
 * both Saturn SH-2s has the same unit). Writing DVDNT starts a signed
 * 32/32 divide; DVDNTH then DVDNTL starts a signed 64/32 divide. The
 * quotient is read back from the same register ~13-16 cycles later. */
#define SATURN_DIVU_DVSR   (*(volatile int *)0xFFFFFF00u)
#define SATURN_DIVU_DVDNT  (*(volatile int *)0xFFFFFF04u)
#define SATURN_DIVU_DVDNTH (*(volatile int *)0xFFFFFF10u)
#define SATURN_DIVU_DVDNTL (*(volatile int *)0xFFFFFF14u)

/* Red-strip fix (2026-09-02): the raycaster era warmed textures through
 * the sweep's per-column candidate cache, and that machinery is compiled
 * out under SATURN_SEG_RENDERER. The per-line preload walk only re-runs
 * while saturn_tex_dirty re-arms it, so a wall texture that got evicted
 * â€” or never composited â€” stayed missing: the slave's
 * Saturn_TexInfoNoLoad cannot composite on miss, and paint_wall_span
 * filled the span with the solid fallback_col (the full-height red
 * strips on the slave's half). Walk the finished viswall list here on
 * the MASTER, every frame before the dispatch purge; a cached texture
 * is one lookup, so the steady-state cost is a few hundred cycles. */
void saturn_segs_preload_textures(void)
{
    int i;

    for (i = 0; i < segwalls_count; i++)
    {
        segwall_t *w = &segwalls[i];

        /* Round-52 net 9: preload FRT acc — the reload-storm theory
         * makes THIS loop the crawl (every wall texture re-fetched
         * through the CD/cart path each frame). */
        SAT_FRT_NET(saturn_pre_frt_acc, saturn_pre_frt_last, 9);

        if (w->t_tex > 0) (void)Saturn_TexData(w->t_tex);
        if (w->b_tex > 0) (void)Saturn_TexData(w->b_tex);
        if (w->m_tex > 0) (void)Saturn_TexData(w->m_tex);

        /* SW1COMP face probe: does the preload's TexData actually hand
         * back pixels for the switch texture? */
        if (w->b_tex == saturn_sw1_tex)
        {
            extern byte *Saturn_TexData(int texnum);

            saturn_sw1_ok = (Saturn_TexData(w->b_tex) != 0);
        }
    }

    /* sprite pixels: the slave's Saturn_SpriteDataNoLoad cannot load
     * lumps â€” convert every collected sprite on the master first */
    for (i = 0; i < spr_vis_count; i++)
        (void)Saturn_SpriteData(spr_vis[i].scache);

#ifdef SATURN_VDP1_WALL
#ifndef SATURN_SPRITES_OFF
    /* stage 3a: convert fully-visible sprites to VDP1 quads — AFTER
     * the conversions above (the texture upload reads them) and
     * before the dispatch purge (both halves must see the skip).
     * BUILD B (2026-09-08 teal-wedge bisect): prep call DISABLED;
     * flip the define back to 0 once the culprit is found. */
#if !SATURN_3A_BYPASS
    saturn_v1s_prep();
#endif
#endif
#endif
}

/*----- FRT stage windows (2026-09-02) -----
 * The raycaster's stage accumulators (saturn_render.c render_columns)
 * are DEAD CODE under SATURN_SEG_RENDERER â€” the first stage-split build
 * shipped with the T/S/E/W/L rows still fed from that dead path and
 * every row read 0. The rows read THIS file's windows instead: same
 * externs (defined in saturn_render.c), reset + published here at
 * cpu==0, TCNT ticks >>8 (1 printed unit = 256 ticks = 0.198 ms).
 *   saturn_frt_total = whole saturn_segs_render_half body
 *   saturn_frt_fire  = fr_emit_flat staging calls (ceiling/floor bands)
 *   saturn_frt_walk  = paint_wall_span calls (the wall ASM kernel)
 *   saturn_frt_ploop = the flat KERNEL ONLY (paint_flat_run +
 *                      paint_sky_run row loops, master-gated in
 *                      saturn_render.c's frt_ploop_acc; reset/published
 *                      here through saturn_frt_ploop_reset/kernel)
 *   saturn_frt_scan  = macc - fire - walk - ploop (clamped) = flat
 *                      MERGE machinery (flush_column/flush_pending)
 *                      + glue — all sub-vbl accumulators, HW-honest
 *                      (the frt_all_acc section stamps FOLD across
 *                      multi-vbl spans; 2026-09-03 photo S=577214)
 * First live split (J=29152 / J=29952): L dominated T (67%/75%), so
 * L was narrowed to the kernel to split kernel vs merge overhead. */
extern int saturn_frt_fire;
extern int saturn_frt_scan;
extern int saturn_frt_walk;
extern int saturn_frt_total;
extern int saturn_frt_ploop;
/* 2026-09-03 round 7 RED SCREEN probe: the slave publishes its walk
 * progress for the master's watchdog (globals + mirror macro live in
 * saturn_render.c next to saturn_frt_vmerge). */
extern volatile unsigned int saturn_heart_pub[4];
extern int                  saturn_heart_pairs;
/* Round-36: slave self-bail deadline (see saturn_render.c). The slave
 * callback sets it to entry-vbl + 90 before the render half; the wall
 * walk and the final open-band fill below bail out once the uncached
 * vblank count passes it, instead of looping the master's join into a
 * dead console. saturn_seg_bailed tells the callback which phase
 * verdict (DEAD vs A004) to publish. */
extern volatile unsigned int saturn_slave_bail_vbl;
/* Round-40 master-side deadlines + dead-chain detector (see
 * saturn_render.c). saturn_seg_bailed kinds: 1 = slave walk/fill hit
 * its deadline, 2 = MASTER render half bailed, 3 = pre-dispatch BSP
 * build bailed, 4 = vblank chain dead (FRT advanced, vbl frozen). */
extern volatile unsigned int saturn_master_bail_vbl;
extern volatile unsigned int saturn_build_bail_vbl;
extern volatile unsigned int saturn_bail_arm_vbl;
extern volatile unsigned int saturn_bail_arm_frt;
extern volatile unsigned int saturn_vbl_count;
volatile int                 saturn_seg_bailed = 0;
static volatile unsigned int *seg_vbl_mirror =
    (volatile unsigned int *)((unsigned int)&saturn_vbl_count
                              + 0x20000000u);
/* Round-44 ROOT CAUSE (round-43 record: trip = 0xC0AB, threshold +
 * 171 ticks): the kind-4 dead-chain check read saturn_vbl_count and
 * saturn_bail_arm_vbl through direct C reads. On the slave — whose OC
 * is ON at entry and never sees the master's writes (caches don't
 * snoop, the round-34 heart lesson) — both reads returned LAST frame's
 * stale clean lines, so "vbl frozen since arm" was ALWAYS true and the
 * check fired the instant the (uncached, correct) FRT passed 0xC000
 * (~2.3 Kronos vbl) into any walk longer than that. Light sectors
 * finished under 2.3 vbl; the E1M1 flat expanse did not — killed
 * every attempt, the magenta flapping. All three shared variables now
 * go through the P2 mirror; a REAL chain death still trips (vbl stays
 * frozen in uncached memory too). */
static volatile unsigned int *seg_arm_vbl_mirror =
    (volatile unsigned int *)((unsigned int)&saturn_bail_arm_vbl
                              + 0x20000000u);
static volatile unsigned int *seg_arm_frt_mirror =
    (volatile unsigned int *)((unsigned int)&saturn_bail_arm_frt
                              + 0x20000000u);

/* Shared kind-4 evaluator (forward-declared above seg_decode_bbox).
 * Returns 1 and sets kind 4 only on a TRUE frozen-vblank window: the
 * uncached vbl count still equals the arm value while the hardware
 * FRT has run past the 0xC000 threshold. */
static int saturn_dc_check(int site)
{
    /* Round-47: DISARMED. The round-46 stamped views proved the
     * mechanism: on Kronos the slave's WRAM view of saturn_vbl_count
     * freezes at the arm value for 2.3+ vbl stretches during heavy
     * walks (then catches up in bursts) while the master's own count
     * advances normally — so this check fires on EVERY long walk the
     * moment the FRT passes 0xC000, healthy or not (trips 0xC0AB,
     * 0xC57A, 0xC028, 0xC79D — always threshold+overshoot). It is also
     * redundant: the master's join-poll bail (vbl-v0 > 60, master-side
     * cached reads — always live) and the slave's own 180-vbl deadline
     * already break a truly infinite walk. Keep the mirror reads for
     * the record's view stamps; never fire. */
    unsigned int av = *seg_arm_vbl_mirror;
    unsigned int vv = *seg_vbl_mirror;

    saturn_dc_seen_vbl = vv;
    saturn_dc_seen_arm = av;
    /* Round-50: the ISR freeze reporter's heartbeat + fine site. This
     * check runs per BSP-build node (site 1 -> 0x41) and per walk
     * wall-pair (site 2 -> 0x42) on BOTH cores — a wedge inside any
     * single paint call below this granularity stops the beat here. */
    saturn_master_beat++;
    saturn_master_site = 0x40u | (unsigned int)site;
    (void)site;
    return 0;
#if 0
    unsigned int df;

    saturn_dc_seen_vbl = vv;   /* round-46: live view, every call */
    saturn_dc_seen_arm = av;
    if (av == 0u || vv != av)
        return 0;
    df = (unsigned short)(*(volatile unsigned short *)0xFFFFFE12u
                          - *seg_arm_frt_mirror);
    if (df <= 0xC000u)
        return 0;
    saturn_dc_trip_frt = df;
    saturn_dc_site     = site;
    saturn_seg_bailed = 4;
    return 1;
#endif
}
/* Round-34: the published heart is its own cache line, mirror-only —
 * see saturn_render.c (the old saturn_slave_heart shared a line with
 * the slave's cached saturn_heart_pairs and got rolled back by OC
 * write-backs). */
#define SATURN_HEART_MIRROR ((volatile unsigned int *)           \
    ((unsigned int)&saturn_heart_pub[0] + 0x20000000u))
/* Round-39 stack-overflow probe: the slave's deepest SP this frame,
 * sampled at every heart publish and maintained through the P2 mirror
 * (caches don't snoop — a cached write would never reach the master's
 * red-screen readout mid-wedge). If the SGL resident program's slave
 * stack is shallow and the crash spot's walk is the deepest, SP-min
 * dips toward the floor and the wild master-static writes are the
 * stack being pushed through. Reset to 0xFFFFFFFF by the master at
 * each dispatch (saturn_render.c). */
extern volatile unsigned int saturn_slave_sp_min;
#define SAT_SP_MIN_SAMPLE()  do {                                  \
        volatile unsigned int *spp_ = (volatile unsigned int *)     \
            ((unsigned int)&saturn_slave_sp_min + 0x20000000u);     \
        unsigned int spq_;                                          \
        __asm__ __volatile__ ("mov r15,%0" : "=r" (spq_));          \
        if (spq_ < *spp_)                                           \
            *spp_ = spq_;                                           \
    } while (0)
/* Round-40 master bail + dead-chain check (see the externs above).
 * cpu==0 halves stop at their deadline (kind 2); ANY walker stops when
 * the vblank chain is dead — FRT advanced >= 0xC000 ticks (~2.3 vbl on
 * Kronos, ~15 min of real-HW FRT at 55 ticks/vbl... ~15 s — a
 * last-resort clock that works with every interrupt dead) while the
 * vblank count has not moved since the arm (kind 4). */
/* Round-49: FRT-accumulator bail for the MASTER's build (kind 3) and
 * render half (kind 2). Every master-side bail deadline was vbl-based;
 * if the vblank chain dies mid-frame (the round-37 mode) all of them
 * go frozen-blind — the round-47/48 silent freeze at the expanse
 * (identical pixels 60+ s, no red screen, no bkram record) is exactly
 * that: the master wedged with every deadline unreadable. These
 * accumulate poll-style 16-bit FRT sub-deltas across successive check
 * calls (each call << 1 FRT wrap, so no fold) and fire at ~60/30
 * Kronos vbl of FRT time regardless of the vblank chain. Harmless on
 * real HW: the FRT there is per-vblank-phase garbage (~55 ticks/vbl),
 * so the accumulators never reach a threshold sized for Kronos ticks. */
static unsigned short half_frt_last = 0;
static unsigned short build_frt_last = 0;
volatile unsigned int saturn_half_frt_acc = 0;
volatile unsigned int saturn_build_frt_acc = 0;

static int saturn_half_frt_over(void)
{
    unsigned short now = saturn_frt16();

    saturn_half_frt_acc += (unsigned short)(now - half_frt_last);
    half_frt_last = now;
    return saturn_half_frt_acc > 1300000u;   /* ~60 Kronos vbl */
}

static int saturn_build_frt_over(void)
{
    unsigned short now = saturn_frt16();

    saturn_build_frt_acc += (unsigned short)(now - build_frt_last);
    build_frt_last = now;
    return saturn_build_frt_acc > 650000u;   /* ~30 Kronos vbl */
}

void saturn_frt_bail_reset(void)
{
    saturn_half_frt_acc = 0;
    saturn_build_frt_acc = 0;
    half_frt_last = saturn_frt16();
    build_frt_last = half_frt_last;
    /* Round-51 legs: sprite pass (5), flush runs (6), blit rows (7),
     * TVSTAT flip wait (8) — same per-frame re-zero as the walk accs. */
    saturn_spr_frt_acc  = 0;
    saturn_flu_frt_acc  = 0;
    saturn_blit_frt_acc = 0;
    saturn_pre_frt_acc  = 0;
    saturn_spr_frt_last  = half_frt_last;
    saturn_flu_frt_last  = half_frt_last;
    saturn_blit_frt_last = half_frt_last;
    saturn_pre_frt_last  = half_frt_last;
}

#define SAT_BAIL_CHECK(cpu_, kind_)                                        \
    (((cpu_) == 0 && saturn_master_bail_vbl != 0u                          \
      && (saturn_vbl_count > saturn_master_bail_vbl                        \
          || saturn_half_frt_over()))                                      \
         ? (saturn_seg_bailed = (kind_), 1)                                \
     : saturn_dc_check(2)                                                  \
         ? 1                                                               \
     : 0)
/* 2026-09-03 photo #147: the master's own half read M=(vbl 5, fd<256) â€”
 * only consistent with an elapsed of exactly 2 FRT wraps (131,072 ticks
 * = 100.2 ms), but too round to trust. This is the fold-proof ground
 * truth: per-column poll-accumulated 16-bit sub-deltas (each column
 * << 1 fold), summed into an exact 32-bit count of the whole half â€”
 * entry staging through sprites. Displayed as L (>>8, 0.198 ms/unit). */
int saturn_frt_macc = 0;   /* master-half poll-acc, raw TCNT ticks */
/* 2026-09-03 photo #149: macc AND walk each lost exactly one 65,536 fold
 * inside a single paint leg. paintmax = the largest single SEG_FRT_SUB
 * delta (>>8, 0.198 ms units): if it reads >= 255, one paint call really
 * ran >= 50 ms; if it stays small while walk shows the fold, the fold
 * happened OUTSIDE any sub-stamped call (poll gap between stamps). */
int saturn_frt_paintmax = 0;
/* 2026-09-03 photo #150 decode: the master's half = 6 vbl with ONE
 * stamped ~50 ms paint leg (X=255) and ~50 ms unaccounted. These name
 * the unstamped legs. psite tags where the max leg lives: 1 = a
 * SEG_FRT_SUB-stamped staging/fire call, 2 = the flush_pending leg,
 * 3 = the sprite pass, 0 = outside any named leg (poll gap â€” see
 * gapmax). spr/flush accumulate their legs in >>8 units like the rows. */
int saturn_frt_spr    = 0;
/* 2026-09-03 photo #151: a stamped leg >= 2 folds reads ZERO in the
 * 16-bit delta â€” X/q/i=0 means "folded", not "fast". foldcnt counts
 * per-column gaps that span >= 1 vblank (fm sample: vbl advanced but
 * the 16-bit delta is small), foldx = the last such column. */
int saturn_frt_foldcnt = 0;
int saturn_frt_foldx   = 0;
static unsigned int fm_vbl_last;
/* flush binds to saturn_render.c's existing definition (the dead
 * raycaster's frt_flush_acc publish is compiled out under
 * SATURN_SEG_RENDERER, so the seg half owns the variable). */
extern int saturn_frt_flush;
int saturn_frt_psite  = 0;
int saturn_frt_gapmax = 0;
/* 2026-09-03 photos #152/#153: the FRT is dead on real HW (~55 ticks
 * per vblank â€” the boot "rate" was a phase artifact of a per-vblank
 * reset), so every 16-bit-delta row above reads phase garbage. The
 * vbl clock is the only honest one. These four sub-window counters
 * are VBL counts (16.7 ms units), not ticks:
 *   vwalls = the segwall column loop, vfill = the flush_pending leg,
 *   vspr = the sprite pass (master core only). rcomp = composites
 * fired DURING this half (cache-thrash detector: warm standing-still
 * frames must pay ZERO). */
int saturn_frt_vwalls = 0;
int saturn_frt_vfill  = 0;
int saturn_frt_vspr   = 0;
int saturn_frt_rcomp  = 0;
/* Vbl-span split of the wall loop (photos #154/#155 follow-up): how
 * many of the loop's vblanks were spent INSIDE flat emits (vfire) vs
 * INSIDE wall paints (vwalk) — edge crossings distribute across legs
 * proportionally to time. Glue = vwalls - vwalk - vfire. */
int saturn_frt_vwalk = 0;
int saturn_frt_vfire = 0;
static int rcomp0;
extern unsigned int saturn_frt_ploop_kernel(void);
extern volatile unsigned int saturn_vbl_count;

/* Fold-proof per-column fm sample (photo #151): 16-bit delta + gapmax,
 * plus a vblank-advance check â€” if the vbl clock moved across a gap
 * whose 16-bit delta is small, the gap spanned >= 1 vblank and the
 * FRT read folded (>= 21,814 ticks of unstamped work in ONE column). */
#define SEG_FM_SAMPLE(xv) \
    do { \
        unsigned short fm_ = SEG_SAT_FRT(); \
        unsigned int   gp_ = (unsigned short)(fm_ - fm_last) >> 8; \
        \
        if ((int)gp_ > saturn_frt_gapmax) \
            saturn_frt_gapmax = (int)gp_; \
        if (saturn_vbl_count != fm_vbl_last) \
        { \
            saturn_frt_foldcnt++; \
            saturn_frt_foldx = (xv); \
        } \
        fm_vbl_last = saturn_vbl_count; \
        frt_m_acc += (unsigned short)(fm_ - fm_last); \
        fm_last    = fm_; \
    } while (0)
extern void saturn_frt_ploop_reset(void);

#define SEG_SAT_FRT() (*(volatile unsigned short *)0xFFFFFE12u)
/* Sub-window stamp for one staging/paint call: time expr, add to acc.
 * Leaves enclosing stamps untouched; uint16 wrap-safe (< 3.04 vbl).
 * 2026-09-03 photos #154/#155: the FRT is phase garbage on real HW
 * (TCNT reset per vblank), but "did the vblank clock advance inside
 * this call" is honest â€” with the wall loop running ~9 vbl, the edge
 * crossings distribute across legs proportionally to their time share
 * (Â±1 vbl quantization). vacc accumulates vblanks spanned per leg. */
#define SEG_FRT_SUB(acc, vacc, expr) \
    do { unsigned int tq_ = SEG_SAT_FRT(); \
         unsigned int tvb_ = saturn_vbl_count; expr; \
         unsigned int td_ = (unsigned short)(SEG_SAT_FRT() - tq_); \
         acc += td_; \
         vacc += (unsigned int)(saturn_vbl_count - tvb_); \
         if ((int)(td_ >> 8) > saturn_frt_paintmax) { \
             saturn_frt_paintmax = (int)(td_ >> 8); \
             saturn_frt_psite = 1; } } while (0)

#ifdef SATURN_VDP1_WALL
/* VDP1 quad capture helpers (2026-09-07). texel-u at column x — the
 * exact formula the paint path uses (tex[u & (w-1)] per column), so
 * repetition boundaries land where the software texture wraps. */
static int saturn_v1w_utex(const segwall_t *w, int x)
{
    return (w->offset
            - FixedMul(w->distance,
                       finetangent((w->centerangle
                                    + ((unsigned)seg_xtoviewangle[x]
                                       << FRACBITS))
                                   >> ANGLETOFINESHIFT))) >> FRACBITS;
}

/* Face span at column x, view-clamped (the gate verified every column
 * fully visible against the clip band, so view is the only clamp). */
static void saturn_v1w_edges(const segwall_t *w, int x,
                             int *ytop, int *ybot)
{
    int s  = w->scalefrac + (x - w->start) * w->scalestep;
    int y0 = HALF_H - (FixedMul(s, w->t_top) >> FRACBITS);
    int y1 = HALF_H - (FixedMul(s, w->t_bot) >> FRACBITS);

    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (y1 > VIEW_BOT) y1 = VIEW_BOT;
    *ytop = y0;
    *ybot = y1;
}

/* Commit one captured wall as VDP1 quad(s), texture-accurate on BOTH
 * axes (2026-09-07 round 2 — round 1 mapped the FULL texture over
 * every face, stretching a 64-tex texture 2x on a 128-unit wall and
 * ignoring the peg anchor entirely). Two orthogonal splits:
 *   u — [xa, xb] is cut at the texture-repetition boundaries (u =
 *       k*texw, u linear in x), so every interior u-piece maps one
 *       full wrap with zero phase error (first/last pieces keep the
 *       accepted phase shift; VDP1 cannot source-clip u).
 *   v — the face is cut at its HORIZONTAL TEXTURE SEAMS: row 0
 *       recurs at the world heights where paint_wall_span's v wraps
 *       (v = v_top + depth-below-face-top; seams at v = k*texh).
 *       Rows ARE source-addressable (SRCA + row offset, SIZE.Y =
 *       band rows), so every v-band is EXACT — correct peg anchor,
 *       correct wrap, no stretch.
 * Mirrored walls (u running right-to-left) swap A/B and D/C corners.
 * A cell with any corner outside the VDP1's safe screen range is
 * skipped (coordinates wrap there; software paints it — paint-both).
 * Cells fill the quad table in near-to-far walk order; a full table
 * stops the whole capture. */
/* 2026-09-08: 4 -> 8 each. These caps bind preskip's "bands/pieces
 * cover the whole window" test — a wall wider than 4 texture reps or
 * taller than 4 v-bands fell back to paint-both. Preskip and commit
 * share the same pure math over the same caps, so agreement (and the
 * skipdie tripwire) is preserved by construction. Stack cost of the
 * wider arrays: ~80 B across preskip+commit. */
#define V1W_MAX_REPS   8
#define V1W_MAX_VBANDS 8
/* 2026-09-08 census v3 verdict: reason 6 (run below this minimum)
 * DOMINATES the actionable rejects ~5:1 over the next reason (265 vs
 * 56 cumulative), with the quad table at 3/32 — the 16-column bar was
 * throwing away most of the capturable wall area. 2 = the smallest
 * non-degenerate run (1 column is zero-area); preskip and the walk's
 * two commit sites must keep using the SAME value (preskip's
 * per-column gate scan makes span == run, so the tests stay
 * equivalent). */
#define V1W_MIN_RUN    2

/* Capture-gate kill test, one face at one column (2026-09-07): 1 iff
 * the face span here may NOT become a quad — cropped tiny, outside the
 * open clip band, a masked-mid grate is already recorded nearer at this
 * column, a nearer sprite overlaps the face rows, or the player weapon
 * rect covers them. saturn_v1w_preskip calls the SAME function against
 * the same clipbounds, so preskip/walk agreement is structural; any
 * remaining divergence is the skip-then-die hole bug
 * (saturn_v1w_skipdie). vt/vb (the view-clamped face span) are written
 * back for the caller's start-corner record. */
static int v1w_gate_why;        /* last gate kill reason (rj index)   */

static int saturn_v1w_gate(segwall_t *w, int face, int x, int scale2,
                           int ce, int fl, int *vt_out, int *vb_out)
{
    int ztop = face ? w->b_top : w->t_top;
    int zbot = face ? w->b_bot : w->t_bot;
    int qy0  = HALF_H - (FixedMul(scale2, ztop) >> FRACBITS);
    int qy1  = HALF_H - (FixedMul(scale2, zbot) >> FRACBITS);
    int vt, vb;

    v1w_gate_why = 0;
    if (qy0 < VIEW_TOP) vt = VIEW_TOP; else vt = qy0;
    if (qy1 > VIEW_BOT) vb = VIEW_BOT; else vb = qy1;
    *vt_out = vt;
    *vb_out = vb;
    /* SATURN_V1W_FULL (2026-09-09, operator: full-VDP1 walls). The
     * tiny-span bar (vb-vt<8) stays DROPPED — a visible 2-row quad
     * beats a 2-row hole. But the clip-band test (reason 8) is
     * RESTORED (2026-09-09 triage: the right-hand speckle streak was
     * a GHOST far-wall sliver — census quad xa=291/xb=348/y 28..-3
     * matched the on-screen texel wedge exactly, and its columns'
     * clip band was closed by the nearer roof edge + ceiling).
     * Painter's order only sequences quad-vs-quad; the occluder here
     * is NBG0 software paint (flats, and any wall row the software
     * half still owns), and VDP1 draws ABOVE NBG0 — an occluded
     * face's quad can never be covered, so it must not commit.
     * Wall-vs-sprite is quad-vs-quad now (stage 3a) and wall-vs-mmid
     * stays reason 9; the gate returns when flats move to VDP1/VDP2
     * with a real occlusion contract (stage 3b). The walk's kill-
     * commit/re-arm and preskip's run scan share this gate, so the
     * skip-then-die tripwire stays structural. */
    if (qy1 <= VIEW_TOP || qy0 > VIEW_BOT)
    {
        v1w_gate_why = 7;               /* off-screen                  */
        return 1;
    }
    if (vt < ce || vb > fl)
    {
        v1w_gate_why = 8;               /* outside the open clip band */
        return 1;
    }
#if defined(SATURN_V1W_ALL)
    /* P5 Phase 3 (2026-09-10, verdict C): only a grate that stayed
     * SOFTWARE vetoes farther wall faces — its NBG0 paint must sit
     * over them. A QUAD'd grate (face 2 marked by the capture pass)
     * draws at its own table position: farther walls' quads draw
     * BEFORE it (under it — the PMOD texel-0 holes show them), nearer
     * walls' quads draw AFTER (over its opaque texels). The capture
     * memsets the whole skip map at frame start, so the test reads
     * THIS frame's marks. The hybrid (no capture pass) has no marks
     * at walk time — the old test compiles there unchanged. */
    if (seg_mmid[x].w != NULL && !V1W_MAP_TEST(2, x))
#else
    if (seg_mmid[x].w != NULL)          /* nearer grate paints over   */
#endif
    {
        v1w_gate_why = 9;
        return 1;
    }
    if (v1w_spr_ymax != NULL
        && v1w_spr_ymax[x] > scale2
        && v1w_spr_top[x] < vb && v1w_spr_bot[x] > vt)
    {
        v1w_gate_why = 10;              /* nearer sprite / weapon     */
        return 1;
    }
    return 0;
}

static void saturn_v1w_commit(segwall_t *w, int xa, int xb, int face)
{
    /* Face-selected fields (2026-09-07 round N): face 0 = the
     * TOP/solid band (t_*), face 1 = the BOTTOM band (b_*, pegged at
     * b_peg_abs exactly as the paint site passes it). u math is
     * horizontal and face-independent. */
    int     texnum = face ? w->b_tex : w->t_tex;
    int     texw   = Saturn_TexWidth(texnum);
    int     texh   = Saturn_TexHeight(texnum);
    int     ztop   = face ? w->b_top : w->t_top;
    int     zbot   = face ? w->b_bot : w->t_bot;
    int     peg    = face ? w->b_peg_abs : w->t_peg_top;
    side_t *side = &sides[w->side_i];
    int     rowoff = side->rowoffset
                   | (((side->textureoffset >> 12) & 0x0f) << 8);
    int     eye_u  = (int)(seg_viewz >> FRACBITS);
    /* v anchor (paint_wall_span's world anchor, hoisted to the face
     * top): htop there = eye_u + t_top (the projection's scale and
     * t_u = PROJ/scale cancel), so v_top = peg + rowoff - eye_u -
     * t_top, whole texels, world-anchored. */
    int     v_top  = peg + rowoff - eye_u
                   - (ztop >> FRACBITS);
    int     window = (ztop - zbot) >> FRACBITS;
    short   px0[V1W_MAX_REPS + 1], px1[V1W_MAX_REPS + 1];
    byte    mir[V1W_MAX_REPS + 1];
    short   br0[V1W_MAX_VBANDS], br1[V1W_MAX_VBANDS];
    int     npieces = 0, nbands = 0, p, b;

    if (xa >= xb || window <= 0)
    {
        saturn_v1w_duds++;      /* census (2026-09-08): B=0 with 64 empty
                                   commits — was this return? Painted on
                                   the served-out I row (y=136). */
        return;
    }

    /* v bands: seams where (v_top + row) wraps to row 0. Band k gets
     * SRCA row (v_top + br0) mod texh and SIZE.Y = br1 - br0. */
    if (texh >= 8)
    {
        int rprev = 0, rnext = ((-v_top) % texh + texh) % texh;

        while (rprev < window && nbands < V1W_MAX_VBANDS)
        {
            if (rnext <= rprev || rnext > window)
                rnext = window;
            br0[nbands] = (short)rprev;
            br1[nbands] = (short)rnext;
            nbands++;
            rprev = rnext;
            rnext = rprev + texh;
        }
    }
    if (nbands == 0)
    {                           /* unusable period: anchor-only cell */
        br0[0] = 0;
        br1[0] = 128;
        nbands = 1;
    }

    /* u pieces: repetition boundaries (u signed — mirrored walls
     * make u1 - u0 and the interpolation negative) */
    if (texw >= 8)
    {
        int u0   = saturn_v1w_utex(w, xa);
        int u1   = saturn_v1w_utex(w, xb);
        int step = (u1 > u0) ? 1 : -1;
        int k, cur = xa, reps = 0;

        {
            int u_lo = (u1 > u0) ? u0 : u1;

            k = u_lo / texw + 1;
            if (u_lo < 0 && u_lo % texw != 0)
                k--;
        }
        for (;; k++)
        {
            int ub = k * texw;      /* repetition boundary        */
            int xm;

            if ((step > 0 && ub >= u1) || (step < 0 && ub <= u1))
                break;              /* no boundary left inside    */
            xm = xa + ((ub - u0) * (xb - xa)) / (u1 - u0);
            if (xm <= cur || xm > xb)
                break;
            px0[npieces] = (short)cur;
            px1[npieces] = (short)xm;
            mir[npieces] = (byte)(step < 0);
            npieces++;
            cur = xm;
            if (++reps >= V1W_MAX_REPS)
                break;
        }
        if (cur < xb)               /* final partial piece        */
        {
            px0[npieces] = (short)cur;
            px1[npieces] = (short)xb;
            mir[npieces] = (byte)(step < 0);
            npieces++;
        }
    }
    else
    {
        px0[0] = (short)xa;
        px1[0] = (short)xb;
        mir[0] = 0;
        npieces = 1;
    }

    /* cells: piece x band. Edges sit on the band's own world lines
     * (t = t_top - seam row), view-clamp-free — the gate proved the
     * whole face visible, and clamping would tear the v anchor. */
    for (p = 0; p < npieces && !saturn_v1w_captured; p++)
    {
        int xa_ = px0[p], xb_ = px1[p];
        int ex_l, ex_r, cl_l, cl_r;
        /* Distance shading (2026-09-07): the SAME light the paint path
         * applies — Saturn_LightRow(sector light, depth) — fed to the
         * quad as a uniform gouraud rate. Depth from the piece's mid
         * column, via the paint path's iscale identity. One divide per
         * piece (<=4 per wall), cheap next to the capture math. */
        int    sh;
        {
            int      sa_mid = w->scalefrac
                              + (((xa_ + xb_) >> 1) - w->start) * w->scalestep;
            unsigned iscale_m = 0xffffffffu
                                / (unsigned)(sa_mid >= 1 ? sa_mid : 1);
            int      t_um     = (int)(((int64_t)PROJ * (int64_t)iscale_m)
                                      >> 16);

            if (t_um > 30000) t_um = 30000;
            if (t_um < 1)     t_um = 1;
            sh = Saturn_LightRow(
                sectors[sides[w->side_i].sector].lightlevel, t_um);
#if defined(SATURN_V1W_FORCE_SHADE)
            /* Gouraud DIAGNOSTIC: every quad at a fixed mid shade —
             * if the strips/walls do NOT darken, the GRDA/PMOD gouraud
             * encoding is wrong (or Kronos skips CC mode 4 on palette
             * sprites); if they do, shading works and the real sh
             * computation needs the look. */
            sh = 12;
#endif
        }

        /* U-PHASE (2026-09-07 round 3): VDP1 cannot source-clip u, so a
         * PARTIAL repetition piece (first/last) drawn as-is would
         * squeeze the whole texture into the piece — the "textures
         * don't match the software render" bug. Instead the piece is
         * EXTENDED to its containing full repetition [k*texw,
         * (k+1)*texw] (screen x via the u-linear interpolation) and
         * USER-CLIPPED to its true screen span (VDP1 CMD 0x0008 — user
         * clip, low-nibble types; see saturn_vdp1.c 2026-09-09 — +
         * PMOD Window-In). Full-wrap pieces draw as-is.
         * Degenerate u, or extended corners outside the VDP1's safe
         * coordinate range: the piece is SKIPPED — the software paint
         * underneath is exact. */
        {
            int ua  = saturn_v1w_utex(w, xa_);
            int ub_ = saturn_v1w_utex(w, xb_);
            int ulo = (ua < ub_) ? ua : ub_;
            int uhi = (ua < ub_) ? ub_ : ua;
            int k0  = ulo / texw;

            if (ulo < 0 && ulo % texw != 0)
                k0--;
            if (uhi - ulo >= texw || texw < 8)
            {
                ex_l = (ua <= ub_) ? xa_ : xb_;     /* full wrap: as-is */
                ex_r = (ua <= ub_) ? xb_ : xa_;
            }
            else if (ub_ == ua)
            {
                saturn_v1w_kills++;     /* degenerate u: software paints */
                continue;
            }
            else
            {
                int ubd0 = k0 * texw, ubd1 = ubd0 + texw;
                int x0 = xa_ + (ubd0 - ua) * (xb_ - xa_) / (ub_ - ua);
                int x1 = xa_ + (ubd1 - ua) * (xb_ - xa_) / (ub_ - ua);

                if (ua <= ub_)  { ex_l = x0; ex_r = x1; }
                else            { ex_l = x1; ex_r = x0; }
            }
            cl_l = (xa_ < xb_) ? xa_ : xb_;
            cl_r = (xa_ < xb_) ? xb_ : xa_;
        }

        for (b = 0; b < nbands && !saturn_v1w_captured; b++)
        {
            int t_hi  = ztop - ((int)br0[b] << FRACBITS);
            int t_lo  = ztop - ((int)br1[b] << FRACBITS);
            /* A edge = texture texel 0 (mirrored walls run u right-to-
             * left, so their texel-0 edge is the screen-RIGHT edge);
             * B edge = the opposite screen edge. */
            int xA    = mir[p] ? ex_r : ex_l;
            int xB    = mir[p] ? ex_l : ex_r;
            int sA    = w->scalefrac + (xA - w->start) * w->scalestep;
            int sB    = w->scalefrac + (xB - w->start) * w->scalestep;
            int yA0   = HALF_H - (FixedMul(sA, t_hi) >> FRACBITS);
            int yA1   = HALF_H - (FixedMul(sA, t_lo) >> FRACBITS);
            int yB0   = HALF_H - (FixedMul(sB, t_hi) >> FRACBITS);
            int yB1   = HALF_H - (FixedMul(sB, t_lo) >> FRACBITS);
            int srcrow = 0, rows = (int)br1[b] - (int)br0[b];

            /* STRIPE PROBE (2026-09-07): drop every other cell so the
             * proven software paint shows through in vertical gaps. If
             * the quad u/v anchor is correct the picture is continuous
             * (striped brightness only); a texture jump at a gap = the
             * quad math bug, localized by which seam jumps. */
#if defined(SATURN_V1W_STRIPES)
            if (((p + b) & 1) == 0)
                continue;
#endif

            /* VDP1 screen coordinates wrap near ±512 — a cell this
             * close can't be drawn safely; software paints it. The
             * EXTENDED edges are checked (they can sit far off the
             * piece), so a partial piece that can't extend safely
             * falls back to software rather than drawing wrong-phase. */
            if (yA0 < -500 || yA0 > 1023 || yA1 < -500 || yA1 > 1023
                || yB0 < -500 || yB0 > 1023
                || yB1 < -500 || yB1 > 1023
                || xA < -500 || xA > 1023
                || xB < -500 || xB > 1023)
            {
                saturn_v1w_kills++;     /* corner outside the VDP1's safe
                                           coordinate range: near/tall
                                           walls die here — their world
                                           lines run far off-screen */
                continue;
            }

            if (texh >= 8)
            {
                srcrow = (v_top + (int)br0[b]) % texh;
                if (srcrow < 0)
                    srcrow += texh;
            }
            Saturn_Vdp1Wall_Add(xA, yA0, yA1,
                                xB, yB0, yB1,
                                texnum, srcrow, rows, sh,
                                cl_l, cl_r);
        }
    }
}

/* Paint-skip pre-pass (2026-09-07; v2 2026-09-08): returns which of
 * this wall's face columns are PROVABLY covered by quads the walk is
 * about to add — 0 = none (paint all), 1 = ALL (the old whole-face
 * skip), 2 = partial (v1w_skimap[face] marks the proven runs, the
 * paint sites skip exactly those columns; master half only — the
 * halves run in parallel, so the slave must not read the map).
 * Per run the proof is the same pure math as saturn_v1w_commit,
 * evaluated BEFORE the column walk (the capture gate proved the clip
 * band covers the face at every column of the run, so the software
 * paint span [vt,vb) equals the quad coverage exactly). The gate is
 * saturn_v1w_gate, shared with the walk. A run keeps its software
 * paint on ANY doubt:
 *   - the walk's own capture gate would kill a column (run scan
 *     below — the walk's kill-commit/re-arm reproduces these runs),
 *   - the v bands don't cover the whole window (V1W_MAX_VBANDS cap /
 *     sub-8 texh anchor cell),
 *   - a piece is degenerate under the u-phase extension (ub == ua),
 *   - any cell corner leaves the VDP1's safe coordinate range,
 *   - the summed cell count lacks table headroom or the texture
 *     isn't drawable (v1_tex_get == 0), or its cached size disagrees
 *     with Saturn_TexWidth/Height (Add clamps rows against ti.h).
 * Nothing else adds between here and this wall's own commits, so the
 * once-per-wall CanAdd(total) holds at commit time. Under the STRIPES
 * probe commit drops cells — never skip the paint there. */
#if defined(SATURN_V1W_STRIPES) || defined(SATURN_V1W_ALL)
static int saturn_v1w_preskip(segwall_t *w, int xs, int xe, int face)
{
    (void)w; (void)xs; (void)xe; (void)face;
    return 0;
}
#else
static int saturn_v1w_preskip(segwall_t *w, int xs, int xe, int face)
{
    int      texnum = face ? w->b_tex : w->t_tex;
    int      texw   = Saturn_TexWidth(texnum);
    int      texh   = Saturn_TexHeight(texnum);
    int      ztop   = face ? w->b_top : w->t_top;
    int      zbot   = face ? w->b_bot : w->t_bot;
    int      peg    = face ? w->b_peg_abs : w->t_peg_top;
    side_t  *side = &sides[w->side_i];
    int      rowoff = side->rowoffset
                    | (((side->textureoffset >> 12) & 0x0f) << 8);
    int      eye_u  = (int)(seg_viewz >> FRACBITS);
    int      v_top  = peg + rowoff - eye_u
                    - (ztop >> FRACBITS);
    int      window = (ztop - zbot) >> FRACBITS;
    short    px0[V1W_MAX_REPS + 1], px1[V1W_MAX_REPS + 1];
    byte     mir[V1W_MAX_REPS + 1];
    short    br0[V1W_MAX_VBANDS], br1[V1W_MAX_VBANDS];
    int      npieces = 0, nbands = 0, p, b, x;
    int      r0[V1W_MAP_SPANS], r1[V1W_MAP_SPANS], rok[V1W_MAP_SPANS];
    int      nruns = 0, first_why = -1, drop_why = -1;

    if (xe - xs + 1 < V1W_MIN_RUN)
    {
        saturn_v1w_rj[6]++;         /* census: run below V1W_MIN_RUN */
        return 0;
    }
    if (window <= 0)
    {
        saturn_v1w_rj[4]++;         /* census: zero face window      */
        return 0;
    }

    /* RUN SCAN (2026-09-08 preskip v2): the walk's capture gate per
     * column — the saturn_v1w_gate helper IS the walk's own gate (clip
     * band + nearer masked-mid grate + nearer sprite + the weapon
     * rect), so agreement is structural. The clip only narrows from
     * NEARER walls; this wall hasn't touched it yet. Maximal
     * gate-passing spans of >= V1W_MIN_RUN columns become candidate
     * runs — the walk's kill-commit/re-arm reproduces exactly these
     * runs (same gate, same V1W_MIN_RUN predicate). Overflowing runs
     * are simply not marked (software paints them, the quad still
     * overdraws — safe). */
    {
        int run_start = -1;

        for (x = xs; x <= xe; x++)
        {
            int      scale2 = w->scalefrac + (x - w->start) * w->scalestep;
            unsigned cbv    = sat_colstate->clipbounds[x];
            int      ce = cbv >> 8, fl = cbv & 0xff;
            int      vt, vb;

            if (saturn_v1w_gate(w, face, x, scale2, ce, fl, &vt, &vb))
            {
                if (first_why < 0)
                    first_why = v1w_gate_why;
                if (run_start >= 0 && x - run_start >= V1W_MIN_RUN
                    && nruns < V1W_MAP_SPANS)
                {
                    r0[nruns] = run_start;
                    r1[nruns] = x - 1;
                    rok[nruns] = 1;
                    nruns++;
                }
                run_start = -1;
            }
            else if (run_start < 0)
                run_start = x;
        }
        if (run_start >= 0 && xe - run_start + 1 >= V1W_MIN_RUN
            && nruns < V1W_MAP_SPANS)
        {
            r0[nruns] = run_start;
            r1[nruns] = xe;
            rok[nruns] = 1;
            nruns++;
        }
    }
    if (nruns == 0)
    {
        /* nothing capturable: same census as the old first-kill exit */
        saturn_v1w_rj[first_why >= 0 ? first_why : 6]++;
        return 0;
    }

    /* v bands: verbatim commit, plus the coverage proof (a capped or
     * short loop leaves the lower face unpainted-by-quad) */
    if (texh >= 8)
    {
        int rprev = 0, rnext = ((-v_top) % texh + texh) % texh;

        while (rprev < window && nbands < V1W_MAX_VBANDS)
        {
            if (rnext <= rprev || rnext > window)
                rnext = window;
            br0[nbands] = (short)rprev;
            br1[nbands] = (short)rnext;
            nbands++;
            rprev = rnext;
            rnext = rprev + texh;
        }
        if (rprev < window)
        {
            saturn_v1w_rj[11]++;    /* census: v-bands don't cover    */
            return 0;
        }
    }
    else
    {
        br0[0] = 0;                 /* commit's anchor-only fallback: */
        br1[0] = 128;               /* 128 rows from the face top      */
        nbands = 1;
        if (window > 128)
        {
            saturn_v1w_rj[11]++;    /* census: anchor band too short  */
            return 0;
        }
    }

    /* Per-run proof (2026-09-08 preskip v2): the bands above are
     * span-independent (face anchor + window only). For each candidate
     * run, redo the u pieces, the u-phase extension and the
     * safe-coordinate check VERBATIM from commit; a run failing any of
     * them is dropped from the map (software keeps painting it, the
     * quad still overdraws it — harmless). CanAdd runs ONCE against
     * the summed cell count of the surviving runs — nothing else adds
     * between preskip and this wall's own commits. */
    {
        int cells_total = 0, r;

        for (r = 0; r < nruns; r++)
        {
            int ra = r0[r], rb = r1[r];

            npieces = 0;
            if (texw >= 8)
            {
                int u0   = saturn_v1w_utex(w, ra);
                int u1   = saturn_v1w_utex(w, rb);
                int step = (u1 > u0) ? 1 : -1;
                int k, cur = ra, reps = 0;

                {
                    int u_lo = (u1 > u0) ? u0 : u1;

                    k = u_lo / texw + 1;
                    if (u_lo < 0 && u_lo % texw != 0)
                        k--;
                }
                for (;; k++)
                {
                    int ub = k * texw;      /* repetition boundary    */
                    int xm;

                    if ((step > 0 && ub >= u1) || (step < 0 && ub <= u1))
                        break;
                    xm = ra + ((ub - u0) * (rb - ra)) / (u1 - u0);
                    if (xm <= cur || xm > rb)
                        break;
                    px0[npieces] = (short)cur;
                    px1[npieces] = (short)xm;
                    mir[npieces] = (byte)(step < 0);
                    npieces++;
                    cur = xm;
                    if (++reps >= V1W_MAX_REPS)
                        break;
                }
                if (cur < rb)               /* final partial piece    */
                {
                    px0[npieces] = (short)cur;
                    px1[npieces] = (short)rb;
                    mir[npieces] = (byte)(step < 0);
                    npieces++;
                }
            }
            else
            {
                px0[0] = (short)ra;
                px1[0] = (short)rb;
                mir[0] = 0;
                npieces = 1;
            }

            /* cells: the u-phase extension and the safe-coordinate
             * check, verbatim commit — any skipped cell is unpainted
             * texels. A failed check drops only THIS run. */
            for (p = 0; p < npieces && rok[r]; p++)
            {
                int xa_ = px0[p], xb_ = px1[p];
                int ex_l, ex_r;
                int ua  = saturn_v1w_utex(w, xa_);
                int ub_ = saturn_v1w_utex(w, xb_);
                int ulo = (ua < ub_) ? ua : ub_;
                int uhi = (ua < ub_) ? ub_ : ua;
                int k0  = ulo / texw;

                if (ulo < 0 && ulo % texw != 0)
                    k0--;
                if (uhi - ulo >= texw || texw < 8)
                {
                    ex_l = (ua <= ub_) ? xa_ : xb_; /* full wrap: as-is */
                    ex_r = (ua <= ub_) ? xb_ : xa_;
                }
                else if (ub_ == ua)
                {
                    rok[r] = 0;         /* degenerate u piece         */
                    if (drop_why < 0)
                        drop_why = 13;
                    break;
                }
                else
                {
                    int ubd0 = k0 * texw, ubd1 = ubd0 + texw;
                    int x0 = xa_ + (ubd0 - ua) * (xb_ - xa_) / (ub_ - ua);
                    int x1 = xa_ + (ubd1 - ua) * (xb_ - xa_) / (ub_ - ua);

                    if (ua <= ub_)  { ex_l = x0; ex_r = x1; }
                    else            { ex_l = x1; ex_r = x0; }
                }

                for (b = 0; b < nbands && rok[r]; b++)
                {
                    /* FACE-SELECTED anchor (2026-09-07 bugfix): this
                     * block ran w->t_top for BOTH faces — commit anchors
                     * face 1 at w->b_top (ztop), so preskip validated
                     * the wrong vertical span for bottom faces. Verbatim
                     * commit math, face-selected. */
                    int t_hi = ztop - ((int)br0[b] << FRACBITS);
                    int t_lo = ztop - ((int)br1[b] << FRACBITS);
                    int xA   = mir[p] ? ex_r : ex_l;
                    int xB   = mir[p] ? ex_l : ex_r;
                    int sA   = w->scalefrac + (xA - w->start) * w->scalestep;
                    int sB   = w->scalefrac + (xB - w->start) * w->scalestep;
                    int yA0  = HALF_H - (FixedMul(sA, t_hi) >> FRACBITS);
                    int yA1  = HALF_H - (FixedMul(sA, t_lo) >> FRACBITS);
                    int yB0  = HALF_H - (FixedMul(sB, t_hi) >> FRACBITS);
                    int yB1  = HALF_H - (FixedMul(sB, t_lo) >> FRACBITS);

                    if (yA0 < -500 || yA0 > 1023 || yA1 < -500 || yA1 > 1023
                        || yB0 < -500 || yB0 > 1023
                        || yB1 < -500 || yB1 > 1023
                        || xA < -500 || xA > 1023
                        || xB < -500 || xB > 1023)
                    {
                        rok[r] = 0;     /* corner off the safe range  */
                        break;
                    }
                }
            }

            if (rok[r])
                cells_total += npieces * nbands;
        }

        if (cells_total == 0
            || !Saturn_Vdp1Wall_CanAdd(texnum, cells_total, texw, texh))
        {
            /* nothing survived its proof, or the summed cells don't
             * fit — paint everything (rj[12], or the first drop
             * reason) */
            saturn_v1w_rj[drop_why >= 0 ? drop_why : 12]++;
            return 0;
        }

        /* MARK the surviving runs' columns (unmark is a full clear, so
         * there is no bookkeeping to overflow) */
        {
            int marked = 0;

            for (r = 0; r < nruns; r++)
            {
                if (!rok[r])
                    continue;
                v1w_map_bits(face, r0[r], r1[r], 1);
                marked += r1[r] - r0[r] + 1;
            }
            if (marked == 0)
                return 0;
            return (marked == xe - xs + 1) ? 1 : 2;
        }
    }
}
#endif  /* SATURN_V1W_STRIPES */

/* ---- Stage 2a (2026-09-08): masked-mid capture prescan ----
 * The round-10 mmid flush paints the nearest masked midtexture per
 * column with paint_wall_span_masked AFTER the sprite pass. This
 * prescan converts those paints to VDP1 quads the way preskip v2 does
 * for wall faces: run-scan the half for columns with a recorded grate
 * (one seg per run), prove each run against the commit math (u pieces
 * + u-phase extension + safe corners, verbatim saturn_v1w_commit; ONE
 * v band = the FULL texture — masked mids never tile vertically), add
 * the cells via Saturn_Vdp1Wall_AddMid, and mark face 2 of the skip
 * map; the flush loop below then skips the marked columns' software
 * paint.
 *
 * Ordering makes the quads safe: they are appended AFTER every wall
 * quad = drawn FIRST / under all of them (v1q[] is walked near-to-far,
 * written far-to-near). Farther walls' captures die on gate reason 9
 * (seg_mmid set during their walk), nearer walls draw later/on top,
 * and the software sprite pass draws into NBG0 UNDER the VDP1 layer,
 * so things show through the PMOD-transparent texel-0 holes. The
 * reason-18 sprite veto only fires for NEARER sprites (ymax >
 * scale2m), which a quad would overpaint.
 *
 * A quad has no v clip, so a run is only proven when the FULL texture
 * rect sits inside the recorded open band [y0, y1) at every run column
 * (gate reason 17) — a grate taller than its opening keeps its
 * software paint (stage-2b lever if the census shows reason 17
 * dominating). Kill reasons: saturn_v1w_rj[14..19]. */
static int v1w_gate_m_why;

static int saturn_v1w_gate_m(segwall_t *mw, side_t *ms, int x,
                             int y0, int y1, int *vt_out, int *vb_out)
{
    int texnum = mw->m_tex;
    int scale2 = mw->scalefrac + (x - mw->start) * mw->scalestep;
    int rowoff = ms->rowoffset
               | (((ms->textureoffset >> 12) & 0x0f) << 8);
    int eye_u  = (int)(seg_viewz >> FRACBITS);
    int texh, dz, ztop_m, zbot_m, qy0, qy1, vt, vb;

    v1w_gate_m_why = 0;

    /* The flush clamps t_u into [1,30000]; a clamped column maps v
     * nonlinearly and the quad corners would diverge from the paint —
     * mirror its exact divide and kill on clamp. */
    {
        unsigned denom = (unsigned)(scale2 >= 1 ? scale2 : 1);
        int64_t  ttm   = ((int64_t)PROJ
                          * (int64_t)(0xffffffffu / denom)) >> 16;

        if (ttm > 30000 || ttm < 1)
        {
            v1w_gate_m_why = 14;
            return 1;
        }
    }

    texh = Saturn_TexHeight(texnum);
    if (texh < 8)
    {
        v1w_gate_m_why = 15;    /* VDP1 texture cache minimum */
        return 1;
    }

    /* Texture rect from the masked painter's own anchor: dz = whole
     * texels of texture row 0 above the eye, so the screen lines are
     * HALF_H - scale2*dz — the same identity paint_wall_span_masked's
     * htop encoding uses, with t_u = PROJ*2^16/scale2 unclamped. */
    dz     = mw->t_peg_abs + rowoff - eye_u;
    ztop_m = dz << FRACBITS;
    zbot_m = ztop_m - (texh << FRACBITS);
    qy0    = HALF_H - (FixedMul(scale2, ztop_m) >> FRACBITS);
    qy1    = HALF_H - (FixedMul(scale2, zbot_m) >> FRACBITS);
    if (qy0 < VIEW_TOP) vt = VIEW_TOP; else vt = qy0;
    if (qy1 > VIEW_BOT) vb = VIEW_BOT; else vb = qy1;
    *vt_out = vt;
    *vb_out = vb;
    if (qy1 <= VIEW_TOP || qy0 > VIEW_BOT || vb - vt < 8)
    {
        v1w_gate_m_why = 16;
        return 1;
    }
    if (vt < y0 || vb > y1)
    {
        v1w_gate_m_why = 17;    /* texture rect escapes the open band */
        return 1;
    }
    /* nearer sprite / weapon rect — same veto as the wall gate.
     * (2026-09-08 stage-2a verify: both live dumps read the vetoing
     * sprite as fully covering the clamped band — the weapon psprite
     * at the E1M9 start view, a full-height near sprite [0,195) in a
     * mid-level view. A v-crop above spr_top would only ever recover
     * a <=4-row sliver, so the veto stands as-is.) */
    if (v1w_spr_ymax != NULL && v1w_spr_ymax[x] > scale2
        && v1w_spr_top[x] < vb && v1w_spr_bot[x] > vt)
    {
        v1w_gate_m_why = 18;
        return 1;
    }
    return 0;
}

/* Prove + add one maximal gate-passing run [ra, rb] of a single grate.
 * Returns 1 when EVERY cell stored (only then may the run be marked). */
static int saturn_v1w_mid_run(segwall_t *mw, side_t *ms, int ra, int rb)
{
    int      texnum = mw->m_tex;
    int      texw   = Saturn_TexWidth(texnum);
    int      texh   = Saturn_TexHeight(texnum);
    int      rowoff = ms->rowoffset
                    | (((ms->textureoffset >> 12) & 0x0f) << 8);
    int      eye_u  = (int)(seg_viewz >> FRACBITS);
    int      dz     = mw->t_peg_abs + rowoff - eye_u;
    int      ztop_m = dz << FRACBITS;
    int      zbot_m = ztop_m - (texh << FRACBITS);
    short    px0[V1W_MAX_REPS + 1], px1[V1W_MAX_REPS + 1];
    byte     mir[V1W_MAX_REPS + 1];
    int      npieces = 0, p;

    /* u pieces: verbatim commit (saturn_v1w_commit) */
    if (texw >= 8)
    {
        int u0   = saturn_v1w_utex(mw, ra);
        int u1   = saturn_v1w_utex(mw, rb);
        int step = (u1 > u0) ? 1 : -1;
        int k, cur = ra, reps = 0;

        {
            int u_lo = (u1 > u0) ? u0 : u1;

            k = u_lo / texw + 1;
            if (u_lo < 0 && u_lo % texw != 0)
                k--;
        }
        for (;; k++)
        {
            int ub = k * texw;      /* repetition boundary            */
            int xm;

            if ((step > 0 && ub >= u1) || (step < 0 && ub <= u1))
                break;
            xm = ra + ((ub - u0) * (rb - ra)) / (u1 - u0);
            if (xm <= cur || xm > rb)
                break;
            px0[npieces] = (short)cur;
            px1[npieces] = (short)xm;
            mir[npieces] = (byte)(step < 0);
            npieces++;
            cur = xm;
            if (++reps >= V1W_MAX_REPS)
                break;
        }
        if (cur < rb)               /* final partial piece            */
        {
            px0[npieces] = (short)cur;
            px1[npieces] = (short)rb;
            mir[npieces] = (byte)(step < 0);
            npieces++;
        }
    }
    else
    {
        px0[0] = (short)ra;
        px1[0] = (short)rb;
        mir[0] = 0;
        npieces = 1;
    }

    if (!Saturn_Vdp1Wall_CanAdd(texnum, npieces, texw, texh))
    {
        saturn_v1w_rj[12]++;
        return 0;
    }

    for (p = 0; p < npieces; p++)
    {
        int xa_ = px0[p], xb_ = px1[p];
        int ex_l, ex_r, cl_l, cl_r;
        /* Distance shading: the piece's mid column via the paint path's
         * iscale identity, verbatim commit. */
        int    sh;
        {
            int      sa_mid = mw->scalefrac
                              + (((xa_ + xb_) >> 1) - mw->start) * mw->scalestep;
            unsigned iscale_m = 0xffffffffu
                                / (unsigned)(sa_mid >= 1 ? sa_mid : 1);
            int      t_um     = (int)(((int64_t)PROJ * (int64_t)iscale_m)
                                      >> 16);

            if (t_um > 30000) t_um = 30000;
            if (t_um < 1)     t_um = 1;
            sh = Saturn_LightRow(sectors[ms->sector].lightlevel, t_um);
        }

        /* u-phase extension + user clip: verbatim commit */
        {
            int ua  = saturn_v1w_utex(mw, xa_);
            int ub_ = saturn_v1w_utex(mw, xb_);
            int ulo = (ua < ub_) ? ua : ub_;
            int uhi = (ua < ub_) ? ub_ : ua;
            int k0  = ulo / texw;

            if (ulo < 0 && ulo % texw != 0)
                k0--;
            if (uhi - ulo >= texw || texw < 8)
            {
                ex_l = (ua <= ub_) ? xa_ : xb_;     /* full wrap: as-is */
                ex_r = (ua <= ub_) ? xb_ : xa_;
            }
            else if (ub_ == ua)
            {
                saturn_v1w_rj[19]++;    /* degenerate u piece         */
                return 0;
            }
            else
            {
                int ubd0 = k0 * texw, ubd1 = ubd0 + texw;
                int x0 = xa_ + (ubd0 - ua) * (xb_ - xa_) / (ub_ - ua);
                int x1 = xa_ + (ubd1 - ua) * (xb_ - xa_) / (ub_ - ua);

                if (ua <= ub_)  { ex_l = x0; ex_r = x1; }
                else            { ex_l = x1; ex_r = x0; }
            }
            cl_l = (xa_ < xb_) ? xa_ : xb_;
            cl_r = (xa_ < xb_) ? xb_ : xa_;
        }

        /* corners on the FULL texture rect (srcrow 0, SIZE.Y = texh):
         * A edge = texture texel 0 (mirrored walls run u right-to-
         * left, so their texel-0 edge is the screen-RIGHT edge). */
        {
            int xA  = mir[p] ? ex_r : ex_l;
            int xB  = mir[p] ? ex_l : ex_r;
            int sA  = mw->scalefrac + (xA - mw->start) * mw->scalestep;
            int sB  = mw->scalefrac + (xB - mw->start) * mw->scalestep;
            int yA0 = HALF_H - (FixedMul(sA, ztop_m) >> FRACBITS);
            int yA1 = HALF_H - (FixedMul(sA, zbot_m) >> FRACBITS);
            int yB0 = HALF_H - (FixedMul(sB, ztop_m) >> FRACBITS);
            int yB1 = HALF_H - (FixedMul(sB, zbot_m) >> FRACBITS);

            if (yA0 < -500 || yA0 > 1023 || yA1 < -500 || yA1 > 1023
                || yB0 < -500 || yB0 > 1023
                || yB1 < -500 || yB1 > 1023
                || xA < -500 || xA > 1023
                || xB < -500 || xB > 1023)
            {
                saturn_v1w_rj[19]++;    /* corner off the safe range  */
                return 0;
            }
            if (!Saturn_Vdp1Wall_AddMid(xA, yA0, yA1, xB, yB0, yB1,
                                        texnum, sh, cl_l, cl_r))
            {
                saturn_v1w_rj[12]++;    /* table full / texture refuse */
                return 0;
            }
            saturn_v1w_midquads++;
        }
    }
    return 1;
}

#if !defined(SATURN_V1W_ALL)
/* P5 Phase 3 (2026-09-10): retired under the full-VDP1 define — the
 * capture pass forms and commits the grate runs at their walls' own
 * table positions (see saturn_v1w_capture_pass). The hybrid keeps this
 * flush-time prescan: with no capture pass the quads append at the
 * table tail = drawn FIRST = under every wall quad, and the software
 * sprite pass draws into NBG0 under the VDP1 layer, so the ordering
 * comment below still holds there. */
static void saturn_v1w_mid_prescan(int x0, int x1)
{
    segwall_t *rmw[V1W_MAP_SPANS];
    side_t    *rms[V1W_MAP_SPANS];
    int        r0[V1W_MAP_SPANS], r1[V1W_MAP_SPANS], rok[V1W_MAP_SPANS];
    int        nruns = 0, run_start = -1, first_why = -1, r;
    segwall_t *run_mw = NULL;
    side_t    *run_side = NULL;
    int        x;

    v1w_map_unmark(2);          /* mids persist all frame: one clear  */

    for (x = x0; x < x1; x++)
    {
        int pass = 0;

        if (seg_mmid[x].w != NULL)
        {
            saturn_v1w_midcols++;
            segwall_t *mw = seg_mmid[x].w;
            int        vt, vb;

            if (!saturn_v1w_gate_m(mw, seg_mmid[x].side, x,
                                   seg_mmid[x].y0, seg_mmid[x].y1,
                                   &vt, &vb))
            {
                if (run_start >= 0 && mw != run_mw)
                {
                    /* the run's seg ended here: close it at x-1      */
                    if (x - run_start >= V1W_MIN_RUN
                        && nruns < V1W_MAP_SPANS)
                    {
                        r0[nruns] = run_start;
                        r1[nruns] = x - 1;
                        rok[nruns] = 1;
                        rmw[nruns] = run_mw;
                        rms[nruns] = run_side;
                        nruns++;
                    }
                    run_start = -1;
                }
                if (run_start < 0)
                {
                    run_start = x;
                    run_mw    = mw;
                    run_side  = seg_mmid[x].side;
                }
                continue;
            }
            if (first_why < 0)
                first_why = v1w_gate_m_why;
        }
        /* gate failed or no grate: close any open run                */
        if (run_start >= 0)
        {
            if (x - run_start >= V1W_MIN_RUN && nruns < V1W_MAP_SPANS)
            {
                r0[nruns] = run_start;
                r1[nruns] = x - 1;
                rok[nruns] = 1;
                rmw[nruns] = run_mw;
                rms[nruns] = run_side;
                nruns++;
            }
            run_start = -1;
        }
    }
    if (run_start >= 0 && x1 - run_start >= V1W_MIN_RUN
        && nruns < V1W_MAP_SPANS)
    {
        r0[nruns] = run_start;
        r1[nruns] = x1 - 1;
        rok[nruns] = 1;
        rmw[nruns] = run_mw;
        rms[nruns] = run_side;
        nruns++;
    }

    if (nruns == 0)
    {
        saturn_v1w_rj[first_why >= 0 ? first_why : 6]++;
        return;
    }

    for (r = 0; r < nruns; r++)
    {
        (void)rok[r];
        if (saturn_v1w_mid_run(rmw[r], rms[r], r0[r], r1[r]))
            v1w_map_bits(2, r0[r], r1[r], 1);
    }
}

#endif  /* !SATURN_V1W_ALL */

#endif

/* P5 Phase 2a (2026-09-10): the wall walk's per-column body, extracted
 * VERBATIM from saturn_segs_render_half (refactor, no behavior change;
 * gate = screenshots bit-identical to the Phase 0 freeze). Everything
 * from the per-column DIVU start through the clip/lastsec writes and
 * the per-column flush — the paint and flat-emit calls ride along,
 * because they are interleaved with the clip bookkeeping. Phase 2b's
 * capture pass calls this same helper (paint legs compiled off) so the
 * clip/mmid semantics stay shared by construction. The FRT sub-stamps
 * keep their exact accumulator semantics via pointer args (the
 * SEG_FRT_SUB macro's `acc +=` expands to `(*accp) +=`). Single call
 * site in Phase 2a — the compiler inlines it back into the walk. */
static void seg_column_body(segwall_t *w, side_t *side, int x, int i,
                            int scale2, int ce, int fl, int cpu,
                            sat_colstate_t *cs, int use_divu,
                            unsigned int *frt_fire, unsigned int *frt_vfire,
                            unsigned int *frt_walk, unsigned int *frt_vwalk,
                            int paint)
{
    int      low, high, t_u = 0, utex_raw = 0, r;
    byte     shade = 0;
    /* P5 Phase 2b cost trim (2026-09-10, PVS4 census: fps 60 -> 54):
     * with paint == 0 (capture pass) the u/t_u/shade work feeds only
     * the paint calls and the MID masked decision — on a wall with no
     * masked mid it is all dead. Skip the DIVU start, the utex
     * finetangent, the iscale/t_u block (its __muldi3 is the single
     * fattest per-column op), shade and the lastsec write there; MID
     * walls keep the full path (the masked decision needs t_u). The
     * walk (paint == 1) is bit-identical to Phase 2a. The lastsec
     * write is provably dead at paint == 0: the render halves reset
     * the whole colstate in their prologue, and the gate reads only
     * clipbounds/seg_mmid/v1w_spr_* — lastsec feeds the open-band
     * fill, which the capture never runs. */
    int      need_u = paint || ((w->actionbits & AC_MIDTEXTURE)
                               && w->m_tex > 0);

    /* Start the per-column reciprocal divide FIRST: the
     * quotient lands ~13-16 cycles later and the clip/flat/
     * utex work below runs concurrently (d32xr's R_DrawSeg
     * asm pipelines it exactly like this, r_phase6.c). The
     * 64/32 form with a zero high word = unsigned
     * 0xffffffff/scale2: the dividend 0x00000000FFFFFFFF is a
     * positive 64-bit value, and the quotient 0..0xFFFFFFFF
     * covers every C unsigned result bit-exactly. */
    if (need_u && use_divu)
    {
        unsigned denom = (unsigned)(scale2 >= 1 ? scale2 : 1);
        SATURN_DIVU_DVSR   = (int)denom;
        SATURN_DIVU_DVDNTH = 0;
        SATURN_DIVU_DVDNTL = (int)0xffffffffu;   /* start */
    }

    /* new floor/ceiling clip candidates (R_SegLoop) */
    low = HALF_H - (FixedMul(scale2, w->floornew) >> FRACBITS);
    if (low < 0)
        low = 0;
    else if (low > fl)
        low = fl;

    high = HALF_H - (FixedMul(scale2, w->ceilingnew) >> FRACBITS);
    if (high > VIEW_BOT)
        high = VIEW_BOT;
    else if (high < ce)
        high = ce;

    /* flats: open bands against the PRE-seg clip. Sky is
     * auto-detected by paint_flat_run (ceilingpic == -1).
     * Phase 2b: paint == 0 (capture pass) skips every NBG0
     * paint and nearlo/nearhi write — the capture only needs the
     * DIVU start, u/t_u, the MID masked decision + mmid record, the
     * clip narrowing, lastsec. Flats feed nothing the gate reads. */
    if (paint && (w->actionbits & (AC_ADDCEILING | AC_ADDSKY)))
    {
        int bot = HALF_H - (FixedMul(scale2, w->ceilingheight)
                            >> FRACBITS);
        if (bot > fl) bot = fl;
        if (ce < bot)
            SEG_FRT_SUB((*frt_fire), (*frt_vfire),
                fr_emit_flat(x, imax(ce, VIEW_TOP), imin(bot, VIEW_BOT),
                             &sectors[w->fsec_i], 0, cpu));
    }
    if (paint && (w->actionbits & AC_ADDFLOOR))
    {
        int top = HALF_H - (FixedMul(scale2, w->floorheight)
                            >> FRACBITS);
        if (top < ce) top = ce;
        if (top < fl)
            SEG_FRT_SUB((*frt_fire), (*frt_vfire),
                fr_emit_flat(x, imax(top, VIEW_TOP), imin(fl, VIEW_BOT),
                             &sectors[w->fsec_i], 1, cpu));
    }

    /* wall texture u at this column (R_DrawSeg) — capture no-mid
     * columns skip it (dead: paints only) */
    if (need_u)
    {
        r = finetangent((w->centerangle
                         + ((unsigned)seg_xtoviewangle[x] << FRACBITS))
                        >> ANGLETOFINESHIFT);
        utex_raw = (w->offset - FixedMul(w->distance, r)) >> FRACBITS;
    }

    /* per-column depth for the painter's v-step — the divide
     * was started at the top of this iteration (DIVU path);
     * the 4 nops are only the latency floor, the work above
     * already burned most of it */
    if (need_u)
    {
        int64_t  tt;
        unsigned iscale;

        if (use_divu)
        {
            __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
            iscale = (unsigned)SATURN_DIVU_DVDNTL;
        }
        else
        {
            unsigned denom = (unsigned)(scale2 >= 1 ? scale2 : 1);
            iscale = 0xffffffffu / denom;
        }

        tt = ((int64_t)PROJ * (int64_t)iscale) >> 16;
        if (tt > 30000)     t_u = 30000;
        else if (tt < 1)    t_u = 1;
        else                t_u = (int)tt;

        shade = (byte)(t_u >> 6 > 15 ? 15 : t_u >> 6);
    }

    /* Zero-texture spans (2026-09-02): a texnum <= 0 piece (a
     * one-sided line whose sidedef has no textures at all) must
     * NOT paint the solid fallback fill â€” that was the green
     * why=1 band. Skip the paint; the clip still narrows below,
     * and whatever the farther segs paint shows through. */
    if (paint && (w->actionbits & AC_TOPTEXTURE) && w->t_tex > 0)
    {
        int y0 = HALF_H - (FixedMul(scale2, w->t_top) >> FRACBITS);
        int y1 = HALF_H - (FixedMul(scale2, w->t_bot) >> FRACBITS);

        if (y0 < ce) y0 = ce;
        if (y1 > fl) y1 = fl;
        if (y0 < y1)
        {
            int py0 = imax(y0, VIEW_TOP), py1 = imin(y1, VIEW_BOT);

            if (py0 < py1)
            {
                if (py0 < cs->nearlo[x]) cs->nearlo[x] = (byte)py0;
                if (py1 - 1 > cs->nearhi[x]) cs->nearhi[x] = (byte)(py1 - 1);
                if (x == 28)
                {
                    saturn_dbg_near_i    = i;
                    saturn_dbg_near_span = (py0 << 8) | py1;
                }
                /* Paint-skip (preskip v2): per-column — skip
                 * only where preskip PROVED a quad covers this
                 * column (mode 1 marks the whole face, mode 2
                 * the proven runs). The committed quads repaint
                 * the span on the VDP1 — the walk's own paint
                 * kernel is the single largest per-column cost. */
#ifdef SATURN_VDP1_WALL
#if SATURN_V1W_WALK_CAPTURE
                /* master-only: the map is written by the
                 * master's preskip at wall-setup time, and the
                 * halves run in parallel — the slave could read
                 * marks that already belong to a LATER wall */
                if (!(cpu == 0 && V1W_MAP_TEST(0, x)))
#else
                /* Phase 2b: the capture pass wrote the FULL-FRAME
                 * map before dispatch — both cores skip, the quads
                 * repaint the span above NBG0. */
                if (!V1W_MAP_TEST(0, x))
#endif
#endif
                    SEG_FRT_SUB((*frt_walk), (*frt_vwalk),
                        paint_wall_span(x, py0, py1, w->t_tex, side, 0,
                                        utex_raw, w->t_peg_top, t_u,
                                        (byte)(COL_WALL_TOP + shade),
                                        cpu));
#ifdef SATURN_VDP1_WALL
#if SATURN_V1W_WALK_CAPTURE
                else if (cpu == 0)
#else
                else
#endif
                    saturn_v1w_skipcols++;
#endif
            }
        }
    }

    if (paint && (w->actionbits & AC_BOTTOMTEXTURE) && w->b_tex > 0)
    {
        int y0 = HALF_H - (FixedMul(scale2, w->b_top) >> FRACBITS);
        int y1 = HALF_H - (FixedMul(scale2, w->b_bot) >> FRACBITS);

        if (y0 < ce) y0 = ce;
        if (y1 > fl) y1 = fl;
        if (y0 < y1)
        {
            int py0 = imax(y0, VIEW_TOP), py1 = imin(y1, VIEW_BOT);

            if (py0 < py1)
            {
                if (py0 < cs->nearlo[x]) cs->nearlo[x] = (byte)py0;
                if (py1 - 1 > cs->nearhi[x]) cs->nearhi[x] = (byte)(py1 - 1);
                if (x == 28)
                {
                    saturn_dbg_near_i    = i;
                    saturn_dbg_near_span = (py0 << 8) | py1;
                }
                if (w->b_tex == saturn_sw1_tex)
                    saturn_sw1_paint++;
                /* Paint-skip (2026-09-07 round N): same contract
                 * as the TOP site — bottom-face map, face 1.
                 * Phase 2b: capture's full-frame map, both cores. */
#ifdef SATURN_VDP1_WALL
#if SATURN_V1W_WALK_CAPTURE
                if (!(cpu == 0 && V1W_MAP_TEST(1, x)))
#else
                if (!V1W_MAP_TEST(1, x))
#endif
#endif
                    SEG_FRT_SUB((*frt_walk), (*frt_vwalk),
                        paint_wall_span(x, py0, py1, w->b_tex, side, 0,
                                        utex_raw, w->b_peg_abs, t_u,
                                        (byte)(COL_WALL_LOW + shade),
                                        cpu));
#ifdef SATURN_VDP1_WALL
#if SATURN_V1W_WALK_CAPTURE
                else if (cpu == 0)
#else
                else
#endif
                    saturn_v1w_skipcols++;
#endif
            }
        }
    }

    if ((w->actionbits & AC_MIDTEXTURE) && w->m_tex > 0)
    {
        /* Round 10 (2026-09-04): the old treatment here — full
         * open band, vertically tiled, opaque, clip hard-closed
         * — is only right for an OPAQUE mid covering the whole
         * band (a closed door). Vanilla's masked mids draw ONCE
         * from the peg, skip transparent texels and leave the
         * opening OPEN (R_RenderMaskedSegRange): the tiled grate
         * and the see-through-holes bugs. Gate: a composite with
         * any zero texel (Saturn_TexHasHoles), or one shorter
         * than the open band (rows on screen = h*PROJ/t_u),
         * takes the masked path — recorded here, painted at the
         * end of the open-band fill (paint_wall_span_masked).
         * Doors stay on the solid path (asm kernel, closed
         * clip) — the perf that matters. */
        int py0 = imax(ce, VIEW_TOP), py1 = imin(fl, VIEW_BOT);
        int masked = 0;

        if (py0 < py1 && w->m_tex < numtextures)
        {
            int texrows = (textures[w->m_tex].height * PROJ) / t_u;

            if (Saturn_TexHasHoles(w->m_tex) || texrows < py1 - py0)
                masked = 1;
        }

        if (masked)
        {
            /* Deferred grate: record the nearest one per column
             * (walk is near->far), leave the clip OPEN with the
             * same NEW-bit narrowing as a non-mid piece — the
             * fill's lastsec owns the band until the flush
             * paints the grate over it. nearlo/nearhi stay
             * untouched: the fill must still fill these rows
             * (the holes show it), unlike wall art. */
            if (seg_mmid[x].w == NULL)
            {
                seg_mmid[x].w    = w;
                seg_mmid[x].side = side;
                seg_mmid[x].nside = 0;
                seg_mmid[x].y0   = (byte)py0;
                seg_mmid[x].y1   = (byte)py1;
            }
            else
                saturn_mmid_skip++;

            {
                int nce = (w->actionbits & AC_NEWCEILING) ? high : ce;
                int nfl = (w->actionbits & AC_NEWFLOOR) ? low : fl;

                cs->clipbounds[x] = (unsigned short)((nce << 8) | nfl);
            }
        }
        else
        {
        /* full open band, then CLOSE the clip: nothing farther
         * shows through (raycaster covered=1 parity; closed
         * doors occlude). Phase 2b: capture keeps the clip close
         * (it feeds the gate), skips the paint + nearlo/nearhi. */
            if (py0 < py1 && paint)
                {
                    if (py0 < cs->nearlo[x]) cs->nearlo[x] = (byte)py0;
                    if (py1 - 1 > cs->nearhi[x]) cs->nearhi[x] = (byte)(py1 - 1);
                }
            if (paint)
                SEG_FRT_SUB((*frt_walk), (*frt_vwalk),
                    paint_wall_span(x, imax(ce, VIEW_TOP), imin(fl, VIEW_BOT),
                                    w->m_tex, side, 0, utex_raw, w->t_peg_abs,
                                    t_u, (byte)(COL_WALL_TOP + shade), cpu));
            cs->clipbounds[x] = (unsigned short)((fl << 8) | fl);
        }
    }
    else
    {
        /* d32xr-authentic clip narrowing (2026-09-02, round 24):
         * the global clip narrows ONLY on AC_NEWFLOOR/NEWCEILING
         * (r_phase6.c post_draw copies segl->clipbounds only for
         * those bits). AC_SOLIDSIL is a sprite-clipper silhouette
         * bit, NOT a clip closer â€” the round-23 SOLIDSIL close
         * strangled the bands above/below one-sided walls whose
         * ADD bits didn't fire (door-track jambs: track floor
         * above eye), leaving them unpainted clear (the solid
         * red slabs). The wall-art-overwrite bug the round-23
         * painted (nearlo..nearhi span, tracked at each paint
         * site below). */
        int nce = (w->actionbits & AC_NEWCEILING) ? high : ce;
        int nfl = (w->actionbits & AC_NEWFLOOR) ? low : fl;

        /* Round 25b (2026-09-02): the BISECT round-13 fb-marker
         * probe is GONE â€” it painted markcol[] colours (0xB0 pure
         * red, 0xFB magenta) straight into the scene on cols 23-34
         * wherever a seg narrowed the clip without painting, and
         * it OVERPAINTS valid ADD-flat content: the solid red bar
         * in the 19:59 screenshot was this probe standing on top
         * of painted floor, not a hole. Only the col-28 dbg
         * snapshot (counter reads, no pixels) survives. */
        {
            int top_p = (w->actionbits & AC_TOPTEXTURE) && w->t_tex > 0;
            int bot_p = (w->actionbits & AC_BOTTOMTEXTURE) && w->b_tex > 0;

            if (x == 28 && ((!top_p && nce > ce) || (!bot_p && nfl < fl)))
            {
                saturn_dbg_close_i    = i;
                saturn_dbg_close_ab   = w->actionbits;
                saturn_dbg_close_ttex = w->t_tex;
                saturn_dbg_close_tt   = w->t_top;
                saturn_dbg_close_tb   = w->t_bot;
            }
        }
        cs->clipbounds[x] = (unsigned short)((nce << 8) | nfl);
    }
    /* Phase 2b trim: dead at paint == 0 (see need_u above) — the
     * halves reset lastsec in their prologue and the gate never
     * reads it. */
    if (paint)
        cs->lastsec[x] = (unsigned short)w->bsec_i;

    /* per-seg per-column flat merge (keeps staged spans from
     * accumulating across segs) — paint only; the capture pass
     * stages nothing */
    if (paint)
        saturn_seg_flush_column(x, cpu);
}

#ifdef SATURN_VDP1_WALL
#ifdef SATURN_V1W_ALL
/* P5 Phase 2b (2026-09-10): the capture pass — the wall walk's VDP1
 * capture relocated from saturn_segs_render_half into BUILD (master,
 * single-threaded, runs BEFORE the halves dispatch — order is capture
 * → preload/v1s_prep → dispatch). It walks segwalls[] in the walk's
 * own order over the FULL width, running the same per-column
 * gate/commit ladder and the same seg_column_body helper with
 * paint = 0 (DIVU start, u/t_u, MID masked decision + mmid record,
 * clip narrowing, lastsec — no paint, no flat emits, no staging
 * flush), so the clip/mmid state the gate reads evolves exactly as
 * the walk's does and every capture decision is bit-identical to the
 * walk-mode capture. seg_column_body records seg_mmid first-wins
 * (near->far), so capture owns seg_mmid full-width before dispatch —
 * the render halves' reset is compiled out and their own records
 * become saturn_mmid_skip no-ops.
 * The committed runs' columns are marked into the skip map (faces
 * 0/1) so BOTH render halves skip the software paint there and the
 * quads repaint the span above NBG0. Marking is exact, not trusted:
 * a run is marked only when its commit ADDED cells (saturn_v1w_cells
 * moved), added them ALL (no saturn_v1w_kills piece skipped inside
 * commit — safe-range corners / degenerate u leave holes), and
 * nothing refused (saturn_v1w_captured still clear — the latch means
 * the table overflowed mid-commit and later pieces were dropped).
 * Anything else leaves the run unmarked and the software paint on —
 * the fail-open contract. Near-to-far order means an overflow
 * already favoured the nearest faces before the latch. */
static void saturn_v1w_capture_pass(void)
{
    sat_colstate_t *cs = sat_colstate;
    extern volatile int saturn_divu_probe;

    int          use_divu = saturn_divu_probe;
    int          i, x;
    unsigned int frt_fire_acc = 0, frt_vfire_acc = 0;
    unsigned int frt_walk_acc = 0, frt_vwalk_acc = 0;

    /* capture owns the skip map this frame: one full clear (the
     * walk-side per-wall unmark is compiled out; the flush's
     * mid prescan still clears face 2 itself) */
    memset(v1w_skimap, 0, sizeof(v1w_skimap));

    /* fresh per-column state — the render halves reset the same
     * struct again in their prologue, so nothing leaks into the
     * walk; seg_mmid is the one array that must SURVIVE into the
     * halves (gate reason 9 + the flush's grate pass read it) */
    for (x = 0; x <= SCREEN_W; x++)
    {
        cs->clipbounds[x] = (unsigned short)((VIEW_TOP << 8) | VIEW_BOT);
        cs->lastsec[x]    = (unsigned short)seg_viewsec_i;
        cs->nearlo[x]     = 0xff;   /* empty span = nearlo > nearhi */
        cs->nearhi[x]     = 0;
    }
    memset(seg_mmid, 0, sizeof(seg_mmid));

    for (i = 0; i < segwalls_count; i++)
    {
        segwall_t *w = &segwalls[i];
        int        xs = w->start;
        int        xe = w->stop;
        side_t    *side = &sides[w->side_i];
        int        v1w_alive = 0, v1w_alive_b = 0;
        /* P5 2b trim 2 (2026-09-10, PVS6): admission latches — a face
         * that FAILED admission must never re-arm. The old re-arm
         * branch probed the gate on EVERY column of every dead face
         * (one-sided walls' missing bottom, MIDTEXTURE walls' both
         * faces) and could arm + commit it — commit then evaporated
         * on window<=0 (the I-row duds: 19/frame at pose #1, and an
         * admitted face CANNOT dud since admission rejects zero
         * windows at rj[4] and window is per-wall constant). Removing
         * them is paint-safe by the fail-open contract: a dead-face
         * commit either added nothing, or added cells whose columns
         * now simply keep the software paint. */
        int        v1w_adm = 0, v1w_adm_b = 0;
        int        v1w_xa = xs, v1w_ya0 = 0, v1w_ya1 = 0;
        int        v1w_xa_b = xs, v1w_ya0_b = 0, v1w_ya1_b = 0;

        if (xs > xe)
            continue;

        /* face admission — the walk's rj[0..5] ladder, replicated
         * (the walk's copy compiles out under SATURN_V1W_ALL, so
         * the census keeps its meaning from here) */
        if (!saturn_v1w_captured)
        {
            if (!(w->actionbits & AC_TOPTEXTURE))
                saturn_v1w_rj[0]++;
            else if (w->t_tex <= 0)
                saturn_v1w_rj[1]++;
            else if (!(w->actionbits & (AC_SOLIDSIL | AC_NEWCEILING)))
                saturn_v1w_rj[2]++;
            else if (w->actionbits & AC_MIDTEXTURE)
                saturn_v1w_rj[3]++;
            else if (((w->t_top - w->t_bot) >> FRACBITS) <= 0)
                saturn_v1w_rj[4]++;
            else if (v1w_spr_ymax == NULL)
                saturn_v1w_rj[5]++;
            else
                v1w_alive = v1w_adm = 1;

            if (!(w->actionbits & AC_BOTTOMTEXTURE))
                saturn_v1w_rj[0]++;
            else if (w->b_tex <= 0)
                saturn_v1w_rj[1]++;
            else if (w->actionbits & AC_MIDTEXTURE)
                saturn_v1w_rj[3]++;
            else if (((w->b_top - w->b_bot) >> FRACBITS) <= 0)
                saturn_v1w_rj[4]++;
            else if (v1w_spr_ymax == NULL)
                saturn_v1w_rj[5]++;
            else
                v1w_alive_b = v1w_adm_b = 1;
        }

        for (x = xs; x <= xe; x++)
        {
            int      scale2 = w->scalefrac + (x - w->start) * w->scalestep;
            unsigned cbv    = cs->clipbounds[x];
            int      ce = cbv >> 8, fl = cbv & 0xff;

            /* gate/commit ladder — verbatim walk logic. The walk's
             * kill-branch skipdie probe has no preskip left to
             * disagree with, so it drops; marking replaces it
             * (cells-delta proof above). */
            if (v1w_alive)
            {
                int vt, vb;

                if (saturn_v1w_gate(w, 0, x, scale2, ce, fl, &vt, &vb))
                {
                    if (x - v1w_xa >= V1W_MIN_RUN)
                    {
                        int c0 = saturn_v1w_cells;
                        int k0 = saturn_v1w_kills;

                        saturn_v1w_commit(w, v1w_xa, x - 1, 0);
                        saturn_v1w_commits++;
                        if (!saturn_v1w_captured
                            && saturn_v1w_cells != c0
                            && saturn_v1w_kills == k0)
                            v1w_map_bits(0, v1w_xa, x - 1, 1);
                    }
                    v1w_alive = 0;      /* hidden / cropped / clipped */
                }
                else
                {
                    if (x == xs)
                    {
                        v1w_xa  = x;
                        v1w_ya0 = vt;
                        v1w_ya1 = vb;
                    }
                    if (x == xe)
                    {
                        if (x - v1w_xa + 1 >= V1W_MIN_RUN)
                        {
                            int c0 = saturn_v1w_cells;
                            int k0 = saturn_v1w_kills;

                            saturn_v1w_commit(w, v1w_xa, x, 0);
                            saturn_v1w_commits++;
                            if (!saturn_v1w_captured
                                && saturn_v1w_cells != c0
                                && saturn_v1w_kills == k0)
                                v1w_map_bits(0, v1w_xa, x, 1);
                        }
                        v1w_alive = 0;
                    }
                }
            }
            else if (v1w_adm && x + V1W_MIN_RUN - 1 <= xe)
            {
                int vt2, vb2;

                if (saturn_v1w_gate(w, 0, x, scale2, ce, fl,
                                    &vt2, &vb2) == 0)
                {
                    /* SUB-RUN RE-ARM: a run may start at ANY column.
                     * Trim 2: admitted faces only — dead faces never
                     * re-armed anything but duds (I-row 19/frame). */
                    v1w_alive = 1;
                    v1w_xa  = x;
                    v1w_ya0 = vt2;
                    v1w_ya1 = vb2;
                }
            }
            if (v1w_alive_b)
            {
                int vt, vb;

                if (saturn_v1w_gate(w, 1, x, scale2, ce, fl, &vt, &vb))
                {
                    if (x - v1w_xa_b >= V1W_MIN_RUN)
                    {
                        int c0 = saturn_v1w_cells;
                        int k0 = saturn_v1w_kills;

                        saturn_v1w_commit(w, v1w_xa_b, x - 1, 1);
                        saturn_v1w_commits++;
                        if (!saturn_v1w_captured
                            && saturn_v1w_cells != c0
                            && saturn_v1w_kills == k0)
                            v1w_map_bits(1, v1w_xa_b, x - 1, 1);
                    }
                    v1w_alive_b = 0;
                }
                else
                {
                    if (x == xs)
                    {
                        v1w_xa_b  = x;
                        v1w_ya0_b = vt;
                        v1w_ya1_b = vb;
                    }
                    if (x == xe)
                    {
                        if (x - v1w_xa_b + 1 >= V1W_MIN_RUN)
                        {
                            int c0 = saturn_v1w_cells;
                            int k0 = saturn_v1w_kills;

                            saturn_v1w_commit(w, v1w_xa_b, x, 1);
                            saturn_v1w_commits++;
                            if (!saturn_v1w_captured
                                && saturn_v1w_cells != c0
                                && saturn_v1w_kills == k0)
                                v1w_map_bits(1, v1w_xa_b, x, 1);
                        }
                        v1w_alive_b = 0;
                    }
                }
            }
            else if (v1w_adm_b && x + V1W_MIN_RUN - 1 <= xe)
            {
                int vt2, vb2;

                if (saturn_v1w_gate(w, 1, x, scale2, ce, fl,
                                    &vt2, &vb2) == 0)
                {
                    v1w_alive_b = 1;
                    v1w_xa_b  = x;
                    v1w_ya0_b = vt2;
                    v1w_ya1_b = vb2;
                }
            }

            /* the capture never paints: only the clip narrowing /
             * mmid record / lastsec legs of the body run */
            if (fl <= ce)
                continue;

            seg_column_body(w, side, x, i, scale2, ce, fl, 0, cs,
                            use_divu, &frt_fire_acc, &frt_vfire_acc,
                            &frt_walk_acc, &frt_vwalk_acc, 0);
        }

        /* P5 Phase 3 (2026-09-10, verdict C): this wall's masked-mid
         * runs, formed HERE and committed at the wall's OWN table
         * position — the AddMid cells land after wall i's face cells
         * and before wall i+1's, so the far-to-near writer draws
         * farther walls -> this grate -> nearer walls. Through the
         * grate's PMOD-transparent texel-0 holes the farther walls'
         * quads show; nearer walls' quads cover the grate's opaque
         * texels. seg_mmid[x].w == w picks exactly the columns where
         * this wall owns the nearest grate (first-wins, recorded by
         * seg_column_body during the column loop above — all nearer
         * walls' mids are already resolved quad-or-software, so the
         * face-2 test inside the wall gate above saw this frame's
         * marks). gate_m re-proves each column (t_u clamp, texh,
         * band escape, nearer-sprite veto); a run commits only when
         * EVERY piece stored, and only then is face 2 marked —
         * unmarked columns keep the software grate paint, and the
         * gate's reason-9 branch (same define) keeps farther walls
         * software there. Fail-open by construction. */
        if (!saturn_v1w_captured
            && (w->actionbits & AC_MIDTEXTURE) && w->m_tex > 0)
        {
            int r0 = -1, first_why = -1, had_col = 0, committed = 0;

            for (x = xs; x <= xe; x++)
            {
                int vt, vb;

                if (seg_mmid[x].w == w)
                {
                    had_col = 1;
                    if (!saturn_v1w_gate_m(w, side, x,
                                           seg_mmid[x].y0, seg_mmid[x].y1,
                                           &vt, &vb))
                    {
                        saturn_v1w_midcols++;
                        if (r0 < 0)
                            r0 = x;
                        continue;
                    }
                    if (first_why < 0)
                        first_why = v1w_gate_m_why;
                }
                if (r0 >= 0)
                {
                    if (x - r0 >= V1W_MIN_RUN)
                        committed |= saturn_v1w_mid_run(w, side,
                                                        r0, x - 1);
                    r0 = -1;
                }
            }
            if (r0 >= 0)
            {
                if (xe - r0 + 1 >= V1W_MIN_RUN)
                    committed |= saturn_v1w_mid_run(w, side, r0, xe);
                r0 = -1;
            }
            if (had_col && !committed)
                saturn_v1w_rj[first_why >= 0 ? first_why : 6]++;
        }
    }
}
#endif
#endif

void saturn_segs_render_half(int x0, int x1, int cpu)
{
    /* 2026-09-06 OCRAM: clipbounds/lastsec/nearlo/nearhi moved into the
     * sat_colstate_t struct (sat_colstate, file scope) — on-chip RAM
     * when the boot probe verified the TW-mode data array, else the
     * static WRAM twin. Same layout, same lifetime (per-call reset
     * below), one pointer the compiler can keep in a register. */
    sat_colstate_t *cs = sat_colstate;
    /* Round 25: "a textured SOLID wall painted this column" flag. The
     * final open-band fill paints [ce,fl) with flats; for a one-sided
     * wall the post-wall clip IS the wall's face span (both NEW bits
     * fire against the dummy back sector), so an unguarded fill painted
     * floor/ceiling flats straight over the wall art â€” the "wall split
     * in half at the horizon" family. Round 24 guarded this with the
     * FIRST painter's span per column, but that is the wrong span: the
     * rows that survive into the final open band belong to the LAST
     * solid wall (each solid wall paints its whole remaining clip, so
     * their faces nest), and when a nearer TWO-SIDED wall (a step face
     * or opening jamb) painted different rows first, first-wins filled
     * the distant solid wall's face with lastsec flats â€” "walls are not
     * rendered until I move close". Within the final band the only
     * painted rows are the last solid wall's face (two-sided pieces are
     * exactly the rows their NEW-bit narrowing removes), so a boolean
     * is sufficient: if a solid wall painted the column, the fill has
     * nothing legitimate left to paint and must skip it entirely. */
    /* Round 26c (2026-09-02): per-column painted SPAN (nearlo..nearhi
     * inclusive, empty = nearlo > nearhi) replacing the round-25
     * nearpaint boolean. The boolean was too coarse: a DISTANT one-sided
     * wall paints only a few rows of an otherwise still-open clip (e.g.
     * looking down from E1M1's upper ledge into a pit room whose ceiling
     * is below the eye, the wall's face covers rows ~105-112 while the
     * clip stays open at [10,113) â€” ADDCEILING legitimately does not
     * fire because there is no ceiling flat above the eye there), and
     * the boolean then skipped the whole column's fill: rows 10-105
     * stayed UNPAINTED CLEAR (the full-height clear band family of the
     * 20:46/20:47 screenshots). The fill must skip only the rows walls
     * actually painted, never the rows they merely left open. */
    extern volatile int saturn_divu_probe;
    int use_divu = saturn_divu_probe;
    int i, x;
    unsigned int frt_all_acc = 0, frt_fire_acc = 0, frt_walk_acc = 0;
    unsigned int t_all;
    unsigned short fm_last = 0;     /* master-half poll-acc cursor */
    unsigned int   frt_m_acc = 0;
    unsigned int   tw0 = 0;         /* vbl-honest sub-window cursor */
    unsigned int   frt_vfire_acc = 0;   /* vbls spanned inside flat emits */
    unsigned int   frt_vwalk_acc = 0;   /* vbls spanned inside wall paints */

    if (cpu == 0)
    {
        saturn_frt_fire  = 0;
        saturn_frt_scan  = 0;
        saturn_frt_walk  = 0;
        saturn_frt_total = 0;
        saturn_frt_ploop = 0;
        saturn_frt_macc  = 0;
        saturn_frt_paintmax = 0;
        saturn_frt_spr    = 0;
        saturn_frt_flush  = 0;
        saturn_frt_psite  = 0;
        saturn_master_site = 5;   /* round-50: master half entered */
        saturn_frt_gapmax = 0;
        saturn_frt_foldcnt = 0;
        saturn_frt_foldx   = 0;
        saturn_frt_vwalls = 0;
        saturn_frt_vfill  = 0;
        saturn_frt_vspr   = 0;
        saturn_frt_rcomp  = 0;
        saturn_frt_vwalk  = 0;
        saturn_frt_vfire  = 0;
        saturn_dbg_pairs  = 0;
        {
            extern int saturn_tex_comp_count;   /* saturn_tex.c */

            rcomp0 = saturn_tex_comp_count;
        }
        fm_vbl_last = saturn_vbl_count;
        tw0 = saturn_vbl_count;
        saturn_frt_ploop_reset();   /* the flat-kernel window (L) */
        fm_last = SEG_SAT_FRT();
    }
    t_all = SEG_SAT_FRT();

    /* painter staging: cold-start this core's run-merge state, then
     * render every viswall over [x0, x1) */
    saturn_seg_reset_staging(cpu);

    for (x = 0; x <= SCREEN_W; x++)
    {
        cs->clipbounds[x] = (unsigned short)((VIEW_TOP << 8) | VIEW_BOT);
        cs->lastsec[x]    = (unsigned short)seg_viewsec_i;
        cs->nearlo[x]     = 0xff;   /* empty span = nearlo > nearhi */
        cs->nearhi[x]     = 0;
#ifndef SATURN_V1W_ALL
        seg_mmid[x].w = NULL;       /* Round 10: no deferred grate yet */
#endif
    }
    saturn_dbg_w28 = 0;
    saturn_dbg_w60 = 0;
    saturn_mmid_skip = 0;
    saturn_mmid_paint = 0;
    /* Round-36: fresh verdict each half. Round-40: a MASTER-side bail
     * (2 build / 3 half / 4 dead chain) must SURVIVE this reset — the
     * build bail (3) is set BEFORE the master's render half even
     * starts, and the verdict is consumed in the join poll. */
    if (saturn_seg_bailed < 2)
        saturn_seg_bailed = 0;

    /* round 11b probe reset: HERE, in the function that actually runs
     * the frame (render_columns is the segdiff diagnostic only).
     * mintu 0x7fffffff = no masked call this core this frame. */
    saturn_mm_dbg[cpu].px    = 0;
    saturn_mm_dbg[cpu].rows  = 0;
    saturn_mm_dbg[cpu].mintu = 0x7fffffff;
    saturn_mm_dbg[cpu].maxtu = 0;
    saturn_mm_dbg[cpu].why   = 0;
    saturn_mm_dbg[cpu].calls = 0;

    for (i = 0; i < segwalls_count; i++)
    {
        /* Round-36 slave self-bail: if this half has blown its vblank
         * deadline, the walk is lost (stale-table overrun — the red
         * screen hunt). Stop painting, leave the half's band to the
         * next frame, let the join complete. Master half: deadline is
         * never armed (cpu gate). */
        if (cpu == 1 && saturn_slave_bail_vbl != 0u
            && *seg_vbl_mirror > saturn_slave_bail_vbl)
        {
            saturn_seg_bailed = 1;
            SATURN_HEART_MIRROR[3] = 0xDEADu;
            break;
        }
        /* Round-40: the MASTER's walk gets the same protection — this
         * run's silent freeze hit the unguarded master path. */
        if (SAT_BAIL_CHECK(cpu, 2))
            break;
        segwall_t *w = &segwalls[i];
        int xs = w->start > x0 ? w->start : x0;
        int xe = w->stop < x1 - 1 ? w->stop : x1 - 1;
        side_t *side = &sides[w->side_i];
#if defined(SATURN_VDP1_WALL) && !defined(SATURN_V1W_ALL)
        /* VDP1 multi-quad candidate (2026-09-07): this wall becomes a
         * quad iff its face span stays FULLY inside the open clip band
         * at EVERY walked column — never hidden by nearer geometry, so
         * the quad can never cover a nearer surface, and far-to-near
         * command order renders nested quads correctly (a fully
         * visible farther face is always a screen subset of the nearer
         * one's). State lives across the column loop; committed at the
         * wall's last walked column.
         * 2026-09-07 round N: the gate itself is saturn_v1w_gate
         * (shared with preskip); TOP faces now also admit two-sided
         * UPPER walls (NEWCEILING), and both faces exclude walls that
         * carry AC_MIDTEXTURE (a masked mid on the same line paints
         * over the face at flush time). Allocation failure of the
         * guard arrays disables capture entirely (NULL check). */
        /* Face-window gate (2026-09-08 dud fix): a zero/negative-height
         * face passed every column gate and evaporated at commit's
         * silent entry return — ~64 of 85 commits per frame on E1M1
         * (census S=5515, B=0). Same math as commit's window.
         * (2026-09-08 census CORRECTION: S=515 decimal → commits=2,
         * cells=3 — the dud leak never existed; the gate stays as
         * belt-and-braces. The reject census rj[] now records WHY a
         * wall/face fails the gates — first failing test wins.) */
        int v1w_alive = 0;
        int v1w_alive_b = 0;

        /* preskip v2: retract the previous wall's skip marks — every
         * wall, every frame (master), so a wall that never reaches
         * preskip still paints its face in full */
        if (cpu == 0)
        {
            v1w_map_unmark(0);
            v1w_map_unmark(1);
        }
        if (cpu == 0 && !saturn_v1w_captured)
        {
            if (!(w->actionbits & AC_TOPTEXTURE))
                saturn_v1w_rj[0]++;
            else if (w->t_tex <= 0)
                saturn_v1w_rj[1]++;
            else if (!(w->actionbits & (AC_SOLIDSIL | AC_NEWCEILING)))
                saturn_v1w_rj[2]++;
            else if (w->actionbits & AC_MIDTEXTURE)
                saturn_v1w_rj[3]++;
            else if (((w->t_top - w->t_bot) >> FRACBITS) <= 0)
                saturn_v1w_rj[4]++;
            else if (v1w_spr_ymax == NULL)
                saturn_v1w_rj[5]++;
            else
                v1w_alive = 1;
        }
        /* BOTTOM face (2026-09-07 round N): step/lower walls — same
         * gate machinery, own per-wall state, committed at the same
         * last column. A lower wall always carries AC_NEWFLOOR with
         * AC_BOTTOMTEXTURE (seg setup), so the post-wall clip excludes
         * the face rows exactly like a solid wall does. */
        if (cpu == 0 && !saturn_v1w_captured)
        {
            if (!(w->actionbits & AC_BOTTOMTEXTURE))
                saturn_v1w_rj[0]++;
            else if (w->b_tex <= 0)
                saturn_v1w_rj[1]++;
            else if (w->actionbits & AC_MIDTEXTURE)
                saturn_v1w_rj[3]++;
            else if (((w->b_top - w->b_bot) >> FRACBITS) <= 0)
                saturn_v1w_rj[4]++;
            else if (v1w_spr_ymax == NULL)
                saturn_v1w_rj[5]++;
            else
                v1w_alive_b = 1;
        }
        int v1w_xa = xs, v1w_ya0 = 0, v1w_ya1 = 0;
        int v1w_xa_b = xs, v1w_ya0_b = 0, v1w_ya1_b = 0;
        /* Paint-skip (2026-09-07): preskip proves the commit will add
         * every cell — the quads then cover the face exactly and the
         * software TOP-texture paint is redundant work. nearlo/nearhi
         * and the clip bookkeeping stay (they drive the open-band fill
         * skip). 0 = keep the paint. */
        int v1w_skip = 0, v1w_skip_b = 0;

#if !defined(SATURN_V1W_NOSKIP)
        if (v1w_alive)
            v1w_skip = saturn_v1w_preskip(w, xs, xe, 0);
        if (v1w_alive_b)
            v1w_skip_b = saturn_v1w_preskip(w, xs, xe, 1);
#endif
#endif

        for (x = xs; x <= xe; x++)
        {
            /* master-half poll-acc: each sub-delta = one column + wall
             * glue, far under one 16-bit fold */
            if (cpu == 0)
            {
                SEG_FM_SAMPLE(x);
                saturn_dbg_pairs++;
            }
            else
            {
                /* round 7 RED SCREEN probe: publish walk progress for
                 * the master's watchdog (uncached mirror, see
                 * saturn_render.c). One uncached write per pair —
                 * noise against ~1300+ cycles of glue. */
                saturn_heart_pairs++;
                *SATURN_HEART_MIRROR = (unsigned int)saturn_heart_pairs;
                SAT_SP_MIN_SAMPLE();   /* round-39 stack probe */
            }
            int      scale2 = w->scalefrac + (x - w->start) * w->scalestep;
            unsigned cbv    = cs->clipbounds[x];
            int      ce = cbv >> 8, fl = cbv & 0xff;

#if defined(SATURN_VDP1_WALL) && !defined(SATURN_V1W_ALL)
            /* VDP1 multi-quad capture gate (2026-09-07): evaluated at
             * EVERY walked column (master half only), for the TOP and
             * the BOTTOM face independently. saturn_v1w_gate is the
             * shared kill test (clip band, masked-mid grate, nearer
             * sprite, weapon rect) and hands back the clamped span for
             * the start-corner record; commit runs at the wall's last
             * walked column when the run is >= V1W_MIN_RUN wide. */
            if (v1w_alive)
            {
                int vt, vb;

                if (saturn_v1w_gate(w, 0, x, scale2, ce, fl, &vt, &vb))
                {
                    /* 2026-09-08 FIX: the test must be the BIT test — the
                     * old direct v1w_skimap[0][x] indexed BYTE x of a
                     * 40-byte-per-face bitmap (x up to 319): a kill at
                     * column x read the byte covering columns [8x, 8x+7],
                     * crossed into the other face's array past x=40, and
                     * ran past the array entirely on face 1 — a stable
                     * false skipdie on every preskip-v2 frame. */
                    if (v1w_skip == 1 || (v1w_skip == 2 && V1W_MAP_TEST(0, x)))
                        saturn_v1w_skipdie++;
                    /* SKIP-THEN-DIE (2026-09-07): preskip proved THIS
                     * column's paint skippable (mode 1 = the whole face,
                     * mode 2 = inside a marked run) but the walk's own
                     * gate killed it — the quad preskip counted on never
                     * happens and the skipped paint is a hole. The gate
                     * inputs are identical, so agreement is structural;
                     * any nonzero = divergence = investigate. */
                    /* SUB-RUN COMMIT (2026-09-08 census v3 follow-up): a
                     * gate kill no longer discards the run — the columns
                     * [v1w_xa, x-1] passed the SAME gate, so a quad there
                     * covers exactly the software-painted pixels; commit
                     * them, then close. (First-kill safety: v1w_xa inits
                     * to xs, so a wall killed at its first or second
                     * column computes a 0/1-length run and commits
                     * nothing.) */
                    if (x - v1w_xa >= V1W_MIN_RUN)
                    {
                        saturn_v1w_commit(w, v1w_xa, x - 1, 0);
                        saturn_v1w_commits++;
                    }
                    v1w_alive = 0;      /* hidden / cropped tiny / clipped */
                }
                else
                {
                    if (x == xs)
                    {
                        v1w_xa  = x;
                        v1w_ya0 = vt;
                        v1w_ya1 = vb;
                    }
                    if (x == xe)
                    {
                        /* split at texture-repetition boundaries and
                         * add one quad per repetition (saturn_v1w_commit
                         * recomputes the exact edges from the same
                         * scalefrac math) */
                        if (x - v1w_xa + 1 >= V1W_MIN_RUN)
                        {
                            saturn_v1w_commit(w, v1w_xa, x, 0);
                            saturn_v1w_commits++;
                        }
                        v1w_alive = 0;
                    }
                }
            }
            else if (cpu == 0 && x + V1W_MIN_RUN - 1 <= xe)
            {
                int vt2, vb2;

                if (saturn_v1w_gate(w, 0, x, scale2, ce, fl, &vt2, &vb2) == 0)
                {
                /* SUB-RUN RE-ARM (2026-09-08): a run may start at ANY
                 * column, not only xs — a wall whose face pokes out of
                 * the clip band for a few columns still contributes its
                 * clean spans. The remaining-span check keeps 1-column
                 * arms from ever opening. */
                v1w_alive = 1;
                v1w_xa  = x;
                v1w_ya0 = vt2;
                v1w_ya1 = vb2;
                }
            }
            if (v1w_alive_b)
            {
                int vt, vb;

                if (saturn_v1w_gate(w, 1, x, scale2, ce, fl, &vt, &vb))
                {
                    if (v1w_skip_b == 1
                        || (v1w_skip_b == 2 && V1W_MAP_TEST(1, x)))
                        saturn_v1w_skipdie++;   /* see the top-face note */
                    /* SUB-RUN COMMIT: same as the top face above. */
                    if (x - v1w_xa_b >= V1W_MIN_RUN)
                    {
                        saturn_v1w_commit(w, v1w_xa_b, x - 1, 1);
                        saturn_v1w_commits++;
                    }
                    v1w_alive_b = 0;
                }
                else
                {
                    if (x == xs)
                    {
                        v1w_xa_b  = x;
                        v1w_ya0_b = vt;
                        v1w_ya1_b = vb;
                    }
                    if (x == xe)
                    {
                        if (x - v1w_xa_b + 1 >= V1W_MIN_RUN)
                        {
                            saturn_v1w_commit(w, v1w_xa_b, x, 1);
                            saturn_v1w_commits++;
                        }
                        v1w_alive_b = 0;
                    }
                }
            }
            else if (cpu == 0 && x + V1W_MIN_RUN - 1 <= xe)
            {
                int vt2, vb2;

                if (saturn_v1w_gate(w, 1, x, scale2, ce, fl, &vt2, &vb2) == 0)
                {
                    /* SUB-RUN RE-ARM: bottom face, same as the top. */
                    v1w_alive_b = 1;
                    v1w_xa_b  = x;
                    v1w_ya0_b = vt2;
                    v1w_ya1_b = vb2;
                }
            }
#endif

            /* Closed-clip skip (2026-09-03 real-HW round 3): the clip
             * only ever NARROWS (ADDFLOOR/ADDCEILING pair with
             * NEWFLOOR/NEWCEILING; mid-textures close it), so a column
             * with fl <= ce stays closed for every farther piece. Such
             * a column gives this piece NOTHING: every span clamps
             * empty, both emits no-op behind their bot/top clamps, the
             * clip write and lastsec are no-ops (the open-band fill
             * only reads lastsec where fl > ce), and flush_column is a
             * no-op at pair top (this pair staged nothing yet — every
             * non-skipped pair ends with the per-column flush). The
             * per-pair glue (DIVU setup, FixedMuls, finetangent,
             * merge call) is the measured dominant cost (glue scales
             * with s=pairs), so skip straight to the next column.
             * d32xr parity: R_SegLoop's solidsegs skip. FM_SAMPLE and
             * the w28/w60 census stay ABOVE the skip so the poll-acc
             * and the piece census keep counting every walked pair. */
            if (fl <= ce)
                continue;

            /* Round 27c: count the pieces that reach the probe columns */
            if (x == 28)
                saturn_dbg_w28++;
            else if (x == 60)
                saturn_dbg_w60++;

            /* round 14: snapshot seg 7's col-28 entry state */
            if (x == 28 && i == 7)
            {
                saturn_dbg_clip_before = (int)cbv;
                saturn_dbg_scale2_28   = scale2;
            }
            /* P5 Phase 2a (2026-09-10): the per-column body moved
             * VERBATIM into seg_column_body above — single call site,
             * the compiler inlines it back into the walk. */
            seg_column_body(w, side, x, i, scale2, ce, fl, cpu, cs,
                            use_divu, &frt_fire_acc, &frt_vfire_acc,
                            &frt_walk_acc, &frt_vwalk_acc, 1);
        }
    }

    frt_all_acc += (unsigned short)(SEG_SAT_FRT() - t_all);
    t_all = SEG_SAT_FRT();

    /* Final open-band fill â€” ray parity. The raycaster's walk-end emit
     * (saturn_render.c, the !covered branch) paints whatever its
     * crossing loop left open with the sector it ended in, split at the
     * horizon. The seg pass has no walk end: rows whose owning wall's
     * ADDFLOOR/ADDCEILING never fired (floor continuing into a
     * properties-identical back sector) stayed clear â€” the blue floor
     * holes. d32xr sidesteps this with a separate visplane pass the
     * port doesn't have; the open band [ce,fl) here is exactly the
     * ray's remaining band, and its owner is the deepest wall's back
     * sector (lastsec, stored above). Safe against double-paint:
     * ADDFLOOR/ADDCEILING/ADDSKY always pair with NEWFLOOR/NEWCEILING,
     * so any emitted band already narrowed the clip out of itself.
     * Round 24: the fill must ALSO stay off the rows the nearest wall
     * PAINTED â€” for a one-sided wall the post-wall clip IS the wall's
     * face span, and filling it drew floor/ceiling flats over the art,
     * split at the horizon (the "wall split in half" family). d32xr
     * never fills those rows: no visplane covers them and the wall art
     * simply stays. Round 26c (2026-09-02): "stay off the painted rows"
     * is enforced ROW-exactly with the nearlo..nearhi span instead of
     * the round-25 whole-column boolean â€” a distant one-sided wall
     * paints only a few rows of an otherwise still-open clip (ceiling
     * below the eye: ADDCEILING legitimately never fires), and the
     * boolean skipped those columns' whole fill, leaving full-height
     * unpainted clear bands (the 20:46/20:47 screenshots). The band
     * minus the painted span is emitted as up to two sub-bands per
     * side. Sim sweep over E1M1 (sim/sim_segw.py sweep): 28 hole poses
     * with the boolean, 0 with the span. */
    for (x = x0; x < x1; x++)
    {
        /* Round-36 slave self-bail (same deadline as the wall walk). */
        if (cpu == 1 && saturn_slave_bail_vbl != 0u
            && *seg_vbl_mirror > saturn_slave_bail_vbl)
        {
            saturn_seg_bailed = 1;
            SATURN_HEART_MIRROR[3] = 0xDEADu;
            break;
        }
        /* Round-40: master fill gets the deadline + dead-chain checks. */
        if (SAT_BAIL_CHECK(cpu, 2))
            break;
        /* master-half poll-acc: same per-column sampling (the final
         * open-band fill is a full second pass over the half) */
        if (cpu == 0)
        {
            SEG_FM_SAMPLE(x);
        }
        else
        {
            /* round 7: +1000 per open-band column so the watchdog's
             * heart snapshot names THIS pass, not the wall walk. */
            saturn_heart_pairs += 1000;
            *SATURN_HEART_MIRROR = (unsigned int)saturn_heart_pairs;
            SAT_SP_MIN_SAMPLE();   /* round-39 stack probe */
        }
        unsigned cbv = cs->clipbounds[x];
        int      ce  = cbv >> 8, fl = cbv & 0xff;
        int      y0, y1;
        /* Safety net (2026-09-02): a wild lastsec used to hand the flat
         * painter a garbage sector -> garbage floorpic -> a wild flat
         * pointer (the screenful of RGB noise). Fold any out-of-range
         * owner back to the view sector; saturn_ls_bad counts it (the
         * census sits after both skips so it only counts fills that
         * actually fired â€” solid walls legitimately carry
         * the emptysector delta). */
        unsigned ls = cs->lastsec[x];
        /* rows walls painted at this column (nearlo > nearhi = none) */
        int      slo = cs->nearlo[x], shi = cs->nearhi[x];

        if (fl <= ce)
        {
            /* Round 10: the clip can close AFTER a masked grate was
             * recorded (a farther solid wall's face covers the band).
             * Round 11d: the grate itself now paints in the post-flush
             * pass (below, after saturn_seg_flush_pending), so this
             * path has nothing to skip to — and with ce >= fl every
             * band below is empty, so falling through is free. */
        }

        if (ls >= (unsigned)numsectors)
        {
            saturn_ls_bad++;
            ls = (unsigned)seg_viewsec_i;
        }

        /* ceiling band [max(ce,VIEW_TOP), min(fl,HALF_H)) minus the
         * painted span, as up to two sub-bands */
        y0 = ce > VIEW_TOP ? ce : VIEW_TOP;
        y1 = fl < HALF_H ? fl : HALF_H;
        if (slo <= shi)
        {
            int b = slo < y1 ? slo : y1;
            int a;

            if (y0 < b)
            {
                SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                    fr_emit_flat(x, y0, b, &sectors[ls], 0, cpu));
            }
            a = shi + 1 > y0 ? shi + 1 : y0;
            if (a < y1)
            {
                SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                    fr_emit_flat(x, a, y1, &sectors[ls], 0, cpu));
            }
        }
        else if (y0 < y1)
        {
            SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                fr_emit_flat(x, y0, y1, &sectors[ls], 0, cpu));
        }
        /* floor band [max(ce,HALF_H), min(fl,VIEW_BOT)) minus the
         * painted span, as up to two sub-bands */
        y0 = ce > HALF_H ? ce : HALF_H;
        y1 = fl < VIEW_BOT ? fl : VIEW_BOT;
        if (slo <= shi)
        {
            int b = slo < y1 ? slo : y1;
            int a;

            if (y0 < b)
            {
                SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                    fr_emit_flat(x, y0, b, &sectors[ls], 1, cpu));
            }
            a = shi + 1 > y0 ? shi + 1 : y0;
            if (a < y1)
            {
                SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                    fr_emit_flat(x, a, y1, &sectors[ls], 1, cpu));
            }
        }
        else if (y0 < y1)
        {
            SEG_FRT_SUB(frt_fire_acc, frt_vfire_acc,
                fr_emit_flat(x, y0, y1, &sectors[ls], 1, cpu));
        }
        /* fr_emit_flat only STAGES; without this per-column merge the
         * staged spans accumulate across all 160 columns â€” under the
         * staging cap the final flush never paints them at all (only
         * FR_PENDING is flushed, and these never got merged into it),
         * and past the cap the safety valve merges a garbled
         * multi-column batch. Same contract as the wall loop above. */
        saturn_seg_flush_column(x, cpu);

        /* Round 11d: the deferred masked grate NO LONGER paints here.
         * In-walk it painted BEFORE flush_pending — and the fill's
         * pending runs (staged across the whole open band, nearlo/
         * nearhi deliberately untouched) painted OVER the fresh grate
         * at the mismatch/frame-end flush. That was the X-grate bug:
         * 86 grate columns painted, byte-perfect composite (h=296/297),
         * and the wall still read as a banded expanse of sector-65
         * flats (FLOOR5_2/CEIL3_5). The grate now paints in the
         * post-flush pass below, after every flat paint — vanilla's
         * contract (flats first, masked column over them, holes show
         * the fill) — and before seg_draw_sprites. */
    }

#ifdef SATURN_SEG_RENDERER
    /* BISECT round 9: the slave draws its FINAL clip state at band
     * col 28 directly into the framebuffer (row 56, retired 'J' row)
     * â€” cross-core counter reads are unreliable, fb writes are not. */
    if (cpu == 1)
    {
        extern void Saturn_HudNum(int y, char label, unsigned int v);
        extern volatile int saturn_dbg_s_clip;
        extern volatile int saturn_dbg_w28;
        extern volatile int saturn_dbg_w60;
        extern volatile int saturn_fd28_cnt;
        extern volatile int saturn_fd28_span;
        extern volatile int saturn_fd28_floor;
        extern volatile int saturn_fd60_cnt;
        extern volatile int saturn_fd60_span;
        extern volatile int saturn_fd60_floor;

        saturn_dbg_s_clip = (int)cs->clipbounds[28];
        {
            /* Round 27b: the col-28 walk story EVERY tic (the odd-tic
             * segwalls[7] story did its job â€” the negative-distance
             * anomaly is understood and neutralized by the late-prep
             * dead-wall guard â€” and parity gating made half the user's
             * captures inconclusive). */
            /* Round 28k (2026-09-03): the whole slave HUD block RETIRED.
             * The magenta glyphs printed over gameplay (rows 24-56 over
             * ceilings, 164-196 over the floor and sprites) and were
             * reported as texture corruption twice (the "magenta on the
             * door / on the corpse" captures 005706). Their missions are
             * banked: col-28 band + col-60 slab fixed and verified on
             * the production ISO, clip/near/close probes answered.
             * Re-enable individually for a future probe session.
            Saturn_HudNum(24, 'L', (unsigned int)clipbounds[60]);
            Saturn_HudNum(40, 'J', (unsigned int)clipbounds[28]);
            Saturn_HudNum(48, 'H', (unsigned int)saturn_dbg_close_i);
            Saturn_HudNum(56, 'I', (unsigned int)saturn_dbg_near_i); */
            /* Round 27j pending-fate probes (H/I/P/M/C/E rows) RETIRED
             * 2026-09-03: they only recorded inside FLAT_DIAG, so on
             * the production build they printed constant 0 while still
             * costing six Saturn_HudNum glyph draws per frame. Their
             * job is done â€” the col-28 band AND the col-60 slab are
             * confirmed fixed on the production ISO (captures 003507 /
             * 003540, zero salmon anywhere, fully textured). */
            /* Round 27j: the n(72) near_span print retired â€” the
             * mismatch-paint probe's M row owns row 72 now (near_span
             * duplicated J's value anyway in every recent capture). */
            /* Round 27c: the LIVE staging records (the old W/A rows read
             * an inert FLAT_DIAG probe â€” stale forever). W = last span
             * staged at col 28, A = (stage count<<1)|isfloor of it.
             * E/F = the same pair inside the black slab (col 60).
             * G/H = how many pieces PROCESSED col 28 / col 60. */
            /* Round 27e (2026-09-02): rows spaced 8 apart on the free
             * rows (the 27d build packed them 4 apart and the 5-px-tall
             * glyphs merged into one unreadable band in the captures). */
            /* Round 27i: the W(156) fd28_span print retired for the
             * fork probe's E row (same row, see the block above). */
            /* Round 28k: A/E/F/G/L rows retired with the block above â€”
             * rows 164-196 printed straight over the floor and sprites.
            Saturn_HudNum(164, 'A',
                          (unsigned int)((saturn_fd28_cnt << 1)
                                         | saturn_fd28_floor));
            Saturn_HudNum(172, 'E', (unsigned int)saturn_fd60_span);
            Saturn_HudNum(180, 'F',
                          (unsigned int)((saturn_fd60_cnt << 1)
                                         | saturn_fd60_floor));
            Saturn_HudNum(188, 'G',
                          (unsigned int)((saturn_dbg_w28 << 8)
                                         | saturn_dbg_w60));
            Saturn_HudNum(196, 'L', (unsigned int)clipbounds[60]); */
        }
    }
#endif

    frt_all_acc += (unsigned short)(SEG_SAT_FRT() - t_all);
    t_all = SEG_SAT_FRT();
    if (cpu == 0)
    {
        saturn_frt_vwalls = (int)(saturn_vbl_count - tw0);
        tw0 = saturn_vbl_count;
    }

    /* 2026-09-03 photo #150: the flush_pending leg is one single
     * 16-bit delta (foldable, unstamped by any named row) â€” a prime
     * suspect for the master's missing ~50 ms. Name it (psite=2). */
    if (cpu == 0)
    {
        unsigned short tf_ = SEG_SAT_FRT();

        saturn_master_site = 6;   /* round-50: entering flush_pending */
        saturn_seg_flush_pending(cpu);
        saturn_frt_flush = (int)((unsigned short)(SEG_SAT_FRT() - tf_) >> 8);
        if (saturn_frt_flush > saturn_frt_paintmax)
        {
            saturn_frt_paintmax = saturn_frt_flush;
            saturn_frt_psite = 2;
        }
    }
    else
        saturn_seg_flush_pending(cpu);
    frt_all_acc += (unsigned short)(SEG_SAT_FRT() - t_all);
    /* master-half poll-acc: close the flush_pending leg before the
     * sprite pass (its cost lands in its own sub-delta) */
    if (cpu == 0)
    {
        SEG_FM_SAMPLE(-1);
        saturn_frt_vfill = (int)(saturn_vbl_count - tw0);
        tw0 = saturn_vbl_count;
        saturn_master_site = 7;   /* round-50: flush_pending returned */
    }

    /* things/sprites: AFTER every wall/flat row is on screen, clipped
     * per column against the nearer segwall pieces. Cost lands in T
     * (and S as the remainder) via the stamps around it. */
    t_all = SEG_SAT_FRT();
    /* 2026-09-03 photo #150: the sprite pass is the other single-delta
     * leg â€” name it too (psite=3). */
    if (cpu == 0)
    {
        unsigned short tf_ = SEG_SAT_FRT();

        saturn_master_site = 8;   /* round-50: entering sprite pass */
        if (spr_vis_count)
            seg_draw_sprites(x0, x1, cpu);
        saturn_frt_spr = (int)((unsigned short)(SEG_SAT_FRT() - tf_) >> 8);
        if (saturn_frt_spr > saturn_frt_paintmax)
        {
            saturn_frt_paintmax = saturn_frt_spr;
            saturn_frt_psite = 3;
        }
    }
    else if (spr_vis_count)
        seg_draw_sprites(x0, x1, cpu);
    frt_all_acc += (unsigned short)(SEG_SAT_FRT() - t_all);

    /* 2026-09-06 (user shots): the deferred masked grates now paint
     * AFTER the sprite pass — vanilla R_DrawMasked order (sprites
     * back-to-front first, then every maskedtexturecol seg over them),
     * so things behind a grate show through the holes instead of
     * painting over the grate. The round-10 masked path leaves the
     * clip OPEN, so the grate is the only thing between the sprite
     * and the camera here. Depth re-derived per column exactly like
     * the walk did (offset - dist*tangent); the plain unsigned divide
     * is fine — rare path, a few columns a frame, DIVU is serial. */
    t_all = SEG_SAT_FRT();
    {
        int xg;

#ifdef SATURN_VDP1_WALL
#if !defined(SATURN_V1W_NOSKIP) && !defined(SATURN_V1W_STRIPES)
        /* Stage 2a: convert this half's recorded grates to AddMid quads
         * and mark their columns BEFORE the paint loop below — the
         * marked columns then skip paint_wall_span_masked (the quads
         * repaint them on the VDP1, above NBG0).
         * P5 Phase 3 (2026-09-10): under the full-VDP1 define the
         * capture pass already converted this frame's grates AT THEIR
         * WALLS' TABLE POSITIONS (verdict C) and marked face 2 — this
         * prescan's after-the-walk appends would file the quads at the
         * table tail = drawn FIRST = UNDER every wall quad, the
         * pre-Phase-3 order. The flush loop below keeps skipping the
         * marked columns and software-painting the fail-open ones. */
#if !defined(SATURN_V1W_ALL)
        if (cpu == 0)
            saturn_v1w_mid_prescan(x0, x1);
#endif
#endif
#endif

        for (xg = x0; xg < x1; xg++)
        {
            segwall_t *mw;

            if (seg_mmid[xg].w == NULL)
                continue;
            mw = seg_mmid[xg].w;
#ifdef SATURN_VDP1_WALL
#if !defined(SATURN_V1W_NOSKIP) && !defined(SATURN_V1W_STRIPES)
            if (cpu == 0 && V1W_MAP_TEST(2, xg))
            {
                saturn_v1w_skipmid++;
                seg_mmid[xg].w = NULL;  /* paint once — never carry over */
                continue;
            }
#endif
#endif
            /* round 11b fill-side capture: what the holes in this grate
             * just showed. lastsec[x] is the walk-end owner (the fill's
             * lastsec for the column, folded). */
            {
                unsigned lsc = (unsigned)cs->lastsec[xg];

                if (lsc >= (unsigned)numsectors)
                    lsc = (unsigned)seg_viewsec_i;
                saturn_mm_fill_sec   = (int)cs->lastsec[xg];
                saturn_mm_fill_light = (int)sectors[lsc].lightlevel;
            }
            {
                int        scale2m = mw->scalefrac
                                   + (xg - mw->start) * mw->scalestep;
                unsigned   denom = (unsigned)(scale2m >= 1 ? scale2m : 1);
                unsigned   iscalem;
                int        rr, u_raw;

                /* Same pipelined DIVU idiom as the solid path above:
                 * issue first, hide the 13-16 cycle latency under the
                 * finetangent/FixedMul u_raw work, then read DVDNTL.
                 * (A reciprocal LUT was tried here 2026-09-06 and
                 * RETIRED: gcc -m2 compiles every variable shift into
                 * a libgcc __lshrsi3/__ashlsi3 call loop — no shad/
                 * shld on SH-2 — which ate the LUT's whole win. See
                 * sim/make_divtab.py.) */
                if (saturn_divu_probe)
                {
                    SATURN_DIVU_DVSR   = (int)denom;
                    SATURN_DIVU_DVDNTH = 0;
                    SATURN_DIVU_DVDNTL = (int)0xffffffffu;   /* start */
                }

                rr = finetangent((mw->centerangle
                     + ((unsigned)seg_xtoviewangle[xg] << FRACBITS))
                     >> ANGLETOFINESHIFT);
                u_raw = (mw->offset - FixedMul(mw->distance, rr))
                        >> FRACBITS;

                if (saturn_divu_probe)
                {
                    __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
                    iscalem = (unsigned)SATURN_DIVU_DVDNTL;
                }
                else
                    iscalem = 0xffffffffu / denom;

                int64_t    ttm = ((int64_t)PROJ * (int64_t)iscalem) >> 16;
                int        t_um;

                if (ttm > 30000)     t_um = 30000;
                else if (ttm < 1)    t_um = 1;
                else                 t_um = (int)ttm;

                paint_wall_span_masked(xg, seg_mmid[xg].y0, seg_mmid[xg].y1,
                                       mw->m_tex, seg_mmid[xg].side,
                                       seg_mmid[xg].nside, u_raw,
                                       mw->t_peg_abs, t_um, cpu);
                saturn_mmid_paint++;
                seg_mmid[xg].w = NULL;   /* paint once — never carry it over */
            }
        }
    }
    frt_all_acc += (unsigned short)(SEG_SAT_FRT() - t_all);
    if (cpu == 0)
    {
        extern int saturn_tex_comp_count;   /* saturn_tex.c */

        saturn_frt_vspr  = (int)(saturn_vbl_count - tw0);
        saturn_frt_rcomp = saturn_tex_comp_count - rcomp0;
    }

    if (cpu == 0)
    {
        /* Same >>8 publish as the raycaster's (1 printed unit = 256
         * ticks = 0.198 ms). L = the flat kernel window accumulated
         * inside saturn_render.c's painter; S is computed as the
         * remainder so the four sub-windows always sum to T exactly
         * (S = flat merge machinery + per-column glue). */
        {
            /* master-half poll-acc: final leg = the sprite pass */
            unsigned short fm = SEG_SAT_FRT();

            frt_m_acc += (unsigned short)(fm - fm_last);
            saturn_frt_macc = (int)frt_m_acc;
        }
        saturn_frt_fire  = (int)(frt_fire_acc >> 8);
        saturn_frt_walk  = (int)(frt_walk_acc >> 8);
        saturn_frt_ploop = (int)(saturn_frt_ploop_kernel() >> 8);
        saturn_frt_total = (int)(frt_all_acc >> 8);
        /* S (2026-09-03 real-HW round 3): the >>8 section stamps
         * (frt_all_acc) SPAN multiple vblanks — the wall loop alone is
         * ~9 vbl at 21,814 ticks/vbl — so their 16-bit deltas fold and
         * frt_all_acc is phase junk (photo: S read 577214, I read 0).
         * Every accumulator honest on HW is built from sub-vbl deltas,
         * so compute S from those instead: macc (per-pair poll, covers
         * wall loop + open band, FM_SAMPLE sits above the closed-clip
         * skip) minus the three painted legs. ploop may also contain
         * frame-end flush_pending paints that fall OUTSIDE macc, hence
         * the clamp. S = glue + in-loop flat-kernel + merge paints. */
        {
            unsigned int m    = (unsigned int)frt_m_acc;
            unsigned int legs = frt_walk_acc + frt_fire_acc
                                + saturn_frt_ploop_kernel();

            saturn_frt_scan = (m > legs) ? (int)((m - legs) >> 8) : 0;
        }
        /* Vbl-span split of the wall loop (FRT is dead on real HW):
         * vwalk/vfire = vblanks whose edge fell inside a wall paint /
         * flat emit; the rest of vwalls is glue (staging, clip math,
         * divides, column bookkeeping). */
        saturn_frt_vwalk = (int)frt_vwalk_acc;
        saturn_frt_vfire = (int)frt_vfire_acc;
    }

#ifdef SATURN_SEG_DIFF
    /* Hole census: hand the master's final clip/owner state to the diff
     * compare loop (it finds the first fully-blue column and prints this
     * column's clip + owning sector on the D/H rows). Slave half skipped
     * â€” its state would need a second shared array + coherency care. */
    if (cpu == 0)
    {
        extern volatile unsigned short saturn_dbg_clip[];
        extern volatile unsigned short saturn_dbg_lastsec[];

        for (x = 0; x <= SCREEN_W; x++)
        {
            saturn_dbg_clip[x]    = cs->clipbounds[x];
            saturn_dbg_lastsec[x] = cs->lastsec[x];
        }
    }
#endif
}