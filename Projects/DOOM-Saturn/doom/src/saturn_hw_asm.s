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

! --------------------------------------------------------------------------
! int hw_asm_spanap(int sx, int ex, const void *const *ctx)
!
! == HW_GetSpanAperture (saturn_hw_occl.c, HW_AP_HALF=0). Returns -1 (span
! closed), vh (untouched span), or (yt_min << 16) | (uint16)yb_max.
! ctx (longs): 0 solid_mask, 4 ap_words, 8 ytop (ybot = +352),
! 12 &dirty_words, 16 &hw_solid_summary, 20 hw_mask_left,
! 24 hw_mask_right (= hw_span_mask_lut + 1), 28 &saturn_view_h.
! There is no barrel shifter (no shld on SH-2): "m = sm >> b0; m & 1" is
! done as a moving test bit (1 << b0 = ml[b0] & -ml[b0]).
! --------------------------------------------------------------------------
	.align	2
	.global	_hw_asm_spanap
_hw_asm_spanap:
	add	#-1,r5			! ex_m1
	mov	r4,r0
	xor	r5,r0
	mov	#32,r1
	cmp/hs	r1,r0
	bt	.Lg_multi

	! ---- span inside one word ----
	mov	r4,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0		! w0 * 4
	mov.l	@r6,r1
	mov.l	@(r0,r1),r7		! sm
	add	r0,r0			! w0 * 8
	mov.l	@(4,r6),r3
	add	r0,r3			! aw
	mov	r4,r0
	and	#31,r0
	shll2	r0
	mov.l	@(20,r6),r1
	mov.l	@(r0,r1),r2		! ml[b0]
	mov	r5,r0
	and	#31,r0
	shll2	r0
	mov.l	@(24,r6),r1
	mov.l	@(r0,r1),r1		! mr[b1]
	and	r2,r1			! mask
	mov	r7,r0
	and	r1,r0
	cmp/eq	r1,r0
	bt	.Lg_ret_m1		! every column solid
	mov.w	@r3,r1			! yt_min
	mov.w	@(4,r3),r0		! yt_max
	sub	r1,r0
	add	#-5,r0
	cmp/pz	r0			! yt_max - yt_min > 4 ?
	bt	.Lg_scan1
	mov.w	@(6,r3),r0		! yb_min
	mov	r0,r2
	mov.w	@(2,r3),r0		! yb_max
	sub	r0,r2
	add	#4,r2
	cmp/pz	r2			! yb_max - yb_min > 4 ?
	bf	.Lg_scan1
.Lg_ret:				! r1 yt_min, r0 yb_max
	cmp/ge	r0,r1
	bt	.Lg_ret_m1
	shll16	r1
	extu.w	r0,r0
	rts
	or	r1,r0
.Lg_ret_m1:
	rts
	mov	#-1,r0

.Lg_scan1:				! per-column scan of sx..ex_m1
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	sts.l	pr,@-r15
	mov	r4,r0
	and	#31,r0
	shll2	r0
	mov.l	@(20,r6),r1
	mov.l	@(r0,r1),r2
	neg	r2,r0
	and	r0,r2			! bit = 1 << b0
	mov	r5,r3
	sub	r4,r3
	add	#1,r3			! columns
	mov.l	@(8,r6),r1
	add	r1,r4			! &ytop[sx]
	mov	r7,r1			! sm
	mov.l	@(28,r6),r0
	mov.l	@r0,r10
	exts.b	r10,r10			! yt_min = vh (byte domain)
	bsr	.Lg_cols
	mov	#0,r11			! yb_max = 0
	extu.b	r10,r1
	extu.b	r11,r0
	lds.l	@r15+,pr
	mov.l	@r15+,r11
	bra	.Lg_ret
	mov.l	@r15+,r10

! Column scan. In: r1 solid bits, r2 test bit, r3 count (>= 1), r4 &ytop[x],
! r10 yt_min / r11 yb_max in the sign-extended byte domain. Clobbers r0-r4, r7.
.Lg_cols:
	mov.w	.Lg_352,r0
.Lg_cl:
	tst	r1,r2
	bf/s	.Lg_cs			! solid column
	shll	r2
	mov.b	@r4,r7			! yt
	cmp/hi	r7,r10
	bf	.Lg_c1
	mov	r7,r10
