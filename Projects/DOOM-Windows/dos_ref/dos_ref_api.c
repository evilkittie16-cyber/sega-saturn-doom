/* dos_ref_api.c - Isolated DOS DOOM Reference Renderer API DLL */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>

#include "doomdef.h"
#include "doomstat.h"
#include "info.h"
#include "r_local.h"
#include "p_local.h"
#include "p_setup.h"
#include "w_wad.h"
#include "z_zone.h"
#include "v_video.h"
#include "r_sky.h"
#include "m_swap.h"

extern boolean setsizeneeded;
extern void R_ExecuteSetViewSize(void);
extern subsector_t *R_PointInSubsector(fixed_t x, fixed_t y);

#define DOS_EXPORT __declspec(dllexport)

/* Global Doom State Definitions */
boolean   nomonsters = false;
boolean   respawnparm = false;
boolean   fastparm = false;
boolean   devparm = false;
boolean   modifiedgame = false;
skill_t   gameskill = sk_medium;
int       gameepisode = 1;
int       gamemap = 1;
boolean   respawnmonsters = false;
boolean   netgame = false;
boolean   deathmatch = false;
boolean   automapactive = false;
boolean   menuactive = false;
boolean   paused = false;
int       consoleplayer = 0;
int       totalkills = 0;
int       totalitems = 0;
int       totalsecret = 0;
boolean   demoplayback = false;
int       gametic = 0;
player_t  players[MAXPLAYERS];
boolean   playeringame[MAXPLAYERS] = { true, false, false, false };
wbstartstruct_t wminfo;
boolean   precache = true;
int       bodyqueslot = 0;
int       detailLevel = 0;
int       screenblocks = 10;
char    **myargv = NULL;

/* OS / Engine Stubs */
void I_Error(char *error, ...)
{
    va_list argptr;
    va_start(argptr, error);
    fprintf(stderr, "DOS_REF ERROR: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n");
    va_end(argptr);
    exit(1);
}

void NetUpdate(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}

byte* I_AllocLow(int size)
{
    byte *mem = (byte *)malloc(size);
    if (mem) memset(mem, 0, size);
    return mem;
}

byte *I_ZoneBase(int *size)
{
    *size = 16 * 1024 * 1024;
    byte *ptr = (byte *)malloc(*size);
    if (!ptr) {
        fprintf(stderr, "[dos_ref] Failed to allocate 16MB zone memory\n");
        exit(1);
    }
    return ptr;
}

int I_GetTime(void)
{
    static int s_tic = 0;
    return ++s_tic;
}

void I_BaseTiccmd(ticcmd_t *cmd)
{
    if (cmd) memset(cmd, 0, sizeof(*cmd));
}

void I_WaitVBL(int count) { (void)count; }
void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

/* Gameplay & UI Stubs */
void G_ExitLevel(void) {}
void G_SecretExitLevel(void) {}
void G_PlayerReborn(int player) { (void)player; }
void G_DeathMatchSpawnPlayer(int player) { (void)player; }
void AM_Stop(void) {}
void ST_Start(void) {}
void HU_Start(void) {}
void S_Start(void) {}
int  M_CheckParm(char *check) { (void)check; return 0; }

/* Sound Stubs */
void S_StartSound(void *origin, int sfx_id) { (void)origin; (void)sfx_id; }
void S_StopSound(void *origin) { (void)origin; }
void S_ChangeMusic(int music_id, int looping) { (void)music_id; (void)looping; }
void S_StopMusic(void) {}
void S_PauseSound(void) {}
void S_ResumeSound(void) {}
void S_UpdateSounds(void *listener) { (void)listener; }
void S_SetMusicVolume(int volume) { (void)volume; }
void S_SetSfxVolume(int volume) { (void)volume; }
void S_Init(int sfxVolume, int musicVolume) { (void)sfxVolume; (void)musicVolume; }

int ticdup = 1;

/* Camera and State */
static int s_current_map = -1;
static int s_initialized = 0;
static const uint8_t *s_playpal = NULL;

