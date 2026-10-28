/* saturn_vdp1.c — VDP1 scaled-sprite wall prototype (2026-09-06).
 * See saturn_vdp1.h for the design. Everything no-ops unless the
 * build defines SATURN_VDP1_WALL. */
#include "doomdef.h"
#include "i_system.h"
#include "saturn_memmap.h"
#include "saturn_cmdbuf.h"
#include "saturn_vdp1.h"
#include "saturn_tex.h"
#include "r_local.h"    /* texture/flat translation + counts (tag tables) */

#ifdef SATURN_VDP1_WALL

/* Stage 3a sprite sources (saturn_tex.c): the composited 8bpp pixels
 * (texel 0 = structural transparency) and the patch geometry. */
extern byte *Saturn_SpriteData(int s);
extern byte *Saturn_SpritePatch(int s, int *w, int *h, int *lofs,
                                int *tofs);

/* VDP1 register block. THE SATURN MAP (jo/sega_saturn.h:89-108, Sega
 * VDP1 manual §4): VRAM = 0x25C00000, registers = 0x25D00000, FB
 * readable at 0x25C80000. The VDP1's INTERNAL map (0x100004 PTMR etc.)
 * is NOT the SH-2 map — r5 wrote "registers" at 0x25C00010 = VRAM
 * bytes the command table overwrote; PTMR never landed, so no draw
 * ever started (empty FB, EDSR=0, four blind r1-r4 experiments). */
#define V1_REG_BASE      SATURN_VDP1_REGS
#define V1_REG(x)        (*(volatile unsigned short *)(V1_REG_BASE + (x)))
#define V1_TVMR          V1_REG(0x00)    /* TV mode + VBE (bit 3)        */
#define V1_FBCR          V1_REG(0x02)    /* FCM (bit 1) | FCT (bit 0)    */
#define V1_PTMR          V1_REG(0x04)    /* plot trigger (2 = auto)      */
#define V1_EWDR          V1_REG(0x06)    /* erase write data             */
#define V1_EWLR          V1_REG(0x08)    /* erase upper-left             */
#define V1_EWRR          V1_REG(0x0A)    /* erase lower-right            */
#define V1_EDSR          V1_REG(0x10)    /* draw end status (read)       */
#define V1_LOPR          V1_REG(0x12)    /* prev frame last cmd / 8      */
#define V1_COPR          V1_REG(0x14)    /* curr frame cmd / 8           */
#define V1_MODR          V1_REG(0x16)    /* mode status (read)           */
/* Command table + texture live in VRAM (0x25C00000). */
#define V1_BASE          SATURN_VDP1_VRAM
#define V1_VRAM(x)       (*(volatile unsigned short *)(V1_BASE + (x)))
/* VDP1 framebuffers (CPU-readable, jo/sega_saturn.h JO_VDP1_FB +
 * the second half of the 1 MB VDP1 address space): FB0 = 0x25C80000,
 * FB1 = 0x25CC0000, each 512x256 16-bit pixels. */
#define V1_FB0           SATURN_VDP1_FB0
#define V1_FB1           SATURN_VDP1_FB1

/* Command table lives at VDP1 VRAM offset 0 (VDP1 executes from there).
 * SGL's own lists also target offset 0, but in-game nothing queues
 * commands — and even if SGL's VBlank int rewrites the area at vblank,
 * our draw already ran mid-frame. */
#define V1_CMD           0u
/* Wall/flat textures: 8bpp, ROW-major (the software path's composited
 * format, saturn_tex.c), 2 texels per VRAM word, first/leftmost texel
 * in the HIGH byte (VDP1 is big-endian). SRCA = VDP1 VRAM byte
 * address / 8.
 *
 * Stage 3a (2026-09-08) sprite textures use the same 8bpp composited
 * format (Saturn_SpriteData row-major, texel 0 = structural
 * transparency, 247 = real black), but patch widths are NOT multiples
 * of 8 — VDP1 SIZE.X is in 8-texel units, so every sprite uploads
 * PADDED to a multiple of 8 texels (pad texels = 0 = transparent; the
 * quad extends over the pad and the extra columns draw nothing).
 *
 * VDP1 VRAM plan (0x25C00000, 512 KB total) — 2026-10-05 rework:
 *   0x00000..0x0007F   SGL system command slots 0..3
 *   0x00080..0x0FFFF   our user command table (capped at V1_CMD_LIMIT
 *                      by v1_write_commands — it can never spill into
 *                      the texture pages)
 *   0x10000..0x47FFF   arena PAGE 0 (224 KB)
 *   0x48000..0x7FFFF   arena PAGE 1 (224 KB)
 * Each page is ONE bump allocator shared by walls, flats and sprites
 * (no fixed split, so a texture-heavy view can't starve on an empty
 * sprite region or vice versa). Pages alternate per COMMITTED frame:
 * the CPU uploads frame N+1 into one page while VDP1 keeps
 * rasterising frame N's command list, which references only the other
 * page (see Saturn_Vdp1Wall_Reset). The old direct-mapped slot classes
 * (class A/B walls, S/M/L sprites) and the 64x64 checker at 0x40000
 * are retired — the per-frame arena replaced them and overwrote the
 * checker anyway; quads with unusable textures are SKIPPED. */
#define V1_CMD_LIMIT       0x10000u
#define V1_PAGE0_START     0x10000u
#define V1_PAGE0_END       0x80000u
#define V1_PAGE1_START     0x80000u
#define V1_PAGE1_END       0x80000u

/* Distance shading (2026-09-07, revised): shaded CRAM banks, NOT VDP1
 * gouraud rates. VDP1 gouraud is ADDITIVE per 5-bit channel (rate 16 =
 * no change) — it cannot reproduce DOOM's multiplicative COLORMAP fade
 * (0..30 rows = down to ~6% brightness) and blows dark texels out to
 * white at negative rates (the forced-shade build's white strip). The
 * scheme instead bakes the COLORMAP into CRAM: banks 1..7 hold the
 * active palette remapped through COLORMAP rows 4k (i_video.c's
 * Saturn_Cram_ShadedBanks), so a quad at CMDCOLR = k*256 displays
 * BIT-IDENTICAL to the software paint at shade row 4k. Per-quad bank =
 * nearest k to the walk's own shade (max error 2 colormap rows, at the
 * k boundaries). CMDCOLR for CL256Bnk is the START COLOR INDEX (Jo's
 * sprites.h:367: color_table_index = palette_id * 256), so bank k =
 * k << 8. PMOD stays non-gouraud; the 0x70000 table area is unused. */
#define TW               64
#define TH               64
/* Textures up to these dimensions get real uploads; anything wider or
 * taller is refused (the quad is skipped). Width must be a multiple of
 * 8 (VDP1 SIZE.X is in 8-texel units). */
#define V1_TEX_MAX_W     256
#define V1_TEX_MAX_H     256

/* SGL priority call: SL_DEF.H defines slPriorityNbg0(n) etc. as
 * macros over slPriority(screen, n); the screen codes are #defines
 * (scnNBG0 = 1, scnSPR0..7 = -7..-2). Declared with plain short —
 * doomdef.h doesn't pull SGL's typedefs and the ABI is identical. */
extern void slPriority(short screen, unsigned short num);
/* SGL sprite-mode + synch APIs (SL_DEF.H:1047/1049/1835/1838). */
extern void slSpriteColMode(unsigned short mode);
extern void slSpriteType(unsigned short type);
extern void slInitSynch(void);
extern void slSynchFunction(void (*func)(void));
#define SPR_PAL_RGB 1u                  /* SL_DEF.H:609 — mixed mode  */

#define SCN_NBG0      1
/* SGL's real sprite screen codes (SL_DEF.H:661-668) — scattered, not
 * a contiguous run. All 8 set to priority 5 so every sprite-type
 * pixel routes above NBG0 (3) whichever screen it lands on. */
static const short scn_spr[8] = { -7, -8, -5, -6, -3, -4, -1, -2 };

/* Set by Init: i_video.c's flip path then ORs JO_SPRITE_SCREEN into
 * its per-frame jo_set_displayed_screens — without it the SPR display
 * bits are cleared EVERY frame (slScrAutoDisp(NBG0ON) replaces the
 * whole enable set) and the VDP1 layer never displays. That was the
 * r1 "quad armed but invisible" verdict (yss vdp1.yss decode:
 * commands correct, layer disabled). */
int saturn_v1w_spr;

/* NBG0 bitmap pages (same addresses as i_video.c:177-178) — the VDP2
 * VRAM pages the StageText glyph channel paints. */
#define V1W_PAGE_A      0x25E00000u
#define V1W_PAGE_B      0x25E20000u

int  saturn_v1w_captured;
int  saturn_v1w_refuse;         /* Add refused a cell (texture/rows) —
                                 * paint-skip forensics (2026-09-07) */
int  saturn_v1w_cells;          /* cells actually stored this frame */
int  saturn_v1w_drop;           /* quads lost this frame to the table cap
                                 * or the command-slot budget (perf HUD X;
                                 * the farthest geometry goes first) */
int  saturn_v1w_slotdrop;       /* of the LAST committed list: quads cut
                                 * by v1_write_commands' slot budget */
volatile unsigned int saturn_prof_cef = 0;
volatile unsigned int saturn_prof_del = 0;
static int v1_inited;
/* Quad table (2026-09-07): up to 16 wall CELLS per frame (a cell is
 * one u-piece x one v-band of a wall), filled in walk order
 * (near-to-far) by Saturn_Vdp1Wall_Add; Frame() writes them far-to-
 * near so nearer quads draw last/on top. Per-quad texture: srca
 * (VDP1 VRAM /8, may carry a source-ROW offset) + SIZE (sizew/8 in
 * the high byte, rows in the low byte). 16 cells + END = VRAM
 * 0x80..0x2A0, well inside the command area. */
/* 32 cells + END = VRAM 0x80..0x4A0, still far under the texture
 * regions at 0x41000. */
/* 2026-09-08 CORRECTED: the 32-cell table is NOT the cap — the census
 * (S row 85 commits / 21 cells) held with the table at 256, i.e. it was
 * never full (21 < 32 even before). The 64 zero-cell commits die inside
 * saturn_v1w_commit (safe-range / degenerate-u, now counted in
 * saturn_v1w_kills) or in Add's texture refuses (saturn_v1w_refuse).
 * REVERTED to 32 (the 05:09-proven .bss/heap layout) until those kill
 * reasons are measured and fixed; only then does a bigger table pay.
 * 2026-09-09 FULL-VDP1 move: the cap lives in saturn_vdp1.h (64) and
 * is shared with the seg walk's sprite gate — the FULL gate admits the
 * why=8 walls, so the table must headroom past the old 6-cell census. */
/* Paint-skip forensics (2026-09-07): SATURN_V1W_QOFF shifts every
 * emitted quad (and its user-clip window) right by N px in the
 * COMMAND WRITER only — the software paint stays at the true
 * position. Ghost walls N px right of their true faces = the quads
 * draw; nothing shifted = the submission never reaches the screen. */
#ifndef SATURN_V1W_QOFF
#define SATURN_V1W_QOFF 0
#endif
/* P5 PHASE 1 (2026-09-10): the quad table left .bss for the cart zone
 * (Z_Malloc PU_STATIC in Saturn_Vdp1Wall_Init) and grew to the
 * full-VDP1 budget (V1W_MAX_QUADS = 512 cells = 14 KB). The rig's
 * worst observed scene was 67 quads; the capture pass + reason-8 clamp
 * phases need headroom past the old 64-cell .bss array. .bss SHRANK by
 * the old array's 1792 B -- the makefile pool rebate pays it back 1:1
 * (the ritual: __bend must return to 0x060C1030). A failed allocation
 * (the cartless z_zone_pool) leaves v1q_cap = 0: every Add hits the
 * table-full latch and the frame software-paints -- fail-open. */
short (*v1q)[16];        /* x0 y0 y3 x1 y1 y2 srca tw
                                         rows bank x3 clipx0 clipx1 flags
                                         x2 pad */
