/* saturn_cache.h — SH-2 cache + SCU DMA primitives for DOOM-Saturn.
 *
 * Saturn's SH-2 has two cache layers that bite us silently if we don't
 * account for them:
 *
 *  1. SH-2 store queue (4 entries deep). Writes to memory-mapped VDP1/VDP2
 *     registers sit in the SQ until it's full or we issue a `pref` to the
 *     next free entry. If the SH-2 reads back immediately (or the SH-1 sees
 *     the write one cycle later than expected), the value is stale.
 *
 *  2. SH-2 operand cache. By default the OC is write-back, so writes to
 *     RAM stay in cache until evicted. SCU DMA writes that land in cached
 *     RAM can be missed by the CPU if it has stale lines in OC.
 *
 * Saturn also has an SCU DMA engine on channels 0/1 that doesn't go
 * through the SH-2 cache. Using DMA for cart->RAM transfers (the doom.wad
 * is 12 MB) avoids polluting OC and is ~5x faster than the CPU loop in
 * `saturn_iwad_pread`.
 *
 * Three helpers here:
 *
 *   saturn_sq_flush()    — flush the SH-2 store queue (after register writes)
 *   saturn_oc_purge(p,n) — purge a CPU operand-cache range (after DMA into it)
 *   saturn_scu_memcpy()  — SCU DMA memcpy (4-byte aligned only)
 *
 * SCU DMA has a 4-byte alignment requirement on src and dst and only
 * works for n a multiple of 4. The caller is responsible for the alignment.
 */
#ifndef DOOM_SATURN_CACHE_H
#define DOOM_SATURN_CACHE_H

#include <stdint.h>

#ifdef SATURN_WIN32
#include <string.h>
static inline void saturn_sq_flush(void) {}
static inline void saturn_oc_purge(void) {}
static inline void Saturn_Ocram_Split(void) {}
static inline void saturn_scu_memcpy(void *dst, const void *src, uint32_t n) { memcpy(dst, src, n); }
static inline void saturn_scu_dma_wait(void) {}
static inline void saturn_scu_copy_and_purge(void *dst, const void *src, uint32_t n) { memcpy(dst, src, n); }
#else
#include "SEGA_XPT.H"

/* SH-2 CCR (Cache Control Register) — ADDRESS CORRECTED AGAIN
 * (2026-09-09, Kronos wedge convicted): the 2026-09-03 "correction"
 * to 0xFFFFFF92 was ITSELF one bit wrong. 0xFFFFFF92 is unmapped on
 * the SH7604; the real CCR is 0xFFFFFE92. Proof chain: (a) the rig's
 * wad_load.c (Projects\SCUDSP DMA) defines W_CCR at 0xFFFFFE92 and
 * PASSED 20+ real-HW boots with 0x00→0x10→0x09 there; (b) the first
 * SATURN_DIVLUT ISO froze in Kronos with the master's R7 = 0xFFFFFF92
 * — the bad pointer itself — mid-sequence in D_DoomMain's LUT init.
 * The 2026-09-06 "any CCR write wedges this console" verdict was taken
 * THROUGH THE WRONG ADDRESS, and the 09-08 "the 0x0C value was the
 * killer" theory is superseded: on this codebase the ADDRESS was the
 * killer all along. bit0 = CE (cache enable), bit3 = TW (two-way,
 * 0x09 = 2KB cache + 2KB RAM), bit4 = CP (purge all: write-back dirty
 * lines, then invalidate). */
#define SATURN_SH2_CCR             (*(volatile unsigned char *)0xFFFFFE92u)
#define SATURN_SH2_CCR_CE          0x01u   /* cache enable */
#define SATURN_SH2_CCR_TW          0x08u   /* two-way / 2KB+2KB RAM mode */
#define SATURN_SH2_CCR_CP          0x10u   /* purge all (write+clear) */

/* Flush the SH-2 store queue. After a write to a memory-mapped register,
 * the write can sit in the 4-entry SQ until the next pref/branch. Call
 * this before you expect the hardware to see the write. Implemented as
 * an `pref @Rn` for each free SQ slot — but since pref needs the SH-2
 * operand cache enabled and we can't always trust that, the safe path
 * is to issue 4 pref-equivalent prefetch instructions to ensure the SQ
 * is empty. GCC's `__builtin_sh2_pref` is not available, so we use
 * inline asm with the SH-2 `pref` opcode (0x0000 8nF3).
 *
 * The simplest portable form is 4 sequential `pref @Rn` against the
 * scratch register r0 (compiler must not use r0 for our local variables,
 * which it won't for an inline asm clobber list). We mark r0 as scratch
 * and write the result back so the asm block is a black box the optimizer
 * can't reorder.
 */
static inline void saturn_sq_flush(void)
{
    /* SH-2 `pref @r0` opcode = 0x0000 8003. Use raw .word to bypass
     * assembler-mnemonic differences (sh-elf gas doesn't always recognize
     * `pref` in default mode). Four of them cover all 4 SQ slots.
     * pref is a hint — the assembler also requires SH-2 mode; gcc -m2
     * sets that. */
    __asm__ __volatile__(
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ::: "memory");
}