DOS_EXPORT int DOS_Init(const char *wad_path)
{
    if (s_initialized) return 1;

    _set_fmode(_O_BINARY);

    printf("[dos_ref] Initializing Zone memory...\n");
    Z_Init();

    printf("[dos_ref] Loading WAD: %s\n", wad_path);
    char *files[2];
    files[0] = (char *)wad_path;
    files[1] = NULL;
    W_InitMultipleFiles(files);

    printf("[dos_ref] Initializing R_Init...\n");
    gamemode = registered; /* default to registered DOOM 1 */
    language = english;

    /* Allocate screens buffers: 4 screens of 320x200 */
    screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    screens[1] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    screens[2] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    screens[3] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);

    R_Init();

    printf("[dos_ref] Initializing Game structures...\n");
    nomonsters = false;

    extern void P_InitSwitchList(void);
    extern void P_InitPicAnims(void);
    extern void R_InitSprites(char **namelist);
    extern char *sprnames[NUMSPRITES];

    printf("[dos_ref] P_InitSwitchList...\n");
    P_InitSwitchList();
    printf("[dos_ref] P_InitPicAnims...\n");
    P_InitPicAnims();

    printf("[dos_ref] R_InitSprites (NUMSPRITES=%d)...\n", NUMSPRITES);
    static char *s_safe_sprnames[NUMSPRITES + 1];
    for (int i = 0; i < NUMSPRITES; i++) {
        s_safe_sprnames[i] = sprnames[i];
    }
    s_safe_sprnames[NUMSPRITES] = NULL;
    R_InitSprites(s_safe_sprnames);
    printf("[dos_ref] R_InitSprites finished successfully!\n");

    s_playpal = (const uint8_t *)W_CacheLumpName("PLAYPAL", PU_STATIC);

    /* Initialize sky flat and sky texture defaults */
    extern int skyflatnum;
    extern int skytexture;
    extern int skytexturemid;
    skyflatnum = R_FlatNumForName(SKYFLATNAME);
    skytexturemid = 100 * FRACUNIT;
    skytexture = R_TextureNumForName("SKY1");

    memset(players, 0, sizeof(players));
    players[0].viewheight = 41 * FRACUNIT;
    players[0].playerstate = PST_LIVE;

    s_initialized = 1;
    printf("[dos_ref] Initialization complete!\n");
    return 1;
}

DOS_EXPORT void DOS_LoadMap(int map)
{
    if (!s_initialized) return;
    if (map == s_current_map) return;

    int ep = 1;
    int m = map;
    if (map >= 100) {
        ep = map / 100;
        m = map % 100;
    }

    printf("[dos_ref] Loading Level E%dM%d...\n", ep, m);
    gameepisode = ep;
    gamemap = m;

    /* Set up sky mapping for the active episode / map */
    skyflatnum = R_FlatNumForName(SKYFLATNAME);
    skytexturemid = 100 * FRACUNIT;
    if (gamemode == commercial) {
        skytexture = R_TextureNumForName("SKY3");
        if (gamemap < 12)
            skytexture = R_TextureNumForName("SKY1");
        else if (gamemap < 21)
            skytexture = R_TextureNumForName("SKY2");
    } else {
        switch (gameepisode) {
            case 1: skytexture = R_TextureNumForName("SKY1"); break;
            case 2: skytexture = R_TextureNumForName("SKY2"); break;
            case 3: skytexture = R_TextureNumForName("SKY3"); break;
            case 4: skytexture = R_TextureNumForName("SKY4"); break;
            default: skytexture = R_TextureNumForName("SKY1"); break;
        }
    }
    printf("[dos_ref] Sky initialized: flat=%d ('%s') texture=%d ('SKY%d') mid=%d\n",
           skyflatnum, SKYFLATNAME, skytexture, gameepisode, skytexturemid >> FRACBITS);

    P_SetupLevel(ep, m, 0, sk_medium);
    s_current_map = map;
    printf("[dos_ref] Level E%dM%d loaded! numvertexes=%d numsegs=%d numsectors=%d\n",
           ep, m, numvertexes, numsegs, numsectors);
}

DOS_EXPORT void DOS_LoadMapEx(int episode, int map)
{
    DOS_LoadMap((episode * 100) + map);
}

DOS_EXPORT void DOS_SetCamera(int map, int x, int y, int z, unsigned int angle, int viewheight)
{
    if (map != s_current_map) {
        DOS_LoadMap(map);
    }

    if (!players[0].mo) {
        players[0].mo = P_SpawnMobj(x, y, z, MT_PLAYER);
        players[0].mo->player = &players[0];
    }
    players[0].mo->x = x;
    players[0].mo->y = y;
    players[0].mo->z = z;
    players[0].mo->angle = angle;
    players[0].mo->subsector = R_PointInSubsector(x, y);

    players[0].viewz = z;
    players[0].viewheight = 41 * FRACUNIT;

    /* Align view size: 10 = 320x168 (status bar mode), 11 = 320x200 (fullscreen) */
    if (viewheight == 168) {
        R_SetViewSize(10, 0);
    } else {
        R_SetViewSize(11, 0);
    }
}

