/* i_wad.c â€” Saturn's hybrid WAD loader.
 *
 * DOOM's renderer/sound/game logic reads ~6 MB of WAD lumps every frame
 * during play (wall patches, flat bitmaps, sprites, sounds). Streaming
 * those from CD per-frame would cost 100+ ms CD seek per lump and would
 * be unplayable. Solution: copy hot lumps (walls, flats, sprites, palette,
 * colormap, font, GENMIDI) into the 4 MB extended-RAM cart at startup,
 * and stream cold per-level data (THINGS, LINEDEFS, etc.) from CD
 * on first access via a 64 KB scratch buffer.
 *
 * Mechanics:
 *   1. At I_WadBase() time, probe the 4 MB extended-RAM cart and run the
 *      A-bus init sequence. Open the IWAD via one permanent GFS handle
 *      (direct SGL calls â€” see the direct-GFS block below).
 *   2. Read the 12-byte IWAD header from CD into the cart base.
 *   3. Read the lump directory into cart at offset 12.
 *   4. Classify each lump as HOT (cart) or COLD (CD). HOT = PLAYPAL,
 *      COLORMAP, TEXTURE1/2, all patches (P_START..P_END), all flats
 *      (F_START..F_END), all sprites (S_START..S_END), GENMIDI, FONT1/2,
 *      TITLEPIC, status bar graphics, intermission graphics, etc. COLD
 *      = THINGS, LINEDEFS, SIDEDEFS, VERTEXES, SECTORS, BLOCKMAP,
 *      REJECT, NODES, SEGS, SSECTORS, BEHAVIOR, and per-map data.
 *   5. Copy each HOT lump body from CD to cart, recording the cart offset.
 *   6. Rewrite the directory in cart-resident memory: each lump's
 *      filepos field is rewritten to its cart offset for HOT lumps and
 *      to its original CD offset for COLD lumps. The 12-byte header
 *      stays at cart_base + 0; lumpinfo_t[2308] at cart_base + 12.
 *   7. Set wadfile[0].fileptr = cart_base, wadfile[0].cdlength = 1
 *      (forces d32xr's W_GetRawLumpData through the CD chain). Our
 *      I_SeekCDFile/I_ReadCDFile/I_GetCDFileBuffer shims dispatch:
 *        - HOT lump: I_GetCDFileBuffer returns cart_base + pos (no CD).
 *        - COLD lump: I_ReadCDFile reads from CD into 64 KB scratch.
 *
 * d32xr's W_GetRawLumpData always calls I_SeekCDFile/I_ReadCDFile when
 * wad->cdlength > 0, so all our dispatch happens in those three shims.
 * No modifications to w_wad.c needed.
 *
 * The user has D:\SteamLibrary\...\rerelease\doom.wad (12.4 MB, 2308
 * lumps). DOOM1.WAD shareware (~2.4 MB) fits in cart entirely; the
 * rerelease doesn't, so the hybrid handles both transparently.
 */

#include <stddef.h>
#include <string.h>

#include <jo/jo.h>

#include "doomdef.h"
#include "saturn.h"     /* SATURN_CART_REC_OFF (r92) */
#include "i_system.h"
#include "i_wad.h"
#include "saturn_cache.h"
#include "saturn_dsp.h"

/* FRT tick read (SH-2 Free-Running Timer, TCNT @ 0xFFFFFE12, phi/8 â‰ˆ
 * 3.6 MHz) â€” bounds the per-frame background fill so the main loop
 * never stalls inside the queue drain. Same primitive the render
 * prologue's windows use. */
static inline unsigned short Saturn_Wad_ReadFRT(void)
{
    volatile unsigned char *frt = (volatile unsigned char *)0xFFFFFE12u;
    unsigned char h1 = frt[0];
    unsigned char l  = frt[1];
    unsigned char h2 = frt[0];
    if (__builtin_expect(h1 != h2, 0))
        l = frt[1];
    return (unsigned short)(((unsigned int)h2 << 8) | (unsigned int)l);
}
#define SAT_FRT() Saturn_Wad_ReadFRT()

/* Stage fill WITHOUT the 10-vbl hold (i_video.c owns it; the boot
 * ladder's Saturn_StageColor wraps this with the hold). The wedge
 * bracket probe paints through it so the probe never paces the train
 * it is measuring. */
extern void saturn_vdp2_stage_fill(jo_color color);

/* Vblank counter (i_video.c owns it) â€” paces the async batch pump's
 * wedge watchdog. */
extern volatile unsigned int saturn_vbl_count;

/* Per-frame drain budget: ~3.3 ms of FRT ticks, well under one
 * vblank. The check runs BETWEEN 8 KB chunks (see FILL_CHUNK_BYTES),
 * so a frame can overshoot the budget by at most one chunk's CD
 * transfer, never a whole 16 KB entry. */
#define FILL_BUDGET_TICKS  12000u
/* Vblank budget for the per-frame drain (see the gate in
 * saturn_iwad_background_fill): ~2 vbl of CD time per displayed frame
 * plus one chunk overshoot. Replaces the FRT budget above, which was
 * smaller than a single chunk's disc transfer. */
#define FILL_BUDGET_VBLS   2
/* Max bytes of one GFS read inside the drain. GFS_Fread is
 * synchronous â€” the SH-2 spins until the SH-1 finishes the transfer â€”
 * and a 16 KB entry costs 3-10 vbl of pure CD time, which showed up
 * as "fps is poor while the fill bar drains, leaps when it finishes".
 * Chunking keeps the worst-case stall to one chunk (~1-3 vbl) while
 * the per-frame byte cap keeps total throughput the same. */
#define FILL_CHUNK_BYTES   8192u
/* Hard per-frame byte cap for the batch drain (the FRT budget's
 * independent backstop). */
#define FILL_FRAME_BYTES   32768u

/* Async Nw batch (2026-09-02): the drain ISSUES one sector-aligned
 * GFS_NwFread directly into the cart and then PUMPS it with
 * GFS_NwExecOne under an FRT budget â€” across frames â€” instead of
 * spinning to completion inside one frame. Between pumps the CD block
 * keeps streaming into its own sector buffers on the SH-1 (512 KB CD
 * cache â€” the drive reads autonomously once the transfer is issued), so
 * the transfer overlaps the render instead of stalling it. The old
 * fully-synchronous chunk showed up as the steady "G" bar ~3 vbl / fps
 * ~20 while the queue drained; async should drop the per-frame cost
 * toward 0-1 vbl. Batch size starts at 64 KB and HALVES on any failure
 * down to FILL_NW_MIN â€” a size SGL's Nw path refuses costs one watchdog
 * interval, not the whole mechanism. */
#define FILL_NW_MAX        (1u * 2048u)   /* r71: one sector per batch â€”
                                           * batches land in the WRAM-H
                                           * stage, then the DSP engine
                                           * moves them to the cart */
#define FILL_NW_MIN        (1u * 2048u)
static unsigned int fill_nw_batch = FILL_NW_MAX;
/* FRT budget for one pump call (~1.1 ms at phi/8 3.6 MHz). The first
 * pump round of a call always runs (progress guarantee); later rounds
 * stop once the budget is spent, so a call can overshoot by at most one
 * NwExecOne step. */
#define FILL_PUMP_TICKS    4000u
/* Wedge watchdog: if the in-flight batch shows no byte progress for
 * this many vblanks (~0.5 s), the pump fails it (NwStop + error) so the
 * caller shrinks the batch / falls back to the sync path. Replaces the
 * old fixed pump-round cap, which would false-trip on large batches
 * whose NwExecOne polls without progress while the drive catches up. */
#define FILL_WEDGE_VBLS    30u

/* r64: boot-drain sync-only gate. The blocking boot fill used to run
 * its ~2 MB through the async Nw machinery (64 KB GFS_NwFread batches
 * direct into cart, pumped across frames). That machinery has never
 * completed once on real hardware â€” the 09-04 run wedged on a
 * pure-cyan STARTING screen with S=0 (D_DoomLoop never entered), i.e.
 * inside saturn_iwad_fill_drain_all or G_InitNew, where the drain's
 * 60 s watchdog CANNOT preempt a single GFS call that never returns.
 * The sync staged path (1-sector TMODE_CPU GFS_Fread + memcpy) is the
 * only CD mechanism proven on the console â€” it carried the directory
 * load and the PLAYPAL/COLORMAP boot copies. While
 * saturn_fill_boot_mode is set, EVERY chunk â€” sync and multi alike â€”
 * goes through it; the async batches stay enabled for the in-game
 * drain (where the shrink/demote fallbacks can act on a failure
 * instead of a boot hang). */
static int saturn_fill_boot_mode = 0;

/* IMPORTANT endianness note (this cost us a black screen â€” twice):
 *
 * doomdef.h under -DMARS=1 defines __BIG_ENDIAN__, which makes
 * BIGLONG(x) the IDENTITY on Saturn and LITTLELONG(x) the byte swap.
 * d32xr's W_Init / W_GetRawLumpData read the header and every lump's
 * filepos/size through BIGLONG â€” i.e. they expect those values to be
 * ALREADY big-endian in memory (the 32X convention: its WADs were
 * pre-swapped on the medium).
 *
 * The on-disc PC IWAD is standard little-endian, so a raw read is NOT
 * in native order â€” the header fields and every directory entry must
 * be byte-swapped ONCE, right where they are read from CD below
 * (LITTLELONG on the raw value). 2306 lumps otherwise decodes as
 * 0x02090000 and the 12,371,396 directory offset as 0xC4B0BC00, which
 * the geometry guard rejects (after the pread has chased it across
 * half the disc â€” that was the MEDIUM GREEN freeze).
 *
 * Every REWRITE below (filepos = cart offset / cd_off | COLD_BIAS,
 * hdr->infotableofs = 12) is a plain native int store â€” no macros. On
 * the big-endian SH-2 that stores big-endian, so d32xr's BIGLONG
 * (identity) reads them back exactly as written. Do not add swap
 * macros to the rewrites â€” only to the initial disc->memory reads.
 * Lump NAMES are byte strings and are never swapped, and lump BODIES
 * stay raw little-endian on disc/in cart because every d32xr consumer
 * (p_setup.c, r_data.c) decodes them with LITTLESHORT/LITTLELONG.
 *
 * After W_Init sets up wadfile[0], g_game.c re-applies
 * wadfile[0].cdlength = 1 (W_Init's D_memset wipes whatever we set
 * here) to force W_GetRawLumpData through the CD chain, where our
 * I_SeekCDFile/I_ReadCDFile/I_GetCDFileBuffer shims dispatch on the
 * filepos top bit (see COLD_BIAS below).
 *
 * wadinfo_t, wadfile_t, and wadfile[] all come from doomdef.h now
 * (we removed the static copies from w_wad.c). */

/* ====== 4 MB extended-RAM cart constants ====== */
/* All register addresses are plain 32-bit SCU / A-bus values (Sega SCU
 * User's Manual: A-bus set register ASR0 = 0x25FE00B0, A-bus refresh
 * AREF = 0x25FE00B8, ARFEN must stay enabled). The old 0x22xxxxxxx
 * literals were >32-bit constants that only worked because the
 * compiler silently truncated them back to these addresses. */
#define CART_ID_ADDR_16    ((volatile unsigned short *)0x24FFFFFE)
#define CART_ID_ADDR_4M    ((volatile unsigned char *)0x24FFFFFF)
#define CART_ID_BYTE_4M    0x5C
#define CART_ID_BYTE_1M    0x5A
#define CART_INIT_ADDR     ((volatile unsigned short *)0x257EFFFE)
#define A_BUS_SET_REG      ((volatile unsigned long  *)0x25FE00B0)
#define A_BUS_REF_REG      ((volatile unsigned long  *)0x25FE00B8)
#define CART_RAM_BASE      0x42400000UL   /* cache-through mirror of 0x22400000 */
#define CART_RAM_CACHED    0x22400000UL   /* cached mirror (CPU-only data) */
#define CART_RAM_SIZE_4M   (4UL * 1024UL * 1024UL)

/* HOT lump bodies are capped to the first 2 MB of the cart. The Z zone
 * (Z_Malloc arena) lives in the upper 2 MB via Saturn_ZoneBase, so the
 * background fill queue must never advance into it. */
#define CART_HOT_CAP       0x00200000UL
#define CART_ZONE_OFF      0x00200000UL
#define CART_ZONE_SIZE     0x00200000UL

/* COLD lumps get their on-disc offset biased with the top bit so the
 * dispatch shims can tell "cart-resident" from "CD-stream" without
 * relying on numeric-range overlap (a COLD lump at CD offset 0x1000
 * would otherwise look cart-resident). HOT cart offsets are < 2 MB,
 * so the ranges are unambiguous. */
#define COLD_BIAS          0x80000000u

/* Header + directory size: 12 (wadinfo_t) + 2308 * 16 (lumpinfo_t) = 36940. */
#define IWAD_DIR_BYTES     (12 + 2308 * 16)

/* No-cart fallback: when the 4MB extended-RAM cart is missing (e.g. on
 * Yabause/Mednafen emulator, or on real hardware without the cart
 * inserted), we still want DOOM to run. In that mode every lump is
 * COLD â€” we allocate the IWAD header + directory in work-RAM and
 * stream every lump body from CD on demand. Performance is poor
 * (cold reads per-frame) but the game boots. */
#define MAX_WAD_LUMPS_NO_CART  2308

/* ====== Saturn WAD loader globals (declared in i_wad.h) ====== */
int            saturn_wad_open = 0;
unsigned       saturn_wad_size = 0;
int            saturn_cd_seek_pos = 0;

/* The IWAD's GFS handle â€” ONE GFS_Open() at startup, kept for the whole
 * session. See the direct-GFS comment block above saturn_open_iwad() for
 * why the IWAD never goes through Jo's jo_fs_* wrapper. */
static GfsHn   saturn_wad_gfs;

/* One aligned staging sector: every GFS_Fread lands here and is memcpy'd
 * to the caller's destination, so destinations never need DMA alignment
 * (cart A-bus mirror, odd lump offsets, BSS scratch all work).
 *
 * v5.13 STAGING POLICY (2026-09-05): the stages are CACHED. The uncached
 * mirror (+0x20000000) was a DSP-engine visibility workaround — the
 * DSP's DMA reads raw WRAM-H and cannot see the SH-2's operand cache.
 * That engine is DEMOTED (saturn_dsp.c forces saturn_dsp_cart_ok = 0;
 * the demo1 v5.11/12 real-HW duel: CPU memcpy from CACHED WRAM 740ms/2MB
 * vs cart-src CPU 1777ms vs DSP 41s), so every chunk now moves
 * stage->cart by plain SH-2 memcpy — and a cached stage reads ~2.4x
 * faster than the uncached mirror. Coherency: sync GFS runs
 * GFS_TMODE_CPU (an SH-2 CPU copy — its stores go through the same
 * cache the copy loops read), and the runtime staging tripwire
 * (saturn_stage_probe, below) double-checks every landed batch: cached
 * word 0 vs the uncached view. A mismatch means SGL DMA'd behind the
 * cache — demote to the uncached mirrors permanently and count the
 * event in saturn_stage_trip (HUD row W). RE-ARMING THE DSP: the DSP
 * call sites below always hand it the UNCACHED mirror (raw WRAM-H),
 * but with a cached stage the DSP would read whatever RAM holds — only
 * trustworthy if the cache is write-through-clean at that moment.
 * Re-arm only with the staging tripwire green and re-verified on real
 * HW; saturn_oc_purge() is a permanent no-op (any CCR write wedges
 * real HW) and cannot fix stale lines. */
static unsigned char saturn_sector_buf[2048] __attribute__((aligned(16)));

/* 2 KB stage for the async Nw batch. GFS_NwFread may DMA (or CPU-copy
 * under TMODE_CPU) into WRAM-H — allowed — but NEVER into the cart:
 * "Writing to A-Bus by SCU-DMA cannot be used" (SCU manual 2.1), the
 * r63-69 boot wedge. Landed batches move to the cart by CPU memcpy
 * (the DSP engine call remains as the one-line re-arm path). */
static unsigned char saturn_fill_stage[2048] __attribute__((aligned(16)));

/* Staging trust state. 1 = cached views (fast path); 0 = a tripwire
 * event demoted staging to the uncached mirrors permanently (the
 * pre-v5.13 behavior). saturn_stage_trip is the HUD-visible event
 * count (d_main.c row W; healthy = 0). */
static int saturn_stage_cached = 1;
volatile unsigned int saturn_stage_trip = 0;

/* Uncached mirror macros — handed to the DSP engine (raw WRAM-H is
 * what its DMA reads) and used as the tripwire's truth side. */
#define SATURN_SECT_UNC \
    ((volatile unsigned char *)(0x20000000u | (unsigned int)saturn_sector_buf))
#define SATURN_STAGE_UNC \
    ((volatile unsigned char *)(0x20000000u | (unsigned int)saturn_fill_stage))

/* GFS destination for the next sector / Nw batch: always use direct cached WRAM. */
static __inline__ void *saturn_sect_dest(void)
{
    return (void *)saturn_sector_buf;
}
static __inline__ void *saturn_stage_dest(void)
{
    return (void *)saturn_fill_stage;
}

/* Read view of the sectors stage: the mirror the copy loops trust. */
static __inline__ const volatile unsigned char *saturn_sect_view(void)
{
    return (const volatile unsigned char *)saturn_sector_buf;
}
static __inline__ const volatile unsigned char *saturn_stage_view(void)
{
    return (const volatile unsigned char *)saturn_fill_stage;
}

/* The staging tripwire: demotion disabled to prevent passing uncached
 * 0x26xxxxxx addresses to SGL GFS_Fread. */
