/* i_video.c Ã¢â‚¬â€ Sega Saturn video I/O.
 *
 * DOOM's renderer writes 8-bit palette indices into a 320x200 framebuffer.
 * The Saturn's VDP1 cannot consume an 8bpp CPU buffer directly, so we use
 * Jo Engine's software renderer (jo_software_renderer_create) which manages
 * a 16bpp color buffer and DMAs it to VDP1 VRAM each frame.
 *
 * Frame pipeline:
 *
 *   1. DOOM's renderer writes 8bpp indices into `I_VideoBuffer` (a CPU
 *      shadow buffer).
 *   2. `I_SetPalette(palette)` (called from r_main's R_PaletteStuff) stores
 *      the active PLAYPAL as 768 bytes of 6-bit DOOM-format RGB triplets.
 *      We expand to 256-entry 5-bit Saturn palette (JO_COLOR_RGB) for the
 *      frame-conversion step.
 *   3. `I_FinishUpdate` walks the 320x200 framebuffer, looks each index up
 *      in the active Saturn palette, and writes the 16bpp result into the
 *      two software-renderer `color_buffer`s (160x200 each Ã¢â‚¬â€ VDP1 caps
 *      16bpp sprites at 256 px width, so the frame is split in halves).
 *      Then `jo_software_renderer_flush` DMAs both buffers to VDP1.
 *   4. `saturn_draw_sprite` (inlined into I_FinishUpdate) displays the
 *      two half-sprites side by side at x = -80 / +80, covering the
 *      full TV frame.
 *
 * This bypasses all of the 8bpp sprite machinery (jo_sprite_add_8bits_image,
 * CRAM palette binding, cmd->colr) that was failing silently on the non-SGL
 * Jo Engine path. The 16bpp sprite pipeline is what the Sonic / Storyboard /
 * Software Rendering samples use, and it works out of the box.
 *
 * d32xr's `I_SetPalette` was a no-op stub here; we rewire it to update the
 * active palette so palette animations (red pain flash, bonus, invuln) work.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "saturn.h"     /* SATURN_CART_REC_OFF (r92) */
#include "i_video.h"
#include "i_system.h"
#include "saturn_cache.h"
#include "saturn_slave.h"
#include "saturn_tex.h"
#include "r_local.h"       /* side_t/sector_t for saturn_segs.h */
#include "saturn_segs.h"   /* round 11b probe struct */
#include "saturn_vdp1.h"   /* VDP1 scaled-sprite wall prototype */
#include "saturn_memmap.h"

/* Under JO_COMPILE_USING_SGL=1, jo_engine's vdp1_command_pipeline.c body
 * is excluded (#if !JO_COMPILE_USING_SGL). The non-SGL helpers
 * jo_vdp1_buffer_reset/flush do not exist as linkable symbols. SGL
 * drives VDP1 frame changes via slSynch(), which waits for the next
 * VBlank and flushes the SGL command table. We use that as our
 * per-frame "advance VDP1" primitive. */
#if JO_COMPILE_USING_SGL
extern void slSynch(void);
extern void slSmpcCashSend(void);
/* Round-54: slSynch() is GONE from the commit. Under SATURN_VDP2_FB the
 * VDP1 sprite surfaces are never created (saturn_gfx_l == NULL, both
 * saturn_draw_half calls return), so SGL's command table is always
 * empty and slSynch was provably just an UNBOUNDED vblank wait plus
 * bookkeeping â€” and the terminal-wedge site (r53's freeze record: kind
 * 8 fires, master lurches, then dies with every leg healthy; slSynch
 * was the only unguarded call left, and photo-#148 already measured it
 * waiting 2.8 vbl deterministically). The flip's own TVSTAT
 * double-wait below already lands the NBG0 flip inside the VBlank;
 * D_DoomLoop's tic gate paces frames. If SGL's master-side state
 * machine misses slSynch, the symptom will be a picture/slave-job
 * regression, not a hang â€” revert then. */
# define SATURN_VDP1_FLUSH()          do { saturn_draw_sprite(); } while (0)
#else
extern void jo_vdp1_buffer_reset(void);
extern void jo_vdp1_flush(void);
# define SATURN_VDP1_FLUSH()          do { jo_vdp1_buffer_reset();           \
                                          saturn_draw_sprite();              \
                                          jo_vdp1_flush();                   \
                                          JO_VDP1_PTMR = 0x3;                \
                                     } while (0)
#endif

/* d32xr's renderer expects these symbols. */
byte   *I_VideoBuffer  = NULL;
byte   *I_VideoBuffer2 = NULL;

/* Full-width render (2026-09-05): the 8bpp shadow framebuffer is
 * DOOM_SCREENWIDTH (320) COLUMNS wide (saturn_render.c's SCREEN_W);
 * the NBG0 bitmap displays it 1:1 (zoom 1.0). Every I_VideoBuffer
 * stride in this file goes through FB_W. */
#define FB_W  DOOM_SCREENWIDTH

/* Saturn framebuffer (CPU side). DOOM writes 8-bit palette indices here.
 * ONE 32 KB buffer, not two: the heap is 250,000 bytes and
 * jo_software_renderer_create_ex needs another 128 KB for its
 * color_buffer Ã¢â‚¬â€ a second shadow page would fail the malloc and abort
 * in I_Error before the first frame. d32xr's 32X page-flip code never
 * runs on Saturn (our D_Display renders every frame into
 * I_VideoBuffer), so I_VideoBuffer2 is aliased to the same memory to
 * keep the exported symbol valid. */
static byte            *shadow_fb  = NULL;

/* Jo Engine software-renderer surfaces. VDP1 DrawNormalSprite caps
 * 16bpp sprites at 256 px width, and Jo's texture SIZE encoding
 * (`width & 0x1f8`, sprites.c:212) silently mangles anything wider Ã¢â‚¬â€
 * a 320-wide sprite gets a corrupted size field and the draw command
 * is dropped by the VDP1 (black frame). The 320x200 DOOM frame is
 * therefore displayed as two side-by-side 160x200 sprites. Each
 * surface owns a 160x200 16bpp color_buffer (64,000 bytes) that is
 * DMA'd to its VDP1 VRAM slot on flush. */
static jo_software_renderer_gfx  *saturn_gfx_l = NULL;
static jo_software_renderer_gfx  *saturn_gfx_r = NULL;

#define SATURN_HALF_WIDTH    (DOOM_SCREENWIDTH / 2)

/* Active 256-color palette in 16-bit Saturn RGB (5 bits per channel).
 * Populated by I_SetPalette from the active PLAYPAL palette. */
static unsigned short   saturn_palette16[256];
/* Same palette pre-doubled for the finish expand: each entry is the
 * 16-bit color twice (c | c<<16), so one 32-bit store writes the 2:1
 * horizontally-doubled output pixel pair. Both u16 halves are
 * identical, so SH-2 big-endian storage needs no swap handling. */
static unsigned int     saturn_palette32[256];

/* VBlank counter. Incremented by the registered VBlank callback once
 * per Saturn VBlank (~16.7 ms at 60 Hz NTSC). */
volatile unsigned int saturn_vbl_count = 0;

/* r82 A/B — master gate for ALL VBlank-ISR diagnostics (nets, census,
 * verdict, P/W reporter). 0 = the callback is just the vblank counter
 * + one-time disc ID. r84: RE-ARMED (=1). The A/B verdict is in: the
 * de-instrumented r83 disc still wedges (silent cyan inside
 * G_InitNew), so the ISR instrumentation is exonerated as the sole
 * cause — and with the r83 false-fire detectors fixed, the eyes are
 * needed again: the r83 cyan wedge is invisible precisely because
 * nothing repaints over the stage fills. */
volatile unsigned int saturn_instrument_armed = 0;

/* =====================================================================
 * VDP2 8bpp DIRECT DISPLAY (2026-09-02, performance).
 *
 * The 8bpp DOOM framebuffer is copied once per frame into an NBG0
 * 256-color BITMAP in VDP2 VRAM and displayed with a 2x horizontal
 * zoom. The old path Ã¢â‚¬â€ 8->16 palette expand into two 160x200 16bpp
 * color buffers + DMA to VDP1 VRAM + two fullscreen VDP1 sprites Ã¢â‚¬â€ is
 * gone entirely: VDP1 was only ever used to show the framebuffer, and
 * the expand+purge measured z = 10.3 ms of the 14.9 ms finish window.
 * The new finish = one 32 KB strided word-copy (no palette work, no OC
 * purge Ã¢â‚¬â€ the CPU reads its own cache; nothing DMA-reads the fb) +
 * slSynch.
 *
 * Bitmap geometry: NBG0 bitmap line stride is fixed at 512 bytes
 * (BM_512x256); the fb's 320 bytes land at the head of each line and
 * zoom 1.0 maps them 1:1 across the 320-wide screen. Only lines
 * 0..199 are displayed.
 *
 * Pages: 512 x 200 = 100,000 bytes each. Both live in VDP2 VRAM bank A,
 * whose previous occupants are all dead after the takeover:
 *   page A = VDP2_VRAM_A0 (0x25E00000) Ã¢â‚¬â€ Jo's NBG1 16bpp boot bitmap
 *   page B = VDP2_VRAM_A1 (0x25E20000) Ã¢â‚¬â€ dead RBG0 K-table region
 *            (plus jo_printf's NBG0 cells Ã¢â‚¬â€ bitmap mode never reads
 *            pattern-name data, and no in-game jo_printf exists)
 * The back-screen color word at 0x25E3FFFE sits past page B's line 199
 * and is preserved (rewritten black once). Bank B (NBG1 map B0, NBG0
 * printf map B1) is left alone, so no bank-B cycle patterns are needed.
 *
 * Boot coexistence: jo_init_nbg0_printf already runs NBG0 as a
 * 256-color layer UNDER the NBG1 stage-color bitmap, so switching NBG0
 * to bitmap mode changes nothing visible while NBG1 is up. NBG1 is
 * hidden at the first I_FinishUpdate (slScrAutoDisp(NBG0ON)); stage
 * colors requested after that (WAD-load markers, I_Error) are painted
 * by saturn_vdp2_stage_fill() into the NBG0 pages instead Ã¢â‚¬â€ see
 * Saturn_StageColor in i_system.c.
 * ===================================================================== */
#define SATURN_VDP2_FB 1

#if SATURN_VDP2_FB
#define SATURN_FB_PAGE_A   0x25E00000u
#define SATURN_FB_PAGE_B   0x25E20000u
#define SATURN_FB_LINES    224   /* = DOOM_FBHEIGHT: the full visible window */
#define SATURN_FB_STRIDE   512
#define SATURN_FB_CRAM     0x25F00000u
#define SATURN_FB_BACKCOL  0x25E3FFFEu   /* A1 + 0x1FFFE, past page B */

/* 0 = page A displayed, 1 = page B. Flipped after each commit. */
static unsigned char saturn_fb_front = 0;
/* Set once the NBG0 bitmap mode is programmed; gates the stage-fill
 * hook and the per-frame copy. */
static unsigned char saturn_fb_live = 0;
/* Set by saturn_vdp2_stage_fill whenever it retargets CRAM[1] to a
 * stage color; I_FinishUpdate clears it by re-pushing the whole game
 * palette. Without this, the LAST boot stage color (d_main's DarkCyan
 * "IN-LOOP" marker, fired after I_SetPalette) stayed in CRAM[1] for the
 * entire game, and every framebuffer pixel holding palette index 1 Ã¢â‚¬â€
 * PLAYPAL[1] (31,23,11), the dark-brown grain color that saturates
 * DOOM's wood/stone textures, plus everything the dark COLORMAP levels
 * crush down to index 1 Ã¢â‚¬â€ displayed as teal RGB(0,64,64) (0xA100). */
static unsigned char saturn_cram_stage_dirty = 0;

/* Fill both pages entirely Ã¢â‚¬â€ visible lines 0..199 (head 160 bytes) with
 * one palette index, and the never-copied lines 200..255 with index 0.
 * The full-page sweep matters because jo_vdp2_clear_bitmap_nbg1 (still
 * called by Saturn_StageColor for NBG1) scribbles 0x23000 bytes of raw
 * stage-color bytes across page A + page B's head, and anything it
 * leaves in lines 200..223 shows at the bottom of the 224-line window. */
/* True once the NBG0 direct-display takeover has run. Saturn_StageColor
 * (i_system.c) checks this to skip jo_vdp2_clear_bitmap_nbg1: after the
 * takeover NBG1 is hidden and page A *is* the NBG0 framebuffer, so that
 * call only scribbles raw 16bpp stage-color words across the live
 * display pages (Jo fills at a 640-byte line pitch over our 512-byte
 * page stride) - the surviving bytes display as the 2026-09-04 cyan
 * staircase (RGB 0x03FF, fb rows 9-10, cols 4-13) - and re-enables
 * NBG1 against our slScrAutoDisp(NBG0ON). Pre-takeover the clear is
 * still wanted: NBG1 is the visible boot-marker channel then. */
int saturn_vdp2_nbg0_live(void)
{
    return saturn_fb_live;
}

void saturn_vdp2_stage_fill(jo_color color)
{
    unsigned int            page;
    volatile unsigned short *cram = (volatile unsigned short *)SATURN_FB_CRAM;
    extern int              saturn_quiet_hud;

    if (!saturn_fb_live || saturn_quiet_hud)
        return;
    cram[1] = color;                     /* index 1 = this stage color */
    saturn_cram_stage_dirty = 1;         /* I_FinishUpdate restores */
    for (page = 0; page < 2; page++)
    {
        volatile unsigned int *d = (volatile unsigned int *)
            (page ? SATURN_FB_PAGE_B : SATURN_FB_PAGE_A);
        int y;
        for (y = 0; y < SATURN_FB_LINES; y++, d += SATURN_FB_STRIDE / 4)
        {
            int i;
            for (i = 0; i < (FB_W + 32) / 4; i++)
                d[i] = 0x01010101u;      /* 4 x palette index 1 */
        }
        for (; y < 256; y++, d += SATURN_FB_STRIDE / 4)
        {
            int i;
            for (i = 0; i < SATURN_FB_STRIDE / 4; i++)
                d[i] = 0;                /* transparent -> back color */
        }
    }
}

/* Round-33: on-red-screen text. The VDP1 boot font is INVISIBLE once
 * the NBG0 direct-display takeover is live (the sprite layer sits
 * under NBG0 in this configuration) â€” which is why the watchdog's
 * live verdict printed nothing on the solid-red screen (2026-09-04
 * Kronos repro: two captures, red, no glyphs). This stamps glyphs
 * STRAIGHT into both NBG0 pages as palette-index-0 bytes â€” no CRAM
 * writes, no VDP1, no SGL commit; VDP2 re-reads the pages every
 * frame, so the text is visible as soon as the CPU stores land.
 * 8x8 glyphs drawn 2x (16 px cells, 20 px pitch â€” the boot font's
 * 2.0f pitch; max 8 glyphs per 160-px row from x=8). */
static const unsigned char stage_glyphs[38][8] = {
    { 0x38, 0x40, 0x40, 0x38, 0x04, 0x04, 0x78, 0x00 },  /* S */
    { 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7C, 0x00 },  /* L */
    { 0x38, 0x44, 0x44, 0x7C, 0x44, 0x44, 0x44, 0x00 },  /* A */
    { 0x44, 0x44, 0x44, 0x44, 0x28, 0x28, 0x10, 0x00 },  /* V */
    { 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x7C, 0x00 },  /* E */
    { 0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00 },  /* T */
    { 0x44, 0x44, 0x44, 0x7C, 0x44, 0x44, 0x44, 0x00 },  /* H */
    { 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x00 },  /* F (r66:
                                                          * the fill-
                                                          * progress
                                                          * row label
                                                          * drew blank
                                                          * before) */
    { 0x78, 0x48, 0x48, 0x78, 0x50, 0x48, 0x48, 0x00 },  /* R (r66:
                                                          * read-call
                                                          * count) */
    { 0x38, 0x40, 0x40, 0x5C, 0x44, 0x44, 0x38, 0x00 },  /* G (r66:
                                                          * CD-call
                                                          * time) */
    { 0x3C, 0x40, 0x40, 0x40, 0x40, 0x40, 0x3C, 0x00 },  /* C (r66:
                                                          * cart-write
                                                          * time) */
    { 0x7C, 0x42, 0x42, 0x7C, 0x42, 0x42, 0x7C, 0x00 },  /* B (r68:
                                                          * fill-beat
                                                          * row â€” main-
                                                          * thread chunk
                                                          * counter) */
    { 0x00, 0x00, 0x78, 0x00, 0x78, 0x00, 0x00, 0x00 },  /* = */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* ' ' */
    { 0x38, 0x44, 0x4C, 0x54, 0x64, 0x44, 0x38, 0x00 },  /* 0 */
    { 0x10, 0x30, 0x10, 0x10, 0x10, 0x10, 0x38, 0x00 },  /* 1 */
    { 0x38, 0x44, 0x04, 0x08, 0x10, 0x20, 0x7C, 0x00 },  /* 2 */
    { 0x38, 0x44, 0x04, 0x18, 0x04, 0x44, 0x38, 0x00 },  /* 3 */
    { 0x08, 0x18, 0x28, 0x48, 0x7C, 0x08, 0x08, 0x00 },  /* 4 */
    { 0x7C, 0x40, 0x78, 0x04, 0x04, 0x44, 0x38, 0x00 },  /* 5 */
    { 0x18, 0x24, 0x40, 0x78, 0x44, 0x44, 0x38, 0x00 },  /* 6 */
    { 0x7C, 0x04, 0x08, 0x10, 0x10, 0x20, 0x20, 0x00 },  /* 7 */
    { 0x38, 0x44, 0x44, 0x38, 0x44, 0x44, 0x38, 0x00 },  /* 8 */
    { 0x38, 0x44, 0x44, 0x7C, 0x04, 0x48, 0x30, 0x00 },  /* 9 */
    { 0x00, 0x10, 0x10, 0x00, 0x10, 0x10, 0x00, 0x00 },  /* : */
    { 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00 },  /* - */
    { 0x3C, 0x42, 0x02, 0x1C, 0x20, 0x00, 0x20, 0x00 },  /* ? */
    { 0x78, 0x44, 0x44, 0x44, 0x44, 0x44, 0x78, 0x00 },  /* D (r69)  */
    { 0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7C, 0x00 },  /* I (r69)  */
    { 0x44, 0x6C, 0x54, 0x54, 0x44, 0x44, 0x44, 0x00 },  /* M (r69)  */
    { 0x44, 0x64, 0x54, 0x54, 0x4C, 0x44, 0x44, 0x00 },  /* N (r69)  */
    { 0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00 },  /* O (r69)  */
    { 0x78, 0x44, 0x44, 0x78, 0x40, 0x40, 0x40, 0x00 },  /* P (r69)  */
    { 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00 },  /* U (r69)  */
    { 0x44, 0x44, 0x44, 0x54, 0x54, 0x6C, 0x44, 0x00 },  /* W (r69)  */
    { 0x44, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10, 0x00 },  /* Y (r69)  */
    { 0x44, 0x48, 0x50, 0x60, 0x50, 0x48, 0x44, 0x00 },  /* K (r74:
                                                          * the site-
                                                          * mask row â€”
                                                          * r73 painted
                                                          * its label
                                                          * BLANK, K was
                                                          * never in the
                                                          * 37-glyph
                                                          * set) */
};
static const char stage_glyph_chars[38] = "SLAVETHFRGCB= 0123456789:-?DIMNOPUWYK";

/* VDP1 wall prototype (2026-09-06): when set, every diagnostic HUD
 * stamp is silenced so only saturn_vdp1.c's V1 row stays on screen.
 * Cleared by I_Error (fatal rows must always show) and never set
 * before the first I_FinishUpdate (boot stages keep their rows). */
int saturn_quiet_hud = 1;

void Saturn_StageTextIdx(int x, int y, const char *s, unsigned char px)
{
    int cx = x;

    if (saturn_quiet_hud)
        return;

    for (; *s; s++)
    {
        int g = -1, k, row;

        for (k = 0; k < 38; k++)
            if (stage_glyph_chars[k] == *s)
            {
                g = k;
                break;
            }
        if (g < 0)
        {
            cx += 20;               /* unmapped char -> blank cell */
            continue;
        }
        if (cx > FB_W - 16)
            break;                  /* past the visible strip */
        for (row = 0; row < 8; row++)
        {
            unsigned int bits = stage_glyphs[g][row];
            int               r;

            for (r = 0; r < 2; r++)          /* vertical 2x */
            {
                volatile unsigned char *pa =
                    (volatile unsigned char *)SATURN_FB_PAGE_A
                    + (unsigned int)(y + row * 2 + r) * SATURN_FB_STRIDE
                    + cx;
                volatile unsigned char *pb =
                    (volatile unsigned char *)SATURN_FB_PAGE_B
                    + (unsigned int)(y + row * 2 + r) * SATURN_FB_STRIDE
                    + cx;
                int col;

                for (col = 0; col < 8; col++)
                {
                    if (bits & (0x80u >> col))
                    {
                        pa[0] = px;
                        pa[1] = px;
                        pb[0] = px;
                        pb[1] = px;
                    }
                    pa += 2;                     /* horizontal 2x */
                    pb += 2;
                }
            }
        }
        cx += 20;               /* 16 px cell + 4 px gap */
    }
}

void Saturn_StageText(int x, int y, const char *s)
{
    Saturn_StageTextIdx(x, y, s, 0);
}

/* r97: HIGH-CONTRAST ladder text. The r96 real-HW test showed the m3
 * fill with ZERO letters — because glyph pixels are palette index 0,
 * which displays as the VDP2 BACK colour (jo_core_init DarkBlue =
 * RGB555 (0,0,8)) and the m3 fill is (0,0,24): an 8-blue-level
 * difference — genuinely painted, invisible to the eye and the camera.
 * (The red report never hit this: (0,0,8) on (31,0,0) is readable.)
 *
 * r101: index 2 was FORCED WHITE in CRAM here (cram[2]=0x7FFF, never
 * restored). That poisoned the GAME palette: PLAYPAL[2] is the
 * near-black brown (23,15,7), and the dark COLORMAP rows crush huge
 * bands of texel values down to index 2 (FLAT5_1 row 24 = 66%,
 * FLAT5_2 = 69%, FLOOR5_2 = 96%) — every one of those texels rendered
 * as a white 1-2px speck on the dark flats. THE r100 WHITE-SPECKLE
 * BUG. Deterministic on Kronos and real HW, independent of the cart
 * fill (palette, not data), so the bisect's fill-on/fill-off runs
 * showed the identical speckle. This is the same bug class as the
 * CRAM[1] stage-fill poisoning above — but the dirty-flag restore
 * does not apply: the beacon must stay white BEFORE the first frame
 * commits (I_FinishUpdate never ran) and vanish afterwards anyway.
 * FIX: PLAYPAL[4] IS pure white (255,255,255) — stamp glyphs with
 * palette index 4 and never touch CRAM. Glyphs display true white on
 * every stage fill, and the game palette stays pristine. */
void Saturn_StageTextHi(int x, int y, const char *s)
{
    Saturn_StageTextIdx(x, y, s, 4);    /* PLAYPAL[4] = pure white */
}

/* r58: tiny strlen for the I/M verdict rows (mystrlen links from the
 * d32xr lib, which this file's objects predate in link order). */
static unsigned int saturn_ivlen(const char *s)
{
    unsigned int n = 0;
    while (s[n])
        n++;
    return n;
}

/* r57: main-thread ISR-death verdict. The cap-wait detector calls this
 * when ~3 vbl of FRT pass with saturn_vbl_count unchanged â€” the vblank
 * ISR is dead and every ISR-side net with it. Paint YELLOW + rows from
 * the MAIN thread (both pages, like all staging); the same iteration's
 * commit + next flip exposes it, and I_FinishUpdate repaints it after
 * every subsequent commit. S = saturn_sim_site (16 = detector fired in
 * the display-cap wait), V = the FROZEN vbl count, A = frame count. */
void saturn_vbl_dead_verdict(void)
{
    extern volatile unsigned int saturn_sim_site;
    extern volatile unsigned int saturn_frame_count;
    static char                  row[12];

    saturn_vdp2_stage_fill(0x7FE0u);   /* YELLOW */
    row[0] = 'S'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_sim_site & 0xFFFFu);
    Saturn_StageText(8, 8, row);
    row[0] = 'V'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_vbl_count & 0xFFFFu);
    Saturn_StageText(8, 28, row);
    row[0] = 'A'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_frame_count & 0xFFFFu);
    Saturn_StageText(8, 48, row);
    /* r58/r72: name the interrupt machinery at verdict time. Addresses
     * corrected to the SCU manual 3.5: IMS = 0x25FE00A0 (1 = masked),
     * IST = 0x25FE00A4 (1 = pending; VBlank-IN = bit 0, DSP-End =
     * bit 5). I = 1 means the VBlank-IN status bit is SET but un-
     * serviced (the ISR was entered and died, or is masked); I = 0
     * with a frozen vbl count means the vblank IRQ is not even
     * reaching the SCU. M shows the live mask so a stuck bit is
     * visible. Also stamped here: P = the GFS call site in flight
     * (i_wad.c saturn_gfs_site) and D/W = the r71 DSP-engine state +
     * cart KB written, so one photo names the transport AND the
     * progress point of the wedge. */
    {
        extern volatile unsigned int saturn_gfs_site;
        extern volatile unsigned int saturn_cart_kb;
        extern int                   Saturn_DspCartOk(void);
        unsigned int ist   = *(volatile unsigned int *)0x25FE00A4u;
        unsigned int imask = *(volatile unsigned int *)0x25FE00A0u;
        static char  im[28];

        /* r83: these two rows lived at y=188/208 — under the TV bezel
         * on the real-HW set (the r82 photos show y=188 half-eaten and
         * y=208 gone), and variable-width, so the M= half of the I/M
         * row could not be trusted. Both move into the safe zone
         * (y<=168) and go zero-padded fixed-width. */
        im[0] = 'I'; im[1] = '=';
        Saturn_Udec0(im + 2, ist & 0xFFFFu, 5);
        im[7] = ' ';
        im[8] = 'M'; im[9] = '=';
        Saturn_Udec0(im + 10, imask & 0xFFFFu, 5);
        im[15] = 0;
        Saturn_StageText(8, 148, im);

        im[0] = 'P'; im[1] = '=';
        Saturn_Udec0(im + 2, saturn_gfs_site & 0xFFFFu, 2);
        im[4] = ' ';
        im[5] = 'D'; im[6] = '=';
        im[7] = Saturn_DspCartOk() ? '1' : '0';
        im[8] = ' ';
        im[9] = 'W'; im[10] = '=';
        Saturn_Udec0(im + 11, saturn_cart_kb & 0xFFFFu, 4);
        im[15] = 0;
        Saturn_StageText(8, 168, im);
    }
}

/* r58 ISR->main verdict latch. The ISR no longer paints the 2x64KB
 * stage fills (the r56 state save caught the master SH-2 wedged inside
 * the SCU interrupt-entry glue â€” interrupt context that never completes
 * freezes the vbl count and produces the silent terminal freeze). The
 * ISR latches here; D_DoomLoop paints the full verdict from this latch
 * via saturn_isr_paint(). */
volatile unsigned int saturn_isr_site  = 0;  /* net-1/2: master site */
volatile unsigned int saturn_isr_vbl   = 0;
volatile unsigned int saturn_isr_gap   = 0;  /* net-4: between-frames gap */
volatile unsigned int saturn_isr_sim   = 0;  /* net-4: sim phase stamp */
volatile unsigned int saturn_isr_ist   = 0;  /* SCU IST latched at trip */
volatile unsigned int saturn_isr_imask = 0;
volatile unsigned int saturn_isr_dirty = 0;  /* bit0 net2, bit1 net1,
                                              * bit2 net4 */
