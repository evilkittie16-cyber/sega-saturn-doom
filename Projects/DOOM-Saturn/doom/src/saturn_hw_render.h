/* saturn_hw_render.h — High-performance Sega Saturn Hardware Renderer
 * Uses VDP1 distorted quads with shared-vertex grid projection (rectTransform)
 * and VDP2 background planes. Replaces CPU software raycasting.
 */
#ifndef SATURN_HW_RENDER_H
#define SATURN_HW_RENDER_H

#include "doomdef.h"

/* Viewport modes:
 * SATURN_VIEW_DOS:  320x168, CY=84, STBAR=168 (1:1 DOS pixel parity, locked 30 FPS)
 * SATURN_VIEW_FULL: 320x192, CY=96, STBAR=192 (Fullscreen borderless)
 */
enum {
    SATURN_VIEW_DOS  = 0,
    SATURN_VIEW_FULL = 1
};

extern int saturn_view_mode;
extern int saturn_view_w;
extern int saturn_view_h;
extern int saturn_view_cy;
extern int saturn_stbar_y;
extern int saturn_view_latch;
extern int saturn_stbar_dirty_latch;

/* Wall Strip Subdivision (Perspective-Correct VDP1 1D Strips):
 * 1 = Enabled (subdivides into 16px perspective-correct vertical strips)
 * 0 = Disabled (classic monolithic quads with VDP1 affine trapezoid warping)
 */
extern int saturn_wall_subdivision;
void Saturn_ToggleWallSubdivision(void);
extern unsigned int saturn_hw_wall_strips;
extern unsigned int saturn_hw_flat_lod0_subs;
extern unsigned int saturn_hw_flat_lod1_subs;
extern unsigned int saturn_hw_flat_lod_saved_quads;

void Saturn_SetViewMode(int mode);
void Saturn_ToggleViewMode(void);

/* One-time initialization of hardware renderer state */
void Saturn_HW_RenderInit(void);

/* Hardware player view renderer called from Saturn_G_Display() */
void Saturn_HW_RenderPlayerView(int displayplayer);

/* Force sky background repaint next frame (e.g. after camera teleport) */
void Saturn_HW_ForceSkyRepaint(void);

#endif /* SATURN_HW_RENDER_H */
