! saturn_draw.s — hand-scheduled SH-2 pixel kernels for saturn_render.c.
!
! Ported from d32xr's sh2_draw.s (Chilly Willy's 32X DOOM drawers — the
! reason the 32X build is fast on one SH-2). Same ISA, same machine
! class; the data layout differs (our textures/flats are ROW-major, the
! 32X's were column-major), so these are adapted rather than copied.
! Both kernels are bit-identical to the C loops they replace:
!
!   saturn_wall_col_asm  <->  paint_wall_span's per-pixel loop
!       (pow2 textures only: row*w = row << log2w; NPO2 stays in C)
!   saturn_flat_row_asm  <->  paint_flat_run's inner row loop
!       (dmuls.l low word == the C `col_wxs[i] * d` 32-bit product,
!        swap.w+exts.w >>16 == the C arithmetic shift — bit-exact)

	.section .text
	.align	4

! --------------------------------------------------------------------------
! void saturn_wall_col_asm(byte *fb, const byte *cmaprow, const byte *tex,
!                          int count, int v0, int vstep, int vh,
!                          int w, int utex)
! --------------------------------------------------------------------------
	.global _saturn_wall_col_asm
_saturn_wall_col_asm:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.l	@(28,r15),r3	! v0
	mov.l	@(32,r15),r8	! vstep
	mov.l	@(36,r15),r9	! vh
	mov.l	@(40,r15),r13	! w, the texture width (pow2)
	mov.l	@(44,r15),r10	! utex
	mov	#80,r11
	shll	r11
	shll	r11		! r11 = 320 = SCREEN_W
	mov	r9,r12
	shlr16	r12		! h  (vh>>16; pow2: h-1 is the row mask)
	add	#-1,r12		! h-1

.Lwall_loop:
	swap.w	r3,r0		! r0 = v0>>16 (low word) | v0<<16 (HIGH BITS!)
	extu.w	r0,r0		! clear the swapped-up high half
	and	r12,r0		! row & (h-1)
	dmuls.l	r13,r0		! row * w
	sts	macl,r0		! low word == row*w
	add	r10,r0		! + utex
	mov.b	@(r0,r6),r0	! texel = tex[row*w + utex]
	extu.b	r0,r0		! unsigned
	add	r8,r3		! v0 += vstep
	cmp/ge	r9,r3		! v0 >= vh ?
	bf/s	.Lwall_nw
	mov.b	@(r0,r5),r14	! dpix = cmaprow[texel]   (delay slot)
	sub	r9,r3		! wrap: v0 -= vh
.Lwall_nw:
	mov.b	r14,@r4		! *fb = dpix
	dt	r7		! count--
	add	r11,r4		! fb += SCREEN_W
	bf/s	.Lwall_loop
	nop

	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! --------------------------------------------------------------------------
! void saturn_flat_row_asm(byte *fb, int count,
!                          const fixed_t *wxs, const fixed_t *wys,
!                          int viewx_u, int viewy_u, int d,
!                          const byte *flat, const byte *cmaprow)
! --------------------------------------------------------------------------
	.global _saturn_flat_row_asm
_saturn_flat_row_asm:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.l	@(28,r15),r8	! viewx_u
	mov.l	@(32,r15),r9	! viewy_u
	mov.l	@(36,r15),r10	! d
	mov.l	@(40,r15),r11	! flat
	mov.l	@(44,r15),r12	! cmaprow

.Lflat_loop:
	mov.l	@r6+,r0		! wxs[i]   (post-increment)
	dmuls.l	r10,r0		! 48-bit wxs*d
	sts	macl,r0		! low 32 (== the C 32-bit product)
	swap.w	r0,r0		! >>16 (arithmetic) is swap.w + exts.w
	exts.w	r0,r0
	add	r8,r0		! + viewx_u
	and	#63,r0		! wx & 63
	mov	r0,r2		! save wx&63 (and #imm is r0-only)
	mov.l	@r7+,r0		! wys[i]
	dmuls.l	r10,r0		! 48-bit wys*d
	sts	macl,r0
	swap.w	r0,r0		! >>16 arithmetic
	exts.w	r0,r0
	add	r9,r0
	and	#63,r0		! wy & 63
	shll8	r0		! (wy&63)<<8
	shlr2	r0		! <<6
	or	r2,r0		! spot
	mov.b	@(r0,r11),r0	! texel = flat[spot]
	extu.b	r0,r0		! unsigned
	mov.b	@(r0,r12),r1	! dpix = cmaprow[texel]
	mov.b	r1,@r4		! *fb = dpix
	dt	r5		! count--
	add	#1,r4
	bf/s	.Lflat_loop
	nop

	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! --------------------------------------------------------------------------
! void saturn_flat_step_asm(byte *fb, int count, int u_fx, int v_fx,
!                           int ustep, int vstep, const byte *flat,
!                           const byte *cmaprow)
! --------------------------------------------------------------------------
	.global _saturn_flat_step_asm
_saturn_flat_step_asm:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.l	@(28,r15),r8	! ustep (.16 texels/px)
	mov.l	@(32,r15),r9	! vstep
	shll2	r9
	shll2	r9
	shll2	r9		! vstep6 = vstep << 6 (v runs pre-shifted)
	mov.l	@(36,r15),r10	! flat
	mov.l	@(40,r15),r11	! cmaprow
	mov	#63,r12
	mov	#63,r3
	shll2	r3
	shll2	r3
	shll2	r3		! r3 = 63<<6 = 0x0FC0: v mask AFTER swap.w
	mov	r6,r13		! u_fx (arg3 arrives in r6)
	mov	r7,r14		! v_fx (arg4 arrives in r7)
	shll2	r14
	shll2	r14
	shll2	r14		! v6 = v_fx << 6

	mov	r5,r0
	tst	#1,r0		! T=1 when count is even
	bt	.Lflatstep_even	! even: skip the single pixel
	swap.w	r13,r0		! u_fx>>16 (bits 31:16) in low word
	and	r12,r0		! u & 63
	swap.w	r14,r2		! v6>>16: (v&63)<<6 via the r3 mask
	and	r3,r2
	or	r2,r0		! spot
	mov.b	@(r0,r10),r0	! texel = flat[spot]
	extu.b	r0,r0
	mov.b	@(r0,r11),r0	! dpix = cmaprow[texel]
	mov.b	r0,@r4		! *fb = dpix
	add	r8,r13		! u_fx += ustep
	add	r9,r14		! v6 += vstep6
	add	#1,r4
	dt	r5		! count--; T=1 iff count was 1
	bt	.Lflatstep_exit	! the single pixel was the whole run
	nop
.Lflatstep_even:		! r5 = remaining pixel count (even)
	mov	r5,r0
	shlr	r0		! the pair loop counts PAIRS
	mov	r0,r5

	.balign	4
.Lflatstep_pair:
	! pixel A
	swap.w	r13,r0		! u_fx>>16
	and	r12,r0		! u & 63
	swap.w	r14,r2		! v6>>16: (v&63)<<6 via the r3 mask
	and	r3,r2
	or	r2,r0		! spot
	mov.b	@(r0,r10),r0	! texel = flat[spot]
	extu.b	r0,r0
	mov.b	@(r0,r11),r0	! dpix = cmaprow[texel]
	mov.b	r0,@r4		! *fb = dpix
	add	r8,r13		! u_fx += ustep
	add	r9,r14		! v6 += vstep6
	! pixel B
	swap.w	r13,r0
	and	r12,r0
	swap.w	r14,r2
	and	r3,r2
	or	r2,r0
	mov.b	@(r0,r10),r0
	extu.b	r0,r0
	mov.b	@(r0,r11),r0
	mov.b	r0,@(1,r4)	! fb[1]
	add	r8,r13
	add	r9,r14
	dt	r5		! pair consumed
	bf/s	.Lflatstep_pair
	add	#2,r4		! delay slot: fb += 2 (r4 is dead after exit)

.Lflatstep_exit:
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! --------------------------------------------------------------------------
! unsigned __udivsi3(unsigned aa, unsigned bb)
! Preserves r2-r7 strictly and clamps quotients >= 2^30 to 0x7fffffff.
! --------------------------------------------------------------------------
	.balign 4
	.global ___udivsi3
	.global __udivsi3
___udivsi3:
__udivsi3:
	mov	r4,r1
	shlr16	r1
	shll2	r1
	shlr16	r1
	cmp/hs	r5,r1
	bt	.L_udiv_clamp
	mov.l	r4,@-r15
	mov.l	r5,@-r15
	cmp/hi	r5,r4
	bf/s	.L_udiv_loop_init
	mov	#1,r1
.L_udiv_shift:
	mov	r5,r0
	shll	r0
	bt	.L_udiv_loop_init
	add	r5,r5
	cmp/hi	r5,r4
	bt/s	.L_udiv_shift
	add	r1,r1
.L_udiv_loop_init:
	mov	#0,r0
.L_udiv_loop:
	cmp/hs	r5,r4
	bf	.L_udiv_no_sub
	sub	r5,r4
	or	r1,r0
.L_udiv_no_sub:
	shlr	r5
	shlr	r1
	tst	r1,r1
	bt/s	.L_udiv_done
	tst	r4,r4
	bf	.L_udiv_loop
.L_udiv_done:
	mov.l	@r15+,r5
	rts
	mov.l	@r15+,r4
.L_udiv_clamp:
	mov.l	.L_udiv_max,r0
	rts
	nop
	.balign 4
.L_udiv_max:
	.long	0x7fffffff
