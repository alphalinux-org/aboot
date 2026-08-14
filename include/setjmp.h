/*
 * @COPYRIGHT@
 *
 * x-kernel v3.2
 *
 * Copyright (c) 1993,1991,1990  Arizona Board of Regents
 *
 * @COPYRIGHT@
 *
 * $RCSfile: setjmp.h,v $
 *
 * HISTORY
 * $Log: setjmp.h,v $
 * Revision 1.1  2001/10/08 23:03:52  wgwoods
 * Initial revision
 *
 * Revision 1.1.1.1  2000/05/03 03:58:22  dhd
 * Initial import (from 0.7 release)
 *
 * Revision 1.1  1995/03/06  16:41:07  davidm
 * Initial revision
 *
 * Revision 1.1  1994/10/07  00:47:11  davidm
 * Initial revision
 *
 */
#ifndef _setjmp_h
#define _setjmp_h

#define		JB_GP		0x00
#define		JB_SP		0x08
#define		JB_RA		0x10
#define		JB_S0		0x18
#define		JB_S1		0x20
#define		JB_S2		0x28
#define		JB_S3		0x30
#define		JB_S4		0x38
#define		JB_S5		0x40
#define		JB_S6		0x48
#define		JB_MAGIC	0x50
# ifndef SCOUT_FPU_SUPPORT
#  define JBLEN			(0x58 / 8)
# else
#  define	JB_F2		0x58
#  define	JB_F3		0x60
#  define	JB_F4		0x68
#  define	JB_F5		0x70
#  define	JB_F6		0x78
#  define	JB_F7		0x80
#  define	JB_F8		0x88
#  define	JB_F9		0x90
#  define 	JBLEN		(0x98 / 8)
# endif /* SCOUT_FPU_SUPPORT */

#define	JBMAGIC	0x2ceb1ade

/*
 * GCC predefines LANGUAGE_ASSEMBLY on Alpha when preprocessing assembly,
 * but that is a GCC extension that other compilers do not provide.
 * __ASSEMBLER__ is defined by any compiler preprocessing a .S file.
 */
#if !defined(LANGUAGE_ASSEMBLY) && !defined(__ASSEMBLER__)

typedef long	jmp_buf[JBLEN];

void	_longjmp(jmp_buf, int);
int	_setjmp(jmp_buf);

#endif /* !LANGUAGE_ASSEMBLY && !__ASSEMBLER__ */
#endif /* _setjmp_h */
