/* saturn_hud.c — the classic DOOM status bar, drawn from the rerelease
 * WAD's PC-format STBAR art + STTNUM/STGNUM/STYSNUM/AMMNUM digits, STKEYS
 * key icons and the STF* face set through the saturn_vgl.c blitter.
 *
 * The 3D view ends at VIEW_BOT 192 (saturn_render.c), so rows 192-223
 * are exclusively ours: a full-bar redraw every frame needs no erase
 * logic (nobody else writes those rows — the letterbox memsets use
 * VIEW_BOT).
 *
 * Overlay geometry is linuxdoom-1.10 st_stuff.c's ST_* block, Y-shifted
 * +24 into the fb's 224-line window (2026-09-06 full-screen layout:
 * the bar sits at fb rows 192-223, flush against the physical bottom
 * and connected to the view's bottom edge; X coordinates are verbatim).
 * d32xr keeps player->health in sync with mo->health during
 * levels (p_inter.c), so players[0].health is the live value.
 */
#include "saturn.h"
#include "st_main.h"
#include "i_video.h"
#include "saturn_vgl.h"
#include "doomdef.h"
#include "r_local.h"

/* s_sound.c (no header prototype in this tree) */
extern void S_StartSound(void *origin, int sfx_id);

saturn_hud_diag_t g_saturn_hud_diag = { 0 };

/* Bar top row in the 224-line fb:
 * Option A (DOS):  ST_BAR_Y = 168 (flush with 320x168 3D view, rows 168..199, rows 200..223 black border)
 * Option B (Full): ST_BAR_Y = 192 (flush with 320x192 3D view, rows 192..223) */
extern int saturn_stbar_y;
#define ST_BAR_Y    saturn_stbar_y

/* ---- layout constants ----
 * Verbatim linuxdoom-1.10 st_stuff.c ST_* coordinates with ST_Y (168)
 * replaced by ST_BAR_Y. DOOM1.WAD carries the PC STBAR art, so the PC
 * positions are the correct ones. Numbers are right-aligned at their X
 * (STlib_drawNum), and every patch is placed with its WAD left/top
 * offsets applied (hud_patch), exactly like V_DrawPatch. */
#define ST_AMMOX     44       /* right edge, 3 digits */
#define ST_AMMOY     (ST_BAR_Y + 3)
#define ST_HEALTHX   90       /* right edge; STTPRCNT drawn at the same x */
#define ST_HEALTHY   (ST_BAR_Y + 3)
#define ST_ARMORX    221
#define ST_ARMORY    (ST_BAR_Y + 3)
#define ST_ARMSXSPACE 12
#define ST_ARMSYSPACE 10
#define ST_ARMSBGX   104
#define ST_ARMSBGY   ST_BAR_Y
#define ST_WEAPON0X  111
#define ST_WEAPON0Y  (ST_BAR_Y + 4)
#define ST_FX        143      /* face patches carry their own offsets */
#define ST_FY        ST_BAR_Y
#define ST_KEY0X     239
#define ST_KEY0Y     (ST_BAR_Y + 3)
#define ST_KEY1Y     (ST_BAR_Y + 13)
#define ST_KEY2Y     (ST_BAR_Y + 23)
#define ST_AMMO0X    288      /* per-type ammo counts, right edge */
#define ST_MAXAMMOX  314      /* max ammo, right edge */

/* PC layout: rows 173/179/185/191 = clip/shells/rockets/cells.
 * d32xr's ammotype_t orders cell(2) before misl(3) — remap. */
static const unsigned char hud_ammo_offset[NUMAMMO] = {
    5,   /* am_clip  */
    11,  /* am_shell */
    23,  /* am_cell  (PC ST_AMMO2Y)  */
    17   /* am_misl  (PC ST_AMMO3Y)  */
};

/* ARMS box numbers 2-7 = PC weapons 1-6 -> d32xr indices. Box 3
 * (slot 1) lights for shotgun OR super shotgun (d32xr st_main.c:
 * ind+1 == wp_shotgun -> shotgun || ssg), so ssg(3) needs no slot
 * of its own. */
static const unsigned char hud_arms_weapon[6] = {
    1, 2, 4, 5, 6, 7          /* pistol shotgun chaingun missile plasma bfg */
};

/* ---- cached art ---- */
static const void *hud_stbar;
static const void *hud_armsbg;        /* STARMS panel bg (boxes+label) */
static byte       *hud_stbar_linear = NULL;
static const void *hud_tallnum[10];   /* STTNUM0-9, 14x16 red */
static const void *hud_tallminus;     /* STTMINUS             */
static const void *hud_tallpct;       /* STTPRCNT             */
static const void *hud_graynum[10];   /* STGNUM0-9            */
static const void *hud_ylwnum[10];    /* STYSNUM0-9           */
static const void *hud_keys[6];       /* STKEYS0-5            */

static void hud_patch(int x, int y, const void *patch);

/* ---- message line ----
 * d32xr's gameplay code sets player->message in ~40 places but the
 * HU module was cut. We own the whole lifecycle like vanilla's
 * HU_Ticker: consume the pointer (so a repeat pickup of the same
 * literal re-triggers), hold it on screen HU_MSGTIME render frames,
 * then fade out by simply stopping. Drawn with Saturn_Print (the
 * WAD's red STCFN HU font; lowercase folds onto uppercase). */
#define HU_MSGTIME   100
static const char *hud_msg;
static int         hud_msg_tics;

static void hud_message(player_t *p)
{
    const char *m = p->message;

    if (m)
    {
        p->message   = NULL;        /* consumed (vanilla HU_Ticker) */
        hud_msg      = m;
        hud_msg_tics = HU_MSGTIME;
    }
    if (hud_msg_tics > 0 && hud_msg)
    {
        hud_msg_tics--;
        Saturn_Print(2, 2, hud_msg);
    }
}

