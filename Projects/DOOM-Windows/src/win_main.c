/* win_main.c - Windows Dual-Window Comparator & Telemetry Dashboard */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#include "saturn_win32.h"
#include "tester_telemetry.h"

#define PANE_SCALE 1.5f
#define MAIN_CLIENT_W (int)(1056 * PANE_SCALE) /* 1584 */
#define MAIN_CLIENT_H (int)(240 * PANE_SCALE) /* 360 */

#define DASH_CLIENT_W 1584
#define DASH_CLIENT_H 340

/* Map Table & Menu IDs */
typedef struct {
    int episode;
    int map;
    const char *name;
} doom_map_entry_t;

static const doom_map_entry_t s_all_maps[] = {
    /* Episode 1: Knee-Deep in the Dead */
    { 1, 1, "E1M1: Hangar" },
    { 1, 2, "E1M2: Nuclear Plant" },
    { 1, 3, "E1M3: Toxin Refinery" },
    { 1, 4, "E1M4: Command Control" },
    { 1, 5, "E1M5: Phobos Lab" },
    { 1, 6, "E1M6: Central Processing" },
    { 1, 7, "E1M7: Computer Station" },
    { 1, 8, "E1M8: Phobos Anomaly" },
    { 1, 9, "E1M9: Military Base" },

    /* Episode 2: The Shores of Hell */
    { 2, 1, "E2M1: Deimos Anomaly" },
    { 2, 2, "E2M2: Containment Area" },
    { 2, 3, "E2M3: Refinery" },
    { 2, 4, "E2M4: Deimos Lab" },
    { 2, 5, "E2M5: Command Center" },
    { 2, 6, "E2M6: Halls of the Damned" },
    { 2, 7, "E2M7: Spawning Vats" },
    { 2, 8, "E2M8: Tower of Babel" },
    { 2, 9, "E2M9: Fortress of Mystery" },

    /* Episode 3: Inferno */
    { 3, 1, "E3M1: Hell Keep" },
    { 3, 2, "E3M2: Slough of Despair" },
    { 3, 3, "E3M3: Pandemonium" },
    { 3, 4, "E3M4: House of Pain" },
    { 3, 5, "E3M5: Unholy Cathedral" },
    { 3, 6, "E3M6: Mt. Erebus" },
    { 3, 7, "E3M7: Limbo" },
    { 3, 8, "E3M8: Dis" },
    { 3, 9, "E3M9: Warrens" },

    /* Episode 4: Thy Flesh Consumed */
    { 4, 1, "E4M1: Hell Beneath" },
    { 4, 2, "E4M2: Perfect Hatred" },
    { 4, 3, "E4M3: Sever the Wicked" },
    { 4, 4, "E4M4: Unruly Evil" },
    { 4, 5, "E4M5: They Will Repent" },
    { 4, 6, "E4M6: Against Thee Wickedly" },
    { 4, 7, "E4M7: And Hell Followed" },
    { 4, 8, "E4M8: Unto the Cruel" },
    { 4, 9, "E4M9: Fear" }
};
#define NUM_DOOM_MAPS (sizeof(s_all_maps) / sizeof(s_all_maps[0]))

#define ID_MAP_BASE        2000
#define ID_MAP(ep, m)      (ID_MAP_BASE + ((ep) * 10) + (m))

#define IDC_BTN_CSV        1001
#define IDC_BTN_WARP       1002
#define IDC_BTN_RESET      1003
#define IDC_BTN_JOURNEY    1004
#define IDC_BTN_LEVEL_MENU 1005
#define IDC_COMBO_LEVEL    1006

#define IDM_ACTION_JOURNEY 1010
#define IDM_ACTION_WARP_SP 1011
#define IDM_ACTION_RESET_SP 1012
#define IDM_ACTION_CSV     1013
#define IDM_TOGGLE_STRIPS  1014
#define IDM_TOGGLE_FPS     1015
#define IDM_TOGGLE_OCCL    1016
#define IDM_TOGGLE_NOCLIP  1017
#define IDM_TOGGLE_GOD     1018

