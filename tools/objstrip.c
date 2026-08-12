/*
 * arch/alpha/boot/tools/objstrip.c
 *
 * Strip the object file headers/trailers from an executable (ELF or ECOFF).
 *
 * Copyright (C) 1996 David Mosberger-Tang.
 */
/*
 * Converts an ECOFF or ELF object file into a bootable file.  The
 * object file must be a OMAGIC file (i.e., data and bss follow immediately
 * behind the text).  See DEC "Assembly Language Programmer's Guide"
 * documentation for details.  The SRM boot process is documented in
 * the Alpha AXP Architecture Reference Manual, Second Edition by
 * Richard L. Sites and Richard T. Witek.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/a.out.h>
#include <linux/coff.h>
#include <linux/param.h>
#ifdef __ELF__
# include <linux/elf.h>
# define elfhdr elf64_hdr
# define elf_phdr elf64_phdr
# define elf_check_arch(x) ((x)->e_machine == EM_ALPHA)
#endif

/* bootfile size must be multiple of BLOCK_SIZE: */
#define BLOCK_SIZE	512

const char * prog_name;


static void
usage (void)
{
    fprintf(stderr,
	    "usage: %s [-v] -p file primary\n"
	    "       %s [-vb] file [secondary]\n", prog_name, prog_name);
    exit(1);
}


int
main (int argc, char *argv[])
{
    size_t nwritten, tocopy, n, mem_size, fil_size, pad = 0;
    int fd, ofd, i, j, verbose = 0, primary = 0;
    char buf[8192], *inname;
    long offset;
#ifdef __ELF__
    struct elfhdr *elf;
    struct elf_phdr *phdrs;	/* all program headers */
    unsigned long long e_entry;
#endif

    prog_name = argv[0];

    for (i = 1; i < argc && argv[i][0] == '-'; ++i) {
	for (j = 1; argv[i][j]; ++j) {
	    switch (argv[i][j]) {
	      case 'v':
		  verbose = ~verbose;
		  break;

	      case 'b':
		  pad = BLOCK_SIZE;
		  break;

	      case 'p':
		  primary = 1;		/* make primary bootblock */
		  break;
	    }
	}
    }

    if (i >= argc) {
	usage();
    }
    inname = argv[i++];

    fd = open(inname, O_RDONLY);
    if (fd == -1) {
	perror("open");
	exit(1);
    }

    ofd = 1;
    if (i < argc) {
	ofd = open(argv[i++], O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (ofd == -1) {
	    perror("open");
	    exit(1);
	}
    }

    if (primary) {
	/* generate bootblock for primary loader */

	unsigned long bb[64], sum = 0;
	struct stat st;
	off_t size;
	int i;

	if (ofd == 1) {
	    usage();
	}

	if (fstat(fd, &st) == -1) {
	    perror("fstat");
	    exit(1);
	}

	size = (st.st_size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);
	memset(bb, 0, sizeof(bb));
	strcpy((char *) bb, "Linux SRM bootblock");
	bb[60] = size / BLOCK_SIZE;	/* count */
	bb[61] = 1;			/* starting sector # */
	bb[62] = 0;			/* flags---must be 0 */
	for (i = 0; i < 63; ++i) {
	    sum += bb[i];
	}
	bb[63] = sum;
	if (write(ofd, bb, sizeof(bb)) != sizeof(bb)) {
	    perror("boot-block write");
	    exit(1);
	}
	printf("%lu\n", size);
	return 0;
    }

    /* read and inspect exec header: */

    if (read(fd, buf, sizeof(buf)) < 0) {
	perror("read");
	exit(1);
    }

#ifdef __ELF__
    elf = (struct elfhdr *) buf;

    if (elf->e_ident[0] == 0x7f && strncmp((char *)elf->e_ident + 1, "ELF", 3) == 0) {
	if (elf->e_type != ET_EXEC) {
	    fprintf(stderr, "%s: %s is not an ELF executable\n",
		    prog_name, inname);
	    exit(1);
	}
	if (!elf_check_arch(elf)) {
	    fprintf(stderr, "%s: is not for this processor (e_machine=%d)\n",
		    prog_name, elf->e_machine);
	    exit(1);
	}
	/*
	 * Flatten every PT_LOAD segment into one image starting at the lowest
	 * p_vaddr.  aboot.lds deliberately splits text (R+E) from data (RW), so
	 * there is more than one PT_LOAD; copying only the first silently drops
	 * .data.  That also moves _end, and net_aboot locates its boot header at
	 * align_512(&_end), so a truncated image makes the kernel and initrd
	 * sizes read as garbage.
	 */
	unsigned long base = ~0UL, end = 0;
	char *image;

	e_entry = elf->e_entry;

	phdrs = malloc((size_t) elf->e_phnum * sizeof(*phdrs));
	if (!phdrs) {
	    perror("malloc");
	    exit(1);
	}
	lseek(fd, elf->e_phoff, SEEK_SET);
	if ((size_t) read(fd, phdrs, elf->e_phnum * sizeof(*phdrs))
	    != elf->e_phnum * sizeof(*phdrs)) {
	    perror("read");
	    exit(1);
	}

	for (i = 0; i < elf->e_phnum; ++i) {
	    if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
		continue;
	    }
	    /* work around ELF bug: */
	    if (phdrs[i].p_vaddr < e_entry
		&& e_entry < phdrs[i].p_vaddr + phdrs[i].p_filesz) {
		unsigned long delta = e_entry - phdrs[i].p_vaddr;

		phdrs[i].p_offset += delta;
		phdrs[i].p_memsz  -= delta;
		phdrs[i].p_filesz -= delta;
		phdrs[i].p_vaddr  += delta;
	    }
	    if (phdrs[i].p_vaddr < base) {
		base = phdrs[i].p_vaddr;
	    }
	    if (phdrs[i].p_vaddr + phdrs[i].p_memsz > end) {
		end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
	    }
	}
	if (base == ~0UL) {
	    fprintf(stderr, "%s: no PT_LOAD segments\n", prog_name);
	    exit(1);
	}

	mem_size = end - base;
	if (pad) {
	    mem_size = ((mem_size + pad - 1) / pad) * pad;
	}
	image = calloc(1, mem_size);
	if (!image) {
	    perror("calloc");
	    exit(1);
	}

	for (i = 0; i < elf->e_phnum; ++i) {
	    if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0) {
		continue;
	    }
	    if (verbose) {
		fprintf(stderr,
			"%s: extracting %#016lx-%#016lx (at %lx)\n",
			prog_name, (unsigned long) phdrs[i].p_vaddr,
			(unsigned long) (phdrs[i].p_vaddr + phdrs[i].p_filesz),
			(unsigned long) phdrs[i].p_offset);
	    }
	    if (lseek(fd, phdrs[i].p_offset, SEEK_SET) != (off_t) phdrs[i].p_offset) {
		perror("lseek");
		exit(1);
	    }
	    if ((size_t) read(fd, image + (phdrs[i].p_vaddr - base),
			      phdrs[i].p_filesz) != phdrs[i].p_filesz) {
		perror("read");
		exit(1);
	    }
	}

	if (verbose) {
	    fprintf(stderr, "%s: writing %lu byte image (bss zero-filled,"
		    " aligned to %lu)\n",
		    prog_name, (unsigned long) mem_size, pad);
	}
	tocopy = mem_size;
	while (tocopy > 0) {
	    nwritten = write(ofd, image + (mem_size - tocopy), tocopy);
	    if ((ssize_t) nwritten == -1) {
		perror("write");
		exit(1);
	    }
	    tocopy -= nwritten;
	}
	free(image);
	free(phdrs);
	return 0;
    } else