/* r74: set ONCE by the ISR's freeze-verdict watch â€” every diagnostic
 * painter (ISR nets AND the main-thread saturn_isr_paint) stands down
 * while it is set, so the one-shot verdict panel stays clean. */
volatile unsigned int saturn_rv_done  = 0;

/* Main-thread paint of the ISR-latched verdicts. Called from the
 * D_DoomLoop top every iteration while the ISR holds the latch.
 * r58.1: ROWS ONLY â€” no stage fill. The fill is a 2x64KB sweep and the
 * net-1 latch is sticky, so a fill here would blank the screen for the
 * whole run (the false-positive cyan hid the demo picture entirely);
 * the glyph rows carry the complete verdict and stay visible over the
 * world. Row set (all decimal): S = master site at trip, V = vbl count,
 * L = level loads, T = frames rendered, E = segwalls, A = build FRT acc
 * >>8, H = half (walk) FRT acc >>8, I/M = SCU IST / IMASK at trip. */
void saturn_isr_paint(void)
{
    extern volatile unsigned int saturn_load_count;
    extern volatile unsigned int saturn_frame_count;
    extern int                   segwalls_count;
    extern volatile unsigned int saturn_build_frt_acc;
    extern volatile unsigned int saturn_half_frt_acc;
    static char                  row[12];

    /* r74: the one-shot freeze verdict owns the screen once painted â€”
     * the ISR stopped setting dirty bits, so drop any stale latch and
     * keep its panel clean. */
    if (saturn_rv_done)
    {
        saturn_isr_dirty = 0;
        return;
    }

    row[0] = 'S'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_isr_site & 0xFFFFu);
    Saturn_StageText(8, 8, row);
    row[0] = 'V'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_isr_vbl & 0xFFFFu);
    Saturn_StageText(8, 28, row);
    row[0] = 'L'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_load_count & 0xFFFFu);
    Saturn_StageText(8, 48, row);
    row[0] = 'T'; row[1] = '=';
    Saturn_Udec(row + 2, saturn_frame_count & 0xFFFFu);
    Saturn_StageText(8, 68, row);
    row[0] = 'E'; row[1] = '=';
    Saturn_Udec(row + 2, (unsigned int)(segwalls_count & 0xFFFFu));
    Saturn_StageText(8, 88, row);
    row[0] = 'A'; row[1] = '=';
    Saturn_Udec(row + 2, (saturn_build_frt_acc >> 8) & 0xFFFFu);
    Saturn_StageText(8, 108, row);
    row[0] = 'H'; row[1] = '=';
    Saturn_Udec(row + 2, (saturn_half_frt_acc >> 8) & 0xFFFFu);
    Saturn_StageText(8, 128, row);

    /* r58: name the interrupt machinery â€” SCU IST (0x25FE00A8, bit15 =
     * VBLANK-IN pending) and IMASK (0x25FE00A4), decimal. */
    {
        static char im[28];

        im[0] = 'I'; im[1] = '=';
        Saturn_Udec(im + 2, saturn_isr_ist);
        im[saturn_ivlen(im)] = ' ';
        im[saturn_ivlen(im)] = 'M';
        im[saturn_ivlen(im)] = '=';
        Saturn_Udec(im + saturn_ivlen(im), saturn_isr_imask);
        Saturn_StageText(8, 188, im);
    }
}

/* One-time NBG0 bitmap setup. Called from I_InitVideo (D_DoomMain boot
 * start): NBG1 stays enabled above NBG0, so the boot stage colors keep
 * covering the screen until the first real frame flips NBG1 off. */
static void saturn_vdp2_init_display(void)
{
    volatile unsigned short *cram = (volatile unsigned short *)SATURN_FB_CRAM;
    volatile unsigned int   *a0  = (volatile unsigned int *)SATURN_FB_PAGE_A;
    volatile unsigned int   *a1  = (volatile unsigned int *)SATURN_FB_PAGE_B;
    int i;

    /* CRAM bank 0 (entries 0..255) is unused by Jo (its palettes start
     * at bank 1); NBG0 256-color reads it via color-RAM offset 0. Seed
     * with the same grayscale palette the expand path used. */
    for (i = 0; i < 256; i++)
        cram[i] = saturn_palette16[i];
    /* r101: StageTextHi glyphs stamp palette index 4 (white in PLAYPAL
     * as well, so I_SetPalette keeps it white and the game palette is
     * never poisoned — the r100 white-speckle bug was cram[2]=0x7FFF).
     * Seed CRAM[4] white here so the boot beacon contrasts with the
     * stage fills BEFORE I_SetPalette runs; the wholesale 256-entry
     * rewrite at I_SetPalette replaces this seed with PLAYPAL[4] —
     * also pure white — so it cannot survive into the game. */
    cram[4] = 0x7FFFu;

    /* Clear both pages so the never-displayed lines 200..255 (the NBG0
     * window can read them at 224 visible lines) are palette index 0.
     * Page A spans its full 128 KB bank; page B stops 2 bytes short of
     * the back-screen color word. */
    for (i = 0; i < 0x20000 / 4; i++)
        a0[i] = 0;
    for (i = 0; i < (0x1FFFC) / 4; i++)
        a1[i] = 0;
    *(volatile unsigned short *)((volatile unsigned char *)a1 + 0x1FFFC) = 0;

    /* Letterbox / transparent pixels read black: DOOM palette index 0 is
     * black, and whether VDP2 treats palette code 0 as transparent or
     * not the result is the same. The direct write alone is NOT enough:
     * Jo's back-color table lives wherever jo_vdp2_malloc_autosize put
     * it (inside the NBG1 bitmap region), not at SATURN_FB_BACKCOL Ã¢â‚¬â€ a
     * stale 0xA100 DarkCyan byte-pair there made every index-0 pixel
     * display teal. slBack1ColSet re-points the VDP2 register at our
     * dead address AND writes the color through SGL's shadow. */
    *(volatile unsigned short *)SATURN_FB_BACKCOL = 0x0000;
    slBack1ColSet((void *)SATURN_FB_BACKCOL, 0x0000);

    /* NBG0 = 256-color bitmap at page A; zoom 1.0/1.0 — the fb is
     * 320 columns wide since 2026-09-05 (full-width render) and maps
     * 1:1 onto the 320-wide screen. (The half-width era used 0.5
     * horizontal to stretch 160 fb columns across 320 screen px.) */
    slBitMapNbg0(COL_TYPE_256, BM_512x256, (void *)SATURN_FB_PAGE_A);
    slZoomNbg0(toFIXED(1.0), toFIXED(1.0));
    slColRAMOffsetNbg0(0);
    /* 2026-09-06: no scroll — the fb is now the full 224-line window
     * (DOOM_FBHEIGHT): the 3D view spans rows 0..191 and the status
     * bar 192..223, so nothing needs shifting (the earlier -24 scroll
     * experiment moved the view down with the bar; the fb itself grew
     * instead). */
    slScrMoveNbg0(toFIXED(0), toFIXED(0));

    saturn_fb_live = 1;
}

/* Freeze-forensics probe register. Erases the 16x16 probe cell at
 * (x,186) on BOTH pages, then stamps one glyph. StageText paints
 * set bits only, so without the erase consecutive stamps OR into an
 * unreadable union block. One cell per tail stage (digits=188,
 * E=228, Y=248, Z=268 — all inside the chain-letter region, which
 * never paints while the chain is wedged): a cell showing its glyph
 * means that stage's VRAM writes completed; a cell left EMPTY means
 * the machine died inside that stage's own erase/paint — a plain
 * CPU write to VDP2 VRAM that never completed.
 *
 * 2026-09-07 (real-HW photos): the RAW 0xF7 FILL here is the black
 * box over the status-bar ARMOR % — it runs EVERY frame, ungated by
 * saturn_quiet_hud (only the StageText glyph below is guarded), and
 * rows 186..201 sit over the bar top, which the old TV bezel cropped.
 * 0xF7 is the letterbox second-black (PLAYPAL[247]), so the box is
 * whatever CRAM[0xF7] holds at display time: black on real HW, gold
 * under stale boot diagnostic CRAM. Gate the whole stamp in quiet
 * mode; non-quiet debug builds keep the freeze forensics. */
static void saturn_probe_stamp(int x, char c)
{
    volatile unsigned char *pe;
    char                    s[2];
    int                     r2, i;

    if (saturn_quiet_hud)
        return;
    for (r2 = 0; r2 < 16; r2++)
    {
        pe = (volatile unsigned char *)SATURN_FB_PAGE_A
             + (unsigned int)(186 + r2) * SATURN_FB_STRIDE + x;
        for (i = 0; i < 16; i++)
            pe[i] = 0xF7;
        pe = (volatile unsigned char *)SATURN_FB_PAGE_B
             + (unsigned int)(186 + r2) * SATURN_FB_STRIDE + x;
        for (i = 0; i < 16; i++)
            pe[i] = 0xF7;
    }
    s[0] = c;
    s[1] = 0;
    Saturn_StageTextHi(x, 186, s);
}

static inline void saturn_vdp2_copy_row_320(const void *src_row, volatile void *dst_row)
{
    const unsigned int *s = (const unsigned int *)src_row;
    unsigned int *d = (unsigned int *)dst_row;
    int bursts = 5;
    __asm__ volatile (
        "1: \n\t"
        "mov.l  @%0+, r0 \n\t"
        "mov.l  @%0+, r1 \n\t"
        "mov.l  r0, @%1 \n\t"
        "mov.l  r1, @(4, %1) \n\t"
        "mov.l  @%0+, r2 \n\t"
        "mov.l  @%0+, r3 \n\t"
        "mov.l  r2, @(8, %1) \n\t"
        "mov.l  r3, @(12, %1) \n\t"
        "mov.l  @%0+, r0 \n\t"
        "mov.l  @%0+, r1 \n\t"
        "mov.l  r0, @(16, %1) \n\t"
        "mov.l  r1, @(20, %1) \n\t"
        "mov.l  @%0+, r2 \n\t"
        "mov.l  @%0+, r3 \n\t"
        "mov.l  r2, @(24, %1) \n\t"
        "mov.l  r3, @(28, %1) \n\t"
        "mov.l  @%0+, r0 \n\t"
        "mov.l  @%0+, r1 \n\t"
        "mov.l  r0, @(32, %1) \n\t"
        "mov.l  r1, @(36, %1) \n\t"
        "mov.l  @%0+, r2 \n\t"
        "mov.l  @%0+, r3 \n\t"
        "mov.l  r2, @(40, %1) \n\t"
        "mov.l  r3, @(44, %1) \n\t"
        "mov.l  @%0+, r0 \n\t"
        "mov.l  @%0+, r1 \n\t"
        "mov.l  r0, @(48, %1) \n\t"
        "mov.l  r1, @(52, %1) \n\t"
        "mov.l  @%0+, r2 \n\t"
        "mov.l  @%0+, r3 \n\t"
        "mov.l  r2, @(56, %1) \n\t"
        "mov.l  r3, @(60, %1) \n\t"
        "dt     %2 \n\t"
        "bf/s   1b \n\t"
        "add    #64, %1 \n\t"
        : "+r"(s), "+r"(d), "+r"(bursts)
        :
        : "r0", "r1", "r2", "r3", "t", "memory"
    );
}

/* Copy the 320x200 8bpp fb into the back NBG0 bitmap page (head 320
 * bytes of each 512-byte line). Word copy when the fb is 4-aligned
 * (jo_malloc guarantees it); the stride arithmetic stays exact either
 * way. No cache maintenance: the CPU reads the fb through its own
 * operand cache and writes VRAM directly — nothing here is DMA. */
static void saturn_vdp2_copy_back(void)
{
    extern int          saturn_game_started;
    extern int          saturn_hw_sky_visible;
    extern int          saturn_hw_sky_latch;
    extern int          saturn_hw_sky_prime;
    extern int          saturn_stbar_y;
    extern int          saturn_view_cy;
    extern int          saturn_view_latch;
    extern int          saturn_stbar_dirty_latch;
    extern int          saturn_perf_dirty_latch;
    extern int          saturn_face_dirty_latch;
    extern int          saturn_hw_is_sky;
    extern int          saturn_show_fps;
    const unsigned char *src = I_VideoBuffer;
    unsigned char       *dst = (unsigned char *)(saturn_fb_front
                                                  ? SATURN_FB_PAGE_A
                                                  : SATURN_FB_PAGE_B);
    int y;

    int sky_active = (saturn_hw_sky_latch > 0 || saturn_hw_sky_prime > 0);

    if (saturn_hw_sky_prime > 0)
        saturn_hw_sky_prime--;

    int st_y = (saturn_view_latch > 0) ? 168 : ((saturn_stbar_y > 0) ? saturn_stbar_y : 192);
    int cy_y = (saturn_view_cy > 0) ? saturn_view_cy : 96;

    if (((unsigned int)src & 3) == 0)
    {
        if (saturn_game_started)
        {
            if (sky_active || saturn_view_latch > 0)
            {
                int max_sky = (saturn_hw_is_sky || saturn_view_latch > 0) ? st_y : 108;
                if (max_sky > st_y) max_sky = st_y;
                const unsigned char *s = src;
                volatile unsigned char *d = dst + SATURN_BORDER_X;
                for (y = 0; y < max_sky; y++)
                {
                    saturn_vdp2_copy_row_320(s, d);
                    s += FB_W;
                    d += SATURN_FB_STRIDE;
                }
            }
            if (st_y < SATURN_FB_LINES && (saturn_stbar_dirty_latch > 0 || saturn_view_latch > 0))
            {
                int end_y = saturn_show_fps ? SATURN_FB_LINES : (st_y + 32);
                if (end_y > SATURN_FB_LINES) end_y = SATURN_FB_LINES;
                const unsigned char *s = src + st_y * FB_W;
                volatile unsigned char *d = dst + st_y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                for (y = st_y; y < end_y; y++)
                {
                    saturn_vdp2_copy_row_320(s, d);
                    s += FB_W;
                    d += SATURN_FB_STRIDE;
                }
            }
            else if (st_y < SATURN_FB_LINES)
            {
                if (saturn_face_dirty_latch > 0 && st_y + 32 <= SATURN_FB_LINES)
                {
                    for (y = 0; y < 32; y++)
                    {
                        const unsigned int *s32 = (const unsigned int *)(src + (st_y + y) * FB_W + 140);
                        volatile unsigned int *da = (volatile unsigned int *)((unsigned char *)SATURN_FB_PAGE_A + (st_y + y) * SATURN_FB_STRIDE + SATURN_BORDER_X + 140);
                        volatile unsigned int *db = (volatile unsigned int *)((unsigned char *)SATURN_FB_PAGE_B + (st_y + y) * SATURN_FB_STRIDE + SATURN_BORDER_X + 140);
                        unsigned int w0 = s32[0], w1 = s32[1], w2 = s32[2], w3 = s32[3], w4 = s32[4];
                        unsigned int w5 = s32[5], w6 = s32[6], w7 = s32[7], w8 = s32[8], w9 = s32[9];
                        da[0] = w0; da[1] = w1; da[2] = w2; da[3] = w3; da[4] = w4;
                        da[5] = w5; da[6] = w6; da[7] = w7; da[8] = w8; da[9] = w9;
                        db[0] = w0; db[1] = w1; db[2] = w2; db[3] = w3; db[4] = w4;
                        db[5] = w5; db[6] = w6; db[7] = w7; db[8] = w8; db[9] = w9;
                    }
                }
                if (saturn_perf_dirty_latch > 0 && saturn_show_fps)
                {
                    int r1_y = (st_y < 192) ? (st_y + 34) : (st_y + 22);
                    const unsigned char *s = src + r1_y * FB_W;
                    volatile unsigned char *da = (volatile unsigned char *)SATURN_FB_PAGE_A + r1_y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                    volatile unsigned char *db = (volatile unsigned char *)SATURN_FB_PAGE_B + r1_y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                    for (y = 0; y < 5; y++)
                    {
                        saturn_vdp2_copy_row_320(s, da);
                        saturn_vdp2_copy_row_320(s, db);
                        s += FB_W;
                        da += SATURN_FB_STRIDE;
                        db += SATURN_FB_STRIDE;
                    }
                    if (r1_y + 8 + 6 <= SATURN_FB_LINES)
                    {
                        int r2_y = r1_y + 8;
                        s = src + r2_y * FB_W;
                        da = (volatile unsigned char *)SATURN_FB_PAGE_A + r2_y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                        db = (volatile unsigned char *)SATURN_FB_PAGE_B + r2_y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                        for (y = 0; y < 5; y++)
                        {
                            saturn_vdp2_copy_row_320(s, da);
                            saturn_vdp2_copy_row_320(s, db);
                            s += FB_W;
                            da += SATURN_FB_STRIDE;
                            db += SATURN_FB_STRIDE;
                        }
                    }
                }
            }
        }
        else
        {
            const unsigned char *s = src;
            volatile unsigned char *d = dst + SATURN_BORDER_X;
            for (y = 0; y < SATURN_FB_LINES; y++)
            {
                saturn_vdp2_copy_row_320(s, d);
                s += FB_W;
                d += SATURN_FB_STRIDE;
            }
        }
    }
    else
    {
        if (saturn_game_started)
        {
            if (sky_active)
            {
                int max_sky = st_y;
                for (y = 0; y < max_sky; y++)
                {
                    const unsigned char *s = src + y * FB_W;
                    volatile unsigned char *d = dst + y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                    int i;
                    for (i = 0; i < FB_W; i++)
                        d[i] = s[i];
                }
            }
            if (st_y < SATURN_FB_LINES && (saturn_stbar_dirty_latch > 0 || saturn_view_latch > 0))
            {
                int end_y = saturn_show_fps ? SATURN_FB_LINES : (st_y + 32);
                if (end_y > SATURN_FB_LINES) end_y = SATURN_FB_LINES;
                for (y = st_y; y < end_y; y++)
                {
                    const unsigned char *s = src + y * FB_W;
                    volatile unsigned char *d = dst + y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                    int i;
                    for (i = 0; i < FB_W; i++)
                        d[i] = s[i];
                }
            }
        }
        else
        {
            for (y = 0; y < SATURN_FB_LINES; y++)
            {
                const unsigned char *s = src + y * FB_W;
                volatile unsigned char *d = dst + y * SATURN_FB_STRIDE + SATURN_BORDER_X;
                int i;
                for (i = 0; i < FB_W; i++)
                    d[i] = s[i];
            }
        }
    }

    if (saturn_hw_sky_latch > 0)
        saturn_hw_sky_latch--;
    if (saturn_view_latch > 0)
        saturn_view_latch--;
    if (saturn_stbar_dirty_latch > 0)
        saturn_stbar_dirty_latch--;
    if (saturn_perf_dirty_latch > 0)
        saturn_perf_dirty_latch--;
    if (saturn_face_dirty_latch > 0)
        saturn_face_dirty_latch--;
    /* Probe cell 188 (monotonic register, continues the digits):
     * 'E' = the copy loop ran to completion. Cell left at '?' =
     * died in the last rows' copy; left at a digit = died in that
     * band. */
    saturn_probe_stamp(188, 'E');
}
#endif /* SATURN_VDP2_FB */

/* This callback runs in the VBlank-IN interrupt handler (Jo registers it
 * through SGL's slIntFunction) Ã¢â‚¬â€ keep it to trivial CPU work only.
 *
 * It used to drain the cart-fill queue here (one saturn_copy_to_cart per
 * tick), but every drain entry does a full jo_fs_close/jo_fs_open/
 * jo_fs_seek_forward/jo_fs_read_next_bytes on __jo_iwad_handle Ã¢â‚¬â€ i.e.
 * GFS_Close/GFS_Open/GFS_Seek/GFS_Fread plus jo_malloc/jo_free Ã¢â‚¬â€ and:
 *   - GFS is a single non-reentrant state machine (one GfsMng) shared
 *     with the main thread, which is inside its OWN GFS calls on the
 *     same handle during I_WadBase's sync copies and every cold lump
 *     read. Concurrent use from the ISR corrupts the GFS state and the
 *     main thread's blocking GFS_Fread never completes -> machine
 *     frozen mid-WAD-load ("stuck at LOADING WAD").
 *   - Jo's heap (jo_malloc/jo_free) is not interrupt-safe.
 *   - A blocking GFS_Fread inside the VBlank-in ISR can deadlock on
 *     its own completion path if that relies on interrupts masked at
 *     this IPL.
 * The queue is now drained from the main thread in D_DoomLoop. */