HWND g_hwnd_main = NULL;
HWND g_hwnd_telemetry = NULL;
static HWND s_btn_csv = NULL;
static HWND s_btn_warp = NULL;
static HWND s_btn_reset = NULL;
static HWND s_btn_journey = NULL;
static HWND s_btn_level_menu = NULL;
static HWND s_combo_level = NULL;

tester_telemetry_t g_telemetry = { 0 };

extern void Win32_SetWindowHandle(HWND hwnd);
extern void D_DoomMain(void);
extern void Win32_ToggleNoclip(void);
extern void Win32_ToggleGodMode(void);
extern void Win32_ToggleStrips(void);
extern void Win32_ToggleFps(void);
extern void Win32_ToggleOcclusion(void);

extern volatile int saturn_warp_episode;
extern volatile int saturn_warp_map;

void Win32_RequestLevelWarp(int episode, int map)
{
    saturn_warp_episode = episode;
    saturn_warp_map = map;
    printf("[Win32] Level warp queued -> Episode %d, Map %d\n", episode, map);
    fflush(stdout);
}

static HMENU CreateLevelSubmenu(void)
{
    HMENU hMenu = CreatePopupMenu();
    HMENU hEpMenu[4];
    const char *ep_titles[4] = {
        "Episode 1: Knee-Deep in the Dead",
        "Episode 2: The Shores of Hell",
        "Episode 3: Inferno",
        "Episode 4: Thy Flesh Consumed"
    };

    for (int e = 0; e < 4; e++) {
        hEpMenu[e] = CreatePopupMenu();
    }

    for (size_t i = 0; i < NUM_DOOM_MAPS; i++) {
        int ep = s_all_maps[i].episode;
        int m = s_all_maps[i].map;
        if (ep >= 1 && ep <= 4) {
            AppendMenuA(hEpMenu[ep - 1], MF_STRING, ID_MAP(ep, m), s_all_maps[i].name);
        }
    }

    for (int e = 0; e < 4; e++) {
        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hEpMenu[e], ep_titles[e]);
    }

    return hMenu;
}

static HMENU BuildMainMenuBar(void)
{
    HMENU hMenuBar = CreateMenu();

    /* 1. Level Selection Submenu */
    HMENU hLevelMenu = CreateLevelSubmenu();
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hLevelMenu, "&Select Level");

    /* 2. Flight & Diagnostics Submenu */
    HMENU hDiagMenu = CreatePopupMenu();
    AppendMenuA(hDiagMenu, MF_STRING, IDM_ACTION_JOURNEY, "Automated Flight Tour (F9)");
    AppendMenuA(hDiagMenu, MF_STRING, IDM_ACTION_WARP_SP, "Warp to Error Spike (F8)");
    AppendMenuA(hDiagMenu, MF_STRING, IDM_ACTION_RESET_SP, "Reset Spike Tracker");
    AppendMenuA(hDiagMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hDiagMenu, MF_STRING, IDM_ACTION_CSV, "Toggle CSV Flight Recording (F7)");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hDiagMenu, "&Diagnostics");

    /* 3. Renderer & Options Submenu */
    HMENU hOptMenu = CreatePopupMenu();
    AppendMenuA(hOptMenu, MF_STRING, IDM_TOGGLE_STRIPS, "Toggle Strip Subdivision (F5)");
    AppendMenuA(hOptMenu, MF_STRING, IDM_TOGGLE_FPS, "Toggle On-Screen FPS (F6)");
    AppendMenuA(hOptMenu, MF_STRING, IDM_TOGGLE_OCCL, "Toggle Occlusion Culling");
    AppendMenuA(hOptMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hOptMenu, MF_STRING, IDM_TOGGLE_NOCLIP, "Toggle Noclip Mode (F3)");
    AppendMenuA(hOptMenu, MF_STRING, IDM_TOGGLE_GOD, "Toggle God Mode (F4)");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hOptMenu, "&Options");

    return hMenuBar;
}

