/* g_game.c — Saturn stand-ins for d32xr's G_InitNew / G_Ticker and
 * Saturn-specific init helpers. d32xr's real g_game.c is not part of
 * the Saturn build, so the game-state essentials of G_InitNew are
 * reproduced here (ported from d32xr/d_main.c:497-520). */
#include <stdbool.h>
#include <stdio.h>
#include "doomdef.h"
#include "p_local.h"
#include "saturn.h"
#include "i_system.h"   /* Saturn_Format */

#ifndef SAT_FRT
#ifdef SATURN_WIN32
extern unsigned short Win32_GetFRT(void);
#define SAT_FRT() Win32_GetFRT()
#else
static inline unsigned short Saturn_Game_ReadFRT(void)
{
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned char h1 = frt[0];
    unsigned char l  = frt[1];
    unsigned char h2 = frt[0];
    if (__builtin_expect(h1 != h2, 0))
        l = frt[1];
    return (unsigned short)(((unsigned int)h2 << 8) | (unsigned int)l);
}
#define SAT_FRT() Saturn_Game_ReadFRT()
#endif
#endif

/* Not declared extern in doomdef.h — defined in d32xr_stubs.c. */
extern VINT gameepisode;
extern VINT gamemap;

/* d_main.c — quit-to-title flag (the pause plaque's "TO QUIT") and the
 * level-completion latches (saturn_inter.c chain). */
extern volatile int saturn_quit_to_title;
extern volatile int saturn_level_done;
extern volatile int saturn_secret_exit;

/* d32xr_stubs.c — real bodies (ported from d32xr/g_game.c). */
extern void G_PlayerFinishLevel(int player);

/* Pick the lump name. For DOOM2: "MAPnn". For DOOM1: "EXMyy".
 * Hand-built character by character — NO number formatter in the path.
 * The lookup compares this name against the WAD directory byte for
 * byte, and the 2026-08-30 MAP-NOT-FOUND screen showed no digits at
 * all: if Saturn_Format's %d emitted nothing, the search name became
 * "E-M-" and E1M1 (directory entry 7) was missed even though the
 * directory itself was fine. p_setup.c's diagnostic now prints a
 * FMT=OK/FMT=NO verdict so the formatter's health is readable on the
 * same screen. */
static void pick_lump_name(int episode, int map, char *out, int outsz)
{
    if (outsz < 9)
        return;
    if (saturn_gamemission == 1)
    {
        out[0] = 'M';
        out[1] = 'A';
        out[2] = 'P';
        out[3] = (char)('0' + (map / 10) % 10);
        out[4] = (char)('0' + map % 10);
        out[5] = 0;
    }
    else
    {
        out[0] = 'E';
        out[1] = (char)('0' + episode % 10);
        out[2] = 'M';
        out[3] = (char)('0' + map % 10);
        out[4] = 0;
    }
}

/* r67: level-init breadcrumb. The r66 real-HW photo showed the boot
 * fill COMPLETING (F and R both 4-digit, G/C small = healthy CD speed)
 * and then wedging BEFORE the DARK YELLOW "map found" marker — i.e.
 * inside Saturn_G_InitNew/P_SetupLevel but outside every stamped GFS
 * call. The ISR paints I=<this> pre-loop (in the census H row's slot);
 * the last site reached names the wedged step:
 *   1 = Saturn_G_InitNew entry     5 = after R_ClearTextures
 *   2 = before Saturn_P_SetupLevel 6 = after W_LoadPWAD(PWAD_CD)
 *   3 = P_SetupLevel entry         7 = after W_CheckNumForName
 *   4 = after P_InitThinkers       (next paint would be DARK YELLOW)
 *   9 = Saturn_P_SetupLevel RETURNED (never reached while wedged)
 * NOTE on ordering: the ISR paints these rows; the main thread only
 * stores, so a photo can lag the true site by < 1 window — but the
 * wedge freezes it at the true value. */
volatile unsigned int saturn_init_site = 0;

/* Level timer for the intermission's TIME row (saturn_inter.c). Tics
 * at TICRATE (30); frozen while paused, zeroed per level in
 * Saturn_G_InitNew. d32xr's g_game.c (not linked) never tracked one —
 * the Jaguar WI screen was cut. */
int saturn_leveltime = 0;

/* 1 = the next Saturn_G_InitNew is a FRESH GAME start (pistol + 50
 * bullets via P_ResetResp); 0 = a level ADVANCE (the loadout saved by
 * G_PlayerFinishLevel at the exit latch comes back through
 * P_RestoreResp in G_PlayerReborn). d_main.c (title start) and
 * saturn_demo.c set it before calling; the first InitNew clears it. */
int saturn_new_game = 1;

