/* d_main.c — Saturn-tuned DOOM entry. The original id release is ~800
 * lines with config-file parsing, shareware detection, and a multi-mode
 * start screen. The Saturn port strips all of that and goes straight
 * into the first available map.
 *
 * Boot order matters:
 *   1. I_InitVideo first — it registers the VBlank callback that ticks
 *      saturn_vbl_count (the frame clock the main loop runs on) and it
 *      sets up the software renderer.
 *   2. WAD next — Saturn_W_Init probes the cart and builds the lump
 *      directory. The Z zone lives in the cart's upper half, so Z_Init
 *      must run after this.
 *   3. Z_Init, then the player arrays (d32xr's G_Init does this; the
 *      real g_game.c is not part of the Saturn build).
 */
#include <stdint.h>
#include <string.h>

#ifdef SATURN_WIN32
#define WIN32_LEAN_AND_MEAN
#define boolean win32_boolean_t
#define byte win32_byte_t
#include <windows.h>
#include <mmsystem.h>
#undef boolean
#undef byte
#endif

#include "saturn.h"
#include "d_main.h"
#include "dstrings.h"
#include "i_system.h"
#include "i_video.h"
#include "saturn_tex.h"
#include "saturn_slave.h"
#include "saturn_cache.h"

#define DEFAULT_WAD_NAME   "DOOM.WAD"

/* d32xr's P_Init (p_setup.c). Declared locally rather than pulling in
 * p_local.h: it allocates d32xr's six POINTER-based tables (anims,
 * switchlist, buttonlist, linespeciallist, activeplats, activeceilings)
 * and fills the anim/switch lump lists. d32xr's own d_main calls it
 * right after Z_Init; the Saturn entry was missing it, leaving all six
 * NULL — the "SPC-MISC yellow" hang was P_SpawnSpecials memsetting
 * address 0 (BIOS ROM area on real hardware: silent freeze). */
extern void P_Init(void);

/* No-cart fallback Z zone. Only used when the extended-RAM cart is
 * absent — big enough to reach a visible I_Error, not to play (a real
 * level needs ~150-250 KB; with a cart we get the 2 MB upper half).
 * BSS headroom is razor-thin (user RAM ends at 0x060C0000 and the map
 * was within a hundred bytes of it), so keep this as small as still
 * serves its purpose — and trim it further whenever a debug build
 * needs the slack. */
#define ZONE_POOL_BYTES    (2 * 1024)

static byte   z_zone_pool[ZONE_POOL_BYTES];

#ifndef SATURN_BOOT_MAP
#define SATURN_BOOT_MAP 1
#endif
saturn_gamemode_t  saturn_gamemode    = saturn_shareware;
int                saturn_gamemission = 0;
int                saturn_episode     = 1;
int                saturn_map         = 0;
int                saturn_skill       = 2;       /* sk_medium */
/* Recorder consume gate (i_video.c): the boot smoke test ALSO runs
 * I_FinishUpdate/fps_measure, and the recorder's BUP path needs the
 * Z zone (Z_Init hasn't run until after the smoke test) — so the
 * consume waits until D_DoomLoop is actually entered. */
int                saturn_game_started = 0;
/* Quit-to-title (r105 UI): set by the pause plaque's "TO QUIT"
 * (Saturn_G_Ticker), checked at the D_DoomLoop loop top. D_DoomMain
 * then frees the level and re-runs the title menu. */
volatile int       saturn_quit_to_title = 0;
/* Level completion (2026-09-06): the exit-switch specials set
 * gameaction = ga_completed / ga_secretexit (d32xr_stubs.c); D_DoomLoop
 * latches them here and returns. D_DoomMain's level chain then runs the
 * intermission (saturn_inter.c), frees the level and inits the next
 * map — all without touching the title menu. */
volatile int       saturn_level_done    = 0;
volatile int       saturn_secret_exit   = 0;
volatile int       saturn_warp_episode  = 0;
volatile int       saturn_warp_map      = 0;
/* r106 attract demos (saturn_demo.c): FeedTic runs in D_DoomLoop's tic
 * loop once per tic while demoplayback is on; the extern lives at file
 * scope so both the pad-capture and tic-loop scopes see it. */
extern int         Saturn_Demo_FeedTic(int real_pad);
extern volatile int saturn_demo_active;
/* SH-2 DIVU probe result (d_main.c D_DoomMain): 1 = hardware divide at
 * 0xFFFFFF00/08 works (100/7 -> 14). Read by the HUD 'V' row. */
volatile int       saturn_divu_probe  = 0;

/* OCRAM divide LUT (SATURN_DIVLUT, 2026-09-08 — the SCUDSP-rig V3ocr
 * port). ok = boot install verified (Saturn_Divlut_Init); on = live
 * A/B toggle flipped by the A+B+C pad chord in I_ReadControls (starts
 * = ok); err = 0 ok, 1 CCR readback, 2 fill data verify. Boot-painted
 * glyphs are erased by the next I_FinishUpdate copy_back, so the
 * verdict must live in file-scope ints and be republished in-game
 * (i_video.c perf rows, X row). */
volatile int       saturn_divlut_ok  = 0;
volatile int       saturn_divlut_on  = 0;
volatile int       saturn_divlut_err = 0;
volatile int       saturn_divlut_badd = 0;   /* failing verify index    */
volatile int       saturn_divlut_badr = 0;   /* read-back at that index */
volatile int       saturn_divlut_badsub = 0; /* 1 data / 2 64-bit math  */
#ifdef SATURN_DIVLUT_NOOCRAM
/* Kronos diagnostic variant: the LUT lives in WRAM .bss (2048B —
 * check __bend < 0x060C0000 after linking) and no CCR write happens.
 * The real-HW build keeps the rig-proven 2KB OCRAM scratchpad. */
volatile unsigned int sat_divlut_table[512];
#endif

/* OCRAM probe (SATURN_OCRAM_PROBE, D_DoomMain): ok = CCR 0x09 write
 * survived + both data-array words verified; ccr0/ccr1 = the CCR byte
 * before/after the write. Republished by the HUD 'O' row — boot-painted
 * glyphs are erased by the next I_FinishUpdate copy_back, so the
 * verdict must live in file-scope ints. */
volatile int       saturn_oc_ok   = 0;
volatile int       saturn_oc_ccr0 = -1;
volatile int       saturn_oc_ccr1 = -1;

/* d32xr's d_main.c owns this on 32X; the Saturn replacement file must
 * provide it (G_InitNew-style state init parks dead players' mo
 * pointers on it). */
degenmobj_t  emptymobj;

/* FRT tick reading (SH-2 Free-Running Timer, TCNT @ 0xFFFFFE12, φ/8 ≈
 * 3.6 MHz — same primitive the render prologue's windows use). */
#ifndef SATURN_WIN32
static inline unsigned short Saturn_ReadFRT(void)
{
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned short hi0 = frt[0];
    __asm__ volatile("" ::: "memory");
    unsigned short lo = frt[1];
    __asm__ volatile("" ::: "memory");
    unsigned short hi1 = frt[0];
    if (__builtin_expect(hi0 != hi1, 0))
    {
        __asm__ volatile("" ::: "memory");
        lo = frt[1];
        hi0 = hi1;
    }
    return (unsigned short)((hi0 << 8) | lo);
}
#define SAT_FRT() Saturn_ReadFRT()
#else
extern unsigned short Win32_GetFRT(void);
#define SAT_FRT() Win32_GetFRT()
#endif

/* FRT ticks per vblank — MEASURED on this machine, not assumed. The
 * 21536 constant was calibrated on Kronos; the 2026-09-03 real-HW
 * capture had self-contradictory rows (M = 512 units while the whole
 * render window it sits inside read 6 vblanks), which is the signature
 * of a wrong ticks/vbl in every wrap-reconstruction. One number on the
 * HUD 'R' row settles every row's units: 21536 = Kronos rate; ~0 =
 * dead FRT; anything else = the real prescale. */
/* 2026-09-03 photo #144 killed the single 16-bit-delta calibration: the
 * raw readback was 65409 ticks across 3 vblanks — only 127 under a full
 * fold — so the true rate could be 21803, 43648, or any +21845 step, and
 * EVERY reconstructed row inherited the ambiguity (s+M summed past D no
 * matter which rate was assumed). Fix: accumulate 32-bit by summing
 * 16-bit deltas in a poll loop, which cannot fold as long as each poll
 * gap is << 65536 ticks (a wrap is ~0.8-3 ms; a poll iteration is ~µs). */
volatile int saturn_frt_rate = 21536;
volatile int saturn_frt_vbl_reset = 0;

void Saturn_FrtCalibrate(void)
{
    extern volatile unsigned int saturn_vbl_count;
    static int done = 0;
    unsigned int v0, total, rst_count;
    unsigned short f0, f1;

    if (done)
        return;
    done = 1;
#ifdef SATURN_WIN32
    saturn_frt_rate = 21536;
    saturn_frt_vbl_reset = 0;
    return;
#endif

    /* Guarantee Master SH-2 FRT runs at phi/32 without output-compare reset */
    (*(volatile unsigned char *)0xFFFFFE10u) = 0x00u; /* TIER: disable all FRT interrupts */
    (*(volatile unsigned char *)0xFFFFFE16u) = 0x01u; /* TCR: internal clock phi/32 */
    (*(volatile unsigned char *)0xFFFFFE11u) = 0x00u; /* FTCSR: CCLRA=0 (free-running) */

    v0 = saturn_vbl_count;
    while (saturn_vbl_count == v0)
        ;                                   /* next vblank edge */
    v0 = saturn_vbl_count;
    f0 = SAT_FRT();
    total = 0;
    rst_count = 0;
    while ((int)(saturn_vbl_count - (v0 + 3)) < 0)
    {
        f1 = SAT_FRT();
        if (f1 >= f0)
        {
            total += (unsigned int)(f1 - f0);
        }
        else if (f0 > 0xE000u)
        {
            total += (unsigned int)(unsigned short)(f1 - f0);
        }
        else if (f1 < 0x1000u && f0 > 0x2000u)
        {
            total += (unsigned int)f1;
            rst_count++;
        }
        f0 = f1;
    }
    f1 = SAT_FRT();
    if (f1 >= f0)
        total += (unsigned int)(f1 - f0);
    else if (f0 > 0xE000u)
        total += (unsigned int)(unsigned short)(f1 - f0);
    else if (f1 < 0x1000u && f0 > 0x2000u)
    {
        total += (unsigned int)f1;
        rst_count++;
    }
    saturn_frt_vbl_reset = (rst_count >= 2u) ? 1 : 0;
    saturn_frt_rate = (int)(total / 3u);
    if (saturn_frt_rate < 3000 || saturn_frt_rate > 60000)
        saturn_frt_rate = 14913;
}

