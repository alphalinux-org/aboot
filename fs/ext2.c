/*
 * This is a set of functions that provides minimal filesystem
 * functionality to the Linux bootstrapper.  All we can do is
 * open and read files... but that's all we need 8-)
 *
 * This file has been ported from the DEC 32-bit Linux version
 * by David Mosberger (davidm@cs.arizona.edu).
 */
#include <sys/stat.h>
#include <ext2fs/ext2_fs.h>

#include "bootfs.h"
#include "cons.h"
#include "disklabel.h"
#include "ext4.h"
#include "utils.h"
#include <string.h>
#include <stdint.h>
#define MAX_OPEN_FILES		5

/*
 * What this build of aboot can safely cope with.
 *
 * Keep these masks conservative; you can relax them as you implement
 * more features.
 */

#ifndef EXT4_FEATURE_INCOMPAT_SUPP
#define EXT4_FEATURE_INCOMPAT_SUPP \
    (EXT4_FEATURE_INCOMPAT_FILETYPE    | \
     EXT4_FEATURE_INCOMPAT_RECOVER     | \
     EXT4_FEATURE_INCOMPAT_EXTENTS     | \
     EXT4_FEATURE_INCOMPAT_64BIT       | \
     EXT4_FEATURE_INCOMPAT_FLEX_BG     | \
     EXT4_FEATURE_INCOMPAT_MMP         | \
     EXT4_FEATURE_INCOMPAT_CSUM_SEED)
#endif

