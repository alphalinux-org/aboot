/*
 * Copyright (c) 1995	David Mosberger (davidm@cs.arizona.edu)
 */
#include <setjmp.h>
#include "system.h"

	.extern	printf

	.globl	_longjmp
	.ent	_longjmp

_longjmp:
	ldgp	$29, 0($27)

	mov	$17, $0
	lda	$1, JBMAGIC & 0xffff
	ldah	$1, ((JBMAGIC >> 16) & 0xffff)($1)
	cmoveq	$0, 1, $0		# ensure $0 != 0

	ldq	$2, JB_MAGIC($16)
	subq	$1, $2, $1
	ldq	$30, JB_SP($16)
	bne	$1, bad_magic

	ldq	 $9, JB_S0($16)
	ldq	$10, JB_S1($16)
	ldq	$11, JB_S2($16)
	ldq	$12, JB_S3($16)
	ldq	$13, JB_S4($16)
	ldq	$14, JB_S5($16)
	ldq	$15, JB_S6($16)
	ldq	$29, JB_GP($16)
	ldq	$26, JB_RA($16)
#ifdef FPU
	ldt	$f2, JB_F2($16)
	ldt	$f3, JB_F3($16)
	ldt	$f4, JB_F4($16)
	ldt	$f5, JB_F5($16)
	ldt	$f6, JB_F6($16)
	ldt	$f7, JB_F7($16)
	ldt	$f8, JB_F8($16)
	ldt	$f9, JB_F9($16)
#endif
	ret	($26)

	.data
error_msg:
#ifdef __osf__
	.asciiz	"_longjmp: bad magic number"
#else
	.asciz	"_longjmp: bad magic number"
#endif

	.text
bad_magic:
	lda	$16, error_msg
	lda	$27, printf
	jsr	$27, printf
	call_pal PAL_halt

	.end _longjmp

	.section .note.GNU-stack,"",@progbits