DOS_EXPORT void DOS_SetWeapon(int readyweapon, int weapon_state, int weapon_sx, int weapon_sy,
                              int flash_state, int flash_sx, int flash_sy, int extralight)
{
    if (readyweapon >= 0 && readyweapon < NUMWEAPONS)
        players[0].readyweapon = (weapontype_t)readyweapon;
    players[0].extralight = extralight;

    /*
     * Coordinate Alignment:
     * In Saturn DOOM (saturn_hw_render.c):
     *   y0 = 2 + (psp->sy >> FRACBITS) - tofs;
     * In PC DOOM (r_things.c):
     *   vis->texturemid = (BASEYCENTER << FRACBITS) + FRACUNIT/2 - (psp->sy - tofs);
     *   sprtopscreen = centeryfrac - FixedMul(vis->texturemid, vis->scale);
     *   topscreen = sprtopscreen;
     *   dc_yl = (topscreen + FRACUNIT - 1) >> FRACBITS;
     * With BASEYCENTER = 100, spryscale = FRACUNIT:
     *   topscreen = (centery - 100) * FRACUNIT + psp->sy - tofs;
     * To make topscreen == (2 * FRACUNIT + saturn_sy - tofs):
     *   (centery - 100) * FRACUNIT + pc_sy = 2 * FRACUNIT + saturn_sy
     *   pc_sy = saturn_sy + (100 - centery + 2) * FRACUNIT;
     */
    int y_adj = (100 - centery + 2) * FRACUNIT;

    if (weapon_state > 0 && weapon_state < NUMSTATES) {
        players[0].psprites[ps_weapon].state = &states[weapon_state];
        players[0].psprites[ps_weapon].sx = weapon_sx;
        players[0].psprites[ps_weapon].sy = weapon_sy + y_adj;
    } else {
        players[0].psprites[ps_weapon].state = NULL;
    }

    if (flash_state > 0 && flash_state < NUMSTATES) {
        players[0].psprites[ps_flash].state = &states[flash_state];
        players[0].psprites[ps_flash].sx = flash_sx;
        players[0].psprites[ps_flash].sy = flash_sy + y_adj;
    } else {
        players[0].psprites[ps_flash].state = NULL;
    }
}

typedef struct {
    int x, y, z;
    unsigned int angle;
    char name[4];
    int frame;
    int flags;
    int sector_idx;
} dos_sync_mobj_t;

#define MAX_SYNC_MOBJS 1024
static mobj_t s_sync_mobjs_pool[MAX_SYNC_MOBJS];

DOS_EXPORT void DOS_SyncMobjs(const dos_sync_mobj_t *mobjs, int count)
{
    if (!s_initialized || !sectors || numsectors <= 0) return;

    /* Detach all mobjs from sectors so only live synced mobjs render */
    for (int i = 0; i < numsectors; i++) {
        sectors[i].thinglist = NULL;
    }

    if (count <= 0 || !mobjs) return;
    if (count > MAX_SYNC_MOBJS) count = MAX_SYNC_MOBJS;

    extern char *sprnames[NUMSPRITES];

    for (int i = 0; i < count; i++) {
        mobj_t *m = &s_sync_mobjs_pool[i];
        memset(m, 0, sizeof(*m));

        m->x = mobjs[i].x;
        m->y = mobjs[i].y;
        m->z = mobjs[i].z;
        m->angle = mobjs[i].angle;
        m->frame = mobjs[i].frame;
        m->flags = mobjs[i].flags;

        /* Look up sprite index by 4-letter name in PC Doom sprnames table */
        m->sprite = SPR_POSS;
        for (int s = 0; s < NUMSPRITES; s++) {
            if (strncmp(mobjs[i].name, sprnames[s], 4) == 0) {
                m->sprite = (spritenum_t)s;
                break;
            }
        }

        int sec = mobjs[i].sector_idx;
        if (sec >= 0 && sec < numsectors) {
            m->snext = sectors[sec].thinglist;
            sectors[sec].thinglist = m;
        }
    }
}

typedef struct {
    int floorheight;
    int ceilingheight;
    short lightlevel;
} dos_sync_sector_t;

