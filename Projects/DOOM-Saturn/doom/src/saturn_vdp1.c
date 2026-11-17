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
#include "saturn_light_lut.h"   /* saturn_shade_bank[]: shade -> CRAM bank */

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
 * active palette remapped through COLORMAP rows saturn_bank_row[k]
 * (4,9,13,18,22,27,31; i_video.c's Saturn_Cram_ShadedBanks), so a quad at
 * CMDCOLR = k*256 displays BIT-IDENTICAL to the software paint at that
 * row. Per-quad bank =
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
/* VDP2 priority + sprite control registers are written directly in
 * v1_write_priority_regs() on every VBlank. */

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
/* [v1_tag_warp0 .. +V1_WARP_SLOTS): per-piece warped flat textures
 * (Saturn_Vdp1_GetWarpSrca), direct-mapped by baked quad index. */
#define V1_WARP_SLOTS SATURN_WARP_SLOTS
static int       v1_tag_warp0;
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
/* Texture cache = a ring over [v1_page_base[0], V1_PAGE0_END) with
 * EVICTION instead of whole-cache flushes (2026-11-15).
 *
 * A full flush re-uploaded the whole on-screen working set (170-260 KB)
 * in one frame — a ~180 ms hitch every time E1M1's walk filled the cache
 * — and, worse, VDP1 auto-draw keeps re-rasterising the LAST committed
 * list every vblank until the next commit, so the uploads overwrote
 * textures that list still used: garbage textures on screen for the
 * whole hitch (wall texels on floors while moving, on hardware).
 *
 * Now: every upload is logged (VRAM range + owning tag + age), the log
 * kept sorted by offset, and uploads go FIRST-FIT into the gaps between
 * logged ranges (the textures VDP1 still needs are scattered, so one bump
 * run can't use the free space). At the frame boundary
 * (Saturn_Vdp1Wall_Reset), if free space is low or an upload was refused,
 * v1_evict() scans the live command table, protects (and re-ages) every
 * logged range it references, and drops the oldest unprotected uploads
 * until V1_EVICT_TARGET is free and a hole fits the largest refused
 * upload. Only tag fields pointing into dropped ranges are cleared, so
 * only textures not drawn last frame are ever re-uploaded, and nothing
 * the live list references is overwritten. Mid-frame overflow refuses
 * the texture for that frame (missing quad, never garbage). A full flush
 * remains for level start / mode toggles / log overflow. */
#ifndef V1_EVICT_LOWWATER                 /* overridable: eviction stress tests */
#define V1_EVICT_LOWWATER (64u * 1024u)   /* evict when free space drops below */
#endif
#ifndef V1_EVICT_TARGET
#define V1_EVICT_TARGET   (128u * 1024u)  /* free space to reach when evicting */
#endif
#define V1_OPT_RESERVE    (32u * 1024u)   /* headroom kept when adding optional wall copies */
#define V1_LOG_MAX        1024
#if defined(__GNUC__) && !defined(__cplusplus)
_Static_assert(V1_LOG_MAX * 2 <= SATURN_WRAM_L_EVICT_SIZE, "v1_evict candidate list must fit SATURN_WRAM_L_EVICT");
#endif
typedef struct {
    unsigned short off32;   /* VRAM byte offset / 32 */
    unsigned short len32;   /* rounded length / 32 */
    unsigned int   age;     /* upload sequence (refreshed when seen live) */
    v1_tag_t      *tag;     /* owning tag (fields pointing in range get cleared) */
} v1_alloc_t;
static v1_alloc_t    v1_log[V1_LOG_MAX];    /* sorted by off32 */
static unsigned char v1_log_prot[V1_LOG_MAX];
static int           v1_log_n = 0;
static int           v1_log_full = 0;       /* log overflowed: full flush at next Reset */
static int           v1_want_evict = 0;     /* an upload was refused this frame */
static unsigned int  v1_want_bytes = 0;     /* largest refused upload since the last evict */
static unsigned int  v1_used_bytes = 0;     /* sum of logged lengths */
static unsigned int  v1_age = 0;
/* v1_age at the last two frame boundaries (2026-11-16 thrash fix; v1_age
 * also ticks once per frame). Entries with age >= v1_age_mark_prev were
 * uploaded, touched (cache hit) or drawn in the last two frames and are not
 * evicted for the free-space target alone (see v1_evict step 3). */
static unsigned int  v1_age_mark_prev = 0, v1_age_mark_cur = 0;
unsigned int         saturn_v1_evictions = 0;    /* HUD 'E' */
unsigned int         saturn_v1_evicted_bytes = 0;
unsigned int         saturn_v1_bad_srca = 0;     /* live textured cmds with SRCA below the arena (seen at evict) */
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

/* Double-buffered command tables (manual frame-change mode only): VDP1
 * draws one table while the CPU writes the other, so the CPU never waits
 * for a draw to finish. Slot 3 (local coord) jumps to the table to draw
 * ("jump assign", JP=001, VDP1 manual p.77; CMDLINK = address / 8). In
 * 1-cycle mode only table A is used and slot 3 falls through to it. */
#define V1M_TABLE_A      V1_USER0           /* 0x0080 */
#define V1M_TABLE_B      0x8000u
#define V1M_TABLE_A_END  0x7FE0u            /* last slot kept for END */
#define V1M_TABLE_B_END  0xFFE0u
unsigned int saturn_v1_write_table = V1M_TABLE_A;     /* table the CPU fills (saturn_cmdbuf.c) */
unsigned int saturn_v1_write_limit = V1M_TABLE_B_END; /* its END-slot limit */
static volatile unsigned int v1m_draw_table = V1M_TABLE_A;  /* table slot 3 points VDP1 at */
static volatile int v1m_double = 0;         /* double-buffered tables active */
extern int saturn_v1_manual;

static void v1_patch_slot3_ex(int from_vblank);
static void v1_patch_slot3(void)
{
    v1_patch_slot3_ex(0);
}

static void v1_patch_slot3_ex(int from_vblank)
{
    if (!v1_have_frame)
        return;                 /* no list yet — leave END */
    if (v1m_double && !from_vblank)
        return;                 /* only the vblank callback re-points slot 3 */
    V1_VRAM(0x0000u + 0x02) = 0x0000u;      /* slot 0 LINK = 0 */
    V1_VRAM(0x0000u + 0x00) = V1_CMD_SYSCLIP; /* slot 0 CTRL: sys clip (overwrite Jo's 0x9000 jump) */
    V1_VRAM(0x0020u + 0x02) = 0x0000u;      /* slot 1 LINK = 0 */
    V1_VRAM(0x0020u + 0x00) = V1_CMD_USERCLIP; /* slot 1 CTRL: user clip (overwrite Jo's 0x8000 END) */
    V1_VRAM(0x0040u + 0x02) = 0x0000u;      /* slot 2 LINK = 0 */
    V1_VRAM(0x0040u + 0x00) = 0x000Au;      /* slot 2 CTRL: local coord */
    V1_VRAM(V1_SLOT3 + 0x0C) = 16;          /* local XA = 16 (centered 320 view in 352 screen) */
    V1_VRAM(V1_SLOT3 + 0x0E) = 0;           /* local YA */
    if (v1m_double)
    {
        V1_VRAM(V1_SLOT3 + 0x02) = (unsigned short)(v1m_draw_table >> 3);  /* LINK */
        V1_VRAM(V1_SLOT3 + 0x00) = 0x100Au; /* CTRL: local coord + jump assign */
    }
    else
    {
        V1_VRAM(V1_SLOT3 + 0x02) = 0x0000u; /* slot 3 LINK = 0 */
        V1_VRAM(V1_SLOT3 + 0x00) = 0x000Au; /* CTRL: local coord (falls through to slot 4) */
    }
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
/* 2D screens (pause, menus, intermission): sprite layer at priority 0 =
 * not displayed (2026-11-16). Clearing it with blank VDP1 lists depends on
 * the manual frame-change handshake, and on hardware the last 3D frame
 * still sometimes stayed over the pause menu. Set by Saturn_Vdp1_Enter2D,
 * cleared by the next 3D commit. */
static volatile int v1_hide_sprites = 0;
static volatile int v1_hide_pending = 0;   /* Enter2D ran; hide at the 2D page flip */

static void v1_write_priority_regs(void)
{
    if (v1_hide_sprites)
    {
        *(volatile unsigned short *)0x25F800E0u = 0x0020u;
        *(volatile unsigned short *)0x25F800F0u = 0x0000u;
        *(volatile unsigned short *)0x25F800F2u = 0x0000u;
        *(volatile unsigned short *)0x25F800F4u = 0x0000u;
        *(volatile unsigned short *)0x25F800F6u = 0x0000u;
        *(volatile unsigned short *)0x25F800F8u = 0x0603u;
        *(volatile unsigned short *)0x25F800FCu = 0x0004u;
        return;
    }
    /* Jo's own proven value (core.c:278 writes 0x506 once at init):
     * priority 5 in the fields that matter, layering identical to what
     * the game has always displayed (these vblank writes never reached
     * the real registers before the 2026-09-07 base-address fix, so
     * Jo's init value is the known-good config). */
    *(volatile unsigned short *)0x25F800E0u = 0x0020u;  /* SPCTL: Sprite Type 0 + Mixed Palette/RGB (bit 5) */
    *(volatile unsigned short *)0x25F800F0u = 0x0506u;  /* PRISA */
    *(volatile unsigned short *)0x25F800F2u = 0x0506u;  /* PRISB */
    *(volatile unsigned short *)0x25F800F4u = 0x0506u;  /* PRISC */
    *(volatile unsigned short *)0x25F800F6u = 0x0506u;  /* PRISD */
    /* PRINA (0x25F800F8, NBG0 = low byte / NBG1 = high — jo/sega_saturn.h:370):
     * NBG0 priority MUST DROP BELOW the sprite layer or the sprite screen
     * sits UNDER the opaque NBG0 bitmap and no VDP1 pixel is ever visible. */
    *(volatile unsigned short *)0x25F800F8u = 0x0603u;  /* PRINA  */
    *(volatile unsigned short *)0x25F800FCu = 0x0004u;  /* PRIR: RBG0 = 4 */
}

extern void Saturn_VDP2_Plane_VBlank(void);
extern volatile int saturn_v2p_last, saturn_v2p_pending, saturn_v2p_drawing, saturn_v2p_shown;
#if defined(HW_VDP2_PLANE_TEST) && HW_VDP2_PLANE_TEST
extern volatile uint32_t saturn_v2p_list_ang[3], saturn_v2p_frame_ang;
#define V2P_LIST_COMMIT()  (saturn_v2p_list_ang[0] = saturn_v2p_frame_ang)
#define V2P_LIST_CHANGE(d) do { if (d) saturn_v2p_list_ang[2] = saturn_v2p_list_ang[1]; \
                                saturn_v2p_list_ang[1] = saturn_v2p_list_ang[0]; } while (0)
#define V2P_LIST_REDRAW()  (saturn_v2p_list_ang[0] = saturn_v2p_list_ang[1])
#else
#define V2P_LIST_COMMIT()  ((void)0)
#define V2P_LIST_CHANGE(d) ((void)0)
#define V2P_LIST_REDRAW()  ((void)0)
#endif

/* MANUAL FRAME-CHANGE MODE (SATURN_V1_MANUAL, 2026-11-15).
 *
 * In the default 1-cycle mode (FBCR FCM=FCT=0) VDP1 swaps buffers and
 * restarts the command list at EVERY field. A list that needs more than
 * one field is cut off at the swap ("transfer-over", VDP1 manual p.53),
 * which loses the NEAREST quads because the list runs far-to-near: the
 * striped near floors and missing close walls seen on hardware. And at 30
 * FPS every list is drawn twice, so the CPU's CEF wait usually lands in
 * a redundant redraw (~6 ms/frame measured in 5thPlanet).
 *
 * Manual mode (FBCR FCM=1; VDP1 manual p.38-39, Jo Engine's own
 * JO_VDP1_MANUAL_CHANGE pipeline): VDP1 draws a list ONCE, at the frame
 * change WE request (FBCR = FCM|FCT = 3, written at vblank), and the
 * display keeps the last finished frame until the next change — a slow
 * frame is held, never cut. There is no automatic erase in this mode, so
 * every list starts with a transparent polygon over the 3D view
 * (Saturn_CmdBuf_WriteClear). The r6 manual-mode attempt failed because
 * it used the VBE vblank erase, which hit the buffer about to DISPLAY;
 * this pipeline never sets VBE.
 *
 * State (all touched by the vblank callback and the main loop):
 *   v1m_pending  a committed list is waiting for its frame change
 *   v1m_writing  the CPU is rewriting the command table: no change now
 *   v1m_drawing  a change was requested; VDP1 is (or will be) drawing
 *   v1m_trig_seq the callback sequence number of that request. EDSR.CEF
 *                still reads 1 (old draw) until the change actually happens
 *                at the END of that vblank, so while we are still inside
 *                the same vblank the draw counts as not finished. */
#ifndef SATURN_V1_MANUAL
#define SATURN_V1_MANUAL 0
#endif
int saturn_v1_manual = 0;           /* manual mode active (read by saturn_cmdbuf.c) */
volatile unsigned int saturn_v1m_watchdog = 0;  /* stuck-draw recoveries (HUD 'W') */
#define V1M_WATCHDOG_FIELDS 8u
#if SATURN_V1_MANUAL && !defined(SATURN_WIN32)
#define V2_TVSTAT (*(volatile unsigned short *)0x25F80004u)   /* bit 3 = VBLANK */
static volatile int          v1m_req = 0, v1m_pending = 0, v1m_writing = 0, v1m_drawing = 0;
static volatile int          v1m_blank_shown = 0;
static volatile unsigned int v1m_seq = 0, v1m_trig_seq = 0;

/* Has VDP1 finished the last list we asked it to draw? */
static int v1m_draw_done(void)
{
    if (!v1m_drawing)
        return 1;
    if (v1m_seq == v1m_trig_seq && (V2_TVSTAT & 8u))
        return 0;                   /* change requested, not yet happened */
    return (V1_EDSR & 2u) != 0;     /* CEF: END fetched */
}

static volatile unsigned int v1m_pending_table = V1M_TABLE_A;

static void v1m_request_change(void)
{
    V1_FBCR = 0x0003u;              /* FCM=1 FCT=1: change at the next field */
    v1m_draw_table = v1m_pending_table;   /* the callback re-points slot 3 next */
    v1m_pending = 0;
    /* At this change the list drawn last goes on screen and the pending one
     * starts drawing: the VDP2 ground plane follows (saturn_vdp2_plane.c). */
    V2P_LIST_CHANGE(v1m_drawing);
    if (v1m_drawing)
        saturn_v2p_shown = saturn_v2p_drawing;
    saturn_v2p_drawing = saturn_v2p_pending;
    saturn_v2p_pending = -1;
    v1m_drawing = 1;
    v1m_trig_seq = v1m_seq;
}

/* Called from the vblank callback. */
static void v1m_vblank(void)
{
    v1m_seq++;
    if (!saturn_v1_manual)
    {
        if (v1m_req && !v1m_writing)
        {
            /* 1-cycle -> manual: "change" is the documented switch (p.39).
             * The live list is in table A; from now on the CPU alternates. */
            saturn_v1_manual = 1;
            V1_VRAM(V1M_TABLE_B + 0x02) = 0x0000u;   /* table B starts empty */
            V1_VRAM(V1M_TABLE_B + 0x00) = 0x8000u;
            v1m_pending_table = V1M_TABLE_A;
            v1m_double = 1;
            v1m_request_change();
        }
        return;
    }
    /* Watchdog (2026-11-15 hardware freeze: the 3D picture stopped
     * changing). A list takes ~1-2 fields; if CEF still isn't set 8 fields
     * after the request, VDP1 is stuck (never started, or looping), and
     * without this nothing would ever request another change. Give up on
     * that draw: the next request's frame change restarts VDP1 at slot 0. */
    if (v1m_drawing && (v1m_seq - v1m_trig_seq) > V1M_WATCHDOG_FIELDS && !(V1_EDSR & 2u))
    {
        saturn_v1m_watchdog++;
        v1m_drawing = 0;
        if (!v1m_pending && !v1m_writing)
        {
            v1m_pending_table = v1m_draw_table;   /* redraw the last list */
            v1m_pending = 1;
            saturn_v2p_pending = saturn_v2p_drawing;
            V2P_LIST_REDRAW();
        }
    }
    if (v1m_pending && !v1m_writing && v1m_draw_done())
        v1m_request_change();
}

/* Main loop: claim a command table to write. Returns with v1m_writing set
 * (the callback won't change frames meanwhile) and saturn_v1_write_table /
 * _limit pointing at a table VDP1 is not reading. With double tables that
 * is always the one VDP1 is NOT drawing: no wait. If it still holds an
 * older list waiting for its change, that list is simply replaced by the
 * newer one. */
static unsigned int v1m_begin_write(void)
{
    unsigned guard = 3500u;
    v1m_writing = 1;
    if (v1m_double)
    {
        unsigned int t = (v1m_draw_table == V1M_TABLE_A) ? V1M_TABLE_B : V1M_TABLE_A;
        saturn_v1_write_table = t;
        saturn_v1_write_limit = (t == V1M_TABLE_A) ? V1M_TABLE_A_END : V1M_TABLE_B_END;
        return guard;
    }
    while (!v1m_draw_done() && --guard)
    {
        for (int w = 0; w < 32; w++)
            __asm__ volatile("nop");
    }
    return guard;
}

static void v1m_end_write(void)
{
    v1m_pending_table = saturn_v1_write_table;
    saturn_v2p_pending = saturn_v2p_last;   /* this list's VDP2 plane slot */
    V2P_LIST_COMMIT();
    v1m_pending = 1;
    v1m_writing = 0;
}
#endif

/* Benchmark tour (saturn_bench.c, 2026-11-16): wait for the list committed
 * last to be drawn and return VDP1's draw time in FRT ticks (0 = not
 * available / timed out). It spins, so only the frozen-world benchmark
 * calls it. Timing: the change requested at vblank N happens at the end of
 * that vblank, when VDP1 starts the list; it sets EDSR.CEF when it fetches
 * the END command. */
volatile int          saturn_bench_vdp1_want  = 0;   /* set by saturn_bench.c */
volatile unsigned int saturn_bench_vdp1_ticks = 0;

unsigned int Saturn_Vdp1_BenchDrawTime(void)
{
#if SATURN_V1_MANUAL && !defined(SATURN_WIN32)
    extern unsigned int Saturn_ProfSpan(unsigned short f0, unsigned int v0);
    extern volatile unsigned int saturn_vbl_count;
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned int guard = 4000000u;
    unsigned int v0;
    unsigned short f0;

    if (!saturn_v1_manual)
        return 0xFFFF0000u;
    while ((v1m_pending || v1m_writing) && --guard)
        ;                                       /* change not requested yet */
    if (!guard)
        return 0xFFFF0001u;
    /* The frame change clears CEF (EDSR >>= 1) and starts the draw; until
     * then CEF still holds the previous draw's 1. Start the clock when it
     * drops. */
    while ((V1_EDSR & 2u) && --guard)
        ;
    if (!guard)
        return 0xFFFF0002u;
    do {
        v0 = saturn_vbl_count;
        unsigned char hi = frt[0];
        unsigned char lo = frt[1];
        f0 = (unsigned short)((hi << 8) | lo);
    } while (saturn_vbl_count != v0);
    while (!(V1_EDSR & 2u) && --guard)
        ;                                       /* CEF: END fetched */
    if (!guard)
        return 0xFFFF0003u;
    return Saturn_ProfSpan(f0, v0);
#else
    return 0u;
#endif
}

static void v1_vblank_callback(void)
{
#ifndef SATURN_WIN32
    {
        /* latch pad presses every field (i_system.c): a press shorter
         * than a slow frame is otherwise never sampled */
        extern void Saturn_PadVblankSample(void);
        Saturn_PadVblankSample();
    }
#endif
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
#if SATURN_V1_MANUAL && !defined(SATURN_WIN32)
    v1m_vblank();                   /* may re-point v1m_draw_table first */
#endif
    v1_patch_slot3_ex(1);
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

    saturn_v1w_spr = 1;

    /* Register v1_vblank_callback to re-patch VDP1 slot 3 and refresh VDP2
     * priority/sprite registers on every VBlank. */
    jo_core_add_vblank_callback(v1_vblank_callback);
    v1_write_priority_regs();
#if SATURN_V1_MANUAL && !defined(SATURN_WIN32)
    v1m_req = 1;                    /* switch to manual frame change at the next vblank */
#endif
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

        /* ARM THE STOP FIRST before enabling slots 0..3 walk into V1_USER0 */
        V1_VRAM(V1_USER0 + 0x00) = 0x8000u;  /* CTRL: END            */
        V1_VRAM(V1_USER0 + 0x02) = 0x0000;   /* LINK                 */
        /* slot 0: system clip (0,0)-(511,255) — the full VDP1 canvas.
         * FIXED 2026-09-09: was CTRL 0x0008 = USER clipping (low-nibble
         * types!) with the rect read from XC/YC = (0,0) — combined with
         * slot 1's 0x0009 (= system clip, also XC/YC=0) the whole list
         * drew into zero clip rects. System clip = 0x0009, rect
         * (0,0) .. XC/YC, bottom-right written at C (and B, harmless). */
        base = 0x0000u;
        V1_VRAM(base + 0x02) = 0x0000;
        V1_VRAM(base + 0x0C) = 0;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x10) = 511;
        V1_VRAM(base + 0x12) = 255;
        V1_VRAM(base + 0x14) = 511;     /* XC: system clip rect      */
        V1_VRAM(base + 0x16) = 255;     /*  = (0,0)..(511,255)       */
        V1_VRAM(base + 0x00) = V1_CMD_SYSCLIP;
        /* slot 1: user clip = full 3D view (rows 0..191, cols 16..335) */
        v1_write_clip(0x0020u, 16, 0, 319 + 16, 191);
        /* slot 2: local coord (16,0) — 320 view centered in 352 screen */
        base = 0x0040u;
        V1_VRAM(base + 0x02) = 0x0000;
        V1_VRAM(base + 0x0C) = 16;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x00) = 0x000Au;
        /* slot 3: local coord too (the callback re-patches it)      */
        base = 0x0060u;
        V1_VRAM(base + 0x02) = 0x0000;
        V1_VRAM(base + 0x0C) = 16;
        V1_VRAM(base + 0x0E) = 0;
        V1_VRAM(base + 0x00) = 0x000Au;
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
    n     = (int)numtextures + (int)numflats + (int)numsprites + V1_WARP_SLOTS;
    bytes = n * (int)sizeof(v1_tag_t);

    /* Allocate in fast 32-bit WRAM-H via Jo heap; fallback to WRAM-L scratch (14,232B < 16KB) */
    v1_tag[0] = (v1_tag_t *)jo_malloc_with_behaviour(bytes, 0);
    if (v1_tag[0] == NULL)
        v1_tag[0] = (v1_tag_t *)SATURN_WRAM_L_SCRATCH_ADDR;

    v1_tag[1]    = v1_tag[0];
    v1_tag_n     = n;
    v1_tag_flat0 = (int)numtextures;
    v1_tag_spr0  = (int)numtextures + (int)numflats;
    v1_tag_warp0 = v1_tag_spr0 + (int)numsprites;
    saturn_v1_flat_tags = &v1_tag[0][v1_tag_flat0];
    saturn_v1_wall_tags = &v1_tag[0][0];
    D_memset(v1_tag[0], 0, bytes);
    v1_page_flush[0] = v1_page_flush[1] = 1;
}