static void saturn_vblank_callback(void)
{
    saturn_vbl_count++;
    *(volatile unsigned int *)
        ((unsigned int)&saturn_vbl_count + 0x20000000u)
        = saturn_vbl_count;

    /* Continuous 60Hz SMPC peripheral cashier polling.
     * SGL normally only calls slSmpcCashSend() during _BlankIn when _SynchCount == 0.
     * Since DOOM does not use slSynch(), _SynchCount only hits 0 once every 256 VBlanks (4.26s).
     * By calling slSmpcCashSend() here whenever _SynchCount != 0, we ensure SMPC INTBACK
     * results are fetched and new INTBACK commands are queued on EVERY vertical blank (60 Hz),
     * while avoiding duplicate calls if SGL's _BlankIn already ran it this frame. */
    *(volatile unsigned char *)0x060FFFBAu |= 0x02u;
    if (*(volatile signed char *)0x060FFC13u != 0)
    {
        slSmpcCashSend();
    }

    /* r82 A/B: INSTRUMENTATION STRIPPED FROM THE ISR. The user
     * recalled the real-HW freeze family beginning right after the
     * performance-timer rework (rounds ~43-58: the SAT_FRT_NET
     * render bail-nets + this callback growing the net painting /
     * verdict / census / P-W reporter). Every documented good real-HW
     * boot predates the ISR-side painting; every freeze postdates it.
     * This disc gates EVERYTHING diagnostic on
     * saturn_instrument_armed (= 0): the callback is now just the
     * vblank counter + the one-time disc ID, and SAT_FRT_NET is a
     * no-op. If r82 boots on real HW, the instrumentation is
     * convicted; if it still wedges pre-loop, the GSUB record (which
     * needs no ISR) names the step and the timers are exonerated. */
    if (!saturn_instrument_armed)
        return;

    /* r80/r98: DISC-ID BEACON — every vbl until the first frame
     * completes, at a VISIBLE cell. The old one-shot stamp at
     * (220,168) was dead code: Saturn_StageText breaks past
     * cx > 144 (the 160-col strip), so no disc was ever identified
     * in a photo. Worse, r95/r96/r97 all freeze to the SAME
     * solid-blue m3 screen, so "no text" photos could not tell a
     * stale disc from a genuine pre-letter wedge. Painting the same
     * static string at the same cells every vbl cannot ghost-union,
     * and the beat digit beside it is an ISR-LIVENESS probe: a
     * frozen digit = the ISR died at that count; a MISSING r98 = the
     * ISR never ran (dead vblank chain — the r95 'creeping colours'
     * condition). White (StageTextHi) so it contrasts on any fill. */
    {
        extern volatile unsigned int saturn_frame_done_vbl;

        if (saturn_frame_done_vbl == 0u)
        {
            char b[2];

            Saturn_StageTextHi(8, 160, "r104");
            b[0] = "0123456789ABCDEF"[(saturn_vbl_count >> 4) & 0xF];
            b[1] = 0;
            Saturn_StageTextHi(100, 160, b);
        }
    }

    /* r74: ONE-SHOT FREEZE VERDICT. The r73 real-HW photo defeated the
     * instrument itself: the nets repainted the diagnostic rows at
     * changing values every 16/64 vbl, and minutes later the cells we
     * needed (S/C/K) were ghost-unioned into solid blocks â€” while the
     * census panel and the K row even COLLIDED at y=68 pre-loop. So
     * instead of another repainting net: watch the progress signature
     * (every counter any loader or loop bumps) every 16 vbl; when more
     * than 600 vbl have passed since the last completed frame AND the
     * signature has not moved for 20 consecutive samples (~5.3 s of
     * nothing moving anywhere), paint every diagnostic ONCE on its own
     * zero-padded row below, set saturn_rv_done, and every painter â€”
     * ISR nets and main-thread saturn_isr_paint alike â€” stands down
     * forever. The panel then reads clean in any photo taken minutes
     * later. Two safety properties:
     *  - a legitimate long load never fires: its CD calls move R/G/F
     *    and its tics move T every window;
     *  - a false fire self-heals: the next real frame commit repaints
     *    both pages and wipes the text.
     * Site tracking runs every vbl (two compares) so a fast-cycling
     * site still lands in the K bitmask and the C toggle count. */
    {
        static unsigned int rv_sig_last  = 0;
        static unsigned int rv_hold      = 0;
        static unsigned int rv_mask      = 0;
        static unsigned int rv_togs      = 0;
        static unsigned int rv_site_last = 0;
        extern volatile unsigned int saturn_sim_site;

        if (saturn_sim_site != rv_site_last)
        {
            rv_togs++;
            rv_site_last = saturn_sim_site;
        }
        rv_mask |= 1u << (saturn_sim_site & 15u);

        if ((saturn_vbl_count & 15u) == 0u)
        {
            extern volatile unsigned int saturn_cart_kb;
            extern volatile unsigned int saturn_fill_progress;
            extern int                   saturn_io_diag_freads;
            extern volatile unsigned int saturn_prof_gfs_vbl;
            extern volatile unsigned int saturn_prof_cpy_vbl;
            extern volatile unsigned int saturn_tic_count;
            extern volatile unsigned int saturn_frame_count;
            extern volatile unsigned int saturn_iter_count;
            extern volatile unsigned int saturn_gfs_site;
            extern volatile unsigned int saturn_frame_done_vbl;
            unsigned int sig =
                saturn_cart_kb + saturn_fill_progress
                + (unsigned int)saturn_io_diag_freads
                + saturn_prof_gfs_vbl + saturn_prof_cpy_vbl
                + saturn_tic_count + saturn_frame_done_vbl
                + saturn_iter_count + saturn_sim_site + saturn_gfs_site;
            /* Stall = vbl since the last completed frame; before the
             * first frame ever completes, elapsed vbl IS the stall
             * (a 5.3 s all-counters-frozen window during boot is just
             * as dead â€” and a false fire self-heals on frame commit). */
            unsigned int stall = saturn_frame_done_vbl
                ? (unsigned int)(saturn_vbl_count
                                 - saturn_frame_done_vbl)
                : saturn_vbl_count;

            /* Trigger 2 â€” the r75/r76 real-HW wedge is PRE-LOOP (the
             * D/W+I census rows paint CLEAN = the in-loop T/H census
             * never ran; ISR P row = 8 = saturn_gfs_setup stuck), so
             * keying on sim_site != 0 was blind too. The honest
             * metric is the COMMIT, qualified by boot progress: once
             * ANY frame has completed (frame_done_vbl != 0 â€” the
             * smoke test stamps it ~40 s in), stall > 1200 vbl (20 s)
             * for 20 samples is a bug in any state, pre-loop or not â€”
             * level loads are seconds on r66+ HW and the fill drains
             * between frames. Before the first commit (frame_done ==
             * 0) the ~40 s boot is legitimately commit-free. */
            static unsigned int rv_nc_hold = 0;

            /* r86: the first t=45 Kronos fire sat over a freeze the
             * demo recovered from by RESTARTING — rv_done then stood
             * the panel down forever and the second, deterministic
             * freeze painted nothing. A changed frame_done_vbl after
             * a fire means the world moved on (frame committed or the
             * smoke test re-stamped it), so re-arm the verdict. */
            static unsigned int rv_fd_snap = 0;

            if (saturn_rv_done && saturn_frame_done_vbl != rv_fd_snap)
            {
                saturn_rv_done = 0;
                rv_hold        = 0;
                rv_nc_hold     = 0;
                rv_sig_last    = sig;
            }

            if (saturn_frame_done_vbl != 0u && stall > 1200u)
            {
                ++rv_nc_hold;
            }
            else
            {
                rv_nc_hold = 0;
            }

            if (stall <= 600u || sig != rv_sig_last)
            {
                rv_hold     = 0;
                rv_sig_last = sig;
            }
            else
            {
                /* r81: THE INCREMENT WAS MISSING â€” rv_hold was reset
                 * and compared but never counted, so trigger 1 (frozen
                 * signature) has been dead since r74 and trigger 2 was
                 * the only path to the panel. */
                ++rv_hold;
            }

            if (rv_hold >= 20u || rv_nc_hold >= 20u)
            {
                static char row[24];

                /* FIRE. Snap every value, paint each row once, then
                 * stand down. I/M = SCU IST/IMS at fire time. */
                unsigned int ist = *(volatile unsigned int *)0x25FE00A4u;
                unsigned int ims = *(volatile unsigned int *)0x25FE00A0u;

                extern void Saturn_Uhex(char *out, unsigned int v);
                extern volatile unsigned int saturn_cart_kb;
                extern volatile unsigned int saturn_fill_progress;
                extern int                   saturn_io_diag_freads;
                extern volatile unsigned int saturn_prof_gfs_vbl;
                extern volatile unsigned int saturn_prof_cpy_vbl;
                extern volatile unsigned int saturn_tic_count;
                extern volatile unsigned int saturn_frame_count;
                extern volatile unsigned int saturn_iter_count;
                extern volatile unsigned int saturn_gfs_site;
                extern volatile unsigned int saturn_frame_open;
                extern volatile unsigned int saturn_init_site;
                extern volatile unsigned int saturn_gsub_rec;
                extern int                   Saturn_DspCartOk(void);

                row[0] = 'S'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_sim_site & 0xFFFFu, 2);
                row[4] = ' '; row[5] = 'O'; row[6] = '=';
                row[7] = saturn_frame_open ? '1' : '0';
                row[8] = '\0';
                Saturn_StageText(8, 8, row);

                row[0] = 'K'; row[1] = '=';
                Saturn_Uhex(row + 2, rv_mask);
                row[10] = '\0';
                Saturn_StageText(8, 28, row);

                row[0] = 'P'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_gfs_site & 0xFFFFu, 2);
                row[4] = ' '; row[5] = 'W'; row[6] = '=';
                Saturn_Udec0(row + 7, saturn_cart_kb & 0xFFFFu, 4);
                row[11] = '\0';
                Saturn_StageText(8, 48, row);

                row[0] = 'D'; row[1] = '=';
                row[2] = Saturn_DspCartOk() ? '1' : '0';
                row[3] = ' '; row[4] = 'V'; row[5] = '=';
                Saturn_Udec0(row + 6, stall & 0xFFFFu, 4);
                row[10] = '\0';
                Saturn_StageText(8, 68, row);

                row[0] = 'F'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_fill_progress & 0xFFFFu, 4);
                row[6] = ' '; row[7] = 'R'; row[8] = '=';
                Saturn_Udec0(row + 9,
                             (unsigned int)saturn_io_diag_freads
                             & 0xFFFFu, 4);
                row[13] = '\0';
                Saturn_StageText(8, 88, row);

                row[0] = 'T'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_frame_count & 0xFFFFu, 4);
                row[6] = ' '; row[7] = 'N'; row[8] = '=';
                Saturn_Udec0(row + 9, saturn_iter_count & 0xFFFFu, 4);
                row[13] = '\0';
                Saturn_StageText(8, 108, row);

                row[0] = 'G'; row[1] = '=';
                Saturn_Udec0(row + 2,
                             (saturn_prof_gfs_vbl >> 4) & 0xFFFFu, 4);
                row[6] = ' '; row[7] = 'U'; row[8] = '=';
                Saturn_Udec0(row + 9,
                             (saturn_prof_cpy_vbl >> 4) & 0xFFFFu, 4);
                row[13] = '\0';
                Saturn_StageText(8, 128, row);

                row[0] = 'I'; row[1] = '=';
                Saturn_Uhex(row + 2, ist);
                row[6] = ' '; row[7] = 'M'; row[8] = '=';
                Saturn_Uhex(row + 9, ims);
                row[13] = '\0';
                Saturn_StageText(8, 148, row);

                /* r85: the census I row dies when this panel fires
                 * (rv_done stands the census down), so the level-init
                 * breadcrumb moves onto the panel: J = saturn_init_site
                 * (g_game.c decode), B = the previous boot's GSUB
                 * record. J leads so both digits land in the half of
                 * the NBG1 page the real-HW TV actually shows (photos
                 * cut everything past page x=160). */
                row[0] = 'J'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_init_site & 0xFFFFu, 2);
                row[4] = ' '; row[5] = 'B'; row[6] = '=';
                row[7] = (char)('0' + (saturn_gsub_rec & 15u));
                row[8] = '\0';
                Saturn_StageText(8, 168, row);

                saturn_rv_done   = 1;
                rv_fd_snap       = saturn_frame_done_vbl;
                saturn_isr_dirty = 0;
            }
        }
    }

    /* Round-51: ISR-side freeze reporter â€” moved FIRST in the handler
     * (a hang in any later ISR work must not suppress the verdict) and
     * now three nets. Round-50's run froze with NO verdict at all: no
     * cyan (net 1 never fired) AND no record (the join-poll red path
     * never ran), which pins the freeze to either a dead/masked vblank
     * chain at that spot, or a master that kept beating inside a stuck
     * loop. The nets split those cases:
     *   net 1 (beat-idle, latched=1, sticky CYAN): saturn_master_beat
     *     frozen for 120 vbl â€” a wedge inside any single call.
     *   net 2 (frame-open age, NON-sticky MAGENTA): saturn_frame_open
     *     held > 120 vbl even while beats continue â€” a master spinning
     *     in a beating loop (the round-42 re-post poll) never trips
     *     net 1. Non-sticky: the next real frame's copy erases it, so
     *     a recovered >2 s hitch self-heals. (Round-50 defined the
     *     flag but never set it â€” dead. The frame arm now sets it.)
     *   net 3 (early marker): at 30 beatless vbl paint S=<site> at the
     *     top-left â€” pre-latch evidence the interrupt chain is ALIVE
     *     mid-stall, even if the full verdict later fails to paint.
     *   Latched rows: S (last site stamp), V (vbl count), H
     *     (ISR-aliveness toggle, flips every 32 vbl), K (last bail
     *     kind â€” now also 5 sprite pass / 6 flush runs / 7 blit rows /
     *     8 TVSTAT flip wait). No BUP here â€” GFS calls are banned in
     *     ISR context; the display IS the readout.
     * r74: every net below STANDS DOWN once the freeze verdict has
     * painted (saturn_rv_done) â€” no more overprints, the panel stays
     * clean for any later photo. */
    if (!saturn_rv_done)
    {
        static unsigned int beat_snap = 0;
        static unsigned int beat_idle = 0;
        static unsigned int open_since = 0;
        static int          latched   = 0;

        extern volatile int          saturn_freeze_watch;
        extern volatile unsigned int saturn_master_beat;
        extern volatile unsigned int saturn_master_site;
        extern volatile int          saturn_seg_bailed;
        extern volatile unsigned int saturn_frame_open;
        extern volatile unsigned int saturn_build_frt_acc;
        extern volatile unsigned int saturn_half_frt_acc;

        /* r56 net-3.5: ALWAYS-ON iteration/tic census. The terminal
         * crawl paints no verdict (loop beating, frames completing
         * < 120 vbl, picture static), so the T/H census runs every
         * normal frame too: every 64 vbl paint the tic and iteration
         * counts of that window at the top-left.
         *   T row = game tics run in the last 64 vbl (healthy: ~32)
         *   H row = main-loop iterations in the last 64 vbl (~32)
         * T~0 with H~32 = the sim is starved (tic gate / demo source);
         * T~32 with H~2 = the loop itself is slow; both high with a
         * static picture = the demo input never advances the world.
         * r68: PRE-LOOP (sim_site == 0) both rows swap to the boot
         * diagnostic â€” T/H are structurally 0 there:
         *   B row = COMPLETED FILL CHUNKS in the last 64 vbl
         *       (saturn_fill_beat delta, i_wad.c). B climbing while
         *       frozen on cyan = the drain loop is cycling; B frozen
         *       = wedged inside ONE call (then P/W names the site)
         *       or the machine is hard-frozen (two photos identical).
         *   I row = level-init breadcrumb (saturn_init_site,
         *       decode in g_game.c). I > 0 = the fill is done and
         *       the wedge is inside level init; the value names the
         *       step. B = 0 + I >= 3 = wedged OUTSIDE GFS entirely.
         *       Both rows are zero-padded fixed-width (Saturn_Udec0)
         *       so frozen values read clean â€” the r66/r67 photos'
         *       variable-width ghost unions made them undecodable. */
        {
            extern void Saturn_Udec(char *out, unsigned int v);
            extern volatile unsigned int saturn_tic_count;
            extern volatile unsigned int saturn_iter_count;
            extern volatile unsigned int saturn_sim_site;
            static unsigned int snap_tic  = 0;
            static unsigned int snap_iter = 0;
            static unsigned int snap_rd   = 0;

            if ((saturn_vbl_count & 63u) == 0u)
            {
                static char row[14];

                if (saturn_sim_site == 0u)
                {
                    extern volatile unsigned int saturn_fill_beat;
                    extern volatile unsigned int saturn_init_site;
                    extern volatile unsigned int saturn_gsub_rec;

                    /* r72: the r68 B row (fill-beat delta) is
                     * structurally 0 pre-loop since r70 moved the
                     * drain into D_DoomLoop. In its place: D = the
                     * DSP cart engine's arm state (1 = DSP DMA writes
                     * active, 0 = demoted to CPU memcpy) and W = cart
                     * KB written so far â€” a single photo now names the
                     * transport AND the write-stream progress at the
                     * moment of any boot wedge. r78: 'r8' disc ID rides
                     * the D row (the in-loop S row carries it too). */
                    {
                        extern int Saturn_DspCartOk(void);
                        extern volatile unsigned int saturn_cart_kb;

                        row[0] = 'D'; row[1] = '=';
                        row[2] = Saturn_DspCartOk() ? '1' : '0';
                        row[3] = 'W'; row[4] = '=';
                        Saturn_Udec0(row + 5, saturn_cart_kb & 0xFFFFu, 4);
                        row[9] = 'r'; row[10] = '1'; row[11] = '0';
                        row[12] = '4';
                        row[13] = '\0';
                    }
                    Saturn_StageText(8, 148, row);
                    /* r85: I (level-init breadcrumb) leads, B (prev-
                     * boot GSUB record) trails — both cells land in the
                     * visible half of the page (x<=160). */
                    row[0] = 'I'; row[1] = '=';
                    Saturn_Udec0(row + 2, saturn_init_site & 0xFFFFu, 2);
                    row[4] = ' '; row[5] = 'B'; row[6] = '=';
                    row[7] = (char)('0' + (saturn_gsub_rec & 15u));
                    row[8] = '\0';
                    Saturn_StageText(8, 168, row);
                    snap_rd = (unsigned int)saturn_fill_beat;
                }
                else
                {
                    /* r102 DSP-eval: the T/H census rows are REPLACED
                     * for the DSP cart-engine evaluation build by the
                     * verify rows the standing directive asks for
                     * ("check that the data transferred is correct"):
                     *   X row = 2 KB chunks whose full readback verify
                     *           FAILED (each repaired; healthy = 0000)
                     *   V row = KB fully verified so far (climbs to
                     *           the fill total ~511 KB and holds)
                     * r104: this ISR census no longer paints them. It
                     * only fires once per 64 vbl, and the next frame's
                     * copy_back erases the glyphs 63/64 of the time —
                     * r103 steady-state captures NEVER showed the rows
                     * (unphotographable on real HW, which defeats the
                     * eval). The main loop now paints X/V every
                     * iteration, right after the previous frame's
                     * copy_back, so they persist like the S/V/L rows —
                     * see D_DoomLoop in d_main.c. The tic/iteration
                     * census returns after the eval. */
                }
                snap_tic  = saturn_tic_count;
                snap_iter = saturn_iter_count;
            }
        }

        /* r64: GFS-stall reporter. saturn_gfs_site (i_wad.c) is nonzero
         * only while the MAIN thread is inside one blocking SGL GFS
         * primitive â€” the exact window no other net can name, because
         * the boot fill's 60 s watchdog only checks BETWEEN fill calls
         * and cannot preempt a call that never returns. The ISR is
         * alive in that state (it is painting this), so if the same
         * site stays in flight for > 600 vbl (~10 s; a healthy call
         * finishes in << 1 s), paint P = call site and W = stall
         * length in vbl at y=188/208 (r66: moved below the F/R/G/C
         * timing panel) â€” free during boot (the net-1/2
         * crawl rows only exist once the render path arms the freeze
         * watch; the net-4 rows live at y=8/28/48).
         * Site decode (i_wad.c): 1 = 1-sector sync Fread, 2 = NwFread
         * issue, 3 = Nw pump, 4 = relative GFS_Seek, 5 = NwStop,
         * 6 = Close/Open re-open, 7 = NameToId/Open, 8 = GFS_SetTmode/
         * ReadPara/TransPara (saturn_gfs_setup), 9 = GFS_GetFileSize,
         * 10 = the 2 KB cart memcpy in saturn_copy_from_here (site is
         * 0 during it â€” which is exactly why the r64 photo showed no
         * P/W: the r63 DMA fill and the r64 memcpy fill share only
         * GFS calls and BULK CART WRITES, so site 10 is the non-GFS
         * suspect).
         *
         * r65 additions:
         *  - F = fill progress in KB (saturn_fill_progress, i_wad.c)
         *    at y=68, painted every 64 vbl while the game loop has
         *    never been entered (sim_site == 0). CLIMBING F = the
         *    boot fill is alive (slow, not wedged â€” wait for the 60 s
         *    watchdog); FROZEN F across two photos = wedged inside
         *    one chunk, and P then names the call.
         *  - The ISR also stamps the stall into the cart's top 512
         *    bytes (rec[32..38], 'GSTL' magic â€” disjoint from the
         *    flight recorder's REC1 at rec[0..18]) the first time it
         *    fires, refreshed with W every 256 vbl while the stall
         *    persists. If the wedged primitive later raises the IPL
         *    (ISR dies: the r64 photo is also consistent with a stall
         *    that killed the ISR before the 600-vbl gate), this is
         *    the only surviving trace. The NEXT boot's ISR consumes
         *    it (clears the magic) and keeps painting P/W from it â€”
         *    CAVEAT: the SAROO boot menu may wipe cart DRAM, so an
         *    absent G row after a menu pass proves nothing. */
        {
            extern volatile unsigned int saturn_gfs_site;
            extern volatile unsigned int saturn_fill_progress;
            extern volatile unsigned int saturn_sim_site;
            extern void Saturn_Udec(char *out, unsigned int v);
            static unsigned int last_site = 0;
            static unsigned int since     = 0;
            static unsigned int rec_site  = 0;
            static unsigned int rec_vbl   = 0;
            static int          rec_looked = 0;
            static char         row[12];

            /* r65 next-boot consume of the 'GSTL' cart record, once,
             * the first time this block runs (the ISR starts before
             * any game code could have re-stamped the slot). */
            if (!rec_looked)
            {
                extern unsigned int saturn_cart_base_addr;
                extern unsigned int saturn_cart_size;

                rec_looked = 1;
                if (saturn_cart_base_addr != 0u)
                {
                    volatile unsigned int *rc =
                        (volatile unsigned int *)
                        (saturn_cart_base_addr + SATURN_CART_REC_OFF);

                    if (rc[32] == 0x4753544Cu)      /* 'GSTL' */
                    {
                        rec_site = rc[33];
                        rec_vbl  = rc[34];
                        rc[32]   = 0u;   /* one boot per record */
                    }
                }
            }

            /* r66 boot-fill timing panel, pre-loop only, every 64 vbl
             * (y=68/88/108/128 â€” all free until the game loop runs):
             *   F = KB copied so far (saturn_fill_progress)
             *   R = staged sector-read GFS calls since IWAD open
             *   G = vbl INSIDE the CD calls, in 16-vbl units
             *   C = vbl INSIDE the 2 KB cart memcpys, 16-vbl units
             * Decode with V (elapsed vbl): per-call cost = G*16/R vbl,
             * per-copy cost = C*16/R vbl. G >> C = the CD calls are
             * the crawl; C >> G = the cart writes are (SAROO). Both
             * tiny while F stalls = wedged OUTSIDE both (then P/W
             * names it). */
            if (saturn_sim_site == 0u && (saturn_vbl_count & 63u) == 0u)
            {
                extern int                   saturn_io_diag_freads;
                extern volatile unsigned int saturn_prof_gfs_vbl;
                extern volatile unsigned int saturn_prof_cpy_vbl;
                extern volatile unsigned int saturn_gsub_rec;

                row[0] = 'F'; row[1] = '=';
                Saturn_Udec0(row + 2, saturn_fill_progress & 0xFFFFu, 4);
                Saturn_StageText(8, 68, row);
                row[0] = 'R'; row[1] = '=';
                Saturn_Udec0(row + 2,
                             (unsigned int)saturn_io_diag_freads
                             & 0xFFFFu, 4);
                Saturn_StageText(8, 88, row);
                row[0] = 'G'; row[1] = '=';
                Saturn_Udec0(row + 2,
                             (saturn_prof_gfs_vbl >> 4) & 0xFFFFu, 4);
                Saturn_StageText(8, 108, row);
                row[0] = 'C'; row[1] = '=';
                Saturn_Udec0(row + 2,
                             (saturn_prof_cpy_vbl >> 4) & 0xFFFFu, 4);
                Saturn_StageText(8, 128, row);

                /* r85: the GSUB repaint that lived at (140,88) is gone
                 * — the record rides the init-site breadcrumb (B/I and
                 * B/J rows) instead of a cell past the x=160 page cut. */
            }

            if (saturn_gfs_site == 0u || saturn_gfs_site != last_site)
            {
                last_site = saturn_gfs_site;
                since     = saturn_vbl_count;
            }
            else if ((unsigned int)(saturn_vbl_count - since) > 600u)
            {
                if ((saturn_vbl_count & 63u) == 0u)
                {
                    /* r78: zero-padded like every other row â€” a frozen
                     * value must photograph clean. r81: moved to
                     * (140,168)/(140,188) â€” every real-HW photo has cut
                     * off y=188/208 at x=8, and W (climbing = ISR
                     * alive, frozen = dead, value = death time) is the
                     * single most decisive cell on the screen. */
                    row[0] = 'P'; row[1] = '=';
                    Saturn_Udec0(row + 2, saturn_gfs_site & 0xFFFFu, 2);
                    row[4] = '\0';
                    Saturn_StageText(140, 168, row);
                    row[0] = 'W'; row[1] = '=';
                    Saturn_Udec0(row + 2,
                                 (saturn_vbl_count - since) & 0xFFFFu, 5);
                    row[7] = '\0';
                    Saturn_StageText(140, 188, row);
                }
                /* r79: the ISR-side 'GSTL' cart stamp is DELETED. The
                 * stamp was a CPU write to A-bus cart space FROM INSIDE
                 * the VBlank ISR â€” and every real-HW photo since r75
                 * fits the ISR dying in the very pass that first fires
                 * this reporter (P/W painted at gap ~608-640, but the
                 * verdict panel â€” which needs the ISR alive until gap
                 * ~920 (trigger 1) / ~1520 (trigger 2) â€” never fired,
                 * on four boots across three discs). With the main
                 * thread wedged in a GFS call, an A-bus write here can
                 * stall forever on the wedged CD-block transaction.
                 * The verdict panel is the cross-boot trace now: it
                 * paints everything on screen and needs no cart write.
                 * The next-boot consume below still reads (and clears)
                 * a record left by r78-or-earlier discs â€” harmless,
                 * one word, early boot, A-bus idle. */
            }
            /* Surviving record from the PREVIOUS boot (consumed
             * above): keep P/W on screen while pre-loop so a photo
             * names the call even if this boot wedges before any
             * live stall accumulates. */
            else if (rec_site != 0u && saturn_sim_site == 0u
                     && (saturn_vbl_count & 255u) == 0u)
            {
                row[0] = 'P'; row[1] = '=';
                Saturn_Udec(row + 2, rec_site & 0xFFFFu);
                Saturn_StageText(8, 188, row);
                row[0] = 'W'; row[1] = '=';
                Saturn_Udec(row + 2, rec_vbl & 0xFFFFu);
                Saturn_StageText(8, 208, row);
            }
        }

        /* r58 net-4: the BETWEEN-FRAMES gap net. Frames complete each
         * loop iteration, so anything that eats seconds between them
         * (fill drain, display-cap pump, one slow tic) sits in the one
         * window no earlier net names. With no frame open and > 180
         * vbl since the last frame completed, LATCH + print rows (the
         * YELLOW fill now happens on the main thread in
         * saturn_isr_paint): S = the between-frames phase stamp
         * (saturn_sim_site: 1 loop top, 2/3 fill drain, 4/5 display
         * cap, 6/7 tics, 8 after tics, 9 = G_DoLoadLevel â€” a legit
         * multi-second load), V = vbl count, A = the gap itself. */
        {
            extern void Saturn_Udec(char *out, unsigned int v);
            extern volatile unsigned int saturn_frame_done_vbl;
            extern volatile unsigned int saturn_sim_site;

            if (!saturn_frame_open && saturn_frame_done_vbl != 0u)
            {
                /* r73: livelock net. The r72 real-HW run survived the
                 * cart-write stream (the VBlank mask worked â€” the ISR
                 * stayed alive where r70/r71's died) and then sat
                 * BETWEEN frames for minutes with sim_site cycling â€”
                 * a garbled S cell and ghosted V/A digits, invisible
                 * to P/W (a toggling site never accumulates 600 vbl).
                 * Count site CHANGES between paints (C row: high =
                 * the main thread is cycling, zero = truly wedged) and
                 * latch a BITMASK of every site seen during the gap
                 * (K row) plus the live GFS site â€” together they name
                 * the loop that spins. W rides the A row so the cart
                 * write stream's progress is visible in this net too. */
                static unsigned int sim_last  = 0;
                static unsigned int sim_togs  = 0;
                static unsigned int seen_mask = 0;
                static unsigned int p_last    = 0;
                static unsigned int p_seen    = 0;
                extern volatile unsigned int saturn_gfs_site;

                unsigned int gap = (unsigned int)(saturn_vbl_count
                                                  - saturn_frame_done_vbl);

                if (gap <= 180u)
                {
                    /* r58: gap healed â€” un-latch net-4 (non-sticky). */
                    saturn_isr_dirty &= ~4u;
                    sim_last  = saturn_sim_site;
                    seen_mask = 0;
                    p_last    = saturn_gfs_site;
                    p_seen    = 0;
                }
                else
                {
                    static char row[16];
                    static char hx[] = "0123456789ABCDEF";

                    if (saturn_sim_site != sim_last)
                    {
                        sim_togs++;
                        sim_last = saturn_sim_site;
                    }
                    if (saturn_gfs_site != p_last)
                    {
                        p_last = saturn_gfs_site;
                        p_seen++;
                    }
                    if (saturn_sim_site <= 15u)
                        seen_mask |= (1u << saturn_sim_site);
                    saturn_isr_sim    = saturn_sim_site;
                    saturn_isr_gap    = gap;
                    saturn_isr_vbl    = saturn_vbl_count;
                    saturn_isr_ist    = *(volatile unsigned int *)
                        0x25FE00A4u;         /* r72: IST */
                    saturn_isr_imask  = *(volatile unsigned int *)
                        0x25FE00A0u;         /* r72: IMS */
                    saturn_isr_dirty |= 4u;
                    if ((saturn_vbl_count & 15u) == 0u)
                    {
                        extern volatile unsigned int saturn_cart_kb;
                        unsigned int m = seen_mask;

                        /* r78: zero-pad every net row (the r68 lesson
                         * never reached net-4 â€” variable-width repainting
                         * is what turns a frozen value into the solid
                         * glyph-union blocks that made r75/r76/r77 photos
                         * unreadable). The 'r8' tail on the S row is the
                         * DISC ID: r75/r76/r77 pre-verdict screens were
                         * visually identical and unattributable. */
                        row[0] = 'S'; row[1] = '=';
                        Saturn_Udec0(row + 2, saturn_sim_site & 0xFFFFu, 2);
                        row[4] = ' '; row[5] = 'C'; row[6] = '=';
                        Saturn_Udec0(row + 7, sim_togs & 0xFFFFu, 3);
                        row[10] = ' '; row[11] = 'r'; row[12] = '1';
                        row[13] = '0'; row[14] = '4';
                        row[15] = '\0';
                        sim_togs = 0;   /* r73: count per 16-vbl window */
                        Saturn_StageText(8, 8, row);
                        /* r80: Hxx = ISR HEARTBEAT ((vbl>>5)&0xFF, hex,
                         * repaints every 16 vbl). One photo: frozen Hxx
                         * with W frozen = the ISR died at vbl â‰ˆ Hxx*32
                         * (mod 8192); climbing = alive. Two photos
                         * ~30 s apart: Hxx differs = alive, same =
                         * dead. This is the aliveness verdict the
                         * unioned V cells could never give. */
                        row[0] = 'V'; row[1] = '=';
                        Saturn_Udec0(row + 2, saturn_vbl_count & 0xFFFFu, 5);
                        row[7] = 'H';
                        row[8] = hx[((saturn_vbl_count >> 9) & 15u)];
                        row[9] = hx[((saturn_vbl_count >> 5) & 15u)];
                        row[10] = '\0';
                        Saturn_StageText(8, 28, row);
                        row[0] = 'A'; row[1] = '=';
                        Saturn_Udec0(row + 2, gap & 0xFFFFu, 5);
                        row[7] = ' '; row[8] = 'W'; row[9] = '=';
                        Saturn_Udec0(row + 10, saturn_cart_kb & 0xFFFFu, 4);
                        row[14] = '\0';
                        Saturn_StageText(8, 48, row);
                        /* r73: K = hex bitmask of sim_sites seen since
                         * the gap opened (bit0 = site 0 = pre-loop);
                         * P = the count of GFS-site changes. Static
                         * once gap > 600 vbl so a photo reads it.
                         * r75: painted at x=140/x=220 â€” the census F
                         * row also lives at y=68 (x=8, ends ~x=128),
                         * and the r74b photo showed K overprinting F,
                         * hiding the fill progress â€” the key counter
                         * in the no-I/O livelock state. */
                        if (gap > 600u)
                        {
                            int              n;
                            row[0] = 'K'; row[1] = '=';
                            for (n = 0; n < 4; n++)
                                row[2 + n] = hx[(m >> ((3 - n) * 4)) & 15u];
                            row[6] = '\0';
                            Saturn_StageText(140, 68, row);
                            row[0] = 'P'; row[1] = '=';
                            Saturn_Udec0(row + 2, p_seen & 0xFFFFu, 2);
                            row[4] = '\0';
                            Saturn_StageText(220, 68, row);
                            p_seen = 0;
                        }
                    }
                }
            }
        }

        if (saturn_freeze_watch)
        {
            if (!latched)
            {
                /* r58: net-1 now ALSO requires saturn_frame_count frozen.
                 * The demo plays back through MiniLoop (G_PlayDemoPtr),
                 * which never increments saturn_master_beat â€” beat-idle
                 * alone false-trips 2 s into every demo (the r57
                 * "solid cyan boot"). Frames advancing at ~60/s = the
                 * machine is healthy; frames AND beats frozen = wedge. */
                static unsigned int frame_snap = 0;
                static unsigned int frame_idle = 0;
                extern volatile unsigned int saturn_frame_count;

                if (saturn_master_beat == beat_snap)
                    beat_idle++;
                else
                    beat_idle = 0;
                beat_snap = saturn_master_beat;

                if (saturn_frame_count == frame_snap)
                    frame_idle++;
                else
                    frame_idle = 0;
                frame_snap = saturn_frame_count;

                if (saturn_frame_open)
                {
                    if (open_since == 0u)
                        open_since = saturn_vbl_count;
                    else if ((unsigned int)(saturn_vbl_count - open_since)
                             > 120u)
                    {
                        /* Round-52: the crawl readout â€” 7 rows every vbl
                         * while a frame stays open. Glyph set is
                         * "SLAVETH=", so the labels are S/V/L/T/E/A/H:
                         *   S = last site stamp (hex)
                         *   V = vbl count
                         *   L = level-load count (climbing = reload
                         *       storm: the demo is re-running
                         *       G_DoLoadLevel every frame)
                         *   T = frames rendered since boot
                         *   E = segwalls count (quota blowup detector)
                         *   A = BUILD FRT acc >>8 (0.198 ms units)
                         *   H = master-half (walk) FRT acc >>8
                         * A/H climbing while L sits still = the crawl
                         * is inside the build/walk code; L climbing
                         * with a static picture = the reload storm. */
                        extern void Saturn_Udec(char *out, unsigned int v);
                        extern void Saturn_Uhex(char *out, unsigned int v);
                        extern volatile unsigned int saturn_load_count;
                        extern volatile unsigned int saturn_frame_count;
                        extern int                   segwalls_count;
                        static char row[12];

                        /* r58: LATCH the verdict (main thread paints the
                         * MAGENTA fill + full rows via saturn_isr_paint)
                         * and keep only the throttled glyph rows here â€”
                         * no 2x64KB fill in interrupt context. */
                        saturn_isr_site   = saturn_master_site;
                        saturn_isr_vbl    = saturn_vbl_count;
                        saturn_isr_ist    = *(volatile unsigned int *)
                            0x25FE00A8u;
                        saturn_isr_imask  = *(volatile unsigned int *)
                            0x25FE00A4u;
                        saturn_isr_dirty |= 1u;
                        if ((saturn_vbl_count & 63u) == 0u)
                        {
                        row[0] = 'S'; row[1] = '=';
                        Saturn_Udec(row + 2,
                                    saturn_master_site & 0xFFFFu);
                        Saturn_StageText(8, 8, row);
                        row[0] = 'V'; row[1] = '=';
                        Saturn_Udec(row + 2, saturn_vbl_count & 0xFFFFu);
                        Saturn_StageText(8, 28, row);
                        row[0] = 'L'; row[1] = '=';
                        Saturn_Udec(row + 2, saturn_load_count & 0xFFFFu);
                        Saturn_StageText(8, 48, row);
                        row[0] = 'T'; row[1] = '=';
                        Saturn_Udec(row + 2, saturn_frame_count & 0xFFFFu);
                        Saturn_StageText(8, 68, row);
                        row[0] = 'E'; row[1] = '=';
                        Saturn_Udec(row + 2,
                                    (unsigned int)(segwalls_count
                                                   & 0xFFFFu));
                        Saturn_StageText(8, 88, row);
                        row[0] = 'A'; row[1] = '=';
                        Saturn_Udec(row + 2,
                                    (saturn_build_frt_acc >> 8) & 0xFFFFu);
                        Saturn_StageText(8, 108, row);
                        row[0] = 'H'; row[1] = '=';
                        Saturn_Udec(row + 2,
                                    (saturn_half_frt_acc >> 8) & 0xFFFFu);
                        Saturn_StageText(8, 128, row);
                        }
                    }
                }
                else
                {
                    open_since = 0u;
                    /* r58: frame completed â€” un-latch net-2 (sticky
                     * net-1, bit 1, survives by design). */
                    saturn_isr_dirty &= ~1u;
                }

                if (!latched && beat_idle >= 120u && frame_idle >= 120u)
                {
                    /* r58: latch net-1 (CYAN now painted on the main
                     * thread); no fill in interrupt context. */
                    latched = 1;
                    saturn_isr_site  = saturn_master_site;
                    saturn_isr_vbl   = saturn_vbl_count;
                    saturn_isr_ist   = *(volatile unsigned int *)
                        0x25FE00A8u;
                    saturn_isr_imask = *(volatile unsigned int *)
                        0x25FE00A4u;
                    saturn_isr_dirty |= 2u;
                }
                else if (!latched && beat_idle >= 30u)
                {
                    extern void Saturn_Udec(char *out, unsigned int v);
                    static char row[12];

                    if ((saturn_vbl_count & 7u) == 0u)   /* r57: slim */
                    {
                        row[0] = 'S'; row[1] = '=';
                        Saturn_Udec(row + 2, saturn_master_site & 0xFFFFu);
                        Saturn_StageText(8, 8, row);
                    }
                }
            }
            else
            {
                /* Net-1 latched (heartbeat stopped): same 7 rows, r57
                 * throttled to every 64th vbl like net-2. */
                if ((saturn_vbl_count & 63u) == 0u)
                {
                extern void Saturn_Udec(char *out, unsigned int v);
                extern void Saturn_Uhex(char *out, unsigned int v);
                extern volatile unsigned int saturn_load_count;
                extern volatile unsigned int saturn_frame_count;
                extern int                   segwalls_count;
                static char row[12];

                row[0] = 'S'; row[1] = '=';
                Saturn_Udec(row + 2, saturn_master_site & 0xFFFFu);
                Saturn_StageText(8, 8, row);
                row[0] = 'V'; row[1] = '=';
                Saturn_Udec(row + 2, saturn_vbl_count & 0xFFFFu);
                Saturn_StageText(8, 28, row);
                row[0] = 'L'; row[1] = '=';
                Saturn_Udec(row + 2, saturn_load_count & 0xFFFFu);
                Saturn_StageText(8, 48, row);
                row[0] = 'T'; row[1] = '=';
                Saturn_Udec(row + 2, saturn_frame_count & 0xFFFFu);
                Saturn_StageText(8, 68, row);
                row[0] = 'E'; row[1] = '=';
                Saturn_Udec(row + 2,
                            (unsigned int)(segwalls_count & 0xFFFFu));
                Saturn_StageText(8, 88, row);
                row[0] = 'A'; row[1] = '=';
                Saturn_Udec(row + 2,
                            (saturn_build_frt_acc >> 8) & 0xFFFFu);
                Saturn_StageText(8, 108, row);
                row[0] = 'H'; row[1] = '=';
                Saturn_Udec(row + 2,
                            (saturn_half_frt_acc >> 8) & 0xFFFFu);
                Saturn_StageText(8, 128, row);
                }
            }
        }
    }

    /* Round-45: the slave reads this counter through the P2 mirror
     * (its OC never sees master writes â€” caches don't snoop). A cached
     * increment alone sat dirty in the master's OC, and the join-poll
     * loop reading the same variable kept that line hot, so the
     * write-back never happened while the slave waited: WRAM held a
     * frozen pre-dispatch value and every mirror-read "did a vblank
     * land" test on the slave stayed false forever (the round-44
     * kind-4 fires at 0xC57A, and the self-bail deadline reading the
     * same frozen mirror). Publish the new count per vbl. */
    *(volatile unsigned int *)
        ((unsigned int)&saturn_vbl_count + 0x20000000u)
        = saturn_vbl_count;
}

