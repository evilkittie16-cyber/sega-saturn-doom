/* saturn_bench.c — benchmark tour (build with -DSATURN_BENCH=1), 2026-11-16.
 *
 * Measures the renderer over whole levels instead of one spot. The game
 * skips the menu, freezes the world (no thinkers), and for each level
 * E1M1..E1M<SATURN_BENCH_MAPS> moves the camera through up to BENCH_POS
 * viewpoints (subsector centres spread over the map) x BENCH_ANGLES angles.
 * Each view renders BENCH_SETTLE frames (texture cache, camera interpolation)
 * and then one measured frame:
 *   render  master time in Saturn_HW_RenderPlayerView (FRT ticks)
 *   wait    of which waiting for the slave's flat jobs
 *   vdp1    VDP1 draw time of that frame's command list (Saturn_Vdp1_
 *           BenchDrawTime: frame change -> EDSR.CEF), measured by spinning
 *   quads   wall / flat quads, subsectors visited
 * Per level the Saturn computes median / 90th percentile / max of render and
 * vdp1 (0.1 ms units), quad and subsector averages, and the 4 worst views
 * (by max(render, vdp1)), into saturn_bench_sum[]. When every level is done
 * saturn_bench_done = 1 and the screen shows the table (hardware readout);
 * 5thPlanet reads saturn_bench_sum with sdbg `m`.
 */
#include "doomdef.h"
#include "p_local.h"
#include "saturn.h"
#include "saturn_vgl.h"

#ifndef SATURN_BENCH
#define SATURN_BENCH 0
#endif

#if SATURN_BENCH

#ifndef SATURN_BENCH_MAPS
#define SATURN_BENCH_MAPS 9
#endif
#ifndef BENCH_POS
#ifdef BENCH_HOLD_MAP
#define BENCH_POS 1     /* hold mode uses one view: keeps __bend under 0x060C0000 */
#else
#define BENCH_POS 32
#endif
#endif
#ifndef BENCH_ANGLES
#define BENCH_ANGLES 8
#endif
#ifndef BENCH_SETTLE
#define BENCH_SETTLE 3
#endif
#define BENCH_MAX_VIEWS (BENCH_POS * BENCH_ANGLES)
/* Hold mode: -DBENCH_HOLD_MAP=m -DBENCH_HOLD_X=x -DBENCH_HOLD_Y=y
 * -DBENCH_HOLD_A8=a parks the camera on that one view (angle a * 45 deg)
 * and keeps re-measuring it, for sdbg `prof` of a single viewpoint. */
#ifdef BENCH_HOLD_MAP
#define BENCH_HOLD 1
#else
#define BENCH_HOLD 0
#endif
#define BENCH_WORST 4

typedef struct {
    int16_t  x, y;
    uint8_t  ang8;
    uint8_t  pad;
    uint16_t subs;
    uint16_t wq, fq;
    uint32_t render, wait, vdp1;     /* FRT ticks */
} bench_rec_t;

typedef struct {
    int16_t  x, y;
    uint16_t ang8;
    uint16_t render, vdp1;           /* 0.1 ms */
} bench_worst_t;

typedef struct {
    uint16_t map, views;
    uint16_t r_p50, r_p90, r_max;    /* master render, 0.1 ms */
    uint16_t v_p50, v_p90, v_max;    /* VDP1 draw, 0.1 ms */
    uint16_t w_p50, w_p90;           /* slave wait inside render, 0.1 ms */
    uint16_t q_avg, s_avg;           /* quads (wall + flat), subsectors */
    uint16_t over_r, over_v;         /* views with render / vdp1 > 16.7 ms */
    bench_worst_t worst[BENCH_WORST];
} bench_sum_t;

/* Read by 5thPlanet (sdbg `m`): keep them global and simple. */
volatile int         saturn_bench_done = 0;
volatile int         saturn_bench_rate = 0;      /* FRT ticks per 1/60 s */
bench_sum_t          saturn_bench_sum[SATURN_BENCH_MAPS];
/* Slave idle summary per map (FRT ticks): [0] sum render [1] sum slave busy
 * [2] views [3] views with render > 1/60 s [4] slave busy in those views
 * [5] render in those views [6] slow views where the slave was idle > 1/3
 * of render [7] sum slave wait (master waiting on the slave) */
