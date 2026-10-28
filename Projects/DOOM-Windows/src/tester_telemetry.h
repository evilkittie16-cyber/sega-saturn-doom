#ifndef _TESTER_TELEMETRY_H_
#define _TESTER_TELEMETRY_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

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
} saturn_hud_diag_t;
#endif

extern saturn_hud_diag_t g_saturn_hud_diag;

typedef struct {
    int gametic;
    int episode;
    int map;
    float fps;

    /* Camera / Location */
    int cam_x;
    int cam_y;
    int cam_z;
    unsigned int cam_angle;
    float cam_angle_deg;
    int sector;
    int subsector;
    int floor_height;
    int ceiling_height;

    /* Heat Map Error Metrics */
    float min_err_pct;
    float max_err_pct;
    float avg_err_pct;
    int count_lt_01;
    int count_lt_1;
    int count_gte_1;
    int total_viewport_pixels;
    float ratio_lt_01;
    float ratio_lt_1;
    float ratio_gte_1;

    /* Error Spike Tracker */
    int spike_screen_x;
    int spike_screen_y;
    float peak_spike_pct;
    int spike_cam_x;
    int spike_cam_y;
    int spike_cam_z;
    unsigned int spike_cam_angle;

    /* CSV Flight Recorder */
    int csv_recording;
    int csv_frame_count;
    char csv_filename[256];

    /* VDP1 Hardware Perf */
    float vdp1_load_pct;
    int bus_kb;
    int quads_drawn;
    int quad_pixels;
    int strips_enabled;

    /* Saturn Hardware Diagnostic HUD (Rows 200-223) */
    saturn_hud_diag_t hud_diag;
} tester_telemetry_t;

extern tester_telemetry_t g_telemetry;
extern HWND g_hwnd_main;
extern HWND g_hwnd_telemetry;

void Telemetry_ToggleCSV(void);
void Telemetry_WarpToSpike(void);
void Telemetry_ResetSpike(void);
void Telemetry_StartJourney(void);
void Telemetry_StopJourney(void);
int  Telemetry_IsJourneyActive(void);

#endif /* _TESTER_TELEMETRY_H_ */