/* Perf HUD (2026-10-05): per-phase FRT tick accumulators, summed every
 * frame and averaged + cleared by the HUD's FPS window (saturn_hud.c).
 * [0] T = game tics, [1] R = render (Saturn_G_Display), [2] V = VDP1
 * frame (CEF wait + command write), [3] C = fb letterbox + VDP2 copy,
 * [4] S = vblank/flip + display-cap waits, [5] D = background CD
 * cart-fill drain. */
volatile unsigned int saturn_pf_acc[6];

/* 32-bit FRT span from (f0, v0): handles both real Saturn hardware
 * (where the BIOS VBlank-In dispatcher resets FRC to 0 every vblank)
 * and emulators (where FRC free-runs 0..65535 across vblanks). */
unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0)
{
    extern volatile unsigned int saturn_vbl_count;
    unsigned int   v1;
    unsigned short f1;

    do {
        v1 = saturn_vbl_count;
        f1 = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != v1, 0));

    if (saturn_frt_vbl_reset)
    {
        unsigned int dv = v1 - v0;
        int span = (int)(dv * (unsigned int)saturn_frt_rate) + (int)f1 - (int)f0;
        if (span < 0)
            span += saturn_frt_rate;
        return (span > 0) ? (unsigned int)span : 0u;
    }

    unsigned int fd = (unsigned int)(unsigned short)(f1 - f0);
    if (v1 == v0)
        return fd;

    unsigned int rate     = (unsigned int)saturn_frt_rate;
    unsigned int expected = (v1 - v0) * rate;
    int          diff     = (int)expected - (int)fd;
    int          wraps    = (diff + 32768) >> 16;

    if (wraps < 0)
        wraps = 0;
    while (wraps > 0 && fd + (unsigned int)wraps * 65536u > expected + rate)
        wraps--;
    return fd + (unsigned int)wraps * 65536u;
}

/* Game-logic tic body cost, FRT ticks >>8, accumulated across the
 * catch-up loop's tics and read by the i_video.c HUD 'B' row. */
volatile int saturn_frt_tic = 0;

/* Background-fill internals, peak-held for the HUD: FRT cost of the
 * drain (>>8) and queue entries drained in the worst recent frame. */
volatile int saturn_frt_fill  = 0;
volatile int saturn_prof_fllen = 0;

/* Frame-window profiler, filled from D_DoomLoop (see the loop below):
 * saturn_prof_fill  = vblanks spent in the background cart-fill drain
 * saturn_prof_ticvbl= vblanks spent in the tic catch-up loop (wall) */
volatile int saturn_prof_fill   = 0;
volatile int saturn_prof_ticvbl = 0;

/* Z-zone base: the cart's upper 2 MB via the CACHED mirror when the
 * 4 MB cart is present (the zone is CPU-only memory — Z_Malloc headers,
 * thinker lists, map structs), else the small static fallback pool. */
#ifndef SATURN_WIN32
byte *I_ZoneBase(int *size)
{
    byte *cart_zone = Saturn_ZoneBase(size);   /* i_wad.c */
    if (cart_zone)
        return cart_zone;
    if (size)
        *size = (int)sizeof(z_zone_pool);
    return z_zone_pool;
}
#endif

/* r89 bisect park — see SATURN_BISECT in saturn.h. Paints the level
 * marker + PARK and pumps frames forever. */
#if SATURN_BISECT
static void saturn_bisect_park(int level)
{
    extern void Saturn_StageText(int x, int y, const char *s);
    /* r93: the StageText park paint NEVER reaches the display (missing
     * on Kronos screenshots too) — "no park text" was not a reliable
     * discriminator. The ISR census I row repaints every 64 vbl and is
     * proven visible on real HW, so ride it: A<n> = parked at B<n>.
     * 0xA0+level can never collide with init sites 1-9 or 0x50+sub. */
    {
        extern volatile unsigned int saturn_init_site;

        saturn_init_site = 0xA0u + (unsigned)level;
    }
    {
        char bl[4];
        bl[0] = 'B';
        bl[1] = (char)('0' + level);
        bl[2] = 0;
        /* r89: Saturn_StageText (NBG0 pages) — Saturn_BootText draws into
         * the hidden NBG1 layer and is invisible on this display path. */
        Saturn_StageText(8, 32, bl);
        Saturn_StageText(8, 48, "PARK");
    }
    for (;;)
        Saturn_PumpFrame();
}
#endif