/* Draw one 160x200 half of the DOOM frame as a VDP1 sprite. `x` is the
 * CENTER of the half in SGL screen coordinates (origin = screen center,
 * screen spans -160..+160). Inlined into I_FinishUpdate because DOOM's
 * main loop never calls jo_core_run(), so a per-frame callback would
 * never fire. The sample uses jo_sprite_draw3D(id, 0, 0, 500) Ã¢â‚¬â€ same
 * centered-style path. */
static inline void saturn_draw_half(jo_software_renderer_gfx *gfx, int x)
{
    jo_pos3D  pos;
    if (gfx == NULL || gfx->sprite_id < 0)
        return;
    pos.x = x;
    pos.y = 0;
    pos.z = 500;
    jo_sprite_draw(gfx->sprite_id, &pos, true /*centered*/, false /*billboard*/);
}

/* Left half covers screen x -160..0 (center -80), right half 0..+160
 * (center +80). Vertically the 200-tall halves are centered on the
 * 224-tall screen (12 px border top and bottom). */
static inline void saturn_draw_sprite(void)
{
    saturn_draw_half(saturn_gfx_l, -(SATURN_HALF_WIDTH / 2));
    saturn_draw_half(saturn_gfx_r,   SATURN_HALF_WIDTH / 2);
}

void I_InitVideo(void)
{
    /* Allocate the single CPU-side 8bpp framebuffer (320x224 = 70 KB —
     * the full NBG0 display window since the 2026-09-06 HUD-at-bottom
     * layout; the old 320x200 is the art/render space, see
     * DOOM_FBHEIGHT in i_video.h). See the shadow_fb comment above
     * for why there is no second page. */
    /* Relocated from Jo heap to dedicated WRAM-L DRAM partition (saturn_memmap.h).
     * Frees 71.7 KB of fast 32-bit WRAM-H Jo heap for hardware command buffers (cmd_m, cmd_s). */
    shadow_fb  = (byte *)SATURN_WRAM_L_SHADOW_FB_ADDR;
    memset(shadow_fb, 0, FB_W * DOOM_FBHEIGHT);
    I_VideoBuffer  = shadow_fb;
    I_VideoBuffer2 = shadow_fb;   /* alias Ã¢â‚¬â€ no page flip on Saturn */

    /* Jo Engine's software renderer, twice (one per 160x200 half Ã¢â‚¬â€ see
     * the saturn_gfx_l comment above for why the frame is split):
     *   - allocates a 160x200 VDP1 sprite (16bpp, JO_SPRITE_SCREEN)
     *   - allocates a 16bpp color_buffer (160 * 200 * 2 = 64 KB each)
     *   - no z-buffer (DOOM's renderer does its own clipping)
     * On every flush, it DMAs color_buffer to the sprite's VDP1 VRAM.
     * Heap cost: 32 KB shadow fb + 2 x 64 KB color buffers = 164 KB of
     * the 250 KB jo heap Ã¢â‚¬â€ fits.
     * VDP2 direct display: the surfaces are not created at all Ã¢â‚¬â€ the
     * frame is displayed by the NBG0 bitmap path (see the SATURN_VDP2_FB
     * block above), which also frees 128 KB of the jo heap. */
#if !SATURN_VDP2_FB
    saturn_gfx_l = jo_software_renderer_create_ex(SATURN_HALF_WIDTH,
                                                   DOOM_SCREENHEIGHT,
                                                   JO_SPRITE_SCREEN,
                                                   false /* no z-buffer */);
    saturn_gfx_r = jo_software_renderer_create_ex(SATURN_HALF_WIDTH,
                                                   DOOM_SCREENHEIGHT,
                                                   JO_SPRITE_SCREEN,
                                                   false /* no z-buffer */);
    if (saturn_gfx_l == NULL || saturn_gfx_r == NULL)
    {
        I_Error("I_InitVideo: jo_software_renderer_create failed");
    }
#endif

    /* Fill the 16bpp palette with grayscale so we have a sane display
     * before r_main pushes the real DOOM palette via I_SetPalette. */
    {
        int i;
        for (i = 0; i < 256; i++)
        {
            unsigned short c = JO_COLOR_RGB(i, i, i);

            saturn_palette16[i] = c;
            saturn_palette32[i] = ((unsigned int)c << 16) | c;
        }
    }

#if SATURN_VDP2_FB
    /* NBG0 8bpp bitmap takeover (one-time register programming; the
     * screen stays on the NBG1 stage colors until the first frame). */
    saturn_vdp2_init_display();
#endif

    /* Register the VBlank callback (D_DoomLoop uses this to rate-limit
     * gameplay ticks to DOOM's 35 Hz TICRATE). The sprite-draw callback
     * we used to register here was never invoked because DOOM's main
     * loop calls D_DoomLoop directly without jo_core_run(), so the
     * callback list is never processed. Instead we drive the VDP1 frame
     * inline from I_FinishUpdate. */
    jo_core_add_vblank_callback(saturn_vblank_callback);
}

/* DOOM calls this whenever the active palette changes. PLAYPAL stores
 * FULL 8-BIT bytes per channel (the WAD's own entries run up to 255 Ã¢â‚¬â€
 * a 6-bit read is impossible), so pass them straight through:
 * JO_COLOR_RGB already truncates each channel to the 5-bit Saturn
 * range (>>3 & 0x1f) internally.
 *
 * The previous code assumed 6-bit values and re-expanded with
 * (v<<2)|(v>>4) Ã¢â‚¬â€ for every channel >= 64 that wrapped mod 256 and
 * scrambled the color. Almost the whole DOOM palette displayed as
 * cyan/green garbage (index 0x20's dark red 155,51,51 showed as cyan) Ã¢â‚¬â€
 * the uniform-cyan "IN-LOOP" frame. */
/* Shaded CRAM banks for the VDP1 wall quads — see the definition below. */
void Saturn_Cram_ShadedBanks(void);

void I_SetPalette(const byte *palette)
{
    int i;
    if (!palette)
        return;
    for (i = 0; i < 256; i++)
    {
        unsigned short c = JO_COLOR_RGB(palette[i * 3 + 0],
                                        palette[i * 3 + 1],
                                        palette[i * 3 + 2]);

        saturn_palette16[i] = c;
        saturn_palette32[i] = ((unsigned int)c << 16) | c;
    }
#if SATURN_VDP2_FB
    /* VDP2 direct display: the palette IS the display Ã¢â‚¬â€ push the whole
     * bank into CRAM 0..255. 256 halfword writes (~0.1 ms); a write
     * during active display can speckle one frame, which is invisible
     * next to the damage/bonus flash itself. */
    if (saturn_fb_live)
    {
        volatile unsigned short *cram =
            (volatile unsigned short *)SATURN_FB_CRAM;

        for (i = 0; i < 256; i++)
            cram[i] = saturn_palette16[i];
        /* This push already includes the correct index 1. */
        saturn_cram_stage_dirty = 0;
        /* CRAM banks 1..7 serve the VDP1 wall quads (saturn_vdp1.c):
         * each holds the CURRENT palette remapped through COLORMAP row
         * 4k, which is exactly the software paint's cmap[(sh<<8)+texel]
         * index remap, so a quad at bank k displays bit-identical to a
         * software wall shaded at COLORMAP row 4k. Rebuilt here so the
         * damage/bonus palette flashes shade the quads too. */
        Saturn_Cram_ShadedBanks();
    }
#endif
}

/* Shaded CRAM banks for the VDP1 wall quads (2026-09-07). CRAM total is
 * 4 KB = 8 banks of 256; bank 0 stays the raw palette (NBG0 + the HUD
 * glyph channel), banks 1..7 = palette[COLORMAP[4k][i]] for k = 1..7.
 * Nothing else in this project writes CRAM above entry 255 (Jo's
 * palette allocator is never called), and COLORMAP rows 4..28 sit well
 * inside the lump's 32 light rows. Called from I_SetPalette (palette
 * flashes re-shade the banks) and once from Saturn_Vdp1Wall_Init (the
 * WAD is fully loaded there, so COLORMAP is guaranteed present). */
void Saturn_Cram_ShadedBanks(void)
{
    const byte              *cm = Saturn_Colormap();
    volatile unsigned short *cram;

    if (!cm || !saturn_fb_live)
        return;
    cram = (volatile unsigned short *)SATURN_FB_CRAM;
    {
        int k, i;

        for (k = 1; k < 8; k++)
        {
            const byte *row = cm + ((k << 8) << 2);  /* COLORMAP row 4k */

            for (i = 0; i < 256; i++)
                cram[(k << 8) + i] = saturn_palette16[row[i]];
        }
    }
}

/*===== FPS overlay (Phase 0 instrumentation) =====
 *
 * Render-time measurement without any of the broken text paths
 * (jo_printf under SGL is invisible, slPrint has no default font):
 * draw 3x5 digits straight into the 8bpp I_VideoBuffer BEFORE the
 * 8->16 expand, so the HUD rides the normal sprite pipeline.
 *
 * Frame time in vblanks comes for free: I_FinishUpdate runs once per
 * displayed frame and contains slSynch() (waits VBlank), so the
 * saturn_vbl_count delta across frames IS the frame time. One 32-bit
 * divide per 32 frames to convert to fps*10 Ã¢â‚¬â€ everything else is
 * counters.
 */
static unsigned int fps_last_vbl = 0;
static unsigned int fps_acc_vbl  = 0;
static unsigned int fps_frames   = 0;
static unsigned int fps10_show   = 0;   /* fps x 10, updated every 16 frames */

/* 2026-09-07 BSS-STOMP CANARY GRID. The fps window state reads impossible
 * values in-game (acc=0 with frames=9; the display cap forces delta>=2,
 * so acc>=2*frames always) and the old expanded glyph tables (540 B of
 * this same object's BSS) rendered as all-nonzero blocks. Something
 * writes over i_video.o BSS at runtime. fps_guard sits next to the fps
 * statics in the object's BSS; it is patterned at first frame and
 * verified every frame. A broken word latches (a) the index, (b) the
 * WRITER'S DATA (what the stomp wrote), and (c) the execution-site
 * telemetry at detection — printed sticky on the q/p/i rows. */
#define FPS_GUARD_N     64
#define FPS_GUARD_PAT   0xC0DE0000u
static volatile unsigned int fps_guard[FPS_GUARD_N];
static volatile unsigned int fps_guard_hit;   /* 0 = clean; else index+1  */
/* 2026-09-08 V1W reject census outputs (computed in the q-row block,
 * painted by the q / N / G rows — see the census block at fps_num(40)):
 * top-2 ACTIONABLE reject reasons ((id<<8)|count) and the benign total.
 * Counts are CUMULATIVE since boot (the per-frame rj reset is disabled
 * in saturn_segs.c), so any readable frame carries the whole session. */
static int v1w_cens_id1, v1w_cens_c1;
static int v1w_cens_id2, v1w_cens_c2;
static int v1w_cens_ben;
static volatile unsigned int fps_guard_val;   /* what the writer wrote    */
static volatile unsigned int fps_guard_site;  /* saturn_master_site latch */
static volatile unsigned int fps_guard_isr;   /* saturn_isr_site latch    */
static volatile unsigned int fps_guard_vbl;   /* vbl at detection         */
static volatile unsigned int fps_guard_last;  /* fps_last_vbl at detect   */
static unsigned int           fps_prev_vbl;   /* monotonic-vbl check      */
/* Peak-hold mirrors of the bottom bars (rows 160-190 are below the
 * Kronos viewport cut Ã¢â‚¬â€ the digit rows can still carry them). Same
 * ~3%/frame decay: a 1-frame stall stays visible for seconds. */
static unsigned int fps_w_hold = 0;     /* tic catch-up WALL, vbl        */
static unsigned int fps_g_hold = 0;     /* cart-fill drain, vbl          */
static unsigned int fps_j_hold = 0;     /* displayed-frame delta, vbl    */

/* Phase-4 profiler, fed by d_main.c each displayed frame and printed by
 * fps_measure: render vblanks (camera tables + preload + both halves +
 * slave join), finish vblanks (8->16 expand + DMA), tics consumed in
 * the catch-up loop, composites performed, and SYNC CD lump reads
 * (each one is a GFS seek + sector transfer Ã¢â‚¬â€ dozens of ms). */
volatile int saturn_prof_render = 0;
volatile int saturn_prof_finish = 0;
volatile int saturn_prof_tics   = 0;
volatile int saturn_prof_comp   = 0;
volatile int saturn_prof_cd     = 0;

/* Filled on saturn_render.c (render-window internals). */
extern volatile int saturn_prof_setup;
extern volatile int saturn_prof_join;
extern volatile int saturn_prof_slave;
extern volatile int saturn_prof_active;

/* FRT sub-windows (saturn_render.c), TCNT ticks >>8:
 * N = crossing scan, Q = sweep fire, O = walk + paint. */
extern int saturn_frt_fire;
extern int saturn_frt_scan;
extern int saturn_frt_total;
extern int saturn_frt_pre;    /* preload walk (Saturn_TexData sweep)   */
extern volatile int saturn_frt_disp_pre;  /* entry -> slave dispatch, >>8 */
extern volatile int saturn_frt_disp_post; /* join-return -> section end */
extern int saturn_frt_cl;
extern int saturn_frt_tic;
extern int saturn_frt_walk;
extern int saturn_frt_fill;      /* d_main.c: fill drain cost, FRT >>8 */
extern int saturn_prof_fllen;    /* d_main.c: fill entries, worst frame */

/* Cumulative flat-cache fills (saturn_tex.c). K only counts texture
 * composites Ã¢â‚¬â€ this row catches flat misses refiring every frame. */
extern int saturn_tex_flat_miss;

/* Slave-cache experiment (saturn_render.c): CCR & 0xFF Ã¢â‚¬â€ bit 0 set =
 * operand cache ON. Shown for the master at dispatch and the slave at
 * callback entry + after its enable attempt. */
extern volatile int saturn_prof_sccr0;
extern volatile int saturn_prof_sccr1;
extern volatile int saturn_prof_mccr;

/* Per-frame work counters from saturn_render.c (repurposed L/X/Y/Z):
 * L = active-list line visits, X = solves, Y = crossings, Z = spans. */
extern int saturn_cnt_scan;
extern int saturn_cnt_solve;
extern int saturn_cnt_cross;
extern int saturn_cnt_span;
/* Flat-paint census: paint_flat_run calls + total rows painted per
 * frame (rows >> 64000 unique flat pixels = merge/flush overdraw). */
extern int saturn_cnt_fruns;
extern int saturn_cnt_frows;
/* Walk-window decomposition (saturn_render.c): fr_* staging vs
 * paint_wall_span FRT totals, >>8 like the other frt rows. */
extern int saturn_frt_stage;
extern int saturn_frt_paint;
extern int saturn_frt_emit;
extern int saturn_frt_flush;
extern int saturn_frt_merge;
extern int saturn_frt_ploop;
extern int saturn_cnt_fpx;

/* d_main.c frame windows the older rows never timed. */
extern volatile int saturn_prof_fill;
extern volatile int saturn_prof_ticvbl;
extern volatile int saturn_prof_render;   /* D_Display: render only   */
extern volatile int saturn_prof_finish;   /* D_Display: finish only   */

/* Door-trace probe (p_map.c). */
extern int saturn_cnt_usebbox;

/* Prologue (camera tables + camline build), from saturn_render.c. */
extern volatile int saturn_prof_pre;

/* SCU DSP capability probe verdict (main.c, boot stage 3.5): 1 = the
 * DSP loads, starts and DMA-round-trips WRAM-H. Shown on the T row so
 * a single in-game capture carries the verdict (the boot-report text
 * is gone by the time gameplay starts). */
extern int saturn_dsp_ok;
extern int saturn_dsp_ramin;   /* RAM0 == payload (read stride-1 count) */
extern int saturn_dsp_zero;    /* dsp_out == preload (write a=1 moved)  */
extern int saturn_dsp_match;   /* dsp_out == payload (write a=1 chain)  */
extern int saturn_dsp_ppaf;    /* dsp_out2 == preload (write a=2 moved) */
extern int saturn_dsp_str2;    /* dsp_out2 == payload (write a=2 chain) */
extern int saturn_dsp_str4;    /* RAM0 == payload[0] (read stride disc) */
extern int saturn_tex_fill_bytes;  /* cart lump bytes pulled into caches */
extern int saturn_tex_comp_bytes;  /* composite output bytes stamped     */

/* Sub-window profile of the F row: where inside I_FinishUpdate the
 * vblanks go. E = 8->16 palette expand, D = slDMACopy flushes to
 * VDP1 VRAM, V = saturn_draw_sprite + slSynch frame commit. */
static volatile int saturn_prof_exp = 0;
static volatile int saturn_prof_dma = 0;
static volatile int saturn_prof_syn = 0;

/* Micro (FRT) versions of the same windows. The vbl counters above
 * round to 1/60 s, which hides whether the DMA leg is a real ~1-vbl
 * transfer or a fast DMA plus rounding Ã¢â‚¬â€ that distinction decides
 * whether overlapping the DMA into the slSynch vblank wait is worth
 * pursuing. CALIBRATED (2026-09-02, probe 'c' shot #80): the FRT ticks
 * 21536 times per vblank Ã¢â‚¬â€ ~1.29 MHz, NOT the 3.6 MHz i_wad.c assumed.
 * Printed >>2, so 1 vbl reads ~= 5384 and 1 unit = 3.10 us. 16-bit
 * wraps at 65536 ticks = 50.7 ms; unsigned short subtraction handles
 * even a wrap correctly. */
static inline unsigned short Saturn_Video_ReadFRT(void)
{
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned char h1 = frt[0];
    unsigned char l  = frt[1];
    unsigned char h2 = frt[0];
    if (__builtin_expect(h1 != h2, 0))
        l = frt[1];
    return (unsigned short)(((unsigned int)h2 << 8) | (unsigned int)l);
}
#define SAT_FRT()  Saturn_Video_ReadFRT()
static volatile unsigned short saturn_prof_exp_us = 0;
static volatile unsigned short saturn_prof_dma_us = 0;
static volatile unsigned short saturn_prof_syn_us = 0;
/* 2026-09-03 photo #147: the slSynch() wait ALONE inside the syn
 * window (raw 16-bit FRT ticks; the wait measured 46.5 ms = 2.8 vblank
 * periods, which a plain vblank wait cannot cost). Displayed as F. */
static volatile int saturn_frt_syn_syn = 0;
/* Gap window 'u': everything in I_FinishUpdate NOT covered by z/x/y
 * above Ã¢â‚¬â€ entry + fps_measure (HUD draw) + the fill-progress strip.
 * The first micro build showed z+x+y ~= 4 ms while A spanned 2-3 vbl
 * (~33-50 ms), leaving ~29 ms unaccounted. The FRT is a hardware
 * counter and cannot miss wall time, so the gap must sit in this
 * stretch (or in an ISR host that runs inside it). */
static volatile unsigned short saturn_prof_hud_us = 0;
/* Whole-finish probe 'i' (2026-09-02; replaces the 'c' calibration
 * probe, whose answer is in: 21536 FRT ticks per vblank Ã¢â‚¬â€ FRT ~1.29
 * MHz Ã¢â‚¬â€ and the w-row/J-bar agreement confirms it independently).
 * 'i' times ALL of I_FinishUpdate (entry to exit) by FRT, printed >>2.
 * This closes the A-row case: with FRT honest, u+z+x+y summed ~14.5 ms
 * while A (vbl-rounded) read 2 vbl. If 'i' reads ~4700 (~14.5 ms) the
 * A=2 was pure vbl-boundary rounding (window starts mid-vblank, slSynch
 * ends at one) and the finish window is fully accounted for. */
static volatile unsigned short saturn_prof_fin_us = 0;

/* 3x5 digit glyphs, one byte per row, bit 2 = left column. */
static const unsigned char fps_font[10][5] = {
    { 7, 5, 5, 5, 7 },   /* 0 */
    { 2, 6, 2, 2, 7 },   /* 1 */
    { 7, 1, 7, 4, 7 },   /* 2 */
    { 7, 1, 7, 1, 7 },   /* 3 */
    { 5, 5, 7, 1, 1 },   /* 4 */
    { 7, 4, 7, 1, 7 },   /* 5 */
    { 7, 4, 7, 5, 7 },   /* 6 */
    { 7, 1, 1, 1, 1 },   /* 7 */
    { 7, 5, 7, 5, 7 },   /* 8 */
    { 7, 5, 7, 1, 7 },   /* 9 */
};

/* 3x5 letter glyphs for row labels (bit 2 = left column). Only the
 * letters the profile rows use are faithful; the rest never render. */