.Lg_c1:
	mov.b	@(r0,r4),r7		! yb
	cmp/hi	r11,r7
	bf	.Lg_cs
	mov	r7,r11
.Lg_cs:
	dt	r3
	bf/s	.Lg_cl
	add	#1,r4
	rts
	nop
.Lg_352:
	.word	352

	! ---- span over several words ----
.Lg_multi:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	mov	r6,r14			! ctx
	mov.l	@r14,r8			! solid_mask
	mov	r4,r12
	shlr2	r12
	shlr2	r12
	shlr	r12			! w0
	mov	r5,r13
	shlr2	r13
	shlr2	r13
	shlr	r13			! w1
	! left word solid over [b0,31] -> start at the next word
	mov	r4,r0
	and	#31,r0
	tst	r0,r0
	bt	.Lg_m1
	shll2	r0
	mov.l	@(20,r14),r1
	mov.l	@(r0,r1),r1		! ml[b0]
	mov	r12,r0
	shll2	r0
	mov.l	@(r0,r8),r2
	and	r1,r2
	cmp/eq	r1,r2
	bf	.Lg_m1
	add	#1,r12
	mov	r12,r4
	shll2	r4
	shll2	r4
	shll	r4			! sx = w0 << 5
.Lg_m1:
	! right word solid over [0,b1] -> end at the previous word
	mov	r5,r0
	and	#31,r0
	cmp/eq	#31,r0
	bt	.Lg_m2
	shll2	r0
	mov.l	@(24,r14),r1
	mov.l	@(r0,r1),r1		! mr[b1]
	mov	r13,r0
	shll2	r0
	mov.l	@(r0,r8),r2
	and	r1,r2
	cmp/eq	r1,r2
	bf	.Lg_m2
	add	#-1,r13
	mov	r13,r5
	shll2	r5
	shll2	r5
	shll	r5
	add	#31,r5			! ex_m1 = (w1 << 5) + 31
.Lg_m2:
	cmp/gt	r13,r12
	bf	.Lg_m2a
	bra	.Lg_m_m1		! w0 > w1
	nop
.Lg_m2a:
	! nothing dirty or solid in [w0,w1] -> full view height
	mov.l	@(12,r14),r0
	mov.l	@r0,r1
	mov.l	@(16,r14),r0
	mov.l	@r0,r0
	or	r0,r1
	mov.l	@(24,r14),r2		! lut + 1
	mov	r13,r0
	shll2	r0
	mov.l	@(r0,r2),r3		! lut[w1 + 1]
	add	#-4,r2
	mov	r12,r0
	shll2	r0
	mov.l	@(r0,r2),r2		! lut[w0]
	xor	r2,r3
	tst	r3,r1
	bf	.Lg_m2b
	bra	.Lg_m_vh
	nop
.Lg_m2b:
	mov.l	@(28,r14),r0
	mov.l	@r0,r10			! yt_min = vh
	mov	#0,r11			! yb_max = 0
	mov.l	@(4,r14),r9		! ap_words
	! whole words wf0..wf1
	mov	r12,r2
	mov	r4,r0
	and	#31,r0
	tst	r0,r0
	bt	.Lg_m3
	add	#1,r2			! wf0 = w0 + 1
.Lg_m3:
	mov	r13,r3
	mov	r5,r0
	and	#31,r0
	cmp/eq	#31,r0
	bt	.Lg_m4
	add	#-1,r3			! wf1 = w1 - 1
.Lg_m4:
	cmp/gt	r3,r2
	bt	.Lg_m_left
	sub	r2,r3
	add	#1,r3			! word count
	mov	r2,r0
	shll2	r0
	mov	r8,r1
	add	r0,r1			! &solid[wf0]
	shll	r0
	mov	r9,r7
	add	r0,r7			! &aw[wf0]
.Lg_m_wl:
	mov.l	@r1+,r0
	cmp/eq	#-1,r0
	bt	.Lg_m_wn		! fully solid word
	mov.w	@r7,r0
	cmp/gt	r0,r10
	bf	.Lg_m_w1
	mov	r0,r10
.Lg_m_w1:
	mov.w	@(2,r7),r0
	cmp/gt	r11,r0
	bf	.Lg_m_wn
	mov	r0,r11
.Lg_m_wn:
	dt	r3
	bf/s	.Lg_m_wl
	add	#8,r7

	! ---- partial left word w0 (columns b0..31) ----