uint32_t             saturn_bench_idle[SATURN_BENCH_MAPS][8];
uint32_t             saturn_hw_prof_slave_busy = 0;
static bench_rec_t   bench_rec[BENCH_MAX_VIEWS];

static int16_t       bench_pos[BENCH_POS][2];
static int           bench_npos, bench_nviews;
#if BENCH_HOLD
static int           bench_map = BENCH_HOLD_MAP;
#else
static int           bench_map = 1;              /* level being measured */
#endif
static int           bench_built = 0;            /* viewpoints built for bench_map */
static int           bench_view = 0;             /* current view index */
static int           bench_frame = 0;            /* frames rendered at this view */
static int           bench_measured = 0;         /* this view's measured frame is out */

extern int saturn_map;
extern void Saturn_WarpToLevel(int episode, int map);
extern volatile int saturn_frt_rate;

int Saturn_Bench_Active(void) { return 1; }
int Saturn_Bench_Done(void)   { return saturn_bench_done; }

static unsigned int bench_tenths(unsigned int ticks)
{
    /* ms = ticks / rate * 16.667  ->  tenths = ticks * 500 / (rate * 3) */
    unsigned int rate = (unsigned int)saturn_frt_rate;
    unsigned int t;
    if (rate == 0)
        return 0;
    if (ticks > 8000000u)               /* keep ticks * 500 in 32 bits */
        ticks = 8000000u;
    t = (ticks * 500u) / (rate * 3u);
    return t > 65535u ? 65535u : t;
}

/* Viewpoints: the vertex centroid of every k-th subsector (subsectors are
 * convex, so the centroid is inside), skipping closed or low sectors. */
static void bench_build_views(void)
{
    int nss = numsubsectors;
    int stride = nss / BENCH_POS;
    int i;
    if (stride < 1)
        stride = 1;
    bench_npos = 0;
#if BENCH_HOLD
    bench_pos[0][0] = (int16_t)(BENCH_HOLD_X);
    bench_pos[0][1] = (int16_t)(BENCH_HOLD_Y);
    bench_npos   = 1;
    bench_nviews = 1;
    (void)nss; (void)i;
    return;
#endif
    for (i = stride / 2; i < nss - 1 && bench_npos < BENCH_POS; i += stride)
    {
        subsector_t *ss = &subsectors[i];
        int n = SSEC_NUMLINES(ss);
        int k;
        int sx = 0, sy = 0;
        sector_t *sec;
        if (n <= 0)
            continue;
        for (k = 0; k < n; k++)
        {
            const seg_t *sg = &segs[ss->firstline + k];
            sx += vertexes[SEG_UNPACK_V1(sg)].x;
            sy += vertexes[SEG_UNPACK_V1(sg)].y;
        }
        sx /= n;
        sy /= n;
        sec = SSEC_SECTOR(ss);
        if (sec->ceilingheight - sec->floorheight < (64 << FRACBITS))
            continue;
        if (R_PointInSubsector((fixed_t)sx << FRACBITS, (fixed_t)sy << FRACBITS) != ss)
            continue;
        bench_pos[bench_npos][0] = (int16_t)sx;
        bench_pos[bench_npos][1] = (int16_t)sy;
        bench_npos++;
    }
    bench_nviews = bench_npos * BENCH_ANGLES;
    if (bench_nviews > BENCH_MAX_VIEWS)
        bench_nviews = BENCH_MAX_VIEWS;
}

