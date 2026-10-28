/* saturn_demo.c — attract-demo playback for the title screen.
 *
 * The retail DOOM attract cycle is title -> demo -> title ...; d32xr
 * plays its DEMO lumps through the MiniLoop UI this port replaced (see
 * the saturn_menu.c header), so playback is reimplemented on the
 * Saturn main loop instead: the WAD's vanilla-format DEMO1..4 lumps
 * are converted ONCE, at open, into d32xr BT_* ticbuttons words, and
 * D_DoomLoop feeds them into players[0] one per tic
 * (Saturn_Demo_FeedTic) in place of the gamepad. Any real button
 * press aborts back to the title; exhausting the data ends the demo
 * the same way (saturn_quit_to_title, which returns D_DoomMain to the
 * menu loop).
 *
 * Lump format (vanilla 1.9; byte-for-byte per ManagedDoom's Demo.cs):
 *   [0]      = 109 version marker
 *   [1]      = skill (sk_baby..sk_nightmare — same ordinals as this
 *              build's skill_t)
 *   [2],[3]  = episode, map
 *   [4..8]   = deathmatch, respawn, fast, nomonsters, consoleplayer
 *              (ignored: Saturn is always single-player, player 0)
 *   [9..12]  = players-in-game flags (LSB = player 0). Only
 *              single-player demos are supported; the multiplayer
 *              demo lumps (if any) are skipped by the availability
 *              check below.
 *   then 4-byte tics: forward (sbyte), side (sbyte), turn (byte, in
 *   1/256 turn units), buttons byte (0x01 attack, 0x02 use — the
 *   weapon-change 0x04 and BT_SPECIAL 0x80 bits have no attract
 *   equivalent here and are dropped). Terminated by one 0x80
 *   DEMOMARKER byte (also accepted: stream end / < 4 bytes left).
 *
 * Tic conversion is approximate by construction: vanilla demo bytes
 * are analog, d32xr's digital input has fixed walk/run speeds and a
 * turnheld turn ramp. The thresholds below sit between vanilla's
 * walk/run magnitudes (fwd +-25/+-50, side +-18/+-40, turn 2..4) so
 * walk tics stay walk and run tics run. Playback is 1 tic-to-1 tic;
 * at the Saturn's 30 Hz player sim a 35 Hz demo runs ~14% fast —
 * fine for attract duty.
 *
 * Lump I/O uses W_CacheLumpNum(PU_STATIC) — the DEMO lumps sit inside
 * the first 64 KB of the IWAD (cart-hot region) and are 3-16 KB, well
 * under the sustained A-bus burst threshold, so the normal small-lump
 * cache path is safe (the TITLEPIC 57 KB lesson in saturn_menu.c does
 * not apply here).
 */
#include <stdint.h>

#include "doomdef.h"

/* d_main.c — demo end rides the same return path as quit-to-title. */
extern volatile int saturn_quit_to_title;

#define DEMO_MAX_NUM  4
#define DEMOMARKER    0x80

/* vanilla demo buttons byte */
#define VAN_BT_ATTACK   0x01
#define VAN_BT_USE      0x02

/* run thresholds (see header comment) */
#define RUN_FWD   38
#define RUN_SIDE  30
#define RUN_TURN  4

/* abort mask: any real pad press during playback (once armed) */
#define DEMO_ABORT_MASK \
    (BT_START | BT_ATTACK | BT_USE | BT_SPEED | \
     BT_STRAFELEFT | BT_STRAFERIGHT)

/* r105 UI: set while an attract demo is on screen (d_main.c checks it
 * to route the post-D_DoomLoop cleanup; also makes a future HUD row
 * able to name the mode). */
volatile int saturn_demo_active = 0;

/* converted tic stream */
static int *demo_words   = NULL;
static int  demo_count   = 0;
static int  demo_cursor  = 0;

/* header fields kept from the last Open */
static int  demo_skill   = 2;
static int  demo_episode = 1;
static int  demo_map     = 1;

/* abort arming: the menu's press-release handshake must not bleed
 * into playback — abort only fires on an edge once the pad was seen
 * fully released for a frame. */
static int  demo_armed   = 0;
static int  demo_prevpad = 0;

/* last word fed (clean demo-internal old/new ticbuttons edges) */
static int  demo_lastw   = 0;

