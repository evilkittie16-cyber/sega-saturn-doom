/* saturn_hw_walk2.h - two-CPU screen split (HW_WALK2, 2026-11-24): the
 * mailbox between the master renderer (saturn_hw_render.o) and the slave's
 * private renderer copy (saturn_hw_walk2.o). See saturn_hw_walk2.c.
 *
 * Frame protocol (all fields through the cache-through alias, both CPUs):
 *   master: frame setup, fills the inputs, seq++      -> slave starts
 *   slave:  purges its cache, renders columns [x0, x1) into its own command
 *           lists, publishes the outputs, done = seq
 *   master: renders [0, x0) meanwhile, serves the slave's requests (rpc_*)
 *           at every subsector and while it waits for done, then delivers
 *           both halves, each behind a VDP1 user clip of its columns.
 * Requests (slave -> master): everything that touches the VDP1 texture
 * allocator or the WAD/cart loaders runs on the master only. The slave
 * writes rpc_fn / rpc_a[], then rpc_req++, and spins until rpc_ack ==
 * rpc_req; results come back in rpc_r[]. */
#ifndef SATURN_HW_WALK2_H
#define SATURN_HW_WALK2_H

#include <stdint.h>

#ifndef HW_WALK2
#define HW_WALK2 0
#endif

/* The slave's lists are the bottom of the master's buffers (saturn_cmdbuf.c):
 * in a split frame the slave writes cmd_m [0, M), cmd_s [0, S) and visit
 * ranges [0, V); the master's counts start at M / S / V. Multiples of 256 /
 * 256 / 4 (immediates in saturn_hw_asm.s); the makefile assembles the
 * slave's asm copy with the same values (--defsym HW_CMDM_CAP=...). */
#define HW_W2_M_CAP      512
#define HW_W2_S_CAP      512
#define HW_W2_VISIT_CAP  192

enum {
    HW_W2_RPC_TEX = 1,      /* a0 texnum, a1 half request -> r0 srca, r1 tw, r2 th */
    HW_W2_RPC_FLAT,         /* a0 flatnum -> r0 srca */
    HW_W2_RPC_FLATVAR,      /* a0 flatnum, a1 variant -> r0 srca */
    HW_W2_RPC_FLATAVG,      /* a0 flatnum -> r0 colour */
    HW_W2_RPC_SPR,          /* a0 scache -> r0 srca, r1 pw, r2 h */
    HW_W2_RPC_SPRPATCH      /* a0 s -> r0 ptr, r1 w, r2 h, r3 lofs, r4 tofs */
};

typedef struct {
    /* control (each written by one side only) */
    volatile uint32_t seq;          /* master: ++ starts a slave walk */
    volatile uint32_t done;         /* slave: = seq once its outputs are published */
    volatile uint32_t rpc_req;      /* slave: ++ per request */
    volatile uint32_t rpc_ack;      /* master: = rpc_req when served */
    volatile int32_t  rpc_fn;
    volatile int32_t  rpc_a[3];
    volatile int32_t  rpc_r[6];

    /* frame inputs (master) */
    int32_t  x0, x1;                /* slave columns [x0, x1) */
    int32_t  viewx, viewy, viewz;
    uint32_t viewangle;
    int32_t  viewsin, viewcos;
    int32_t  cam_x, cam_y;
    int32_t  cz_origin, cx_origin;
    int32_t  displayplayer;
    const uint8_t *pvs_row;
    void    *wall_tags, *flat_tags, *spr_tags;   /* cache-through aliases */

    /* outputs (slave) */
    void    *cmd_m, *cmd_s, *ranges;             /* its lists (cached addresses) */
    int32_t  m_count, s_count, visit_count, flat_quads;
    int32_t  needs_ytrim, sky_visible;
    uint32_t busy;                  /* slave FRT ticks spent in the walk */
    uint32_t wall_quads, wall_strips;
    uint32_t rpcs;                  /* requests this frame */
} hw_w2_mail_t;

extern hw_w2_mail_t hw_w2_mail;     /* saturn_hw_render.o */
#define HW_W2_MAIL ((volatile hw_w2_mail_t *)((uintptr_t)&hw_w2_mail | 0x20000000u))

/* slave copy entry points (saturn_hw_walk2.o; the only globals it keeps) */
void hw_w2_entry(void);             /* slave CPU: one split frame */
void hw_w2_init(void);              /* master CPU, once: buffers for the copy */

#endif /* SATURN_HW_WALK2_H */
