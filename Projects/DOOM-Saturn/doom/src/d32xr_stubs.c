/* d32xr_stubs.c — Definitions of d32xr symbols that don't have a
 * Saturn equivalent. Stubbed to satisfy the linker only. */

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

#include "doomdef.h"
#include "p_local.h"
#include "r_local.h"
#include "mars.h"
#include "saturn_memmap.h"

/* ----- Globals d32xr declares 'extern' in doomdef.h ----- */
VINT            vblsinframe     = 1;
VINT            ticsperframe    = 1;
VINT            ticsinframe     = 1;
int             gamevbls        = 0;
VINT            debugmode       = 0;
VINT            netgame         = 0;
VINT            bodyqueslot     = 0;
VINT            consoleplayer   = 0;
boolean         demoplayback    = false;
boolean         demorecording   = false;
int             prevgametic     = 0;
VINT            gameaction      = 0;
const char     *gamemaplump     = "";

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

player_t       *players         = NULL;
playerresp_t   *playersresp     = NULL;
int             maxplayers      = 4;
boolean         playeringame[MAXPLAYERS];
dmapinfo_t     *gamemapinfo     = NULL;

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

void D_printf(char *str, ...)
{
    (void)str;
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

#ifdef SATURN_DIVLUT
volatile unsigned int sat_fd_total = 0;
volatile unsigned int sat_fd_env   = 0;
#endif
fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if (b == 0) return (a < 0) ? -0x7fffffff : 0x7fffffff;
    {
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 2)
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

void I_WriteSRAM(int offset, int val) { (void)offset; (void)val; }
void I_WriteU32SRAM(int offset, uint32_t val) { (void)offset; (void)val; }
uint8_t   I_ReadSRAM(int offset) { (void)offset; return 0; }
uint32_t  I_ReadU32SRAM(int offset) { (void)offset; return 0; }

#ifndef SATURN_WIN32
#define SCRATCH_BUF_SIZE (48 * 1024u)
#define scratch_buf      ((byte *)((uintptr_t)SATURN_WRAM_L_SKY_ADDR + 0x8000u))
#else
static byte scratch_buf[48 * 1024];
#define SCRATCH_BUF_SIZE (sizeof scratch_buf)
#endif
static int  scratch_used = 0;

__attribute__((aligned(16))) unsigned int saturn_master_oclut[3072];
__attribute__((aligned(16))) uint32_t     hw_wram_h_sub_flat_info[608 * 4];
__attribute__((aligned(16))) uint32_t     hw_wram_h_proj_verts_slave[960 * 4];
#if defined(HW_WALK2) && HW_WALK2
__attribute__((aligned(16))) uint32_t     hw_wram_h_slave_stack[1024];  /* + the split-frame walk */
#elif defined(HW_SLAVE_WALLS) && HW_SLAVE_WALLS
__attribute__((aligned(16))) uint32_t     hw_wram_h_slave_stack[640];   /* + wall emitters */
#else
__attribute__((aligned(16))) uint32_t     hw_wram_h_slave_stack[224];
#endif
__attribute__((aligned(16))) uint32_t     hw_wram_h_portal_segs[336 * 3];
byte *I_TempBuffer(int size)
{
    if (size <= 0) size = 1;
    if ((size_t)size > SCRATCH_BUF_SIZE) size = (int)SCRATCH_BUF_SIZE;
    memset(scratch_buf, 0, size);
    return scratch_buf;
}
byte *I_WorkBuffer(void)                 { return scratch_buf; }
void  I_FreeWorkBuffer(void)             { scratch_used = 0; }
byte *I_AllocWorkBuffer(int size)        { (void)size; return scratch_buf; }
pixel_t *I_ViewportBuffer(void)          { return (pixel_t *)scratch_buf; }
pixel_t *I_FrameBuffer(void)             { return (pixel_t *)scratch_buf; }

int  I_ReadCDDirectory(const char *path, char **pbuf)    { (void)path; (void)pbuf; return 0; }
void *I_GetCDFileCache(int length)                       { (void)length; return NULL; }
void I_SetCDFileCache(int length)                        { (void)length; }
int  I_PlayCinematic(const char *fn, void *mem, size_t size, int allowpause)
{ (void)fn; (void)mem; (void)size; (void)allowpause; return 0; }
void I_LoadFonts(void)                                    { }

boolean I_RefreshCompleted(void) { return true; }
boolean I_RefreshLatched(void)   { return true; }

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

int __mulsi3(int a, int b) { return a * b; }

#ifndef SATURN_WIN32
extern unsigned __udivsi3(unsigned aa, unsigned bb);
#else
unsigned __udivsi3(unsigned aa, unsigned bb)
{
    unsigned bit, c;

    if ((aa >> 30) >= bb)
        return 0x7fffffff;

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

int saturn_cnt_use     = 0;
int saturn_cnt_usespec = 0;
int saturn_cnt_doorspawn = 0;
int saturn_door_secnum = -1;
int saturn_door_ceil   = 0;
int saturn_cnt_usevisit  = 0;
int saturn_cnt_usehit    = 0;
int saturn_cnt_usebbox   = 0;

int  G_Ticker(void) { return 0; }
void G_DeathMatchSpawnPlayer(int p) { (void)p; }
void G_DoLoadLevel(void) { }
/* Dead player pressed USE (p_user.c sets PST_REBORN; P_Ticker calls this
 * every tic while it stays set). 2026-11-21: was an empty stub, so the
 * player stayed dead until Pause -> Restart. Single player in vanilla
 * restarts the map with a pistol start: request exactly the pause menu's
 * Restart (D_DoomLoop runs Saturn_WarpToLevel outside the tic). */
void G_DoReborn(int p)
{
    extern volatile int saturn_warp_episode, saturn_warp_map;
    extern int saturn_episode, saturn_map;
    (void)p;
    if (saturn_warp_map <= 0)
    {
        saturn_warp_episode = (saturn_episode > 0) ? saturn_episode : 1;
        saturn_warp_map     = (saturn_map > 0) ? saturn_map : 1;
    }
}
void G_ExitLevel(void)      { gameaction = ga_completed; }
void G_SecretExitLevel(void){ gameaction = ga_secretexit; }

void G_PlayerFinishLevel(int player)
{
    player_t *p = &players[player];

    D_memset(p->powers, 0, sizeof(p->powers));
    D_memset(p->cards, 0, sizeof(p->cards));
    p->mo->flags &= ~MF_SHADOW;
    p->extralight  = 0;
    p->damagecount = 0;
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
    p->usedown = p->attackdown = true;
    p->playerstate = PST_LIVE;
    P_RestoreResp(p);
}
int  G_PlayDemoPtr(unsigned *demo) { (void)demo; return 0; }
void G_InitNew(skill_t skill, int map, gametype_t gt, boolean ss)
{ (void)skill; (void)map; (void)gt; (void)ss; }
void G_Init(void)    { }
void G_DeInit(void)  { }
void G_RecordDemo(void) { }

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

void F_Start(void) { }
void F_Stop(void)  { }
int  F_Ticker(void) { return 0; }
void F_Drawer(void) { }
void IN_Start(void) { }
void IN_Stop(void)  { }
int  IN_Ticker(void) { return 0; }
void IN_Drawer(void) { }

void GS_Start(void) { }
void GS_Stop(void)  { }
int  GS_Ticker(void) { return 0; }
void GS_Drawer(void) { }

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

int8_t *dc_colormaps;
int8_t *dc_colormaps2;
int8_t *dc_hcolormaps;
int8_t *dc_hcolormaps2;
int8_t *dc_lcolormaps;
int8_t *dc_lcolormaps2;

unsigned int Saturn_TlsSlots[8];

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

void I_InitMenuFire(jagobj_t *p) { (void)p; }
void I_StopMenuFire(void) { }
void I_DrawMenuFire(void) { }