void D_DoomMain(void)
{
    byte   *playpal;
#ifdef SATURN_WIN32
    printf("D_DoomMain: Entered\n"); fflush(stdout);
#endif

    /* WHITE: the boot report loop completed, so slSynch() returns —
     * and D_DoomMain was actually entered. */
    Saturn_StageColor(JO_COLOR_White);

    /* No command line on Saturn; fake one. */
    M_FakeArgv("-iwad /CD/DOOM.WAD");

    /* Framebuffer + software renderer + VBlank tick callback. */
#ifdef SATURN_WIN32
    printf("D_DoomMain: Calling I_InitVideo\n"); fflush(stdout);
#endif
    I_InitVideo();
#ifdef SATURN_WIN32
    printf("D_DoomMain: I_InitVideo returned, starting 45 smoke frames\n"); fflush(stdout);
#endif

    /* Display smoke test: fill the framebuffer with a vertical grayscale
     * gradient (the default palette is grayscale) and push it through
     * the complete display path for ~45 frames. Visible bands prove the
     * whole game-frame pipeline works BEFORE the WAD load can muddy the
     * waters. Under the VDP2 8bpp direct display this is
     * fb-copy -> NBG0 bitmap flip -> slSynch (the first frame also
     * hides NBG1, so the boot stage colors end here). The fb is
     * 320 COLUMNS wide (full-width render since 2026-09-05; NBG0 zoom
     * 1.0 displays it 1:1). */
    {
        int i, f;
        int fb_w = DOOM_SCREENWIDTH;

        for (i = 0; i < fb_w * DOOM_FBHEIGHT; i++)
            I_VideoBuffer[i] = (byte)((i / fb_w) * 256
                                       / DOOM_FBHEIGHT);
        for (f = 0; f < 45; f++)
            I_FinishUpdate();
        /* Wipe the gradient: rows the game never redraws (above the 3D
         * view, the status-bar strip when ST_Drawer is off) would
         * otherwise keep showing smoke-test indices recolored by
         * PLAYPAL for the rest of the session. */
        D_memset(I_VideoBuffer, 0, fb_w * DOOM_FBHEIGHT);
        I_FinishUpdate();
        /* r86: I_FinishUpdate stamped frame_done_vbl ~= 46 — the last
         * commit the boot ever makes until the demo starts. With the
         * stamp left standing, verdict trigger 2 (frame_done != 0 +
         * stall > 1200) false-fires ~20 s into any slow boot. The
         * stamp's job (qualifying the stall as post-first-frame) is
         * done; zero it so the pre-demo boot is commit-free again. */
        {
            extern volatile unsigned int saturn_frame_done_vbl;

            saturn_frame_done_vbl = 0;
        }
    }

#ifdef SATURN_OCRAM_PROBE
    /* OCRAM PROBE (2026-09-06): the 2026-09-03 "any CCR write wedges
     * the bus" verdict (saturn_cache.h:94-108) is SUSPECT — the cart
     * slot was later proven flaky (two solid-blue wedges 2026-09-06
     * cleared by a reseat, same boot region). This probe attempts the
     * write ONCE, verifies the 2KB data-array window, and RESTORES the
     * boot CCR so the rest of boot runs on SGL's config.
     *   Green + "OCPRB" frozen  = the write wedged (reseat cart, one
     *                             retry, then the verdict stands).
     *   Yellow "LOADING WAD"    = write survived. Read row O in-game. */
    {
        volatile unsigned int *oc0 = (volatile unsigned int *)0xC0000000u;
        volatile unsigned int *oc1 = (volatile unsigned int *)0xC00007F0u;
        unsigned char c0, c1;
        unsigned int  a_ok, b_ok;

        Saturn_StageColor(JO_COLOR_Green);
        Saturn_BootMsg("OCPRB");

        c0 = SATURN_SH2_CCR;                       /* read: proven safe */
        /* STN-28 init sequence: CP purge FIRST (write back dirty lines,
         * invalidate every line) so no valid line occupies ways 0/1 when
         * they become OCRAM — only then the enable value. Without the
         * purge the data-array writes below can shadow LIVE cache lines
         * (seen in Kronos 21:49 as boot-text glyph garbage). */
        SATURN_SH2_CCR = SATURN_SH2_CCR_CP;        /* 0x10: purge all */
        SATURN_SH2_CCR = (unsigned char)(SATURN_SH2_CCR_CE
                                         | SATURN_SH2_CCR_TW); /* 0x09 */
        c1   = SATURN_SH2_CCR;                     /* read back */
        *oc0 = 0x5A3C0C0Cu;  a_ok = (*oc0 == 0x5A3C0C0Cu);
        *oc1 = 0xA53CF0F0u;  b_ok = (*oc1 == 0xA53CF0F0u);
        saturn_oc_ok   = (c1 == (unsigned char)(SATURN_SH2_CCR_CE
                                                | SATURN_SH2_CCR_TW))
                         && a_ok && b_ok;
        saturn_oc_ccr0 = c0;
        saturn_oc_ccr1 = c1;
#if !defined(SATURN_OCRAM_KEEP_TW)
        SATURN_SH2_CCR = c0;    /* restore SGL's config for boot */
#endif
#if defined(SATURN_OCRAM_STATE)
        {
            extern void Saturn_Ocram_State_Init(void);

            Saturn_Ocram_State_Init();  /* pick OCRAM vs WRAM twin */
        }
#endif
    }
#endif

    /* WAD first: the cart init + directory load happen inside, and
     * the Z zone lives in the cart, so Z_Init must run after this. */
    Saturn_StageColor(JO_COLOR_Yellow);
    Saturn_BootMsg("LOADING WAD");
#ifdef SATURN_WIN32
    printf("D_DoomMain: Calling Saturn_W_Init\n"); fflush(stdout);
#endif
    Saturn_W_Init(DEFAULT_WAD_NAME);
#ifdef SATURN_WIN32
    printf("D_DoomMain: Saturn_W_Init done. Calling Z_Init\n"); fflush(stdout);
#endif

    /* Tagged zone (cart-backed when the 4 MB cart is present). */
    Z_Init();
#ifdef SATURN_WIN32
    printf("D_DoomMain: Z_Init done\n"); fflush(stdout);
#endif

    /* Round-33: reserve the backup-RAM BUP buffers (16 KB lib + 8 KB
     * work) NOW, while the zone is empty. Left lazy, the first
     * mid-gameplay Saturn_BupSave failed its 24 KB Z_Malloc and the
     * red-screen flight recorder silently saved nothing. */
    {
        extern void Saturn_BupPrealloc(void);
        Saturn_BupPrealloc();
    }

    /* Player arrays. d32xr's G_Init does this before anything can
     * touch players[i]; without it the first render derefs NULL. */
#ifdef SATURN_WIN32
    printf("D_DoomMain: Allocating player arrays with Z_Malloc\n"); fflush(stdout);
#endif
    players     = (player_t *)Z_Malloc(sizeof(player_t) * MAXPLAYERS, PU_STATIC);
    playersresp = (playerresp_t *)Z_Malloc(sizeof(playerresp_t) * MAXPLAYERS, PU_STATIC);
    D_memset(players, 0, sizeof(player_t) * MAXPLAYERS);
    D_memset(playersresp, 0, sizeof(playerresp_t) * MAXPLAYERS);
#ifdef SATURN_WIN32
    printf("D_DoomMain: Player arrays ready. Calling Saturn_R_InitData\n"); fflush(stdout);
#endif

    /* Texture pipeline (saturn_tex.c): parses TEXTURE1/PNAMES/COLORMAP
     * and fills d32xr's textures[]/numflats/flattranslation globals.
     * This is the piece the Saturn boot path never ran — d32xr's own
     * R_InitData is Jaguar-specific (T_START markers, PLAYPALS, decal
     * model) and I_Errors on PC WAD data, so nothing ever initialized
     * numtextures and every sidedef texture resolved to 0 (the flat-
     * colour renderer). Must run BEFORE P_Init: P_InitSwitchList and
     * P_LoadSidedefs resolve texture NAMES to numbers through
     * R_TextureNumForName, which needs textures[] to exist. */
    Saturn_R_InitData();
#ifdef SATURN_WIN32
    printf("D_DoomMain: Saturn_R_InitData done. Calling P_Init\n"); fflush(stdout);
#endif

    /* d32xr's pointer-table allocations (see the P_Init note above).
     * Must run before any level setup — P_SpawnSpecials touches
     * activeceilings/activeplats/buttonlist/linespeciallist on every
     * map load. */
    P_Init();
#ifdef SATURN_WIN32
    printf("D_DoomMain: P_Init returned. Calling M_LoadDefaults\n"); fflush(stdout);
#endif

    /* Stage marker: WAD + zone + player arrays all survived. */
    Saturn_StageColor(JO_COLOR_Purple);

    /* DOOM has no savegame support, no config file, no demo. */
    M_LoadDefaults();
#ifdef SATURN_WIN32
    printf("D_DoomMain: M_LoadDefaults done. Checking gamemode\n"); fflush(stdout);
#endif

    /* Identify the WAD so gamemode is set correctly. */
    if (W_CheckNumForName("MAP01") >= 0)
    {
        saturn_gamemode    = saturn_commercial;
        saturn_gamemission = 1;
        if (saturn_episode <= 0) saturn_episode = 1;
        if (saturn_map <= 0) saturn_map = SATURN_BOOT_MAP;
    }
    else if (W_CheckNumForName("E1M1") >= 0)
    {
        saturn_gamemode    = saturn_retail;
        saturn_gamemission = 0;
        if (saturn_episode <= 0) saturn_episode = 1;
        if (saturn_map <= 0) saturn_map = SATURN_BOOT_MAP;
    }
    else
    {
        saturn_gamemode    = saturn_shareware;
        saturn_gamemission = 0;
        if (saturn_episode <= 0) saturn_episode = 1;
        if (saturn_map <= 0) saturn_map = SATURN_BOOT_MAP;
    }
#ifdef SATURN_WIN32
    printf("D_DoomMain: Gamemode set: %d, map=%d. Caching PLAYPAL\n", saturn_gamemode, saturn_map); fflush(stdout);
#endif

    /* Palette so the 8bpp->16bpp frame conversion isn't stuck in one
     * color. PLAYPAL is sync-copied into cart at I_WadBase time, so
     * this read doesn't race the background fill. */
    playpal = (byte *)W_CacheLumpName("PLAYPAL", PU_STATIC);
#ifdef SATURN_WIN32
    printf("D_DoomMain: PLAYPAL cached at %p. Setting palette\n", playpal); fflush(stdout);
#endif
    if (playpal)
        I_SetPalette(playpal);
#ifdef SATURN_WIN32
    printf("D_DoomMain: Palette set. Calling Saturn_HUD_Init\n"); fflush(stdout);
#endif

    /* r105 UI: the status-bar art (STBAR/digits/keys/face) caches into
     * the cart-backed zone while it is empty. The menu reuses the same
     * patch cache for TITLEPIC/M_* art.
     * (Was gated behind the r105 UI-OFF bisect; the bisect is long over
     * and the 09-06 exit-freeze fix is verified in Kronos + real HW, so
     * the HUD cache is on for production. All ~39 patches are small
     * (<16 KB STBAR included) — under the big-patch slice threshold.) */
    {
        extern void Saturn_HUD_Init(void);

        Saturn_HUD_Init();
    }
#ifdef SATURN_WIN32
    printf("D_DoomMain: Saturn_HUD_Init done. Calling R_InitTables\n"); fflush(stdout);
#endif

    {
        extern void R_InitTables(void);
        R_InitTables();
    }
#ifdef SATURN_WIN32
    printf("D_DoomMain: R_InitTables done. Ready for main loop\n"); fflush(stdout);
#endif

    /* Game state. */
    gameaction = ga_nothing;

    /* SH-2 hardware-divide probe (2026-09-01, from the 32X study):
     * the SH7604's divide unit sits at 0xFFFFFF00 (DVSR), with TWO
     * trigger forms — write the 32-bit dividend to 0xFFFFFF04 (DVDNT,
     * signed 32/32) or write DVDNTH 0xFFFFFF10 then DVDNTL 0xFFFFFF14
     * (signed 64/32, the form FixedDiv's ((int64)a<<16)/b needs). The
     * quotient is read back from the same register ~13-16 cycles
     * later. The FIRST probe version wrote DVDNT at 0xFFFFFF08 — the
     * wrong address (d32xr 32x.h:143-146 is the authority) — so it
     * could never succeed; this one probes BOTH forms at the real
     * addresses. Result shows on the HUD 'V' row (11111 = both pass). */
#ifndef SATURN_WIN32
    {
        static volatile int *const DVSR   = (volatile int *)0xFFFFFF00u;
        static volatile int *const DVDNT  = (volatile int *)0xFFFFFF04u;
        static volatile int *const DVDNTH = (volatile int *)0xFFFFFF10u;
        static volatile int *const DVDNTL = (volatile int *)0xFFFFFF14u;
        unsigned int q32, q64;

        /* signed 32/32: 100/7 -> 14 */
        *DVSR  = 7;
        *DVDNT = 100;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          ::: "memory");
        q32 = *DVDNT;

        /* signed 64/32 with a zero high word: 100/7 -> 14 */
        *DVSR   = 7;
        *DVDNTH = 0;
        *DVDNTL = 100;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                          ::: "memory");
        q64 = *DVDNTL;

        saturn_divu_probe = (q32 == 14u && q64 == 14u) ? 1 : 0;
    }
#else
    saturn_divu_probe = 0;
#endif

    /* Boot-time OCRAM divide-LUT install (2026-09-08, operator
     * directive — RESTORED to this site): CCR writes at the CORRECT
     * address 0xFFFFFE92 are rig-proven across 20+ real-HW boots
     * (Projects\SCUDSP DMA\wad_load.c W_CCR); the 09-06 boot wedges
     * that convicted "CCR wedges this console" went through the
     * unmapped 0xFFFFFF92, and the ONE correct-address wedge was the
     * IN-GAME lazy window (FULL burn 1, solid blue). So the switch
     * happens here — the calmest window the machine has, before any
     * WAD/A-bus traffic. MARKER DISCIPLINE (probe-era): green + OCSET
     * painted BEFORE the install, the cyan STARTING below paints only
     * AFTER it — a freeze with OCSET on screen convicts the
     * CCR+install window exactly. The ladder-era boot wedges were the
     * divide-ABI bug (crash12-convicted, fixed same day), not this
     * site. */
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 1)
    extern void Saturn_Divlut_Install(void);   /* defined below (static) */
    Saturn_StageColor(JO_COLOR_Green);
    Saturn_BootMsg("OCSETUP");
    Saturn_Divlut_Install();
#endif

    /* Go straight into the first map. */
    Saturn_StageColor(JO_COLOR_Cyan);
    /* BOOT-FLIGHT RECORDER (i_system.c): the boot wedge is the
     * SYSTEM-RESET family — WRAM is wiped, so the only surviving
     * record is the cart. Paint the PREVIOUS boot's death milestone
     * first, then start recording. Real-HW wedge screen = cyan +
     * PREV=<n> (+ any letters past the strike). */
    Saturn_BootRec_PaintPrev();
    Saturn_BootRec(1);
    Saturn_BootMsg("STARTING");
    Saturn_BootRec(2);
    /* r85: the previous boot's GSUB record no longer needs its own
     * paint here — it rides the init-site breadcrumb (0x50+sub, set in
     * I_WadBase) and shows in the ISR census B/I row and the verdict
     * B/J row, both at x=8 where the real-HW TV actually displays. */
    /* r70: the 2026-09-02 blocking boot prewarm is GONE. Every real-HW
     * boot with it (r63, r64, r67 memcpy ~F=1000-1100 KB, r68 first SCU
     * DMA chunk, r69 memcpy F~700-800 KB below the clamp) froze the
     * A-bus mid-fill — the wedge point MOVES, so it is the sustained
     * boot-time write burst itself, not any address line. The red-
     * screen-era boots (the user's "it was working in the past") had NO
     * boot block: the queue drains in small per-frame batches from
     * D_DoomLoop (saturn_iwad_background_fill — real-HW-proven through
     * the perf-hunt campaign). Only the ~15 KB first-frame-critical
     * sync copies (PLAYPAL/COLORMAP/FONT) still happen at boot, inside
     * I_WadBase. */

    /* Phase A (parallelization plan): resident slave CPU for the render
     * prologue/column split. Launches via SMPC Disable -> SYS_SETSINT
     * -> Enable and installs the mailbox; the NOP handshake probe runs
     * from the loop below. -DSATURN_NO_SLAVE_HS keeps both cores off.
     * r105 UI: launched ONCE here — D_DoomLoop can now RETURN (quit-
     * to-title) and the for(;;) below re-runs the menu; the slave and
     * its mailbox must survive that. */
    /* TEMP freeze test (2026-10-09): 1 = never enable the slave SH-2, so
     * the whole frame renders master-only. Set to 0 to restore dual-CPU. */
