/*
 * aboot.c
 *
 * This file is part of aboot, the SRM bootloader for Linux/Alpha
 * Copyright (C) 1996 Linus Torvalds, David Mosberger, and Michael Schwingen.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <asm/console.h>
#include "hwrpb.h"
#include "system.h"

#include <elf.h>
#include <errno.h>

#include "aboot.h"
#include "config.h"
#include "cons.h"
#include "setjmp.h"
#include "utils.h"
#include <string.h>

const struct bootfs *	bfs = 0;		/* filesystem to boot from */
char *		dest_addr = 0;
jmp_buf		jump_buffer;

unsigned long	start_addr = START_ADDR; /* default only */
struct segment* chunks;
int             nchunks;

char		boot_file[256] = "";
char		initrd_file[256] = "";
char		kernel_args[256] = "";
char *		bss_start = 0;
long		bss_size = 0;		 /* size of bss */
unsigned long   initrd_start = 0;
unsigned long   initrd_size = 0;

/* Offsets from start_addr (yes, they are negative, because start_addr
   is actually the load address plus the text offset) */
#define PARAM_OFFSET	-0x6000 /* load address + 0xa000 */
#define STACK_OFFSET	-0xe000 /* load address + 0x2000 */

/* eventually we may have to deal with 44-bit physical addressing and
   also possibly larger pages */
unsigned long	page_offset = 0xfffffc0000000000;
unsigned long   page_shift  = 13;

static unsigned long entry_addr = START_ADDR;

/*
 * Checks if content of buf contain a valid ELF header.
 * Each ELF section is checked whether it can be loaded in
 * memory.
 */
bool
is_loadable_elf(const unsigned char *buf, long blocksize)
{
	Elf64_Ehdr *elf;
	Elf64_Phdr *phdrs;
	int i, j;

	elf  = (Elf64_Ehdr *) buf;

	if (elf->e_ident[0] != 0x7f
	    || elf->e_ident[1] != 'E'
	    || elf->e_ident[2] != 'L'
	    || elf->e_ident[3] != 'F')
	{
		/* Fail silently, it might be a compressed file */
		return false;
	}
	if (elf->e_ident[EI_CLASS] != ELFCLASS64
	    || elf->e_ident[EI_DATA] != ELFDATA2LSB
	    || elf->e_machine != EM_ALPHA)
	{
		printf("aboot: ELF executable not for this machine\n");
		return false;
	}

	/* Looks like an ELF binary. */
	if (elf->e_type != ET_EXEC) {
		printf("aboot: not an executable ELF file\n");
		return false;
	}

	if (elf->e_phoff + elf->e_phnum * sizeof(*phdrs) > (unsigned) blocksize)
	{
		printf("aboot: "
		       "ELF program headers not in first block (%ld)\n",
		       (long) elf->e_phoff);
		return false;
	}

	phdrs = (Elf64_Phdr *) (buf + elf->e_phoff);
	chunks = malloc(sizeof(struct segment) * elf->e_phnum);
	start_addr = phdrs[0].p_vaddr; /* assume they are sorted */
	entry_addr = elf->e_entry;

#ifdef DEBUG
	printf("aboot: %d program headers, start address %#lx, entry %#lx\n",
	       elf->e_phnum, start_addr, entry_addr);
#endif

	for (i = j = 0; i < elf->e_phnum; ++i) {
		int status;

		if (phdrs[i].p_type != PT_LOAD)
			continue;

		chunks[j].addr   = phdrs[i].p_vaddr;
		chunks[j].offset = phdrs[i].p_offset;
		chunks[j].size   = phdrs[i].p_filesz;

#ifdef DEBUG
		printf("aboot: PHDR %d vaddr %#lx offset %#lx size %#lx\n",
		       i, chunks[j].addr, chunks[j].offset, chunks[j].size);
#endif

#ifndef TESTING
		status = check_memory(chunks[j].addr, chunks[j].size);
		if (status) {
			printf("aboot: Can't load kernel.\n"
			       "  Memory at %lx - %lx (PHDR %i) "
			         "is %s\n",
			       chunks[j].addr,
			       chunks[j].addr + chunks[j].size - 1,
			       i,
			       (status == -ENOMEM) ?
			          "Not Found" :
				  "Busy (Reserved)");
			return false;
		}
#endif

		if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
			if (bss_size > 0) {
				printf("aboot: Can't load kernel.\n"
				       "  Multiple BSS segments"
				       " (PHDR %d)\n", i);
				return false;
			}
			bss_start = (char *) (phdrs[i].p_vaddr +
				              phdrs[i].p_filesz);
			bss_size = (phdrs[i].p_memsz - phdrs[i].p_filesz);
		}

		j++;
	}
	nchunks = j;