.Lg_m_left:
	mov	r4,r0
	and	#31,r0
	tst	r0,r0
	bt	.Lg_m_right
	mov	r0,r6			! b0
	shll2	r0
	mov.l	@(20,r14),r2
	mov.l	@(r0,r2),r2
	neg	r2,r0
	and	r0,r2			! bit = 1 << b0
	mov	#32,r0
	sub	r6,r0
	mov	r0,r6			! 32 - b0 columns
	mov	r12,r3			! word w0
	mov.l	@(8,r14),r0
	bsr	.Lg_edge
	add	r0,r4			! &ytop[sx]

	! ---- partial right word w1 (columns 0..b1) ----
.Lg_m_right:
	mov	r5,r0
	and	#31,r0
	cmp/eq	#31,r0
	bt	.Lg_m_fin
	add	#1,r0
	mov	r0,r6			! b1 + 1 columns
	mov	#1,r2			! bit 0
	mov	r13,r3			! word w1
	mov	r13,r4
	shll2	r4
	shll2	r4
	shll	r4			! w1 << 5
	mov.l	@(8,r14),r0
	bsr	.Lg_edge
	add	r0,r4			! &ytop[w1 << 5]

.Lg_m_fin:
	cmp/ge	r11,r10
	bt	.Lg_m_m1
	shll16	r10
	extu.w	r11,r0
	bra	.Lg_m_out
	or	r10,r0
.Lg_m_vh:
	mov.l	@(28,r14),r0
	bra	.Lg_m_out
	mov.l	@r0,r0
.Lg_m_m1:
	mov	#-1,r0
.Lg_m_out:
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! --------------------------------------------------------------------------
! int hw_asm_subap(const saturn_flat_bbox_t *fb, int fully_inside,
!                  hw_subap_out_t *o, const void *const *ctx)
!
! Front half of HW_CheckSubsectorAperture (saturn_hw_walls.c): bbox ->
! camera-space depth/x range, frustum culls, LOD1 choice, screen x span and
! the single-word aperture check. The C caller does the rest.
! Returns -1 culled; 0 default (no bbox data / degenerate / straddles the
! near plane): caller applies the default aperture; 1 done; 2 near-plane
! subsector not known inside: caller runs HW_CheckNearSubsectorFrustum;
! 3 caller runs HW_EvalSubsectorSpanSlow(o->sx, o->ex) for ytop/ybot.
! o (int32): 0 max_cz, 4 s_max, 8 s_min, 12 ytop_min, 16 ybot_max, 20 sx,
! 24 ex, 28 flags = fully_inside | use_lod1 (0x10) | any_behind (0x100).
! ctx (longs): 0 &hw_cam_x, 4 &hw_cam_y, 8 &hw_frust (Nx +12, Ny +14,
! cz_off12 +36, cx_off12 +40), 12 saturn_master_oclut, 16 &hw_solid_count,
! 20 &hw_aperture_state (aperture_active +2), 24 hw_solid_mask,
! 28 hw_mask_left, 32 hw_mask_right, 36 hw_ap_words, 40 &saturn_view_h.
! Locals @r15: 0 true_min_cz, 4 min_cx, 8 max_cx, 12 raw_min_cz, 16 cz_fmin,
! 20 x_cz_min, 24 y_cz_min, 28 x_cx_min, 32 y_cx_min, 36 cz_right/cx_near.
! Products are muls.w (16-bit operands) exactly like HW_MulsW_PairAdd.
! --------------------------------------------------------------------------
	.align	2
	.global	_hw_asm_subap
_hw_asm_subap:
	tst	r4,r4
	bf	.Ls_go
	rts				! no baked bboxes
	mov	#0,r0
.Ls_go:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	add	#-40,r15
	mov.w	@r4+,r8			! min_x
	mov.w	@r4+,r9			! max_x
	mov.w	@r4+,r10		! min_y
	mov.w	@r4,r11			! max_y
	cmp/eq	r8,r9
	bf	.Ls_1
	cmp/eq	r10,r11
	bt	.Ls_defE		! degenerate bbox
