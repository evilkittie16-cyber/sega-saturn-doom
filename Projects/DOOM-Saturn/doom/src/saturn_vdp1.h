/* saturn_vdp1.h — VDP1 scaled-sprite wall prototype (2026-09-06).
 *
 * The DoomFS2 thesis: draw walls as VDP1 distorted sprites instead of
 * per-pixel CPU column fills. The wall's top/bottom edges are straight
 * screen-space lines (scale2 is linear in x), so a single distorted-
 * sprite quad is shape-EXACT; only the texture warp is affine (the
 * perspective error DoomFS2/PSX-DOOM bounded by splitting wide walls).
 *
 * Mechanism (all raw VDP1, independent of SGL's command pipeline —
 * slSynch left the commit path in round 54, so SGL's VBlank-int kick
 * is unproven here):
 *   - The command table is written DIRECTLY into VDP1 VRAM offset 0
 *     (VDP1 always executes from there) and kicked with a PTMR=1 write
 *     mid-frame, from I_FinishUpdate.
 *   - Per-frame FB clear = VDP1 hardware erase-write: TVMR.VBE=1 +
 *     FBCR.FCM=1|FCT=1, EWDR=0x0000 (transparent), region = full
 *     512x256. Re-armed every frame (manual modes are one-shot per
 *     FBCR write). This also kills the stale boot-font bleed.
 *   - Sprite layer raised above NBG0 (slPrioritySpr0-7(5), NBG0=3) —
 *     NBG0 is opaque since the letterbox fix, so the layer order is
 *     the only thing that decides visibility.
 *   - Texture: 64x64 8bpp (CL256Bnk) synthetic pattern uploaded once
 *     to VDP1 VRAM 0x25C40000; COLR bank 0 -> CRAM[0..255] = PLAYPAL,
 *     so texel index i displays PLAYPAL[i] with zero conversion.
 *     PMOD = MSBon|ECdis|SPdis (black texels opaque, no end codes).
 *
 * Capture: saturn_segs.c records the first (nearest, near-to-far
 * walk) fully-visible one-sided solid face per frame via
 * Saturn_Vdp1Wall_Set. The quad corners use the walk's own linear
 * scalefrac/scalestep edge math, clamped to VIEW_TOP/VIEW_BOT to match
 * the software paint (paint-both validation).
 *
 * KNOWN PROTOTYPE LIMITS (documented, not bugs):
 *   - Occlusion: the sprite has no per-pixel depth vs NBG0; any seg
 *     behind it is still overdrawn if the nearest wall's quad covers
 *     it (the chosen test view has a clean nearest wall).
 *   - Affine u/v warp inside the quad (the thesis's accepted error).
 *   - v squash: the sprite maps the FULL 64-row texture over the
 *     span; the software path top-pegs with real texel heights.
 */
#ifndef SATURN_VDP1_H
#define SATURN_VDP1_H

/* Quad-table capacity — P5 Phase 1 (2026-09-10): this is now the
 * REQUESTED cell count, not an array bound. The table itself is
 * Z_Malloc'd from the cart zone in Saturn_Vdp1Wall_Init (512 cells =
 * 14 KB, zero .bss) and the live cap is the runtime
 * Saturn_Vdp1Wall_Cap() — 0 until the alloc lands (fail-open to the
 * software paint). The old 64-cell .bss array is gone; the makefile
 * pool rebate pays the .bss shrink back 1:1. The rig's worst scene was
 * 67 quads; the capture pass + reason-8 clamp need this headroom. */
#define V1W_MAX_QUADS 1536

/* Called once from I_FinishUpdate's first in-game frame (main thread,
 * SGL context): sprite-layer priorities, texture upload, erase regs. */
void Saturn_Vdp1Wall_Init(void);

/* Per-frame capture (saturn_segs.c wall walk, master half only).
 * Coordinates: VDP1-native top-left-origin screen pixels. A wall is
 * added only when its face span stayed FULLY inside the open clip band
 * at every walked column (the saturn_segs.c gate), so a quad can never
 * cover a nearer surface. One call per TEXTURE-REPETITION sub-quad
 * (the caller splits wide walls at u = k*texwidth boundaries AND at
 * the horizontal texture seams, one call per cell); quads arrive in
 * walk order (near-to-far); Frame() writes them far-to-near so nearer
 * quads draw on top. texnum selects the texture through the VDP1 VRAM
 * cache here (unusable textures SKIP the quad — the software paint
 * already shows underneath). srcrow =
 * first texture ROW the cell displays (VDP1 rows are source-
 * addressable via SRCA; 0 for the full texture), rows = how many
 * texture rows it spans (SIZE.Y). shade = COLORMAP row 0..31 from
 * the paint path's own Saturn_LightRow — quantized to the nearest
 * shaded CRAM bank (COLORMAP rows 0,4,..,28, bank k = CMDCOLR k<<8;
 * max error 2 rows at the bank boundaries). clip_x0/clip_x1: user-clip
 * x span for u-phase pieces (partial repetitions drawn EXTENDED to
 * their full texture repetition); clip_x0 == clip_x1 = no clip.
 * Returns 1 = stored, 0 = table full (saturn_v1w_captured set). */
