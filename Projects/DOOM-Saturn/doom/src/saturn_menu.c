/* saturn_menu.c — Full DOOM Title, Main Menu, Episode/Skill Selection,
 * Options & Level Select, Read This! / Credits, and Interactive Pause Menu.
 */
#include "saturn.h"
#include "i_video.h"
#include "saturn_vgl.h"

/* saturn_demo.c — attract-demo availability probes */
extern int Saturn_Demo_Available(void);
extern int Saturn_Demo_Exists(int num);

/* i_system.c boot-flight recorder */
extern void Saturn_BootRec(int n);

/* i_wad.c async fill */
extern int  saturn_iwad_background_fill(void);
extern void saturn_iwad_fill_pump(void);

/* saturn_vdp1.c — switch to 2D full-screen NBG0 mode */
extern void Saturn_Vdp1_Enter2D(void);

/* Renderer & HUD options */
extern int  saturn_wall_subdivision;
extern void Saturn_ToggleWallSubdivision(void);
extern int  saturn_quiet_hud;
extern int  saturn_show_fps;
extern volatile int saturn_warp_episode;
extern volatile int saturn_warp_map;
extern int  saturn_quit_to_title;

#define MENU_MAIN_X      97
#define MENU_MAIN_Y0     64
#define MENU_ITEM_X      54
#define MENU_ITEM_Y0     63
#define MENU_ITEM_PITCH  16
#define SKULL_FRAMES     8

enum {
    MENU_STATE_TITLE   = 0,
    MENU_STATE_MAIN    = 1,
    MENU_STATE_EPISODE = 2,
    MENU_STATE_SKILL   = 3,
    MENU_STATE_OPTIONS = 4,
    MENU_STATE_READ1   = 5,
    MENU_STATE_READ2   = 6,
    MENU_STATE_CREDIT  = 7
};

static int menu_frame = 0;
static int menu_start_map = 1;
static void *s_title_patch = 0;
static void *s_help1_patch = 0;
static void *s_help2_patch = 0;
static void *s_credit_patch = 0;

/* patch width straight from the header (byte-wise LE) */
static int patch_width(const void *patch)
{
    const byte *p = (const byte *)patch;
    return p[0] | (p[1] << 8);
}

static void draw_centered(int y, const void *patch)
{
    if (patch)
        Saturn_DrawPatchFS((DOOM_SCREENWIDTH - patch_width(patch)) / 2, y, patch);
}

static void print_centered(int y, const char *s)
{
    Saturn_Print((DOOM_SCREENWIDTH - Saturn_PrintWidth(s)) / 2, y, s);
}

/* ---- one menu frame: pump the CD fill, read the pad ---- */
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

static void menu_wait_release(void)
{
    int prev = 0;
    int timeout = 20;

    while (I_ReadControls() != 0 && --timeout > 0)
    {
        menu_pump(&prev);
        I_WaitVBL(1);
    }
}

static void draw_skull(int x, int y)
{
    const char *sk = ((menu_frame / SKULL_FRAMES) & 1) ? "M_SKULL2" : "M_SKULL1";
    const void *p = Saturn_CachePatch(sk);
    if (p)
        Saturn_DrawPatchFS(x, y, p);
}

static void draw_title_bg(void)
{
    Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);
    if (!s_title_patch)
        s_title_patch = Saturn_CachePatchBig("TITLEPIC");
    if (s_title_patch)
        Saturn_DrawPatchFS(0, 0, s_title_patch);
}

