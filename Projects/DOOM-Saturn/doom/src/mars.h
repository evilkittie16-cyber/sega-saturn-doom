/* mars.h — stub for d32xr's 32X include. The Saturn replaces the 32X's
 * dual-SH-2 with its own dual-SH-2, but the SH-2/68EC000 bus tricks
 * (cache hints, COMM register mbox) are not ported yet. So d32xr's
 * sources can be compiled without modification, but every "mars_*"
 * call is a no-op on Saturn.
 *
 * The Saturn's slave SH-2 runs d32xr's "slave" column-drawer code in
 * a later phase; for now we single-thread. */
#ifndef __MARS_H_SATURN__
#define __MARS_H_SATURN__

#include <stdint.h>

/* Cache hint macros — Saturn has no equivalent, so no-op. */
#define MARS_ATTR_CACHE_OFF
#define MARS_ATTR_CACHE_ON
#define MARS_ATTR_USER_CACHE_OFF
#define MARS_ATTR_USER_CACHE_ON

/* Cache control. Saturn's SH-2 has its own cache, but we leave it
 * at the Jo Engine default. */
static inline void MARS_ClearCache(void *p, int n) { (void)p; (void)n; }
static inline void MARS_StoreCache(void *p, int n) { (void)p; (void)n; }

/* Slave CPU. On Saturn we don't drive a slave yet, but the slave
 * SH-2 is present and running d32xr's slave code (configured by
 * JO_COMPILE_WITH_DUAL_CPU_MODULE=1). The 32X's COMM register
 * mbox is replaced by Saturn's shared 1 MB work RAM. d32xr's
 * "MasterSH2 / SlaveSH2" split is compiled but the slave's
 * output is currently ignored.
 *
 * d32xr uses MARS_GetCpuID() / MARS_Send/RecvFifo / etc. to
 * dispatch. We stub them as no-ops. */
static inline int  MARS_GetCpuID(void)        { return 0; }
static inline int  MARS_InitSlave(void (*fn)(void)) { (void)fn; return 0; }
static inline void MARS_WaitForSlave(void)    { }
static inline void MARS_SendFifo(int ch, int v){ (void)ch; (void)v; }
static inline int  MARS_RecvFifo(int ch)      { (void)ch; return 0; }

/* d32xr uses MARS_VblankTiming to time frame work. On Saturn the
 * equivalent is jo_get_ticks(). */
static inline int  MARS_GetTicks(void)        { extern unsigned int jo_get_ticks(void); return (int)jo_get_ticks(); }

/* MARS_VideoReset — reset VRAM. The Saturn version is in i_video.c. */
static inline void MARS_VideoReset(void)      { }

/* 32X-compatibility types the .c files reference. */
typedef struct { int dummy; } MARS_Mbox;

/* Saturn SH-2 hardware divider — REAL registers (the SH7604 core in
 * each Saturn SH-2 has the same divide unit as the 32X's SH-2; d32xr
 * 32x.h:143-146 is the address authority). Writing SH2_DIVU_DVDNT
 * starts a signed 32/32 divide; SH2_DIVU_DVDNTH then DVDNTL starts
 * 64/32 (the form FixedDiv needs). The quotient is read back from the
 * same register ~13-16 cycles later — leave independent work between
 * the start write and the read, like d32xr's MARS asm does. The old
 * stub mapped these to plain zero-initialized variables, which would
 * have silently returned garbage had any ported d32xr code run. */
#define SH2_DIVU_DVSR   (*(volatile int *)0xFFFFFF00u)
#define SH2_DIVU_DVDNT  (*(volatile int *)0xFFFFFF04u)
#define SH2_DIVU_DVDNTH (*(volatile int *)0xFFFFFF10u)
#define SH2_DIVU_DVDNTL (*(volatile int *)0xFFFFFF14u)

#endif