static void Win32_HandleCommand(HWND hwnd, int cmdId)
{
    (void)hwnd;
    if (cmdId >= ID_MAP(1, 1) && cmdId <= ID_MAP(4, 9)) {
        int ep = (cmdId - ID_MAP_BASE) / 10;
        int map = (cmdId - ID_MAP_BASE) % 10;
        Win32_RequestLevelWarp(ep, map);
        if (s_combo_level) {
            for (size_t i = 0; i < NUM_DOOM_MAPS; i++) {
                if (s_all_maps[i].episode == ep && s_all_maps[i].map == map) {
                    SendMessageA(s_combo_level, CB_SETCURSEL, (WPARAM)i, 0);
                    break;
                }
            }
        }
        if (g_hwnd_telemetry) InvalidateRect(g_hwnd_telemetry, NULL, FALSE);
        return;
    }

    switch (cmdId)
    {
    case IDC_BTN_CSV:
    case IDM_ACTION_CSV:
        Telemetry_ToggleCSV();
        break;
    case IDC_BTN_WARP:
    case IDM_ACTION_WARP_SP:
        Telemetry_WarpToSpike();
        break;
    case IDC_BTN_RESET:
    case IDM_ACTION_RESET_SP:
        Telemetry_ResetSpike();
        break;
    case IDC_BTN_JOURNEY:
    case IDM_ACTION_JOURNEY:
        Telemetry_StartJourney();
        break;
    case IDM_TOGGLE_STRIPS:
        Win32_ToggleStrips();
        break;
    case IDM_TOGGLE_FPS:
        Win32_ToggleFps();
        break;
    case IDM_TOGGLE_OCCL:
        Win32_ToggleOcclusion();
        break;
    case IDM_TOGGLE_NOCLIP:
        Win32_ToggleNoclip();
        break;
    case IDM_TOGGLE_GOD:
        Win32_ToggleGodMode();
        break;
    }

    if (g_hwnd_telemetry) InvalidateRect(g_hwnd_telemetry, NULL, FALSE);
}

/* Window 1 (Main Comparator) Message Handler */
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        Win32_HandleCommand(hwnd, LOWORD(wParam));
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