static int saturn_stage_probe(unsigned int buf)
{
    (void)buf;
    return 1;
}
static void saturn_stage_trip_now(void)
{
    saturn_stage_cached = 1;
    saturn_stage_trip++;
}

/* File id the IWAD handle was opened with â€” needed by the sequential-skip
 * fallback, which may re-open the file to rewind to position 0. */
static Sint32 saturn_wad_fid = -1;

/* Sector count of the opened IWAD, straight from GFS_GetFileSize. */
static unsigned int saturn_wad_nsct = 0;

/* cart-base pointer for I_GetCDFileBuffer dispatch. NULL when no
 * cart is present â€” in that mode every lump is COLD and the I_*CD*
 * shims always stream from CD into saturn_cd_scratch. */
static unsigned char *cart_base_ptr = NULL;
static unsigned int   cart_used_bytes = 0;   /* bytes of cart used by hot data (0 if no cart) */
static unsigned int   cart_total_bytes = 0;  /* size of cart in bytes (4 MB) */
static unsigned int   cart_real_bytes  = 0;  /* r69: pre-clamp size â€” the
                                              * Z-zone (2 MB..4 MB) sits
                                              * ABOVE the fill region and
                                              * must not shrink with the
                                              * fill clamp */
static int            cart_initialized = 0;

/* Boot-probe results, reported on the boot screen by main.c. */
unsigned char  saturn_cart_id_seen = 0;
int            saturn_cart_rw_ok   = 0;
unsigned int   saturn_cart_size    = 0;
int            saturn_cart_rw_point = 0;   /* 0 = passed; failure codes 1-9, see Saturn_CartInit */
unsigned int   saturn_cart_base_addr = 0;  /* mirror the cart actually answers on (0x22400000 or 0x42400000) */
int            saturn_cart_probe_failed = 0; /* cart seen on the bus but not writable â€” CD-stream mode */

/* Heap-allocated IWAD header + directory used when cart is absent.
 * Same layout (12-byte wadinfo_t followed by numlumps * 16 lumpinfo_t)
 * as the cart-resident version, so the dispatch shims work
 * uniformly â€” the only difference is the buffer location. */
static unsigned char *saturn_iwad_heap = NULL;

/* Pre-read position: the saturn_cd_seek_pos value BEFORE the most
 * recent I_ReadCDFile advance. We need this for I_GetCDFileBuffer so
 * that HOT lumps return cart_base + (start_of_last_read), not the
 * post-advance position. */
static int            saturn_cd_pre_read_pos = 0;

/* The 64 KB scratch buffer used by the CD streaming path. Returned by
 * I_GetCDFileBuffer when the requested lump is not in cart. */
static byte saturn_cd_scratch[64 * 1024];

/* Background cart-fill queue. Each pending entry is (cd_offset,
 * cart_offset, lump_size, lump_index). The VBlank callback drains
 * one entry per tick. Without this, I_WadBase had to copy ~600 HOT
 * lumps synchronously at boot (~3 seconds of GFS round-trips), which
 * exceeded the emulator's "no VBlank progress" patience. */
typedef struct {
    unsigned int  cd_off;
    unsigned int  cart_off;
    unsigned int  size;
    int           lump_index;   /* directory index | FILL_SYNCED */
} saturn_fill_t;
/* lump_index flag: this entry was copy-completed synchronously at boot,
 * so the drain loop just skips it. Kept in the high bits of
 * lump_index â€” a separate `synced` field would grow the 2048-entry
 * queue by 4 KB of BSS we don't have (user RAM ends at 0x060C0000). */
#define FILL_SYNCED      0x40000000
#define FILL_LUMP_INDEX(i) ((i) & ~FILL_SYNCED)
/* Background prewarm cap per lump. Was 16 KB when the drain copied one
 * FULL lump per frame synchronously (a 100 KB entry cost 20-60+ vbl
 * EVERY frame â€” the "very low fps" without pauses). Since the async Nw
 * drain (FILL_NW_BYTES below) streams batches across frames at ~zero
 * per-frame cost, big lumps no longer need demotion: prewarming a fat
 * patch/sound removes its one-time on-demand cold-read stall instead.
 * The cart-capacity check in the arm loop is the real limiter â€” it
 * demotes anything that no longer fits under CART_HOT_CAP. */
#define FILL_PREWARM_MAX  (128u * 1024u)
/* r68: was 2048 — halved to 16KB to buy back BSS headroom (the r68
 * build overran 0x060C0000 by 640B). The arm loop's queue-full guard
 * demotes surplus lumps to COLD (CD streaming), which is correct, only
 * slower on their first access. 768 entries accommodates all ~722 hot lumps
 * under SATURN_CART_REC_OFF while saving 4KB of BSS. */
#define SATURN_FILL_QUEUE_MAX  768
static saturn_fill_t saturn_fill_queue[SATURN_FILL_QUEUE_MAX];
static int           saturn_fill_head = 0;
static int           saturn_fill_count = 0;
static int           saturn_fill_active = 0;

/* Partial-copy cursor for the head fill entry: the drain reads at most
 * FILL_CHUNK_BYTES per GFS call, so an entry can span several frames.
 * While an entry is mid-copy, these hold where the next chunk resumes
 * (cd/cart offsets + bytes left); saturn_iwad_pread's position cache
 * makes the continuation seek-free. */
static unsigned int  fill_part_cd, fill_part_cart, fill_part_left;

/* Async Nw transaction in flight (see FILL_NW_MAX): 1 = a batch was
 * issued with GFS_NwFread and is still being pumped. fill_txn_bytes is
 * the batch size (whole sectors); fill_txn_last/_progress_vbl feed the
 * no-progress wedge watchdog. On completion the pump advances
 * saturn_gfs_pos and the fill cursor â€” the same cursor state the old
 * sync path updated per chunk. */
static int            fill_txn;        /* 1 = NwFread issued, not complete */
static unsigned int   fill_txn_bytes;  /* bytes the pending batch moves  */
static unsigned int   fill_txn_last;   /* highest nbyte seen this batch  */
static unsigned int   fill_txn_prog_vbl; /* vbl of last progress          */

/* Round-33b: TRUE while an async GFS_NwFread batch is mid-flight on
 * the CD block. The red-screen watchdog must not issue BUP_Write
 * (also a CD-block SH-1 operation) while this is set â€” that is the
 * 2026-09-04 07:52 Kronos crash (stripes, dead window, ~5 min into
 * the demo); the 07:01 red screen with an idle CD saved fine. */
/* Forward decl so Saturn_CdFillStallAbort (below) can call it; the
 * abort body sits next to the drain (saturn_iwad_background_fill). */
static void fill_txn_abort(void);

int Saturn_CdFillBusy(void)
{
    return fill_txn;
}

/* Round-36: stop an in-flight async batch so a CD-block SH-1
 * operation (BUP_Write from the red-screen recorder) is safe to issue.
 * The old fire-path gate SKIPPED the save whenever a batch was
 * pending â€” mid-demo that is always, so stall records never landed.
 * Aborting first (the same recovery pread's entry hook uses) makes
 * the CD block idle and the save legal. Safe when nothing is pending
 * (fill_txn_abort no-ops). */
void Saturn_CdFillStallAbort(void)
{
    fill_txn_abort();
}

/* defined next to the drain (saturn_iwad_background_fill); pread's
 * entry hook cancels a pending batch before repositioning the handle */
static int  fill_txn_pump(unsigned int budget_ticks);

/* Fill-queue progress, read by the HUD strip in i_video.c: total is
 * latched once when I_WadBase finishes arming the queue; remaining is
 * refreshed by the per-frame drain (count - head). */
volatile int saturn_fill_total     = 0;
volatile int saturn_fill_remaining = 0;

/* Forward decl so I_WadBase can enqueue without circular include. */
static lumpinfo_t *saturn_wad_dir_ptr(void);

/* ====== Saturn-side helpers ====== */

/* Write/verify one longword at a cart address, restoring the original
 * value afterwards. All accesses go through the cache-through mirror so
 * a stale SH-2 OC line can't fake the result. */
static int probe_cart_long(volatile unsigned long *addr, unsigned long pattern)
{
    unsigned long saved = *addr;
    unsigned long rb;
    *addr = pattern;
    rb = *addr;
    *addr = saved;
    return (rb == pattern);
}

/* 16-bit variant â€” the user's hardware-proven 4 MB tester validates the
 * cart exclusively with 16-bit accesses through the uncached mirror. */
static int probe_cart_short(volatile unsigned short *addr, unsigned short pattern)
{
    unsigned short saved = *addr;
    unsigned short rb;
    *addr = pattern;
    rb = *addr;
    *addr = saved;
    return (rb == pattern);
}

/* Three-point 16-bit write/verify at one mirror base. Returns 0 on
 * success, or 1/2/3 for a failed probe at start / middle / last word. */
static int probe_cart_mirror(unsigned long base, unsigned int size)
{
    if (!probe_cart_short((volatile unsigned short *)(base + 0u), 0x5555u))
        return 1;
    if (!probe_cart_short((volatile unsigned short *)(base + (size >> 1)), 0xAAAAu))
        return 2;
    if (!probe_cart_short((volatile unsigned short *)(base + size - 2u), 0x5555u))
        return 3;
    return 0;
}

/* Probe + init the extended-RAM cart. Returns 1 if a cart is present
 * and initialized, 0 otherwise. Public and idempotent â€” main.c calls
 * it for the boot report, then I_WadBase relies on the cached result.
 *
 * The strict probe would read `*0x24FFFFFF` and expect 0x5C (4 MB) or
 * 0x5A (1 MB). Some emulators with non-stock BIOSes may report a
 * different probe byte or return 0xFF for "no cart mapped." To stay
 * compatible, we accept any non-zero byte that isn't an obvious "no
 * hardware" sentinel (0xFF) and assume a 4 MB cart in that case. A
 * false positive would crash on the first cart write, which surfaces
 * immediately instead of hiding as a black screen; the no-cart
 * fallback in I_WadBase handles the real absence cleanly.
 *
 * The three-point write/verify (start/middle/last word) mirrors
 * ram_test.c. Its result is recorded in saturn_cart_rw_ok but does
 * NOT fail the init â€” on tolerant emulators a verify hiccup at one
 * probe point shouldn't demote the whole cart. */
int Saturn_CartInit(void)
{
    unsigned char cart_id;
    unsigned int  size;

    if (cart_initialized)
        return 1;

    /* A-Bus 16-bit alignment: read 16-bit word at even boundary 0x24FFFFFE and mask low byte */
    cart_id = (unsigned char)(*CART_ID_ADDR_16 & 0x00FF);
    saturn_cart_id_seen = cart_id;
    /* PINE (4,12,4): the CS2 ID byte READ RETURNED. With a Saroo in the
     * slot this read answers 0x00/0xFF (no RAM cart behind the flash) or
     * the flashcart's own ID â€” either way we get here. Held on PINE =
     * the ID read itself never returned (A-bus CS2 access hang). */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(4, 12, 4));
    if (cart_id == 0x00 || cart_id == 0xFF)
        return 0;

    /* KNOWN-DRAM-IDS-ONLY (2026-09-03 real-HW bring-up): the Saroo
     * answers the CS2 ID read with 0x06 â€” a flashcart-specific value.
     * The old "assume 4 MB on any other id" path accepted it, set
     * cart_initialized=1 and pointed the WAD loader's HOT-lump reads at
     * the Saroo's flash window: garbage data, and cart writes into
     * someone else's address space. Only Sega DRAM cart IDs may be
     * driven â€” everything else is treated as NO CART (all-CD mode). */
    if (cart_id != CART_ID_BYTE_4M && cart_id != CART_ID_BYTE_1M &&
        cart_id != 0x1A && cart_id != 0xDA)
        return 0;

    /* ALL init writes go ONLY with the 1 MB DRAM cart. The user's
     * hardware-proven 4 MB tester (2026-08-30, "Ultra Stable Native
     * 16-Bit Dual Cartridge Tester") writes NOTHING for id 0x5C â€” no
     * CART_INIT, no A-bus set/refresh â€” "Do NOT modify A_BUS registers
     * to keep SAROO flashcarts functional", and the CART_INIT write is
     * inside its 1 MB branch too. Writing CART_INIT (0x257EFFFE=1)
     * unconditionally coincided with CART-4MB-RW-FAIL-1 on real
     * hardware; this now mirrors the proven sequence exactly. */
    if (cart_id == CART_ID_BYTE_1M || cart_id == 0x1A || cart_id == 0xDA)
    {
        *CART_INIT_ADDR = 0x0001;
        {
            volatile int d;
            for (d = 0; d < 0x200; d++) { __asm__("nop"); }
        }
        *A_BUS_SET_REG = 0x23301FF0UL;
        *A_BUS_REF_REG = 0x00000013UL;
        {
            volatile int d;
            for (d = 0; d < 0x200; d++) { __asm__("nop"); }
        }
    }

    if (cart_id == CART_ID_BYTE_1M || cart_id == 0x1A || cart_id == 0xDA)
        size = 1UL * 1024UL * 1024UL;
    else
        size = (unsigned)CART_RAM_SIZE_4M;   /* assume 4 MB on any other id */

    cart_total_bytes = size;
    saturn_cart_size = size;
    cart_real_bytes  = size;   /* r69: pre-clamp truth â€” ZoneBase only  */
    cart_initialized = 1;

    /* Decide WHICH virtual mirror the cart answers on. The user's
     * hardware-proven 4 MB tester stresses 0x22400000 â€” its
     * "(EXT_RAM_BASE | 0x20000000)" uncached pointer is a NO-OP because
     * 0x22400000 already has bit 29 set, so the whole stress test ran on
     * the plain cached CS1 address â€” while this port's 0x42400000 area-2
     * mirror failed write/verify on the same hardware (every probe at
     * offset 0, 2026-08-30) even though the CS2 ID read at 0x24FFFFFF
     * works. Probe the CS1 cached address FIRST; only fall back to
     * 0x42400000 if it doesn't answer. Every later cart access (hot
     * lump copies, W_GetRawLumpData dispatch) goes through
     * cart_base_ptr, so both paths stay uniform. */
    saturn_cart_rw_point = 0;
    /* SMOKE (12,12,12): entering the mirror write-probes (16-bit
     * write/verify at 0x22400000, then 0x42400000). Held on SMOKE = the
     * first cart WRITE hung or the bus locked up â€” on real hardware this
     * is where a flashcart that dislikes writes shows itself. */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(12, 12, 12));
    {
        int p_cached  = probe_cart_mirror(CART_RAM_CACHED, size);
        int p_mirror  = (p_cached == 0) ? 0 : probe_cart_mirror(CART_RAM_BASE, size);

        if (p_cached == 0)
        {
            cart_base_ptr          = (unsigned char *)CART_RAM_CACHED;
            saturn_cart_base_addr  = (unsigned int)CART_RAM_CACHED;
        }
        else if (p_mirror == 0)
        {
            cart_base_ptr          = (unsigned char *)CART_RAM_BASE;
            saturn_cart_base_addr  = (unsigned int)CART_RAM_BASE;
        }
        else
        {
            /* Neither mirror answers 16-bit writes. Point codes: 1/2/3 =
             * 0x22400000 failed at start/middle/end, 4/5/6 = 0x42400000
             * failed likewise. Demote to CD-stream mode â€” I_WadBase
             * dispatches on cart_initialized, so leaving it set with a
             * NULL cart_base_ptr would write through NULL. */
            cart_base_ptr           = NULL;
            saturn_cart_base_addr   = 0;
            saturn_cart_rw_point    = (p_mirror == p_cached) ? p_cached : (3 + p_mirror);
            saturn_cart_rw_ok       = 0;
            saturn_cart_probe_failed = 1;
            cart_initialized        = 0;
            return 0;
        }
    }

    /* 32-bit longword write/verify on the CHOSEN mirror â€” memcpy into
     * cart moves longwords, so this width must work too. Point codes
     * 7/8/9 = failed at start/middle/end of the chosen base. */
    if (!probe_cart_long((volatile unsigned long *)(saturn_cart_base_addr + 0u), 0xDEADBEEFUL))
    { saturn_cart_rw_point = 7; saturn_cart_rw_ok = 0; return 1; }
    if (!probe_cart_long((volatile unsigned long *)(saturn_cart_base_addr + (size >> 1)), 0xCAFEBABEUL))
    { saturn_cart_rw_point = 8; saturn_cart_rw_ok = 0; return 1; }
    if (!probe_cart_long((volatile unsigned long *)(saturn_cart_base_addr + size - 4u), 0x12345678UL))
    { saturn_cart_rw_point = 9; saturn_cart_rw_ok = 0; return 1; }
    saturn_cart_rw_ok = 1;

    /* r68 SIZE MARCH: the probes above are three one-off writes, but the
     * boot fill hammers sustained 2 KB write bursts for a minute â€” and
     * both r66/r67 real-HW freeze photos died with the fill cursor at
     * ~1 MB (the 1 MB cart line). March 16-bit write/verify probes up
     * the cart, painting an on-screen breadcrumb BEFORE each pair (a
     * bus hang freezes the machine with the step number visible), and
     * CLAMP the cart size to the highest verified point so the fill can
     * never write past it â€” the hot-lump cap clamps off cart_total_bytes
     * and the WAD loader dispatches on saturn_cart_size, so one clamp
     * covers everything. Point codes 10-13 = march failed at 512K/1M/
     * 2M/4M respectively. A "C=" breadcrumb at y=208 survives on screen
     * as the record of the verified size. */
    {
        static const unsigned int steps[4] = {
            512u * 1024u, 1024u * 1024u, 2048u * 1024u, 4096u * 1024u
        };
        char         bc[8];   /* stack local â€” BSS budget is tight  */
        int          si;

        extern void Saturn_Udec0(char *, unsigned int, int wide);
        extern void Saturn_StageText(int x, int y, const char *s);

        bc[0] = 'C';
        bc[1] = '=';
        for (si = 0; si < 4; si++)
        {
            if (steps[si] >= size)
                break;   /* at/above the assumed size â€” already probed */
            Saturn_Udec0(bc + 2, steps[si] >> 10, 5);
            Saturn_StageText(8, 208, bc);
            if (!probe_cart_short((volatile unsigned short *)
                                  (saturn_cart_base_addr + steps[si] - 2u),
                                  0x5A5Au))
            {
                cart_total_bytes         = steps[si];  /* verified extent */
                saturn_cart_size         = steps[si];
                saturn_cart_rw_point     = 10 + si;
                saturn_cart_rw_ok        = 0;
                break;
            }
        }
        /* r69 HARD CLAMP: r63-r67 froze at F~1000-1100 KB and the r68
         * march saw the 1 MB point fail outright. Every cart write is
         * held below 1008 KB â€” 16 KB of safety margin under the 1 MB
         * line. The prewarm cap (hot_cap) follows cart_total_bytes, so
         * surplus hot lumps demote to COLD/CD at arm time. Paint the
         * clamp as the final breadcrumb â€” "C=01008" on screen = the
         * clamp ran. */
        if (cart_total_bytes > 1008u * 1024u)
        {
            char bc2[8];
            cart_total_bytes = 1008u * 1024u;
            saturn_cart_size = cart_total_bytes;
            bc2[0] = 'C'; bc2[1] = '=';
            Saturn_Udec0(bc2 + 2, cart_total_bytes >> 10, 5);
            Saturn_StageText(8, 208, bc2);
        }
    }
    return 1;
}