static const unsigned char fps_letters[26][5] = {
    { 2, 5, 7, 5, 5 },   /* A */
    { 6, 5, 6, 5, 6 },   /* B */
    { 7, 4, 4, 4, 7 },   /* C */
    { 6, 5, 5, 5, 6 },   /* D */
    { 7, 4, 6, 4, 7 },   /* E */
    { 7, 4, 6, 4, 4 },   /* F */
    { 7, 4, 5, 5, 7 },   /* G */
    { 5, 5, 7, 5, 5 },   /* H */
    { 7, 2, 2, 2, 7 },   /* I */
    { 1, 1, 1, 5, 7 },   /* J */
    { 5, 5, 6, 5, 5 },   /* K */
    { 4, 4, 4, 4, 7 },   /* L */
    { 5, 7, 7, 5, 5 },   /* M */
    { 5, 7, 7, 7, 5 },   /* N */
    { 7, 5, 5, 5, 7 },   /* O */
    { 6, 5, 6, 4, 4 },   /* P */
    { 7, 5, 5, 7, 1 },   /* Q */
    { 6, 5, 6, 5, 5 },   /* R */
    { 7, 4, 7, 1, 7 },   /* S */
    { 7, 2, 2, 2, 2 },   /* T */
    { 5, 5, 5, 5, 7 },   /* U */
    { 5, 5, 5, 5, 2 },   /* V */
    { 5, 5, 7, 7, 5 },   /* W */
    { 5, 5, 2, 5, 5 },   /* X */
    { 5, 5, 2, 2, 2 },   /* Y */
    { 7, 1, 2, 4, 7 },   /* Z */
};

/* Pre-expanded glyphs (2026-09-02): the u row showed the HUD draw at
 * ~2.5 ms/frame Ã¢â‚¬â€ the plot loops re-tested every glyph bit per pixel
 * (15 tests + a multiply-indexed store each). These tables hold the
 * same 3x5 glyphs as 15 flat bytes (row-major, 0 = transparent), built
 * once from the packed tables above on first use; the plot loops
 * become 3 tests + 3 in-range stores per row. 540 B of BSS. */
/* 2026-09-07: the expanded tables are COMPILE-TIME const (.rodata).
 * The runtime-expanded BSS copies rendered as SOLID 3x5 blocks in the
 * PERF_ROWS build — every byte read nonzero, i.e. something stomps
 * this BSS region with 0xFF; rodata can't be stomped, and the boot-time
 * expansion cost disappears with it. Values = fps_font/fps_letters
 * unpacked (bit 2 = left column). */
static const byte fps_exp_digits[10][15] = {
    { 1,1,1, 1,0,1, 1,0,1, 1,0,1, 1,1,1 },   /* 0 */
    { 0,1,0, 1,1,0, 0,1,0, 0,1,0, 1,1,1 },   /* 1 */
    { 1,1,1, 0,0,1, 1,1,1, 1,0,0, 1,1,1 },   /* 2 */
    { 1,1,1, 0,0,1, 1,1,1, 0,0,1, 1,1,1 },   /* 3 */
    { 1,0,1, 1,0,1, 1,1,1, 0,0,1, 0,0,1 },   /* 4 */
    { 1,1,1, 1,0,0, 1,1,1, 0,0,1, 1,1,1 },   /* 5 */
    { 1,1,1, 1,0,0, 1,1,1, 1,0,1, 1,1,1 },   /* 6 */
    { 1,1,1, 0,0,1, 0,0,1, 0,0,1, 0,0,1 },   /* 7 */
    { 1,1,1, 1,0,1, 1,1,1, 1,0,1, 1,1,1 },   /* 8 */
    { 1,1,1, 1,0,1, 1,1,1, 0,0,1, 1,1,1 },   /* 9 */
};
static const byte fps_exp_letters[26][15] = {
    { 0,1,0, 1,0,1, 1,1,1, 1,0,1, 1,0,1 },   /* A */
    { 1,1,0, 1,0,1, 1,1,0, 1,0,1, 1,1,0 },   /* B */
    { 1,1,1, 1,0,0, 1,0,0, 1,0,0, 1,1,1 },   /* C */
    { 1,1,0, 1,0,1, 1,0,1, 1,0,1, 1,1,0 },   /* D */
    { 1,1,1, 1,0,0, 1,1,0, 1,0,0, 1,1,1 },   /* E */
    { 1,1,1, 1,0,0, 1,1,0, 1,0,0, 1,0,0 },   /* F */
    { 1,1,1, 1,0,0, 1,0,1, 1,0,1, 1,1,1 },   /* G */
    { 1,0,1, 1,0,1, 1,1,1, 1,0,1, 1,0,1 },   /* H */
    { 1,1,1, 0,1,0, 0,1,0, 0,1,0, 1,1,1 },   /* I */
    { 0,0,1, 0,0,1, 0,0,1, 1,0,1, 1,1,1 },   /* J */
    { 1,0,1, 1,0,1, 1,1,0, 1,0,1, 1,0,1 },   /* K */
    { 1,0,0, 1,0,0, 1,0,0, 1,0,0, 1,1,1 },   /* L */
    { 1,0,1, 1,1,1, 1,1,1, 1,0,1, 1,0,1 },   /* M */
    { 1,0,1, 1,1,1, 1,1,1, 1,1,1, 1,0,1 },   /* N */
    { 1,1,1, 1,0,1, 1,0,1, 1,0,1, 1,1,1 },   /* O */
    { 1,1,0, 1,0,1, 1,1,0, 1,0,0, 1,0,0 },   /* P */
    { 1,1,1, 1,0,1, 1,0,1, 1,1,1, 0,0,1 },   /* Q */
    { 1,1,0, 1,0,1, 1,1,0, 1,0,1, 1,0,1 },   /* R */
    { 1,1,1, 1,0,0, 1,1,1, 0,0,1, 1,1,1 },   /* S */
    { 1,1,1, 0,1,0, 0,1,0, 0,1,0, 0,1,0 },   /* T */
    { 1,0,1, 1,0,1, 1,0,1, 1,0,1, 1,1,1 },   /* U */
    { 1,0,1, 1,0,1, 1,0,1, 1,0,1, 0,1,0 },   /* V */
    { 1,0,1, 1,0,1, 1,1,1, 1,1,1, 1,0,1 },   /* W */
    { 1,0,1, 1,0,1, 0,1,0, 1,0,1, 1,0,1 },   /* X */
    { 1,0,1, 1,0,1, 0,1,0, 0,1,0, 0,1,0 },   /* Y */
    { 1,1,1, 0,0,1, 0,1,0, 1,0,0, 1,1,1 },   /* Z */
};

static void fps_expand_hud(void)
{
    /* Compile-time tables since 2026-09-07 (see block comment). */
}

/* Plot one letter at (x, y). */
static void fps_plot_char(int x, int y, char c)
{
    const byte *g;
    byte       *fb;
    int         ry;

    if (c < 'A' || c > 'Z')
        return;
    g  = fps_exp_letters[c - 'A'];
    fb = I_VideoBuffer + y * FB_W + x;
    for (ry = 0; ry < 5; ry++)
    {
        if (g[0]) fb[0] = 0xFC;
        if (g[1]) fb[1] = 0xFC;
        if (g[2]) fb[2] = 0xFC;
        g  += 3;
        fb += FB_W;
    }
}

/* HUD base x. Kronos's GL viewport corrupts on resize: the frame grows
 * past the window and the visible game-x range shrinks (fullscreen
 * showed game x 0..238, windowed showed even less). 2026-09-01: park
 * the text column at x=96 Ã¢â‚¬â€ inside the visible range in both the
 * healthy and the corrupted viewport. Digits need 24 px (label + 5),
 * bars cap at 10 cells. Half-width: fb x is 2:1-stretched at display,
 * so x=48 in the fb lands at the same on-screen spot as x=96 did. */
#define HUD_X0 48

/* fps readout home (2026-09-10): the user's TV crops the top ~8 display
 * lines, so the row-2 readout — and with it the real-HW fps number —
 * never reached the screen. New home = the status-bar metal between the
 * HEALTH digits (art ends ~x=86) and the ARMS box (art starts ~x=104),
 * fb rows 184..189: fully visible, clear of the census column (its
 * erase extent ends at x=87, n/H rows) and of the ARMS art. */
#define HUD_FPS_ROW 184
#define HUD_FPS_X   90

/* Plot one glyph: 3 wide + 1 spacing, 5 tall. */
static void fps_plot(int x, int y, unsigned int digit, byte color)
{
    const byte *g   = fps_exp_digits[digit % 10];
    byte       *fb  = I_VideoBuffer + y * FB_W + x;
    int         ry;

    for (ry = 0; ry < 5; ry++)
    {
        if (g[0]) fb[0] = color;
        if (g[1]) fb[1] = color;
        if (g[2]) fb[2] = color;
        g  += 3;
        fb += FB_W;
    }
}

/* Print "<letter> <value>" for the row at y. The letter kills row
 * misidentification in screenshots; the value keeps at least one digit.
 * Rows alternate between x=4 and x=10 by 8-line band so adjacent values
 * never sit at the same x. */
static void fps_num(int y, char label, unsigned int v);

/* BISECT round 9: export a HUD-number writer for the SLAVE Ã¢â‚¬â€ it draws
 * its own probe rows into the framebuffer (slave fb writes are proven
 * to reach the display; cross-core COUNTER reads are not reliable).
 * The master's 'J' row (56) was retired to give the slave a slot. */
void Saturn_HudNum(int y, char label, unsigned int v)
{
    fps_num(y, label, v);
}

static void fps_num(int y, char label, unsigned int v)
{
    int          x = HUD_X0 + ((y & 8) ? 6 : 0);
    unsigned int d;
    int          started = 0;
    int          ex, ry;

    /* Ghost-proofing (2026-09-03 photos #144/#146): fps_num never
     * erased, so every TV photo was a UNION of the first-frame value
     * and the current one â€” the boot frames' BootMsg/StageColor
     * slSynch stalls made early windows huge, and rows that shrank
     * kept showing their stale digits forever. Wipe the cell extent
     * before drawing, like fps_bar already does. 9 rows (y-1..y+7) so
     * the next band's glyphs (y+8) are never clipped; x extent covers
     * the band x-shift and a 5-digit value. */
    for (ex = HUD_X0 - 2; ex < HUD_X0 + 40; ex++)
        for (ry = -1; ry < 8; ry++)
            I_VideoBuffer[(y + ry) * FB_W + ex] = 0x00;

    fps_plot_char(x, y, label);
    x += 4;

    for (d = 10000u; d >= 1; d /= 10)
    {
        unsigned int dig = v / d % 10;

        if (dig || started || d == 1)
        {
            fps_plot(x, y, dig, 0xFC);
            x += 4;
            started = 1;
        }
    }
}

#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 3) && defined(SATURN_PERF_ROWS)
/* 2026-09-08 DIVLUT census (STEP 3 of the one-site-at-a-time ladder):
 * snapshot+reset one per-frame counter for the perf rows. The render
 * has finished by the time the rows run and everything is master-side,
 * so the read+zero cannot race. fps_num is 5 digits wide — cap the
 * display. Counters live in saturn_render.c (LightRow/light_div) and
 * d32xr_stubs.c (FixedDiv). */
static unsigned int Saturn_Divlut_Row(volatile unsigned int *cnt)
{
    unsigned int v = *cnt;

    *cnt = 0;
    return v > 99999u ? 99999u : v;
}
extern volatile unsigned int sat_divlut_lhits;
extern volatile unsigned int sat_divlut_dfallback;
extern volatile unsigned int sat_fd_total;
extern volatile unsigned int sat_fd_env;
#endif

/* Plot "<letter> " + a LOG-SCALE bar for the FRT windows (N/Q/O/T).
 * Cell n = value has bit n set Ã¢â€¡â€™ cell k means the window is Ã¢â€°Â¥ 2^kÃ‚Â·256
 * FRT ticks (k=0..16). One visible cell per binary order of magnitude
 * makes any magnitude readable Ã¢â‚¬â€ and linear-vs-log is never ambiguous
 * because the digit rows still carry the exact >>8 values. The erase
 * spans one row beyond the band on BOTH sides so scene pixels can't
 * peek through between bands (the thin-line artifact). */
static void fps_bar(int y, char label, unsigned int v)
{
    int n  = 0;
    int x, ry;

    while ((v >> n) != 0 && n < 10)   /* 10 cells max: 60+80 <= 160 */
        n++;
    /* Erase only the extent a bar can occupy (label + 10 cells end at
     * HUD_X0+12+80 = 140), not the whole fb width Ã¢â‚¬â€ the full-width
     * wipe was ~40% of the HUD draw cost (u row). */
    for (x = HUD_X0 - 2; x < HUD_X0 + 12 + 80; x++)
        for (ry = -1; ry < 9; ry++)
            I_VideoBuffer[(y + ry) * FB_W + x] = 0x00;
    fps_plot_char(HUD_X0, y, label);
    for (x = 0; x < n; x++)
    {
        byte *fb = I_VideoBuffer + y * FB_W + HUD_X0 + 12 + x * 8;

        for (ry = 0; ry < 6; ry++)
        {
            fb[0] = fb[1] = fb[2] = fb[3] = 0xFC;
            fb[4] = fb[5] = fb[6] = 0xFC;
            fb[7] = 0x00;                      /* gap between cells */
            fb += FB_W;
        }
    }
}

