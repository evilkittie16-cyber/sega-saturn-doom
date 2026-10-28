/* saturn.h — Saturn DOOM's small custom header. We rely on d32xr's
 * doomdef.h for almost everything; this file is only for things
 * specific to the Saturn port. */
#ifndef __SATURN__
#define __SATURN__

/* ===== r89 REAL-HW BOOT BISECT (2026-09-05 user directive) =====
 * Real HW freezes on the solid CYAN "STARTING" screen — i.e. inside
 * Saturn_G_InitNew. Find the last boot point that WORKS on real HW,
 * then re-enable blocks one burn at a time until the breaker fires.
 * Levels (each = one burn + photo):
 *   1 = park right after "STARTING" — Saturn_G_InitNew NEVER runs.
 *   2 = run G_InitNew with the P_SetupLevel call gated OUT, then park.
 *   3 = full G_InitNew (P_SetupLevel runs), park before the IN-LOOP
 *       flash — never enters D_DoomLoop.
 *   4 = boot fully into D_DoomLoop but the background cart-fill is
 *       gated OUT (render path + tics run).
 *   0 = full unmodified boot.
 * The park paints "B<n> PARK" at (8,32)/(8,48) and pumps slSynch
 * frames forever — a photo of B<n>+PARK proves every block up to and
 * including level <n> works on real HW.
 * r89 B1 VERDICT (2026-09-05 real-HW photo): boot through the FULL
 * I/O chain to the park, ISR census painting, panel layout identical
 * to Kronos (S=00/K=1/P=00/B=00r../R=00/6=00/W=../I=00/E=../O=/C=).
 * Boot I/O chain EXONERATED. Breaker is inside Saturn_G_InitNew.
 * r90 B2 VERDICT (2026-09-05): park reached (cyan + B2 PARK) —
 * G_InitNew body (M_ClearRandom/player resets) EXONERATED. The B3
 * burn answers: P_SetupLevel = breaker yes/no.
 * r98 B3 VERDICT (2026-09-05 real HW): THE PARK WAS REACHED on the
 * console — P_SetupLevel (the r91 breaker) is EXONERATED on real
 * HW; the r92 record-slot relocation + the r95 bounded holds
 * cleared it.
 * r99 B4 VERDICT (2026-09-05 real HW): the console BOOTED INTO
 * D_DoomLoop — the game ran (E1M1 view) with the expected missing-
 * COLD-lump white speckle (cart-fill gated OUT by design). The full
 * render path + tics survive on real HW. Next rung: BISECT 0 = full
 * unmodified boot with the background cart-fill back IN. */
#define SATURN_BISECT 4

/* ===== r92 CART RECORD SLOT RELOCATION (B3 verdict fix) =====
 * r91 B3 real-HW result: solid CYAN, no census, no park text = wedged
 * INSIDE P_SetupLevel with the ISR dead. Kronos parks fine. The one
 * element of the per-chunk CD-stream path that first exercises at
 * P_SetupLevel (map lumps stream from CD in 2KB chunks) is the GSUB
 * 8/9/10 stamps into the record slot at cart_size-512 = 1008KB-512.
 * That address sits in the ONE region the r68 write march never
 * verified: the march proved writes good at 512KB-2 and BAD at 1MB-2,
 * and the r63-r67 freezes all died with the fill cursor at
 * F~1000-1100KB. The r69 clamp to 1008KB kept every record write
 * inside that unverified window. Red-screen-era boots prove the Z
 * zone at 2MB+ and sustained writes below ~500KB are fine.
 * FIX: relocate the record slot (REC1 rec[0..18], GSTL rec[32..36],
 * GSUB rc[40..42]) to 512KB-512 — the last march-verified write
 * point — and cap the hot-lump fill region below it. If B3 still
 * wedges after this, the 2MB+ Z-zone lump-body writes are implicated
 * instead (next rung: move the level buffers out of the cart). */
#define SATURN_CART_REC_OFF   (512u * 1024u - 512u)

/* d32xr's doomdef.h uses `true` as a token in #define Z_Malloc(...) and
 * similar. We must have <stdbool.h> in scope before that macro fires.
 * Include it first so the token expansion works. */