/* For the boot screen: 1 if the cart init succeeded. */
int Saturn_CartPresent(void)
{
    return cart_initialized;
}

/* Z-zone base: the cart's upper half via the CACHED mirror (the zone
 * is CPU-only memory â€” Z_Malloc headers, mobjthinker lists â€” so cached
 * access is correct and fast). Returns NULL when no cart is present;
 * the caller falls back to its static pool. Only available on the 4 MB
 * cart â€” on a 1 MB cart there is no room above the hot-lump cap. */
byte *Saturn_ZoneBase(int *size)
{
    /* r69: guard on cart_real_bytes (pre-clamp) â€” the clamp holds the
     * FILL region below the ~1 MB real-HW wedge line, but the Z-zone
     * lives at 2 MB..4 MB, far above it, and worked in every r63-r67
     * boot. The 12 KB z_zone_pool fallback is NOT survivable. */
    if (!cart_initialized || cart_real_bytes < (CART_HOT_CAP + CART_ZONE_SIZE))
        return NULL;
    if (size)
        *size = (int)CART_ZONE_SIZE;
    return (byte *)(CART_RAM_CACHED + CART_ZONE_OFF);
}

/* Compare 8-character big-endian DOOM lump name (case-insensitive).
 * tag may be shorter than 8 chars; pad with zeros. */
static int lump_name_eq(const char *name, const char *tag)
{
    int  i;
    for (i = 0; i < 8; i++)
    {
        unsigned char c = (unsigned char)name[i];
        unsigned char t = (unsigned char)tag[i];
        if (t == 0)
        {
            if (c == 0) return 1;
            return 0;
        }
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (t >= 'a' && t <= 'z') t -= 'a' - 'A';
        if (c != t) return 0;
    }
    return 1;
}

/* Map-marker lump names: E#M# (this IWAD uses E1M1 style) or MAP##. */
static int lump_name_is_map(const char *name)
{
    if (name[0] == 'E' && name[1] >= '1' && name[1] <= '9' &&
        name[2] == 'M' && name[3] >= '1' && name[3] <= '9' &&
        name[4] == 0)
        return 1;
    if (name[0] == 'M' && name[1] == 'A' && name[2] == 'P' &&
        name[3] >= '0' && name[3] <= '9' &&
        name[4] >= '0' && name[4] <= '9' &&
        name[5] == 0)
        return 1;
    return 0;
}

/* Lumps we keep in cart. Anything not listed here (and not in
 * P/F/S_START..END ranges) is COLD and goes through the CD stream. */
static int lump_is_named_hot(const char *name)
{
    /* Single-lump resources read on every frame. */
    if (lump_name_eq(name, "PLAYPAL"))  return 1;
    if (lump_name_eq(name, "COLORMAP")) return 1;
    if (lump_name_eq(name, "TEXTURE1")) return 1;
    if (lump_name_eq(name, "TEXTURE2")) return 1;
    if (lump_name_eq(name, "GENMIDI"))  return 1;
    if (lump_name_eq(name, "FONT1"))    return 1;
    if (lump_name_eq(name, "FONT2"))    return 1;
    /* Title-screen / menu graphics. */
    if (lump_name_eq(name, "TITLEPIC")) return 1;
    if (lump_name_eq(name, "CREDIT"))   return 1;
    if (lump_name_eq(name, "HELP1"))    return 1;
    if (lump_name_eq(name, "HELP2"))    return 1;
    if (lump_name_eq(name, "VICTORY2")) return 1;
    if (lump_name_eq(name, "PFUB1"))    return 1;
    if (lump_name_eq(name, "PFUB2"))    return 1;
    if (lump_name_eq(name, "END0"))     return 1;
    if (lump_name_eq(name, "END1"))     return 1;
    if (lump_name_eq(name, "END2"))     return 1;
    if (lump_name_eq(name, "END3"))     return 1;
    if (lump_name_eq(name, "END4"))     return 1;
    if (lump_name_eq(name, "END5"))     return 1;
    if (lump_name_eq(name, "END6"))     return 1;
    /* Intermission / finale graphics. */
    if (lump_name_eq(name, "INTERPIC")) return 1;
    if (lump_name_eq(name, "FINALE1"))  return 1;
    if (lump_name_eq(name, "FINALE2"))  return 1;
    if (lump_name_eq(name, "FINALE3"))  return 1;
    /* Status bar / HUD. */
    if (lump_name_eq(name, "STBAR"))    return 1;
    if (lump_name_eq(name, "STARMS"))   return 1;
    if (lump_name_eq(name, "STCFN045")) return 1;
    /* Blood splats. */
    if (lump_name_eq(name, "BLOOD1"))   return 1;
    if (lump_name_eq(name, "BLOOD2"))   return 1;
    if (lump_name_eq(name, "BLOOD3"))   return 1;
    return 0;
}

/* ====== Direct-GFS IWAD I/O ======
 *
 * Jo's fs wrapper is NOT used for the IWAD. Under JO_DEBUG (on in this
 * project's makefile) every fs.c failure path raises Jo's blue-screen
 * error handler and then KEEPS RUNNING â€” jo_fs_seek_forward retried
 * GFS_Seek 10x, painted that screen mid-WAD-load ("LOADING WAD" stuck
 * with blue/cyan text), and returned false with its file->read
 * bookkeeping already advanced, leaving the handle inconsistent. The
 * IWAD path therefore talks to SGL's GFS directly:
 *   - ONE handle, GFS_Open()ed once at startup and never closed (no
 *     per-read close/open churn, no jo_malloc/jo_free per read);
 *   - every random access is ONE relative GFS_Seek(GFS_SEEK_CUR) with
 *     the forward sector delta (Jo's own fs.c seek_forward pattern â€”
 *     absolute GFS_SEEK_SET has refused every far seek on this setup),
 *     followed by strictly sequential GFS_Fread()s; a walk-with-staged-
 *     reads fallback covers a refused relative seek;
 *   - every sector is staged through saturn_sector_buf and memcpy'd out;
 *   - failures return -1 up to DOOM's own I_Error (red flashing screen
 *     with the lump name), never Jo's blue handler. */

/* Fill saturn_sector_buf with the next sector from the IWAD. Bounded
 * retry with a settle spin; -1 on refusal (caller reports via I_Error).
 *
 * v5.13: the r71 OC purge after a successful GFS_Fread is GONE â€”
 * saturn_oc_purge() is a permanent no-op (any CCR write wedges real
 * HW), and with GFS_TMODE_CPU the transfer is an SH-2 CPU copy whose
 * stores keep the cached stage coherent by construction. The purge's
 * old job ("in case this SGL build DMAs") is now the runtime staging
 * tripwire right after each successful read: a cached-vs-uncached
 * mismatch on word 0 demotes the stage to the uncached mirrors
 * permanently (saturn_stage_trip_now). */
/* Read-path diagnostics, surfaced through I_Error so a short read names
 * the failing primitive. stage: 0 = not started, 1 = in the seek retry
 * loop, 2 = in the Fread retry loop. -99 = never attempted. */
static int saturn_io_diag_stage = 0;
static int saturn_io_diag_seek  = -99;   /* last GFS_Seek return value */
static int saturn_io_diag_fread = -99;   /* last GFS_Fread return value */
static int saturn_io_diag_seeks = 0;    /* GFS_Seek attempts so far   */
int saturn_io_diag_freads = 0;   /* GFS_Fread attempts so far
                                  * (r66: non-static â€” the ISR's
                                  * pre-loop R row reads it)        */
static int saturn_io_diag_oks    = 0;    /* sectors staged in this pread (incl. skips) */
static int saturn_io_diag_mode   = 0;    /* 0=seek path 1=walk on current handle
                                          * 2=walk after re-open 3=copy phase */

/* r64: GFS call-site stamp. Nonzero while the MAIN thread is inside
 * one blocking SGL GFS primitive; the vblank ISR's stall reporter
 * (i_video.c) paints P=<site> once the same site has been in flight
 * for >600 vbl. This exists because the 09-04 real-HW freeze was a
 * silent pure-cyan STARTING screen (S=0 â€” D_DoomLoop never entered),
 * i.e. a wedge inside the boot fill or G_InitNew where the drain's
 * 60 s watchdog CANNOT preempt a single GFS call that never returns.
 * Every CD read in the game funnels through the sites below:
 *   1 = 1-sector sync GFS_Fread (saturn_gfs_read_sector)
 *   2 = multi-sector GFS_NwFread issue
 *   3 = Nw pump (GFS_NwExecOne/GFS_NwGetStat)
 *   4 = relative GFS_Seek (pread positioning)
 *   5 = GFS_NwStop (abort/shrink)
 *   6 = GFS_Close/GFS_Open (handle re-open)
 *   7 = GFS_NameToId/GFS_Open (IWAD open)
 * r65: 8 = GFS_SetTmode/ReadPara/TransPara (saturn_gfs_setup),
 *   9 = GFS_GetFileSize, 10 = the 2 KB cart memcpy in
 *   saturn_copy_from_here (the non-GFS bulk-cart-write suspect â€”
 *   r63's DMA fill and r64's memcpy fill share only GFS calls and
 *   cart writes, and during a memcpy the site is 0, which is why
 *   the r64 freeze photo showed no P/W row). */
volatile unsigned int saturn_gfs_site = 0;

/* r65: boot-fill progress in KB copied (fill_part_cart >> 10). The
 * vblank ISR paints F=<this> at y=68 while pre-loop â€” the alive-vs-
 * wedged discriminator for the whole drain: climbing = slow, frozen
 * = wedged inside one chunk (then P names the call). */
volatile unsigned int saturn_fill_progress = 0;

/* r66: WHERE the boot-drain time goes. The r65 real-HW photo showed
 * F CLIMBING (garbled) with no P/W row â€” the fill is alive but
 * crawling ~50-100x slower than Kronos, so the question is which
 * primitive is slow: the CD calls (G) or the 2 KB cart writes (C â€”
 * the SAROO RAM cart is the r63-DMA/r64-memcpy common factor).
 * Units: raw vblanks accumulated around each call; the ISR paints
 * G/C >> 4 (16-vbl units) next to R = total staged sector-read
 * calls, so per-call cost = (G*16)/R vbl. */
volatile unsigned int saturn_prof_gfs_vbl = 0;
volatile unsigned int saturn_prof_cpy_vbl = 0;

/* r94: the SCU DSP cart engine is ARMED with the demo1-proven shape.
 * The r71-73 "D=0TT then first-GFS wedge" root cause is now KNOWN and
 * had nothing to do with A-bus arbitration: the r71 program used
 * JMP-T0 spin-waits after every DMA op, and T0 NEVER CLEARS on real
 * silicon (demo1 v5.6 hardware burn 09-05 proved this: the E-control
 * leg parks on T with its data fully landed; wait-free programs all
 * pass 8/8). The r71 CPU-side 4M-iteration hammering PPAF poll then
 * choked the bus hard enough to wedge the next GFS call. The game now
 * runs the exact engine demo1 validated on hardware: wait-free
 * program (no T0 waits), quiet register-only delays, then a
 * space-out E-poll. */
volatile unsigned int saturn_dsp_engine_armed = 1;   /* r95: armed (A/B leg done — Kronos identical with and without) */

/* r102 DSP-eval: FULL readback verify of EVERY DSP-copied 2 KB chunk
 * (the r71 one-time-full + 2-byte-spot policy only proved the FIRST
 * chunk byte-exact). kb counts verified KB (+2 per chunk); bad counts
 * chunks whose readback mismatched (repaired by CPU memcpy, per the
 * r71 policy — the DSP path is NOT demoted so the failure census
 * keeps accumulating). The in-loop census row X paints bad and V
 * paints kb every 64 vbl, so one photo mid-play reads the totals:
 * the standing directive is that the CD->cart transfer must be proven
 * correct, not assumed. */
volatile unsigned int saturn_dsp_verify_kb  = 0;
volatile unsigned int saturn_dsp_verify_bad = 0;

/* r72: total cart bytes written (KB, +2 per staged 2 KB sector â€” both
 * the DSP path and its memcpy repairs/fallbacks). The ISR's pre-loop
 * census paints it (W row) so any real-HW photo shows exactly how far
 * the write stream got before a wedge â€” the number r63-r70 could only
 * be guessed at from F. */
volatile unsigned int saturn_cart_kb = 0;

/* r68: MAIN-THREAD FILL BEAT â€” incremented once per COMPLETED chunk
 * (sync copy or async pump completion) in saturn_iwad_background_fill.
 * The ISR's pre-loop B row paints the delta over the last 64 vbl:
 * climbing = the drain loop is cycling (main thread alive); frozen =
 * wedged inside ONE call (then P/W names the site) or the machine is
 * hard-frozen (then two photos of the row set are identical). This is
 * the alive-vs-wedged discrimination every earlier glyph net lacked. */
volatile unsigned int saturn_fill_beat = 0;

/* r71: the r68/r69 SCU-DMA cart-write path is GONE â€” the SCU manual
 * (2.1 precautions) bans CPU-initiated SCU-DMA writes to the A-bus
 * outright, which is exactly the r68 instant wedge, and the sustained
 * CPU memcpy bursts it fell back to were the r64+ wedge (the A-bus
 * flash-timing rule: no interrupts inside a write sequence). Every
 * cart write now goes through the SCU DSP's own DMA
 * (Saturn_DspCartCopy2K â€” the manual-sanctioned engine) with the CPU
 * memcpy from the stage view as the permanent fallback (the engine is
 * demoted, so the CPU memcpy from the CACHED stage is the live path â€”
 * the v5.11/12 demo1 duel: 740ms/2MB cached vs 1777ms uncached-src). */

/* Our own record of the handle's sector position. Every GFS_Fread in
 * this file is exactly one 2048-byte sector and every positioning move
 * (accepted relative seek or staged read) advances it by a known amount,
 * so this is exact â€” and it avoids betting on GFS_Tell's return units
 * (undocumented in SEGA_GFS.H). */
static unsigned int saturn_gfs_pos = 0;

/* Per-handle read configuration. SGL's defaults are GFS_TMODE_SCU (value
 * 0!) with 1-sector read/trans paras â€” the 1-sector default is the 2 KB
 * per-GFS-call tax, and the SCU-DMA default is fine for WRAM staging
 * (saturn_sector_buf) but is exactly the wrong mechanism for the
 * multi-sector direct reads INTO CART (0x22400000 cacheable A-bus
 * mirror): SCU writes behind the SH-2's OC plus different completion
 * mechanics hung the first directory read solid (MEDIUM GREEN, no SKIP
 * counter). TMODE_CPU makes every GFS_Fread a synchronous CPU copy â€”
 * same mechanism as the memcpy path we know works â€” and ReadPara=8
 * lets the CD-block fetch 8 sectors per command so batched reads cost
 * one SH-1 round-trip per 16 KB. Must be called after EVERY GFS_Open
 * (the walk path re-opens its handle too). */
/* r81: GSUB boot-order flight record. The main thread stamps
 * rec[40..42] (cart top sector; disjoint from 'GSTL' rec[32..36] and
 * 'REC1' rec[0..18]) BEFORE every suspect boot step; a wedge leaves
 * the last stamp readable at the NEXT boot, which paints G=<sub> at
 * (140,88) pre-loop. Works even if the ISR dies inside the wedge â€”
 * the one witness that needs neither a live ISR nor a good photo.
 * CAVEAT: the SAROO boot menu may wipe cart DRAM; an absent G row
 * after a menu pass proves nothing.
 * Sub decode: 1 = cart probed, 2 = DSP init (r77+: skipped),
 * 3 = before saturn_open_iwad, 4 = before GFS_SetTmode,
 * 5 = before GFS_SetReadPara, 6 = before GFS_SetTransPara,
 * 7 = saturn_gfs_setup completed, 8 = staging chunk GFS sector read
 * in flight, 9 = staging chunk masked cart-write leg in flight,
 * 10 = staging chunk completed (a parked verdict with G=10 = the ISR
 * is dead â€” the honest 256-chunk trigger fired), 12/13 = SetReadPara/
 * SetTransPara (site 8 split).
 * r86: 21+ = the level-init path (mirrors saturn_init_site, decode in
 * g_game.c) â€” the real-HW ISR dies ~200 vbl into the G_InitNew wedge,
 * too fast for any verdict, so the NEXT boot's B row is the only
 * trace: 21 = G_InitNew entry, 22 = before P_SetupLevel,
 * 23 = P_SetupLevel entry, 24 = after P_InitThinkers,
 * 25 = after R_ClearTextures, 26 = after W_LoadPWAD, 27 = after
 * W_CheckNumForName, 28 = MAP-FOUND paint done, 29 = P_SetupLevel
 * RETURNED. */
