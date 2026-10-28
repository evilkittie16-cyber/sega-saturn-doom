/* saturn_dsp.c — SCU DSP capability probe for DOOM-Saturn.
 *
 * Program and handshake are hand-assembled from the instruction format
 * cross-checked against MAME's scudsp.cpp, Kronos's scu.c (the target
 * emulator), original yabause, the scu-dsp-asm ISA tables, and the
 * working saturn-examples scu-dsp programs (div10_vdp2.asm, advent
 * day1). All sources agree on the decode:
 *
 *   0x25FE0080 PPAF  program control port (bit15 LE + bits 7-0 PC load,
 *                    bit16 EX run, bit18 E end flag)
 *   0x25FE0084 PPD   program RAM data port (longword writes, PC++)
 *   0x25FE0088 PDA   data RAM address port (page = val>>6, addr = val&0x3F)
 *   0x25FE008C PDD   data RAM data port (R/W longwords, host ptr auto-inc,
 *                    gated on !EX in Kronos)
 *
 * Instruction words (class = bits 31-30):
 *   00......  ALU op + D1-bus move (field bits 13-12 case 1 = MOV SImm
 *             to dest bits 11-8; 0xC/0xD = CT0/CT1)
 *   10......  MVI: dest bits 29-26 (6 = RA0, 7 = WA0 — scu-dsp-asm
 *             stmt_ins.hpp lines 707-708), imm25 = bits 24-0
 *   11......  DMA (bits 31-28 = 0xC): add bits 17-15, DIRECTION bit 12
 *             (0 = READ D0->DSP via RA0, 1 = WRITE DSP->D0 via WA0 —
 *             MAME dir_from_D0 + yabause dsp_dma01/02 agree; probes
 *             2a/2b proved it empirically: bit12=0 words pulled RA0
 *             data into the DSP banks, bit12=1 words wrote through a
 *             stale WA0 and never touched the target buffer), bank
 *             bits 8-10 (mc0-3), count bits 7-0. Read stride on
 *             WRAM-H sources: add=2 (+4 bytes; add=1 gives stride 0
 *             per MAME's (1 << (add & 2)) & ~1 C-bus rule).
 *   11......  JUMP: cond bits 19-25 (t0 = 0b101000), target bits 7-0
 *   11......  ENDI = 0xF8000000 (REQUIRED: plain END never sets E)
 *
 * Handshake (mirrors the known-good saturn-examples loader):
 *   PPAF = 0; PPAF = 1<<15 (PC=0); PPD x N; PDA/PDD preload; PPAF =
 *   1<<15 AGAIN (loading leaves PC at N — skipping this runs NOPs
 *   forever); PPAF = 1<<16; poll PPAF bit 18 (E); PPAF = 0.
 *
 * Phase 2c probe — corrected directions, one run:
 *
 *   leg A  READ   0xC0010040  bit12=0, add=2, mc0, 64  (RA0 = dsp_in)
 *   leg B  WRITE  0xC0009040  bit12=1, add=1, mc0, 64  (WA0 = dsp_out)
 *   leg C  WRITE  0xC0011040  bit12=1, add=2, mc0, 64  (WA0 = dsp_out2)
 *
 * The payload is self-describing (dsp_in[i] = 0xC0DE0000+i) and RAM0
 * is preloaded with a distinct pattern, so each leg's outcome is
 * scored CPU-side against what landed where:
 *
 * HUD rows:
 *   D = verdict: 1 iff read stride-1 AND write add=1 chain verified
 *   T = RAM0[i] == in[i]     (leg A read, stride-1; want 64)
 *   W = RAM0[i] == in[0]     (stride discriminator: 0 dead, 1 one
 *                             word, 2 = stride 2, 4 = stride 4,
 *                             64 = stride 0)
 *   G = out[i] == preload    (leg B moved RAM0's preload out)
 *   K = out[i] == in[i]      (leg B chain: read then write-back; 64)
 *   J = out2[i] == preload   (leg C moved RAM0's preload out)
 *   R = out2[i] == in[i]     (leg C chain with write add=2)
 */
#include "doomdef.h"
#include "saturn_cache.h"

/* Saturn_StageColor — bring-up color ladder (i_system.c). Declared here
 * instead of via i_system.h: that header drags jo.h/ANGLE types into
 * this compilation unit without the SGL include context. */