unsigned int saturn_v1_alloc_bytes = 0;   /* cumulative texture bytes uploaded to VDP1 VRAM */

static unsigned int v1_free_total(void)
{
    unsigned int cap = V1_PAGE0_END - v1_page_base[0];
    return (cap > v1_used_bytes) ? (cap - v1_used_bytes) : 0u;
}

unsigned int Saturn_Vdp1_ArenaFree(void)
{
    return v1_free_total();
}

/* First gap of at least `len` bytes between logged ranges (sorted log).
 * Returns its offset (never 0: the arena starts at 0x10000) or 0, and the
 * log index to insert at. */
static unsigned int v1_find_gap(unsigned int len, int *pos)
{
    unsigned int prev_end = v1_page_base[0];
    int i;
    for (i = 0; i < v1_log_n; i++)
    {
        unsigned int eo = (unsigned int)v1_log[i].off32 << 5;
        unsigned int ee = eo + ((unsigned int)v1_log[i].len32 << 5);
        if (eo >= prev_end && eo - prev_end >= len)
        {
            *pos = i;
            return prev_end;
        }
        if (ee > prev_end)
            prev_end = ee;
    }
    if (prev_end + len <= V1_PAGE0_END)
    {
        *pos = v1_log_n;
        return prev_end;
    }
    return 0;
}