void Saturn_GsubStamp(unsigned int sub)
{
    /* r93: GSUB STAMPS DISABLED — unmasked A-bus writes racing the
     * vblank. Every stamp is 3 cart writes OUTSIDE the CartWriteBegin/
     * End mask; the SCU manual (p03_06) bans interrupts splitting an
     * A-bus write sequence (the convicted r64+ moving-point wedge).
     * Boot stamps 1-13 are rare and survived B1; P_SetupLevel's chunk
     * streaming fires them thousands of times — the B3 lockup fires
     * exactly there. The r93 burn: if B3 parks with stamps gone, the
     * unmasked stamps were the breaker; if it still locks, the masked
     * 2KB bursts into the 2MB+ Z zone are next (r94 moves them). */
    (void)sub;
}

static void saturn_gfs_setup(GfsHn gfs)
{
    /* r65 site 8: these three SGL config calls were unstamped â€” a
     * wedge inside one showed no P/W row at all.
     * r80: site 8 SPLIT â€” 8 = GFS_SetTmode, 12 = GFS_SetReadPara,
     * 13 = GFS_SetTransPara. Four boots of P=8 name the GROUP; the
     * split names the primitive (P frozen at 8/12/13 = that call
     * never returned). r81: GSUB stamps ride along (4/5/6). */
    unsigned int t0 = saturn_vbl_count;
    Saturn_GsubStamp(4);
    saturn_gfs_site = 8;
    GFS_SetTmode(gfs, GFS_TMODE_CPU);
    Saturn_GsubStamp(5);
    saturn_gfs_site = 12;
    GFS_SetReadPara(gfs, 8);
    Saturn_GsubStamp(6);
    saturn_gfs_site = 13;
    GFS_SetTransPara(gfs, 8);
    Saturn_GsubStamp(7);
    saturn_prof_gfs_vbl += saturn_vbl_count - t0;   /* r66 */
    saturn_gfs_site = 0;
}

static int saturn_gfs_read_sector(void)
{
    int retry;
    for (retry = 0; retry < 8; retry++)
    {
        unsigned int t0;
        saturn_io_diag_stage = 2;
        t0 = saturn_vbl_count;                          /* r66 */
        saturn_gfs_site = 1;
        saturn_io_diag_fread = GFS_Fread(saturn_wad_gfs, 1,
                saturn_sect_dest(), 2048);
        saturn_prof_gfs_vbl += saturn_vbl_count - t0;   /* r66 */
        saturn_gfs_site = 0;
        saturn_io_diag_freads++;
        if (saturn_io_diag_fread > 0)
        {
            /* v5.13 staging tripwire (replaces the r71 purge â€”
             * saturn_oc_purge is a permanent no-op): GFS_TMODE_CPU
             * makes this an SH-2 CPU copy, so the cached stage should
             * already be coherent; a cached-vs-uncached mismatch on
             * word 0 means SGL DMA'd behind the cache â€” demote to the
             * uncached mirrors permanently. */
            if (!saturn_stage_probe((unsigned int)saturn_sector_buf))
                saturn_stage_trip_now();
            saturn_io_diag_oks++;
            saturn_gfs_pos++;
            return 0;
        }
        { volatile int t; for (t = 0; t < 10000; t++) { } } /* settle */
    }
    return -1;
}

/* r71: saturn_gfs_fread_multi (multi-sector NwFread INTO the cart, the
 * G_InitNew-pread write-burst path) is deleted â€” under the SCU manual
 * no mechanism may write the A-bus except the DSP's own DMA, and
 * multi-sector reads now belong to the WRAM-H staged batch in
 * fill_txn_issue + the DSP engine. saturn_direct_reads_ok survives as
 * the async-Nw enable for the fill machinery (fill_txn_issue lands
 * every batch in the WRAM stage, never the cart). */
static int saturn_direct_reads_ok = 1;   /* async Nw batch path enabled */

/* Walk forward to `sector` with staged sector reads (data discarded).
 * If `reopen` is set, first re-open the file (position 0) â€” used when
 * the current position is past the target or the handle is suspected
 * wedged. Returns 0 / -1. */
static int saturn_walk_to_sector(unsigned int sector, int reopen)
{
    saturn_io_diag_mode = reopen ? 2 : 1;
    if (reopen)
    {
        saturn_gfs_site = 6;
        GFS_Close(saturn_wad_gfs);
        saturn_wad_gfs = GFS_Open(saturn_wad_fid);
        saturn_gfs_site = 0;
        if (saturn_wad_gfs == JO_NULL)
            return -1;
        saturn_gfs_setup(saturn_wad_gfs);
        saturn_gfs_pos = 0;
    }
    if (saturn_gfs_pos > sector)
        return -1;   /* walking can only move forward */
    while (saturn_gfs_pos < sector)
    {
        /* Live progress on the stage-color background: a climbing
         * SKIP counter = the walk is working (just slow); a frozen
         * counter = GFS_Fread itself hung. Redraw LOADING WAD too â€”
         * every pump flips the VDP1 table, so text drawn once (by
         * d_main) vanishes on the next frame. */
        if ((saturn_gfs_pos & 0xFFu) == 0)
        {
            /* Round-33d: hand-built digits â€” Saturn_Format's varargs
             * call is broken on target (garbled/no digits and it
             * destabilizes the boot), and this fires on every CD walk. */
            char line[24];
            extern void Saturn_Udec(char *, unsigned int);
            line[0] = 'S'; line[1] = 'K'; line[2] = 'I'; line[3] = 'P';
            line[4] = ' ';
            Saturn_Udec(line + 5, (unsigned)saturn_gfs_pos);
            Saturn_BootText(8, 8, "LOADING WAD");
            Saturn_BootText(8, 40, line);
            Saturn_PumpFrame();
            /* WALK PROGRESS FLIP (2026-09-03 real-HW bring-up): the SKIP
             * text is VDP1-invisible on the user's TV, so flip the stage
             * background orange/black every 256 sectors instead â€” each
             * flip is countable from the boot video (frozen orange or
             * black = GFS_Fread hung mid-walk; steady flipping = the
             * walk is working, just slow). */
            {
                extern void saturn_vdp2_stage_fill(jo_color color);

                saturn_vdp2_stage_fill(
                    (saturn_gfs_pos & 0x200u)
                        ? JO_COLOR_SATURN_RGB(31, 16, 0)
                        : JO_COLOR_Black);
            }
        }
        if (saturn_gfs_read_sector() < 0)
            return -1;
    }
    return 0;
}

/* Open-failure diagnostics, surfaced through the I_Error in I_WadBase so
 * the red screen names the failing primitive. -99 = never reached. */
static int saturn_open_diag_fid  = -99;  /* GFS_NameToId result, last candidate tried */
static int saturn_open_diag_open = -99;  /* 1 = GFS_Open refused, 0 = opened */
static int saturn_open_diag_font = -99;  /* sanity: NameToId("FONT.TGA") at failure time
                                          * (FONT.TGA was already read successfully at boot,
                                          * so a negative here means the GFS directory state
                                          * degraded between font load and WAD open) */

/* Open the IWAD: GFS_NameToId over the candidate list, then one GFS_Open.
 * The handle stays open for the whole session.
 *
 * FILENAMES MUST BE BARE â€” no "/CD/" prefix. GFS_NameToId matches against
 * the ISO root directory table, and the Jo "demo - filesystem" sample
 * opens files as "FILE2.TXT" (this project's font load is the same form:
 * jo_font_load(JO_ROOT_DIR, "FONT.TGA")). "/CD/DOOM.WAD" does not
 * resolve â€” that mismatch was the original "no DOOM.WAD" / blue-screen
 * WAD-load failure. */
static int saturn_open_iwad(void)
{
    static const char *const candidates[] = {
        "DOOM.WAD",
        "DOOM1.WAD",
        "DOOM2.WAD",
        "DOOMULT.WAD",
        NULL
    };
    /* 2026-09-08: retry RETIRED for the wedge bisect — the retry build
     * was the first teal wedge and every later build (which kept it)
     * wedged blacker/earlier. One-shot open restored (the ISO-1
     * control that booted to I_Error) until the wedge is solved.
     * CAUTION: when the retry went under #if 0 the whole candidate
     * loop went with it (boot reached I_Error with fid=-99 open=-99
     * = "never tried" sentinels) — the loop below is the LIVE copy. */
    {
        int i;
        for (i = 0; candidates[i]; i++)
        {
            Sint32 fid;
            saturn_gfs_site = 7;
            fid = GFS_NameToId((Sint8 *)candidates[i]);
            saturn_open_diag_fid = (int)fid;
            if (fid < 0)
            {
                saturn_gfs_site = 0;
                continue;
            }
            saturn_wad_gfs = GFS_Open(fid);
            saturn_gfs_site = 0;
            if (saturn_wad_gfs == JO_NULL)
            {
                saturn_open_diag_open = 1;
                continue;
            }
            saturn_gfs_setup(saturn_wad_gfs);
            saturn_open_diag_open = 0;
            saturn_wad_fid   = fid;   /* for skip-to-sector's rewind re-open */
            /* File geometry from GFS itself: nsct = sector count. Used by
             * I_WadBase to reject a garbage header decode (an infotableofs
             * past EOF) before it turns into a wild pread. */
            {
                Sint32 ssz, nsct, lsz;
                saturn_gfs_site = 9;   /* r65: was unstamped */
                GFS_GetFileSize(saturn_wad_gfs, &ssz, &nsct, &lsz);
                saturn_gfs_site = 0;
                saturn_wad_nsct = (nsct > 0) ? (unsigned)nsct : 0u;
            }
            saturn_wad_open  = 1;
            /* Fresh read-path diagnostics for this handle. */
            saturn_io_diag_stage  = 0;
            saturn_io_diag_seek   = -99;
            saturn_io_diag_fread  = -99;
            saturn_io_diag_seeks  = 0;
            saturn_io_diag_freads = 0;
            saturn_io_diag_oks    = 0;
            saturn_io_diag_mode   = 0;
            saturn_gfs_pos        = 0;   /* fresh handle sits at sector 0 */
            return 1;
        }
    }
    saturn_open_diag_font = (int)GFS_NameToId((Sint8 *)"FONT.TGA");
    return 0;
}

/* Sequential read from the IWAD start. Only used for the 12-byte header
 * immediately after open (GFS position 0), so it is just pread(0). */
int saturn_iwad_read(void *dest, unsigned int nbytes)
{
    return saturn_iwad_pread(0, dest, nbytes);
}

/* ====== title-screen read-path probe (2026-09-06 title stripes) ======
 * One 16-byte pread at an absolute offset into a POISONED static buffer,
 * plus the position cache and the io diag counters, handed to the title
 * screen's on-screen readout. 0xA5 probe bytes = the copy never ran;
 * wrong-but-real bytes (e.g. directory ASCII) = the read "succeeded"
 * from the wrong place / landed stale stage content. */
void Saturn_IwadProbe(unsigned int pos, unsigned char *out,
                      unsigned int *dbg)
{
    static unsigned char pbuf[16];
    unsigned int         pre;
    int                  n, r;

    for (n = 0; n < 16; n++)
        pbuf[n] = 0xA5;
    pre    = saturn_gfs_pos;
    r      = saturn_iwad_pread(pos, pbuf, 16);
    dbg[0] = (unsigned)r;                 /* pread return */
    dbg[1] = saturn_gfs_pos;              /* position cache after */
    dbg[2] = (unsigned)saturn_io_diag_seek;
    dbg[3] = (unsigned)saturn_io_diag_fread;
    dbg[4] = (unsigned)saturn_io_diag_oks;
    dbg[5] = (unsigned)saturn_io_diag_seeks;
    dbg[6] = (unsigned)saturn_io_diag_mode;
    dbg[7] = (unsigned)saturn_io_diag_stage;
    dbg[8] = pre;                         /* position cache before */
    for (n = 0; n < 16; n++)
        out[n] = pbuf[n];
}

/* Read nbytes from the IWAD at absolute byte position pos. GFS tracks CD
 * positions in whole 2048-byte sectors: seek to the sector containing
 * pos, then read sectors sequentially, staging each through the static
 * buffer so the destination can be anywhere (cart, heap, BSS). */
/* Copy nbytes from the CURRENT GFS position into d, where `within` is
 * how far into the current sector the logical start offset falls (0 =
 * read from sector start). Returns 0 / -1. */
