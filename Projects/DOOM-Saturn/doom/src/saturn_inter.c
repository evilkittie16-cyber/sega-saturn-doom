/* saturn_inter.c — Saturn-native intermission, art edition.
 *
 * The 2026-09-06 minimal edition (zero WAD reads) was the workaround
 * for the exit-wedge: the art-based WI screen read cold CD lumps
 * mid-game and wedged between the exit switch and the first frame.
 * That wedge was the hybrid-loader CD bug family (hot cart offsets
 * used as disc offsets + silent backward preads after a handle
 * re-open), fixed the same day in i_wad.c — see the title-screen
 * ledger. This edition restores the authentic screen:
 *   - background   = WIMAP0/1/2 per episode (the episode world map)
 *   - splats       = WISPLAT on every finished level's map dot
 *   - you-are-here = WIURH0 on the next level's dot
 *   - title        = WILV<e><m> level name + WIF "FINISHED"
 *   - stats        = WIOSTK/WIOSTI/WIOSTS + WIPCNT + WINUM0-9 at the
 *                    vanilla wi_stuff.c coordinates
 *   - time/par     = WITIME/WIPAR + WICOLON at the vanilla positions
 * All WI patches are cached ONCE at screen entry (one cold-read
 * burst through the fixed pread); the per-frame loop then does zero
 * WAD reads. The background (68 KB) goes through the sliced big-
 * patch cache; the previous episode's WIMAP is Z_Free'd on switch.
 *
 * Flow: D_DoomMain's level chain calls Saturn_Inter_Run after
 * D_DoomLoop returns on the latched exit. One screen: stats + map +
 * a blinking PRESS START. START (or A/B) returns the next map number
 * (0 = episode over — back to the title). No auto-advance.
 */
#include "doomdef.h"
#include "i_video.h"
#include "saturn.h"
#include "saturn_vgl.h"

/* d32xr_stubs.c — intermission totals (live-updated by p_mobj.c /
 * p_spec.c, zeroed by Saturn_G_InitNew) */
extern VINT totalkills, totalitems, totalsecret;

/* i_wad.c — keep the background cart fill draining while we idle */
extern void saturn_iwad_fill_pump(void);

/* g_game.c — level timer (tics at TICRATE; zeroed by Saturn_G_InitNew,
 * counted in Saturn_G_Ticker) */
extern int saturn_leveltime;

/* saturn_vgl.c — sliced big-patch cache (WIMAP* are 68 KB) */
extern void *Saturn_CachePatchBig(const char *name);

/* ---- shift-subtract unsigned divide (the SH-2 divide-stub ledger) */
static unsigned udiv32(unsigned a, unsigned b)
{
    unsigned q = 0, r = 0;
    int     i;

    if (b == 0)
        return 0;
    for (i = 31; i >= 0; i--)
    {
        r = (r << 1) | ((a >> i) & 1u);
        if (r >= b)
        {
            r -= b;
            q |= 1u << i;
        }
    }
    return q;
}

/* ---- vanilla wi_stuff.c layout ---- */
#define WI_TITLEY   2
#define SP_STATSX   50
#define SP_STATSY   50
#define SP_TIMEX    16
#define SP_TIMEY    (DOOM_SCREENHEIGHT - 32)    /* 168 */
#define WI_NUMW     11      /* WINUM digit advance (WINUM0 width) */
#define WI_COLONW   5       /* WICOLON width */

/* row pitch: vanilla lh = (3 * num[0]->height) / 2 = 18 */
#define WI_LH       18

/* map-dot coordinates (linuxdoom wi_stuff.c lnodes; episode 4 has no
 * row — vanilla's table is zero-filled there; we reuse episode 3's
 * row, matching its WIMAP2 background) */
static const short lnodes[3][9][2] = {
    { {185, 164}, {148, 143}, { 69, 122}, {209, 102},
      {116,  89}, {166,  55}, { 71,  56}, {135,  29},
      { 71,  24} },
    { {254,  25}, { 97,  50}, {188,  64}, {128,  78},
      {214,  92}, {133, 130}, {208, 136}, {148, 140},
      {235, 158} },
    { {156, 168}, { 48, 154}, {174,  95}, {265,  75},
      {130,  48}, {279,  23}, {198,  48}, {140,  25},
      {281, 136} }
};

/* V_DrawPatch semantics: the patch's (leftoffset, topoffset) hotspot
 * lands on (x, y). Saturn_DrawPatch ignores offsets, so subtract them
 * here. All WI patches have 0 offsets except WISPLAT/WIURH0, which are
 * DESIGNED to be hotspot-centered on the map dots. */