void Saturn_HUD_Init(void)
{
    static const char *const talls[10] = {
        "STTNUM0", "STTNUM1", "STTNUM2", "STTNUM3", "STTNUM4",
        "STTNUM5", "STTNUM6", "STTNUM7", "STTNUM8", "STTNUM9"
    };
    static const char *const grays[10] = {
        "STGNUM0", "STGNUM1", "STGNUM2", "STGNUM3", "STGNUM4",
        "STGNUM5", "STGNUM6", "STGNUM7", "STGNUM8", "STGNUM9"
    };
    static const char *const ylws[10] = {
        "STYSNUM0", "STYSNUM1", "STYSNUM2", "STYSNUM3", "STYSNUM4",
        "STYSNUM5", "STYSNUM6", "STYSNUM7", "STYSNUM8", "STYSNUM9"
    };
    static const char *const keyn[6] = {
        "STKEYS0", "STKEYS1", "STKEYS2", "STKEYS3", "STKEYS4", "STKEYS5"
    };
    int i;

    hud_stbar     = Saturn_CachePatch("STBAR");
    hud_armsbg    = Saturn_CachePatch("STARMS");
    hud_tallminus = Saturn_CachePatch("STTMINUS");
    hud_tallpct   = Saturn_CachePatch("STTPRCNT");
    for (i = 0; i < 10; i++)
    {
        hud_tallnum[i] = Saturn_CachePatch(talls[i]);
        hud_graynum[i] = Saturn_CachePatch(grays[i]);
        hud_ylwnum[i]  = Saturn_CachePatch(ylws[i]);
    }
    for (i = 0; i < 6; i++)
        hud_keys[i] = Saturn_CachePatch(keyn[i]);

    /* Pre-decode static status bar background into a linear 320x32 buffer */
    {
        extern void *jo_malloc_with_behaviour(unsigned int n, const int behaviour);
        extern byte *I_VideoBuffer;

        if (!hud_stbar_linear)
            hud_stbar_linear = (byte *)jo_malloc_with_behaviour(320 * 32, 0);

        if (hud_stbar_linear && I_VideoBuffer)
        {
            byte *saved_fb = I_VideoBuffer;
            I_VideoBuffer = hud_stbar_linear - ST_BAR_Y * 320;
            hud_patch(0, ST_BAR_Y, hud_stbar);
            hud_patch(ST_ARMSBGX, ST_ARMSBGY, hud_armsbg);
            I_VideoBuffer = saved_fb;
        }
    }
    {
        extern void Saturn_Vgl_MarkBootDone(void);
        Saturn_Vgl_MarkBootDone();
    }
}

/* Patch header fields (little-endian WAD data, read bytewise). */
static int hud_patch_w(const void *patch)
{
    const byte *p = (const byte *)patch;
    return p[0] | (p[1] << 8);
}

/* V_DrawPatch equivalent: Saturn_DrawPatch* place the patch's top-left
 * corner at (x,y) and ignore the WAD offsets, so subtract them here. */
static void hud_patch(int x, int y, const void *patch)
{
    const byte *p = (const byte *)patch;
    short lo, to;

    if (!patch)
        return;
    lo = (short)(p[4] | (p[5] << 8));
    to = (short)(p[6] | (p[7] << 8));
    Saturn_DrawPatchFS(x - lo, y - to, patch);
}

/* STlib_drawNum: right-aligned at x, at most 3 digits, fixed pitch =
 * width of the '0' glyph, a lone 0 for zero. Negative values clamp to 0
 * (d32xr clamps player health at 0, armor/ammo never go negative). */
static void hud_drawnum(int x, int y, int value, const void *const *font)
{
    int w, digits = 3;

    if (!font[0])
        return;
    w = hud_patch_w(font[0]);
    if (value < 0)   value = 0;
    if (value > 999) value = 999;

    if (value == 0)
    {
        hud_patch(x - w, y, font[0]);
        return;
    }
    while (value && digits--)
    {
        x -= w;
        hud_patch(x, y, font[value % 10]);
        value /= 10;
    }
}

/* ---- face ----
 * STFST{class}{0-2} straight (idle rotation), STFTL/STFTR{c}0 turns,
 * STFOUCH/STFEVL/STFKILL{c} specials, STFGOD0/STFDEAD0. The special
 * face shows ~1 s; tryopen[] flashes a key icon then must be cleared
 * by US (d32xr's ST_Ticker is a stub).
 * Priority mirrors d32xr st_main.c: god > dead > special > straight.
 * Face class: STFST00 is the 100% face, so class = 4 - health/20
 * (the d32xr sheet is ordered the same way: base = 4 - health/20). */
static int hud_face_frame = 0;
static int hud_special_left = 0;

/* All STF* faces are positioned by their own WAD offsets from (143,ST_Y),
 * as in st_stuff.c's w_faces widget. */
static void hud_draw_face(const void *patch)
{
    hud_patch(ST_FX, ST_FY, patch);
}

static void hud_face(int health)
{
    stbar_t *sb = stbar;
    char     name[9];
    int      c;
    const void *glyph = 0;

    if (health > 100) health = 100;

    c = health / 20;                    /* 100/20 = 5 -> cap 4 */
    if (c > 4) c = 4;
    c = 4 - c;                          /* STFST00 = healthy */

    if (players[0].cheats & CF_GODMODE ||
        players[0].powers[pw_invulnerability] > 0)
    {
        glyph = Saturn_CachePatch("STFGOD0");
    }
    else if (health <= 0)
    {
        glyph = Saturn_CachePatch("STFDEAD0");
    }
    else if (sb && sb->specialFace)
    {
        switch (sb->specialFace)
        {
            case f_faceleft:            /* look left */
                name[0]='S';name[1]='T';name[2]='F';name[3]='T';
                name[4]='L';name[5]=(char)('0'+c);name[6]='0';name[7]=0;
                glyph = Saturn_CachePatch(name);
                break;
            case f_faceright:           /* look right */
                name[0]='S';name[1]='T';name[2]='F';name[3]='T';
                name[4]='R';name[5]=(char)('0'+c);name[6]='0';name[7]=0;
                glyph = Saturn_CachePatch(name);
                break;
            case f_hurtbad:             /* ouch */
                name[0]='S';name[1]='T';name[2]='F';name[3]='O';
                name[4]='U';name[5]='C';name[6]='H';name[7]=(char)('0'+c);
                name[8]=0;
                glyph = Saturn_CachePatch(name);
                break;
            case f_gotgat:              /* evil grin */
                name[0]='S';name[1]='T';name[2]='F';name[3]='E';
                name[4]='V';name[5]='L';name[6]=(char)('0'+c);name[7]=0;
                glyph = Saturn_CachePatch(name);
                break;
            case f_mowdown:             /* rampage */
                name[0]='S';name[1]='T';name[2]='F';name[3]='K';
                name[4]='I';name[5]='L';name[6]='L';name[7]=(char)('0'+c);
                name[8]=0;
                glyph = Saturn_CachePatch(name);
                break;
            default:
                break;
        }
    }

    if (!glyph)
    {
        /* straight face, rotating every ~2 s (120 frames) */
        int idx = (hud_face_frame / 120) % 3;

        name[0]='S';name[1]='T';name[2]='F';name[3]='S';name[4]='T';
        name[5]=(char)('0'+c);name[6]=(char)('0'+idx);name[7]=0;
        glyph = Saturn_CachePatch(name);
    }

    if (glyph)
        hud_draw_face(glyph);
}