#define SATURN_FREEZE_TEST_NO_SLAVE 0
#if !SATURN_FREEZE_TEST_NO_SLAVE
    saturn_slave_init();
#endif
    Saturn_BootRec(3);

    /* r105 UI: title screen -> episode/skill -> game, forever. The
     * menu writes saturn_episode/saturn_skill; D_DoomLoop returns when
     * the pause plaque's "TO QUIT" sets saturn_quit_to_title, the
     * level is freed (Z_FreeTags), and the menu shows again.
     * r106 attract demos: an idle title returns act = 1 with a demo
     * number; that "game" starts from the demo lump's own header and
     * runs through the same D_DoomLoop with demoplayback input feeding
     * (saturn_demo.c), then lands back on the title. */
    /* BOOT-WEDGE GATE (2026-09-09, PERMANENT): main.c jo_main set bit 1
     * of 0x060FFC1B right after jo_core_init to skip SGL's VBlank-OUT
     * ISR SCU-DMA section (its spin at 0x60510EA on the never-armed
     * DMA-end flag 0x060FFCBC wedged boot1/2/3.yss at PC=0x060510EA).
     * This used to clear the bit at menu-up "to restore stock SGL
     * behavior" — boot4.yss proved that wrong: with the gate cleared
     * the stalled section re-engages and nests ~60 ISR levels deep
     * (ch0 DMA never completes its last 0x8FC words, flag never sets,
     * every vblank adds a frame). We own VDP1 with our own command
     * pipeline; SGL's sort-table DMA is never armed by us and nothing
     * in gameplay needs it. The gate stays SET for the process lifetime. */

    for (;;)
    {
        extern int Saturn_Menu_Run(int *out_episode, int *out_skill,
                                   int *out_demo);
        extern int Saturn_Demo_Open(int num);
        extern void Saturn_Demo_Begin(void);
        extern void Saturn_Demo_Close(void);
        extern volatile int saturn_demo_active;

        int act       = 0;
        int demo_num  = 0;

#if defined(SATURN_UI_MENU) && SATURN_UI_MENU == 1
        act = Saturn_Menu_Run(&saturn_episode, &saturn_skill, &demo_num);
#endif
        /* UI-OFF BISECT: fall straight through with the defaults
         * (episode 1, skill 2) — the r104-era boot path. */

        /* TEST RUN (2026-09-06) REMOVED 2026-09-09 (operator: "the game
         * should load to the title screen no matter what we do with the
         * level renderer; only engage that code after a request to load
         * a level") — it force-started E1M1 on the menu's idle return,
         * bypassing the episode/skill UI and engaging the level renderer
         * with no user request. Boot now always lands on the title. */

        saturn_quit_to_title = 0;
        gameaction = ga_nothing;
        if (saturn_game_started)
            Z_FreeTags(mainzone);   /* free the previous level */

        if (act == 1)
        {
            /* FULL-VDP1 CAMPAIGN GATE (2026-09-09, operator: the level
             * renderer engages only on an explicit load request): the
             * menu's ~10 s idle-attract return lands HERE — it renders
             * a level through the same seg walk being debugged, so an
             * idle title now STAYS on the title. Restore the demo path
             * (delete this gate) for ship. */
            continue;

            /* attract demo: start from the demo lump's own skill/
             * episode/map, run the loop with demoplayback input, and
             * land back on the title when the data ends or the pad
             * aborts. An open failure (lump vanished) returns to the
             * title rather than starting a surprise game. */
            if (!Saturn_Demo_Open(demo_num))
                continue;
            {
                extern void ST_InitEveryLevel(void);

                ST_InitEveryLevel();    /* zero the stbar face/tryopen state */
            }
            Saturn_Demo_Begin();
            saturn_game_started = 1;    /* recorder consume gate (i_video.c) */
            saturn_demo_active  = 1;
            D_DoomLoop();
            saturn_game_started = 0;
            Saturn_Demo_Close();
            Z_FreeTags(mainzone);       /* free the demo level */
            continue;
        }

        {
            extern void ST_InitEveryLevel(void);

            ST_InitEveryLevel();    /* zero the stbar face/tryopen state */
        }

#if SATURN_BISECT == 1
        saturn_bisect_park(1);   /* r89: G_InitNew never runs */
#endif
        {
            /* Fresh game from the title: pistol start (saturn_new_game
             * gates the P_ResetResp inside Saturn_G_InitNew). Level
             * ADVANCES reach Saturn_G_InitNew without touching the
             * flag, so they keep the loadout G_PlayerFinishLevel
             * saved at the exit latch. */
            extern int saturn_new_game;

            saturn_new_game = 1;
        }
#ifdef SATURN_WIN32
        printf("D_DoomMain: Calling Saturn_G_InitNew(skill=%d, ep=%d, map=%d)\n", saturn_skill, saturn_episode, saturn_map); fflush(stdout);
#endif
        Saturn_G_InitNew(saturn_skill, saturn_episode, saturn_map);
#ifdef SATURN_WIN32
        printf("D_DoomMain: Saturn_G_InitNew returned. Entering D_DoomLoop\n"); fflush(stdout);
#endif
#if SATURN_BISECT == 2 || SATURN_BISECT == 3
        saturn_bisect_park(SATURN_BISECT);   /* r89: park before IN-LOOP */
#endif

        /* Stage marker: level setup (P_SetupLevel) completed. DARK CYAN +
         * IN-LOOP (distinct from the pre-white boot CYAN and from the
         * STARTING cyan above): P_SetupLevel returned and D_DoomLoop is
         * entered next. From here the DOOM frame should cover the screen —
         * persistent DARK CYAN means the hang is inside D_DoomLoop (first
         * background cart fill / first render / I_FinishUpdate). See the
         * P_SetupLevel sub-stage markers in d32xr/p_setup.c for the colors
         * between CYAN and here. */
        /* IN-LOOP FLASH (2026-09-03 real-HW bring-up): the console froze on
         * a static dark cyan that was ambiguous with the ~1.7 s report-window
         * marker of the same colour. Flash dark-cyan/black for ~2.7 s first
         * (each StageColor paints holds ~10 vblanks, so alternation is
         * automatic) — a photo-provable "level setup completed, D_DoomLoop
         * is next". After the flash the final static DARK CYAN means the
         * hang is inside D_DoomLoop's first frame; red on top of it = the
         * slave-join watchdog in saturn_render.c (slave never finished).
         * r105 UI: FIRST START ONLY — re-plays of the menu would each pay
         * the 2.7 s flash for no diagnostic value. */
        saturn_level_done = 0;
        saturn_game_started = 1;   /* recorder consume gate (i_video.c) */

        Saturn_PaintNow(JO_COLOR_DarkBlue);

        /* Drop into the main loop. Returns on quit-to-title OR on
         * level completion (ga_completed / ga_secretexit, latched into
         * saturn_level_done). The chain below runs the intermission,
         * frees the finished level and inits the next map, keeping the
         * same D_DoomLoop instance running for each new level. */
        for (;;)
        {
            int next;

            D_DoomLoop();

            if (!saturn_level_done)
                break;              /* quit-to-title (or demo end) */
            saturn_level_done = 0;

            /* Intermission: stats for the finished level, then the
             * next map (0 = episode over, back to the title). The
             * stage letters (StageTextHi, the freeze-forensics
             * protocol) name the last step reached on a wedged
             * screen: C = before the intermission, F = it returned,
             * N = next-level init returned. (No 'Q' — the stage
             * glyph table has no Q; it painted as a blank cell and
             * cost a round of forensics.) */
            {
                extern int Saturn_Inter_Run(int episode, int map,
                                            int secretexit);
                extern void Saturn_StageTextHi(int x, int y,
                                               const char *s);
                char srow[2];

                srow[0] = 'C'; srow[1] = 0;
                Saturn_StageTextHi(188, 186, srow);

                next = Saturn_Inter_Run(saturn_episode, saturn_map,
                                        saturn_secret_exit);

                srow[0] = 'F'; srow[1] = 0;
                Saturn_StageTextHi(268, 186, srow);
            }

            Z_FreeTags(mainzone);   /* free the finished level */

            if (next == 0)
                break;              /* episode finished -> title menu */

            saturn_map = next;
            {
                extern void ST_InitEveryLevel(void);

                ST_InitEveryLevel();    /* zero the stbar face/tryopen */
            }
            Saturn_G_InitNew(saturn_skill, saturn_episode, saturn_map);
            {
                extern void Saturn_StageTextHi(int x, int y,
                                               const char *s);
                char srow[2];

                srow[0] = 'N'; srow[1] = 0;
                Saturn_StageTextHi(288, 186, srow);
            }
        }

        saturn_game_started = 0;
        Z_FreeTags(mainzone);      /* free THIS level before the menu */
    }
}

/* Saturn-driven main loop. d32xr's loop is a MiniLoop that uses
 * `gamevbls / TICVBLS > gametic` to decide when to advance a tic. We
 * replicate that here with a global saturn_vbl_count incremented by
 * the VBlank callback registered in I_InitVideo.
 *
 * Each iteration:
 *   1. Read the gamepad -> players[0].ticbuttons.
 *   2. If we've consumed enough vblanks for a new tic, bump gametic
 *      and run Saturn_G_Ticker (which calls P_Ticker inside).
 *   3. Render one frame via D_Display (-> Saturn_R_RenderPlayerView
 *      -> I_FinishUpdate).
 *   4. Wait one VBlank so the rendering pace matches the Saturn's
 *      60 Hz display.
 */
/* Whole-displayed-frame probe (2026-09-02): FRT delta and vblank delta
 * across one D_DoomLoop iteration, printed as 'w' (FRT >>2) in the
 * i_video.c HUD and measurable against the J bar (frame vbl). With the
 * 'c' FRT calibration row this says whether the frame's vbl count and
 * its FRT time agree about wall time. */
volatile unsigned short saturn_prof_frame_frt = 0;
volatile int            saturn_prof_frame_vbl = 0;

/* Render window by FRT (2026-09-02): Saturn_G_Display entry to exit,
 * printed as 'D' (>>2) in the i_video.c HUD. D was the last major
 * window still on the vbl clock (rounds to 17-50 ms for a 2-3 read).
 * The 16-bit FRT wraps every 65536 ticks = 3.04 vbl, which a 3-vbl
 * render can exceed — so D_Display reconstructs the 32-bit span using
 * the calibrated 21536 ticks/vblank: wraps = round((vbl*21536 - frt)
 * / 65536), ticks = frt + wraps*65536. */
