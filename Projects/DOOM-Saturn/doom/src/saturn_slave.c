/* saturn_slave.c — ROUND-61 (2026-09-04): direct mailbox RETIRED.
 *
 * HISTORY: (1) slSlaveFunc takeover deadlocks (it JOINS). (2) Raw
 * SMPC/SETSINT takeover kills SGL's resident slave program and SGL's
 * master frame flow dies at the next slSynch — structural dead end
 * under SGL. (3) Round-59/60: locate SGL's mailbox empirically and
 * post jobs directly (no join).
 *
 * ROUND-60 VERDICT (file5.yss forensics, 2026-09-04): the direct post
 * TERMINAL-FROZE the demo at vbl 30657 (~8.5 min). The post's
 * `*cbp = 0` transition-reset raced the resident program's slave-side
 * change-detector: when the slave polled inside the window between the
 * zero write and the callback write, it launched bootstrap with
 * param = 0. bootstrap does `jsr @r4` — the slave jumped to address 0
 * and executed the vector table as code (the round-59 exception
 * signature PC=0x212). The garbage execution wandered until it
 * re-entered jo_core_slave_bootstrap with r4 = bootstrap itself —
 * which self-recurses (sts.l pr,@-r15 / jsr @r4), one 4-byte frame per
 * level. 149,530 levels later the slave's stack had descended
 * ~599 KB from 0x060FFC00 through ALL of low WRAM (saturn_render.bss,
 * the jo heap, heart_pub, every diagnostic global) and the slave was
 * wedged in the vblank callback dispatcher; the master died on the
 * next join with every watchdog's state smeared. The word 0x0601FDB6
 * (bootstrap+6, the return address bootstrap's jsr pushes) filled
 * 149,530 words of the state file — the fingerprint of the recursion.
 *
 * LESSON: SGL's slave mailbox has no atomic (fn, param) post from
 * outside SGL's own SlaveControl; any direct write sequence is a
 * window the resident program can catch. The only safe per-frame
 * dispatch is SGL's OWN slSlaveFunc (which jo_core_exec_on_slave wraps
 * with the round-59 pre-post slCashPurge). Its residual hazard — the
 * master joining forever if the slave dies MID-JOB — is accepted:
 * with the cache races purged and no direct-mailbox stomping, the
 * slave stops crashing mid-job in the first place.
 *
 * This file keeps the public surface (init / frame_tick / direct_ok /
 * direct_post) as inert stubs so the render path silently falls back
 * to jo_core_exec_on_slave, and keeps the round-60 degrade ladder's
 * master_only flag (saturn_render.c still consults it).
 */
#include <jo/jo.h>

#include "saturn_slave.h"

/* The frame clock — incremented by the VBlank callback (i_video.c). */
extern volatile unsigned int saturn_vbl_count;

/* Boot progress text (i_system.c). */
void Saturn_BootMsg(const char *msg);

/* HUD-capture surfaces kept for the header's extern declarations
 * (BSS, a few words; written nowhere since the probes retired). */
volatile unsigned int saturn_cap_cmd;
volatile unsigned int saturn_cap_state;
volatile unsigned int saturn_cap_param;
volatile unsigned int saturn_cap_match;
volatile unsigned int saturn_idle_cmd;
volatile unsigned int saturn_idle_state;
volatile unsigned int saturn_idle_param;
volatile unsigned int saturn_scan;
volatile unsigned int saturn_slave_execs;
volatile unsigned int saturn_slave_execs_hud;
volatile int saturn_slave_hs_vbl = 0;

/* Round-60 degrade flag: 1 = the render path rendered master-only
 * (the SLV DEAD ladder fired). Stays readable by saturn_render.c. */
volatile unsigned int saturn_slave_master_only;

/* .data, not .bss: the known BSS stomper must not flip this mid-frame
 * (a flip between slave launch and slave wait desyncs the two CPUs). */
unsigned char saturn_slave_enabled __attribute__((section(".data"))) = 0;
volatile unsigned int saturn_slave_frame_count = 0;
static volatile int slave_boot_canary = 0;
extern volatile bool __jo_slave_done_flag;

void Saturn_Slave_Divlut_Install(void)
{
    /* No OCRAM on Kronos/Saturn hardware renderer; divlut lives in WRAM-L (SATURN_DIVLUT_ADDR) */
}

static void slave_boot_test(void)
{
    Saturn_Slave_Divlut_Install();
    *((volatile int *)((unsigned int)&slave_boot_canary | 0x20000000)) = 0x51A7E;
}

void saturn_slave_init(void)
{
    saturn_slave_enabled = 1;
    saturn_slave_master_only = 0u;
    saturn_slave_hs_vbl = 0;
    Saturn_BootMsg("SLV ON");
}

void saturn_slave_frame_tick(void)
{
    /* Handshake verified OK: production slave execution runs in Saturn_HW_RenderPlayerView */
}

/* Always 0: the direct mailbox is retired (see the round-60 verdict
 * above). The render path's `direct_ok() ? direct_post : exec_on_slave`
 * branches collapse onto jo_core_exec_on_slave at compile-zero cost. */
unsigned saturn_slave_direct_ok(void)
{
    return 0u;
}

unsigned saturn_slave_direct_post(void *cb)
{
    (void)cb;
    return 0u;
}