static void bench_place_camera(int view)
{
    mobj_t *mo = players[0].mo;
#if BENCH_HOLD
    int     p  = 0;
    int     a  = (BENCH_HOLD_A8);
    (void)view;
#else
    int     p  = view / BENCH_ANGLES;
    int     a  = view % BENCH_ANGLES;
#endif
    if (!mo)
        return;
    P_UnsetThingPosition(mo);
    mo->x = (fixed_t)bench_pos[p][0] << FRACBITS;
    mo->y = (fixed_t)bench_pos[p][1] << FRACBITS;
    P_SetThingPosition(mo);
    if (mo->subsector)
    {
        sector_t *sec = SSEC_SECTOR(mo->subsector);
        mo->floorz   = sec->floorheight;
        mo->ceilingz = sec->ceilingheight;
        mo->z        = mo->floorz;
    }
    mo->angle = (angle_t)a * (ANG90 / 2);
#if BENCH_HOLD && defined(BENCH_HOLD_DEG)
    /* exact angle in degrees (e.g. to match a hardware photo's HUD "A57") */
    mo->angle = (angle_t)((unsigned int)(BENCH_HOLD_DEG) * (ANG90 / 90u));
#endif
#if BENCH_HOLD && defined(BENCH_HOLD_TURN)
    {
        /* turning hold: BENCH_HOLD_TURN degrees more at every placement */
        static unsigned int bench_turn_n = 0;
        mo->angle += (angle_t)(bench_turn_n++ * (unsigned int)(BENCH_HOLD_TURN) * (ANG90 / 90u));
    }
#endif
#if BENCH_HOLD && defined(BENCH_HOLD_WALK)
    {
        /* walking hold: BENCH_HOLD_WALK units further along the view angle at
         * every placement, looping back to the start after BENCH_HOLD_WALKN */
#ifndef BENCH_HOLD_WALKN
#define BENCH_HOLD_WALKN 32
#endif
        static unsigned int bench_walk_n = 0;
        fixed_t d = (fixed_t)((bench_walk_n++ % (BENCH_HOLD_WALKN)) * (BENCH_HOLD_WALK)) << FRACBITS;
        unsigned int fa = mo->angle >> ANGLETOFINESHIFT;
        P_UnsetThingPosition(mo);
        mo->x += FixedMul(d, finecosine(fa));
        mo->y += FixedMul(d, finesine(fa));
        P_SetThingPosition(mo);
        if (mo->subsector)
        {
            sector_t *sec = SSEC_SECTOR(mo->subsector);
            mo->floorz = sec->floorheight;
            mo->ceilingz = sec->ceilingheight;
            mo->z = mo->floorz;
        }
    }
#endif
    mo->momx = mo->momy = mo->momz = 0;
    players[0].viewz = mo->z + VIEWHEIGHT;
    players[0].viewheight = VIEWHEIGHT;
    players[0].deltaviewheight = 0;
    players[0].cheats |= CF_GODMODE;
    players[0].health = 100;
    mo->health = 100;
#if BENCH_HOLD && defined(BENCH_HOLD_DEAD)
    /* dead-player eye (P_DeathThink settles viewheight at 6): reproduces a
     * hardware photo taken after dying */
    players[0].playerstate = PST_DEAD;
    players[0].viewheight  = 6 * FRACUNIT;
    players[0].viewz       = mo->z + 6 * FRACUNIT;
#endif
    bench_rec[view].x    = bench_pos[p][0];
    bench_rec[view].y    = bench_pos[p][1];
    bench_rec[view].ang8 = (uint8_t)a;
}

static void bench_sort(uint32_t *v, int n)
{
    int i, j;
    for (i = 1; i < n; i++)
    {
        uint32_t x = v[i];
        for (j = i - 1; j >= 0 && v[j] > x; j--)
            v[j + 1] = v[j];
        v[j + 1] = x;
    }
}