void Saturn_StageColor(unsigned short color);
/* ISR glyph painter (i_video.c) — boot breadcrumb for the engine
 * verdict (same channel as Saturn_CartInit's C= probe breadcrumb). */
extern void Saturn_StageText(int x, int y, const char *s);

/* Bring-up ladder (diagnostic build): each step paints NBG1 its own
 * color BEFORE attempting the step, so the last color seen localizes a
 * crash/hang to the step between it and the next. Values are raw RGB15
 * (jo_color = 0x8000 | B<<10 | G<<5 | R) chosen to not collide with the
 * boot stages in main.c. */
#define DSP_CLR_ENTER   0x83E0u   /* green    (0,31,0)  */
#define DSP_CLR_LOADED  0xBC00u   /* blue     (0,0,31)  */
#define DSP_CLR_STARTED 0x83FFu   /* yellow   (31,31,0) */
#define DSP_CLR_ENDED   0xBC1Fu   /* magenta  (31,0,31) */
#define DSP_CLR_DMARD   0x821Fu   /* orange   (31,16,0) */
#define DSP_CLR_ADD     0xBC10u   /* violet   (16,0,31) */
#define DSP_CLR_FAIL    0x801Fu   /* red      (31,0,0)  */

#define SCU_DSP_PPAF (*(volatile unsigned int *)0x25FE0080u)
#define SCU_DSP_PPD  (*(volatile unsigned int *)0x25FE0084u)
#define SCU_DSP_PDA  (*(volatile unsigned int *)0x25FE0088u)
#define SCU_DSP_PDD  (*(volatile unsigned int *)0x25FE008Cu)

#define DSP_EX   0x00010000u   /* PPAF bit 16: 1 = executing          */
#define DSP_LE   0x00008000u   /* PPAF bit 15: load PC from bits 7-0  */

/* WRAM-H BSS: both SH-2 (cache-through 0x06xxxxxx) and the DSP's DMA
 * (byte address = RA0/WA0 << 2) reach these. The SH-2 compares against
 * dsp_in itself (dsp_ref was a verbatim copy — removed). */
static unsigned int dsp_in[64]   __attribute__((aligned(16)));
static unsigned int dsp_out[64]  __attribute__((aligned(16)));
static unsigned int dsp_out2[64] __attribute__((aligned(16)));
/* (dsp_ref and dsp_pat0 were removed: ref == dsp_in and the preload
 * pattern is a generated constant, so both were rebuilt inline — this
 * reclaimed 512 B of BSS for the seg renderer's footprint.) */

/* Post-mortem counters for the HUD rows (semantics per the file
 * header; negative = probe never got there / timed out):
 *   saturn_dsp_ramin = RAM0 == in            (read, stride 1)
 *   saturn_dsp_zero  = dsp_out == preload    (write add=1 moved data)
 *   saturn_dsp_match = dsp_out == in         (write add=1 chain)
 *   saturn_dsp_ppaf  = dsp_out2 == preload   (write add=2 moved data)
 *   saturn_dsp_str2  = dsp_out2 == in        (write add=2 chain)
 *   saturn_dsp_str4  = RAM0 == in[0]         (read stride discriminator) */
int saturn_dsp_ramin = -1;
int saturn_dsp_zero  = -1;
int saturn_dsp_match = -1;
int saturn_dsp_ppaf  = -1;
int saturn_dsp_str2  = -1;
int saturn_dsp_str4  = -1;

/* MVI dest field values (bits 29-26) — scu-dsp-asm stmt_ins.hpp. */
#define MVI_RA0 0x06u
#define MVI_WA0 0x07u

#define MVI(dest, imm) (0x80000000u | ((unsigned)(dest) << 26) | ((unsigned)(imm) & 0x01FFFFFFu))

/* D1-bus "MOV SImm,[d]" (field bits 13-12 = 01): imm = sign-extended
 * low byte, dest bits 11-8. 0xC/0xD = CT0/CT1 direct write. */
#define MOV_IMM_CT0 0x00001C00u   /* mov 0,ct0 — yaul test1's word 1   */

/* DMA words: class 11 (bits 31-28 = 0xC), add bits 17-15, direction
 * bit 12 (0 = READ via RA0, 1 = WRITE via WA0), bank bits 8-10,
 * count bits 7-0. */
#define DMA_READ_ADD2(n)     (0xC0000000u | (2u << 15) | (n))
#define DMA_WRITE_ADD1(n)    (0xC0000000u | (1u << 15) | (1u << 12) | (n))
#define DMA_WRITE_ADD2(n)    (0xC0000000u | (2u << 15) | (1u << 12) | (n))

