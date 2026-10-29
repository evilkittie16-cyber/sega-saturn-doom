/* saturn_dsp.c — SCU A-Bus Cart-Write Critical Section & DSP Stubs.
 *
 * The SCU DSP cart-copy engine is permanently disabled by policy (v5.9b real-HW
 * verdict: multi-pair DSP DMA chains exhibit heavy-tail stalls on real silicon,
 * whereas CPU memcpy through the uncached mirror inside Saturn_CartWriteBegin/End
 * is faster and 100% deterministic).
 *
 * This module retains Saturn_CartWriteBegin / Saturn_CartWriteEnd (which mask
 * VBlank-IN and DSP-End in the 16-bit SCU IMS register 0x25FE00A0 during A-bus
 * cart writes) and the stubbed DSP status accessors queried by main.c, i_wad.c,
 * and i_video.c.
 */

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

static unsigned short saturn_cart_ims_save = 0;

void Saturn_CartWriteBegin(void)
{
    saturn_cart_ims_save = *(volatile unsigned short *)0x25FE00A0u;
    *(volatile unsigned short *)0x25FE00A0u =
        saturn_cart_ims_save | 1u | (1u << 5);
}

void Saturn_CartWriteEnd(void)
{
    *(volatile unsigned short *)0x25FE00A0u = saturn_cart_ims_save;
}
