/* d32xr_stubs.c — Definitions of d32xr symbols that don't have a
 * Saturn equivalent. Stubbed to satisfy the linker only.
 *
 * IMPORTANT: with -DMARS=1 and -DUSE_C_DRAW undefined, doomdef.h does:
 *   #define I_DrawColumn     I_DrawColumnA
 *   #define I_DrawColumnNPo2 I_DrawColumnNPo2A
 *   #define I_DrawFuzzColumn I_DrawFuzzColumnA
 *   #define I_DrawSpan       I_DrawSpanA
 *   #define I_DrawColumnLow      I_DrawColumnLowA
 *   #define I_DrawColumnNPo2Low  I_DrawColumnNPo2LowA
 *   #define I_DrawFuzzColumnLow  I_DrawFuzzColumnLowA
 *   #define I_DrawSpanLow        I_DrawSpanLowA
 *   #define I_DrawSpanLowSwap    I_DrawSpanLowSwapA
 *   #define I_Draw4bColumn           I_Draw4bColumnA
 *   #define I_Draw4bColumnNPo2       I_Draw4bColumnNPo2A
 *   #define I_Draw4bColumnLow        I_Draw4bColumnLowA
 *   #define I_Draw4bColumnNPo2Low    I_Draw4bColumnNPo2LowA
 * So we MUST define the *A names. The 4b* and I_DrawSpanPotato* are not
 * macro'd and we provide them directly. */

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

#include "doomdef.h"
#include "p_local.h"
#include "r_local.h"
#include "mars.h"

/* ----- Globals d32xr declares 'extern' in doomdef.h ----- */
VINT            vblsinframe     = 1;
VINT            ticsperframe    = 1;
VINT            ticsinframe     = 1;
int             gamevbls        = 0;
VINT            debugmode       = 0;
VINT            netgame         = 0;
VINT            bodyqueslot     = 0;
/* initmathtables is defined by r_main.c (doom/d32xr/r_data.c) */
VINT            consoleplayer   = 0;
boolean         demoplayback    = false;
boolean         demorecording   = false;
int             prevgametic     = 0;
VINT            gameaction      = 0;
const char     *gamemaplump     = "";

/* Game state. d32xr uses simple ints, not an enum. */
int             gamestate       = 0;
gametype_t      gametype        = gt_single;
VINT            totalkills      = 0;
VINT            totalitems      = 0;
VINT            totalsecret     = 0;
VINT            gameskill       = 2;
VINT            gameepisode     = 1;
VINT            gamemap         = 1;
VINT            startskill;
VINT            startmap;
VINT            starttype;
VINT            startsave;
boolean         startsplitscreen;
int             gamemode;
int             gamemission;

/* Player array. */
player_t       *players         = NULL;
playerresp_t   *playersresp     = NULL;
int             maxplayers      = 4;
boolean         playeringame[MAXPLAYERS];
dmapinfo_t     *gamemapinfo     = NULL;

/* jagonly-only globals (32X hardware state). */
boolean         debugscreenstate  = false;
boolean         debugscreenactive = false;
int            *screenshade;
byte           *debugscreen       = NULL;
int             workpage;
int             worklist;
int             isrvmode          = 0;
int            *listbuffers[2]    = {NULL, NULL};
int            *displaylist_p;
int            *readylist_p;
int            *worklist_p;
int            *work_p;
int             joypad[32];
int             joystick1;
int             ticcount;
int             junk;
int             spincount;
int             ZERO = 0, zero = 0, zero2 = 0;
byte           *framebuffer_p;
int             lastticcount;
int             lasttics;
int             workingscreen;
int             gpufinished;
int             phasetime;
int             showAllLines    = 0;
int             showAllThings   = 0;
boolean         splitscreen     = false;
boolean         mousepresent    = false;
int             ticon;
int             ticrealbuttons  = 0;
int             cy              = 0;
int             oldticrealbuttons;

/* ----- D_* (mem/printf) -----
 * D_printf is a NO-OP. On the 32X it went to the debug console; on
 * Saturn there is no stdout sink. The original body called newlib's
 * vprintf, which walks the un-initialized _reent/FILE machinery and is
 * the prime suspect for the cyan STARTING hang (P_SetupLevel calls
 * D_printf at p_setup.c:931 BEFORE any stage marker — same crash class
 * as the library vsnprintf that used to kill I_Error). Format with
 * Saturn_Format + Saturn_BootText if a message ever needs to be seen. */