static void fps_measure(void)
{
    unsigned int now   = saturn_vbl_count;
    unsigned int delta = now - fps_last_vbl;

    /* BSS-stomp canary: pattern on the first frame, verify every frame.
     * Also re-check the fps window's own invariant (acc >= 2*frames by
     * the display cap) — a violation latches the same way. */
    {
        static int guard_armed = 0;
        int        i, bad = -1;

        if (!guard_armed)
        {
            for (i = 0; i < FPS_GUARD_N; i++)
                fps_guard[i] = FPS_GUARD_PAT + (unsigned int)i;
            guard_armed = 1;
        }
        else
        {
            for (i = 0; i < FPS_GUARD_N; i++)
                if (fps_guard[i] != FPS_GUARD_PAT + (unsigned int)i)
                {
                    bad = i;
                    break;
                }
            if (bad < 0 && now < fps_prev_vbl)
                bad = FPS_GUARD_N + 1;   /* vbl_count went BACKWARDS */
            if (bad < 0 && (fps_acc_vbl > 320u || fps_frames > 20u
                            || (fps_frames > 0
                                && fps_acc_vbl < 2u * fps_frames)))
                bad = FPS_GUARD_N;   /* fps state itself broke */
            if (bad >= 0 && fps_guard_hit == 0)
            {
                extern volatile unsigned int saturn_master_site;
                extern volatile unsigned int saturn_isr_site;

                fps_guard_hit  = (unsigned int)(bad + 1);
                fps_guard_val  = (bad < FPS_GUARD_N)
                                 ? fps_guard[bad]
                                 : ((bad == FPS_GUARD_N) ? fps_acc_vbl
                                                         : now);
                fps_guard_site = saturn_master_site;
                fps_guard_isr  = saturn_isr_site;
                fps_guard_vbl  = saturn_vbl_count;
                fps_guard_last = fps_last_vbl;
            }
            fps_prev_vbl = now;
        }
    }

    fps_expand_hud();        /* one-time glyph expansion (u-row cost) */
    fps_last_vbl = now;
    if (delta > 60)          /* GFS stall or first call Ã¢â‚¬â€ do not skew the average */
        delta = 60;
    fps_acc_vbl += delta;
    if (++fps_frames >= 16 || fps_acc_vbl >= 240)
    /* 16 frames OR 4 seconds: at very low fps the frame-count window
     * alone took minutes to refresh and froze a stale value on the
     * top row Ã¢â‚¬â€ the time cap keeps it honest on both sides. The old
     * average divided by a CONSTANT 16 even when the 240-vblank cap
     * fired after only ~4 slow frames, so every slow build displayed
     * a phantom "4.0" Ã¢â‚¬â€ divide by the frames actually accumulated. */
    {
        unsigned int avg = (fps_acc_vbl + (fps_frames >> 1)) / fps_frames;

        fps10_show = avg ? (600 / avg) : 0;
        fps_acc_vbl = 0;
        fps_frames = 0;
    }
    /* Draw every frame (the game scene repaints the pixels beneath).
     * Layout: "[fps10]." with the decimal point as a lone pixel Ã¢â‚¬â€ e.g.
     * 243 renders as "24.3", 87 as "8.7" (tenths as the last digit). */
    /* Diagnostic HUD readout: gated behind SATURN_PERF_ROWS like the
     * probe rows below - in the clean build this un-gated block painted
     * magenta digits over the sky at fb rows 2..7 every frame. */
#ifdef SATURN_PERF_ROWS
    if (fps10_show)
    {
        unsigned int v    = fps10_show;
        int          x    = HUD_FPS_X;
        const int    y    = HUD_FPS_ROW;
        int          ex, ry;

        /* Ghost-proofing: unlike the old row-2 spot (3D view repainted
         * beneath every frame), the status bar art does NOT repaint
         * under us every frame — wipe the cell extent first, same as
         * fps_num. Rows 183..190, x 88..103: clear of the census
         * n/H rows (erase x<=87) and of the ARMS box (x>=104). */
        for (ex = HUD_FPS_X - 2; ex < HUD_FPS_X + 14; ex++)
            for (ry = -1; ry < 7; ry++)
                I_VideoBuffer[(y + ry) * FB_W + ex] = 0x00;

        if (v >= 100)
        {
            fps_plot(x, y, v / 100, 0xFC);
            x += 4;
        }
        fps_plot(x, y, (v / 10) % 10, 0xFC);
        /* 2026-09-10: the decimal point was ONE pixel at row 7 —
         * invisible in gameplay screenshots, so "5.4" read as "54" and
         * the whole P5 census narrative mis-scaled by 10x (pose #1's
         * real frame rate is 5.4 fps = 11 vbl/frame, not 54). 2x2
         * baseline dot, unmissable. The MEASUREMENT was always
         * correct — only the display lied. Dot rides the glyph rows
         * (y+4/y+5), same relative geometry as before. */
        I_VideoBuffer[(y + 4) * FB_W + x + 3] = 0xFC;
        I_VideoBuffer[(y + 4) * FB_W + x + 4] = 0xFC;
        I_VideoBuffer[(y + 5) * FB_W + x + 3] = 0xFC;
        I_VideoBuffer[(y + 5) * FB_W + x + 4] = 0xFC;
        fps_plot(x + 4, y, v % 10, 0xFC);
    }
#endif
    /* Profile rows (top-left, 8 px apart):
     *   y= 2  fps
     *   y= 8  R  render vblanks last frame (tables+preload+both halves+join)
     *   y=16  F  finish vblanks (8->16 expand + VDP1 DMA)
     *   y=24  T  tics consumed by the catch-up loop last frame
     *   y=32  K  composites last frame
     *   y=40  C  SYNC CD lump reads last frame (seeks!)
     *   y=48  S  of R: setup vblanks (preload walk + camera tables)
     *   y=56  J  of R: dispatch..join (master half + slave wait)
     *   y=64  L  slave's left-half vblanks (measured on the slave)
     *   y=72  A  active-line list length, both cores summed
     *   y=80  M  flat-cache fills since boot (cumulative)
     *   y=88  X  master CCR & 0xFF (bit0 = operand cache on)
     *   y=96  Y  slave CCR & 0xFF at callback entry
     *   y=104 Z  slave CCR & 0xFF after enable attempt
     *   y=112 G  background cart-fill drain, vblanks
     *   y=120 W  tic catch-up loop WALL duration, vblanks
     *   y=128 P  render prologue (entryÃ¢â€ â€™preload walk), vblanks
     *   y=136 N  crossing scan, FRT ticks >>8 (cycle-resolution)
     *   y=144 Q  sweep fire chains, FRT ticks >>8
     *   y=152 H  slave NOP round trip, FRT ticks >>4
     *   y=160/168/176  N/Q/O bar rows (1 cell = 256 FRT ticks)    */
    /* R row ceded to the RENDER STAGE SPLIT (2026-09-02, join probe
     * banked: healthy ~1): T = the whole per-frame column loop by FRT
     * (saturn_frt_total, >>8; 1 unit = 256 FRT ticks = 0.198 ms).
     * The stage rows make the traversal/flat/wall split readable on
     * every shot Ã¢â‚¬â€ the NULLWALL bisect numbers it backs up:
     * D_A 28.7 = 17.2 (traversal+flats) + 0.9 (wall math) + 10.6 (wall
     * px) indoor. T should land near the traversal+flats+wall total
     * (both cores' accumulations); glue = D - T - join - prologue. */
    {
        extern int saturn_frt_total;

        /* Round 28k (2026-09-03): ALL probe rows except the fps display
         * RETIRED Ã¢â‚¬â€ the magenta glyphs printed straight over the game
         * (mid-screen rows 88-152 over wall faces, rows 164-196 over
         * sprites) and the user reported them three times as texture
         * corruption. Their missions are banked (27k/28k, movement
         * chain, DIVU, DSP). Re-enable individually for the perf pass.
        fps_num(8,  'T', (unsigned int)saturn_frt_total); */
        (void)saturn_frt_total;
    }
    /* DSP probe verdict is BANKED (Kronos v2.1.5: D=1, T=64, K=64 Ã¢â‚¬â€ see
     * saturn_dsp.c); the row is ceded to the frame-budget split
     * (2026-09-01 "warps forward" hunt: O=2, J bar 32-63 vbl):
     * D = RENDER window, FRT-honest (>>2; 1 vbl reads ~5384) Ã¢â‚¬â€ the
     * 32-bit span is reconstructed in d_main.c from the 16-bit FRT
     * plus the calibrated 21536 ticks/vblank. Last-frame value. */
    {
        extern volatile unsigned int saturn_prof_render_frt;
        (void)saturn_prof_render_frt;
    }
    /* MOVEMENT DIAGNOSTIC build (2026-09-01 "can't move / can't open
     * doors"): T/W/G/K/J ceded from the phase-2c DMA counters (verdict
     * already banked: D=1, T=64, K=64 Ã¢â‚¬â€ see saturn_dsp.c) to the
     * input->sim->position chain, one row per link:
     *   W = derived ticbuttons      (0 idle; UP = 4, USE = 32)
     *   G = gametic                 (must advance even while standing still)
     *   K = player x, map units     (offset +32768 so negatives print)
     *   J = player y, map units     (same offset)
     *   T = player z, map units     (2026-09-01: rawpad ceded Ã¢â‚¬â€ the raw
     *       chain was proven by the n latch (28671); T now shows the
     *       player's ELEVATION so step-up can be watched live: pushing
     *       forward up the exit-platform steps must raise T in 8/16/24
     *       unit jumps. T frozen at the dirt-floor height while W is
     *       held = the step climb is what's blocked.) */
    {
        extern int       gametic;   /* doomstat.c; players/consoleplayer come
                                     from doomdef.h (VINT / player_t*)   */
        extern volatile int saturn_blk_total;
        unsigned short   tics   = 0;
        unsigned int     px = 0, py = 0, pz = 0;

        if (players && consoleplayer >= 0 && consoleplayer < MAXPLAYERS
            && players[consoleplayer].mo)
        {
            tics = players[consoleplayer].ticbuttons;
            /* +32768 units so the negative spawn y prints as a stable
             * positive; deltas are what matter, not absolutes. */
            px = (unsigned)((players[consoleplayer].mo->x >> FRACBITS) + 32768);
            py = (unsigned)((players[consoleplayer].mo->y >> FRACBITS) + 32768);
            pz = (unsigned)(players[consoleplayer].mo->z >> FRACBITS);
        }
        /* T row ceded to the CPU-availability probe this diagnostic
         * build (pz moved to J's row family, see 'b' below):
        fps_num(24, 'T', pz); */
#ifdef SATURN_SEG_SHOW
        /* Round-22 build: the round-20 fallback census is gone (its
         * counters were SEG_SHOW-only in saturn_render.c, which now
         * compiles production painters). G shows gametic. Row 40
         * ceded to the slave's Saturn_HudNum probe (round 9). */
        /* fps_num(40, 'G', (unsigned)gametic); */
#endif
        /* W row ceded to the whole-finish FRT probe this build:
        fps_num(32, 'W', tics); */
        {
            extern volatile unsigned int saturn_prof_finish_frt;
            /* i = ALL of I_FinishUpdate by FRT (>>2): expand + VDP1 DMA
             * + slSynch. The old fin_us micro probe read a stuck 3 Ã¢â‚¬â€
             * this is the honest FRT span stamped in d_main.c. */
#ifdef SATURN_SEG_SHOW
            /* Round-23 probe: open-band-fill wild-owner census. A
             * nonzero count = columns whose final-fill lastsec was
             * out of range (folded back to the view sector). Healthy
             * = 0. (The COLORMAP identity probe read 0 in rounds 21+;
             * that suspect is closed.) */
            extern volatile int saturn_ls_bad;
            fps_num(32, 'i', (unsigned int)saturn_ls_bad);
#else
            /* BISECT round 9: 'i' row = slave's final clip at band col
             * 28 ((ce<<8)|fl; fl<=ce = the band was clip-closed). */
            extern volatile int saturn_dbg_s_clip;
            (void)saturn_dbg_s_clip;
#endif
        }
        /* G row ceded to the ROUND-20 fallback census this SEG_SHOW
         * build (gametic banked; see the flags decode at the w comment
         * above Ã¢â‚¬â€ the block prints here via that 'G' fps_num). */
        /* K row ceded to the SH-2 hardware-divide probe (2026-09-01):
         * V=11111 = the DIVU at 0xFFFFFF00 computed 100/7=14 at boot Ã¢â‚¬â€
         * our FixedDivs (~250-cycle libgcc) can become ~15-cycle DIVU
         * writes. V=0 = absent or garbage. */
        /* V row ceded to the HITCH LATCH (2026-09-02): the divu probe is
         * banked (V=11111 = the SH-2 DIVU alive). h = total vbl of the
         * LAST >=10-vbl frame Ã¢â‚¬â€ w's peak-hold caught ~63-vbl 1-second
         * hitches in steady play, but the per-frame rows are overwritten
         * before a screenshot can catch one; these latches only update
         * on the NEXT hitch. */
        {
            extern volatile int saturn_frt_slavehalf;
            /* Round 25 segwall census (printed on the 'u' row, below):
             * the BSP walk's segwalls[] quota state for this frame. */
            extern int segwalls_count;
            extern int saturn_seg_caphits;

            /* h row ceded (2026-09-02) to the SLAVE HALF's own
             * render_half body by FRT (>>8; the slave's FRT Ã¢â‚¬â€ if dead
             * under SGL this reads a stuck ~255). T is the MASTER half
             * only: n vs T says whether the dual-core split is
             * balanced or the wall clock is slave-bound. */
            /* Row 48 ceded to the slave's Saturn_HudNum probe (round 9). */
            /* fps_num(48, 'n', (unsigned int)saturn_frt_slavehalf); */
        }
        /* fps_num(56, 'J', py & 0xFFFFu); Ã¢â‚¬â€ row 56 ceded to the slave's
         * own Saturn_HudNum probe (BISECT round 9) */
        /* Sticky-latch era over (2026-09-01): the input chain was proven
         * (n=28671 / r=36 latches, 21:37 capture) and the pad rows are
         * repurposed Ã¢â‚¬â€ n = the SLAVE half's own vblanks (measured on the
         * slave, uncached vbl counter). Healthy ~8-9; ~20+ = sick half. */
        /* n/r rows ceded back to the seg stage split 2026-09-02: the
         * slave-vblank and blockage probes were ANSWERED long ago, but
         * the prints were still live Ã¢â‚¬â€ and fps_plot does not erase, so
         * two fps_num calls at the same y (n then S, r then E) painted
         * the UNION of both values' glyphs. Differing digits at the same
         * cell turned into solid blocks ('1' u '0' and '1' u '8' are the
         * only fully-solid unions), which is exactly the Ã¢â€“Â  cells seen in
         * the stepping-kernel screenshots' S/E rows. S/E are exclusive
         * again.
        fps_num(72, 'n', (unsigned int)saturn_prof_slave);
        fps_num(80, 'r', (unsigned int)saturn_blk_total); */
    }
    /* e/m rows ceded to the Y/D remap this diagnostic build:
    fps_num(64, 'e', (unsigned int)saturn_prof_exp);
    fps_num(72, 'm', (unsigned int)saturn_prof_dma); */
    /* M/X/K rows ceded to the H/L/R remap this diagnostic build:
    fps_num(80, 'M', (unsigned int)saturn_tex_flat_miss);
    fps_num(88, 'X', (unsigned int)saturn_prof_fllen);
    fps_num(96, 'K', (unsigned int)saturn_prof_comp); */
    /* RENDER SUB-WINDOW rows (the render bisect, 2026-09-01): the
     * edition-4 probe rows (s/E/N) went static (D=FFFF, match=0) once
     * the capture concluded, and the 78-vblank frame needed the FRT
     * sub-windows the column loop already accumulates. All >>8.
     *   A = active-line list length, both cores summed
     *   I = sweep fire chains   Q = crossing scan
     *   O = solve+staging+paint Y = whole column loop (I+Q+O+overhead)
     *   D = crossings walked per frame  H = solves started (candidates
     *       that passed the straddle test; each paid 2 FixedDivs)
     * I+Q+O Ã¢â€°Ë† Y Ã¢â€°Ë† J is the healthy state; a big gap means per-column
     * overhead outside the stamped windows. */
    /* A row ceded to the W remap this diagnostic build:
    fps_num(104,'A', (unsigned int)saturn_prof_active); */
    /* I row ceded to the door-visit probe this diagnostic build:
    fps_num(112,'I', (unsigned int)saturn_frt_fire); */
    /* Q row ceded to the door-ceiling live readout this build:
    fps_num(120,'Q', (unsigned int)saturn_frt_scan); */
    /* O row: MEASURED FPS Ã¢â‚¬â€ displayed frames per 60 vblanks (this
     * function runs once per displayed frame; the vblank count is the
     * hardware clock, so this reads true fps regardless of emulation
     * throttling). The 133 ms blocking calibration spin that used to
     * sit here is GONE (answer banked: 21536 FRT ticks/vbl) Ã¢â‚¬â€ it was
     * costing ~8 vbl every 16th frame and dragging the fps rows down. */
    {
        static int fps_frames, fps_last, fps_vbl0;
        extern volatile unsigned int saturn_vbl_count;

        fps_frames++;
        if ((int)(saturn_vbl_count - fps_vbl0) >= 60)
        {
            fps_last  = fps_frames;
            fps_frames = 0;
            fps_vbl0  = (int)saturn_vbl_count;
        }
        /* fps_num(128,'O', (unsigned int)fps_last); Ã¢â‚¬â€ 28k retired */
        (void)fps_last;
    }
    /* Y row ceded to the door-intercept probe this diagnostic build:
     * lines that PASSED the use-trace intercept on the LAST B tap
    fps_num(136,'Y', (unsigned int)saturn_frt_total); */
    {
        extern int saturn_cnt_usehit;
        extern volatile int saturn_blk_reason;
        extern volatile int saturn_blk_floorz;
        /* I row ceded (use-visit chain proven: I>0/Y>0/L>0, 2026-09-01)
         * to the BLOCKAGE reason: <reason>*1000 + <floorz map units> of
         * the LAST refused player move.
         *   10xxx = blocked by a solid thing (e.g. an unlit barrel)
         *   11xxx = blocked by a LINE (one-sided or ML_BLOCKING)
         *    2xxx = doesn't fit (ceiling-floor < player height)
         *    3xxx = must lower itself to fit (low overhang)
         *    4xxx = STEP TOO BIG (floorz - z > 24 at height xxx)
         * 11128 = "the move into floor-128 was refused by a line". */
        /* I/Y rows ceded to the DIFF PROBE block below (2026-09-02). */
    }
    /* 2026-09-01: the Kronos viewport bug cuts the frame bottom, hiding
     * rows >=136. Diagnostic build: the staging split rides the visible
     * band Ã¢â‚¬â€ E = fr_emit_flat total, F = fr_flush_for_wall total,
     * M = fr_merge_column total, P = paint_wall_span total (all FRT
     * >>8; stage = E+F+M(+pending), walk - stage - P = glue). */
    /* E row ceded to the HITCH LATCH (slave-CCR probe banked: bit0=1,
     * the slave's operand cache is ON in this build): H = the hitched
     * frame's render window in vbl. h - H ~= the non-render part. */
    {
        extern volatile int saturn_prof_hitch_r;

        /* fps_num(64, 'H', (unsigned int)saturn_prof_hitch_r);
         * Ã¢â‚¬â€ row 64 ceded to the slave's round-13 close-seg probe */
        (void)saturn_prof_hitch_r;
    }
    /* 2026-09-01 flat-census build: flush is 0 since the deferral and
     * merge = ploop + ~10, so rows F/M carry the fruns/frows counts the
     * ploop split needs (call overhead = frows*~50cyc vs kernel
     * 25.3 steps/px). */
    /* n/r rows carry the sticky input latches this diagnostic build:
    fps_num(72, 'n', (unsigned int)saturn_cnt_fruns);
    fps_num(80, 'r', (unsigned int)saturn_cnt_frows); */
    /* X/L/P/Q ceded to the DIFF PROBE (2026-09-02), now watching probe
     * col 104 (the still-red right wall; col 28's wall went clean after
     * the peg-units fix). 99999 = painted nothing there this frame.
     * Deltas aliased to the texnum (X printed 70/17 with L=Q in every
     * shot), so 01:40 build prints RAW values for both passes:
     *   B = seg BUILD peg (t_peg_abs at seg_store_wall_range, build
     *       time), c = seg BUILD texnum Ã¢â‚¬â€ vs S/D (paint-time first-span
     *       tex/peg) splits "early_prep computed a garbage peg"
     *       (B==D!=H) from "the peg is lost between build and paint"
     *       (B==H!=D). u rows retired (parity: within 0-13 every shot).
     *   P = seg (y0<<8)|y1,  I = ray (y0<<8)|y1
     *   S = seg texnum,  R = ray texnum     (same span -> whole-span red)
     *   D = seg peg_u,  H = ray peg_u       (whole map units)
     *   n = ray floor ROWS painted at col (accumulated),
     *   r = seg floor ROWS painted at col (accumulated)
     * ALL wall probes record the FIRST (nearest) painted span at the
     * col: both passes paint the same nearest wall first, so the values
     * are same-wall comparable Ã¢â‚¬â€ the old last-writer probes aliased to
     * different walls (seg's last span = a lower wall pegged to a floor
     * near z0 reading "2", ray's = an upper wall pegged to the ceiling
     * 224) and made peg deltas meaningless. t_u dropped from the HUD
     * (parity proven: 514/515, 175/175, 328/328). */
#ifdef SATURN_SEG_DIFF
    /* DIFF PROBE rows (2026-09-02) Ã¢â‚¬â€ diff builds only; the probe
     * globals live in saturn_render.c under the same #ifdef. Story so
     * far: pegs/texnums/spans proved EQUAL every frame (#62-#69, the
     * old "seg peg 2-3" was the fps row misattributed), u drifts 0-13
     * texels (d32xr-authentic), blue-hole census (D/H) armed but the
     * stripe never reproduced on 02:22+. Rows:
     *   B/c  = seg BUILD peg/texnum (seg_store_wall_range)
     *   P/I  = seg/ray first-span (y0<<8)|y1 at probe col 104
     *   S/R  = seg/ray first-span texnum
     *   D/H  = blue-hole census: first >=2/3-blue column's final clip
     *          ((ce<<8)|fl) / (col<<8)|owner; 99999 = no hole
     *   n/r  = ray/seg floor ROWS painted at the probe col */
    {
        extern volatile int saturn_probe_seg_y, saturn_probe_ray_y;
        extern volatile int saturn_probe_seg_stex, saturn_probe_ray_tex;
        extern volatile int saturn_probe_seg_floor, saturn_probe_ray_floor;
        extern volatile int saturn_probe_build_peg, saturn_probe_build_tex;
        extern volatile int saturn_probe_hole_x, saturn_probe_hole_clip;
        extern volatile int saturn_probe_hole_sec;

        fps_num(88, 'B', (unsigned int)saturn_probe_build_peg);
        fps_num(96, 'c', (unsigned int)saturn_probe_build_tex);
        fps_num(104,'P', (unsigned int)saturn_probe_seg_y);
        fps_num(112,'I', (unsigned int)saturn_probe_ray_y);
        fps_num(120,'S', (unsigned int)saturn_probe_seg_stex);
        fps_num(136,'R', (unsigned int)saturn_probe_ray_tex);
        fps_num(144,'D', (unsigned int)saturn_probe_hole_clip);
        fps_num(152,'H', (unsigned int)((saturn_probe_hole_x << 8)
                                        | saturn_probe_hole_sec));
        fps_num(72, 'n', (unsigned int)saturn_probe_ray_floor);
        fps_num(80, 'r', (unsigned int)saturn_probe_seg_floor);
    }
#else
    /* Production rows: the stage split (2026-09-02), fed from
     * saturn_segs.c's saturn_segs_render_half (the raycaster's
     * render_columns Ã¢â‚¬â€ the OLD feeding path Ã¢â‚¬â€ is dead code under
     * SATURN_SEG_RENDERER, so the first stage build read all zeros).
     * Same >>8 units, 1 printed unit = 0.198 ms:
     *   S = per-column GLUE (scale2/DIVU/finetangent, clipbounds Ã¢â‚¬â€
     *       computed as T - E - W - L)
     *   E = fr_emit_flat staging (ceiling/floor band setup)
     * T (row 8) = whole render half (master's), S+E+W+L+glue. */
    {
        extern int saturn_frt_scan;
        extern int saturn_frt_fire;

        /* fps_num(72, 'S', (unsigned int)saturn_frt_scan);
         * fps_num(80, 'E', (unsigned int)saturn_frt_fire);
         * Ã¢â‚¬â€ rows 72/80 ceded to the slave's round-13 nearpaint probe */
        (void)saturn_frt_scan; (void)saturn_frt_fire;
    }
    /* FRAME BUDGET SPLIT (2026-09-02, production): the render (D) reads
     * 3 vbl but the frame (J bar) is ~15 at fps 4 Ã¢â‚¬â€ decompose the
     * non-render window numerically on the free rows:
     *   F = cart fill vbl inside the tic window (saturn_prof_fill)
     *   K = whole tics window vbl (sim + fills; d_main D_DoomLoop)
     *   A = finish vbl (I_FinishUpdate: 8->16 expand + VDP1 commit)
     * Frame Ã¢â€°Ë† K + D + A (+ pacing); F Ã¢Å â€š K. Whatever dominates K
     * besides F is sim cost; F Ã¢â€°Ë† K means the fill drain is the stall.
     * The e/s/x rows went to the HITCH LATCH (h/H/q/p/C) Ã¢â‚¬â€ expand and
     * synch are covered by the z/y micro rows below. */
    {
        extern volatile int saturn_prof_fill;
        extern volatile int saturn_prof_ticvbl;
        extern volatile int saturn_prof_finish;
        extern volatile int saturn_prof_hitch_t;
        extern volatile int saturn_prof_hitch_f;
        extern volatile int saturn_prof_hitch_c;

        /* F row ceded (2026-09-02): the pre-render drain emptied the
         * fill queue before frame 1 (F read 0 in steady play), so the
         * row now shows the TIC BODY by FRT (>>8, 1 unit = 0.198 ms)
         * Ã¢â‚¬â€ Saturn_G_Ticker's cost inside the tic catch-up loop, the
         * last frame-budget component without a row. 'x' (the old x
         * micro row was ceded to C long ago). */
        {
            extern int saturn_frt_tic;

            /* fps_num(88, 'x', (unsigned int)saturn_frt_tic);
             * Ã¢â‚¬â€ 28k retired */
            (void)saturn_frt_tic;
        }
        /* BISECT round 9: 'H' row = (y0<<8)|y1 of the LAST valid
         * fr_emit_flat at band col 28 Ã¢â‚¬â€ now slave-drawn on row 48. */
        /* K/A rows ceded to the RENDER STAGE SPLIT (2026-09-02): K read
         * a constant 0 and A a constant 2 (both banked). Same >>8 units
         * as T (1 unit = 0.198 ms), seg-renderer semantics:
         *   W = paint_wall_span calls (the wall ASM kernel)
         *   L = flat merge/flush path (flush_column + flush_pending,
         *       where the flat kernel paints)
         * W vs L says wall pixels vs flat pixels; T - W - L - E says
         * whether the seg math glue dominates the 17.2 ms bucket. */
        {
            extern int saturn_frt_walk;
            extern int saturn_frt_ploop;

            /* fps_num(96, 'W', (unsigned int)saturn_frt_walk);
             * fps_num(104,'L', (unsigned int)saturn_frt_ploop);
             * Ã¢â‚¬â€ 28k retired */
            (void)saturn_frt_walk; (void)saturn_frt_ploop;
        }
        /* q/p rows ceded to the RENDER PREAMBLE split (2026-09-02):
         * D reads 26.6 ms but the column loop (T) only 16.2 Ã¢â‚¬â€ the ~10 ms
         * gap is master-only serial work the dual-core split cannot
         * hide. Both >>8 (1 unit = 0.198 ms), FRT-honest:
         *   q = camline rebuild (saturn_frt_cl, per numlines)
         *   p = segs_build + preload_textures (saturn_frt_segb).
         * Hitch latches remain on h/H/C. */
        {
            extern int saturn_frt_cl;
            extern int saturn_frt_segb;

            /* BISECT round 4: q/p rows hijacked (preamble split parked) Ã¢â‚¬â€
             * q = fd_stage (spans staged into pending at hole cols),
             * p = fd_flush (pending runs at the frame-end flush). */
            extern int saturn_fd_stage;
            extern int saturn_fd_flush;

            /* fps_num(112,'q', (unsigned int)saturn_fd_stage);
             * fps_num(120,'p', (unsigned int)saturn_fd_flush);
             * Ã¢â‚¬â€ 28k retired */
            (void)saturn_fd_stage; (void)saturn_fd_flush;
        }
        /* Micro rows (FRT ticks >> 2; 1 vbl ~= 15000, 900 units = 1 ms):
         *   y = slSynch commit, z = expand+purge. These see under the
         * vbl rounding that the rows above show. */
        /* fps_num(144,'y', (unsigned int)saturn_prof_syn_us >> 2);
         * Ã¢â‚¬â€ 28k retired */
        (void)saturn_prof_syn_us;
        /* (the y row was printed TWICE here Ã¢â‚¬â€ same union hazard as n/S) */
        /* fps_num(152,'z', (unsigned int)saturn_prof_exp_us >> 2); */
        /* 'u' ceded row 160 -> 152 (2026-09-02): the Kronos windowed
         * viewport renders the frame oversized and cuts everything at
         * fb y~157, so the segwall census at 160 was off-screen in
         * every windowed capture. 152 is the deepest visible row. */
        /* Round 27f (2026-09-02): the whole z-row block OFF Ã¢â‚¬â€ its glyphs
         * cover fb rows 152-156 and union over the dbg W row at 156, so
         * the last staged col-28 span never decodes. Restore when the
         * merge-loss probe retires. */
#if 0
        {
            extern int segwalls_count;
            extern int saturn_seg_caphits;
            extern int saturn_fd_emit;
            extern int saturn_fd_paint;
            extern int saturn_fd_bad;

            fps_num(152,'z', (unsigned int)((saturn_fd_emit > 99 ? 99
                                             : saturn_fd_emit) * 10000
                                           + (saturn_fd_paint > 99 ? 99
                                              : saturn_fd_paint) * 100
                                           + (saturn_fd_bad > 99 ? 99
                                              : saturn_fd_bad)));
        }
#endif
        /* x row ceded to the HITCH LATCH (dma micro read a constant 1
         * unit Ã¢â‚¬â€ dead): C = synchronous CD cold reads inside the
         * hitched frame. C > 0 at a hitch = a lump was read from CD
         * mid-play (the boot-frame mechanism recurring on new lumps). */
        /* C row ceded (2026-09-02): the pre-render drain made cold
         * reads extinct (C read 0 in every steady shot). It now shows
         * the JOIN window by FRT (>>8): jo_core_wait_for_slave's block
         * on the master Ã¢â‚¬â€ slave excess + SGL join overhead. j vs n vs
         * T decides whether to rebalance the column split. */
        {
            extern volatile int saturn_frt_join;

            /* fps_num(136,'j', (unsigned int)saturn_frt_join);
             * Ã¢â‚¬â€ 28k retired */
            (void)saturn_frt_join;
        }
        /* w = WORST displayed frame of the recent past, in vblanks
         * (peak-hold with slow decay, like the CD row). The last-frame
         * probes kept catching the FAST iterations (w read 2.57 vbl
         * while O said 15 fps = 4-vbl average Ã¢â‚¬â€ the long frames hid
         * behind the 16-bit FRT fold); the peak says what the pacing
         * actually pays for. Last-frame total is still on the J bar. */
        {
            extern volatile int saturn_prof_frame_vbl;
            static int          peak = 0;
            int                 fv = saturn_prof_frame_vbl;

            if (fv > peak)
                peak = fv;
            else if (peak > 8)
                /* (peak >> 6) alone is 0 for any peak in 9..127 Ã¢â‚¬â€ the
                 * first version froze there FOREVER and all three
                 * 04:0x shots read a sticky 63 off the boot frame's
                 * decay path. |1 forces >=1/frame below 128. */
                peak -= (peak >> 6) | 1;
            else if (peak > 0
                     && ((unsigned int)saturn_vbl_count & 63) == 0)
                peak--;
            /* fps_num(24, 'w', (unsigned int)peak);
             * Ã¢â‚¬â€ row 24 ceded to the slave's round-13 close-seg probe */
            (void)peak;
        }
    }
#endif
    /* D/H ceded to the DSP-mover gate this diagnostic build (crossing
     * counters already read: DÃ¢â€°Ë†325, HÃ¢â€°Ë†2.4k Ã¢â‚¬â€ see the 21:04 captures):
     *   D = per-frame cart FILL bytes (flat refills + patch fills) Ã¢â‚¬â€
     *       steady nonzero = the DSP bulk mover has work to take
     *   H = per-frame composite bytes stamped (CPU work the mover
     *       cannot take; splits fill from stamp cost). */
    /* D/H ceded to the frame-budget split (2026-09-01), then to the
     * DIFF PROBE (2026-09-02 01:40: seg/ray peg_u raw). The fill/ticvbl
     * budget read is parked until the diff probes come back out. */
#ifdef SATURN_SEG_DIFF
    /* Seg-vs-ray diff build: Z = mismatching pixels this frame (the
     * mismatches themselves are painted red in the view). */
    {
        extern volatile int saturn_seg_diff_count;
        fps_num(156,'Z', (unsigned int)saturn_seg_diff_count);
    }
#endif
    /* Flat census under the bars: PARKED (2026-09-02). Rows 198/206/214
     * start at the fb's last line Ã¢â‚¬â€ rows 200+ wrote ~50 bytes/frame
     * PAST the end of I_VideoBuffer (fb = 200x160 = 32000 bytes),
     * corrupting whatever the heap placed after it. After the 128 KB
     * software-renderer buffers were freed the layout shifted and the
     * OOB landed on live renderer data: magenta ceiling band, floor
     * speckles, garbled HUD digits. The census is cumulative counters;
     * re-home it to y<=190 rows if it's needed again. */
#if 0
    fps_num(198,'L', (unsigned int)saturn_cnt_fruns);
    fps_num(206,'R', (unsigned int)saturn_cnt_frows);
    fps_num(214,'W', (unsigned int)saturn_cnt_span);
#endif
    /* (No free row for the span counter Ã¢â‚¬â€ the bar rows start at 160.) */
    /* Bottom edge: log bars. S/W/G/P are vbl counts, J is the whole
     * displayed frame's vblank count.
     * W and G are PEAK-HOLD (slow ~3%/frame decay): the "camera can't
     * move" pauses are 1-frame stalls Ã¢â‚¬â€ a last-frame bar hides them,
     * a peaked bar keeps them visible for seconds. G = the per-frame
     * background cart-fill drain (GFS on the CD handle): a cold-lump
     * entry stalls the main thread Ã¢â‚¬â€ tics starve, camera freezes. */
    {
        unsigned int v;
        /* Round 25 segwall census (see the 'n' row block above). */
        extern int segwalls_count;
        extern int saturn_seg_caphits;

        v = (unsigned int)saturn_prof_ticvbl;
        fps_w_hold = (v > fps_w_hold) ? v : (fps_w_hold - (fps_w_hold >> 3));
        v = (unsigned int)saturn_prof_fill;
        fps_g_hold = (v > fps_g_hold) ? v : (fps_g_hold - (fps_g_hold >> 3));
        v = delta;   /* already capped at 60 by the average gate */
        fps_j_hold = (v > fps_j_hold) ? v : (fps_j_hold - (fps_j_hold >> 3));
        /* (S bar ceded 2026-09-02: row 160 showed 'u', the
         * I_FinishUpdate gap window. Round 25 ceded it to the SEGWALL
         * census: segwalls_count*100 + seg_store_wall_range's SEGW_MAX
         * quota drops this frame (capped at 99). "u02500" = 250 walls,
         * 0 drops; ANY non-zero last two digits = distant walls are
         * being quota-dropped (the "walls render only up close"
         * family).) */
        /* 'u' moved to row 152 (visible in the windowed viewport); the
         * row-160 print is dead Ã¢â‚¬â€ see the 'z' row block above. */
        /* Bars OFF for the round-27d live-probe build: each bar erases
         * its y-1..y+9 band at x 46..140, and together they wiped every
         * fb row 167-199 Ã¢â‚¬â€ the slave dbg rows relocated below (156-184)
         * would have been erased right after drawing. Restore when the
         * probe retires. */
        /* fps_bar(168, 'W', fps_w_hold);
         * fps_bar(176, 'G', fps_g_hold);
         * fps_bar(183, 'J', (unsigned int)delta);
         * fps_bar(190, 'P', (unsigned int)saturn_prof_pre); */
    }
    /* PERF PASS rows (2026-09-03): the round-28k retirement left the HUD
     * with only the fps readout; the optimization pass needs the frame
     * budget split again. Build with -DSATURN_PERF_ROWS (i_video.o rule
     * in the Makefile). One fps_num per row (no union hazard), all rows
     * <= 136 (the windowed Kronos viewport cuts fb at ~157). Frame =
     * render (D) + tics (x, F inside) + finish (i) + pacing; render =
     * column loop (T = W+L+q+p+join+glue, both cores accumulated). */
#ifdef SATURN_PERF_ROWS
    {
        extern volatile unsigned int saturn_prof_render_frt;
        extern volatile unsigned int saturn_prof_finish_frt;
        extern volatile int          saturn_prof_fill;
        extern int                   saturn_frt_total;
        extern int                   saturn_frt_tic;
        extern int                   saturn_frt_walk;
        extern int                   saturn_frt_ploop;
        extern int                   saturn_frt_cl;
        extern int                   saturn_frt_segb;
        extern volatile int          saturn_frt_rate;
        extern volatile int          saturn_frt_join;
        extern volatile int          saturn_prof_frame_vbl;
        extern volatile int          saturn_frt_slavehalf;
        extern volatile int          saturn_prof_sccr0;
        extern volatile int          saturn_prof_sccr1;
        extern volatile int          saturn_prof_slave;
        extern volatile int          saturn_slave_frt_cal;
        extern volatile int          saturn_frt_masterhalf;
        extern volatile int          saturn_split_x;
        extern int                   saturn_frt_segb2;
        extern int                   saturn_frt_pre_acc;
        extern int                   saturn_frt_macc;
        extern int                   saturn_frt_paintmax;
        extern int                   saturn_frt_spr;
        extern int                   saturn_frt_flush;
        extern int                   saturn_frt_psite;
        extern int                   saturn_frt_gapmax;
        extern volatile int          saturn_frt_prea;
        extern volatile int          saturn_frt_preb;
        /* 2026-09-03 photos #152/#153 round: vbl-honest rows (the FRT
         * is dead on real HW â€” ~55 ticks/vbl, phase garbage). */
        extern volatile int          saturn_frt_joinvbl;
        extern volatile int          saturn_prof_pre;
        extern int                   saturn_frt_rcomp;
        extern int                   saturn_frt_vwalls;
        extern int                   saturn_frt_vfill;
        extern int                   saturn_frt_vspr;
        extern int                   saturn_frt_vwalk;
        extern int                   saturn_frt_vfire;
        /* 2026-09-03 real-HW round 2: the >>8 FRT ACCUMULATOR rows â€”
         * each sub-delta is one call (< 1 vbl, never folds), and the
         * L=767-vs-p=11 cross-check proves the tick rate on HW. These
         * give the exact sub-vbl split the vbl rows can't. */
        extern int                   saturn_frt_fire;
        extern int                   saturn_frt_scan;
        extern volatile int          saturn_dbg_w28;
        extern volatile int          saturn_dbg_w60;
        extern int                   saturn_dbg_pairs;
        /* 2026-09-03 round 7, the RED SCREEN (slave watchdog fired) +
         * build stamp: C = (red events << 8) | stall vbl, M = slave
         * heart delta at red (0 = no red yet; decode table at
         * saturn_slave_heart in saturn_render.c), Y = build minute code
         * (hh*60+mm of __TIME__) so a burned disc identifies itself. */
        extern int                   saturn_dbg_red_events;
        extern int                   saturn_dbg_red_stallvbl;
        extern int                   saturn_dbg_red_heart;
        extern int                   saturn_frt_vmerge;
        extern int                   saturn_cnt_fruns;
        extern int                   saturn_cnt_fpx;
extern int                   saturn_cnt_femit;   /* round 8b: staging vol */
extern int                   saturn_frt_mbook;   /* round 8b: merge book */
        extern int                   segwalls_count;
        extern int                   saturn_seg_caphits;
        extern volatile int          saturn_prof_render;
        extern volatile int          saturn_prof_finish;
        static int                   peak = 0;
        int                          fv   = saturn_prof_frame_vbl;

        /* Round-10e FLIGHT RECORDER consume (2026-09-04): if the last
         * session ended in a (permanent) red watchdog screen, the
         * snapshot it stashed survives the reset. Round-11c: the record
         * is read from the INTERNAL BACKUP RAM first (Saturn_BupLoad â€”
         * the SAROO boot menu wipes cart DRAM, so the cart copy may be
         * gone); the cart's top 512 bytes stay as the fallback. Seed
         * the C/M/X rows with it, then consume the magic either way.
         *   C = the full stall length in vbl ((events<<8)|stall
         *       reconstruction shows rec[7] up to 2^23),
         *   M = slave heart delta at red (WHERE the slave was:
         *       <1000 = wall pair N, >=1000 = fill column N-1000s,
         *       decode table at saturn_slave_heart),
         *   X = (masked-grate columns painted << 8) | second-grate
         *       drops at red.
         * Live red events after this boot continue on top of the
         * seeded values. */
        {
            static int rec_done = 0;
            extern int saturn_game_started;   /* d_main.c: set at D_DoomLoop */

            /* The boot smoke test ALSO runs I_FinishUpdate -> here, and
             * the BUP consume needs the Z zone (Z_Init runs after the
             * smoke test) â€” round-11c boot hang. Wait for the game. */
            if (saturn_game_started && !rec_done)
            {
                extern unsigned int saturn_cart_base_addr;
                extern unsigned int saturn_cart_size;
                unsigned int        recw[19];
                int                 have = 0;

                rec_done = 1;
                {
                    extern int Saturn_BupLoad(unsigned int *, int);
                    extern int Saturn_CdFillBusy(void);

                    /* Round-34: BupLoad is CD-block traffic (BUP_Init/
                     * Stat/Read run on the CD-block SH-1) â€” skip it
                     * while an async CD fill is in flight; an
                     * unconsumed record just survives to the next
                     * boot. */
                    if (!Saturn_CdFillBusy()
                        && Saturn_BupLoad(recw, 19) == 0
                        && recw[0] == 0x52454331u)
                        have = 1;
                }
                if (!have && saturn_cart_base_addr != 0u)
                {
                    volatile unsigned int *rec =
                        (volatile unsigned int *)
                        (saturn_cart_base_addr + SATURN_CART_REC_OFF);

                    if (rec[0] == 0x52454331u)          /* 'REC1' */
                    {
                        int k;

                        for (k = 0; k < 19; k++)
                            recw[k] = (unsigned int)rec[k];
                        have = 1;
                        rec[0] = 0; /* consume: one boot per record */
                        saturn_oc_purge();
                    }
                }
                if (have)
                {
                    extern int saturn_dbg_red_events;
                    extern int saturn_dbg_red_stallvbl;
                    extern int saturn_dbg_red_heart;
                    extern int saturn_mmid_paint;
                    extern int saturn_mmid_skip;

                    saturn_dbg_red_heart    = (int)recw[1];
                    /* MISMATCH FIX (2026-09-04 round 12): the watchdog
                     * save writes recw[7] as the RAW stall length in
                     * vblanks (saturn_render.c climb block), but this
                     * restore used to read it back as
                     * (events<<8)|stallvbl â€” a 61-vbl stall seeded
                     * "events=0, stall=61" and the session's real event
                     * count was lost. The count is per-session anyway:
                     * start it at 0 and restore only the stall length. */
                    saturn_dbg_red_events   = 0;
                    saturn_dbg_red_stallvbl = (int)(recw[7] & 0xFFu);
                    saturn_mmid_paint       = (int)(recw[4] >> 16);
                    saturn_mmid_skip        = (int)(recw[4] & 0xFFFFu);
                    /* Round-10g: seed the colormap canary rows from
                     * the record (index/count + boot/live), so a
                     * red screen during the stomp hunt still shows
                     * the verdict on the next boot's first HUD. */
                    saturn_cm_bad_index = (int)(recw[8] >> 16) & 0xFF;
                    saturn_cm_bad_count = (int)(recw[8] & 0xFFFFu);
                    /* saturn_cm_bad itself is NOT seeded: this
                     * boot's own probe already ran and is the
                     * authoritative verdict for THIS load. */
                    saturn_cm_bad_live = (int)(recw[9] & 0xFFFFu);
                }
            }
        }

        /* D (2026-09-03 photos #158/#159 round): VBLANKS spanned by the
         * flat paints that fire from the MERGE paths (mismatch repaint
         * + frame-end flush) â€” the P row only wraps the emit/staging
         * calls, so these landed unmeasured in K. If D is fat, the
         * "glue" is really flat painting; the old FRT render window
         * here is dead on real HW. */
        /* 2026-09-09 BUG-VIEW TRIAGE round 2 (temporary; the vmerge/
         * render/finish probes served out): D/T/W/L/O/C dump the
         * TOPMOST quad's stored geometry via Saturn_Vdp1Wall_Q(0,f)
         * (min ya0, tie-break larger xb, right-edge-aware — the
         * speckle band draws in the top of the RIGHT quarter, where
         * no legitimate wall top should reach). M = its [13] flags
         * (0 wall / 1 masked-mid / 3 sprite), X = its [7]/[8] pw and
         * rows, and n = the LAST quad's [13] flags (the nearest world
         * sprite — sprites append after the walls).
         *   D=[0] xa   T=[1] ya0  W=[2] ya1   L=[3] xb
         *   O=[4] yb0  C=[5] yb1  M=[13] flags
         *   X=(pw<<8)|rows  n=last-quad flags
         * Decode against the speckle band's screen box: if the
         * topmost quad's rect lands there and its flags read 1 or 3,
         * the band is a sprite/mid quad; if flags read 0, a wall quad
         * is drawing from bad VRAM. */
        {
            extern short Saturn_Vdp1Wall_Q(int n, int f);
            fps_num(8,  'D', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 0));
            fps_num(16, 'T', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 1));
            fps_num(24, 'W', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 2));
        }
        /* (T / W prof rows RETIRED for the triage — they double-painted
         * y=16/24 and their glyphs won the union, hiding the quad dump's
         * ya0/ya1. Restore after the speckle hunt.) */
        /* fps_num(16, 'T', (unsigned int)saturn_prof_render); */
        /* fps_num(24, 'W', (unsigned int)saturn_prof_finish); */
        /* L (2026-09-03 photo #147): the master's whole half, per-column
         * POLL-ACCUMULATED inside saturn_segs_render_half (each 16-bit
         * sub-delta < one fold), >>8 â€” fold-proof ground truth for the
         * packed M row (whose low byte read 0 = exactly 2 FRT wraps).
         * 1 unit = 256 ticks = 0.198 ms; 512 = 101.5 ms. */
        {
            extern short Saturn_Vdp1Wall_Q(int n, int f);
            fps_num(32, 'L', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 3));
        }
        /* q / p (2026-09-03 real-HW): the FINISH split Ã¢â‚¬â€ camline was an
         * honest 0 and segb vbl-rounded 0, so the rows print the two
         * I_FinishUpdate halves (>>2, same units as i/D):
         *   q = the 32 KB fb word-copy into the back NBG0 page
         *   p = the slSynch wait + NBG0 flip
         * i - q - p = the fill-strip + HUD stamping. */
        /* q (2026-09-03 round 8b): the MERGE MACHINERY's match/bookkeep
         * time (>>8, master side) â€” the span-sequence compare + pending
         * refill fr_merge_column runs TWICE per (piece,column) pair.
         * S (macc remainder) - q = the rest of the pair glue. The old
         * rcomp thrash detector read a dead path's zero.
         * (round-10f/10g/10h colormap canary RENTIRED 2026-09-04: the
         * poll still runs every frame and latches into the flight
         * recorder, but the Y123 photo read all-healthy with the
         * corrected premise â€” the WRAM-L colormap is byte-exact to the
         * Ultimate WAD and the round-10 speckle is closed with the
         * real-sh lookup restored. q back to merge bookkeeping.) */
        Saturn_ColormapPoll();
        /* round-11b PROBE (2026-09-04): q repointed â€” the LAST masked
         * call's TEXTURE number (raw TEXTURE1 index). Cross-checks
         * against the WAD: wrong texture = wrong art at any distance.
         * mbook returns with the probe. */
        {
            /* 2026-09-08 REJECT CENSUS v2 (temporary; the BSS-stomp
             * canary served out): reasons 0 (no TOP/BOTTOMTEXTURE —
             * faceless two-sided openings, the walk sets both bits only
             * when a face exists) and 4 (zero window) are BENIGN —
             * verified against saturn_segs.c:1497 (single-sided walls DO
             * set AC_TOPTEXTURE, so reason 0 is not them). q = top
             * ACTIONABLE reason (id<<8)|count over the rest; N = second;
             * G = benign total. The actionable id is stage-1's blocker.
             * Full ledger in saturn_segs.c at saturn_v1w_rj[]: 1 tex<=0,
             * 2 no SOLIDSIL/NEWCEILING, 3 MIDTEXTURE, 5 spr_ymax NULL,
             * 6 run<16, 7 off-screen/tiny, 8 clip band, 9 mmid grate,
             * 10 sprite/weapon, 11 v-bands, 12 CanAdd, 13 degenerate u;
             * stage-2a MID prescan: 14 t_u clamp, 15 texh<8, 16
             * off-view/tiny, 17 band escape, 18 sprite/weapon,
             * 19 run proof (degenerate u / safe range).
             * Ties pick the lowest id. */
            extern int saturn_v1w_rj[20];
            static const unsigned char act[20] =
                { 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                  1, 1, 1, 1, 1, 1 };
            int k, b1 = 0, i1 = 0, b2 = 0, i2 = 0, ben = 0;

            for (k = 0; k < 20; k++)
            {
                if (!act[k])
                {
                    ben += saturn_v1w_rj[k];
                    continue;
                }
                if (saturn_v1w_rj[k] > b1)
                {
                    b2 = b1; i2 = i1;
                    b1 = saturn_v1w_rj[k]; i1 = k;
                }
                else if (saturn_v1w_rj[k] > b2)
                {
                    b2 = saturn_v1w_rj[k]; i2 = k;
                }
            }
            v1w_cens_id1 = i1; v1w_cens_c1 = b1;
            v1w_cens_id2 = i2; v1w_cens_c2 = b2;
            v1w_cens_ben = ben;
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; the FB probe served):
             * q = top ACTIONABLE rj reason (id<<8)|count — the id names
             * the dominant blocker on faces that never became quads.
             * The census above computes it every frame. */
            fps_num(40, 'q', (unsigned int)
                    (((v1w_cens_id1 & 0xFFu) << 8)
                     | (v1w_cens_c1 & 0xFFu)));
        }
        /* round-11b PROBE (2026-09-04): p repointed â€” the LAST masked
         * call's SHADE ROW (cmap row index, 0=bright .. 31=black). The
         * tan-band suspect: a shade near 0 on the far wall = lighting
         * feed wrong (should be dark at that depth). vwalls returns
         * with the probe.
         * P5 Phase 3 (2026-09-10): p = rj[9] — farther wall faces
         * refused because a NEARER grate stayed software. Phase 3
         * re-scoped the reason: a quad'd grate (face 2 marked) no
         * longer vetoes, so on grate poses this should read ~0 (the
         * residual = grates the capture could not prove). Grate-free
         * poses read 0 always. (The old (segwalls<<8)|caphits read
         * duplicated the H row at 192.) */
        {
            extern int saturn_v1w_rj[20];
            fps_num(48, 'p', (unsigned int)saturn_v1w_rj[9]);
        }
        /* j (2026-09-03 round 8b): flat span rows EMITTED per frame
         * (master side, pre-merge staging volume). j - F = the volume
         * the merge extends already dedup'd; j vs the ~16K half-screen
         * area measures the portal-nesting overdraw (every piece
         * re-emits its full floor/ceiling band per column). The old
         * join-wait row read a constant 0.
         * round-11c PROBE: repointed â€” the master's masked-CALL count
         * this frame (dbg[0].calls). With S (eligible rows) it checks
         * the S arithmetic: S / calls <= band height (~180). The old
         * S=56311 across 73 "calls" was 771 rows/call â€” impossible,
         * the probe struct was cross-core polluted (aligned(16) now). */
        {
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; the FB1 probe
             * served): j = columns whose TOP paint was skipped for
             * committed quads this frame (saturn_v1w_skipcols) — the
             * VDP1-coverage denominator. Stage-1 courtyard banked 58;
             * if the bug view reads ~0 the faces never became quads. */
            extern int saturn_v1w_skipcols;
            fps_num(56, 'j', (unsigned int)saturn_v1w_skipcols);
        }
        /* i (2026-09-03 photos #152/#153 round): the master's
         * flush_pending (open-band fill) leg in VBLANKS. (Canary value
         * print retired with the round-10h closure.)
         * round-11 PROBE (2026-09-04): repointed â€” high nibble = OR of
         * the masked painter's early-return reasons (1..5), low 12
         * bits = the LAST masked call's utex fetch column. vfill
         * returns with the probe.
         * 2026-09-08 COPR PROBE (TEMPORARY; skipdie 0 banked): the
         * VDP1's own end-of-draw command address / 32 = the walk slot
         * index. 3 = the walk stops at SGL's slot 3 (our user slots
         * never execute); 4+canary+N+1 = the walk runs the full user
         * list (then only per-quad visuals can hide the gun). */
        {
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; the Copr probe
             * served out — it read a racy mid-frame position, slot 1,
             * in both the census and control builds): O = quad 0's
             * [4] yb0 (bottom-edge y at the A/xa side). */
            extern short Saturn_Vdp1Wall_Q(int n, int f);

            fps_num(64, 'O', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 4));
        }
        /* 2026-09-10 TIMING SPLIT (user request after the 2.4 fps real-HW
         * read; the old slot-3 CTRL probe here was already erased every
         * frame by the D paint at the same y — dead since 09-09): the E
         * paint is retired. The frame budget now reads straight off the
         * census, all rows in VBLANKS per displayed frame (vbl-honest on
         * real HW, unlike every FRT row):
         *   r(88)  render   = D_Display render window (saturn_prof_render)
         *   f(96)  finish   = expand + VDP1 DMA + slSynch (saturn_prof_finish)
         *   t(144) tics     = tic catch-up loop wall (saturn_prof_ticvbl)
         *   v(152) slave    = slave's own half (saturn_prof_slave)
         *   e(184) expand   = 8->16 expand alone (saturn_prof_exp)
         * Identity to check in photos: frame(fps row) >= r + f + t (+pacing
         * gaps); render = master half + v + prologue/join. The rows this
         * retired: D FRT-BSP (garbage on real HW), X quad0 pw/rows and s
         * rj[9] (speckle triage, served; quad0 coords live on at D/T/W/L/
         * O/C), N second-reason and G benign (q/F carry the ledger; they
         * return with Phase 4), n last-quad flags (served). */
        /* x / w (2026-09-03): the PRE window SPLIT (>>8, bare 16-bit
         * deltas, each < one fold): x = preload_textures alone,
         * w = segs_build alone. s (below) carries their exact sum. */
        /* h (2026-09-03 photos #163/#164 round): FLAT ROWS painted per
         * frame, master half. With F (px) and R (runs) it pins the
         * per-row cost: cycles/row = D vbl x 477K / h, and F/h = mean
         * run width. Replaces the FRT rate probe â€” dead on real HW
         * (TCNT is reset every vblank; photo rows read 65334 garbage).
         * round-11c PROBE: repointed â€” the ENGINE composite's zero
         * per-mille for tex 4 (BRNBIGC, the X-grate). PC truth = 297.
         * A fat value convicts the engine's composite; ~297 exonerates
         * it and the fill/shade own the tan bands. */
        {
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; the FB0 probe
             * served): h = saturn_v1w_skipdie — preskip marked a column
             * for a quad but the walk gate refused to commit it.
             * Non-zero = preskip/walk disagreement (the paint-hole
             * family); 0 = the holes are faces that never got marked. */
            extern int saturn_v1w_skipdie;
            fps_num(72, 'h', (unsigned int)saturn_v1w_skipdie);
        }
        /* F (2026-09-03 photos #161/#162 round): FLAT PIXELS painted per
         * frame, master half (every paint_flat_run call, merge + flush
         * paths). Decides the D decomposition: F ~ 10-20K = honest pixel
         * volume (then the 9 vbl is per-row/per-call overhead, e.g. the
         * per-row Saturn_LightRow software divide); F ~ 100K+ = the merge
         * mismatch repaints are multiplying the pixel volume. The old
         * macc-remainder row is dead on real HW.
         * round-11c PROBE: repointed â€” zero per-mille for tex 6
         * (BRNBIGR, the LAST masked call's texture). PC truth = 74. */
        {
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; MODR readback
             * served): F = rj[3] — faces rejected as MIDTEXTURE (the
             * grate/transparent-wall family). With q's top reason it
             * splits the benign mass into named buckets. */
            extern int saturn_v1w_rj[20];
            fps_num(80, 'F', (unsigned int)saturn_v1w_rj[3]);
        }
        /* s (2026-09-03 photos #158/#159 round): TOTAL (piece, column)
         * pairs the master walked this frame. With K it names the true
         * per-pair cost: cycles/pair = K vbl x 477K / s. The old FRT
         * pre_acc row here is dead on real HW. */
        /* round-11 PROBE (2026-09-04): s repointed â€” masked-painter
         * TEXELS actually written this frame, master half. ~0 = the
         * grate paints nothing (all-transparent sampling or no
         * eligible rows) and the fill's flats show through. */
        {
            /* 2026-09-10 TIMING SPLIT: f = FINISH vblanks per displayed
             * frame — the 8->16 expand + VDP1 DMA + slSynch window
             * (saturn_prof_finish). The old s = rj[9] paint was erased
             * by the X paint at this y every frame (dead since it
             * landed); rj[9] returns in the ledger when Phase 3 needs
             * it. */
            extern volatile int saturn_prof_finish;
            fps_num(96, 'f', (unsigned int)saturn_prof_finish);
        }
        /* C / v (2026-09-03 photo #149): the SLAVE's own render_half by
         * its own FRT (>>8) and the slave body in VBLANKS. Both drawn
         * UNCACHED further down (after M) â€” see the A/C/v block there. */
        /* R (2026-09-03 photos #161/#162 round): flat RUNS painted per
         * frame, master half â€” paint_flat_run call count. R x avg-span
         * vs F splits the D cost into per-call overhead vs pixel volume.
         * The old FRT-rate witness here is dead (FRT dead on real HW). */
        /* C (2026-09-03 round 7, RED SCREEN): high byte = slave-stall
         * events (the watchdog painted red this session), low byte =
         * last stall in VBLANKS (capped 255). 0 both = no red yet.
         * Red on screen + C high byte advancing = recurring stalls.
         * (Replaces the R flat-run census â€” redundant with h/F.) */
        /* 2026-09-08 OCRAM-DIVLUT CENSUS (temporary repoint): the
         * red-watchdog channel is dead in master-only mode (the slave
         * never runs a heart, so red_events/heart are frozen 0), so C
         * now reads the LightRow OCRAM-LUT divides THIS FRAME
         * (snapshot+reset). M = the DIVU/C fallbacks. */
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 3)
        {
            /* 2026-09-09 BUG-VIEW TRIAGE (temporary; the DIVLUT row
             * served out — campaign closed): C = quad 0's [5] yb1
             * (bottom-edge y at the B/xb side). */
            extern short Saturn_Vdp1Wall_Q(int n, int f);

            fps_num(120, 'C', (unsigned int)(unsigned short)Saturn_Vdp1Wall_Q(0, 5));
        }