/* Jump: cond bits 19-25 (t0 = 0b101000, direct test), target bits 7-0.
 * Spin-while-T0 with the mandatory NOP delay slot (known-good
 * "jmp t0,dma_wait; nop" from div10_vdp2.asm). */
#define DSP_JMP_T0(pc) (0xD0000000u | (0x28u << 19) | (pc))
#define DSP_NOP        0x00000000u

#define DSP_ENDI 0xF8000000u   /* END + bit 27: sets E (PPAF bit 18)  */

/* demo1's quiet delay, verbatim (v5.6 hardware burn): register-only
 * dt/bf spin, zero memory accesses, zero SCU touches. ~4
 * cycles/iteration at 28.6 MHz -> 7143 iters = 1 ms. Any SCU or
 * memory touch during a DSP DMA transfer window stalls it (demo1
 * v2/v3 lesson: hammering PPAF chokes transfers). */
static void dsp_quiet_delay(unsigned int n)
{
    __asm__ __volatile__(
        "1: dt %[n]\n"
        "   bf 1b" : [n]"+r"(n));
}

static void dsp_quiet_ms(unsigned int ms)
{
    dsp_quiet_delay(ms * 7143u);
}

/* Verdicts: 1 = bit12=0/add=2 is a stride-1 read AND bit12=1/add=1
 * writes through WA0; 0 = program ended but the data says otherwise
 * (counters localize which leg failed); 2 = end flag never set.
 * r94: rebuilt to the demo1-proven WAIT-FREE shape (no T0 waits —
 * they never end on real silicon) and the quiet E-poll (the r71-era
 * 4M back-to-back PPAF hammer chokes transfers). */