.Ls_1:
	mov.l	@r7,r0
	mov.l	@r0,r0			! hw_cam_x
	sub	r0,r8			! dx0
	sub	r0,r9			! dx1
	mov.l	@(4,r7),r0
	mov.l	@r0,r0			! hw_cam_y
	sub	r0,r10			! dy0
	sub	r0,r11			! dy1
	mov.l	@(8,r7),r4		! &hw_frust
	mov.w	@(12,r4),r0
	mov	r0,r12			! nx
	mov.w	@(14,r4),r0
	mov	r0,r13			! ny
	mov.l	@(36,r4),r14		! cz_off
	! max_cz = (x_cz_max*nx + y_cz_max*ny + cz_off) << 4
	cmp/pz	r12
	mov	r9,r1			! x_cz_max = nx >= 0 ? dx1 : dx0
	bt	.Ls_2
	mov	r8,r1
.Ls_2:
	cmp/pz	r13
	mov	r11,r2			! y_cz_max = ny >= 0 ? dy1 : dy0
	bt	.Ls_3
	mov	r10,r2
.Ls_3:
	muls.w	r1,r12
	sts	macl,r3
	muls.w	r2,r13
	add	r14,r3
	sts	macl,r0
	add	r0,r3
	shll2	r3
	shll2	r3			! max_cz
	mov.l	r3,@r6
	mov	#4,r1
	shll16	r1			! HW_NEAR_Z
	cmp/ge	r1,r3
	bt	.Ls_4
	cmp/pl	r8			! wholly behind the near plane:
	bt	.Ls_cullE		! culled unless the camera is inside
	cmp/pz	r9
	bf	.Ls_cullE
	cmp/pl	r10
	bt	.Ls_cullE
	cmp/pz	r11
	bt	.Ls_defE
.Ls_cullE:				! near landing pads for the early exits
	bra	.Ls_ret
	mov	#-1,r0
.Ls_defE:
	bra	.Ls_ret
	mov	#0,r0
.Ls_4:
	! raw_min_cz = x_cz_min*nx + y_cz_min*ny
	cmp/pz	r12
	mov	r8,r1			! x_cz_min = nx >= 0 ? dx0 : dx1
	bt	.Ls_5
	mov	r9,r1
.Ls_5:
	cmp/pz	r13
	mov	r10,r2			! y_cz_min = ny >= 0 ? dy0 : dy1
	bt	.Ls_6
	mov	r11,r2
.Ls_6:
	mov.l	r1,@(20,r15)
	mov.l	r2,@(24,r15)
	muls.w	r1,r12
	sts	macl,r0
	muls.w	r2,r13
	sts	macl,r1
	add	r1,r0			! raw_min_cz
	mov.l	r0,@(12,r15)
	mov	#1,r1
	shll16	r1
	shll8	r1
	shll2	r1			! HW_FAR_Z_UNITS << 12
	cmp/gt	r1,r0
	bt	.Ls_cullE
	add	r14,r0
	shll2	r0
	shll2	r0			! true_min_cz
	mov.l	r0,@r15
	! min_cx / max_cx
	cmp/pz	r13
	mov	r8,r0			! x_cx_min = ny >= 0 ? dx0 : dx1
	mov	r9,r1			! x_cx_max = ny >= 0 ? dx1 : dx0
	bt	.Ls_7
	mov	r9,r0
	mov	r8,r1
.Ls_7:
	mov.l	r0,@(28,r15)
	muls.w	r0,r13
	neg	r12,r3			! neg_nx
	sts	macl,r0
	muls.w	r1,r13
	cmp/pz	r12
	sts	macl,r1
	mov	r11,r2			! y_cx_min = nx >= 0 ? dy1 : dy0
	bt	.Ls_8
	mov	r10,r2
.Ls_8:
	mov.l	r2,@(32,r15)
	muls.w	r2,r3
	cmp/pz	r12
	sts	macl,r2
	add	r2,r0
	mov	r10,r2			! y_cx_max = nx >= 0 ? dy0 : dy1
	bt	.Ls_9
	mov	r11,r2
.Ls_9:
	muls.w	r2,r3
	sts	macl,r2
	add	r2,r1
	mov.l	@(40,r4),r2		! cx_off
	add	r2,r0
	add	r2,r1
	shll2	r0
	shll2	r0			! min_cx
	shll2	r1
	shll2	r1			! max_cx
	mov.l	r0,@(4,r15)
	mov.l	r1,@(8,r15)
	! cz_fmin: depth below which the box is outside the 90-degree frustum
	mov	#0,r2
	cmp/pl	r0
	bt	.Ls_fa
	cmp/pz	r1
	bt	.Ls_fd
	bra	.Ls_fb
	neg	r1,r2
