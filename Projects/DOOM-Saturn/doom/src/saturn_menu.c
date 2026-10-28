/* saturn_menu.c — title screen + attract-demo cycling + episode/skill
 * selection, and the in-game pause plaque.
 *
 * A blocking boot menu inside D_DoomMain (the d32xr MiniLoop UI needs
 * Jaguar-only lumps + DMAPINFO/save/net infrastructure this port never
 * had — see the saturn_vgl.c header). Draws the rerelease WAD's PC
 * menu art (M_EPISOD/M_EPI1-4/M_SKILL/M_JKILL.../M_SKULL1/2/M_PAUSE)
 * through the saturn_vgl blitter. Item layout follows linuxdoom
 * m_menu.c: banner at (54,38), items at x=54, y=63+16*i, skull at
 * (x-8, y+5) animating every 8 frames.
 *
 * Attract cycle: ~10 s idle on the title rolls the next attract demo
 * (saturn_demo.c; the demo itself runs through D_DoomLoop from
 * d_main.c — this menu only picks the number). START skips straight
 * to the episode/skill pick; any button during a demo quits it and
 * lands back on this title.
 *
 * Runs after I_InitVideo + PLAYPAL are up, so I_FinishUpdate works and
 * art shows in the authentic palette. slSynch inside I_FinishUpdate
 * paces the loop to 60 Hz; the background cart-fill pump runs so the
 * CD stream keeps moving during the wait.
 */
#include "saturn.h"
#include "i_video.h"
#include "saturn_vgl.h"

/* saturn_demo.c — attract-demo availability probes */
extern int Saturn_Demo_Available(void);
extern int Saturn_Demo_Exists(int num);

/* i_system.c boot-flight recorder: cart record survives the SYSTEM-
 * RESET wedge on real HW; milestones 4..9 = letters A..F. */
extern void Saturn_BootRec(int n);

/* --- i_wad.c async fill (keep draining while the menu idles) --- */
extern int  saturn_iwad_background_fill(void);
extern void saturn_iwad_fill_pump(void);

/* menu item layout (linuxdoom m_menu.c coordinates) */
#define MENU_ITEM_X      54
#define MENU_ITEM_Y0     63
#define MENU_ITEM_PITCH  16
#define SKULL_FRAMES     8

static int menu_frame = 0;

/* patch width straight from the header (byte-wise LE) */
static int patch_width(const void *patch)
{
    const byte *p = (const byte *)patch;

    return p[0] | (p[1] << 8);
}

static void draw_centered(int y, const void *patch)
{
    if (patch)
        Saturn_DrawPatchFS((DOOM_SCREENWIDTH - patch_width(patch)) / 2, y,
                           patch);
}

/* ---- one menu frame: pump the CD fill, read the pad, commit ---- */
static int menu_pump(int *prev_buttons)
{
    int buttons, edges;

    menu_frame++;
    saturn_iwad_fill_pump();

    buttons = I_ReadControls();
    edges = (buttons ^ *prev_buttons) & buttons;
    *prev_buttons = buttons;

    return edges;
}

/* ---- draw banner + items + animated skull ----
 * Applies up/down edges to *cursor; returns +1 on select edge,
 * -1 on back edge, 0 otherwise. */
static int menu_list(const char *banner, const char *const *items,
                     int nitems, int *cursor, int edges, int allow_back)
{
    const void *p;
    int         i;
    int         act = 0;

    p = Saturn_CachePatch(banner);
    draw_centered(38, p);

    for (i = 0; i < nitems; i++)
    {
        p = Saturn_CachePatch(items[i]);
        if (p)
            Saturn_DrawPatchFS(MENU_ITEM_X,
                               MENU_ITEM_Y0 + MENU_ITEM_PITCH * i, p);
    }

    /* skull cursor: M_SKULL1/M_SKULL2 toggled every SKULL_FRAMES */
    {
        const char *sk = ((menu_frame / SKULL_FRAMES) & 1) ? "M_SKULL2"
                                                           : "M_SKULL1";
        int         y  = MENU_ITEM_Y0 + MENU_ITEM_PITCH * *cursor;

        p = Saturn_CachePatch(sk);
        if (p)
            Saturn_DrawPatchFS(MENU_ITEM_X - 8, y + 5, p);
    }

    if (edges & BT_MOVEUP)
    {
        if (--*cursor < 0)
            *cursor = nitems - 1;
    }
    if (edges & BT_MOVEDOWN)
    {
        if (++*cursor >= nitems)
            *cursor = 0;
    }
    if (edges & (BT_ATTACK | BT_START | BT_SPEED))
        act = 1;
    else if (allow_back && (edges & BT_USE))
        act = -1;
    return act;
}

