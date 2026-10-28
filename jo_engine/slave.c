/*
** Jo Sega Saturn Engine
** Copyright (c) 2012-2024, Johannes Fetz (johannesfetz@gmail.com)
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of the Johannes Fetz nor the
**       names of its contributors may be used to endorse or promote products
**       derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
** DISCLAIMED. IN NO EVENT SHALL Johannes Fetz BE LIABLE FOR ANY
** DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
** (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
** LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
** ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
** INCLUDES
*/
#include <stdbool.h>
#include "jo/sgl_prototypes.h"
#include "jo/conf.h"
#include "jo/types.h"
#include "jo/sega_saturn.h"
#include "jo/smpc.h"
#include "jo/math.h"
#include "jo/tools.h"
#include "jo/core.h"
#include "jo/malloc.h"
#include "jo/list.h"

#ifdef JO_COMPILE_WITH_DUAL_CPU_SUPPORT

/*
** INTERNAL MACROS
*/
# define SETSINT(_Num_, _Hdr_) ((**(void(**)(unsigned int, void *))0x6000310)((_Num_), (_Hdr_)))
# define IPRA ((unsigned short *)0xfffffee2)
# define IPRB ((unsigned short *)0xfffffe60)
# define TIER ((unsigned char *)0xfffffe10)
# define FTCSR ((unsigned char *)0xfffffe11)

jo_list                             __slave_callbacks;

inline int			jo_core_add_slave_callback(const jo_event_callback callback)
{
    jo_node         *node;

#ifdef JO_DEBUG
    if (callback == JO_NULL)
    {
        jo_core_error("callback is null");
        return (0);
    }
#endif
    node = jo_list_add_ptr(&__slave_callbacks, callback);
    if (node == JO_NULL)
        return (0);
    return (int)node;
}

inline void    jo_core_remove_slave_callback(const int event_id)
{
#ifdef JO_DEBUG
    if (event_id == 0)
    {
        jo_core_error("Invalid event_id (%d)", event_id);
        return;
    }
#endif
    jo_list_remove(&__slave_callbacks, (jo_node *)event_id);
}

#if JO_COMPILE_USING_SGL

volatile bool __jo_slave_done_flag = false;

void                                jo_core_slave_init(void)
{
    jo_list_init(&__slave_callbacks);
}

static __jo_force_inline void      jo_core_slave_bootstrap(void *param)
{
    ((jo_slave_callback)param)();
    /* DOOM-Saturn (2026-09-03): write the done flag through the
     * UNCACHED mirror. The callback's exit purge already ran by the
     * time this line executes, so a plain cached write sits dirty in
     * the SLAVE's operand cache and only reaches RAM when the line
     * happens to be evicted — the master polls the uncached mirror and
     * spins on join for that whole eviction latency (measured as a
     * ~7.9 ms 'j' tail). The slave never re-reads the flag, so the
     * stale cached copy (if any) is harmless. */
    *((volatile bool *)((unsigned int)&__jo_slave_done_flag | 0x20000000)) = true;
}

inline void                        jo_core_wait_for_slave(void)
{
    while (!*((volatile bool *)((unsigned int)&__jo_slave_done_flag | 0x20000000)))
        ;
    *((volatile bool *)((unsigned int)&__jo_slave_done_flag | 0x20000000)) = false;
}

inline void                        jo_core_exec_on_slave(jo_slave_callback callback)
{
    *((volatile bool *)((unsigned int)&__jo_slave_done_flag | 0x20000000)) = false;
    /* DOOM-Saturn round-59 (2026-09-04): purge BEFORE the post too.
     * The frozen state file (round-58 verdict) has the master wedged
     * inside SGL's SlaveControl while the slave storm-rejects in the
     * callback: every byte the master wrote before the dispatch (the
     * heart shadow, the SGL mailbox words themselves, camera/segwall/
     * clip tables the slave is about to read) sat dirty in the
     * MASTER's OC for the whole dispatch, racing the slave's reads.
     * The round-34 purge below only runs AFTER slSlaveFunc returns —
     * useless for this dispatch if the join wedges. SGL's own purge is
     * the one cache operation proven safe on real silicon. */
    slCashPurge();
    slSlaveFunc(jo_core_slave_bootstrap, callback);
    /* DOOM-Saturn round-34 (2026-09-04): flush the dispatch out of the
     * MASTER's operand cache NOW. slSlaveFunc posts the slave's
     * function pointer with a cached store; the slave polls the
     * mailbox through the uncached mirror, and a store left dirty in
     * the master's OC reaches WRAM only when the line happens to be
     * evicted. Usually the next few hundred cycles of master work win
     * that race — but when they don't, the slave spins on a stale "no
     * job" forever while the master's watchdog paints the red screen
     * (2026-09-04 flight record: V=112 climbing, H=0, E=0 — the slave
     * callback never ran at all). This also guarantees the slave's
     * entry purge re-reads fresh camera/clip data written during the
     * master's pre-dispatch half. */
    slCashPurge();
}

#else //!JO_COMPILE_USING_SGL

static jo_slave_callback           __jo_current_callback = JO_NULL;

inline void                        jo_core_wait_for_slave(void)
{
    while ((*(volatile unsigned short *)0x21000000));
}

inline void                        jo_core_exec_on_slave(jo_slave_callback callback)
{
    __jo_current_callback = callback;
    *(volatile unsigned short *)0x21000000 = 0xffff;
}

static inline unsigned int	get_sr(void)
{
    unsigned int	sr;

    asm volatile ( "stc\tsr,%0": "=r" (sr) );
    return (sr);
}

static inline void	set_sr2(unsigned int sr)
{
    asm volatile ( "ldc\t%0,sr":: "r" (sr) );
}

static inline void	set_imask(unsigned int imask)
{
    unsigned int	sr = get_sr();

    sr &= ~0x000000f0;
    sr |= (imask << 4);
    set_sr2(sr);
}

static void     __jo_slave_cpu_main(void)
{
    set_imask(0xF);
    *IPRA = 0x0000;
    *IPRB = 0x0000;
    *TIER = 0x01;
    for (;;)
    {
        if ((*FTCSR & 0x80) == 0x80)
        {
            jo_slave_callbacks();
            //if (__jo_current_callback != JO_NULL)
            //    __jo_current_callback();
            //(*(void(*)(void))*(void**)((unsigned int)&__jo_current_callback + 0x20000000))();
            *FTCSR = 0x00;
        }
    }
}

static __jo_force_inline void       jo_core_slave_init(void)
{
    jo_smpc_begin_command();
    jo_smpc_end_command(DisableSlaveCPU);
    jo_spin_wait(10);
    SETSINT(0x94, (void*)&__jo_slave_cpu_main);
    jo_smpc_begin_command();
    jo_smpc_end_command(EnableSlaveCPU);
}

#endif

#endif //!JO_COMPILE_WITH_DUAL_CPU_SUPPORT