int Saturn_DspProbe(void)
{
#if defined(SATURN_DSP_OFF)
    /* WEDGE SUSPECT (2026-09-07): see Saturn_DspCartInit — the boot
     * probe EX-ran a DSP program on every boot for a purely
     * informational verdict, on a DSP the engine no longer uses and
     * that real HW now reports as FAIL. Return "not run" (-1). */
    return -1;
#endif
    static const unsigned int prog_init[] =
    {
        MOV_IMM_CT0,                  /* 0: CT0 = 0                      */
        MVI(MVI_RA0, 0),              /* 1: RA0 = dsp_in>>2   (patched)  */
        DMA_READ_ADD2(64),            /* 2: READ dsp_in -> RAM0 (a=2)    */
        MOV_IMM_CT0,                  /* 3: CT0 = 0                      */
        MVI(MVI_WA0, 0),              /* 4: WA0 = dsp_out>>2  (patched)  */
        DMA_WRITE_ADD1(64),           /* 5: WRITE RAM0 -> dsp_out (a=1)  */
        MOV_IMM_CT0,                  /* 6: CT0 = 0                      */
        MVI(MVI_WA0, 0),              /* 7: WA0 = dsp_out2>>2 (patched)  */
        DMA_WRITE_ADD2(64),           /* 8: WRITE RAM0 -> dsp_out2 (a=2) */
        DSP_ENDI                      /* 9: end (sets E, bit 18)         */
    };
    unsigned int prog[10];
    int i, ok;
    int c_str1, c_write1, c_chain1, c_write2, c_chain2, c_stride;

    for (i = 0; i < 10; i++)
        prog[i] = prog_init[i];
    /* RA0/WA0 hold WORD addresses (external byte address = reg << 2). */
    prog[1] = MVI(MVI_RA0, (unsigned int)dsp_in   >> 2);
    prog[4] = MVI(MVI_WA0, (unsigned int)dsp_out  >> 2);
    prog[7] = MVI(MVI_WA0, (unsigned int)dsp_out2 >> 2);

    /* Payload. The preload pattern is 0x3C3C0000+i (computed inline at
     * every use — see the PDD preload and the post-mortem scoring). */
    for (i = 0; i < 64; i++)
    {
        dsp_in[i]   = 0xC0DE0000u + (unsigned int)i;   /* self-describing */
        dsp_out[i]  = 0u;
        dsp_out2[i] = 0u;
    }

    /* SH-2 -> DSP visibility for the DMA reads: purge the operand
     * cache so the DSP's DMA sees the buffers (write-back cache). */
    saturn_oc_purge();

    Saturn_StageColor(DSP_CLR_ENTER);
    SCU_DSP_PPAF = 0;                        /* stop                   */
    SCU_DSP_PPAF = DSP_LE;                   /* LE=1, PC = 0           */

    /* Preload RAM0 with a distinct pattern (RAM1 not used this run) so
     * a dead read leg is provable (pattern surviving = the read
     * transferred nothing, and the writes then export the pattern). */
    SCU_DSP_PDA = 0;
    for (i = 0; i < 64; i++)
        SCU_DSP_PDD = 0x3C3C0000u + (unsigned int)i;   /* RAM0 preload */

    for (i = 0; i < 10; i++)
        SCU_DSP_PPD = prog[i];               /* program RAM, PC 0->10  */
    Saturn_StageColor(DSP_CLR_LOADED);

    /* THE missed-step fix: reset PC again before executing. */
    SCU_DSP_PPAF = DSP_LE;                   /* LE=1, PC = 0 AGAIN     */
    Saturn_StageColor(DSP_CLR_STARTED);
    SCU_DSP_PPAF = 0;                        /* park (demo1 shape)     */
    SCU_DSP_PPAF = DSP_EX;                   /* EX=1: run from PC 0    */

    /* End handshake: QUIET wait, then spaced E-flag samples (r94 —
     * demo1 protocol; the old 4M back-to-back PPAF hammer chokes
     * transfers). Still bounded (R1 freeze lesson: a wedged DSP
     * starves the whole SCU). */
    {
        int ended = 0;
        dsp_quiet_ms(4);
        for (i = 0; i < 40; i++)
        {
            if ((SCU_DSP_PPAF >> 18) & 1u) { ended = 1; break; }
            dsp_quiet_ms(2);
        }
        SCU_DSP_PPAF = 0;                    /* parked, stopped        */
        if (!ended)
        {
            Saturn_StageColor(DSP_CLR_FAIL); /* end flag never set     */
            saturn_dsp_ramin = saturn_dsp_zero = saturn_dsp_match = -2;
            saturn_dsp_ppaf  = saturn_dsp_str2  = saturn_dsp_str4  = -2;
            return 2;
        }
    }
    Saturn_StageColor(DSP_CLR_ENDED);
    Saturn_StageColor(DSP_CLR_DMARD);

    /* Post-mortem: DSP -> SH-2 visibility purge, then score every
     * hypothesis against what actually landed where. */
    saturn_oc_purge();
    Saturn_StageColor(DSP_CLR_ADD);
    c_str1 = c_write1 = c_chain1 = c_write2 = c_chain2 = c_stride = 0;
    for (i = 0; i < 64; i++)
    {
        unsigned int v;
        SCU_DSP_PDA = (unsigned int)i;       /* bank 0, word i         */
        v = SCU_DSP_PDD;
        if (v == dsp_in[i]) c_str1++;
        if (v == dsp_in[0]) c_stride++;
        if (dsp_out[i]  == 0x3C3C0000u + (unsigned int)i) c_write1++;
        if (dsp_out[i]  == dsp_in[i])   c_chain1++;
        if (dsp_out2[i] == 0x3C3C0000u + (unsigned int)i) c_write2++;
        if (dsp_out2[i] == dsp_in[i])   c_chain2++;
    }
    saturn_dsp_ramin = c_str1;
    saturn_dsp_zero  = c_write1;
    saturn_dsp_match = c_chain1;
    saturn_dsp_ppaf  = c_write2;
    saturn_dsp_str2  = c_chain2;
    saturn_dsp_str4  = c_stride;

    ok = (c_str1 == 64 && c_chain1 == 64);
    if (!ok)
        Saturn_StageColor(DSP_CLR_FAIL);     /* verify failed          */
    return ok;
}