int   v1q_cap;           /* 0 = not allocated (software fallback) */
int   v1q_count;
static void v1w_table_try_alloc(void);
/* PERSISTENT PER-PAGE TEXTURE CACHE (2026-10-05 perf pass).
 * Before: every frame reset the arena and re-uploaded every visible
 * texture into VDP1 VRAM (hundreds of KB of B-bus writes per frame),
 * and every Add linear-searched a per-frame list for dedupe.
 * Now each double-buffer page keeps what it holds: a direct-mapped tag
 * table per page, indexed by the TRANSLATED texture / flat number or the
 * sprite cache index (all three have fixed content for the session —
 * nothing evicts tex_cache/flat_cache/sprite data mid-level), maps to
 * the VRAM address already holding it. A page is only re-filled when it
 * overflows (flush on its next use) or at level start
 * (Saturn_Vdp1Wall_FlushCache). Tag layout:
 *   [0 .. numtextures)                       walls (texturetranslation)
 *   [v1_tag_flat0 .. +numflats)              flats (flattranslation)
 *   [v1_tag_spr0  .. +numsprites)            sprites (scache)
 * srca 0 = not resident; V1_TAG_BAD = known-unusable (don't retry). */
v1_tag_t *saturn_v1_flat_tags = NULL;
v1_tag_t *saturn_v1_wall_tags = NULL;

#define V1_TAG_BAD 0xFFFFu      /* never a real SRCA: starts are 32-B aligned */

static v1_tag_t *v1_tag[2];     /* per page; NULL = not allocated yet */
static int       v1_tag_n, v1_tag_flat0, v1_tag_spr0;
static void      v1_tags_try_alloc(void);
void             Saturn_Vdp1_ClearTexCacheMRU(void);

/* Arena double-buffer state. v1_tex_page = the page the CPU is filling
 * this frame; v1_arena_off/end = its shared (wall+flat+sprite) bump
 * allocator. v1_page_committed = Frame() wrote a command list that
 * references v1_tex_page — only then may the next Reset flip pages
 * (a frame that commits nothing leaves VDP1 on the OLD list, which
 * still references the other page, so the CPU must stay where it is).
 * v1_page_off persists each page's bump offset across frames (the
 * cache); v1_page_flush marks a page that overflowed. */
static int             v1_tex_page = 0;
static int             v1_page_committed = 0;
static unsigned int    v1_arena_off = V1_PAGE0_START;
static unsigned int    v1_arena_end = V1_PAGE0_END;
static unsigned int    v1_page_off[2] = { V1_PAGE0_START, V1_PAGE1_START };
static unsigned int    v1_page_base[2] = { V1_PAGE0_START, V1_PAGE1_START };
static int             v1_page_flush[2] = { 1, 1 };
/* Preemptive flush: re-start a page whose free space dropped under this
 * when it comes back into use, so a growing working set doesn't hit a
 * mid-frame refusal (= missing walls for a frame).
 * Keep at 16 KB (1 max-size 128x128 texture) so scenes with a ~330 KB wall+flat
 * working set never fall into a catastrophic per-frame full-cache flush loop. */
#define V1_PAGE_LOWWATER  (16u * 1024u)
/* diagnostics for the HUD row */
int saturn_v1w_modr;            /* MODR readback (TVM/FCM/VBE bits)   */
int saturn_v1w_edsr = 3;        /* EDSR at HUD paint time             */
int saturn_v1w_copr = 0;        /* Last VDP1 quad slot index reached  */
int saturn_v1w_slot3_at_vbl;    /* VDP1 VRAM slot-3 CTRL at vblank cb */
int saturn_v1w_user0_at_vbl;    /* VDP1 VRAM user-slot-0 CTRL at vbl  */

/* VDP1 command words. Distorted sprite = function 2; END bit = CTRL
 * bit 15. PMOD (2026-09-07): MSBon DROPPED — for VDP2 sprite palette
 * types MSB=1 pixels route to transparency/color-calc, which would
 * hide a perfectly drawn quad (all r1-r10 evidence: command table
 * perfect, FB untouched, EDSR flags move). ECdis (0x0080, 0xFE/0xFF
 * texels are palette, not end codes), SPdis (0x0040, texel 0 draws as
 * palette black — wall textures use index 0), CL256Bnk (4<<3=0x0020).
 * COLR = shaded CRAM bank start index (k<<8); bank 0 = PLAYPAL. */
#ifndef PMOD_WALL
#define PMOD_WALL  (0x1000u | 0x0080u | 0x0040u | (4u << 3))
#endif
#define SIZE_64x64 (((TW / 8u) << 8) | TH)

/* SGL's VDP1 architecture (revealed by saturn-libs' Saturn PAL,
 * which mirrors the shipping arcade/bomberman pattern): SGL rebuilds
 * 4 system commands EVERY VBLANK in slots 0..3 (VRAM 0x00..0x7F) —
 * slot 0 System Clip, 1 User Clip, 2 Local Coord, 3 END — and runs
 * its own auto-draw from that table at vblank. NOTHING NEEDS PTMR.
 * Every earlier failure is explained: our command table lived at
 * VRAM 0 = SGL's system area, and SGL's vblank rebuild OVERWROTE
 * our slot-3 quad with END every frame; the walk was executing
 * SGL's 4-slot list all along (the EDSR wiggles) and halting there.
 *
 * The pattern that works: never touch slots 0..2; patch slot 3 from
 * END to LOCAL_COORD(0,0) (data-before-trigger) so the walk continues
 * into OUR slots 4..N; write user commands during the display period,
 * END LAST; wait for EDSR.CEF first so we never race the walk; and
 * re-patch every vblank (SGL rebuilds slot 3) via slSynchFunction. */
#define V1_SLOT_BYTES   0x20u
#define V1_SLOT3        (V1_CMD + 3 * V1_SLOT_BYTES)   /* SGL's END    */
#define V1_USER0        (V1_CMD + 4 * V1_SLOT_BYTES)   /* first slot we own */
/* user slots that fit below V1_CMD_LIMIT (arena page 0) */
#define V1_MAX_SLOTS    ((V1_CMD_LIMIT - V1_USER0) / V1_SLOT_BYTES)

/* User commands: the frame's captured wall quads (Saturn_Vdp1Wall_Add,
 * paint-both mode — the software walls still draw underneath, so
 * alignment is checkable). A = top-left, B = top-right, C = bot-right,
 * D = bot-left. CTRL = 0x0002, function 2 (distorted sprite), NO END
 * BIT. The END bit (CTRL bit 15) is checked when the walk READS the
 * command — before execution — so 0x8002 on a quad itself terminates
 * the list before that quad ever drew (the 13-invisible-builds bug).
 * END lives in its own 0x8000 command written after the last quad.
 * Quads are written far-to-near (array is near-to-far, so reversed):
 * VDP1 draws later commands on top, and a fully-visible farther face
 * is always a screen subset of the nearer one's, so the painter's
 * order is exact.
 *
 * U-PHASE CLIPPING (2026-09-07 round 3): a partial repetition piece
 * (first/last piece of a wall — VDP1 cannot source-clip u) is stored
 * EXTENDED to its full texture repetition with a user-clip rect = the
 * piece's true screen span. The writer emits a user-clip command
 * (CTRL 0x0008, A = top-left, C = bottom-right) before such a quad
 * and arms PMOD Window-In (2<<9); SGL's slot-1 user clip (rebuilt
 * every vblank before our slots run) resets the rect for the next
 * frame, and an explicit full-screen clip is inserted before any
 * unclipped quad that follows a clipped one in draw order. */
#define V1_PMOD_WINDOW_IN  ((unsigned short)(2u << 9))
/* CTRL LOW-NIBBLE COMMAND TYPES (authoritative: MAME saturn.cpp +
 * the ST-157 manual; confirmed live via the Kronos Debug VDP1 window,
 * 2026-09-09): 0x0002 = distorted sprite, 0x0008 = USER clipping
 * (rect = XA/YA top-left .. XC/YC bottom-right), 0x0009 = SYSTEM
 * clipping (rect = (0,0) .. XC/YC), 0x000A = local coordinates,
 * 0x8000 = draw end. The old V1_CMD_USERCLIP = 0x0009 was SYSTEM
 * clipping — and since the rect is read from XC/YC (0x14/0x16), which
 * this writer left ZERO, every clip command set a (0,0)-(0,0) clip
 * rect. Slot 1 sits at the head of the list, so EVERY draw in EVERY
 * frame was clipped to nothing — the root cause of the entire
 * "VDP1 never displays" campaign (the interleaved "System Clipping
 * x1=0,y1=0,x2=0,y2=0" entries in the Kronos VDP1 command list were
 * these commands). */
#define V1_CMD_USERCLIP    0x0008u
#define V1_CMD_SYSCLIP     0x0009u

static void v1_write_clip(unsigned short base, int x0, int y0, int x1, int y1)
{
    V1_VRAM(base + 0x00) = V1_CMD_USERCLIP;
    V1_VRAM(base + 0x02) = 0x0000;      /* LINK                 */
    V1_VRAM(base + 0x04) = 0x0000;      /* PMOD                 */
    V1_VRAM(base + 0x06) = 0x0000;      /* COLR                 */
    V1_VRAM(base + 0x08) = 0x0000;      /* SRCA                 */
    V1_VRAM(base + 0x0A) = 0x0000;      /* SIZE                 */
    V1_VRAM(base + 0x0C) = (unsigned short)x0;  /* A/XA: top-left  */
    V1_VRAM(base + 0x0E) = (unsigned short)y0;
    V1_VRAM(base + 0x10) = (unsigned short)x1;  /* B: bot-right (also
                                                   written below at C —
                                                   the hardware reads
                                                   XC/YC for clips)  */
    V1_VRAM(base + 0x12) = (unsigned short)y1;
    V1_VRAM(base + 0x14) = (unsigned short)x1;  /* C/XC: bot-right  */
    V1_VRAM(base + 0x16) = (unsigned short)y1;
    V1_VRAM(base + 0x18) = 0;           /* D                    */
    V1_VRAM(base + 0x1A) = 0;
    V1_VRAM(base + 0x1C) = 0;           /* GRDA                 */
    V1_VRAM(base + 0x1E) = 0;           /* DMMY                 */
}

static void v1_write_commands(void)
{
    int n, n_first, slot = 0, clip_open = 0;

    /* Slot budget (2026-10-05): with V1W_MAX_QUADS = 1024 and a clip
     * command per u-clipped piece (+ a reset clip after each), the worst
     * case is ~2050 slots = 0x100C0 bytes — past V1_CMD_LIMIT (arena
     * page 0 starts there) and past the 16-bit `base` arithmetic. Find
     * the FARTHEST quad that still fits, counting quads from the near
     * end (v1q[0] = weapon/nearest): cost(n) = 1 + 1 if quad n is
     * clipped or the quad drawn just before it (n + 1) was — exactly the
     * clip / reset-clip emission rule below. 3 slots stay reserved for
     * the initial clip, the END marker and the optional test canary. */
    {
        int budget = (int)V1_MAX_SLOTS - 3;
        if (v1q_count * 2 <= budget)
        {
            n_first = v1q_count - 1;
            saturn_v1w_slotdrop = 0;
        }
        else
        {
            int need = 0;
            for (n = 0; n < v1q_count; n++)
            {
                int clipped      = (v1q[n][11] != v1q[n][12]);
                int clipped_prev = (n + 1 < v1q_count)
                                && (v1q[n + 1][11] != v1q[n + 1][12]);

                need += 1 + ((clipped || clipped_prev) ? 1 : 0);
                if (need > budget)
                    break;
            }
            n_first = n - 1;
            saturn_v1w_slotdrop = v1q_count - n;
        }
    }

    /* slot 0: Initialize user clip to 3D viewport (0, 0) .. (319, 191).
     * Rows 192..223 are the VDP2 NBG0 status bar and must never be touched by VDP1. */
    v1_write_clip((unsigned short)(V1_USER0 + (unsigned)slot * V1_SLOT_BYTES),
                  0, 0, 319, 191);
    slot++;

#if defined(SATURN_V1W_TESTQUAD)
    /* CANARY rev 6 (2026-09-08 GUN DIAG): written FIRST = slot 0 =
     * drawn FIRST = UNDER every real quad, so nothing can hide it and
     * no fb->snap mapping guess matters. FULL-SCREEN (VDP1 x 0..400
     * covers the fb + wall QOFF, y 0..223) from the LAST-ADDED quad's
     * texture (a wall the scene already shows), bank 0, PMOD_WALL
     * (SPD on). Any STARTAN tint anywhere on the frame = the walk
     * entered our user slots. Screen 100% unchanged = the walk never
     * reaches slot 0 (everything visible is software paint). */
    if (v1q_count > 0)
    {
        unsigned short base = (unsigned short)(V1_USER0 + 0u * V1_SLOT_BYTES);

        V1_VRAM(base + 0x00) = 0x0002u;     /* CTRL: distorted      */
        V1_VRAM(base + 0x02) = 0x0000;      /* LINK                 */
        V1_VRAM(base + 0x04) = PMOD_WALL;   /* SPD on               */
        V1_VRAM(base + 0x06) = 0x0000;      /* COLR: bank 0         */
        V1_VRAM(base + 0x08) = (unsigned short)v1q[v1q_count - 1][6];
        V1_VRAM(base + 0x0A) = (unsigned short)
            ((((unsigned)v1q[0][7] / 8u) << 8) | 64u);
        V1_VRAM(base + 0x0C) = 0;           /* A tl                 */
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x10) = 400;         /* B tr                 */
        V1_VRAM(base + 0x12) = 0;
        V1_VRAM(base + 0x14) = 400;         /* C br                 */
        V1_VRAM(base + 0x16) = 223;
        V1_VRAM(base + 0x18) = 0;           /* D bl                 */
        V1_VRAM(base + 0x1A) = 223;
        V1_VRAM(base + 0x1C) = 0;
        V1_VRAM(base + 0x1E) = 0;
        slot = 1;
    }
