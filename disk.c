/*
 * aboot/disk.c
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
#ifdef TESTING
#  include <stdlib.h>
#  include <stdio.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include "config.h"
#include "aboot.h"
#include "bootfs.h"
#include "cons.h"
#include "disklabel.h"
#include "utils.h"
#include <string.h>

#include <asm/console.h>
#include "system.h"

extern struct bootfs ext2fs;
extern struct bootfs iso;
extern struct bootfs ufs;
extern struct bootfs dummyfs;
extern struct bootfs xfsfs;

struct disklabel * label;
int boot_part = -1;

static const struct bootfs *bootfs[] = {
	&ext2fs,
	&iso,
	&ufs,
	&xfsfs
};

/*
 * Attempt a "raw" boot (uncompressed ELF kernel follows right after aboot).
 *
 * This will eventually be rewritten to accept compressed kernels
 * (along with net_aboot).  It should also be merged with the other
 * load methods as there is some code duplication here we don't want.
 */

int
load_raw (long dev)
{
	unsigned char *buf;
	long aboot_size = &_end - (char *) BOOT_ADDR;
	long ksect = (aboot_size + SECT_SIZE - 1) / SECT_SIZE + BOOT_SECTOR;
	long nread;
	int i;

	printf("aboot: loading kernel from boot sectors...\n");

	/* We only need the program headers so this should be fine */
	buf = malloc(SECT_SIZE);

	/* Read ELF headers: */
	nread = cons_read(dev, buf, SECT_SIZE, ksect * SECT_SIZE);
	if (nread != SECT_SIZE) {
		printf("aboot: read returned %ld instead of %ld bytes\n",
		       nread, (long) SECT_SIZE);
		return -1;
	}
	if (!is_loadable_elf(buf, SECT_SIZE)) {
		return -1;
	}

	for (i = 0; i < nchunks; ++i) {
		char *dest;

		printf("aboot: segment %d, %ld bytes at %#lx\n", i, chunks[i].size,
		       chunks[i].addr);
#ifdef TESTING
		dest = malloc(chunks[i].size);
#else
		dest = (char *) chunks[i].addr;
#endif

		nread = cons_read(dev, dest, chunks[i].size,
				  chunks[i].offset + ksect * SECT_SIZE);
		if (nread != chunks[i].size) {
			printf("aboot: read returned %ld instead of %ld bytes\n",
			       nread, chunks[i].size);
			return -1;
		}
	}
	return 0;
}


int
load_uncompressed (int fd)
{
	long nread, nblocks;
	unsigned char *buf;
	int i;

	buf = malloc(bfs->blocksize);

	/* read ELF headers: */
	nread = (*bfs->bread)(fd, 0, 1, (char *) buf);
	if (nread != bfs->blocksize) {
		printf("aboot: read returned %ld instead of %ld bytes\n",
		       nread, sizeof(buf));
		return -1;
	}
#ifdef DEBUG
	{
		int i,j,c;

		for(i = 0; i < 16; i++) {
			for (j = 0; j < 16; j++)
				printf("%02X ", buf[j+16*i]);
			for(j = 0; j < 16; j++) {
				c = buf[j+16*i];
				printf("%c", (c >= ' ') ? c : ' ');
			}
			printf("\n");
		}
	}
#endif
	if (!is_loadable_elf(buf, bfs->blocksize)) {
		return -1;
	}

	/* read one segment at a time */
	for (i = 0; i < nchunks; ++i) {
		char *dest;

		/* include any unaligned bits of the offset */
		nblocks = (chunks[i].size + (chunks[i].offset & (bfs->blocksize - 1)) +
			   bfs->blocksize - 1) / bfs->blocksize;
		printf("aboot: segment %d, %ld bytes at %#lx\n", i, chunks[i].size,
		       chunks[i].addr);
#ifdef TESTING
		dest = malloc(nblocks * bfs->blocksize);
#else
		dest = (char *) chunks[i].addr;
#endif

		nread = (*bfs->bread)(fd, chunks[i].offset / bfs->blocksize,
				      nblocks, dest);
		if (nread != nblocks * bfs->blocksize) {
			printf("aboot: read returned %ld instead of %ld bytes\n",
			       nread, nblocks * bfs->blocksize);
			return -1;
		}
		/* In practice, they will always be aligned */
		if ((chunks[i].offset & (bfs->blocksize - 1)) != 0)
			memmove(dest,
				dest + (chunks[i].offset & (bfs->blocksize - 1)),
				chunks[i].size);
	}
	return 0;
}