/* First-fit allocate and log `bytes` for `tag`, keeping `reserve` bytes of
 * total free space untouched. 0 = no room. */
static unsigned int v1_alloc_fit(unsigned int bytes, v1_tag_t *tag, unsigned int reserve)
{
    unsigned int len = (bytes + 31u) & ~31u;
    unsigned int off;
    int pos, i;

    if (v1_log_n >= V1_LOG_MAX)
    {
        v1_log_full = 1;
        return 0;
    }
    if (v1_free_total() < len + reserve)
        return 0;
    off = v1_find_gap(len, &pos);
    if (off == 0)
        return 0;
    for (i = v1_log_n; i > pos; i--)
        v1_log[i] = v1_log[i - 1];
    v1_log[pos].off32 = (unsigned short)(off >> 5);
    v1_log[pos].len32 = (unsigned short)(len >> 5);
    v1_log[pos].age   = ++v1_age;
    v1_log[pos].tag   = tag;
    v1_log_n++;
    v1_used_bytes += len;
    saturn_v1_alloc_bytes += bytes;
    if (off + len > v1_arena_off)
        v1_arena_off = off + len;   /* high-water: the preload base uses it */
    return off;
}

/* Required texture: on failure the quad is refused THIS frame and the next
 * Reset evicts (evicting now could overwrite textures this frame's quads
 * already reference). */
static unsigned int v1_arena_alloc(int bytes, v1_tag_t *tag)
{
    unsigned int off = v1_alloc_fit((unsigned int)bytes, tag, 0u);
    if (off == 0)
    {
        v1_want_evict = 1;
        if ((unsigned int)bytes > v1_want_bytes)
            v1_want_bytes = (unsigned int)bytes;
        saturn_v1w_refuse++;        /* HUD 'X': texture missing this frame */
    }
    return off;
}

/* Optional textures (flat variant pyramids): only while `reserve` bytes stay
 * free for walls and sprites, and a failure never triggers an eviction —
 * the surface just draws without it. */
static unsigned int v1_arena_alloc_soft(int bytes, unsigned int reserve, v1_tag_t *tag)
{
    return v1_alloc_fit((unsigned int)bytes, tag, reserve);
}

/* Does tag field value `s` (SRCA = byte offset / 8) point into log entry e? */
static int v1_field_in(unsigned short s, const v1_alloc_t *e)
{
    unsigned int a, lo, hi;
    if (s == 0 || s == V1_TAG_BAD)
        return 0;
    a  = (unsigned int)s << 3;
    lo = (unsigned int)e->off32 << 5;
    hi = lo + ((unsigned int)e->len32 << 5);
    return a >= lo && a < hi;
}

/* Entry no longer referenced by its tag (re-uploaded elsewhere / cleared). */
static int v1_entry_stale(const v1_alloc_t *e)
{
    const v1_tag_t *t = e->tag;
    return !(v1_field_in(t->srca, e) || v1_field_in(t->srca_mono, e) || v1_field_in(t->srca_64, e));
}

/* Is tag `t` a flat's? Flats use srca_mono/srca_64 = 0 for "variant not
 * resident"; walls (and sprites) use field == srca for "no optional copy". */