#endif
    for (n = n_first; n >= 0; n--)
    {
        unsigned short base = (unsigned short)
            (V1_USER0 + (unsigned)slot * V1_SLOT_BYTES);
        int qx0 = v1q[n][0] + SATURN_V1W_QOFF;
        int qx1 = v1q[n][3] + SATURN_V1W_QOFF;
        int qx2 = v1q[n][14] + SATURN_V1W_QOFF;
        int qx3 = v1q[n][10] + SATURN_V1W_QOFF;

        if (v1q[n][11] != v1q[n][12])       /* clipped piece        */
        {
            v1_write_clip(base, v1q[n][11] + SATURN_V1W_QOFF, 0,
                          v1q[n][12] + SATURN_V1W_QOFF, 191);
            base += V1_SLOT_BYTES;
            slot++;
            clip_open = 1;
        }
        else if (clip_open)
        {
            /* a clipped quad precedes this one in draw order: reset
             * the user clip to the full 3D view for this quad       */
            v1_write_clip(base, 0, 0, 319, 191);
            base += V1_SLOT_BYTES;
            slot++;
            clip_open = 0;
        }

        volatile unsigned int *cmd32 = (volatile unsigned int *)(V1_BASE + base);
        unsigned int pmod = ((v1q[n][13] & 1) ? (PMOD_WALL & ~0x0040u) : PMOD_WALL) | V1_PMOD_WINDOW_IN;
        unsigned int colr = (unsigned int)v1q[n][9] << 8;
        unsigned int srca = (unsigned short)v1q[n][6];
        unsigned int size = (((unsigned)v1q[n][7] / 8u) << 8) | (unsigned)v1q[n][8];

        cmd32[0] = 0x00020000u;     /* CTRL: distorted only (0x0002), LINK (0x0000) */
        cmd32[1] = (pmod << 16) | (colr & 0xFFFFu);
        cmd32[2] = (srca << 16) | (size & 0xFFFFu);
        cmd32[3] = ((unsigned int)(unsigned short)qx0 << 16) | (unsigned short)v1q[n][1];
        cmd32[4] = ((unsigned int)(unsigned short)qx1 << 16) | (unsigned short)v1q[n][4];
        cmd32[5] = ((unsigned int)(unsigned short)qx2 << 16) | (unsigned short)v1q[n][5];
        cmd32[6] = ((unsigned int)(unsigned short)qx3 << 16) | (unsigned short)v1q[n][2];
        cmd32[7] = 0;               /* GRDA (0x0000), DMMY (0x0000) */
        slot++;
    }
#if defined(SATURN_V1W_TESTQUAD)
    /* CANARY rev 6 (2026-09-08 GUN DIAG): written AFTER every real
     * quad = drawn LAST = OVER everything, bottom half of the frame
     * (VDP1 y 112..223), from the WEAPON quad's own texture
     * (v1q[0][6], SPD on, bank 0). Unmissable in ANY fb->snap mapping:
     *   bottom half solid BLACK  = walk complete + weapon texture
     *                              staged EMPTY (the root cause);
     *   bottom half shows the gun's texel pattern = walk complete +
     *                              weapon texture fine (then only the
     *                              weapon quad's PMOD/COLR differ);
     *   bottom half unchanged    = walk stops before the last slot
     *                              (prime suspect: the psprite quad
     *                              command, drawn just before it). */
    if (v1q_count > 0)
    {
        unsigned short base = (unsigned short)
            (V1_USER0 + (unsigned)slot * V1_SLOT_BYTES);

        V1_VRAM(base + 0x00) = 0x0002u;     /* CTRL: distorted      */
        V1_VRAM(base + 0x02) = 0x0000;      /* LINK                 */
        V1_VRAM(base + 0x04) = PMOD_WALL;   /* SPD on               */
        V1_VRAM(base + 0x06) = 0x0000;      /* COLR: bank 0         */
        V1_VRAM(base + 0x08) = (unsigned short)v1q[0][6];
        V1_VRAM(base + 0x0A) = (unsigned short)
            ((((unsigned)v1q[0][7] / 8u) << 8) | 64u);
        V1_VRAM(base + 0x0C) = 112;         /* A tl (right of the   */
        V1_VRAM(base + 0x0E) = 112;         /*  census column)      */
        V1_VRAM(base + 0x10) = 400;         /* B tr                 */
        V1_VRAM(base + 0x12) = 112;
        V1_VRAM(base + 0x14) = 400;         /* C br                 */
        V1_VRAM(base + 0x16) = 223;
        V1_VRAM(base + 0x18) = 112;         /* D bl                 */
        V1_VRAM(base + 0x1A) = 223;
        V1_VRAM(base + 0x1C) = 0;
        V1_VRAM(base + 0x1E) = 0;
        slot++;
    }
#endif
    {
        unsigned short end = (unsigned short)
            (V1_USER0 + (unsigned)slot * V1_SLOT_BYTES);

    /* END marker, written LAST (after the quads) so the walk never
     * terminates early on a half-written list. */
    V1_VRAM(end + 0x00) = 0x8000u;      /* CTRL: END            */
    V1_VRAM(end + 0x02) = 0x0000;      /* LINK                 */
    V1_VRAM(end + 0x04) = 0x0000;      /* PMOD                 */
    V1_VRAM(end + 0x06) = 0x0000;      /* COLR                 */
    V1_VRAM(end + 0x08) = 0x0000;      /* SRCA                 */
    V1_VRAM(end + 0x0A) = 0x0000;      /* SIZE                 */
    V1_VRAM(end + 0x0C) = 0;           /* XA                   */
    V1_VRAM(end + 0x0E) = 0;           /* YA                   */
    V1_VRAM(end + 0x10) = 0;           /* XB                   */
    V1_VRAM(end + 0x12) = 0;           /* YB                   */
    V1_VRAM(end + 0x14) = 0;           /* XC                   */
    V1_VRAM(end + 0x16) = 0;           /* YC                   */
    V1_VRAM(end + 0x18) = 0;           /* XD                   */
    V1_VRAM(end + 0x1A) = 0;           /* YD                   */
    V1_VRAM(end + 0x1C) = 0;           /* GRDA                 */
    V1_VRAM(end + 0x1E) = 0;           /* DMMY                 */
    }
}

/* Patch SGL's slot-3 END into LOCAL_COORD(0,0) so the auto-draw walk
 * continues past slot 3 into our user region. Data (xa/ya) BEFORE the
 * CTRL flip. Called from the vblank callback (SGL rebuilds slot 3
 * every vblank) and from Frame() as belt-and-braces. */
static int v1_have_frame;

static void v1_patch_slot3(void)
{
    if (!v1_have_frame)
        return;                 /* no list yet — leave SGL's END */
    V1_VRAM(V1_SLOT3 + 0x0C) = 16;          /* local XA = 16 (centered 320 view in 352 screen) */
    V1_VRAM(V1_SLOT3 + 0x0E) = 0;           /* local YA */
    V1_VRAM(V1_SLOT3 + 0x00) = 0x000Au;     /* CTRL: local coord */
}

/* Belt-and-braces register refresh (saturn-libs pattern): sprite
 * priorities above NBG0 (3) and a transparent EWDR, re-written from
 * the vblank callback so SGL's per-frame register refresh cannot
 * undo them. PRISA..D = 0x5555 = every sprite register priority 5
 * (sprite type 0 palette pixels take PR bits 15-14 = 0 -> SPR0).
 * ADDRESS BUG FIXED 2026-09-07: these are VDP2 REGISTERS, which live
 * at 0x25F80000 (TVSTAT's PAL bit at 0x25f80004, jo/sega_saturn.h:366
 * JO_VDP2_PRISA = 0x25F800F0). The old 0x25F000F0..F6 base is CRAM
 * (SL_DEF.H VDP2_COLRAM = 0x25f00000 = our SATURN_FB_CRAM) — every
 * vblank it poisoned CRAM entries 120..123 with 0x5555 pink
 * (170,82,170), and those are exactly the nukage/computer-panel
 * texel indices, so all dark mid-range surfaces speckled pink. */
static void v1_write_priority_regs(void)
{
    /* Jo's own proven value (core.c:278 writes 0x506 once at init):
     * priority 5 in the fields that matter, layering identical to what
     * the game has always displayed (these vblank writes never reached
     * the real registers before the 2026-09-07 base-address fix, so
     * Jo's init value is the known-good config). */
    *(volatile unsigned short *)0x25F800F0u = 0x0506u;  /* PRISA */
    *(volatile unsigned short *)0x25F800F2u = 0x0506u;  /* PRISB */
    *(volatile unsigned short *)0x25F800F4u = 0x0506u;  /* PRISC */
    *(volatile unsigned short *)0x25F800F6u = 0x0506u;  /* PRISD */
    /* PRINA (0x25F800F8, NBG0 = low byte / NBG1 = high — jo/sega_saturn.h:370):
     * NBG0 priority MUST DROP BELOW the sprite layer or the sprite screen
     * sits UNDER the opaque NBG0 bitmap and no VDP1 pixel is ever visible.
     * The slPriority(SCN_NBG0, 3) call cannot be trusted for this — the
     * 2026-09-07 root cause proved SGL commits nothing in this config
     * (no frame flow, no register rebuild), so Jo's init value PRINA =
     * 0x607 (NBG0 = 7) would stay live forever: priority 7 over sprite
     * 5 = the VDP1-FB canary rasterized (G/I rows nonzero) but never
     * reached the screen. NBG1 = 6 keeps Jo's boot-stage layer above
     * NBG0 while it is still displayed. (First attempt wrote 0x25F80080
     * — the wrong register, jo header PRINA = 0x25F800F8.) */
    *(volatile unsigned short *)0x25F800F8u = 0x0603u;  /* PRINA  */
    *(volatile unsigned short *)0x25F800FCu = 0x0004u;  /* PRIR: RBG0 = 4 */
    /* No VDP1 register writes here (2026-09-09): EWDR included — SGL
     * owns the VDP1 register state end to end. */
}

extern void Saturn_VDP2_Plane_VBlank(void);

static void v1_vblank_callback(void)
{
    /* QUAD-DRAW FORENSICS (2026-09-07): sample the command table the
     * moment our callback runs, BEFORE re-patching. SGL's slot-3 word:
     *   0x8000 = SGL's END rebuild already ran (rebuild is AFTER this
     *            callback -> it will overwrite our patch again and the
     *            draw's walk stops at slot 3 — our slots never run);
     *   0x000A = our previous frame's patch survived (rebuild runs
     *            BEFORE the callback -> the patch below wins).
     * User slot 0 CTRL: 0x0002 = our quad table intact at vblank. */
    saturn_v1w_slot3_at_vbl = (int)V1_VRAM(V1_SLOT3 + 0x00);
    saturn_v1w_user0_at_vbl = (int)V1_VRAM(V1_USER0 + 0x00);
    v1_patch_slot3();
    v1_write_priority_regs();
    Saturn_VDP2_Plane_VBlank();
}

