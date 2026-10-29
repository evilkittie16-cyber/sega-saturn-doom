/* saturn_render.c — Frame telemetry counters and level invalidation hook.
 *
 * The legacy CPU software raycaster was retired in favor of the VDP1/VDP2
 * hardware renderer (saturn_hw_render.c). This module retains the level
 * reset hook (Saturn_RenderInvalidateLevel) and the frame-timing / watchdog
 * globals referenced by i_video.c, d_main.c, and g_game.c.
 */

#include "doomdef.h"
#include "r_local.h"
#include "saturn_segs.h"

/* Keep LIBCPK.A(slavesh.o) archive resolution order identical for SGL slave dispatch */
extern volatile unsigned int SlaveCommand;

void Saturn_RenderInvalidateLevel(void)
{
    (void)SlaveCommand;
}

/* --- Frame & Watchdog Telemetry Globals (referenced by d_main.c / g_game.c / i_video.c) --- */
volatile unsigned int saturn_master_beat    = 0;
volatile unsigned int saturn_master_site    = 0;
volatile unsigned int saturn_frame_open     = 0;
volatile int          saturn_freeze_watch   = 0;
volatile unsigned int saturn_iter_count     = 0;
volatile unsigned int saturn_tic_count      = 0;
volatile unsigned int saturn_sim_site       = 0;
volatile unsigned int saturn_frame_done_vbl = 0;
volatile unsigned int saturn_vbl_dead       = 0;
volatile unsigned int saturn_load_count     = 0;
volatile unsigned int saturn_frame_count    = 0;

/* --- FRT Sub-Phase Accumulators (referenced by i_video.c / saturn_segs.h) --- */
volatile unsigned int saturn_spr_frt_acc  = 0;
volatile unsigned int saturn_flu_frt_acc  = 0;
volatile unsigned int saturn_blit_frt_acc = 0;
volatile unsigned int saturn_pre_frt_acc  = 0;
unsigned int          saturn_spr_frt_last = 0;
unsigned int          saturn_flu_frt_last = 0;
unsigned int          saturn_blit_frt_last = 0;
unsigned int          saturn_pre_frt_last = 0;

/* --- Legacy HUD Diagnostic Counters (referenced by i_video.c under SATURN_PERF_ROWS) --- */
int                   saturn_dbg_red_events   = 0;
int                   saturn_dbg_red_stallvbl = 0;
int                   saturn_dbg_red_heart    = -1;
volatile unsigned int sat_divlut_lhits        = 0;
volatile unsigned int sat_divlut_dfallback    = 0;
volatile int          saturn_dbg_s_clip       = 0;

int                   saturn_fd_emit  = 0;
int                   saturn_fd_paint = 0;
int                   saturn_fd_bad   = 0;
int                   saturn_fd_stage = 0;
int                   saturn_fd_flush = 0;

int                   saturn_frt_vmerge = 0;
int                   saturn_frt_mbook  = 0;
int                   saturn_cnt_femit  = 0;

volatile saturn_mm_dbg_t saturn_mm_dbg[2] __attribute__((aligned(16)));
volatile int          saturn_mm_zfull[3] = { -1, -1, -1 };
volatile int          saturn_mm_zcol     = -1;
volatile int          saturn_mm_ztex     = -1;

volatile int          saturn_prof_setup      = 0;
volatile int          saturn_prof_join       = 0;
volatile int          saturn_prof_slave      = 0;
volatile int          saturn_prof_active     = 0;
volatile int          saturn_frt_slavehalf   = 0;
volatile int          saturn_slave_frt_cal   = 0;
volatile int          saturn_frt_join        = 0;
volatile int          saturn_frt_masterhalf  = 0;
volatile int          saturn_split_x         = 160;
volatile int          saturn_frt_prea        = 0;
volatile int          saturn_frt_preb        = 0;
volatile int          saturn_prof_sccr0      = -1;
volatile int          saturn_prof_sccr1      = -1;
volatile int          saturn_prof_mccr       = -1;
volatile int          saturn_prof_pre        = 0;
volatile int          saturn_frt_disp_pre    = 0;
volatile int          saturn_frt_disp_post   = 0;
volatile int          saturn_frt_joinvbl     = 0;

int                   saturn_cnt_scan   = 0;
int                   saturn_cnt_solve  = 0;
int                   saturn_cnt_cross  = 0;
int                   saturn_cnt_span   = 0;
int                   saturn_cnt_fruns  = 0;
int                   saturn_cnt_frows  = 0;
int                   saturn_cnt_fpx    = 0;
int                   saturn_frt_fire   = 0;
int                   saturn_frt_scan   = 0;
int                   saturn_frt_walk   = 0;
int                   saturn_frt_total  = 0;
int                   saturn_frt_stage  = 0;
int                   saturn_frt_paint  = 0;
int                   saturn_frt_emit   = 0;
int                   saturn_frt_flush  = 0;
int                   saturn_frt_merge  = 0;
int                   saturn_frt_ploop  = 0;
int                   saturn_frt_pre    = 0;
int                   saturn_frt_cl     = 0;
int                   saturn_frt_segb   = 0;
int                   saturn_frt_segb2  = 0;
int                   saturn_frt_pre_acc = 0;