static long
read_kernel (const char *filename)
{
	volatile int attempt, method;
	long len;
	int fd;
	static struct {
		const char *name;
		int (*func)(int fd);
	} read_method[]= {
		{"uncompressed", load_uncompressed},
		{"compressed",	 uncompress_kernel}
	};
	long res;
#	define NUM_METHODS ((int)(sizeof(read_method)/sizeof(read_method[0])))

#ifdef DEBUG
	printf("read_kernel(%s)\n", filename);
#endif

	method = 0;
	len = strlen(filename);
	if (len > 3 && filename[len - 3] == '.'
	    && filename[len - 2] == 'g' && filename[len - 1] == 'z')
	{
		/* if filename ends in .gz we don't try plain method: */
		method = 1;
	}

	for (attempt = 0; attempt < NUM_METHODS; ++attempt) {
		fd = (*bfs->open)(filename);
		if (fd < 0) {
			printf("%s: file not found\n", filename);
			return -1;
		}
		printf("aboot: loading %s %s...\n",
		       read_method[method].name, filename);

		if (!_setjmp(jump_buffer)) {
			res = (*read_method[method].func)(fd);

			(*bfs->close)(fd);
			if (res >= 0) {
				return 0;
			}
		}
		method = (method + 1) % NUM_METHODS;
	}
	return -1;
}

long
read_initrd()
{
	int nblocks, nread, fd;
	struct stat buf;

	fd = (*bfs->open)(initrd_file);
	if (fd < 0) {
		printf("%s: file not found\n", initrd_file);
		return -1;
	}
	(*bfs->fstat)(fd, &buf);
	initrd_size = buf.st_size;

#ifdef TESTING
	initrd_start = (unsigned long) malloc(initrd_size);
#else
	/* put it as high up in memory as possible */
	if (!free_mem_ptr)
		free_mem_ptr = memory_end();
	/* page aligned (downward) */
	initrd_start = (free_mem_ptr - initrd_size) & ~(PAGE_SIZE-1);
	/* update free_mem_ptr so malloc() still works */
	free_mem_ptr = initrd_start;
#endif

	nblocks = initrd_size / bfs->blocksize;
	printf("aboot: loading initrd (%ld bytes/%d blocks) at %#lx\n",
		initrd_size, nblocks, initrd_start);
	if (nblocks & (bfs->blocksize - 1)) nblocks++;
	nread = (*bfs->bread)(fd, 0, nblocks, (char*) initrd_start);
	if (nread != nblocks * bfs->blocksize) {
		printf("aboot: read returned %d instead of %d (%d*%d) bytes\n",
			nread, nblocks * bfs->blocksize,
			nblocks, bfs->blocksize);
		return -1;
	}
	return 0;
}

static void
get_disklabel (long dev)
{
	static char lsect[512];
	long nread;

#ifdef DEBUG
	printf("load_label(dev=%lx)\n", dev);
#endif
	nread = cons_read(dev, &lsect, LABELOFFSET + sizeof(*label),
			  LABELSECTOR);
	if (nread != LABELOFFSET + sizeof(*label)) {
		printf("aboot: read of disklabel sector failed (nread=%ld)\n",
		       nread);
		return;
	}
	label = (struct disklabel*) &lsect[LABELOFFSET];
	if (label->d_magic  == DISKLABELMAGIC &&
	    label->d_magic2 == DISKLABELMAGIC)
	{
		printf("aboot: valid disklabel found: %d partitions.\n",
		       label->d_npartitions);
	} else {
		printf("aboot: no disklabel found.\n");
		label = 0;
	}
}


const struct bootfs *
mount_fs (long dev, int partition)
{
	struct d_partition * part;
	const struct bootfs * fs = 0;
	int i;

#ifdef DEBUG
	printf("mount_fs(%lx, %d)\n", dev, partition);
#endif
	if (partition == 0) {
		fs = &dummyfs;
		if ((*fs->mount)(dev, 0, 0) < 0) {
			printf("aboot: disk mount failed\n");
			return 0;
		}
	} else if (!label) {
		/* floppies and such, no disklabel */
		for (i = 0; i < (int)(sizeof(bootfs)/sizeof(bootfs[0])); ++i) {
			if ((*bootfs[i]->mount)(dev, 0, 1) >= 0) {
				fs = bootfs[i];
				break;
			}
		}
		if (!fs) {
			printf("aboot: unknown filesystem type\n");
			return 0;
		}
	} else {
		if ((unsigned) (partition - 1) >= label->d_npartitions) {
			printf("aboot: invalid partition %u\n", partition);
			return 0;
		}
		part = &label->d_partitions[partition - 1];
		for (i = 0; i < (int)(sizeof(bootfs)/sizeof(bootfs[0])); i++) {
			if (bootfs[i]->fs_type == part->p_fstype) {
			        fs = bootfs[i];
				if (!((*fs->mount)(dev, (long)(part->p_offset) * (long)(label->d_secsize), 1)
				< 0))
				return fs;
			}
		}
		printf("aboot: don't know how to mount "
		       "partition %d (filesystem type %d)\n",
	  	        partition, part->p_fstype);
		return 0;
	}
	return fs;
}

