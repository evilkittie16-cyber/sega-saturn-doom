! saturn_hw_asm.s — hand-written SH-2 kernels for the hardware renderer
! (saturn_hw_*.c). Each routine is bit-identical to the C it replaces; the
! C stays as the reference and as the DOOM-Windows (MSVC) path. Switch:
! HW_ASM (saturn_hw_internal.h), default 1 on Saturn.
!
! Conventions: GCC SH ABI. r0-r7 scratch, r8-r14 callee-saved, args r4-r7,
! return r0, pr saved by the caller of a leaf. Byte compares use the
! sign-extended mov.b value with UNSIGNED compares (cmp/hi, cmp/hs): the
! mapping byte -> sign-extended 32-bit keeps unsigned order, so no extu.b
! is needed inside loops.

	.section .text
	.align	4

! --------------------------------------------------------------------------
! void hw_asm_rescan(uint32_t touched, const uint32_t *solid_mask,
!                    hw_ap_word_t *ap_words, const uint8_t *ytop)
!
! == HW_RescanApertureWords (saturn_hw_occl.c, HW_AP_HALF=0) minus the
! ap->dirty_words update (done by the C caller). ybot = ytop + 352
! (HW_MAX_SCREEN_W); 10 words x 32 columns (HW_VIEW_W 320). Per touched
! word: min/max of ytop[] and ybot[] over the non-solid columns, written
! to hw_ap_word_t {yt_min, yb_max, yt_max, yb_min} (int16 each).
! Registers: r3 words left, r4 touched (shifted), r5 &solid[w], r6 &aw[w],
! r7 &ytop[w*32], r8 min_yt, r9 max_yt, r10 min_yb, r11 max_yb,
! r12/r13 column pointers, r14 = 352, r1 column counter.
! --------------------------------------------------------------------------
	.global _hw_asm_rescan
_hw_asm_rescan:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.w	.Lr_352,r14
	mov	#10,r3

.Lr_word:
	tst	r4,r4
	bt	.Lr_done		! no touched words left
	shlr	r4
	bf	.Lr_next		! word not touched
	mov.l	@r5,r0			! sm
	mov	r7,r12			! ytop column pointer
	cmp/eq	#-1,r0
	bt/s	.Lr_full
	mov	r7,r13
	tst	r0,r0
	bf/s	.Lr_masked
	add	r14,r13			! ybot column pointer

	! ---- sm == 0: all 32 columns open; seed from column 0 ----
	mov.b	@r12+,r8		! min_yt
	mov.b	@r13+,r10		! min_yb
	mov	r8,r9			! max_yt
	mov	r10,r11			! max_yb
	mov	#31,r1
.Lr_c0:
	mov.b	@r12+,r2		! yt
	mov.b	@r13+,r0		! yb
	cmp/hi	r2,r8			! min_yt > yt ?
	bt	.Lr_u0a
	cmp/hi	r9,r2			! yt > max_yt ?
	bt	.Lr_u0b
.Lr_r0b:
	cmp/hi	r0,r10			! min_yb > yb ?
	bt	.Lr_u0c
	cmp/hi	r11,r0			! yb > max_yb ?
	bt	.Lr_u0d
.Lr_r0d:
	dt	r1
	bf	.Lr_c0
	bra	.Lr_fin
	nop
.Lr_u0a:
	bra	.Lr_r0b			! a new min can't also be a new max
	mov	r2,r8
.Lr_u0b:
	bra	.Lr_r0b
	mov	r2,r9
.Lr_u0c:
	bra	.Lr_r0d
	mov	r0,r10
.Lr_u0d:
	bra	.Lr_r0d
	mov	r0,r11

	! ---- partly solid word: skip solid columns (bit x set) ----
.Lr_masked:
	mov.l	r6,@-r15
	mov	r0,r6			! sm, shifted per column
	mov	#-1,r8			! min_yt (unsigned max; >=1 column is open)
	mov	#0,r9
	mov	#-1,r10
	mov	#0,r11
	mov	#32,r1
.Lr_c1:
	mov.b	@r13+,r0		! yb
	shlr	r6			! T = column solid
	bt/s	.Lr_s1
	mov.b	@r12+,r2		! yt
	cmp/hi	r2,r8
	bt	.Lr_u1a
	cmp/hi	r9,r2
	bt	.Lr_u1b
.Lr_r1b:
	cmp/hi	r0,r10
	bt	.Lr_u1c
	cmp/hi	r11,r0
	bt	.Lr_u1d
.Lr_s1:
	dt	r1
	bf	.Lr_c1
	bra	.Lr_fin
	mov.l	@r15+,r6
.Lr_u1a:
	bra	.Lr_r1b
	mov	r2,r8
.Lr_u1b:
	bra	.Lr_r1b
	mov	r2,r9
.Lr_u1c:
	bra	.Lr_s1
	mov	r0,r10
.Lr_u1d:
	bra	.Lr_s1
	mov	r0,r11

	! ---- store {yt_min, yb_max, yt_max, yb_min} ----
.Lr_fin:
	extu.b	r8,r8
	extu.b	r9,r9
	extu.b	r10,r10
	extu.b	r11,r11
	cmp/gt	r11,r8			! min_yt > max_yb -> closed word
	bf	.Lr_store
.Lr_full:
	mov.l	.Lr_pvh,r0
	mov.l	@r0,r8			! vh
	mov	r8,r9
	mov	#0,r10
	mov	#0,r11
.Lr_store:
	mov.w	r8,@r6
	mov	r11,r0
	mov.w	r0,@(2,r6)
	mov	r9,r0
	mov.w	r0,@(4,r6)
	mov	r10,r0
	mov.w	r0,@(6,r6)
.Lr_next:
	add	#4,r5
	add	#8,r6
	dt	r3
	bf/s	.Lr_word
	add	#32,r7
.Lr_done:
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

	.align	1
.Lr_352:
	.word	352
	.align	2
.Lr_pvh:
	.long	_saturn_view_h