static void bench_summarize(void)
{
    static uint32_t tmp[BENCH_MAX_VIEWS];
    bench_sum_t *s = &saturn_bench_sum[bench_map - 1];
    int n = bench_nviews;
    int i, w;
    unsigned int qsum = 0, ssum = 0;
    const unsigned int frame = (unsigned int)saturn_frt_rate;   /* 1/60 s */

    s->map = (uint16_t)bench_map;
    s->views = (uint16_t)n;
    s->over_r = s->over_v = 0;
    if (n <= 0)
        return;
    for (i = 0; i < n; i++)
    {
        tmp[i] = bench_rec[i].render;
        qsum += bench_rec[i].wq + bench_rec[i].fq;
        ssum += bench_rec[i].subs;
        if (bench_rec[i].render > frame) s->over_r++;
        if (bench_rec[i].vdp1 > frame)   s->over_v++;
    }
    bench_sort(tmp, n);
    s->r_p50 = (uint16_t)bench_tenths(tmp[n / 2]);
    s->r_p90 = (uint16_t)bench_tenths(tmp[(n * 9) / 10]);
    s->r_max = (uint16_t)bench_tenths(tmp[n - 1]);
    for (i = 0; i < n; i++)
        tmp[i] = bench_rec[i].vdp1;
    bench_sort(tmp, n);
    s->v_p50 = (uint16_t)bench_tenths(tmp[n / 2]);
    s->v_p90 = (uint16_t)bench_tenths(tmp[(n * 9) / 10]);
    s->v_max = (uint16_t)bench_tenths(tmp[n - 1]);
    for (i = 0; i < n; i++)
        tmp[i] = bench_rec[i].wait;
    bench_sort(tmp, n);
    s->w_p50 = (uint16_t)bench_tenths(tmp[n / 2]);
    s->w_p90 = (uint16_t)bench_tenths(tmp[(n * 9) / 10]);
    s->q_avg = (uint16_t)(qsum / (unsigned int)n);
    s->s_avg = (uint16_t)(ssum / (unsigned int)n);

    /* worst views by max(render, vdp1) */
    for (w = 0; w < BENCH_WORST; w++)
    {
        int best = -1;
        uint32_t best_v = 0;
        for (i = 0; i < n; i++)
        {
            uint32_t v = bench_rec[i].render > bench_rec[i].vdp1 ? bench_rec[i].render : bench_rec[i].vdp1;
            int taken = 0, k;
            for (k = 0; k < w; k++)
                if (s->worst[k].x == bench_rec[i].x && s->worst[k].y == bench_rec[i].y &&
                    s->worst[k].ang8 == bench_rec[i].ang8)
                    taken = 1;
            if (!taken && v >= best_v)
            {
                best_v = v;
                best = i;
            }
        }
        if (best < 0)
            break;
        s->worst[w].x      = bench_rec[best].x;
        s->worst[w].y      = bench_rec[best].y;
        s->worst[w].ang8   = bench_rec[best].ang8;
        s->worst[w].render = (uint16_t)bench_tenths(bench_rec[best].render);
        s->worst[w].vdp1   = (uint16_t)bench_tenths(bench_rec[best].vdp1);
    }
}

/* Main loop, top of every iteration (d_main.c). Returns 1 when it used the
 * iteration for a level warp. */
int Saturn_Bench_Step(void)
{
    if (saturn_bench_done)
        return 0;
    saturn_bench_rate = saturn_frt_rate;

    if (saturn_map != bench_map)
    {
        bench_built = 0;
#ifndef BENCH_HOLD_EP
#define BENCH_HOLD_EP 1     /* -DBENCH_HOLD_EP=3 for E3Mx (retail WAD) */
#endif
        Saturn_WarpToLevel(BENCH_HOLD_EP, bench_map);
        return 1;
    }
    if (!bench_built)
    {
        bench_build_views();
        bench_built = 1;
        bench_view = 0;
        bench_frame = 0;
        bench_measured = 0;
        if (bench_nviews > 0)
            bench_place_camera(0);
    }

    /* The previous iteration submitted this view's measured frame: time its
     * VDP1 draw, then move on. */
    if (bench_measured)
    {
        extern volatile unsigned int saturn_bench_vdp1_ticks;
        bench_rec[bench_view].vdp1 = saturn_bench_vdp1_ticks;   /* timed at the commit */
        bench_measured = 0;
        bench_frame = 0;
#if BENCH_HOLD
#if defined(BENCH_HOLD_TURN) || defined(BENCH_HOLD_WALK)
        bench_place_camera(0);  /* turning / walking hold: next step */
#endif
        return 0;               /* stay on the held view */
#endif
        bench_view++;
        if (bench_view >= bench_nviews)
        {
            bench_summarize();
            if (bench_map >= SATURN_BENCH_MAPS)
            {
                saturn_bench_done = 1;
                return 0;
            }
            bench_map++;
            return 0;   /* next iteration warps */
        }
        bench_place_camera(bench_view);
    }
    else if (bench_nviews <= 0)
    {
        bench_summarize();
        if (bench_map >= SATURN_BENCH_MAPS)
            saturn_bench_done = 1;
        else
            bench_map++;
    }
    return 0;
}

