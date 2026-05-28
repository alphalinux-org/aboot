/* This is a library of functions that allow user-level programs to
 * read and manipulate ext2 file systems.  For convenience sake,
 * this library maintains a lot of state information in static
 * variables; therefore,  it's not reentrant.  We don't care for
 * our applications 8-)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <bio.h>
#include <e2lib.h>
#include <ext2fs/ext2_fs.h>


#define		MAX_OPEN_FILES		8

int				fd = -1;
struct ext2_super_block		sb;
struct ext2_group_desc		*gds;
int				ngroups = 0;
int				blocksize;	/* Block size of this fs */
int				directlim;	/* Maximum direct blkno */
int				ind1lim;	/* Maximum single-indir blkno */
int				ind2lim;	/* Maximum double-indir blkno */
int				ptrs_per_blk;	/* ptrs/indirect block */
char				filename[256];
int				readonly;	/* Is this FS read-only? */
int				verbose = 0;

static void	ext2_ifree(int ino);
static void	ext2_free_indirect(int indirect_blkno, int level);


struct inode_table_entry {
	struct	ext2_inode	inode;
	int			inumber;
	int			free;
	unsigned short		old_mode;
} inode_table[MAX_OPEN_FILES];

/* Initialize an ext2 filesystem; this is sort-of the same idea as
 * "mounting" it.  Read in the relevant control structures and
 * make them available to the user.  Returns 0 if successful, -1 on
 * failure.
 */