/* ====== r71: the SCU DSP as the cart's memory manager ======
 *
 * The SCU User's Manual (infochunk hon/p02_10.htm, section 2.1
 * "precautions") bans CPU-initiated SCU-DMA writes to the A-bus
 * outright: "Writing to A-Bus by SCU-DMA cannot be used." That is the
 * r68 instant wedge, explained — the parked SGL DMA path (now deleted
 * from i_wad.c) could never work on real hardware. The manual's
 * register-level rules (hon/p03_02.htm) DO specify write-addition
 * values for A-bus targets and note "this also applies to DSP DMA
 * instructions": the DSP's OWN DMA instruction is the sanctioned
 * engine for moving data to the A-bus (its transferable-area map,
 * Figure 2.3, is separate from the CPU-initiated Figure 2.2).
 *
 * It is also the right engine against the other real-HW failure: the
 * A-bus "Restrictions when using A-Bus" section (hon/p03_06.htm) says
 * of flash memory that write-to-write spacing cannot be guaranteed if
 * "interrupts or DMA occur in the middle" — the r64+ boot fill's CPU
 * memcpy bursts were interrupted by the vblank ISR at chunk
 * boundaries that MOVED between boots (F wedged at ~1000-1100 KB,
 * then ~700-800 KB). The DSP's DMA performs each 64-word burst as one
 * atomic bus-master transfer no interrupt can split.
 *
 * Per 2 KB copy the SH-2 patches the two MVI address words (DSP
 * stopped), then EX-runs a 27-word WAIT-FREE program: 8 x (CT0 = 0;
 * DMA READ 64 words from WRAM-H into MC0 via RA0, add=010b — the
 * contiguous encoding the 09-01 leg-A probe proved; DMA WRITE 64
 * words MC0 -> cart via WA0, add=010b — the A-bus write addition
 * value from manual 3.2, same encoding leg C proved contiguous), then
 * ENDI. NO T0 spin-waits: demo1's v5.6 hardware burn (09-05, 8/8)
 * proved T0 NEVER CLEARS on real silicon — the r71-73 T0-wait shape
 * parked there forever, and the hammering E-poll that followed choked
 * the bus into the r71-73 first-GFS wedge. Completion is PPAF bit 18
 * (E) sampled QUIETLY (register-only dt/bf delays before and between
 * samples) under a bounded budget — the vblank counter is NOT usable
 * as a timeout here (a wedged A-bus freezes the ISR, the r68 lesson).
 *
 * Addresses: DSP external addressing is (RA0/WA0 << 2), a 27-bit
 * space — it reaches the cart's PRIMARY window (0x02400000) but NOT
 * the CPU's 0x22400000/0x42400000 mirrors, so the caller's cart
 * pointer is stripped of the mirror bits. WRAM-H sources (0x060xxxxx)
 * are directly reachable. The caller stages every sector through an
 * UNCACHED mirror so the DSP's DMA — which reads raw WRAM-H, blind to
 * the SH-2 operand cache — sees GFS's data.
 *
 * Failures demote PERMANENTLY: i_wad.c then writes the cart by CPU
 * memcpy through uncached mirrors (the red-screen-era path). One-shot
 * full-2KB readback verify in i_wad.c catches a DSP copy that "ends"
 * but lands garbage (the r68 verify lesson: compare against a source
 * the SH-2 independently trusts). */

/* Self-test buffers: 2 KB each, UNCACHED-mirror filled/compared so the
 * DSP's DMA reads what the SH-2 actually wrote and vice versa. */
static unsigned int dsp_ct_a[512] __attribute__((aligned(16)));
static unsigned int dsp_ct_b[512] __attribute__((aligned(16)));

#define DSP_DMA_READ64   (0xC0000000u | (2u << 15) | 64u)
#define DSP_DMA_WRITE64  (0xC0000000u | (2u << 15) | (1u << 12) | 64u)

/* r94 — the demo1-proven WAIT-FREE program shape (v5.6 hardware burn
 * 09-05, 8/8 pass on real silicon). The r71 program shape is RETIRED:
 * its JMP-T0 spins after every DMA op never end on real hardware —
 * the DSP's T0 flag NEVER CLEARS there (Kronos clears it; real
 * silicon parks forever with the data already moved, which is exactly
 * what every r71-73 "D=0TT" breadcrumb showed). The proven handshake
 * is: issue the DMA ops back-to-back, let the CPU go QUIET
 * (register-only dt/bf spins — hammering PPAF chokes transfers),
 * then poll E with silence between samples. demo1's CD2/CD3 legs ran
 * this exact 8x64-pair program shape to bad=0 on real HW. */
#define DSP_PAIR_BASE    2u
#define DSP_PAIR_WORDS   3u                     /* CT0, READ, WRITE    */
#define DSP_CART_PROG_WORDS (DSP_PAIR_BASE + 8u * DSP_PAIR_WORDS + 1u)

static unsigned int dsp_cart_prog[DSP_CART_PROG_WORDS];   /* 27 words */