static void draw_wi(int x, int y, const void *patch)
{
    const byte *p = (const byte *)patch;
    int         lo = p[4] | (p[5] << 8);
    int         to = p[6] | (p[7] << 8);

    if (lo >= 0x8000)
        lo -= 0x10000;
    if (to >= 0x8000)
        to -= 0x10000;
    Saturn_DrawPatch(x - lo, y - to, patch, 0, DOOM_SCREENHEIGHT);
}

/* right-aligned WINUM digits ending at x (vanilla WI_drawNum; no
 * minus sign needed — all our values are non-negative; w < 0 = as
 * many digits as the value needs, w >= 0 = zero-padded to w).
 * Returns the new x (the digits' LEFT edge), as vanilla does. */
static int wi_num(int x, int y, int n, int w)
{
    const void *digit;
    int         digits = (w >= 0) ? w : 0;
    int         t;
    char        nm[9];

    if (w < 0)
    {
        t = n;
        do
        {
            t = (int)udiv32((unsigned)t, 10u);
            digits++;
        } while (t);
        if (digits == 0)
            digits = 1;
    }
    while (digits-- > 0)
    {
        int d = (int)udiv32((unsigned)n, 10u);

        nm[0] = 'W'; nm[1] = 'I'; nm[2] = 'N'; nm[3] = 'U';
        nm[4] = 'M'; nm[5] = (char)('0' + (n - d * 10)); nm[6] = 0;
        n = d;
        digit = Saturn_CachePatch(nm);
        if (digit)
        {
            x -= WI_NUMW;
            Saturn_DrawPatch(x, y, digit, 0, DOOM_SCREENHEIGHT);
        }
        else
            x -= WI_NUMW;
    }
    return x;
}

/* "n%" — % patch's left edge at x, number right edge at x (vanilla
 * WI_drawPercent) */
static void wi_percent(int x, int y, int p)
{
    const void *pct = Saturn_CachePatch("WIPCNT");

    if (p < 0)
        return;
    if (pct)
        draw_wi(x, y, pct);
    wi_num(x, y, p, -1);
}

/* "MM:SS" (or H:MM:SS) right-aligned ending at x (vanilla
 * WI_drawTime; minutes zero-padded like vanilla's w=2 groups).
 * ALL divisions go through udiv32 — a raw / on a variable divisor
 * self-recurses the libgcc stub (the divide-stub ledger; the first
 * cut froze the console at the screen's first frame). */
static void wi_time(int x, int y, int t)
{
    const void *colon;
    int         div = 1;

    if (t < 0)
        return;
    for (;;)
    {
        int q   = (int)udiv32((unsigned)t, (unsigned)div);
        int q60 = (int)udiv32((unsigned)q, 60u);
        int n   = q - q60 * 60;

        x = wi_num(x, y, n, 2) - WI_COLONW;
        colon = Saturn_CachePatch("WICOLON");
        if (div == 60 || q)
        {
            if (colon)
                draw_wi(x, y, colon);
        }
        div *= 60;
        if (!udiv32((unsigned)t, (unsigned)div))
            break;
    }
}

/* patch width straight from the header (byte-wise LE) */
static int wi_width(const void *patch)
{
    const byte *p = (const byte *)patch;

    return p[0] | (p[1] << 8);
}

/* centered HU-font line */
static void print_centered(int y, const char *s)
{
    Saturn_Print((DOOM_SCREENWIDTH - Saturn_PrintWidth(s)) / 2, y, s);
}

/* ---- next-map progression (vanilla Doom 1 rules) ----
 * secret exit -> E*M9 (from E*M9 the normal exit rejoins at E*M4);
 * normal exit -> map+1; E*M8 ends the episode; E*M9's normal exit
 * rejoins at E*M4.
 * Returns 0 = episode finished. */
static int next_map(int map, int secretexit)
{
    if (secretexit)
        return map == 9 ? 4 : 9;
    if (map == 8)
        return 0;
    if (map == 9)
        return 4;
    return map + 1;
}

/* ---- one screen frame: pump the CD fill, read the pad, commit ---- */
static int inter_pump(int *prev_buttons)
{
    int buttons, edges;

    saturn_iwad_fill_pump();

    buttons = I_ReadControls();
    edges   = (buttons ^ *prev_buttons) & buttons;
    *prev_buttons = buttons;

    return edges;
}

/* wait until the pad is fully released (the exit switch's held USE
 * press must not bleed into the next level as a phantom use) */
static void inter_wait_release(void)
{
    int prev = ~0;
    int guard = 0;

    while (I_ReadControls() != 0 && ++guard < 60)
    {
        inter_pump(&prev);
        I_WaitVBL(1);
    }
}

/* ---- WI art cache (filled once per screen; PU_STATIC so the NEXT
 * intermission reuses it — after the first visit the only read is a
 * switched episode's WIMAP) ---- */