static int saturn_copy_from_here(unsigned char *d, unsigned int within,
                                 unsigned int nbytes)
{
    unsigned int left = nbytes;
    int          in_cart;
    /* CD-bus throttle phase — PER CALL (2026-09-07 bugfix: it was a
     * function-level static, so "park first" fired on the first chunk
     * after power-on only; every later train began mid-phase and the
     * park-first interception never ran again). */
    unsigned int burst_n = 0;

#if defined(SATURN_PREAD_TRACE)
    /* WEDGE BRACKET B: the COPY phase began (positioning answered).
     * SALMON (31,12,6) — a small pread (tail-only, no chunks) freezes
     * here too; that is still a bound, not noise. */
    saturn_vdp2_stage_fill(JO_COLOR_SATURN_RGB(31, 12, 6));
#endif

    /* Multi-sector GFS reads are only direct into cart DRAM (A-bus,
     * uncached â€” no SH-2 OC coherency question) when the cart exists.
     * Cached destinations (heap/BSS fallback) keep the staged path. */
    in_cart = cart_initialized && d >= cart_base_ptr
              && d < cart_base_ptr + cart_total_bytes;

    /* r64: during the boot drain even this multi-sector path is gated
     * to the staged 1-sector loop (saturn_fill_boot_mode â€” see
     * saturn_iwad_fill_drain_all). */

    /* Partial first sector: copy the valid tail only. */
    if (within > 0 && left > 0)
    {
        unsigned int take = 2048u - within;
        if (take > left)
            take = left;
        if (saturn_gfs_read_sector() < 0)
            return -1;
        saturn_gfs_site = 10;   /* r65: the cart write itself â€” during
                                 * a memcpy the GFS site is 0, which is
                                 * why the r64 freeze photo showed no
                                 * P/W row at all */
        {
            unsigned int t0 = saturn_vbl_count;         /* r66 */
            memcpy(d, (const void *)(saturn_sect_view() + within), take);
            saturn_prof_cpy_vbl += saturn_vbl_count - t0;
        }
        saturn_gfs_site = 0;
        d    += take;
        left -= take;
    }
    /* Whole middle sectors â€” r71: every one goes to the cart through
     * the SCU DSP's own DMA (Saturn_DspCartCopy2K), with the CPU
     * memcpy from the stage as the live repair/fallback path (the
     * engine is demoted â€” see the v5.13 staging block). The former
     * paths are both banned by the SCU manual: CPU-initiated SCU-DMA
     * writes to the A-bus ("cannot be used", 2.1) and CPU memcpy bursts
     * that the vblank ISR splits mid-sequence (the flash-timing rule,
     * 3.6 â€” the moving-point r64+ wedge). The staged 1-sector loop is
     * the only mechanism left here; multi-sector reads belong to the
     * fill's WRAM-H staged batch. */
    while (left >= 2048u)
    {
        static unsigned int trace_n = 0;   /* PREAD_TRACE chunk counter */
        unsigned int dead_v0 = saturn_vbl_count;   /* r72: chunk-start vbl */

#if defined(SATURN_PREAD_TRACE)
        /* WEDGE PROBE (2026-09-07): paint the completed-chunk count
         * STRAIGHT into the live NBG0 page every 4 chunks. I_FinishUpdate
         * never runs during P_SetupLevel, but StageText writes the
         * VISIBLE page directly (the boot messages prove it) — so when
         * the machine wedges mid-train, the frozen screen names the
         * exact chunk the train died on. A machine reset wipes WRAM but
         * NOT the VDP2 page: the count survives on screen. Cell (8,160):
         * below the perf rows (y<=136), above the y>=198 hazard.
         * (2026-09-07 edition 2: every chunk, not every 4th — the wedge
         * strikes before the first quartet. The milestone FILLS at the
         * loop bottom wipe this text only on chunks 1/4/8; a freeze
         * mid-read shows the last milestone colour + the in-flight
         * chunk's own count.) */
        if (1)
        {
            extern void Saturn_StageText(int x, int y, const char *s);
            static char        buf[10];
            unsigned int       v = trace_n;
            int                k;

            buf[8] = 0;
            for (k = 7; k >= 2; k--)
            {
                buf[k] = (char)('0' + (v % 10u));
                v /= 10u;
            }
            buf[0] = 'C';
            buf[1] = '=';
            Saturn_StageText(8, 160, buf);
        }
#endif

        /* r84: stamp the chunk legs so a wedge inside the staging loop
         * names its leg in the cart flight record (consumed + repainted
         * by the next boot): 8 = wedged in the GFS sector read, 9 =
         * wedged inside the masked cart-write leg, 10 = chunk completed
         * (a parked verdict after 10 = the ISR is dead â€” the honest
         * 256-chunk trigger fired). */
        Saturn_GsubStamp(8);
        if (saturn_gfs_read_sector() < 0)
            return -1;
        saturn_gfs_site = 10;   /* r68: the cart write itself â€” a hang
                                 * in the A-bus write leg paints P=10/W
                                 * instead of nothing */
        {
            unsigned int t0 = saturn_vbl_count;         /* r66 */
            /* r72: cart-write critical section â€” VBlank-IN masked
             * (SCU manual p03_06: A-bus write sequences must not be
             * split by interrupts) across the write burst, the verify
             * reads and any repair memcpy. */
            Saturn_GsubStamp(9);
            if (in_cart)
            {
                Saturn_CartWriteBegin();
                if (Saturn_DspCartCopy2K(d,
                        (const void *)SATURN_SECT_UNC) == 0)
                {
                    const volatile unsigned int *src_v =
                        (const volatile unsigned int *)saturn_sect_view();
                    volatile unsigned int *cart_v =
                        (volatile unsigned int *)(0x40000000u |
                            (0x0FFFFFFFu & (unsigned int)d));
                    unsigned int j;
                    int bad = 0;

                    for (j = 0; j < 512u; j++)
                        if (cart_v[j] != src_v[j]) { bad = 1; break; }
                    saturn_dsp_verify_kb += 2;
                    if (bad)
                    {
                        saturn_dsp_verify_bad++;
                        memcpy(d, (const void *)saturn_sect_view(), 2048u);
                    }
                }
                else
                {
                    memcpy(d, (const void *)saturn_sect_view(), 2048u);
                    saturn_dsp_verify_kb += 2;
                }
                Saturn_CartWriteEnd();
            }
            else
            {
                memcpy(d, (const void *)saturn_sect_view(), 2048u);
            }
            Saturn_GsubStamp(10);   /* r84: chunk completed */
            saturn_cart_kb += 2;
            saturn_prof_cpy_vbl += saturn_vbl_count - t0;
        }
        saturn_gfs_site = 0;
        d    += 2048u;
        left -= 2048u;

        /* r72 pre-loop ISR-death detector â€” r83: THE FRT TRIGGER IS
         * GONE. Its threshold (64608 ticks ~ 3 Kronos vbl) assumed TCNT
         * free-runs; on real HW TCNT is per-vblank-reset (~55 ticks), so
         * every vblank landing between two chunk samples produces a
         * 16-bit WRAP delta of ~65491 > 64608 â€” and CartWriteBegin
         * masks VBlank-IN across the write, which is exactly the window
         * where TCNT resets while vbl_count does not. Wrap + frozen vbl
         * = verdict fire = park, on a healthy machine. The r82 real-HW
         * verdict (W~198 KB, ~99 chunks) is that false fire.
         *
         * The remaining trigger is FRT-free and honest on both machines:
         * 256 completed chunks on one frozen vbl count â€” no honest
         * machine pulls 512 KB through GFS + a cart copy inside one
         * vblank (Kronos ~200 us/chunk => ~51 ms; a live ISR lags a
         * masked vblank by microseconds, it never drops whole frames).
         * Fire = paint the full main-thread verdict and park. */
        {
            static unsigned int dead_chk_v = 0;
            static unsigned int dead_chk_n = 0;

            if (saturn_vbl_count != dead_chk_v)
            {
                dead_chk_v = saturn_vbl_count;
                dead_chk_n = 0;
            }
            if (saturn_vbl_count == dead_v0 && ++dead_chk_n >= 256u)
            {
                extern volatile unsigned int saturn_vbl_dead;
                extern void saturn_vbl_dead_verdict(void);

                saturn_vbl_dead = 1;
                saturn_vbl_dead_verdict();
                for (;;)
                    ;                    /* nothing left that can help */
            }
        }

        /* CD-BUS THROTTLE (2026-09-07 wedge forensics): the three saved
         * .yss states show both SH-2s back in the BIOS bootstrap recycle
         * with HighWram WIPED — the machine was system-RESET mid-level-
         * load, and the only SYS_Exit caller in the linked binary
         * (jo_goto_boot_menu) is unreachable from a level load. The reset
         * comes from outside the game program, under a SUSTAINED GFS read
         * train — the same family that killed the boot fill and the
         * TITLEPIC read, both fixed by slicing (one vblank per 16 KB).
         * The level-load trains never got that treatment. Give pread the
         * same: park until the next vblank (bounded spin — if the ISR is
         * dead, proceed; the 256-chunk frozen-vbl detector above still
         * arbitrates). Edition 2 (2026-09-07, PREAD_TRACE evidence): the
         * frozen wedge screens carry NO C= counter — the reset strikes
         * BEFORE the first 8 KB of the train complete, so the edition-1
         * cadence (first park at 16 KB) could never intercept it. Park
         * FIRST (at pread entry, below) and after every 4th chunk (8 KB
         * cadence). (2026-09-07 bugfix on this block: burst_n was a
         * function-level STATIC, so the park-first leg fired on the
         * first chunk after power-on only, and this bottom-of-loop park
         * landed AFTER chunk 1, not before it — the interception the
         * design names never ran. The per-call counter + the pread-entry
         * park deliver it.) */
        if (++burst_n >= 4u)
        {
            unsigned int v0 = saturn_vbl_count;
            int           spin;

            burst_n = 0;
            for (spin = 0; spin < 200000; spin++)
                if (saturn_vbl_count != v0)
                    break;
        }

#if defined(SATURN_PREAD_TRACE)
        /* WEDGE BRACKET C (2026-09-07): chunk COMPLETED. Milestone fills
         * bound the death point on the proven stage-color channel:
         * LIME (16,31,0) = >=1 chunk, YELLOW (31,31,0) = >=4, WHITE =
         * >=8. Only three fills per train, so the C= text painted at the
         * next chunk's head survives every non-milestone chunk. */
        if (trace_n == 1u)
            saturn_vdp2_stage_fill(JO_COLOR_SATURN_RGB(16, 31, 0));
        else if (trace_n == 4u)
            saturn_vdp2_stage_fill(JO_COLOR_SATURN_RGB(31, 31, 0));
        else if (trace_n == 8u)
            saturn_vdp2_stage_fill(JO_COLOR_White);
#endif
    }
    /* Partial tail: read the final (possibly short) sector, copy what
     * was asked. */
    if (left > 0)
    {
        if (saturn_gfs_read_sector() < 0)
            return -1;
        saturn_gfs_site = 10;
        {
            unsigned int t0 = saturn_vbl_count;         /* r66 */
            memcpy(d, (const void *)saturn_sect_view(), left);
            saturn_prof_cpy_vbl += saturn_vbl_count - t0;
        }
        saturn_gfs_site = 0;
    }
    return 0;
}

int saturn_iwad_pread(unsigned int pos, void *dest, unsigned int nbytes)
{
    unsigned int  sector = pos / 2048u;
    unsigned int  within = pos % 2048u;
    unsigned char *d    = (unsigned char *)dest;
    int           retry;

    if (!saturn_wad_open || saturn_wad_gfs == JO_NULL)
        return -1;

    /* CD-BUS THROTTLE, park-first leg RETIRED (2026-09-07): the design
     * guess — park BEFORE the train so the interception precedes the
     * strike — made the wedge WORSE: 2/2 boots wedged with it (crash5
     * = the same PC=0/WRAM-wiped reset), vs 1/4 for edition 2 without
     * it and 7/7 for the A-bus-traffic probe. The parks are bus-IDLE
     * windows at train onset — the OPPOSITE perturbation to the probe's
     * VDP2 fills, and the evidence says idle-at-once promotes the
     * wedge. The every-4th-chunk park (below) stays: it matches
     * edition 2's tested 3/4 behavior. */

#if defined(SATURN_PREAD_TRACE)
    /* WEDGE BRACKET A (2026-09-07): a pread BEGAN. The bare-cyan wedge
     * screens prove the reset strikes at train onset, but never showed
     * the C= glyphs — the text channel is unproven at level-load time,
     * the stage-COLOR channel is not. ORANGE (31,10,0) = a pread was
     * entered (distinct from the walk flip's (31,16,0)). */
    saturn_vdp2_stage_fill(JO_COLOR_SATURN_RGB(31, 10, 0));
#endif

    /* A background Nw batch may be pending on the shared IWAD handle.
     * CANCEL it rather than draining it to completion: with batches up
     * to FILL_NW_MAX, draining here could stall this on-demand read by
     * ~13 vbl of leftover CD time. Bytes already moved into the cart
     * are idempotent â€” the drain re-issues the batch from fill_part_cd
     * (which has not advanced) and recopies them. The abort re-opens
     * the handle so cache and physical position agree again. */
    fill_txn_abort();

    saturn_io_diag_oks = 0;   /* count sectors staged by THIS pread */

    /* Positioning. Absolute GFS_Seek(GFS_SEEK_SET) has refused every far
     * seek on this setup, so use Jo Engine's own proven pattern instead
     * (jo_engine/fs.c seek_forward): ONE relative GFS_Seek(GFS_SEEK_CUR)
     * with the forward sector delta. A backward target re-opens first
     * (fresh handle sits at sector 0, so the delta is then the target
     * itself). If we are already at the target sector, no seek at all â€”
     * the header read right after open is therefore a pure sequential
     * read, same as the font load. */
    if (saturn_gfs_pos != sector)
    {
        int positioned = 0;

        if (saturn_gfs_pos > sector)
        {
            if (saturn_walk_to_sector(0, 1) < 0)   /* re-open: position 0 */
                return -1;
            /* PRIMER (2026-09-06 title stripes): the first GFS_Fread
             * after a mid-session re-open ANSWERS (probe2: F2 returned
             * 0800, seek and position bookkeeping honest) without
             * landing data â€” the stage kept the boot's last directory
             * sector. The boot's own proven pattern is open -> read
             * sector 0 -> seek -> read, so replicate it: one primer
             * read of sector 0 VERIFIED against the IWAD magic; if the
             * magic never lands, walk forward from 0 on the fresh
             * handle (slow, proven) instead of trusting the relative
             * seek. */
            if (sector > 0)
            {
                int primed = 0;

                for (retry = 0; retry < 4 && !primed; retry++)
                {
                    const unsigned char *sv;

                    if (saturn_gfs_read_sector() < 0)
                        continue;
                    sv = (const unsigned char *)saturn_sect_view();
                    if (sv[0] == 'I' && sv[1] == 'W' &&
                        sv[2] == 'A' && sv[3] == 'D')
                        primed = 1;
                }
                if (!primed)
                {
                    if (saturn_walk_to_sector(sector, 1) < 0)
                        return -1;
                    positioned = 1;    /* walk left us at the target */
                }
            }
            if (saturn_gfs_pos == sector)
                positioned = 1;        /* sector 0: the re-open was it */
        }
        if (!positioned)
        {
        for (retry = 0; retry < 8; retry++)
        {
            unsigned int t0;
            saturn_io_diag_stage = 1;
            saturn_io_diag_seeks++;
            t0 = saturn_vbl_count;                      /* r66 */
            saturn_gfs_site = 4;
            saturn_io_diag_seek = GFS_Seek(saturn_wad_gfs,
                                           (Sint32)(sector - saturn_gfs_pos),
                                           GFS_SEEK_CUR);
            saturn_prof_gfs_vbl += saturn_vbl_count - t0;
            saturn_gfs_site = 0;
            if (saturn_io_diag_seek >= 0)
            {
                saturn_gfs_pos = sector;   /* only on an accepted seek */
                break;
            }
            { volatile int t; for (t = 0; t < 10000; t++) { } } /* settle */
        }
        if (retry >= 8)
        {
            /* Relative seek refused too â€” walk forward on the current
             * handle first; if that fails (the refused seeks may have
             * wedged the handle), re-open and walk from sector 0. */
            if (saturn_walk_to_sector(sector, 0) < 0 &&
                saturn_walk_to_sector(sector, 1) < 0)
                return -1;
        }
        }
    }

    saturn_io_diag_mode = 3;   /* positioned; the copy phase decides */
    if (saturn_copy_from_here(d, within, nbytes) == 0)
        return (int)nbytes;

    /* Data reads refused even though positioning answered â€” the handle is
     * likely wedged. One last resort: fresh handle, walk to the target,
     * copy again. */
    if (saturn_walk_to_sector(sector, 1) < 0)
        return -1;
    saturn_io_diag_mode = 3;
    if (saturn_copy_from_here(d, within, nbytes) == 0)
        return (int)nbytes;
    return -1;
}

/* Read nbytes from CD at offset `cd_offset` directly into cart at
 * `cart_offset`. Used to bulk-load HOT lumps at boot. */
static int saturn_copy_to_cart(unsigned int cd_offset, unsigned int cart_offset,
                               unsigned int nbytes)
{
    int n = saturn_iwad_pread(cd_offset,
                              cart_base_ptr + cart_offset,
                              nbytes);
    return (n == (int)nbytes) ? 0 : -1;
}

/* ====== I_WadBase â€” main entry ====== */

/* r84: the consumed PREVIOUS-boot GSUB record (0 = none). Lives in a
 * global so the row can be REPAINTED after the boot stage fills â€” the
 * one-time I_WadBase consume paint is wiped by the CYAN/STARTING fill
 * at d_main.c, which is exactly the colour a silent wedge shows. The
 * ISR census and the post-CYAN main-thread paint both re-derive the
 * row from here. */
volatile unsigned int saturn_gsub_rec = 0;