int Saturn_Menu_Run(int *out_episode, int *out_skill, int *out_demo)
{
    static const char *const main_items[4] = {
        "M_NGAME", "M_OPTION", "M_RDTHIS", "M_QUITG"
    };
    static const char *const epi_names[4] = {
        "M_EPI1", "M_EPI2", "M_EPI3", "M_EPI4"
    };
    static const char *const epi_maps[4] = {
        "E1M1", "E2M1", "E3M1", "E4M1"
    };
    static const char *const skill_names[5] = {
        "M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE"
    };
    const char *epi_show[4];
    int         n_epi = 0;
    int         state = MENU_STATE_TITLE;
    int         main_cursor = 0;
    int         epi_cursor = 0;
    int         skill_cursor = 2; /* Default: Hurt Me Plenty */
    int         opt_cursor = 0;
    int         episode = 1;
    int         prev = ~0;
    int         cooldown = 8;
    int         i;

    (void)out_demo;
    Saturn_Vdp1_Enter2D();

    for (i = 0; i < 4; i++)
    {
        if (W_CheckNumForName(epi_maps[i]) >= 0)
            epi_show[n_epi++] = epi_names[i];
    }
    if (n_epi == 0)
    {
        /* Show all 3 classic episodes in the Episode Menu even on Shareware */
        epi_show[0] = "M_EPI1";
        epi_show[1] = "M_EPI2";
        epi_show[2] = "M_EPI3";
        n_epi = 3;
    }
    else if (n_epi == 1)
    {
        epi_show[1] = "M_EPI2";
        epi_show[2] = "M_EPI3";
        n_epi = 3;
    }

    Saturn_BootRec(4);
    if (!s_title_patch)
        s_title_patch = Saturn_CachePatchBig("TITLEPIC");
    Saturn_BootRec(5);

    menu_wait_release();

    for (;;)
    {
        int edges = menu_pump(&prev);

        if (cooldown > 0)
        {
            cooldown--;
            edges = 0;
        }

        /* Allow external level warp (e.g. Win32 menu bar or agent_cmd.txt) */
        if (saturn_warp_map > 0)
        {
            *out_episode = (saturn_warp_episode > 0) ? saturn_warp_episode : 1;
            *out_skill   = skill_cursor;
            saturn_map   = saturn_warp_map;
            saturn_warp_episode = 0;
            saturn_warp_map     = 0;
            return 0;
        }

        if (state == MENU_STATE_TITLE)
        {
            draw_title_bg();
            if (((menu_frame >> 4) & 1) == 0)
                print_centered(184, "PRESS START");

            if (edges & (BT_START | BT_ATTACK | BT_USE | BT_SPEED))
            {
                state = MENU_STATE_MAIN;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_MAIN)
        {
            const void *logo = Saturn_CachePatch("M_DOOM");

            draw_title_bg();
            if (logo)
                Saturn_DrawPatchFS(94, 2, logo);

            for (i = 0; i < 4; i++)
            {
                const void *p = Saturn_CachePatch(main_items[i]);
                if (p)
                    Saturn_DrawPatchFS(MENU_MAIN_X, MENU_MAIN_Y0 + MENU_ITEM_PITCH * i, p);
            }
            draw_skull(MENU_MAIN_X - 32, MENU_MAIN_Y0 + MENU_ITEM_PITCH * main_cursor - 5);

            if (edges & BT_MOVEUP)
                main_cursor = (main_cursor + 3) & 3;
            if (edges & BT_MOVEDOWN)
                main_cursor = (main_cursor + 1) & 3;

            if (edges & (BT_START | BT_ATTACK | BT_SPEED))
            {
                if (main_cursor == 0)
                    state = MENU_STATE_EPISODE;
                else if (main_cursor == 1)
                    state = MENU_STATE_OPTIONS;
                else if (main_cursor == 2)
                    state = MENU_STATE_READ1;
                else if (main_cursor == 3)
                    state = MENU_STATE_TITLE;
                menu_wait_release();
            }
            else if (edges & BT_USE)
            {
                state = MENU_STATE_TITLE;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_EPISODE)
        {
            const void *newg = Saturn_CachePatch("M_NEWG");
            const void *ep_hdr = Saturn_CachePatch("M_EPISOD");

            draw_title_bg();
            if (newg)
                Saturn_DrawPatchFS(96, 14, newg);
            if (ep_hdr)
                Saturn_DrawPatchFS(54, 38, ep_hdr);

            for (i = 0; i < n_epi; i++)
            {
                const void *p = Saturn_CachePatch(epi_show[i]);
                if (p)
                    Saturn_DrawPatchFS(MENU_ITEM_X, MENU_ITEM_Y0 + MENU_ITEM_PITCH * i, p);
            }
            draw_skull(MENU_ITEM_X - 32, MENU_ITEM_Y0 + MENU_ITEM_PITCH * epi_cursor - 5);

            if (edges & BT_MOVEUP)
            {
                if (--epi_cursor < 0)
                    epi_cursor = n_epi - 1;
            }
            if (edges & BT_MOVEDOWN)
            {
                if (++epi_cursor >= n_epi)
                    epi_cursor = 0;
            }

            if (edges & (BT_START | BT_ATTACK | BT_SPEED))
            {
                int chosen_ep = epi_cursor + 1;
                if (W_CheckNumForName(epi_maps[chosen_ep - 1]) < 0)
                    chosen_ep = 1; /* Shareware WAD fallback to Episode 1 */
                episode = chosen_ep;
                state = MENU_STATE_SKILL;
                menu_wait_release();
            }
            else if (edges & BT_USE)
            {
                state = MENU_STATE_MAIN;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_SKILL)
        {
            const void *newg = Saturn_CachePatch("M_NEWG");
            const void *sk_hdr = Saturn_CachePatch("M_SKILL");

            draw_title_bg();
            if (newg)
                Saturn_DrawPatchFS(96, 14, newg);
            if (sk_hdr)
                Saturn_DrawPatchFS(54, 38, sk_hdr);

            for (i = 0; i < 5; i++)
            {
                const void *p = Saturn_CachePatch(skill_names[i]);
                if (p)
                    Saturn_DrawPatchFS(MENU_ITEM_X, MENU_ITEM_Y0 + MENU_ITEM_PITCH * i, p);
            }
            draw_skull(MENU_ITEM_X - 32, MENU_ITEM_Y0 + MENU_ITEM_PITCH * skill_cursor - 5);

            if (edges & BT_MOVEUP)
            {
                if (--skill_cursor < 0)
                    skill_cursor = 4;
            }
            if (edges & BT_MOVEDOWN)
            {
                if (++skill_cursor >= 5)
                    skill_cursor = 0;
            }

            if (edges & (BT_START | BT_ATTACK | BT_SPEED))
            {
                *out_episode = episode;
                *out_skill   = skill_cursor;
                saturn_map   = menu_start_map;
                print_centered(190, "LOADING...");
                I_FinishUpdate();
                menu_wait_release();
                return 0;
            }
            else if (edges & BT_USE)
            {
                state = MENU_STATE_EPISODE;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_OPTIONS)
        {
            const void *opt_hdr = Saturn_CachePatch("M_OPTTTL");
            char line[32];

            draw_title_bg();
            Saturn_ClearRect(36, 10, 248, 176, 0);
            if (opt_hdr)
                draw_centered(16, opt_hdr);
            else
                print_centered(18, "OPTIONS");

            /* Item 0: Start Map E1M1..E1M9 */
            line[0] = 'S'; line[1] = 'T'; line[2] = 'A'; line[3] = 'R'; line[4] = 'T';
            line[5] = ' '; line[6] = 'M'; line[7] = 'A'; line[8] = 'P'; line[9] = ':';
            line[10] = ' '; line[11] = 'E'; line[12] = '1'; line[13] = 'M';
            line[14] = (char)('0' + menu_start_map);
            line[15] = 0;
            Saturn_Print(68, 56, line);

            /* Item 1: Launch Selected Map Immediately */
            line[0] = 'W'; line[1] = 'A'; line[2] = 'R'; line[3] = 'P'; line[4] = ' ';
            line[5] = 'T'; line[6] = 'O'; line[7] = ' '; line[8] = 'E'; line[9] = '1';
            line[10] = 'M'; line[11] = (char)('0' + menu_start_map);
            line[12] = ' '; line[13] = 'N'; line[14] = 'O'; line[15] = 'W';
            line[16] = 0;
            Saturn_Print(68, 76, line);

            /* Item 2: Wall Strips */
            Saturn_Print(68, 96, saturn_wall_subdivision ? "WALL STRIPS: ON" : "WALL STRIPS: OFF");

            /* Item 3: Perf HUD */
            Saturn_Print(68, 116, (!saturn_quiet_hud || saturn_show_fps) ? "PERF HUD: ON" : "PERF HUD: OFF");

            /* Item 4: Back */
            Saturn_Print(68, 136, "BACK TO MAIN MENU");

            draw_skull(38, 50 + opt_cursor * 20);

            if (edges & BT_MOVEUP)
            {
                if (--opt_cursor < 0)
                    opt_cursor = 4;
            }
            if (edges & BT_MOVEDOWN)
            {
                if (++opt_cursor >= 5)
                    opt_cursor = 0;
            }

            if (opt_cursor == 0)
            {
                if (edges & (BT_MOVELEFT | BT_STRAFELEFT))
                {
                    if (--menu_start_map < 1)
                        menu_start_map = 9;
                }
                if (edges & (BT_MOVERIGHT | BT_STRAFERIGHT | BT_ATTACK | BT_START | BT_SPEED))
                {
                    if (++menu_start_map > 9)
                        menu_start_map = 1;
                }
            }
            else if (opt_cursor == 1 && (edges & (BT_START | BT_ATTACK | BT_SPEED)))
            {
                *out_episode = 1;
                *out_skill   = skill_cursor;
                saturn_map   = menu_start_map;
                print_centered(164, "LOADING...");
                I_FinishUpdate();
                menu_wait_release();
                return 0;
            }
            else if (opt_cursor == 2 && (edges & (BT_MOVELEFT | BT_MOVERIGHT | BT_STRAFELEFT | BT_STRAFERIGHT | BT_START | BT_ATTACK | BT_SPEED)))
            {
                Saturn_ToggleWallSubdivision();
            }
            else if (opt_cursor == 3 && (edges & (BT_MOVELEFT | BT_MOVERIGHT | BT_STRAFELEFT | BT_STRAFERIGHT | BT_START | BT_ATTACK | BT_SPEED)))
            {
                saturn_quiet_hud ^= 1;
                saturn_show_fps  = !saturn_quiet_hud;
            }
            else if (opt_cursor == 4 && (edges & (BT_START | BT_ATTACK | BT_SPEED)))
            {
                state = MENU_STATE_MAIN;
                menu_wait_release();
            }

            if (edges & BT_USE)
            {
                state = MENU_STATE_MAIN;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_READ1)
        {
            Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);
            if (!s_help1_patch)
                s_help1_patch = Saturn_CachePatchBig("HELP1");
            if (s_help1_patch)
                Saturn_DrawPatchFS(0, 0, s_help1_patch);
            if (edges & (BT_START | BT_ATTACK | BT_USE | BT_SPEED))
            {
                state = MENU_STATE_READ2;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_READ2)
        {
            Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);
            if (!s_help2_patch)
                s_help2_patch = Saturn_CachePatchBig("HELP2");
            if (s_help2_patch)
                Saturn_DrawPatchFS(0, 0, s_help2_patch);
            if (edges & (BT_START | BT_ATTACK | BT_USE | BT_SPEED))
            {
                state = MENU_STATE_CREDIT;
                menu_wait_release();
            }
        }
        else if (state == MENU_STATE_CREDIT)
        {
            Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, DOOM_FBHEIGHT, 0);
            if (!s_credit_patch)
                s_credit_patch = Saturn_CachePatchBig("CREDIT");
            if (s_credit_patch)
                Saturn_DrawPatchFS(0, 0, s_credit_patch);
            if (edges & (BT_START | BT_ATTACK | BT_USE | BT_SPEED))
            {
                state = MENU_STATE_MAIN;
                menu_wait_release();
            }
        }

        Saturn_BootRec(8);
        I_FinishUpdate();
        Saturn_BootRec(9);
    }
}

/* ---- Interactive In-Game Pause Menu ---- */
static int pause_cursor = 0;
static int pause_warp_map = 1;

void Saturn_Pause_Init(void)
{
    pause_cursor = 0;
    if (saturn_map >= 1 && saturn_map <= 9)
        pause_warp_map = saturn_map;
}

void Saturn_Pause_Tick(int edges)
{
    extern boolean gamepaused;

    menu_frame++;

    if (edges & BT_MOVEUP)
    {
        if (--pause_cursor < 0)
            pause_cursor = 5;
    }
    if (edges & BT_MOVEDOWN)
    {
        if (++pause_cursor >= 6)
            pause_cursor = 0;
    }

    if (pause_cursor == 1)
    {
        if (edges & (BT_MOVELEFT | BT_STRAFELEFT))
        {
            if (--pause_warp_map < 1)
                pause_warp_map = 9;
        }
        if (edges & (BT_MOVERIGHT | BT_STRAFERIGHT))
        {
            if (++pause_warp_map > 9)
                pause_warp_map = 1;
        }
    }
    else if (pause_cursor == 3 && (edges & (BT_MOVELEFT | BT_MOVERIGHT | BT_STRAFELEFT | BT_STRAFERIGHT)))
    {
        Saturn_ToggleWallSubdivision();
    }
    else if (pause_cursor == 4 && (edges & (BT_MOVELEFT | BT_MOVERIGHT | BT_STRAFELEFT | BT_STRAFERIGHT)))
    {
        saturn_quiet_hud ^= 1;
        saturn_show_fps  = !saturn_quiet_hud;
    }

    if (edges & (BT_ATTACK | BT_SPEED | BT_START))
    {
        if (pause_cursor == 0)
        {
            gamepaused = 0;
        }
        else if (pause_cursor == 1)
        {
            saturn_warp_episode = (saturn_episode > 0) ? saturn_episode : 1;
            saturn_warp_map     = pause_warp_map;
            gamepaused          = 0;
        }
        else if (pause_cursor == 2)
        {
            saturn_warp_episode = (saturn_episode > 0) ? saturn_episode : 1;
            saturn_warp_map     = (saturn_map > 0) ? saturn_map : 1;
            gamepaused          = 0;
        }
        else if (pause_cursor == 3)
        {
            Saturn_ToggleWallSubdivision();
        }
        else if (pause_cursor == 4)
        {
            saturn_quiet_hud ^= 1;
            saturn_show_fps  = !saturn_quiet_hud;
        }
        else if (pause_cursor == 5)
        {
            saturn_quit_to_title = 1;
            gamepaused           = 0;
        }
    }
    else if (edges & BT_USE)
    {
        gamepaused = 0;
    }
}

void Saturn_Pause_Draw(void)
{
    const void *p = Saturn_CachePatch("M_PAUSE");
    char        wline[24];

    Saturn_Vdp1_Enter2D();

    /* Draw background over the 3D viewport rows (0..167) while keeping the status bar (168..223) */
    Saturn_ClearRect(0, 0, DOOM_SCREENWIDTH, 168, 0);
    if (!s_title_patch)
        s_title_patch = Saturn_CachePatchBig("TITLEPIC");
    if (s_title_patch)
        Saturn_DrawPatch(0, 0, s_title_patch, 0, 168);
    Saturn_ClearRect(44, 6, 232, 156, 0);

    draw_centered(12, p);

    Saturn_Print(76, 40, "RESUME GAME");

    wline[0] = 'W'; wline[1] = 'A'; wline[2] = 'R'; wline[3] = 'P';
    wline[4] = ' '; wline[5] = 'L'; wline[6] = 'E'; wline[7] = 'V';
    wline[8] = 'E'; wline[9] = 'L'; wline[10] = ':'; wline[11] = ' ';
    wline[12] = 'E'; wline[13] = (char)('0' + (saturn_episode > 0 ? saturn_episode : 1));
    wline[14] = 'M'; wline[15] = (char)('0' + pause_warp_map);
    wline[16] = 0;
    Saturn_Print(76, 58, wline);

    Saturn_Print(76, 76, "RESTART LEVEL");
    Saturn_Print(76, 94, saturn_wall_subdivision ? "WALL STRIPS: ON" : "WALL STRIPS: OFF");
    Saturn_Print(76, 112, (!saturn_quiet_hud || saturn_show_fps) ? "PERF HUD: ON" : "PERF HUD: OFF");
    Saturn_Print(76, 130, "QUIT TO TITLE");

    draw_skull(48, 34 + pause_cursor * 18);
}