/* Window 2 (Telemetry Dashboard) Message Handler */
static LRESULT CALLBACK TelemetryWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_BTN_LEVEL_MENU) {
                HMENU hPopup = CreateLevelSubmenu();
                RECT rc;
                GetWindowRect(s_btn_level_menu, &rc);
                TrackPopupMenu(hPopup, TPM_LEFTALIGN | TPM_BOTTOMALIGN, rc.left, rc.top, 0, hwnd, NULL);
                DestroyMenu(hPopup);
                return 0;
            } else if (id == IDC_COMBO_LEVEL && code == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(s_combo_level, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)NUM_DOOM_MAPS) {
                    Win32_RequestLevelWarp(s_all_maps[sel].episode, s_all_maps[sel].map);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }

            Win32_HandleCommand(hwnd, id);
            return 0;
        }
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            /* Double buffering */
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            /* Dark background */
            HBRUSH bgBrush = CreateSolidBrush(RGB(18, 22, 30));
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            /* Fonts */
            HFONT hFontTitle = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HFONT hFontText = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_DONTCARE, "Consolas");
            HFONT hFontBold = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            SetBkMode(memDC, TRANSPARENT);

            /* Update Button Text */
            if (s_btn_csv) {
                SetWindowTextA(s_btn_csv, g_telemetry.csv_recording ? "STOP CSV RECORDING (F7)" : "START CSV RECORDING (F7)");
            }
            if (s_btn_journey) {
                SetWindowTextA(s_btn_journey, Telemetry_IsJourneyActive() ? "STOP FLIGHT (F9)" : "FLIGHT TOUR (F9)");
            }

            /* Draw 3 sleek card panels */
            int card_w = 450;
            int card_h = 240;
            int card_y = 15;

            for (int col = 0; col < 3; col++) {
                int card_x = 20 + col * (card_w + 20);
                RECT card_rc = { card_x, card_y, card_x + card_w, card_y + card_h };

                HBRUSH cardBrush = CreateSolidBrush(RGB(28, 34, 46));
                FillRect(memDC, &card_rc, cardBrush);
                DeleteObject(cardBrush);

                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(50, 60, 80));
                HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, card_rc.left, card_rc.top, card_rc.right, card_rc.bottom);
                SelectObject(memDC, oldPen);
                DeleteObject(borderPen);
            }

            /* === PANEL 1: Map & Coordinates === */
            SelectObject(memDC, hFontTitle);
            SetTextColor(memDC, RGB(100, 200, 255));
            TextOutA(memDC, 35, 25, "LOCATION & TELEMETRY", 21);

            SelectObject(memDC, hFontText);
            SetTextColor(memDC, RGB(220, 225, 235));
            char buf[256];

            int cur_ep = g_telemetry.episode ? g_telemetry.episode : 1;
            int cur_map = g_telemetry.map;

            static int s_last_synced_ep = 0;
            static int s_last_synced_map = 0;
            if (s_combo_level && (cur_ep != s_last_synced_ep || cur_map != s_last_synced_map)) {
                s_last_synced_ep = cur_ep;
                s_last_synced_map = cur_map;
                for (size_t i = 0; i < NUM_DOOM_MAPS; i++) {
                    if (s_all_maps[i].episode == cur_ep && s_all_maps[i].map == cur_map) {
                        SendMessageA(s_combo_level, CB_SETCURSEL, (WPARAM)i, 0);
                        break;
                    }
                }
            }

            sprintf(buf, "Map: E%dM%d   Gametic: %d   FPS: %.1f",
                    cur_ep, cur_map, g_telemetry.gametic, g_telemetry.fps);
            TextOutA(memDC, 35, 55, buf, (int)strlen(buf));

            sprintf(buf, "Camera X: %-6d   Y: %-6d   Z: %-6d",
                    g_telemetry.cam_x, g_telemetry.cam_y, g_telemetry.cam_z);
            TextOutA(memDC, 35, 80, buf, (int)strlen(buf));

            sprintf(buf, "Angle:    0x%08X (%.1f deg)",
                    g_telemetry.cam_angle, g_telemetry.cam_angle_deg);
            TextOutA(memDC, 35, 105, buf, (int)strlen(buf));

            sprintf(buf, "Sector:   %-4d    Subsector: %-4d",
                    g_telemetry.sector, g_telemetry.subsector);
            TextOutA(memDC, 35, 130, buf, (int)strlen(buf));

            sprintf(buf, "Ceiling:  %-4d    Floor:     %-4d",
                    g_telemetry.ceiling_height, g_telemetry.floor_height);
            TextOutA(memDC, 35, 155, buf, (int)strlen(buf));

            sprintf(buf, "Saturn HUD: FPS:%.1f  R:%d V:%d T:%d C:%d",
                    g_telemetry.hud_diag.fps, g_telemetry.hud_diag.ms_r, g_telemetry.hud_diag.ms_v,
                    g_telemetry.hud_diag.ms_t, g_telemetry.hud_diag.ms_c);
            TextOutA(memDC, 35, 185, buf, (int)strlen(buf));

            sprintf(buf, "Quads: W:%d F:%d Q:%d D:%d | Culls: N:%d K:%d U:%d",
                    g_telemetry.hud_diag.wall_quads, g_telemetry.hud_diag.flat_quads,
                    g_telemetry.hud_diag.total_quads, g_telemetry.hud_diag.quads_dropped,
                    g_telemetry.hud_diag.subsectors, g_telemetry.hud_diag.bsp_culls,
                    g_telemetry.hud_diag.segs_vis);
            TextOutA(memDC, 35, 205, buf, (int)strlen(buf));

            extern int saturn_wall_subdivision;
            sprintf(buf, "Strips: %d (%s) | Flats LOD0:%d LOD1:%d (-%d Q)",
                    g_telemetry.hud_diag.wall_strips,
                    saturn_wall_subdivision ? "ON" : "OFF",
                    g_telemetry.hud_diag.flat_lod0_subs,
                    g_telemetry.hud_diag.flat_lod1_subs,
                    g_telemetry.hud_diag.flat_lod_saved_quads);
            SetTextColor(memDC, RGB(120, 220, 255));
            TextOutA(memDC, 35, 228, buf, (int)strlen(buf));
            SetTextColor(memDC, RGB(220, 225, 235));

            /* === PANEL 2: Heat Map & Error Metrics === */
            int col2_x = 490;
            SelectObject(memDC, hFontTitle);
            SetTextColor(memDC, RGB(255, 180, 80));
            TextOutA(memDC, col2_x + 15, 25, "DIFFERENTIAL ERROR HEAT MAP", 27);

            SelectObject(memDC, hFontText);
            SetTextColor(memDC, RGB(220, 225, 235));

            sprintf(buf, "Peak Max Error: %6.2f%%   Average: %6.3f%%",
                    g_telemetry.max_err_pct, g_telemetry.avg_err_pct);
            TextOutA(memDC, col2_x + 15, 55, buf, (int)strlen(buf));

            sprintf(buf, "Minimum Error:  %6.3f%%   Active Px: %d",
                    g_telemetry.min_err_pct, g_telemetry.total_viewport_pixels);
            TextOutA(memDC, col2_x + 15, 80, buf, (int)strlen(buf));

            /* Graphical Distribution Bars */
            /* 1. Match < 0.1% */
            sprintf(buf, "< 0.1%% (Match):    %5.1f%% (%d px)",
                    g_telemetry.ratio_lt_01, g_telemetry.count_lt_01);
            SetTextColor(memDC, RGB(80, 230, 120));
            TextOutA(memDC, col2_x + 15, 110, buf, (int)strlen(buf));

            RECT bar1 = { col2_x + 15, 130, col2_x + 15 + (int)(g_telemetry.ratio_lt_01 * 4.0f), 138 };
            HBRUSH br1 = CreateSolidBrush(RGB(50, 200, 90));
            FillRect(memDC, &bar1, br1);
            DeleteObject(br1);

            /* 2. Minor < 1.0% */
            sprintf(buf, "< 1.0%% (Tolerable):%5.1f%% (%d px)",
                    g_telemetry.ratio_lt_1, g_telemetry.count_lt_1);
            SetTextColor(memDC, RGB(255, 215, 0));
            TextOutA(memDC, col2_x + 15, 145, buf, (int)strlen(buf));

            RECT bar2 = { col2_x + 15, 165, col2_x + 15 + (int)(g_telemetry.ratio_lt_1 * 4.0f), 173 };
            HBRUSH br2 = CreateSolidBrush(RGB(230, 190, 20));
            FillRect(memDC, &bar2, br2);
            DeleteObject(br2);

            /* 3. Discrepancy >= 1.0% */
            sprintf(buf, ">=1.0%% (Discrepant):%4.1f%% (%d px)",
                    g_telemetry.ratio_gte_1, g_telemetry.count_gte_1);
            SetTextColor(memDC, RGB(255, 80, 80));
            TextOutA(memDC, col2_x + 15, 180, buf, (int)strlen(buf));

            RECT bar3 = { col2_x + 15, 200, col2_x + 15 + (int)(g_telemetry.ratio_gte_1 * 4.0f), 208 };
            HBRUSH br3 = CreateSolidBrush(RGB(240, 50, 50));
            FillRect(memDC, &bar3, br3);
            DeleteObject(br3);

            /* === PANEL 3: Error Spikes & Flight Recorder === */
            int col3_x = 960;
            SelectObject(memDC, hFontTitle);
            SetTextColor(memDC, RGB(220, 120, 255));
            TextOutA(memDC, col3_x + 15, 25, "SPIKE TRACKER & RECORDER", 24);

            SelectObject(memDC, hFontText);
            SetTextColor(memDC, RGB(220, 225, 235));

            sprintf(buf, "Spike Pixel:  Screen [%d, %d]",
                    g_telemetry.spike_screen_x, g_telemetry.spike_screen_y);
            TextOutA(memDC, col3_x + 15, 55, buf, (int)strlen(buf));

            sprintf(buf, "Spike Camera: X=%d  Y=%d  Z=%d",
                    g_telemetry.spike_cam_x, g_telemetry.spike_cam_y, g_telemetry.spike_cam_z);
            TextOutA(memDC, col3_x + 15, 80, buf, (int)strlen(buf));

            sprintf(buf, "Spike Angle:  0x%08X (%.1f deg)",
                    g_telemetry.spike_cam_angle, (float)g_telemetry.spike_cam_angle * (360.0f / 4294967296.0f));
            TextOutA(memDC, col3_x + 15, 105, buf, (int)strlen(buf));

            sprintf(buf, "Worst Spike:  %.2f%% Error", g_telemetry.peak_spike_pct);
            SetTextColor(memDC, RGB(255, 100, 100));
            TextOutA(memDC, col3_x + 15, 130, buf, (int)strlen(buf));

            SetTextColor(memDC, RGB(220, 225, 235));
            sprintf(buf, "CSV Flight Log: %s",
                    g_telemetry.csv_recording ? "[RECORDING]" : "[STANDBY]");
            TextOutA(memDC, col3_x + 15, 160, buf, (int)strlen(buf));

            sprintf(buf, "Frames Logged: %-5d", g_telemetry.csv_frame_count);
            TextOutA(memDC, col3_x + 15, 185, buf, (int)strlen(buf));

            if (g_telemetry.csv_filename[0]) {
                sprintf(buf, "File: %s", g_telemetry.csv_filename);
                TextOutA(memDC, col3_x + 15, 210, buf, (int)strlen(buf));
            } else {
                TextOutA(memDC, col3_x + 15, 210, "File: journey_telemetry_MAP_*.csv", 33);
            }

            /* Blit to screen */
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);

            DeleteObject(hFontTitle);
            DeleteObject(hFontText);
            DeleteObject(hFontBold);

            EndPaint(hwnd, &ps);
            return 0;
        }
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

