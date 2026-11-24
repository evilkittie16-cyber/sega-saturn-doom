/* saturn_hw_walk2.c - the slave SH-2's private copy of the renderer walk
 * (HW_WALK2, two-CPU screen split, 2026-11-24).
 *
 * In a split frame both CPUs walk the BSP for their own columns: the
 * master renders [0, x) with saturn_hw_render.o, the slave renders [x, 320)
 * with this object. The renderer keeps its per-frame state in file-scope
 * variables (occlusion masks, aperture, sector cache, emit contexts, the
 * asm kernels' ctx tables), so the slave gets a second compiled copy of the
 * whole renderer TU:
 *   - this file includes saturn_hw_render.c with HW_WALK2_TU defined
 *     (master-only code compiled out, per-walker externals renamed),
 *   - the makefile partially links it with saturn_hw_asm.o, so the asm
 *     kernels' ctx references bind to THIS copy's tables,
 *   - then localizes every global except the walker entry points:
 *     everything defined here is private to the slave copy, and every
 *     undefined reference (level data, game state, the VDP1 texture cache)
 *     binds to the shared master definitions.
 * State the two copies must share (configuration, level data the master
 * builds at level start) is declared extern under HW_WALK2_TU at its
 * definition (HW_W2_SHARED in saturn_hw_internal.h).
 * The slave never touches the VDP1 texture allocator or the WAD/cart
 * loaders: texture misses go to the master through a mailbox
 * (saturn_hw_walk2_rpc in saturn_hw_render.c).
 */
#if defined(HW_WALK2) && HW_WALK2 && !defined(SATURN_WIN32)
#define HW_WALK2_TU 1
#include "saturn_hw_render.c"
#endif
