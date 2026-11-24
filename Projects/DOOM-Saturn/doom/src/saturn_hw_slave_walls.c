/* saturn_hw_slave_walls.c - the slave SH-2's copy of the deferred wall
 * emitters (HW_SLAVE_WALLS, 2026-11-16).
 *
 * The wall emitters in saturn_hw_walls.c work on file-scope state: the
 * emit context hw_wctx, the hw_wall_coord_tmp scratch, the light delta, the
 * solid mask (all static), and they append to saturn_cmd_m /
 * saturn_cmd_m_count and bump a few frame counters (extern). Compiling the
 * same sources a second time here gives the slave private copies of every
 * static; the extern names below are mapped to slave-only variables, and
 * saturn_cmd_m points at the slave's region after the master's
 * CMDBUF_M_CAP slots. Only HW_SlaveWallsService (called from the slave's
 * idle loop) and what it reaches is emitted.
 *
 * The makefile weakens every global symbol of this object (objcopy
 * --weaken), so the header-defined globals duplicated here (counters,
 * _hw_purge_cache, ...) resolve to the master object's definitions.
 */
#if defined(HW_SLAVE_WALLS) && HW_SLAVE_WALLS && !defined(SATURN_WIN32)

#define HW_SLAVE_TU 1
/* the asm kernels address the master's lists and state through their
 * context tables: this copy uses the C emitters */
#define HW_ASM 0

#include "saturn_cmdbuf.h"
#include "saturn_vdp1.h"

#define HW_SW_M_BASE CMDBUF_M_CAP          /* first slave slot in the real saturn_cmd_m */
#undef  CMDBUF_M_CAP
#define CMDBUF_M_CAP CMDBUF_SW_CAP         /* the emitters' capacity checks: our region */

#define saturn_cmd_m              hw_sw_cmd
#define saturn_cmd_m_count        hw_sw_cmd_count
#define saturn_cmd_m_needs_ytrim  hw_sw_needs_ytrim
#define saturn_hw_wall_quads      hw_sw_wall_quads
#define saturn_hw_wall_strips     hw_sw_wall_strips
#define saturn_hw_tex_miss        hw_sw_tex_miss
#define saturn_v1w_cells          hw_sw_v1w_cells
#define saturn_v1w_drop           hw_sw_v1w_drop
#define saturn_v1w_captured       hw_sw_v1w_captured

vdp1_cmd_t *hw_sw_cmd = 0;
volatile uint16_t hw_sw_cmd_count = 0;   /* same type as saturn_cmd_m_count */
uint8_t     hw_sw_needs_ytrim = 0;
int         hw_sw_v1w_cells = 0;
int         hw_sw_v1w_drop = 0;
int         hw_sw_v1w_captured = 0;
/* hw_sw_wall_quads / _strips / _tex_miss: defined by saturn_hw_internal.h */

#include "saturn_hw_internal.h"
#include "saturn_hw_occl.c"
#include "saturn_hw_math.h"
#include "saturn_hw_sprites.c"
#include "saturn_hw_flats.c"
#include "saturn_hw_walls.c"

#endif