/* wait until the pad is fully released (a held press must not bleed
 * into the next screen) */
static void menu_wait_release(void)
{
    int prev = 0;
    int timeout = 30;

    while (I_ReadControls() != 0 && --timeout > 0)
        menu_pump(&prev);
}

/* Stage letter helper: StageTextHi is gated by saturn_quiet_hud (set
 * by Saturn_Vdp1Wall_Init at the smoke test's first commit), so lift
 * the flag around the stamp and restore it — the in-game census rows
 * must stay silenced. Zero new BSS. */
static void menu_stage_letter(int x, const char *s)
{
    extern void Saturn_StageTextHi(int x, int y, const char *s);
    extern int   saturn_quiet_hud;
    int          q = saturn_quiet_hud;

    saturn_quiet_hud = 0;
    Saturn_StageTextHi(x, 24, s);
    saturn_quiet_hud = q;
}

/* ---- title screen: TITLEPIC + blinking PRESS START ----
 * Returns 1 when the user pressed start, 0 when the attract-demo
 * countdown ran out (demos present in the WAD only; otherwise the
 * title waits forever, as before). */
static int menu_title(void)
{
    /* TITLEPIC is 57 KB: above the ~50 KB sustained A-bus burst
     * threshold, so the normal cache path wedges the console (the
     * 2026-09-05 verdict-park A/B pair). CachePatchBig streams it
     * through the shared scratch and slices the copy into cart. */
    /* STAGE LETTERS (2026-09-09 real-HW wedge, freeze-forensics
     * protocol): BootMsg text is VDP1-drawn and INVISIBLE post-takeover
     * (i_system.c Saturn_BootText), so a wedge anywhere after the cyan
     * STARTING paint shows a bare colour. These StageTextHi letters go
     * straight into BOTH NBG0 pages (visible on a frozen screen) and
     * name the last step reached:
     *   A = menu_title entered            (missing = STARTING/SLV-M
     *                                        BootMsg slSynch wedged)
     *   B = CachePatchBig("TITLEPIC") returned
     *   C = Saturn_Demo_Available returned
     *   D = menu_pump returned (fill pump + I_ReadControls)
     *   E = TITLEPIC + PRESS START drawn into the fb
     *   F = first I_FinishUpdate returned (title committed + flipped)
     * Zero new BSS: existing machinery, literal letters. The first
     * commit's copy_back erases them (title shows); a wedge freezes
     * whatever letters stand. */
    const void   *title;
    int           prev = 0;
    int           cooldown = 10;        /* vbls: debounce boot pad state */
    int           timeout = 0;          /* vbls: attract-demo countdown */
    unsigned int  lv = 0;
    {
        extern volatile unsigned int saturn_vbl_count;

        lv = saturn_vbl_count;
    }

    Saturn_BootRec(4);
    title = Saturn_CachePatchBig("TITLEPIC");
    Saturn_BootRec(5);

    if (Saturn_Demo_Available())
        timeout = 60 * 10;              /* ~10 s of title before auto-start */
    Saturn_BootRec(6);

    for (;;)
    {
        int          edges;
        unsigned int nv;
        int          dv;
        {
            extern volatile unsigned int saturn_vbl_count;

            nv = saturn_vbl_count;
        }
        dv        = (int)(nv - lv);
        cooldown -= dv;
        timeout  -= dv;
        lv        = nv;
        edges = menu_pump(&prev);

        Saturn_BootRec(7);
        if (title)
            Saturn_DrawPatchFS(0, 0, title);

        /* blink every 32 menu frames after the cooldown */
        if (cooldown <= 0 && ((menu_frame >> 5) & 1) == 0)
        {
            /* Centre bottom: drawn straight over the title artwork */
            const char *msg = "PRESS START";

            Saturn_Print((DOOM_SCREENWIDTH - Saturn_PrintWidth(msg)) / 2,
                         DOOM_SCREENHEIGHT - 16, msg);
        }

        Saturn_BootRec(8);
        I_FinishUpdate();
        Saturn_BootRec(9);

        if (cooldown <= 0 && (edges & (BT_START | BT_ATTACK | BT_USE | BT_SPEED)))
        {
            menu_wait_release();
            return 1;
        }
        /* Timeout fallback: auto-advance to game if left idle */
        if (cooldown <= 0 && timeout <= 0 && timeout + dv > 0)
        {
            menu_wait_release();
            return 1;
        }
    }
}

