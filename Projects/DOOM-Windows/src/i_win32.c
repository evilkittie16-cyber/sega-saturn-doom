/* i_win32.c - Win32 Triple-Pane Video, Input, Real-Time Heat Map & Telemetry */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "saturn_win32.h"
#include "vdp1_emu.h"
#include "doomdef.h"
#include "p_local.h"
#include "i_system.h"
#include "i_video.h"
#include "r_local.h"
#include "saturn_vdp1.h"
#include "saturn_hw_render.h"
#include "tester_telemetry.h"

#define PANE_W      352
#define PANE_H      224
#define NUM_PANES   3
#define COMBINED_W  (PANE_W * NUM_PANES) /* 1056 */
#define HEADER_H    16                   /* Dedicated header bar height */
#define COMBINED_H  (PANE_H + HEADER_H)  /* 240 */

byte *I_VideoBuffer = NULL;
byte *I_VideoBuffer2 = NULL;

static uint32_t s_saturn_fb[PANE_W * PANE_H];
static uint32_t s_dos_fb[PANE_W * PANE_H];
static uint32_t s_heat_fb[PANE_W * PANE_H];
static uint32_t s_combined_fb[COMBINED_W * COMBINED_H];

static HWND s_hwnd = NULL;
static BITMAPINFO s_bmi;

volatile unsigned int saturn_vbl_count = 0;
volatile unsigned int saturn_master_beat = 0;
volatile unsigned int saturn_iter_count = 0;
volatile unsigned int saturn_sim_site = 0;
volatile unsigned int saturn_tic_count = 0;
volatile int          saturn_prof_render = 0;
volatile int          saturn_prof_finish = 0;
volatile int          saturn_prof_tics = 0;
volatile int          saturn_prof_comp = 0;
volatile int          saturn_prof_cd = 0;
volatile unsigned int saturn_pf_acc[8] = {0};

/* Dynamic DOS DOOM Reference DLL binding */
typedef int  (*fn_DOS_Init)(const char *wad_path);
typedef void (*fn_DOS_SetCamera)(int map, int x, int y, int z, unsigned int angle, int viewheight);
typedef void (*fn_DOS_SetWeapon)(int readyweapon, int weapon_state, int weapon_sx, int weapon_sy,
                                 int flash_state, int flash_sx, int flash_sy, int extralight);
typedef void (*fn_DOS_RenderFrame)(uint32_t *out_rgba_352x224);
typedef const uint8_t *(*fn_DOS_GetPalette)(void);

typedef struct {
    int x, y, z;
    unsigned int angle;
    char name[4];
    int frame;
    int flags;
    int sector_idx;
} dos_sync_mobj_t;

typedef void (*fn_DOS_SyncMobjs)(const dos_sync_mobj_t *mobjs, int count);

static HMODULE            s_dos_dll = NULL;
static fn_DOS_Init        pDOS_Init = NULL;
static fn_DOS_SetCamera   pDOS_SetCamera = NULL;
static fn_DOS_SetWeapon   pDOS_SetWeapon = NULL;
static fn_DOS_SyncMobjs   pDOS_SyncMobjs = NULL;
static fn_DOS_RenderFrame pDOS_RenderFrame = NULL;
static fn_DOS_GetPalette  pDOS_GetPalette = NULL;
static int                s_dos_ready = 0;

static FILE *s_csv_fp = NULL;