volatile unsigned int   saturn_prof_render_frt = 0;

/* I_FinishUpdate window by raw FRT ticks (2026-09-02): entry→exit of
 * the finish phase (expand, VDP1 DMA, slSynch). Printed 'i' (>>2).
 * The old saturn_prof_fin_us micro probe read a stuck 3 — replaced. */
volatile unsigned int   saturn_prof_finish_frt = 0;

/* Hitch latches (2026-09-02): w's peak-hold caught ~63-vbl (~1 s)
 * frames in steady play — rare, but one costs ~15 normal frames. The
 * per-frame rows (D/F/K/A) are overwritten by the next fast frame
 * before a screenshot can catch a hitch, so when a frame exceeds 10
 * vbl the end-of-loop capture latches its breakdown into rows that
 * only update on the NEXT hitch:
 *   saturn_prof_hitch_fv = total frame vbl          (HUD 'h')
 *   saturn_prof_hitch_r  = D_Display render vbl     (HUD 'H')
 *   saturn_prof_hitch_t  = tic-window vbl           (HUD 'q')
 *   saturn_prof_hitch_f  = cart-fill vbl            (HUD 'p')
 *   saturn_prof_hitch_c  = synchronous CD cold reads (HUD 'C')
 * h ~= H + q (+p) means the windows account for the hitch; a gap means
 * pacing/wait outside them. */
volatile int saturn_prof_hitch_fv = 0;
volatile int saturn_prof_hitch_r  = 0;
volatile int saturn_prof_hitch_t  = 0;
volatile int saturn_prof_hitch_f  = 0;
volatile int saturn_prof_hitch_c  = 0;

#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 1)
/* OCRAM divide LUT install — back in the boot path (2026-09-08): the
 * lazy in-game window wedged real HW once (FULL burn 1), while boot is
 * the rig-proven CCR window (20+ real-HW boots at 0xFFFFFE92; the
 * "CCR wedges" verdict came from the unmapped 0xFFFFFF92). The
 * fill+verify below is the ABI-fixed, 32-bit-only form proven across
 * the STEP ladder. Called from D_DoomMain right after the DIVU probe,
 * before the cyan STARTING stage. */
void Saturn_Divlut_Install(void)
{
    unsigned int d, bad;
#ifdef SATURN_DIVLUT_NOOCRAM
    volatile unsigned int *const oclut = sat_divlut_table;
#else
    volatile unsigned char *const ccr = &SATURN_SH2_CCR;
    volatile unsigned int  *const oclut =
        (volatile unsigned int *)0xC0000000u;

    *ccr = 0x00u;                    /* CE off: stop the cache   */
    *ccr = 0x10u;                    /* CP = 1: full purge       */
    *ccr = 0x09u;                    /* 2KB cache + 2KB OCRAM    */
#endif

    /* Verify is now PURE 32-bit arithmetic (2026-09-08): the old check
     * compared (unsigned long long)r*d against 0x100000000ull, and that
     * 64-bit compare FAILED at d=4 on real codegen (r = 0x40000000, hi =
     * exactly 2^32) while the shape-identical d=2 entry passed — the
     * gcc sh-elf 64-bit compare at the exact 2^32 boundary is not
     * trustworthy, so no 64-bit math survives in this function. The
     * exact-value check is strictly stronger anyway: it pins the full
     * ceil(2^32/d) sequence, not just the bounds. d=2,3 use pinned
     * constants because __udivsi3 CLAMPS quotients >= 2^30 (d <= 3);
     * d >= 4 recomputes through the exact shift-subtract path.
     * MARKERS: oclut[0]/[1] are never consumed (divisors < 2 never
     * reach the LUT), so they carry sentinel values — where 0x11111111
     * lands in a memory dump settles the oclut base pointer in one
     * boot (the crash11.yss table content read one slot low, which no
     * uniform file->VA mapping could explain). */
    bad = 0;
    saturn_divlut_badd = 0;
    saturn_divlut_badr = 0;
    oclut[0] = 0x12341234u;                    /* sentinel: mid-install */
    oclut[1] = 0x56785678u;                    /* sentinel: mid-install */
    oclut[2] = 0x80000000u;                    /* ceil(2^32/2) = 2^31  */
    oclut[3] = 0x55555556u;                    /* ceil(2^32/3)         */
    for (d = 4; d < 512u; d++)
        oclut[d] = 0xFFFFFFFFu / d + 1u;       /* ceil(2^32/d)         */
    for (d = 2; d < 512u; d++)
    {
        unsigned int r = oclut[d];
        unsigned int q2;

        if (d == 2u)
            q2 = 0x80000000u;
        else if (d == 3u)
            q2 = 0x55555556u;
        else
            q2 = 0xFFFFFFFFu / d + 1u;         /* d >= 4: exact        */

        if (r != q2)
        {
            bad = 1;
            saturn_divlut_badd = (int)d;             /* failing index      */
            saturn_divlut_badr = (int)r;             /* what we read back  */
            saturn_divlut_badsub = 1;                /* data mismatch      */
            break;
        }
    }
#ifndef SATURN_DIVLUT_NOOCRAM
    if (!bad && (*ccr & 0x09u) != 0x09u)
        saturn_divlut_err = 1;           /* CCR readback */
    else if (bad)
#else
    if (bad)
#endif
    {
        saturn_divlut_err = 2;           /* LUT data verify */
        oclut[0] = 0xEEEEEEEEu;          /* table latch: verify FAILED */
    }
    else
    {
        saturn_divlut_ok = 1;
        saturn_divlut_on = 1;
        oclut[0] = 0x11111111u;          /* table latch: installed OK  */
        oclut[1] = 0x22222222u;          /* table latch: LUT ON        */
    }
}
#endif

void D_DoomLoop(void)
{
    static int dbg_iter = 0;      /* first-frames stage-marker budget */
    extern volatile unsigned int saturn_vbl_count;
    extern int gametic;
    extern void Saturn_G_Ticker(void);
    extern volatile int saturn_prof_tics;
    extern volatile int saturn_prof_comp;
    extern volatile int saturn_prof_cd;
    extern volatile int saturn_prof_fill;
    extern volatile int saturn_prof_ticvbl;
    extern void saturn_iwad_fill_pump(void);   /* i_wad.c: async fill pump */
    extern volatile int saturn_prof_render;   /* D_Display render vbl */
    extern int          saturn_tex_comp_count;   /* saturn_tex.c */
    extern int          saturn_wad_cold_reads;   /* i_wad.c      */
#ifdef SATURN_WIN32
    printf("D_DoomLoop: Entered\n"); fflush(stdout);
#endif
    for (;;)
    {
#ifdef SATURN_WIN32
        static unsigned int s_win32_start_time = 0;
        if (!s_win32_start_time) s_win32_start_time = timeGetTime();
        saturn_vbl_count = (timeGetTime() - s_win32_start_time) * 60 / 1000;
#endif
        /* r105 UI: the pause plaque's "TO QUIT" — return to D_DoomMain,
         * which frees the level and re-runs the title menu. */
        if (saturn_quit_to_title)
            return;

        /* LUT install lives in D_DoomMain again (2026-09-08, operator
         * directive): the lazy in-game CCR window is the one real-HW
         * wedge (burn 1), while boot-time CCR at the CORRECT address
         * 0xFFFFFE92 is the rig's 20+ real-HW-boot-proven window. The
         * boot-site kronos wedges of the ladder era were the divide-ABI
         * bug, since fixed. The .bss ok/on shadow-per-frame dance is
         * gone with it — the BSS stomper was solved (divide-ABI), so
         * the install's own ok=1/on=1 stores persist. */

        /* Level completion: Saturn_G_Ticker latches the exit-switch
         * special (ga_completed / ga_secretexit) into saturn_level_done
         * the instant its tic returns — P_Ticker clears gameaction at
         * the start of every tic (p_tick.c:338), so reading gameaction
         * here loses the exit on any two-tic catch-up frame. Hand
         * control back to D_DoomMain's level chain (intermission ->
         * next map). */
        /* Freeze-forensics: R = a loop top was reached. Painted on the
         * clean y=186 band every iteration. */
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char rrow[2];

            rrow[0] = 'R'; rrow[1] = 0;
            Saturn_StageTextHi(28, 186, rrow);
        }
        if (saturn_level_done)
        {
            gameaction = ga_nothing;
            return;
        }

        /* Check for pending level warp request (e.g. from Windows UI menu or cheat) */
        if (saturn_warp_map > 0)
        {
            extern void Saturn_WarpToLevel(int ep, int map);
            int wep = saturn_warp_episode > 0 ? saturn_warp_episode : 1;
            int wmap = saturn_warp_map;
            saturn_warp_episode = 0;
            saturn_warp_map = 0;
            Saturn_WarpToLevel(wep, wmap);
            continue;
        }

        extern volatile unsigned int saturn_master_beat;
        extern volatile unsigned int saturn_iter_count;
        extern volatile unsigned int saturn_tic_count;
        extern volatile unsigned int saturn_sim_site;

        unsigned int    frame_v0 = saturn_vbl_count;
        unsigned short  frame_f0 = SAT_FRT();

        saturn_master_beat++;   /* round-50: main-loop heartbeat */
        saturn_iter_count++;    /* r56: census row H */
        saturn_sim_site = 0x1;  /* r56: loop top */
        /* r58: paint the ISR-latched verdicts from the MAIN thread. The
         * ISR only latches now (its 2x64KB stage fills were the heavy
         * interrupt-path work — r56 froze with the master wedged in the
         * SCU IRQ-entry glue); while the latch holds, every iteration
         * repaints fill + rows + SCU IST/IMASK here. */
        {
            extern volatile unsigned int saturn_isr_dirty;
            extern void saturn_isr_paint(void);
            if (saturn_isr_dirty)
                saturn_isr_paint();
        }
        /* r104 DSP-eval: the X/V cart-verify rows, painted EVERY
         * iteration on the MAIN thread. r102 put them in the ISR's
         * once-per-64-vbl census, but each frame's copy_back erases
         * the glyphs, so they were on screen 1 frame in 64 — r103
         * steady-state captures never showed them, and a real-HW
         * photo would almost always miss them too (the eval dies
         * blind). Painted here, immediately after the previous
         * frame's copy_back (I_FinishUpdate), they persist exactly
         * like the S/V/L/T/E/A/H rows. Respects the one-shot freeze
         * verdict's stand-down. Zero-padded fixed width per r83.
         *   X = 2 KB chunks whose full readback verify FAILED
         *       (each CPU-repaired; healthy = 0000)
         *   V = KB fully verified so far (climbs to the fill total
         *       ~511 KB, then holds) */
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            extern volatile unsigned int saturn_dsp_verify_kb;
            extern volatile unsigned int saturn_dsp_verify_bad;
            extern volatile unsigned int saturn_rv_done;
            static char xrow[12];

            if (saturn_rv_done == 0u)
            {
                /* r104: StageTextHi (PLAYPAL[4] white, no CRAM write —
                 * the r101 lesson) — black glyphs vanish on the dark
                 * maps; a real-HW photo must read these on ANY scene. */
                /* 2026-09-06 SW1COMP face probe (replaces the dormant
                 * DSP-verify X row — engine demoted r96, row read 0000
                 * every boot): P = switch-face counters from
                 * saturn_segs.c — P(built 3) ok(1) paint(3):
                 *   built = walls built this frame carrying SW1COMP as
                 *           the lower texture (master BSP walk)
                 *   ok    = preload Saturn_TexData handed back pixels
                 *   paint = master-half AC_BOTTOMTEXTURE paint columns */
                {
                    extern int saturn_sw1_built;
                    extern int saturn_sw1_ok;
                    extern int saturn_sw1_paint;

                    xrow[0] = 'P';
                    Saturn_Udec0(xrow + 1,
                                 (unsigned)(saturn_sw1_built & 0x7Fu), 3);
                    xrow[4] = (char)('0' + (saturn_sw1_ok ? 1 : 0));
                    Saturn_Udec0(xrow + 5,
                                 (unsigned)(saturn_sw1_paint & 0xFFu), 3);
                    xrow[8] = '\0';
                }
                Saturn_StageTextHi(8, 148, xrow);
                /* r104's "V = DSP-verify" row is RETIRED here (engine
                 * demoted r96, read ~0000 every boot). The cell now
                 * carries the OCRAM probe verdict + A/B metric
                 * (2026-09-06):
                 *   O#c A#B# R###
                 *     c  = CCR byte before the probe write (hex)
                 *     A/B = the two 2KB data-array word verifies (1=ok)
                 *     R  = render-window vbl count (saturn_prof_render)
                 * Painted HERE because (8,168) is the last proven-
                 * readable row above the status bar; the original
                 * (0,178) site sat 8-10 px under this row and 8 px
                 * above the bar counters — with ~20 px glyphs all
                 * three interleaved into one union block (seen in the
                 * 21:49 Kronos captures: "R W I D E B Y E"). */
                {
                    extern volatile int saturn_prof_render;

                    xrow[0] = 'O';
#if defined(SATURN_OCRAM_PROBE)
                    {
                        extern volatile int saturn_oc_ok;
                        extern volatile int saturn_oc_ccr0;

                        Saturn_Udec0(xrow + 1,
                                     (unsigned)saturn_oc_ccr0 & 0xFu, 1);
                        xrow[2] = (char)('0' + (saturn_oc_ok ? 1 : 0));
                    }
#else
                    xrow[1] = '-';
                    xrow[2] = '-';
#endif
                    xrow[3] = 'R';
                    Saturn_Udec0(xrow + 4,
                                 (unsigned)saturn_prof_render & 0xFFFu, 3);
                    xrow[7] = '\0';
                }
#ifndef SATURN_VDP1_WALL
                /* VDP1 wall prototype (2026-09-06) owns this row: its
                 * V1 telemetry paints at (8,168) and would union with
                 * the O row here (both x=8, rows 10 px apart). */
                Saturn_StageTextHi(8, 168, xrow);
#endif
                /* v5.13's W = staging-tripwire row (saturn_stage_trip)
                 * is RETIRED from the screen: it painted at (8,208),
                 * which the 2026-09-06 full-screen layout gave to the
                 * status bar (fb rows 192-223) — its white glyphs
                 * stomped the AMMO/HEALTH/FRAG label row every frame.
                 * The tripwire itself still works in i_wad.c (each
                 * mismatch permanently demotes the stage views); only
                 * the on-screen counter is gone. Healthy was 0000
                 * every boot. */
            }
        }
