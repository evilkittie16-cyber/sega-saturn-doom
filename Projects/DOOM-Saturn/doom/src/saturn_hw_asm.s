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
.Lr_c1:					! min/max start at the extremes here, so
	mov.b	@r13+,r0		! yb	  a new min may also be a new max
	shlr	r6			! T = column solid
	bt/s	.Lr_s1
	mov.b	@r12+,r2		! yt
	cmp/hi	r2,r8
	bf	.Lr_r1a
	mov	r2,r8
.Lr_r1a:
	cmp/hi	r9,r2
	bf	.Lr_r1b
	mov	r2,r9
.Lr_r1b:
	cmp/hi	r0,r10
	bf	.Lr_r1c
	mov	r0,r10
.Lr_r1c:
	cmp/hi	r11,r0
	bf	.Lr_s1
	mov	r0,r11
.Lr_s1:
	dt	r1
	bf	.Lr_c1
	bra	.Lr_fin
	mov.l	@r15+,r6

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
! --------------------------------------------------------------------------
! void hw_asm_walk(const baked_seg_t *seg, const baked_seg_t *seg_end,
!                  const saturn_wl_pillar_t *pillars, const void *const *ctx)
!
! Compact rewrite stage A (saturn_hw_walls.c, default build: no defer /
! vis-stats / tiny-wall): HW_WalkSubsectorBakedSegs + HW_ProjectAndCull-
! PillarPair + HW_TransformSubsectorPillar (+ the 16-bit case of
! HW_TransformPillar_Shifted) + the front of HW_EmitVisibleBakedSeg, in one
! routine. Rare paths and the emitter call the C functions through ctx.
! ctx (longs): 0 &hw_wctx, 4 &hw_cam_x, 8 &hw_cam_y, 12 local_trans_pillars,
! 16 hw_pil_gen, 20 &hw_cur_pil_gen, 24 &hw_cur_pfrac, 28 &hw_frust,
! 32 saturn_master_oclut, 36 hw_solid_mask, 40 hw_mask_left,
! 44 hw_mask_right, 48 hw_ap_words, 52 &hw_solid_count,
! 56 &hw_aperture_state, 60 &saturn_view_h, 64 &saturn_view_cy,
! 68 hw_sec_fast, 72 &saturn_cmd_m_needs_ytrim, 76 hw_asm_spanap_ctx,
! fn: 80 TransformPillar_Shifted(tp,pdx,pdy), 84 CheckSpanOcclusion(sx,ex),
! 88 OccludeZeroHeightSeg(seg,bsec,solid), 92 ClipSegNearPlane(ctx,seg),
! 96 MarkSubSolidSpan(x0,x1), 100 RecordSegPortalAperture(ctx,bsec),
! 104 EmitWallFromCtx(ctx,seg); 108 &hw_p0_clipped, 112 &hw_p1_clipped.
! hw_wctx offsets: 0 portal_recorded, 1 was_near_clipped, 2 occl_state,
! 3 is_sky_ceil, 4 n_vis, 5 top_overlap, 6 bot_overlap, 7 needs_near_clip,
! 8 seg_sx, 10 seg_ex, 12 ytop_min, 14 ybot_max, 16 ya0, 18 ya1, 20 yb0,
! 22 yb1, 28 wall_h, 30 cur_v, 32 sec_ceiling, 36 sec_floor, 40 p0, 44 p1,
! 48 seg, 52 dz_top, 56 dz_bot. baked_seg_t (16 B): 0 pil0, 1 pil1,
! 2 light, 3 flags, 4 tex_64, 6 v_off, 14 back_sec_idx.
! hw_trans_pillar_t (16 B): 0 cx, 4 cz, 8 s, 12 px.
! Registers: r8 seg, r9 seg_end, r10 pillars, r11 ctx, r12 last pair key,
! r13 last pair state, r14 &hw_wctx. Locals @r15: 0 cam_x, 4 cam_y,
! 8 is_solid, 12 bsec, 16 flags.
! --------------------------------------------------------------------------
	.align	2
	.global	_hw_asm_walk
_hw_asm_walk:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	add	#-20,r15
	mov	r4,r8
	mov	r5,r9
	mov	r6,r10
	mov	r7,r11
	mov.l	@r11,r14		! &hw_wctx
	mov.l	@(4,r11),r0
	mov.l	@r0,r0
	mov.l	r0,@r15			! cam_x
	mov.l	@(8,r11),r0
	mov.l	@r0,r0
	mov.l	r0,@(4,r15)		! cam_y
	mov	#-1,r12			! last_pair_key
	bra	.Lw_loop
	mov	#0,r13			! last_pair_state

.Lw_next:
	add	#16,r8
.Lw_loop:
	cmp/hs	r9,r8
	bt	.Lw_done
	mov.l	@r8,r0			! pil0 | pil1 | light | flags
	tst	#255,r0
	bt	.Lw_next		! no flags: nothing to draw
	shlr16	r0			! pair_key
	cmp/eq	r12,r0
	bf	.Lw_newpair
	cmp/pz	r13
	bf	.Lw_next		! same pair, rejected
	bra	.Lw_evbs
	nop

.Lw_newpair:
	mov	r0,r12			! last_pair_key = pair_key
	! backface: cross = obj_dy*seg_dx - obj_dx*seg_dy (muls.w as in C)
	extu.b	r0,r1
	shlr8	r0
	shll2	r1
	shll2	r0
	add	r10,r1			! &pillars[p1]
	add	r10,r0			! &pillars[p0]
	mov.w	@r0+,r5			! p0.x
	mov.w	@r0,r6			! p0.z
	mov.w	@r1+,r2			! p1.x
	mov.w	@r1,r3			! p1.z
	sub	r5,r2			! seg_dx
	sub	r6,r3			! seg_dy
	mov.l	@(4,r15),r0
	sub	r0,r6			! obj_dy
	mov.l	@r15,r0
	muls.w	r6,r2
	sub	r0,r5			! obj_dx
	sts	macl,r2
	muls.w	r5,r3
	sts	macl,r3
	sub	r3,r2			! cross
	cmp/pl	r2
	bt	.Lw_front
	mov.b	@(2,r8),r0		! back-facing: skip this seg's
	and	#15,r0			! co-linear followers too
	shll2	r0
	shll2	r0
	add	r0,r8
	bra	.Lw_next
	mov	#-1,r13

	! ---- HW_ProjectAndCullPillarPair(pair_key, obj_dx r5, obj_dy r6) ----
.Lw_front:
	mov	#0,r0
	mov.b	r0,@r14			! portal_recorded = 0
	mov.l	@(16,r11),r1		! hw_pil_gen
	mov.l	@(20,r11),r0
	mov.b	@r0,r7			! pil_gen
	mov	r12,r4
	shlr8	r4			! idx0
	mov	r4,r0
	mov.b	@(r0,r1),r2
	cmp/eq	r7,r2
	bt	.Lw_p0ok
	mov.b	r7,@(r0,r1)
	bsr	.Lw_tp			! transform pillar idx0 (dx r5, dy r6)
	nop
.Lw_p0ok:
	mov	r12,r0
	extu.b	r0,r4			! idx1
	mov.l	@(16,r11),r1
	mov.l	@(20,r11),r0
	mov.b	@r0,r7
	mov	r4,r0
	mov.b	@(r0,r1),r2
	cmp/eq	r7,r2
	bt	.Lw_p1ok
	mov.b	r7,@(r0,r1)
	shll2	r0
	add	r10,r0
	mov.w	@r0+,r5
	mov.w	@r0,r6
	mov.l	@r15,r0
	sub	r0,r5			! p1.x - cam_x
	mov.l	@(4,r15),r0
	bsr	.Lw_tp
	sub	r0,r6			! p1.z - cam_y
.Lw_p1ok:
	mov.l	@(12,r11),r1		! local_trans_pillars
	mov	r12,r0
	shlr8	r0
	shll2	r0
	shll2	r0
	mov	r1,r4
	add	r0,r4			! tp0
	mov	r12,r0
	extu.b	r0,r0
	shll2	r0
	shll2	r0
	mov	r1,r5
	add	r0,r5			! tp1
	mov.l	@(4,r4),r2		! tp0->cz
	mov.l	@(4,r5),r3		! tp1->cz
	mov	#4,r1
	shll16	r1			! HW_NEAR_Z
	cmp/ge	r1,r2
	bt	.Lw_c1
	cmp/ge	r1,r3
	bf	.Lw_rej			! both in front of the near plane
.Lw_c1:
	mov	#64,r0
	shll16	r0
	shll8	r0			! HW_FAR_Z = 1 << 30
	cmp/gt	r0,r2
	bf	.Lw_c2
	cmp/gt	r0,r3
	bt	.Lw_rej			! both beyond the far plane
.Lw_c2:
	mov.l	r4,@(40,r14)		! ctx->p0
	mov.l	r5,@(44,r14)		! ctx->p1
	cmp/ge	r1,r2
	bf	.Lw_near
	cmp/ge	r1,r3
	bf	.Lw_near
	mov.l	@(12,r4),r6		! xa
	mov.l	@(12,r5),r7		! xb
	cmp/gt	r6,r7
	bf	.Lw_rej			! xb <= xa
	cmp/pl	r7
	bf	.Lw_rej			! xb <= 0
	mov.w	.Lw_k320,r1
	cmp/ge	r1,r6
	bt	.Lw_rej			! xa >= 320
	cmp/pz	r6
	bt	.Lw_c3
	mov	#0,r6			! seg_sx
.Lw_c3:
	cmp/gt	r1,r7
	bf	.Lw_c4
	mov	r1,r7			! seg_ex
.Lw_c4:
	bra	.Lw_span
	nop
.Lw_near:
	mov	#1,r0
	mov.b	r0,@(7,r14)		! needs_near_clip = 1
	bra	.Lw_evbs
	mov	#2,r13

.Lw_rej:
	bra	.Lw_next
	mov	#-1,r13

	.align	1
.Lw_k320:	.word	320

	! seg_sx r6, seg_ex r7
.Lw_span:
	mov	r7,r2
	add	#-1,r2			! ex_m1
	mov	r6,r0
	xor	r2,r0
	mov	#32,r1
	cmp/hs	r1,r0
	bt	.Lw_mword
	mov	r6,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0		! w0 * 4
	mov	r0,r5
	mov.l	@(36,r11),r1
	mov.l	@(r0,r1),r3		! sm
	tst	r3,r3
	bt/s	.Lw_s1
	mov	#0,r4			! occl_state
	mov	r6,r0
	and	#31,r0
	shll2	r0
	mov.l	@(40,r11),r1
	mov.l	@(r0,r1),r1		! ml[sx & 31]
	mov	r2,r0
	and	#31,r0
	shll2	r0
	mov.l	@(44,r11),r4
	mov.l	@(r0,r4),r4		! mr[ex_m1 & 31]
	and	r4,r1			! mask
	and	r1,r3			! hit
	cmp/eq	r1,r3
	bt	.Lw_rej			! fully solid
	tst	r3,r3
	movt	r0
	xor	#1,r0
	mov	r0,r4			! occl_state = (hit != 0)
.Lw_s1:
	mov.l	@(48,r11),r1
	mov	r5,r0
	add	r0,r0
	add	r0,r1			! &hw_ap_words[w0]
	mov.w	@r1,r3			! yt
	mov.w	@(4,r1),r0		! yt_max
	sub	r3,r0
	add	#-9,r0
	cmp/pz	r0
	bt	.Lw_sap			! yt_max - yt > 8
	mov.w	@(6,r1),r0
	mov	r0,r5			! yb_min
	mov.w	@(2,r1),r0		! yb
	sub	r0,r5
	add	#8,r5
	cmp/pz	r5
	bf	.Lw_sap			! yb - yb_min > 8
	cmp/ge	r0,r3
	bt	.Lw_rej			! yt >= yb
	bra	.Lw_vis
	mov	r0,r5			! r3 yt, r5 yb
.Lw_mword:
	mov.l	@(52,r11),r0
	mov.l	@r0,r0			! hw_solid_count
	cmp/pl	r0
	bf/s	.Lw_m1
	mov	#0,r4
	mov	#84,r0			! HW_CheckSpanOcclusion(sx, ex)
	mov.l	@(r0,r11),r0
	mov.l	r6,@(8,r15)
	mov.l	r7,@(12,r15)
	mov	r6,r4
	jsr	@r0
	mov	r7,r5
	mov.l	@(8,r15),r6
	mov.l	@(12,r15),r7
	mov	r0,r4
	cmp/eq	#2,r0
	bt	.Lw_rej			! fully occluded
.Lw_m1:
	mov.l	@(56,r11),r1
	mov.b	@(2,r1),r0		! aperture_active
	tst	r0,r0
	bf	.Lw_sap
	mov	#0,r3
	mov.l	@(60,r11),r0
	bra	.Lw_vis
	mov.l	@r0,r5			! yt 0, yb vh
.Lw_sap:				! occl r4: HW_GetSpanAperture(sx, ex)
	mov.l	r4,@(16,r15)
	mov.l	r6,@(8,r15)
	mov.l	r7,@(12,r15)
	mov	r6,r4
	mov	r7,r5
	mov	#76,r0
	mov.l	@(r0,r11),r6
	bsr	_hw_asm_spanap
	nop
	mov.l	@(8,r15),r6
	mov.l	@(12,r15),r7
	cmp/pz	r0
	bf	.Lw_rej
	mov.l	@(16,r15),r4
	mov	r0,r5			! yb = (int16) pack
	mov	r0,r3
	shlr16	r3			! yt = pack >> 16
.Lw_vis:
	mov	r6,r0
	mov.w	r0,@(8,r14)		! seg_sx
	mov	r7,r0
	mov.w	r0,@(10,r14)		! seg_ex
	mov	r3,r0
	mov.w	r0,@(12,r14)		! ytop_min
	mov	r5,r0
	mov.w	r0,@(14,r14)		! ybot_max
	mov	r4,r0
	mov.b	r0,@(2,r14)		! occl_state
	mov	#0,r0
	mov.b	r0,@(7,r14)		! needs_near_clip = 0
	mov	#3,r13			! last_pair_state = 3

	! ---- front of HW_EmitVisibleBakedSeg(seg r8) ----
.Lw_evbs:
	mov.b	@(3,r8),r0
	extu.b	r0,r7			! seg_flags
	mov.l	r7,@(16,r15)
	mov.l	@(32,r14),r2		! sec_ceiling -> top
	mov.l	@(36,r14),r3		! sec_floor -> bot
	mov	#1,r4			! is_solid
	mov	#-1,r5			! bsec_idx
	mov	r7,r0
	tst	#0x40,r0		! SEG_FLAG_STATIC ?
	bt	.Lw_dyn
	tst	#6,r0			! LOWER | UPPER ?
	bt	.Lw_st1
	mov.w	@(14,r8),r0
	mov	r0,r5			! bsec
	shll2	r0
	shll2	r0
	mov	r0,r1
	mov	#68,r0
	mov.l	@(r0,r11),r0
	add	r0,r1			! &hw_sec_fast[bsec]
	mov	#0,r4
	mov	r7,r0
	tst	#2,r0
	bt	.Lw_st0
	bra	.Lw_st1
	mov.l	@r1,r3			! UPPER: bot = bsec dz_ceil
.Lw_st0:
	mov.l	@(4,r1),r2		! LOWER: top = bsec dz_floor
.Lw_st1:
	mov.w	@(6,r8),r0
	bra	.Lw_hgt
	mov	r0,r6			! v_base = mid_static_v_offset

.Lw_dyn:
	tst	#6,r0
	bt	.Lw_mid
	mov.w	@(14,r8),r0
	mov	r0,r5			! bsec
	extu.w	r0,r0
	mov	#1,r1
	shll8	r1			! 256
	cmp/hs	r1,r0
	bt	.Lw_next		! bad back sector
	shll2	r0
	shll2	r0
	mov	r0,r1
	mov	#68,r0
	mov.l	@(r0,r11),r0
	add	r0,r1			! bsec_f
	mov.l	@r1,r6			! bceil
	mov.l	@(4,r1),r4		! bfloor
	! is_solid = !is_sky_ceil && !bsec_f->is_sky && min(ceil,bceil) <= max(floor,bfloor)
	mov.b	@(3,r14),r0
	tst	r0,r0
	bf	.Lw_d0
	mov.b	@(12,r1),r0
	tst	r0,r0
	bf	.Lw_d0
	mov	r2,r0			! mtop = min(sec_ceiling, bceil)
	cmp/gt	r6,r0
	bf	.Lw_d1
	mov	r6,r0
.Lw_d1:
	mov	r3,r1			! mbot = max(sec_floor, bfloor)
	cmp/gt	r1,r4
	bf	.Lw_d2
	mov	r4,r1
.Lw_d2:
	cmp/gt	r1,r0			! mtop > mbot -> not solid
	movt	r0
	xor	#1,r0
	bra	.Lw_d3
	mov.l	r0,@(8,r15)
.Lw_d0:
	mov	#0,r0
	mov.l	r0,@(8,r15)
.Lw_d3:
	mov	r7,r0
	tst	#2,r0
	bt	.Lw_d4
	mov	r2,r1			! UPPER: top = sec_ceiling, bot = bceil
	bra	.Lw_d5
	mov	r6,r6
.Lw_d4:
	mov	r4,r1			! LOWER: top = bfloor, bot = sec_floor
	mov	r3,r6
