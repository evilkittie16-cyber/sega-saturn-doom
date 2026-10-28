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
!
! SH-2 ISA limits learned the hard way (Kronos R1 freeze, 2026-09-01):
! SHAD/SHLD DO NOT EXIST on SH-2 (SH-3/SH-2A only). gas with the
! Makefile's no-arch-flag default SILENTLY ASSEMBLES them into SH-3
! encodings; the strict check is `sh-elf-gcc -m2 -c saturn_draw.s`
! which rejects them with "opcode not valid for this cpu variant".
! Dynamic shifts are therefore done as:
!   left by wshift  ->  dmuls.l (pass the width, not the shift count)
!   arithmetic >>16 ->  swap.w rN,rN + exts.w rN,rN  (gcc's own idiom)
!
! Toolchain notes: C symbols carry a LEADING UNDERSCORE; r8-r14 are
! callee-saved and pushed here; args r4-r7 + stack. ABI (verified by
! compiling a 9-arg reference with sh-elf-gcc 9.3 -m2 and reading its
! prologue CAREFULLY — gcc pushes r8 AND r9 before its first stack-arg
! load): stack args start at r15+0, arg9 at +16. After our 7-register
! push (28 bytes) that is @(28,r15)..@(44,r15). A "reserved 4-byte
! slot" myth (from miscounting the prologue's pushes) briefly shipped
! here and turned the flat kernel's flat/cmaprow POINTERS into garbage
! -> wild reads -> machine freeze. Offsets below are +0-based.

	.section .text
	.align	4

! --------------------------------------------------------------------------
! void saturn_wall_col_asm(byte *fb, const byte *cmaprow, const byte *tex,
!                          int count, int v0, int vstep, int vh,
!                          int w, int utex)
!
! fb points at the first pixel (x + y0*SCREEN_W); the loop walks DOWN the
! column (fb += SCREEN_W). Caller guarantees: 0 <= v0 < vh, 0 <= vstep < vh,
! count >= 1, cmaprow = COLORMAP + (sh << 8).
! Full-width render (2026-09-05): SCREEN_W is 320 (vanilla width,
! displayed 1:1), so the column stride is 320.
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
	! stack args (after 7 pushes, 28 bytes; ABI: arg5 at orig+0):
	! v0 vstep vh wshift utex at byte offsets 28/32/36/40/44
	mov.l	@(28,r15),r3	! v0
	mov.l	@(32,r15),r8	! vstep
	mov.l	@(36,r15),r9	! vh
	mov.l	@(40,r15),r13	! w, the texture width (pow2). NOT a shift
				! count: SH-2 has no shad/shld (SH-3+ only),
				! so row<<log2w is a dmuls.l by w instead.
				! (mov.w @(disp>30,r15) would also be silently
				! mis-encoded by the no-arch-flag gas.)
	mov.l	@(44,r15),r10	! utex
	mov	#80,r11
	shll	r11
	shll	r11		! r11 = 320 = SCREEN_W (full-width render:
				! the fb is 320 columns; the C NPO2 loop in
				! saturn_render.c strides by the same 320.
				! HISTORY: the half-width era shifted once
				! (=160); before that this was 64<<2 + 64 =
				! 320, and a buggy "copy after the shift"
				! order once made it 512 — a 1.6-row write
				! stride that blasted ~38 KB past the
				! framebuffer and froze the machine.)
	mov	r9,r12
	shlr16	r12		! h  (vh>>16; pow2: h-1 is the row mask)
	add	#-1,r12		! h-1

! NOTE: SH-2 indexed mov.b is @(r0,rN) ONLY — the index register must be
! r0 — and and #imm / or #imm accept r0 as their ONLY destination. Both
! kernels therefore funnel every texel/cmap lookup through r0.
.Lwall_loop:
	swap.w	r3,r0		! r0 = v0>>16 (low word) | v0<<16 (HIGH BITS!)
	extu.w	r0,r0		! clear the swapped-up high half (gcc emits
				! exts.w here — the and below only masks the
				! LOW bits, the v0<<16 garbage survives it)
	and	r12,r0		! row & (h-1)
	dmuls.l	r13,r0		! row * w  (SH-2 has no dynamic shift:
	sts	macl,r0		!  shad/shld are SH-3+; low word == row*w)
	add	r10,r0		! + utex
	mov.b	@(r0,r6),r0	! texel = tex[row*w + utex]
	extu.b	r0,r0		! (mov.b to a register SIGN-extends — C is unsigned)
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
!
! One horizontal run: fb[0..count) = cmaprow[flat[spot]] with
! spot from the per-column perspective tables, exactly as the C loop:
!   wx = viewx_u + ((wxs[i] * d) >> 16);  wy = viewy_u + ((wys[i] * d) >> 16)
!   spot = ((wy & 63) << 6) | (wx & 63)
!
! A 2-px interleaved variant (mul chains overlapped to hide load stalls)
! was sim-verified bit-identical but measured WORSE in Kronos (J 16->17:
! +1.1 instr/px, and Kronos's SH-2 core doesn't reward hiding pipeline
! stalls). Kept out; the sim tooling (sim/sim_flat.py, generic opcode
! handlers) can re-validate it if a real-hardware port ever wants it.
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
	! stack args (after 7 pushes, 28 bytes; ABI: arg5 at orig+0):
	! viewx viewy d flat cmaprow at byte offsets 28/32/36/40/44
	mov.l	@(28,r15),r8	! viewx_u
	mov.l	@(32,r15),r9	! viewy_u
	mov.l	@(36,r15),r10	! d
	mov.l	@(40,r15),r11	! flat
	mov.l	@(44,r15),r12	! cmaprow

.Lflat_loop:
	mov.l	@r6+,r0		! wxs[i]   (post-increment)
	dmuls.l	r10,r0		! 48-bit wxs*d
	sts	macl,r0		! low 32 (== the C 32-bit product)
	swap.w	r0,r0		! >>16 (arithmetic) is swap.w + exts.w —
	exts.w	r0,r0		! SH-2 has NO shad (SH-3+ only); this is
				! exactly gcc's idiom for a variable-value
				! constant-16 arithmetic shift
	add	r8,r0		! + viewx_u
	and	#63,r0		! wx & 63
	mov	r0,r2		! save wx&63 (and #imm is r0-only)
	mov.l	@r7+,r0		! wys[i]
	dmuls.l	r10,r0		! 48-bit wys*d
	sts	macl,r0
	swap.w	r0,r0		! >>16 arithmetic (see above: no shad on SH-2)
	exts.w	r0,r0
	add	r9,r0
	and	#63,r0		! wy & 63
	shll8	r0		! (wy&63)<<8
	shlr2	r0		! <<6
	or	r2,r0		! spot
	mov.b	@(r0,r11),r0	! texel = flat[spot]
	extu.b	r0,r0		! (mov.b to a register SIGN-extends — C is unsigned)
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
!
! Stepping flat kernel (2026-09-02): DOOM has no camera roll, so a flat
! row is a straight line in world space ? u/v are AFFINE in screen x.
! Instead of two per-pixel dmuls + two table loads (the 26-instruction
! saturn_flat_row_asm), the caller seeds u/v in 16.16 fixed point and
! this loop only ADDS the step. Texel index = bits [21:16] of the .16
! accumulator, so 64-texel wrap and int32 overflow both fall out for
! free (2^22 | 2^32; no masking of the accumulators is needed).
!
!   for (i = 0; i < count; i++) {
!       fb[i] = cmaprow[flat[(((v_fx >> 16) & 63) << 6) | ((u_fx >> 16) & 63)]];
!       u_fx += ustep;  v_fx += vstep;
!   }
!
! Sample-then-add: pixel 0 is EXACTLY the C loop's wx/wy; later pixels
! drift <=1 texel from the old kernel (the old loop truncated each
! product separately, the step carries the exact fractional remainder).
!
! ABI: r4 fb, r5 count, r6 u_fx, r7 v_fx; stack args after the 7-push
! prologue (28 bytes): ustep @28, vstep @32, flat @36, cmaprow @40.
! The 63 constant lives in r12 (register AND, since and #imm is
! r0-destination only). The sampled spot must sit in r0: SH-2 indexed
! mov.b is @(r0,Rm) only.
! --------------------------------------------------------------------------
! 2x-unrolled + v-pre-shifted kernel (2026-09-02, was 18 cycles/px):
!   - v runs as v6 = v_fx << 6 (vstep6 = vstep << 6, both in the
!     prologue). After swap.w, masking with 0x0FC0 (r3) yields
!     (v6 >> 16) & 0x0FC0 = (v_fx >> 10) & 0x0FC0 = (v & 63) << 6 —
!     the shll8/shlr2 pair is gone. Addition commutes with <<6 mod
!     2^32, so the wrap trick carries over bit-exactly.
!   - dt/bf/s and the fb advance amortize over TWO pixels (12 executed
!     instructions per pixel, was 18).
!   - odd counts take the one-pixel preamble; the pair loop counts
!     PAIRS (r5 halved at .Lflatstep_even); count=0 never reaches the
!     asm (the caller guards spanw > 0 -- the dt-loop would run away).
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
	mov	#63,r12		! (and #imm is r0-only, so the 63 mask lives
					! in a register)
	mov	#63,r3
	shll2	r3
	shll2	r3
	shll2	r3		! r3 = 63<<6 = 0x0FC0: v mask AFTER
					! swap.w (the (v&63)<<6 field,
					! pre-shifted). HISTORY: this was
					! mov #15/shll8/shll2 = 15<<10 =
					! 0x3C00 — the sim's preseeds folded
					! the prologue with the INTENDED
					! 0x0FC0, so 13/13 vectors passed
					! while the real kernel sampled the
					! v texel from v_fx bits 23:20
					! (floor flats banded/garbled).
	mov	r6,r13		! u_fx (arg3 arrives in r6)
	mov	r7,r14		! v_fx (arg4 arrives in r7)
	shll2	r14
	shll2	r14
	shll2	r14		! v6 = v_fx << 6

	mov	r5,r0
	tst	#1,r0		! T=1 when count is even
	bt	.Lflatstep_even	! even: skip the single pixel
	! odd preamble: one pixel (r5 still holds the pixel count)
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