void
list_directory (const struct bootfs *fs, char *dir)
{
	int fd = (*fs->open)(dir);
	/* yes, our readdir() is not exactly like the real one */
	int rewind = 0;
	const char * ent;

	if (fd < 0) {
		printf("%s: directory not found\n", dir);
		return;
	}

	while ((ent = (*fs->readdir)(fd, !rewind++))) {
		printf("%s\n", ent);
	}
	(*fs->close)(fd);
}

int
open_config_file(const struct bootfs *fs)
{
	static const char *configs[] = {
		"/etc/aboot.conf",
		"/aboot.conf",
		"/etc/aboot.cfg",
		"/aboot.cfg"
	};
	const int nconfigs = sizeof(configs) / sizeof(configs[0]);
	int i, fd = -1;

	for (i = 0; i < nconfigs; i++) {
		fd = (*fs->open)(configs[i]);
		if (fd >= 0)
			break;
	}
	return fd;
}

void
print_config_file (const struct bootfs *fs)
{
	int fd, nread, blkno = 0;
	char *buf;

	fd = open_config_file(fs);
	if (fd < 0) {
		printf("%s: file not found\n", CONFIG_FILE);
		return;
	}
	buf = malloc(fs->blocksize + 1);
	if (!buf) {
		printf("aboot: malloc failed!\n");
		return;
	}
	do {
		nread = (*fs->bread)(fd, blkno++, 1, buf);
		buf[nread] = '\0';
		printf("%s", buf);
	} while (nread > 0);
	(*fs->close)(fd);
}


int
get_default_args (const struct bootfs *fs, char *str, int num)
{
	int fd, nread, state, line, blkno = 0;
	char *buf, *d, *p;

	*str = '\0';
	fd = open_config_file(fs);
	if (fd < 0) {
		printf("%s: file not found\n", CONFIG_FILE);
		return -1;
	}
	buf = malloc(fs->blocksize);
	if (!buf) {
		printf("aboot: malloc failed!\n");
		return -1;
	}
	d = str;
	line = 1;
	state = 2;
	do {
		nread = (*fs->bread)(fd, blkno++, 1, buf);
		p = buf;
		while (p < buf + nread && *p && state != 5) {
			switch (state) {
			      case 0: /* ignore rest of line */
			      case 1: /* in comment */
				if (*p == '\n') state = 2;
				break;

			      case 2: /* after end of line */
				line++;
				if (*p == num) {
					state = 3;	/* found it... */
					break;
				}
				if (*p == '#') {
					state = 1;	/* comment */
					break;
				}
				if (*p == '-') {
					state = 5;	/* end-of-file mark */
					break;
				}
				state = 0;	/* ignore rest of line */
				break;

			      case 3: /* after matched number */
				if (*p == ':') {
					state = 4;	/* copy string */
				} else {
					state = 2;	/* ignore rest */
					printf("aboot: syntax error in line "
					       "%d: `:' expected\n", line);
				}
				break;

			      case 4: /* copy until EOL */
				if (*p == '\n') {
					*d = 0;
					state=5;
				} else {
					*d++ = *p;
				}
				break;

			      default:
				break;
			}
			p++;
		}
	} while (nread > 0 && state != 5);
	(*fs->close)(fd);
#ifdef DEBUG
	printf("get_default done\n");
#endif

	if (state != 5) {
		printf("aboot: could not find default config `%c'\n", num);
		return -1;
	}
#ifdef DEBUG
	printf("get_default_args(%s,%d)\n", str, num);
#endif
	return 0;
}


static void
print_help(void)
{
	printf("Commands:\n"
	       " h, ?			Display this message\n"
	       " q			Halt the system and return to SRM\n"
	       " p 1-8			Look in partition <num> for configuration/kernel\n"
	       " l			List preconfigured kernels\n"
	       " d <dir>		List directory <dir> in current filesystem\n"
	       " b <file> <args>	Boot kernel in <file> (- for raw boot)\n"
	       " i <file>		Use <file> as initial ramdisk\n"
	       "			with arguments <args>\n"
	       " 0-9			Boot preconfiguration 0-9 (list with 'l')\n");
}