.Ls_fa:
	mov	r0,r2
.Ls_fb:
	mov	r2,r0
	shar	r0
	shar	r0
	shar	r0
	sub	r0,r2
.Ls_fd:
	mov.l	r2,@(16,r15)
	mov.l	@r6,r3			! max_cz
	cmp/gt	r3,r2
	bt	.Ls_cullE
	mov.l	@r15,r1			! true_min_cz
	cmp/gt	r1,r2
	bt/s	.Ls_10
	mov	r2,r8			! min_cz = cz_fmin
	mov	r1,r8			! min_cz = true_min_cz
.Ls_10:
	! use_lod1 (HW_DBG_FLAT_LOD thresholds 192/240, 132/336)
	mov	#0,r9
	mov	#-64,r0
	extu.b	r0,r0			! 192
	shll16	r0
	cmp/ge	r0,r1
	bf	.Ls_lb
	mov	#-16,r0
	extu.b	r0,r0			! 240
	shll16	r0
	cmp/ge	r0,r3
	bt	.Ls_ly
.Ls_lb:
	mov	#-124,r0
	extu.b	r0,r0			! 132
	shll16	r0
	cmp/ge	r0,r1
	bf	.Ls_ln
	mov	#84,r0
	shll2	r0			! 336
	shll16	r0
	cmp/ge	r0,r3
	bf	.Ls_ln
.Ls_ly:
	mov	#16,r9			! HW_FLAT_USE_LOD1
.Ls_ln:
	mov	#0,r10			! s_min
	mov	#4,r0
	shll16	r0
	cmp/ge	r0,r1			! true_min_cz >= HW_NEAR_Z ?
	bt	.Ls_11
	! box reaches the near plane
	mov.l	.Ls_nzs,r11		! s_max = HW_NEAR_Z_SCALE
	mov	#1,r0
	shll8	r0
	or	r0,r9			! any_behind
	tst	r5,r5
	bt/s	.Ls_full
	mov	#2,r13
	bra	.Ls_full
	mov	#1,r13
.Ls_11:
	tst	r5,r5
	bt	.Ls_else
	mov.l	@(16,r7),r1
	mov.l	@r1,r1			! hw_solid_count
	mov.l	@(20,r7),r2
	mov.b	@(2,r2),r0		! aperture_active
	or	r1,r0
	tst	r0,r0
	bf	.Ls_else
	mov	r8,r0			! inside, nothing occludes yet
	bsr	.Ls_div
	mov	#1,r13
	bra	.Ls_full
	mov	r0,r11
.Ls_else:
	mov.l	@r15,r1			! true_min_cz
	mov.l	@r6,r3			! max_cz
	! cz_left = (x_cx_min*nx + y_cx_min*ny + cz_off) << 4
	mov.l	@(28,r15),r0
	muls.w	r0,r12
	mov.l	@(32,r15),r2
	sts	macl,r0
	muls.w	r2,r13
	add	r14,r0
	sts	macl,r2
	add	r2,r0
	shll2	r0
	shll2	r0
	mov	r0,r2			! cz_left
	mov	r1,r0
	add	r3,r0
	sub	r2,r0
	mov.l	r0,@(36,r15)		! cz_right = true_min + max_cz - cz_left
	mov	r1,r0
	bsr	.Ls_div
	nop
	mov	r0,r11			! s_max = div(true_min_cz)
	mov	r2,r0
	bsr	.Ls_div
	nop
	mov	r0,r14			! s_left
	mov.l	@(36,r15),r0
	bsr	.Ls_div
	nop
	mov	r0,r2			! s_right
	! cx_near = (x_cz_min*ny + y_cz_min*(-nx) + cx_off) << 4
	mov.l	@(20,r15),r0
	muls.w	r0,r13
	mov.l	@(24,r15),r1
	neg	r12,r3
	sts	macl,r0
	muls.w	r1,r3
	mov.l	@(40,r4),r1
	add	r1,r0
	sts	macl,r1
	add	r1,r0
	shll2	r0
	shll2	r0			! cx_near
	mov.l	r0,@(36,r15)
	! screen x: view_cx + hi32(cx * s)
	dmuls.l	r0,r11
	mov.l	@(4,r15),r1		! min_cx
	sts	mach,r12		! px_near
	dmuls.l	r1,r14
	mov.l	@(8,r15),r3		! max_cx
	sts	mach,r13		! sx
	dmuls.l	r3,r2
	mov	#-96,r0
	extu.b	r0,r0			! HW_VIEW_CX 160
	sts	mach,r14		! ex
	add	r0,r12
	add	r0,r13
	add	r0,r14
	cmp/gt	r12,r13
	bf	.Ls_12
	mov	r12,r13