static int          saturn_dsp_cart_ok       = 0;
static int          saturn_dsp_cart_verified = 0;
static int          dsp_ct_last_timeout      = 0;   /* r73: E-poll flag */

static void dsp_cart_build_prog(void);
int Saturn_DspCartCopy2K(void *cart_dst, const void *wram_src);

static void dsp_cart_build_prog(void)
{
    unsigned int w, i;

    dsp_cart_prog[0] = MVI(MVI_RA0, 0);          /* patched per copy */
    dsp_cart_prog[1] = MVI(MVI_WA0, 0);          /* patched per copy */
    for (i = 0; i < 8u; i++)
    {
        w = DSP_PAIR_BASE + i * DSP_PAIR_WORDS;
        dsp_cart_prog[w + 0] = MOV_IMM_CT0;      /* CT0 = 0          */
        dsp_cart_prog[w + 1] = DSP_DMA_READ64;   /* RA0 -> MC0 (64)  */
        dsp_cart_prog[w + 2] = DSP_DMA_WRITE64;  /* MC0 -> WA0 (64)  */
    }
    dsp_cart_prog[DSP_CART_PROG_WORDS - 1] = DSP_ENDI;
}

/* r72 register-map corrections (SCU manual 3.5, p03_05): IMS lives at
 * 0x25FE00A0 and WRITING 1 MASKS the interrupt (reset 0x0000BFFF); IST
 * at 0x25FE00A4 is status-only and WRITING IT IS PROHIBITED. The r71
 * build had both backwards: it cleared IMS bit 5 (UNmasking the
 * DSP-End level-A interrupt with no handler installed) and wrote 1<<5
 * to IST after every copy. 0x25FE00A4 = IST, not IMASK — the r58 I/M
 * row in i_video.c had the same swap. */

/* Load the program once, mask the DSP-end SCU interrupt (ENDI raises
 * vector 0x45 and nothing in this port handles it — IMS bit 5, 1 =
 * masked), then prove the engine twice: the second pass with a
 * different pattern catches a first pass that "passed" by recycling
 * stale DRAM. */