#endif
        /* B/M rows retired 2026-09-03 (real-HW round 2): psite read a
         * constant 1 (the sprite pass X=0 every frame) and the packed
         * masterhalf was p (high byte) + dead-FRT (low byte). Rows 128
         * and 136 now carry the FRT-accumulator prints near their
         * comment block above. */
        /* 2026-09-03 photo #149 round: N/G/P/K/A/X/H. Real HW shows the
         * full frame, so rows may run to y192 (last safe row). */
        /* round-11b PROBE (2026-09-04): N repointed â€” the masked wall's
         * SIDEDEF SECTOR lightlevel (0-255, what Saturn_LightRow was
         * fed). Far from the fill's light: if N is dark but the bands
         * are bright, the FILL (not the wall) owns the wrong light.
         * pre returns with the probe. */
        {
            extern volatile saturn_mm_dbg_t saturn_mm_dbg[2];
            /* 2026-09-08 DIVLUT CENSUS (temporary repoint; the 09-07
             * sprite-visibility CYCA forensics returns here): N =
             * FixedDiv calls THIS FRAME (snapshot+reset), G = the
             * in-envelope fraction (|b|<=511, |a|*|b|<2^16 — the shape
             * a future OCRAM recip path could serve). The ratio G/N
             * decides whether a FixedDiv reroute is worth anything. */
            (void)saturn_mm_dbg;
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 3)
            /* 2026-09-10 TIMING SPLIT: t = TICS vblanks per displayed
             * frame — the tic catch-up loop's wall duration
             * (saturn_prof_ticvbl; measured d_main.c:1480). The second
             * actionable reject reason returns with the Phase 3/4
             * ledger (q/F still carry the top). */
            fps_num(144, 't', (unsigned int)saturn_prof_ticvbl);
#endif
        }
        /* G / P / K (2026-09-03 photos #154/#155 round): the VBL-SPAN
         * split of the master's 9-vbl wall loop (edge crossings
         * distribute across legs proportionally to time share):
         *   G = vblanks spent inside WALL paints (paint_wall_span)
         *   P = vblanks spent inside FLAT emits (fr_emit_flat kernel)
         *   K = glue = vwalls - G - P (staging, clip math, divides).
         * The old FRT rows here (segs_build/preload/camline) are dead
         * â€” the pre window is row N now. */
        /* round-11b PROBE (2026-09-04): G repointed â€” the FILL side of
         * the grate: high byte = the lastsec sector the fill's flats
         * used for a masked column, low byte = that sector's lightlevel
         * (sectors[] indexed 0-255). Wrong sector id = the open-band
         * fill is painting the wrong room's flats through the holes.
         * vwalk returns with the probe. */
        {
            /* 2026-09-08: DIVLUT envelope row served out (divide
             * campaign closed) — G now paints the V1W reject census
             * BENIGN total (reasons 0 + 4: faceless two-sided openings
             * and zero windows — correct rejections, not blockers).
             * Computed in the q-row census block; cumulative. */
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 3)
            /* 2026-09-10 TIMING SPLIT: v = the SLAVE's own render half
             * in vblanks (saturn_prof_slave, published by the slave at
             * saturn_render.c:3636). With r (render total) it splits
             * master-half + prologue/join: r - v = master's share.
             * The benign-reject total returns with the Phase 4 ledger
             * (v1w_cens_ben is still computed every frame). */
            fps_num(152, 'v', (unsigned int)saturn_prof_slave);