.Ls_12:
	cmp/gt	r14,r12
	bf	.Ls_13
	mov	r12,r14
.Ls_13:
	cmp/pz	r1			! min_cx >= 0 || max_cx <= 0:
	bt	.Ls_far			! the far corner can widen the span
	cmp/pl	r3
	bt	.Ls_nofar
.Ls_far:
	mov.l	@r6,r0
	bsr	.Ls_div
	nop
	mov	r0,r10			! s_min = div(max_cz)
	mov.l	@(4,r15),r1
	add	r3,r1
	mov.l	@(36,r15),r0
	sub	r0,r1			! cx_far = min_cx + max_cx - cx_near
	dmuls.l	r1,r10
	mov	#-96,r0
	extu.b	r0,r0
	sts	mach,r1
	add	r0,r1			! px_far
	cmp/gt	r1,r13
	bf	.Ls_14
	mov	r1,r13
.Ls_14:
	cmp/gt	r14,r1
	bf	.Ls_nofar
	mov	r1,r14
.Ls_nofar:
	mov.l	@(16,r15),r0		! cz_fmin
	mov.l	@r15,r1			! true_min_cz
	cmp/gt	r1,r0
	bf	.Ls_15
	mov	r8,r0
	bsr	.Ls_div
	nop
	mov	r0,r11			! s_max = div(min_cz)
.Ls_15:
	tst	r5,r5			! promote to fully inside?
	bf	.Ls_16
	mov.l	@(12,r15),r0
	mov	#1,r1
	shll16	r1			! 16 << 12
	cmp/ge	r1,r0
	bf	.Ls_16
	cmp/pz	r13
	bf	.Ls_16
	mov.w	.Ls_k320,r1
	cmp/gt	r1,r14
	bt	.Ls_16
	mov	#1,r5
.Ls_16:
	add	#-1,r13
	add	#1,r14
	cmp/pz	r13
	bt	.Ls_17
	mov	#0,r13
.Ls_17:
	mov.w	.Ls_k320,r1
	cmp/gt	r1,r14
	bf	.Ls_18
	mov	r1,r14
.Ls_18:
	mov	r14,r2
	add	#-1,r2			! ex_m1
	cmp/gt	r2,r13
	bt	.Ls_cull
	mov.l	r13,@(20,r6)
	mov.l	r14,@(24,r6)
	mov	r13,r0
	xor	r2,r0
	mov	#32,r1
	cmp/hs	r1,r0
	bt	.Ls_multi
	! span inside one word: solid test, then that word's aperture summary
	mov	r13,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0		! w0 * 4
	mov	r0,r12
	mov.l	@(24,r7),r1
	mov.l	@(r0,r1),r3		! sm
	tst	r3,r3
	bt	.Ls_19
	mov	r13,r0
	and	#31,r0
	shll2	r0
	mov.l	@(28,r7),r1
	mov.l	@(r0,r1),r1		! ml[sx & 31]
	mov	r2,r0
	and	#31,r0
	shll2	r0
	mov.l	@(32,r7),r4
	mov.l	@(r0,r4),r4		! mr[ex_m1 & 31]
	and	r4,r1
	and	r1,r3
	cmp/eq	r1,r3
	bt	.Ls_cull		! every column solid
.Ls_19:
	mov.l	@(20,r7),r1
	mov.b	@(2,r1),r0
	tst	r0,r0
	bt	.Ls_ok			! no portal aperture yet
	mov.l	@(36,r7),r1
	mov	r12,r0
	add	r0,r0
	add	r0,r1			! &hw_ap_words[w0]
	mov.w	@r1,r3			! yt_min
	mov.w	@(4,r1),r0
	sub	r3,r0
	add	#-5,r0
	cmp/pz	r0
	bt	.Ls_slow
	mov.w	@(6,r1),r0
	mov	r0,r4
	mov.w	@(2,r1),r0		! yb_max
	sub	r0,r4
	add	#4,r4
	cmp/pz	r4
	bf	.Ls_slow
	cmp/ge	r0,r3
	bt	.Ls_cull		! closed word
	mov.l	r3,@(12,r6)
	mov.l	r0,@(16,r6)
	bra	.Ls_out
	mov	#1,r13