byte *I_WadBase(void)
{
    static int initialized = 0;
    static byte *wad_ptr = NULL;
    wadinfo_t *hdr;
    lumpinfo_t *dir;
    int n, numlumps;
    unsigned int infotableofs;
    int i;
    int first_patch = -1, last_patch = -1;
    int first_flat  = -1, last_flat  = -1;
    int first_sprt  = -1, last_sprt  = -1;
    int first_map_block_start = -1, first_map_block_end = -1;
    unsigned int hot_cap = 0;  /* max cart offset HOT bodies may occupy */
    unsigned char *base;   /* either cart_base_ptr or saturn_iwad_heap */

    if (initialized)
        return wad_ptr;
    initialized = 1;

    /* r83: MAIN-THREAD disc ID at (220,48) â€” the r80-style ISR pass-1
     * paint dies with the ISR (and the r82 photos could not settle
     * which disc was in the drive). Painted before anything can wedge;
     * virgin cell, nothing else writes (220,48). */
    {
        extern void Saturn_StageText(int x, int y, const char *s);

        Saturn_StageText(220, 48, "r91");
    }

    /* Init cart (non-fatal if absent â€” we fall back to heap streaming). */
    Saturn_CartInit();

    /* r81: consume the PREVIOUS boot's GSUB record â€” paint G=<sub> at
     * (140,88), next to the R census row, then clear the magic (one
     * boot per record). Painted before this boot stamps anything, so
     * G= names where the PREVIOUS boot died. */
    {
        extern unsigned int saturn_cart_base_addr;
        extern unsigned int saturn_cart_size;
        extern void         Saturn_StageText(int x, int y, const char *s);

        if (saturn_cart_base_addr != 0u)
        {
            volatile unsigned int *rc = (volatile unsigned int *)
                (saturn_cart_base_addr + SATURN_CART_REC_OFF);

            if (rc[40] == 0x47535542u)          /* 'GSUB' */
            {
                extern volatile unsigned int saturn_init_site;

                saturn_gsub_rec = rc[41] & 15u;   /* r84: for repaints */
                /* r85: carry the record in the init-site breadcrumb
                 * itself â€” 0x50+sub (80..90) can never collide with the
                 * real 1..9 init sites, the census I row and the
                 * verdict J row both show it, and neither needs its
                 * own paint (the old (140,88) G= paint was past the
                 * x=160 page cut real-HW photos proved). G_InitNew
                 * overwrites this with 1 the moment it is entered. */
                saturn_init_site = 0x50u + (rc[41] & 15u);
                rc[40] = 0u;
            }
        }
    }
    Saturn_GsubStamp(1);   /* cart probed */

    /* r71: arm the SCU DSP cart-write engine (the manual-sanctioned
     * A-bus write mechanism â€” CPU-initiated SCU-DMA to the A-bus is
     * banned by SCU manual 2.1, and the ISR-split CPU memcpy bursts
     * violate the flash-timing rule in 3.6). Non-fatal: on failure
     * every cart write falls back to the CPU memcpy through the
     * uncached mirrors.
     * r94: re-enabled. The r77 "A-bus arbitration deadlock" suspect is
     * EXONERATED: demo1's hardware-validated rig (burn 09-05, 8/8) proved
     * the real failure was our program shape â€” JMP-T0 spins never end on
     * real silicon (T0 never clears) and the hammering PPAF poll that
     * followed choked the bus. saturn_dsp.c now runs the exact wait-free
     * protocol demo1 validated on the same silicon. */
    if (saturn_dsp_engine_armed)
    {
        Saturn_GsubStamp(2);
        Saturn_DspCartInit();
    }

    Saturn_GsubStamp(3);   /* before saturn_open_iwad */

    /* Open IWAD. */
    if (!saturn_open_iwad())
    {
        I_Error("I_WadBase: no WAD on /CD fid %d open %d font %d",
                saturn_open_diag_fid, saturn_open_diag_open, saturn_open_diag_font);
        return NULL;
    }

    /* Decide where to put the IWAD header + directory. With a cart we
     * use it directly (cart_base_ptr â€” whichever mirror Saturn_CartInit
     * verified); without a cart we malloc a heap buffer of the same
     * layout. The d32xr code downstream treats both as
     * `wadfile[0].fileptr` so the location is transparent. */
    if (cart_initialized)
    {
        base = cart_base_ptr;
    }
    else
    {
        /* Heap-resident IWAD metadata. Allocate IWAD_DIR_BYTES for the
         * worst-case 2308-lump directory. Wastes a few KB on smaller
         * WADs but simplifies the layout. jo_malloc returns NULL on
         * failure; bail out rather than corrupting the heap later. */
        saturn_iwad_heap = (unsigned char *)jo_malloc(IWAD_DIR_BYTES);
        if (!saturn_iwad_heap)
        {
            I_Error("I_WadBase: heap alloc for IWAD dir failed");
            return NULL;
        }
        memset(saturn_iwad_heap, 0, IWAD_DIR_BYTES);
        base = saturn_iwad_heap;
    }

    /* Read IWAD header into base. */
    hdr = (wadinfo_t *)base;
    n = saturn_iwad_read(base, sizeof(wadinfo_t));
    if (n != sizeof(wadinfo_t))
    {
        /* stage: 1 = GFS_Seek refused, 2 = GFS_Fread refused; seek/fread
         * = last return values; s-tries/f-tries = attempts used. */
        I_Error("I_WadBase: short hdr read: stage %d seek %d fread %d st %d ft %d fid %d",
                saturn_io_diag_stage, saturn_io_diag_seek, saturn_io_diag_fread,
                saturn_io_diag_seeks, saturn_io_diag_freads, saturn_open_diag_fid);
        return NULL;
    }
    if (hdr->identification[0] != 'I' || hdr->identification[1] != 'W' ||
        hdr->identification[2] != 'A' || hdr->identification[3] != 'D')
    {
        /* First 4 bytes + the file id we opened, so a repeat failure
         * names what we actually read (zeros = stale-OC/staged-read bug,
         * PWAD = wrong file, garbage = wrong offset). */
        I_Error("I_WadBase: not IWAD got %02X %02X %02X %02X fid %d",
                (unsigned char)hdr->identification[0],
                (unsigned char)hdr->identification[1],
                (unsigned char)hdr->identification[2],
                (unsigned char)hdr->identification[3],
                saturn_open_diag_fid);
        return NULL;
    }
    /* Byte-swap the header fields now â€” they arrived raw little-endian
     * and every reader below (ours AND d32xr's W_Init) goes through
     * BIGLONG, which is the identity on this build. See the endianness
     * block at the top of this file. identification[4] is a char array,
     * untouched. */
    hdr->numlumps     = LITTLELONG(hdr->numlumps);
    hdr->infotableofs = LITTLELONG(hdr->infotableofs);

    /* Sub-stage marker (see the decode table in main.c): MEDIUM GREEN =
     * IWAD opened and header validated; the next call is the lump-
     * directory pread, which on the 12.4 MB rerelease WAD seeks ~11.8 MB
     * (â‰ˆ6035 sectors) forward through GFS â€” the longest single GFS
     * operation of the boot. A freeze on MEDIUM GREEN names that seek. */
    Saturn_StageColor(JO_COLOR_MediumGreen);
    numlumps     = BIGLONG(hdr->numlumps);
    infotableofs = (unsigned)BIGLONG(hdr->infotableofs);
    saturn_wad_size = infotableofs + (unsigned)numlumps * 16u;

    /* r91: the directory buffer holds exactly 2308 entries (IWAD_DIR_BYTES).
     * A bigger WAD would overflow it â€” cart layout: hot lumps are placed
     * from IWAD_DIR_BYTES upward and would overwrite the directory tail;
     * heap layout: a multi-KB jo_heap overflow. Refuse with a named
     * verdict instead (DOOM2.WAD is 2458 lumps). */
    if ((unsigned)numlumps > (IWAD_DIR_BYTES - 12u) / 16u)
    {
        I_Error("WAD TOO BIG");
        return NULL;
    }

    /* Geometry guard: the decoded directory offset must land inside the
     * file GFS actually opened. A stale-OC or otherwise garbage header
     * decode would otherwise send the pread chasing an offset past EOF
     * (or coincidentally "already there"), muddying every diagnostic
     * below with a failure that isn't the transport's fault. */
    if (numlumps <= 0 || infotableofs / 2048u >= saturn_wad_nsct ||
        infotableofs + (unsigned)numlumps * 16u > saturn_wad_nsct * 2048u)
    {
        I_Error("DIR OFFSET PAST END");
        return NULL;
    }

    /* Read lump directory into base+12. */
    dir = (lumpinfo_t *)(base + 12);
    n = saturn_iwad_pread(infotableofs, dir, (unsigned)numlumps * 16u);
    if ((unsigned)n != (unsigned)numlumps * 16u)
    {
        /* Pure WORDS, no numbers: glued digits on the 8x8 font are not
         * readable back from the screen, but words are. MODE names the
         * transport that failed; the SEEK/READ lines say whether the
         * last GFS_Seek/GFS_Fread answered, refused, or never ran;
         * STAGED says how far the sector staging got (ZERO = the very
         * first read refused; ALL = everything moved but the copy
         * still failed). */
        {
            const char *mode, *sw, *rw, *ow;
            unsigned int need = ((infotableofs % 2048u)
                               + (unsigned)numlumps * 16u + 2047u) / 2048u;
            switch (saturn_io_diag_mode)
            {
                case 1:  mode = "WALK ON HANDLE FAILED";  break;
                case 2:  mode = "WALK ON REOPEN FAILED";  break;
                case 3:  mode = "POSITIONED READ FAILED"; break;
                default: mode = "SEEK OK READ FAILED";    break;
            }
            sw = (saturn_io_diag_seek == -99) ? "SEEK NEVER TRIED"
               : (saturn_io_diag_seek < 0)    ? "SEEK REFUSED"
                                               : "SEEK ANSWERED";
            rw = (saturn_io_diag_fread == -99) ? "READ NEVER TRIED"
               : (saturn_io_diag_fread <= 0)   ? "READ REFUSED"
                                               : "READ ANSWERED";
            ow = (saturn_io_diag_oks == 0)          ? "STAGED ZERO"
               : (saturn_io_diag_oks >= need)       ? "STAGED ALL"
                                                    : "STAGED SOME";
            I_Error("DIR READ FAILED\n%s\n%s\n%s\n%s", mode, sw, rw, ow);
            return NULL;
        }
    }

    /* Sanity tripwire: the first directory entry must start with a
     * printable lump name. A walk that overshot its target (position
     * bookkeeping vs. a half-moved refused seek) hands back plausible
     * lengths but garbage bytes â€” catch that here instead of crashing
     * deep in the lump classifier. */
    if ((unsigned char)dir[0].name[0] < 0x21u ||
        (unsigned char)dir[0].name[0] > 0x7Eu)
    {
        I_Error("DIR READ GAVE BAD DATA");
        return NULL;
    }

    /* Byte-swap every directory entry's filepos/size â€” same reason as
     * the header swap above: the entries arrived raw little-endian and
     * every consumer (our classifier loop below, d32xr's W_Init /
     * W_LumpLength / W_GetRawLumpData) reads them through BIGLONG
     * (identity). The name[8] fields are byte strings, untouched. */
    for (i = 0; i < numlumps; i++)
    {
        dir[i].filepos = LITTLELONG(dir[i].filepos);
        dir[i].size    = LITTLELONG(dir[i].size);
    }

    /* Sub-stage marker: SEMI GREEN = directory read + numlumps/infotableofs
     * decoded OK. Everything below is CPU-only classification plus the
     * â‰¤4 first-frame-critical GFS sync copies (PLAYPAL/COLORMAP/FONT) â€”
     * a freeze on SEMI GREEN names one of those copies (or the enqueue
     * loop), not the directory seek. */
    Saturn_StageColor(JO_COLOR_SemiGreen);

    /* W_Init recomputes `lumpinfo = fileptr + BIGLONG(infotableofs)`.
     * BIGLONG is the identity on Saturn, and the directory now lives
     * at fileptr+12 (not at the on-disc infotableofs), so rewrite the
     * header field RAW to point at our copy. Without this, lumpinfo
     * lands 12 MB out into A-bus space. */
    hdr->infotableofs = 12;

    /* Find P/F/S marker ranges. */
    for (i = 0; i < numlumps; i++)
    {
        if (lump_name_eq(dir[i].name, "P_START"))
            first_patch = i;
        else if (lump_name_eq(dir[i].name, "P_END"))
            last_patch = i;
        else if (lump_name_eq(dir[i].name, "F_START"))
            first_flat = i;
        else if (lump_name_eq(dir[i].name, "F_END"))
            last_flat = i;
        else if (lump_name_eq(dir[i].name, "S_START"))
            first_sprt = i;
        else if (lump_name_eq(dir[i].name, "S_END"))
            last_sprt = i;
        else if (lump_name_is_map(dir[i].name))
        {
            /* First map's level-data block: [marker, next marker).
             * Everything after the second marker is a later map. */
            if (first_map_block_start < 0)
                first_map_block_start = i;
            else if (first_map_block_end < 0)
                first_map_block_end = i;
        }
    }

    /* Allocate cart space for HOT lump bodies, packed back-to-back.
     * Only meaningful when the cart is present. With no cart, every
     * lump stays COLD and cart_used_bytes stays 0, which the dispatch
     * shims interpret as "no cart-resident range" so they always
     * stream from CD.
     *
     * IMPORTANT: we do NOT copy any HOT bodies here at boot. Doing so
     * synchronously would mean ~600 jo_fs_open/seek/read round-trips
     * before the first frame â€” exceeding the emulator's "no VBlank
     * progress" timeout and looking like a frozen black screen. We
     * instead just RESERVE the cart space (advance cart_used_bytes
     * as if we'd copied) and enqueue the actual copies in
     * saturn_fill_queue. The VBlank callback drains one entry per
     * tick so the cart progressively fills during gameplay.
     *
     * A small fast-path copy is done synchronously for lumps that
     * the first frame absolutely needs (PLAYPAL, COLORMAP, FONT) so
     * the title screen can render correctly without waiting for
     * background fills. */
    cart_used_bytes = cart_initialized ? IWAD_DIR_BYTES : 0u;
    /* HOT bodies may never cross into the cart's upper half (the Z zone)
     * or the end of a 1 MB cart. */
    hot_cap = cart_total_bytes < CART_HOT_CAP ? cart_total_bytes : (unsigned)CART_HOT_CAP;
    /* r91: RESERVE the cart's top 512 bytes for the boot-order records
     * (REC1 rec[0..18], GSTL rec[32..36], GSUB rc[40..42] â€” every one
     * is addressed from saturn_cart_size-512). Before this reserve the
     * hot-lump fill region ran right up to cart_size, so every stamp
     * into the record slot overwrote 12 bytes of whichever hot lump
     * body the greedy pack had placed over the slot â€” silent WAD
     * corruption (sprite/flat/map data) on every boot and every level
     * init. Boot stamps (1-13) fire before the fills so they only
     * scratched previous-boot DRAM, but the level-init stamps (21-29)
     * fire while the region is fully filled. */
    /* r92: the records moved to SATURN_CART_REC_OFF (512KB-512, the
     * last march-verified write point) — cap the fill region below
     * the slot so neither side can clobber the other. Costs ~480KB of
     * hot-cart capacity; those lumps just stream from CD instead. */
    if (hot_cap > SATURN_CART_REC_OFF)
        hot_cap = SATURN_CART_REC_OFF;
    saturn_fill_head   = 0;
    saturn_fill_count  = 0;
    saturn_fill_active = cart_initialized ? 1 : 0;
    fill_part_left     = 0;   /* no half-copied entry from a prior arm */

    /* Multi-pass directory classification:
     * Pass 0: First-frame critical named lumps + first map block (THINGS..BLOCKMAP)
     * Pass 1: ALL flats (F_START..F_END) - critical for VDP1 floor/ceiling rendering
     * Pass 2: Patches and Sprites (remaining space under hot_cap and queue limit)
     */
    uint8_t placed[512];
    memset(placed, 0, sizeof(placed));

    int pass;
    for (pass = 0; pass < 3; pass++)
    {
        for (i = 0; i < numlumps; i++)
        {
            if (i < 4096 && (placed[i >> 3] & (1 << (i & 7))))
                continue;

            /* Skip the marker lumps themselves. */
            if (lump_name_eq(dir[i].name, "P_START") ||
                lump_name_eq(dir[i].name, "P_END") ||
                lump_name_eq(dir[i].name, "F_START") ||
                lump_name_eq(dir[i].name, "F_END") ||
                lump_name_eq(dir[i].name, "S_START") ||
                lump_name_eq(dir[i].name, "S_END"))
            {
                continue;
            }

            int is_hot = 0;
            if (pass == 0)
            {
                if (first_map_block_start >= 0 &&
                    first_map_block_end > first_map_block_start &&
                    i >= first_map_block_start && i < first_map_block_end)
                    is_hot = 1;
                else if (lump_is_named_hot(dir[i].name))
                    is_hot = 1;
            }
            else if (pass == 1)
            {
                if (first_flat >= 0 && last_flat >= 0 &&
                    i > first_flat && i < last_flat)
                    is_hot = 1;
            }
            else /* pass == 2 */
            {
                if (first_patch >= 0 && last_patch >= 0 &&
                    i > first_patch && i < last_patch)
                    is_hot = 1;
                else if (first_sprt >= 0 && last_sprt >= 0 &&
                         i > first_sprt && i < last_sprt)
                    is_hot = 1;
            }

            if (!is_hot)
                continue;

            unsigned int lump_size = (unsigned)BIGLONG(dir[i].size);
            unsigned int cd_off    = (unsigned)BIGLONG(dir[i].filepos);
            unsigned int cart_off;
            int          is_first_frame_critical = 0;

            cart_off = cart_used_bytes;
            if (is_hot && lump_size > FILL_PREWARM_MAX)
                is_hot = 0;

            if (is_hot && lump_size > 0 && cart_initialized &&
                cart_off + lump_size <= hot_cap &&
                saturn_fill_count < SATURN_FILL_QUEUE_MAX)
            {
                if (lump_name_eq(dir[i].name, "PLAYPAL")  ||
                    lump_name_eq(dir[i].name, "COLORMAP") ||
                    lump_name_eq(dir[i].name, "FONT1")    ||
                    lump_name_eq(dir[i].name, "FONT2"))
                    is_first_frame_critical = 1;

                if (is_first_frame_critical)
                {
                    char namebuf[9];
                    int  k;
                    for (k = 0; k < 8 && dir[i].name[k]; k++)
                        namebuf[k] = dir[i].name[k];
                    namebuf[k] = '\0';
                    Saturn_StageColor(JO_COLOR_SemiYellow);
                    Saturn_BootText(8, 24, namebuf);
                    Saturn_PumpFrame();
                    if (saturn_copy_to_cart(cd_off, cart_off, lump_size) < 0)
                    {
                        I_Error("I_WadBase: cart copy failed for lump %d", i);
                        return NULL;
                    }
                }
                saturn_fill_queue[saturn_fill_count].cd_off     = cd_off;
                saturn_fill_queue[saturn_fill_count].cart_off   = cart_off;
                saturn_fill_queue[saturn_fill_count].size       = lump_size;
                saturn_fill_queue[saturn_fill_count].lump_index =
                    i | (is_first_frame_critical ? FILL_SYNCED : 0);
                saturn_fill_count++;

                dir[i].filepos = (int)cart_off;
                cart_used_bytes = cart_off + lump_size;
                cart_used_bytes = (cart_used_bytes + 3u) & ~3u;
                if (i < 4096)
                    placed[i >> 3] |= (uint8_t)(1 << (i & 7));
            }
        }
    }

    /* Any lump that was not placed in cart gets biased with COLD_BIAS */
    for (i = 0; i < numlumps; i++)
    {
        if (i >= 4096 || !(placed[i >> 3] & (1 << (i & 7))))
        {
            unsigned int cd_off = (unsigned)BIGLONG(dir[i].filepos);
            dir[i].filepos = (int)(cd_off | COLD_BIAS);
        }
    }

    /* Tell d32xr's W_GetRawLumpData to use the CD chain for ALL lumps
     * â€” even hot ones â€” so our shims can dispatch per-lump. (W_Init's
     * D_memset(wadfile,...) runs before I_WadBase is called, so this
     * store survives; g_game.c re-applies it after W_Init anyway as
     * insurance.) */
    wadfile[0].cdlength = 1;
    wadfile[0].cdoffset = 0;

    /* HUD progress strip (i_video.c): total queue length is known now;
     * saturn_fill_remaining tracks count-head from the per-frame drain. */
    saturn_fill_total = saturn_fill_count;
    saturn_fill_remaining = saturn_fill_count;

    /* Sub-stage marker: TEAL-GREEN = I_WadBase complete (directory
     * rewritten, fill queue armed). The next stages are Z_Init + player
     * arrays in d_main.c, which paint PURPLE when they survive.
     * (2026-09-08: recolored from pure GREEN — it shared the exact
     * OCSETUP green in d_main.c and the Kronos boot wedge froze on an
     * ambiguous solid green; a frozen TEAL convicts this site, a frozen
     * pure GREEN convicts OCSETUP/Divlut_Install.) */
    Saturn_StageColor(JO_COLOR_SATURN_RGB(0, 31, 10));

    wad_ptr = base;
    return wad_ptr;
}

/* ====== Saturn CD-read shims (called by d32xr's w_wad.c when wad->cdlength > 0) ====== */

/* Abort the in-flight batch AND restore the handle to a position both
 * GFS and our saturn_gfs_pos cache agree on. A stopped Nw leaves the
 * physical position unknown (consumed sectors never reached the cache),
 * and this SGL only honors forward relative seeks from a known
 * position â€” so re-open: a fresh handle sits at sector 0, the same
 * recovery walk_to_sector trusts. The next positioning (an on-demand
 * pread's target, or the drain's next batch) is then a plain forward
 * seek from 0. */