.Lw_d5:					! r1 orig_top, r6 orig_bot, r2 ceil, r3 floor
	mov	r1,r0
	cmp/gt	r2,r0
	bf	.Lw_d6
	mov	r2,r0			! top = min(orig_top, sec_ceiling)
.Lw_d6:
	mov	r6,r4
	cmp/gt	r4,r3
	bf	.Lw_d7
	mov	r3,r4			! bot = max(orig_bot, sec_floor)
.Lw_d7:
	cmp/gt	r4,r0
	bt	.Lw_d8
	! zero height: still occludes
	mov.b	@r14,r0
	tst	r0,r0
	bf	.Lw_next
	mov	r7,r0
	tst	#0x80,r0
	bf	.Lw_next
	mov	#88,r0
	mov.l	@(r0,r11),r0		! HW_OccludeZeroHeightSeg(seg, bsec, is_solid)
	mov	r8,r4
	jsr	@r0
	mov.l	@(8,r15),r6
	bra	.Lw_next
	nop
.Lw_d8:					! r0 top, r4 bot, r1 orig_top, r6 orig_bot
	mov.l	r5,@(12,r15)		! keep bsec
	mov	r0,r5			! top
	mov.w	@(6,r8),r0		! v_off
	mov	r0,r3
	mov	r7,r0
	tst	#0x20,r0
	bt	.Lw_d9
	mov	r2,r0			! LOWER_UNPEGGED: ((ceil - top) >> 16) + v_off
	sub	r5,r0
	bra	.Lw_d11
	nop
.Lw_d9:
	tst	#0x10,r0
	bt	.Lw_d10
	mov	r5,r0			! PEG_BOTTOM: v_off - ((top - orig_bot) >> 16)
	sub	r6,r0
	shlr16	r0
	exts.w	r0,r0
	sub	r0,r3
	bra	.Lw_d12
	mov	r3,r6
.Lw_d10:
	mov	r1,r0			! v_off + ((orig_top - top) >> 16)
	sub	r5,r0
.Lw_d11:
	shlr16	r0
	exts.w	r0,r0
	add	r0,r3
	mov	r3,r6
.Lw_d12:
	mov	r5,r2			! top
	mov	r4,r3			! bot
	mov.l	@(8,r15),r4		! is_solid
	bra	.Lw_hgt
	mov.l	@(12,r15),r5		! bsec

.Lw_mid:				! single-sided or masked mid
	tst	#8,r0
	bt	.Lw_m2
	mov	#0,r4
	mov.w	@(14,r8),r0
	mov	r0,r5			! bsec
	extu.w	r0,r0
	mov	#1,r1
	shll8	r1			! 256
	cmp/hs	r1,r0
	bt	.Lw_m2
	shll2	r0
	shll2	r0
	mov	r0,r1
	mov	#68,r0
	mov.l	@(r0,r11),r0
	add	r0,r1
	mov.l	@r1,r0			! bsec dz_ceil
	cmp/gt	r0,r2
	bf	.Lw_m3
	mov	r0,r2
.Lw_m3:
	mov.l	@(4,r1),r0		! bsec dz_floor
	cmp/gt	r3,r0
	bf	.Lw_m2
	mov	r0,r3
.Lw_m2:
	cmp/gt	r3,r2
	bf	.Lw_next		! top <= bot
	mov.w	@(6,r8),r0
	mov	r0,r6			! v_base = v_off
	mov	r7,r0
	tst	#0x10,r0
	bt	.Lw_hgt
	mov	r2,r0			! PEG_BOTTOM: v_off - ((top - bot) >> 16)
	sub	r3,r0
	shlr16	r0
	exts.w	r0,r0
	sub	r0,r6

	! r2 top, r3 bot, r4 is_solid, r5 bsec, r6 v_base, r7 flags
.Lw_hgt:
	mov	r2,r0
	sub	r3,r0
	shlr16	r0
	exts.w	r0,r0			! wall_h = (top - bot) >> 16
	cmp/pl	r0
	bt	.Lw_h1
	mov	#1,r0
.Lw_h1:
	mov.w	r0,@(28,r14)		! wall_h
	mov.l	r2,@(52,r14)		! dz_top
	mov.l	r3,@(56,r14)		! dz_bot
	mov	r6,r0
	mov.w	r0,@(30,r14)		! cur_v
	mov	#0,r0
	mov.b	r0,@(1,r14)		! was_near_clipped = 0
	mov.l	r4,@(8,r15)
	mov.l	r5,@(12,r15)
	mov.b	@(7,r14),r0
	tst	r0,r0
	bt	.Lw_h2
	mov	#92,r0
	mov.l	@(r0,r11),r0		! HW_ClipSegNearPlane(ctx, seg)
	mov	r14,r4
	jsr	@r0
	mov	r8,r5
	tst	r0,r0
	bt	.Lw_next
	mov	#108,r0
	mov.l	@(r0,r11),r0
	mov.l	r0,@(40,r14)		! ctx->p0 = &hw_p0_clipped
	mov	#112,r0
	mov.l	@(r0,r11),r0
	mov.l	r0,@(44,r14)		! ctx->p1 = &hw_p1_clipped
.Lw_h2:
	mov.b	@r14,r0			! portal_recorded ?
	tst	r0,r0
	bf	.Lw_tex
	mov.l	@(16,r15),r0
	tst	#0x80,r0		! SEG_FLAG_PORTAL_DONE ?
	bf	.Lw_tex
	mov.l	@(8,r15),r0
	tst	r0,r0
	bt	.Lw_h3
	mov	#1,r0			! solid: mark its span
	mov.b	r0,@r14
	mov.w	@(8,r14),r0
	mov	r0,r4
	mov.w	@(10,r14),r0
	cmp/gt	r4,r0
	bf	.Lw_tex
	mov	r0,r5
	mov	#96,r0
	mov.l	@(r0,r11),r0		! HW_MarkSubSolidSpan(sx, ex)
	jsr	@r0
	nop
	bra	.Lw_tex
	nop
.Lw_h3:
	mov.l	@(12,r15),r5
	cmp/pz	r5
	bf	.Lw_tex
	mov	#100,r0
	mov.l	@(r0,r11),r0		! HW_RecordSegPortalAperture(ctx, bsec)
	jsr	@r0
	mov	r14,r4
.Lw_tex:
	mov.w	@(4,r8),r0		! tex_64
	extu.w	r0,r0
	tst	r0,r0
	bt	.Lw_next
	mov.b	@(4,r14),r0
	add	#1,r0
	mov.b	r0,@(4,r14)		! n_vis++
	! HW_ProjYQuad: y = cy - hi32(dz * s)
	mov.l	@(40,r14),r0
	mov.l	@(8,r0),r4		! p0->s
	mov.l	@(44,r14),r0
	mov.l	@(8,r0),r5		! p1->s
	mov.l	@(52,r14),r2		! dz_top
	mov.l	@(56,r14),r3		! dz_bot
	mov	#64,r0
	mov.l	@(r0,r11),r0
	dmuls.l	r2,r4
	mov.l	@r0,r1			! cy
	sts	mach,r6
	dmuls.l	r3,r4
	mov	r1,r4
	sub	r6,r4			! ya0
	sts	mach,r6
	dmuls.l	r2,r5
	mov	r1,r2
	sub	r6,r2			! ya1
	sts	mach,r6
	dmuls.l	r3,r5
	mov	r1,r5
	sub	r6,r5			! yb0
	sts	mach,r6
	mov	r1,r3
	sub	r6,r3			! yb1
	! r4 ya0, r2 ya1, r5 yb0, r3 yb1: aperture reject
	mov.w	@(12,r14),r0		! ytop_min
	cmp/gt	r0,r2
	bt	.Lw_y1
	cmp/gt	r0,r3
	bf	.Lw_next		! ya1 <= ytop_min && yb1 <= ytop_min
.Lw_y1:
	mov.w	@(14,r14),r0		! ybot_max
	cmp/ge	r0,r4
	bf	.Lw_y2
	cmp/ge	r0,r5
	bt	.Lw_next		! ya0 >= ybot_max && yb0 >= ybot_max
.Lw_y2:
	! seam overlaps
	mov	#1,r6			! top_overlap
	mov	#1,r7			! bot_overlap
	mov.w	@(28,r14),r0		! wall_h
	mov	#16,r1
	cmp/gt	r1,r0
	mov.l	@(16,r15),r0		! flags
	bf	.Lw_y3
	tst	#2,r0
	bt	.Lw_y5
	bra	.Lw_y5
	mov	#2,r7			! UPPER: bot_overlap 2
.Lw_y3:
	tst	#4,r0
	bt	.Lw_y4
	bra	.Lw_y5
	mov	#0,r6			! short LOWER: top_overlap 0
.Lw_y4:
	tst	#2,r0
	bt	.Lw_y5
	mov	#0,r7			! short UPPER: bot_overlap 0
.Lw_y5:
	sub	r6,r4
	sub	r6,r5
	add	r7,r2
	add	r7,r3
	cmp/ge	r2,r4
	bf	.Lw_y6
	cmp/ge	r3,r5
	bt	.Lw_next		! ya0 >= ya1 && yb0 >= yb1
.Lw_y6:
	mov	#-8,r1
	cmp/ge	r1,r4
	bt	.Lw_y7
	cmp/ge	r1,r5
	bf	.Lw_y8			! both tops above -8
.Lw_y7:
	mov.l	@(60,r11),r0
	mov.l	@r0,r1
	add	#8,r1			! vh + 8
	cmp/gt	r1,r2
	bf	.Lw_y9
	cmp/gt	r1,r3
	bf	.Lw_y9
.Lw_y8:
	mov	#72,r0
	mov.l	@(r0,r11),r1
	mov	#1,r0
	mov.b	r0,@r1			! saturn_cmd_m_needs_ytrim = 1
.Lw_y9:
	mov.l	r8,@(48,r14)		! ctx->seg
	mov	r4,r0
	mov.w	r0,@(16,r14)		! ya0
	mov	r2,r0
	mov.w	r0,@(18,r14)		! ya1
	mov	r5,r0
	mov.w	r0,@(20,r14)		! yb0
	mov	r3,r0
	mov.w	r0,@(22,r14)		! yb1
	mov	r6,r0
	mov.b	r0,@(5,r14)		! top_overlap
	mov	r7,r0
	mov.b	r0,@(6,r14)		! bot_overlap

! ---- stage B1: HW_EmitWallFromCtx -> EmitBakedWallQuads -> one-band,
! unclipped HW_EmitMonolithicBakedSeg (+ the merge into the previous
! quad). Any other shape calls the C emitter (.Le_c) before anything is
! written, so C redoes the whole wall. em ctx (walk ctx 116, NULL = off):
! 0 &saturn_cmd_m, 4 &saturn_cmd_m_count, 8 &hw_wall_light_delta,
! 12 wall_zlight_cmdcolr_LUT, 16 header (solid), 20 header (mid),
! 24 &saturn_v1w_cells, 28 &saturn_hw_wall_quads, 32 &saturn_v1w_drop,
! 36 &saturn_v1w_captured, 40 &saturn_hw_tex_miss, 44 fn LoadWallTexCtx.
! hw_wctx: 24 srca, 26 th, 60 cur_tex_id, 68 tw, 70 srca_mono, 72 srca_64,
! 74 sub_m_start, 76 last_clip_x0, 92 u_start, 96 u_span.
! Scratch: @(8,r15) header, @(12,r15) w2 (is_solid/bsec are dead here).
	mov	#116,r0
	mov.l	@(r0,r11),r0
	tst	r0,r0
	bt	.Le_c
	mov.w	@(4,r8),r0
	extu.w	r0,r1			! tex
	mov.l	@(60,r14),r0		! cur_tex_id
	cmp/eq	r0,r1
	bf	.Le_load
	mov.w	@(24,r14),r0		! srca
	tst	r0,r0
	bt	.Le_miss
.Le_have:
	mov.b	@(3,r8),r0
	tst	#0x40,r0
	bf	.Le_st
	! not STATIC: DOOM pegging of cur_v (EmitWallFromCtx). Idempotent for
	! power-of-two th, so a later C fallback may redo it; other th -> C.
	and	#0x30,r0
	mov	r0,r2			! PEG_LOWER_UNPEGGED | PEG_BOTTOM
	mov.w	@(26,r14),r0
	mov	r0,r1			! th
	mov.w	@(30,r14),r0
	mov	r0,r3			! v_top = cur_v
	mov	r2,r0
	cmp/eq	#0x10,r0
	bf	.Le_p2
	add	r1,r3			! bottom-pegged: v_top += th
.Le_p2:
	cmp/pl	r1
	bf/s	.Le_p1
	mov	#0,r0			! th <= 0: v_top = 0
	mov	r1,r0
	add	#-1,r0
	tst	r1,r0
	bf	.Le_mod			! th not a power of two
	and	r0,r3			! v_top &= th - 1
.Le_pc:
	mov	#64,r0
	cmp/ge	r0,r1
	bf/s	.Le_p1
	mov	r3,r0
	mov.w	@(28,r14),r0
	mov	r0,r4			! wall_h
	add	r3,r0
	cmp/gt	r1,r0
	bf/s	.Le_p1
	mov	r3,r0
	mov.w	@(16,r14),r0
	mov	r0,r5
	mov.w	@(18,r14),r0
	sub	r5,r0
	mov	#32,r5
	cmp/gt	r5,r0
	bt/s	.Le_p1
	mov	r3,r0
	mov.w	@(20,r14),r0
	mov	r0,r6
	mov.w	@(22,r14),r0
	sub	r6,r0
	cmp/gt	r5,r0
	bt/s	.Le_p1
	mov	r3,r0
	cmp/gt	r1,r4
	bf	.Le_p3
	mov	r1,r4
	mov	r4,r0
	mov.w	r0,@(28,r14)		! wall_h = th
.Le_p3:
	mov	r1,r0
	sub	r4,r0			! v_top = th - wall_h
.Le_p1:
	mov.w	r0,@(30,r14)		! ctx->cur_v
.Le_st:
	mov.b	@(1,r14),r0
	tst	r0,r0
	bf	.Le_c2			! near-clipped: subdivided path
	mov.l	@(40,r14),r5		! p0
	mov.l	@(44,r14),r6		! p1
	mov.l	@(12,r6),r7
	mov.l	@(12,r5),r0
	sub	r0,r7			! dx
	mov	#68,r0
	mov.w	@(r0,r14),r1		! tw
	mov.w	@(8,r8),r0
	extu.w	r0,r2			! u_start
	mov.w	@(10,r8),r0
	extu.w	r0,r3			! u_span
	mov	#92,r0
	mov.l	r2,@(r0,r14)		! ctx->u_start
	mov	#96,r0
	mov.l	r3,@(r0,r14)		! ctx->u_span
	mov	#16,r0
	cmp/gt	r0,r1
	bt	.Le_t17
	! tw <= 16: whole texture only (partial = clipped mono -> C)
	tst	r2,r2
	bf	.Le_c3
	cmp/eq	r1,r3
	bf	.Le_c3
	tst	r3,r3
	bt	.Le_c3
	mov	#70,r0
	mov.w	@(r0,r14),r2		! base = srca_mono
	bra	.Le_mono
	nop
.Le_t17:
	mov	r3,r0
	cmp/eq	#16,r0
	bf	.Le_gen
	mov	r2,r0
	tst	#15,r0
	bf	.Le_gen
	cmp/hs	r1,r2
	bt	.Le_gen
.Le_m16:				! base = srca + (u_start >> 4) * (th << 1)
	mov.w	@(26,r14),r0
	add	r0,r0
	shlr2	r2
	shlr2	r2
	mulu.w	r0,r2
	mov.w	@(24,r14),r0
	sts	macl,r2
	add	r0,r2
	bra	.Le_mono
	mov	#16,r1
.Le_gen:
	! perspective candidate (C runs HW_NeedPerspectiveStrips)
	mov	r2,r0
	add	r3,r0
	add	#-1,r0
	xor	r2,r0
	mov	#15,r4
	cmp/hi	r4,r0
	bf	.Le_np
	mov.w	@(28,r14),r0
	mov	#24,r4
	cmp/gt	r4,r0
	bt	.Le_ps
	mov	#64,r4
	shll16	r4
	add	r4,r4			! 128 << 16
	mov.l	@(4,r5),r0
	cmp/ge	r4,r0
	bt	.Le_np
	mov.l	@(4,r6),r0
	cmp/ge	r4,r0
	bt	.Le_np
.Le_ps:					! HW_NeedPerspectiveStrips (tw > 16 here)
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(52,r0),r0
	mov.l	@r0,r0			! saturn_wall_subdivision
	tst	r0,r0
	bt	.Le_np
	mov	#16,r0
	cmp/ge	r0,r7
	bf	.Le_np			! dx < 16
	mov.l	@(4,r5),r0
	mov.l	@(4,r6),r4
	cmp/gt	r4,r0
	bf	.Le_ps1
	mov	r0,r6
	mov	r4,r0
	mov	r6,r4			! r0 min_z, r4 max_z
.Le_ps1:
	mov	#7,r6
	shll2	r6
	shll2	r6
	shll2	r6
	shll16	r6			! 448 << 16
	cmp/gt	r6,r0
	bt	.Le_npr
	mov	#20,r6
	cmp/ge	r6,r7
	bt	.Le_ps2
	mov	#64,r6
	shll16	r6
	add	r6,r6			! 128 << 16
	cmp/gt	r6,r0
	bt	.Le_npr