void D_printf(char *str, ...)
{
    (void)str;   /* args live in registers; SH-2 caller pops them */
}
void D_memset(void *dest, int val, size_t count)        { memset(dest, val, count); }
void D_memcpy(void *dest, const void *src, size_t count) { memcpy(dest, src, count); }
void D_strncpy(char *dest, const char *src, int maxcount) { strncpy(dest, src, maxcount); }
int  D_strncasecmp(const char *a, const char *b, int n)
{
    while (n-- && *a && *b) {
        int ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
    }
    return 0;
}
int  D_strcasecmp(const char *a, const char *b) { return D_strncasecmp(a, b, 0x7fffffff); }
void D_isort(int *a, int len) { (void)a; (void)len; }

/* ----- Byte/word swaps ----- */
short ShortSwap(short v)
{
    unsigned short u = (unsigned short)v;
    return (short)((u >> 8) | (u << 8));
}
long  LongSwap(long v)
{
    unsigned long u = (unsigned long)v;
    return (long)(((u >> 24) & 0xffUL) |
                  ((u >> 8)  & 0xff00UL) |
                  ((u << 8)  & 0xff0000UL) |
                  ((u << 24) & 0xff000000UL));
}

/* ----- Z zone -----
 * I_ZoneBase is REAL now — defined in d_main.c (cart-backed zone via
 * Saturn_ZoneBase, static-pool fallback). Z_Setup is gone; d32xr's
 * Z_Init asks I_ZoneBase for the pool. */

/* ----- FixedDiv -----
 * ((int64)a<<16)/b is a signed 64/32 divide — exactly what the SH-2's
 * divide unit eats (DVDNTH/DVDNTL at 0xFFFFFF10/14; d32xr's marsroq.c
 * feeds FixedDiv-style operands through it the same way). ~250-cycle
 * __divdi3 -> ~16-cycle register writes. Gated on the boot probe
 * (d_main.c, HUD 'V' row) so an absent unit falls back to the C path.
 * Overflow (quotient past 32 bits) returns whatever the unit puts in
 * DVDNTL — the old 64-bit divide truncated to low 32 bits too. */

/* Divide census counters (SATURN_DIVLUT, read+reset once per frame by
 * i_video.c's perf rows — N = total, G = in-envelope). */
#ifdef SATURN_DIVLUT
volatile unsigned int sat_fd_total = 0;
volatile unsigned int sat_fd_env   = 0;
#endif
fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if (b == 0) return (a < 0) ? -0x7fffffff : 0x7fffffff;
    {
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 2)
        /* Divide CENSUS (2026-09-08, the rig campaign's "census first"
         * rule): total calls, and the in-envelope fraction — |b| <=
         * 511 and |a|*|b| < 2^16 (the only shape a future OCRAM recip
         * path could serve, since the dividend here is |a|<<16 and
         * exactness needs (|a|<<16)*|b| < 2^32). Counted, not diverted:
         * burn 1 measures before any FixedDiv reroute. */
        {
            extern volatile unsigned int sat_fd_total;
            extern volatile unsigned int sat_fd_env;
            fixed_t ab = (a < 0 ? -a : a), bb = (b < 0 ? -b : b);

            sat_fd_total += 1u;
            if (bb <= 511 && (unsigned)ab <= 0xFFFFu / bb)
                sat_fd_env += 1u;
        }
#endif
        extern volatile int saturn_divu_probe;
#ifndef SATURN_WIN32
        if (saturn_divu_probe)
        {
            volatile int *const dvsr   = (volatile int *)0xFFFFFF00u;
            volatile int *const dvdnth = (volatile int *)0xFFFFFF10u;
            volatile int *const dvdntl = (volatile int *)0xFFFFFF14u;

            *dvsr   = b;
            *dvdnth = a >> 16;
            *dvdntl = a << 16;              /* start the divide */
            __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                              "\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
                              ::: "memory");
            return *dvdntl;
        }