void Saturn_G_InitNew(int skill, int episode, int map)
{
    extern degenmobj_t emptymobj;
    int  i;
    /* gamemaplump must outlive this frame (d32xr keeps it pointing at
     * the WAD-resident name), so the buffer is static. */
    static char maplump_buf[16];

    saturn_init_site = 1;
    Saturn_GsubStamp(21);
    M_ClearRandom();
    pick_lump_name(episode, map, maplump_buf, sizeof(maplump_buf));
    gamemaplump = maplump_buf;
    gameskill  = skill;
    gameepisode = episode;
    gamemap    = map;
    for (i = 0; i < MAXPLAYERS; i++)
    {
        players[i].playerstate = PST_REBORN;
        players[i].mo = (mobj_t *)&emptymobj;   /* net consistency checks */
        if (saturn_new_game)
            P_ResetResp(&players[i]);   /* fresh game: 100 hp, pistol, 50 */
    }
    saturn_new_game = 0;
    playeringame[0] = true;
    playeringame[1] = false;
    demoplayback  = false;
    demorecording = false;
    gametic       = 0;
    netgame       = 0;
    splitscreen   = false;

    /* A stale ga_completed/ga_secretexit from the previous level would
     * make D_DoomLoop return before the first tic ran. d32xr's
     * G_InitNew clears it (g_game.c:154); this port must too. */
    gameaction    = ga_nothing;
    saturn_level_done = 0;

    /* Intermission totals + level timer. The kill/item counters must
     * zero BEFORE P_SetupLevel — p_mobj.c's spawn path counts the
     * level's population into them. */
    totalkills  = 0;
    totalitems  = 0;
    totalsecret = 0;
    saturn_leveltime = 0;

    /* Level setup with the correct lump name. */
    {
        /* Round-52: level-load census. If the demo-restart path ever
         * loops here every frame, the verdict's L row climbs while the
         * picture stays identical — the reload storm made visible. */
        extern volatile unsigned int saturn_load_count;

        saturn_load_count++;
    }
    saturn_init_site = 2;
    Saturn_GsubStamp(22);
#if !defined(SATURN_BISECT) || SATURN_BISECT != 2
    Saturn_P_SetupLevel(episode, map, skill);
#endif
}

void Saturn_G_Ticker(void)
{
    /* r105 UI: pause + quit-to-title. d32xr's MARS P_CheckCheats only
     * handles cheats once ALREADY paused — the toggle itself is ours.
     * BT_PAUSE rides the Start button (i_system.c); edges come from
     * the ticbuttons pair I_ReadControls maintains. Runs BEFORE
     * P_Ticker so the frozen state is seen this same tic (P_Ticker
     * returns 0 while gamepaused — p_tick.c:359). */
    {
        extern void Saturn_Pause_Init(void);
        extern void Saturn_Pause_Tick(int edges);
        player_t *p = &players[consoleplayer];
        int       edges = p->ticbuttons & ~p->oldticbuttons;

        if (!gamepaused)
        {
            if (edges & BT_PAUSE)
            {
                gamepaused = 1;
                Saturn_Pause_Init();
            }
        }
        else
        {
            Saturn_Pause_Tick(edges);
        }
    }

    /* Level time for the WI screen: count playing tics only (the
     * pause freeze above means P_Ticker is a no-op while paused). */
    if (!gamepaused)
        saturn_leveltime++;

    Saturn_P_Ticker();
    G_Ticker();

    /* d32xr's P_Ticker clears gameaction at the START of every tic
     * (p_tick.c:338 — its MiniLoop consumed the return value, so that
     * clear is the "one-shot consumed" mechanism). A level exit set by
     * the switch mid-tic is therefore lost whenever a catch-up frame
     * runs two tics before D_DoomLoop's loop-top check reads it. Latch
     * the completion the moment the tic returns, before any later tic
     * can clear it; D_DoomLoop returns on saturn_level_done. */
    if (gameaction == ga_completed || gameaction == ga_secretexit)
    {
        /* Level exit: clear powers/cards and save the loadout into
         * playersresp[] (P_UpdateResp) so the next map's G_PlayerReborn
         * restores it. d32xr does this in G_DoLoadLevel's teardown;
         * this port consumes gameaction here instead. */
        G_PlayerFinishLevel(consoleplayer);
        saturn_secret_exit = (gameaction == ga_secretexit);
        saturn_level_done  = 1;
        gameaction         = ga_nothing;
    }
}