.Le_ps2:
	sub	r0,r4			! dz
	mov	r0,r6
	shar	r6
	shar	r6
	shar	r6
	cmp/ge	r6,r4
	bf	.Le_npr			! dz < min_z >> 3
	mov	#9,r6
	shll2	r6
	shll2	r6
	shll	r6
	shll16	r6			! 288 << 16
	cmp/gt	r6,r0
	bf	.Le_ps3
	mov	#32,r6
	cmp/ge	r6,r7
	bf	.Le_npr
	mov	r0,r6
	shar	r6
	shar	r6
	cmp/ge	r6,r4
	bf	.Le_npr
.Le_ps3:
	mov	r3,r6
	shll2	r6
	add	r3,r6			! u_span * 5
	mov	r7,r0
	shll2	r0
	shll2	r0			! dx << 4
	cmp/ge	r6,r0
	bt	.Le_c4			! perspective strips: C
.Le_npr:
	mov.l	@(40,r14),r5
	mov.l	@(44,r14),r6
.Le_np:
	! ((wall_h<=16 && dx<=16 && u_span<=16) || (dx<=6 && p0.cz > 512<<16))
	mov.w	@(28,r14),r0
	mov	#16,r4
	cmp/gt	r4,r0
	bt	.Le_g2
	cmp/gt	r4,r7
	bt	.Le_g2
	cmp/gt	r4,r3
	bf	.Le_g3
.Le_g2:
	mov	#6,r4
	cmp/gt	r4,r7
	bt	.Le_g4
	mov	#2,r4
	shll8	r4
	shll16	r4			! 512 << 16
	mov.l	@(4,r5),r0
	cmp/gt	r4,r0
	bf	.Le_g4
.Le_g3:
	cmp/hs	r1,r2
	bf	.Le_m16
.Le_g4:
	mov	r1,r0
	cmp/eq	#32,r0
	bf	.Le_g6
	tst	r2,r2
	bf	.Le_g6
	mov.w	@(20,r14),r0		! yb0
	mov	r0,r4
	mov.w	@(16,r14),r0		! ya0
	sub	r4,r0
	add	#3,r0
	mov	#6,r4
	cmp/hi	r4,r0
	movt	r4			! r4 = 1: not a level top edge
	mov.w	@(24,r14),r0
	mov	r0,r5			! srca
	mov	#70,r0
	mov.w	@(r0,r14),r6		! srca_mono
	mov	r3,r0
	cmp/eq	#32,r0
	bt	.Le_g5
	mov	#28,r0
	cmp/gt	r0,r7
	bt	.Le_g5x
	tst	r4,r4
	bf	.Le_g5x
.Le_g5:
	cmp/eq	r5,r6
	bf	.Le_mr6
.Le_g5x:
	mov	r3,r0
	cmp/eq	#64,r0
	bf	.Le_c5
	tst	r4,r4
	bf	.Le_c5
	mov	#72,r0
	mov.w	@(r0,r14),r6		! srca_64
	cmp/eq	r5,r6
	bt	.Le_c5
	mov	r6,r2
	bra	.Le_mono
	mov	#64,r1
.Le_mr6:
	mov	r6,r2
	bra	.Le_mono
	mov	#32,r1
.Le_g6:
	mov	#64,r0
	cmp/ge	r0,r1
	bf	.Le_c6
	mov	r3,r0
	cmp/eq	#64,r0
	bf	.Le_c6
	mov	r2,r0
	tst	#63,r0
	bf	.Le_c6
	cmp/hs	r1,r2
	bt	.Le_c6
	mov.w	@(24,r14),r0
	mov	r0,r5			! srca
	mov	#72,r0
	mov.w	@(r0,r14),r6		! srca_64
	mov	#70,r0
	mov.w	@(r0,r14),r4		! srca_mono
	mov.w	@(26,r14),r0
	mov	r0,r3			! th
	shlr2	r2
	shlr2	r2
	shlr2	r2			! u_start >> 6
	mov	#20,r0
	cmp/gt	r0,r7
	bf	.Le_g7
	cmp/eq	r5,r6
	bf	.Le_g8
	mov	#56,r0
	cmp/gt	r0,r7
	bt	.Le_g8
.Le_g7:
	cmp/eq	r5,r4
	bt	.Le_g8
	shll2	r3
	mulu.w	r3,r2
	sts	macl,r2
	add	r4,r2			! srca_mono + (u_start>>6)*(th<<2)
	bra	.Le_mono
	mov	#32,r1
.Le_g8:
	cmp/eq	r5,r6
	bt	.Le_c6
	shll2	r3
	add	r3,r3
	mulu.w	r3,r2
	sts	macl,r2
	add	r6,r2			! srca_64 + (u_start>>6)*(th<<3)
	mov	#64,r1

! r1 mono_tw, r2 base srca (low 16 bits valid)
.Le_mono:
	mov	#116,r0
	mov.l	@(r0,r11),r7		! em
	mov.l	@(4,r7),r0
	mov.w	@r0,r0
	extu.w	r0,r0			! m_cnt
	add	#16,r0
	mov	#5,r4
	shll8	r4			! CMDBUF_M_CAP 1280
	cmp/ge	r4,r0
	bt	.Le_ret
	mov	#76,r0
	mov.w	@(r0,r14),r0		! last_clip_x0
	cmp/eq	#-1,r0
	bf	.Le_c7			! user clip reset: C
	mov.l	@(40,r14),r0
	mov.l	@(12,r0),r4		! q_xa
	mov.l	@(44,r14),r0
	mov.l	@(12,r0),r5		! q_xb
	cmp/gt	r4,r5
	bf	.Le_ret
	! light_s = clamp((baked_light >> 4) + delta, 0, 15)
	mov.l	@(8,r7),r0
	mov.l	@r0,r3
	mov.b	@(2,r8),r0
	extu.b	r0,r0
	shlr2	r0
	shlr2	r0
	add	r3,r0
	cmp/pz	r0
	bt	.Le_l1
	mov	#0,r0
.Le_l1:
	mov	#15,r3
	cmp/gt	r3,r0
	bf	.Le_l2
	mov	r3,r0
.Le_l2:
	cmp/eq	r3,r0
	bt/s	.Le_l3
	mov	#0,r3			! zb = 0 at full light
	mov.l	@(40,r14),r3
	mov.l	@(4,r3),r3
	mov.l	@(44,r14),r6
	mov.l	@(4,r6),r6
	add	r6,r3
	shlr16	r3
	shlr2	r3
	shlr2	r3
	shlr	r3			! (cz0 + cz1) >> 21
	mov	#127,r6
	cmp/hi	r6,r3
	bf	.Le_l3
	mov	r6,r3
.Le_l3:
	shll8	r0
	add	r3,r3
	add	r3,r0
	mov.l	@(12,r7),r3
	mov.w	@(r0,r3),r3
	extu.w	r3,r3
	shlr8	r3
	shll8	r3			! k_shade
	mov.b	@(3,r8),r0
	mov.l	@(16,r7),r6
	tst	#8,r0
	bt	.Le_l4
	mov.l	@(20,r7),r6		! MID: no SPD
.Le_l4:
	or	r6,r3
	mov.l	r3,@(8,r15)		! cmd_header1
	! one band? rem_h <= (th > 0 ? th - cur_v : rem_h) && rem_h <= 255
	mov.w	@(26,r14),r0
	mov	r0,r3			! th
	mov.w	@(28,r14),r0
	mov	r0,r6			! rem_h
	mov	#-1,r0
	extu.b	r0,r0
	cmp/gt	r0,r6
	bt	.Le_c8			! multi-band: C
	cmp/pl	r3
	bf	.Le_r1
	mov.w	@(30,r14),r0
	neg	r0,r0
	add	r3,r0			! th - cur_v
	cmp/gt	r0,r6
	bt	.Le_c8
	cmp/gt	r3,r6
	bf	.Le_r1
	mov	r3,r6			! rows = th
.Le_r1:
	mov	r1,r0
	shlr2	r0
	shlr	r0
	mov	r0,r3			! tw_stride
	mov.w	@(30,r14),r0
	muls.w	r0,r3
	sts	macl,r0
	add	r2,r0			! strip_srca
	shll16	r0
	shll8	r3
	or	r3,r0
	extu.b	r6,r6
	or	r6,r0
	mov.l	r0,@(12,r15)		! w2
	! HW_WALL_XYCLAMP fast test (slow clamp: C)
	mov	#125,r7
	shll2	r7			! 500
	mov	r7,r6
	sub	r5,r6			! 500 - xb
	add	r4,r7			! xa + 500
	or	r7,r6
	mov.w	@(16,r14),r0
	mov	r0,r1			! ya0
	mov.w	@(18,r14),r0
	mov	r0,r2			! ya1
	mov.w	@(20,r14),r0
	mov	r0,r3			! yb0
	mov.w	@(22,r14),r0
	mov	r0,r7			! yb1
	sub	r3,r0
	or	r0,r6
	mov	r2,r0
	sub	r1,r0
	or	r0,r6
	mov	#4,r0
	shll8	r0
	add	#-1,r0			! 1023
	cmp/hi	r0,r6
	bt	.Le_c9
	! now ya0 <= ya1 <= ya0+1023 (same for b): outside [-1024,1023]
	! iff ya0 < -1024 || ya1 > 1023 (or b)
	cmp/gt	r0,r2
	bt	.Le_c9
	cmp/gt	r0,r7
	bt	.Le_c9
	add	#1,r0
	mov	r1,r6
	add	r0,r6
	cmp/pz	r6
	bf	.Le_c9
	mov	r3,r6
	add	r0,r6
	cmp/pz	r6
	bf	.Le_c9
.Le_w:
	shll16	r4
	shll16	r5
	extu.w	r1,r1
	or	r4,r1			! w3
	extu.w	r2,r2
	or	r4,r2			! w6
	extu.w	r3,r3
	or	r5,r3			! w4
	extu.w	r7,r7
	or	r5,r7			! w5
	mov	#116,r0
	mov.l	@(r0,r11),r6		! em
	mov.l	@(4,r6),r0
	mov.w	@r0,r0
	extu.w	r0,r4			! m_cnt
	mov.l	@r6,r0
	mov.l	@r0,r6			! saturn_cmd_m
	mov	r4,r0
	shll2	r0
	shll2	r0
	shll	r0
	add	r0,r6			! &cmd[m_cnt]
	mov	#74,r0
	mov.w	@(r0,r14),r0
	extu.w	r0,r0			! sub_m_start
	cmp/hi	r0,r4
	bf	.Le_new
	add	#-32,r6			! prev
	mov.l	@(16,r6),r0
	cmp/eq	r1,r0
	bf	.Le_new0
	mov.l	@(20,r6),r0
	cmp/eq	r2,r0
	bf	.Le_new0
	mov.l	@(4,r6),r0
	mov.l	@(8,r15),r5
	cmp/eq	r5,r0
	bf	.Le_new0
	mov.l	@(8,r6),r0
	mov.l	@(12,r15),r5
	cmp/eq	r5,r0
	bf	.Le_new0
	mov	r5,r0
	shlr8	r0
	and	#0x3F,r0
	cmp/eq	#4,r0			! 32-wide texture strip
	bf	.Le_new0
	mov.l	@(12,r6),r5
	shlr16	r5
	exts.w	r5,r5			! prev xa
	mov	r3,r0
	shlr16	r0
	exts.w	r0,r0			! xb
	sub	r5,r0
	mov	#48,r5
	cmp/gt	r5,r0
	bt	.Le_new0		! merged_dx > 48
	exts.w	r1,r0
	exts.w	r3,r5
	sub	r5,r0
	add	#3,r0
	mov	#6,r5
	cmp/hi	r5,r0
	bt	.Le_new0
	mov	#72,r0
	mov.w	@(r0,r14),r5		! srca_64
	mov.w	@(24,r14),r0
	cmp/eq	r0,r5
	bt	.Le_new0
	mov	#68,r0
	mov.w	@(r0,r14),r0
	cmp/eq	#32,r0
	bf	.Le_new0
	mov.w	@(30,r14),r0
	shll2	r0
	shll	r0
	add	r0,r5
	shll16	r5
	mov.l	@(12,r15),r0
	extu.b	r0,r0
	or	r0,r5
	mov	#8,r0
	shll8	r0
	or	r0,r5
	mov.l	r5,@(8,r6)		! prev[2]: 64-wide copy
	mov.l	r3,@(16,r6)		! prev[4] = w4
	bra	.Le_ret
	mov.l	r7,@(20,r6)		! prev[5] = w5
.Le_new0:
	add	#32,r6
.Le_new:
	mov	#5,r0
	shll8	r0
	add	#-1,r0
	cmp/ge	r0,r4
	bt	.Le_drop
	add	#1,r4
	mov	#116,r0
	mov.l	@(r0,r11),r5		! em
	mov.l	@(4,r5),r0
	mov.w	r4,@r0			! saturn_cmd_m_count = m_cnt + 1
	mov	#2,r0
	shll16	r0
	mov.l	r0,@r6
	mov.l	@(8,r15),r0
	mov.l	r0,@(4,r6)
	mov.l	@(12,r15),r0
	mov.l	r0,@(8,r6)
	mov.l	r1,@(12,r6)
	mov.l	r3,@(16,r6)
	mov.l	r7,@(20,r6)
	mov.l	r2,@(24,r6)
	mov	#0,r0
	mov.l	r0,@(28,r6)
	mov.l	@(24,r5),r1
	mov.l	@r1,r0
	add	#1,r0
	mov.l	r0,@r1			! saturn_v1w_cells++
	mov.l	@(28,r5),r1
	mov.l	@r1,r0
	add	#1,r0
	bra	.Le_ret
	mov.l	r0,@r1			! saturn_hw_wall_quads++
.Le_drop:
	mov	#116,r0
	mov.l	@(r0,r11),r5
	mov.l	@(32,r5),r1
	mov.l	@r1,r0
	add	#1,r0
	mov.l	r0,@r1			! saturn_v1w_drop++
	mov.l	@(36,r5),r1
	mov	#1,r0
	bra	.Le_ret
	mov.l	r0,@r1			! saturn_v1w_captured = 1
.Le_load:
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(44,r0),r0		! HW_LoadWallTexCtx(tex, ctx)
	mov	r1,r4
	jsr	@r0
	mov	r14,r5
	tst	r0,r0
	bf	.Le_have
.Le_miss:
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(40,r0),r1
	mov.l	@r1,r0
	add	#1,r0
	bra	.Le_ret
	mov.l	r0,@r1			! saturn_hw_tex_miss++
.Le_c1:
	bra	.Le_cs
	mov	#1,r1
.Le_c2:					! near-clipped: Subdivided(ctx, dx >= 24 && subdivision)
	mov.l	@(40,r14),r0
	mov.l	@(12,r0),r1
	mov.l	@(44,r14),r0
	mov.l	@(12,r0),r5
	sub	r1,r5
	mov	#24,r0
	cmp/ge	r0,r5
	bf/s	.Le_sd
	mov	#0,r5
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(52,r0),r0
	mov.l	@r0,r0
	tst	r0,r0
	bt	.Le_sd
	bra	.Le_sd
	mov	#1,r5
.Le_c4:					! perspective strips
	bra	.Le_sd
	mov	#1,r5
.Le_c5:					! no mono copy: linear strips
.Le_c6:
	mov	#0,r5
.Le_sd:					! HW_EmitSubdividedBakedSeg(ctx, r5)
	mov	#116,r0
	mov.l	@(r0,r11),r1
	mov	#68,r0
	mov.l	@(r0,r1),r0
	jsr	@r0
	mov	r14,r4
	bra	.Le_ret
	nop
.Le_c8:					! multi-band mono: HW_EmitMultiBandBakedWall(ctx)
	mov.l	@(8,r15),r7
	mov	#104,r0
	mov.l	r7,@(r0,r14)		! cmd_header1
	mov	#0,r7
	mov	#64,r0
	mov.b	r7,@(r0,r14)		! should_subdivide
	mov	#65,r0
	mov.b	r7,@(r0,r14)		! needs_clip
	mov	#66,r0
	mov.b	r7,@(r0,r14)		! persp_strips
	mov	#80,r0
	mov.w	r1,@(r0,r14)		! mono_tw
	mov	#82,r0
	mov.w	r2,@(r0,r14)		! mono_base_srca
	mov	#84,r0
	mov.w	r6,@(r0,r14)		! rem_h
	mov	#86,r0
	mov.w	r4,@(r0,r14)		! q_xa
	mov	#88,r0
	mov.w	r5,@(r0,r14)		! q_xb
	mov	#116,r0
	mov.l	@(r0,r11),r1
	mov	#72,r0
	mov.l	@(r0,r1),r0
	jsr	@r0
	mov	r14,r4
	bra	.Le_ret
	nop
.Le_c2s:
	bra	.Le_cs
	mov	#2,r1
.Le_c3:
	bra	.Le_cs
	mov	#3,r1
.Le_c7:
	bra	.Le_cs
	mov	#7,r1