static void
get_aboot_options (long dev)
{
	int preset      = 0; /* no preset */
	int interactive = 0;  /* non-interactive */
	char *extra_args = NULL;

#ifdef DEBUG
	printf("get_aboot_options(%lx)\n",dev);
	printf("kernel_args=\"%s\"\n",kernel_args);
#endif

	/* Forms of -flags argument from SRM */
	if (kernel_args[0] >= '1' && kernel_args[0] <= '9'
	    && kernel_args[1] == ':' && kernel_args[2]
	    && (kernel_args[3] == '\0' || kernel_args[3] == ' '))
	{
		/* <partition>:<preset> - where <preset> is an entry
                   in /etc/aboot.conf (to be found on <partition>), or
                   'i' for interactive */
		config_file_partition = kernel_args[0] - '0';
		preset = kernel_args[2];
		if (kernel_args[3]) {
			extra_args=&kernel_args[3];
			while (*extra_args == ' ') { extra_args++; }
			if (*extra_args == '\0') extra_args = NULL;
		}
#ifdef DEBUG
		printf("partition:preset = %ld:%c\n", config_file_partition,
		       preset);
#endif
	} else if (kernel_args[0]
		   && (kernel_args[1] == '\0' || kernel_args[1] == ' '))  {
		/* Single character option, for Jensen and friends -
                   this is either a preconfigured entry in
                   /etc/aboot.conf or 'i' for interactive*/
		if (kernel_args[0] == 'i') interactive = 1;
		else {
			preset = kernel_args[0];
			if (kernel_args[1]) {
				/* are there actually extra args? */
				extra_args=&kernel_args[1];
				while (*extra_args == ' ') { extra_args++; }
				if (*extra_args == '\0') extra_args = NULL;
			}
		}
	} else if (kernel_args[0] == '\0') {
		interactive = 1;
	} else {
		/* attempt to parse the arguments given */
	}

#ifdef DEBUG
	if (extra_args) printf("extra args: \"%s\"\n",extra_args);
#endif

	if (preset || interactive) {
		char buf[256], *p;
		const struct bootfs *fs = 0;
		static int first = 1;
		int done = 0;

		while (!done) {
			/* If we have a setting from /etc/aboot.conf, use it */
			if (preset) {
#ifdef DEBUG
				printf("trying preset %c\n", preset);
#endif
				if (!fs) {
					fs = mount_fs(dev, config_file_partition);
					if (!fs) {
						preset = 0;
						continue;
					}
				}
				if (get_default_args(fs, buf, preset) >= 0)
					break;

				/* Doh, keep on going */
				preset = 0;
				continue;
			}

			/* Otherwise, clear out kernel_args and prompt the user */
			kernel_args[0] = 0;
			if (first) {
				printf("Welcome to aboot " ABOOT_VERSION "\n");
				print_help();
				first = 0;
			}
			printf("aboot> ");
#ifdef TESTING
			fgets(buf, sizeof(buf), stdin);
			buf[strlen(buf)-1] = 0;
#else
			getline(buf, sizeof(buf));
#endif
			printf("\n");

			switch (buf[0]) {
			case 'h':
			case '?':
				print_help();
				break;
			case 'q':
				halt();
				break;
			case 'p':
				p = strchr(buf, ' ');
				while (p && *p == ' ') ++p;

				if (p && p[0] >= '1' && p[0] <= '8'
				    && (p[1] == '\0' || p[1] == ' ')) {
					config_file_partition = p[0] - '0';
					fs = 0; /* force reread */
				} else {
					printf("Please specify a number between 1 and 8\n");
				}
				break;
			case 'l':
				if (!fs) {
					fs = mount_fs(dev, config_file_partition);
					if (!fs) {
						printf("Partition %ld is invalid. "
						       "Please specify another with 'p'\n",
						       config_file_partition);
						continue;
					}
				}
				print_config_file(fs);
				break;
			case 'd':
				if (!fs) {
					fs = mount_fs(dev, config_file_partition);
					if (!fs) {
						printf("Partition %ld is invalid. "
						       "Please specify another with 'p'\n",
						       config_file_partition);
						continue;
					}
				}
				/* skip past whitespace */
				p = strchr(buf, ' ');
				while (p && *p == ' ') ++p;
				if (p)
					list_directory(fs, p);
				else
					list_directory(fs, "/");
				break;
			case 'b':
				/* skip past whitespace */
				p = strchr(buf, ' ');
				while (p && *p == ' ') ++p;
				if (p) {
					strcpy(buf, p);
					done = 1;
				} else {
					printf("Please specify a file to load the kernel from, "
					       "or '-' to load the kernel from the boot sector\n");
				}
				break;
			case 'i':
				/* skip past whitespace */
				p = strchr(buf, ' ');
				while (p && *p == ' ') ++p;
				if (p)
					strcpy(initrd_file, p);
				else {
					printf("Please specify a file to use as initial ramdisk\n");
				}
				break;
			case '0' ... '9':
				preset = buf[0];
				p = strchr(buf, ' ');
				while (p && *p == ' ') ++p;
				if (p) {
					strcpy(kernel_args, p);
					extra_args=kernel_args;
				}
				break;
			default:
				break;

			}
		}

		/* if we have extra args, append them to buf */
		if (extra_args) {
			p=buf; while (p && *p) p++;
			*p++=' ';
			strcpy(p, extra_args);
		}

		/* split on space into kernel + args */
		p = strchr(buf, ' ');
		if (p) {
			/* skip past leading whitespace */
			*p++ = '\0';
			while (p && *p == ' ') ++p;
			strcpy(kernel_args, p);
		}
		strcpy(boot_file, buf);
	}

#ifdef DEBUG
	printf("boot_file=\"%s\", kernel_args=\"%s\"\n",boot_file,kernel_args);
#endif

	{
		/* parse off initrd= option from kernel_args if any */
		char *p = kernel_args;
		/* poor man's strstr */
		do {
			if (strncmp(p, "initrd=", 7) == 0)
				break;
		} while (*p++);

		if (*p) {
			char *a = p + 7; /* argument */
			char *e = strchr (a, ' ');
			if (e) {
				strncpy(initrd_file, a, e-a);
				initrd_file[e-a] = 0;
				strcpy(p, e);
			} else {
				strcpy(initrd_file, a);
				*p = 0;
			}
		}
	}


	/* parse off partition number from boot_file if any: */
	if (boot_file[0] >= '0' && boot_file[0] <= '9' && boot_file[1] == '/')
	{
		boot_part = boot_file[0] - '0';
		strcpy(boot_file, boot_file + 2);
	} else {
		boot_part = config_file_partition;
	}
}

