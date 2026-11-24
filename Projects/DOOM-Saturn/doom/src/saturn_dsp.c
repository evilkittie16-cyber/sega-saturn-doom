/* saturn_dsp.c — SCU A-Bus Cart-Write Critical Section & DSP Stubs.
 *
 * The SCU DSP cart-copy engine is permanently disabled by policy (v5.9b real-HW
 * verdict: multi-pair DSP DMA chains exhibit heavy-tail stalls on real silicon,
 * whereas CPU memcpy through the uncached mirror inside Saturn_CartWriteBegin/End
 * is faster and 100% deterministic).
 *
 * This module retains Saturn_CartWriteBegin / Saturn_CartWriteEnd (which hold
 * off all interrupts at the CPU during A-bus cart writes) and the stubbed DSP
 * status accessors queried by main.c, i_wad.c, and i_video.c.
 */

#include <jo/jo.h>

#include "doomdef.h"
#include "saturn_dsp.h"

int saturn_dsp_ramin = -1;
int saturn_dsp_zero  = -1;
int saturn_dsp_match = -1;
int saturn_dsp_ppaf  = -1;
int saturn_dsp_str2  = -1;
int saturn_dsp_str4  = -1;

int Saturn_DspProbe(void)
{
    return -1;
}

int Saturn_DspCartInit(void)
{
    return 0;
}

int Saturn_DspCartCopy2K(void *cart_dst, const void *wram_src)
{
    (void)cart_dst;
    (void)wram_src;
    return -1;
}

int Saturn_DspCartOk(void)
{
    return 0;
}

/* Cart-write critical section: all interrupts are held off at the CPU
 * (SR interrupt mask) for the duration of one cart write burst. A VBlank
 * that happens meanwhile stays pending in the SCU and is taken as soon
 * as Saturn_CartWriteEnd restores SR.
 *
 * Why not the SCU mask register (IMS, 0x25FE00A0), as before:
 *   - the old code accessed it as a 16-bit word at 0x25FE00A0, which on
 *     the big-endian SH-2 is bits 31-16: it never masked VBlank-IN (bit 0);
 *   - IMS cannot be relied on for reading: jo and the BIOS each keep
 *     their own copy of the mask (jo_core_scu_mask, 0x06000348) for that
 *     reason, so "read, OR, write back" restores an unknown value;
 *   - jo's interrupt handlers (VBlank-IN, SMPC) rewrite IMS from jo's
 *     copy when they return, which would undo a mask set here.
 * Not nestable, main thread only (the only callers are in i_wad.c). */
static unsigned int saturn_cart_sr_save = 0;

void Saturn_CartWriteBegin(void)
{
    saturn_cart_sr_save = __jo_core_disable_interrupts();
}

void Saturn_CartWriteEnd(void)
{
    __jo_core_restore_interrupts(saturn_cart_sr_save);
}