DOS_EXPORT void DOS_SyncSectors(const dos_sync_sector_t *sec_list, int count)
{
    if (!s_initialized || !sectors || numsectors <= 0 || !sec_list) return;
    if (count > numsectors) count = numsectors;
    for (int i = 0; i < count; i++) {
        sectors[i].floorheight   = sec_list[i].floorheight;
        sectors[i].ceilingheight = sec_list[i].ceilingheight;
        sectors[i].lightlevel    = sec_list[i].lightlevel;
    }
}

/* ------------------------------------------------------------------
 * Status bar (classic DOS STBAR layout, st_stuff.c coordinates minus ST_Y)
 * The caller fills dos_status_t from the Saturn player each frame; the bar
 * is composed into a 320x32 strip in screens[1] and copied into the output
 * at stbar_y (168 in DOS view mode, 192 in Saturn full view mode) so it sits
 * at the same rows as the Saturn pane's HUD. Weapon/ammo indices here are
 * already converted to PC DOOM order by the caller.
 * ------------------------------------------------------------------ */
typedef struct {
    int health;            /* mo->health */
    int armor;             /* armorpoints */
    int ready_ammo;        /* ammo count of the ready weapon, -1 = none (fist/saw) */
    int ammo[4];           /* clip, shell, cell, misl */
    int maxammo[4];
    int arms[6];           /* slots 2..7 owned (pistol, shotgun, chaingun, rocket, plasma, bfg) */
    int cards[6];          /* blue/yellow/red card, blue/yellow/red skull */
    int god;               /* god mode cheat active */
} dos_status_t;

static dos_status_t s_status;
static int          s_status_valid = 0;
static int          s_stbar_y = 168;

static patch_t *s_p_stbar, *s_p_starms, *s_p_tallnum[10], *s_p_tallpct;
static patch_t *s_p_shortnum[10], *s_p_graynum[10], *s_p_keys[6];
static patch_t *s_p_faces[5], *s_p_god, *s_p_dead;
static int      s_st_loaded = 0;

static patch_t *ST_Lump(const char *name)
{
    int lump = W_CheckNumForName((char *)name);
    return (lump >= 0) ? (patch_t *)W_CacheLumpNum(lump, PU_STATIC) : NULL;
}

static void ST_LoadPatches(void)
{
    char name[9];
    if (s_st_loaded) return;
    s_p_stbar  = ST_Lump("STBAR");
    s_p_starms = ST_Lump("STARMS");
    s_p_tallpct = ST_Lump("STTPRCNT");
    for (int i = 0; i < 10; i++) {
        sprintf(name, "STTNUM%d", i);  s_p_tallnum[i]  = ST_Lump(name);
        sprintf(name, "STYSNUM%d", i); s_p_shortnum[i] = ST_Lump(name);
        sprintf(name, "STGNUM%d", i);  s_p_graynum[i]  = ST_Lump(name);
    }
    for (int i = 0; i < 6; i++) {
        sprintf(name, "STKEYS%d", i);  s_p_keys[i] = ST_Lump(name);
    }
    for (int i = 0; i < 5; i++) {
        sprintf(name, "STFST%d1", i);  s_p_faces[i] = ST_Lump(name);  /* straight-ahead face */
    }
    s_p_god  = ST_Lump("STFGOD0");
    s_p_dead = ST_Lump("STFDEAD0");
    s_st_loaded = 1;
}

static void ST_Patch(int x, int y, patch_t *p)
{
    if (p) V_DrawPatch(x, y, 1, p);
}

/* STlib_drawNum: right-aligned at x, up to 'digits' digits, 0 draws one zero. */
static void ST_Num(int x, int y, int num, int digits, patch_t **font)
{
    if (!font[0]) return;
    int w = SHORT(font[0]->width);
    if (num < 0) num = 0;
    if (num == 0) { ST_Patch(x - w, y, font[0]); return; }
    while (num && digits--) {
        x -= w;
        ST_Patch(x, y, font[num % 10]);
        num /= 10;
    }
}