.Le_c9:					! HW_WallXYClamp_Slow(hw_wall_coord_tmp)
	mov	#116,r0
	mov.l	@(r0,r11),r6
	mov	#64,r0
	mov.l	@(r0,r6),r0		! hw_wall_coord_tmp
	mov.l	r4,@r0
	mov.l	r5,@(4,r0)
	mov.l	r1,@(8,r0)
	mov.l	r3,@(12,r0)
	mov.l	r2,@(16,r0)
	mov.l	r7,@(20,r0)
	mov.l	@(60,r6),r1
	jsr	@r1
	mov	r0,r4
	mov	#116,r0
	mov.l	@(r0,r11),r6
	mov	#64,r0
	mov.l	@(r0,r6),r0
	mov.l	@r0,r4			! xa
	mov.l	@(4,r0),r5		! xb
	mov.l	@(8,r0),r1		! ya0
	mov.l	@(12,r0),r3		! yb0
	mov.l	@(16,r0),r2		! ya1
	bra	.Le_w
	mov.l	@(20,r0),r7		! yb1
.Le_mod:				! v_top = HW_ModNonPow2(v_top, th)
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(56,r0),r0
	mov	r3,r4
	jsr	@r0
	mov	r1,r5
	mov	r0,r3
	mov.w	@(26,r14),r0
	bra	.Le_pc
	mov	r0,r1			! th
.Le_cs:			! fallback statistics: em[48] = counts[10] or NULL
	mov	#116,r0
	mov.l	@(r0,r11),r0
	mov.l	@(48,r0),r0
	tst	r0,r0
	bt	.Le_c
	shll2	r1
	add	r1,r0
	mov.l	@r0,r1
	add	#1,r1
	mov.l	r1,@r0
.Le_c:
	mov	#104,r0
	mov.l	@(r0,r11),r0		! HW_EmitWallFromCtx(ctx, seg)
	mov	r14,r4
	jsr	@r0
	mov	r8,r5
.Le_ret:
	bra	.Lw_next
	nop

.Lw_done:
	add	#20,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8


! HW_TransformSubsectorPillar(idx r4, dx r5, dy r6): uses r11 ctx.
! Clobbers r0-r7 (and r4-r7 when it calls the C far path).
.Lw_tp:
	mov.l	@(12,r11),r7
	mov	r4,r0
	shll2	r0
	shll2	r0
	add	r0,r7			! tp
	mov.l	@(24,r11),r0
	mov.l	@r0,r1			! hw_cur_pfrac
	mov	r4,r0
	mov.b	@(r0,r1),r0
	extu.b	r0,r0			! fr
	mov	r5,r1
	shll2	r1
	shll2	r1
	mov	r0,r2
	shlr2	r2
	shlr2	r2
	or	r2,r1			! pdx = (dx << 4) | (fr >> 4)
	mov	r6,r2
	shll2	r2
	shll2	r2
	and	#15,r0
	or	r0,r2			! pdy = (dy << 4) | (fr & 15)
	mov.w	.Lw_k1400,r0
	add	r0,r5
	add	r0,r6
	or	r6,r5
	mov.w	.Lw_k2800,r0
	cmp/hi	r0,r5
	bt	.Lw_tfar		! |dx| or |dy| beyond the fast range
	mov	#0,r6			! fast: direct oclut index
.Lw_tmath:				! ProjectPillar_2D_16_Frust(pdx r1, pdy r2)
	mov.l	@(28,r11),r3
	add	#12,r3			! &hw_frust.Nx
	mov.w	@r3+,r4			! Nx
	muls.w	r1,r4
	mov.w	@r3+,r5			! Ny
	sts	macl,r0			! pdx*Nx
	muls.w	r1,r5
	mov.l	@(28,r3),r1		! cz_origin
	sub	r1,r0
	sts	macl,r1			! pdx*Ny
	muls.w	r2,r4
	mov.l	@(32,r3),r4		! cx_origin
	sub	r4,r1
	sts	macl,r4			! pdy*Nx
	muls.w	r2,r5
	sub	r4,r1			! cx = pdx*Ny - cx_origin - pdy*Nx
	sts	macl,r4
	add	r4,r0			! cz = pdx*Nx - cz_origin + pdy*Ny
	mov.l	r1,@r7			! tp->cx
	mov.l	r0,@(4,r7)		! tp->cz
	mov	#4,r2
	shll16	r2
	cmp/ge	r2,r0
	bf/s	.Lw_tbehind
	mov	#0,r3
	shlr16	r0			! dist
	tst	r6,r6
	bt	.Lw_tlk
	mov.w	.Lw_k2048,r2		! FastDiv far table
	cmp/hs	r2,r0
	bf	.Lw_tlk
	shlr2	r0
	shlr	r0
	mov.w	.Lw_k1023,r3
	and	r3,r0
	add	r2,r0
.Lw_tlk:
	shll2	r0
	mov.l	@(32,r11),r2
	mov.l	@(r0,r2),r3		! s
	dmuls.l	r1,r3
	mov	#-96,r0
	extu.b	r0,r0			! HW_VIEW_CX
	sts	mach,r1
	mov.l	r3,@(8,r7)		! tp->s
	add	r1,r0
	rts
	mov.l	r0,@(12,r7)		! tp->px
.Lw_tbehind:
	mov.l	r3,@(8,r7)		! s = 0
	rts
	mov.l	r3,@(12,r7)		! px = 0
.Lw_tfar:				! HW_TransformPillar_Shifted(tp, pdx, pdy)
	mov.w	.Lw_k32767,r0
	mov	r1,r3
	add	r0,r3
	mov	r2,r4
	add	r0,r4
	or	r4,r3
	mov.w	.Lw_k65534,r0
	extu.w	r0,r0
	cmp/hi	r0,r3
	bf/s	.Lw_tmath		! 16-bit case: same math, FastDiv
	mov	#1,r6
	mov	#80,r0
	mov.l	@(r0,r11),r0
	mov	r7,r4
	mov	r1,r5
	jmp	@r0			! tail call (returns to our caller)
	mov	r2,r6

	.align	1
.Lw_k2048:	.word	2048
.Lw_k1023:	.word	1023
.Lw_k1400:	.word	1400
.Lw_k2800:	.word	2800
.Lw_k32767:	.word	32767
.Lw_k65534:	.word	-2

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


! ===================================================================
! hw_asm_lingen(ctx r4, s_min r5, s_max r6): drop-in for
! HW_EmitWallStrips_Linear_General (saturn_hw_walls.c, stage B2).
! Same arithmetic; the rare cases call the C helpers through
! hw_asm_lingen_ctx: [0] &saturn_cmd_m [4] &saturn_cmd_m_count
! [8] &v1w_cells [12] &hw_wall_quads [16] &hw_wall_strips [20] &v1w_drop
! [24] &v1w_captured [28] hw_wall_coord_tmp [32] TryCoalesce
! [36] IsStripOccluded_Slow [40] EmitStripQuad_Slow [44] ModNonPow2
! regs in the loop: r8 ctx, r9 s, r10 cmd_w2_cur, r11 s_px0 (full int),
! r12 L_top (px0<<16|u16 ya0), r13 L_bot, r14 lin_flags (C bit layout)
! frame: 0/4/8 cur px/ya0/ya1, 12/16/20 steps, 24 w2 step, 28 w2 base,
! 32 wrap, 36 s_max, 40 coal, 44 emit_cmd2, 48/52 R_top/R_bot save,
! 56 s_px1 save, 60 partial-first save
! ===================================================================
	.align	2
	.global	_hw_asm_lingen
_hw_asm_lingen:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	add	#-64,r15
	mov	r4,r8
	mov	r5,r9
	mov.l	r6,@(36,r15)
	mov	#92,r0
	mov.l	@(r0,r8),r10		! u_start
	add	#4,r0
	mov.l	@(r0,r8),r11
	add	r10,r11			! u_end
	mov	#68,r0
	mov.w	@(r0,r8),r12		! tw
	mov.w	@(24,r8),r0
	mov	r0,r13			! srca (sign-ext: only == and <<16 use it)
	mov	#0,r14
! can_coal
	mov	#32,r0
	cmp/eq	r0,r12
	bf	.Ll_cB
	mov	#70,r0
	mov.w	@(r0,r8),r0
	cmp/eq	r13,r0
	bt	.Ll_cB
	mov	r10,r0
	add	#31,r0
	mov	#-32,r1
	and	r1,r0
	add	#32,r0
	cmp/gt	r11,r0
	bt	.Ll_cB
	bra	.Ll_cdone
	mov	#1,r14
.Ll_cB:
	mov	#64,r0
	cmp/ge	r0,r12
	bf	.Ll_cdone
	mov	r12,r0
	tst	#63,r0
	bf	.Ll_cdone
	mov	#72,r0
	mov.w	@(r0,r8),r0
	cmp/eq	r13,r0
	bf	.Ll_cB2
	mov	#70,r0
	mov.w	@(r0,r8),r0
	cmp/eq	r13,r0
	bt	.Ll_cdone
.Ll_cB2:
	mov	r10,r0
	add	#63,r0
	mov	#-64,r1
	and	r1,r0
	add	#64,r0
	cmp/gt	r11,r0
	bt	.Ll_cdone
	mov	#1,r14
.Ll_cdone:
! partial first (bit 4) / partial last (bit 5)
	mov	r10,r0
	tst	#15,r0
	bt	.Ll_pf0
	add	#16,r14
.Ll_pf0:
	mov	r11,r0
	tst	#15,r0
	bt	.Ll_pl0
	add	#32,r14
.Ll_pl0:
! num_strips -> r12, cur_strip_idx -> @(40)
	cmp/pl	r12
	bf/s	.Ll_n1
	mov	#1,r0
	mov	r12,r0
	add	#15,r0
	shlr2	r0
	shlr2	r0
.Ll_n1:
	mov	r0,r12
	mov	r0,r1
	add	#-1,r1
	tst	r1,r0
	bf	.Ll_npow
	mov	r9,r0
	bra	.Ll_idx
	and	r1,r0
.Ll_npow:
	mov.l	.Ll_ctx,r0
	mov.l	@(44,r0),r0
	mov	r9,r4
	jsr	@r0
	mov	r12,r5
.Ll_idx:
	mov	r0,r3			! idx
! cmd_w2: base @28, step @24, wrap @32, cur @44
	mov	#90,r0
	mov.w	@(r0,r8),r0
	extu.b	r0,r2
	mov	#2,r1
	shll8	r1
	or	r1,r2			! size_lo
	mov.w	@(30,r8),r0
	add	r0,r0
	add	r13,r0
	shll16	r0
	or	r2,r0
	mov.l	r0,@(28,r15)		! base
	mov	r0,r2
	mov.w	@(26,r8),r0
	shll16	r0
	add	r0,r0			! th << 17
	mov.l	r0,@(24,r15)
	mul.l	r12,r0
	sts	macl,r1
	mov.l	r1,@(32,r15)		! wrap
	mul.l	r3,r0
	sts	macl,r0
	add	r2,r0
	mov.l	r0,@(44,r15)		! cmd_w2_cur
! geometry
	mov.l	@(40,r8),r1
	mov.l	@(12,r1),r11		! s_px0
	mov.l	@(44,r8),r1
	mov.l	@(12,r1),r2		! end_px
	mov.w	@(16,r8),r0
	mov	r0,r3			! s_ya0
	mov.w	@(18,r8),r0
	mov	r0,r4			! s_ya1
	mov.w	@(20,r8),r0
	mov	r0,r5			! end_ya0
	mov.w	@(22,r8),r0
	mov	r0,r6			! end_ya1
! wall_needs_clamp (bit 7)
	mov.l	.Ll_k500,r7
	mov	r11,r0
	add	r7,r0
	mov	r7,r1
	sub	r2,r1
	or	r1,r0
	mov	r4,r1
	sub	r3,r1
	or	r1,r0
	mov	r6,r1
	sub	r5,r1
	or	r1,r0
	mov.l	.Ll_k1022,r1
	cmp/hi	r1,r0
	bt	.Ll_clamp
	mov.l	.Ll_k1024,r7
	mov	r3,r0
	add	r7,r0
	mov	r5,r1
	add	r7,r1
	or	r1,r0
	mov	r4,r1
	add	r7,r1
	or	r1,r0
	mov	r6,r1
	add	r7,r1
	or	r1,r0
	mov.l	.Ll_k2047,r1
	cmp/hi	r1,r0
	bf	.Ll_noclamp
.Ll_clamp:
	mov	r14,r0
	or	#136,r0			! clamp (bit 7) + wall_slow (bit 3)
	mov	r0,r14
.Ll_noclamp:
! band_all_vis (bit 2), else wall_slow (bit 3)
	cmp/pz	r11
	bf	.Ll_nbav
	mov.l	.Ll_k320,r1
	cmp/gt	r1,r2
	bt	.Ll_nbav
	cmp/gt	r3,r4
	bf	.Ll_nbav
	cmp/gt	r5,r6
	bf	.Ll_nbav
	mov.w	@(12,r8),r0		! ytop_min
	cmp/gt	r0,r4
	bf	.Ll_nbav
	cmp/gt	r0,r6
	bf	.Ll_nbav
	mov.w	@(14,r8),r0		! ybot_max
	cmp/gt	r3,r0
	bf	.Ll_nbav
	cmp/gt	r5,r0
	bf	.Ll_nbav
	bra	.Ll_bav
	add	#4,r14
.Ll_nbav:
	mov	r14,r0
	or	#8,r0
	mov	r0,r14
.Ll_bav:
	mov.b	@(2,r8),r0		! occl_state
	tst	r0,r0
	bt	.Ll_nocc
	mov	r14,r0
	or	#72,r0			! has_occl (bit 6) + wall_slow
	mov	r0,r14
.Ll_nocc:
	mov	r14,r0			! first_slow (bit 1) = wall_slow | partial_first | clip
	tst	#24,r0
	bt	.Ll_fs0
	or	#2,r0
.Ll_fs0:
	mov	r0,r14
	mov	#76,r0
	mov.w	@(r0,r8),r0
	cmp/eq	#-1,r0
	bt	.Ll_fs1
	mov	r14,r0
	or	#2,r0
	mov	r0,r14
.Ll_fs1:
! steps (only if s_min < s_max)
	mov.l	@(36,r15),r0
	cmp/gt	r9,r0
	bf	.Ll_nost
	sub	r11,r2			! d_px
	sub	r3,r5			! d_ya0
	sub	r4,r6			! d_ya1
	mov	#100,r0
	mov.l	@(r0,r8),r7		! inv_u_span
	mov	r7,r1
	shlr2	r1
	shlr2	r1			! step_t1
	mov	r10,r0
	and	#15,r0
	tst	r0,r0
	bt/s	.Ll_al
	mov	r1,r12			! first_t1 = step_t1 when aligned
	mov	#16,r12
	sub	r0,r12
	mul.l	r7,r12
	sts	macl,r12
	shlr8	r12
.Ll_al:
	mov	#1,r7
	shll16	r7
	shlr	r7			! 0x8000
	mul.l	r1,r2
	sts	macl,r0
	mov.l	r0,@(12,r15)
	mul.l	r12,r2
	mov	r11,r13
	shll16	r13
	add	r7,r13
	sts	macl,r0
	add	r0,r13
	mov.l	r13,@(0,r15)
	mul.l	r1,r5
	sts	macl,r0
	mov.l	r0,@(16,r15)
	mul.l	r12,r5
	mov	r3,r13
	shll16	r13
	add	r7,r13
	sts	macl,r0
	add	r0,r13
	mov.l	r13,@(4,r15)
	mul.l	r1,r6
	sts	macl,r0
	mov.l	r0,@(20,r15)
	mul.l	r12,r6
	mov	r4,r13
	shll16	r13
	add	r7,r13
	sts	macl,r0
	add	r0,r13
	mov.l	r13,@(8,r15)
.Ll_nost:
	mov	r11,r12
	shll16	r12
	extu.w	r3,r0
	or	r0,r12			! L_top
	mov	r11,r13
	shll16	r13
	extu.w	r4,r0
	or	r0,r13			! L_bot
	mov.l	@(44,r15),r10		! cmd_w2_cur
	mov.l	@(36,r15),r0
	cmp/gt	r0,r9
	bt	.Ll_end
! ---------------- strip loop ----------------
.Ll_loop:
	mov	#0,r0
	mov.l	r0,@(40,r15)		! coal = 0
	mov	r14,r0
	tst	#1,r0
	bt	.Ll_nocoal
	mov	r9,r0
	tst	#1,r0
	bf	.Ll_nocoal
	mov.l	@(36,r15),r1
	cmp/ge	r1,r9
	bt	.Ll_nocoal
	bra	.Ll_coal
	nop
.Ll_nocoal:
	mov.l	@(36,r15),r0
	cmp/ge	r0,r9
	bt	.Ll_last
	mov.l	@(0,r15),r1
	mov.l	@(12,r15),r0
	add	r1,r0
	mov.l	r0,@(0,r15)
	mov.l	@(4,r15),r2
	mov.l	@(16,r15),r0
	add	r2,r0
	mov.l	r0,@(4,r15)
	mov.l	@(8,r15),r3
	mov.l	@(20,r15),r0
	add	r3,r0
	mov.l	r0,@(8,r15)
	swap.w	r1,r1
	xtrct	r1,r2			! R_top = px1<<16 | yb0
	xtrct	r1,r3			! R_bot
	bra	.Ll_have
	exts.w	r1,r1			! s_px1