/* v1_arm_erase DELETED 2026-09-09: the 2026-09-07 "SGL never arms"
 * rework added per-frame TVMR(VBE)/FBCR/EWDR writes + a manual PTMR=1
 * kick — and rev-9/rev-10 (2026-09-08) proved that pipeline draws
 * NOTHING: with EWDR=0x0104 armed every frame the screen never went
 * white, and direct FB reads hold boot-era values only (0x006A at FB0/
 * FB1 word 0 — BIOS-logo remnants; the erase would have overwritten
 * them). Meanwhile the PROVEN 2026-09-06/07 pipeline — canary quads
 * VISIBLE in Kronos — wrote NO VDP1 registers at all ("never write
 * PTMR"): SGL arms and triggers its own draw. Both facts together
 * falsify "SGL never arms"; the register writes themselves were the
 * regression. (History: r6 used FCM=1 manual change and the vblank
 * erase landed on the buffer about to DISPLAY — vdp5.yss: table
 * perfect, FB 100% zero, screen blank.) */

/* v1_upload_texture (the synthetic 64x64 white/magenta checker, a
 * 2026-09-06 bring-up aid) DELETED 2026-10-05: nothing referenced its
 * SRCA any more, its 0x40000 home sat inside the per-frame texture
 * arena (overwritten on the first in-game frame), and any other home
 * collides with the command table or an arena page. */


void Saturn_Vdp1Wall_Init(void)
{
    if (v1_inited)
        return;
    v1_inited = 1;

    /* P5 Phase 1 (2026-09-10 BOOT-WEDGE FIX): NO allocation here. This
     * runs at the first COMMITTED frame — and the boot pump commits
     * frames BEFORE Z_Init (d_main.c: Saturn_PumpFrame at the boot
     * report loop, Z_Init after it) — so mainzone was still NULL and
     * the Z_Malloc hit Z_Malloc2's fail I_Error. I_Error's terminal
     * loop then spun in slSynch forever, because the synch callback
     * (slSynchFunction, further down this very function) was never
     * installed. The table allocates lazily in Frame() — see
     * v1w_table_try_alloc. */
    v1w_table_try_alloc();
#ifdef SATURN_WIN32
    v1_tags_try_alloc();
    return;
#endif


    /* First I_FinishUpdate = the game loop is live: silence every
     * other diagnostic HUD stamp (saturn_quiet_hud, i_video.c). Our
     * own row paints with the flag temporarily lifted. */
    {
        extern int saturn_quiet_hud;

        saturn_quiet_hud = 1;
    }

    /* Sprite layer ABOVE NBG0 (NBG0 opaque since the letterbox fix).
     * NBG1 is hidden every frame; its priority is irrelevant. */
    slPriority(SCN_NBG0, 3);
    {
        int s;

        for (s = 0; s < 8; s++)
            slPriority(scn_spr[s], 5);
    }
    saturn_v1w_spr = 1;

    /* SGL sprite mode + the vblank slot-3 patch (saturn-libs pattern,
     * 2026-09-07). slInitSynch arms SGL's synch-callback mechanism
     * (Jo Engine never registers one — the slot is free);
     * slSynchFunction then installs our callback, which re-patches
     * slot 3 and refreshes the priority registers every vblank, after
     * SGL's rebuild and before the auto-draw walks the list.
     * Sprite type 0 + mixed palette/RGB: our palette pixels (MSB=0)
     * take PR bits 15-14 = 0 -> sprite register 0 -> priority 5. */
    slInitSynch();
    slSynchFunction(v1_vblank_callback);
    slSpriteType(0u);
    slSpriteColMode(SPR_PAL_RGB);
    v1_write_priority_regs();
    /* Option A (168 lines default): X2 = 351 (44*8-1), Y2 = 167 (0x00A7), EWRR = 0x58A7.
     * Reclaims ~0.3 ms of VDP1 hardware auto-erase rasterizer time. */
    V1_EWRR = 0x58A7u;

    /* REWORK REVERTED (2026-09-09): the 2026-09-07 "SGL NEVER ARMS"
     * conclusion was falsified by its own successor's evidence — the
     * manual-kick pipeline it produced draws nothing (erase-color
     * probe + FB reads negative), while the register-silent 2026-09-06/
     * 07 pipeline displayed canary quads. SGL owns the VDP1 registers;
     * we write NO TVMR/FBCR/EWDR/EWLR/EWRR/PTMR anywhere (see the
     * v1_arm_erase deletion note above). The system-command writes
     * below stay as a safety net in case SGL does not rebuild slots
     * 0-3 in this config; if it does rebuild, ours are overwritten
     * harmlessly. The slot-3 END→LOCAL_COORD patch (callback + main
     * thread) is what lets the walk reach our user quads. */
    {
        unsigned short base;

        /* slot 0: system clip (0,0)-(511,255) — the full VDP1 canvas.
         * FIXED 2026-09-09: was CTRL 0x0008 = USER clipping (low-nibble
         * types!) with the rect read from XC/YC = (0,0) — combined with
         * slot 1's 0x0009 (= system clip, also XC/YC=0) the whole list
         * drew into zero clip rects. System clip = 0x0009, rect
         * (0,0) .. XC/YC, bottom-right written at C (and B, harmless). */
        base = 0x0000u;
        V1_VRAM(base + 0x00) = V1_CMD_SYSCLIP;
        V1_VRAM(base + 0x02) = 0x0000;
        V1_VRAM(base + 0x0C) = 0;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x10) = 511;
        V1_VRAM(base + 0x12) = 255;
        V1_VRAM(base + 0x14) = 511;     /* XC: system clip rect      */
        V1_VRAM(base + 0x16) = 255;     /*  = (0,0)..(511,255)       */
        /* slot 1: user clip = full 3D view (rows 0..191, cols 16..335) */
        v1_write_clip(0x0020u, 16, 0, 319 + 16, 191);
        /* slot 2: local coord (16,0) — 320 view centered in 352 screen */
        base = 0x0040u;
        V1_VRAM(base + 0x0C) = 16;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x00) = 0x000Au;
        /* slot 3: local coord too (the callback re-patches it)      */
        base = 0x0060u;
        V1_VRAM(base + 0x0C) = 16;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x00) = 0x000Au;
        /* REAL-HW BOOT WEDGE ROOT CAUSE (2026-09-09): v1_have_frame=1
         * below means from the FIRST vblank after this Init the
         * callback patches SGL's slot-3 END into LOCAL_COORD and the
         * auto-draw walk runs PAST slot 3 into our user region — which
         * on the title/menu path is EMPTY (quads never commit there;
         * v1_write_commands only runs when v1q_count > 0, so its END
         * marker at slot 4+N is never written either). The VDP1 then
         * executed UNWRITTEN power-up VRAM as commands: real HW drew
         * random polygons over the cyan STARTING fill (the dithered
         * curved garbage, photo 20260909_072106 — the sprite layer
         * shows the VDP1 FB at priority 5 above NBG0); Kronos
         * zero-fills VRAM so the same walk executed degenerate
         * zero-size draws and looked innocent. ARM THE STOP: write the
         * END marker at V1_USER0 now, so the walk terminates at an
         * empty list until the first quad commit overwrites slot 4
         * (v1_write_commands then writes its own END after the quads,
         * as always). */
        V1_VRAM(V1_USER0 + 0x00) = 0x8000u;  /* CTRL: END            */
        V1_VRAM(V1_USER0 + 0x02) = 0x0000;   /* LINK                 */
        v1_have_frame = 1;
    }

    /* WAD is fully loaded by the first in-game frame: bake the shaded
     * CRAM banks now (I_SetPalette keeps them current afterwards). */
    {
        extern void Saturn_Cram_ShadedBanks(void);

        Saturn_Cram_ShadedBanks();
    }
}

/* Allocate the two per-page tag tables in fast RAM (WRAM-H or WRAM-L)
 * once the WAD's texture / flat / sprite counts are known.
 * Completely avoids slow 16-bit A-bus Cart RAM. */
static void v1_tags_try_alloc(void)
{
    int n, bytes;

    if (v1_tag[0] != NULL || numtextures <= 0 || numflats <= 0 || numsprites <= 0)
        return;
    n     = (int)numtextures + (int)numflats + (int)numsprites;
    bytes = n * (int)sizeof(v1_tag_t);

    /* Allocate in fast 32-bit WRAM-H via Jo heap; fallback to WRAM-L scratch (14,232B < 16KB) */
    v1_tag[0] = (v1_tag_t *)jo_malloc_with_behaviour(bytes, 0);
    if (v1_tag[0] == NULL)
        v1_tag[0] = (v1_tag_t *)SATURN_WRAM_L_SCRATCH_ADDR;

    v1_tag[1]    = v1_tag[0];
    v1_tag_n     = n;
    v1_tag_flat0 = (int)numtextures;
    v1_tag_spr0  = (int)numtextures + (int)numflats;
    saturn_v1_flat_tags = &v1_tag[0][v1_tag_flat0];
    saturn_v1_wall_tags = &v1_tag[0][0];
    D_memset(v1_tag[0], 0, bytes);
    v1_page_flush[0] = v1_page_flush[1] = 1;
}

/* Bump-allocate `bytes` in the current page. 0 = page full: the quad is
 * refused THIS frame and the page is flushed the next time it comes
 * into use (flushing now would overwrite textures this frame's quads
 * already reference). */
static unsigned int v1_arena_alloc(int bytes)
{
    unsigned int off = v1_arena_off;

    if (off + (unsigned int)bytes > v1_arena_end)
    {
        v1_page_flush[v1_tex_page] = 1;
        saturn_v1w_refuse++;        /* HUD 'X': texture missing this frame */
        return 0;
    }
    v1_arena_off += (unsigned int)((bytes + 31) & ~31);
    return off;
}

/* Copy a contiguous row-major 8bpp block into VDP1 VRAM. The source's
 * byte order IS the VRAM layout (first texel = high byte of each
 * big-endian word), so this is a plain block copy: 32-bit stores when
 * the source is 4-aligned (bytes is a multiple of 8 for every caller —
 * widths are multiples of 8), else 16-bit assembled pairs. The old loop
 * rebuilt every word from two byte loads + shift + recomputed indices. */
static void v1_copy_block(unsigned int off, const byte *src, int bytes)
{
    extern uint32_t saturn_vdp1_bus_tex_bytes;
    saturn_vdp1_bus_tex_bytes += (uint32_t)bytes;
#ifdef SATURN_WIN32
    memcpy((void *)(V1_BASE + off), src, bytes);
#else
    if (((unsigned int)src & 3u) == 0)
    {
        const unsigned int    *s = (const unsigned int *)src;
        volatile unsigned int *d = (volatile unsigned int *)(V1_BASE + off);
        int                    n = bytes >> 2;

        while (n >= 4)
        {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            d += 4; s += 4; n -= 4;
        }
        while (n-- > 0)
            *d++ = *s++;
    }
    else
    {
        volatile unsigned short *d = (volatile unsigned short *)(V1_BASE + off);
        int                      n = bytes >> 1;

        while (n-- > 0)
        {
            *d++ = (unsigned short)((src[0] << 8) | src[1]);
            src += 2;
        }
    }
#endif
}

/* Copy an 8bpp texture transposed into 16-pixel vertical strips (Section 36) */
static void v1_copy_strip_major(unsigned int off, const byte *src, int w, int h)
{
    extern uint32_t saturn_vdp1_bus_tex_bytes;
    saturn_vdp1_bus_tex_bytes += (uint32_t)(w * h);
    int num_strips = (w + 15) / 16;
    int s;

    for (s = 0; s < num_strips; s++)
    {
        int cur_strip_w = (s == num_strips - 1) ? (w - s * 16) : 16;
        unsigned int strip_off = off + (unsigned int)(s * (16 * h));
#ifdef SATURN_WIN32
        uint8_t *dst = (uint8_t *)(V1_BASE + strip_off);
        int y, x;
        for (y = 0; y < h; y++)
        {
            const byte *sr = src + y * w + s * 16;
            for (x = 0; x < cur_strip_w; x++)
                dst[y * 16 + x] = sr[x];
            for (x = cur_strip_w; x < 16; x++)
                dst[y * 16 + x] = 0;
        }
#else
        volatile unsigned short *dst = (volatile unsigned short *)(V1_BASE + strip_off);
        int y, xw;
        for (y = 0; y < h; y++)
        {
            const byte *sr = src + y * w + s * 16;
            for (xw = 0; xw < 8; xw++)
            {
                int xl = xw * 2, xr = xl + 1;
                unsigned b_l = (xl < cur_strip_w) ? (unsigned)sr[xl] : 0u;
                unsigned b_r = (xr < cur_strip_w) ? (unsigned)sr[xr] : 0u;
                *dst++ = (unsigned short)((b_l << 8) | b_r);
            }
        }
#endif
    }
}