static void *wi_bg;         /* WIMAP0/1/2, sliced big-patch cache */
static int   wi_bg_epi = -1;
static void *wi_lname;      /* this level's WILV<e><m> */
static void *wi_next_lname; /* next level's WILV<e><next-1> */
static void *wi_finished;   /* WIF */
static void *wi_entering;   /* WIENTER */
static void *wi_kills, *wi_items, *wi_secret;   /* WIOSTK/I/S */
static void *wi_splat, *wi_yah;                 /* WISPLAT, WIURH0 */

/* ---- the screen ----
 * episode/map identify the level that just ended; secretexit says the
 * player took the secret exit line.
 * Returns the next map number (1..9), or 0 when the episode ended. */
int Saturn_Inter_Run(int episode, int map, int secretexit)
{
    /* vanilla pars[4][10], 1-based [episode][map] (E1..E3; the original
     * release had no episode 4, hence no PAR row there) */
    static const int pars[3][10] = {
        {0, 30, 75, 120, 90, 165, 180, 180, 30, 165},
        {0, 90, 90, 90, 120, 90, 360, 240, 30, 170},
        {0, 90, 45, 90, 150, 90, 90, 165, 30, 135}
    };
    extern void Saturn_Vdp1_Enter2D(void);
    player_t *pl = &players[consoleplayer];
    int       next = next_map(map, secretexit);
    int       pct_k, pct_i, pct_s, secs, par;
    int       cnt_k = 0, cnt_i = 0, cnt_s = 0, cnt_t = 0, cnt_par = 0;
    int       stats_done = 0;
    int       stage = 0; /* 0 = stats, 1 = entering next map / victory */
    int       stage_frame = 0;
    int       epi = (episode > 3) ? 2 : (episode < 1 ? 0 : episode - 1);
    int       frame, prev = ~0;
    char      nm[9];

    /* Switch VDP1/VDP2 into full-screen 2D mode so 3D quads do not cover NBG0 */
    Saturn_Vdp1_Enter2D();

    pct_k = totalkills > 0
          ? (int)udiv32((unsigned)(pl->killcount * 100),
                        (unsigned)totalkills) : 100;
    pct_i = totalitems > 0
          ? (int)udiv32((unsigned)(pl->itemcount * 100),
                        (unsigned)totalitems) : 100;
    pct_s = totalsecret > 0
          ? (int)udiv32((unsigned)(pl->secretcount * 100),
                        (unsigned)totalsecret) : 100;
    secs  = (int)udiv32((unsigned)saturn_leveltime, (unsigned)TICRATE);
    par   = (episode >= 1 && episode <= 3 && map >= 1 && map <= 9)
          ? pars[episode - 1][map] : 0;

    /* ---- one-shot WI art load ---- */
    {
        static const char *const bgname[3] = {
            "WIMAP0", "WIMAP1", "WIMAP2"
        };

        if (wi_bg_epi != epi)
        {
            if (wi_bg)
                Z_Free(wi_bg);
            wi_bg     = Saturn_CachePatchBig(bgname[epi]);
            if (!wi_bg)
                wi_bg = Saturn_CachePatchBig("WIMAP0");
            wi_bg_epi = epi;
        }
        nm[0] = 'W'; nm[1] = 'I'; nm[2] = 'L'; nm[3] = 'V';
        nm[4] = (char)('0' + (episode - 1) % 10);
        nm[5] = (char)('0' + (map - 1) % 10);
        nm[6] = 0;
        wi_lname = Saturn_CachePatch(nm);

        if (next > 0)
        {
            nm[0] = 'W'; nm[1] = 'I'; nm[2] = 'L'; nm[3] = 'V';
            nm[4] = (char)('0' + (episode - 1) % 10);
            nm[5] = (char)('0' + (next - 1) % 10);
            nm[6] = 0;
            wi_next_lname = Saturn_CachePatch(nm);
        }
        else
        {
            wi_next_lname = 0;
        }

        wi_finished = Saturn_CachePatch("WIF");
        wi_entering = Saturn_CachePatch("WIENTER");
        wi_kills    = Saturn_CachePatch("WIOSTK");
        wi_items    = Saturn_CachePatch("WIOSTI");
        wi_secret   = Saturn_CachePatch("WIOSTS");
        wi_splat    = Saturn_CachePatch("WISPLAT");
        wi_yah      = Saturn_CachePatch("WIURH0");
    }
    inter_wait_release();

    for (frame = 0;; frame++, stage_frame++)
    {
        int edges = inter_pump(&prev);
        int press = (frame > 8) && (edges & (BT_START | BT_ATTACK | BT_USE));

        Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);

        /* background + map dots */
        if (wi_bg)
            Saturn_DrawPatchFS(0, 0, wi_bg);
        if (wi_splat)
        {
            int i;
            int max_splat = (map <= 9) ? map : 9;
            for (i = 0; i < max_splat; i++)
                draw_wi(lnodes[epi][i][0], lnodes[epi][i][1], wi_splat);
        }

        if (stage == 0)
        {
            /* Animated stat count-up */
            if (!stats_done)
            {
                if (press)
                {
                    cnt_k = pct_k;
                    cnt_i = pct_i;
                    cnt_s = pct_s;
                    cnt_t = secs;
                    cnt_par = par;
                    stats_done = 1;
                    press = 0;
                }
                else
                {
                    if (cnt_k < pct_k) { cnt_k += 2; if (cnt_k > pct_k) cnt_k = pct_k; }
                    else if (cnt_i < pct_i) { cnt_i += 2; if (cnt_i > pct_i) cnt_i = pct_i; }
                    else if (cnt_s < pct_s) { cnt_s += 2; if (cnt_s > pct_s) cnt_s = pct_s; }
                    else if (cnt_t < secs)  { cnt_t += 3; if (cnt_t > secs)  cnt_t = secs; }
                    else if (cnt_par < par) { cnt_par += 3; if (cnt_par > par) cnt_par = par; }
                    else stats_done = 1;
                }
            }

            if (next > 0 && wi_yah && ((frame >> 4) & 1))
                draw_wi(lnodes[epi][next - 1][0], lnodes[epi][next - 1][1], wi_yah);

            /* title: level name + FINISHED (vanilla WI_drawLF) */
            if (wi_lname)
                draw_wi((DOOM_SCREENWIDTH - wi_width(wi_lname)) / 2,
                        WI_TITLEY, wi_lname);
            if (wi_finished)
                draw_wi((DOOM_SCREENWIDTH - wi_width(wi_finished)) / 2,
                        WI_TITLEY + WI_LH - 6, wi_finished);

            /* stats (vanilla WI_drawStats: labels at x=50, % right edge
             * at 320-50, rows at y=50/68/86) */
            if (wi_kills)
                draw_wi(SP_STATSX, SP_STATSY, wi_kills);
            wi_percent(DOOM_SCREENWIDTH - SP_STATSX, SP_STATSY, cnt_k);
            if (wi_items)
                draw_wi(SP_STATSX, SP_STATSY + WI_LH, wi_items);
            wi_percent(DOOM_SCREENWIDTH - SP_STATSX, SP_STATSY + WI_LH, cnt_i);
            if (wi_secret)
                draw_wi(SP_STATSX, SP_STATSY + 2 * WI_LH, wi_secret);
            wi_percent(DOOM_SCREENWIDTH - SP_STATSX, SP_STATSY + 2 * WI_LH, cnt_s);

            /* time + par (vanilla: TIME at (16,168) with the value right
             * edge at 144; PAR at (176,168) with the value right edge at
             * 304) */
            {
                const void *pt = Saturn_CachePatch("WITIME");

                if (pt)
                    draw_wi(SP_TIMEX, SP_TIMEY, pt);
                wi_time(DOOM_SCREENWIDTH / 2 - SP_TIMEX, SP_TIMEY, cnt_t);
                if (par > 0)
                {
                    const void *pp = Saturn_CachePatch("WIPAR");

                    if (pp)
                        draw_wi(DOOM_SCREENWIDTH / 2 + SP_TIMEX, SP_TIMEY, pp);
                    wi_time(DOOM_SCREENWIDTH - SP_TIMEX, SP_TIMEY, cnt_par);
                }
            }

            if ((frame >> 4) & 1)
                print_centered(184, "PRESS START");

            I_FinishUpdate();

            if (stats_done && press)
            {
                stage = 1;
                stage_frame = 0;
            }
        }
        else
        {
            /* Stage 1: Entering next level (or Episode Victory if next == 0) */
            if (next > 0)
            {
                if (wi_entering)
                    draw_wi((DOOM_SCREENWIDTH - wi_width(wi_entering)) / 2,
                            WI_TITLEY, wi_entering);
                if (wi_next_lname)
                    draw_wi((DOOM_SCREENWIDTH - wi_width(wi_next_lname)) / 2,
                            WI_TITLEY + WI_LH - 2, wi_next_lname);
                if (wi_yah && ((stage_frame >> 4) & 1))
                    draw_wi(lnodes[epi][next - 1][0], lnodes[epi][next - 1][1], wi_yah);

                if (press || stage_frame >= 150)
                {
                    print_centered(184, "LOADING...");
                    I_FinishUpdate();
                    inter_wait_release();
                    break;
                }
                if ((stage_frame >> 4) & 1)
                    print_centered(184, "PRESS START");
            }
            else
            {
                print_centered(60, "EPISODE COMPLETED!");
                print_centered(84, "CONGRATULATIONS!");
                if ((stage_frame >> 4) & 1)
                    print_centered(184, "PRESS START");
                if (press)
                {
                    inter_wait_release();
                    break;
                }
            }

            I_FinishUpdate();
        }
    }

    return next;
}