#ifdef SATURN_SPRCLIP_DIAG
        /* Round-33b sprite-clip probe: republish the seg_draw_sprites
         * capture rows HERE (after the flip) — StageTextHi writes the
         * VDP2 display pages directly, so glyphs painted from inside
         * the render are erased by the next I_FinishUpdate copy_back.
         * Decode (see saturn_segs.c sprdiag block):
         *   M/S J999 Y999 B999 — master/slave half: clipping wall index,
         *     clip row, unclipped base row (999 = none/invalid)
         *   F99999 N999        — clipping wall's floornew (16.16 hi
         *     word), walls passing the depth test
         *   G99999 D99999      — sprite gzt-viewz hi word, dist hi word
         * Also: a white 1-px marker line at B-1 across the watched
         * sprite's columns, painted by the render itself. */
        {
            extern volatile int sd_j[2], sd_yy[2], sd_base[2], sd_dist[2];
            extern volatile int sd_n[2], sd_fl[2], sd_gz[2];
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            extern void Saturn_Udec0(char *out, unsigned int v, int width);
            /* 2026-09-06 EXIT-FREEZE ROOT CAUSE: was char drow[16], but
             * the vanish-hunt row writes drow[0..21] + NUL at drow[22]
             * (Udec0(drow+17, width 5) = the S/X fields), and the T/A/Z
             * rows NUL at drow[16] — 7 bytes of census ASCII sprayed
             * past the buffer into D_DoomLoop's own frame EVERY frame.
             * Harmless while the victims are rewritten locals; fatal on
             * the exit iteration, when the chain consumed a pointer slot
             * adjacent to the buffer holding the row's own X-field ASCII
             * (PC = 0x58303231 = "X021" — decoded from the frozen .yss).
             * 24 = 22-char row + NUL + margin. */
            char drow[24];

            /* compact rows (13 cells = 260 fb px) so the last field
             * clears the right edge of a full-width capture. */
            drow[0] = 'M'; drow[1] = 'J';
            Saturn_Udec0(drow + 2,
                         sd_j[0] < 0 ? 999u : (unsigned)sd_j[0], 3);
            drow[5] = 'Y';
            Saturn_Udec0(drow + 6,
                         sd_yy[0] == -2 ? 998u :
                         sd_yy[0] < 0 ? 999u : (unsigned)sd_yy[0], 3);
            drow[9] = 'B';
            Saturn_Udec0(drow + 10,
                         sd_base[0] < 0 ? 999u : (unsigned)sd_base[0], 3);
            drow[13] = '\0';
            Saturn_StageTextHi(0, 8, drow);

            drow[0] = 'S'; drow[1] = 'J';
            Saturn_Udec0(drow + 2,
                         sd_j[1] < 0 ? 999u : (unsigned)sd_j[1], 3);
            drow[5] = 'Y';
            Saturn_Udec0(drow + 6,
                         sd_yy[1] == -2 ? 998u :
                         sd_yy[1] < 0 ? 999u : (unsigned)sd_yy[1], 3);
            drow[9] = 'B';
            Saturn_Udec0(drow + 10,
                         sd_base[1] < 0 ? 999u : (unsigned)sd_base[1], 3);
            drow[13] = '\0';
            Saturn_StageTextHi(0, 28, drow);

            /* round-34b: when watch 3 caught a FULLY-ERASED sprite this
             * frame, the F/G slots (always inside the capture window's
             * visible band, unlike the y138 row) print the eraser
             * identity instead: y48 = e/J###C#Y### (segwalls index,
             * clip class, clip row), y68 = S#####X#### (occluder scale
             * vs sprite yscale, both >>6). eok[0] wins over eok[1]. */
            {
                extern volatile int sd_eok[2];
                int e0 = *(volatile int *)(0x20000000u | (unsigned)&sd_eok[0]);
                int e1 = *(volatile int *)(0x20000000u | (unsigned)&sd_eok[1]);

                if (e0 || e1)
                {
                    int w = e0 ? 0 : 1;
                    extern volatile int sd_j[2], sd_yy[2];
                    extern volatile int sd_sc[2], sd_ys[2], sd_cl[2];
                    int j_ = *(volatile int *)
                             (0x20000000u | (unsigned)&sd_j[w]);
                    int y_ = *(volatile int *)
                             (0x20000000u | (unsigned)&sd_yy[w]);
                    int c_ = *(volatile int *)
                             (0x20000000u | (unsigned)&sd_cl[w]);
                    int s_ = *(volatile int *)
                             (0x20000000u | (unsigned)&sd_sc[w]);
                    int x_ = *(volatile int *)
                             (0x20000000u | (unsigned)&sd_ys[w]);

                    drow[0] = 'e';
                    Saturn_Udec0(drow + 1,
                                 j_ < 0 ? 999u : (unsigned)j_, 3);
                    drow[4] = 'C';
                    Saturn_Udec0(drow + 5, (unsigned)c_, 1);
                    drow[6] = 'Y';
                    Saturn_Udec0(drow + 7,
                                 y_ == -2 ? 998u :
                                 y_ < 0 ? 999u : (unsigned)y_, 3);
                    drow[10] = '\0';
                    Saturn_StageTextHi(0, 48, drow);

                    drow[0] = 'S';
                    Saturn_Udec0(drow + 1,
                                 s_ < 0 ? 0u : ((unsigned)s_ >> 6), 5);
                    drow[6] = 'X';
                    Saturn_Udec0(drow + 7,
                                 x_ < 0 ? 0u : ((unsigned)x_ >> 6), 5);
                    drow[12] = '\0';
                    Saturn_StageTextHi(0, 68, drow);
                }
                else
                {
                    drow[0] = 'F';
                    Saturn_Udec0(drow + 1,
                                 (unsigned)(sd_fl[0] >> 16) & 0xFFFFu, 5);
                    drow[6] = 'N';
                    Saturn_Udec0(drow + 7, (unsigned)sd_n[0], 3);
                    drow[10] = '\0';
                    Saturn_StageTextHi(0, 48, drow);

                    drow[0] = 'G';
                    Saturn_Udec0(drow + 1,
                                 (unsigned)(sd_gz[0] >> 16) & 0xFFFFu, 5);
                    drow[6] = 'D';
                    Saturn_Udec0(drow + 7,
                                 sd_dist[0] < 0 ? 0u
                                                : (unsigned)sd_dist[0], 5);
                    drow[12] = '\0';
                    Saturn_StageTextHi(0, 68, drow);
                }
            }

            /* round-33f CACHE VITALS of the watched (most-erased)
             * sprite — decides WHERE the bottom texels went:
             *   K### = all-zero bottom rows of its converted cache
             *     (healthy POSS = 0; K>0 = cache truncated/clobbered
             *     or sv->h inflated)
             *   G### = its columns whose post chain died on the pend
             *     guard instead of the 0xFF terminator (G=w = plen
             *     short or a short lump read = bottom posts dropped)
             *   P#### = the lump length the conversion bounded with
             *     (POSSA1 = 1392; a smaller P = directory lie) */
            {
                /* round-33g rows — watched sprite per HALF: the largest
                 * base-on-screen sprite of that half (= the floater when
                 * one is visible). cpu0 = left half, cpu1 = right half
                 * (the E1M1 floaters span fbcols 171-271 = cpu1). cpu1's
                 * writes cross shared WRAM, so they are read UNCACHED
                 * (the master's cached line never invalidates; the purge
                 * is a no-op); cpu0's are plain cached reads.
                 *   T### = projected patch TOP row (row0f)
                 *   B### = projected base row-1 (white bar row)
                 *   A### = highest fb row its paint WROTE (999 = none)
                 *   Z### = lowest fb row its paint wrote (999 = none)
                 *   A==T && Z==B-1 -> paint matches the projection;
                 *   A<T -> paint anchored ABOVE the projection;
                 *   Z<B-1 -> paint stopped early (cache truncation). */
                extern volatile int sd_top[2], sd_base[2];
                extern volatile int sd_hi[2], sd_lo[2];
                /* both halves' vitals are written UNCACHED by the sprite
                 * pass — read them the same way here */
                int t0 = *(volatile int *)(0x20000000u | (unsigned)&sd_top[0]);
                int b0 = *(volatile int *)(0x20000000u | (unsigned)&sd_base[0]);
                int a0 = *(volatile int *)(0x20000000u | (unsigned)&sd_hi[0]);
                int z0 = *(volatile int *)(0x20000000u | (unsigned)&sd_lo[0]);
                int t1 = *(volatile int *)(0x20000000u | (unsigned)&sd_top[1]);
                int b1 = *(volatile int *)(0x20000000u | (unsigned)&sd_base[1]);
                int a1 = *(volatile int *)(0x20000000u | (unsigned)&sd_hi[1]);
                int z1 = *(volatile int *)(0x20000000u | (unsigned)&sd_lo[1]);

                drow[0] = 'T';
                Saturn_Udec0(drow + 1, t0 < 0 ? 999u : (unsigned)t0, 3);
                drow[4] = 'B';
                Saturn_Udec0(drow + 5,
                             b0 < 2 ? 999u : (unsigned)(b0 - 1), 3);
                drow[8] = 'A';
                Saturn_Udec0(drow + 9, a0 < 0 ? 999u : (unsigned)a0, 3);
                drow[12] = 'Z';
                Saturn_Udec0(drow + 13, z0 < 0 ? 999u : (unsigned)z0, 3);
                drow[16] = '\0';
                Saturn_StageTextHi(0, 88, drow);

                drow[0] = 'U';      /* cpu1 twin of the T row */
                Saturn_Udec0(drow + 1, t1 < 0 ? 999u : (unsigned)t1, 3);
                drow[4] = 'B';
                Saturn_Udec0(drow + 5,
                             b1 < 2 ? 999u : (unsigned)(b1 - 1), 3);
                drow[8] = 'A';
                Saturn_Udec0(drow + 9, a1 < 0 ? 999u : (unsigned)a1, 3);
                drow[12] = 'Z';
                Saturn_Udec0(drow + 13, z1 < 0 ? 999u : (unsigned)z1, 3);
                drow[16] = '\0';
                Saturn_StageTextHi(0, 108, drow);
            }
            {
                extern volatile int sd_botz[2], sd_guard[2], sd_plen[2];
                int k_ = *(volatile int *)(0x20000000u | (unsigned)&sd_botz[1]);
                int g_ = *(volatile int *)(0x20000000u | (unsigned)&sd_guard[1]);
                int p_ = *(volatile int *)(0x20000000u | (unsigned)&sd_plen[0]);

                drow[0] = 'K';
                Saturn_Udec0(drow + 1, k_ < 0 ? 999u : (unsigned)k_, 3);
                drow[4] = 'G';
                Saturn_Udec0(drow + 5, g_ < 0 ? 999u : (unsigned)g_, 3);
                drow[8] = 'P';
                Saturn_Udec0(drow + 9, p_ < 0 ? 0u : (unsigned)p_, 4);
                drow[13] = '\0';
                Saturn_StageTextHi(0, 128, drow);
            }
            /* vanish hunt row: the T-row watch's sprite and the last
             * wall that clipped its centre column.
             *   J### = segwalls[] index (999 = none clipped centre)
             *   C#   = clip class 1 SOLID/MID 2 TOPSIL 3 BOTTOMSIL
             *   Y### = clip row (998 = solid full block, 999 = none)
             *   S##### = occluder's scale at the centre column >>6
             *   X##### = the sprite's own yscale >>6
             * S < X with a full erase = the wall erased a thing it is
             * farther than (over-occlusion). */
            {
                extern volatile int sd_j[2], sd_yy[2];
                extern volatile int sd_sc[2], sd_ys[2], sd_cl[2];
                extern volatile int sd_eok[2];
                int j_ = *(volatile int *)(0x20000000u | (unsigned)&sd_j[0]);
                int y_ = *(volatile int *)(0x20000000u | (unsigned)&sd_yy[0]);
                int c_ = *(volatile int *)(0x20000000u | (unsigned)&sd_cl[0]);
                int s_ = *(volatile int *)(0x20000000u | (unsigned)&sd_sc[0]);
                int x_ = *(volatile int *)(0x20000000u | (unsigned)&sd_ys[0]);
                int e_ = *(volatile int *)(0x20000000u | (unsigned)&sd_eok[0]);

                /* 'e' prefix = watch 3 (a FULLY-ERASED sprite owns the
                 * row); 'J' prefix = watch 2 (biggest live sprite). */
                drow[0] = e_ ? 'e' : 'J';
                Saturn_Udec0(drow + 1, j_ < 0 ? 999u : (unsigned)j_, 3);
                drow[4] = 'C';
                Saturn_Udec0(drow + 5, (unsigned)c_, 1);
                drow[6] = 'Y';
                Saturn_Udec0(drow + 7,
                             y_ == -2 ? 998u :
                             y_ < 0 ? 999u : (unsigned)y_, 3);
                drow[10] = 'S';
                Saturn_Udec0(drow + 11, s_ < 0 ? 0u : ((unsigned)s_ >> 6), 5);
                drow[16] = 'X';
                Saturn_Udec0(drow + 17, x_ < 0 ? 0u : ((unsigned)x_ >> 6), 5);
                drow[22] = '\0';
                Saturn_StageTextHi(0, 138, drow);  /* 148/160/168 taken */
            }
            /* The O row (OCRAM verdict + R metric) lives at (8,168) in
             * the SPRCLIP republish block above — the retired V-row
             * cell. (0,178) unioned with that row and the bar counters
             * into one unreadable glyph block. */
        }