static int v1_tag_is_flat(const v1_tag_t *t)
{
    int p;
    for (p = 0; p < 2; p++)
    {
        if (v1_tag[p] && t >= v1_tag[p] + v1_tag_flat0 && t < v1_tag[p] + v1_tag_spr0)
            return 1;
    }
    return 0;
}

static void v1_entry_drop(const v1_alloc_t *e)
{
    v1_tag_t *t = e->tag;
    int flat = v1_tag_is_flat(t);
    if (v1_field_in(t->srca, e))
    {
        int ws = v1_tag[0] ? (int)(t - (v1_tag[0] + v1_tag_warp0)) : -1;
        if (ws >= 0 && ws < V1_WARP_SLOTS)
            saturn_warp_tab[ws].srca = 0;           /* slave stops using it */
        t->srca = 0;
    }
    /* A wall's optional downsampled copy dropped while its main texture
     * stays resident must fall back to the alias (== srca), NOT 0: the wall
     * emitters test `srca_mono != srca` and would otherwise draw from
     * SRCA 0 + offset = the VDP1 command table (hardware 2026-11-15: a
     * band of regular garbage marks on a distant step riser, persisting
     * from the eviction frame until the texture was re-uploaded). */
    if (v1_field_in(t->srca_mono, e)) t->srca_mono = flat ? 0 : t->srca;
    if (v1_field_in(t->srca_64, e))   t->srca_64   = flat ? 0 : t->srca;
}

/* Read 16-bit field `idx` of the 32-byte VDP1 command at VRAM `base`
 * (both builds store commands as native uint32 words, first field high). */
static unsigned int v1_cmd_field(unsigned int base, int idx)
{
    uint32_t w = *(volatile uint32_t *)(V1_BASE + base + (unsigned int)((idx >> 1) << 2));
    return (idx & 1) ? (w & 0xFFFFu) : (w >> 16);
}

/* Warped per-piece textures are not v1_touch'ed on every lookup (that
 * binary search cost ~1 ms/frame of master time); their last use lives in
 * v1_warp_used instead. Used in the last two frames = recent. */
static unsigned int v1_warp_used_of(int slot);
static int v1_warp_recent(const v1_tag_t *t)
{
    int s;
    if (!v1_tag[0])
        return 0;
    s = (int)(t - (v1_tag[0] + v1_tag_warp0));
    return s >= 0 && s < V1_WARP_SLOTS && v1_warp_used_of(s) >= v1_age_mark_prev;
}

/* Free space by dropping the oldest log entries the live command list
 * doesn't reference, until `target` bytes are free and a hole fits the
 * largest refused upload. Returns 0 if almost nothing could be freed
 * (the live list covers the arena: caller full-flushes). */
static int v1_evict(unsigned int target)
{
    const unsigned int base = v1_page_base[0];
    int i, n;

    /* 1. drop stale entries (the log stays sorted by offset) */
    n = 0;
    for (i = 0; i < v1_log_n; i++)
    {
        v1_alloc_t e = v1_log[i];
        if ((unsigned int)e.off32 << 5 < base || v1_entry_stale(&e))
        {
            v1_used_bytes -= (unsigned int)e.len32 << 5;
            continue;
        }
        v1_log[n++] = e;
    }
    v1_log_n = n;
    for (i = 0; i < n; i++)
        v1_log_prot[i] = 0;

    /* 2. protect everything the live (last committed) list references:
     *    VDP1 re-draws it every vblank until the next commit */
    /* With double-buffered tables (manual mode) VDP1 may be drawing either
     * table and the other may hold a list waiting for its frame change:
     * protect both. */
    for (int tb = 0; v1_have_frame && tb < (v1m_double ? 2 : 1); tb++)
    {
        unsigned int c;
        const unsigned int t_base = tb ? V1M_TABLE_B : V1_USER0;
        const unsigned int t_end  = (v1m_double && !tb) ? V1M_TABLE_B : V1_CMD_LIMIT;
        for (c = t_base; c + 32u <= t_end; c += 32u)
        {
            unsigned int ctrl = v1_cmd_field(c, 0);
            unsigned int a;
            int lo, hi;
            if (ctrl & 0x8000u)
                break;                          /* END */
            if ((ctrl & 0xFu) > 2u)
                continue;                       /* untextured / clip / local */
            a = v1_cmd_field(c, 4) << 3;        /* CMDSRCA -> byte offset */
            if (a < V1_PAGE0_START)
                saturn_v1_bad_srca++;           /* textured from below the arena: garbage */
            lo = 0; hi = n - 1;
            while (lo <= hi)                    /* entry with off <= a < end */
            {
                int mid = (lo + hi) >> 1;
                unsigned int eo = (unsigned int)v1_log[mid].off32 << 5;
                if (a < eo)
                    hi = mid - 1;
                else if (a >= eo + ((unsigned int)v1_log[mid].len32 << 5))
                    lo = mid + 1;
                else
                {
                    v1_log_prot[mid] = 1;
                    v1_log[mid].age = v1_age;   /* in use: counts as recent */
                    break;
                }
            }
        }
    }

    /* 2b. a texture's copies live or die together: protect every entry of
     *    a tag that has one protected entry. A distant wall draws only its
     *    half-width copy (srca_mono); evicting the unreferenced main copy
     *    cleared tag->srca, so the next lookup re-uploaded BOTH copies -
     *    every frame (E1M2 start, hardware E16/251: 30 KB/frame, ~26% of
     *    the master in 5thPlanet). */
    for (i = 0; i < n; i++)
    {
        const v1_tag_t *t;
        int f;
        if (v1_log_prot[i] != 1)
            continue;
        t = v1_log[i].tag;
        for (f = 0; f < 3; f++)
        {
            unsigned short s = f == 0 ? t->srca : f == 1 ? t->srca_mono : t->srca_64;
            unsigned int a;
            int lo = 0, hi = n - 1;
            if (s == 0 || s == V1_TAG_BAD)
                continue;
            a = (unsigned int)s << 3;
            while (lo <= hi)
            {
                int mid = (lo + hi) >> 1;
                unsigned int eo = (unsigned int)v1_log[mid].off32 << 5;
                if (a < eo)
                    hi = mid - 1;
                else if (a >= eo + ((unsigned int)v1_log[mid].len32 << 5))
                    lo = mid + 1;
                else
                {
                    if (!v1_log_prot[mid] && v1_log[mid].tag == t)
                    {
                        v1_log_prot[mid] = 2;
                        v1_log[mid].age  = v1_age;
                    }
                    break;
                }
            }
        }
    }

    /* 3. drop the oldest unprotected entries until enough is free and a
     *    hole fits the largest upload refused since the last eviction */
    /* Textures uploaded in the last two frames but not (yet) in a command -
     * e.g. flat variant pyramids the master requests for every nearby
     * floor, which the slave only sometimes uses - are needed again next
     * frame. Evicting them for the free-space target alone re-uploaded
     * ~65 KB every frame at E1M2 (0,160): the uploads ate exactly the space
     * the eviction freed, a self-sustaining thrash (56 ms/frame). Only a
     * refused upload (need > 0) may take them (pass 1).
     *
     * One pass per class (2026-11-17): candidates are collected once,
     * sorted oldest first and dropped in that order (marked 3), then the
     * log is compacted once. The old loop rescanned the whole log (with an
     * uncached warp-slot read per entry) and shifted it down for every
     * victim - ~8 ms per eviction with ~700 entries (E1M1 courtyard walk). */
    {
        unsigned short *cand = (unsigned short *)SATURN_WRAM_L_EVICT_ADDR;   /* V1_LOG_MAX entries */
        const unsigned int need = (v1_want_bytes + 31u) & ~31u;
        int pass, k, nc, gap;
        for (pass = 0; pass < 2; pass++)
        {
            if (pass == 1 && need == 0)
                break;
            nc = 0;
            for (i = 0; i < n; i++)
            {
                if (v1_log_prot[i])
                    continue;
                if (pass == 0 && (v1_log[i].age >= v1_age_mark_prev || v1_warp_recent(v1_log[i].tag)))
                    continue;
                cand[nc++] = (unsigned short)i;
            }
            /* shell sort by age, ties by index (= the old first-found order) */
            for (gap = nc >> 1; gap > 0; gap >>= 1)
            {
                for (k = gap; k < nc; k++)
                {
                    unsigned short c = cand[k];
                    unsigned int   ca = v1_log[c].age;
                    int j = k;
                    while (j >= gap && (v1_log[cand[j - gap]].age > ca ||
                                        (v1_log[cand[j - gap]].age == ca && cand[j - gap] > c)))
                    {
                        cand[j] = cand[j - gap];
                        j -= gap;
                    }
                    cand[j] = c;
                }
            }
            for (k = 0; k < nc; k++)
            {
                int v = cand[k];
                if (v1_free_total() >= target)
                {
                    if (need == 0)
                        break;
                    /* first-fit hole for the refused upload, skipping
                     * entries already dropped (marked 3) */
                    {
                        unsigned int prev_end = v1_page_base[0];
                        int fits = 0;
                        for (i = 0; i < n && !fits; i++)
                        {
                            unsigned int eo, ee;
                            if (v1_log_prot[i] == 3)
                                continue;
                            eo = (unsigned int)v1_log[i].off32 << 5;
                            ee = eo + ((unsigned int)v1_log[i].len32 << 5);
                            if (eo >= prev_end && eo - prev_end >= need)
                                fits = 1;
                            if (ee > prev_end)
                                prev_end = ee;
                        }
                        if (fits || prev_end + need <= V1_PAGE0_END)
                            break;
                    }
                }
                v1_entry_drop(&v1_log[v]);
                saturn_v1_evicted_bytes += (unsigned int)v1_log[v].len32 << 5;
                v1_used_bytes -= (unsigned int)v1_log[v].len32 << 5;
                v1_log_prot[v] = 3;
            }
            if (k < nc)
                break;          /* target reached inside this pass */
        }
        /* compact: drop the marked entries, keep offset order */
        k = 0;
        for (i = 0; i < n; i++)
        {
            if (v1_log_prot[i] == 3)
                continue;
            v1_log[k] = v1_log[i];
            v1_log_prot[k] = v1_log_prot[i];
            k++;
        }
        n = k;
        v1_log_n = n;
    }
    v1_want_bytes = 0;
    Saturn_Vdp1_ClearTexCacheMRU();
    saturn_v1_evictions++;
    return v1_free_total() >= (16u * 1024u);
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
 * blocks (each covering 64 world texels at 2:1 horizontal LOD) for distant
 * walls (dx <= 24 screen pixels), saving 50% VDP1 DDA steps at only 0.5x extra
 * VDP1 VRAM when >= 128 KB of arena headroom remains. */
static void v1_copy_32_blocks(unsigned int off, const byte *src, int w, int h)
{
    int num_blocks = w >> 6;
    int b, y;
    for (b = 0; b < num_blocks; b++)
    {
        unsigned int block_off = off + (unsigned int)(b * (32 * h));
        const byte *bsrc = src + b * 64;
#ifdef SATURN_WIN32
        uint8_t *dst = (uint8_t *)(V1_BASE + block_off);
        int x;
        for (y = 0; y < h; y++)
        {
            const byte *sr = bsrc + y * w;
            uint8_t *dr = dst + y * 32;
            for (x = 0; x < 32; x++)
                dr[x] = sr[x * 2];
        }
#else
        volatile unsigned short *dst = (volatile unsigned short *)(V1_BASE + block_off);
        int xw;
        for (y = 0; y < h; y++)
        {
            const byte *sr = bsrc + y * w;
            for (xw = 0; xw < 16; xw++)
                *dst++ = (unsigned short)(((unsigned)sr[xw * 4] << 8) | (unsigned)sr[xw * 4 + 2]);
        }
#endif
    }
}

/* Per-page persistent texture cache (walls: texnum >= 1; flats:
 * texnum = -(flatnum + 1)). Returns the SRCA of the texture in the
 * current page (uploading it on a miss) and its size; 0 = not drawable
 * (unusable texture, or page full this frame — counted by the caller in
 * saturn_v1w_refuse). */
/* A cache hit counts as use: refresh the age of the log entry holding SRCA
 * `srca` (8-byte units), so a texture requested every frame is never "old"
 * to v1_evict even when no command references it yet. */
static void v1_touch(unsigned int srca)
{
    unsigned int a = srca << 3;
    int lo = 0, hi = v1_log_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        unsigned int eo = (unsigned int)v1_log[mid].off32 << 5;
        if (a < eo)
            hi = mid - 1;
        else if (a >= eo + ((unsigned int)v1_log[mid].len32 << 5))
            lo = mid + 1;
        else
        {
            v1_log[mid].age = v1_age;
            return;
        }
    }
}

