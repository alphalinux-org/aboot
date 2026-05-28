/*
 * Copyright (c) 1995	David Mosberger (davidm@cs.arizona.edu)
 */
#include <setjmp.h>

	.globl	_setjmp
	.ent	_setjmp

_setjmp:
	stq	$29, JB_GP($16)
	stq	$30, JB_SP($16)
	stq	 $9, JB_S0($16)
	stq	$10, JB_S1($16)
	stq	$11, JB_S2($16)
	stq	$12, JB_S3($16)
	stq	$13, JB_S4($16)
	stq	$14, JB_S5($16)
	stq	$15, JB_S6($16)
	stq	$26, JB_RA($16)
#ifdef FPU
	stt	$f2, JB_F2($16)
	stt	$f3, JB_F3($16)
	stt	$f4, JB_F4($16)
	stt	$f5, JB_F5($16)
	stt	$f6, JB_F6($16)
	stt	$f7, JB_F7($16)
	stt	$f8, JB_F8($16)
	stt	$f9, JB_F9($16)
#endif
	lda	$1, JBMAGIC & 0xffff
	ldah	$1, ((JBMAGIC >> 16) & 0xffff)($1)
	stq	$1, JB_MAGIC($16)
	clr	$0
	ret	($26)

	.end _setjmp

	.section .note.GNU-stack,"",@progbits