#endif
        /* Drain one background cart-fill entry per frame. MUST run on
         * the main thread: each entry does GFS work on the IWAD handle,
         * which is neither reentrant nor interrupt-safe (it used to be
         * drained from the VBlank ISR — that froze the boot mid-WAD-load;
         * see the comment on saturn_vblank_callback in i_video.c). */
        {
            unsigned int   f0;
            unsigned short ft;
            int            n;

            do {
                f0 = saturn_vbl_count;
                ft = SAT_FRT();
            } while (__builtin_expect(saturn_vbl_count != f0, 0));

            saturn_sim_site = 0x2;   /* r56: fill drain entered */
#if SATURN_BISECT == 4
            n = 0;   /* r89 B4: background cart-fill DISABLED */
#else
            n = saturn_iwad_background_fill();
#endif
            saturn_sim_site = 0x3;   /* r56: fill drain returned */
            saturn_prof_fill = (int)(saturn_vbl_count - f0);
            saturn_frt_fill  = (int)(Saturn_ProfSpan(ft, f0) >> 8);
            /* saturn_pf_acc[5] is now dedicated to flat quad render timing (tag 'F') */
            /* Peak-hold with slow decay — the HUD must see a single
             * budget-overflow frame even though the bar decays. */
            if (n > saturn_prof_fllen)
                saturn_prof_fllen = n;
            else if (saturn_prof_fllen > 0)
                saturn_prof_fllen--;
        }
        {
            /* Phase-4 profile: per-displayed-frame counters (profiler
             * rows in the i_video.c HUD). saturn_prof_fill / _ticvbl
             * are the two windows the earlier rows never timed: the
             * background cart-fill (GFS on the CD handle) and the tic
             * catch-up loop's WALL duration (T on the HUD is only the
             * tic count). */
            int          tics_done = 0;
            int          prof_c0 = saturn_tex_comp_count;
            int          prof_r0 = saturn_wad_cold_reads;
            unsigned int prof_w0 = saturn_vbl_count;
            int          pad_buttons = 0;
            int          demo_feed = 0;

            /* Display cap (2026-09-01 user request: "limit fps to a
             * maximum of 30"). slSynch in I_FinishUpdate alone paces
             * the loop to <=1 frame/vblank, so a fast scene showed each
             * 30 Hz sim position twice with uneven cadence. Hold every
             * displayed frame to a full TICVBLS window: tics then map
             * 1:1 onto displayed frames. Slow frames (render > 2 vbl,
             * today's 17) pay nothing extra — this only bites scenes
             * that could render at 60. Static start-stamp, not
             * end-stamp: the previous frame's render time already
             * satisfied part of the window. */
            {
                static unsigned int last_display_vbl;
                unsigned short      pf_cf0;
                unsigned int        pf_cv0;

                do {
                    pf_cv0 = saturn_vbl_count;
                    pf_cf0 = SAT_FRT();
                } while (__builtin_expect(saturn_vbl_count != pf_cv0, 0));

                saturn_sim_site = 0x4;   /* r56: display-cap wait */
                while ((int)(saturn_vbl_count - last_display_vbl) < 1)
                {
#ifdef SATURN_WIN32
                    Sleep(1);
                    saturn_vbl_count = (timeGetTime() - s_win32_start_time) * 60 / 1000;
#endif
                    /* r57 ISR-death detector — r83: the FRT-elapsed
                     * trigger is gone (same real-HW wrap bug as the r72
                     * staging detector: TCNT is per-vblank-reset on real
                     * HW, so a vblank landing between two samples wraps
                     * the 16-bit delta to ~65491 > 64608 with vbl_count
                     * frozen = false verdict on a healthy machine).
                     * Replacement, FRT-honest: count TCNT reset
                     * boundaries (now < last = one hardware vblank
                     * crossed). Eight consecutive boundaries on one
                     * frozen vbl count = the ISR is dead — a live ISR
                     * lags a masked vblank by microseconds, it never
                     * drops eight whole frames. Paint the verdict
                     * straight from the main thread (the next commit +
                     * flip this same iteration exposes it) and skip the
                     * cap. */
                    static unsigned short cap_frt_last = 0;
                    static unsigned int   cap_frt_miss = 0;
                    static unsigned int   cap_frt_v    = 0;
                    unsigned short        cap_frt_now  = SAT_FRT();

                    /* Idle dead time: pump the async cart-fill batch so
                     * the CD stream keeps moving while the loop waits
                     * for the next tic boundary (i_wad.c, FILL_NW_BYTES). */
                    saturn_iwad_fill_pump();
#ifndef SATURN_WIN32
                    if (cap_frt_now < cap_frt_last)
                    {
                        if (saturn_vbl_count == cap_frt_v)
                        {
                            if (++cap_frt_miss >= 8u)
                            {
                                extern volatile unsigned int saturn_vbl_dead;
                                extern void saturn_vbl_dead_verdict(void);

                                saturn_vbl_dead = 1;
                                saturn_sim_site = 0x10;   /* 16 = ISR dead */
                                saturn_vbl_dead_verdict();
                                break;
                            }
                        }
                        else
                        {
                            cap_frt_miss = 0;
                            cap_frt_v    = saturn_vbl_count;
                        }
                    }
#endif
                    cap_frt_last = cap_frt_now;
                }
                last_display_vbl = saturn_vbl_count;
                saturn_pf_acc[4] += Saturn_ProfSpan(pf_cf0, pf_cv0);
                saturn_sim_site = 0x5;   /* r56: display-cap wait done */
#ifdef SATURN_WIN32
                static int s_frame_logged = 0;
                if (++s_frame_logged <= 10 || s_frame_logged % 60 == 0) {
                    printf("D_DoomLoop: Frame %d, vbl=%u, gametic=%d\n", s_frame_logged, saturn_vbl_count, gametic);
                    fflush(stdout);
                }
#endif
            }

            /* Phase A handshake probe (NOP round trip), every 60 frames. */
            saturn_slave_frame_tick();

            /* r106 attract demos + PER-TIC PAD SAMPLE (2026-09-09):
             * demo mode only needs the frame's REAL pad word as the
             * abort signal (saturn_demo.c). REAL play must NOT sample
             * here — I_ReadControls rotates the ticbuttons edge pair
             * (i_system.c:831), so a once-per-frame call sampled at
             * FRAME rate on a slow renderer (taps pressed+released
             * between two frames never existed) AND left the pair
             * unrotated across a frame's catch-up tics, making every
             * edge action (Start pause, plaque confirm) fire on ALL of
             * them — a Start tap toggled pause twice = "input didn't
             * register". The sample moved INTO the tic loop below, so
             * edges rotate exactly once per tic and the freshest vblank
             * poll is consumed every tic. */
            demo_feed = demoplayback && saturn_demo_active;
            if (demo_feed)
                pad_buttons = I_ReadControls();
            /* Cap tic debt: without this the catch-up loop runs away
             * (frame slower than 4 vbl → more tics owed → slower frame).
             * Run at most N catch-up tics per displayed frame and DROP
             * the leftover debt — DOOM's game time slows gracefully on a
             * slow renderer instead of fast-forwarding.
             * P5 Phase 3 gate (2026-09-10): the cap was 2, which at the
             * 10-vbl Kronos frame (5 tics owed) starved the sim to 12
             * tics/s = 40% speed — "controls don't respond" (movement,
             * turn, weapon and thinkers all scale with tic rate because
             * p_user multiplies by vblsinframe/TICVBLS = 1 per tic). The
             * tic window measured t=0 vbl, so 5 costs ~nothing; the
             * reset below re-clamps EVERY frame, so the loop still
             * cannot spiral. 5 tics × 6 fps = the full 30 tics/s on
             * Kronos; real HW at 2.4 fps goes 16% → 40%. */
            {
                int debt = (int)(saturn_vbl_count / TICVBLS) - gametic;
                if (debt > 5)
                    gametic = (int)(saturn_vbl_count / TICVBLS) - 5;
            }
            {
                unsigned int   tic_frt_acc = 0;
                unsigned short pf_tf0;
                unsigned int   pf_tv0;

                do {
                    pf_tv0 = saturn_vbl_count;
                    pf_tf0 = SAT_FRT();
                } while (__builtin_expect(saturn_vbl_count != pf_tv0, 0));

                saturn_sim_site = 0x6;   /* r56: tic loop */
                while ((int)(saturn_vbl_count / TICVBLS) > gametic)
                {
                    unsigned int   tt_v0;
                    unsigned short tt;

                    do {
                        tt_v0 = saturn_vbl_count;
                        tt    = SAT_FRT();
                    } while (__builtin_expect(saturn_vbl_count != tt_v0, 0));

                    gametic++;
                    tics_done++;
                    /* d32xr sets vblsinframe to the vblanks consumed by the
                     * last tic (d32xr/d_main.c:381); steady state at 15 Hz is
                     * exactly TICVBLS. P_ZMovement / p_user.c scale gravity by
                     * vblsinframe/TICVBLS, so leaving it at the boot value of 1
                     * made everything fall at 1/4 speed (and player->bob
                     * accumulated 1/4 rate). No netgame on Saturn, so the
                     * per-tic value is always the constant. */
                    vblsinframe = TICVBLS;
                    /* r106 attract demo: while demoplayback is on, the
                     * pad word is replaced by the demo tic stream (one
                     * word per tic). FeedTic returns 1 when the demo is
                     * over — demoplayback is already off and
                     * saturn_quit_to_title is set, so the loop returns
                     * at the top of the next iteration.
                     * REAL play samples the pad HERE, once per tic
                     * (see the demo_feed block above): edge pair rotates
                     * once per tic, taps down to a single 30 Hz tic
                     * register, latency <= 1 tic regardless of frame
                     * rate. Zero new BSS (no state added). */
                    if (demo_feed)
                        Saturn_Demo_FeedTic(pad_buttons);
                    else
                        (void)I_ReadControls();
                    Saturn_G_Ticker();
                    saturn_tic_count++;  /* r56: census row T */
                    saturn_sim_site = 0x7;   /* r56: tic returned */
                    tic_frt_acc += Saturn_ProfSpan(tt, tt_v0);
                }
                saturn_frt_tic = (int)(tic_frt_acc >> 8);
                saturn_pf_acc[0] += Saturn_ProfSpan(pf_tf0, pf_tv0);
                saturn_sim_site = 0x8;   /* r56: tics done */
            }
            saturn_prof_ticvbl = (int)(saturn_vbl_count - prof_w0);
            saturn_prof_tics = tics_done;   /* tics actually processed */
            saturn_prof_comp = saturn_tex_comp_count - prof_c0;
            /* Peak-hold with slow decay: the pause frames are sporadic
             * (one sync cold read behind a tic) and a per-frame delta
             * drops back to 0 before a screenshot can catch it. */
            {
                int cd = saturn_wad_cold_reads - prof_r0;

                if (cd > saturn_prof_cd)
                    saturn_prof_cd = cd;
                else if (saturn_prof_cd > 0)
                    saturn_prof_cd--;
            }
            D_Display();
            saturn_prof_frame_frt = (unsigned short)Saturn_ProfSpan(frame_f0, frame_v0);
            saturn_prof_frame_vbl = (int)(saturn_vbl_count - frame_v0);
            /* Hitch latch: freeze the breakdown of any >=10-vbl frame
             * (see the block comment above the definitions). */
            if (saturn_prof_frame_vbl > 10)
            {
                saturn_prof_hitch_fv = saturn_prof_frame_vbl;
                saturn_prof_hitch_r  = saturn_prof_render;
                saturn_prof_hitch_t  = saturn_prof_ticvbl;
                saturn_prof_hitch_f  = saturn_prof_fill;
                saturn_prof_hitch_c  = saturn_wad_cold_reads - prof_r0;
            }
            /* NO I_WaitVBL(1) here: I_FinishUpdate's VDP1 commit
             * (slSynch) already blocks until the VBlank that flips the
             * VDP1 command list — waiting one more VBlank added a
             * second full display period of dead time to every frame
             * (the 's' row showed the pacing inside F). slSynch still
             * paces the loop to the display at <= 1 frame/vblank. */
        }
    }
}