.Ll_last:
	mov.l	@(44,r8),r1
	mov.l	@(12,r1),r1		! s_px1 = p1->px
	mov	r1,r4
	shll16	r4
	mov.w	@(20,r8),r0
	extu.w	r0,r2
	or	r4,r2
	mov.w	@(22,r8),r0
	extu.w	r0,r3
	or	r4,r3
	mov	r14,r0
	shlr2	r0
	shlr2	r0
	and	#2,r0
	or	r0,r14			! partial_last -> cur_slow
.Ll_have:
	cmp/gt	r11,r1
	bf	.Ll_next
	mov	r14,r0
	tst	#2,r0
	bf	.Ll_slow
.Ll_fast:
	mov.l	.Ll_ctx,r4
	mov.l	@(4,r4),r5
	mov.w	@r5,r0
	extu.w	r0,r0
	mov.l	.Ll_k1279,r6
	cmp/hs	r6,r0
	bt	.Ll_drop
	mov	r0,r6
	add	#1,r6
	mov.w	r6,@r5
	shll2	r0
	shll2	r0
	shll	r0
	mov.l	@r4,r6
	mov.l	@r6,r6
	add	r0,r6			! &saturn_cmd_m[m_cnt]
	mov.l	.Ll_k20000,r0
	mov.l	r0,@r6
	mov	#104,r0
	mov.l	@(r0,r8),r0
	mov.l	r0,@(4,r6)
	mov.l	@(40,r15),r7
	tst	r7,r7
	bt/s	.Ll_f1
	mov	r10,r0
	mov.l	@(44,r15),r0
.Ll_f1:
	mov.l	r0,@(8,r6)
	mov.l	r12,@(12,r6)
	mov.l	r2,@(16,r6)
	mov.l	r3,@(20,r6)
	mov.l	r13,@(24,r6)
	mov	#0,r0
	mov.l	r0,@(28,r6)
	tst	r7,r7
	mov.l	.Ll_k10100,r0
	bt	.Ll_f2
	mov.l	.Ll_k10000,r0
.Ll_f2:
	add	r0,r14
.Ll_next:
	mov	r1,r11
	mov	r2,r12
	mov	r3,r13
	mov	r14,r0
	tst	#2,r0
	bt	.Ll_n2
	shlr2	r0
	and	#2,r0
	mov	r0,r1
	mov	#-19,r0
	and	r14,r0
	or	r1,r0
	mov	r0,r14
	mov	#76,r0
	mov.w	@(r0,r8),r0
	cmp/eq	#-1,r0
	bt	.Ll_n2
	mov	r14,r0
	or	#2,r0
	mov	r0,r14
.Ll_n2:
	mov.l	@(24,r15),r0
	add	r0,r10
	mov.l	@(32,r15),r1
	tst	r1,r1
	bt	.Ll_nw
	mov.l	@(28,r15),r2
.Ll_w:
	mov	r10,r0
	sub	r2,r0
	cmp/hs	r1,r0
	bf	.Ll_nw
	bra	.Ll_w
	sub	r1,r10
.Ll_nw:
	add	#1,r9
	mov.l	@(36,r15),r0
	cmp/gt	r0,r9
	bt	.Ll_end
	bra	.Ll_loop
	nop
.Ll_end:
	mov.l	.Ll_ctx,r4
	mov	r14,r0
	shlr16	r0			! quads
	mov	r14,r1
	shlr8	r1
	extu.b	r1,r1			! strips
	mov.l	@(8,r4),r2
	mov.l	@r2,r3
	add	r0,r3
	mov.l	r3,@r2
	mov.l	@(12,r4),r2
	mov.l	@r2,r3
	add	r0,r3
	mov.l	r3,@r2
	mov.l	@(16,r4),r2
	mov.l	@r2,r3
	add	r1,r3
	mov.l	r3,@r2
	add	#64,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

	.align	2
.Ll_ctx:	.long	_hw_asm_lingen_ctx
.Ll_k500:	.long	500
.Ll_k1022:	.long	1022
.Ll_k1024:	.long	1024
.Ll_k2047:	.long	2047
.Ll_k320:	.long	320
.Ll_k1279:	.long	1279
.Ll_k20000:	.long	0x00020000
.Ll_k10100:	.long	0x00010100
.Ll_k10000:	.long	0x00010000

! ---- cold: command table full ----
.Ll_drop:
	mov.l	@(20,r4),r5
	mov.l	@r5,r0
	add	#1,r0
	mov.l	r0,@r5
	mov.l	@(24,r4),r5
	mov	#1,r0
	bra	.Ll_next
	mov.l	r0,@r5

! ---- coalesce (C helper decides, like the C loop) ----
.Ll_coal:
	mov.l	.Ll_ctx2,r1
	mov.l	@(28,r1),r2
	mov.l	@(0,r15),r0
	mov.l	r0,@r2
	mov.l	@(12,r15),r0
	mov.l	r0,@(4,r2)
	mov.l	@(28,r15),r0
	mov.l	r0,@(8,r2)
	mov.l	@(32,r1),r0
	mov	r9,r4
	mov	r11,r5
	jsr	@r0
	mov	r10,r6
	cmp/pl	r0
	bf	.Ll_c_no
	mov.l	r0,@(40,r15)		! coal
	mov	r0,r3
	add	#-1,r3			! adv
	add	r3,r9
	mov.l	.Ll_ctx2,r1
	mov.l	@(28,r1),r2
	mov.l	@r2,r0
	mov.l	r0,@(44,r15)		! emit_cmd2
	mov.l	@(4,r2),r10		! cmd_w2_cur
	mov.l	@(12,r15),r0
	mul.l	r3,r0
	mov.l	@(0,r15),r1
	sts	macl,r0
	add	r0,r1
	mov.l	r1,@(0,r15)
	mov.l	@(16,r15),r0
	mul.l	r3,r0
	mov.l	@(4,r15),r1
	sts	macl,r0
	add	r0,r1
	mov.l	r1,@(4,r15)
	mov.l	@(20,r15),r0
	mul.l	r3,r0
	mov.l	@(8,r15),r1
	sts	macl,r0
	add	r0,r1
	mov.l	r1,@(8,r15)
.Ll_c_no:
	bra	.Ll_nocoal
	nop

! ---- slow strip: visibility / occlusion / clipped or clamped quad ----
.Ll_slow:
	mov.l	r1,@(56,r15)
	mov.l	r2,@(48,r15)
	mov.l	r3,@(52,r15)
	mov	r14,r0
	and	#16,r0
	mov.l	r0,@(60,r15)		! is_partial_first
	mov	#-17,r0
	and	r0,r14
	mov	r14,r0
	tst	#4,r0
	bf	.Ll_s_vis
	cmp/pl	r1
	bf	.Ll_s_next
	mov.l	.Ll_k320b,r0
	cmp/ge	r0,r11
	bt	.Ll_s_next
	exts.w	r12,r4			! s_ya0
	exts.w	r13,r5			! s_ya1
	exts.w	r2,r6			! s_yb0
	exts.w	r3,r7			! s_yb1
	cmp/ge	r5,r4
	bf	.Ll_s_v1
	cmp/ge	r7,r6
	bt	.Ll_s_next
.Ll_s_v1:
	mov.w	@(12,r8),r0
	cmp/ge	r5,r0
	bf	.Ll_s_v2
	cmp/ge	r7,r0
	bt	.Ll_s_next
.Ll_s_v2:
	mov.w	@(14,r8),r0
	cmp/ge	r0,r4
	bf	.Ll_s_vis
	cmp/ge	r0,r6
	bt	.Ll_s_next
.Ll_s_vis:
	mov	r14,r0
	tst	#64,r0
	bt	.Ll_s_nocc
	mov.l	.Ll_ctx2,r0
	mov.l	@(36,r0),r0
	mov	r11,r4
	mov.l	@(40,r15),r6
	jsr	@r0
	mov	r1,r5
	tst	r0,r0
	bf	.Ll_s_skip
	mov.l	@(56,r15),r1
	mov.l	@(48,r15),r2
	mov.l	@(52,r15),r3
.Ll_s_nocc:
	mov.l	@(40,r15),r7		! coal
	mov.l	@(60,r15),r6		! is_partial_first
	mov	#0,r5			! is_partial_last
	mov.l	@(36,r15),r0
	cmp/eq	r0,r9
	bf	.Ll_s_q0
	mov	r14,r0
	and	#32,r0
	mov	r0,r5
.Ll_s_q0:
	tst	r7,r7
	bf	.Ll_s_q1
	mov	r6,r0
	or	r5,r0
	tst	r0,r0
	bf	.Ll_s_q
.Ll_s_q1:
	mov	r14,r0
	tst	#128,r0
	bf	.Ll_s_q
	mov	#76,r0
	mov.w	@(r0,r8),r0
	cmp/eq	#-1,r0
	bf	.Ll_s_q
	bra	.Ll_fast
	nop
.Ll_s_q:
	tst	r6,r6
	bt	.Ll_s_u0
	mov	#92,r0
	bra	.Ll_s_u1
	mov.l	@(r0,r8),r4		! u_lo = u_start
.Ll_s_u0:
	mov	r9,r4
	tst	r7,r7
	bt	.Ll_s_u2
	sub	r7,r4
	add	#1,r4
.Ll_s_u2:
	shll2	r4
	shll2	r4
.Ll_s_u1:
	mov.l	@(36,r15),r0
	cmp/eq	r0,r9
	bf	.Ll_s_u3
	mov	#92,r0
	mov.l	@(r0,r8),r5
	add	#4,r0
	mov.l	@(r0,r8),r0
	bra	.Ll_s_u4
	add	r0,r5			! u_hi = u_start + u_span
.Ll_s_u3:
	mov	r9,r5
	add	#1,r5
	shll2	r5
	shll2	r5
.Ll_s_u4:
	extu.w	r4,r4
	shll16	r5
	or	r4,r5			! u_lo | u_hi << 16
	mov.l	.Ll_ctx2,r4
	mov.l	@(28,r4),r4		! hw_wall_coord_tmp
	mov.l	r11,@r4
	mov.l	r1,@(4,r4)
	exts.w	r12,r0
	mov.l	r0,@(8,r4)
	exts.w	r2,r0
	mov.l	r0,@(12,r4)
	exts.w	r13,r0
	mov.l	r0,@(16,r4)
	exts.w	r3,r0
	mov.l	r0,@(20,r4)
	tst	r7,r7
	bt/s	.Ll_s_e
	mov	r10,r7
	mov.l	@(44,r15),r7		! emit_cmd2
.Ll_s_e:
	mov	#104,r0
	mov.l	@(r0,r8),r6		! cmd_header1
	mov.l	.Ll_ctx2,r0
	mov.l	@(40,r0),r0
	jsr	@r0
	mov	r9,r4
	tst	r0,r0
	bt	.Ll_s_skip
	mov.l	@(40,r15),r1
	tst	r1,r1
	mov.l	.Ll_k10100b,r0
	bt	.Ll_s_c
	mov.l	.Ll_k10000b,r0
.Ll_s_c:
	add	r0,r14
.Ll_s_skip:
	mov.l	@(56,r15),r1
	mov.l	@(48,r15),r2
	mov.l	@(52,r15),r3
.Ll_s_next:
	bra	.Ll_next
	nop

	.align	2
.Ll_ctx2:	.long	_hw_asm_lingen_ctx
.Ll_k320b:	.long	320
.Ll_k10100b:	.long	0x00010100
.Ll_k10000b:	.long	0x00010000
! ===================================================================
! hw_asm_commit(): HW_CommitSubsectorOcclusion (saturn_hw_occl.c) with
! HW_CommitSubSolidMask, HW_CommitPortalSpan, HW_UpdatePortalWordPartial
! and HW_UpdatePortalWordCols inlined (stage C1). HW_CommitPortalClosedWord
! stays C (cold); the rescan calls hw_asm_rescan. Table hw_asm_commit_ctx:
! [0] ap [4] hw_solid_mask [8] hw_ap_words [12] sub_solid_mask
! [16] &sub_solid_active [20] &sub_solid_min_w [24] &sub_solid_max_w
! [28] &hw_solid_any [32] &hw_slave_dirty_words [36] &hw_solid_summary
! [40] &hw_solid_words [44] &hw_solid_count [48] &saturn_view_h
! [52] hw_bit_lut [56] HW_CommitPortalClosedWord
! ap: +2 aperture_active +3 portal_span_count +8 dirty_words +24 spans,
! +408 ytop, +760 ybot. regs: r9 touched, r10 ps, r12 ap, r13 solid mask,
! r14 ap_words, r8 w. frame: 0 ps_end 4 w1 8 check 12 vh 16 touch 20 x_s
! 24 x_e 28 x_w0 32 w_mod 36 w 40 flags 44 ps
! ===================================================================
	.align	2
	.global	_hw_asm_commit
_hw_asm_commit:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	add	#-48,r15
	mov.l	.Lc_ctx0,r1
	mov.l	@r1,r12
	mov.l	@(4,r1),r13
	mov.l	@(8,r1),r14
	mov.l	@(48,r1),r0
	mov.l	@r0,r0
	mov.l	r0,@(12,r15)		! vh (int)
	mov	#0,r9
	mov.l	@(16,r1),r2
	mov.l	@r2,r0
	tst	r0,r0
	bt	.Lc_nosub
! ---- HW_CommitSubSolidMask ----
	mov.l	@(20,r1),r0
	mov.l	@r0,r3			! i = min
	mov.l	@(24,r1),r0
	mov.l	@r0,r4			! max
	mov.l	@(12,r1),r5		! sub_solid_mask
.Lc_s_loop:
	cmp/gt	r4,r3
	bt	.Lc_s_done
	mov	r3,r0
	shll2	r0
	mov.l	@(r0,r5),r6		! bits
	tst	r6,r6
	bt	.Lc_s_next
	mov	#0,r7
	mov.l	r7,@(r0,r5)
	mov.l	@(r0,r13),r7		! old
	or	r7,r6			! new
	cmp/eq	r7,r6
	bt	.Lc_s_next
	mov.l	r6,@(r0,r13)
	mov.l	@(52,r1),r7
	mov.l	@(r0,r7),r7		! wbit
	mov.l	@(28,r1),r2
	mov.l	@r2,r0
	or	r7,r0
	mov.l	r0,@r2
	mov.l	@(32,r1),r2
	mov.l	@r2,r0
	or	r7,r0
	mov.l	r0,@r2
	mov	r3,r2
	shll2	r2
	add	r2,r2
	add	r14,r2			! aw
	mov	#-1,r0
	cmp/eq	r0,r6
	bf	.Lc_s_part
	mov.l	@(40,r1),r6
	mov.l	@r6,r0
	add	#1,r0
	mov.l	r0,@r6
	mov.l	@(36,r1),r6
	mov.l	@r6,r0
	or	r7,r0
	mov.l	r0,@r6
	mov.l	@(12,r15),r0
	mov.w	r0,@r2			! yt_min = vh
	mov.w	r0,@(4,r2)		! yt_max = vh
	mov	#0,r0
	mov.w	r0,@(2,r2)
	mov.w	r0,@(6,r2)
	mov.l	@(8,r12),r0
	or	r7,r0
	bra	.Lc_s_next
	mov.l	r0,@(8,r12)
.Lc_s_part:
	mov.l	@(8,r12),r0
	tst	r7,r0
	bt	.Lc_s_next
	mov.l	@r2,r0
	mov.l	@(4,r2),r6
	cmp/eq	r6,r0
	bt	.Lc_s_next
	or	r7,r9
.Lc_s_next:
	bra	.Lc_s_loop
	add	#1,r3
	.align	2
.Lc_ctx0:	.long	_hw_asm_commit_ctx
.Lc_s_done:
	mov.l	@(16,r1),r2
	mov	#0,r0
	mov.l	r0,@r2			! sub_solid_active = 0
	mov.l	@(40,r1),r2
	mov.l	@r2,r3
	mov	#10,r2
	cmp/ge	r2,r3
	bf/s	.Lc_s_cnt
	mov	#1,r0
	mov	#80,r0
	shll2	r0
.Lc_s_cnt:
	mov.l	@(44,r1),r2
	mov.l	r0,@r2
! ---- portal spans ----
.Lc_nosub:
	mov.b	@(3,r12),r0
	extu.b	r0,r0
	tst	r0,r0
	bt	.Lc_fin
	mov	r0,r1
	mov	#0,r0
	mov.b	r0,@(3,r12)
	mov	r12,r10
	add	#24,r10
	shll2	r1
	add	r1,r1
	add	r10,r1
	mov.l	r1,@(0,r15)
.Lc_span:
	mov.w	@r10,r1			! sx
	mov.w	@(2,r10),r0
	mov	r0,r2			! ex
	cmp/pz	r1
	bt	.Lc_sp0
	mov	#0,r1
.Lc_sp0:
	mov	#80,r3
	shll2	r3
	cmp/gt	r3,r2
	bf	.Lc_sp1
	mov	r3,r2
.Lc_sp1:
	cmp/ge	r2,r1
	bt	.Lc_span_next
	mov.b	@(4,r10),r0
	extu.b	r0,r3			! yt
	mov.b	@(5,r10),r0
	extu.b	r0,r4			! yb
	mov.b	@(6,r10),r0
	extu.b	r0,r5			! yt_att
	mov.b	@(7,r10),r0
	extu.b	r0,r6			! yb_att
	mov	#0,r7
	cmp/pl	r3
	bf	.Lc_sp2
	mov	r3,r0
	add	#1,r0
	cmp/gt	r0,r5
	bt	.Lc_sp2
	mov	#1,r7			! check_yt