#endif
    }
    return ((int64_t)a << 16) / b;
}
fixed_t IDiv(fixed_t a, fixed_t b) { return FixedDiv(a, b); }

/* ----- SRAM ----- */
void I_WriteSRAM(int offset, int val) { (void)offset; (void)val; }
void I_WriteU32SRAM(int offset, uint32_t val) { (void)offset; (void)val; }
uint8_t   I_ReadSRAM(int offset) { (void)offset; return 0; }
uint32_t  I_ReadU32SRAM(int offset) { (void)offset; return 0; }

/* ----- Work buffers (Saturn just uses static memory) ----- */
static byte scratch_buf[48 * 1024];
static int  scratch_used = 0;
byte *I_TempBuffer(int size)
{
    if (size <= 0) size = 1;
    if ((size_t)size > sizeof scratch_buf) size = sizeof scratch_buf;
    memset(scratch_buf, 0, size);
    return scratch_buf;
}
byte *I_WorkBuffer(void)                 { return scratch_buf; }
void  I_FreeWorkBuffer(void)             { scratch_used = 0; }
byte *I_AllocWorkBuffer(int size)        { (void)size; return scratch_buf; }
/* d32xr's render targets. DOOM's 8bpp renderer uses I_VideoBuffer
 * (defined in i_video.c) for the visible framebuffer, but p_setup.c
 * DOES write through I_FrameBuffer: P_SetupLevel encodes node bboxes
 * into `I_FrameBuffer()` treated as a short[] scratch (up to 8 words
 * per node, thousands of nodes). The old 1-element sentinels silently
 * corrupted whatever followed them in BSS. Back all three with the
 * 64 KB scratch buffer (plenty for numnodes*8 shorts) instead. */
pixel_t *I_ViewportBuffer(void)          { return (pixel_t *)scratch_buf; }
pixel_t *I_FrameBuffer(void)             { return (pixel_t *)scratch_buf; }

/* CD file API. I_OpenCDFileByName, I_OpenCDFileByOffset, I_SeekCDFile,
 * I_ReadCDFile, I_GetCDFileBuffer are defined in i_wad.c (the Saturn
 * hybrid WAD loader). I_ReadCDDirectory is unused on Saturn.
 * I_GetCDFileCache / I_SetCDFileCache are referenced by W_LoadPWAD
 * for caching the PWAD directory on 32X. On Saturn we don't load
 * PWADs from CD (the IWAD is the only WAD), so these are stubs. */
int  I_ReadCDDirectory(const char *path, char **pbuf)    { (void)path; (void)pbuf; return 0; }
void *I_GetCDFileCache(int length)                       { (void)length; return NULL; }
void I_SetCDFileCache(int length)                        { (void)length; }
int  I_PlayCinematic(const char *fn, void *mem, size_t size, int allowpause)
{ (void)fn; (void)mem; (void)size; (void)allowpause; return 0; }
void I_LoadFonts(void)                                    { }

/* ----- Refresh gating ----- */
boolean I_RefreshCompleted(void) { return true; }
boolean I_RefreshLatched(void)   { return true; }

/* ----- I_* functions ----- */
void InitDisplay(void) { }
void Spin(void)        { }
void ReadEEProm(void)  { }
void Jag68k_main(void) { }
void I_Print8(int x, int y, const char *string) { (void)x; (void)y; (void)string; }
int  I_Print8Len(const char *s) { (void)s; return 0; }
void I_Print8_fb(int x, int y, char *s) { (void)x; (void)y; (void)s; }
void I_DebugScreen(void) { }
void I_DrawSbar(void) { }
void I_ClearFrameBuffer(void) { }
void I_ClearWorkBuffer(int len) { (void)len; }
void I_ResetLineTable(void) { }
/* I_SetPalette lives in i_video.c (pushes DOOM's 256-color palette into the
 * CRAM slot owned by the 8bpp VDP1 sprite). I_ReadControls lives in
 * i_system.c (translates Jo Engine gamepad bitmask
 * to DOOM's BT_* ticbuttons). I_ReadControls2 / I_ReadMouse are not used
 * on Saturn (single-player gamepad only). */
