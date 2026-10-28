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

#endif /* SATURN_VDP2_PLANE_H */
