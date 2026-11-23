# DOOM for Sega Saturn - Jo Engine port
# Full boot path: cart probe -> IWAD load (hybrid cart/CD) -> D_DoomMain.

SHELL := cmd.exe

SATURN_DIVLUT_MODE = STEP5
ifeq ($(SATURN_DIVLUT_MODE),FULL)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=9
else ifeq ($(SATURN_DIVLUT_MODE),STEP1)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=1 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP2)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=2 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP3)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=3 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP4)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=4 -DSATURN_DIVLUT_NOOCRAM
else ifeq ($(SATURN_DIVLUT_MODE),STEP5)
DIVLUT_DEFS := -DSATURN_DIVLUT -DSATURN_DIVLUT_STEP=5 -DSATURN_DIVLUT_NOOCRAM
else
DIVLUT_DEFS :=
endif

# Match the demo's proven boot configuration exactly.
JO_COMPILE_WITH_VIDEO_MODULE = 0   # 2026-11-23: unused by DOOM; 5.7 KB of the 0x060C0000 static ceiling
JO_COMPILE_WITH_SOFTWARE_RENDERER_MODULE = 1
JO_COMPILE_WITH_BACKUP_MODULE = 0
JO_COMPILE_WITH_TGA_MODULE = 1
JO_COMPILE_WITH_AUDIO_MODULE = 0
JO_COMPILE_WITH_3D_MODULE = 0
JO_COMPILE_WITH_PSEUDO_MODE7_MODULE = 0
JO_COMPILE_WITH_EFFECTS_MODULE = 0
JO_COMPILE_WITH_STORYBOARD_MODULE = 0   # 2026-11-23: unused (1.8 KB)
# JO_COMPILE_WITH_FAST_BUT_LESS_ACCURATE_MATH removed in Jo Engine v2026.1
JO_DEBUG = 1

JO_NTSC = 1
# JO_COMPILE_USING_SGL removed in Jo Engine v2026.1
JO_MAX_FS_BACKGROUND_JOBS = 4
JO_COMPILE_WITH_RAM_CARD_MODULE = 0

JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC = 102400
JO_MAX_SPRITE = 64

SRCS = main.c \
       doom/src/d_main.c \
       doom/src/doomstat.c \
       doom/src/dstrings.c \
       doom/src/g_game.c \
       doom/src/i_sound.c \
       doom/src/i_system.c \
       doom/src/i_video.c \
       doom/src/i_wad.c \
       doom/src/m_argv.c \
       doom/src/m_cheat.c \
       doom/src/m_misc.c \
       doom/src/saturn_render.c \
       doom/src/saturn_segs.c \
       doom/src/saturn_leaf.c \
       doom/src/saturn_vdp1.c \
       doom/src/saturn_cmdbuf.c \
       doom/src/saturn_hw_render.c \
       doom/src/saturn_hw_slave_walls.c \
       doom/src/saturn_vdp2_plane.c \
       doom/src/saturn_baked_flats.c \
       doom/src/saturn_baked_segs.c \
       doom/src/saturn_pak.c \
       doom/src/saturn_dsp.c \
       doom/src/saturn_slave.c \
       doom/src/saturn_tex.c \
       doom/src/saturn_vgl.c \
       doom/src/saturn_hud.c \
       doom/src/saturn_menu.c \
       doom/src/saturn_inter.c \
       doom/src/saturn_demo.c \
       doom/src/saturn_bench.c \
       doom/src/st_stub.c \
       doom/src/s_sound.c \
       doom/src/d32xr_stubs.c \
       doom/d32xr/info.c \
       doom/d32xr/p_base.c \
       doom/d32xr/p_ceilng.c \
       doom/d32xr/p_change.c \
       doom/d32xr/p_doors.c \
       doom/d32xr/p_enemy.c \
       doom/d32xr/p_floor.c \
       doom/d32xr/p_inter.c \
       doom/d32xr/p_lights.c \
       doom/d32xr/p_map.c \
       doom/d32xr/p_maputl.c \
       doom/d32xr/p_mobj.c \
       doom/d32xr/p_move.c \
       doom/d32xr/p_plats.c \
       doom/d32xr/p_pspr.c \
       doom/d32xr/p_setup.c \
       doom/d32xr/p_shoot.c \
       doom/d32xr/p_sight.c \
       doom/d32xr/p_slide.c \
       doom/d32xr/p_spec.c \
       doom/d32xr/p_switch.c \
       doom/d32xr/p_telept.c \
       doom/d32xr/p_tick.c \
       doom/d32xr/p_user.c \
       doom/d32xr/r_data.c \
       doom/d32xr/r_main.c \
       doom/d32xr/sounds.c \
       doom/d32xr/tables.c \
       doom/d32xr/w_wad.c \
       doom/d32xr/z_zone.c