int  I_ReadMouse(int *pmx, int *pmy) { (void)pmx; (void)pmy; return 0; }
void I_NetSetup(void) { }
void I_NetStop(void) { }
void I_NetReady(void) { }
unsigned I_NetTransfer(unsigned b) { (void)b; return 0; }
int  I_GetFRTCounter(void) { static int c = 0; return c; }
void I_Update(void) { }
void I_StoreScreenCopy(void) { }
void I_RestoreScreenCopy(void) { }
void I_SwapScreenCopy(void) { }
int  I_FrameBufferHeight(void) { return SCREENHEIGHT; }
int  I_IsPAL(void) { return 0; }
int  I_ViewportYPos(void) { return 0; }
pixel_t *I_OverwriteBuffer(void) { return (pixel_t *)scratch_buf; }

void DrawTiledBackground(void)  { }
void DrawTiledBackground2(int f){ (void)f; }

/* SH-2 has NO divide instruction — gcc lowers every variable-divisor
 * `a / b` / `a % b` into calls to these helpers. The previous one-line
 * versions ("return b ? a / b : 0") each compiled into a call to
 * THEMSELVES: infinite recursion, stack overflow, and a frozen solid
 * red screen the moment anything formatted a number (2026-08-30 —
 * I_Error's %d died exactly here while the CD directory read failed).
 * These are d32xr's jagonly.c shift-subtract implementations, proven on
 * the 32X SH-2, and deliberately contain no / or % operators. */
int __mulsi3(int a, int b) { return a * b; } /* mul.l is a hardware op — safe */

#ifndef SATURN_WIN32
/* GCC SH-2 target (sh.md udivsi3_i1) models calls to __udivsi3 as only
 * clobbering r0, r1, r4, t, pr — it assumes r2, r3, r5, r6, r7 are PRESERVED!
 * A normal C implementation of __udivsi3 modifies r5 (bb) in-place,
 * corrupting any caller that keeps values in r5 across divisions (such as
 * Saturn_HUD_Draw dividing bsp_ms, seg_ms, slv_ms by per_ms in r5).
 * Saving/restoring r4 and r5 on the stack and using only r0, r1, r4, r5
 * preserves r2-r7 strictly. */
__asm__(
    ".section .text\n\t"
    ".balign 4\n\t"
    ".global ___udivsi3\n\t"
    ".global __udivsi3\n\t"
    "___udivsi3:\n"
    "__udivsi3:\n\t"
    "mov     r4, r1\n\t"
    "shlr16  r1\n\t"
    "shll2   r1\n\t"
    "shlr16  r1\n\t"
    "cmp/hs  r5, r1\n\t"
    "bt      .L_udiv_clamp\n\t"
    "mov.l   r4, @-r15\n\t"
    "mov.l   r5, @-r15\n\t"
    "cmp/hi  r5, r4\n\t"
    "bf/s    .L_udiv_loop_init\n\t"
    "mov     #1, r1\n"
    ".L_udiv_shift:\n\t"
    "mov     r5, r0\n\t"
    "shll    r0\n\t"
    "bt      .L_udiv_loop_init\n\t"
    "add     r5, r5\n\t"
    "cmp/hi  r5, r4\n\t"
    "bt/s    .L_udiv_shift\n\t"
    "add     r1, r1\n"
    ".L_udiv_loop_init:\n\t"
    "mov     #0, r0\n"
    ".L_udiv_loop:\n\t"
    "cmp/hs  r5, r4\n\t"
    "bf      .L_udiv_no_sub\n\t"
    "sub     r5, r4\n\t"
    "or      r1, r0\n"
    ".L_udiv_no_sub:\n\t"
    "shlr    r5\n\t"
    "shlr    r1\n\t"
    "tst     r1, r1\n\t"
    "bt/s    .L_udiv_done\n\t"
    "tst     r4, r4\n\t"
    "bf      .L_udiv_loop\n"
    ".L_udiv_done:\n\t"
    "mov.l   @r15+, r5\n\t"
    "rts\n\t"
    "mov.l   @r15+, r4\n"
    ".L_udiv_clamp:\n\t"
    "mov.l   .L_udiv_max, r0\n\t"
    "rts\n\t"
    "nop\n\t"
    ".balign 4\n"
    ".L_udiv_max:\n\t"
    ".long   0x7fffffff\n"
);
extern unsigned __udivsi3(unsigned aa, unsigned bb);
#else
unsigned __udivsi3(unsigned aa, unsigned bb)
{
    unsigned bit, c;

    if ((aa >> 30) >= bb)
        return 0x7fffffff; /* also catches bb == 0 */

    bit = 1;
    while (aa > bb && bb < 0x80000000u)
    {
        bb <<= 1;
        bit <<= 1;
    }

    c = 0;
    do
    {
        if (aa >= bb)
        {
            aa -= bb;
            c |= bit;
        }
        bb >>= 1;
        bit >>= 1;
    } while (bit && aa);

    return c;
}
#endif