#ifdef DEBUG
	printf("aboot: bss at 0x%p, size %#lx\n", bss_start, bss_size);
#endif

	return true;
}

static void
get_boot_args(void)
{
	/* get boot command line: */
#ifdef TESTING
	const char *e;
	if ((e = getenv("BOOTED_FILE"))) {
		strncpy(boot_file, e, sizeof(boot_file)-1);
		boot_file[sizeof(boot_file)-1] = 0;
	} else {
		strcpy(boot_file, "vmlinux.gz");
	}
	if ((e = getenv("BOOTED_OSFLAGS"))) {
		strncpy(kernel_args, e, sizeof(kernel_args)-1);
		kernel_args[sizeof(kernel_args)-1] = 0;
	} else {
		strcpy(kernel_args, "i");
	}
#else
	long result;

	result = cons_getenv(ENV_BOOTED_FILE, boot_file, sizeof(boot_file));
	if (result < 0) {
		printf("aboot: warning: can't get ENV_BOOTED_FILE "
		       "(result=%lx)!\n", result);
		strcpy(boot_file, "vmlinux.gz");
	}
	result = cons_getenv(ENV_BOOTED_OSFLAGS,
			     kernel_args, sizeof(kernel_args));
	if  (result < 0) {
		printf("aboot: warning: can't get ENV_BOOTED_OSFLAGS "
		       "(result=%lx)!\n", result);
		strcpy(kernel_args, "i");
	}
#endif /* TESTING */
}

#ifdef TESTING
long config_file_partition = 1;
void halt()
{
	exit(0);
}

void unzip_error(char *x)
{
	printf("unzip: %s\n", x);
}


int main()
{
	long result;

	get_boot_args();
	result = load_kernel();
	if (result < 0) {
		printf("aboot: kernel load failed (%ld)\n", result);
		return 0;
	}
	printf("aboot: starting kernel %s with arguments %s\n",
	       boot_file, kernel_args);
	return 0;
}
#else /* not TESTING */
/*
 * Head transfers control to this function.  Don't call it main() to avoid
 * gcc doing magic initialization things that we don't want.
 */
void
main_ (void)
{
	long i, result;

	cons_init();

	printf("aboot: Linux/Alpha SRM bootloader version "ABOOT_VERSION"\n");

	/* don't know how to deal with this yet */
	if (INIT_HWRPB->pagesize != 8192) {
		printf("aboot: expected 8kB pages, got %ldkB\n",
		       INIT_HWRPB->pagesize >> 10);

		cons_close_console();
		return;
	}

	pal_init();
	print_memory_map();
	get_boot_args();
	result = load_kernel();
	if (result < 0) {
		printf("aboot: kernel load failed (%ld)\n", result);
		cons_close_console();
		return;
	}
	printf("aboot: starting kernel %s with arguments %s\n",
	       boot_file, kernel_args);
	strcpy((char*)start_addr + PARAM_OFFSET, kernel_args);
	*(unsigned long *)(start_addr + PARAM_OFFSET + 0x100)
		= initrd_start;
	*(unsigned long *)(start_addr + PARAM_OFFSET + 0x108)
		= initrd_size;

	cons_close_console();
	run_kernel(entry_addr, start_addr + STACK_OFFSET);

	cons_open_console();
	printf("aboot: kernel returned unexpectedly.  Halting slowly...\n");
	for (i = 0 ; i < 0x100000000 ; i++)
		/* nothing */;
	cons_close_console();
	halt();
}
#endif /* TESTING */