static void fill_txn_abort(void)
{
    if (!fill_txn)
        return;   /* nothing pending â€” do NOT touch the handle */
    saturn_gfs_site = 5;
    GFS_NwStop(saturn_wad_gfs);
    fill_txn = 0;
    GFS_Close(saturn_wad_gfs);
    saturn_wad_gfs = GFS_Open(saturn_wad_fid);
    saturn_gfs_site = 0;
    if (saturn_wad_gfs == JO_NULL)
        return;   /* pread's entry check reports the dead handle */
    saturn_gfs_setup(saturn_wad_gfs);
    saturn_gfs_pos = 0;
}

/* Pump the pending async Nw batch. Returns bytes completed (> 0 â€” the
 * fill cursor is advanced), 0 = still pending (resume next call),
 * -1 = error (batch stopped; caller falls back to the sync path).
 * Never spins unbounded: FRT budget per call plus a hard round cap
 * since issue (same 4096-round guard as the old synchronous multi
 * path). The first round always runs so every call makes progress. */
static int fill_txn_pump(unsigned int budget_ticks)
{
    unsigned int f0   = SAT_FRT();
    Sint32       stat = 0, nbyte = 0;

    for (;;)
    {
        saturn_gfs_site = 3;
        GFS_NwExecOne(saturn_wad_gfs);
        GFS_NwGetStat(saturn_wad_gfs, &stat, &nbyte);
        saturn_gfs_site = 0;
        if ((unsigned int)nbyte > fill_txn_last)
        {
            fill_txn_last      = (unsigned int)nbyte;
            fill_txn_prog_vbl  = saturn_vbl_count;   /* progress */
        }
        if (nbyte >= (Sint32)fill_txn_bytes ||
            stat == GFS_SVR_COMPLETED || stat == GFS_SVR_ERROR ||
            (unsigned int)(saturn_vbl_count - fill_txn_prog_vbl) >= FILL_WEDGE_VBLS)
            break;
        if ((unsigned short)(SAT_FRT() - f0) >= budget_ticks)
            return 0;   /* still pending â€” resume on the next call */
    }
    if (nbyte <= 0 || (stat != GFS_SVR_COMPLETED && nbyte < (Sint32)fill_txn_bytes))
    {
        fill_txn_abort();
        return -1;
    }
    saturn_gfs_pos += fill_txn_bytes >> 11;   /* sectors landed */
    fill_part_cd   += fill_txn_bytes;
    fill_part_cart += fill_txn_bytes;
    fill_part_left -= fill_txn_bytes;
    fill_txn        = 0;
    return (int)fill_txn_bytes;
}

/* r71: no shrink ladder â€” a batch is one 2 KB sector landed in the
 * WRAM-H stage. Any Nw failure demotes the async path to the proven
 * sync path permanently. */
static void fill_nw_shrink(void)
{
    saturn_direct_reads_ok = 0;
}

/* r71: cart offset the in-flight staged batch was issued at â€” the
 * drain's pump-completion leg moves the stage there via the DSP
 * engine (SCU-DMA may never write the A-bus directly). */
static unsigned int fill_txn_cart_off = 0;

/* Issue one async Nw batch: `nbytes` (sector-aligned, 2 KB) from the
 * CURRENT handle position into the WRAM-H stage â€” NEVER the cart
 * ("Writing to A-Bus by SCU-DMA cannot be used", SCU manual 2.1; the
 * r63-69 boot wedge). The handle must already sit at the batch's
 * first sector â€” the drain positions it with a forward seek first. */
static int fill_txn_issue(unsigned int nbytes)
{
    saturn_gfs_site = 2;
    /* v5.13: the batch lands in the stage view the tripwire currently
     * trusts (cached while green). TMODE_CPU is a property of the
     * GfsHn, and the transfer executes during GFS_NwExecOne polling on
     * the main thread — expected to be an SH-2 CPU copy; the pump's
     * tripwire below demotes staging permanently if it ever isn't. */
    if (GFS_NwFread(saturn_wad_gfs, (Sint32)(nbytes >> 11),
                    saturn_stage_dest(),
                    (Sint32)nbytes) < 0)
    {
        saturn_gfs_site = 0;
        return -1;
    }
    saturn_gfs_site = 0;
    fill_txn_cart_off  = fill_part_cart;
    fill_txn_bytes     = nbytes;
    fill_txn_last      = 0;
    fill_txn_prog_vbl  = saturn_vbl_count;
    fill_txn           = 1;
    return 0;
}

/* Public light pump: advances a pending batch without issuing new work.
 * Called from idle spins (the display-cap wait in D_DoomLoop) so the CD
 * stream keeps moving during vblank dead time. No-op when no batch is
 * pending. */
void saturn_iwad_fill_pump(void)
{
    if (fill_txn && saturn_wad_open && saturn_wad_gfs != JO_NULL)
        (void)fill_txn_pump(FILL_PUMP_TICKS);
}

/* Blocking full drain (2026-09-02, user request): run the background
 * fill in a loop until the queue is EMPTY, called once from D_DoomLoop
 * BEFORE the first rendered frame â€” the whole ~2 MB prewarm absorbs
 * into the boot screen, and no CD reads / SCU DMA / cart writes ever
 * compete with rendering afterwards (saturn_fill_active is disabled by
 * the last iteration, so the per-frame call in D_DoomLoop turns free).
 * The per-call vblank budget still bounds each slice; the loop just
 * keeps slicing. Watchdog: if the CD stalls for >60 s, hand the
 * remainder back to the in-game async drain instead of hanging boot. */
void saturn_iwad_fill_drain_all(void)
{
    extern volatile unsigned int saturn_vbl_count;
    unsigned int v0 = saturn_vbl_count;

    /* r64: boot-drain sync-only gate (see saturn_fill_boot_mode). */
    saturn_fill_boot_mode = 1;
    while (saturn_fill_active && saturn_fill_head < saturn_fill_count &&
           (int)(saturn_vbl_count - v0) < 60 * 60)
        saturn_iwad_background_fill();
    saturn_fill_boot_mode = 0;
    saturn_fill_remaining = 0;
    saturn_fill_active = 0;
}

/* Drain the background cart-fill queue. Called once per displayed frame
 * from D_DoomLoop. Since 2026-09-02 the queue moves through ONE
 * outstanding async Nw batch (FILL_NW_BYTES): aligned chunks are issued
 * with GFS_NwFread and pumped across frames (fill_txn_pump), so the CD
 * transfer overlaps the render instead of stalling it; unaligned heads
 * and <2 KB tails still stage through the synchronous
 * saturn_copy_to_cart path (at most two sectors per entry). The return
 * value is entries COMPLETED this frame (the caller's HUD bar treats it
 * as an activity measure). */
int saturn_iwad_background_fill(void)
{
    unsigned int   cursor  = 0;    /* cd position after the last chunk */
    int            entries = 0;    /* entries completed this frame   */
    int            copied  = 0;    /* any chunk copied this frame    */
    unsigned int   bytes   = 0;

    if (!saturn_fill_active || saturn_fill_count == 0)
    {
        saturn_fill_remaining = 0;   /* strip off */
        return 0;
    }
    if (saturn_fill_head >= saturn_fill_count)
    {
        saturn_fill_remaining = 0;
        return 0;
    }

    {
        /* Vblank-budget gate (2026-09-02): the old FRT budget
         * (FILL_BUDGET_TICKS = 12000 ~ 3.3 ms) is far SMALLER than one
         * 8 KB chunk's disc time (~27 ms at 2x speed), so every frame
         * broke right after the first chunk and the ~2 MB prewarm queue
         * drained at ~8 KB/frame â€” the multi-minute 4-5 fps window in
         * early play (F row read up to 7 vbl, fps 4-5, then leapt to 12
         * when the queue emptied). Budget in VBLANKS instead: keep
         * copying while the drain is <= FILL_BUDGET_VBLS old (plus one
         * chunk overshoot), so the drain costs a bounded slice of every
         * frame but the CD pipe stays saturated â€” minutes -> ~10-20 s. */
        extern volatile unsigned int saturn_vbl_count;
        unsigned int v0 = saturn_vbl_count;

        while (bytes < FILL_FRAME_BYTES)
        {
            saturn_fill_t *nx;

            /* The first chunk always runs (progress guarantee); later
             * chunks only while inside the vblank budget. */
            if (copied && (int)(saturn_vbl_count - v0) >= FILL_BUDGET_VBLS)
                break;

        if (fill_part_left == 0)
        {
            nx = &saturn_fill_queue[saturn_fill_head];
            if (nx->lump_index & FILL_SYNCED)
            {
                saturn_fill_head++;   /* boot-copied; skip free of charge */
                entries++;
                continue;
            }
            /* Entries are packed in cd order (the queue was built in
             * directory order), so a CONTIGUOUS successor is seek-free
             * through pread's saturn_gfs_pos cache. A gap (demoted fat
             * lump between hot placements) ends the batch; it resumes
             * next frame with one fresh seek. */
            if (copied && nx->cd_off != cursor)
                break;
            fill_part_cd   = nx->cd_off;
            fill_part_cart = nx->cart_off;
            fill_part_left = nx->size;
        }
        nx = &saturn_fill_queue[saturn_fill_head];

        /* Batch in flight from an earlier frame: pump it under the FRT
         * budget. While pending, the CD streams on the SH-1 between
         * frames â€” the drain costs near nothing per frame. */
        if (fill_txn)
        {
            int got = fill_txn_pump(FILL_PUMP_TICKS);

            if (got > 0)
            {
                /* r71: the Nw batch landed in the WRAM-H stage â move
                 * it to the cart (SCU-DMA may never write the A-bus;
                 * the DSP engine call is the one-line re-arm path, the
                 * CPU memcpy from the stage is the live path). r102
                 * DSP-eval: full readback verify of every landed batch
                 * (was 2-byte spot), through the cart's cache-through
                 * mirror â on mismatch or DSP timeout, repair with the
                 * CPU memcpy from the stage view and count the chunk
                 * in saturn_dsp_verify_bad. */
                unsigned char *cdst = cart_base_ptr + fill_txn_cart_off;

                /* r72: cart-write critical section â VBlank-IN masked
                 * (p03_06) across the write burst and its verify. */
                Saturn_CartWriteBegin();
                {
                    /* v5.13 staging tripwire: the batch was issued into
                     * the cached stage; if the cached and uncached
                     * views of its first longword disagree, SGL DMA'd
                     * behind the cache â demote to the uncached mirrors
                     * permanently. The stage view below picks the
                     * fresh side up immediately, so even the
                     * just-landed batch copies correctly. */
                    if (!saturn_stage_probe((unsigned int)saturn_fill_stage))
                        saturn_stage_trip_now();
                    const volatile unsigned int *src_v =
                        (const volatile unsigned int *)
                            saturn_stage_view();
                    volatile unsigned int *cart_v =
                        (volatile unsigned int *)(0x40000000u |
                            (0x0FFFFFFFu & (unsigned int)cdst));
                    unsigned int j;
                    unsigned int nw = ((unsigned int)got) >> 2;
                    int bad = (Saturn_DspCartCopy2K(cdst,
                                  (const void *)SATURN_STAGE_UNC)
                              != 0);

                    /* Only the landed bytes are truth: got may be < 2 KB
                     * on a partial batch, and beyond it both the stage
                     * and the cart hold stale data. */
                    for (j = 0; bad == 0 && j < nw; j++)
                        if (cart_v[j] != src_v[j]) { bad = 1; break; }
                    saturn_dsp_verify_kb += 2;
                    if (bad)
                    {
                        saturn_dsp_verify_bad++;
                        memcpy(cdst, (const void *)saturn_stage_view(),
                               (unsigned int)got);
                    }
                }
                Saturn_CartWriteEnd();
                saturn_cart_kb += 2;
                bytes  += (unsigned int)got;
                cursor  = fill_part_cd;   /* contiguity check stays true */
                copied  = 1;
                saturn_fill_progress = fill_part_cart >> 10;   /* r65 */
                saturn_fill_beat++;
                if (fill_part_left == 0)
                {
                    saturn_fill_head++;
                    entries++;
                }
                continue;   /* budget may still allow the next batch */
            }
            if (got < 0)
                fill_nw_shrink();   /* smaller batch, or sync from now on */
            break;   /* still pending, or retry this chunk sync next frame */
        }

        {
            unsigned int chunk = fill_part_left;
            unsigned int head  = fill_part_cd & 2047u;
            int          async_ok = (saturn_direct_reads_ok && head == 0 &&
                                     chunk >= 2048u &&
                                     !saturn_fill_boot_mode
#if defined(SATURN_FILL_SYNC_ONLY) && SATURN_FILL_SYNC_ONLY == 1
                                     && 0   /* FILL-SYNC BISECT (2026-09-05):
                                             * r103/r105 wedge repro — drain
                                             * the background cart fill
                                             * through the sync copy path
                                             * only, no GFS_NwFread batches.
                                             * Diagnostic boot; remove. */
#endif
            );

            if (async_ok)
            {
                /* The async batch is NOT bounded by the per-frame byte
                 * cap: it streams across frames on the SH-1 (512 KB CD
                 * cache absorbs the read-ahead), so size it for CD
                 * efficiency â€” the whole entry remainder, batch-capped. */
                if (chunk > fill_nw_batch)
                    chunk = fill_nw_batch;
                chunk &= ~2047u;   /* whole sectors only; the <2 KB tail
                                    * (if any) drains through the sync
                                    * path on a later iteration */

                /* Position the handle at the batch's first sector with
                 * one relative forward seek (Jo's proven pattern; the
                 * queue is in cd order so the target only moves
                 * forward). Backward target or refused seek: fall to
                 * the sync path, which owns repositioning and wedge
                 * recovery. */
                if (saturn_gfs_pos > (fill_part_cd >> 11))
                    async_ok = 0;
                else if (saturn_gfs_pos != (fill_part_cd >> 11))
                {
                    if (GFS_Seek(saturn_wad_gfs,
                                 (Sint32)((fill_part_cd >> 11) - saturn_gfs_pos),
                                 GFS_SEEK_CUR) < 0)
                        async_ok = 0;
                    else
                        saturn_gfs_pos = (fill_part_cd >> 11);
                }
                if (async_ok && fill_txn_issue(chunk) < 0)
                {
                    fill_nw_shrink();
                    break;   /* retry this chunk through sync next frame */
                }
                if (async_ok)
                {
                    copied = 1;   /* bytes counted when the pump completes */
                    continue;
                }
                /* fall through: sync copy of the same chunk */
            }

            /* Sync path (unchanged): unaligned heads (stop at the sector
             * boundary so the remainder starts aligned), <2 KB tails,
             * and the fallback after any async failure. This work IS
             * synchronous, so the per-frame byte cap applies. */
            if (head && chunk > 2048u - head)
                chunk = 2048u - head;
            if (chunk > FILL_FRAME_BYTES - bytes)
                chunk = FILL_FRAME_BYTES - bytes;
            if (chunk > FILL_CHUNK_BYTES)
                chunk = FILL_CHUNK_BYTES;
            if (chunk > FILL_FRAME_BYTES - bytes)
                chunk = FILL_FRAME_BYTES - bytes;
            if (saturn_copy_to_cart(fill_part_cd, fill_part_cart, chunk) < 0)
            {
                /* Permanent copy failure â€” stop the queue to avoid
                 * spinning. Negative: caller keeps its counters. */
                saturn_fill_active = 0;
                saturn_fill_remaining = 0;
                return -entries;
            }
            cursor          = fill_part_cd + chunk;
            fill_part_cd   += chunk;
            fill_part_cart += chunk;
            fill_part_left -= chunk;
            bytes  += chunk;
            copied  = 1;
            saturn_fill_progress = fill_part_cart >> 10;   /* r65 */
            saturn_fill_beat++;
            if (fill_part_left == 0)
            {
                saturn_fill_head++;
                entries++;
            }
        }
    }
    }

    saturn_fill_remaining = saturn_fill_count - saturn_fill_head;
    /* Cart is on A-bus, not cached by SH-2 â€” no OC purge needed. But
     * reads stage through the SH-1 CD block; flush SQ once per frame
     * so any pending writes to the cart A-bus control regs commit. */
    saturn_sq_flush();
    /* Once we've drained everything, disable the queue so subsequent
     * frames return immediately. */
    if (saturn_fill_head >= saturn_fill_count && fill_part_left == 0)
        saturn_fill_active = 0;
    return entries;
}

/* ====== Hot-fill state (queue-indexed lookup) ====== */

/* The fill queue is ordered by cart_off (placements are packed
 * back-to-back in directory order), so binary search finds the entry
 * that owns a given cart offset. Returns the queue index or -1. */
