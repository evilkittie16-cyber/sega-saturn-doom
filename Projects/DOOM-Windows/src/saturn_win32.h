/* saturn_win32.h - Windows Platform HAL & Compatibility Layer for Sega Saturn DOOM */

#ifndef __SATURN_WIN32_H__
#define __SATURN_WIN32_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifdef _MSC_VER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define boolean win32_boolean_t
#define byte win32_byte_t
#include <windows.h>
#undef boolean
#undef byte

#pragma warning(disable: 4244) /* conversion from int to short/byte */
#pragma warning(disable: 4267) /* conversion from size_t to int */
#pragma warning(disable: 4018) /* signed/unsigned mismatch */
#pragma warning(disable: 4101) /* unreferenced local variable */
#pragma warning(disable: 4996) /* CRT secure deprecation */
#pragma warning(disable: 4146) /* unary minus on unsigned */
#pragma warning(disable: 4700) /* uninitialized local variable */
#define __attribute__(x)
#define __attribute(x)
#define __builtin_expect(e, v) (e)
#define __inline__ __inline
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strupr doom_strupr
#ifdef LC_NONE
#undef LC_NONE
#endif
#ifndef i386
#define i386 1
#endif
#endif

/* --- Simulated Saturn Memory Buffers --- */
extern int g_win32_test_mode;
extern uint8_t saturn_win32_wram_l[0x100000];   /* 1 MB Low Work RAM */
extern uint8_t saturn_win32_wram_h[0x100000];   /* 1 MB High Work RAM */
extern uint8_t saturn_win32_vdp1_vram[0x80000]; /* 512 KB VDP1 VRAM */
extern uint8_t *saturn_win32_iwad;
extern uint8_t *saturn_win32_zone;
void SaveFramebufferBMP(const char *filename, const uint32_t *fb, int w, int h);

/* --- Jo Engine Types & Color Constants --- */
typedef unsigned short jo_color;
typedef void jo_font;
#define JO_COLOR_RGB(r, g, b)            ((jo_color)(((b) >> 3) << 10 | (((g) >> 3) << 5) | ((r) >> 3) | 0x8000))
#define JO_COLOR_SATURN_RGB(r, g, b)     ((jo_color)(((b) << 10) | ((g) << 5) | (r) | 0x8000))

#define JO_COLOR_White			(65535)
#define JO_COLOR_Black			(32768)
#define JO_COLOR_Red			(32799)
#define JO_COLOR_Green			(33760)
#define JO_COLOR_Yellow			(33791)
#define JO_COLOR_Blue			(64512)
#define JO_COLOR_Purple			(64543)
#define JO_COLOR_Cyan			(65504)
#define JO_COLOR_Gray		    (58136)
#define JO_COLOR_DarkRed		(32776)
#define JO_COLOR_DarkGreen		(33024)
#define JO_COLOR_DarkYellow		(33032)
#define JO_COLOR_DarkBlue		(40960)
#define JO_COLOR_DarkPurple		(40968)
#define JO_COLOR_DarkCyan		(41216)
#define JO_COLOR_DarkGray		(41224)
#define JO_COLOR_MediumRed		(32784)
#define JO_COLOR_MediumGreen	(33280)
#define JO_COLOR_MediumYellow	(33296)
#define JO_COLOR_MediumBlue		(49152)
#define JO_COLOR_MediumPurple	(49168)
#define JO_COLOR_MediumCyan		(49664)
#define JO_COLOR_MediumWhite	(49680)
#define JO_COLOR_SemiRed		(32792)
#define JO_COLOR_SemiGreen		(33536)
#define JO_COLOR_SemiYellow		(33560)
#define JO_COLOR_SemiBlue		(57344)
#define JO_COLOR_SemiPurple		(57368)
#define JO_COLOR_SemiCyan		(58112)

#define JO_NULL                          NULL
#define JO_FAST_ALLOCATION               0
#define JO_SPRITE_SCREEN                 0

#define jo_malloc(sz)                    malloc(sz)
#define jo_malloc_with_behaviour(sz, b)  malloc(sz)
#define jo_free(p)                       free(p)
#define jo_vdp2_malloc(sz)               malloc(sz)
#define jo_spin_wait(n)                  ((void)0)
#define jo_core_exec_on_slave(fn)        ((void)0)
#define jo_core_wait_for_slave()         ((void)0)
#define jo_set_displayed_screens(s)      ((void)0)

/* Gamepad Keys */
#define JO_KEY_RIGHT                     (1<<7)
#define JO_KEY_LEFT                      (1<<6)
#define JO_KEY_DOWN                      (1<<5)
#define JO_KEY_UP                        (1<<4)
#define JO_KEY_START                     (1<<11)
#define JO_KEY_A                         (1<<10)
#define JO_KEY_B                         (1<<8)
#define JO_KEY_C                         (1<<9)
#define JO_KEY_Z                         (1<<0)
#define JO_KEY_Y                         (1<<1)
#define JO_KEY_X                         (1<<2)
#define JO_KEY_R                         (1<<3)
#define JO_KEY_L                         (1<<15)

/* High resolution timer emulation */
unsigned short Win32_GetFRT(void);

/* Atomic flag stubs for single-threaded Win32 simulation */
static inline int atomic_flag_test_and_set(char *p) { char old = *p; *p = 1; return old; }
static inline void atomic_flag_clear(char *p) { *p = 0; }

/* VDP1 Emulation Rendering function */
void VDP1_RenderFrame(uint32_t *dst_bgra, int fb_w, int fb_h, const uint8_t *video_buffer);

/* Level Warp Request */
void Win32_RequestLevelWarp(int episode, int map);

#endif /* __SATURN_WIN32_H__ */