void D_Display(void)
{
    extern volatile unsigned int saturn_vbl_count;
    extern volatile int          saturn_prof_render;
    extern volatile int          saturn_prof_finish;
    unsigned int                 v0, v1;
    unsigned short               rf0;

    Saturn_FrtCalibrate();       /* first call: measure this machine's FRT */
    do {
        v0  = saturn_vbl_count;
        rf0 = SAT_FRT();
    } while (__builtin_expect(saturn_vbl_count != v0, 0));
    /* Stage markers for the first 4 frames (see D_DoomLoop): R1 before
     * the 3D render, R2 after it / before the finish commit, R3 after
     * D_Display returns (printed by D_DoomLoop). Each BootMsg commits
     * immediately (slSynch), so on a freeze the last marker on screen
     * names the phase that was entered and never finished. */
    {
        static int dbg_marks = 0;
        /* Freeze-forensics: L = entering the render, H = render+HUD
         * returned (y=186 band; I_FinishUpdate owns D/E/B below). */
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char lrow[2];

            lrow[0] = 'L'; lrow[1] = 0;
            Saturn_StageTextHi(48, 186, lrow);
        }
        Saturn_G_Display();
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char hrow[2];

            hrow[0] = 'H'; hrow[1] = 0;
            Saturn_StageTextHi(68, 186, hrow);
        }
    }
    v1 = saturn_vbl_count;
    saturn_prof_render_frt = Saturn_ProfSpan(rf0, v0);
    saturn_pf_acc[1] += saturn_prof_render_frt;
    {
        unsigned short ff0;
        unsigned int   fv0;

        do {
            fv0 = saturn_vbl_count;
            ff0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != fv0, 0));

        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char irow[2];

            irow[0] = 'I'; irow[1] = 0;
            Saturn_StageTextHi(88, 186, irow);
        }
        I_FinishUpdate();
        saturn_prof_finish_frt = Saturn_ProfSpan(ff0, fv0);
    }
    saturn_prof_render = (int)(v1 - v0);
    saturn_prof_finish = (int)(saturn_vbl_count - v1);
}