.Ls_multi:
	mov.l	@(16,r7),r1
	mov.l	@r1,r1
	mov.l	@(20,r7),r3
	mov.b	@(2,r3),r0
	or	r1,r0
	tst	r0,r0
	bt	.Ls_ok
.Ls_slow:
	bra	.Ls_full
	mov	#3,r13
.Ls_ok:
	mov	#1,r13
.Ls_full:				! ytop_min = 0, ybot_max = vh
	mov	#0,r0
	mov.l	r0,@(12,r6)
	mov.l	@(40,r7),r0
	mov.l	@r0,r0
	mov.l	r0,@(16,r6)
.Ls_out:
	mov.l	r11,@(4,r6)
	mov.l	r10,@(8,r6)
	or	r5,r9
	mov.l	r9,@(28,r6)
	mov	r13,r0
.Ls_ret:
	add	#40,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8
.Ls_cull:
	bra	.Ls_ret
	mov	#-1,r0
.Ls_def:
	bra	.Ls_ret
	mov	#0,r0

! HW_FastDiv: r0 = cz -> r0 = saturn_master_oclut[dist]. Clobbers r1.
.Ls_div:
	shlr16	r0			! dist = (unsigned)cz >> 16
	mov.w	.Ls_k2048,r1
	cmp/hs	r1,r0
	bt	.Ls_dfar
.Ls_dlk:
	shll2	r0
	mov.l	@(12,r7),r1
	rts
	mov.l	@(r0,r1),r0
.Ls_dfar:
	shlr2	r0
	shlr	r0
	mov.w	.Ls_k1023,r1
	and	r1,r0
	mov.w	.Ls_k2048,r1
	bra	.Ls_dlk
	add	r1,r0

	.align	1
.Ls_k320:	.word	320
.Ls_k2048:	.word	2048
.Ls_k1023:	.word	1023
	.align	2
.Ls_nzs:	.long	2621440		! HW_NEAR_Z_SCALE = 10485760 / 4

! Partial edge word of a multi-word span. In: r3 word index, r2 first test
! bit, r4 &ytop[first column], r6 column count; r8 solid_mask, r9 ap_words,
! r10 yt_min / r11 yb_max (real values, updated). Clobbers r0-r4, r6, r7, r12.
.Lg_edge:
	mov	r3,r0
	shll2	r0
	mov.l	@(r0,r8),r1		! sm
	shll	r0
	mov	r9,r3
	add	r0,r3			! aw
	mov	r1,r0
	cmp/eq	#-1,r0
	bt	.Lg_e_ret		! fully solid word
	mov.w	@r3,r7			! aw yt_min
	cmp/gt	r7,r10
	bt	.Lg_e_need
	mov.w	@(2,r3),r0
	cmp/gt	r11,r0
	bf	.Lg_e_ret		! can't widen the result
.Lg_e_need:
	mov.w	@(4,r3),r0
	sub	r7,r0
	add	#-5,r0
	cmp/pz	r0			! yt_max - yt_min > 4 ?
	bt	.Lg_e_scan
	mov.w	@(6,r3),r0
	mov	r0,r12
	mov.w	@(2,r3),r0
	sub	r0,r12
	add	#4,r12
	cmp/pz	r12			! yb_max - yb_min > 4 ?
	bf	.Lg_e_scan
	cmp/gt	r7,r10			! flat word: use its summary
	bf	.Lg_e_1
	mov	r7,r10
.Lg_e_1:
	cmp/gt	r11,r0
	bf	.Lg_e_ret
	mov	r0,r11
.Lg_e_ret:
	rts
	nop
.Lg_e_scan:
	sts.l	pr,@-r15
	mov	r6,r3
	exts.b	r10,r10
	bsr	.Lg_cols
	exts.b	r11,r11
	extu.b	r10,r10
	lds.l	@r15+,pr
	rts
	extu.b	r11,r11
