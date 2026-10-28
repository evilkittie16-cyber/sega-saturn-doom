/*
 * saturn_segs.h — interface between the seg/BSP wall renderer
 * (saturn_segs.c, ported from the 32X d32xr r_phase1/r_phase2/r_phase6)
 * and the painter machinery that stays in saturn_render.c.
 *
 * The seg renderer replaces the per-column raycaster (the crossing
 * scan + walk) with the classic vanilla pipeline: one BSP traversal
 * builds a viswall list front-to-back, then each viswall paints its
 * columns once — flats through the existing run-merge staging, walls
 * through paint_wall_span. Same painters, same fb, same HUD.
 */

#ifndef SATURN_SEGS_H
#define SATURN_SEGS_H

#include "doomdef.h"

/* Round 11b masked-mid probe (defined in saturn_render.c): per-core
 * counters, 64 B stride so the two SH-2 caches never share a line.
 * Round 11c: the array itself carries aligned(16) — without it the
 * compiler may place [0] straddling a cache-line boundary and the
 * slave's writes pollute core0's counters (the impossible S=56311). */
typedef struct
{
    int px, rows, mintu, maxtu, why, utex, v0;
    int tex, sh, light;         /* what the masked painter was fed    */
    int calls, band, vstep;     /* 11c: call count, y1-y0, texels/row */
    int pad[3];                 /* 64 B total: core0 [0,64), core1 [64,128) */
} saturn_mm_dbg_t;
extern volatile saturn_mm_dbg_t saturn_mm_dbg[2];
/* Round 11c composite forensics (saturn_render.c): for the suspect
 * textures 4..6 (BRNBIGC/L/R), zfull = per-mille of zero texels in the
 * ENGINE's composite (-1 = not built yet; PC truth: 297/58/74), zcol =
 * zero count in the LAST masked call's sampled column (0..h). */
extern volatile int saturn_mm_zfull[3];
extern volatile int saturn_mm_zcol;
extern volatile int saturn_mm_ztex;
/* Round 11b fill-side capture (saturn_segs.c): lastsec sector + light
 * for the column when its masked band was flushed after the fill. */
extern volatile int saturn_mm_fill_sec;
extern volatile int saturn_mm_fill_light;

/* painters exported from saturn_render.c (were static for the
 * raycaster; the seg renderer drives them per column) */
void paint_wall_span(int x, int y0, int y1, int texnum, side_t *side,
                     int nside, int utex_raw, int peg_u, int t_u,
                     byte fallback_col, int cpu);
/* Round 10: vanilla masked mid texture (grate) — draws ONCE from the
 * peg (no vertical tiling), skips texel 0 (the composite's 0-filled
 * holes), paints nothing on a cache miss. */
void paint_wall_span_masked(int x, int y0, int y1, int texnum, side_t *side,
                            int nside, int utex_raw, int peg_u, int t_u,
                            int cpu);
void fr_emit_flat(int x, int y0, int y1, sector_t *sec, int isfloor,
                  int cpu);
void fr_merge_column(int x, int cpu);
void fr_flush_pending(int cpu);

/* BSP traversal + viswall build. Master-only, once per frame, BEFORE
 * the slave dispatch purge (the slave reads the finished list). */
void saturn_segs_build(sector_t *player_sec);

/* P4: per-level PVS blob load (E1M1 only). Called from P_SetupLevel —
 * NULL drops the previous level's row pointer, the map name loads
 * cd\E1M1.BLOB (any failure = feature off = the plain BSP walk). The
 * GFS read lives in i_wad.c; the loader body is compiled only when
 * -DSATURN_PVS is on (empty stub otherwise). */
void Saturn_PvsLevelLoad(const char *lumpname);
int  Saturn_Pvs_BlobRead(void *buf, unsigned cap);
const byte *Saturn_Pvs_GetRow(fixed_t x, fixed_t y);

/* Composite/stage every texture the frame's viswall list references,
 * on the master, before dispatch — the slave's fetches are NoLoad-only
 * and paint the solid fallback on a miss (red-strip fix). */
void saturn_segs_preload_textures(void);

/* Render every viswall over columns [x0, x1) with this core's own
 * clipbounds + staging state (cpu = 0 master, 1 slave). */
void saturn_segs_render_half(int x0, int x1, int cpu);

/* Staging hooks implemented in saturn_render.c (run-merge machinery
 * lives there; these expose it to the seg renderer). */
void saturn_seg_reset_staging(int cpu);
void saturn_seg_flush_column(int x, int cpu);
void saturn_seg_flush_pending(int cpu);

/* Round-51: per-iteration FRT-acc bail nets for the legs the walk's
 * check sites never cover (sprite pass, flush runs, blit copy rows).
 * Each loop body accumulates 16-bit FRT sub-deltas into its acc
 * (zeroed per frame by saturn_frt_bail_reset); > ~60 Kronos-vbl of
 * ticks inside ONE frame's leg = the leg wedged below per-iteration
 * granularity -> set the bail kind and return. Kinds: 5 sprite pass,
 * 6 flush runs, 7 blit copy rows (i_video.c), 8 TVSTAT flip wait
 * (i_video.c). Harmless on real HW: the FRT is dead there (~55
 * ticks/vbl — never accumulates). */
extern volatile unsigned int saturn_spr_frt_acc;
extern volatile unsigned int saturn_flu_frt_acc;
extern volatile unsigned int saturn_blit_frt_acc;
extern volatile unsigned int saturn_pre_frt_acc;
extern unsigned int saturn_spr_frt_last;
extern unsigned int saturn_flu_frt_last;
extern unsigned int saturn_blit_frt_last;
extern unsigned int saturn_pre_frt_last;

/* r86 verdict: OUT — CONVICTED. The r86 Kronos run degenerated into
 * static frames + magenta master-bail + SLV DEAD (slave parked in
 * SlaveControl in the crash state): on Kronos TCNT free-runs at full
 * speed, so ONE frame's render easily crosses the 1.3M bail in BOTH
 * cores' legs every frame. On real HW the dead FRT (~55 ticks/vbl)
 * never trips them, so the nets are useless there too. Disarmed for
 * good — the join-poll watchdog (not gated by this flag) remains the
 * in-frame reporter. */
#define SAT_FRT_NET_ARMED 0

#if SAT_FRT_NET_ARMED
#define SAT_FRT_NET(acc_, last_, kind_)                                   \
    do {                                                                  \
        unsigned int now_ =                                               \
            (unsigned int)(*(volatile unsigned short *)0xFFFFFE12u);      \
        (acc_) += (unsigned short)(now_ - (last_));                       \
        (last_) = now_;                                                   \
        if ((acc_) > 1300000u)                                            \
        {                                                                 \
            extern volatile int saturn_seg_bailed;                        \
            saturn_seg_bailed = (kind_);                                  \
            return;                                                       \
        }                                                                 \
    } while (0)
#else
#define SAT_FRT_NET(acc_, last_, kind_) do { } while (0)
#endif

#endif /* SATURN_SEGS_H */