/* ---- episode + skill pick; writes out_episode/out_skill ---- */
static void menu_episode_skill(int *out_episode, int *out_skill)
{
    static const char *const epi_names[4] = {
        "M_EPI1", "M_EPI2", "M_EPI3", "M_EPI4"
    };
    static const char *const epi_maps[4] = { "E1M1", "E2M1", "E3M1", "E4M1" };
    static const char *const skill_names[5] = {
        "M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE"
    };
    const char *epi_show[4];
    int         n_epi = 0;
    int         state = 0;              /* 0 = episode, 1 = skill */
    int         epi_cursor = 0, skill_cursor = 0;
    int         episode = 1;
    int         prev = 0;
    int         i;

    for (i = 0; i < 4; i++)
        if (W_CheckNumForName(epi_maps[i]) >= 0)
            epi_show[n_epi++] = epi_names[i];
    if (n_epi == 0)                     /* fallback: episode 1 */
        epi_show[n_epi++] = epi_names[0];

    for (;;)
    {
        int edges = menu_pump(&prev);
        int act;

        Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);

        if (state == 0)
            act = menu_list("M_EPISOD", epi_show, n_epi, &epi_cursor,
                            edges, 0);
        else
            act = menu_list("M_SKILL", skill_names, 5, &skill_cursor,
                            edges, 1);

        if (act > 0)
        {
            if (state == 0)
            {
                episode = epi_cursor + 1;
                state = 1;
                menu_wait_release();
            }
            else
            {
                *out_episode = episode;
                *out_skill   = skill_cursor;
                return;
            }
        }
        else if (act < 0 && state == 1)
        {
            state = 0;
            menu_wait_release();
        }

        I_FinishUpdate();
    }
}

/* ---- public entry: title -> attract demo / episode -> skill ----
 * Returns 0 to start a normal game (episode/skill written), 1 to play
 * one attract demo (1..4 written to *out_demo — playback happens in
 * d_main.c through D_DoomLoop; when it ends we land back here, on the
 * title). The demo number cycles 1..4 across calls so the attract
 * loop rotates through the WAD's demos. */
int Saturn_Menu_Run(int *out_episode, int *out_skill, int *out_demo)
{
    static int attract_next = 0;

    if (menu_title() > 0)
    {
        menu_episode_skill(out_episode, out_skill);
        return 0;
    }

    /* advance to the next demo the WAD actually ships (WADs with
     * fewer than four DEMO lumps just have a shorter cycle) */
    {
        int n;

        for (n = 0; n < 4; n++)
        {
            attract_next = attract_next % 4 + 1;
            if (Saturn_Demo_Exists(attract_next))
                break;
        }
    }

    *out_demo = attract_next;
    return 1;
}

/* ---- in-game pause plaque (drawn from Saturn_G_Display, on top of
 * the frozen view + status bar) ----
 * A = resume (the pause toggle in Saturn_G_Ticker), B = quit to title
 * (saturn_quit_to_title, checked in Saturn_G_Ticker too). */
void Saturn_Pause_Draw(void)
{
    static const char *const lines[4] = {
        "PRESS A", "TO RESUME", "PRESS B", "TO QUIT"
    };
    const void *p = Saturn_CachePatch("M_PAUSE");
    int         i;
    extern int  saturn_wall_subdivision;
    const char *strip_str = saturn_wall_subdivision ? "PRESS C: WALL STRIPS [ON]" : "PRESS C: WALL STRIPS [OFF]";
    int         w_strip;

    draw_centered(32, p);
    for (i = 0; i < 4; i++)
    {
        int w = Saturn_PrintWidth(lines[i]);

        Saturn_Print((DOOM_SCREENWIDTH - w) / 2, 108 + i * 14, lines[i]);
    }

    w_strip = Saturn_PrintWidth(strip_str);
    Saturn_Print((DOOM_SCREENWIDTH - w_strip) / 2, 168, strip_str);
}