/* ---- read a little-endian 32-bit field off the lump bytes ---- */
static unsigned int le32(const byte *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* ---- one 4-byte vanilla tic -> d32xr ticbuttons word ---- */
static int demo_tic_word(const byte *t)
{
    int fwd   = (signed char)t[0];
    int side  = (signed char)t[1];
    int turn  = (signed char)t[2];
    int vb    = t[3];
    int w     = 0;
    int mag   = fwd < 0 ? -fwd : fwd;
    int mags  = side < 0 ? -side : side;
    int magt  = turn < 0 ? -turn : turn;

    if (vb & VAN_BT_ATTACK) w |= BT_ATTACK;
    if (vb & VAN_BT_USE)    w |= BT_USE;

    if (fwd > 0)        w |= BT_MOVEUP;
    else if (fwd < 0)   w |= BT_MOVEDOWN;
    if (side > 0)       w |= BT_STRAFERIGHT;
    else if (side < 0)  w |= BT_STRAFELEFT;
    if (turn > 0)       w |= BT_MOVELEFT;
    else if (turn < 0)  w |= BT_MOVERIGHT;

    if (mag >= RUN_FWD || mags >= RUN_SIDE || magt >= RUN_TURN)
        w |= BT_SPEED;

    return w;
}

int Saturn_Demo_Available(void)
{
    return W_CheckNumForName("DEMO1") >= 0;
}

/* Does DEMO<num> exist? (The menu's cycle skips gaps — some WADs ship
 * fewer than four demo lumps.) */
int Saturn_Demo_Exists(int num)
{
    char name[8];

    if (num < 1 || num > DEMO_MAX_NUM)
        return 0;
    name[0] = 'D'; name[1] = 'E'; name[2] = 'M'; name[3] = 'O';
    name[4] = (char)('0' + num);
    name[5] = 0;
    return W_CheckNumForName(name) >= 0;
}

/* Open DEMO<num> (1..DEMO_MAX_NUM): parse the header, convert every
 * tic into a ticbuttons word. Returns 1 on success, 0 when the lump
 * is missing or malformed (the caller falls back to a normal game
 * start). */
int Saturn_Demo_Open(int num)
{
    char  name[8];
    int   lump, len, ntic, i;
    const byte *raw;
    unsigned int ingame;

    if (num < 1 || num > DEMO_MAX_NUM)
        return 0;

    name[0] = 'D'; name[1] = 'E'; name[2] = 'M'; name[3] = 'O';
    name[4] = (char)('0' + num);
    name[5] = 0;

    lump = W_CheckNumForName(name);
    if (lump < 0)
        return 0;

    raw = (const byte *)W_CacheLumpNum(lump, PU_STATIC);
    if (raw == NULL)
        return 0;
    len = W_LumpLength(lump);

    /* 109 header + 4 players-in-game flags, then 4-byte tics, then
     * the DEMOMARKER. */
    if (len < 13 + 4 + 4 || raw[0] != 109)
    {
        Z_Free((void *)raw);
        return 0;
    }

    /* single-player demos only (player 0 in game, nobody else) */
    ingame = le32(raw + 9);
    if (ingame != 1u)
    {
        Z_Free((void *)raw);
        return 0;
    }

    demo_skill   = raw[1];
    demo_episode = raw[2];
    demo_map     = raw[3];

    if (demo_skill < 0 || demo_skill > sk_nightmare ||
        demo_episode < 1 || demo_episode > 4 ||
        demo_map < 1 || demo_map > 9)
    {
        Z_Free((void *)raw);
        return 0;
    }

    ntic = (len - 13) / 4;
    demo_words = (int *)Z_Malloc((ntic ? ntic : 1) * sizeof(int),
                                 PU_STATIC);
    if (demo_words == NULL)
    {
        Z_Free((void *)raw);
        return 0;
    }

    demo_count  = 0;
    for (i = 0; i < ntic; i++)
    {
        const byte *t = raw + 13 + i * 4;

        /* the DEMOMARKER can sit at any 4-byte boundary in theory;
         * vanilla writes exactly one byte after the last tic */
        if (t[0] == DEMOMARKER)
            break;
        demo_words[demo_count++] = demo_tic_word(t);
    }

    Z_Free((void *)raw);

    if (demo_count == 0)
    {
        Z_Free(demo_words);
        demo_words = NULL;
        return 0;
    }

    demo_cursor = 0;
    demo_armed  = 0;
    demo_prevpad = 0;
    demo_lastw  = 0;
    return 1;
}

/* Start playback: run the level the demo header names. Saturn_G_InitNew
 * clears demoplayback, so the flag goes on AFTER it. */
void Saturn_Demo_Begin(void)
{
    extern void Saturn_G_InitNew(int skill, int episode, int map);
    extern int  saturn_new_game;

    /* Demos replay the recorded state, which is a fresh-game loadout
     * (100 hp, pistol, 50 bullets) — not whatever loadout the last
     * real playthrough left in playersresp[]. */
    saturn_new_game = 1;
    Saturn_G_InitNew(demo_skill, demo_episode, demo_map);
    demoplayback = true;
    demo_cursor  = 0;
    demo_armed   = 0;
    demo_prevpad = 0;
    demo_lastw   = 0;
}

/* Feed one tic of input. Called from D_DoomLoop's tic loop instead of
 * trusting the gamepad while demoplayback is on. real_pad is this
 * frame's REAL pad word (D_DoomLoop captured it from I_ReadControls).
 *
 * Returns 1 when the demo is over (data end or player abort) — the
 * caller must stop feeding; playback state is already torn down and
 * saturn_quit_to_title is set so D_DoomLoop returns to the menu.
 * Returns 0 while the demo continues. */
int Saturn_Demo_FeedTic(int real_pad)
{
    player_t *p = &players[consoleplayer];

    /* abort arming: wait for one fully-released frame after the menu
     * handshake before edges count */
    if (!demo_armed)
    {
        if (real_pad == 0)
            demo_armed = 1;
    }
    else
    {
        int edges = real_pad & ~demo_prevpad;

        if (edges & DEMO_ABORT_MASK)
            goto demo_over;
    }
    demo_prevpad = real_pad;

    if (demo_cursor >= demo_count)
        goto demo_over;

    p->oldticbuttons = demo_lastw;
    p->ticbuttons    = demo_words[demo_cursor++];
    demo_lastw       = p->ticbuttons;
    return 0;

demo_over:
    demoplayback = false;
    saturn_demo_active = 0;
    saturn_quit_to_title = 1;
    return 1;
}

/* Post-playback cleanup (D_DoomMain, after D_DoomLoop returned). */
void Saturn_Demo_Close(void)
{
    if (demo_words != NULL)
    {
        Z_Free(demo_words);
        demo_words = NULL;
    }
    demo_count = demo_cursor = 0;
    demoplayback = false;
    saturn_demo_active = 0;
}