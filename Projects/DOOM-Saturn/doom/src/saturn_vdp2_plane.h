#ifndef SATURN_VDP2_PLANE_H
#define SATURN_VDP2_PLANE_H

#include "doomdef.h"

extern int saturn_vdp2_plane_active;
extern int saturn_vdp2_plane_sec;
extern fixed_t saturn_vdp2_plane_floorz;
extern int saturn_vdp2_plane_checker;

void Saturn_VDP2_Plane_Init(void);
void Saturn_VDP2_Plane_LoadFlat(const char *flatname);
void Saturn_VDP2_Plane_Frame(fixed_t camx, fixed_t camy, fixed_t camz, angle_t camang, fixed_t floorz);
void Saturn_VDP2_Plane_VBlank(void);

/* Frame sync with the VDP1 manual frame change (saturn_vdp1.c): each built
 * frame writes its rotation/coefficient tables into its own slot; the slot
 * travels with the VDP1 list (last -> pending -> drawing -> shown) and RBG0
 * shows the slot of the VDP1 frame on screen. -1 = none. */
#define SATURN_V2P_SLOTS 4

/* RAMCTL is write-only on hardware: the game keeps the full value here and
 * writes it whole. Base = CRAM mode 1 (2048 colours, bits 13:12 = 01), VRAM A
 * and B split (bits 8/9), no bank designated for RBG0 (bits 7:0 = 0). */
#define SATURN_VDP2_RAMCTL_BASE 0x1300u
extern unsigned short saturn_vdp2_ramctl;
/* RBG0 shown this page (set by the i_video.c flip: plane active AND a 3D page) */
extern volatile unsigned char saturn_vdp2_rbg0_on;

/* Per-level ground plane (HW_VDP2_PLANE, default on): at level load the
 * largest group of sectors sharing floor height + flat + light (static,
 * non-animated, untagged) becomes the RBG0 plane when it carries enough of
 * the level's floor quads; VDP1 skips those floors. */
#ifndef HW_VDP2_PLANE
#ifdef SATURN_WIN32
/* The Windows harness has no VDP2 rotation plane: with the plane on, those
 * floors were skipped on VDP1 and showed garbage streaks (2026-11-21 sweep).
 * Off there, so harness screenshots show every floor. */
#define HW_VDP2_PLANE 0
#else
#define HW_VDP2_PLANE 1
#endif
#endif
#define SATURN_V2P_MAX_SECTORS 1024
extern unsigned char saturn_vdp2_plane_secmask[SATURN_V2P_MAX_SECTORS];
void Saturn_VDP2_Plane_SelectForLevel(void);
/* VDP1 skips this sector's floor (the RBG0 plane draws it) */
#define SATURN_V2P_SKIP_FLOOR(sec) \
    (saturn_vdp2_plane_active && saturn_vdp2_plane_secmask[(unsigned)((sec) - sectors) & (SATURN_V2P_MAX_SECTORS - 1)])
extern volatile int saturn_v2p_last, saturn_v2p_pending, saturn_v2p_drawing, saturn_v2p_shown;

#endif /* SATURN_VDP2_PLANE_H */