#endif
        }
        /* P (2026-09-03 real-HW round): vblanks inside FLAT emits
         * (fr_emit_flat kernel). (Canary shape print retired with the
         * round-10h closure.)
         * round-11c PROBE: repointed â€” high byte = the tex the zcol
         * column census ran on (4 or 6), low byte = zero COUNT in that
         * call's sampled column (0..128). This is the painter's actual
         * skip rate for the sampled column, not the texture average. */
        {
            /* 2026-09-07 PAINT-SKIP FORENSICS (temporary): P repointed
             * from the SW1COMP probe (idle) to the live quad-table
             * count at display time (v1q_count, 0..32). */
            extern int Saturn_Vdp1Wall_Count(void);
            fps_num(160, 'P', (unsigned int)Saturn_Vdp1Wall_Count());
        }
        /* (The K glue row that also lived at y168 was retired round 8 â€”
         * two glyphs at one y union-corrupt the row, and the glue metric
         * is superseded by the Lâˆ’Bâˆ’Eâˆ’S identity.) */
        /* M (2026-09-03 round 7): slave progress at the moment the
         * watchdog painted red â€” heart delta since dispatch. 0 = no
         * red has fired. (Canary boot/imax print retired with the
         * round-10h closure; the heart still latches into the flight
         * recorder.) */
        /* 2026-09-08 DIVLUT CENSUS (temporary repoint): M = LightRow
         * DIVU/C fallbacks this frame (divides past the LUT's
         * dist<=511 envelope, or taken while the A/B toggle is off).
         * 2026-09-08 STAGE 2a: M repointed — high byte = masked-mid
         * paints SKIPPED for AddMid quads this frame (saturn_v1w_skipmid),
         * low byte = masked-mid columns still painted by software
         * (saturn_mmid_paint). Both <= 160 (one render half). */
        {
            /* P5 Phase 1 repoint (2026-09-10, temporary): M =
             * (saturn_v1w_captured << 8) | cells — high byte latches
             * nonzero the moment the quad table fills (the full-VDP1
             * capture's fail-open signature), low byte = cells stored
             * this frame. The flags read it replaced (topmost quad
             * [13]) returns in a later phase. */
            extern int saturn_v1w_captured;
            extern int saturn_v1w_cells;

            fps_num(168, 'M', (unsigned int)
                    ((saturn_v1w_captured << 8) | (saturn_v1w_cells & 0xFF)));
        }
        /* n (2026-09-08 stage-2a verify, temporary): the mid prescan's
         * own census, packed DECIMAL as MCCCCC QK:
         *   value = midcols*1000 + midquads*10 + best_k
         * midcols = master-half columns with a recorded grate the
         * prescan saw; midquads = AddMid pieces committed; best_k =
         * the dominant rj[14..19] reject (0=t_u clamp 1=texh 2=off-view
         * 3=band escape 4=sprite veto 5=run proof). rj is cumulative. */
        {
            /* 2026-09-10 TIMING SPLIT: e = the 8->16 EXPAND alone in
             * vblanks (saturn_prof_exp, sampled inside I_FinishUpdate).
             * f(96) - e = the rest of finish (VDP1 DMA + slSynch wait).
             * The last-quad flags row served out with the speckle/
             * gun triage. */
            fps_num(184, 'e', (unsigned int)saturn_prof_exp);
        }
        /* A (2026-09-03 round 5): the SPLIT POSITION, printed live â€”
         * the master's loop got ~4x faster with the closed-clip skip,
         * so the adaptive balancer (which moves saturn_split_x every
         * frame on 1-vbl-quantized load readings) now wanders across
         * its whole [24,136] range in motion. Any per-column state not
         * perfectly partitioned at the boundary then drags a 1px
         * full-height stripe across the frame â€” the motion-only
         * artifact the user photographed at ~col 40. If the stripe's
         * screen column == this number, the boundary is the culprit.
         * (The old w28/w60 census lost its meaning to the skip.) */
        fps_num(176, 'A', (unsigned int)saturn_split_x);
        /* H (2026-09-03 photos #156/#157 round): high byte = live
         * segwall piece count this frame, low byte = SEGW_MAX quota
         * drops (nonzero = pieces DROPPED â€” visual-loss risk AND a
         * pair-count signal). Replaces the dead FRT preload-walk row. */
        fps_num(192, 'H', (unsigned int)
                ((segwalls_count << 8) | saturn_seg_caphits));
        /* C row retired 2026-09-03 (real-HW round 2): the slave-half FRT
         * read a dead-TCNT phase constant (398 every frame). Row 104 now
         * carries the glue scan print (see the accumulator block above).
         * v still reads via the UNCACHED mirror (+0x20000000): the slave
         * publishes it through ITS cache, and the master's cached HUD
         * read can serve stale lines (photo #149's C=0 while v=5). */
        /* Y (2026-09-03 round 7): BUILD STAMP â€” hh*60+mm of __TIME__,
         * changes every rebuild. A photo of this row proves WHICH
         * build the disc carries (round 6 burned a stale disc and the
         * footage was read against the wrong build). (Replaces the v
         * slave-half row; slave health now rides C/M.) */
        fps_num(112, 'Y', (unsigned int)
                ((((__TIME__[0] - '0') * 10 + (__TIME__[1] - '0')) * 60)
                 + (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0')));
        /* X (2026-09-03 photos #152/#153 round): the master's SPRITE
         * pass in VBLANKS (replaces the dead FRT paintmax).
         * 2026-09-08 DIVLUT STATE (temporary repoint; the masked-grate
         * paint/skip pair returns): decimal decode hundreds=ok (LUT
         * installed+verified at boot), tens=on (A/B toggle state, flip
         * with the A+B+C pad chord), units=err (0 ok, 1 CCR readback,
         * 2 LUT data verify). 110 = live LUT, 100 = installed but
         * toggled off, 0 = not installed. */
#if defined(SATURN_DIVLUT) && (SATURN_DIVLUT_STEP >= 3)
        {
            extern volatile int saturn_divlut_ok;
            extern volatile int saturn_divlut_on;
            extern volatile int saturn_divlut_err;
            extern volatile int saturn_divlut_badd;

            /* y=96, NOT 128/184: 128 collides with the live B row
             * (fps_num(128,'B') at line ~3447 — its later paint erases
             * X), 184 is under the big white fps row. 96 = free (the
             * c@96 row is SATURN_SEG_DIFF-only).
             * err==2 (verify failed): X repaints as the FAILING INDEX
             * d (2..511) instead of the status code — d=2/3 means the
             * pinned entries didn't read back (table/pointer), d=4 the
             * first filled entry (div path), anything else a stomp. */
            /* 2026-09-09 BUG-VIEW TRIAGE round 2 (temporary; the fb
             * word probe served out): X = the topmost quad's source
             * shape, packed DECIMAL (pw<<8)|rows — [7] = pw/tw (texel
             * columns; sprites store pw), [8] = rows. A sprite quad
             * for the speckle band would show a small pw/rows pair
             * matching its lump; a wall quad shows tw/rows.
             * 2026-09-10: X paint RETIRED for the timing split (f row
             * owns y=96 now — see the s-row block above). Quad0's
             * coords stay on D/T/W/L/O/C; [7]/[8] return with Phase 7
             * (distortion subdivision needs pw/rows there). */
        }
#endif
        /* (2026-09-03 photos #158/#159 fix: the stale saturn_frt_pre
         * draw here OVERWROTE the census H below â€” H read 0 all round.
         * The duplicate is gone; H is drawn once, above.) */
        /* w (2026-09-03): segs_build alone, >>8 raw â€” with x (preload
         * alone) it splits the pre-dispatch window's 50 ms. The old
         * peak-hold is computed but no longer displayed. */
        if (fv > peak)
            peak = fv;
        else if (peak > 8)
            peak -= (peak >> 6) | 1;
        else if (peak > 0 && ((unsigned int)saturn_vbl_count & 63) == 0)
            peak--;
        /* E (2026-09-03 real-HW round 2): flat EMIT staging (fr_emit_flat
         * calls), >>8 FRT acc â€” replaces the fold detector, which is
         * meaningless with the FRT phase-garbage on real HW. (Canary
         * boot/live verdict retired with the round-10h closure.) */
        /* round-11 PROBE (2026-09-04): E repointed â€” the LAST masked
         * call's v0 at its band top, in TEXELS (signed; a negative
         * prints as ~65xxx). v0 < 0 for every row = nothing paints
         * (band top is above the texture top â€” peg/t_u error);
         * v0 >= texture height = the whole band breaks instantly. */
        {
            /* 2026-09-07 QUAD-DRAW FORENSICS (temporary): E repointed
             * to the VDP1 VRAM slot-3 CTRL sampled in the vblank
             * callback BEFORE the re-patch (0x8000 = SGL's END rebuild
             * won the ordering race; 0x000A = our patch survives).
             * 2026-09-10: E paint RETIRED — the D row at the same y
             * erased it every frame anyway (dead since 09-09). */
            extern int saturn_v1w_slot3_at_vbl;
            (void)saturn_v1w_slot3_at_vbl;
        }
        /* S (2026-09-03 real-HW round 3): glue = macc - walk - fire -
         * ploop, >>8, clamped >=0. The old frt_all_acc-based remainder
         * FOLDED (section stamps span ~9 vbl > 65,536 ticks) and read
         * 577214 while I read 0. Everything else in the wall loop that
         * is not a paint, staging or the flat kernel: clip math,
         * divides, finetangent, the per-pair merge, open-band fill.
         * The prime real-HW suspect (K reads 8-10 of the 11 loop vbl). */
        /* round-11 PROBE (2026-09-04): S repointed â€” masked-painter
         * ELIGIBLE rows this frame (v0 in [0,vh) at row top), master
         * half. Compare with s (texels written): s ~= S = art present;
         * S fat / s ~ 0 = the wall samples transparent columns. */
        {
            /* 2026-09-07 PAINT-SKIP FORENSICS (temporary): S repointed
             * to (walls reaching commit)<<8 | cells stored this frame. */
            extern int saturn_v1w_commits;
            extern int saturn_v1w_cells;
            fps_num(104, 'S', (unsigned int)
                    ((saturn_v1w_commits << 8) | saturn_v1w_cells));
        }
        /* D (2026-09-08 STAGE 1): BSP TRAVERSAL ONLY — seg_render_bsp_node
         * (walk + clip + viswall build), TCNT ticks >>8 (1 unit = 0.198 ms,
         * ~84 units/vbl) high byte | vblanks the walk spanned low byte.
         * vbls>0 = the walk crossed a vblank edge, tick byte is the
         * wrapped tail. Decides whether walk or glue owns the
         * post-VDP1 frame (and whether PVS/portals ever pay).
         * (Painted at the served-out E/SPCTL slot y=88: y=184 is under
         * the big white fps row and below the default Kronos window.) */
        {
            /* 2026-09-10 TIMING SPLIT: r = RENDER vblanks per displayed
             * frame (D_Display's render window). The old FRT-BSP row is
             * garbage on real HW (TCNT per-vbl reset) and its Kronos
             * question (walk vs glue) is stale post-2b — the capture
             * pass owns the walk now. */
            extern volatile int saturn_prof_render;
            fps_num(88, 'r', (unsigned int)saturn_prof_render);
        }
        /* B (2026-09-03 real-HW round 2): wall PAINTS (paint_wall_span),
         * >>8 FRT acc. The old psite row read a constant 1 (X=0 always).
         * Identity check per frame: B + E <= L, and S + I ~= L - B - E
         * (I also carries frame-end flush_pending paints outside macc). */
        /* round-11 PROBE (2026-09-04): B repointed â€” the masked
         * painter's MIN t_u this frame, master half. A tiny value =
         * near-distance shade/scale on the far wall. 16777215 = no
         * masked call this frame. */
        {
            /* 2026-09-10 INPUT TRIAGE step 2 (temporary; the 09-08 kill
             * census served out — restore it when this row is repointed
             * back): the ticbuttons row answered "does the pad reach the
             * sim" (yes), but the static audit then EXONERATED the
             * mapping table — the SGL JO_KEY enum is bit-identical to
             * SGL's own PER_DGT_* constants (SL_DEF.H:1250-1262), so
             * LEFT must read bit14. The scramble feel and the ghost
             * left-spin now hang on the RAW word itself. B = the raw
             * ACTIVE-LOW hardware word jo_inputs[0].data (PerDigital,
             * exactly what I_ReadControls inverts):
             *   idle pad        = 65535 (0xFFFF) if the SGL decode is live
             *   D-pad RIGHT held = bit15 clear -> 32767 (0x7FFF)
             *   D-pad LEFT held  = bit14 clear -> 49151 (0xBFFF)
             *   D-pad DOWN held  = bit13 clear -> 57343 (0xDFFF)
             *   D-pad UP held    = bit12 clear -> 61439 (0xEFFF)
             *   START held       = bit11 clear -> 63487 (0xF7FF)
             *   A held           = bit10 clear -> 64511 (0xFBFF)
             *   C held           = bit9  clear -> 65023 (0xFDFF)
             *   B held           = bit8  clear -> 65279 (0xFEFF)
             *   R trig held      = bit7  clear -> 65407 (0xFF7F)
             *   X held           = bit6  clear -> 65471 (0xFFBF)
             *   Y held           = bit5  clear -> 65503 (0xFFDF)
             *   Z held           = bit4  clear -> 65519 (0xFFEF)
             *   L trig held      = bit3  clear -> 65527 (0xFFF7)
             * Any OTHER bit moving = the buffer is raw undecoded SMPC
             * bytes (rewrite I_ReadControls against the real bits).
             * Word never leaves 65535 = sampling is dead. Word changes
             * only seconds after a press = the SMPC read cadence is the
             * 7-second-lag suspect itself. Release test: after letting
             * go the word must return to 65535; stuck = lost key-up =
             * the eternal left-spin. (The old kill census read a
             * constant 0 at every recent pose; its externs return with
             * the grate-pose gate.) */
            fps_num(128, 'B', (unsigned int)jo_inputs[0].data);
        }
        /* I (2026-09-03 real-HW round 2): flat KERNEL window only
         * (paint_flat_run's row/pixel loop, >>8 FRT acc). The old packed
         * masterhalf row is redundant with p (high byte) + dead FRT
         * (low byte). With E and h/F it splits per-row vs per-px cost. */
        /* round-11 PROBE (2026-09-04): I repointed â€” the masked
         * painter's MAX t_u this frame, master half. B..I brackets the
         * depth range the flush fed the painter; the back wall's true
         * distance sits mid-range if the feed is honest. */
        {
            /* 2026-09-10 INPUT TRIAGE step 2 (temporary; the dud census
             * read a constant 0x0000 and its externs return when this
             * row is repointed back): I = (pad id byte << 8) | tic count
             * low byte. The id byte is the FORMAT FINGERPRINT for the B
             * row word: 2 = SGL decoded it as the standard pad
             * (PER_ID_StnPad, data is the PER_DGT active-low layout the
             * B row legend expects); 0xF1 or anything else = the buffer
             * holds raw SMPC bytes and I_ReadControls must be rewritten
             * against the raw layout. Low byte = saturn_tic_count & 0xFF,
             * climbing at ~30/s whenever the game is unpaused (wraps
             * every ~8.5 s — two screenshots seconds apart must differ).
             * Frozen low byte = the tic loop is dead; climbing while B
             * shows the held key = the sim is alive. */
            extern volatile unsigned int saturn_tic_count;
            fps_num(136, 'I', (unsigned int)
                    (((unsigned int)jo_inputs[0].id << 8)
                     | ((unsigned int)saturn_tic_count & 0xFFu)));
        }
    }
#endif /* SATURN_PERF_ROWS */
}

void I_FinishUpdate(void)
{
    unsigned char          *src8;
    int                    n;
    extern volatile unsigned int saturn_vbl_count;
    extern volatile unsigned int saturn_master_site;  /* round-50 */
    extern volatile unsigned int saturn_master_beat;
    unsigned int           t0;
    unsigned short         fi0;
    /* Perf HUD buckets (d_main.c saturn_pf_acc) */
    extern volatile unsigned int saturn_pf_acc[6];
    extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
    unsigned short         pf_f0;
    unsigned int           pf_v0;

#if SATURN_VDP2_FB
    if (!saturn_fb_live)
        return;
#else
    if (saturn_gfx_l == NULL || saturn_gfx_r == NULL)
        return;
#endif

    saturn_master_site = 0xB;   /* round-50: blit/commit entered */
    saturn_master_beat++;

    /* Whole-finish FRT probe ('i' row): entry to exit. */
    fi0 = SAT_FRT();
    {
        unsigned int   tv0;
        unsigned short tf;
        do {
            tv0 = saturn_vbl_count;
            tf  = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != tv0, 0));
        /* fps_measure(); -- Bypassed: legacy software raycaster profiler */

    /* Cart-fill progress strip (top edge, y=0..1), visible only while
     * the boot prewarm queue is still draining from CD: white fill on
     * black, full width = all queued lumps resident in cart. When the
     * queue drains fully the strip disappears (saturn_fill_remaining
     * goes 0 from i_wad.c's drain). */
    {
        extern volatile int saturn_fill_total;
        extern volatile int saturn_fill_remaining;

        if (saturn_fill_total > 0 && saturn_fill_remaining > 0 &&
            saturn_fill_remaining <= saturn_fill_total)
        {
            int fill_px = (FB_W *
                           (saturn_fill_total - saturn_fill_remaining))
                          / saturn_fill_total;
            int x;

            for (x = 0; x < FB_W; x++)
            {
                unsigned char c = (x < fill_px) ? 0xFC : 0x00;
                I_VideoBuffer[0 * FB_W + x] = c;
                I_VideoBuffer[1 * FB_W + x] = c;
            }
        }
    }

        saturn_prof_hud_us = (unsigned short)Saturn_ProfSpan(tf, tv0);
    }   /* hud-gap micro-profile block */

    t0 = saturn_vbl_count;
    /* Step 1: expand the 8bpp DOOM framebuffer to the 16bpp Saturn
     * palette. DEAD under SATURN_VDP2_FB (kept for a sprite build):
     * written for the half-width era's 160-column fb + 2:1 stretch; a
     * sprite build on the 320-wide fb needs these loops re-derived
     * (left/right halves take columns 0..159 / 160..319 1:1).
     *
     * 2026-09-02: the row loop was a byte store per output pixel
     * (28.8k lookups + 57.6k 16-bit stores Ã¢â‚¬â€  a measured 'e' vbl of the
     * finish window). The pre-doubled saturn_palette32 table writes
     * each doubled PAIR as one 32-bit store: 28.8k lookups -> 14.4k,
     * stores -> 14.4k words. Row stride is 160 u16 = 320 bytes (4- and
     * 2-aligned), and both u16 halves of the word are identical, so
     * big-endian storage needs no swap. */
    src8 = (unsigned char *)I_VideoBuffer;
#if SATURN_VDP2_FB
    /* VDP2 direct display: no palette expand (CRAM does the color), no
     * OC purge (the CPU reads its own cache Ã¢â‚¬â€  nothing DMA-reads the fb).
     * Step 1 = strided 32 KB word-copy into the back NBG0 bitmap page
     * (z row), step 2 = flip NBG0 to it + hide NBG1, committed at the
     * vblank slSynch waits for (y row). */
    unsigned int v_pre_vdp1 = 0;
    int          pre_vdp1_in_vbl = 0;
    {
        unsigned short tf;
        do {
            t0 = saturn_vbl_count;
            tf = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != t0, 0));

        if (saturn_cram_stage_dirty)
        {
            /* A stage fill retargeted CRAM[1] since the last frame (the
             * boot stage ladder's last color, or any future runtime
             * marker). Put the whole game palette back BEFORE this
             * frame's indices reach the screen Ã¢â‚¬â€  otherwise index 1
             * displays the stage color for the rest of the session.
             * Runs once, before the first real frame is copied. */
            volatile unsigned short *cram =
                (volatile unsigned short *)SATURN_FB_CRAM;
            int ci;

            for (ci = 0; ci < 256; ci++)
                cram[ci] = saturn_palette16[ci];
            saturn_cram_stage_dirty = 0;
        }

        /* Round 7: SPLIT-BOUNDARY NOTCH â€” paint fb column saturn_split_x,
         * rows 4..15, magenta so the next footage shows the adaptive
         * split's live position ON SCREEN. If the motion stripe's photo
         * column == the notch's photo column, the boundary is the
         * stripe; if not, they are decoupled â€” both features land in
         * the SAME frame, so no photo->fb mapping is needed. Painted
         * after the join (both cores done), before the copy. */
#if 0
        /* Round 7: SPLIT-BOUNDARY NOTCH - paint fb column saturn_split_x,
         * rows 4..15, magenta so footage shows the adaptive split's live
         * position ON SCREEN. Diagnostic only: in the clean build it
         * displayed as the 1-fb-col magenta bar at screen x ~164, rows
         * 4..15, every frame. */
        {
            extern volatile int saturn_split_x;
            int                 ny;

            for (ny = 4; ny < 16; ny++)
                I_VideoBuffer[ny * FB_W + saturn_split_x] = 0xFC;
        }
#endif
#if SATURN_FB_TESTPAT
        /* One-boot display-geometry probe: overwrite the fb with 4-column
         * index bands (16 + col/4, DOOM PLAYPAL greys) + a magenta row
         * every 32 lines. What Kronos shows maps screen-x -> fb-column
         * directly: black = unwritten/stale page, band edges = stretch
         * factor, band k at screen x = the live mapping. Diagnostic only. */
        {
            int px, py;

            for (py = 0; py < SATURN_FB_LINES; py++)
                for (px = 0; px < FB_W; px++)
                    I_VideoBuffer[py * FB_W + px] =
                        (byte)((py & 31) == 0 ? 0xFC : (0x10 + (px >> 2)));
        }
#endif
        /* Opaque letterbox (2026-09-04, the "cyan staircase"): the
         * 320x180 crop leaves fb pixels unpainted (index 0), and index
         * 0 is TRANSPARENT in bitmap mode - any layer still enabled
         * behind NBG0 (NBG1's boot bitmap, the VDP1 layer still holding
         * the boot font's last frame - jo_core_run never runs in this
         * port, so nothing ever clears it) showed through as colour
         * noise. slScrAutoDisp(NBG0ON) does not clear the sprite bit,
         * so the bleed must be made impossible instead: PLAYPAL[247]
         * is a second pure black - repaint every index-0 pixel with it
         * so NBG0 is opaque edge to edge. A legitimately black rendered
         * texel also matches - same colour, no visual change. */
        do {
            pf_v0 = saturn_vbl_count;
            pf_f0 = SAT_FRT();
        } while (__builtin_expect(saturn_vbl_count != pf_v0, 0));
        {
            extern int saturn_game_started;
            if (!saturn_game_started)
            {
                unsigned int       *w   = (unsigned int *)I_VideoBuffer;
                const unsigned int  blk = 0xF7F7F7F7u;
                int                 wi;

                for (wi = 0; wi < FB_W * SATURN_FB_LINES / 4; wi++)
                    if (w[wi] == 0)
                        w[wi] = blk;
            }
        }
        /* Freeze-forensics: D = pre-commit work done (fps/strip/CRAM),
         * next come the letterbox scan + the full-page copy_back. */
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char drow[2];

            drow[0] = 'D'; drow[1] = 0;
            Saturn_StageTextHi(108, 186, drow);
        }
        saturn_vdp2_copy_back();
        saturn_pf_acc[3] += Saturn_ProfSpan(pf_f0, pf_v0);
        /* Freeze-forensics: E = the full-page copy_back completed
         * (every glyph painted before it is now erased from the
         * displayed page). */
        {
            extern void Saturn_StageTextHi(int x, int y, const char *s);
            char erow[2];

            erow[0] = 'E'; erow[1] = 0;
            Saturn_StageTextHi(128, 186, erow);
        }
        /* VDP1 scaled-sprite wall prototype (2026-09-06): one-time
         * init (sprite-layer priorities + texture upload) on the first
         * committed frame, then the per-frame emit — re-arm the
         * erase-write, build the command table at VDP1 VRAM offset 0,
         * kick PTMR. Painted glyphs land AFTER copy_back, so the V1
         * HUD row survives this frame's flip (same window as D/E). */
        {
            static int v1w_first = 0;

            if (!v1w_first)
            {
                v1w_first = 1;
                Saturn_Vdp1Wall_Init();
            }
            do {
                pf_v0 = saturn_vbl_count;
                pf_f0 = SAT_FRT();
            } while (__builtin_expect(saturn_vbl_count != pf_v0, 0));
            v_pre_vdp1 = pf_v0;
            pre_vdp1_in_vbl = ((*(volatile unsigned short *)0x25F80004u) & 0x0008u) != 0;
            Saturn_Vdp1Wall_Frame();
            saturn_pf_acc[2] += Saturn_ProfSpan(pf_f0, pf_v0);
            /* (The 2026-09-06 CANARY row is retired: the VDP1 quad
             * renders on screen since the CTRL END-bit fix, so the
             * call chain into saturn_vdp1.o is proven.) */
        }
        saturn_prof_exp = (int)(saturn_vbl_count - t0);
        saturn_prof_exp_us = (unsigned short)Saturn_ProfSpan(tf, t0);
    }

        {
            unsigned short tf;
            do {
                t0 = saturn_vbl_count;
                tf = SAT_FRT();
            } while (__builtin_expect(saturn_vbl_count != t0, 0));

            {
                extern volatile unsigned int saturn_blit_frt_acc;
                extern unsigned int          saturn_blit_frt_last;
                extern volatile int          saturn_seg_bailed;
                unsigned int v0 = t0;
                pf_f0 = tf;
                pf_v0 = v0;
                unsigned int last_ = (unsigned int)pf_f0;
                unsigned int acc_  = 0;

                /* In VDP1.sv (lines 2675-2695), SCU VBlank-IN (saturn_vbl_count++)
                 * fires at Scanline 224 (14.20 ms), whereas VDP1 FRAME_CHANGE
                 * (buffer swap & command walk start) fires at VBlank-OUT (Scanline 262,
                 * 16.67 ms — 39 scanlines / 2.47 ms AFTER VBlank-IN).
                 * If at least one new VBlank has started since the previous frame's flip
                 * (v0 != last_flip_vbl) AND either VBlank-IN fired inside
                 * Saturn_Vdp1Wall_Frame() (v0 != v_pre_vdp1) or the raster beam is
                 * currently inside VBlank (VDP2 TVSTAT bit 3 at 0x25F80004u, Scanlines
                 * 224..262, when VBlank-IN fired during saturn_vdp2_copy_back() or at
                 * the tail end of Saturn_HW_RenderPlayerView()), flip NBG0 immediately
                 * instead of waiting a redundant 16.67 ms VBlank! */
                static unsigned int last_flip_vbl = 0xFFFFFFFFu;
                int in_new_vblank = (v0 != last_flip_vbl) &&
                                    (v0 != v_pre_vdp1 || pre_vdp1_in_vbl ||
                                     ((*(volatile unsigned short *)0x25F80004u) & 0x0008u) != 0);
                if (!in_new_vblank)
                {
                    while (saturn_vbl_count == v0)
                    {
                        extern volatile unsigned int saturn_vbl_dead;

                        if (saturn_vbl_dead)
                            break;   /* r57: ISR dead — no vblank will ever come */
                        {
                            unsigned int now_ = (unsigned int)SAT_FRT();
                            if (now_ >= last_)
                                acc_ += now_ - last_;
                            last_ = now_;
                            if (acc_ > 1292160u)
                            { saturn_seg_bailed = 10; break; }
                        }
                    }
                }
                last_flip_vbl = saturn_vbl_count;
                saturn_blit_frt_acc += acc_;
                saturn_blit_frt_last = last_;
                saturn_pf_acc[4] += Saturn_ProfSpan(pf_f0, pf_v0);
            }
        saturn_frt_syn_syn = (int)Saturn_ProfSpan(tf, t0);
        saturn_master_site = 0xD;   /* r53: flip-write entered */
        slBitMapNbg0(COL_TYPE_256, BM_512x256,
                     (void *)(saturn_fb_front ? SATURN_FB_PAGE_A
                                              : SATURN_FB_PAGE_B));
        {
            extern int saturn_v1w_spr;
            extern int saturn_vdp2_plane_active;

            jo_set_displayed_screens((jo_scroll_screen)
                (JO_NBG0_SCREEN
                 | (saturn_v1w_spr ? JO_SPRITE_SCREEN : 0)
                 | (saturn_vdp2_plane_active ? JO_RBG0_SCREEN : 0)));
        }
        saturn_fb_front ^= 1;
        saturn_prof_syn = (int)(saturn_vbl_count - t0);
        saturn_prof_syn_us = (unsigned short)(SAT_FRT() - tf);
    }
#else
    {
        unsigned short tf = SAT_FRT();
        for (n = 0; n < DOOM_SCREENHEIGHT; n++)
    {
        const unsigned char *srow  = src8 + n * FB_W;
        unsigned int        *drow32;
        int                  i;

        drow32 = (unsigned int *)(saturn_gfx_l->color_buffer
                                  + n * SATURN_HALF_WIDTH);
        for (i = 0; i < SATURN_HALF_WIDTH / 2; i++)
            drow32[i] = saturn_palette32[*srow++];
        drow32 = (unsigned int *)(saturn_gfx_r->color_buffer
                                  + n * SATURN_HALF_WIDTH);
        for (i = 0; i < SATURN_HALF_WIDTH / 2; i++)
            drow32[i] = saturn_palette32[*srow++];
    }

    /* The CPU writes above were cached. Purge OC BEFORE the flushes:
     * SCU DMA that `flush` issues to copy color_buffer -> VDP1 VRAM
     * bypasses SH-2 OC, so any dirty lines still sitting in this CPU's
     * OC when the DMA fires would be read stale. The purge used to run
     * AFTER both flushes Ã¢â‚¬â€ i.e. after any DMA that mattered Ã¢â‚¬â€ leaving
     * the DMA reading whatever cachable lines happened to be clean.
     * `jo_software_renderer_flush` is a black box; assume the worst
     * case and be coherent in time ourselves. */
    saturn_oc_purge();
    saturn_prof_exp = (int)(saturn_vbl_count - t0);
    saturn_prof_exp_us = (unsigned short)(SAT_FRT() - tf);
    }   /* expand micro-profile block */

    /* Step 2: DMA both color_buffers to VDP1 VRAM (sprite pixels). */
    t0 = saturn_vbl_count;
    {
        unsigned short tf = SAT_FRT();
        jo_software_renderer_flush(saturn_gfx_l);
        jo_software_renderer_flush(saturn_gfx_r);
        saturn_prof_dma = (int)(saturn_vbl_count - t0);
        saturn_prof_dma_us = (unsigned short)(SAT_FRT() - tf);
    }

    /* Step 3: VDP1 frame commit. Under JO_COMPILE_USING_SGL, this is
     * slSynch() (which waits for VBlank and flushes the SGL command
     * table); under the non-SGL path we have to inline the buffer
     * reset + draw + flush + PTMR trigger sequence. */
    t0 = saturn_vbl_count;
    {
        unsigned short tf = SAT_FRT();
        /* r53: the terminal wedge (r52 record: kind-8 fired, frame 82
         * then stayed open ~60 s with every render acc healthy) must
         * be localised inside this commit tail. 0xD = flip write,
         * 0xE = slSynch (SATURN_VDP1_FLUSH) â€” slSynch has NO bounded
         * net; if the frame wedges with S=0xE, slSynch never returned
         * and SGL's vblank sync is the disease. */
        saturn_master_site = 0xE;   /* r53: slSynch entered */
        SATURN_VDP1_FLUSH();
        /* Flush SQ one more time so any writes pending from slSynch or
         * jo_vdp1_* path land before we return to DOOM's main loop. */
        saturn_sq_flush();
        saturn_prof_syn = (int)(saturn_vbl_count - t0);
        saturn_prof_syn_us = (unsigned short)(SAT_FRT() - tf);
    }
#endif

    saturn_prof_fin_us = (unsigned short)(SAT_FRT() - fi0);
    /* Round-50: frame complete â€” the freeze watch is disarmed until
     * the next frame's build arm re-opens the window. */
    saturn_master_site = 0xC;
    {
        extern volatile unsigned int saturn_frame_open;
        extern volatile unsigned int saturn_frame_done_vbl;

        saturn_frame_open     = 0;
        saturn_frame_done_vbl = saturn_vbl_count;   /* r56: gap-net zero */
    }
}

void I_WaitVBL(int count)
{
    unsigned int target = saturn_vbl_count + (unsigned int)count;
    while (saturn_vbl_count < target)
    {
        /* Jo Engine's VBlank callback fires on the SH-2 master CPU between
         * iterations; do not burn cycles with busy-tick polling here. */
    }
}

/* Saturn_ShowTitle Ã¢â‚¬â€ minimal TITLEPIC blit for boot verification. */
extern void *W_CacheLumpName(const char *name, int tag);
extern int   W_LumpLength(int lump);
void Saturn_ShowTitle(void)
{
    static const char *const candidates[] = {
        "TITLEPIC", "TITLE", "DMENUPIC", "CREDIT", NULL
    };
    const byte *pic;
    const byte *playpal_data;
    int         pic_lump = -1;
    int         pal_lump = -1;
    int         i;

#if SATURN_VDP2_FB
    if (!saturn_fb_live)
        return;
#else
    if (saturn_gfx_l == NULL || saturn_gfx_r == NULL)
    {
        jo_printf(0, 0, "no gfx");
        return;
    }
#endif
    for (i = 0; candidates[i]; i++)
    {
        extern int W_CheckNumForName(const char *name);
        int lump = W_CheckNumForName((char *)candidates[i]);
        if (lump >= 0)
        {
            pic_lump = lump;
            break;
        }
    }
    if (pic_lump < 0)
    {
        jo_printf(0, 0, "no TITLEPIC");
        return;
    }
    pic = (const byte *)W_CacheLumpName((char *)candidates[i], 1 /* PU_STATIC */);
    {
        extern int W_CheckNumForName(const char *name);
        pal_lump = W_CheckNumForName("PLAYPAL");
    }
    if (pal_lump >= 0)
    {
        playpal_data = (const byte *)W_CacheLumpName("PLAYPAL", 1 /* PU_STATIC */);
        I_SetPalette(playpal_data);
    }
    if (pic)
    {
        int n = W_LumpLength(pic_lump);
        /* Half-width fb: a 320-wide TITLEPIC's raw bytes would
         * overflow the 160x200 buffer Ã¢â‚¬â€ cap at its size (the picture
         * reads as sheared but this is a boot-only aid). */
        if (n > FB_W * DOOM_FBHEIGHT)
            n = FB_W * DOOM_FBHEIGHT;
        memcpy(I_VideoBuffer, pic, (unsigned)n);
        I_FinishUpdate();
    }
    /* Hold the title on screen for ~3 seconds. */
    I_WaitVBL(60 * 3);
}