#include <stdbool.h>

/* d32xr's doomdef.h (with -DMARS=1 -DSATURN=1) defines
 * I_Set/GetThreadLocalVar as macros backed by Saturn_TlsSlots[] —
 * plain-memory emulation of the 32X GBR TLS. Nothing to do here; the
 * old no-op macros silently broke every DOOMTLS_VALIDCOUNT fetcher
 * (movement/collision dereferenced an uninitialised pointer). */

#include "doomdef.h"   /* d32xr's doomdef.h */

/* Saturn DOOM doesn't have a "real" gamemode, but we keep the type
 * local. The original id code had a `language` global too; the
 * Saturn port doesn't expose it. */
typedef enum {
    saturn_shareware = 0,
    saturn_registered,
    saturn_commercial,
    saturn_retail
} saturn_gamemode_t;

extern saturn_gamemode_t  saturn_gamemode;
extern int                saturn_gamemission;     /* 0=doom, 1=doom2 */

/* Saturn-only state. d32xr replaces id's int "gameepisode + gamemap"
 * with a lump-name string ("MAP01", "E1M1", ...). The Saturn boot
 * path needs to pick one. */
extern int   saturn_episode;     /* 1..4 for doom1; ignored for doom2 */
extern int   saturn_map;         /* 1..32 */
extern int   saturn_skill;       /* 0..4 (0 = easiest) */

/* ----- Platform shim declarations (only Saturn-specific ones) ----- */

/* m_argv */
void   M_FakeArgv(const char *args);
int    M_CheckParm(const char *check);
int    M_GetArgc(void);
char **M_GetArgv(void);

/* m_misc */
void   M_LoadDefaults(void);

/* z_zone: d32xr's Z_Init() asks I_ZoneBase() (defined in d_main.c) for
 * the pool — cart-backed when the 4 MB cart is present. No Z_Setup. */

/* i_system */
void   I_WaitVBL(int count);

/* i_sound */
void   I_InitSound(void);

/* i_video */
void   I_InitVideo(void);

/* w_wad: d32xr's W_Init() takes no args and reads I_WadBase() for the
 * buffer. We provide I_WadBase() in i_wad.c and a thin wrapper here
 * for callers that wanted a filename. */
void   Saturn_W_Init(const char *filename);

/* g_game — Saturn wrapper that picks a lump from episode/map/skill. */
void   Saturn_G_InitNew(int skill, int episode, int map);

/* i_wad — cart GSUB breadcrumb (survives reboot; consumed by the next
 * boot's I_WadBase into saturn_init_site = 0x50+sub). */
void   Saturn_GsubStamp(unsigned int sub);
void   Saturn_G_Ticker(void);
void   Saturn_G_Display(void);

/* p_setup — Saturn wrapper that takes episode/map. */
void   Saturn_P_SetupLevel(int episode, int map, int skill);
void   Saturn_P_Ticker(void);
void   Saturn_WarpToLevel(int episode, int map);
void   Saturn_SetSkyTexture(const char *name);

/* Saturn Hardware & Diagnostic HUD telemetry (Rows 200-223) */
#ifndef _SATURN_HUD_DIAG_T_
#define _SATURN_HUD_DIAG_T_
typedef struct {
    float fps;
    int   ms_r;
    int   ms_v;
    int   ms_t;
    int   ms_c;
    int   ms_b;
    int   ms_g;
    int   ms_s;
    int   ms_j;
    int   wall_quads;
    int   flat_quads;
    int   total_quads;
    int   quads_dropped;
    int   subsectors;
    int   occlusion_mode;
    int   bsp_culls;
    int   segs_iter;
    int   segs_vis;
    int   tex_refused;
    int   tex_miss;
    int   wall_strips;
    int   flat_lod0_subs;
    int   flat_lod1_subs;
    int   flat_lod_saved_quads;
    int   ms_v_cef;
    int   ms_v_del;
    int   vdp1_edsr;
    int   vdp1_copr;
    int   vdp1_dda_k;
} saturn_hud_diag_t;
#endif

extern saturn_hud_diag_t g_saturn_hud_diag;

#endif