int Saturn_DspCartInit(void)
{
    int i, pass1, pass2, to1, to2;

#if defined(SATURN_DSP_OFF)
    /* WEDGE SUSPECT (2026-09-07): the engine is OFF BY POLICY (v5.9b
     * verdict below) yet this self-test still EX-ran the 8-pair program
     * on EVERY boot — the exact shape demo1 convicted of mid-chain
     * stalls on real silicon (6/30 fails; real-HW boot report now reads
     * SCU-DSP-FAIL). A DSP stalled mid-DMA is a bus master holding SCU
     * state, and the level-start wedge strikes at the first heavy A-bus
     * train after boot. The self-tests buy nothing; gate them out. */
    saturn_dsp_cart_ok = 0;
    return saturn_dsp_cart_ok;
#endif

    /* r94: IMS is a 16-bit register — the old 32-bit poke straddled
     * IST (0x25FE00A4), which the manual PROHIBITS writing. 16-bit
     * only, and r71-73's real-HW D=0TT + first-GFS wedge happened
     * immediately after this exact 32-bit write. */
    *(volatile unsigned short *)0x25FE00A0u |= (1u << 5); /* mask DSP-End */

    dsp_cart_build_prog();
    SCU_DSP_PPAF = 0;                        /* stop                   */
    SCU_DSP_PPAF = DSP_LE;                   /* LE=1, PC = 0           */
    for (i = 0; i < (int)DSP_CART_PROG_WORDS; i++)
        SCU_DSP_PPD = dsp_cart_prog[i];

    /* Pass 1: pattern A, a -> b. Buffers go through the UNCACHED
     * aliases: the CPU's cached view is invisible to the DSP's DMA. */
#define DSP_UNC32(p) ((volatile unsigned int *)(0x20000000u | (unsigned int)(p)))
    for (i = 0; i < 512; i++)
    {
        DSP_UNC32(dsp_ct_a)[i] = 0xC0DE0000u + (unsigned int)i;
        DSP_UNC32(dsp_ct_b)[i] = 0u;
    }
    pass1 = (Saturn_DspCartCopy2K(dsp_ct_b, dsp_ct_a) == 0);
    to1 = dsp_ct_last_timeout;
    for (i = 0; i < 512 && pass1; i++)
        if (DSP_UNC32(dsp_ct_b)[i] != 0xC0DE0000u + (unsigned int)i)
            pass1 = 0;

    /* Pass 2: pattern B — a copy that only shuffled stale bytes
     * fails here. */
    for (i = 0; i < 512; i++)
    {
        DSP_UNC32(dsp_ct_a)[i] = 0x3C3C0000u + (unsigned int)i;
        DSP_UNC32(dsp_ct_b)[i] = 0u;
    }
    pass2 = (Saturn_DspCartCopy2K(dsp_ct_b, dsp_ct_a) == 0);
    to2 = dsp_ct_last_timeout;
    for (i = 0; i < 512 && pass2; i++)
        if (DSP_UNC32(dsp_ct_b)[i] != 0x3C3C0000u + (unsigned int)i)
            pass2 = 0;

    saturn_dsp_cart_ok = (pass1 && pass2);

    /* Boot breadcrumb with pass detail: D=1 engine armed, D=0 engine
     * dead, and per-pass letters — P = clean pass, T = ENDI/E flag
     * never set (timeout), M = ended but data wrong. "D=0TT" on real
     * HW means the DSP program never ran to completion there; "D=0MM"
     * means it ran but the DMA moved wrong bytes. r78: painted at
     * x=140 — (8,208) is the P/W stall reporter's W row and the two
     * overprinted into unreadable garbage in every r75/r76 photo.
     * With the v5.9b policy-off below, "D=0PP" reads: self-test
     * PASSED, engine disabled by policy. */
    {
        char bc[8];
        bc[0] = 'D'; bc[1] = '=';
        bc[2] = saturn_dsp_cart_ok ? '1' : '0';
        bc[3] = pass1 ? 'P' : (to1 ? 'T' : 'M');
        bc[4] = pass2 ? 'P' : (to2 ? 'T' : 'M');
        bc[5] = '\0';
        Saturn_StageText(140, 208, bc);
    }

    /* v5.9b REAL-HW VERDICT (demo1 rig, burn 09-05) — the engine is
     * OFF BY POLICY. The demo1 drain calibration convicted the
     * multi-pair program shape on real silicon, independent of the
     * mid-chain CT0 re-init:
     *   - 8-pair WITH re-init (this file's program): floor 86ms,
     *     6/30 stress FAILS even at 320ms (heavy-tail mid-chain
     *     stall — pairs 2-7 never land; fb=127 signature).
     *   - 8-pair RAW (no re-init): floor 36ms, 2/30 stress fails at
     *     floor+25%, and it failed the matrix CD2 leg (M bad=385,
     *     same fb=127) on a boot where the re-init shape passed —
     *     so the stall follows neither the re-init nor boot luck.
     *   - 64-pair: 0/5 confirms even at ~4s (the v5.7 verdict).
     *   - 1-pair (one 64-word pair per program): 1ms c5, and the
     *     v5.8 whole-window sweep moved all 16384 pages clean — but
     *     at ~1ms/page it is an order of magnitude slower than the
     *     CPU memcpy this demote falls back to, with no atomicity
     *     win (the caller already holds Saturn_CartWriteBegin's IMS
     *     masking).
     * The fill therefore runs on CPU memcpy through the uncached
     * mirror inside CartWriteBegin/End — the path every real-HW
     * boot has effectively been using anyway after the first engine
     * stall tripped the timeout demote. The self-test above still
     * runs each boot and paints the breadcrumb for the record; a
     * future burn can flip this one line to re-arm the engine.
     * v5.13 WARNING before re-arming: i_wad.c stages CD data through
     * CACHED WRAM now (the staging tripwire keeps it coherent) and
     * hands the DSP the UNCACHED mirror — but a cached stage means
     * RAM may lag the cache lines GFS's CPU transfer wrote. Re-arm
     * only after re-verifying cart integrity byte-exact on real HW
     * with the cached stage in place (the boot self-test + X=0000
     * after a full fill); saturn_oc_purge() is a permanent no-op and
     * cannot reconcile the two views. */
    saturn_dsp_cart_ok = 0;
    return saturn_dsp_cart_ok;
}