/* Copies a >=64-wide wall texture as contiguous 32 x h horizontally-downsampled
 * blocks (each covering 64 world texels at 2:1 horizontal LOD) followed by
 * 32 x (h/2) blocks, so 64-unit wall segments emit a single 32-wide VDP1 quad
 * with 50% to 75% fewer VDP1 DDA steps and only 0.75x extra VDP1 VRAM (1.75x total vs 2.75x). */
static void v1_copy_32_blocks(unsigned int off, const byte *src, int w, int h)
{
    int num_blocks = w >> 6;
    int h_half = h >> 1;
    unsigned int half_base = off + (unsigned int)((w >> 1) * h);
    int b, y;
    for (b = 0; b < num_blocks; b++)
    {
        unsigned int block_off = off + (unsigned int)(b * (32 * h));
        unsigned int half_off  = half_base + (unsigned int)(b * (32 * h_half));
        const byte *bsrc = src + b * 64;
#ifdef SATURN_WIN32
        uint8_t *dst = (uint8_t *)(V1_BASE + block_off);
        uint8_t *dst_h = (uint8_t *)(V1_BASE + half_off);
        int x;
        for (y = 0; y < h; y++)
        {
            const byte *sr = bsrc + y * w;
            uint8_t *dr = dst + y * 32;
            for (x = 0; x < 32; x++)
                dr[x] = sr[x * 2];
        }
        for (y = 0; y < h_half; y++)
        {
            const byte *sr = bsrc + (y * 2) * w;
            uint8_t *dr = dst_h + y * 32;
            for (x = 0; x < 32; x++)
                dr[x] = sr[x * 2];
        }
#else
        volatile unsigned short *dst = (volatile unsigned short *)(V1_BASE + block_off);
        volatile unsigned short *dst_h = (volatile unsigned short *)(V1_BASE + half_off);
        int xw;
        for (y = 0; y < h; y++)
        {
            const byte *sr = bsrc + y * w;
            for (xw = 0; xw < 16; xw++)
                *dst++ = (unsigned short)(((unsigned)sr[xw * 4] << 8) | (unsigned)sr[xw * 4 + 2]);
        }
        for (y = 0; y < h_half; y++)
        {
            const byte *sr = bsrc + (y * 2) * w;
            for (xw = 0; xw < 16; xw++)
                *dst_h++ = (unsigned short)(((unsigned)sr[xw * 4] << 8) | (unsigned)sr[xw * 4 + 2]);
        }
#endif
    }
}

/* Per-page persistent texture cache (walls: texnum >= 1; flats:
 * texnum = -(flatnum + 1)). Returns the SRCA of the texture in the
 * current page (uploading it on a miss) and its size; 0 = not drawable
 * (unusable texture, or page full this frame — counted by the caller in
 * saturn_v1w_refuse). */
static int v1_tex_get(int texnum, int *w, int *h)
{
    v1_tag_t   *tag;
    const byte *src;
    int         tw, th;
    unsigned    off;

    v1_tags_try_alloc();
    if (v1_tag[v1_tex_page] == NULL)
        return 0;

    if (texnum < 0)
    {
        int flatnum = (-texnum) - 1, f;

        if (flatnum >= (int)numflats)
            return 0;
        f = flattranslation ? (int)flattranslation[flatnum] : flatnum;
        if (f < 0 || f >= (int)numflats)
            f = flatnum;                /* Saturn_FlatData's own fallback */
        tag = &v1_tag[v1_tex_page][v1_tag_flat0 + f];
        if (tag->srca)
        {
            if (tag->srca == V1_TAG_BAD)
                return 0;
            *w = (int)tag->w8 << 3;
            *h = (int)tag->h;
            return (int)tag->srca;
        }
        src = Saturn_FlatData(flatnum);
        if (!src)
            return 0;                   /* not cached as BAD: may load later */
        tw = th = 64;
    }
    else
    {
        stexinfo_t ti;
        int        t;

        if (texnum == 0 || texnum >= (int)numtextures)
            return 0;
        t = texturetranslation ? (int)texturetranslation[texnum] : texnum;
        if (t < 0 || t >= (int)numtextures)
            return 0;
        tag = &v1_tag[v1_tex_page][t];
        if (tag->srca)
        {
            if (tag->srca == V1_TAG_BAD)
                return 0;
            *w = (int)tag->w8 << 3;
            *h = (int)tag->h;
            return (int)tag->srca;
        }
        if (!Saturn_TexInfo(texnum, &ti)
            || ti.data == NULL || ti.w < 8 || ti.h < 8
            || ti.w > V1_TEX_MAX_W || ti.h > V1_TEX_MAX_H
            || (ti.w & 7) != 0)
        {
            tag->srca = V1_TAG_BAD;     /* oversize / not /8: never usable */
            return 0;
        }
        src = ti.data;
        tw  = ti.w;
        th  = ti.h;
    }

    extern int saturn_wall_subdivision;
    if (texnum > 0 && saturn_wall_subdivision && tw > 16)
    {
        int strip_bytes = ((tw + 15) / 16) * 16 * th;
        unsigned int off_strip = v1_arena_alloc(strip_bytes);
        if (off_strip == 0)
            return 0;
        v1_copy_strip_major(off_strip, src, tw, th);
        tag->srca = (unsigned short)(off_strip / 8u);
        tag->srca_mono = tag->srca;
        tag->srca_64   = tag->srca;
        if (tw >= 64 && (tw & 63) == 0)
        {
            int bytes_32 = (tw >> 1) * th + (tw >> 1) * (th >> 1);
            if (v1_arena_off + (unsigned int)bytes_32 + (16u * 1024u) <= v1_arena_end)
            {
                unsigned int off_32 = v1_arena_alloc(bytes_32);
                if (off_32 != 0)
                {
                    v1_copy_32_blocks(off_32, src, tw, th);
                    tag->srca_mono = (unsigned short)(off_32 / 8u);
                }
            }
        }
        else if (v1_arena_off + (unsigned int)(tw * th) + (48u * 1024u) <= v1_arena_end)
        {
            unsigned int off_mono = v1_arena_alloc(tw * th);
            if (off_mono != 0)
            {
                v1_copy_block(off_mono, src, tw * th);
                tag->srca_mono = (unsigned short)(off_mono / 8u);
                if (tw == 32 && th <= 16 &&
                    v1_arena_off + (unsigned int)(64 * th) + (48u * 1024u) <= v1_arena_end)
                {
                    unsigned int off_64 = v1_arena_alloc(64 * th);
                    if (off_64 != 0)
                    {
                        int y;
                        for (y = 0; y < th; y++)
                        {
                            v1_copy_block(off_64 + (unsigned int)(y * 64), src + y * 32, 32);
                            v1_copy_block(off_64 + (unsigned int)(y * 64 + 32), src + y * 32, 32);
                        }
                        tag->srca_64 = (unsigned short)(off_64 / 8u);
                    }
                }
            }
        }
    }
    else if (texnum < 0 && tw == 64 && th == 64)
    {
        /* Flat hardware mipmap pyramid in VDP1 VRAM (5,440 bytes contiguous):
         *   +0    (srca + 0):   64x64 Level 0 (4,096 B, CMDSIZE 0x0840)
         *   +4096 (srca + 512): 32x32 Level 1 (1,024 B, CMDSIZE 0x0420)
         *   +5120 (srca + 640): 16x16 Level 2 (  256 B, CMDSIZE 0x0210)
         *   +5376 (srca + 672):   8x8 Level 3 (   64 B, CMDSIZE 0x0108) */
        off = v1_arena_alloc(5440);
        if (off == 0)
            return 0;
        v1_copy_block(off, src, 4096);
#ifdef SATURN_WIN32
        {
            uint8_t *dst32 = (uint8_t *)(V1_BASE + off + 4096u);
            uint8_t *dst16 = (uint8_t *)(V1_BASE + off + 5120u);
            uint8_t *dst8  = (uint8_t *)(V1_BASE + off + 5376u);
            int y, x;
            for (y = 0; y < 32; y++)
            {
                const byte *sr = src + (y * 2) * 64;
                for (x = 0; x < 32; x++)
                    dst32[y * 32 + x] = sr[x * 2];
            }
            for (y = 0; y < 16; y++)
            {
                const byte *sr = src + (y * 4) * 64;
                for (x = 0; x < 16; x++)
                    dst16[y * 16 + x] = sr[x * 4];
            }
            for (y = 0; y < 8; y++)
            {
                const byte *sr = src + (y * 8) * 64;
                for (x = 0; x < 8; x++)
                    dst8[y * 8 + x] = sr[x * 8];
            }
        }
#else
        {
            volatile unsigned short *dst32 = (volatile unsigned short *)(V1_BASE + off + 4096u);
            volatile unsigned short *dst16 = (volatile unsigned short *)(V1_BASE + off + 5120u);
            volatile unsigned short *dst8  = (volatile unsigned short *)(V1_BASE + off + 5376u);
            int y, xw;
            for (y = 0; y < 32; y++)
            {
                const byte *sr = src + (y * 2) * 64;
                for (xw = 0; xw < 16; xw++)
                    *dst32++ = (unsigned short)(((unsigned)sr[xw * 4] << 8) | (unsigned)sr[xw * 4 + 2]);
            }
            for (y = 0; y < 16; y++)
            {
                const byte *sr = src + (y * 4) * 64;
                for (xw = 0; xw < 8; xw++)
                    *dst16++ = (unsigned short)(((unsigned)sr[xw * 8] << 8) | (unsigned)sr[xw * 8 + 4]);
            }
            for (y = 0; y < 8; y++)
            {
                const byte *sr = src + (y * 8) * 64;
                for (xw = 0; xw < 4; xw++)
                    *dst8++ = (unsigned short)(((unsigned)sr[xw * 16] << 8) | (unsigned)sr[xw * 16 + 8]);
            }
        }
#endif
        tag->srca = (unsigned short)(off / 8u);
        tag->srca_mono = (unsigned short)(tag->srca + 512u);
        tag->srca_64   = (unsigned short)(tag->srca + 640u);
    }
    else
    {
        off = v1_arena_alloc(tw * th);
        if (off == 0)
            return 0;
        v1_copy_block(off, src, tw * th);
        tag->srca = (unsigned short)(off / 8u);
        tag->srca_mono = tag->srca;
        tag->srca_64   = tag->srca;
    }
    tag->w8   = (unsigned char)(tw >> 3);
    tag->h    = (unsigned char)th;
    *w = tw;
    *h = th;
    return (int)tag->srca;
}

/* Stage 3a sprite texture cache (master-half only — the prep pass
 * calls this; Saturn_SpriteData may lazily convert, which is fine on
 * the master). Returns the SRCA of the sprite's padded 8bpp texture
 * and its padded width; 0 = not drawable (no data, oversize, or
 * padded bytes over the largest class). SRCA of source row r is
 * base + (r * pw) / 8 — pw is a multiple of 8, so row offsets keep
 * the 8-byte SRCA quantum.
 * 2026-09-08 teal-wedge bisect: the whole sprite surface (this, Spr_Add,
 * PspMarked) compiles out under SATURN_3A_BYPASS. */