static void ST_DrawBar(void)
{
    ST_LoadPatches();
    memset(screens[1], 0, SCREENWIDTH * 32);

    ST_Patch(0, 0, s_p_stbar);
    ST_Patch(104, 0, s_p_starms);

    /* Ready-weapon ammo, health %, armor % (tall red digits) */
    if (s_status.ready_ammo >= 0)
        ST_Num(44, 3, s_status.ready_ammo, 3, s_p_tallnum);
    ST_Num(90, 3, s_status.health, 3, s_p_tallnum);
    ST_Patch(90, 3, s_p_tallpct);
    ST_Num(221, 3, s_status.armor, 3, s_p_tallnum);
    ST_Patch(221, 3, s_p_tallpct);

    /* Arms 2..7: yellow if owned, gray otherwise */
    for (int i = 0; i < 6; i++) {
        patch_t *p = s_status.arms[i] ? s_p_shortnum[i + 2] : s_p_graynum[i + 2];
        ST_Patch(111 + (i % 3) * 12, 4 + (i / 3) * 10, p);
    }

    /* Face: dead / god / straight face by pain level (ST_calcPainOffset) */
    {
        patch_t *face;
        if (s_status.health <= 0) {
            face = s_p_dead;
        } else if (s_status.god) {
            face = s_p_god;
        } else {
            int h = s_status.health > 100 ? 100 : s_status.health;
            int pain = ((100 - h) * 5) / 101;
            face = s_p_faces[pain];
        }
        ST_Patch(143, 0, face);
    }

    /* Keys: skull overrides card in the same slot */
    for (int i = 0; i < 3; i++) {
        int k = -1;
        if (s_status.cards[i])     k = i;
        if (s_status.cards[i + 3]) k = i + 3;
        if (k >= 0) ST_Patch(239, 3 + i * 10, s_p_keys[k]);
    }

    /* Ammo / max ammo table (small yellow digits). On-screen rows top to
     * bottom are bull, shel, rckt, cell, so cell and misl swap rows. */
    {
        static const int ammo_y[4] = { 5, 11, 23, 17 };   /* clip, shell, cell, misl */
        for (int i = 0; i < 4; i++) {
            ST_Num(288, ammo_y[i], s_status.ammo[i], 3, s_p_shortnum);
            ST_Num(314, ammo_y[i], s_status.maxammo[i], 3, s_p_shortnum);
        }
    }
}

DOS_EXPORT void DOS_SetStatus(const dos_status_t *st, int stbar_y)
{
    if (!st) { s_status_valid = 0; return; }
    s_status = *st;
    s_stbar_y = stbar_y;
    s_status_valid = 1;
}

DOS_EXPORT void DOS_RenderFrame(uint32_t *out_rgba_352x224)
{
    if (!s_initialized || !s_playpal || !out_rgba_352x224) return;

    if (setsizeneeded) {
        R_ExecuteSetViewSize();
    }

    /* Render classic player view */
    R_RenderPlayerView(&players[0]);

    /* Convert 8-bit screens[0] (320x200) to 352x224 RGBA.
     * Horizontal centering: 16 pixels left border, 16 pixels right border.
     * Rows 0..167 are the 3D viewport.
     * Rows 168..199 are status bar (or black in reference renderer if not drawn).
     * Rows 200..223 are black bottom border.
     */
    memset(out_rgba_352x224, 0, 352 * 224 * sizeof(uint32_t));

    const byte *src = screens[0];
    const uint8_t *pal = s_playpal; /* 256 RGB triplets */

    for (int y = 0; y < SCREENHEIGHT && y < 224; y++) {
        uint32_t *dst_row = out_rgba_352x224 + y * 352 + 16;
        const byte *src_row = src + y * 320;
        for (int x = 0; x < 320; x++) {
            byte col_idx = src_row[x];
            uint8_t r = pal[col_idx * 3 + 0];
            uint8_t g = pal[col_idx * 3 + 1];
            uint8_t b = pal[col_idx * 3 + 2];
            /* Standard DIB BI_RGB: 0x00RRGGBB */
            dst_row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    /* Overlay the status bar at the same rows as the Saturn HUD */
    if (s_status_valid && s_stbar_y >= 0 && s_stbar_y + 32 <= 224) {
        ST_DrawBar();
        for (int y = 0; y < 32; y++) {
            uint32_t *dst_row = out_rgba_352x224 + (s_stbar_y + y) * 352 + 16;
            const byte *src_row = screens[1] + y * SCREENWIDTH;
            for (int x = 0; x < 320; x++) {
                const uint8_t *c = pal + src_row[x] * 3;
                dst_row[x] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
            }
        }
    }
}

DOS_EXPORT const uint8_t *DOS_GetPalette(void)
{
    return s_playpal;
}