unsigned __umodsi3(unsigned aa, unsigned bb)
{
    return aa - __udivsi3(aa, bb) * bb;
}

int __divsi3(int a, int b)
{
    unsigned aa = (a < 0) ? -(unsigned)a : (unsigned)a;
    unsigned bb = (b < 0) ? -(unsigned)b : (unsigned)b;
    unsigned q  = __udivsi3(aa, bb);
    return ((a ^ b) < 0) ? -(int)q : (int)q;
}

int __modsi3(int a, int b)
{
    unsigned aa = (a < 0) ? -(unsigned)a : (unsigned)a;
    unsigned bb = (b < 0) ? -(unsigned)b : (unsigned)b;
    unsigned r  = aa - __udivsi3(aa, bb) * bb;
    return (a < 0) ? -(int)r : (int)r;
}

/* ----- Game state machine ----- */
/* Use-path counters (door diagnosis, 2026-09-01): CUMULATIVE — never
 * reset, so a single screenshot after tapping B at a door proves the
 * whole sim chain without catching a live frame. Shown on the X/L/P
 * HUD rows (i_video.c). */
int saturn_cnt_use     = 0;   /* P_UseLines entered (B rising edge)          */
int saturn_cnt_usespec = 0;   /* closest line had a special -> special call  */
int saturn_cnt_doorspawn = 0; /* EV_VerticalDoor spawned a door thinker      */
int saturn_door_secnum = -1;  /* sector of the last spawned door (-1 none)   */
int saturn_door_ceil   = 0;   /* that sector's live ceiling (fixed) — T_VerticalDoor */
int saturn_cnt_usevisit  = 0; /* blockmap lines visited by the LAST use      */
int saturn_cnt_usehit    = 0; /* lines passing the intercept by the LAST use */
int saturn_cnt_usebbox   = 0; /* visited lines that passed the bbox test      */

int  G_Ticker(void) { return 0; }
void G_DeathMatchSpawnPlayer(int p) { (void)p; }
void G_DoLoadLevel(void) { }
void G_DoReborn(int p) { (void)p; }
void G_ExitLevel(void)      { gameaction = ga_completed; }
void G_SecretExitLevel(void){ gameaction = ga_secretexit; }

/* G_PlayerFinishLevel / G_PlayerReborn are REAL (ported from
 * d32xr/g_game.c:187/220). They were empty stubs until 2026-09-06,
 * which meant every level start spawned the player with 0 health, no
 * pistol and no ammo — players[] is zeroed at boot (d_main.c) and
 * P_SpawnPlayer copies p->health (0) into the mobj. That was the
 * "HUD shows 0% health" report; dead men pick nothing up, so the
 * message line had nothing to show either. The loadout travels in
 * playersresp[]: FinishLevel saves it at level exit (g_game.c exit
 * latch), Reborn/P_RestoreResp puts it back when the next map spawns
 * the player. Saturn_G_InitNew gates its own P_ResetResp on
 * saturn_new_game so only a FRESH GAME (or demo) is a pistol start. */
void G_PlayerFinishLevel(int player)
{
    player_t *p = &players[player];

    D_memset(p->powers, 0, sizeof(p->powers));
    D_memset(p->cards, 0, sizeof(p->cards));
    p->mo->flags &= ~MF_SHADOW;     /* cancel invisibility  */
    p->extralight  = 0;             /* cancel gun flashes   */
    p->damagecount = 0;             /* no palette changes   */
    p->bonuscount  = 0;
    if (p->health > 0)
        P_UpdateResp(p);
}