/* r72: cart-write critical section (SCU manual 3.6/p03_06, flash on
 * the A-bus: "software cannot guarantee write-to-write spacing because
 * interrupts or DMA occur in the middle" — its remedy is to disable
 * interrupts and DMA around the write sequence). Every cart-write
 * burst — DSP copies AND CPU memcpys — runs between Begin/End, which
 * mask VBlank-IN (IMS bit 0; level F, the ISR that paints and counts).
 * A vblank swallowed during a <1 ms window is a 1-tick drift in
 * vbl_count — boot-time only, harmless. Save/restore the whole IMS so
 * SGL's own mask state survives byte-exact; if bit 0 was already 0
 * (some other config), the pair is a no-op both ways.
 * r94: IMS is a 16-BIT register (0x25FE00A0) — the old 32-bit
 * read/writes here straddled IST (0x25FE00A4), which the manual
 * PROHIBITS writing. Same fix Saturn_DspCartInit already carried; the
 * Begin/End pair ran on the hot path of every r71-73 boot. */
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

/* Move 2048 bytes WRAM-H -> cart with the DSP's own DMA. Returns 0 on
 * a clean end handshake, -1 on timeout/demote (caller then memcpys).
 * Both pointers must be 4-byte aligned; the cart pointer may be any
 * CPU mirror — only the primary A-bus window is formed. The caller
 * must hold Saturn_CartWriteBegin() — this deliberately does NOT mask
 * internally so the boot self-test (which needs no masking) and the
 * per-copy policy stay in the caller's hands.
 *
 * r94 completion protocol (demo1 v5.6, real HW 8/8): EX, then go
 * QUIET (dsp_quiet_ms — the register-only dt/bf spin; ANY SCU touch
 * during the transfer window stalls it), then poll E with silence
 * between samples. The r71 4M-iteration back-to-back PPAF hammer is
 * RETIRED — it polls the SCU millions of times while the DMA runs,
 * and demo1 measured that this chokes transfers (~1 word moved). A
 * 2 KB copy is 8x(64 read + 64 write) = 1024 word-moves; demo1's
 * 512-word CD leg completed inside a 300 ms quiet window with huge
 * margin, so 4 ms initial silence + 40 spaced polls (2 ms apart =
 * ~84 ms worst case) is deep inside the proven envelope while a
 * healthy copy typically ends on the first poll. */
int Saturn_DspCartCopy2K(void *cart_dst, const void *wram_src)
{
    unsigned int dstw, srcw;
    int i, ended = 0;

    dsp_ct_last_timeout = 0;
    if (!saturn_dsp_cart_ok)
        return -1;
    if ((((unsigned int)cart_dst) | ((unsigned int)wram_src)) & 3u)
        return -1;                           /* DMA is longword-granular */

    dstw = (((unsigned int)cart_dst) & ~0x60000000u) >> 2;
    srcw = ((unsigned int)wram_src) >> 2;

    SCU_DSP_PPAF = 0;                        /* stop                   */
    SCU_DSP_PPAF = DSP_LE | 0u;              /* LE=1, PC = 0           */
    SCU_DSP_PPD = MVI(MVI_RA0, srcw);        /* word 0: source         */
    SCU_DSP_PPD = MVI(MVI_WA0, dstw);        /* word 1: cart dst       */
    SCU_DSP_PPAF = DSP_LE | 0u;              /* RE-RESET PC (known-good:
                                              * loading leaves PC at N) */
    SCU_DSP_PPAF = 0;                        /* park (demo1 shape)     */
    SCU_DSP_PPAF = DSP_EX;                   /* run from PC 0          */

    dsp_quiet_ms(4);                         /* transfer window: dead
                                              * quiet, no SCU touches  */
    for (i = 0; i < 40; i++)
    {
        if ((SCU_DSP_PPAF >> 18) & 1u) { ended = 1; break; }
        dsp_quiet_ms(2);                     /* silence between samples */
    }
    SCU_DSP_PPAF = 0;                        /* parked                 */
    /* r72: the write-1 to IST (0x25FE00A4) here is GONE — the manual
     * prohibits writing IST outright. With DSP-End masked in IMS the
     * pending status bit is inert. */

    if (!ended)
    {
        saturn_dsp_cart_ok = 0;              /* permanent demote       */
        dsp_ct_last_timeout = 1;
        {   /* r73: repaint the breadcrumb so a photo can tell a
             * mid-boot demote (D=2) from a failed self-test (D=0). */
            char bc[4];
            bc[0] = 'D'; bc[1] = '='; bc[2] = '2'; bc[3] = '\0';
            Saturn_StageText(140, 208, bc);
        }
        return -1;
    }
    return 0;
}

int Saturn_DspCartOk(void)
{
    return saturn_dsp_cart_ok;
}