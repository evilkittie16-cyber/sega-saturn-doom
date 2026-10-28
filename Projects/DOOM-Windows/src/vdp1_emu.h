/* vdp1_emu.h - High-Fidelity Sega Saturn VDP1 Software Rasterizer in C */

#ifndef __VDP1_EMU_H__
#define __VDP1_EMU_H__

#include <stdint.h>
#include <stdbool.h>

/* Renders the delivered VDP1 command list into a 320x224 32-bit BGRA framebuffer.
 * Composites on top of I_VideoBuffer (VDP2 background sky & status bar). */
void VDP1_RenderFrame(uint32_t *dst_bgra, int fb_w, int fb_h, const uint8_t *video_buffer);

/* Set palette from DOOM's 768-byte RGB PLAYPAL */
void VDP1_SetPalette(const uint8_t *palette_rgb24);

/* Authentic Sega Saturn VDP1 & B-Bus hardware performance metrics */
typedef struct {
    int quads_drawn;
    int quad_pixels;
    int poly_drawn;
    int poly_pixels;
    int vdp1_cycles;          /* VDP1 rasterizer hardware cycles */
    float vdp1_load_pct;      /* VDP1 rasterizer load % */

    /* B-Bus hardware transfer metrics (14.3 MB/s sustained burst ceiling) */
    int bus_cmd_bytes;        /* Command table bytes delivered over B-Bus */
    int bus_tex_bytes;        /* Texture bytes uploaded to VDP1 VRAM */
    int bus_total_bytes;      /* Total bytes pushed across B-Bus */
    int bus_cycles;           /* B-Bus clock cycles consumed (2.0 cycles/byte) */
    float bus_load_pct;       /* B-Bus load % relative to 60 Hz budget (238 KB cap) */
    bool bus_vblank_overflow; /* true if transfer > 35 KB (exceeds V-Blank window) */

    /* Strip-granular texture streaming metrics (Section 36) */
    int bus_tex_strip_bytes;   /* Texture payload if streamed as visible 16px strips */
    int bus_strip_total_bytes; /* bus_cmd_bytes + bus_tex_strip_bytes */
    float strip_savings_pct;   /* % bandwidth saved by strip streaming vs full textures */
    int strip_bus_cycles;      /* B-Bus clock cycles under strip streaming */
    float strip_total_load_pct;/* Total frame load % under strip streaming */
    int scene_tex_full_bytes;  /* Total size of full textures active in scene */
    int scene_tex_strip_bytes; /* Total size of active strips in scene */

    /* Total combined hardware frame cost (Bus Transfer + VDP1 Rasterizer) */
    int total_frame_cycles;   /* bus_cycles + vdp1_cycles */
    float total_load_pct;     /* total_frame_cycles / 477,272 */
    int vblanks_required;     /* 1 = 60 FPS, 2 = 30 FPS, 3 = 20 FPS */
    int target_fps;           /* 60, 30, 20 */
} vdp1_perf_t;

void VDP1_GetPerfStats(vdp1_perf_t *out_perf);
void VDP1_SetHardwareLimitEnabled(bool enabled);
bool VDP1_GetHardwareLimitEnabled(void);
void VDP1_DumpCommands(const char *filename);
void VDP1_TestStripMajorTransposition(void);

#endif /* __VDP1_EMU_H__ */