void G_PlayerReborn(int player)
{
    player_t *p = &players[player];
    int frags       = p->frags;
    int killcount   = p->killcount;
    int itemcount   = p->itemcount;
    int secretcount = p->secretcount;

    D_memset(p, 0, sizeof(*p));
    p->frags       = frags;
    p->killcount   = killcount;
    p->itemcount   = itemcount;
    p->secretcount = secretcount;
    p->usedown = p->attackdown = true;  /* don't act immediately */
    p->playerstate = PST_LIVE;
    P_RestoreResp(p);
}
int  G_PlayDemoPtr(unsigned *demo) { (void)demo; return 0; }
void G_InitNew(skill_t skill, int map, gametype_t gt, boolean ss)
{ (void)skill; (void)map; (void)gt; (void)ss; }
void G_Init(void)    { }
void G_DeInit(void)  { }
void G_RecordDemo(void) { }

/* ----- AM_*, M_*, O_*, HUD_* ----- */
void AM_Control(player_t *p) { (void)p; }
void AM_Drawer(void)  { }
void AM_Start(void) { }
int  canwipe = 0;
int  M_Ticker(void) { return 0; }
void M_Drawer(void) { }
void M_Stop(void)   { }
void M_Start(void)  { }
void M_Start2(boolean startup) { (void)startup; }
void O_Init(void) { }
void O_Control(player_t *p) { (void)p; }
void O_Drawer(void)  { }
void O_SetButtonsFromControltype(void) { }
void HUD_AddMessage(const char *m) { (void)m; }
void HUD_Drawer(void) { }
void HUD_Init(void) { }
void HUD_Ticker(void) { }

/* ----- F_*, IN_*, WI_* ----- */
void F_Start(void) { }
void F_Stop(void)  { }
int  F_Ticker(void) { return 0; }
void F_Drawer(void) { }
void IN_Start(void) { }
void IN_Stop(void)  { }
int  IN_Ticker(void) { return 0; }
void IN_Drawer(void) { }

/* ----- GS_* ----- */
void GS_Start(void) { }
void GS_Stop(void)  { }
int  GS_Ticker(void) { return 0; }
void GS_Drawer(void) { }

/* ----- Random -----
 * Real d32xr implementation (ported from d32xr/d_main.c:197-241 — that
 * file isn't part of the Saturn build). The old stubs returned 0 every
 * call, which degenerated gameplay: every monster made the same choice
 * on every tick, damage rolls were constant, and P_Random-based
 * stochastic state never advanced. rndindex/prndindex are NOT declared
 * extern in doomdef.h, so defining them here is collision-free. */
static const unsigned char rndtable[256] = {
    0,   8, 109, 220, 222, 241, 149, 107,  75, 248, 254, 140,  16,  66,
    74,  21, 211,  47,  80, 242, 154,  27, 205, 128, 161,  89,  77,  36,
    95, 110,  85,  48, 212, 140, 211, 249,  22,  79, 200,  50,  28, 188,
    52, 140, 202, 120,  68, 145,  62,  70, 184, 190,  91, 197, 152, 224,
    149, 104,  25, 178, 252, 182, 202, 182, 141, 197,   4,  81, 181, 242,
    145,  42,  39, 227, 156, 198, 225, 193, 219,  93, 122, 175, 249,   0,
    175, 143,  70, 239,  46, 246, 163,  53, 163, 109, 168, 135,   2, 235,
    25,  92,  20, 145, 138,  77,  69, 166,  78, 176, 173, 212, 166, 113,
    94, 161,  41,  50, 239,  49, 111, 164,  70,  60,   2,  37, 171,  75,
    136, 156,  11,  56,  42, 146, 138, 229,  73, 146,  77,  61,  98, 196,
    135, 106,  63, 197, 195,  86,  96, 203, 113, 101, 170, 247, 181, 113,
    80, 250, 108,   7, 255, 237, 129, 226,  79, 107, 112, 166, 103, 241,
    24, 223, 239, 120, 198,  58,  60,  82, 128,   3, 184,  66, 143, 224,
    145, 224,  81, 206, 163,  45,  63,  90, 168, 114,  59,  33, 159,  95,
    28, 139, 123,  98, 125, 196,  15,  70, 194, 253,  54,  14, 109, 226,
    71,  17, 161,  93, 186,  87, 244, 138,  20,  52, 123, 251,  26,  36,
    17,  46,  52, 231, 232,  76,  31, 221,  84,  37, 216, 165, 212, 106,
    197, 242,  98,  43,  39, 175, 254, 145, 190,  84, 118, 222, 187, 136,
    120, 163, 236, 249
};
int rndindex = 0;
int prndindex = 0;