#ifndef SATURN_3A_BYPASS
#define SATURN_3A_BYPASS 0
#endif
#if !SATURN_3A_BYPASS
static int v1_spr_get(int scache, int *pw_out, int *h_out)
{
    byte     *pix;
    v1_tag_t *tag;
    int       w, h, lo, to, pw, bytes;
    unsigned  off;

    if (scache < 0)
        return 0;
    v1_tags_try_alloc();
    if (v1_tag[v1_tex_page] == NULL || scache >= (int)numsprites)
        return 0;

    /* Persistent per-page cache hit (sprite content is fixed per scache) */
    tag = &v1_tag[v1_tex_page][v1_tag_spr0 + scache];
    if (tag->srca)
    {
        if (tag->srca == V1_TAG_BAD)
            return 0;
        *pw_out = (int)tag->w8 << 3;
        *h_out  = (int)tag->h;
        return (int)tag->srca;
    }

    pix = Saturn_SpriteData(scache);
    if (!pix || !Saturn_SpritePatch(scache, &w, &h, &lo, &to))
        return 0;                       /* not BAD: data may load later */
    if (w < 1 || h < 1 || h > 255 || w > 128)
    {
        tag->srca = V1_TAG_BAD;         /* oversize: never usable */
        return 0;
    }
    pw    = (w + 7) & ~7;
    bytes = pw * h;

    off = v1_arena_alloc(bytes);
    if (off == 0)
        return 0;

    if (pw == w)
    {
        v1_copy_block(off, pix, bytes); /* no pad columns: straight copy */
    }
    else
    {
#ifdef SATURN_WIN32
        uint8_t *dst = (uint8_t *)(V1_BASE + off);
        int y, x;
        for (y = 0; y < h; y++)
        {
            const byte *sr = pix + y * w;
            for (x = 0; x < pw; x++)
                dst[y * pw + x] = (x < w) ? sr[x] : 0;
        }
#else
        /* PADDED to a multiple of 8 texels; pad texels = 0 = transparent */
        volatile unsigned short *dst =
            (volatile unsigned short *)(V1_BASE + off);
        int y, xw;

        for (y = 0; y < h; y++)
        {
            const byte *sr = pix + y * w;

            for (xw = 0; xw < pw / 2; xw++)
            {
                int      xl = xw * 2, xr = xl + 1;
                unsigned b_l = (xl < w) ? (unsigned)sr[xl] : 0u;
                unsigned b_r = (xr < w) ? (unsigned)sr[xr] : 0u;

                *dst++ = (unsigned short)((b_l << 8) | b_r);
            }
        }
#endif
    }

    tag->srca = (unsigned short)(off / 8u);
    tag->w8   = (unsigned char)(pw >> 3);
    tag->h    = (unsigned char)h;
    *pw_out = pw;
    *h_out  = h;
    return (int)tag->srca;
}

int Saturn_Vdp1Wall_Base(void)
{
    return v1q_count;
}

void Saturn_Vdp1Wall_Restore(int base)
{
    if (base >= 0 && base < v1q_count)
        v1q_count = base;
}

/* 2026-09-09 BUG-VIEW TRIAGE: read accessor for the quad table (zero
 * BSS — .text only). i_video.c census rows dump the SELECTED quad's
 * stored geometry. n==0 = the TOPMOST quad (min [1] ya0; among ties
 * the larger xb; right-edge-aware: quads whose xb reaches >= 200 are
 * preferred, since the reported right-hand artifact draws in the top
 * of the RIGHT quarter where no legitimate wall top should reach).
 * n==1 = the LAST quad (world sprites are appended after the wall
 * quads, near-to-far, so the last cell is the nearest world sprite).
 * n>=2 = raw index (unused by the census). */
short Saturn_Vdp1Wall_Q(int n, int f)
{
    if (f < 0 || f > 13)
        return 0;
    if (n == 1)
        return v1q_count > 0 ? v1q[v1q_count - 1][f] : 0;
    if (n == 0)
    {
        int best = -1, best_r = -1, q;

        for (q = 0; q < v1q_count; q++)
        {
            if (v1q[q][3] >= 200
                && (best_r < 0 || v1q[q][1] < v1q[best_r][1]))
                best_r = q;
            if (best < 0 || v1q[q][1] < v1q[best][1])
                best = q;
        }
        if (best_r >= 0)
            return v1q[best_r][f];
        return best >= 0 ? v1q[best][f] : 0;
    }
    if (n < 0 || n >= v1q_count)
        return 0;
    return v1q[n][f];
}

/* Stage 3a: one sprite (or psprite) quad. xA = the texel-0 edge
 * (screen LEFT for normal sprites, screen RIGHT for flipped ones —
 * the caller swaps the edges), xB = the opposite edge; the quad spans
 * the PADDED width so the affine mapping stays exact and the pad
 * columns overdraw as transparency. yt/yb = top / bottom-edge screen
 * rows for texel rows srcrow..srcrow+rows-1 (the caller clamps them
 * to the view window and shifts srcrow accordingly, exactly like the
 * wall path's v-anchor). Unlike the wall Add, a full table does NOT
 * latch saturn_v1w_captured — sprite refuses must never disarm the
 * walk's wall capture for the frame; the caller keeps the sprite in
 * software instead. Returns 1 = quad stored, 0 = keep software. */
int Saturn_Vdp1Spr_Add(int scache, int xA, int xB, int yt, int yb,
                       int srcrow, int rows, int shade, int mark)
{
    int pw, h, srca, k;

    srca = v1_spr_get(scache, &pw, &h);
    if (srca == 0)
        return 0;
    if (srcrow < 0)
        srcrow = 0;
    if (srcrow + rows > h)
        rows = h - srcrow;
    if (rows <= 0)
        return 0;
    srca += (srcrow * pw) / 8;
    k = (shade + 2) >> 2;
    if (k < 0)  k = 0;
    if (k > 7)  k = 7;

    uint32_t pmod = ((PMOD_WALL & ~0x0040u) | PMOD_CLIP_IN) & 0xFFFFu;
    uint32_t colr = ((uint32_t)k << 8) & 0xFFFFu;
    uint32_t size = (((uint32_t)pw / 8u) << 8) | ((uint32_t)rows & 0xFFu);

    if (mark)
    {
        /* Weapon / psprite: emit to dedicated weapon buffer saturn_cmd_w */
        if (saturn_cmd_w_count < CMDBUF_W_CAP)
        {
            uint32_t *cmd32 = (uint32_t *)&saturn_cmd_w[saturn_cmd_w_count];
            cmd32[0] = 0x00020000u;
            cmd32[1] = (pmod << 16) | colr;
            cmd32[2] = ((uint32_t)srca << 16) | size;
            cmd32[3] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yt;
            cmd32[4] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yt;
            cmd32[5] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yb;
            cmd32[6] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yb;
            cmd32[7] = 0;

            saturn_cmd_w_count++;
            saturn_v1w_cells++;
            return 1;
        }
    }
    else
    {
        /* World subsector sprite: emit to cmdbuf M */
        if (saturn_cmd_m_count < CMDBUF_M_CAP)
        {
            uint32_t *cmd32 = (uint32_t *)&saturn_cmd_m[saturn_cmd_m_count];
            cmd32[0] = 0x00020000u;
            cmd32[1] = (pmod << 16) | colr;
            cmd32[2] = ((uint32_t)srca << 16) | size;
            cmd32[3] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yt;
            cmd32[4] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yt;
            cmd32[5] = ((uint32_t)(uint16_t)xB << 16) | (uint16_t)yb;
            cmd32[6] = ((uint32_t)(uint16_t)xA << 16) | (uint16_t)yb;
            cmd32[7] = 0;

            saturn_cmd_m_count++;
            saturn_v1w_cells++;
            return 1;
        }
    }

    saturn_v1w_drop++;
    return 0;
}

int Saturn_Vdp1Quad_PspMarked(void)
{
    return (saturn_cmd_w_count > 0);
}

/* 2026-09-08 COPR PROBE (TEMPORARY): the VDP1's COPR register
 * (0x25D00014, jo/sega_saturn.h JO_VDP1_COPR) holds the command-table
 * address the draw last fetched — at draw end it names the slot the
 * walk stopped on; LOPR (0x25D00012) holds the last local coordinate.
 * Row = copr_slot * 100 + (lopr >> 3 & 0x3F). Decodes: copr_slot =
 * O / 100 (3 = walk stopped at SGL's slot 3; 4+canary+N+1 = full
 * list; 2 = stopped at slot 2). lopr>>3 = O % 100: 0 = local coord
 * (0,0) executed (the walk ran at least to slot 2); 15/20 = a stale
 * (160,120)-ish BIOS value (the walk never executed a local-coord
 * command — the PTMR kick is inert); other = foreign. */
int Saturn_V1w_Copr_Row(void)
{
    return (int)((V1_REG(0x14) >> 5) * 100u)
         + (int)((V1_REG(0x12) >> 3) & 0x3Fu);
}

/* 2026-09-08 FB PROBE (TEMPORARY): raw word from a VDP1 framebuffer —
 * bypasses the whole VDP2 sprite-display path and shows what the
 * erase/draw machinery ACTUALLY wrote. sel 0 = FB0 (0x25C80000),
 * sel 1 = FB1 (0x25CC0000); wi = the 16-bit word index (x + y*512).
 * With the white erase armed (EWDR 0x0104): 0x0104 everywhere = the
 * erase executes; boot-era values (0x0000-ish) = it does not. */
unsigned int Saturn_V1w_Fb_Row(int sel, int wi)
{
    volatile unsigned short *fb =
        (volatile unsigned short *)(sel ? V1_FB1 : V1_FB0);

    return fb[wi];
}

/* 2026-09-08 TEMPORARY diagnostic: export the first psprite-marked
 * quad's fields for the h-row dump (zero new state — recompute).
 * k = -1 when no marked quad exists. */
void Saturn_Vdp1Quad_PspDump(int *k, int *rows, int *xa)
{
    if (saturn_cmd_w_count > 0 && saturn_cmd_w != NULL)
    {
        *k = (int)((saturn_cmd_w[0].colr >> 8) & 0xFF);
        *rows = (int)(saturn_cmd_w[0].size & 0xFF);
        *xa = (int)saturn_cmd_w[0].xa;
        return;
    }
    *k = -1;
    *rows = 0;
    *xa = 0;
}
#endif /* !SATURN_3A_BYPASS */

void Saturn_Vdp1Wall_Reset(void)
{
    /* Arena double buffer (2026-10-05). VDP1 keeps re-rasterising the
     * LAST COMMITTED command list (every vblank, for as many vblanks as
     * the CPU frame takes), and that list references only the page it
     * was built from. So:
     *  - previous frame committed (Frame() wrote its list): VDP1 now
     *    reads page P; flip and fill the other page;
     *  - previous frame committed nothing (zero quads), or this is a
     *    second Reset in the same frame (saturn_segs_build also calls
     *    Reset): VDP1 is still on the list from before, which
     *    references the OTHER page — stay on the current page.
     * No EDSR/CEF wait here: right after slSynch the wait would either
     * stall the CPU for VDP1's whole draw (serialising the two chips)
     * or pass on the previous draw's stale CEF. The wait that matters
     * is in Saturn_Vdp1Wall_Frame, before the command list is rewritten
     * — once that list is replaced, the old page is free. */
    /* Remember how full the outgoing page is: its uploads stay resident
     * (persistent cache) for the next time it comes into use. */
    v1_page_off[0] = v1_arena_off;
    v1_page_off[1] = v1_arena_off;
    v1_tex_page = 0;
    v1_page_committed = 0;
    v1_arena_end = V1_PAGE0_END;

    /* Flush the unified persistent cache if it overflowed or is nearly full. */
    v1_tags_try_alloc();
    if (v1_page_flush[0]
        || v1_arena_end - v1_page_off[0] < V1_PAGE_LOWWATER)
    {
#ifndef SATURN_WIN32
        /* Never overwrite VDP1 texture VRAM while VDP1 is still rasterizing the previous frame */
        if (v1_have_frame)
        {
            unsigned guard = 16000u;
            while (!(V1_EDSR & 2u) && --guard)
            {
                for (int w = 0; w < 32; w++)
                    __asm__ volatile("nop");
            }
        }
#endif
        if (v1_tag[0])
        {
            /* Flush walls and sprites ONLY — keep preloaded flats permanently resident */
            if (v1_tag_flat0 > 0)
                D_memset(&v1_tag[0][0], 0,
                         (size_t)v1_tag_flat0 * sizeof(v1_tag_t));
            if (v1_tag_n > v1_tag_spr0)
                D_memset(&v1_tag[0][v1_tag_spr0], 0,
                         (size_t)(v1_tag_n - v1_tag_spr0) * sizeof(v1_tag_t));
        }
        v1_page_off[0] = v1_page_base[0];
        v1_page_off[1] = v1_page_base[0];
        v1_page_flush[0] = 0;
        v1_page_flush[1] = 0;
        Saturn_Vdp1_ClearTexCacheMRU();
    }
    v1_arena_off = v1_page_off[0];

    v1w_table_try_alloc();
    v1q_count = 0;
    saturn_v1w_refuse = 0;
    saturn_v1w_drop = 0;
    saturn_v1w_cells = 0;
}