/* Called from Saturn_G_Display, after the 3D render has letterboxed
 * down to VIEW_BOT. Full redraw every frame — rows 192-223 belong to
 * us alone. */
/* Tiny 3x5 debug font for the FPS / perf readout. Rows are 3-bit masks
 * (bit 2 = left column). Characters outside the table draw blank. */
static const unsigned char *hud_tiny_glyph(char c)
{
    static const unsigned char glyphs[40][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 },
        { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 },
        { 7, 5, 7, 1, 7 }, /* 0..9 (0..9) */
        { 7, 4, 6, 4, 4 }, /* 10: F */
        { 7, 5, 7, 4, 4 }, /* 11: P */
        { 7, 4, 7, 1, 7 }, /* 12: S */
        { 7, 2, 2, 2, 2 }, /* 13: T */
        { 6, 5, 6, 5, 5 }, /* 14: R */
        { 5, 5, 5, 5, 2 }, /* 15: V */
        { 7, 4, 4, 4, 7 }, /* 16: C */
        { 6, 5, 5, 5, 6 }, /* 17: D */
        { 7, 5, 5, 7, 1 }, /* 18: Q */
        { 0, 2, 0, 2, 0 }, /* 19: : */
        { 0, 0, 0, 0, 2 }, /* 20: . */
        { 5, 5, 2, 5, 5 }, /* 21: X */
        { 6, 5, 6, 5, 6 }, /* 22: B */
        { 7, 4, 5, 5, 7 }, /* 23: G */
        { 5, 6, 4, 6, 5 }, /* 24: K */
        { 5, 5, 5, 7, 5 }, /* 25: W */
        { 4, 4, 4, 4, 7 }, /* 26: L */
        { 5, 5, 7, 5, 5 }, /* 27: H */
        { 7, 4, 7, 4, 7 }, /* 28: E */
        { 1, 1, 1, 5, 2 }, /* 29: J */
        { 7, 5, 7, 5, 5 }, /* 30: A */
        { 5, 7, 5, 5, 5 }, /* 31: M */
        { 6, 5, 5, 5, 5 }, /* 32: N */
        { 7, 5, 5, 5, 7 }, /* 33: O */
        { 7, 2, 2, 2, 7 }, /* 34: I */
        { 5, 5, 5, 5, 7 }, /* 35: U */
        { 0, 0, 7, 0, 0 }, /* 36: - */
        { 1, 2, 2, 4, 4 }, /* 37: / */
        { 5, 5, 2, 2, 2 }, /* 38: Y */
        { 7, 1, 2, 4, 7 }, /* 39: Z */
    };
    /* Direct ASCII 0x20..0x5F lookup (-1 = blank) */
    static const signed char glyph_lut[64] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 36, 20, 37, /* 0x20..0x2F: '-', '.', '/' */
         0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 19, -1, -1, -1, -1, -1, /* 0x30..0x3F: '0'..'9', ':' */
        -1, 30, 22, 16, 17, 28, 10, 23, 27, 34, 29, 24, 26, 31, 32, 33, /* 0x40..0x4F: '@','A'..'O' */
        11, 18, 14, 12, 13, 35, 15, 25, 21, 38, 39, -1, -1, -1, -1, -1  /* 0x50..0x5F: 'P'..'Z' */
    };
    unsigned int u = (unsigned int)(unsigned char)c - 0x20u;
    if (u >= 64u)
        return NULL;
    int idx = glyph_lut[u];
    return (idx >= 0) ? glyphs[idx] : NULL;
}

/* Clear a (w+2) x 7 black box at (x-1, y-1) and draw `s` in PLAYPAL 231,
 * 4 px per character, clipped to the box. */
static void hud_tiny(int x, int y, int w, const char *s)
{
    int ry;
    byte *base_clear = I_VideoBuffer + (y - 1) * DOOM_SCREENWIDTH + (x - 1);

    for (ry = -1; ry < 6; ry++, base_clear += DOOM_SCREENWIDTH)
    {
        byte *row = base_clear;
        int rem = w + 2;
        while (((uintptr_t)row & 3u) && rem > 0)
        {
            *row++ = 0;
            rem--;
        }
        uint32_t *row32 = (uint32_t *)row;
        while (rem >= 16)
        {
            row32[0] = 0;
            row32[1] = 0;
            row32[2] = 0;
            row32[3] = 0;
            row32 += 4;
            rem -= 16;
        }
        while (rem >= 4)
        {
            *row32++ = 0;
            rem -= 4;
        }
        row = (byte *)row32;
        while (rem-- > 0)
            *row++ = 0;
    }
    byte *base_glyph = I_VideoBuffer + y * DOOM_SCREENWIDTH + x;
    for (; *s && w >= 3; s++, base_glyph += 4, w -= 4)
    {
        const unsigned char *g = hud_tiny_glyph(*s);

        if (!g)
            continue;
        byte *prow = base_glyph;
        for (ry = 0; ry < 5; ry++, prow += DOOM_SCREENWIDTH)
        {
            unsigned char m = g[ry];
            if (m & 4) prow[0] = 231;
            if (m & 2) prow[1] = 231;
            if (m & 1) prow[2] = 231;
        }
    }
}

static int hud_tiny_num(char *buf, int n, int v)
{
    char tmp[5];
    int  t = 0;

    if (v < 0)
        v = 0;
    if (v > 9999)
        v = 9999;
    do
    {
        unsigned int q = ((unsigned int)v * 52429u) >> 19;
        tmp[t++] = (char)('0' + (v - (int)(q * 10u)));
        v = (int)q;
    } while (v && t < 4);
    while (t)
        buf[n++] = tmp[--t];
    return n;
}