void Saturn_Vdp1Wall_Reset(void);
/* Level start: drop both arena pages' persistent texture caches. */
void Saturn_Vdp1Wall_FlushCache(void);
int  Saturn_Vdp1Wall_Count(void);
/* P5 Phase 1: the live table capacity (512 after Init's Z_Malloc, 0
 * before it / on failure — consumers treat 0 as "no capture possible"). */
int  Saturn_Vdp1Wall_Cap(void);
int  Saturn_Vdp1Wall_CanAdd(int texnum, int count, int ew, int eh);
int  Saturn_Vdp1Wall_Add(int xa, int ya0, int ya1,
                         int xb, int yb0, int yb1,
                         int texnum, int srcrow, int rows, int shade,
                         int clip_x0, int clip_x1);
int  Saturn_Vdp1Flat_Add(int xa, int ya0, int ya1,
                         int xb, int yb0, int yb1,
                         int flatnum, int shade,
                         int clip_x0, int clip_x1);
int  Saturn_Vdp1Quad_Add(int x0, int y0,
                         int x1, int y1,
                         int x2, int y2,
                         int x3, int y3,
                         int texnum, int srcrow, int rows, int shade,
                         int clip_x0, int clip_x1);
int  Saturn_Vdp1FlatQuad_Add(int x0, int y0,
                             int x1, int y1,
                             int x2, int y2,
                             int x3, int y3,
                             int flatnum, int shade);
int  Saturn_Vdp1_GetFlatSrca(int flatnum);
void Saturn_Vdp1_PreloadFlat(int flatnum);
void Saturn_Vdp1_ClearPages(void);
int  Saturn_Vdp1_GetTexSrca(int texnum, int *tw, int *th);
void Saturn_Vdp1_ClearTexCacheMRU(void);
int  Saturn_Vdp1FlatQuad_Fast(const int *qx, const int *qy, int best_r, int srca, int k);
extern short (*v1q)[16];
extern int   v1q_cap;
extern int   v1q_count;
extern int   saturn_v1w_cells;
extern int   saturn_v1w_drop;
/* Stage 2a (2026-09-08): one MASKED MIDTEXTURE cell — the full texture
 * (srcrow 0, SIZE.Y = full height), PMOD WITHOUT SPdis so texel 0
 * draws TRANSPARENT (the grate's holes; the software open-band fill
 * behind shows through, exactly like paint_wall_span_masked's texel-0
 * skip). Draw order: appended after every wall quad = drawn under all
 * of them — correct, because farther walls' captures die on gate
 * reason 9 (seg_mmid set) and nearer walls draw later/on top. */
int  Saturn_Vdp1Wall_AddMid(int xa, int ya0, int ya1,
                            int xb, int yb0, int yb1,
                            int texnum, int shade,
                            int clip_x0, int clip_x1);

int  Saturn_Vdp1Wall_Base(void);
void Saturn_Vdp1Wall_Restore(int base);
int  Saturn_Vdp1Spr_Add(int scache, int xA, int xB, int yt, int yb,
                        int srcrow, int rows, int shade, int mark);
int  Saturn_Vdp1Quad_PspMarked(void);

/* Per-frame emit (I_FinishUpdate, after copy_back, before the NBG0
 * flip): wait EDSR.CEF, build the command table (all quads + END),
 * patch SGL's slot 3. */
void Saturn_Vdp1Wall_Frame(void);

/* "Quad table full" latch, reset per frame by saturn_segs_build. */
extern int saturn_v1w_captured;

void Saturn_Vdp1_ClearPages(void);
void Saturn_Vdp1_PreloadFlat(int flatnum);
void Saturn_Vdp1_LockPreloadedFlats(void);
int Saturn_Vdp1_GetTexSrcaMono(int texnum);

typedef struct {
    unsigned short srca;
    unsigned short srca_mono;
    unsigned short srca_64;
    unsigned char  w8;
    unsigned char  h;
} v1_tag_t;

#define V1_TAG_BAD 0xFFFFu

extern v1_tag_t *saturn_v1_flat_tags;
extern v1_tag_t *saturn_v1_wall_tags;

#endif /* SATURN_VDP1_H */