/* Level start (Saturn_HW_RenderInit): drop both pages' cached textures
 * so nothing from before the load (or anything else that touched VDP1
 * VRAM meanwhile) survives. Each page is re-started on its next use. */
void Saturn_Vdp1Wall_FlushCache(void)
{
    v1_page_flush[0] = 1;
    v1_page_flush[1] = 1;
    Saturn_Vdp1_ClearTexCacheMRU();
}

int Saturn_Vdp1Wall_Count(void)
{
    extern unsigned int saturn_hw_wall_quads;
    extern unsigned int saturn_hw_flat_quads;
    if (saturn_hw_wall_quads + saturn_hw_flat_quads > 0)
        return (int)saturn_hw_wall_quads + (int)saturn_hw_flat_quads + (int)saturn_cmd_w_count;
    return (int)saturn_cmd_m_count + (int)saturn_cmd_s_count + (int)saturn_cmd_w_count;
}

int Saturn_Vdp1Wall_Cap(void)
{
    return CMDBUF_M_CAP + CMDBUF_S_CAP + CMDBUF_W_CAP;
}

/* Paint-skip pre-pass helper (2026-09-07, saturn_segs.c): 1 iff the
 * table has `count` free slots AND `texnum` is drawable (cache hit or
 * uploaded now — commit's Add will hit the same cache entry) AND its
 * cached size matches the walk's Saturn_TexWidth/Height view (Add
 * clamps `rows` against ti.h; a mismatch could refuse a cell the
 * preskip counted). No table mutation. */
int Saturn_Vdp1Wall_CanAdd(int texnum, int count, int ew, int eh)
{
    int tw, th;

    if (v1q_count + count > v1q_cap)
    {
        saturn_v1w_drop += count;
        return 0;
    }
    if (v1_tex_get(texnum, &tw, &th) == 0)
        return 0;
    return (tw == ew && th == eh);
}

static int v1_add_common(int xa, int ya0, int ya1,
                         int xb, int yb0, int yb1,
                         int texnum, int srcrow, int rows, int shade,
                         int clip_x0, int clip_x1, int mid);

int Saturn_Vdp1Wall_Add(int xa, int ya0, int ya1,
                        int xb, int yb0, int yb1,
                        int texnum, int srcrow, int rows, int shade,
                        int clip_x0, int clip_x1)
{
    return v1_add_common(xa, ya0, ya1, xb, yb0, yb1,
                         texnum, srcrow, rows, shade,
                         clip_x0, clip_x1, 0);
}

int Saturn_Vdp1Wall_AddMid(int xa, int ya0, int ya1,
                           int xb, int yb0, int yb1,
                           int texnum, int shade,
                           int clip_x0, int clip_x1)
{
    /* Stage 2a: the whole texture, one cell — masked mids never tile
     * vertically (paint_wall_span_masked breaks once at v >= h), so
     * there is no v-band seam split and no srcrow offset. */
    return v1_add_common(xa, ya0, ya1, xb, yb0, yb1,
                         texnum, 0, 0, shade,
                         clip_x0, clip_x1, 1);
}

int Saturn_Vdp1Flat_Add(int xa, int ya0, int ya1,
                        int xb, int yb0, int yb1,
                        int flatnum, int shade,
                        int clip_x0, int clip_x1)
{
    if (flatnum < 0)
        return 0;
    return Saturn_Vdp1Wall_Add(xa, ya0, ya1, xb, yb0, yb1,
                               -(flatnum + 1), 0, 64, shade,
                               clip_x0, clip_x1);
}

static int v1_add_quad_common(int x0, int y0,
                              int x1, int y1,
                              int x2, int y2,
                              int x3, int y3,
                              int texnum, int srcrow, int rows, int shade,
                              int clip_x0, int clip_x1, int mid)
{
    int tw, th, srca, k;

    if (v1q == NULL || v1q_count >= v1q_cap)
    {
        saturn_v1w_drop++;
        saturn_v1w_captured = 1;    /* table full — stop evaluating */
        return 0;
    }
    srca = v1_tex_get(texnum, &tw, &th);
    if (srca == 0)
    {
        saturn_v1w_refuse++;
        return 1;   /* unusable texture (oversize / not /8 / slave-safe
                       cache miss): SKIP — the software paint already
                       shows through, and the bank-0 checker drew
                       full-bright over it (the bright strips). Don't
                       trip the table-full latch for it. */
    }
    /* Source row offset (v-anchoring): SRCA is quantized to 8
     * bytes and one texture row is tw bytes — a whole SRCA unit
     * for every usable tw (multiple of 8). A cell never reads
     * past its upload: rows is clamped to what's left below
     * srcrow (commit's seam split already guarantees it). */
    srca += (srcrow * tw) / 8;
    if (mid)
        rows = th;                  /* masked mids: full texture     */
    if (rows <= 0 || srcrow + rows > th)
        rows = th - srcrow;
    if (rows <= 0)
    {
        saturn_v1w_refuse++;
        return 1;                   /* nothing drawable */
    }
    /* Shade (COLORMAP row 0..30) -> nearest shaded CRAM bank k (0..7,
     * exact rows 0,4,..,28). Bank 8 doesn't exist, so sh 30..31 clamp
     * to bank 7 (2 rows bright of the true shade). */
    k = (shade + 2) >> 2;
    if (k < 0)  k = 0;
    if (k > 7)  k = 7;
    v1q[v1q_count][0] = (short)x0;
    v1q[v1q_count][1] = (short)y0;
    v1q[v1q_count][2] = (short)y3;
    v1q[v1q_count][3] = (short)x1;
    v1q[v1q_count][4] = (short)y1;
    v1q[v1q_count][5] = (short)y2;
    v1q[v1q_count][6] = (short)srca;
    v1q[v1q_count][7] = (short)tw;
    v1q[v1q_count][8] = (short)rows;
    v1q[v1q_count][9] = (short)k;
    v1q[v1q_count][10] = (short)x3;
    /* User clip rect (u-phase pieces): x0 == x1 = no clip. The y span
     * is fixed full-screen in the writer. */
    v1q[v1q_count][11] = (short)(clip_x0 < clip_x1 ? clip_x0 : clip_x1);
    v1q[v1q_count][12] = (short)(clip_x0 < clip_x1 ? clip_x1 : clip_x0);
    v1q[v1q_count][13] = (short)mid;    /* flags: bit0 = masked-mid  */
    v1q[v1q_count][14] = (short)x2;
    v1q[v1q_count][15] = 0;
    v1q_count++;
    saturn_v1w_cells++;
    return 1;
}

static int v1_add_common(int xa, int ya0, int ya1,
                         int xb, int yb0, int yb1,
                         int texnum, int srcrow, int rows, int shade,
                         int clip_x0, int clip_x1, int mid)
{
    return v1_add_quad_common(xa, ya0, xb, yb0, xb, yb1, xa, ya1,
                              texnum, srcrow, rows, shade,
                              clip_x0, clip_x1, mid);
}

int Saturn_Vdp1Quad_Add(int x0, int y0,
                        int x1, int y1,
                        int x2, int y2,
                        int x3, int y3,
                        int texnum, int srcrow, int rows, int shade,
                        int clip_x0, int clip_x1)
{
    return v1_add_quad_common(x0, y0, x1, y1, x2, y2, x3, y3,
                              texnum, srcrow, rows, shade,
                              clip_x0, clip_x1, 0);
}

int Saturn_Vdp1FlatQuad_Add(int x0, int y0,
                            int x1, int y1,
                            int x2, int y2,
                            int x3, int y3,
                            int flatnum, int shade)
{
    if (flatnum < 0)
        return 0;
    return Saturn_Vdp1Quad_Add(x0, y0, x1, y1, x2, y2, x3, y3,
                               -(flatnum + 1), 0, 64, shade,
                               0, 0);
}

static int s_mru_flat = -999;
static int s_mru_flat_srca = 0;

int Saturn_Vdp1_GetFlatSrca(int flatnum)
{
    if (flatnum < 0)
        return 0;
    if (flatnum == s_mru_flat)
        return s_mru_flat_srca;

    int tw, th;
    s_mru_flat_srca = v1_tex_get(-(flatnum + 1), &tw, &th);
    s_mru_flat = flatnum;
    return s_mru_flat_srca;
}

void Saturn_Vdp1_ClearPages(void)
{
    v1_tags_try_alloc();
    if (v1_tag[0])
        D_memset(v1_tag[0], 0, (size_t)v1_tag_n * sizeof(v1_tag_t));
    if (v1_tag[1] && v1_tag[1] != v1_tag[0])
        D_memset(v1_tag[1], 0, (size_t)v1_tag_n * sizeof(v1_tag_t));
    v1_page_off[0] = V1_PAGE0_START;
    v1_page_off[1] = V1_PAGE0_START;
    v1_page_base[0] = V1_PAGE0_START;
    v1_page_base[1] = V1_PAGE0_START;
    v1_arena_off = V1_PAGE0_START;
    v1_arena_end = V1_PAGE0_END;
    v1_tex_page = 0;
    v1_page_committed = 0;
    v1_page_flush[0] = 0;
    v1_page_flush[1] = 0;
    Saturn_Vdp1_ClearTexCacheMRU();
}

void Saturn_Vdp1_PreloadFlat(int flatnum)
{
    int tw, th;

    if (flatnum < 0 || flatnum >= (int)numflats)
        return;

    v1_tags_try_alloc();
    if (v1_tag[0] == NULL)
        return;

    /* Preload into Unified Persistent Cache (Page 0) */
    v1_tex_page = 0;
    v1_arena_off = v1_page_off[0];
    v1_arena_end = V1_PAGE0_END;
    v1_tex_get(-(flatnum + 1), &tw, &th);
    v1_page_off[0] = v1_arena_off;
    v1_page_off[1] = v1_arena_off;
}

void Saturn_Vdp1_LockPreloadedFlats(void)
{
    v1_page_base[0] = v1_page_off[0];
    v1_page_base[1] = v1_page_off[0];
    v1_arena_off = v1_page_off[0];
    v1_arena_end = V1_PAGE0_END;
}

static int s_mru_texnum = -1;
static int s_mru_srca = 0;
static int s_mru_srca_mono = 0;
static int s_mru_w = 0;
static int s_mru_h = 0;

void Saturn_Vdp1_ClearTexCacheMRU(void)
{
    s_mru_texnum = -1;
    s_mru_srca = 0;
    s_mru_srca_mono = 0;
    s_mru_w = 0;
    s_mru_h = 0;
    s_mru_flat = -999;
}

int Saturn_Vdp1_GetTexSrca(int texnum, int *tw, int *th)
{
    if (texnum <= 0)
        return 0;

    if (texnum == s_mru_texnum && s_mru_srca != 0)
    {
        *tw = s_mru_w;
        *th = s_mru_h;
        return s_mru_srca;
    }

    s_mru_srca = v1_tex_get(texnum, tw, th);
    s_mru_texnum = texnum;
    s_mru_w = *tw;
    s_mru_h = *th;
    int t = texturetranslation ? texturetranslation[texnum] : texnum;
    if ((unsigned)t >= (unsigned)numtextures)
        t = texnum;
    if (v1_tag[v1_tex_page] && v1_tag[v1_tex_page][t].srca_mono)
        s_mru_srca_mono = (int)v1_tag[v1_tex_page][t].srca_mono;
    else
        s_mru_srca_mono = s_mru_srca;
    return s_mru_srca;
}

int Saturn_Vdp1_GetTexSrcaMono(int texnum)
{
    if (texnum <= 0)
        return 0;
    if (texnum == s_mru_texnum && s_mru_srca_mono != 0)
        return s_mru_srca_mono;
    int tw, th;
    Saturn_Vdp1_GetTexSrca(texnum, &tw, &th);
    return s_mru_srca_mono;
}

