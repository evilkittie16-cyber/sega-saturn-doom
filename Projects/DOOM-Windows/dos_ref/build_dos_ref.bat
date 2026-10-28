@echo off
setlocal enabledelayedexpansion

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 (
    echo Error: Failed to init MSVC x86.
    exit /b 1
)

set DOOM_DIR=C:\Users\charl\Desktop\DOOM-master\DOOM-master\linuxdoom-1.10
set CFLAGS=/nologo /O2 /MD /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32=1 /D_WIN32=1 /D__WIN32__=1 /DNORMALUNIX /I. /I%DOOM_DIR% /FI dos_ref_compat.h /wd4244 /wd4267 /wd4018 /wd4146 /wd4005

set SRCS=^
 dos_ref_api.c ^
 %DOOM_DIR%\d_items.c ^
 %DOOM_DIR%\doomstat.c ^
 %DOOM_DIR%\info.c ^
 %DOOM_DIR%\m_bbox.c ^
 %DOOM_DIR%\m_fixed.c ^
 %DOOM_DIR%\m_random.c ^
 %DOOM_DIR%\m_swap.c ^
 %DOOM_DIR%\p_ceilng.c ^
 %DOOM_DIR%\p_doors.c ^
 %DOOM_DIR%\p_enemy.c ^
 %DOOM_DIR%\p_floor.c ^
 %DOOM_DIR%\p_inter.c ^
 %DOOM_DIR%\p_lights.c ^
 %DOOM_DIR%\p_map.c ^
 %DOOM_DIR%\p_maputl.c ^
 %DOOM_DIR%\p_mobj.c ^
 %DOOM_DIR%\p_plats.c ^
 %DOOM_DIR%\p_pspr.c ^
 %DOOM_DIR%\p_setup.c ^
 %DOOM_DIR%\p_sight.c ^
 %DOOM_DIR%\p_spec.c ^
 %DOOM_DIR%\p_switch.c ^
 %DOOM_DIR%\p_telept.c ^
 %DOOM_DIR%\p_tick.c ^
 %DOOM_DIR%\p_user.c ^
 %DOOM_DIR%\r_bsp.c ^
 %DOOM_DIR%\r_data.c ^
 %DOOM_DIR%\r_draw.c ^
 %DOOM_DIR%\r_main.c ^
 %DOOM_DIR%\r_plane.c ^
 %DOOM_DIR%\r_segs.c ^
 %DOOM_DIR%\r_sky.c ^
 %DOOM_DIR%\r_things.c ^
 %DOOM_DIR%\sounds.c ^
 %DOOM_DIR%\tables.c ^
 %DOOM_DIR%\v_video.c ^
 %DOOM_DIR%\w_wad.c ^
 %DOOM_DIR%\z_zone.c

cl.exe %CFLAGS% /LD %SRCS% /Fe:dos_doom_ref.dll /link /MACHINE:X86
if not errorlevel 1 (
    copy /Y dos_doom_ref.dll ..\dos_doom_ref.dll
)