static long
load (long dev)
{
	char *fname;

#ifdef DEBUG
	printf("load(%lx)\n", dev);
#endif
	fname = boot_file;
	if (fname[0] == '-' && fname[1] == '\0') {
		/* a single "-" implies raw boot: */
		if (load_raw(dev) < 0) {
			return -1;
		}
	} else {
		/* if there's no disklabel, boot_part will be ignored anyway */
		bfs = mount_fs(dev, boot_part);
		if (!bfs) {
			printf("aboot: mount of partition %d failed\n", boot_part);
			return -1;
		}
		if (read_kernel(fname) < 0) {
			return -1;
		}
	}
	/* clear bss: */
	printf("aboot: zero-filling %ld bytes at 0x%p\n", bss_size, bss_start);
#ifndef TESTING
	memset((char*)bss_start, 0, bss_size);
#endif

	if (initrd_file[0] == 0)
		return 0;

	/* work around a bug in the ext2 code */
	bfs = mount_fs(dev, boot_part);
	if (!bfs) {
		printf("aboot: mount of partition %d failed\n", boot_part);
		return -1;
	}
	if (read_initrd() < 0) {
		return -1;
	}
	return 0;
}


long
load_kernel (void)
{
	char envval[256];
	long result;
	long dev;

#ifdef TESTING
	const char *e;
	if ((e = getenv("BOOTED_DEV"))) {
		strncpy(envval, e, sizeof(envval)-1);
		envval[sizeof(envval)-1] = 0;
	} else {
		printf("aboot: Can't get BOOTED_DEV environment variable!\n");
		return -1;
	}
#else
	if (cons_getenv(ENV_BOOTED_DEV, envval, sizeof(envval)) < 0) {
		printf("aboot: Can't get BOOTED_DEV environment variable!\n");
		return -1;
	}
#endif

	printf("aboot: booting from device '%s'\n", envval);
	dev = cons_open(envval);
	if (dev < 0) {
		printf("aboot: unable to open boot device `%s': %lx\n",
		       envval, dev);
		return -1;
	}
	dev &= 0xffffffff;
	get_disklabel(dev);

	while (1) {
		get_aboot_options(dev);
		result = load(dev);
		if (result != -1)
			break;
		/* load failed---query user interactively */
		strcpy(kernel_args, "i");
	}
#ifdef DEBUG
	printf("load done\n");
#endif
	cons_close(dev);
	return result;
}