void Saturn_G_Display(void)
{
    /* High-performance Sega Saturn Hardware Renderer (saturn_hw_render.c).
     * Replaces CPU software raycasting with VDP1 distorted quads,
     * hardware DIVU pipelining, and VDP2 background planes. */
    extern void Saturn_HW_RenderPlayerView(int displayplayer);
    extern void Saturn_PSprite_Draw(void);
    extern void Saturn_HUD_Draw(void);
    extern int  saturn_game_started;

    if (gamepaused)
    {
        extern void Saturn_Pause_Draw(void);

        Saturn_HUD_Draw();
        Saturn_Pause_Draw();
        return;
    }
    saturn_game_started = 1;
    Saturn_HW_RenderPlayerView(0);
    {
        extern volatile unsigned int saturn_hw_prof_hud;
        extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
        extern volatile unsigned int saturn_vbl_count;
        unsigned int vh0;
        unsigned short th0;
        do {
            vh0 = saturn_vbl_count;
            th0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != vh0, 0));
        Saturn_HUD_Draw();
        saturn_hw_prof_hud = Saturn_ProfSpan(th0, vh0);
    }
}

/* Saturn_W_Init wrapper — loads via i_wad.c's I_WadBase. */
void Saturn_W_Init(const char *filename)
{
    extern byte *I_WadBase(void);
    extern wadfile_t wadfile[MAXWADS];
    (void)filename;
    I_WadBase();
    {
        extern void W_Init(void);
        W_Init();
    }
    /* W_Init does D_memset(wadfile,...) before calling I_WadBase, but
     * re-apply the dispatch flags anyway — cdlength=1 is what routes
     * EVERY W_GetRawLumpData through our I_*CDFile* shims (hot =
     * cart-resident, cold = CD stream; see i_wad.c). If this ever gets
     * wiped, lump reads silently fall back to `fileptr + filepos`,
     * which points into unmapped A-bus space. */
#ifndef SATURN_WIN32
    wadfile[0].cdlength = 1;
#else
    wadfile[0].cdlength = 0;
#endif
    wadfile[0].cdoffset = 0;
}

void Saturn_P_SetupLevel(int episode, int map, int skill)
{
    extern volatile unsigned int saturn_init_site;
    extern void Saturn_SetSkyTexture(const char *name);
    char lump[16];
    pick_lump_name(episode, map, lump, sizeof(lump));
#ifdef SATURN_WIN32
    printf("Saturn_P_SetupLevel: lump=%s, skill=%d. Calling P_SetupLevel\n", lump, skill); fflush(stdout);
#endif
    const char *sky = "SKY1";
    if (episode == 2) sky = "SKY2";
    else if (episode == 3) sky = "SKY3";
    else if (episode == 4) sky = "SKY4";

    Saturn_SetSkyTexture(sky);
    P_SetupLevel(lump, (skill_t)skill, sky);
#ifdef SATURN_WIN32
    printf("Saturn_P_SetupLevel: P_SetupLevel returned. Calling saturn_iwad_fill_drain_all\n"); fflush(stdout);
#endif

    /* Drain all pending cart fills so every queued lump is resident in cart RAM */
    extern void saturn_iwad_fill_drain_all(void);
    saturn_iwad_fill_drain_all();
#ifdef SATURN_WIN32
    printf("Saturn_P_SetupLevel: saturn_iwad_fill_drain_all done. Calling Saturn_HW_RenderInit\n"); fflush(stdout);
#endif

    /* Preload all level flats, textures, weapon sprites, and SKY into RAM */
    extern void Saturn_HW_RenderInit(void);
    Saturn_HW_RenderInit();
#ifdef SATURN_WIN32
    printf("Saturn_P_SetupLevel: Saturn_HW_RenderInit done\n"); fflush(stdout);
#endif

    saturn_init_site = 9;   /* returned — never reached while wedged */
    Saturn_GsubStamp(29);
}

void Saturn_WarpToLevel(int episode, int map)
{
    extern memzone_t *mainzone;
    extern void ST_InitEveryLevel(void);
    extern int saturn_skill;
    extern int saturn_episode;
    extern int saturn_map;
    extern volatile int saturn_level_done;
#ifdef SATURN_WIN32
    extern void Telemetry_StopJourney(void);
    extern void Telemetry_ResetSpike(void);
#endif

    printf("[Engine] Level Warp requested -> Episode %d, Map %d\n", episode, map);
    fflush(stdout);

#ifdef SATURN_WIN32
    Telemetry_StopJourney();
    Telemetry_ResetSpike();
#endif

    /* Free previous level allocations from zone memory */
    Z_FreeTags(mainzone);

    /* Update game episode and map globals */
    saturn_episode = episode;
    saturn_map = map;
    saturn_new_game = 1;

    /* Initialize status bar */
    ST_InitEveryLevel();

    /* Initialize new game and load map */
    Saturn_G_InitNew(saturn_skill, saturn_episode, saturn_map);

    /* Reset state latches */
    saturn_level_done = 0;
    gameaction = ga_nothing;

    printf("[Engine] Level Warp to E%dM%d complete!\n", episode, map);
    fflush(stdout);
}

void Saturn_P_Ticker(void)
{
    P_Ticker();
}