static int fill_queue_index_for(unsigned int cart_off)
{
    int lo = 0, hi = saturn_fill_count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        unsigned int v = saturn_fill_queue[mid].cart_off;
        if (v == cart_off)
            return mid;
        if (v < cart_off)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* Is the hot lump placed at cart offset `pos` already in cart?
 * Filled = synced at boot, or already drained by the background
 * filler (index < head). When not yet filled, *cd_off_out receives
 * the on-disc offset so the caller can stream the lump from CD. */
static int hot_lump_filled(unsigned int pos, unsigned int *cd_off_out)
{
    int idx = fill_queue_index_for(pos);
    if (idx < 0)
        return 0;
    if (idx < saturn_fill_head || (saturn_fill_queue[idx].lump_index & FILL_SYNCED))
        return 1;
    if (cd_off_out)
        *cd_off_out = saturn_fill_queue[idx].cd_off;
    return 0;
}

/* ---- big-patch source dispatch (2026-09-06 title stripes) ----
 * After the boot classifier rewrites the in-memory directory, filepos
 * is a CART offset (raw, no bias) for hot lumps and a DISC offset |
 * COLD_BIAS for cold ones. Saturn_CachePatchBig used to strip the bias
 * and treat every filepos as a disc offset â€" for TITLEPIC that read
 * the cart offset 0x054654 as a CD position and rendered stripes of
 * boot-directory garbage. This names the real source for the caller:
 * returns 1 when the lump's bytes are resident in cart NOW
 * (*cart_ptr_out = source pointer), 0 when they must stream from CD
 * (*cd_off_out = on-disc offset: biased -> stripped; hot-but-unfilled
 * -> the fill queue's saved cd_off, mirroring I_ReadCDFile). */
int Saturn_LumpResident(int lump, const unsigned char **cart_ptr_out,
                        unsigned int *cd_off_out)
{
    lumpinfo_t  *l   = &wadfile[0].lumpinfo[lump - wadfile[0].firstlump];
    unsigned int pos = (unsigned)l->filepos;

    if (!(pos & COLD_BIAS) && cart_initialized && hot_lump_filled(pos, NULL))
    {
        *cart_ptr_out = (const unsigned char *)cart_base_ptr + pos;
        return 1;
    }
    if (pos & COLD_BIAS)
        *cd_off_out = pos & ~COLD_BIAS;
    else
    {
        *cd_off_out = pos;   /* unreachable: hot placements are queued */
        hot_lump_filled(pos, cd_off_out);
    }
    return 0;
}

/* Open IWAD by name. d32xr's W_InitCDPWAD calls this with name="SOUNDS.WAD";
 * we don't have SOUNDS.WAD on disc, so we return -1. */
int I_OpenCDFileByName(const char *name, int *poffset)
{
    if (poffset) *poffset = 0;
    if (name && name[0] != '\0')
        return -1;
    return saturn_wad_open ? (int)saturn_wad_size : -1;
}

void I_OpenCDFileByOffset(int length, int offset)
{
    (void)length; (void)offset;
}

int I_SeekCDFile(int offset, int whence)
{
    if (whence == 0)
    {
        saturn_cd_seek_pos = offset;
        return 0;
    }
    return -1;
}

/* Phase-4 profile counter: synchronous CD lump reads. Each one is a
 * GFS seek + sector staging read (dozens of ms); d_main.c reports the
 * per-displayed-frame delta on the HUD. */
int saturn_wad_cold_reads = 0;

int I_ReadCDFile(int length)
{
    unsigned int pos     = (unsigned int)saturn_cd_seek_pos;
    unsigned int real_cd = 0;
    int          from_cart = 0;

    saturn_cd_pre_read_pos = (int)pos;

    if (length <= 0)
    {
        /* Marker/zero-size lump: nothing to read; just advance. */
        saturn_cd_seek_pos = (int)(pos + (unsigned)length);
        return length;
    }

    if (pos & COLD_BIAS)
    {
        /* Explicitly COLD lump: strip the bias for the on-disc offset. */
        real_cd = pos & ~COLD_BIAS;
    }
    else if (!cart_initialized)
    {
        /* No cart at all: the un-biased offset IS the on-disc offset
         * (every lump was biased COLD in that mode, so this shouldn't
         * happen â€” but be safe). */
        real_cd = pos;
    }
    else if (hot_lump_filled(pos, &real_cd))
    {
        /* HOT lump whose cart bytes are actually present. */
        from_cart = 1;
    }
    /* else: hot placement not yet filled â€” real_cd now holds the
     * on-disc offset; fall through and stream it from CD so the caller
     * never sees stale cart DRAM (this is what keeps TEXTURE1/2 and
     * early patch reads correct during the ~10 s background fill). */

    if (from_cart)
    {
        saturn_cd_seek_pos = (int)(pos + (unsigned)length);
        return length;
    }
    saturn_wad_cold_reads++;    /* profile: this read hit the CD */
    if ((unsigned)length > sizeof(saturn_cd_scratch))
    {
        I_Error("I_ReadCDFile: lump of %d bytes exceeds %u-byte scratch",
                length, (unsigned)sizeof(saturn_cd_scratch));
        return -1;
    }
    if (saturn_iwad_pread(real_cd, saturn_cd_scratch, (unsigned)length) == length)
    {
        /* saturn_iwad_pread stages each sector through saturn_sector_buf
         * before memcpy'ing into saturn_cd_scratch. The r71 "purge OC
         * anyway" belt-and-braces is GONE: saturn_oc_purge() is a
         * permanent no-op (any CCR write wedges real HW), and coherence
         * is owned by the v5.13 staging tripwire in
         * saturn_gfs_read_sector â€” sync GFS is TMODE_CPU (an SH-2 CPU
         * copy), so the cached stage is coherent by construction, and
         * any DMA-behind-cache surprise demotes the stage views to the
         * uncached mirrors permanently. */
        saturn_cd_seek_pos = (int)(pos + (unsigned)length);
        return length;
    }
    return -1;
}

void *I_GetCDFileBuffer(void)
{
    unsigned int pos = (unsigned int)saturn_cd_pre_read_pos;
    if (!(pos & COLD_BIAS) && cart_initialized && hot_lump_filled(pos, NULL))
        return cart_base_ptr + pos;
    return saturn_cd_scratch;
}

/* ====== Stable lump pointers for callers that KEEP them ====== */

/* d32xr's W_POINTLUMPNUM (R_CheckPixels) is only safe to hold past the
 * next WAD read when the lump's bytes live in cart DRAM: our
 * I_GetCDFileBuffer returns the SHARED 64 KB saturn_cd_scratch for
 * every cold lump, so long-lived pointers (sky, per-texture and per-flat
 * pixel data in r_main.c / p_setup.c) would all alias each other and
 * whatever the LAST CD read left in the scratch. This is the
 * persistent accessor for such callers:
 *   - cart-resident lump: the zero-copy cart pointer, already stable;
 *   - cold lump: a PU_LEVEL Z_Malloc'd copy â€” the uncompressed bytes
 *     the probe just streamed into the scratch are memcpy'd out (no
 *     second CD read); compressed lumps go through W_ReadLump so the
 *     copy holds the DECODED bytes. Freed with the rest of the level
 *     allocations by Z_FreeTags at the next level load;
 *   - zone too full for the copy: the aliased scratch pointer (the old
 *     behavior) rather than failing the level setup. */
void *I_PointStableLump(int lump)
{
	void	*raw;
	void	*data;
	int		length;

	raw = W_POINTLUMPNUM(lump);	/* cart bytes, or fresh bytes in scratch */
	if (raw != saturn_cd_scratch)
		return raw;			/* stable: cart DRAM or in-RAM wad */

	length = W_LumpLength(lump);
	if (length <= 0)
		return raw;			/* marker/empty lump: nothing to alias */

	if (Z_LargestFreeBlock(mainzone) < length + 64)
		return raw;			/* zone full: alias beats a dead level */

	data = Z_Malloc(length, PU_LEVEL);
	if (W_IsCompressed(lump))
		W_ReadLump(lump, data);		/* copy must hold decoded bytes */
	else
		memcpy(data, raw, length);	/* bytes are already in scratch */
	return data;
}

/* ======================================================================
 * Flight-recorder persistence in the Saturn's INTERNAL BACKUP RAM.
 *
 * The red-screen watchdog's snapshot used to live in the cart's top
 * 512 bytes â€” which the SAROO boot menu reinitializes on the next
 * reset, losing the verdict. The BUP library (BIOS vectors at
 * 0x06000350, see SEGA_BUP.H) reaches the battery-backed 32 KB
 * internal RAM in supervisor mode; on a SAROO the emulated BUP stores
 * the file on the cart itself (tpunix/SAROO Firm_Saturn/game_save.c:
 * dev 0 = internal unit, name-matched, 128-byte blocks, >=0x2C-byte
 * work area). Sega ST-162-R1-092994 sizes for the REAL BIOS: 16 KB
 * library expansion area + 8192-byte work area â€” both from the zone
 * at first use (BSS headroom is ~2.8 KB).
 * ==================================================================== */
#include <SEGA_BUP.H>

#define BUP_REC_NAME   "DSREC1"
#define BUP_REC_WORDS  128u    /* 512 bytes, same layout as the cart stash */

static int     saturn_bup_state = -1;  /* -1 untried, 0 dead, 1 ok */
static Uint32 *saturn_bup_lib;         /* 16 KB library load area  */
static Uint32 *saturn_bup_work;        /* 8 KB work area           */

/* Round-34 REAL-HW FIX: allocation ONLY. The old Saturn_BupPrealloc
 * called the full Saturn_BupInit at boot â€” BUP_Init downloads a 16 KB
 * library to the CD-block SH-1 and BUP_Stat runs there, i.e. CD-block
 * traffic issued WHILE the boot's WAD reads stream. On real HW that
 * wedged the CD block: 2026-09-04, boot stuck forever on the cyan
 * LOADING WAD stage (Kronos emulates the block and never showed it).
 * The CD-block half stays in Saturn_BupInit, which now runs lazily
 * from Save/Load â€” every call site is behind a !Saturn_CdFillBusy()
 * gate. */
void Saturn_BupPrealloc(void)
{
    if (saturn_bup_lib != 0 && saturn_bup_work != 0)
        return;
    saturn_bup_lib  = (Uint32 *)Z_Malloc(0x4000, PU_STATIC);
    saturn_bup_work = (Uint32 *)Z_Malloc(0x2000, PU_STATIC);
    if (!saturn_bup_lib || !saturn_bup_work)
    {
        if (saturn_bup_lib)
            Z_Free(saturn_bup_lib);
        if (saturn_bup_work)
            Z_Free(saturn_bup_work);
        saturn_bup_lib = saturn_bup_work = 0;
        /* zone pressure is transient: the lazy path retries */
    }
}

static int Saturn_BupInit(void)
{
    BupConfig cfg[3];
    BupStat   st;

    if (saturn_bup_state >= 0)
        return saturn_bup_state;
    saturn_bup_state = 0;
    if (!saturn_bup_lib || !saturn_bup_work)
        Saturn_BupPrealloc();      /* boot prealloc missed: try now */
    if (!saturn_bup_lib || !saturn_bup_work)
    {
        saturn_bup_state = -1;     /* still no room: RETRY later */
        return 0;
    }
    cfg[0].unit_id   = BUP_MAIN_UNIT;
    cfg[0].partition = 1;
    /* NO BUP_CURTRIDGE entry: the SAROO sits in the cart slot and is
     * not a backup-RAM cart â€” probing it as one risks an CS2-bus hang
     * (round-11c). Zero terminator right after the internal unit. */
    cfg[1].unit_id   = 0;
    cfg[1].partition = 0;
    cfg[2].unit_id   = 0;
    cfg[2].partition = 0;
    BUP_Init(saturn_bup_lib, saturn_bup_work, cfg);
    /* Device 0 = the cfg[0] entry = internal unit. A failed stat
     * leaves the flag at 0 and the cart stash stays the fallback. */
    if (BUP_Stat(0, 0, &st) < 0)
        return 0;
    saturn_bup_state = 1;
    return 1;
}

/* Stash `nwords` little-endian words as backup-RAM file "DSREC1"
 * (512-byte record, zero-padded â€” same layout as the cart stash).
 * Returns 0 written / negative on any failure. */
int Saturn_BupSave(const unsigned int *words, int nwords)
{
    static unsigned int buf[BUP_REC_WORDS];  /* shared save/load scratch */
    BupDir dir;
    unsigned int k;

    if (!Saturn_BupInit())
        return -1;
    for (k = 0; k < BUP_REC_WORDS; k++)
        buf[k] = 0;
    for (k = 0; k < (unsigned)nwords && k < BUP_REC_WORDS; k++)
        buf[k] = words[k];

    D_memset(&dir, 0, sizeof(dir));
    D_memcpy(dir.filename, BUP_REC_NAME, 6);
    D_memcpy(dir.comment, "redscreen", 9);
    dir.language = BUP_ENGLISH;
    dir.date     = 0;
    dir.datasize = BUP_REC_WORDS * 4u;
    dir.blocksize = 4;               /* 512 / 128 (internal block size) */
    return BUP_Write(0, &dir, (Uint8 *)buf, 0);
}

/* Read + DELETE the recorder file (one boot per record, like the cart
 * stash). Returns 0 and fills min(nwords,128) words on success. */
int Saturn_BupLoad(unsigned int *words, int nwords)
{
    static unsigned int buf[BUP_REC_WORDS];
    int ret;
    int k;

    if (!Saturn_BupInit())
        return -1;
    ret = BUP_Read(0, (Uint8 *)BUP_REC_NAME, (Uint8 *)buf);
    if (ret != 0)
        return -1;          /* NOT_FOUND or a dead device: no record */
    BUP_Delete(0, (Uint8 *)BUP_REC_NAME);
    for (k = 0; k < (unsigned)nwords && k < BUP_REC_WORDS; k++)
        words[k] = buf[k];
    return 0;
}
/* ====== P4: one-shot small-file GFS read (E1M1 PVS blob) ====== */

/* Synchronous one-shot read of cd\E1M1.BLOB into buf (cap bytes).
 * Main thread only (P_SetupLevel) — the VBlank-ISR GFS ban does not
 * apply here. Separate GfsHn/fid path — never touches the persistent
 * IWAD handle or its sector-walk position. Filename is BARE 8.3
 * (GFS_NameToId matches the ISO root directory table — see
 * saturn_open_iwad above). Returns bytes read, 0 on any failure
 * (file absent, size > cap, short read) so the caller fail-opens to
 * the ungated BSP walk. Mirrors jo_engine/fs.c's proven
 * NameToId -> GetFileSize -> GFS_Load(fid) pattern. */
int Saturn_Pvs_BlobRead(void *buf, unsigned cap)
{
    Sint32  fid, ssz, nsct, lsz, fsize;
    GfsHn   gfs;
    int     retry;

    fid = GFS_NameToId((Sint8 *)"E1M1.BLOB");
    if (fid < 0)
        return 0;
    gfs = GFS_Open(fid);
    if (gfs == JO_NULL)
        return 0;
    GFS_GetFileSize(gfs, &ssz, &nsct, &lsz);
    GFS_Close(gfs);
    if (ssz <= 0 || nsct <= 0 || lsz < 0)
        return 0;
    fsize = ssz * (nsct - 1) + lsz;
    if (fsize <= 0 || (unsigned)fsize > cap)
        return 0;
    for (retry = 10; GFS_Load(fid, 0, (void *)buf, fsize) <= 0; --retry)
    {
        if (!retry)
            return 0;
        jo_spin_wait(16);
    }
    return (int)fsize;
}

/* ====== M2: one-shot small-file GFS read (LEAF blob, named 8.3) ====== */

/* Same one-shot pattern as Saturn_Pvs_BlobRead above, parameterized on
 * the filename so the leaf loader stays map-generic ("E1M1.LEAF" from
 * Saturn_LeafLevelLoad). Main thread only (P_SetupLevel). Returns
 * bytes read, 0 on any failure so the caller fail-opens to the plain
 * seg walk. */
volatile int dbg_leaf_fid = -999;
volatile int dbg_leaf_err = 0;

int Saturn_Leaf_BlobRead(void *buf, unsigned cap, const char *fname)
{
    Sint32  fid, ssz, nsct, lsz, fsize;
    GfsHn   gfs;
    int     retry;

    fid = GFS_NameToId((Sint8 *)fname);
    dbg_leaf_fid = (int)fid;
    if (fid < 0)
    {
        dbg_leaf_err = 1;
        return 0;
    }
    gfs = GFS_Open(fid);
    if (gfs == JO_NULL)
    {
        dbg_leaf_err = 2;
        return 0;
    }
    GFS_GetFileSize(gfs, &ssz, &nsct, &lsz);
    if (ssz <= 0 || nsct <= 0 || lsz < 0)
    {
        GFS_Close(gfs);
        dbg_leaf_err = 3;
        return 0;
    }
    fsize = ssz * (nsct - 1) + lsz;
    if (fsize <= 0)
    {
        GFS_Close(gfs);
        dbg_leaf_err = 4;
        return 0;
    }
    if (buf == NULL)
    {
        GFS_Close(gfs);
        return (int)fsize;
    }
    if ((unsigned)fsize > cap)
    {
        GFS_Close(gfs);
        dbg_leaf_err = 5;
        return 0;
    }
    {
        static byte leaf_chunk_buf[2048] __attribute__((aligned(16)));
#ifndef SATURN_WIN32
        /* Write through uncached mirror (0x20000000 bit set) so CD data lands
         * directly and synchronously in physical DRAM/SDRAM, visible to both CPUs */
        byte *dst = (byte *)((uint32_t)buf | 0x20000000u);
#else
        byte *dst = (byte *)buf;
#endif
        uint32_t remaining = (uint32_t)fsize;

        GFS_SetTmode(gfs, GFS_TMODE_CPU);
        GFS_SetReadPara(gfs, 8);
        GFS_SetTransPara(gfs, 8);

        while (remaining > 0)
        {
            uint32_t chunk = (remaining > 2048u) ? 2048u : remaining;

            for (retry = 10; GFS_Fread(gfs, 1, (void *)leaf_chunk_buf, 2048) <= 0; --retry)
            {
                if (!retry)
                {
                    GFS_Close(gfs);
                    dbg_leaf_err = 6;
                    return 0;
                }
                jo_spin_wait(16);
            }
            D_memcpy(dst, leaf_chunk_buf, chunk);
            dst += chunk;
            remaining -= chunk;
        }
        GFS_Close(gfs);
#ifndef SATURN_WIN32
        /* Purge Master L1 Cache from uncached Area 1 trampoline so subsequent cached reads fetch fresh DRAM */
        extern void _hw_purge_cache(void);
        ((void (*)(void))((uint32_t)_hw_purge_cache | 0x20000000u))();
#endif
        dbg_leaf_err = 7;
        return (int)fsize;
    }
}
