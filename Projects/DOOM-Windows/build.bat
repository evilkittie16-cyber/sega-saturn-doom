@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo =========================================================
echo  Building SEGA SATURN DOOM - Standalone Windows Port
echo  (Hardware-Accurate VDP1 Quad Rasterizer via 5thPlanet)
echo =========================================================

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 (
    echo Error: Failed to initialize MSVC x86 environment.
    exit /b 1
)
cd /d "%~dp0"

set CFLAGS=/nologo /Zi /O2 /MD /W3 /D_CRT_SECURE_NO_WARNINGS /DMARS=1 /DSATURN=1 /DSATURN_WIN32=1 /DSATURN_UI_MENU=1 /DSATURN_VDP1_WALL=1 /DSATURN_V1W_FULL=1 /DSATURN_V1W_ALL /DSATURN_PVS=1 /DSATURN_LEAF_RENDERER=1 /I.\src /I.\src\jo /I..\DOOM-Saturn\doom\src /I..\DOOM-Saturn\doom\d32xr

set SRCS=^
 src\win_main.c ^
 src\saturn_win32.c ^
 src\vdp1_emu.c ^
 src\i_win32.c ^
 ..\DOOM-Saturn\doom\src\d_main.c ^
 ..\DOOM-Saturn\doom\src\doomstat.c ^
 ..\DOOM-Saturn\doom\src\dstrings.c ^
 ..\DOOM-Saturn\doom\src\g_game.c ^
 ..\DOOM-Saturn\doom\src\m_argv.c ^
 ..\DOOM-Saturn\doom\src\m_cheat.c ^
 ..\DOOM-Saturn\doom\src\m_misc.c ^
 ..\DOOM-Saturn\doom\src\saturn_leaf.c ^
 ..\DOOM-Saturn\doom\src\saturn_vdp1.c ^
 ..\DOOM-Saturn\doom\src\saturn_cmdbuf.c ^
 ..\DOOM-Saturn\doom\src\saturn_hw_render.c ^
 ..\DOOM-Saturn\doom\src\saturn_vdp2_plane.c ^
 ..\DOOM-Saturn\doom\src\saturn_baked_flats.c ^
 ..\DOOM-Saturn\doom\src\saturn_baked_segs.c ^
 ..\DOOM-Saturn\doom\src\saturn_tex.c ^
 ..\DOOM-Saturn\doom\src\saturn_vgl.c ^
 ..\DOOM-Saturn\doom\src\saturn_hud.c ^
 ..\DOOM-Saturn\doom\src\saturn_menu.c ^
 ..\DOOM-Saturn\doom\src\saturn_inter.c ^
 ..\DOOM-Saturn\doom\src\st_stub.c ^
 ..\DOOM-Saturn\doom\src\s_sound.c ^
 ..\DOOM-Saturn\doom\src\d32xr_stubs.c ^
 ..\DOOM-Saturn\doom\d32xr\info.c ^
 ..\DOOM-Saturn\doom\d32xr\p_base.c ^
 ..\DOOM-Saturn\doom\d32xr\p_ceilng.c ^
 ..\DOOM-Saturn\doom\d32xr\p_change.c ^
 ..\DOOM-Saturn\doom\d32xr\p_doors.c ^
 ..\DOOM-Saturn\doom\d32xr\p_enemy.c ^
 ..\DOOM-Saturn\doom\d32xr\p_floor.c ^
 ..\DOOM-Saturn\doom\d32xr\p_inter.c ^
 ..\DOOM-Saturn\doom\d32xr\p_lights.c ^
 ..\DOOM-Saturn\doom\d32xr\p_map.c ^
 ..\DOOM-Saturn\doom\d32xr\p_maputl.c ^
 ..\DOOM-Saturn\doom\d32xr\p_mobj.c ^
 ..\DOOM-Saturn\doom\d32xr\p_move.c ^
 ..\DOOM-Saturn\doom\d32xr\p_plats.c ^
 ..\DOOM-Saturn\doom\d32xr\p_pspr.c ^
 ..\DOOM-Saturn\doom\d32xr\p_setup.c ^
 ..\DOOM-Saturn\doom\d32xr\p_shoot.c ^
 ..\DOOM-Saturn\doom\d32xr\p_sight.c ^
 ..\DOOM-Saturn\doom\d32xr\p_slide.c ^
 ..\DOOM-Saturn\doom\d32xr\p_spec.c ^
 ..\DOOM-Saturn\doom\d32xr\p_switch.c ^
 ..\DOOM-Saturn\doom\d32xr\p_telept.c ^
 ..\DOOM-Saturn\doom\d32xr\p_tick.c ^
 ..\DOOM-Saturn\doom\d32xr\p_user.c ^
 ..\DOOM-Saturn\doom\d32xr\r_data.c ^
 ..\DOOM-Saturn\doom\d32xr\r_main.c ^
 ..\DOOM-Saturn\doom\d32xr\sounds.c ^
 ..\DOOM-Saturn\doom\d32xr\tables.c ^
 ..\DOOM-Saturn\doom\d32xr\w_wad.c ^
 ..\DOOM-Saturn\doom\d32xr\z_zone.c

set LIBS=user32.lib gdi32.lib winmm.lib

cl.exe %CFLAGS% %SRCS% /Fe:doom_saturn_win.exe /link %LIBS% /DEBUG /MAP:doom_saturn_win.map /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo Compilation failed!
    exit /b 1
)

echo.
echo =========================================================
echo  Build SUCCESS! doom_saturn_win.exe ready.
echo =========================================================