int  P_Random(void)     { prndindex = (prndindex + 1) & 0xff; return rndtable[prndindex]; }
int  M_Random(void)     { rndindex  = (rndindex  + 1) & 0xff; return rndtable[rndindex]; }
void M_ClearRandom(void){ rndindex = prndindex = 0; }
void P_RandomSeed(int s){ prndindex = s & 0xff; }

/* ----- P_Init / Start / Stop / Drawer / Update / Ticker (provided by p_tick.c) ----- */

/* ----- P_* gameplay helpers (declarations from p_local.h) -----
 *
 * 2026-08-31: the movement/collision core is NO LONGER stubbed. The
 * original d32xr p_move.c / p_slide.c / p_maputl.c are now compiled
 * into the build (see Makefile). Before that, P_SlideMove was a no-op
 * stub while p_user.c's P_PlayerMove called it with an UNINITIALISED
 * stack struct and then looped `for (i = 0; i < sm.numspechit; i++)
 * P_CrossSpecialLine(sm.spechit[i], mo)` — a garbage count iterating
 * garbage line pointers, which froze the machine the moment the player
 * walked forward. Everything those three files define was removed from
 * this stub list; what remains is what the still-missing d32xr files
 * (p_base.c, p_change.c) would provide. */

/* 2026-08-31, later the same day: p_base.c and p_change.c are now in the
 * build too (verbatim from pristine d32xr — see Makefile). That gives
 * us the REAL mobj thinker driver (P_RunMobjBase2 / P_RunMobjLate ->
 * P_MobjThinker -> P_XYMovement / P_ZMovement), so monster AI is no
 * longer inert, plus P_ChangeMover (sector height changes / crushing).
 * P_ApplyFriction, P_ChangeMover, P_RunMobjBase2 and P_RunMobjLate
 * were removed from this stub list accordingly. */

/* M_ClearBox / M_AddToBox — verbatim from d32xr d_main.c:244-260 (the
 * Saturn d_main.c doesn't carry them; p_maputl's blockmap iterators
 * need them). */
void M_ClearBox (fixed_t *box)
{
    box[BOXTOP] = box[BOXRIGHT] = D_MININT;
    box[BOXBOTTOM] = box[BOXLEFT] = D_MAXINT;
}
void M_AddToBox (fixed_t *box, fixed_t x, fixed_t y)
{
    if (x<box[BOXLEFT])
        box[BOXLEFT] = x;
    else if (x>box[BOXRIGHT])
        box[BOXRIGHT] = x;
    if (y<box[BOXBOTTOM])
        box[BOXBOTTOM] = y;
    else if (y>box[BOXTOP])
        box[BOXTOP] = y;
}

/* Positional-sound variant — sound output is not wired on Saturn yet,
 * so no-op (the real body lives in d32xr marssound.c:670). */
void S_StartFixedSound(int sound_id, fixed_t *origin)
{ (void)sound_id; (void)origin; }

/* ----- R_* renderer stubs (most provided by r_main.c / r_data.c) ----- */
void R_BSP(void) { }
int  R_Cache(int lump) { (void)lump; return 0; }
void R_ClearTexCache(r_texcache_t *c) { (void)c; }
void R_DrawPlanes(void) { }
void R_InitTables(void) { }
void R_InitTexCache(r_texcache_t *c) { (void)c; }
void R_InitTexCacheZone(r_texcache_t *c, int zonesize) { (void)c; (void)zonesize; }
int  R_InTexCache(r_texcache_t *c, void *p) { (void)c; (void)p; return 0; }
void R_SegCommands(void) { }
void R_SpritePrep(void) { }
void R_Sprites(void) { }
void R_Update(void) { }

/* Column-drawer asm-stubs. d32xr's doomdef.h maps I_Draw*() -> I_Draw*A,
 * so we must define the *A names. */