JO_ENGINE_SRC_DIR=../../jo_engine
COMPILER_DIR=../../Compiler
CCFLAGS += -I./doom/src -I./doom/d32xr
# -fpermissive removed for C compilation
CCFLAGS += -DMARS=1 -DSATURN=1
# Profiling build: `set SATURN_PROF=1` before compile.bat. Turns on the
# per-subsector sub-timers, so the HUD prints B<bsp>/<aperture>/<occlusion>
# and G<segs>/<wall quads>/<portals> (ms).
# VDP1 manual frame-change mode (saturn_vdp1.c): VDP1 draws each list once
# and slow frames are held instead of cut off. SATURN_V1_MANUAL=0 restores
# the old 1-cycle mode.
SATURN_V1_MANUAL ?= 1
CCFLAGS += -DSATURN_V1_MANUAL=$(SATURN_V1_MANUAL)
SATURN_PROF ?= 0
ifeq ($(SATURN_PROF),1)
CCFLAGS += -DHW_PROF_SUBTIMERS=1
endif
# Extra -D flags from the environment (profiling variants), e.g. "-DHW_SUBMAX=0".
EXTRA_DEFS ?=
CCFLAGS += $(EXTRA_DEFS)
# JO_FS_MAX_FILES is 256 in Jo Engine v2026.1
CCFLAGS += -fno-strict-aliasing

include $(COMPILER_DIR)/COMMON/jo_engine_makefile
CCFLAGS := $(filter-out -flto,$(CCFLAGS))
LDFLAGS += -fno-use-linker-plugin
# SATURN_HOT_ORDER=1 (2026-11-15): one section per function and a project
# linker script that places the profiled hot render functions first and
# contiguous (doom_hot.linker), to cut SH-2 instruction-cache waste.
# Measured in 5thPlanet: no change (22.75 -> 22.79 ms render, same fetch
# stalls) - the misses are capacity (81 KB of hot code vs a 4 KB cache), not
# layout. Off by default; kept for re-testing once the hot code is smaller.
SATURN_HOT_ORDER ?= 0
ifeq ($(SATURN_HOT_ORDER),1)
CCFLAGS += -ffunction-sections
LDFILE := ./doom_hot.linker
endif

OBJS += doom/src/saturn_draw.o doom/src/saturn_hw_asm.o
game.elf: doom/src/saturn_draw.o doom/src/saturn_hw_asm.o

CCFLAGS_OPT = $(filter-out -Os,$(CCFLAGS)) -O2

doom/src/saturn_segs.o: doom/src/saturn_segs.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_SEG_RENDERER -DSATURN_LEAF_RENDERER -DSATURN_VDP1_WALL=1 -DSATURN_V1W_FULL=1 -DSATURN_V1W_ALL -DSATURN_PVS -o $@

doom/src/saturn_leaf.o: doom/src/saturn_leaf.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_LEAF_RENDERER -o $@

doom/src/saturn_vdp1.o: doom/src/saturn_vdp1.c
	$(CC) $< $(CCFLAGS) -DSATURN_VDP1_WALL=1 -o $@

doom/src/saturn_cmdbuf.o: doom/src/saturn_cmdbuf.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

SATURN_HW_DEPS = doom/src/saturn_hw_internal.h doom/src/saturn_hw_occl.c doom/src/saturn_hw_math.h doom/src/saturn_hw_sky.c doom/src/saturn_hw_sprites.c doom/src/saturn_hw_flats.c doom/src/saturn_hw_walls.c
# SATURN_HW_OS=1: experiment — compile the renderer for size (-Os, no per-function
# O2) to shrink its instruction-cache footprint.
SATURN_HW_OS ?= 0
ifeq ($(SATURN_HW_OS),1)
HW_RENDER_FLAGS = $(filter-out -O2,$(CCFLAGS_OPT)) -Os -DHW_SIZE_OPT
else
HW_RENDER_FLAGS = $(CCFLAGS_OPT)
endif
doom/src/saturn_hw_render.o: doom/src/saturn_hw_render.c $(SATURN_HW_DEPS)
	$(CC) $< $(DFLAGS) $(HW_RENDER_FLAGS) -DSATURN_VDP1_WALL=1 -DSATURN_LEAF_RENDERER -DSATURN_PVS -o $@