/* Win32 Message Pump hook called from I_FinishUpdate */
void Win32_PumpEvents(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            exit(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    printf("=== DOOM-Saturn Win32 Boot Starting ===\n");
    fflush(stdout);

    /* 1. Register Main Comparator Class */
    WNDCLASSA wc1;
    memset(&wc1, 0, sizeof(wc1));
    wc1.lpfnWndProc = MainWndProc;
    wc1.hInstance = hInstance;
    wc1.lpszClassName = "SaturnDoomWinClass";
    wc1.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc1.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    if (!RegisterClassA(&wc1)) {
        printf("Failed to register main window class! Error: %lu\n", GetLastError());
        return 1;
    }

    /* 2. Register Telemetry Dashboard Class */
    WNDCLASSA wc2;
    memset(&wc2, 0, sizeof(wc2));
    wc2.lpfnWndProc = TelemetryWndProc;
    wc2.hInstance = hInstance;
    wc2.lpszClassName = "SaturnDoomTelemetryClass";
    wc2.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc2.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(18, 22, 30));

    if (!RegisterClassA(&wc2)) {
        printf("Failed to register telemetry window class! Error: %lu\n", GetLastError());
        return 1;
    }

    HMENU hMenuBar = BuildMainMenuBar();

    /* Window 1 Geometry */
    RECT rc1 = { 0, 0, MAIN_CLIENT_W, MAIN_CLIENT_H };
    AdjustWindowRect(&rc1, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, TRUE);
    int w1 = rc1.right - rc1.left;
    int h1 = rc1.bottom - rc1.top;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x1 = (screen_w - w1) / 2;
    if (x1 < 10) x1 = 10;
    int y1 = 20;

    g_hwnd_main = CreateWindowExA(0,
                                 "SaturnDoomWinClass",
                                 "SEGA SATURN DOOM - Real-Time Comparator [Pane 1: Saturn VDP1 | Pane 2: DOS Doom Ref | Pane 3: Heat Map]",
                                 (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
                                 x1, y1, w1, h1,
                                 NULL, hMenuBar, hInstance, NULL);

    if (!g_hwnd_main) {
        printf("Failed to create main window! Error: %lu\n", GetLastError());
        return 1;
    }

    ShowWindow(g_hwnd_main, nCmdShow);
    UpdateWindow(g_hwnd_main);
    Win32_SetWindowHandle(g_hwnd_main);

    /* Window 2 Geometry */
    RECT rc2 = { 0, 0, DASH_CLIENT_W, DASH_CLIENT_H };
    AdjustWindowRect(&rc2, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
    int w2 = rc2.right - rc2.left;
    int h2 = rc2.bottom - rc2.top;
    int x2 = x1;
    int y2 = y1 + h1 + 10;

    g_hwnd_telemetry = CreateWindowExA(0,
                                       "SaturnDoomTelemetryClass",
                                       "SEGA SATURN DOOM - Telemetry & Analysis Dashboard [Window 2]",
                                       (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
                                       x2, y2, w2, h2,
                                       NULL, NULL, hInstance, NULL);

    if (g_hwnd_telemetry) {
        /* Add Interactive Control Buttons in Window 2 */
        s_btn_level_menu = CreateWindowExA(0, "BUTTON", "SELECT LEVEL...",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           20, 270, 160, 36,
                                           g_hwnd_telemetry, (HMENU)IDC_BTN_LEVEL_MENU, hInstance, NULL);

        s_combo_level = CreateWindowExA(0, "COMBOBOX", "",
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        190, 274, 250, 400,
                                        g_hwnd_telemetry, (HMENU)IDC_COMBO_LEVEL, hInstance, NULL);

        if (s_combo_level) {
            for (size_t i = 0; i < NUM_DOOM_MAPS; i++) {
                int idx = (int)SendMessageA(s_combo_level, CB_ADDSTRING, 0, (LPARAM)s_all_maps[i].name);
                SendMessageA(s_combo_level, CB_SETITEMDATA, idx, (LPARAM)(s_all_maps[i].episode * 10 + s_all_maps[i].map));
            }
            SendMessageA(s_combo_level, CB_SETCURSEL, 0, 0);
        }

        s_btn_reset = CreateWindowExA(0, "BUTTON", "RESET SPIKE",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      675, 270, 125, 36,
                                      g_hwnd_telemetry, (HMENU)IDC_BTN_RESET, hInstance, NULL);

        s_btn_csv = CreateWindowExA(0, "BUTTON", "START CSV RECORDING (F7)",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    810, 270, 200, 36,
                                    g_hwnd_telemetry, (HMENU)IDC_BTN_CSV, hInstance, NULL);

        s_btn_warp = CreateWindowExA(0, "BUTTON", "WARP TO SPIKE (F8)",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     1020, 270, 175, 36,
                                     g_hwnd_telemetry, (HMENU)IDC_BTN_WARP, hInstance, NULL);

        s_btn_journey = CreateWindowExA(0, "BUTTON", "FLIGHT TOUR (F9)",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        1205, 270, 205, 36,
                                        g_hwnd_telemetry, (HMENU)IDC_BTN_JOURNEY, hInstance, NULL);

        HFONT hFontBtn = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        if (s_btn_level_menu) SendMessage(s_btn_level_menu, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (s_combo_level) SendMessage(s_combo_level, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (s_btn_reset) SendMessage(s_btn_reset, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (s_btn_csv) SendMessage(s_btn_csv, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (s_btn_warp) SendMessage(s_btn_warp, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (s_btn_journey) SendMessage(s_btn_journey, WM_SETFONT, (WPARAM)hFontBtn, TRUE);

        ShowWindow(g_hwnd_telemetry, nCmdShow);
        UpdateWindow(g_hwnd_telemetry);
    }

    printf("Dual Windows created successfully! Launching D_DoomMain...\n");
    fflush(stdout);

    /* Launch DOOM-Saturn engine! */
    D_DoomMain();

    printf("D_DoomMain returned. Exiting.\n");
    fflush(stdout);

    return 0;
}

int g_win32_test_mode = 0;

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
    printf("FATAL CRASH! ExceptionCode: 0x%08X at Address: %p (ExeBase=%p)\n",
           ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, GetModuleHandleA(NULL));
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    char exe_dir[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir))) {
        char *p = strrchr(exe_dir, '\\');
        if (!p) p = strrchr(exe_dir, '/');
        if (p) {
            *p = '\0';
            SetCurrentDirectoryA(exe_dir);
        }
    }
    SetUnhandledExceptionFilter(CrashHandler);
    for (int i = 1; i < argc; i++) {
        if (strstr(argv[i], "-journey")) {
            g_win32_test_mode = 20;
            extern int saturn_map;
            saturn_map = 1;
        }
        else if (strstr(argv[i], "-test_e1m8")) {
            g_win32_test_mode = 10;
            extern int saturn_map;
            saturn_map = 8;
        }
        else if (strstr(argv[i], "-test_lift")) {
            g_win32_test_mode = 11;
            extern int saturn_map;
            saturn_map = 2;
        }
        else if (strstr(argv[i], "-test_platform")) {
            g_win32_test_mode = 12;
            extern int saturn_map;
            saturn_map = 1;
        }
        else if (strstr(argv[i], "-test_improom")) {
            g_win32_test_mode = 13;
            extern int saturn_map;
            saturn_map = 1;
        }
        else if (strstr(argv[i], "-test_user")) {
            g_win32_test_mode = 9;
            extern int saturn_map;
            saturn_map = 1;
        }
        else if (strstr(argv[i], "-test_hallway"))
            g_win32_test_mode = 8;
        else if (strstr(argv[i], "-test_secret"))
            g_win32_test_mode = 7;
        else if (strstr(argv[i], "-test_nukage"))
            g_win32_test_mode = 6;
        else if (strstr(argv[i], "-test_room41"))
            g_win32_test_mode = 5;
        else if (strstr(argv[i], "-test_stairs"))
            g_win32_test_mode = 4;
        else if (strstr(argv[i], "-test_alcove"))
            g_win32_test_mode = 3;
        else if (strstr(argv[i], "-test_zigzag"))
            g_win32_test_mode = 2;
        else if (strstr(argv[i], "-test"))
            g_win32_test_mode = 1;
        if (strstr(argv[i], "-nostrips")) {
            extern int saturn_wall_subdivision;
            saturn_wall_subdivision = 0;
        }
        if ((strcmp(argv[i], "-map") == 0 || strcmp(argv[i], "-warp") == 0) && i + 1 < argc) {
            g_win32_test_mode = 1;
            extern int saturn_episode;
            extern int saturn_map;
            char *arg = argv[i + 1];
            if ((arg[0] == 'e' || arg[0] == 'E') && (arg[2] == 'm' || arg[2] == 'M')) {
                saturn_episode = arg[1] - '0';
                saturn_map = atoi(arg + 3);
            } else if (i + 2 < argc && isdigit((unsigned char)arg[0]) && isdigit((unsigned char)argv[i + 2][0])) {
                saturn_episode = atoi(arg);
                saturn_map = atoi(argv[i + 2]);
                i++;
            } else {
                int val = atoi(arg);
                if (val >= 100) {
                    saturn_episode = val / 100;
                    saturn_map = val % 100;
                } else {
                    saturn_map = val;
                }
            }
        }
        if (strcmp(argv[i], "-occl") == 0 && i + 1 < argc) {
            extern int saturn_hw_bsp_occl;
            saturn_hw_bsp_occl = atoi(argv[i + 1]);
        }
    }
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