static const char *FindWadPath(void)
{
    static const char *candidates[] = {
        "cd/DOOM.WAD", "DOOM.WAD", "cd/doom.wad", "doom.wad",
        "cd/DOOM1.WAD", "DOOM1.WAD", "cd/doom1.wad", "doom1.wad",
        "../DOOM-Saturn/cd/DOOM.WAD", "../DOOM-Saturn/cd/doom.wad",
        "../DOOM-Saturn/cd/DOOM1.WAD", "../DOOM-Saturn/cd/doom1.wad",
        "../DOOM-Saturn/DOOM.WAD", "../DOOM-Saturn/doom.wad",
        "../DOOM-Saturn/DOOM1.WAD", "../DOOM-Saturn/doom1.wad",
        "../../DOOM-Saturn/cd/DOOM.WAD", "../../DOOM-Saturn/cd/doom.wad",
        "../../DOOM-Saturn/cd/DOOM1.WAD", "../../DOOM-Saturn/cd/doom1.wad",
        "C:\\Users\\charl\\Desktop\\joengine-master\\DOOM.WAD",
        "C:\\Users\\charl\\Desktop\\joengine-master\\Projects\\DOOM-Saturn\\cd\\DOOM.WAD"
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return "cd/DOOM.WAD";
}

static void InitDosRefDll(void)
{
    if (s_dos_dll) return;

    printf("[Comparator] Loading dos_doom_ref.dll...\n");
    s_dos_dll = LoadLibraryA("dos_doom_ref.dll");
    if (!s_dos_dll) {
        s_dos_dll = LoadLibraryA("dos_ref/dos_doom_ref.dll");
    }

    if (!s_dos_dll) {
        printf("[Comparator] Note: dos_doom_ref.dll not found. DOS pane will be inactive.\n");
        return;
    }

    pDOS_Init        = (fn_DOS_Init)GetProcAddress(s_dos_dll, "DOS_Init");
    pDOS_SetCamera   = (fn_DOS_SetCamera)GetProcAddress(s_dos_dll, "DOS_SetCamera");
    pDOS_SetWeapon   = (fn_DOS_SetWeapon)GetProcAddress(s_dos_dll, "DOS_SetWeapon");
    pDOS_SyncMobjs   = (fn_DOS_SyncMobjs)GetProcAddress(s_dos_dll, "DOS_SyncMobjs");
    pDOS_RenderFrame = (fn_DOS_RenderFrame)GetProcAddress(s_dos_dll, "DOS_RenderFrame");
    pDOS_GetPalette  = (fn_DOS_GetPalette)GetProcAddress(s_dos_dll, "DOS_GetPalette");

    if (pDOS_Init && pDOS_SetCamera && pDOS_RenderFrame) {
        const char *wad = FindWadPath();
        printf("[Comparator] Initializing DOS reference renderer with WAD: %s\n", wad);
        if (pDOS_Init(wad)) {
            s_dos_ready = 1;
            printf("[Comparator] DOS Reference Renderer online!\n");
        }
    } else {
        printf("[Comparator] Error: Failed to find exported functions in dos_doom_ref.dll\n");
    }
}

void I_InitVideo(void)
{
    if (!I_VideoBuffer) {
        I_VideoBuffer = (byte *)malloc(PANE_W * PANE_H);
        memset(I_VideoBuffer, 0, PANE_W * PANE_H);
    }
    I_VideoBuffer2 = I_VideoBuffer;

    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = COMBINED_W;
    s_bmi.bmiHeader.biHeight = -COMBINED_H; /* Top-down DIB */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;

    InitDosRefDll();
}

void I_SetPalette(const byte *palette)
{
    VDP1_SetPalette((const uint8_t *)palette);
}

void Win32_SetWindowHandle(HWND hwnd)
{
    s_hwnd = hwnd;
}

extern void Win32_PumpEvents(void);

void SaveFramebufferBMP(const char *filename, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    memset(&bfh, 0, sizeof(bfh));
    memset(&bih, 0, sizeof(bih));

    bfh.bfType = 0x4D42; /* "BM" */
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + w * h * 4;

    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h; /* top-down */
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(fb, w * h * 4, 1, f);
    fclose(f);
}

/* Minimal embedded 5x7 bitmap font for rendering crisp labels directly into framebuffers */
static const uint8_t s_font5x7[128][7] = {
    [' '] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    ['-'] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    ['.'] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
    [':'] = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
    ['%'] = { 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13 },
    ['('] = { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06 },
    [')'] = { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18 },
    ['['] = { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E },
    [']'] = { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E },
    ['<'] = { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06 },
    ['>'] = { 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30 },
    ['='] = { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    ['/'] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 },
    ['0'] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    ['1'] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    ['2'] = { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F },
    ['3'] = { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    ['4'] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    ['5'] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    ['6'] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    ['7'] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    ['8'] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    ['9'] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    ['A'] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    ['B'] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    ['C'] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    ['D'] = { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
    ['E'] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    ['F'] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    ['G'] = { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    ['H'] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    ['I'] = { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    ['J'] = { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    ['K'] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    ['L'] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    ['M'] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    ['N'] = { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
    ['O'] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    ['P'] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    ['Q'] = { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    ['R'] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    ['S'] = { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E },
    ['T'] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    ['U'] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    ['V'] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    ['W'] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    ['X'] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    ['Y'] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    ['Z'] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
};

static void DrawChar5x7(uint32_t *fb, int pitch, int px, int py, char ch, uint32_t fg, uint32_t bg)
{
    unsigned char uc = (unsigned char)ch;
    if (uc > 127) uc = ' ';
    const uint8_t *glyph = s_font5x7[uc];

    for (int r = 0; r < 7; r++) {
        int y = py + r;
        if (y < 0 || y >= COMBINED_H) continue;
        uint8_t row = glyph[r];
        for (int c = 0; c < 5; c++) {
            int x = px + c;
            if (x < 0 || x >= COMBINED_W) continue;
            if (row & (0x10 >> c)) {
                fb[y * pitch + x] = fg;
            } else if (bg != 0xFFFFFFFF) {
                fb[y * pitch + x] = bg;
            }
        }
    }
}

static void DrawText5x7(uint32_t *fb, int pitch, int px, int py, const char *text, uint32_t fg, uint32_t bg)
{
    int cur_x = px;
    while (*text) {
        DrawChar5x7(fb, pitch, cur_x, py, *text, fg, bg);
        cur_x += 6;
        text++;
    }
}

/* CSV Flight Recorder Implementation */
void Telemetry_ToggleCSV(void)
{
    if (!g_telemetry.csv_recording) {
        time_t rawtime;
        time(&rawtime);
        struct tm *ti = localtime(&rawtime);
        sprintf(g_telemetry.csv_filename, "journey_telemetry_MAP_E%dM%d_%04d%02d%02d_%02d%02d%02d.csv",
                g_telemetry.episode ? g_telemetry.episode : 1,
                g_telemetry.map,
                ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
                ti->tm_hour, ti->tm_min, ti->tm_sec);

        s_csv_fp = fopen(g_telemetry.csv_filename, "w");
        if (s_csv_fp) {
            fprintf(s_csv_fp, "timestamp_ms,gametic,map,cam_x,cam_y,cam_z,angle_deg,sector,subsector,floor_h,ceil_h,min_err_pct,max_err_pct,avg_err_pct,pct_lt_01,pct_lt_1,pct_gte_1,spike_screen_x,spike_screen_y,sat_fps,sat_ms_r,sat_ms_v,sat_ms_t,sat_ms_c,sat_ms_b,sat_ms_g,sat_ms_s,sat_ms_j,sat_wall_quads,sat_flat_quads,sat_total_quads,sat_quads_dropped,sat_subsectors,sat_occlusion_mode,sat_bsp_culls,sat_segs_iter,sat_segs_vis,sat_tex_refused,sat_tex_miss,sat_wall_strips,sat_flat_lod0,sat_flat_lod1,sat_flat_lod_saved_quads\n");
            fflush(s_csv_fp);
            g_telemetry.csv_recording = 1;
            g_telemetry.csv_frame_count = 0;
            printf("[CSV Flight Recorder] STARTED -> %s\n", g_telemetry.csv_filename);
        }
    } else {
        if (s_csv_fp) {
            fclose(s_csv_fp);
            s_csv_fp = NULL;
        }
        g_telemetry.csv_recording = 0;
        printf("[CSV Flight Recorder] STOPPED. %d frames saved to %s\n",
               g_telemetry.csv_frame_count, g_telemetry.csv_filename);
    }
}

void Telemetry_WarpToSpike(void)
{
    if (players[0].mo && g_telemetry.peak_spike_pct > 0.0f) {
        P_UnsetThingPosition(players[0].mo);
        players[0].mo->x = g_telemetry.spike_cam_x;
        players[0].mo->y = g_telemetry.spike_cam_y;
        players[0].mo->z = g_telemetry.spike_cam_z;
        players[0].mo->angle = g_telemetry.spike_cam_angle;
        players[0].viewz = g_telemetry.spike_cam_z;
        Saturn_HW_ForceSkyRepaint();
        P_SetThingPosition(players[0].mo);
        printf("[Telemetry] Warped to error spike: (%d, %d, %d) angle 0x%08X (Error: %.2f%%)\n",
               g_telemetry.spike_cam_x >> FRACBITS,
               g_telemetry.spike_cam_y >> FRACBITS,
               g_telemetry.spike_cam_z >> FRACBITS,
               g_telemetry.spike_cam_angle,
               g_telemetry.peak_spike_pct);
    }
}

void Telemetry_ResetSpike(void)
{
    g_telemetry.peak_spike_pct = 0.0f;
    g_telemetry.spike_screen_x = 0;
    g_telemetry.spike_screen_y = 0;
    g_telemetry.spike_cam_x = 0;
    g_telemetry.spike_cam_y = 0;
    g_telemetry.spike_cam_z = 0;
    g_telemetry.spike_cam_angle = 0;
    printf("[Telemetry] Spike statistics reset.\n");
}

/* Automated Level Journey Flight Tour */
typedef struct {
    int x;
    int y;
    unsigned int angle;
    int dwell_tics;
    const char *name;
    const char *screenshot_prefix;
} journey_waypoint_t;

static const journey_waypoint_t s_e1m1_journey[] = {
    { 1056, -3616, 0x40000000, 30, "Spawn Room (Start)", "journey_wp0_spawn" },
    { 1056, -3200, 0x40000000, 15, "Spawn Room North Exit", NULL },
    {  800, -3250, 0x80000000, 15, "Entry Hallway to Armor Pool", NULL },
    {  448, -3328, 0x60000000, 30, "Green Armor Secret Acid Pool", "journey_wp1_armor_pool" },
    { 1100, -3150, 0x20000000, 15, "Return to Main Hallway", NULL },
    { 1450, -3050, 0x20000000, 20, "Toxic Sump / Nukage Room Entrance", "journey_wp2_nukage_entry" },
    { 1800, -2800, 0x30000000, 30, "Zigzag Acid Walkway (Center)", "journey_wp3_zigzag" },
    { 2150, -2500, 0x38000000, 20, "Zigzag Bridge to Computer Stairs", NULL },
    { 2450, -2200, 0x60000000, 30, "Computer Control Room Vista", "journey_wp4_computer_room" },
    { 2880, -3236, 0x80000000, 30, "Courtyard Vista Observation Window", "journey_wp5_courtyard_window" },
    { 2056, -3264, 0xC0000000, 35, "Outdoor Courtyard Lawn & Sky Vista", "journey_wp6_courtyard_outdoor" },
    { 3450, -3400, 0x80000000, 20, "Exit Hallway & Columns", NULL },
    { 3200, -3200, 0x80000000, 35, "Exit Chamber & Switch (End)", "journey_wp7_exit_room" }
};
#define NUM_JOURNEY_WAYPOINTS (int)(sizeof(s_e1m1_journey) / sizeof(s_e1m1_journey[0]))

static int s_journey_active = 0;
static int s_journey_wp_idx = 0;
static int s_journey_timer = 0;
static int s_journey_phase = 0; /* 0 = dwelling at waypoint, 1 = interpolating to next */
#define GLIDE_TICS_BETWEEN_WPS 25

/* Journey Summary Stats */
static double s_journey_sum_err = 0.0;
static int    s_journey_frame_count = 0;
static float  s_journey_min_err = 100.0f;
static float  s_journey_max_err = 0.0f;
static double s_journey_sum_fps = 0.0;
static double s_journey_sum_lt01 = 0.0;

void Telemetry_StartJourney(void)
{
    if (!s_journey_active) {
        extern int saturn_episode;
        extern int saturn_map;
        extern void Win32_RequestLevelWarp(int episode, int map);

        /* The automated waypoint tour is defined for E1M1 */
        if (saturn_episode != 1 || saturn_map != 1) {
            printf("[Flight Recorder] Map is E%dM%d. Warping to E1M1 for flight tour...\n",
                   saturn_episode, saturn_map);
            Win32_RequestLevelWarp(1, 1);
        }

        s_journey_active = 1;
        s_journey_wp_idx = 0;
        s_journey_timer = 0;
        s_journey_phase = 0;
        s_journey_sum_err = 0.0;
        s_journey_frame_count = 0;
        s_journey_min_err = 100.0f;
        s_journey_max_err = 0.0f;
        s_journey_sum_fps = 0.0;
        s_journey_sum_lt01 = 0.0;

        printf("====================================================================\n");
        printf(">>> [FLIGHT RECORDER] STARTING AUTOMATED E1M1 JOURNEY TOUR <<<\n");
        printf(">>> 13 Canonical Waypoints: Spawn -> Armor -> Acid -> Vista -> Exit\n");
        printf("====================================================================\n");

        if (!g_telemetry.csv_recording) {
            Telemetry_ToggleCSV();
        }
    } else {
        printf("[Flight Recorder] Journey tour paused / cancelled.\n");
        s_journey_active = 0;
    }
}

void Telemetry_StopJourney(void)
{
    if (s_journey_active) {
        s_journey_active = 0;
        printf("[Flight Recorder] Journey tour stopped.\n");
    }
}

int Telemetry_IsJourneyActive(void)
{
    return s_journey_active;
}

static void ExportTelemetryJSON(void)
{
    FILE *f = fopen("telemetry.json.tmp", "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"gametic\": %d,\n", g_telemetry.gametic);
    fprintf(f, "  \"map\": %d,\n", g_telemetry.map);
    fprintf(f, "  \"fps\": %.1f,\n", g_telemetry.fps);
    fprintf(f, "  \"camera\": {\n");
    fprintf(f, "    \"x\": %d,\n", g_telemetry.cam_x);
    fprintf(f, "    \"y\": %d,\n", g_telemetry.cam_y);
    fprintf(f, "    \"z\": %d,\n", g_telemetry.cam_z);
    fprintf(f, "    \"angle_raw\": %u,\n", g_telemetry.cam_angle);
    fprintf(f, "    \"angle_deg\": %.2f,\n", g_telemetry.cam_angle_deg);
    fprintf(f, "    \"sector\": %d,\n", g_telemetry.sector);
    fprintf(f, "    \"subsector\": %d,\n", g_telemetry.subsector);
    fprintf(f, "    \"floor_height\": %d,\n", g_telemetry.floor_height);
    fprintf(f, "    \"ceiling_height\": %d\n", g_telemetry.ceiling_height);
    fprintf(f, "  },\n");
    fprintf(f, "  \"error\": {\n");
    fprintf(f, "    \"min_pct\": %.3f,\n", g_telemetry.min_err_pct);
    fprintf(f, "    \"max_pct\": %.3f,\n", g_telemetry.max_err_pct);
    fprintf(f, "    \"avg_pct\": %.3f,\n", g_telemetry.avg_err_pct);
    fprintf(f, "    \"pixels_lt_0_1_pct\": %d,\n", g_telemetry.count_lt_01);
    fprintf(f, "    \"pixels_lt_1_0_pct\": %d,\n", g_telemetry.count_lt_1);
    fprintf(f, "    \"pixels_gte_1_0_pct\": %d,\n", g_telemetry.count_gte_1);
    fprintf(f, "    \"ratio_lt_0_1_pct\": %.2f,\n", g_telemetry.ratio_lt_01);
    fprintf(f, "    \"ratio_lt_1_0_pct\": %.2f,\n", g_telemetry.ratio_lt_1);
    fprintf(f, "    \"ratio_gte_1_0_pct\": %.2f,\n", g_telemetry.ratio_gte_1);
    fprintf(f, "    \"spike_screen_x\": %d,\n", g_telemetry.spike_screen_x);
    fprintf(f, "    \"spike_screen_y\": %d\n", g_telemetry.spike_screen_y);
    fprintf(f, "  },\n");
    fprintf(f, "  \"worst_spike\": {\n");
    fprintf(f, "    \"error_pct\": %.3f,\n", g_telemetry.peak_spike_pct);
    fprintf(f, "    \"screen_x\": %d,\n", g_telemetry.spike_screen_x);
    fprintf(f, "    \"screen_y\": %d,\n", g_telemetry.spike_screen_y);
    fprintf(f, "    \"cam_x\": %d,\n", g_telemetry.spike_cam_x);
    fprintf(f, "    \"cam_y\": %d,\n", g_telemetry.spike_cam_y);
    fprintf(f, "    \"cam_z\": %d,\n", g_telemetry.spike_cam_z);
    fprintf(f, "    \"cam_angle\": %u\n", g_telemetry.spike_cam_angle);
    fprintf(f, "  },\n");
    fprintf(f, "  \"vdp1_perf\": {\n");
    fprintf(f, "    \"load_pct\": %.1f,\n", g_telemetry.vdp1_load_pct);
    fprintf(f, "    \"bus_kb\": %d,\n", g_telemetry.bus_kb);
    fprintf(f, "    \"quads_drawn\": %d,\n", g_telemetry.quads_drawn);
    fprintf(f, "    \"quad_pixels\": %d,\n", g_telemetry.quad_pixels);
    fprintf(f, "    \"strips_enabled\": %d\n", g_telemetry.strips_enabled);
    fprintf(f, "  },\n");
    fprintf(f, "  \"saturn_hud_diag\": {\n");
    fprintf(f, "    \"fps\": %.1f,\n", g_telemetry.hud_diag.fps);
    fprintf(f, "    \"timing_ms\": {\n");
    fprintf(f, "      \"r_render\": %d,\n", g_telemetry.hud_diag.ms_r);
    fprintf(f, "      \"v_vdp1\": %d,\n", g_telemetry.hud_diag.ms_v);
    fprintf(f, "      \"t_tics\": %d,\n", g_telemetry.hud_diag.ms_t);
    fprintf(f, "      \"c_letterbox\": %d,\n", g_telemetry.hud_diag.ms_c);
    fprintf(f, "      \"b_bsp\": %d,\n", g_telemetry.hud_diag.ms_b);
    fprintf(f, "      \"g_segs\": %d,\n", g_telemetry.hud_diag.ms_g);
    fprintf(f, "      \"s_vblank\": %d,\n", g_telemetry.hud_diag.ms_s);
    fprintf(f, "      \"j_slave\": %d\n", g_telemetry.hud_diag.ms_j);
    fprintf(f, "    },\n");
    fprintf(f, "    \"vdp1_quads\": {\n");
    fprintf(f, "      \"wall\": %d,\n", g_telemetry.hud_diag.wall_quads);
    fprintf(f, "      \"flat\": %d,\n", g_telemetry.hud_diag.flat_quads);
    fprintf(f, "      \"total\": %d,\n", g_telemetry.hud_diag.total_quads);
    fprintf(f, "      \"dropped\": %d\n", g_telemetry.hud_diag.quads_dropped);
    fprintf(f, "    },\n");
    fprintf(f, "    \"culling\": {\n");
    fprintf(f, "      \"subsectors_visited\": %d,\n", g_telemetry.hud_diag.subsectors);
    fprintf(f, "      \"occlusion_mode\": %d,\n", g_telemetry.hud_diag.occlusion_mode);
    fprintf(f, "      \"bsp_culls\": %d,\n", g_telemetry.hud_diag.bsp_culls);
    fprintf(f, "      \"segs_iterated\": %d,\n", g_telemetry.hud_diag.segs_iter);
    fprintf(f, "      \"segs_visible\": %d\n", g_telemetry.hud_diag.segs_vis);
    fprintf(f, "    },\n");
    fprintf(f, "    \"vdp1_bus\": {\n");
    fprintf(f, "      \"tex_refused\": %d,\n", g_telemetry.hud_diag.tex_refused);
    fprintf(f, "      \"tex_misses\": %d\n", g_telemetry.hud_diag.tex_miss);
    fprintf(f, "    }\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"csv_recording\": %s,\n", g_telemetry.csv_recording ? "true" : "false");
    fprintf(f, "  \"csv_frames\": %d\n", g_telemetry.csv_frame_count);
    fprintf(f, "}\n");
    fclose(f);

    MoveFileExA("telemetry.json.tmp", "telemetry.json", MOVEFILE_REPLACE_EXISTING);
}

static int s_pending_player_warp = 0;
static int s_pending_warp_x = 0;
static int s_pending_warp_y = 0;
static int s_pending_warp_z = 0;
static unsigned int s_pending_warp_ang = 0;
static int s_pending_screenshot = 0;
static int s_pending_quit = 0;

static void PollAgentCommands(void)
{
    FILE *f = fopen("agent_cmd.txt", "r");
    if (!f) return;

    extern volatile int saturn_warp_map;
    char cmd[256];
    while (fgets(cmd, sizeof(cmd), f)) {
        char *p = cmd + strlen(cmd);
        while (p > cmd && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t')) {
            *--p = '\0';
        }
        if (cmd[0] == '\0' || cmd[0] == '#') continue;

        printf("[Agent Interface] Command received: %s\n", cmd);
        if (strncmp(cmd, "WARP_SPIKE", 10) == 0) {
            Telemetry_WarpToSpike();
        } else if (strncmp(cmd, "JOURNEY", 7) == 0) {
            Telemetry_StartJourney();
        } else if (strncmp(cmd, "MAP ", 4) == 0) {
            int ep = 1, map = 1;
            extern void Win32_RequestLevelWarp(int episode, int map);
            if (sscanf(cmd + 4, "%d %d", &ep, &map) == 2) {
                Win32_RequestLevelWarp(ep, map);
            } else if (sscanf(cmd + 4, "%d", &map) == 1) {
                Win32_RequestLevelWarp(1, map);
            }
        } else if (strncmp(cmd, "WARP ", 5) == 0) {
            int x, y, z;
            unsigned int ang = 0;
            if (sscanf(cmd + 5, "%d %d %d %x", &x, &y, &z, &ang) >= 3) {
                if (saturn_warp_map > 0) {
                    s_pending_player_warp = 1;
                    s_pending_warp_x = x;
                    s_pending_warp_y = y;
                    s_pending_warp_z = z;
                    s_pending_warp_ang = ang;
                    printf("[Agent Interface] Queued post-map player warp to (%d, %d, %d)\n", x, y, z);
                } else if (players && players[0].mo) {
                    subsector_t *ss = R_PointInSubsector(x * FRACUNIT, y * FRACUNIT);
                    if (ss && (unsigned)ss->sector < (unsigned)numsectors) {
                        P_UnsetThingPosition(players[0].mo);
                        players[0].mo->x = x * FRACUNIT;
                        players[0].mo->y = y * FRACUNIT;
                        players[0].mo->z = z * FRACUNIT;
                        players[0].viewz = z * FRACUNIT;
                        players[0].mo->angle = ang;
                        Saturn_HW_ForceSkyRepaint();
                        P_SetThingPosition(players[0].mo);
                        printf("[Agent Interface] Warped to (%d, %d, %d) angle 0x%08X\n", x, y, z, ang);
                    } else {
                        printf("[Agent Interface] WARP aborted: (%d, %d) is outside map sector boundaries\n", x, y);
                    }
                }
            }
        } else if (strncmp(cmd, "CSV_START", 9) == 0) {
            if (!g_telemetry.csv_recording) Telemetry_ToggleCSV();
        } else if (strncmp(cmd, "CSV_STOP", 8) == 0) {
            if (g_telemetry.csv_recording) Telemetry_ToggleCSV();
        } else if (strncmp(cmd, "SCREENSHOT", 10) == 0) {
            s_pending_screenshot = 3;
            printf("[Agent Interface] Screenshot queued (waiting 3 frames for render sync).\n");
        } else if (strncmp(cmd, "SET_STRIPS ", 11) == 0) {
            extern int saturn_wall_subdivision;
            saturn_wall_subdivision = atoi(cmd + 11);
            printf("[Agent Interface] Wall strip subdivision set to %d\n", saturn_wall_subdivision);
        } else if (strncmp(cmd, "COMPLETE_LEVEL", 14) == 0) {
            extern int saturn_level_done;
            extern int saturn_secret_exit;
            saturn_secret_exit = 0;
            saturn_level_done = 1;
            gameaction = ga_completed;
            printf("[Agent Interface] Level completion triggered.\n");
        } else if (strncmp(cmd, "SECRET_EXIT", 11) == 0) {
            extern int saturn_level_done;
            extern int saturn_secret_exit;
            saturn_secret_exit = 1;
            saturn_level_done = 1;
            gameaction = ga_secretexit;
            printf("[Agent Interface] Secret exit triggered.\n");
        } else if (strncmp(cmd, "PRESS ", 6) == 0) {
            extern int s_injected_buttons;
            s_injected_buttons |= (int)strtol(cmd + 6, NULL, 0);
            printf("[Agent Interface] Injected buttons 0x%X\n", s_injected_buttons);
        } else if (strncmp(cmd, "QUIT", 4) == 0) {
            if (s_pending_screenshot > 0 || saturn_warp_map > 0 || s_pending_player_warp) {
                s_pending_quit = 1;
                printf("[Agent Interface] Quit queued after pending screenshot.\n");
            } else {
                printf("[Agent Interface] Quit command received.\n");
                fclose(f);
                remove("agent_cmd.txt");
                exit(0);
            }
        }
    }
    fclose(f);
    remove("agent_cmd.txt");
}

void I_FinishUpdate(void)
{
    Win32_PumpEvents();

    /* Apply pending player warp once level load completes */
    extern volatile int saturn_warp_map;
    if (s_pending_player_warp && saturn_warp_map == 0 && players && players[0].mo) {
        subsector_t *ss = R_PointInSubsector(s_pending_warp_x * FRACUNIT, s_pending_warp_y * FRACUNIT);
        if (ss && (unsigned)ss->sector < (unsigned)numsectors) {
            P_UnsetThingPosition(players[0].mo);
            players[0].mo->x = s_pending_warp_x * FRACUNIT;
            players[0].mo->y = s_pending_warp_y * FRACUNIT;
            players[0].mo->z = s_pending_warp_z * FRACUNIT;
            players[0].viewz = s_pending_warp_z * FRACUNIT;
            players[0].mo->angle = s_pending_warp_ang;
            Saturn_HW_ForceSkyRepaint();
            P_SetThingPosition(players[0].mo);
            printf("[Agent Interface] Applied pending warp to (%d, %d, %d) angle 0x%08X\n",
                   s_pending_warp_x, s_pending_warp_y, s_pending_warp_z, s_pending_warp_ang);
        } else {
            printf("[Agent Interface] Pending warp failed: (%d, %d) outside map sector bounds\n",
                   s_pending_warp_x, s_pending_warp_y);
        }
        s_pending_player_warp = 0;
    }

    extern void Saturn_Vdp1Wall_Init(void);
    Saturn_Vdp1Wall_Init();

    /* 1. Deliver Saturn VDP1 commands */
    Saturn_Vdp1Wall_Frame();

    /* 2. Rasterize Saturn VDP1 quads over VDP2 background into Pane 1 */
    VDP1_RenderFrame(s_saturn_fb, PANE_W, PANE_H, I_VideoBuffer);

    extern int gametic;
    extern int saturn_episode;
    extern int saturn_map;
    extern int saturn_view_h;
    extern int saturn_wall_subdivision;
    extern int saturn_show_fps;
    extern int saturn_game_started;
    extern boolean gamepaused;

    /* 3. Sync and Render Canonical DOS DOOM into Pane 2 */
    if (!saturn_game_started || gamepaused) {
        memcpy(s_dos_fb, s_saturn_fb, PANE_W * PANE_H * sizeof(uint32_t));
    } else if (s_dos_ready && pDOS_SetCamera && pDOS_RenderFrame && players && players[0].mo) {
        int cx = players[0].mo->x;
        int cy = players[0].mo->y;
        int cz = players[0].viewz;
        unsigned int c_ang = players[0].mo->angle;
        int dos_map_id = (saturn_episode * 100) + saturn_map;
        pDOS_SetCamera(dos_map_id, cx, cy, cz, c_ang, saturn_view_h);
        if (pDOS_SetWeapon) {
            pDOS_SetWeapon(players[0].readyweapon,
                           players[0].psprites[0].state,
                           players[0].psprites[0].sx,
                           players[0].psprites[0].sy,
                           players[0].psprites[1].state,
                           players[0].psprites[1].sx,
                           players[0].psprites[1].sy,
                           players[0].extralight);
        }
        if (pDOS_SyncMobjs && sectors && numsectors > 0) {
            static dos_sync_mobj_t s_sync_list[1024];
            int sync_count = 0;

            for (int s = 0; s < numsectors && sync_count < 1024; s++) {
                for (mobj_t *mo = (mobj_t *)sectors[s].thinglist; mo && sync_count < 1024; mo = (mobj_t *)mo->snext) {
                    if (mo == players[0].mo) continue;
                    if ((mo->flags & MF_NOSECTOR) || !mo->state || (unsigned)mo->state >= NUMSTATES) continue;
                    const state_t *st = &states[mo->state];
                    if (st->sprite < 0 || st->sprite >= NUMSPRITES) continue;

                    dos_sync_mobj_t *dst = &s_sync_list[sync_count++];
                    dst->x = mo->x;
                    dst->y = mo->y;
                    dst->z = mo->z;
                    dst->angle = mo->angle;
                    memcpy(dst->name, sprnames[st->sprite], 4);
                    dst->frame = st->frame;
                    dst->flags = mo->flags;
                    dst->sector_idx = s;
                }
            }
            pDOS_SyncMobjs(s_sync_list, sync_count);
        }
        pDOS_RenderFrame(s_dos_fb);
    } else {
        /* If DOS ref not ready or level not loaded, paint dark gray in Pane 2 */
        for (int i = 0; i < PANE_W * PANE_H; i++) {
            s_dos_fb[i] = 0x001A1A1A;
        }
    }

    /* 4. Calculate Differential Heat Map & Error Statistics into Pane 3 */
    int active_3d_h = saturn_view_h;
    if (active_3d_h <= 0 || active_3d_h > PANE_H) active_3d_h = 168;

    float min_err = 100.0f;
    float max_err = 0.0f;
    double sum_err = 0.0;
    int count_lt_01 = 0;
    int count_lt_1 = 0;
    int count_gte_1 = 0;
    int x_off = (PANE_W > 320) ? (PANE_W - 320) / 2 : 0;
    int active_w = (PANE_W > 320) ? 320 : PANE_W;
    int total_pixels = active_w * active_3d_h;
    int cur_spike_x = 0, cur_spike_y = 0;

    for (int y = 0; y < PANE_H; y++) {
        uint32_t *sat_row = s_saturn_fb + y * PANE_W;
        uint32_t *dos_row = s_dos_fb + y * PANE_W;
        uint32_t *heat_row = s_heat_fb + y * PANE_W;

        if (y < active_3d_h && s_dos_ready) {
            /* Left border (x < x_off): Dark slate */
            for (int x = 0; x < x_off; x++) {
                heat_row[x] = 0x000F172A;
            }
            /* Active 3D render window (x_off <= x < x_off + active_w) */
            for (int x = x_off; x < x_off + active_w; x++) {
                uint32_t c_sat = sat_row[x];
                uint32_t c_dos = dos_row[x];

                int rs = (c_sat >> 16) & 0xFF;
                int gs = (c_sat >> 8) & 0xFF;
                int bs = (c_sat) & 0xFF;

                int rd = (c_dos >> 16) & 0xFF;
                int gd = (c_dos >> 8) & 0xFF;
                int bd = (c_dos) & 0xFF;

                int dr = abs(rs - rd);
                int dg = abs(gs - gd);
                int db = abs(bs - bd);

                float err_pct = ((float)(dr + dg + db) / (3.0f * 255.0f)) * 100.0f;
                sum_err += err_pct;

                if (err_pct < min_err) min_err = err_pct;
                if (err_pct > max_err) {
                    max_err = err_pct;
                    cur_spike_x = x;
                    cur_spike_y = y;
                }

                if (err_pct < 0.1f) {
                    count_lt_01++;
                    heat_row[x] = 0x000F172A; /* Dark Slate Blue (Match) */
                } else if (err_pct < 1.0f) {
                    count_lt_1++;
                    int g = (int)(err_pct * 220.0f) + 30;
                    if (g > 255) g = 255;
                    heat_row[x] = (0x00 << 16) | (g << 8) | (g >> 1); /* Emerald Cyan */
                } else {
                    count_gte_1++;
                    int r = (int)(err_pct * 30.0f) + 120;
                    if (r > 255) r = 255;
                    int g = (int)((err_pct - 1.0f) * 15.0f);
                    if (g > 200) g = 200;
                    heat_row[x] = (r << 16) | (g << 8) | 0x00; /* Blaze Amber/Red */
                }
            }
            /* Right border (x >= x_off + active_w): Dark slate */
            for (int x = x_off + active_w; x < PANE_W; x++) {
                heat_row[x] = 0x000F172A;
            }
        } else {
            /* Lower HUD or Status Bar area */
            for (int x = 0; x < PANE_W; x++) {
                heat_row[x] = 0x00121620;
            }
        }
    }

    if (total_pixels > 0 && s_dos_ready) {
        g_telemetry.min_err_pct = min_err;
        g_telemetry.max_err_pct = max_err;
        g_telemetry.avg_err_pct = (float)(sum_err / (double)total_pixels);
        g_telemetry.count_lt_01 = count_lt_01;
        g_telemetry.count_lt_1  = count_lt_1;
        g_telemetry.count_gte_1 = count_gte_1;
        g_telemetry.total_viewport_pixels = total_pixels;
        g_telemetry.ratio_lt_01 = ((float)count_lt_01 / (float)total_pixels) * 100.0f;
        g_telemetry.ratio_lt_1  = ((float)count_lt_1  / (float)total_pixels) * 100.0f;
        g_telemetry.ratio_gte_1 = ((float)count_gte_1 / (float)total_pixels) * 100.0f;

        g_telemetry.spike_screen_x = cur_spike_x;
        g_telemetry.spike_screen_y = cur_spike_y;

        /* Track overall worst error spike seen for instant Warp (F8) */
        if (max_err > g_telemetry.peak_spike_pct && players && players[0].mo) {
            g_telemetry.peak_spike_pct   = max_err;
            g_telemetry.spike_cam_x      = players[0].mo->x;
            g_telemetry.spike_cam_y      = players[0].mo->y;
            g_telemetry.spike_cam_z      = players[0].viewz;
            g_telemetry.spike_cam_angle  = players[0].mo->angle;
        }
    }

    /* 5. Composite Header Bar and 3 Panes into s_combined_fb (960x240) */
    /* 5a. Clear & Paint Dedicated Header Bar outside comparison area */
    for (int y = 0; y < HEADER_H; y++) {
        uint32_t *dst = s_combined_fb + y * COMBINED_W;
        for (int x = 0; x < COMBINED_W; x++) {
            dst[x] = 0x000F172A; /* Deep dark slate background */
        }
        /* Vertical column separators in header */
        dst[PANE_W]     = 0x00334155;
        dst[PANE_W * 2] = 0x00334155;
    }
    /* Horizontal divider separating header from viewports */
    uint32_t *hdr_border = s_combined_fb + (HEADER_H - 1) * COMBINED_W;
    for (int x = 0; x < COMBINED_W; x++) {
        hdr_border[x] = 0x00334155;
    }

    /* 5b. Header Labels located in header bar (completely outside the compared area) */
    DrawText5x7(s_combined_fb, COMBINED_W, 8, 4, "1. SEGA SATURN VDP1 (352x224)", 0x00FFFFFF, 0x000F172A);
    DrawText5x7(s_combined_fb, COMBINED_W, PANE_W + 8, 4, "2. DOS DOOM REF (320x168 in 352)", 0x00FFFFFF, 0x000F172A);
    DrawText5x7(s_combined_fb, COMBINED_W, PANE_W * 2 + 8, 4, "3. DIFFERENTIAL HEAT MAP", 0x00FFFFFF, 0x000F172A);

    /* 5c. Blit All 3 Panes below header bar */
    for (int y = 0; y < PANE_H; y++) {
        uint32_t *dst = s_combined_fb + (y + HEADER_H) * COMBINED_W;
        memcpy(dst + 0,          s_saturn_fb + y * PANE_W, PANE_W * sizeof(uint32_t));
        memcpy(dst + PANE_W,      s_dos_fb    + y * PANE_W, PANE_W * sizeof(uint32_t));
        memcpy(dst + PANE_W * 2,  s_heat_fb   + y * PANE_W, PANE_W * sizeof(uint32_t));

        /* Crisp vertical separator lines */
        dst[PANE_W]     = 0x00334155;
        dst[PANE_W * 2] = 0x00334155;
    }

    if (s_pending_screenshot > 0 && saturn_warp_map == 0 && !s_pending_player_warp) {
        s_pending_screenshot--;
        if (s_pending_screenshot == 0) {
            extern void VDP1_DumpCommands(const char *filename);
            VDP1_DumpCommands("vdp1_cmds.txt");
            SaveFramebufferBMP("win32_triple_pane.bmp", s_combined_fb, COMBINED_W, COMBINED_H);
            SaveFramebufferBMP("win32_saturn_pane.bmp", s_saturn_fb, PANE_W, PANE_H);
            SaveFramebufferBMP("win32_dos_pane.bmp", s_dos_fb, PANE_W, PANE_H);
            SaveFramebufferBMP("win32_heatmap_pane.bmp", s_heat_fb, PANE_W, PANE_H);
            printf("[Agent Interface] Captured pending post-warp screenshot.\n");
            printf("[HEATMAP STATS] Avg Error: %.2f%% | Min: %.2f%% | Max: %.2f%% | <0.1%%: %.1f%% | <1.0%%: %.1f%% | >=1.0%%: %.1f%%\n",
                   g_telemetry.avg_err_pct, g_telemetry.min_err_pct, g_telemetry.max_err_pct,
                   g_telemetry.ratio_lt_01, g_telemetry.ratio_lt_1, g_telemetry.ratio_gte_1);
            if (s_pending_quit) {
                printf("[Agent Interface] Exiting after screenshot.\n");
                exit(0);
            }
        }
    }

    /* 6. Blit Combined 960x224 Frame to Window 1 */
    if (s_hwnd) {
        HDC hdc = GetDC(s_hwnd);
        RECT rc;
        GetClientRect(s_hwnd, &rc);
        int win_w = rc.right - rc.left;
        int win_h = rc.bottom - rc.top;

        StretchDIBits(hdc,
                      0, 0, win_w, win_h,
                      0, 0, COMBINED_W, COMBINED_H,
                      s_combined_fb,
                      &s_bmi,
                      DIB_RGB_COLORS,
                      SRCCOPY);

        ReleaseDC(s_hwnd, hdc);
    }

    /* 7. Update Telemetry State for Window 2 & Flight Log */
    vdp1_perf_t perf;
    VDP1_GetPerfStats(&perf);

    g_telemetry.gametic = gametic;
    g_telemetry.episode = saturn_episode;
    g_telemetry.map = saturn_map;
    g_telemetry.fps = (float)perf.target_fps;
    g_telemetry.vdp1_load_pct = perf.vdp1_load_pct;
    g_telemetry.bus_kb = (perf.bus_total_bytes > 0)
        ? ((perf.bus_total_bytes + 1023) / 1024)
        : ((perf.bus_cmd_bytes + perf.scene_tex_full_bytes + 1023) / 1024);
    g_telemetry.quads_drawn = perf.quads_drawn;
    g_telemetry.quad_pixels = perf.quad_pixels;
    g_telemetry.strips_enabled = saturn_wall_subdivision;

    if (players && players[0].mo) {
        g_telemetry.cam_x = players[0].mo->x >> FRACBITS;
        g_telemetry.cam_y = players[0].mo->y >> FRACBITS;
        g_telemetry.cam_z = players[0].viewz >> FRACBITS;
        g_telemetry.cam_angle = players[0].mo->angle;
        g_telemetry.cam_angle_deg = (float)players[0].mo->angle * (360.0f / 4294967296.0f);
        if (players[0].mo->subsector) {
            g_telemetry.subsector = (int)(players[0].mo->subsector - subsectors);
            sector_t *sec = SSEC_SECTOR(players[0].mo->subsector);
            if (sec) {
                g_telemetry.sector = (int)(sec - sectors);
                g_telemetry.floor_height = sec->floorheight >> FRACBITS;
                g_telemetry.ceiling_height = sec->ceilingheight >> FRACBITS;
            }
        }
    }

    /* Invalidate Window 2 for smooth refresh */
    static int s_dash_tick = 0;
    if (g_hwnd_telemetry && (++s_dash_tick % 2 == 0)) {
        InvalidateRect(g_hwnd_telemetry, NULL, FALSE);
    }

    /* 8. Flight Data Recorder: Log Frame to CSV */
    g_telemetry.hud_diag = g_saturn_hud_diag;

    if (g_telemetry.csv_recording && s_csv_fp) {
        DWORD now = GetTickCount();
        fprintf(s_csv_fp, "%lu,%d,%d,%d,%d,%d,%.2f,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%d,%d,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                now,
                g_telemetry.gametic,
                g_telemetry.map,
                g_telemetry.cam_x,
                g_telemetry.cam_y,
                g_telemetry.cam_z,
                g_telemetry.cam_angle_deg,
                g_telemetry.sector,
                g_telemetry.subsector,
                g_telemetry.floor_height,
                g_telemetry.ceiling_height,
                g_telemetry.min_err_pct,
                g_telemetry.max_err_pct,
                g_telemetry.avg_err_pct,
                g_telemetry.ratio_lt_01,
                g_telemetry.ratio_lt_1,
                g_telemetry.ratio_gte_1,
                g_telemetry.spike_screen_x,
                g_telemetry.spike_screen_y,
                /* Saturn Hardware & Diagnostic HUD (Rows 200-223) */
                g_telemetry.hud_diag.fps,
                g_telemetry.hud_diag.ms_r,
                g_telemetry.hud_diag.ms_v,
                g_telemetry.hud_diag.ms_t,
                g_telemetry.hud_diag.ms_c,
                g_telemetry.hud_diag.ms_b,
                g_telemetry.hud_diag.ms_g,
                g_telemetry.hud_diag.ms_s,
                g_telemetry.hud_diag.ms_j,
                g_telemetry.hud_diag.wall_quads,
                g_telemetry.hud_diag.flat_quads,
                g_telemetry.hud_diag.total_quads,
                g_telemetry.hud_diag.quads_dropped,
                g_telemetry.hud_diag.subsectors,
                g_telemetry.hud_diag.occlusion_mode,
                g_telemetry.hud_diag.bsp_culls,
                g_telemetry.hud_diag.segs_iter,
                g_telemetry.hud_diag.segs_vis,
                g_telemetry.hud_diag.tex_refused,
                g_telemetry.hud_diag.tex_miss,
                g_telemetry.hud_diag.wall_strips,
                g_telemetry.hud_diag.flat_lod0_subs,
                g_telemetry.hud_diag.flat_lod1_subs,
                g_telemetry.hud_diag.flat_lod_saved_quads);
        g_telemetry.csv_frame_count++;
        if (g_telemetry.csv_frame_count % 15 == 0) {
            fflush(s_csv_fp);
        }
    }

    /* 9. AI Assistant Programmatic Interface */
    ExportTelemetryJSON();
    PollAgentCommands();

    /* 10. Automated Regression Test Modes */
    int is_test = g_win32_test_mode;

    /* Automated Level Journey Flight Tour */
    if (is_test == 20 && gametic >= 10 && !s_journey_active && s_journey_frame_count == 0) {
        Telemetry_StartJourney();
    }

    if (s_journey_active && players && players[0].mo) {
        extern subsector_t *R_PointInSubsector2(fixed_t x, fixed_t y, int node);

        /* Accumulate journey stats */
        if (g_telemetry.avg_err_pct > 0.001f) {
            s_journey_sum_err += g_telemetry.avg_err_pct;
            s_journey_sum_fps += g_telemetry.fps;
            s_journey_sum_lt01 += g_telemetry.ratio_lt_01;
            s_journey_frame_count++;
            if (g_telemetry.avg_err_pct < s_journey_min_err) s_journey_min_err = g_telemetry.avg_err_pct;
            if (g_telemetry.avg_err_pct > s_journey_max_err) s_journey_max_err = g_telemetry.avg_err_pct;
        }

        const journey_waypoint_t *wp_curr = &s_e1m1_journey[s_journey_wp_idx];
        const journey_waypoint_t *wp_next = (s_journey_wp_idx + 1 < NUM_JOURNEY_WAYPOINTS)
            ? &s_e1m1_journey[s_journey_wp_idx + 1] : wp_curr;

        int cur_x, cur_y;
        unsigned int cur_ang;

        if (s_journey_phase == 0) {
            /* Phase 0: Dwelling at waypoint */
            cur_x = wp_curr->x;
            cur_y = wp_curr->y;
            cur_ang = wp_curr->angle;

            /* Capture screenshot at middle of dwell */
            if (s_journey_timer == (wp_curr->dwell_tics / 2) && wp_curr->screenshot_prefix) {
                char fn[128];
                sprintf(fn, "%s_triple.bmp", wp_curr->screenshot_prefix);
                SaveFramebufferBMP(fn, s_combined_fb, COMBINED_W, COMBINED_H);
                sprintf(fn, "%s_saturn.bmp", wp_curr->screenshot_prefix);
                SaveFramebufferBMP(fn, s_saturn_fb, PANE_W, PANE_H);
                sprintf(fn, "%s_dos.bmp", wp_curr->screenshot_prefix);
                SaveFramebufferBMP(fn, s_dos_fb, PANE_W, PANE_H);
                sprintf(fn, "%s_heat.bmp", wp_curr->screenshot_prefix);
                SaveFramebufferBMP(fn, s_heat_fb, PANE_W, PANE_H);
                printf("[Flight Recorder] Captured screenshots for WP %d: %s\n", s_journey_wp_idx, wp_curr->name);
            }

            s_journey_timer++;
            if (s_journey_timer >= wp_curr->dwell_tics) {
                if (s_journey_wp_idx + 1 < NUM_JOURNEY_WAYPOINTS) {
                    s_journey_phase = 1;
                    s_journey_timer = 0;
                } else {
                    /* Tour Complete! */
                    s_journey_active = 0;
                    if (g_telemetry.csv_recording) {
                        Telemetry_ToggleCSV();
                    }
                    printf("====================================================================\n");
                    printf(">>> [FLIGHT RECORDER] JOURNEY TOUR COMPLETE! <<<\n");
                    printf(">>> Frames Logged: %d\n", s_journey_frame_count);
                    if (s_journey_frame_count > 0) {
                        printf(">>> Full-Level Average Error:   %.3f%%\n", (float)(s_journey_sum_err / s_journey_frame_count));
                        printf(">>> Best Frame Error:           %.3f%%\n", s_journey_min_err);
                        printf(">>> Peak Error:                 %.3f%%\n", s_journey_max_err);
                        printf(">>> Bit-Exact Pixels (<0.1%%):   %.2f%%\n", (float)(s_journey_sum_lt01 / s_journey_frame_count));
                        printf(">>> Average Saturn Render FPS:  %.1f FPS\n", (float)(s_journey_sum_fps / s_journey_frame_count));
                    }
                    printf("====================================================================\n");
                    if (g_win32_test_mode == 20) {
                        printf("[Flight Recorder] Automated journey test complete. Exiting.\n");
                        exit(0);
                    }
                }
            }
        } else {
            /* Phase 1: Smooth interpolation between wp_curr and wp_next */
            int total = GLIDE_TICS_BETWEEN_WPS;
            int step = s_journey_timer;
            cur_x = wp_curr->x + ((wp_next->x - wp_curr->x) * step) / total;
            cur_y = wp_curr->y + ((wp_next->y - wp_curr->y) * step) / total;

            int32_t diff_ang = (int32_t)(wp_next->angle - wp_curr->angle);
            cur_ang = wp_curr->angle + (unsigned int)((int64_t)diff_ang * step / total);

            s_journey_timer++;
            if (s_journey_timer >= total) {
                s_journey_wp_idx++;
                s_journey_phase = 0;
                s_journey_timer = 0;
                printf("[Flight Recorder] Reached WP %d/%d: %s\n",
                       s_journey_wp_idx, NUM_JOURNEY_WAYPOINTS - 1, s_e1m1_journey[s_journey_wp_idx].name);
            }
        }

        /* Update player thing position with noclip so it glides seamlessly */
        P_UnsetThingPosition(players[0].mo);
        players[0].mo->x = cur_x * FRACUNIT;
        players[0].mo->y = cur_y * FRACUNIT;
        players[0].mo->angle = cur_ang;
        subsector_t *ss = R_PointInSubsector2(players[0].mo->x, players[0].mo->y, numnodes - 1);
        if (ss) {
            sector_t *sec = SSEC_SECTOR(ss);
            players[0].mo->subsector = ss;
            players[0].mo->floorz = sec->floorheight;
            players[0].mo->ceilingz = sec->ceilingheight;
            players[0].mo->z = sec->floorheight;
            players[0].viewz = sec->floorheight + 41 * FRACUNIT;
        } else {
            players[0].mo->z = 0;
            players[0].viewz = 41 * FRACUNIT;
        }
        Saturn_HW_ForceSkyRepaint();
        P_SetThingPosition(players[0].mo);
    }
    static int s_saved_bmp = 0;
    if (!s_saved_bmp && gametic >= 10 && is_test == 0) {
        SaveFramebufferBMP("win32_triple_pane.bmp", s_combined_fb, COMBINED_W, COMBINED_H);
        SaveFramebufferBMP("win32_saturn_pane.bmp", s_saturn_fb, PANE_W, PANE_H);
        SaveFramebufferBMP("win32_dos_pane.bmp", s_dos_fb, PANE_W, PANE_H);
        SaveFramebufferBMP("win32_heatmap_pane.bmp", s_heat_fb, PANE_W, PANE_H);
        s_saved_bmp = 1;
        printf("Saved initial triple-pane BMPs at gametic=%d\n", gametic);
    }

    if (is_test == 10) {
        if (gametic >= 12 && gametic < 20 && players[0].mo) {
            P_UnsetThingPosition(players[0].mo);
            players[0].mo->x = 416 * FRACUNIT;
            players[0].mo->y = 2520 * FRACUNIT;
            subsector_t *ss = R_PointInSubsector2(players[0].mo->x, players[0].mo->y, numnodes - 1);
            sector_t *sec = SSEC_SECTOR(ss);
            players[0].mo->subsector = ss;
            players[0].mo->floorz = sec->floorheight;
            players[0].mo->ceilingz = sec->ceilingheight;
            players[0].mo->z = players[0].mo->floorz;
            players[0].viewz = players[0].mo->z + 41 * FRACUNIT;
            players[0].mo->angle = 0x40000000;
            Saturn_HW_ForceSkyRepaint();
            P_SetThingPosition(players[0].mo);
        }
        if (gametic == 20) {
            SaveFramebufferBMP("win32_e1m8_ledge.bmp", s_saturn_fb, PANE_W, PANE_H);
            SaveFramebufferBMP("win32_e1m8_ledge_triple.bmp", s_combined_fb, COMBINED_W, COMBINED_H);
            printf("Saved win32_e1m8_ledge.bmp at gametic=%d\n", gametic); fflush(stdout);
            exit(0);
        }
    }

    /* Pacing */
    if (!is_test && VDP1_GetHardwareLimitEnabled()) {
        static LARGE_INTEGER s_perf_freq = { 0 };
        static LARGE_INTEGER s_frame_start = { 0 };
        if (s_perf_freq.QuadPart == 0) {
            QueryPerformanceFrequency(&s_perf_freq);
            QueryPerformanceCounter(&s_frame_start);
        }

        double target_sec = (double)perf.vblanks_required / 59.94;
        LONGLONG target_ticks = (LONGLONG)(target_sec * (double)s_perf_freq.QuadPart);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG elapsed = now.QuadPart - s_frame_start.QuadPart;

        if (elapsed < target_ticks) {
            LONGLONG remaining_ticks = target_ticks - elapsed;
            DWORD sleep_ms = (DWORD)((remaining_ticks * 1000) / s_perf_freq.QuadPart);
            if (sleep_ms > 1) {
                Sleep(sleep_ms - 1);
            }
            do {
                QueryPerformanceCounter(&now);
            } while ((now.QuadPart - s_frame_start.QuadPart) < target_ticks);
        }
        QueryPerformanceCounter(&s_frame_start);
    }

    saturn_vbl_count += (perf.vblanks_required > 0) ? perf.vblanks_required : 1;
}

/* Keyboard Input Handling */
static int KeyHeld(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int s_injected_buttons = 0;

int I_ReadControls(void)
{
    int buttons = 0;
    extern int gametic;
    int is_test = g_win32_test_mode;

    Win32_PumpEvents();

    if (s_injected_buttons) {
        buttons |= s_injected_buttons;
        s_injected_buttons = 0;
    }

    if (is_test == 1) {
        if (gametic > 10 && gametic <= 40) buttons |= BT_MOVEUP;
        if (gametic > 40 && gametic <= 70) buttons |= BT_MOVERIGHT;
    }

    /* Movement */
    if (KeyHeld('W') || KeyHeld(VK_UP))      buttons |= BT_MOVEUP;
    if (KeyHeld('S') || KeyHeld(VK_DOWN))    buttons |= BT_MOVEDOWN;

    /* Turning & Strafing */
    int strafe_mode = KeyHeld(VK_MENU); /* Alt key */
    if (KeyHeld('A') || KeyHeld(VK_LEFT)) {
        if (strafe_mode || KeyHeld('A'))
            buttons |= BT_STRAFELEFT;
        else
            buttons |= BT_MOVELEFT;
    }
    if (KeyHeld('D') || KeyHeld(VK_RIGHT)) {
        if (strafe_mode || KeyHeld('D'))
            buttons |= BT_STRAFERIGHT;
        else
            buttons |= BT_MOVERIGHT;
    }

    /* Dedicated Strafe */
    if (KeyHeld(VK_OEM_COMMA))  buttons |= BT_STRAFELEFT;
    if (KeyHeld(VK_OEM_PERIOD)) buttons |= BT_STRAFERIGHT;

    /* Actions */
    if (KeyHeld(VK_SPACE) || KeyHeld(VK_BACK))      buttons |= BT_USE;
    if (KeyHeld(VK_CONTROL) || KeyHeld(VK_LBUTTON)) buttons |= BT_ATTACK;
    if (KeyHeld(VK_SHIFT))                          buttons |= BT_SPEED;
    if (KeyHeld(VK_TAB))                            buttons |= BT_AUTOMAP;
    if (KeyHeld(VK_RETURN))                         buttons |= BT_START | BT_ATTACK;
    if (KeyHeld(VK_ESCAPE))                         buttons |= BT_START | BT_PAUSE;

    /* Weapons 1..7 */
    if (KeyHeld('1')) players[0].pendingweapon = wp_fist;
    if (KeyHeld('2')) players[0].pendingweapon = wp_pistol;
    if (KeyHeld('3')) players[0].pendingweapon = wp_shotgun;
    if (KeyHeld('4')) players[0].pendingweapon = wp_chaingun;
    if (KeyHeld('5')) players[0].pendingweapon = wp_missile;
    if (KeyHeld('6')) players[0].pendingweapon = wp_plasma;
    if (KeyHeld('7')) players[0].pendingweapon = wp_bfg;
    if (KeyHeld('Q')) buttons |= BT_PWEAPN;
    if (KeyHeld('E')) buttons |= BT_NWEAPN;

    /* Hotkey F12: Screenshot Triple Pane */
    if (KeyHeld(VK_F12)) {
        SaveFramebufferBMP("win32_triple_pane.bmp", s_combined_fb, COMBINED_W, COMBINED_H);
        SaveFramebufferBMP("win32_saturn_pane.bmp", s_saturn_fb, PANE_W, PANE_H);
        SaveFramebufferBMP("win32_dos_pane.bmp", s_dos_fb, PANE_W, PANE_H);
        SaveFramebufferBMP("win32_heatmap_pane.bmp", s_heat_fb, PANE_W, PANE_H);
        printf("Saved win32_triple_pane.bmp and individual panes!\n"); fflush(stdout);
    }

    /* Hotkey F2: Warp to Courtyard */
    static int f2_prev = 0;
    if (KeyHeld(VK_F2)) {
        if (!f2_prev && players[0].mo) {
            P_UnsetThingPosition(players[0].mo);
            players[0].mo->x = 0;
            players[0].mo->y = -2752 * FRACUNIT;
            players[0].mo->z = players[0].mo->floorz = -136 * FRACUNIT;
            players[0].mo->angle = ANG90;
            P_SetThingPosition(players[0].mo);
        }
        f2_prev = 1;
    } else {
        f2_prev = 0;
    }

    /* Hotkey F3: Noclip */
    static int f3_prev = 0;
    if (KeyHeld(VK_F3)) {
        if (!f3_prev && players[0].mo) {
            players[0].cheats ^= CF_NOCLIP;
        }
        f3_prev = 1;
    } else {
        f3_prev = 0;
    }

    /* Hotkey F4: God Mode */
    static int f4_prev = 0;
    if (KeyHeld(VK_F4)) {
        if (!f4_prev && players[0].mo) {
            players[0].cheats ^= CF_GODMODE;
        }
        f4_prev = 1;
    } else {
        f4_prev = 0;
    }

    /* Hotkey F5: Toggle Wall Strip Subdivision */
    static int f5_prev = 0;
    if (KeyHeld(VK_F5)) {
        if (!f5_prev) {
            extern void Saturn_ToggleWallSubdivision(void);
            Saturn_ToggleWallSubdivision();
        }
        f5_prev = 1;
    } else {
        f5_prev = 0;
    }

    /* Hotkey F6: Toggle On-Screen FPS Counter */
    static int f6_prev = 0;
    if (KeyHeld(VK_F6)) {
        if (!f6_prev) {
            extern int saturn_show_fps;
            saturn_show_fps ^= 1;
            printf("On-Screen FPS Counter: %s\n", saturn_show_fps ? "ON" : "OFF"); fflush(stdout);
        }
        f6_prev = 1;
    } else {
        f6_prev = 0;
    }

    /* Hotkey F7: Toggle CSV Flight Data Recording */
    static int f7_prev = 0;
    if (KeyHeld(VK_F7)) {
        if (!f7_prev) {
            Telemetry_ToggleCSV();
        }
        f7_prev = 1;
    } else {
        f7_prev = 0;
    }

    /* Hotkey F8: Warp to Last Recorded Error Spike */
    static int f8_prev = 0;
    if (KeyHeld(VK_F8)) {
        if (!f8_prev) {
            Telemetry_WarpToSpike();
        }
        f8_prev = 1;
    } else {
        f8_prev = 0;
    }

    /* Hotkey F9: Start/Stop Automated Flight Journey */
    static int f9_prev = 0;
    if (KeyHeld(VK_F9)) {
        if (!f9_prev) {
            Telemetry_StartJourney();
        }
        f9_prev = 1;
    } else {
        f9_prev = 0;
    }

    if (players && consoleplayer >= 0 && consoleplayer < MAXPLAYERS)
    {
        players[consoleplayer].oldticbuttons = players[consoleplayer].ticbuttons;
        players[consoleplayer].ticbuttons = buttons;
    }

    return buttons;
}

void Win32_ToggleNoclip(void)
{
    if (players && players[0].mo) {
        players[0].cheats ^= CF_NOCLIP;
        printf("[Cheats] Noclip %s\n", (players[0].cheats & CF_NOCLIP) ? "ON" : "OFF");
        fflush(stdout);
    }
}

void Win32_ToggleGodMode(void)
{
    if (players && players[0].mo) {
        players[0].cheats ^= CF_GODMODE;
        printf("[Cheats] God Mode %s\n", (players[0].cheats & CF_GODMODE) ? "ON" : "OFF");
        fflush(stdout);
    }
}

void Win32_ToggleStrips(void)
{
    extern void Saturn_ToggleWallSubdivision(void);
    Saturn_ToggleWallSubdivision();
}

void Win32_ToggleFps(void)
{
    extern int saturn_show_fps;
    saturn_show_fps ^= 1;
    printf("[Display] FPS Counter %s\n", saturn_show_fps ? "ON" : "OFF");
    fflush(stdout);
}

void Win32_ToggleOcclusion(void)
{
    extern int saturn_hw_bsp_occl;
    saturn_hw_bsp_occl ^= 1;
    printf("[Renderer] Hardware Occlusion %s\n", saturn_hw_bsp_occl ? "ON" : "OFF");
    fflush(stdout);
}