int
ext2_init (char * name, int access)
{
    int		i;

    /* Initialize the inode table */
    for(i = 0; i < MAX_OPEN_FILES; i++) {
	inode_table[i].free = 1;
	inode_table[i].inumber = 0;
    }

    if((access != O_RDONLY) && (access != O_RDWR)) {
	fprintf(stderr,
		"ext2_init: Access must be O_RDONLY or O_RDWR, not %d\n",
		access);
	return(-1);
    }

    /* Open the device/file */
    fd = open(name, access);
    if(fd < 0) {
	perror(filename);
	return(-1);
    }

    if(access == O_RDONLY) {
	readonly = 1;
    }

    /* Read in the first superblock */
    lseek(fd, EXT2_MIN_BLOCK_SIZE, SEEK_SET);
    if(read(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("ext2 sb read");
	close(fd);
	return(-1);
    }

    if(sb.s_magic != EXT2_SUPER_MAGIC) {
	fprintf(stderr, "ext2 bad magic 0x%x\n", sb.s_magic);
	close(fd);
	return(-1);
    }

    if(sb.s_first_data_block != 1) {
	fprintf(stderr,
	    "Brain-damaged utils can't deal with a filesystem\nwhere s_first_data_block != 1.\nRe-initialize the filesystem\n");
	close(fd);
	return(-1);
    }

    ngroups = (sb.s_blocks_count+sb.s_blocks_per_group-1)/sb.s_blocks_per_group;
    gds = malloc((size_t)(ngroups * sizeof(struct ext2_group_desc)));

    /* Read in the group descriptors (immediately follows superblock) */
    if ((size_t) read(fd, gds, ngroups * sizeof(struct ext2_group_desc))
	!= (ngroups * sizeof(struct ext2_group_desc)))
    {
	perror("ext2_init: group desc read error");
	return(-1);
    }

    strcpy(filename, name);

    /* Calculate direct/indirect block limits for this file system
     * (blocksize dependent)
     */
    blocksize = EXT2_BLOCK_SIZE(&sb);
    directlim = EXT2_NDIR_BLOCKS - 1;
    ptrs_per_blk = blocksize/sizeof(unsigned int);
    ind1lim = ptrs_per_blk + directlim;
    ind2lim = (ptrs_per_blk * ptrs_per_blk) + directlim;

    if(getenv("EXT2_VERBOSE")) {
	verbose = 1;
    }

    binit(fd, blocksize);

    if(verbose) {
	printf("Initialized filesystem %s\n", filename);
	printf("  %d blocks (%dKb), %d free (%dKb)\n",
		sb.s_blocks_count, (sb.s_blocks_count * blocksize)/1024,
		sb.s_free_blocks_count,
		(sb.s_free_blocks_count * blocksize)/1024);
	printf("  %d inodes,  %d free\n",
		sb.s_inodes_count, sb.s_free_inodes_count);
	printf("  %d groups, %d blocks/group\n",
			ngroups, sb.s_blocks_per_group);
    }

    return(0);
}

int
ext2_blocksize (void)
{
    return blocksize;
}

/* Call this when we're all done with the file system.  This will write
 * back any superblock and group changes to the file system.
 */
void
ext2_close (void)
{
    int		i;
    int		errors = 0;
    int		blocks_per_group = sb.s_blocks_per_group;

    if(!readonly) {
	for(i = 0; i < ngroups; i++) {
	    lseek(fd, ((i*blocks_per_group)+1)*blocksize, SEEK_SET);
	    if(write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
		perror("sb write");
		errors = 1;
	    }

	    if ((size_t) write(fd, gds, ngroups*sizeof(struct ext2_group_desc))
		!= ngroups*sizeof(struct ext2_group_desc))
	    {
		perror("gds write");
		errors = 1;
	    }

	    bflush();
	}
    }

    close(fd);
    if(errors) {
	fprintf(stderr, "Errors encountered while updating %s\n", filename);
	fprintf(stderr, "e2fsck is STRONGLY recommended!\n");
    }
}



/* Read the specified inode from the disk and return it to the user.
 * Returns NULL if the inode can't be read...
 */
struct ext2_inode *
ext2_iget (int ino)
{
    int				i;
    struct ext2_inode *		ip = NULL;
    struct inode_table_entry *	itp = NULL;
    int				group;
    int				blkoffset;
    int				byteoffset;
    char                        inobuf[EXT2_MAX_BLOCK_SIZE];

    for(i = 0; i < MAX_OPEN_FILES; i++) {
	if(inode_table[i].free) {
	    itp = &(inode_table[i]);
	    ip = &(itp->inode);
	    break;
	}
    }
    if(!ip) {
	fprintf(stderr, "ext2_iget: no free inodes\n");
	return(NULL);
    }

    group = ino / sb.s_inodes_per_group;
    blkoffset = (gds[group].bg_inode_table * blocksize);
    byteoffset = ((ino-1) % sb.s_inodes_per_group) * sizeof(struct ext2_inode);
    blkoffset += ((byteoffset / blocksize) * blocksize);
    byteoffset = (byteoffset % blocksize);
    bread(blkoffset/blocksize, inobuf);

    memcpy(ip, &(inobuf[byteoffset]), sizeof(struct ext2_inode));

    /* Yes, this is ugly, but it makes iput SOOO much easier 8-) */
    itp->free = 0;
    itp->inumber = ino;
    itp->old_mode = ip->i_mode;

    return(ip);
}

/* Put the specified inode back on the disk where it came from. */
void
ext2_iput (struct ext2_inode *ip)
{
    int				group;
    int				blkoffset;
    int				byteoffset;
    int				ino;
    struct inode_table_entry 	*itp;
    char                        inobuf[EXT2_MAX_BLOCK_SIZE];
    int				inode_mode;

    itp = (struct inode_table_entry *)ip;
    ino = itp->inumber;

    if(ip->i_links_count == 0) {
	ext2_ifree(itp->inumber);
    }

    itp->inumber = 0;

    if(!readonly) {
	group = ino / sb.s_inodes_per_group;
	blkoffset = (gds[group].bg_inode_table * blocksize);
	byteoffset = ((ino-1) % sb.s_inodes_per_group) * sizeof(struct ext2_inode);
	blkoffset += (byteoffset / blocksize) * blocksize;
	byteoffset = byteoffset % blocksize;

	inode_mode = ip->i_mode;
	bread(blkoffset/blocksize, inobuf);
	memcpy(&(inobuf[byteoffset]), ip, sizeof(struct ext2_inode));
	bwrite(blkoffset/blocksize, inobuf);

	if(S_ISDIR(itp->old_mode) && !S_ISDIR(inode_mode)) {
	    /* We deleted a directory */
	    gds[group].bg_used_dirs_count--;
	}
	if(!S_ISDIR(itp->old_mode) && S_ISDIR(inode_mode)) {
	    /* We created a directory */
	    gds[group].bg_used_dirs_count++;
	}
    }

    itp->free = 1;
}

#define BITS_PER_LONG	(8*sizeof(int))

static int
find_first_zero_bit (unsigned int * addr, unsigned size)
{
	unsigned lwsize;
        unsigned int        *ap = (unsigned int *)addr;
        unsigned int        mask;
        unsigned int        longword, bit;
	unsigned int	    lwval;

	if (!size)
		return 0;

	/* Convert "size" to a whole number of longwords... */
	lwsize = (size + BITS_PER_LONG - 1) >> 5;
	for (longword = 0; longword < lwsize; longword++, ap++) {
	    if(*ap != 0xffffffff) {
		lwval = *ap;

		for (bit = 0, mask = 1; bit < BITS_PER_LONG; bit++, mask <<= 1)
		{
		    if ((lwval & mask) == 0) {
			return (longword*BITS_PER_LONG) + bit;
		    }
		}
	    }
	}
	return size;

}

static void
set_bit (unsigned int *addr, int bitno)
{
    addr[bitno / BITS_PER_LONG] |= (1 << (bitno % BITS_PER_LONG));
}

static void
clear_bit (unsigned int *addr, int bitno)
{
    addr[bitno / BITS_PER_LONG] &=
	~((unsigned int)(1 << (bitno % BITS_PER_LONG)));
}


/* Allocate a block from the file system.  Brain-damaged implementation;
 * doesn't even TRY to do load-balancing among groups... just grabs the
 * first block it can find...
 */
int
ext2_balloc (void)
{
    unsigned int blk, blockmap[256];
    int i;

    if(readonly) {
	fprintf(stderr, "ext2_balloc: readonly filesystem\n");
	return(0);
    }

    for(i = 0; i < ngroups; i++) {
	if(gds[i].bg_free_blocks_count > 0) {
	    bread(gds[i].bg_block_bitmap, blockmap);
	    blk = find_first_zero_bit(blockmap, sb.s_blocks_per_group);
	    if (blk == 0 || blk == sb.s_blocks_per_group) {
		fprintf(stderr,
			"group %d has %d blocks free but none in bitmap?\n",
			i, gds[i].bg_free_blocks_count);
		continue;
	    }
	    set_bit(blockmap, blk);
	    bwrite(gds[i].bg_block_bitmap, blockmap);
	    gds[i].bg_free_blocks_count--;
	    sb.s_free_blocks_count--;
	    blk = blk + (i*sb.s_blocks_per_group)+1;

	    if(blk == 0) {
		fprintf(stderr, "ext2_balloc: blk == 0?\n");
	    }
	    return(blk);
	}
    }

    if(verbose) {
	printf("ext2_balloc: can't find a free block\n");
    }
    return(0);
}

/* Deallocate a block */
void
ext2_bfree (int blk)
{
    int		i;
    unsigned int	blockmap[256];

    /* Find which group this block is in */
    i = (blk-1) / sb.s_blocks_per_group;

    /* Read the block map */
    bread(gds[i].bg_block_bitmap, blockmap);

    /* Clear the appropriate bit */
    clear_bit(blockmap, (blk-1) % sb.s_blocks_per_group);

    /* Write the block map back out */
    bwrite(gds[i].bg_block_bitmap, blockmap);

    /* Update free block counts. */
    gds[i].bg_free_blocks_count++;
    sb.s_free_blocks_count++;

}

/* Allocate a contiguous range of blocks.  This is used ONLY for
 * initializing the bootstrapper.  It uses a simple-minded algorithm
 * that works best on a clean or nearly clean file system...  we
 * chunk through the bitmap a longword at a time.  Only if the whole
 * longword indicates free blocks do we use it.  On a 32-bit system,
 * this means we allocate blocks only in units of 32.
 */
int
ext2_contiguous_balloc (int nblocks)
{
    int		i, j;
    int		firstlong, lastlong;
    int		longs_needed;
    int		longs_per_group;
    int		blk;
    unsigned int	blockmap[256];

    if(readonly) {
	fprintf(stderr, "ext2_contiguous_balloc: readonly filesystem\n");
	return(0);
    }

    /* Compute how many longwords we need to fulfill this request */
    longs_needed = (nblocks + BITS_PER_LONG - 1) / BITS_PER_LONG;
    longs_per_group = sb.s_blocks_per_group/BITS_PER_LONG;

    for(i = 0; i < ngroups; i++) {
	/* Don't even bother if this group doesn't have enough blocks! */
	if(gds[i].bg_free_blocks_count >= nblocks) {

	    /* Get the block map. */
	    bread(gds[i].bg_block_bitmap, blockmap);

	    /* Find a run of blocks */
	    firstlong = 0;

	    do {
	        for(; firstlong < longs_per_group; firstlong++) {
		    if(blockmap[firstlong] == 0) break;
		}

	        if(firstlong == longs_per_group) {
		    /* No such thing in this group; try another! */
		    break;
	        }

	        for(lastlong = firstlong; lastlong < longs_per_group;
							lastlong++) {
		    if(blockmap[lastlong] != 0) break;
	        }

		if((lastlong-firstlong) < longs_needed) {
		    firstlong = lastlong;
		}
	    } while((lastlong-firstlong) < longs_needed);

	    /* If we got all the way through the block map,
	     * try another group.
	     */
	    if(firstlong == longs_per_group) {
		continue;
	    }

	    /* If we get here, then we know that we have a run
	     * that will fit our allocation.  Allocate the *actual*
	     * blocks that we need!
	     */
	    blk = firstlong * BITS_PER_LONG;
	    for(j = 0; j < nblocks; j++) {
		set_bit(blockmap, blk+j);
	    }

	    bwrite(gds[i].bg_block_bitmap, blockmap);
	    gds[i].bg_free_blocks_count -= nblocks;
	    sb.s_free_blocks_count -= nblocks;
	    blk = blk + (i*sb.s_blocks_per_group)+1;

	    if(verbose) {
		printf("ext2_contiguous_balloc: allocated %d blks @%d\n",
			nblocks, blk);
	    }
	    return(blk);
	}
    }

    if(verbose) {
	printf("ext2_contiguous_balloc: can't find %d contiguous free blocks\n", nblocks);
    }
    return(0);
}


/* Pre-allocate contiguous blocks to the specified inode.  Note that the
 * DATA blocks must be contiguous; indirect blocks can come from anywhere.
 * This is for the benefit of the bootstrap loader.
 * If successful, this routine returns the block number of the first
 * data block of the file.  Otherwise, it returns -1.
 */
int
ext2_fill_contiguous (struct ext2_inode * ip, int nblocks)
{
    int		iblkno = 0;
    int		firstblock;
    int		i;
    unsigned int *lp = NULL;
    char	blkbuf[EXT2_MAX_BLOCK_SIZE];

    /* For simplicity's sake, we only allow single indirection
     * here.  We shouldn't need more than this anyway!
     */
    if(nblocks > ind1lim) {
	fprintf(stderr,
	  "ext2_fill_contiguous: file size too big (%d); cannot exceed %d\n",
	  nblocks, ind1lim);
	return(-1);
    }

    /* First, try to allocate the data blocks */
    firstblock = ext2_contiguous_balloc(nblocks);

    if(firstblock == 0) {
	fprintf(stderr,
          "ext2_fill_contiguous: Cannot allocate %d contiguous blocks\n", nblocks);
	return(-1);
    }

    ip->i_blocks = nblocks * (blocksize/512);

    /* If we need the indirect block, then allocate it now. */
    if(nblocks > directlim) {
        iblkno = ext2_balloc();
	if(iblkno == 0) {
	    /* Should rarely happen! */
	    fprintf(stderr,
		"ext2_fill_contiguous: cannot allocate indirect block\n");
	    for(i = 0; i < nblocks; i++) {
		ext2_bfree(i);
	    }
	    return(-1);
	}
	ip->i_blocks += (blocksize/512);
        /* Point to indirect block buffer, in case we need it! */
        lp = (unsigned int *)blkbuf;

	for(i = 0; i < ptrs_per_blk; i++) {
	    lp[i] = 0;
	}

	ip->i_block[EXT2_IND_BLOCK] = iblkno;
    }

    /* All set... let's roll! */
    ip->i_size = nblocks * blocksize;

    for(i = 0; i < nblocks; i++) {
	if(i < EXT2_NDIR_BLOCKS) {
	    ip->i_block[i] = firstblock+i;
	}
	else {
	    *lp++ = firstblock+i;
	}
    }

    /* Write back the indirect block if necessary... */
    if(iblkno) {
	bwrite(iblkno, blkbuf);
    }

    return(firstblock);
}

/* Write out a boot block for this file system.  The caller
 * should have instantiated the block.
 */
void
ext2_write_bootblock (char *bb)
{
    bwrite(0, bb);
}


/* Allocate an inode from the file system.  Brain-damaged implementation;
 * doesn't even TRY to do load-balancing among groups... just grabs the
 * first inode it can find...
 */
int
ext2_ialloc (void)
{
    unsigned int inodemap[256];
    int i, ino;

    if(readonly) {
	return(0);
    }
    for(i = 0; i < ngroups; i++) {
	if(gds[i].bg_free_inodes_count > 4) {
	    /* leave a few inodes in each group for slop... */
	    bread(gds[i].bg_inode_bitmap, inodemap);

	    ino = find_first_zero_bit(inodemap, sb.s_inodes_per_group);
	    if (ino == 0 || (unsigned) ino == sb.s_inodes_per_group) {
		fprintf(stderr,
			"group %d has %d inodes free but none in bitmap?\n",
			i, gds[i].bg_free_inodes_count);
		continue;
	    }
	    set_bit(inodemap, ino);
	    bwrite(gds[i].bg_inode_bitmap, inodemap);
	    gds[i].bg_free_inodes_count--;
	    sb.s_free_inodes_count--;
	    ino = ino + (i*sb.s_inodes_per_group) + 1;
	    return ino;
	}
    }
    return 0;
}

/* Deallocate an inode */
static void
ext2_ifree (int ino)
{
    int		i;
    unsigned int	inodemap[256];

    /* Find which group this inode is in */
    i = (ino-1) / sb.s_inodes_per_group;

    /* Read the inode map */
    bread(gds[i].bg_inode_bitmap, inodemap);

    /* Clear the appropriate bit */
    clear_bit(inodemap, (ino-1) % sb.s_inodes_per_group);

    /* Write the inode map back out */
    bwrite(gds[i].bg_inode_bitmap, inodemap);

    /* Update free inode counts. */
    gds[i].bg_free_inodes_count++;
    sb.s_free_inodes_count++;
}

/* Map a block offset into a file into an absolute block number.
 * (traverse the indirect blocks if necessary).  Note: Double-indirect
 * blocks allow us to map over 64Mb on a 1k file system.  Therefore, for
 * our purposes, we will NOT bother with triple indirect blocks.
 *
 * The "allocate" argument is set if we want to *allocate* a block
 * and we don't already have one allocated.
 */
int
ext2_blkno (struct ext2_inode *ip, int blkoff, int allocate)
{
    unsigned int	*lp;
    int			blkno;
    int			iblkno;
    int			diblkno;
    char		blkbuf[EXT2_MAX_BLOCK_SIZE];

    if(allocate && readonly) {
	fprintf(stderr, "ext2_blkno: Cannot allocate on a readonly file system!\n");
	return(0);
    }

    lp = (unsigned int *)blkbuf;

    /* If it's a direct block, it's easy! */
    if(blkoff <= directlim) {
	if((ip->i_block[blkoff] == 0) && allocate) {
	    ip->i_block[blkoff] = ext2_balloc();
	    if(verbose) {
		printf("Allocated data block %d\n", ip->i_block[blkoff]);
	    }
	    ip->i_blocks += (blocksize / 512);
	}
	return(ip->i_block[blkoff]);
    }

    /* Is it a single-indirect? */
    if(blkoff <= ind1lim) {
	iblkno = ip->i_block[EXT2_IND_BLOCK];
	if((iblkno == 0) && allocate) {
	    /* No indirect block and we need one, so we allocate
	     * one, zero it, and write it out.
	     */
	    iblkno = ext2_balloc();
	    if(iblkno == 0) {
		return(0);
	    }
	    ip->i_block[EXT2_IND_BLOCK] = iblkno;
	    if(verbose) {
		printf("Allocated indirect block %d\n", iblkno);
	    }
	    ip->i_blocks += (blocksize / 512);
	    memset(blkbuf, 0, blocksize);
	    bwrite(iblkno, blkbuf);
	}

	if(iblkno == 0) {
	    return(0);
	}

	/* Read the indirect block */
	bread(iblkno, blkbuf);

	blkno = lp[blkoff-(directlim+1)];
	if((blkno == 0) && allocate) {
	    /* No block allocated but we need one. */
	    blkno = lp[blkoff-(directlim+1)] = ext2_balloc();
	    if(blkno == 0) {
		return(0);
	    }
	    ip->i_blocks += (blocksize / 512);
	    if(verbose) {
		printf("Allocated data block %d\n", blkno);
	    }
	    bwrite(iblkno, blkbuf);
	}
	return(blkno);
    }

    /* Is it a double-indirect? */
    if(blkoff <= ind2lim) {
	/* Find the double-indirect block */
	diblkno = ip->i_block[EXT2_DIND_BLOCK];
	if((diblkno == 0) && allocate) {
	    /* No double-indirect block and we need one.  Allocate one,
	     * fill it with zeros, and write it out.
	     */
	    diblkno = ext2_balloc();
	    if(diblkno == 0) {
		return(0);
	    }
	    ip->i_blocks += (blocksize / 512);
	    if(verbose) {
		printf("Allocated double-indirect block %d\n", diblkno);
	    }
	    memset(blkbuf, 0, blocksize);
	    bwrite(diblkno, blkbuf);
	    ip->i_block[EXT2_DIND_BLOCK] = diblkno;
	}

	if(diblkno == 0) {
	    return(0);
	}

	/* Read in the double-indirect block */
	bread(diblkno, blkbuf);

	/* Find the single-indirect block pointer ... */
	iblkno = lp[(blkoff - (ind1lim+1)) / ptrs_per_blk];

	if((iblkno == 0) && allocate) {
	    /* No indirect block and we need one, so we allocate
	     * one, zero it, and write it out.
	     */
	    iblkno = ext2_balloc();
	    if(iblkno == 0) {
		return(0);
	    }
	    ip->i_blocks += (blocksize / 512);
	    if(verbose) {
		printf("Allocated single-indirect block %d\n", iblkno);
	    }
	    lp[(blkoff-(ind1lim+1)) / ptrs_per_blk] = iblkno;
	    bwrite(diblkno, blkbuf);

	    memset(blkbuf, 0, blocksize);
	    bwrite(iblkno, blkbuf);
	}

	if(iblkno == 0) {
	    return(0);
	}


	/* Read the indirect block */
	bread(iblkno, blkbuf);

	/* Find the block itself. */
	blkno = lp[(blkoff-(ind1lim+1)) % ptrs_per_blk];
	if((blkno == 0) && allocate) {
	    /* No block allocated but we need one. */
	    blkno = lp[(blkoff-(ind1lim+1)) % ptrs_per_blk] = ext2_balloc();
	    ip->i_blocks += (blocksize / 512);
	    if(verbose) {
		printf("Allocated data block %d\n", blkno);
	    }
	    bwrite(iblkno, blkbuf);
	}
	return(blkno);
    }

    if(blkoff > ind2lim) {
	fprintf(stderr, "ext2_blkno: block number too large: %d\n", blkoff);
	return(0);
    }
    return 0;
}




/* Read block number "blkno" from the specified file */
void
ext2_bread (struct ext2_inode *ip, int blkno, char * buffer)
{
    int		dev_blkno;

    dev_blkno = ext2_blkno(ip, blkno, 0);
    if(dev_blkno == 0) {
	/* This is a "hole" */
	memset(buffer, 0, blocksize);
    }
    else {
	/* Read it for real */
	bread(dev_blkno, buffer);
    }
}

/* Write block number "blkno" to the specified file */
void
ext2_bwrite (struct ext2_inode *ip, int blkno, char * buffer)
{
    int		dev_blkno;

    if(readonly) {
	fprintf(stderr, "ext2_bwrite: Cannot write to a readonly filesystem!\n");
	return;
    }

    dev_blkno = ext2_blkno(ip, blkno, 1);
    if(dev_blkno == 0) {
	fprintf(stderr, "%s: No space on ext2 device\n", filename);
    }
    else {
	/* Write it for real */
	bwrite(dev_blkno, buffer);
    }
}

struct ext2_inode *
ext2_namei (char *name)
{
    char 	namebuf[256];
    char 	dirbuf[EXT2_MAX_BLOCK_SIZE];
    char *	component;
    struct ext2_inode *		dir_inode;
    struct ext2_dir_entry *dp;
    int		next_ino;

    /* Squirrel away a copy of "namebuf" that we can molest */
    strcpy(namebuf, name);

    /* Start at the root... */
    dir_inode = ext2_iget(EXT2_ROOT_INO);

    component = strtok(namebuf, "/");
    while(component) {
	unsigned diroffset;
	int component_length, blockoffset;

	/* Search for the specified component in the current directory
	 * inode.
	 */

	next_ino = -1;

	component_length = strlen(component);
	diroffset = 0;
	while (diroffset < dir_inode->i_size) {
	    blockoffset = 0;
	    ext2_bread(dir_inode, diroffset / blocksize, dirbuf);
	    while (blockoffset < blocksize) {
		int namelen;

	        dp = (struct ext2_dir_entry *)(dirbuf+blockoffset);
		namelen = dp->name_len;
		if((namelen == component_length) &&
		   (strncmp(component, dp->name, component_length) == 0)) {
			/* Found it! */
			next_ino = dp->inode;
			break;
		}
		/* Go to next entry in this block */
		blockoffset += dp->rec_len;
	    }
	    if(next_ino >= 0) {
		break;
	    }

	    /* If we got here, then we didn't find the component.
	     * Try the next block in this directory...
	     */
	    diroffset += blocksize;
	}

	/* At this point, we're done with this directory whether
	 * we've succeeded or failed...
	 */
	ext2_iput(dir_inode);

	/* If next_ino is negative, then we've failed (gone all the
	 * way through without finding anything)
	 */
	if(next_ino < 0) {
	    return(NULL);
	}

	/* Otherwise, we can get this inode and find the next
	 * component string...
	 */
	dir_inode = ext2_iget(next_ino);

	component = strtok(NULL, "/");
    }

    /* If we get here, then we got through all the components.
     * Whatever we got must match up with the last one.
     */
    return(dir_inode);
}

/* Create a new entry in the specified directory with the specified
 * name/inumber pair.  This routine ASSUMES that the specified
 * entry does not already exist!  Therefore, we MUST use namei
 * first to try and find the entry...
 */

void
ext2_mknod (struct ext2_inode *dip, char * name, int ino)
{
    unsigned diroffset;
    int blockoffset, namelen, new_reclen;
    struct ext2_dir_entry *dp;
    struct ext2_dir_entry *entry_dp;
    char dirbuf[EXT2_MAX_BLOCK_SIZE];
    int dp_inode, dp_reclen, dp_namelen;

    namelen = strlen(name);

    /* Look for an empty directory entry that can hold this
     * item.
     */
    diroffset = 0;
    entry_dp = NULL;
    while (diroffset < dip->i_size) {
	blockoffset = 0;
	ext2_bread(dip, diroffset / blocksize, dirbuf);
	while(blockoffset < blocksize) {

	    dp = (struct ext2_dir_entry *)(dirbuf+blockoffset);
	    dp_inode = dp->inode;
	    dp_reclen = dp->rec_len;
	    dp_namelen = dp->name_len;

	    if((dp_inode == 0) && (dp_reclen >= EXT2_DIR_REC_LEN(namelen))) {
		/* Found an *empty* entry that can hold this name. */
		entry_dp = dp;
		break;
	    }

	    /* If this entry is in use, see if it has space at the end
	     * to hold the new entry anyway...
	     */
	    if((dp_inode != 0) &&
		((dp_reclen - EXT2_DIR_REC_LEN(dp_namelen))
					>= EXT2_DIR_REC_LEN(namelen))) {

		new_reclen = dp_reclen - EXT2_DIR_REC_LEN(dp_namelen);

		/* Chop the in-use entry down to size */
		dp_reclen = EXT2_DIR_REC_LEN(dp->name_len);
		dp->rec_len = dp_reclen;

		/* Point entry_dp to the end of this entry */
		entry_dp = (struct ext2_dir_entry *)((char*)dp + dp_reclen);

		/* Set the record length for this entry */
		entry_dp->rec_len = new_reclen;

		/* all set! */
		break;
	    }

	    /* No luck yet... go to next entry in this block */
	    blockoffset += dp_reclen;
	}
	if(entry_dp != NULL) {
	    break;
	}

	/* If we got here, then we didn't find the component.
	 * Try the next block in this directory...
	 */
	diroffset += blocksize;
    }

    /* By the time we get here, one of two things has happened:
     *
     *	If entry_dp is non-NULL, then entry_dp points to the
     *  place in dirbuf where the entry lives, and diroffset
     *  is the directory offset of the beginning of dirbuf.
     *
     *  If entry_dp is NULL, then we couldn't find an entry,
     *  so we need to add a block to the directory file for
     *  this entry...
     */
    if(entry_dp) {
	entry_dp->inode = ino;
	entry_dp->name_len = namelen;
	memcpy(entry_dp->name, name, namelen);
	ext2_bwrite(dip, diroffset/blocksize, dirbuf);
    }
    else {
	entry_dp = (struct ext2_dir_entry *)dirbuf;
	entry_dp->inode = ino;
	entry_dp->name_len = namelen;
	memcpy(entry_dp->name, name, namelen);
	entry_dp->rec_len = blocksize;
	ext2_bwrite(dip, dip->i_size/blocksize, dirbuf);
	dip->i_size += blocksize;
    }
}

void
ext2_truncate (struct ext2_inode *ip)
{
    int		i;

    /* Deallocate all blocks associated with a particular file
     * and set its size to zero.
     */

    /* Direct blocks */
    for(i = 0; i < EXT2_NDIR_BLOCKS; i++) {
	if(ip->i_block[i]) {
	    ext2_bfree(ip->i_block[i]);
	    ip->i_block[i] = 0;
	}
    }

    /* First-level indirect blocks */
    if(ip->i_block[EXT2_IND_BLOCK]) {
	ext2_free_indirect(ip->i_block[EXT2_IND_BLOCK], 0);
	ip->i_block[EXT2_IND_BLOCK] = 0;
    }

    /* Second-level indirect blocks */
    if(ip->i_block[EXT2_DIND_BLOCK]) {
	ext2_free_indirect(ip->i_block[EXT2_DIND_BLOCK], 1);
	ip->i_block[EXT2_DIND_BLOCK] = 0;
    }

    /* Third-level indirect blocks */
    if(ip->i_block[EXT2_TIND_BLOCK]) {
        ext2_free_indirect(ip->i_block[EXT2_TIND_BLOCK], 2);
        ip->i_block[EXT2_TIND_BLOCK] = 0;
    }

    ip->i_size = 0;
}

/* Recursive routine to free an indirect chain */
static void
ext2_free_indirect (int indirect_blkno, int level)
{
    int i, indirect_block[EXT2_MAX_BLOCK_SIZE/4];

    /* Read the specified indirect block */
    bread(indirect_blkno, indirect_block);

    for(i = 0; i < ptrs_per_blk; i++) {
	if(level == 0) {
	    /* These are pointers to data blocks; just free them up */
	    if(indirect_block[i]) {
	        ext2_bfree(indirect_block[i]);
	        indirect_block[i] = 0;
	    }
	}
	else {
	    /* These are pointers to *indirect* blocks.  Go down the chain */
	    if(indirect_block[i]) {
		ext2_free_indirect(indirect_block[i], level-1);
		indirect_block[i] = 0;
	    }
	}
    }
    ext2_bfree(indirect_blkno);
}