# Slave copy of the wall emitters (HW_SLAVE_WALLS; empty object otherwise).
# Every global is weakened so the duplicated header globals bind to the
# master object's definitions (saturn_hw_slave_walls.c).
doom/src/saturn_hw_slave_walls.o: doom/src/saturn_hw_slave_walls.c $(SATURN_HW_DEPS)
	$(CC) $< $(DFLAGS) $(HW_RENDER_FLAGS) -DSATURN_VDP1_WALL=1 -DSATURN_LEAF_RENDERER -DSATURN_PVS -o $@
	sh-elf-objcopy --weaken $@

doom/src/saturn_baked_flats.o: doom/src/saturn_baked_flats.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

# Cold code (load time / 2D screens only) at -Os (2026-11-21): static BSS +
# text must stay under 0x060C0000 (SGL work area); size beats speed here.
CCFLAGS_COLD = $(filter-out -O2,$(CCFLAGS)) -Os
doom/src/saturn_pak.o: doom/src/saturn_pak.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

# bench tour (SATURN_BENCH builds only; empty otherwise): keeps bench builds near the ceiling
doom/src/saturn_bench.o: doom/src/saturn_bench.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/src/saturn_menu.o: doom/src/saturn_menu.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/src/saturn_inter.o: doom/src/saturn_inter.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/d32xr/p_setup.o: doom/d32xr/p_setup.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

# Rarely-run game code (attract demo, teleports, switch/door activation;
# the door/button thinkers are a few per tic at most) - cold -Os.
doom/src/saturn_demo.o: doom/src/saturn_demo.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/d32xr/p_telept.o: doom/d32xr/p_telept.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/d32xr/p_switch.o: doom/d32xr/p_switch.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/d32xr/p_doors.o: doom/d32xr/p_doors.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/src/i_video.o: doom/src/i_video.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_PERF_ROWS $(DIVLUT_DEFS) -o $@

doom/src/saturn_render.o: doom/src/saturn_render.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_SEG_RENDERER $(DIVLUT_DEFS) -o $@

doom/src/saturn_slave.o: doom/src/saturn_slave.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -o $@

doom/src/d_main.o: doom/src/d_main.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) -DSATURN_UI_MENU=1 -DSATURN_VDP1_WALL=1 $(DIVLUT_DEFS) -o $@

doom/src/saturn_dsp.o: doom/src/saturn_dsp.c
	$(CC) $< $(DFLAGS) $(CCFLAGS) -DSATURN_DSP_OFF -o $@

# i_wad.c: WAD/cart/CD loading - load-time and CD-bound, so cold -Os too
# (2026-11-22: bug-fix round needed ~1 KB back under 0x060C0000).
doom/src/i_wad.o: doom/src/i_wad.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_COLD) -o $@

doom/src/d32xr_stubs.o: doom/src/d32xr_stubs.c
	$(CC) $< $(DFLAGS) $(CCFLAGS_OPT) $(DIVLUT_DEFS) -ffixed-r2 -ffixed-r3 -ffixed-r6 -ffixed-r7 -o $@
doom/src/saturn_draw.o: doom/src/saturn_draw.s
	$(CC) $< $(DFLAGS) -c -o $@
doom/src/saturn_hw_asm.o: doom/src/saturn_hw_asm.s
	$(CC) $< $(DFLAGS) -c -o $@

# Replace the jo_engine_makefile's broken `$(TARGET2):` rule.
TARGET2 := game.iso
$(TARGET2): $(TARGET1) $(IPFILE)
	$(CONV) -O binary $(TARGET) cd/0.bin
	cd $(CURDIR) && .\make-iso.bat

override MKISOFS = cmd.exe
override MKISOFS_FLAGS = /c rem jo_engine_makefile mkisofs step bypassed by make-iso.bat

TARGET3 := game.cue
override CUE_MAKER = cmd.exe /c rem
override CUE_MAKER_FLAGS =
$(TARGET3): $(TARGET1) $(TARGET2)
	@cmd.exe /c rem