#ifndef EXT4_FEATURE_RO_COMPAT_SUPP
#define EXT4_FEATURE_RO_COMPAT_SUPP \
    (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  | \
     EXT4_FEATURE_RO_COMPAT_LARGE_FILE    | \
     EXT4_FEATURE_RO_COMPAT_BTREE_DIR     | \
     EXT4_FEATURE_RO_COMPAT_HUGE_FILE     | \
     EXT4_FEATURE_RO_COMPAT_DIR_NLINK     | \
     EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   | \
     EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
#endif
extern struct bootfs ext2fs;

static struct ext2_super_block sb;
static struct ext2_group_desc *gds;
static struct ext2_inode *root_inode = NULL;
static int ngroups = 0;
static long directlim;			/* Maximum direct blkno */
static long ind1lim;			/* Maximum single-indir blkno */
static long ind2lim;			/* Maximum double-indir blkno */
static long ind3lim;			/* Maximum triple-indir blkno */
static int ptrs_per_blk;		/* ptrs/indirect block */
static char *blkbuf;
static int cached_iblkno = -1;
static char *iblkbuf;
static int cached_diblkno = -1;
static char *diblkbuf;
static int cached_tiblkno = -1;
static char *tiblkbuf;
static long dev = -1;
static long partition_offset;
static unsigned int group_desc_size = 0;

static struct inode_table_entry {
	struct	ext2_inode	inode;
	int			inumber;
	int			free;
	unsigned short		old_mode;
} inode_table[MAX_OPEN_FILES];

static int ext4_check_features(int quiet)
{
    /* sb is the global ext2_super_block already filled in by ext2_mount() */
#ifdef DEBUG_EXT2
    uint32_t compat = sb.s_feature_compat;
    printf("ext2/4: FS compat features: 0x%08x\n", compat);
#endif
    uint32_t ro_compat = sb.s_feature_ro_compat;
    uint32_t incompat  = sb.s_feature_incompat;
    uint32_t missing;

    /* First check incompatible features – these must all be understood */
    missing = incompat & ~EXT4_FEATURE_INCOMPAT_SUPP;
    if (missing) {
        if (!quiet) {
            printf("ext2/4: unsupported INCOMPAT features: 0x%08x", missing);

            if (missing & EXT4_FEATURE_INCOMPAT_64BIT)
                printf(" (64bit)");
            if (missing & EXT4_FEATURE_INCOMPAT_META_BG)
                printf(" (meta_bg)");
            if (missing & EXT4_FEATURE_INCOMPAT_FLEX_BG)
                printf(" (flex_bg)");
            if (missing & EXT4_FEATURE_INCOMPAT_MMP)
                printf(" (mmp)");
	    if (missing & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
		printf(" (csum_seed)");
            /* add more decodes here as you implement them */

            printf("\n");
            printf("        Use an ext2/3/4 filesystem without these features\n");
            printf("        (for example: tune2fs -O ^64bit,^metadata_csum /dev/XXX\n");
            printf("         or create a small ext2/ext3 /boot partition).\n");
        }
        return -1;
    }

    /*
     * Now check RO-compat features.
     * In a read-only loader, unknown RO-compat features are theoretically
     * safe, but some (e.g. metadata_csum) *do* affect on-disk layout, so
     * we still reject unknown ones for now.
     */
    missing = ro_compat & ~EXT4_FEATURE_RO_COMPAT_SUPP;
    if (missing) {
        if (!quiet) {
            printf("ext2/4: unsupported RO_COMPAT features: 0x%08x", missing);

            if (missing & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
                printf(" (metadata_csum)");
            if (missing & EXT4_FEATURE_RO_COMPAT_BIGALLOC)
                printf(" (bigalloc)");
            if (missing & EXT4_FEATURE_RO_COMPAT_QUOTA)
                printf(" (quota)");
            /* add more decodes here as needed */

            printf("\n");
            printf("        This aboot build cannot read this ext4 layout safely.\n");
        }
        return -1;
    }

#ifdef DEBUG_EXT2
    if (!quiet) {
        printf("ext2/4: features OK: compat=0x%08x ro_compat=0x%08x "
               "incompat=0x%08x\n",
               compat, ro_compat, incompat);
    }
#endif
    return 0;
}

/*
 * Initialize an ext2 partition starting at offset P_OFFSET; this is
 * sort-of the same idea as "mounting" it.  Read in the relevant
 * control structures and make them available to the user.  Returns 0
 * if successful, -1 on failure.
 */
static int ext2_mount(long cons_dev, long p_offset, long quiet)
{
	long sb_block = 1;
	long sb_offset;
	long gdt_start;
	int i;

	dev = cons_dev;
	partition_offset = p_offset;

	/* initialize the inode table */
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		inode_table[i].free = 1;
		inode_table[i].inumber = 0;
	}
	/* clear the root inode pointer (very important!) */
	root_inode = NULL;

	/* read in the first superblock */
	sb_offset = sb_block * EXT2_MIN_BLOCK_SIZE;
	if (cons_read(dev, &sb, sizeof(sb), partition_offset + sb_offset)
	    != sizeof(sb))
	{
		printf("ext2 sb read failed\n");
		return -1;
	}

	if (sb.s_magic != EXT2_SUPER_MAGIC) {
		if (!quiet) {
			printf("ext2_init: bad magic 0x%x\n", sb.s_magic);
		}
		return -1;
	}
    /*
     * ext4: group descriptor size.  For classic ext2 this is either
     * zero or 32.  For ext4 with 64bit it is typically 64.
     *
     * We only care about the first sizeof(struct ext2_group_desc)
     * bytes, which are laid out compatibly with ext2.
     */
    group_desc_size = sb.s_desc_size;
    if (group_desc_size == 0)
        group_desc_size = sizeof(struct ext2_group_desc);

    if (group_desc_size < sizeof(struct ext2_group_desc)) {
        if (!quiet)
            printf("ext2/4: group descriptor size %u too small\n",
                   group_desc_size);
        return -1;
    }
	if (ext4_check_features(quiet) < 0)
		return -1;

	/*
	 * s_blocks_count is the low half of a 64-bit count once the 64bit
	 * feature is on.  Everything below, ngroups included, works in 32-bit
	 * block numbers, so a filesystem that actually needs the high half is
	 * one we cannot address.
	 */
	if ((sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
	    && sb.s_blocks_count_hi) {
		if (!quiet)
			printf("ext2/4: filesystem larger than 2^32 blocks\n");
		return -1;
	}

	ngroups = (sb.s_blocks_count -
		   sb.s_first_data_block +
		   EXT2_BLOCKS_PER_GROUP(&sb) - 1)
		/ EXT2_BLOCKS_PER_GROUP(&sb);

	ext2fs.blocksize = EXT2_BLOCK_SIZE(&sb);
	if (group_desc_size > (unsigned int) ext2fs.blocksize) {
		if (!quiet)
			printf("ext2/4: group descriptor size %u > blocksize %d\n",
			       group_desc_size, ext2fs.blocksize);
		return -1;
	}

	gds = malloc((size_t)(ngroups * sizeof(struct ext2_group_desc)));
	if (!gds) {
		if (!quiet)
			printf("ext2: no memory for group descriptors\n");
		return -1;
	}

	blkbuf = malloc(ext2fs.blocksize);
	iblkbuf = malloc(ext2fs.blocksize);
	diblkbuf = malloc(ext2fs.blocksize);
	tiblkbuf = malloc(ext2fs.blocksize);

	/* read in the group descriptors (immediately follows superblock) */

	gdt_start = partition_offset +
		ext2fs.blocksize *
		(EXT2_MIN_BLOCK_SIZE / ext2fs.blocksize + 1);

	/*
	 * Descriptors are group_desc_size apart on disk but only the first
	 * sizeof(struct ext2_group_desc) bytes are laid out the way we read
	 * them, so copy that much out of each.  Both sizes are powers of two
	 * no larger than the block size and the table starts on a block
	 * boundary, so no descriptor straddles a chunk.
	 */
	{
		long gdt_bytes = (long) ngroups * group_desc_size;
		long done = 0;
		int g = 0;

		while (done < gdt_bytes && g < ngroups) {
			long chunk = gdt_bytes - done;
			long off;

			if (chunk > ext2fs.blocksize)
				chunk = ext2fs.blocksize;

			if (cons_read(dev, blkbuf, chunk, gdt_start + done)
			    != chunk) {
				if (!quiet)
					printf("ext2/4: group descriptor read"
					       " failed at %ld\n", done);
				return -1;
			}

			for (off = 0;
			     off + group_desc_size <= chunk && g < ngroups;
			     off += group_desc_size, g++) {
				memcpy(&gds[g], blkbuf + off,
				       sizeof(struct ext2_group_desc));

				/*
				 * The upper halves of the block numbers live
				 * in the second half of a 64-byte descriptor,
				 * which we do not keep.  They are zero on any
				 * filesystem small enough for that to be
				 * irrelevant; if they are not, we would read
				 * from the wrong block rather than fail.
				 */
				if (group_desc_size >=
				    sizeof(struct ext4_group_desc)) {
					const struct ext4_group_desc *g4 =
					    (const struct ext4_group_desc *)
					    (blkbuf + off);

					if (g4->bg_block_bitmap_hi
					    || g4->bg_inode_bitmap_hi
					    || g4->bg_inode_table_hi) {
						if (!quiet)
							printf("ext2/4: group %d"
							       " is beyond 2^32"
							       " blocks\n", g);
						return -1;
					}
				}
			}
			done += chunk;
		}
	}

	/*
	 * Calculate direct/indirect block limits for this file system
	 * (blocksize dependent):
	 */
	ext2fs.blocksize = EXT2_BLOCK_SIZE(&sb);
	directlim = EXT2_NDIR_BLOCKS - 1;
	ptrs_per_blk = ext2fs.blocksize/sizeof(unsigned int);
	ind1lim = (long) ptrs_per_blk + directlim;
	ind2lim = (long) ptrs_per_blk * ptrs_per_blk + ind1lim;
	ind3lim = (long) ptrs_per_blk * ptrs_per_blk * ptrs_per_blk + ind2lim;

	return 0;
}


/*
 * Read the specified inode from the disk and return it to the user.
 * Returns NULL if the inode can't be read...
 */
static struct ext2_inode *ext2_iget(int ino)
{
	int i;
	struct ext2_inode *ip;
	struct inode_table_entry *itp = 0;
	int group;
	long offset;

	ip = 0;
	for (i = 0; i < MAX_OPEN_FILES; i++) {
#ifdef DEBUG_EXT2
		printf("ext2_iget: looping, entry %d inode %d free %d\n",
		       i, inode_table[i].inumber, inode_table[i].free);
#endif
		if (inode_table[i].free) {
			itp = &inode_table[i];
			ip = &itp->inode;
			break;
		}
	}
	if (!ip) {
		printf("ext2_iget: no free inodes\n");
		return NULL;
	}

	group = (ino-1) / sb.s_inodes_per_group;
#ifdef DEBUG_EXT2
	printf("group is %d\n", group);
#endif
	offset = partition_offset
		+ ((long) gds[group].bg_inode_table * (long)ext2fs.blocksize)
		+ (((ino - 1) % EXT2_INODES_PER_GROUP(&sb))
		   * EXT2_INODE_SIZE(&sb));
#ifdef DEBUG_EXT2
	printf("ext2_iget: reading %ld bytes at offset %ld "
	       "(%ld + (%d * %d) + ((%d) %% %d) * %d) "
	       "(inode %d -> table %d)\n",
	       sizeof(struct ext2_inode), offset, partition_offset,
	       gds[group].bg_inode_table, ext2fs.blocksize,
	       ino - 1, EXT2_INODES_PER_GROUP(&sb), EXT2_INODE_SIZE(&sb),
	       ino, (int) (itp - inode_table));
#endif
	if (cons_read(dev, ip, sizeof(struct ext2_inode), offset)
	    != sizeof(struct ext2_inode))
	{
		printf("ext2_iget: read error\n");
		return NULL;
	}

	itp->free = 0;
	itp->inumber = ino;
	itp->old_mode = ip->i_mode;

	return ip;
}


/*
 * Release our hold on an inode.  Since this is a read-only application,
 * don't worry about putting back any changes...
 */
static void ext2_iput(struct ext2_inode *ip)
{
	struct inode_table_entry *itp;

	/* Find and free the inode table slot we used... */
	itp = (struct inode_table_entry *)ip;

#ifdef DEBUG_EXT2
	printf("ext2_iput: inode %d table %d\n", itp->inumber,
	       (int) (itp - inode_table));
#endif
	itp->inumber = 0;
	itp->free = 1;
}


/*
 * Read indirect block BLKNO into BUF, unless *CACHED already holds it.
 * Returns 0 on success, -1 on read failure.
 */
static int ext2_read_indirect(int blkno, int *cached, char *buf, const char *what)
{
	unsigned long offset;

	if (*cached == blkno) {
		return 0;
	}

	offset = partition_offset + (long) blkno * (long) ext2fs.blocksize;
	if (cons_read(dev, buf, ext2fs.blocksize, offset) != ext2fs.blocksize) {
		printf("ext2_blkno: err reading %s blk\n", what);
		return -1;
	}

	*cached = blkno;
	return 0;
}

/*
 * Map a block offset into a file into an absolute block number.
 * (traverse the indirect blocks if necessary).
 *
 * The "allocate" argument is set if we want to *allocate* a block
 * and we don't already have one allocated.
 */
static int ext2_blkno(struct ext2_inode *ip, int blkoff)
{
	unsigned int *ilp;
	unsigned int *dlp;
	unsigned int *tlp;
	int blkno;
	int iblkno;
	int diblkno;
	int tiblkno;

	ilp = (unsigned int *)iblkbuf;
	dlp = (unsigned int *)diblkbuf;
	tlp = (unsigned int *)tiblkbuf;

	/* If it's a direct block, it's easy! */
	if (blkoff <= directlim) {
		return ip->i_block[blkoff];
	}

	/* Is it a single-indirect? */
	if (blkoff <= ind1lim) {
		iblkno = ip->i_block[EXT2_IND_BLOCK];

		if (iblkno == 0) {
			return 0;
		}

		/* Read the indirect block */
		if (ext2_read_indirect(iblkno, &cached_iblkno, iblkbuf, "iblk") < 0) {
			return 0;
		}

		blkno = ilp[blkoff-(directlim+1)];
		return blkno;
	}

	/* Is it a double-indirect? */
	if (blkoff <= ind2lim) {
		/* Find the double-indirect block */
		diblkno = ip->i_block[EXT2_DIND_BLOCK];

		if (diblkno == 0) {
			return 0;
		}

		/* Read in the double-indirect block */
		if (ext2_read_indirect(diblkno, &cached_diblkno, diblkbuf, "dindr") < 0) {
			return 0;
		}

		/* Find the single-indirect block pointer ... */
		iblkno = dlp[(blkoff - (ind1lim+1)) / ptrs_per_blk];

		if (iblkno == 0) {
			return 0;
		}

		/* Read the indirect block */
		if (ext2_read_indirect(iblkno, &cached_iblkno, iblkbuf, "iblk") < 0) {
			return 0;
		}

		/* Find the block itself. */
		blkno = ilp[(blkoff-(ind1lim+1)) % ptrs_per_blk];
		return blkno;
	}

	/* Is it a triple-indirect? */
	if (blkoff <= ind3lim) {
		/* Find the triple-indirect block */
		tiblkno = ip->i_block[EXT2_TIND_BLOCK];

		if (tiblkno == 0) {
			return 0;
		}

		/* Read in the triple-indirect block */
		if (ext2_read_indirect(tiblkno, &cached_tiblkno, tiblkbuf, "tindr") < 0) {
			return 0;
		}

		/* Find the double-indirect block pointer ... */
		diblkno = tlp[(blkoff - (ind2lim+1)) / (ptrs_per_blk * ptrs_per_blk)];

		if (diblkno == 0) {
			return 0;
		}

		/* Read in the double-indirect block */
		if (ext2_read_indirect(diblkno, &cached_diblkno, diblkbuf, "dindr") < 0) {
			return 0;
		}

		/* Find the single-indirect block pointer ... */
		iblkno = dlp[((blkoff - (ind2lim+1)) / ptrs_per_blk) % ptrs_per_blk];

		if (iblkno == 0) {
			return 0;
		}

		/* Read the indirect block */
		if (ext2_read_indirect(iblkno, &cached_iblkno, iblkbuf, "iblk") < 0) {
			return 0;
		}

		/* Find the block itself. */
		blkno = ilp[(blkoff - (ind2lim+1)) % ptrs_per_blk];
		return blkno;
	}

	if (blkoff > ind3lim) {
		printf("ext2_blkno: block number too large: %d\n", blkoff);
		return 0;
	}
	return -1;
}

static int ext4_breadi(struct ext2_inode *ip, long blkno, long nblks, char *buffer)
{
    struct ext4_extent_header *hdr;
    struct ext4_extent *ext_base;
    int entries, maxentries;
    long cur_blk = blkno;     /* logical block we are at */
    long blocks_left = nblks; /* blocks still to read */
    char *bufp = buffer;
    long tot_bytes = 0;

    hdr = (struct ext4_extent_header *)&ip->i_block[0];

    if (hdr->eh_magic != EXT4_EXT_MAGIC) {
        printf("ext4_breadi: Extent header magic wrong.\n");
        return -1;
    }

    /* For now we only handle leaf extents stored inline in the inode. */
    if (hdr->eh_depth != 0) {
        printf("ext4_breadi: Extent tree depth %d not supported.\n",
               hdr->eh_depth);
        return -1;
    }

    /*
     * eh_entries comes off the disk.  The inline extent area is
     * i_block[] minus the header, so anything larger than that walks out
     * of the inode and into whatever the inode table holds next.
     */
    maxentries = (sizeof(ip->i_block) - sizeof(*hdr)) / sizeof(struct ext4_extent);
    if (hdr->eh_max && hdr->eh_max < maxentries)
        maxentries = hdr->eh_max;

    entries  = hdr->eh_entries;
    if (entries <= 0) {
        printf("ext4_breadi: No extents.\n");
        return -1;
    }
    if (entries > maxentries) {
        printf("ext4_breadi: %d extents exceeds the %d that fit inline.\n",
               entries, maxentries);
        return -1;
    }

    /* --- honour file size like ext2_breadi() does --- */
    if ((blkno + nblks) * ext2fs.blocksize > ip->i_size) {
        long maxblk = (ip->i_size + ext2fs.blocksize - 1) / ext2fs.blocksize;
        nblks = maxblk - blkno;
        if (nblks <= 0)
            return 0;  /* nothing to read */
    }
    cur_blk     = blkno;
    blocks_left = nblks;


    /* Extents start right after the header in i_block[] */
    ext_base = (struct ext4_extent *)((char *)hdr + sizeof(*hdr));

    while (blocks_left > 0) {
        struct ext4_extent *ext = NULL;
        int i;

        /* Find the extent that covers cur_blk */
        for (i = 0; i < entries; i++) {
            struct ext4_extent *e = &ext_base[i];
            uint32_t first = e->ee_block;
            uint32_t len   = e->ee_len;

            /*
             * ee_len above EXT_INIT_MAX_LEN marks an uninitialized
             * extent: the blocks are allocated but hold no data yet,
             * and the real length is ee_len - EXT_INIT_MAX_LEN.  Such
             * an extent still covers its share of the logical block
             * space, so take its real length into account here and
             * refuse it only if the block we want falls inside it.
             */
            if (len > EXT_INIT_MAX_LEN)
                len -= EXT_INIT_MAX_LEN;

            if (len == 0)
                continue;

            if ((uint32_t)cur_blk >= first &&
                (uint32_t)cur_blk < first + len) {
                if (e->ee_len > EXT_INIT_MAX_LEN) {
                    printf("ext4_breadi: uninitialized extent at"
                           " block %u.\n", first);
                    return -1;
                }
                ext = e;
                break;
            }
        }

        if (!ext) {
            printf("ext4_breadi: logical block %ld not in any extent.\n",
                   cur_blk);
            return -1;
        }

        {
            uint32_t first = ext->ee_block;
            uint32_t len   = ext->ee_len;
            long within    = cur_blk - first;   /* offset inside this extent */
            long can_read  = len - within;      /* blocks available here */

            if (can_read > blocks_left)
                can_read = blocks_left;

            /* Calculate physical start block of this chunk */
            long ee_start =
                ((long)ext->ee_start_hi << 32) + ext->ee_start_lo;
            long phys_blk = ee_start + within;

            long offset = partition_offset +
                phys_blk * ext2fs.blocksize;
            long nbytes = can_read * ext2fs.blocksize;
            long got;

            got = cons_read(dev, bufp, nbytes, offset);
            if (got != nbytes) {
                printf("ext4_breadi: cons_read failed (wanted %ld, got %ld).\n",
                       nbytes, got);
                return -1;
            }

            bufp       += nbytes;
            tot_bytes  += nbytes;
            cur_blk    += can_read;
            blocks_left -= can_read;
        }
    }

    return tot_bytes;
}

static int ext2_breadi(struct ext2_inode *ip, long blkno, long nblks, char *buffer)
{
	long dev_blkno, ncontig, offset, nbytes, tot_bytes;

	if (ip->i_flags & EXT4_EXTENTS_FL) {
		printf("ext2_breadi: This function does not handle ext4 extents\n");
		return -1;
	}

	tot_bytes = 0;
	if ((blkno+nblks)*ext2fs.blocksize > ip->i_size)
		nblks = (ip->i_size + ext2fs.blocksize - 1) / ext2fs.blocksize
			- blkno;

	while (nblks) {
		/*
		 * Contiguous reads are a lot faster, so we try to group
		 * as many blocks as possible:
		 */
		ncontig = 0; nbytes = 0;
		dev_blkno = ext2_blkno(ip, blkno);
		do {
			++blkno; ++ncontig; --nblks;
			nbytes += ext2fs.blocksize;
		} while (nblks &&
			 ext2_blkno(ip, blkno) == dev_blkno + ncontig);

		if (dev_blkno == 0) {
			/* This is a "hole" */
			memset(buffer, 0, nbytes);
		} else {
			/* Read it for real */
			offset = partition_offset + (long) dev_blkno* (long) ext2fs.blocksize;
#ifdef DEBUG_EXT2
			printf("ext2_bread: reading %ld bytes at offset %ld\n",
			       nbytes, offset);
#endif
			if (cons_read(dev, buffer, nbytes, offset)
			    != nbytes)
			{
				printf("ext2_bread: read error\n");
				return -1;
			}
		}
		buffer    += nbytes;
		tot_bytes += nbytes;
	}
	return tot_bytes;
}

static int extn_breadi(struct ext2_inode *ip, long blkno, long nblks, char *buffer) {
	if (ip->i_flags & EXT4_EXTENTS_FL) {
		return ext4_breadi(ip, blkno, nblks, buffer);
	} else {
		return ext2_breadi(ip, blkno, nblks, buffer);
	}
}

static struct ext2_dir_entry_2 *ext2_readdiri(struct ext2_inode *dir_inode,
					      int rewind)
{
	struct ext2_dir_entry_2 *dp;
	static int diroffset = 0, blockoffset = 0;

	/* Reading a different directory, invalidate previous state */
	if (rewind) {
		diroffset = 0;
		blockoffset = 0;
		/* read first block */
		if (extn_breadi(dir_inode, 0, 1, blkbuf) < 0)
			return NULL;
	}

#ifdef DEBUG_EXT2
	printf("ext2_readdiri: blkoffset %d diroffset %d len %d\n",
		blockoffset, diroffset, dir_inode->i_size);
#endif
	if (blockoffset >= ext2fs.blocksize) {
		diroffset += ext2fs.blocksize;
		if (diroffset >= dir_inode->i_size)
			return NULL;
#ifdef DEBUG_EXT2
		printf("ext2_readdiri: reading block at %d\n",
			diroffset);
#endif
		/* assume that this will read the whole block */
		if (extn_breadi(dir_inode,
				diroffset / ext2fs.blocksize,
				1, blkbuf) < 0)
			return NULL;
		blockoffset = 0;
	}

	dp = (struct ext2_dir_entry_2 *) (blkbuf + blockoffset);
	blockoffset += dp->rec_len;
#ifdef DEBUG_EXT2
	printf("ext2_readdiri: returning %p = %.*s\n", dp, dp->name_len, dp->name);
#endif
	return dp;
}

static struct ext2_inode *ext2_namei(const char *name)
{
	char namebuf[256];
	char *component;
	struct ext2_inode *dir_inode;
	struct ext2_dir_entry_2 *dp;
	int next_ino;

	/* squirrel away a copy of "namebuf" that we can modify: */
	strcpy(namebuf, name);

	/* start at the root: */
	if (!root_inode)
		root_inode = ext2_iget(EXT2_ROOT_INO);
	dir_inode = root_inode;
	if (!dir_inode)
	  return NULL;

	component = strtok(namebuf, "/");
	while (component) {
		int component_length;
		int rewind = 0;
		/*
		 * Search for the specified component in the current
		 * directory inode.
		 */
		next_ino = -1;
		component_length = strlen(component);

		/* rewind the first time through */
		while ((dp = ext2_readdiri(dir_inode, !rewind++))) {
			if ((dp->name_len == component_length) &&
			    (strncmp(component, dp->name,
				     component_length) == 0))
			{
				/* Found it! */
#ifdef DEBUG_EXT2
				printf("ext2_namei: found entry %s\n",
					component);
#endif
				next_ino = dp->inode;
				break;
			}
#ifdef DEBUG_EXT2
			printf("ext2_namei: looping\n");
#endif
		}

#ifdef DEBUG_EXT2
		printf("ext2_namei: next_ino = %d\n", next_ino);
#endif

		/*
		 * At this point, we're done with this directory whether
		 * we've succeeded or failed...
		 */
		if (dir_inode != root_inode)
			ext2_iput(dir_inode);

		/*
		 * If next_ino is negative, then we've failed (gone
		 * all the way through without finding anything)
		 */
		if (next_ino < 0) {
			return NULL;
		}

		/*
		 * Otherwise, we can get this inode and find the next
		 * component string...
		 */
		dir_inode = ext2_iget(next_ino);
		if (!dir_inode)
		  return NULL;

		component = strtok(NULL, "/");
	}

	/*
	 * If we get here, then we got through all the components.
	 * Whatever we got must match up with the last one.
	 */
	return dir_inode;
}


/*
 * Read block number "blkno" from the specified file.
 */
static int ext2_bread(int fd, long blkno, long nblks, char *buffer)
{
	struct ext2_inode * ip;
	ip = &inode_table[fd].inode;

	return extn_breadi(ip, blkno, nblks, buffer);
}

/*
 * Note: don't mix any kind of file lookup or other I/O with this or
 * you will lose horribly (as it reuses blkbuf)
 */
static const char * ext2_readdir(int fd, int rewind)
{
	struct ext2_inode * ip = &inode_table[fd].inode;
	struct ext2_dir_entry_2 * ent;
	if (!S_ISDIR(ip->i_mode)) {
		printf("fd %d (inode %d) is not a directory (mode %x)\n",
		       fd, inode_table[fd].inumber, ip->i_mode);
		return NULL;
	}
	ent = ext2_readdiri(ip, rewind);
	if (ent) {
		ent->name[ent->name_len] = '\0';
		return ent->name;
	} else {
		return NULL;
	}
}

static int ext2_fstat(int fd, struct stat* buf)
{
	struct ext2_inode * ip = &inode_table[fd].inode;

	if (fd >= MAX_OPEN_FILES)
		return -1;
	memset(buf, 0, sizeof(struct stat));
	/* fill in relevant fields */
	buf->st_ino = inode_table[fd].inumber;
	buf->st_mode = ip->i_mode;
	buf->st_nlink = ip->i_links_count;
	buf->st_uid = ip->i_uid;
	buf->st_gid = ip->i_gid;
	buf->st_size = ip->i_size;
	buf->st_blocks = ip->i_blocks;
	buf->st_atime = ip->i_atime;
	buf->st_mtime = ip->i_mtime;
	buf->st_ctime = ip->i_ctime;

	return 0; /* NOTHING CAN GO WROGN! */
}

static struct ext2_inode * ext2_follow_link(struct ext2_inode * from,
					    const char * base)
{
	char *linkto;

	if (from->i_blocks) {
		linkto = blkbuf;
		if (extn_breadi(from, 0, 1, blkbuf) == -1)
			return NULL;
#ifdef DEBUG_EXT2
		printf("long link!\n");
#endif
	} else {
		linkto = (char*)from->i_block;
	}
#ifdef DEBUG_EXT2
	printf("symlink to %s\n", linkto);
#endif

	/* Resolve relative links */
	if (linkto[0] != '/') {
		const char *end = strrchr(base, '/');
		if (end) {
			char fullname[(end - base + 1) + strlen(linkto) + 1];
			strncpy(fullname, base, end - base + 1);
			fullname[end - base + 1] = '\0';
			strcat(fullname, linkto);
#ifdef DEBUG_EXT2
			printf("resolved to %s\n", fullname);
#endif
			return ext2_namei(fullname);
		} else {
			/* Assume it's in the root */
			return ext2_namei(linkto);
		}
	} else {
		return ext2_namei(linkto);
	}
}

static int ext2_open(const char *filename)
{
	/*
	 * Unix-like open routine.  Returns a small integer (actually
	 * an index into the inode table...
	 */
	struct ext2_inode * ip;

	ip = ext2_namei(filename);
	if (ip) {
		struct inode_table_entry *itp;

		while (S_ISLNK(ip->i_mode)) {
			ip = ext2_follow_link(ip, filename);
			if (!ip) return -1;
		}
		itp = (struct inode_table_entry *)ip;
		return itp - inode_table;
	} else
		return -1;
}


static void ext2_close(int fd)
{
	/* blah, hack, don't close the root inode ever */
	if (&inode_table[fd].inode != root_inode)
		ext2_iput(&inode_table[fd].inode);
}

struct bootfs ext2fs = {
	.fs_type = FS_EXT2,
	.blocksize = 0,

	.mount   = ext2_mount,
	.open    = ext2_open,
	.bread   = ext2_bread,
	.close   = ext2_close,
	.readdir = ext2_readdir,
	.fstat   = ext2_fstat,
};
