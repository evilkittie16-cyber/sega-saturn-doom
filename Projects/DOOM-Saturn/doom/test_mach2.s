	.file	"test_mach2.c"
	.text
	.text
	.align 1
	.align 2
	.global	_test
	.type	_test, @function
_test:
	dmuls.l	r5,r4
	rts	
	sts	mach,r0
	.size	_test, .-_test
	.ident	"GCC: (GNU) 9.3.0"
