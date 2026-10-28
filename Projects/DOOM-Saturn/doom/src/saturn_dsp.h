/* saturn_dsp.h — SCU DSP capability probe for DOOM-Saturn.
 *
 * The SCU DSP is the next render offload target (user-endorsed: "scu dsp
 * could do the ceilings and floors"). Before any renderer work, this
 * module proves the four gating capabilities with one round trip:
 *
 *   1. SH-2 can LOAD a program into DSP program RAM (0x25FE0084 port).
 *   2. SH-2 can START it (EX bit, 0x25FE0080) and observe program end
 *      by polling EX (bit 16) back to 0.
 *   3. The DSP's OWN DMA can READ WRAM-H (BSS at 0x060xxxxx) into DSP
 *      data RAM (RA0 register << 2 = byte address).
 *   4. The DSP's own DMA can WRITE DSP data RAM back to WRAM-H (WA0).
 *
 * If any of these fail in an emulator or on hardware, the DSP plan dies
 * here instead of after weeks of renderer work. Verdicts (also shown
 * on the D HUD row in gameplay):
 *   1 = all four passed (data round-trip verified)
 *   0 = program ended cleanly but the data was wrong
 *   2 = program never ended (forced stop)
 */
#ifndef DOOM_SATURN_DSP_H
#define DOOM_SATURN_DSP_H

/* Runs the probe once. Safe to call only after Z/boot but before any
 * SCU-DSP user (SGL's CPK video codec is the only other user;
 * DOOM-Saturn does not use it). */
int Saturn_DspProbe(void);

/* r71 — the DSP as the cart's memory manager. The SCU manual bans
 * CPU-initiated SCU-DMA writes to the A-bus ("Writing to A-Bus by
 * SCU-DMA cannot be used", 2.1 precautions) but sanctions the DSP's
 * own DMA instruction for A-bus targets (3.2: the write-addition
 * rules "also apply to DSP DMA instructions"). Call Init once at
 * boot (after Saturn_CartInit); on success EVERY 2 KB cart write in
 * i_wad.c goes through the engine, with a CPU-uncached-memcpy
 * fallback on any failure. */
int Saturn_DspCartInit(void);
int Saturn_DspCartCopy2K(void *cart_dst, const void *wram_src);
int Saturn_DspCartOk(void);

/* r72 — cart-write critical section (SCU manual p03_06: A-bus flash
 * writes must not be interleaved with interrupts/DMA; its remedy is to
 * disable them around the write sequence). Masks VBlank-IN (IMS bit 0)
 * for the duration. Every cart-write burst — DSP copy or CPU memcpy —
 * runs between these two. */
void Saturn_CartWriteBegin(void);
void Saturn_CartWriteEnd(void);

#endif /* DOOM_SATURN_DSP_H */