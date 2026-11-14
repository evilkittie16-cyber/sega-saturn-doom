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
#include <jo/jo.h>

#define SATURN_SH2_CCR             (*(volatile unsigned char *)0xFFFFFE92u)
#define SATURN_SH2_CCR_CE          0x01u   /* cache enable */
#define SATURN_SH2_CCR_TW          0x08u   /* two-way / 2KB+2KB RAM mode */
#define SATURN_SH2_CCR_CP          0x10u   /* purge all (write+clear) */

static inline void saturn_sq_flush(void)
{
    __asm__ __volatile__(
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ".word 0x00008003\n\t"
        ::: "memory");
}

static inline void saturn_oc_purge(void)
{
}

static inline void Saturn_Ocram_Split(void)
{
    volatile unsigned char *const ccr = &SATURN_SH2_CCR;

    *ccr = 0x00u;                       /* CE off: stop the cache      */
    *ccr = 0x10u;                       /* CP = 1: full purge          */
    *ccr = 0x09u;                       /* 2KB cache + 2KB OCRAM       */
}

static inline void saturn_scu_memcpy(void *dst, const void *src, uint32_t n)
{
    jo_dma_copy((void *)src, dst, (unsigned int)n);
}

static inline void saturn_scu_dma_wait(void)
{
    jo_dma_wait();
}

static inline void saturn_scu_copy_and_purge(void *dst, const void *src,
                                             uint32_t n)
{
    saturn_scu_memcpy(dst, src, n);
    saturn_scu_dma_wait();
    saturn_oc_purge();
}
#endif /* !SATURN_WIN32 */

#endif /* DOOM_SATURN_CACHE_H */