/* Append "<tag><value>" (value 0..9999, no leading zeros) to buf at n. */
static int hud_tiny_tag(char *buf, int n, char tag, int v)
{
    buf[n++] = tag;
    return hud_tiny_num(buf, n, v);
}

static int hud_tiny_signed_tag(char *buf, int n, char tag, int v)
{
    char tmp[6];
    int  t = 0;

    buf[n++] = tag;
    if (v < 0)
    {
        buf[n++] = '-';
        v = -v;
    }
    if (v > 9999)
        v = 9999;
    do
    {
        unsigned int q = ((unsigned int)v * 52429u) >> 19;
        tmp[t++] = (char)('0' + (v - (int)(q * 10u)));
        v = (int)q;
    } while (v && t < 5);
    while (t)
        buf[n++] = tmp[--t];
    return n;
}


int saturn_show_fps = 1;
int saturn_show_perf = 1;
int saturn_stbar_dirty_latch = 2;
int saturn_perf_dirty_latch = 2;
int saturn_face_dirty_latch = 0;

void Saturn_HUD_Draw(void)
{
    player_t *p = &players[consoleplayer];
    if (debugmode == DEBUGMODE_NODRAW)
        return;

    hud_face_frame++;

    int i;
    int health = p->health;
    const void *sb;

    typedef struct {
        int16_t last_stbar_y;
        int16_t last_health;
        int16_t last_armor;
        int16_t last_ammo;
        int16_t last_weapon;
        int16_t last_face_anim;
        int16_t last_special_face;
        int8_t  last_cards[6];
        int8_t  last_owned[6];
        int16_t last_ammo_counts[4];
        int16_t last_maxammo_counts[4];
    } hud_cache_t;
    static hud_cache_t hc = { -1, -999, -999, -999, -999, -999, -999, {-1,-1,-1,-1,-1,-1}, {-1,-1,-1,-1,-1,-1}, {-1,-1,-1,-1}, {-1,-1,-1,-1} };

    if (stbar && stbar->specialFace)
    {
        if (stbar->specialFace != hc.last_special_face || hud_special_left <= 0)
            hud_special_left = 30;
        else if (--hud_special_left <= 0)
            stbar->specialFace = f_faceleft;
    }
    else
    {
        hud_special_left = 0;
    }

    extern volatile int saturn_frt_tic;
    int cur_ammo = (weaponinfo[p->readyweapon].ammo == am_noammo) ? -1 : p->ammo[weaponinfo[p->readyweapon].ammo];
    int cur_face_anim = (hud_face_frame / 120);
    int cur_special = stbar ? stbar->specialFace : 0;
    int stbar_dirty = (hc.last_stbar_y != ST_BAR_Y) ||
                      (health != hc.last_health) ||
                      (p->armorpoints != hc.last_armor) ||
                      (cur_ammo != hc.last_ammo) ||
                      (p->readyweapon != hc.last_weapon);
    int face_dirty = (cur_special != hc.last_special_face) ||
                     (cur_face_anim != hc.last_face_anim && (
#ifdef SATURN_WIN32
                         1
#else
                         saturn_frt_tic <= 4
#endif
                     ));

    if (!stbar_dirty)
    {
        for (i = 0; i < 6; i++)
        {
            int owned = (i == 1) ? (p->weaponowned[wp_shotgun] || p->weaponowned[wp_supershotgun])
                                 : p->weaponowned[hud_arms_weapon[i]];
            if (owned != hc.last_owned[i] || (int)p->cards[i] != hc.last_cards[i] || (stbar && stbar->tryopen[i]))
            {
                stbar_dirty = 1;
                break;
            }
        }
    }

    if (!stbar_dirty)
    {
        for (i = 0; i < NUMAMMO; i++)
        {
            if (p->ammo[i] != hc.last_ammo_counts[i] || p->maxammo[i] != hc.last_maxammo_counts[i])
            {
                stbar_dirty = 1;
                break;
            }
        }
    }

    if (!stbar_dirty && face_dirty && !hud_stbar_linear)
        stbar_dirty = 1;

    if (stbar_dirty)
    {
        saturn_stbar_dirty_latch = 2;
        if (hud_stbar_linear)
        {
            const uint32_t *s = (const uint32_t *)hud_stbar_linear;
            uint32_t       *d = (uint32_t *)(I_VideoBuffer + ST_BAR_Y * 320);
#ifdef SATURN_WIN32
            memcpy(d, s, 320 * 32);
#else
            int bursts = 320;
            __asm__ volatile (
            "1: \n\t"
            "mov.l  @%0+, r0 \n\t"
            "mov.l  @%0+, r1 \n\t"
            "mov.l  @%0+, r2 \n\t"
            "mov.l  @%0+, r3 \n\t"
            "mov.l  @%0+, r4 \n\t"
            "mov.l  @%0+, r5 \n\t"
            "mov.l  @%0+, r6 \n\t"
            "mov.l  @%0+, r7 \n\t"
            "mov.l  r0, @%1       \n\t"
            "mov.l  r1, @(4, %1)  \n\t"
            "mov.l  r2, @(8, %1)  \n\t"
            "mov.l  r3, @(12, %1) \n\t"
            "mov.l  r4, @(16, %1) \n\t"
            "mov.l  r5, @(20, %1) \n\t"
            "mov.l  r6, @(24, %1) \n\t"
            "mov.l  r7, @(28, %1) \n\t"
            "dt     %2            \n\t"
            "bf/s   1b            \n\t"
            "add    #32, %1       \n\t"
            : "+r"(s), "+r"(d), "+r"(bursts)
            :
            : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "t", "memory"
        );
#endif
    }
    else
    {
        sb = hud_stbar;
        if (sb)
            hud_patch(0, ST_BAR_Y, sb);
        if (hud_armsbg)
            hud_patch(ST_ARMSBGX, ST_ARMSBGY, hud_armsbg);
    }

    static int top_border_clean = 0;
    static int bot_border_clean = 0;
    if (hc.last_stbar_y != ST_BAR_Y)
    {
        saturn_stbar_dirty_latch = 2;
        hc.last_stbar_y = ST_BAR_Y;
        top_border_clean = 0;
        bot_border_clean = 0;
    }

    if (ST_BAR_Y < 192)
    {
        int bar_bottom = ST_BAR_Y + 32;
        if (bar_bottom < DOOM_FBHEIGHT && bot_border_clean < 2)
        {
            uint32_t *border = (uint32_t *)(I_VideoBuffer + bar_bottom * 320);
            int border_words = ((DOOM_FBHEIGHT - bar_bottom) * 320) / 4;
            int bi;
            for (bi = 0; bi < border_words; bi += 4)
            {
                border[bi + 0] = 0;
                border[bi + 1] = 0;
                border[bi + 2] = 0;
                border[bi + 3] = 0;
            }
            bot_border_clean++;
        }
    }
    else
    {
        /* In Option B (fullscreen), ensure rows 168..191 are black on mode change (double buffered) */
        if (top_border_clean < 2)
        {
            uint32_t *top_border = (uint32_t *)(I_VideoBuffer + 168 * 320);
            int border_words = ((192 - 168) * 320) / 4;
            int bi;
            for (bi = 0; bi < border_words; bi += 4)
            {
                top_border[bi + 0] = 0;
                top_border[bi + 1] = 0;
                top_border[bi + 2] = 0;
                top_border[bi + 3] = 0;
            }
            top_border_clean++;
        }
    }

    /* main ammo of the ready weapon; blank for fist/chainsaw (DOS) */
    {
        int am = weaponinfo[p->readyweapon].ammo;

        if (am != am_noammo)
            hud_drawnum(ST_AMMOX, ST_AMMOY, p->ammo[am], hud_tallnum);
    }

    health = p->health;
    hud_drawnum(ST_HEALTHX, ST_HEALTHY, health, hud_tallnum);
    hud_patch(ST_HEALTHX, ST_HEALTHY, hud_tallpct);
    hud_drawnum(ST_ARMORX, ST_ARMORY, p->armorpoints, hud_tallnum);
    hud_patch(ST_ARMORX, ST_ARMORY, hud_tallpct);

    for (i = 0; i < 6; i++)
    {
        const void *g;
        int owned;
        int x  = ST_WEAPON0X + (i % 3) * ST_ARMSXSPACE;
        int y  = ST_WEAPON0Y + (i / 3) * ST_ARMSYSPACE;

        if (i == 1)
            owned = p->weaponowned[wp_shotgun] ||
                    p->weaponowned[wp_supershotgun];
        else
            owned = p->weaponowned[hud_arms_weapon[i]];

        g = owned ? hud_ylwnum[i + 2] : hud_graynum[i + 2];
        hud_patch(x, y, g);
    }

    /* keys: one box per colour row, as in DOS — a skull key replaces
     * the card of the same colour (st_stuff.c keyboxes[]). tryopen
     * flashes on a locked-door attempt (p_doors.c) — d32xr's ST_Ticker
     * is a stub, so we own the blink-and-clear plus the itemup blip its
     * ticker played. */
    {
        int key_y[3] = { ST_KEY0Y, ST_KEY1Y, ST_KEY2Y };
        static int open_left[6];

        for (i = 0; i < 6; i++)
        {
            int flash = (stbar && stbar->tryopen[i]) != 0;
            int x     = ST_KEY0X;

            if (!p->cards[i] && !flash)
                continue;
            /* card hidden by the owned skull of the same colour */
            if (i < 3 && p->cards[i + 3] && !flash)
                continue;

            if (flash)
            {
                if (open_left[i] <= 0)
                {
                    open_left[i] = 32;
                    S_StartSound(NULL, sfx_itemup);
                }
                /* 4-frame blink while flashing */
                if (((hud_face_frame >> 2) & 1) == 0)
                    continue;
                if (--open_left[i] <= 0)
                    stbar->tryopen[i] = 0;
            }
            else
                open_left[i] = 0;

            hud_patch(x, key_y[i % 3], hud_keys[i]);
        }
    }

    /* per-type ammo counts + maxes, yellow STYSNUM (st_stuff.c w_ammo /
     * w_maxammo use shortnum[] = STYSNUM for both columns). */
    for (i = 0; i < NUMAMMO; i++)
    {
        int y = ST_BAR_Y + hud_ammo_offset[i];

        hud_drawnum(ST_AMMO0X, y, p->ammo[i], hud_ylwnum);
        hud_drawnum(ST_MAXAMMOX, y, p->maxammo[i], hud_ylwnum);
    }

    hud_face(health);

        hc.last_stbar_y = ST_BAR_Y;
        hc.last_health = health;
        hc.last_armor = p->armorpoints;
        hc.last_ammo = cur_ammo;
        hc.last_weapon = p->readyweapon;
        hc.last_face_anim = cur_face_anim;
        hc.last_special_face = cur_special;
        for (i = 0; i < 6; i++)
        {
            hc.last_owned[i] = (i == 1) ? (p->weaponowned[wp_shotgun] || p->weaponowned[wp_supershotgun])
                                         : p->weaponowned[hud_arms_weapon[i]];
            hc.last_cards[i] = (int8_t)p->cards[i];
        }
        for (i = 0; i < NUMAMMO; i++)
        {
            hc.last_ammo_counts[i] = p->ammo[i];
            hc.last_maxammo_counts[i] = p->maxammo[i];
        }
    }
    else if (face_dirty)
    {
        int ry;
        for (ry = 0; ry < 32; ry++)
        {
            const uint32_t *s = (const uint32_t *)(hud_stbar_linear + ry * 320 + 140);
            uint32_t       *d = (uint32_t *)(I_VideoBuffer + (ST_BAR_Y + ry) * 320 + 140);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4];
            d[5] = s[5]; d[6] = s[6]; d[7] = s[7]; d[8] = s[8]; d[9] = s[9];
        }
        hud_face(health);
        hc.last_face_anim = cur_face_anim;
        hc.last_special_face = stbar ? stbar->specialFace : 0;
        saturn_face_dirty_latch = 1;
        if (ST_BAR_Y >= 192 && saturn_show_fps)
            saturn_perf_dirty_latch = 1;
    }

    /* pickup / secret message line, top-left over the view */
    hud_message(p);

    /* In-game FPS counter + perf breakdown (2026-10-05): 3x5 font in
     * bright yellow (PLAYPAL 231) on black boxes in the status bar.
     * Row at (8,214):  FPS:xx.x
     * Row at (52,214): T R V C S D = average ms per displayed frame of
     *   T game tics, R render (BSP walk + quad build + HUD), V VDP1
     *   frame (wait for the previous draw + command write), C fb
     *   letterbox + VDP2 copy, S vblank/display-cap waits, D background
     *   CD fill; Q = VDP1 quads this frame. Buckets: d_main.c
     *   saturn_pf_acc, FRT ticks at the measured saturn_frt_rate. */
    {
        extern volatile unsigned int saturn_pf_acc[6];
        extern volatile int          saturn_frt_rate;
        extern int                   Saturn_Vdp1Wall_Count(void);
        extern int                   saturn_v1w_drop;
        extern int                   saturn_v1w_slotdrop;
        static const char pf_tag[6] = { 'T', 'R', 'V', 'C', 'S', 'F' };
        static int fps_val = 600;
        static int pf_ms[6];
        static int pf_quads = 0, pf_drop = 0;      /* shown: window max */
        static int win_quads = 0, win_drop = 0;    /* running window max */
        static int pf_ref = 0, win_ref = 0;        /* VDP1 texture refusals */
        static int pf_cef_ms = 0, pf_del_ms = 0;
        static int pf_edsr = 3, pf_copr = 0;
        static int win_min_edsr = 3, win_copr = 0;
        static unsigned int last_vbl = 0;
        static unsigned int acc_vbl = 0;
        static int frames = 0;
        static int fps_inited = 0;

        static int cached_frt_rate = 0;
        static unsigned int cached_per_ms = 0;
        static unsigned int cached_inv_per_ms = 0;
        int cur_rate = saturn_frt_rate;
        if (cur_rate != cached_frt_rate)
        {
            cached_frt_rate = cur_rate;
            cached_per_ms = (unsigned int)cur_rate * 60u / 1000u;
            cached_inv_per_ms = cached_per_ms ? ((1048576u + (cached_per_ms >> 1)) / cached_per_ms) : 0u;
        }
        unsigned int half_ms = cached_per_ms >> 1;

        unsigned int now = saturn_vbl_count;
        if (!fps_inited)
        {
            last_vbl = now;
            fps_val = 600;
            fps_inited = 1;
        }
        unsigned int delta = now - last_vbl;
        last_vbl = now;

        if (delta > 60)
            delta = 60;
        acc_vbl += delta;
        frames++;

        /* Hitch diagnostics (HUD 'Z' / 'Y'): worst frame of the window in
         * vblanks, the largest per-frame VDP1 texture upload, and texture
         * page flushes. saturn_vdp1_bus_tex_bytes only ever grows on the
         * Saturn, so per-frame uploads are differences. */
        static unsigned int win_worst_vbl = 0, pf_worst_ms = 0;
        static unsigned int win_up_max = 0, pf_up_kb = 0, last_bus = 0;
        static unsigned int last_flushes = 0, pf_flushes = 0;
        {
            extern uint32_t     saturn_vdp1_bus_tex_bytes;
            unsigned int bus = (unsigned int)saturn_vdp1_bus_tex_bytes;
            /* (the Windows VDP1 emulator resets the counter every frame) */
            unsigned int up  = (bus >= last_bus) ? (bus - last_bus) : bus;
            last_bus = bus;
            if (up > win_up_max)
                win_up_max = up;
            if (delta > win_worst_vbl)
                win_worst_vbl = delta;
        }

        {
            extern int saturn_v1w_refuse;
            extern int saturn_v1w_edsr;
            extern int saturn_v1w_copr;
            int q = Saturn_Vdp1Wall_Count();
            int x = saturn_v1w_drop + saturn_v1w_slotdrop;

            if (q > win_quads)
                win_quads = q;
            if (x > win_drop)
                win_drop = x;
            if (saturn_v1w_refuse > win_ref)
                win_ref = saturn_v1w_refuse;
            if (saturn_v1w_edsr < win_min_edsr)
            {
                win_min_edsr = saturn_v1w_edsr;
                win_copr = saturn_v1w_copr;
            }
            else if (win_min_edsr >= 2)
            {
                win_min_edsr = saturn_v1w_edsr;
                win_copr = saturn_v1w_copr;
            }
        }

        /* Refresh readout roughly every 15 frames (~0.5 - 1.0 sec) */
        extern volatile int saturn_frt_tic;
        int perf_refreshed = 0;
        if ((frames >= 15 && saturn_frt_tic <= 4) || frames >= 16 || acc_vbl >= 60)
        {
            perf_refreshed = 1;
            if (saturn_show_fps)
                saturn_perf_dirty_latch = 1;
            int k;

            if (acc_vbl > 0)
                fps_val = (600 * frames) / acc_vbl;
            unsigned int inv_frames = (frames == 15) ? 4369u : ((frames == 16) ? 4096u : ((65536u + ((unsigned int)frames >> 1)) / (unsigned int)frames));
            for (k = 0; k < 6; k++)
            {
                unsigned int a = saturn_pf_acc[k];

                saturn_pf_acc[k] = 0;
                unsigned int avg = (a <= 655350u) ? (((a >> 4) * inv_frames) >> 12) : (a / (unsigned int)frames);
                pf_ms[k] = (int)(((avg + half_ms) * cached_inv_per_ms) >> 20);
                if (pf_ms[k] > 999)
                    pf_ms[k] = 999;
            }
            extern volatile unsigned int saturn_prof_cef;
            extern volatile unsigned int saturn_prof_del;
            unsigned int c_acc = saturn_prof_cef;
            unsigned int d_acc = saturn_prof_del;
            unsigned int avg_cef = (c_acc <= 655350u) ? (((c_acc >> 4) * inv_frames) >> 12) : (c_acc / (unsigned int)frames);
            unsigned int avg_del = (d_acc <= 655350u) ? (((d_acc >> 4) * inv_frames) >> 12) : (d_acc / (unsigned int)frames);
            pf_cef_ms = (int)(((avg_cef + half_ms) * cached_inv_per_ms) >> 20);
            pf_del_ms = (int)(((avg_del + half_ms) * cached_inv_per_ms) >> 20);
            if (pf_cef_ms > 999) pf_cef_ms = 999;
            if (pf_del_ms > 999) pf_del_ms = 999;
            saturn_prof_cef = 0;
            saturn_prof_del = 0;
            {
                extern unsigned int saturn_v1_page_flushes;
                pf_worst_ms  = (win_worst_vbl * 50u) / 3u;     /* vblanks -> ms (60 Hz) */
                pf_up_kb     = (win_up_max + 1023u) >> 10;
                pf_flushes   = saturn_v1_page_flushes - last_flushes;
                last_flushes = saturn_v1_page_flushes;
                win_worst_vbl = 0;
                win_up_max    = 0;
            }
            pf_quads  = win_quads;
            pf_drop   = win_drop;
            pf_ref    = win_ref;
            pf_edsr   = win_min_edsr;
            pf_copr   = win_copr;
            win_quads = 0;
            win_drop  = 0;
            win_ref   = 0;
            win_min_edsr = 3;
            acc_vbl = 0;
            frames = 0;
        }

        extern unsigned int saturn_hw_prof_bsp;
        extern unsigned int saturn_hw_prof_segs;
        extern unsigned int saturn_hw_prof_ap;
        extern unsigned int saturn_hw_prof_fsub;
        extern unsigned int saturn_hw_prof_spr;
        extern unsigned int saturn_hw_prof_occl;
        extern unsigned int saturn_hw_prof_wseg;
        extern unsigned int saturn_hw_prof_wquad;
        extern unsigned int saturn_hw_prof_sportal;
        extern unsigned int saturn_hw_prof_unb;
        extern unsigned int saturn_hw_prof_mflat;
        extern unsigned int saturn_hw_wall_quads;
        extern unsigned int saturn_hw_flat_quads;
        extern unsigned int saturn_hw_prof_slave_wait;
        extern unsigned char         saturn_slave_enabled;
        extern int                   saturn_visit_count;
        extern int                   saturn_hw_bsp_occl;
        extern unsigned int saturn_hw_occl_culls;
        extern unsigned int saturn_hw_seg_iter;
        extern unsigned int saturn_hw_seg_vis;
        extern unsigned int saturn_hw_tex_miss;
        extern unsigned int saturn_hw_wall_strips;
        extern unsigned int saturn_hw_flat_lod0_subs;
        extern unsigned int saturn_hw_flat_lod1_subs;
        extern unsigned int saturn_hw_flat_lod_saved_quads;
        extern unsigned int saturn_hw_dda_steps;
        extern unsigned int Saturn_CmdBuf_ComputeDDA(void);
#ifdef SATURN_WIN32
        if (perf_refreshed || saturn_hw_dda_steps == 0)
            Saturn_CmdBuf_ComputeDDA();

        int bsp_ms = (int)(((saturn_hw_prof_bsp + half_ms) * cached_inv_per_ms) >> 20);
        int seg_ms = (int)(((saturn_hw_prof_segs + half_ms) * cached_inv_per_ms) >> 20);
        int slv_ms = (int)(((saturn_hw_prof_slave_wait + half_ms) * cached_inv_per_ms) >> 20);
        int vdp_ms = pf_ms[2];
        int dda_k  = (int)((((saturn_hw_dda_steps + 500u) >> 3) * 8389u) >> 20);

        g_saturn_hud_diag.fps = (float)fps_val / 10.0f;
        g_saturn_hud_diag.ms_r = pf_ms[1];
        g_saturn_hud_diag.ms_v = vdp_ms;
        g_saturn_hud_diag.ms_v_cef = pf_cef_ms;
        g_saturn_hud_diag.ms_v_del = pf_del_ms;
        g_saturn_hud_diag.ms_t = pf_ms[0];
        g_saturn_hud_diag.ms_c = pf_ms[3];
        g_saturn_hud_diag.ms_b = bsp_ms;
        g_saturn_hud_diag.ms_g = seg_ms;
        g_saturn_hud_diag.ms_s = pf_ms[4];
        g_saturn_hud_diag.ms_j = saturn_slave_enabled ? slv_ms : 0;
        g_saturn_hud_diag.wall_quads = (int)saturn_hw_wall_quads;
        g_saturn_hud_diag.flat_quads = (int)saturn_hw_flat_quads;
        g_saturn_hud_diag.total_quads = (saturn_hw_wall_quads + saturn_hw_flat_quads > 0) ? (int)(saturn_hw_wall_quads + saturn_hw_flat_quads) : pf_quads;
        g_saturn_hud_diag.quads_dropped = pf_drop;
        g_saturn_hud_diag.subsectors = saturn_visit_count;
        g_saturn_hud_diag.occlusion_mode = (saturn_hw_bsp_occl == 2) ? 2 : 1;
        g_saturn_hud_diag.bsp_culls = (int)saturn_hw_occl_culls;
        g_saturn_hud_diag.segs_iter = (int)saturn_hw_seg_iter;
        g_saturn_hud_diag.segs_vis = (int)saturn_hw_seg_vis;
        g_saturn_hud_diag.tex_refused = pf_ref;
        g_saturn_hud_diag.tex_miss = (int)saturn_hw_tex_miss;
        g_saturn_hud_diag.wall_strips = (int)saturn_hw_wall_strips;
        g_saturn_hud_diag.flat_lod0_subs = (int)saturn_hw_flat_lod0_subs;
        g_saturn_hud_diag.flat_lod1_subs = (int)saturn_hw_flat_lod1_subs;
        g_saturn_hud_diag.flat_lod_saved_quads = (int)saturn_hw_flat_lod_saved_quads;
        g_saturn_hud_diag.vdp1_edsr = pf_edsr;
        g_saturn_hud_diag.vdp1_copr = pf_copr;
        g_saturn_hud_diag.vdp1_dda_k = dda_k;

        if (saturn_show_fps && fps_val > 0)
#else
        if (saturn_show_fps && fps_val > 0 && (stbar_dirty || perf_refreshed || (face_dirty && ST_BAR_Y >= 192)))
#endif
        {
#ifndef SATURN_WIN32
            static int pf_dda_k = 0;
            if (perf_refreshed)
            {
                Saturn_CmdBuf_ComputeDDA();
                pf_dda_k = (int)((((saturn_hw_dda_steps + 500u) >> 3) * 8389u) >> 20);
            }
            int bsp_ms = (int)(((saturn_hw_prof_bsp + half_ms) * cached_inv_per_ms) >> 20);
            int seg_ms = (int)(((saturn_hw_prof_segs + half_ms) * cached_inv_per_ms) >> 20);
            int ap_ms  = (int)(((saturn_hw_prof_ap + half_ms) * cached_inv_per_ms) >> 20);
            int oc_ms  = (int)(((saturn_hw_prof_occl + half_ms) * cached_inv_per_ms) >> 20);
            int wq_ms  = (int)(((saturn_hw_prof_wquad + half_ms) * cached_inv_per_ms) >> 20);
            int sp_ms  = (int)(((saturn_hw_prof_sportal + saturn_hw_prof_unb + half_ms) * cached_inv_per_ms) >> 20);
            int slv_ms = (int)(((saturn_hw_prof_slave_wait + half_ms) * cached_inv_per_ms) >> 20);
            int vdp_ms = pf_ms[2];
            int dda_k  = pf_dda_k;
#endif
            char buf[48];
            int  n = 0;

            buf[n++] = 'F'; buf[n++] = 'P'; buf[n++] = 'S'; buf[n++] = ':';
            buf[n++] = ' ';
            int fv = fps_val;
            if (fv >= 1000)
            {
                unsigned int q = ((unsigned int)fv * 41943u) >> 22;
                buf[n++] = (char)('0' + (q % 10u));
            }
            unsigned int q10 = ((unsigned int)fv * 52429u) >> 19;
            unsigned int r10 = (unsigned int)fv - q10 * 10u;
            unsigned int q100 = (q10 * 52429u) >> 19;
            unsigned int r100 = q10 - q100 * 10u;
            if (fv >= 100)
                buf[n++] = (char)('0' + (q100 % 10u));
            buf[n++] = (char)('0' + r100);
            buf[n++] = '.';
            buf[n++] = (char)('0' + r10);
            buf[n] = 0;
            int dbg_y = (ST_BAR_Y < 192) ? (ST_BAR_Y + 34) : (ST_BAR_Y + 22);
            hud_tiny(8, dbg_y, 44, buf);

            if (saturn_show_perf)
            {
                char dbg[128];
                int dn = 0;

                dn = hud_tiny_tag(dbg, dn, 'R', pf_ms[1]);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'V', pf_cef_ms);
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, pf_del_ms);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'T', pf_ms[0]);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'C', pf_ms[3]);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'B', bsp_ms);
#if !defined(SATURN_WIN32) && defined(HW_PROF_SUBTIMERS) && HW_PROF_SUBTIMERS
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, ap_ms);
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, oc_ms);
#endif
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'G', seg_ms);
#if !defined(SATURN_WIN32) && defined(HW_PROF_SUBTIMERS) && HW_PROF_SUBTIMERS
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, wq_ms);
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, sp_ms);
#endif
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'S', pf_ms[4]);
                dbg[dn++] = ' ';
                if (saturn_slave_enabled)
                {
                    dn = hud_tiny_tag(dbg, dn, 'J', slv_ms);
                    dbg[dn++] = ' ';
                }
                extern int Saturn_BakedWalls_Active(void);
                dn = hud_tiny_tag(dbg, dn, Saturn_BakedWalls_Active() ? 'W' : 'w', (int)saturn_hw_wall_quads);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'F', (int)saturn_hw_flat_quads);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'Q', pf_quads);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'D', pf_drop);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'Z', (int)pf_worst_ms);
                dbg[dn++] = '/';
                dn = hud_tiny_num(dbg, dn, (int)pf_up_kb);
                dbg[dn++] = ' ';
                dn = hud_tiny_tag(dbg, dn, 'Y', (int)pf_flushes);
                dbg[dn] = 0;
                hud_tiny(50, dbg_y, 262, dbg);

                /* Row 2: N visited subsectors, E VDP1 EDSR/COPR slot, K culled BSP
                 * subtrees, I baked segs iterated, U segs surviving culls, M DDA (k). */
                if (dbg_y + 8 + 6 <= 224)
                {
                    dn = 0;
                    dn = hud_tiny_tag(dbg, dn, 'N', saturn_visit_count);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'E', pf_edsr);
                    dbg[dn++] = '/';
                    dn = hud_tiny_num(dbg, dn, pf_copr);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'K', (int)saturn_hw_occl_culls);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'I', (int)saturn_hw_seg_iter);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'U', (int)saturn_hw_seg_vis);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'X', pf_ref);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'H', (int)saturn_hw_tex_miss);
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'P', (int)saturn_hw_wall_strips);
                    dbg[dn++] = ' ';
                    extern const uint16_t *saturn_baked_flat_lod1_q_count;
                    if (!saturn_baked_flat_lod1_q_count)
                    {
                        dbg[dn++] = 'L';
                        dbg[dn++] = '_';
                        dbg[dn++] = 'N';
                        dbg[dn++] = 'U';
                        dbg[dn++] = 'L';
                        dbg[dn++] = 'L';
                    }
                    else
                    {
                        dn = hud_tiny_tag(dbg, dn, 'L', (int)saturn_hw_flat_lod1_subs);
                        dbg[dn++] = '/';
                        dn = hud_tiny_num(dbg, dn, (int)saturn_hw_flat_lod0_subs);
                    }
                    dbg[dn++] = ' ';
                    dn = hud_tiny_tag(dbg, dn, 'M', dda_k);
                    if (p && p->mo)
                    {
                        extern int saturn_map;
                        int map_x = (int)(p->mo->x >> FRACBITS);
                        int map_y = (int)(p->mo->y >> FRACBITS);
                        int map_z = (int)(p->mo->z >> FRACBITS);
                        int map_a = (int)(((unsigned int)(p->mo->angle >> 20) * 360u) >> 12);
                        dbg[dn++] = ' ';
                        dbg[dn++] = 'E';
                        dbg[dn++] = '1';
                        dn = hud_tiny_tag(dbg, dn, 'M', saturn_map);
                        dbg[dn++] = ' ';
                        dn = hud_tiny_signed_tag(dbg, dn, 'X', map_x);
                        dbg[dn++] = ' ';
                        dn = hud_tiny_signed_tag(dbg, dn, 'Y', map_y);
                        dbg[dn++] = ' ';
                        dn = hud_tiny_signed_tag(dbg, dn, 'Z', map_z);
                        dbg[dn++] = ' ';
                        dn = hud_tiny_tag(dbg, dn, 'A', map_a);
                    }
                    dbg[dn] = 0;
                    hud_tiny(8, dbg_y + 8, 304, dbg);
                }
            }
        }
    }
}