void I_DrawColumnA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                   fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_DrawColumnNPo2A(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                       fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_DrawColumnLowA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                      fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_DrawColumnNPo2LowA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                          fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_Draw4bColumnA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                     fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_Draw4bColumnNPo2A(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                         fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_Draw4bColumnLowA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                        fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_Draw4bColumnNPo2LowA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t dc_iscale,
                            fixed_t dc_texturemid, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)dc_iscale;
    (void)dc_texturemid; (void)dc_source; (void)dc_texheight;
}
void I_DrawSpanA(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                 fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                 inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}
void I_DrawSpanLowA(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                    fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                    inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}
void I_DrawSpanLowSwapA(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                        fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                        inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}
void I_DrawSpanPotato(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                      fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                      inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}
void I_DrawSpanPotatoLow(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                         fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                         inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}
void I_DrawFuzzColumnA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t frac_,
                       fixed_t fracstep, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)frac_;
    (void)fracstep; (void)dc_source; (void)dc_texheight;
}
void I_DrawFuzzColumnLowA(int dc_x, int dc_yl, int dc_yh, int light, fixed_t frac_,
                          fixed_t fracstep, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)frac_;
    (void)fracstep; (void)dc_source; (void)dc_texheight;
}
void I_DrawColumnNoDraw(int dc_x, int dc_yl, int dc_yh, int light, fixed_t frac_,
                        fixed_t fracstep, inpixel_t *dc_source, int dc_texheight)
{
    (void)dc_x; (void)dc_yl; (void)dc_yh; (void)light; (void)frac_;
    (void)fracstep; (void)dc_source; (void)dc_texheight;
}
void I_DrawSpanNoDraw(int ds_y, int ds_x1, int ds_x2, int light, fixed_t ds_xfrac,
                      fixed_t ds_yfrac, fixed_t ds_xstep, fixed_t ds_ystep,
                      inpixel_t *ds_source, int dc_texheight)
{
    (void)ds_y; (void)ds_x1; (void)ds_x2; (void)light; (void)ds_xfrac;
    (void)ds_yfrac; (void)ds_xstep; (void)ds_ystep; (void)ds_source; (void)dc_texheight;
}

/* Column maps (r_data.c references these for palette indices). */
int8_t *dc_colormaps;
int8_t *dc_colormaps2;
int8_t *dc_hcolormaps;
int8_t *dc_hcolormaps2;
int8_t *dc_lcolormaps;
int8_t *dc_lcolormaps2;

/* ----- TLS -----
 * doomdef.h (MARS+SATURN) maps I_Set/GetThreadLocalVar onto this
 * plain array — the memory emulation of the 32X's GBR-relative TLS.
 * DOOMTLS offsets are 0/4/8/12/16/20, so (offs)>>2 indexes slots 0..5.
 * p_setup.c:1073 stores &validcount[0] into slot DOOMTLS_VALIDCOUNT
 * at level setup; p_move/p_slide/p_map/p_shoot fetch it on every
 * collision check. */
unsigned int Saturn_TlsSlots[8];

/* ----- Mars_* (32X DSP dispatch) ----- */
void Mars_ClearCache(void *p, int n)        { (void)p; (void)n; }
void Mars_ClearCacheLine(void *p)           { (void)p; }
void Mars_ClearCacheLines(void *p, int n)   { (void)p; (void)n; }
void Mars_CommSlaveClearCache(void)         { }
void Mars_P_BeginCheckSights(void)          { }
void Mars_P_EndCheckSights(void)            { }
void Mars_R_BeginWallPrep(void)             { }
void Mars_R_EndWallPrep(void)               { }
void Mars_R_SecWait(int s)                  { (void)s; }
void Mars_SetVDPPri(int p)                  { (void)p; }

/* Jo Engine's slave SH-2 init is now provided by jo_engine/slave.c
 * (we enable JO_COMPILE_WITH_DUAL_CPU_MODULE=1), so the stub here
 * is removed to avoid the link-time "multiple definition" error. */

/* Menu fire (the little animated "fire" on the title screen) */
void I_InitMenuFire(jagobj_t *p) { (void)p; }
void I_StopMenuFire(void) { }
void I_DrawMenuFire(void) { }