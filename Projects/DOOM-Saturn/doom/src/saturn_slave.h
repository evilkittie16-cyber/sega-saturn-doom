/* saturn_slave.h — slave-CPU latency probe (parallelization plan Phase A).
 *
 * HISTORY: the first attempt was a permanent takeover — a one-off
 * slSlaveFunc post of a never-returning entry. It DEADLOCKS: SGL's
 * slSlaveFunc is synchronous (it joins before returning; Jo's bootstrap
 * in jo_engine/slave.c:95-99 sets __jo_slave_done_flag only AFTER the
 * posted callback returns), so a resident entry wedges the master
 * inside slSlaveFunc — observed as a freeze right after "SLV 1 POST".
 * A takeover through SGL's dispatch channel is structurally impossible.
 *
 * 2026-09-01, raw SMPC/SETSINT edition (FROZEN at "SLV 2 A", then with
 * bounded SMPC waits STILL frozen there): the only unbounded code left
 * after the bounded disable was the next slSynch — i.e. DisableSlaveCPU
 * kills SGL's resident slave program (LIBCPK slavesh.o SlaveSHmain) and
 * SGL's master-side frame flow DEPENDS on it. Any SMPC slave reset
 * wedges SGL: structural dead end under SGL.
 *
 * CURRENT edition: stay on SGL's resident program, capture its post
 * protocol empirically (SlaveCommand/State/Param at 0x06043940/44/48),
 * then post jobs directly without joining — the async mailbox SGL
 * itself lacks. The slave-side callback snapshots the protocol area
 * through the P2 uncached mirror; the HUD shows cmd*100+state (N row),
 * param low bits (D row), and execs*10000+match (H row).
 *
 * This edition instead measures the REAL cost of SGL's own handshake:
 * every 60 frames the master posts a RETURNING NOP job through
 * jo_core_exec_on_slave + a bounded join, and reports the round trip on
 * the HUD 'H' row. Decides Phase C's fate:
 *   - round trip ~1-3 vbl -> the ~35 vbl the column-split revert blamed
 *     on the handshake was actually the slave's OC-off work penalty;
 *     SGL dispatch is viable for per-frame column splits.
 *   - round trip ~10+ vbl -> SGL dispatch is truly unusable per frame.
 */
#ifndef SATURN_SLAVE_H
#define SATURN_SLAVE_H

/* Launch/init the probe. Call ONCE from D_DoomMain, before
 * D_DoomLoop. No-op when -DSATURN_NO_SLAVE_HS is set. */
void saturn_slave_init(void);
void saturn_slave_frame_tick(void);
void Saturn_Slave_Divlut_Install(void);
extern unsigned char saturn_slave_enabled;

/* HUD probe outputs (i_video.c rows s/E/N/D/H — s/E/N/D repurposed for
 * the protocol capture in this probe build). ALL captures go through
 * the P2 uncached mirror on both CPUs (SH-2 caches don't snoop each
 * other — edition-3 lesson):
 *   saturn_idle_cmd/_state/_param — SGL protocol area after a job
 *                           finished (master snapshot; s row prints
 *                           cmd*100+state, E row prints param&0xFFFF)
 *   saturn_cap_cmd/_state/_param — same words DURING the job, read on
 *                           the slave (N row prints cmd*100+state)
 *   saturn_scan           — SLPROG offset>>2 (+1) where &probe_cb was
 *                           found after the post; 0xFFFF = searched,
 *                           not found; 0 = no scan yet (D row)
 *   saturn_cap_match      — 1 = SlaveParam == &probe callback (H row
 *                           low digit)
 *   saturn_slave_hs_vbl   — 0 = joined, 9999 = slave never ran the job
 *   saturn_slave_execs_hud — snapshot of the slave's own job counter */
extern volatile unsigned int saturn_cap_cmd;
extern volatile unsigned int saturn_cap_state;
extern volatile unsigned int saturn_cap_param;
extern volatile unsigned int saturn_cap_match;
extern volatile unsigned int saturn_idle_cmd;
extern volatile unsigned int saturn_idle_state;
extern volatile unsigned int saturn_idle_param;
extern volatile unsigned int saturn_scan;
extern volatile int  saturn_slave_hs_vbl;
extern volatile unsigned int saturn_slave_execs_hud;

/* ROUND-61 (2026-09-04): the round-60 direct mailbox is RETIRED — its
 * `*cbp = 0` transition-reset raced SGL's slave-side change-detector,
 * launching bootstrap with param = 0 (`jsr @0` → vector-table garbage
 * → bootstrap self-recursion → 599 KB stack flood, the file5.yss
 * terminal freeze). direct_ok() is now compile-zero (always 0), so the
 * render path's dispatch collapses onto jo_core_exec_on_slave — SGL's
 * own atomic slSlaveFunc protocol, wrapped with the round-59 pre-post
 * slCashPurge. The stubs remain so no call site changes. */
unsigned saturn_slave_direct_ok(void);
unsigned saturn_slave_direct_post(void *cb);

/* Round-60 degrade: 1 = the slave died and the render path switched to
 * master-only rendering (full-width render_half, no dispatches). */
extern volatile unsigned int saturn_slave_master_only;

#endif /* SATURN_SLAVE_H */