#endif
#ifdef __alpha__
    {
	struct exec * aout = (struct exec *) buf;

	if (!(aout->fh.f_flags & COFF_F_EXEC)) {
	    fprintf(stderr, "%s: %s is not in executable format\n",
		    prog_name, inname);
	    exit(1);
	}

	if (aout->fh.f_opthdr != sizeof(aout->ah)) {
	    fprintf(stderr, "%s: %s has unexpected optional header size\n",
		    prog_name, inname);
	    exit(1);
	}

	if (N_MAGIC(*aout) != OMAGIC) {
	    fprintf(stderr, "%s: %s is not an OMAGIC file\n",
		    prog_name, inname);
	    exit(1);
	}
	offset = N_TXTOFF(*aout);
	fil_size = aout->ah.tsize + aout->ah.dsize;
	mem_size = fil_size + aout->ah.bsize;

	if (verbose) {
	    fprintf(stderr, "%s: extracting %#016lx-%#016lx (at %lx)\n",
		    prog_name, aout->ah.text_start,
		    aout->ah.text_start + fil_size, offset);
	}
    }
#else
    {
	fprintf(stderr, "%s: ECOFF format not supported on this architecture\n",
		prog_name);
	exit(1);
    }
#endif

    if (lseek(fd, offset, SEEK_SET) != offset) {
	perror("lseek");
	exit(1);
    }

    if (verbose) {
	fprintf(stderr, "%s: copying %lu byte from %s\n",
		prog_name, (unsigned long) fil_size, inname);
    }

    tocopy = fil_size;
    while (tocopy > 0) {
	n = tocopy;
	if (n > sizeof(buf)) {
	    n = sizeof(buf);
	}
	tocopy -= n;
	if ((size_t) read(fd, buf, n) != n) {
	    perror("read");
	    exit(1);
	}
	do {
	    nwritten = write(ofd, buf, n);
	    if ((ssize_t) nwritten == -1) {
		perror("write");
		exit(1);
	    }
	    n -= nwritten;
	} while (n > 0);
    }

    if (pad) {
	mem_size = ((mem_size + pad - 1) / pad) * pad;
    }

    tocopy = mem_size - fil_size;
    if (tocopy > 0) {
	fprintf(stderr,
		"%s: zero-filling bss and aligning to %lu with %lu bytes\n",
		prog_name, pad, (unsigned long) tocopy);

	memset(buf, 0x00, sizeof(buf));
	do {
	    n = tocopy;
	    if (n > sizeof(buf)) {
		n = sizeof(buf);
	    }
	    nwritten = write(ofd, buf, n);
	    if ((ssize_t) nwritten == -1) {
		perror("write");
		exit(1);
	    }
	    tocopy -= nwritten;
	} while (tocopy > 0);
    }
    return 0;
}