.Lc_sp2:
	mov.l	@(12,r15),r0
	cmp/gt	r4,r0
	bf	.Lc_sp3
	mov	r4,r0
	add	#-1,r0
	cmp/ge	r0,r6
	bf	.Lc_sp3
	add	#2,r7			! check_yb
.Lc_sp3:
	tst	r7,r7
	bt	.Lc_span_next
	mov.l	r7,@(8,r15)
	mov	r1,r8
	shlr2	r8
	shlr2	r8
	shlr	r8			! w0
	add	#-1,r2
	shlr2	r2
	shlr2	r2
	shlr	r2
	mov.l	r2,@(4,r15)		! w1
.Lc_word:
	mov	r8,r0
	shll2	r0
	mov.l	@(r0,r13),r1		! sm
	mov	#-1,r2
	cmp/eq	r2,r1
	bt	.Lc_word_next
	mov.l	.Lc_ctx,r2
	mov.l	@(52,r2),r2
	mov.l	@(r0,r2),r7		! wbit
	mov	r8,r2
	shll2	r2
	add	r2,r2
	add	r14,r2			! aw
	mov.b	@(4,r10),r0
	extu.b	r0,r3
	mov.b	@(5,r10),r0
	extu.b	r0,r4
	mov.b	@(6,r10),r0
	extu.b	r0,r5
	mov.b	@(7,r10),r0
	extu.b	r0,r6
	mov	#0,r11
	mov.l	@(8,r15),r0
	tst	#1,r0
	bt	.Lc_nyt
	tst	r7,r9
	bf	.Lc_yt1
	mov.w	@r2,r0			! yt_min
	cmp/gt	r0,r3
	bf	.Lc_nyt
	mov.w	@(4,r2),r0		! yt_max
	add	#2,r0
	cmp/gt	r0,r5
	bt	.Lc_nyt
.Lc_yt1:
	mov	#1,r11
.Lc_nyt:
	mov.l	@(8,r15),r0
	tst	#2,r0
	bt	.Lc_nyb
	tst	r7,r9
	bf	.Lc_yb1
	mov.w	@(2,r2),r0		! yb_max
	cmp/gt	r4,r0
	bf	.Lc_nyb
	mov.w	@(6,r2),r0		! yb_min
	add	#-2,r0
	cmp/ge	r0,r6
	bf	.Lc_nyb
.Lc_yb1:
	add	#2,r11
.Lc_nyb:
	tst	r11,r11
	bt	.Lc_word_next
	tst	r7,r9
	bf	.Lc_tch
	tst	r1,r1
	bf	.Lc_fl0
	bra	.Lc_fl0
	add	#4,r11
.Lc_tch:
	add	#8,r11
.Lc_fl0:
	mov	#0,r0
	mov.l	r0,@(16,r15)		! touch_ret = 0
	mov	r8,r1
	shll2	r1
	shll2	r1
	shll	r1			! x_w0
	mov.l	r1,@(28,r15)
	mov.w	@r10,r0
	cmp/gt	r1,r0
	bt	.Lc_xs
	mov	r1,r0
.Lc_xs:
	mov.l	r0,@(20,r15)		! x_s
	add	#32,r1			! x_w1
	mov.w	@(2,r10),r0
	cmp/gt	r1,r0
	bf	.Lc_xe
	mov	r1,r0
.Lc_xe:
	mov.l	r0,@(24,r15)		! x_e
! ---- HW_UpdatePortalWordPartial ----
	mov	r11,r0
	tst	#4,r0
	bt	.Lc_cols
	mov	#0,r1
	tst	#1,r0
	bt	.Lc_p0
	mov.w	@r2,r0
	add	#2,r0
	cmp/gt	r0,r5
	bt	.Lc_p0
	mov.w	@(4,r2),r0
	cmp/ge	r0,r3
	bf	.Lc_p0
	mov	#1,r1			! full_yt
.Lc_p0:
	mov	r11,r0
	tst	#2,r0
	bt	.Lc_p1
	mov.w	@(2,r2),r0
	add	#-2,r0
	cmp/ge	r0,r6
	bf	.Lc_p1
	mov.w	@(6,r2),r0
	cmp/ge	r4,r0
	bf	.Lc_p1
	add	#2,r1			! full_yb
.Lc_p1:
	tst	r1,r1
	bt	.Lc_cols
	mov.w	@(4,r2),r0
	mov	r0,r5			! next_yt_max
	mov	r1,r0
	tst	#1,r0
	bt	.Lc_p2
	mov	r3,r5
.Lc_p2:
	mov.w	@(6,r2),r0
	mov	r0,r6			! next_yb_min
	mov	r1,r0
	tst	#2,r0
	bt	.Lc_p3
	mov	r4,r6
.Lc_p3:
	cmp/ge	r6,r5
	bt	.Lc_cols_rl
	mov	r1,r0
	tst	#1,r0
	bt	.Lc_p_yb
	mov.l	@(20,r15),r5
	mov.l	@(24,r15),r0
	sub	r5,r0			! count
	mov	#102,r6
	shll2	r6
	add	r12,r6
	add	r5,r6			! &ytop[x_s]
	bsr	.Lc_fill
	mov	r3,r5
	bf	.Lc_p_ytfull
	mov.w	@(4,r2),r0
	mov	r0,r5
	mov.w	@r2,r0
	cmp/eq	r5,r0
	bf	.Lc_p_yttch
	mov	r3,r0
	bra	.Lc_p_ytdone
	mov.w	r0,@(4,r2)
.Lc_p_yttch:
	mov	#1,r0
	bra	.Lc_p_ytdone
	mov.l	r0,@(16,r15)
.Lc_p_ytfull:
	mov	r3,r0
	mov.w	r0,@r2
	mov.w	r0,@(4,r2)
.Lc_p_ytdone:
	mov.l	@(8,r12),r0
	or	r7,r0
	mov.l	r0,@(8,r12)
	mov	#1,r0
	mov.b	r0,@(2,r12)
	mov	#-2,r0
	and	r0,r11
.Lc_p_yb:
	mov	r1,r0
	tst	#2,r0
	bt	.Lc_p_end
	mov.l	@(20,r15),r5
	mov.l	@(24,r15),r0
	sub	r5,r0
	mov	#95,r6
	shll2	r6
	shll	r6			! 760
	add	r12,r6
	add	r5,r6			! &ybot[x_s]
	bsr	.Lc_fill
	mov	r4,r5
	bf	.Lc_p_ybfull
	mov.w	@(2,r2),r0
	mov	r0,r5
	mov.w	@(6,r2),r0
	cmp/eq	r5,r0
	bf	.Lc_p_ybtch
	mov	r4,r0
	bra	.Lc_p_ybdone
	mov.w	r0,@(6,r2)
.Lc_p_ybtch:
	mov	#1,r0
	bra	.Lc_p_ybdone
	mov.l	r0,@(16,r15)
.Lc_p_ybfull:
	mov	r4,r0
	mov.w	r0,@(2,r2)
	mov.w	r0,@(6,r2)
.Lc_p_ybdone:
	mov.l	@(8,r12),r0
	or	r7,r0
	mov.l	r0,@(8,r12)
	mov	#1,r0
	mov.b	r0,@(2,r12)
	mov	#-3,r0
	and	r0,r11
.Lc_p_end:
	mov	r11,r0
	tst	#3,r0
	bf	.Lc_cols_rl
	mov.l	@(16,r15),r0
	tst	r0,r0
	bt	.Lc_res0
	bra	.Lc_res1
	nop
! ---- HW_UpdatePortalWordCols ----
.Lc_cols_rl:
	mov.b	@(6,r10),r0
	extu.b	r0,r5
	mov.b	@(7,r10),r0
	extu.b	r0,r6
.Lc_cols:
	add	#-2,r5
	cmp/pz	r5
	bt	.Lc_c0
	mov	#0,r5			! yt_req = max(yt_att - 2, 0)
.Lc_c0:
	add	#2,r6			! yb_req
	mov.l	r8,@(36,r15)
	mov.l	r11,@(40,r15)
	mov.l	r10,@(44,r15)
	mov.l	@(16,r15),r0
	mov.l	r0,@(32,r15)		! w_mod = touch_ret
	mov.l	@(20,r15),r0
	mov.l	@(24,r15),r10
	sub	r0,r10			! count
	mov	#102,r1
	shll2	r1
	add	r12,r1
	add	r0,r1			! &ytop[x_s]
	mov	#88,r2
	shll2	r2
	add	r1,r2			! &ybot[x_s]
	and	#31,r0
	shll2	r0
	mov.l	.Lc_ctx,r8
	mov.l	@(52,r8),r8
	mov.l	@(r0,r8),r8		! bit
	mov	#0,r7			! newly_closed
	mov	r11,r0
	tst	#2,r0
	bt	.Lc_lt
	tst	#1,r0
	bt	.Lc_lb
.Lc_lbt:
	mov.b	@r1,r0
	extu.b	r0,r0			! yt
	mov.b	@r2,r11
	extu.b	r11,r11			! yb
	cmp/hs	r5,r0
	bf	.Lc_bt3
	cmp/hs	r3,r0
	bt	.Lc_bt3
	mov	r3,r0
	mov.b	r3,@r1
	mov.l	r8,@(32,r15)
	cmp/hi	r4,r11
	bf	.Lc_bt4
	cmp/hi	r6,r11
	bt	.Lc_bt4
	mov	r4,r11
	mov.b	r4,@r2
.Lc_bt4:
	cmp/ge	r11,r0
	bf	.Lc_bt5
	bra	.Lc_bt5
	or	r8,r7
.Lc_bt3:
	cmp/hi	r4,r11
	bf	.Lc_bt5
	cmp/hi	r6,r11
	bt	.Lc_bt5
	mov	r4,r11
	mov.b	r4,@r2
	mov.l	r8,@(32,r15)
	cmp/ge	r11,r0
	bf	.Lc_bt5
	or	r8,r7
.Lc_bt5:
	add	#1,r1
	add	#1,r2
	shll	r8
	dt	r10
	bf	.Lc_lbt
	bra	.Lc_post
	nop
.Lc_lt:
	mov.b	@r1,r0
	extu.b	r0,r0
	cmp/hs	r5,r0
	bf	.Lc_lt1
	cmp/hs	r3,r0
	bt	.Lc_lt1
	mov.b	r3,@r1
	mov.l	r8,@(32,r15)
	mov.b	@r2,r11
	extu.b	r11,r11
	cmp/ge	r11,r3
	bf	.Lc_lt1
	or	r8,r7
.Lc_lt1:
	add	#1,r1
	add	#1,r2
	shll	r8
	dt	r10
	bf	.Lc_lt
	bra	.Lc_post
	nop
.Lc_lb:
	mov.b	@r2,r11
	extu.b	r11,r11
	cmp/hi	r4,r11
	bf	.Lc_lb1
	cmp/hi	r6,r11
	bt	.Lc_lb1
	mov.b	r4,@r2
	mov.l	r8,@(32,r15)
	mov.b	@r1,r0
	extu.b	r0,r0
	cmp/ge	r4,r0
	bf	.Lc_lb1
	or	r8,r7
.Lc_lb1:
	add	#1,r1
	add	#1,r2
	shll	r8
	dt	r10
	bf	.Lc_lb
.Lc_post:
	mov.l	@(36,r15),r8
	mov.l	@(40,r15),r11
	mov.l	@(44,r15),r10
	mov	r8,r2
	shll2	r2
	add	r2,r2
	add	r14,r2			! aw
	tst	r7,r7
	bf	.Lc_closed
	mov.l	@(32,r15),r0
	tst	r0,r0
	bt	.Lc_res0
	mov.l	@(16,r15),r0
	tst	r0,r0
	bf	.Lc_res1
	mov	r11,r0
	tst	#8,r0
	bf	.Lc_res1
	mov	#0,r1			! partial_clean
	tst	#4,r0
	bt	.Lc_pc0
	mov.l	@(28,r15),r7
	mov.l	@(20,r15),r0
	cmp/gt	r7,r0
	bt	.Lc_pc1
	add	#32,r7
	mov.l	@(24,r15),r0
	cmp/gt	r0,r7
	bf	.Lc_pc0
.Lc_pc1:
	mov	#1,r1
.Lc_pc0:
	mov	r11,r0
	tst	#1,r0
	bt	.Lc_okyt
	mov.w	@r2,r0
	cmp/gt	r0,r5
	bt	.Lc_okyt
	tst	r1,r1
	bt	.Lc_res1
	mov	r0,r7
	mov.w	@(4,r2),r0
	cmp/eq	r7,r0
	bf	.Lc_res1
.Lc_okyt:
	mov	r11,r0
	tst	#2,r0
	bt	.Lc_okyb
	mov.w	@(2,r2),r0
	cmp/gt	r6,r0
	bt	.Lc_okyb
	tst	r1,r1
	bt	.Lc_res1
	mov	r0,r7
	mov.w	@(6,r2),r0
	cmp/eq	r7,r0
	bf	.Lc_res1
.Lc_okyb:
	mov	r11,r0
	tst	#1,r0
	bt	.Lc_u1
	mov.w	@(4,r2),r0
	cmp/gt	r0,r3
	bf	.Lc_u1
	mov	r3,r0
	mov.w	r0,@(4,r2)
.Lc_u1:
	mov	r11,r0
	tst	#2,r0
	bt	.Lc_u2
	mov.w	@(6,r2),r0
	cmp/gt	r4,r0
	bf	.Lc_u2
	mov	r4,r0
	mov.w	r0,@(6,r2)
.Lc_u2:
	bsr	.Lc_wbit
	nop
	mov.l	@(8,r12),r0
	or	r1,r0
	mov.l	r0,@(8,r12)
	mov	#1,r0
	bra	.Lc_res0
	mov.b	r0,@(2,r12)
.Lc_closed:
	mov.l	.Lc_ctx,r0
	mov.l	@(56,r0),r0
	mov	r8,r4
	jsr	@r0
	mov	r7,r5
	tst	r0,r0
	bt	.Lc_res1
	bsr	.Lc_wbit
	nop
	not	r1,r1
	bra	.Lc_word_next
	and	r1,r9
.Lc_res1:
	bsr	.Lc_wbit
	nop
	or	r1,r9
	mov	#1,r0
	mov.b	r0,@(2,r12)
.Lc_res0:
.Lc_word_next:
	mov.l	@(4,r15),r0
	add	#1,r8
	cmp/gt	r0,r8
	bt	.Lc_span_next
	bra	.Lc_word
	nop
.Lc_span_next:
	mov.l	@(0,r15),r0
	add	#8,r10
	cmp/hs	r0,r10
	bt	.Lc_fin
	bra	.Lc_span
	nop
.Lc_fin:
	tst	r9,r9
	bt	.Lc_ret
	mov.l	@(8,r12),r0
	or	r9,r0
	mov.l	r0,@(8,r12)
	mov	r9,r4
	mov	r13,r5
	mov	r14,r6
	mov	#102,r7
	shll2	r7
	mov.l	.Lc_rescan,r0
	jsr	@r0
	add	r12,r7
.Lc_ret:
	add	#48,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! r1 = hw_bit_lut[w=r8]; clobbers r0
.Lc_wbit:
	mov.l	.Lc_ctx,r1
	mov.l	@(52,r1),r1
	mov	r8,r0
	shll2	r0
	rts
	mov.l	@(r0,r1),r1

! fill r0 = count (1..32) bytes at r6 with r5; count 32 = a whole aligned
! word: 8 long stores, returns T=0. Byte path returns T=1. Clobbers r0,r5,r6.
.Lc_fill:
	tst	#32,r0
	bt	.Lc_fb
	extu.b	r5,r5
	mov	r5,r0
	shll8	r0
	or	r0,r5
	mov	r5,r0
	shll16	r0
	or	r0,r5
	mov.l	r5,@r6
	mov.l	r5,@(4,r6)
	mov.l	r5,@(8,r6)
	mov.l	r5,@(12,r6)
	mov.l	r5,@(16,r6)
	mov.l	r5,@(20,r6)
	mov.l	r5,@(24,r6)
	rts
	mov.l	r5,@(28,r6)
.Lc_fb:
	mov.b	r5,@r6
	dt	r0
	bf/s	.Lc_fb
	add	#1,r6
	rts
	nop

	.align	2
.Lc_ctx:	.long	_hw_asm_commit_ctx
.Lc_k320:	.long	320
.Lc_rescan:	.long	_hw_asm_rescan
! ===================================================================
! hw_asm_portal(ctx r4, bsec_idx r5): HW_RecordSegPortalAperture
! (saturn_hw_walls.c) with HW_RecordPortalSpan inlined (stage C2).
! HW_ClipPortalScales (cold) stays C. Table hw_asm_portal_ctx:
! [0] hw_sec_fast [4] &hw_aperture_state [8] &saturn_view_cy
! [12] &saturn_view_h [16] HW_ClipPortalScales [20] hw_wall_coord_tmp
! regs: r8 ctx, r9 pad, r10 p_yt, r11 p_yb, r12 s_min, r13 s_max,
! r14 &hw_sec_fast[bsec], r4 vh, r5 vcy, r6 yt_att, r7 yb_att
! ===================================================================
	.align	2
	.global	_hw_asm_portal