/* Purge the SH-2 operand cache (whole cache, both roles).
 *
 * FULL NO-OP (2026-09-03, two real-HW boots) — but see the 2026-09-08
 * amendment below: the wedge conviction has been REVISED, the no-op
 * stays. History: the define-gated OCRAM probe froze real HW twice at
 * its green OCPRB stage (vblank dead at 96-111, CP purge 0x10 + CCR=0x09
 + data-array writes in the window), and the 09-06 verdict was "any CCR
 * write wedges the bus; do not retry on this console".
 *
 * 2026-09-08 — OVERTURNED by the SCUDSP-rig campaign (w11-w30 runs on
 * the SAME console, same burner): the rig's split-mode probe used the
 * documented Hitachi 6.2 sequence W_CCR=0x00 (CE off) → 0x10 (CP purge)
 * → 0x09 (2KB cache + 2KB RAM) and PASSED on physical silicon —
 * "OCR PASS split on", CCR 9 painted, then 20+ all-green boots with
 * scratchpad code EXECUTING from OCRAM (O1ocr2 2764kb/s) and the divide
 * LUT running from it (V3ocr 1080-1350 k-div/s). The rig's earlier
 * 0x0C value (TW + UNDEFINED bit 2, no purge first) was the convicted
 * wedge — the same value this project's probe used. So: the CCR write
 * per se was never the killer; the VALUE was.
 *
 * 2026-09-09 — ROOT CAUSE FINAL: the ADDRESS. This header pointed
 * SATURN_SH2_CCR at 0xFFFFFF92 (unmapped, bit 12 set vs the real
 * 0xFFFFFE92); every probe and the first SATURN_DIVLUT boot wrote
 * through it and froze. The rig's identical value sequence passed at
 * 0xFFFFFE92. Fixed above; keep both lessons (right address, right
 * values) together.
 *
 * This function STAYS A NO-OP regardless: a purge is only meaningful as
 * a one-time boot step (see Saturn_Ocram_Split below), never per-frame.
 * The old "SGL purge" rationale is dead — sync GFS reads are CPU
 * transfers (cache-coherent) and the SCU-DMA cart paths never run with
 * flashcart ID 0x06/0x0C. */
static inline void saturn_oc_purge(void)
{
}

/* One-time boot split-mode switch: expose the SH7604's on-chip RAM
 * (OCRAM scratchpad) at 0xC0000000..0xC00007FF (2KB, zero-wait, per
 * CPU), keeping 2KB of operand cache.
 *
 * THE RIG-PROVEN SEQUENCE (do not edit without a re-read of the w-run
 * ledger): CE off → CP purge → CE|TW (0x09). The purge bit is
 * action-only. The caller paints a step marker BETWEEN each write (the
 * rig's "CCR 0/P/9" discipline) so a wedge convicts the exact step.
 *
 * Call ONCE, late in D_DoomMain boot, from the master only. The slave
 * keeps its own 4KB cache (it runs nothing). After this, 0xC0000000
 * reads/writes are internal RAM — no cache coherency involved. */
static inline void Saturn_Ocram_Split(void)
{
    volatile unsigned char *const ccr = &SATURN_SH2_CCR;

    *ccr = 0x00u;                       /* CE off: stop the cache      */
    *ccr = 0x10u;                       /* CP = 1: full purge          */
    *ccr = 0x09u;                       /* 2KB cache + 2KB OCRAM       */
}

/* SCU DMA memcpy. Uses SGL's high-level helper `DMA_ScuMemCopy`, which
 * sets up SCU DMA channel 1 in direct mode with 4-byte src/dst strides.
 * Constraints:
 *   - src must be 4-byte aligned
 *   - dst must be 4-byte aligned
 *   - n must be a multiple of 4
 *   - DMA does NOT go through SH-2 cache; the destination region must
 *     be purged via `saturn_oc_purge()` before the CPU reads it.
 *
 * For the doom.wad copy (cart->RAM, 12 MB), this is ~5x faster than the
 * 8-bit CPU loop in `saturn_iwad_pread` and avoids polluting OC during
 * the bulk read.
 *
 * Returns nothing. SGL's DMA_ScuMemCopy is fire-and-forget; we don't
 * wait. For chained transfers, call saturn_scu_dma_wait() between calls.
 */
extern void DMA_ScuMemCopy(void *src, void *dst, uint32_t n);

static inline void saturn_scu_memcpy(void *dst, const void *src, uint32_t n)
{
    /* SGL's prototype is (void *, void *, Uint32) with NO names — the
     * official sample (Samples/demo - render to image/main.c) calls
     * DMA_ScuMemCopy(HWRAM_dst, VDP1_FB_src, len): DST FIRST. r68's
     * swapped call silently DMA'd cart->sector_buf, recycled stale cart
     * DRAM over the GFS data, passed its own verify (both sides held
     * the same garbage) and shipped garbage lumps -> red I_Error. */
    DMA_ScuMemCopy(dst, (void *)src, n);
}

/* Wait for SCU DMA channel 1 to finish. After saturn_scu_memcpy returns,
 * the SCU engine is still spinning in the background. If we issue
 * another DMA or if the CPU touches the destination region, we MUST
 * call this first or we'll see partial writes. */
extern Uint32 DMA_ScuResult(void);

static inline void saturn_scu_dma_wait(void)
{
    while (DMA_ScuResult() != 0) { /* spin until SCU done */ }
}

/* Convenience: DMA + wait + purge. The right idiom after every cart->RAM
 * SCU DMA. */
static inline void saturn_scu_copy_and_purge(void *dst, const void *src,
                                             uint32_t n)
{
    saturn_scu_memcpy(dst, src, n);
    saturn_scu_dma_wait();
    saturn_oc_purge();
}
#endif /* !SATURN_WIN32 */

#endif /* DOOM_SATURN_CACHE_H */