/* A strip-major wall's optional copies (half-width 32-blocks for distant
 * walls, or a row-major copy plus a 64-wide doubled copy for 32-wide
 * textures), made only while `reserve` bytes stay free. Walls point
 * srca_mono / srca_64 at srca when they have none. */
static void v1_wall_opt_copies(v1_tag_t *tag, const byte *src, int tw, int th, unsigned int reserve)
{
    /* Optional: a failed allocation (fragmented arena) must not request an
     * eviction the way a required upload does (v1_arena_alloc): that one
     * may drop textures in use, which were re-uploaded next frame - a
     * thrash with late copies (hardware 2026-11-16, E9/396). */
#define v1_arena_alloc(b, t) v1_alloc_fit((unsigned int)(b), (t), 0u)
    if (tw >= 64 && (tw & 63) == 0)
    {
        int bytes_32 = (tw >> 1) * th;
        if (v1_free_total() >= (unsigned int)bytes_32 + reserve)
        {
            unsigned int off_32 = v1_arena_alloc(bytes_32, tag);
            if (off_32 != 0)
            {
                v1_copy_32_blocks(off_32, src, tw, th);
                tag->srca_mono = (unsigned short)(off_32 / 8u);
            }
        }
    }
    else if (v1_free_total() >= (unsigned int)(tw * th) + reserve)
    {
        unsigned int off_mono = v1_arena_alloc(tw * th, tag);
        if (off_mono != 0)
        {
            v1_copy_block(off_mono, src, tw * th);
            tag->srca_mono = (unsigned short)(off_mono / 8u);
            if (tw == 32 && th <= 16 &&
                v1_free_total() >= (unsigned int)(64 * th) + reserve)
            {
                unsigned int off_64 = v1_arena_alloc(64 * th, tag);
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
#undef v1_arena_alloc
}

/* Late optional copies: a wall uploaded while VRAM was tight never got its
 * copies, and drew as many more strips for as long as it stayed resident
 * (E1M1 Zigzag: 166 -> 228 wall quads when the first frame there found
 * the arena full; the eviction a frame later freed 77 KB). The wall
 * emitter's tag hit (saturn_hw_walls.c HW_LoadWallTexCtx) retries one wall
 * per frame while saturn_v1_late_copy_armed (set per frame with headroom). */
#ifndef V1_LATE_COPY_RESERVE
#define V1_LATE_COPY_RESERVE (V1_EVICT_LOWWATER + 16u * 1024u)   /* see V1_WARP_RESERVE */
#endif
int saturn_v1_late_copy_armed = 0;
unsigned int saturn_v1_late_copies = 0;

void Saturn_Vdp1_WallLateCopy(int texnum)
{
    extern int saturn_wall_subdivision;
    stexinfo_t ti;
    v1_tag_t  *tag;
    int        t;
    if (!v1_tag[0] || !saturn_wall_subdivision || texnum <= 0 || texnum >= (int)numtextures)
        return;
    t = texturetranslation ? (int)texturetranslation[texnum] : texnum;
    if (t < 0 || t >= (int)numtextures)
        return;
    tag = &v1_tag[0][t];
    /* narrow walls (<= 16 wide) never get copies: no attempt used up */
    if (!tag->srca || tag->srca == V1_TAG_BAD || tag->srca_mono != tag->srca || tag->w8 <= 2)
        return;
    saturn_v1_late_copy_armed = 0;          /* one attempt per frame */
    if (!Saturn_TexInfo(texnum, &ti) || !ti.data || ti.w != ((int)tag->w8 << 3) ||
        ti.h != (int)tag->h)
        return;
    v1_wall_opt_copies(tag, ti.data, ti.w, ti.h, V1_LATE_COPY_RESERVE);
    if (tag->srca_mono != tag->srca)
        saturn_v1_late_copies++;
}

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
            v1_touch(tag->srca);
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
            v1_touch(tag->srca);
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
        unsigned int off_strip = v1_arena_alloc(strip_bytes, tag);
        if (off_strip == 0)
            return 0;
        v1_copy_strip_major(off_strip, src, tw, th);
        tag->srca = (unsigned short)(off_strip / 8u);
        tag->srca_mono = tag->srca;
        tag->srca_64   = tag->srca;
        v1_wall_opt_copies(tag, src, tw, th, V1_OPT_RESERVE);
    }
    else if (texnum < 0 && tw == 64 && th == 64)
    {
        /* Flat hardware mipmap pyramid in VDP1 VRAM (5,440 bytes contiguous):
         *   +0    (srca + 0):   64x64 Level 0 (4,096 B, CMDSIZE 0x0840)
         *   +4096 (srca + 512): 32x32 Level 1 (1,024 B, CMDSIZE 0x0420)
         *   +5120 (srca + 640): 16x16 Level 2 (  256 B, CMDSIZE 0x0210)
         *   +5376 (srca + 672):   8x8 Level 3 (   64 B, CMDSIZE 0x0108) */
        off = v1_arena_alloc(5440, tag);
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
                const byte *sr = src + (y * 2 + 1) * 64;
                for (x = 0; x < 32; x++)
                    dst32[y * 32 + x] = sr[x * 2 + 1];
            }
            for (y = 0; y < 16; y++)
            {
                const byte *sr = src + (y * 4 + 2) * 64;
                for (x = 0; x < 16; x++)
                    dst16[y * 16 + x] = sr[x * 4 + 2];
            }
            for (y = 0; y < 8; y++)
            {
                const byte *sr = src + (y * 8 + 4) * 64;
                for (x = 0; x < 8; x++)
                    dst8[y * 8 + x] = sr[x * 8 + 4];
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
                const byte *sr = src + (y * 2 + 1) * 64;
                for (xw = 0; xw < 16; xw++)
                    *dst32++ = (unsigned short)(((unsigned)sr[xw * 4 + 1] << 8) | (unsigned)sr[xw * 4 + 3]);
            }
            for (y = 0; y < 16; y++)
            {
                const byte *sr = src + (y * 4 + 2) * 64;
                for (xw = 0; xw < 8; xw++)
                    *dst16++ = (unsigned short)(((unsigned)sr[xw * 8 + 2] << 8) | (unsigned)sr[xw * 8 + 6]);
            }
            for (y = 0; y < 8; y++)
            {
                const byte *sr = src + (y * 8 + 4) * 64;
                for (xw = 0; xw < 4; xw++)
                    *dst8++ = (unsigned short)(((unsigned)sr[xw * 16 + 4] << 8) | (unsigned)sr[xw * 16 + 12]);
            }
        }
#endif
        tag->srca = (unsigned short)(off / 8u);
        /* For flats these two fields hold the lazily uploaded variants
         * (Saturn_Vdp1_GetFlatSrcaVariant): 0 = not resident yet. */
        tag->srca_mono = 0;    /* transposed pyramid */
        tag->srca_64   = 0;    /* LOD1 2x2-tile pyramid */
    }
    else if (texnum > 0 && tw == 8)
    {
        /* VDP1 wall strip emitters use 16-texel (2u << 8) strip width when
         * u_span > tw or near-clipped (e.g. DOORTRAK on 16-unit door jambs).
         * Tile 8-pixel-wide wall textures twice horizontally into a 16xth block. */
        int y;
        off = v1_arena_alloc(16 * th, tag);
        if (off == 0)
            return 0;
        for (y = 0; y < th; y++)
        {
            v1_copy_block(off + (unsigned)(y * 16), src + y * 8, 8);
            v1_copy_block(off + (unsigned)(y * 16 + 8), src + y * 8, 8);
        }
        tw = 16;
        tag->srca = (unsigned short)(off / 8u);
        tag->srca_mono = tag->srca;
        tag->srca_64   = tag->srca;
    }
    else
    {
        off = v1_arena_alloc(tw * th, tag);
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

    off = v1_arena_alloc(bytes, tag);
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
    k = saturn_shade_bank[(shade < 0) ? 0 : ((shade > 31) ? 31 : shade)];

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
    /* Texture cache with eviction (see v1_evict). Evict only right after a
     * committed frame: Reset can run twice per frame, and the live list
     * in VDP1 VRAM must be the one VDP1 is actually re-drawing. */
    int v1_frame_boundary = v1_page_committed || !v1_have_frame;
    if (v1_frame_boundary)
    {
        /* tick per frame too, so marks advance even without uploads */
        v1_age_mark_prev = v1_age_mark_cur;
        v1_age_mark_cur  = ++v1_age;
    }
    v1_page_off[0] = v1_arena_off;
    v1_page_off[1] = v1_arena_off;
    v1_tex_page = 0;
    v1_page_committed = 0;

    v1_tags_try_alloc();
    int v1_do_flush = v1_page_flush[0] || v1_log_full;
    /* Low-water eviction that freed nothing is skipped until something is
     * uploaded again (2026-11-16): with the arena near full and every
     * entry in use, it rescanned both command tables in VDP1 VRAM (slow
     * B-bus reads) every frame for nothing - ~5 ms/frame at E1M2 start.
     * A refused upload (v1_want_evict) still always evicts. */
    static unsigned int evict_idle_alloc = 0xFFFFFFFFu;
    /* An unproductive low-water eviction (freed < V1_EVICT_MIN_GAIN) also
     * waits V1_EVICT_BACKOFF frames (2026-11-17): walking through the E1M1
     * courtyard kept the arena below low water with everything used in the
     * last two frames, so it re-scanned both command tables in VDP1 VRAM
     * (~1200 B-bus reads + a search each) almost every frame to free a few
     * KB - ~2.7 ms/frame of master time while moving. */
#ifndef V1_EVICT_BACKOFF
#define V1_EVICT_BACKOFF  8u
#endif
#define V1_EVICT_MIN_GAIN (8u * 1024u)
    static unsigned int evict_backoff = 0;
    if (v1_frame_boundary && evict_backoff)
        evict_backoff--;
    if (!v1_do_flush && v1_frame_boundary
        && (v1_want_evict
            || (v1_free_total() < V1_EVICT_LOWWATER && saturn_v1_alloc_bytes != evict_idle_alloc
                && !evict_backoff)))
    {
        unsigned int free_before = v1_free_total();
        int refused = v1_want_evict;
        v1_want_evict = 0;
        if (!v1_evict(V1_EVICT_TARGET))
            v1_do_flush = 1;    /* live list fragments the whole ring */
        evict_idle_alloc = (v1_free_total() == free_before) ? saturn_v1_alloc_bytes : 0xFFFFFFFFu;
        if (!refused && v1_free_total() < free_before + V1_EVICT_MIN_GAIN)
            evict_backoff = V1_EVICT_BACKOFF;
    }
    if (v1_do_flush)
    {
#ifndef SATURN_WIN32
        /* Never overwrite VDP1 texture VRAM while VDP1 is still rasterizing the previous frame */
        if (v1_have_frame)
        {
            unsigned guard = 2500u;
            while (!(V1_EDSR & 2u) && --guard)
            {
                if (!(V1_EDSR & 3u))
                    break;
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
            /* warp tags were just cleared with the sprites: drop the slots */
            D_memset(saturn_warp_tab, 0, sizeof(saturn_warp_tab));
            /* The flats' lazily uploaded variants (transposed / LOD1 tile,
             * Saturn_Vdp1_GetFlatSrcaVariant) live in this flushable arena,
             * unlike the preloaded base pyramids: forget them so they are
             * re-uploaded instead of pointing at reused VRAM. */
            {
                int i;
                for (i = v1_tag_flat0; i < v1_tag_spr0; i++)
                {
                    v1_tag[0][i].srca_mono = 0;
                    v1_tag[0][i].srca_64   = 0;
                }
            }
        }
        {
            /* anything else uploaded since the preload (e.g. a flat that
             * wasn't preloaded) must not keep pointing at reused VRAM */
            int i;
            for (i = 0; i < v1_log_n; i++)
                v1_entry_drop(&v1_log[i]);
        }
        v1_page_off[0] = v1_page_base[0];
        v1_page_off[1] = v1_page_base[0];
        v1_page_flush[0] = 0;
        v1_page_flush[1] = 0;
        v1_log_n = 0;
        v1_log_full = 0;
        v1_want_evict = 0;
        v1_used_bytes = 0;
        v1_arena_end = V1_PAGE0_END;
        Saturn_Vdp1_ClearTexCacheMRU();
        {
            extern unsigned int saturn_v1_page_flushes;
            saturn_v1_page_flushes++;   /* HUD 'Y': full texture cache flushes */
        }
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
    /* Shade (COLORMAP row 0..31) -> nearest shaded CRAM bank k (0..7,
     * rows 0,4,9,13,18,22,27,31 — see saturn_light_lut.h). */
    k = saturn_shade_bank[(shade < 0) ? 0 : ((shade > 31) ? 31 : shade)];
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

/* Flat texture variants for the baked QUV mapping (tools/bake_flats.py).
 * Both use the same 5,440-byte pyramid layout as the normal flat
 * (64x64 @+0, 32x32 @+4096, 16x16 @+5120, 8x8 @+5376) so HW_FlatSizeLOD
 * picks levels from them unchanged:
 *   variant 1  transposed: T[u][v] = F[v][u], so quads trimmed in X can
 *              select texture COLUMNS as rows (VDP1 can only slice rows)
 *   variant 2  LOD1 tile: level n = 2x2 tiling of the flat's next-smaller
 *              mip, so one 64-texel texture covers a 128-unit LOD1 cell
 * Point sampling matches the normal pyramid (centre texel of each block).
 * Uploaded on first use only, row by row (no large staging buffer). */
int Saturn_Vdp1_GetFlatSrcaVariant(int flatnum, int variant)
{
    static const unsigned short lvl_ofs[4] = { 0, 4096, 5120, 5376 };
    static unsigned int rowbuf[16];        /* 64 bytes, 4-aligned */
    v1_tag_t       *tag;
    unsigned short *field;
    const byte     *src;
    unsigned int    off;
    int             f, level;

    if (flatnum < 0 || flatnum >= (int)numflats || (variant != 1 && variant != 2))
        return 0;
    if (!Saturn_Vdp1_GetFlatSrca(flatnum))     /* base pyramid + tag table */
        return 0;
    f = flattranslation ? (int)flattranslation[flatnum] : flatnum;
    if (f < 0 || f >= (int)numflats)
        f = flatnum;
    tag   = &v1_tag[v1_tex_page][v1_tag_flat0 + f];
    field = (variant == 1) ? &tag->srca_mono : &tag->srca_64;
    if (*field)
    {
        if (*field == V1_TAG_BAD)
            return 0;
        v1_touch(*field);
        return (int)*field;
    }

    src = Saturn_FlatData(flatnum);
    if (!src)
        return 0;
    /* Above low water like the warp textures and late wall copies
     * (2026-11-17): with a 32 KB reserve these optional variants kept the
     * arena below V1_EVICT_LOWWATER, so eviction ran every few frames and
     * refused wall uploads while walking through the E1M1 courtyard. */
#ifndef V1_FLAT_VAR_RESERVE
#define V1_FLAT_VAR_RESERVE (V1_EVICT_LOWWATER + 8u * 1024u)
#endif
    off = v1_arena_alloc_soft(5440, V1_FLAT_VAR_RESERVE, tag);
    if (off == 0)
        return 0;
    {
        extern unsigned int saturn_v1_variant_uploads;
        saturn_v1_variant_uploads++;
    }
    /* Tight per-level loops (a per-texel sampling call cost ~2.5M SH-2
     * cycles for a dozen variants). Level L, block step s = 1 << L:
     * transposed texel (y, x) = src[(x*s + s/2)*64 + y*s + s/2]; the tile
     * uses step 2s and wraps x, y at n/2. */
    for (level = 0; level < 4; level++)
    {
        int   n = 64 >> level, y, x;
        byte *row = (byte *)rowbuf;

        if (variant == 1)
        {
            /* transposed: row y, column x = source row x, column y */
            const int step = 1 << level, half = step >> 1;
            const int xstride = step * 64;
            for (y = 0; y < n; y++)
            {
                const byte *s = src + half * 64 + y * step + half;
                for (x = 0; x < n; x++, s += xstride)
                    row[x] = *s;
                v1_copy_block(off + lvl_ofs[level] + (unsigned int)(y * n), row, n);
            }
        }
        else
        {
            /* LOD1 2x2 tile: a level-(L+1) image repeated 2x2 */
            const int step = 2 << level, half = step >> 1, m = n >> 1;
            for (y = 0; y < n; y++)
            {
                const byte *s = src + ((y % m) * step + half) * 64 + half;
                for (x = 0; x < m; x++)
                    row[x] = s[x * step];
                for (x = 0; x < m; x++)
                    row[m + x] = row[x];
                v1_copy_block(off + lvl_ofs[level] + (unsigned int)(y * n), row, n);
            }
        }
    }
    *field = (unsigned short)(off / 8u);
    return (int)*field;
}

/* Per-piece warped flat textures (baked QUV_WARP quads, saturn_hw_flats.c
 * HW_PrepareWarp). VDP1 maps a texture rectangle bilinearly onto a quad's
 * corners and can only pick texture rows, so a floor piece cut by a diagonal
 * (or trimmed on both axes) squashed the whole flat into itself (E1M2 start
 * ceiling lights, hardware 2026-11-16). Each such piece gets a w x h texture
 * resampled from the flat through that same bilinear map, so the drawn quad
 * shows world-aligned texels:
 *   texel (s, r) = flat at P(u, a), u = (s + .5) / w, a = (r + .5) / h,
 *   P(u, a) = lerp(lerp(P3, P2, u), lerp(P0, P1, u), a)
 * (row 0 on the p3-p2 edge, column 0 on the p0-p3 edge: the flat emitters'
 * convention), stored as 16-wide column strips (saturn_hw_flats.c
 * HW_EmitWarpStrips). wx/wy = corners in 1/16 map units. Master only; cached in a
 * direct-mapped slot per (quad, floor/ceiling); generation is budgeted per
 * frame, a miss over budget returns 0 (caller keeps the row mapping). */
#ifndef V1_WARP_BUDGET
#define V1_WARP_BUDGET   6144       /* texels generated per frame */
#endif
#ifndef V1_WARP_RESERVE
/* Optional textures must never take the arena below the low-water mark:
 * every upload below it re-ran v1_evict towards V1_EVICT_TARGET, which
 * evicted and re-uploaded textures each frame (hardware 2026-11-16, E1M2
 * start: 4.4 FPS, HUD E9/344). */
#define V1_WARP_RESERVE  (V1_EVICT_LOWWATER + 8u * 1024u)
#endif
/* Slot table, read by the slave flat worker through the uncached alias
 * (saturn_hw_flats.c HW_WarpFind) and written only by the master: srca is
 * cleared first on a re-key / drop and set last after a generation. The
 * slave stamps `used` with saturn_warp_frame when it draws the texture. */
saturn_warp_slot_t  saturn_warp_tab[V1_WARP_SLOTS];
volatile uint16_t   saturn_warp_frame = 1;
volatile uint8_t    saturn_warp_room = 0;
int                 v1_warp_budget = 0;
unsigned int        saturn_v1_warp_gens = 0, saturn_v1_warp_miss = 0;
unsigned int        saturn_v1_warp_miss_c = 0, saturn_v1_warp_miss_b = 0, saturn_v1_warp_miss_a = 0;

static uint16_t v1_warp_used16(int slot)
{
#ifndef SATURN_WIN32
    const volatile saturn_warp_slot_t *e = (const volatile saturn_warp_slot_t *)
        ((uintptr_t)&saturn_warp_tab[slot] | SATURN_WRAM_H_UNCACHED);
    return e->used;
#else
    return saturn_warp_tab[slot].used;
#endif
}
static unsigned int v1_warp_used_of(int slot)
{
    /* recent = drawn or made in the last two frames */
    return (uint16_t)(saturn_warp_frame - v1_warp_used16(slot)) <= 2u ? v1_age_mark_prev : 0u;
}

/* Room for a warp texture of `bytes` without going under V1_WARP_RESERVE
 * (HW_WarpService skips the corner work when there isn't). */
int Saturn_Vdp1_WarpRoom(int bytes)
{
    return v1_tag[0] && v1_free_total() >= (unsigned int)bytes + V1_WARP_RESERVE && v1_warp_budget >= bytes;
}

/* Called once per rendered 3D frame (saturn_hw_render.c frame setup),
 * before any flat job of the frame. */
void Saturn_Vdp1_WarpNewFrame(void)
{
    v1_warp_budget = V1_WARP_BUDGET;
    saturn_warp_frame++;
    /* textures can be made this frame: the flat workers may draw the
     * one-frame placeholder for a missing one (else the row mapping) */
    saturn_warp_room = (uint8_t)(v1_tag[0] && v1_free_total() >= 2048u + V1_WARP_RESERVE);
    saturn_v1_late_copy_armed = v1_tag[0] && v1_free_total() >= V1_LATE_COPY_RESERVE + 8u * 1024u;
}

int Saturn_Vdp1_GetWarpSrca(int quad, int is_ceil, int flatnum, int lv,
                            const int32_t *wx, const int32_t *wy, int w, int h)
{
    static unsigned int rowbuf[16];        /* 64 bytes, 4-aligned */
    const uint32_t k1  = ((uint32_t)quad << 1) | (uint32_t)(is_ceil & 1);
    const uint32_t key = k1 + 1u;
    int slot, way;
    saturn_warp_slot_t *e;
    v1_tag_t   *tag;
    const byte *src;
    unsigned int off;
    int f, r, recent;

    /* w = 8, or a multiple of 16 (stored as 16-wide column strips) */
    if (flatnum < 0 || flatnum >= (int)numflats || w < 8 || w > 64 || (w != 8 && (w & 15)) || h < 1 || h > 64)
        return 0;
    if (v1_tag[0] == NULL)
        return 0;
    f = flatnum;                            /* callers pass the translated flat */
    /* the piece's own slot if it has one, else an empty way, else the way
     * not drawn recently (a recent texture may still be in a command table
     * VDP1 is drawing) */
    {
        const int s0 = saturn_warp_slot(k1, 0), s1 = saturn_warp_slot(k1, 1);
        const int r0 = (uint16_t)(saturn_warp_frame - v1_warp_used16(s0)) <= 2u;
        const int r1 = (uint16_t)(saturn_warp_frame - v1_warp_used16(s1)) <= 2u;
        if (saturn_warp_tab[s0].key == key)      way = 0;
        else if (saturn_warp_tab[s1].key == key) way = 1;
        else if (!v1_tag[0][v1_tag_warp0 + s0].srca) way = 0;
        else if (!v1_tag[0][v1_tag_warp0 + s1].srca) way = 1;
        else if (!r0) way = 0;
        else if (!r1) way = 1;
        else
        {
            saturn_v1_warp_miss++; saturn_v1_warp_miss_c++;
            return 0;
        }
        slot = way ? s1 : s0;
    }
    e = &saturn_warp_tab[slot];
    tag = &v1_tag[0][v1_tag_warp0 + slot];
    recent = (uint16_t)(saturn_warp_frame - v1_warp_used16(slot)) <= 2u;

    if (tag->srca && tag->srca != V1_TAG_BAD && e->key == key &&
        e->lv == (uint8_t)lv && e->flat == (uint16_t)f && e->wh == (uint16_t)((w << 8) | h))
        return (int)tag->srca;                          /* already resident */
    if (tag->srca && tag->srca != V1_TAG_BAD && e->key == key &&
        (uint16_t)(saturn_warp_frame - e->made) <= 3u)
        return (int)tag->srca;  /* re-made under 4 frames ago: keep it (only one
                                 * previous copy stays protected, srca_mono) */
    if (v1_warp_budget < w * h)
    {
        saturn_v1_warp_miss++; saturn_v1_warp_miss_b++;
        return 0;
    }
    src = Saturn_FlatDataExact(flatnum);
    if (!src)
        return 0;
    /* Re-key the slot. The same piece's previous texture (other level or
     * animation frame) may still be in a command table VDP1 draws: keep it
     * referenced through srca_mono until the next re-key, so v1_evict
     * doesn't count it stale yet. Anything older becomes stale. */
    e->srca = 0;
    tag->srca_mono = (e->key == key && recent) ? tag->srca : 0;
    tag->srca = 0;
    tag->srca_64 = 0;
    e->key = 0;
    off = v1_arena_alloc_soft(w * h, V1_WARP_RESERVE, tag);
    if (off == 0)
    {
        saturn_v1_warp_miss++; saturn_v1_warp_miss_a++;
        return 0;
    }
    v1_warp_budget -= w * h;
    saturn_v1_warp_gens++;

    /* 1/16-unit corners with 8 more fraction bits: |coord| < 2^19, so all
     * products below fit 32 bits after the row lerp (done in 64 bits). */
    for (r = 0; r < h; r++)
    {
        const int32_t a = (int32_t)(((2 * r + 1) << 15) / h);     /* 0.16 */
        int32_t lx = (wx[3] << 8) + (int32_t)(((int64_t)((wx[0] - wx[3]) << 8) * a) >> 16);
        int32_t ly = (wy[3] << 8) + (int32_t)(((int64_t)((wy[0] - wy[3]) << 8) * a) >> 16);
        int32_t rx = (wx[2] << 8) + (int32_t)(((int64_t)((wx[1] - wx[2]) << 8) * a) >> 16);
        int32_t ry = (wy[2] << 8) + (int32_t)(((int64_t)((wy[1] - wy[2]) << 8) * a) >> 16);
        int32_t dx = (rx - lx) / w, dy = (ry - ly) / w;
        int32_t x = lx + (dx >> 1), ny = -(ly + (dy >> 1));
        byte *row = (byte *)rowbuf;
        int s;
        for (s = 0; s < w; s++, x += dx, ny -= dy)
            row[s] = src[(((ny >> 12) & 63) << 6) | ((x >> 12) & 63)];
        {
            /* strip k = columns k*sw.., an sw x h texture at off + k*sw*h */
            const int sw = saturn_warp_strip_w(w);
            for (s = 0; s < w; s += sw)
                v1_copy_block(off + (unsigned int)(s * h + r * sw), row + s, sw);
        }
    }
    tag->srca = (unsigned short)(off / 8u);
    e->key  = key;
    e->flat = (uint16_t)f;
    e->wh   = (uint16_t)((w << 8) | h);
    e->lv   = (uint8_t)lv;
    e->used = saturn_warp_frame;
    e->made = saturn_warp_frame;
    e->srca = tag->srca;                    /* last: the slave may use it now */
    return (int)tag->srca;
}
/* Backing colour for raster-hole patches (saturn_hw_flats.c
 * HW_FlatHoleBacking): the flat's most common palette index, computed once
 * per flat from its raw texels (no palette needed; a real texture colour
 * blends in where a hole lets it show). Master only. */
#define V1_FLAT_AVG_MAX 1024
int Saturn_Vdp1_FlatAvgColor(int flatnum)
{
    static unsigned char avg[V1_FLAT_AVG_MAX];
    static unsigned char have[V1_FLAT_AVG_MAX];
    static unsigned short hist[256];
    int f, i, best;
    const byte *src;

    if (flatnum < 0 || flatnum >= (int)numflats)
        return 0;
    f = flattranslation ? (int)flattranslation[flatnum] : flatnum;
    if (f < 0 || f >= (int)numflats || f >= V1_FLAT_AVG_MAX)
        return 0;
    if (have[f])
        return avg[f];
    src = Saturn_FlatData(flatnum);
    if (!src)
        return 0;
    for (i = 0; i < 256; i++)
        hist[i] = 0;
    for (i = 0; i < 4096; i++)
        hist[src[i]]++;
    best = 0;
    for (i = 1; i < 256; i++)
        if (hist[i] > hist[best])
            best = i;
    avg[f] = (unsigned char)best;
    have[f] = 1;
    return best;
}

void Saturn_Vdp1_ClearPages(void)
{
    v1_tags_try_alloc();
    D_memset(saturn_warp_tab, 0, sizeof(saturn_warp_tab));
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
    v1_log_n = 0;
    v1_log_full = 0;
    v1_want_evict = 0;
    v1_used_bytes = 0;
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
    /* preloaded pyramids are permanent: never evicted */
    v1_log_n = 0;
    v1_log_full = 0;
    v1_used_bytes = 0;
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
        saturn_v1w_spr = 1;
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
                unsigned short lo = frt[1];
                tc0 = (unsigned short)((hi << 8) | lo);
            } while (__builtin_expect(saturn_vbl_count != vc0, 0));
            /* Wait for VDP1 CEF (bit 1 of V1_EDSR) before overwriting the
             * command table in VDP1 VRAM. VDP1 clears CEF only at Scanline 262
             * (VBlank-OUT / FRAME_CHANGE) when auto-draw (PTMR=2) starts walking
             * Slot 0, and sets CEF=1 as soon as the 0x8000 END command is reached.
             * Never break out early at Scanline 224 (VBlank-IN) while CEF==0, or
             * Saturn_CmdBuf_Deliver() will overwrite the tail of the command table
             * (foreground flats and player weapon) while VDP1 is still drawing! */
            unsigned guard = 3500u;

#if SATURN_V1_MANUAL
            if (saturn_v1_manual)
            {
                /* Manual mode: wait only while VDP1 is still drawing the
                 * list we last handed it; usually it finished long ago. */
                guard = v1m_begin_write();
            }
            else
#endif
            while (!(V1_EDSR & 2u) && --guard)
            {
                if (saturn_vbl_count != vc0 && !(V1_EDSR & 3u))
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
                unsigned short lo = frt[1];
                td0 = (unsigned short)((hi << 8) | lo);
            } while (__builtin_expect(saturn_vbl_count != vd0, 0));
            Saturn_CmdBuf_Deliver();
            saturn_prof_del += Saturn_ProfSpan(td0, vd0);
        }
        else if (v1q != NULL && v1q_count > 0)
        {
#if SATURN_V1_MANUAL
            /* Legacy writer: always table A. With double tables, make sure
             * VDP1 is not drawing A before overwriting it. */
            if (v1m_double)
            {
                unsigned g = 3500u;
                while (v1m_draw_table == V1M_TABLE_A && !v1m_draw_done() && --g)
                    for (int w = 0; w < 32; w++)
                        __asm__ volatile("nop");
                saturn_v1_write_table = V1M_TABLE_A;
                saturn_v1_write_limit = V1M_TABLE_A_END;
            }
#endif
            v1_write_commands();
        }

        /* the live list now references v1_tex_page: the next Reset may
         * flip the CPU to the other page */
        v1_page_committed = 1;
        v1_have_frame = 1;
        v1_hide_sprites = 0;
        v1_hide_pending = 0;
        v1_patch_slot3();
        v1_write_priority_regs();
#if SATURN_V1_MANUAL
        if (saturn_v1_manual)
        {
            v1m_blank_shown = 0;
            v1m_end_write();        /* the vblank callback requests the change */
            {
                /* Benchmark tour: time this list's VDP1 draw right after
                 * the commit (later, the draw may already be over). */
                extern volatile int          saturn_bench_vdp1_want;
                extern volatile unsigned int saturn_bench_vdp1_ticks;
                if (saturn_bench_vdp1_want)
                {
                    saturn_bench_vdp1_ticks = Saturn_Vdp1_BenchDrawTime();
                    saturn_bench_vdp1_want = 0;
                }
            }
        }
#endif
#endif
    }
    else
    {
#ifdef SATURN_WIN32
        *(uint32_t *)(V1_BASE + V1_USER0) = 0x80000000u;
#else
#if SATURN_V1_MANUAL
        if (saturn_v1_manual)
        {
            /* No 3D this frame (automap, intermission, ...): manual mode has
             * no automatic erase, so draw one blank list to clear the last
             * 3D frame off the sprite layer, once. */
            /* TWICE (2026-11-16): VDP1 draws a list into the BACK buffer,
             * which becomes visible only at the NEXT frame change. One blank
             * list was drawn but never shown, so the last 3D frame stayed on
             * the sprite layer over the pause menu and the intermission
             * (hardware: menu invisible, level end looked stuck). The second
             * blank's change brings the first one to the front. Each waits
             * for the previous change (pending is cleared by the vblank). */
            if (v1m_blank_shown < 2 && !v1m_pending)
            {
                extern void Saturn_CmdBuf_DeliverBlank(void);
                v1m_begin_write();
                Saturn_CmdBuf_DeliverBlank();
                v1m_blank_shown++;
                v1m_end_write();
            }
        }
        else
#endif
        V1_VRAM(V1_USER0 + 0x00u) = 0x8000u;
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

/* I_FinishUpdate, at the flip to a 2D page (no hardware sky): that page
 * replaces the last 3D frame, so hide the sprite layer now (the vblank
 * callback writes the registers on the same vblank the flip takes effect). */
void Saturn_Vdp1_Show2D(void)
{
    if (v1_hide_pending)
    {
        v1_hide_pending = 0;
        v1_hide_sprites = 1;
    }
}

void Saturn_Vdp1_Enter2D(void)
{
    extern int saturn_game_started;

    saturn_game_started = 0;
    /* (saturn_vdp2_plane_active is no longer cleared here: that switched the
     * RBG0 floor off for good after a pause. 2D pages hide RBG0 through the
     * flip -- saturn_vdp2_rbg0_on, i_video.c -- and every level load resets
     * the plane in Saturn_HW_RenderInit.) */
    saturn_v1w_spr = 0;
    /* Hide the sprite layer when the 2D page is SHOWN (I_FinishUpdate ->
     * Saturn_Vdp1_Show2D), not now: the first pause frame loads and draws
     * TITLEPIC (~0.3 s), and with the sprites hidden at once the old page's
     * NBG0 - the hardware sky panorama in its view rows - showed alone for
     * that long (hardware video 2026-11-16, sky before the pause menu). The
     * last 3D frame stays up until the menu replaces it. */
    v1_hide_pending = 1;
    v1q_count = 0;
    saturn_cmd_m_count = 0;
    saturn_cmd_s_count = 0;
    saturn_cmd_w_count = 0;
#ifdef SATURN_WIN32
    *(uint32_t *)(V1_BASE + V1_USER0) = 0x80000000u;
#else
    V1_VRAM(V1_USER0 + 0x00u) = 0x8000u;
#endif
}

#else  /* !SATURN_VDP1_WALL */

void Saturn_Vdp1Wall_Init(void) { }
void Saturn_Vdp1_Enter2D(void) { }
void Saturn_Vdp1_Show2D(void) { }
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