/* Saturn_G_Display, right after Saturn_HW_RenderPlayerView. */
void Saturn_Bench_AfterRender(unsigned int render_ticks)
{
    extern unsigned int saturn_hw_prof_slave_wait;
    extern unsigned int saturn_hw_wall_quads, saturn_hw_flat_quads;
    extern int          saturn_visit_count;

    if (saturn_bench_done || !bench_built || bench_view >= bench_nviews)
        return;
    bench_frame++;
    if (bench_frame <= BENCH_SETTLE)
        return;
    {
        bench_rec_t *r = &bench_rec[bench_view];
        r->render = render_ticks;
        r->wait   = saturn_hw_prof_slave_wait;
        r->wq     = (uint16_t)saturn_hw_wall_quads;
        r->fq     = (uint16_t)saturn_hw_flat_quads;
        r->subs   = (uint16_t)saturn_visit_count;
        bench_measured = 1;
        {
            uint32_t *id = saturn_bench_idle[bench_map - 1];
            uint32_t busy = saturn_hw_prof_slave_busy;
            id[0] += render_ticks;
            id[1] += busy;
            id[2]++;
            id[7] += saturn_hw_prof_slave_wait;
            if (render_ticks > (uint32_t)saturn_frt_rate)
            {
                id[3]++;
                id[4] += busy;
                id[5] += render_ticks;
                if (busy * 3u < render_ticks * 2u)
                    id[6]++;
            }
        }
#if BENCH_HOLD && defined(BENCH_HOLD_FPS)
        /* hold-mode FPS runs: no VDP1 timing spin, so frames pace as in play */
        if (0)
#endif
        {
            /* saturn_vdp1.c times this frame's VDP1 draw at the commit */
            extern volatile int          saturn_bench_vdp1_want;
            extern volatile unsigned int saturn_bench_vdp1_ticks;
            saturn_bench_vdp1_ticks = 0;
            saturn_bench_vdp1_want  = 1;
        }
    }
}

static void bench_put_tenths(char *b, int *n, unsigned int t)
{
    char tmp[8];
    int k = 0;
    unsigned int whole = t / 10u;
    do { tmp[k++] = (char)('0' + whole % 10u); whole /= 10u; } while (whole && k < 6);
    while (k) b[(*n)++] = tmp[--k];
    b[(*n)++] = '.';
    b[(*n)++] = (char)('0' + t % 10u);
}

static void bench_put_uint(char *b, int *n, unsigned int v)
{
    char tmp[8];
    int k = 0;
    do { tmp[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 7);
    while (k) b[(*n)++] = tmp[--k];
}

/* Saturn_G_Display when done: the results table, full screen (2D mode). */
void Saturn_Bench_DrawSummary(void)
{
    extern void Saturn_Vdp1_Enter2D(void);
    extern int  saturn_view_latch;
    int m;
    Saturn_Vdp1_Enter2D();
    saturn_view_latch = 2;      /* copy the view rows (text) to NBG0 every frame */
    Saturn_ClearRect(0, 0, 320, 168, 0);
    Saturn_Print(4, 2, "BENCH  MS: P50 P90 MAX");
    Saturn_Print(4, 12, "MAP  RENDER          VDP1");
    for (m = 0; m < SATURN_BENCH_MAPS; m++)
    {
        const bench_sum_t *s = &saturn_bench_sum[m];
        char b[64];
        int n = 0;
        b[n++] = 'E'; b[n++] = '1'; b[n++] = 'M'; b[n++] = (char)('0' + s->map % 10u);
        b[n++] = ' ';
        bench_put_tenths(b, &n, s->r_p50); b[n++] = ' ';
        bench_put_tenths(b, &n, s->r_p90); b[n++] = ' ';
        bench_put_tenths(b, &n, s->r_max); b[n++] = ' '; b[n++] = ' ';
        bench_put_tenths(b, &n, s->v_p50); b[n++] = ' ';
        bench_put_tenths(b, &n, s->v_p90); b[n++] = ' ';
        bench_put_tenths(b, &n, s->v_max); b[n++] = ' '; b[n++] = 'N';
        bench_put_uint(b, &n, s->views);
        b[n] = 0;
        Saturn_Print(4, 24 + m * 15, b);
    }
}

#endif /* SATURN_BENCH */