int Saturn_Vdp1FlatQuad_Fast(const int *qx, const int *qy, int best_r, int srca, int k)
{
    int i0, i1, i2, i3;

    if (v1q == NULL || v1q_count >= v1q_cap)
    {
        saturn_v1w_drop++;
        return 0;
    }

    i0 = best_r;
    i1 = (best_r + 1) & 3;
    i2 = (best_r + 2) & 3;
    i3 = (best_r + 3) & 3;

    v1q[v1q_count][0]  = (short)qx[i0];
    v1q[v1q_count][1]  = (short)qy[i0];
    v1q[v1q_count][2]  = (short)qy[i3];
    v1q[v1q_count][3]  = (short)qx[i1];
    v1q[v1q_count][4]  = (short)qy[i1];
    v1q[v1q_count][5]  = (short)qy[i2];
    v1q[v1q_count][6]  = (short)srca;
    v1q[v1q_count][7]  = 64;
    v1q[v1q_count][8]  = 64;
    v1q[v1q_count][9]  = (short)k;
    v1q[v1q_count][10] = (short)qx[i3];
    v1q[v1q_count][11] = 0;
    v1q[v1q_count][12] = 0;
    v1q[v1q_count][13] = 0;
    v1q[v1q_count][14] = (short)qx[i2];
    v1q[v1q_count][15] = 0;
    v1q_count++;
    saturn_v1w_cells++;
    return 1;
}

/* Allocate the quad table in fast RAM (WRAM-H or WRAM-L).
 * Completely avoids slow 16-bit A-bus Cart RAM. */
static void v1w_table_try_alloc(void)
{
    /* Retired in Phase 2: unified cmdbuf (saturn_cmdbuf.c) replaces v1q.
     * Do NOT allocate 48 KB of WRAM-H here — cmdbuf M/S/W owns the quad budget. */
    v1q = NULL;
    v1q_cap = V1W_MAX_QUADS;
}

void Saturn_Vdp1Wall_Frame(void)
{
    extern void Saturn_StageTextHi(int x, int y, const char *s);
    extern void Saturn_Udec0(char *out, unsigned int v, int wide);
    static const char hd[17] = "0123456789ABCDEF";
    char row[32];
    int  n = 0, i;

    if (!v1_inited)
        return;

    /* Lazy quad-table alloc (P5 Phase 1) — retried until the zone is
     * live (Z_Init runs after the boot pump's first commits). */
    v1w_table_try_alloc();

    /* Kick ONLY with quads — an empty table would still run SGL's own
     * 4-slot list (which is harmless, but pointless). The HUD row
     * paints every frame regardless, with the captured digit telling
     * the states apart: row absent = Frame() never ran; digit = quads
     * drawn this frame. */
    if (saturn_cmd_m_count > 0 || saturn_cmd_s_count > 0 || saturn_cmd_w_count > 0 || (v1q != NULL && v1q_count > 0))
    {
        saturn_v1w_modr = (int)(V1_MODR & 0x1Fu);

        /* SGL-PIPELINE BUILD (2026-09-07): NO PTMR writes, NO TVMR
         * erase, NO TVMD writes — SGL's auto-draw triggers itself at
         * vblank from ITS command table. We only (a) wait for the
         * previous frame's draw to complete (EDSR.CEF = bit 1; bounded
         * spin so an emulator quirk can't hang the game loop), then
         * (b) write our user command at slot 4 + END at slot 5 during
         * the display period, and (c) patch SGL's slot-3 END into
         * LOCAL_COORD so the next vblank's walk reaches our slots. */
#ifdef SATURN_WIN32
        if (saturn_cmd_m_count > 0 || saturn_cmd_s_count > 0 || saturn_cmd_w_count > 0)
        {
            extern uint32_t saturn_vdp1_bus_cmd_bytes;
            Saturn_CmdBuf_Deliver();
            saturn_v1w_edsr = 3;
            int slots = (int)(saturn_vdp1_bus_cmd_bytes >> 5) - 3;
            saturn_v1w_copr = (slots >= 0) ? slots : 0;
        }
        else if (v1q != NULL && v1q_count > 0)
        {
            v1_write_commands();
            saturn_v1w_edsr = 3;
            saturn_v1w_copr = v1q_count;
        }
        v1_page_committed = 1;
        v1_have_frame = 1;
#else
        {
            extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
            extern volatile unsigned int saturn_vbl_count;
            volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
            unsigned int vc0;
            unsigned short tc0;
            do {
                vc0 = saturn_vbl_count;
                unsigned short hi = frt[0];
                __asm__ volatile("" ::: "memory");
                unsigned short lo = frt[1];
                tc0 = (unsigned short)((hi << 8) | lo);
            } while (__builtin_expect(saturn_vbl_count != vc0, 0));
            unsigned guard = 12000u;
            /* SCU VBlank-IN fires at Scanline 224 (14.20 ms), while VDP1
             * FRAME_CHANGE (VBlank-OUT) is at Scanline 262 (16.67 ms — 2.47 ms later).
             * Each 32-NOP inner loop iteration is ~96 SH-2 cycles (~3.35 us), so
             * vbl_grace = 600u allows VDP1 up to ~2.01 ms into VBlank (Scanline 255)
             * to finish rasterizing while still leaving > 0.45 ms before Scanline 262
             * so Saturn_CmdBuf_Deliver() (~0.35 ms) always completes before VBlank-OUT
             * without colliding with single-ported VDP1 VRAM fetches. */
            unsigned vbl_grace = 600u;

            while (!(V1_EDSR & 2u) && --guard)
            {
                if ((saturn_vbl_count != vc0 || ((*(volatile unsigned short *)0x25F80004u) & 0x0008u)) &&
                    --vbl_grace == 0u)
                    break;
                /* Yield B-bus to VDP1 so status polling never steals VDP1 rasterizer cycles */
                for (int w = 0; w < 32; w++)
                    __asm__ volatile("nop");
            }
            saturn_prof_cef += Saturn_ProfSpan(tc0, vc0);
            /* 0 = CEF never set within the guard (previous draw never
             * completed — writing commands now races the walk);
             * 2/3 = clean; 1 = timeout AND EDSR read 0 after. */
            unsigned short edsr_now = V1_EDSR;
            unsigned short copr_now = V1_COPR;
            unsigned short lopr_now = V1_LOPR;
            saturn_v1w_edsr = guard ? (int)(edsr_now & 3u) : 0;
            unsigned short addr_reg = (copr_now >= 0x18u) ? copr_now : lopr_now;
            int raw_slot = (int)(addr_reg >> 2);
            saturn_v1w_copr = (raw_slot >= 6) ? (raw_slot - 6) : raw_slot;
        }

        if (saturn_cmd_m_count > 0 || saturn_cmd_s_count > 0 || saturn_cmd_w_count > 0)
        {
            extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
            extern volatile unsigned int saturn_vbl_count;
            volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
            unsigned int vd0;
            unsigned short td0;
            do {
                vd0 = saturn_vbl_count;
                unsigned short hi = frt[0];
                __asm__ volatile("" ::: "memory");
                unsigned short lo = frt[1];
                td0 = (unsigned short)((hi << 8) | lo);
            } while (__builtin_expect(saturn_vbl_count != vd0, 0));
            Saturn_CmdBuf_Deliver();
            saturn_prof_del += Saturn_ProfSpan(td0, vd0);
        }
        else if (v1q != NULL && v1q_count > 0)
        {
            v1_write_commands();
        }

        /* the live list now references v1_tex_page: the next Reset may
         * flip the CPU to the other page */
        v1_page_committed = 1;
        v1_have_frame = 1;
        v1_patch_slot3();
        v1_write_priority_regs();
#endif
    }

    /* HUD row RETIRED (2026-09-08, operator request): the V1 register
       readback painted white glyphs across x=8..308 at y=168 — straight
       through the census column and over the M row's cells. The pipeline
       it watched is proven (CTRL END-bit fix); MODR/EDSR still reach the
       census via saturn_v1w_modr / saturn_v1w_edsr. */
#if 0
    /* HUD row — painted after copy_back so it survives this frame's
     * flip. Layout (all chars in the 38-glyph StageText set, 15
     * cells = 300 px):
     *   V1 C <MODR 2-hex> <EDSR 1-hex> <TVMR 4> <FBCR 4> <PTMR 4> <EDSR 4>
     * C = quads drawn this frame (0-8). MODR: bit4 FCM, bit3 VBE,
     * bits2-0 TVM.
     * EDSR bit0 = draw ended.
     * Site: (8,168) — the retired O row's slot. The original (8,178)
     * site sat 10 px under the O row and union'd with it (the O row's
     * own comment records this exact interleaving hazard). */
    row[n++] = 'V'; row[n++] = '1'; row[n++] = hd[v1q_count & 15];
    for (i = 1; i >= 0; i--)
        row[n++] = hd[((unsigned)saturn_v1w_modr >> (i * 4)) & 15u];
    row[n++] = ' ';
    row[n++] = hd[(unsigned)saturn_v1w_edsr & 15u];
    /* LIVE REGISTER READBACKS (2026-09-07): TVMR FBCR PTMR EDSR as the
     * VDP1 sees them at HUD-paint time. If these read back as written
     * (08 / 01 / 0001 / x) nothing clobbers our state and the problem
     * is the VDP1's execution itself; a mismatch = something rewrites
     * the registers after us (VBlank ISR suspect). */
    row[n++] = ' ';
    for (i = 3; i >= 0; i--)
        row[n++] = hd[((unsigned)V1_TVMR >> (i * 4)) & 15u];
    for (i = 3; i >= 0; i--)
        row[n++] = hd[((unsigned)V1_FBCR >> (i * 4)) & 15u];
    for (i = 3; i >= 0; i--)
        row[n++] = hd[((unsigned)V1_PTMR >> (i * 4)) & 15u];
    for (i = 3; i >= 0; i--)
        row[n++] = hd[((unsigned)V1_EDSR >> (i * 4)) & 15u];
    /* (The 2026-09-06 magenta canary bar is retired: the quad renders
     * since the CTRL END-bit fix, so the paint path is proven.) */
    row[n] = '\0';
    {
        extern int saturn_quiet_hud;

        saturn_quiet_hud = 0;
        Saturn_StageTextHi(8, 168, row);
        saturn_quiet_hud = 1;
    }
#endif /* HUD row retired 2026-09-08 */
}

#else  /* !SATURN_VDP1_WALL */

void Saturn_Vdp1Wall_Init(void) { }
void Saturn_Vdp1Wall_Reset(void) { }
void Saturn_Vdp1Wall_FlushCache(void) { }
int  Saturn_Vdp1Wall_Add(int xa, int ya0, int ya1,
                         int xb, int yb0, int yb1,
                         int texnum, int srcrow, int rows, int shade,
                         int clip_x0, int clip_x1)
{
    (void)xa; (void)ya0; (void)ya1;
    (void)xb; (void)yb0; (void)yb1;
    (void)texnum; (void)srcrow; (void)rows; (void)shade;
    return 0;
}
int  Saturn_Vdp1Wall_AddMid(int xa, int ya0, int ya1,
                            int xb, int yb0, int yb1,
                            int texnum, int shade,
                            int clip_x0, int clip_x1)
{
    (void)xa; (void)ya0; (void)ya1;
    (void)xb; (void)yb0; (void)yb1;
    (void)texnum; (void)shade;
    (void)clip_x0; (void)clip_x1;
    return 0;
}
int  Saturn_Vdp1Wall_Base(void) { return 0; }
void Saturn_Vdp1Wall_Restore(int base) { (void)base; }
int  Saturn_Vdp1Spr_Add(int scache, int xA, int xB, int yt, int yb,
                        int srcrow, int rows, int shade, int mark)
{
    (void)scache; (void)xA; (void)xB; (void)yt; (void)yb;
    (void)srcrow; (void)rows; (void)shade; (void)mark;
    return 0;
}
int  Saturn_Vdp1Quad_PspMarked(void) { return 0; }
void Saturn_Vdp1Wall_Frame(void) { }

#endif /* SATURN_VDP1_WALL */