_hw_asm_portal:
	mov	#1,r0
	mov.b	r0,@r4			! portal_recorded = 1
	mov.w	@(8,r4),r0
	mov	r0,r1			! seg_sx
	mov.w	@(10,r4),r0
	cmp/ge	r0,r1
	bt	.Lq_rts			! seg_sx >= seg_ex
	mov	r5,r0
	shlr8	r0
	tst	r0,r0
	bf	.Lq_rts			! (unsigned)bsec >= 256
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	mov	r4,r8
	mov	r5,r9
	mov.l	@(40,r8),r0
	mov.l	@(12,r0),r6		! xa
	mov.l	@(8,r0),r10		! s_A
	mov.l	@(44,r8),r0
	mov.l	@(12,r0),r7		! xb
	mov.l	@(8,r0),r11		! s_B
	cmp/gt	r6,r7
	bf	.Lq_noclip		! xb <= xa
	cmp/pz	r6
	bf	.Lq_clip		! xa < 0
	mov	#80,r0
	shll2	r0
	cmp/gt	r0,r7
	bf	.Lq_noclip		! xb <= 320
.Lq_clip:
	mov.l	.Lq_ctx,r0
	mov.l	@(16,r0),r0
	mov	r6,r4
	mov	r7,r5
	mov	r10,r6
	jsr	@r0
	mov	r11,r7
	mov.l	.Lq_ctx,r0
	mov.l	@(20,r0),r0
	mov.l	@r0,r10
	mov.l	@(4,r0),r11
.Lq_noclip:
	mov	r10,r12			! s_min
	mov	r11,r13			! s_max
	cmp/gt	r11,r10
	bf	.Lq_mm
	mov	r11,r12
	mov	r10,r13
.Lq_mm:
	mov.l	.Lq_ctx,r1
	mov.l	@r1,r14
	shll2	r9
	shll2	r9
	add	r9,r14			! &hw_sec_fast[bsec]
	mov.l	@(8,r1),r0
	mov.l	@r0,r5			! vcy
	mov.l	@(12,r1),r0
	mov.l	@r0,r4			! vh
	mov.l	@(4,r1),r3		! ap
	mov.w	@(4,r3),r0
	mov	r0,r6			! yt_att = cur_sub_yt_attach
	mov.w	@(6,r3),r0
	mov	r0,r7			! yb_att = cur_sub_yb_attach
	mov	#32,r0
	shll8	r0			! 8192
	cmp/ge	r0,r13
	movt	r9			! pad = s_max >= 8192
	mov	#0,r10			! p_yt
	mov	r4,r11			! p_yb = vh
! ---- top ----
	mov.b	@(3,r8),r0		! is_sky_ceil
	tst	r0,r0
	bt	.Lq_tgo
	mov.b	@(12,r14),r0		! bsec is_sky
	tst	r0,r0
	bf	.Lq_tskip
	mov.l	@(32,r8),r1
	mov.l	@r14,r2
	cmp/gt	r2,r1			! f_dz_ceil > b_dz_ceil
	bf	.Lq_tskip
.Lq_tgo:
	mov.l	@(32,r8),r1		! f_dz_ceil
	mov.l	@r14,r2			! b_dz_ceil
	mov	r1,r3
	cmp/gt	r2,r1
	bf	.Lq_t0
	mov	r2,r3			! dzt = min
.Lq_t0:
	cmp/gt	r2,r1
	bt	.Lq_tproj
	cmp/pl	r3
	bf	.Lq_tskip
.Lq_tproj:
	cmp/pz	r3
	bf/s	.Lq_t1
	mov	r12,r0
	mov	r13,r0
.Lq_t1:
	dmuls.l	r3,r0
	sts	mach,r0
	mov	r5,r3
	sub	r0,r3			! yt_min
	mov.b	@(12,r14),r0
	tst	r0,r0
	bf	.Lq_t3
	mov	r3,r0
	cmp/pz	r0
	bt	.Lq_t2
	mov	#0,r0
.Lq_t2:
	mov	r0,r2			! b_yt_att
	mov.w	@(8,r14),r0
	cmp/gt	r2,r0
	bf	.Lq_t3
	mov	r2,r0
	mov.w	r0,@(8,r14)
.Lq_t3:
	mov	r3,r1
	sub	r9,r1			! yt_min - pad
	mov.w	@(12,r8),r0		! ytop_min
	cmp/gt	r0,r1
	bf	.Lq_tskip
	mov	r1,r10			! p_yt
	mov.l	@(32,r8),r1
	mov.l	@r14,r2
	cmp/gt	r2,r1
	bf	.Lq_tskip
	cmp/pz	r1
	bf/s	.Lq_t4
	mov	r12,r0
	mov	r13,r0
.Lq_t4:
	dmuls.l	r1,r0
	sts	mach,r0
	mov	r5,r1
	sub	r0,r1			! w_yt_att
	cmp/gt	r1,r6
	bf	.Lq_tskip
	mov	r1,r6
.Lq_tskip:
! ---- bottom ----
	mov.l	@(36,r8),r1		! f_dz_floor
	mov.l	@(4,r14),r2		! b_dz_floor
	mov	r1,r3
	cmp/gt	r1,r2
	bf	.Lq_b0
	mov	r2,r3			! dzb = max
.Lq_b0:
	cmp/gt	r1,r2
	bt	.Lq_bproj
	cmp/pz	r3
	bt	.Lq_bskip
.Lq_bproj:
	cmp/pl	r3
	bt/s	.Lq_b1
	mov	r12,r0
	mov	r13,r0
.Lq_b1:
	dmuls.l	r3,r0
	sts	mach,r0
	mov	r5,r3
	sub	r0,r3			! yb_max
	mov	r3,r0
	cmp/gt	r4,r0
	bf	.Lq_b2
	mov	r4,r0
.Lq_b2:
	mov	r0,r2			! b_yb_att
	mov.w	@(10,r14),r0
	cmp/gt	r0,r2
	bf	.Lq_b3
	mov	r2,r0
	mov.w	r0,@(10,r14)
.Lq_b3:
	mov	r3,r1
	add	r9,r1			! yb_max + pad
	mov.w	@(14,r8),r0		! ybot_max
	cmp/gt	r1,r0
	bf	.Lq_bskip
	mov	r1,r11			! p_yb
	mov.l	@(36,r8),r1
	mov.l	@(4,r14),r2
	cmp/gt	r1,r2
	bf	.Lq_bskip
	cmp/pl	r1
	bt/s	.Lq_b4
	mov	r12,r0
	mov	r13,r0
.Lq_b4:
	dmuls.l	r1,r0
	sts	mach,r0
	mov	r5,r1
	sub	r0,r1			! w_yb_att
	cmp/gt	r7,r1
	bf	.Lq_bskip
	mov	r1,r7
.Lq_bskip:
	cmp/pl	r10
	bt	.Lq_rec
	cmp/gt	r11,r4
	bf	.Lq_ret
! ---- HW_RecordPortalSpan(sx, ex, p_yt, p_yb, (s16)yt_att, (s16)yb_att) ----
.Lq_rec:
	exts.w	r6,r6
	exts.w	r7,r7
	cmp/pz	r10
	bt	.Lq_c0
	mov	#0,r10
.Lq_c0:
	cmp/gt	r4,r10
	bf	.Lq_c1
	mov	r4,r10
.Lq_c1:
	cmp/pz	r11
	bt	.Lq_c2
	mov	#0,r11
.Lq_c2:
	cmp/gt	r4,r11
	bf	.Lq_c3
	mov	r4,r11
.Lq_c3:
	cmp/pz	r6
	bt	.Lq_c4
	mov	#0,r6
.Lq_c4:
	cmp/gt	r4,r6
	bf	.Lq_c5
	mov	r4,r6
.Lq_c5:
	cmp/pz	r7
	bt	.Lq_c6
	mov	#0,r7
.Lq_c6:
	cmp/gt	r4,r7
	bf	.Lq_c7
	mov	r4,r7
.Lq_c7:
	mov	r10,r0
	add	#1,r0
	cmp/gt	r0,r6
	bf	.Lq_c8
	mov	#0,r10			! yt_att > ytop + 1
.Lq_c8:
	mov	r11,r0
	add	#-1,r0
	cmp/gt	r7,r0
	bf	.Lq_c9
	mov	r4,r11			! yb_att < ybot - 1
.Lq_c9:
	mov.w	@(8,r8),r0
	mov	r0,r1			! sx
	mov.w	@(10,r8),r0
	mov	r0,r2			! ex
	cmp/ge	r2,r1
	bt	.Lq_ret
	cmp/pl	r10
	bt	.Lq_c10
	cmp/ge	r4,r11
	bt	.Lq_ret
.Lq_c10:
	mov.l	.Lq_ctx,r3
	mov.l	@(4,r3),r3		! ap
	mov.b	@(3,r3),r0
	extu.b	r0,r5			! idx
	tst	r5,r5
	bt	.Lq_new
	mov	r5,r0
	shll2	r0
	add	r0,r0
	add	r3,r0
	add	#16,r0
	mov	r0,r12			! prev = &spans[idx - 1]
	mov.w	@r12,r0
	cmp/eq	r1,r0
	bf	.Lq_new
	mov.w	@(2,r12),r0
	cmp/eq	r2,r0
	bf	.Lq_new
	mov.b	@(4,r12),r0
	extu.b	r0,r0
	cmp/gt	r0,r10
	bf	.Lq_m1
	mov	r10,r0
	mov.b	r0,@(4,r12)
.Lq_m1:
	mov.b	@(5,r12),r0
	extu.b	r0,r0
	cmp/gt	r11,r0
	bf	.Lq_m2
	mov	r11,r0
	mov.b	r0,@(5,r12)
.Lq_m2:
	mov.b	@(6,r12),r0
	extu.b	r0,r0
	cmp/gt	r6,r0
	bf	.Lq_m3
	mov	r6,r0
	mov.b	r0,@(6,r12)
.Lq_m3:
	mov.b	@(7,r12),r0
	extu.b	r0,r0
	cmp/gt	r0,r7
	bf	.Lq_ret
	mov	r7,r0
	bra	.Lq_ret
	mov.b	r0,@(7,r12)
.Lq_new:
	mov	#32,r0
	cmp/ge	r0,r5
	bt	.Lq_ret
	mov	r5,r0
	shll2	r0
	add	r0,r0
	add	r3,r0
	add	#24,r0
	mov	r0,r12			! &spans[idx]
	mov.w	r1,@r12
	mov	r2,r0
	mov.w	r0,@(2,r12)
	mov	r10,r0
	mov.b	r0,@(4,r12)
	mov	r11,r0
	mov.b	r0,@(5,r12)
	mov	r6,r0
	mov.b	r0,@(6,r12)
	mov	r7,r0
	mov.b	r0,@(7,r12)
	add	#1,r5
	mov	r5,r0
	mov.b	r0,@(3,r3)
.Lq_ret:
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
.Lq_rts:
	rts
	nop

	.align	2
.Lq_ctx:	.long	_hw_asm_portal_ctx
! ===================================================================
! Stage C2: hw_asm_spanocc(x0, x1) = HW_CheckSpanOcclusion and
! hw_asm_marksub(x0, x1) = HW_MarkSubSolidSpan (saturn_hw_occl.c).
! Leaf routines, r0-r7 only. Table hw_asm_occ_ctx: [0] &hw_solid_count
! [4] hw_solid_mask [8] hw_mask_left [12] hw_mask_right [16]
! hw_span_mask_lut [20] &hw_solid_any [24] &hw_solid_summary
! [28] sub_solid_mask [32] &sub_solid_active [36] &sub_solid_min_w
! [40] &sub_solid_max_w
! ===================================================================
	.align	2
	.global	_hw_asm_spanocc
_hw_asm_spanocc:
	mov.l	.Lo_ctx,r7
	mov.l	@r7,r0
	mov.l	@r0,r0			! hw_solid_count
	tst	r0,r0
	bt	.Lo_ret0
	mov	#80,r1
	shll2	r1			! 320
	cmp/ge	r1,r0
	bt	.Lo_ret2
	cmp/pz	r4
	bt	.Lo_a
	mov	#0,r4
.Lo_a:
	cmp/gt	r1,r5
	bf	.Lo_b
	mov	r1,r5
.Lo_b:
	cmp/ge	r5,r4
	bt	.Lo_ret0
	add	#-1,r5			! x1 - 1
	mov	r4,r0
	xor	r5,r0
	mov	#32,r1
	cmp/hs	r1,r0
	bt	.Lo_multi
	mov	r4,r0
	and	#31,r0
	shll2	r0
	mov.l	@(8,r7),r1
	mov.l	@(r0,r1),r1		! ml[b0]
	mov	r5,r0
	and	#31,r0
	shll2	r0
	mov.l	@(12,r7),r2
	mov.l	@(r0,r2),r2		! mr[b1]
	and	r2,r1			! mask
	mov	r4,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0		! w0 * 4
	mov.l	@(4,r7),r2
	mov.l	@(r0,r2),r2
	and	r1,r2			! hit
	tst	r2,r2
	bt	.Lo_ret0
	cmp/eq	r1,r2
	bt	.Lo_ret2
	rts
	mov	#1,r0
.Lo_multi:
	mov	r4,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0
	mov	r0,r3			! w0 * 4
	mov	r5,r0
	shlr2	r0
	shlr	r0
	and	#0xFC,r0
	mov	r0,r6			! w1 * 4
	mov.l	@(4,r7),r2		! hw_solid_mask
	mov	r4,r0
	and	#31,r0
	shll2	r0
	mov.l	@(8,r7),r1
	mov.l	@(r0,r1),r1		! mask0
	mov	r3,r0
	mov.l	@(r0,r2),r4
	and	r1,r4			! hit0
	mov	r5,r0
	and	#31,r0
	shll2	r0
	mov.l	@(12,r7),r5
	mov.l	@(r0,r5),r5		! mask1
	mov	r6,r0
	mov.l	@(r0,r2),r2
	and	r5,r2			! hit1
	mov	r3,r0
	add	#4,r0
	cmp/eq	r0,r6
	bf	.Lo_far
	mov	r4,r0
	or	r2,r0
	tst	r0,r0
	bt	.Lo_ret0
	cmp/eq	r1,r4
	bf	.Lo_ret1
	cmp/eq	r5,r2
	bf	.Lo_ret1
	rts
	mov	#2,r0
.Lo_far:
	mov.l	@(16,r7),r0		! hw_span_mask_lut
	add	r0,r6
	mov.l	@r6,r6			! lut[w1]
	add	r0,r3
	mov.l	@(4,r3),r3		! lut[w0 + 1]
	xor	r3,r6			! req_bits
	mov	r4,r0
	or	r2,r0
	tst	r0,r0
	bf	.Lo_f2
	mov.l	@(20,r7),r0
	mov.l	@r0,r0			! hw_solid_any
	tst	r6,r0
	bt	.Lo_ret0
	rts
	mov	#1,r0
.Lo_f2:
	cmp/eq	r1,r4
	bf	.Lo_ret1
	cmp/eq	r5,r2
	bf	.Lo_ret1
	mov.l	@(24,r7),r0
	mov.l	@r0,r0			! hw_solid_summary
	and	r6,r0
	cmp/eq	r6,r0
	bt	.Lo_ret2
.Lo_ret1:
	rts
	mov	#1,r0
.Lo_ret0:
	rts
	mov	#0,r0
.Lo_ret2:
	rts
	mov	#2,r0

	.align	2
	.global	_hw_asm_marksub
_hw_asm_marksub:
	mov.l	.Lo_ctx,r7
	add	#-1,r5			! x1 - 1
	mov	r4,r1
	shlr2	r1
	shlr2	r1
	shlr	r1			! w0
	mov	r5,r2
	shlr2	r2
	shlr2	r2
	shlr	r2			! w1
	mov.l	@(32,r7),r3
	mov.l	@r3,r0
	tst	r0,r0
	bf	.Lk_act
	mov	#1,r0
	mov.l	r0,@r3			! sub_solid_active = 1
	mov.l	@(36,r7),r3
	mov.l	r1,@r3
	mov.l	@(40,r7),r3
	bra	.Lk_upd
	mov.l	r2,@r3
.Lk_act:
	mov.l	@(36,r7),r3
	mov.l	@r3,r0
	cmp/gt	r1,r0
	bf	.Lk_a1
	mov.l	r1,@r3
.Lk_a1:
	mov.l	@(40,r7),r3
	mov.l	@r3,r0
	cmp/gt	r0,r2
	bf	.Lk_upd
	mov.l	r2,@r3
.Lk_upd:
	mov	r4,r0
	and	#31,r0
	shll2	r0
	mov.l	@(8,r7),r3
	mov.l	@(r0,r3),r3		! ml[b0]
	mov	r5,r0
	and	#31,r0
	shll2	r0
	mov.l	@(12,r7),r6
	mov.l	@(r0,r6),r6		! mr[b1]
	mov.l	@(28,r7),r7		! sub_solid_mask
	shll2	r1
	shll2	r2
	cmp/eq	r1,r2
	bf	.Lk_multi
	and	r6,r3
	mov	r1,r0
	mov.l	@(r0,r7),r1
	or	r3,r1
	rts
	mov.l	r1,@(r0,r7)
.Lk_multi:
	mov	r1,r0
	mov.l	@(r0,r7),r4
	or	r3,r4
	mov.l	r4,@(r0,r7)
	mov	#-1,r3
.Lk_loop:
	add	#4,r0
	cmp/hs	r2,r0
	bt	.Lk_end
	bra	.Lk_loop
	mov.l	r3,@(r0,r7)
.Lk_end:
	mov.l	@(r0,r7),r4
	or	r6,r4
	rts
	mov.l	r4,@(r0,r7)

	.align	2
.Lo_ctx:	.long	_hw_asm_occ_ctx
! ===================================================================
! hw_asm_subsec(subnum r4, sf r5, fully_inside r6) -> flat_flags or -1:
! the whole HW_CheckSubsectorAperture (saturn_hw_walls.c, stage D1).
! Front half = hw_asm_subap (out struct at @r15), then the tail and
! HW_EvalSubsectorFlatAperture inline. Rare paths stay C. Table
! hw_asm_subsec_ctx: [0] &saturn_baked_flat_bboxes [4] &hw_aperture_state
! [8] &saturn_view_h [12] &saturn_view_cy [16] hw_asm_subap_ctx
! [20] &hw_psec_fast [24] saturn_master_oclut
! [28] HW_CheckNearSubsectorFrustum [32] HW_EvalSubsectorSpanSlow
! [36] HW_CheckPortalSubsectorStepCull
! regs: r9 sf, r10 ap, r11 s_max, r12 s_min, r13 fully_inside, then the
! packed flat flags, r14 vh. frame: 0..31 subap out (0 max_cz 4 s_max
! 8 s_min 12 ytop_min 16 ybot_max 20 sx 24 ex 28 flags), 32 subnum,
! 36 fb, 40 pad, 56 vcy
! Eval regs: r8 allow (bit0 floor, bit1 ceil), r6 yf_max, r7 yc_min,
! r5 ytop_min, r4 ybot_max, r3 pad
! ===================================================================
	.align	2
	.global	_hw_asm_subsec
_hw_asm_subsec:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	add	#-60,r15
	mov.l	r4,@(32,r15)
	mov	r5,r9
	mov	r6,r13
	mov.l	.Lv_ctx,r7
	mov.l	@(4,r7),r10		! ap
	mov.l	@(8,r7),r0
	mov.l	@r0,r14			! vh
	mov.l	@(12,r7),r0
	mov.l	@r0,r0
	mov.l	r0,@(56,r15)		! vcy
	mov.l	@r7,r0
	mov.l	@r0,r0			! saturn_baked_flat_bboxes
	tst	r0,r0
	bt	.Lv_def
	shll2	r4
	add	r4,r4
	add	r0,r4			! fb
	mov.l	r4,@(36,r15)
	mov	r13,r5
	mov	r15,r6
	mov.l	@(16,r7),r7		! hw_asm_subap_ctx
	mov.l	.Lv_subap,r0
	jsr	@r0
	nop
	cmp/pz	r0
	bf	.Lv_cull
	tst	r0,r0
	bt	.Lv_def
	mov	#2,r1
	cmp/ge	r1,r0
	bf	.Lv_tail
	cmp/eq	r1,r0
	bf	.Lv_slow
	mov.l	.Lv_ctx,r0		! 2: HW_CheckNearSubsectorFrustum(fb)
	mov.l	@(28,r0),r0
	jsr	@r0
	mov.l	@(36,r15),r4
	tst	r0,r0
	bt	.Lv_cull
	bra	.Lv_tail
	nop
.Lv_slow:
	mov.l	.Lv_ctx,r0		! 3: HW_EvalSubsectorSpanSlow(sx, ex)
	mov.l	@(32,r0),r0
	mov.l	@(20,r15),r4
	jsr	@r0
	mov.l	@(24,r15),r5
	cmp/pz	r0
	bf	.Lv_cull
	mov	r0,r1
	shlr16	r1			! ap_pack >> 16 (ap_pack >= 0)
	mov.l	r1,@(12,r15)
	exts.w	r0,r0
	mov.l	r0,@(16,r15)
	bra	.Lv_tail
	nop
.Lv_def:
	mov	#0,r0
	mov.w	r0,@(4,r10)		! cur_sub_yt_attach = 0
	mov.b	r0,@r10			! cur_sub_ytop = 0
	mov	r14,r0
	mov.w	r0,@(6,r10)		! cur_sub_yb_attach = vh
	mov.b	r0,@(1,r10)		! cur_sub_ybot = vh
	mov	r13,r0
	bra	.Lv_ret
	or	#12,r0			! fully_inside | HW_FLAT_ALLOW_BOTH
.Lv_cull:
	bra	.Lv_ret
	mov	#-1,r0
! ---- tail ----
.Lv_tail:
	mov.l	@(4,r15),r11		! s_max
	mov.l	@(8,r15),r12		! s_min
	mov	#0,r0
	mov.w	r0,@(4,r10)
	mov.b	r0,@r10
	mov	r14,r0
	mov.w	r0,@(6,r10)
	mov.b	r0,@(1,r10)
	mov.l	@(12,r15),r1		! ytop_min
	mov.l	@(16,r15),r2		! ybot_max
	mov	#20,r0
	mov.w	r1,@(r0,r10)		! cur_ytop_min
	mov	#22,r0
	mov.w	r2,@(r0,r10)		! cur_ybot_max
	mov	#32,r0
	shll8	r0			! 8192
	cmp/ge	r0,r11
	movt	r3
	add	#1,r3			! pad = s_max < 8192 ? 1 : 2
	mov.l	r3,@(40,r15)
	mov.l	@(28,r15),r0
	and	#0x13,r0		! keep = USE_LOD1 | INSIDE_MASK
	mov	r0,r13
	cmp/pl	r1
	bt	.Lv_ap
	cmp/gt	r2,r14
	bf	.Lv_noap
.Lv_ap:
	mov	#1,r0
	shll8	r0
	or	r0,r13			! HW_FLAT_CHECK_AP
	mov	r1,r0
	sub	r3,r0
	cmp/gt	r3,r1
	bt	.Lv_ap1
	mov	#0,r0
.Lv_ap1:
	mov.b	r0,@r10			! cur_sub_ytop
	mov	r2,r0
	add	r3,r0
	cmp/gt	r0,r14
	bt	.Lv_ap2
	mov	r14,r0
.Lv_ap2:
	mov.b	r0,@(1,r10)		! cur_sub_ybot
.Lv_noap:
	mov.l	@(28,r15),r0
	shlr8	r0
	tst	#1,r0			! any_behind
	bf	.Lv_nop
	mov.l	.Lv_ctx,r0
	mov.l	@(20,r0),r0
	mov.l	@r0,r0			! hw_psec_fast
	cmp/eq	r9,r0
	bf	.Lv_nop0
.Lv_nop:
	mov	#2,r0
	shll8	r0
	or	r0,r13			! HW_FLAT_NEAR_OR_PSEC
.Lv_nop0:
	tst	r12,r12
	bf	.Lv_ev
	mov	r13,r0
	shlr8	r0
	tst	#3,r0
	bt	.Lv_ev
	bsr	.Lv_div
	mov.l	@r15,r0			! s_min = HW_FastDiv(max_cz)
	mov	r0,r12
! ---- HW_EvalSubsectorFlatAperture(sf, r13, s_max, s_min) ----
.Lv_ev:
	mov	r13,r0
	shlr8	r0
	tst	#3,r0
	bf	.Lv_full
	mov.l	@(4,r9),r1		! dz_floor
	cmp/pz	r1
	bt	.Lv_f1
	bsr	.Lv_py
	mov	r11,r2			! yf_max
	cmp/gt	r14,r0
	bf	.Lv_f0
	mov	r14,r0
.Lv_f0:
	mov	r0,r1
	mov.w	@(10,r9),r0
	cmp/gt	r0,r1
	bf	.Lv_f0b
	mov	r1,r0
	mov.w	r0,@(10,r9)
.Lv_f0b:
	bra	.Lv_f2
	mov.w	r0,@(6,r10)
.Lv_f1:
	mov.w	@(10,r9),r0
	mov.w	r0,@(6,r10)
.Lv_f2:
	mov.b	@(12,r9),r0
	tst	r0,r0
	bf	.Lv_fsky
	mov.l	@r9,r1			! dz_ceil
	cmp/pl	r1
	bf	.Lv_fnc
	bsr	.Lv_py
	mov	r11,r2			! yc_min
	cmp/pz	r0
	bt	.Lv_f3
	mov	#0,r0
.Lv_f3:
	mov	r0,r1
	mov.w	@(8,r9),r0
	cmp/gt	r1,r0
	bf	.Lv_f3b
	mov	r1,r0
	mov.w	r0,@(8,r9)
.Lv_f3b:
	bra	.Lv_fret
	mov.w	r0,@(4,r10)
.Lv_fnc:
	mov.w	@(8,r9),r0
	bra	.Lv_fret
	mov.w	r0,@(4,r10)
.Lv_fsky:
	mov	#0,r0
	mov.w	r0,@(4,r10)
.Lv_fret:
	mov	r13,r0
	and	#0x13,r0
	bra	.Lv_evd
	or	#12,r0
.Lv_full:
	mov	#3,r8
	mov	r14,r6			! yf_max = vh
	mov	#0,r7			! yc_min = 0
	mov	#20,r0
	mov.w	@(r0,r10),r5		! ytop_min
	mov	#22,r0
	mov.w	@(r0,r10),r4		! ybot_max
	mov.l	@(40,r15),r3		! pad
	mov.l	@(4,r9),r1		! dz_floor
	cmp/pz	r1
	bt	.Lv_flnn
	mov	r13,r0
	shlr8	r0
	tst	#2,r0
	bf	.Lv_flnop
	bsr	.Lv_py
	mov	r11,r2
	mov	r0,r6			! yf_max
	cmp/gt	r14,r0
	bf	.Lv_fa1
	mov	r14,r0
.Lv_fa1:
	mov	r0,r2
	mov.w	@(10,r9),r0
	cmp/gt	r0,r2
	bf	.Lv_fa2
	mov	r2,r0
	mov.w	r0,@(10,r9)
.Lv_fa2:
	mov.w	r0,@(6,r10)
	mov	r13,r0
	shlr8	r0
	tst	#1,r0
	bt	.Lv_fldone
	mov.l	@(4,r9),r1
	bsr	.Lv_py
	mov	r12,r2			! yf_min
	cmp/ge	r4,r0
	bt	.Lv_flkill
	mov	r6,r0
	add	r3,r0
	cmp/gt	r5,r0
	bt	.Lv_fldone
	bra	.Lv_flkill
	nop
.Lv_flnop:
	mov	r14,r0
	mov.w	r0,@(10,r9)		! sf->yb_attach = vh
	bsr	.Lv_py
	mov	r12,r2			! yf_min
	cmp/ge	r4,r0
	bf	.Lv_fb1
	mov	#-2,r0
	and	r0,r8
.Lv_fb1:
	mov	r13,r0
	shlr8	r0
	tst	#1,r0
	bt	.Lv_fldone
	mov.l	@(4,r9),r1
	bsr	.Lv_py
	mov	r11,r2
	mov	r0,r6			! yf_max
	add	r3,r0
	cmp/gt	r5,r0
	bt	.Lv_fldone
.Lv_flkill:
	mov	#-2,r0
	bra	.Lv_fldone
	and	r0,r8
.Lv_flnn:
	mov	r13,r0
	shlr8	r0
	tst	#2,r0
	bf	.Lv_fldone
	mov.w	@(10,r9),r0
	mov.w	r0,@(6,r10)
.Lv_fldone:
	mov.b	@(12,r9),r0
	tst	r0,r0
	bf	.Lv_csky
	mov.l	@r9,r1			! dz_ceil
	cmp/pl	r1
	bf	.Lv_cnp
	mov	r13,r0
	shlr8	r0
	tst	#2,r0
	bf	.Lv_cnop
	bsr	.Lv_py
	mov	r11,r2
	mov	r0,r7			! yc_min
	cmp/pz	r0
	bt	.Lv_ca1
	mov	#0,r0
.Lv_ca1:
	mov	r0,r2
	mov.w	@(8,r9),r0
	cmp/gt	r2,r0
	bf	.Lv_ca2
	mov	r2,r0
	mov.w	r0,@(8,r9)
.Lv_ca2:
	mov.w	r0,@(4,r10)
	mov	r13,r0
	shlr8	r0
	tst	#1,r0
	bt	.Lv_cdone
	mov.l	@r9,r1
	bsr	.Lv_py
	mov	r12,r2			! yc_max
	cmp/gt	r5,r0
	bf	.Lv_ckill
	mov	r7,r0
	sub	r3,r0
	cmp/ge	r4,r0
	bt	.Lv_ckill
	bra	.Lv_cdone
	nop
.Lv_cnop:
	mov	#0,r0
	mov.w	r0,@(8,r9)		! sf->yt_attach = 0
	bsr	.Lv_py
	mov	r12,r2			! yc_max
	cmp/gt	r5,r0
	bt	.Lv_cb1
	mov	#-3,r0
	and	r0,r8
.Lv_cb1:
	mov	r13,r0
	shlr8	r0
	tst	#1,r0
	bt	.Lv_cdone
	mov.l	@r9,r1
	bsr	.Lv_py
	mov	r11,r2
	mov	r0,r7			! yc_min
	sub	r3,r0
	cmp/ge	r4,r0
	bf	.Lv_cdone
.Lv_ckill:
	mov	#-3,r0
	bra	.Lv_cdone
	and	r0,r8
.Lv_cnp:
	mov	r13,r0
	shlr8	r0
	tst	#2,r0
	bf	.Lv_cdone
	mov.w	@(8,r9),r0
	bra	.Lv_cdone
	mov.w	r0,@(4,r10)
.Lv_csky:
	mov	r13,r0
	shlr8	r0
	tst	#2,r0
	bf	.Lv_cdone
	mov	#0,r0
	mov.w	r0,@(4,r10)
.Lv_cdone:
	mov	r13,r0
	shlr8	r0
	tst	#1,r0
	bt	.Lv_evret
	mov.l	@(4,r9),r1		! dz_floor
	cmp/pz	r1
	bt	.Lv_s1
	bra	.Lv_s2
	mov	r6,r0			! sub_y_max = yf_max
.Lv_s1:
	cmp/pl	r1
	bt/s	.Lv_s1b
	mov	r12,r2
	mov	r11,r2
.Lv_s1b:
	bsr	.Lv_py
	nop
.Lv_s2:
	add	r3,r0
	cmp/gt	r5,r0
	bf	.Lv_evcull
	mov.b	@(12,r9),r0
	tst	r0,r0
	bf	.Lv_evret
	mov.l	@r9,r1			! dz_ceil
	cmp/pl	r1
	bf	.Lv_s3
	bra	.Lv_s4
	mov	r7,r0			! sub_y_min = yc_min
.Lv_s3:
	cmp/pz	r1
	bf/s	.Lv_s3b
	mov	r12,r2
	mov	r11,r2
.Lv_s3b:
	bsr	.Lv_py
	nop
.Lv_s4:
	sub	r3,r0
	cmp/ge	r4,r0
	bt	.Lv_evcull
.Lv_evret:
	mov	r13,r0
	and	#0x13,r0
	shll2	r8
	bra	.Lv_evd
	or	r8,r0
.Lv_evcull:
	mov	#-1,r0
! ---- step cull ----
.Lv_evd:
	cmp/pz	r0
	bf	.Lv_ret
	mov	r0,r8			! flat_flags
	mov	r13,r0
	shlr8	r0
	and	#3,r0
	cmp/eq	#1,r0			! check_ap && !near_or_psec
	bf	.Lv_ret8
	mov	r8,r0
	tst	#4,r0			! ALLOW_FLOOR
	bf	.Lv_ret8
	mov.b	@(12,r9),r0
	tst	r0,r0
	bf	.Lv_sc
	mov	r8,r0
	tst	#8,r0			! ALLOW_CEIL
	bf	.Lv_ret8
.Lv_sc:
	mov.l	.Lv_ctx,r0
	mov.l	@(36,r0),r0
	mov.l	@(32,r15),r4
	mov	r9,r5
	jsr	@r0
	mov	r12,r6
	tst	r0,r0
	bf	.Lv_cull
.Lv_ret8:
	mov	r8,r0
.Lv_ret:
	add	#60,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! r1 dz, r2 s -> r0 = vcy - hi32(dz * s); clobbers r2
.Lv_py:
	dmuls.l	r1,r2
	mov.l	@(56,r15),r0
	sts	mach,r2
	rts
	sub	r2,r0

! HW_FastDiv: r0 = cz -> saturn_master_oclut[dist]; clobbers r1
.Lv_div:
	shlr16	r0
	mov	#8,r1
	shll8	r1			! 2048
	cmp/hs	r1,r0
	bt	.Lv_dfar
.Lv_dlk:
	shll2	r0
	mov.l	.Lv_ctx,r1
	mov.l	@(24,r1),r1
	rts
	mov.l	@(r0,r1),r0
.Lv_dfar:
	shlr2	r0
	shlr	r0
	mov	#1,r1
	shll8	r1
	shll2	r1
	add	#-1,r1			! 1023
	and	r1,r0
	mov	#8,r1
	shll8	r1
	bra	.Lv_dlk
	add	r1,r0

	.align	2
.Lv_ctx:	.long	_hw_asm_subsec_ctx
.Lv_subap:	.long	_hw_asm_subap