/*
 *  xfs.c - an implementation of the SGI XFS file system for aboot
 *  Jan-Jaap van der Heijden <J.J.vanderHeijden@home.nl>
 *
 *  Based on fsys_xfs.c from GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2001,2002,2004  Free Software Foundation, Inc.
 */
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "config.h"
#include "aboot.h"
#include "bootfs.h"
#include "cons.h"
#include "disklabel.h"
#include "utils.h"
#include "xfs.h"

static int xfs_mount(long cons_dev, long p_offset, long quiet);
static int xfs_bread(int fd, long blkno, long nblks, char *buffer);
static int xfs_open(const char *dirname);
static void xfs_close(int fd);
static const char *xfs_readdir(int fd, int rewind);
static int xfs_fstat(int fd, struct stat* buf);
static char* first_dentry_dir3(xfs_ino_t *);
static char* first_dentry_dir2(xfs_ino_t *);
static char *next_dentry_dir3(xfs_ino_t *);
static char *next_dentry_dir2(xfs_ino_t *);
static char *next_dentry_local(xfs_ino_t *);
static xfs_ino_t xfs_dir2_sf_get_ino(xfs_dir2_sf_hdr_t *, xfs_dir2_inou_t *);


typedef struct {
    uint32_t inumber_lo;
    uint32_t inumber_hi;
    uint8_t  namelen;
    uint8_t  filetype;
    uint8_t  name[];     /* variable length */
} xfs_dir3_data_entry_t;

typedef union {
    xfs_dir3_data_entry_t entry;
    xfs_dir2_data_unused_t unused;
} xfs_dir3_data_union_t;

struct bootfs xfsfs = {
       FS_EXT2,
       0,
       xfs_mount,
       xfs_open,
       xfs_bread,
       xfs_close,
       xfs_readdir,
       xfs_fstat
};

typedef struct {
    uint8_t namelen;
    uint8_t offset;
    char name[1];   /* name bytes follow */
} xfs_dir3_sf_entry_t;

static long dev = -1;
static long partition_offset;
static long filepos;
static long filemax; /* filelen */

static long xfs_read (void *buf, long len);

/*
 * The path handed to xfs_open() ends at a space or a NUL; the boot
 * command line separates arguments with either a space or a tab.
 */
#define isspace(c) ((c) == ' ' || (c) == '\t')

/*
 * Replacement for old Linux __swabXX() helpers.
 * XFS metadata is big-endian on disk, Alpha CPU is little-endian.
 */
static inline uint16_t __swab16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t __swab32(uint32_t x)
{
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0xFF000000U) >> 24);
}

static inline uint64_t __swab64(uint64_t x)
{
    return ((uint64_t)__swab32((uint32_t)x) << 32) |
            __swab32((uint32_t)(x >> 32));
}


typedef struct xfs_dir2_leaf_entry {
    uint32_t hashval;   /* hash of name */
    uint32_t address;   /* block/offset of data entry */
} xfs_dir2_leaf_entry_t;

typedef struct xfs_dir3_block_tail {
    uint32_t bestcount; /* number of bestfree slots */
    uint32_t count;     /* number of leaf entries */
    uint32_t stale;     /* number of stale leaf entries */
} xfs_dir3_block_tail_t;


static int
devread(long sector, long start, long length, void *buf)
{
       long pos;

       /*
        * agb2daddr() returns -1 for a block pointer the superblock's own
        * geometry says cannot exist.  Every caller feeds that straight
        * here, so this is where it has to stop: turning it into an offset
        * would read from somewhere well before the partition.
        */
       if (sector < 0 || start < 0 || length < 0) {
               printf("xfs: refusing read of %ld bytes at sector %ld+%ld\n",
                      length, sector, start);
               return -1;
       }

       pos = sector * SECT_SIZE;
       pos += partition_offset + start;
#ifdef DEBUG_XFS2
       printf("Reading %ld bytes, starting at sector %ld, disk offset %ld\n",
               length, sector, pos);
#endif
       return cons_read(dev, buf, length, pos);
}

#define MAXNAMELEN     256
#define SECTOR_BITS    9
#define MAX_LINK_COUNT 8

typedef struct xad {
       xfs_fileoff_t offset;
       xfs_fsblock_t start;
       xfs_filblks_t len;
} xad_t;

struct xfs_info {
       int bsize;
       int dirbsize;
       int isize;
       unsigned int agblocks;
       unsigned int agcount;
       int bdlog;
       int blklog;
       int inopblog;
       int agblklog;
       int agnolog;
       unsigned int nextents;
       xfs_daddr_t next;
       xfs_daddr_t daddr;
       xfs_dablk_t forw;
       xfs_dablk_t dablk;
       xfs_bmbt_rec_32_t *xt;
       xfs_bmbt_ptr_t ptr0;
       int btnode_ptr0_off;
       int i8param;
       int dirpos;
       int dirmax;
       int blkoff;
       int fpos;
       xfs_ino_t rootino;
       xfs_ino_t new_ino;
       int is_v5;
       int has_ftype;
       int has_nrext64;
       int dir3_has_tail;
       char* data_fork;
};


static struct xfs_info xfs;

/* On-disk v1/v2/v3 inode layout, used only to compute fork offsets.
 * Matches current Linux struct xfs_dinode. Field order is important,
 * but we only *use* di_version and the size/alignment.
 */
typedef struct xfs_dinode_disk {
    uint16_t        di_magic;       /* XFS_DINODE_MAGIC */
    uint16_t        di_mode;
    uint8_t         di_version;     /* 1, 2 or 3 */
    uint8_t         di_format;
    uint16_t        di_onlink;
    uint32_t        di_uid;
    uint32_t        di_gid;
    uint32_t        di_nlink;
    uint16_t        di_projid_lo;
    uint16_t        di_projid_hi;
    uint8_t         di_pad[6];
    uint16_t        di_flushiter;
    xfs_timestamp_t di_atime;
    xfs_timestamp_t di_mtime;
    xfs_timestamp_t di_ctime;
    uint64_t        di_size;
    uint64_t        di_nblocks;
    uint32_t        di_extsize;
    uint32_t        di_nextents;
    uint16_t        di_anextents;
    uint8_t         di_forkoff;
    int8_t          di_aformat;
    uint32_t        di_dmevmask;
    uint16_t        di_dmstate;
    uint16_t        di_flags;
    uint32_t        di_gen;
    uint32_t        di_next_unlinked;

    /* v3 (v5 filesystem) extra fields follow */
    uint32_t        di_crc;
    uint64_t        di_changecount;
    uint64_t        di_lsn;
    uint64_t        di_flags2;
    uint8_t         di_pad2[16];
    xfs_timestamp_t di_crtime;
    uint64_t        di_ino;
    uint8_t         di_uuid[16];
} xfs_dinode_disk_t;



/* v5 (CRC-enabled) directory block header (48 bytes) */
typedef struct xfs_dir3_blk_hdr {
    uint32_t magic;     /* XFS_DIR3_*_MAGIC */
    uint32_t crc;
    uint64_t blkno;     /* first block of the buffer */
    uint64_t lsn;       /* sequence number of last write */
    uint8_t  uuid[16];  /* filesystem we belong to */
    uint64_t owner;     /* inode that owns the block */
} xfs_dir3_blk_hdr_t;

/*
 * Full v5 directory data header (64 bytes).
 * This is the v5 extension of xfs_dir2_data_hdr_t, so the
 * bestfree[] array must be present and magic must still be
 * at offset 0 (hdr.magic).
 */
typedef struct xfs_dir3_data_hdr {
    xfs_dir3_blk_hdr_t   hdr;                          /* 48 bytes */
    xfs_dir2_data_free_t bestfree[XFS_DIR2_DATA_FD_COUNT];
    uint32_t             pad;                          /* 64-byte alignment */
} xfs_dir3_data_hdr_t;

/*
 * Scratch space, carved into three regions.  The union gives it the
 * alignment of the widest type we lay over it; addressing it as char
 * keeps the pointer arithmetic below from depending on sizeof(long).
 */
#define FSYS_BUF_SIZE     32768
#define DIRBUF_OFFSET     0
#define FILEBUF_OFFSET    4096
#define INODE_OFFSET      8192

static union {
       long align;
       char b[FSYS_BUF_SIZE];
} FSYS_BUF;

#define DIRBUF_SIZE    (FILEBUF_OFFSET - DIRBUF_OFFSET)
#define FILEBUF_SIZE   (INODE_OFFSET - FILEBUF_OFFSET)
#define INODE_SIZE     (FSYS_BUF_SIZE - INODE_OFFSET)

#define dirbuf         (FSYS_BUF.b + DIRBUF_OFFSET)
#define filebuf        (FSYS_BUF.b + FILEBUF_OFFSET)
#define inode          ((xfs_dinode_t *)(FSYS_BUF.b + INODE_OFFSET))

#define icore          (inode->di_core)
#define        mask32lo(n)     (((uint32_t)1 << (n)) - 1)

#define        XFS_INO_MASK(k)         ((uint32_t)((1ULL << (k)) - 1))
#define        XFS_INO_OFFSET_BITS     xfs.inopblog
#define        XFS_INO_AGBNO_BITS      xfs.agblklog
#define        XFS_INO_AGINO_BITS      (xfs.agblklog + xfs.inopblog)
#define        XFS_INO_AGNO_BITS       xfs.agnolog


/* Size of the core+next_unlinked for v1/v2 vs full inode for v3. */
static inline int
xfs_dinode_size_disk(uint8_t version)
{
    if (version == 3)
        return sizeof(xfs_dinode_disk_t);
    /* v1/v2: data fork starts just before di_crc in the v3 layout */
    return offsetof(xfs_dinode_disk_t, di_crc);
}

/* Pointer to the *data fork* (shortform dir / extents / etc). */
static inline char *
xfs_dfork_dptr(void)
{
    xfs_dinode_disk_t *dip = (xfs_dinode_disk_t *)inode;
    uint8_t version = dip->di_version;   /* single byte, no swab needed */

    return ((char *)dip) + xfs_dinode_size_disk(version);
}

/* Pointer to shortform directory header. */
static inline xfs_dir2_sf_t *
xfs_sf_dir(void)
{
    return (xfs_dir2_sf_t *)xfs_dfork_dptr();
}

/* Does a shortform entry carry the file type byte? */
static inline int
xfs_sf_is_dir3(void)
{
    return xfs.has_ftype;
}


static inline xfs_agblock_t
agino2agbno (xfs_agino_t agino)
{
       return agino >> XFS_INO_OFFSET_BITS;
}

static inline xfs_agnumber_t
ino2agno (xfs_ino_t ino)
{
       return ino >> XFS_INO_AGINO_BITS;
}

static inline xfs_agino_t
ino2agino (xfs_ino_t ino)
{
       return ino & XFS_INO_MASK(XFS_INO_AGINO_BITS);
}

static inline int
ino2offset (xfs_ino_t ino)
{
       return ino & XFS_INO_MASK(XFS_INO_OFFSET_BITS);
}

/*
 * XFS metadata is big-endian on disk and Alpha is little-endian, so
 * every on-disk field has to be swapped on the way in.  aboot only ever
 * runs on Alpha; a big-endian port would make these no-ops.
 */
#define be16_to_cpu(x) __swab16(x)
#define be32_to_cpu(x) __swab32(x)
#define be64_to_cpu(x) __swab64(x)

static xfs_fsblock_t
xt_start (xfs_bmbt_rec_32_t *r)
{
       return (((xfs_fsblock_t)(be32_to_cpu(r->l1) & mask32lo(9))) << 43) | 
              (((xfs_fsblock_t)be32_to_cpu(r->l2)) << 11) |
              (((xfs_fsblock_t)be32_to_cpu(r->l3)) >> 21);
}

static xfs_fileoff_t
xt_offset (xfs_bmbt_rec_32_t *r)
{
       return (((xfs_fileoff_t)be32_to_cpu(r->l0) &
               mask32lo(31)) << 23) |
               (((xfs_fileoff_t)be32_to_cpu(r->l1)) >> 9);
}

static xfs_filblks_t
xt_len (xfs_bmbt_rec_32_t *r)
{
       return be32_to_cpu(r->l3) & mask32lo(21);
}

static const char xfs_highbit[256] = {
       -1, 0, 1, 1, 2, 2, 2, 2,                        /* 00 .. 07 */
       3, 3, 3, 3, 3, 3, 3, 3,                 /* 08 .. 0f */
       4, 4, 4, 4, 4, 4, 4, 4,                 /* 10 .. 17 */
       4, 4, 4, 4, 4, 4, 4, 4,                 /* 18 .. 1f */
       5, 5, 5, 5, 5, 5, 5, 5,                 /* 20 .. 27 */
       5, 5, 5, 5, 5, 5, 5, 5,                 /* 28 .. 2f */
       5, 5, 5, 5, 5, 5, 5, 5,                 /* 30 .. 37 */
       5, 5, 5, 5, 5, 5, 5, 5,                 /* 38 .. 3f */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 40 .. 47 */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 48 .. 4f */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 50 .. 57 */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 58 .. 5f */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 60 .. 67 */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 68 .. 6f */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 70 .. 77 */
       6, 6, 6, 6, 6, 6, 6, 6,                 /* 78 .. 7f */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* 80 .. 87 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* 88 .. 8f */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* 90 .. 97 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* 98 .. 9f */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* a0 .. a7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* a8 .. af */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* b0 .. b7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* b8 .. bf */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* c0 .. c7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* c8 .. cf */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* d0 .. d7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* d8 .. df */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* e0 .. e7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* e8 .. ef */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* f0 .. f7 */
       7, 7, 7, 7, 7, 7, 7, 7,                 /* f8 .. ff */
};

static int
xfs_highbit32(uint32_t v)
{
       int             i;

       if (v & 0xffff0000)
               if (v & 0xff000000)
                       i = 24;
               else
                       i = 16;
       else if (v & 0x0000ffff)
               if (v & 0x0000ff00)
                       i = 8;
              else
                       i = 0;
       else
               return -1;
       return i + xfs_highbit[(v >> i) & 0xff];
}

static int
isinxt (xfs_fileoff_t key, xfs_fileoff_t offset, xfs_filblks_t len)
{
       return (key >= offset) ? (key < offset + len ? 1 : 0) : 0;
}

static xfs_daddr_t
agb2daddr (xfs_agnumber_t agno, xfs_agblock_t agbno)
{
       /*
        * Extent records and btree pointers come straight off disk; a
        * corrupt one must not be allowed to turn into an arbitrary disk
        * address for devread() to read from.
        */
       if (agno >= xfs.agcount || agbno >= xfs.agblocks) {
               printf("xfs: corrupt block pointer (ag %u blk %u, have %u ags of %u blocks)\n",
                      (unsigned int) agno, (unsigned int) agbno,
                      xfs.agcount, xfs.agblocks);
               return (xfs_daddr_t) -1;
       }

       return ((xfs_fsblock_t)agno*xfs.agblocks + agbno) << xfs.bdlog;
}

static xfs_daddr_t
fsb2daddr (xfs_fsblock_t fsbno)
{
       return agb2daddr ((xfs_agnumber_t)(fsbno >> xfs.agblklog),
                        (xfs_agblock_t)(fsbno & mask32lo(xfs.agblklog)));
}

/*
 * Returns the number of records the BTREE root can hold, or -1 if
 * di_forkoff/xfs.isize is too small to even hold the root header --
 * a corrupt inode should never be trusted to size a pointer into itself.
 */
static inline int
btroot_maxrecs (void)
{
       int tmp = icore.di_forkoff ? (icore.di_forkoff << 3) : xfs.isize;
       int hdrsize = sizeof(xfs_bmdr_block_t) + offsetof(xfs_dinode_t, di_u);

       if (tmp < hdrsize) {
               return -1;
       }

       return (tmp - hdrsize) /
               (sizeof (xfs_bmbt_key_t) + sizeof (xfs_bmbt_ptr_t));
}


/* Offsets of the data fork (literal area) in bytes.
 * From XFS on-disk format documentation:
 *   - v2 inode: 0x64 (100)
 *   - v3 inode: 0xB0 (176)
 */
#define XFS_DINODE_DFORK_V2_OFF   100
#define XFS_DINODE_DFORK_V3_OFF   176

static inline char *
xfs_dinode_data_fork(void)
{
    /* icore.di_version is already byte-swapped in the inode core */
    if (icore.di_version >= 3)
        return (char *)inode + XFS_DINODE_DFORK_V3_OFF;
    else
        return (char *)inode + XFS_DINODE_DFORK_V2_OFF;
}

/* Read inode from disk (v4/v5 compatible) */

static int
di_read (xfs_ino_t ino)
{
    xfs_agino_t    agino;
    xfs_agnumber_t agno;
    xfs_agblock_t  agbno;
    xfs_daddr_t    daddr;
    int            offset;

    /* -------------------------------
     * Locate the inode on disk
     * ------------------------------- */
    agno   = ino2agno (ino);
    agino  = ino2agino (ino);
    agbno  = agino2agbno (agino);
    offset = ino2offset (ino);
    daddr  = agb2daddr (agno, agbno);

    /* Read the raw inode into FSYS_BUF-backed "inode" */
    devread (daddr, offset * xfs.isize, xfs.isize, (char *)inode);

#ifdef DEBUG_XFS_2
    printf("di_read(): ino=%lu agno=%u agino=%u offset=%d isize=%d\n",
           (unsigned long long) ino,
           (unsigned int) agno,
           (unsigned int) agino,
           offset,
           xfs.isize);

    printf("di_read(): magic=0x%x version=%d format=%d size=%lu flags=0x%x is_v5=%d\n",
           be16_to_cpu(inode->di_core.di_magic),
           inode->di_core.di_version,
           inode->di_core.di_format,
           (unsigned long long) be64_to_cpu(inode->di_core.di_size),
           be16_to_cpu(inode->di_core.di_flags),
           xfs.is_v5);
#endif

    /* -------------------------------
     * Basic sanity checks
     * ------------------------------- */
    if (be16_to_cpu(inode->di_core.di_magic) != XFS_DINODE_MAGIC) {
        printf("XFS: bad inode magic 0x%x for ino %lu\n",
               be16_to_cpu(inode->di_core.di_magic),
               (unsigned long long) ino);
        return 0;
    }

    /* v1 / v2 / v3 inodes are acceptable */
    if (inode->di_core.di_version != 1 &&
        inode->di_core.di_version != 2 &&
        inode->di_core.di_version != 3) {
        printf("XFS: unsupported inode version %d for ino %lu\n",
               inode->di_core.di_version,
               (unsigned long long) ino);
        return 0;
    }

    /*
     * NOTE: For v5 (di_version == 3), the fields we use:
     *   - di_format
     *   - di_forkoff
     *   - di_size
     *   - di_nextents
     *   - di_nblocks
     *   - timestamps, uid/gid, etc.
     * are in the SAME places as for v1/v2 on disk.
     *
     * The extra v3 inode fields live after di_gen, so we do not
     * need to special-case them here for our bootloader.
     */

    /* -------------------------------
     * Prime BTREE root pointer (ptr0)
     * -------------------------------
     * init_extents() will:
     *   - for EXTENTS: use inode->di_u.di_bmx
     *   - for BTREE:   follow xfs.ptr0, etc.
     *
     * xfs.ptr0 is only valid / needed if di_format == BTREE.
     */
	if (inode->di_core.di_format == XFS_DINODE_FMT_BTREE) {
    		char *dfork = xfs_dfork_dptr(); /* start of data fork (v2/v3 aware) */
    		int maxrecs = btroot_maxrecs();

    		if (maxrecs < 0) {
    			printf("XFS: bad btree root for ino %lu\n",
    			       (unsigned long long) ino);
    			return 0;
    		}

    		xfs.ptr0 = *(xfs_bmbt_ptr_t *)(dfork +
                 sizeof(xfs_bmdr_block_t) +
                 maxrecs * sizeof(xfs_bmbt_key_t));
#ifdef DEBUG_XFS
	   printf("di_read(): ino=%lu di_version=%u di_format=%u di_size=%lu di_nextents=%u is_v5=%d\n",
	   (unsigned long long)ino,
           inode->di_core.di_version,
           inode->di_core.di_format,
           (unsigned long long)be64_to_cpu(inode->di_core.di_size),
           be32_to_cpu(inode->di_core.di_nextents),
           xfs.is_v5);
#endif
    }

    return 1;
}

/* On-disk header size (NOT sizeof the C struct) */
static inline int
xfs_sf_hdr_size(uint8_t i8count)
{
    int size = 2;  /* count + i8count */

    if (i8count)
        size += sizeof(xfs_dir2_ino8_t);
    else
        size += sizeof(xfs_dir2_ino4_t);

    return size;
}

static inline xfs_dir2_sf_entry_t *
xfs_sf_firstentry(xfs_dir2_sf_t *sf)
{
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;
    return (xfs_dir2_sf_entry_t *)((char *)hdr + xfs_sf_hdr_size(hdr->i8count));
}



static void
init_extents (void)
{
       xfs_bmbt_ptr_t ptr0;
       xfs_btree_lblock_t h;

       switch (icore.di_format) {
       case XFS_DINODE_FMT_EXTENTS:
	        /* Extent records start at the beginning of the data fork */
    		//xfs.xt = (xfs_bmbt_rec_32_t *)xfs_dfork_dptr();
    		//xfs.nextents = be32_to_cpu(icore.di_nextents);
		xfs_dinode_disk_t *dip = (xfs_dinode_disk_t *)inode;

		uint32_t ne;

		if (xfs.has_nrext64) {
			/* The 8 bytes that hold di_pad[6] and di_flushiter on
			 * every other layout are di_big_nextents, a 64-bit
			 * data extent count, when NREXT64 is set.  32 bits is
			 * plenty here: nothing on a boot partition comes near
			 * 2^32 extents.
			 */
			uint64_t be_big = 0;

			memcpy(&be_big, dip->di_pad, sizeof(be_big));
			ne = (uint32_t) be64_to_cpu(be_big);
		} else {
			ne = be32_to_cpu(icore.di_nextents);
		}

		xfs.xt = (xfs_bmbt_rec_32_t *)xfs_dfork_dptr();
		xfs.nextents = ne;
               break;
       case XFS_DINODE_FMT_BTREE:
               ptr0 = xfs.ptr0;
               for (;;) {
                       xfs.daddr = fsb2daddr (be64_to_cpu(ptr0));
                       devread (xfs.daddr, 0,
                                sizeof(xfs_btree_lblock_t), (char *)&h);
                       if (!h.bb_level) {
                               xfs.nextents = be16_to_cpu(h.bb_numrecs);
                               xfs.next = fsb2daddr (be64_to_cpu(h.bb_rightsib));
                               xfs.fpos = sizeof(xfs_btree_block_t);
                               return;
                       }
                       devread (xfs.daddr, xfs.btnode_ptr0_off,
                                sizeof(xfs_bmbt_ptr_t), (char *)&ptr0);
               }
       }
}

static xad_t *
next_extent (void)
{
       static xad_t xad;

       switch (icore.di_format) {
       case XFS_DINODE_FMT_EXTENTS:
               if (xfs.nextents == 0)
                       return NULL;
               break;
       case XFS_DINODE_FMT_BTREE:
               if (xfs.nextents == 0) {
                       xfs_btree_lblock_t h;
                       if (xfs.next == 0)
                               return NULL;
                       xfs.daddr = xfs.next;
                       devread (xfs.daddr, 0, sizeof(xfs_btree_lblock_t), (char *)&h);
                       xfs.nextents = be16_to_cpu(h.bb_numrecs);
                       xfs.next = fsb2daddr (be64_to_cpu(h.bb_rightsib));
                       xfs.fpos = sizeof(xfs_btree_block_t);
               }
               /* Yeah, I know that's slow, but I really don't care */
               devread (xfs.daddr, xfs.fpos, sizeof(xfs_bmbt_rec_t), filebuf);
               xfs.xt = (xfs_bmbt_rec_32_t *)filebuf;
               xfs.fpos += sizeof(xfs_bmbt_rec_32_t);
       }
       xad.offset = xt_offset (xfs.xt);
       xad.start = xt_start (xfs.xt);
       xad.len = xt_len (xfs.xt);
       ++xfs.xt;
       --xfs.nextents;

       return &xad;
}

/*
 * Name lies - the function reads only first 100 bytes
 */
static int
xfs_dabread (void)
{
       xad_t *xad;
       xfs_fileoff_t offset;

       init_extents ();
       while ((xad = next_extent ())) {
               offset = xad->offset;
               if (isinxt (xfs.dablk, offset, xad->len)) {
                       devread (fsb2daddr (xad->start + xfs.dablk - offset),
                                0, 100, dirbuf);
                       return 0;
               }
       }
       /* No extent covers xfs.dablk: dirbuf still holds the previous block. */
       return -1;
}


/* Get parent inode from shortform header (v4 & v5). */

static inline xfs_ino_t
sf_parent_ino(void)
{
    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;

    return xfs_dir2_sf_get_ino(hdr, &hdr->parent);
}


static inline int
roundup8 (int n)
{
       return ((n+7)&~7);
}


static int
xfs_count_dir3_entries(void)
{
    uint32_t magic;
    int count = 0;
    int has_ftype = xfs.has_ftype;
    xfs_dir3_block_tail_t tail;
    int data_end;

    /* Start at beginning of block */
    filepos   = 0;
    xfs.blkoff = 0;

    /* Read block magic */
    if (xfs_read(&magic, sizeof(magic)) != sizeof(magic)) {
        printf("xfs: failed to read dir3 block magic\n");
        return 0;
    }
    magic = be32_to_cpu(magic);

    if (magic != XFS_DIR3_DATA_MAGIC && magic != XFS_DIR3_BLOCK_MAGIC) {
        printf("xfs: unsupported DIR3 block magic: 0x%x\n", magic);
        return 0;
    }

    /*
     * Only a block-format directory carries a tail; a data block of a
     * multi-block directory has file data all the way to the end, so
     * looking for a leaf count there reads whatever happens to be in
     * the last twelve bytes.
     */
    xfs.dir3_has_tail = (magic == XFS_DIR3_BLOCK_MAGIC);

    /* Skip v5 data header */
    filepos    = sizeof(xfs_dir3_data_hdr_t);
    xfs.blkoff = filepos;

    if (!xfs.dir3_has_tail) {
        data_end = xfs.dirbsize;
    } else {
        /* --- Find block tail to know where the leaf area starts --- */
        long saved_pos  = filepos;
        long saved_off  = xfs.blkoff;

        /* Tail is at end of block */
        filepos = xfs.dirbsize - sizeof(xfs_dir3_block_tail_t);
        if (xfs_read(&tail, sizeof(tail)) != sizeof(tail)) {
            printf("xfs: failed to read dir3 block tail\n");
            return 0;
        }

        /* Convert endianness */
        uint32_t leaf_count = be32_to_cpu(tail.count);
        long leaf_bytes = (long) leaf_count * sizeof(xfs_dir2_leaf_entry_t);
        long avail = (long) xfs.dirbsize - sizeof(xfs_dir3_block_tail_t);

        /*
         * leaf_count comes straight off disk; a corrupt value must not be
         * allowed to underflow data_end into a huge value and send the
         * entry scan below past the real end of directory data.
         */
        if (leaf_bytes < 0 || leaf_bytes > avail) {
            printf("xfs: corrupt dir3 leaf count %u\n", leaf_count);
            return 0;
        }

        /* Leaf array starts just before the tail */
        data_end = avail - leaf_bytes;

        /* Restore position to start scanning data entries */
        filepos    = saved_pos;
        xfs.blkoff = saved_off;
    }

    /* Walk entries only within [header .. data_end) */
    while (xfs.blkoff < data_end) {
        xfs_dir2_data_union_t u;

        /* Read first 4 bytes of slot */
        if (xfs_read(&u, 4) != 4)
            break;

        xfs.blkoff += 4;

        /* Free entry? */
        if (be16_to_cpu(u.unused.freetag) == XFS_DIR2_DATA_FREE_TAG) {
            uint16_t len = be16_to_cpu(u.unused.length);
            int skip = roundup8(len) - 4;  /* 4 already read */
            if (skip < 0) skip = 0;

            filepos    += skip;
            xfs.blkoff += skip;
            continue;
        }

        /* Used entry: read remaining 4 bytes of inumber + namelen (5 bytes total) */
        if (xfs_read(((char *)&u) + 4, 5) != 5)
            break;

        xfs.blkoff += 5;

        if (u.entry.namelen != 0)
            count++;

        /* Compute full entry size */
        {
            int base = 8 + 1 + u.entry.namelen + 2;
            if (has_ftype)
                base += 1;        /* ftype byte */

            int entsize = roundup8(base);

            /* We already consumed 9 bytes: 8 (ino) + 1 (namelen) */
            int skip = entsize - 9;
            if (skip < 0)
                skip = 0;

            filepos    += skip;
            xfs.blkoff += skip;
        }
    }

    /* Leave caller positioned at start of data region */
    filepos    = sizeof(xfs_dir3_data_hdr_t);
    xfs.blkoff = filepos;

    return count;
}

static char *
next_dentry(xfs_ino_t *ino)
{
   
#ifdef DEBUG_XFS       
	printf("next_dentry(): di_format=%d dirpos=%d dirmax=%d is_v5=%d\n",
           icore.di_format, xfs.dirpos, xfs.dirmax, xfs.is_v5);
#endif
    /* Generic end-of-directory handling */
    if (icore.di_format == XFS_DINODE_FMT_LOCAL) {
        /* shortform: dirpos includes -2 (.), -1 (..), then 0..dirmax-1 */
        if (xfs.dirpos >= xfs.dirmax)
            return NULL;
    } else {
        /* block/leaf/btree formats */
        if (xfs.dirpos >= xfs.dirmax) {

            /* v5: we don’t chain leaf blocks yet → stop here */
            if (xfs.is_v5) {
                return NULL;
            }

            /* v4 (DIR2) leaf form: follow forward pointer */
            if (xfs.forw == 0)
                return NULL;

            xfs.dablk = xfs.forw;
            if (xfs_dabread() < 0) {
                printf("xfs: dir2 leaf block %u not found\n", xfs.dablk);
                return NULL;
            }

#define h ((xfs_dir2_leaf_hdr_t *)dirbuf)
            xfs.dirmax = be16_to_cpu(h->count) - be16_to_cpu(h->stale);
            xfs.forw   = be32_to_cpu(h->info.forw);
#undef h
            xfs.dirpos = 0;
        }
    }

    switch (icore.di_format) {
    case XFS_DINODE_FMT_LOCAL:
        return next_dentry_local(ino);

    case XFS_DINODE_FMT_EXTENTS:
    case XFS_DINODE_FMT_BTREE:
        /* v4 → DIR2, v5 → DIR3 */
        return xfs.is_v5 ? next_dentry_dir3(ino) : next_dentry_dir2(ino);

    default:
        printf("xfs: unsupported inode format %d\n", icore.di_format);
        return NULL;
    }
}


/* ------------------------------------------------------------
 *  SHORTFORM DIRECTORY (LOCAL FORMAT)
 * ------------------------------------------------------------ */
static char *
first_dentry_local(xfs_ino_t *ino)
{
    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;

    xfs.forw   = 0;
    xfs.dirmax = hdr->count;          /* number of real entries (no . / ..) */
#ifdef DEBUG_XFS
    printf("sf->hdr.count=%d\n",xfs.dirmax);
#endif
    /* Keep i8param for old helpers, though we no longer rely on it here. */
    xfs.i8param = hdr->i8count ? 0 : 4;

    /*
     * dirpos:
     *   -2 = "."
     *   -1 = ".."
     *    0 = first real entry in sf->list[]
     */
    xfs.dirpos = -2;

#ifdef DEBUG_XFS
    printf("first_dentry(): ino=%lu version=%d format=%d size=%ld is_v5=%d\n",
           (unsigned long long)*ino,
           icore.di_version,
           icore.di_format,
           (long long)be64_to_cpu(icore.di_size),
           xfs.is_v5);
    printf("sf->hdr.count = %u\n", hdr->count);
#endif

    return next_dentry_local(ino);
}
	

static inline char *
xfs_sf_name(xfs_dir2_sf_entry_t *sfe)
{
    /* For both v4 and v5 shortform dirs, the name starts here. */
    return (char *)sfe->name;
}


/* Compute inumber for a shortform entry, from the same layout. */

static inline xfs_ino_t
xfs_sf_ino_from_entry(xfs_dir2_sf_entry_t *sfe)
{
    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;
    uint8_t           *p;
    xfs_ino_t          ino = 0;

    /* Start at end of name */
    p = (uint8_t *)sfe->name + sfe->namelen;

    /* With ftype, name[namelen] is the file type byte, so skip it. */
    if (xfs.has_ftype)
        p++;

    if (hdr->i8count) {
        /* 8-byte inode */
        memcpy(&ino, p, sizeof(xfs_dir2_ino8_t));
        ino = be64_to_cpu(ino) & 0x00ffffffffffffffULL;
    } else {
        uint32_t v32;
        memcpy(&v32, p, sizeof(xfs_dir2_ino4_t));
        ino = be32_to_cpu(v32);
    }

    return ino;
}

/* Size of one shortform entry in a v4 (no ftype) shortform dir.
 *
 * On disk the layout is:
 *   [0]               uint8_t namelen
 *   [1..]             xfs_dir2_sf_off_t offset
 *   [..]              name[namelen]
 *   [..]              inode (4 or 8 bytes)
 */
/* v2 shortform entry size (no ftype stored) */
static inline int
xfs_dir2_sf_entsize(xfs_dir2_sf_hdr_t *hdr, int namelen)
{
    int count = 0;

    count += 1;                               /* namelen */
    count += sizeof(xfs_dir2_sf_off_t);       /* offset */
    count += namelen;                         /* name bytes */
    count += hdr->i8count
           ? (int)sizeof(xfs_dir2_ino8_t)     /* 8-byte inode (#s) */
           : (int)sizeof(xfs_dir2_ino4_t);    /* 4-byte inode (#s) */

    return count;
}
/* v5 shortform entry size: v2 + 1 byte for file type */
static int
xfs_dir3_sf_entsize(xfs_dir2_sf_hdr_t *hdr, int namelen)
{
    return xfs_dir2_sf_entsize(hdr, namelen) + 1;
}



/* Size of one shortform entry.
 *
 * On disk (v4):
 *   [0]               u8  namelen
 *   [1..2]            xfs_dir2_sf_off_t offset
 *   [3..]             name[namelen]
 *   [..]              inode (8 bytes for our purposes)
 *
 * On disk (v5 / ftype=1):
 *   [0]               u8  namelen
 *   [1..2]            xfs_dir2_sf_off_t offset
 *   [3..]             name[namelen]
 *   [..]              u8  ftype
 *   [..]              inode (8 bytes)
 */


/* Version-agnostic helper */
static inline int
xfs_sf_entsize(xfs_dir2_sf_hdr_t *hdr, int namelen)
{
    return xfs.has_ftype ? xfs_dir3_sf_entsize(hdr, namelen)
                         : xfs_dir2_sf_entsize(hdr, namelen);
}
/* Get parent inode from shortform header */
static inline xfs_ino_t
sf_parent_ino_sf(void)
{
    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;
    uint8_t           *p   = (uint8_t *)&hdr->parent;

    if (hdr->i8count)
        return (xfs_ino_t)be64_to_cpu(*(xfs_ino_t *)p);
    else
        return (xfs_ino_t)be32_to_cpu(*(uint32_t *)p);
}



/* Decode an xfs_dir2_inou_t -> xfs_ino_t (v4/v5) */
static inline xfs_ino_t
xfs_dir2_sf_get_ino(xfs_dir2_sf_hdr_t *hdr, xfs_dir2_inou_t *from)
{
    xfs_ino_t ino = 0;

    if (hdr->i8count) {
        /* 64-bit inode stored big-endian in from->i8.i[8] */
        memcpy(&ino, &from->i8, sizeof(xfs_dir2_ino8_t));
        ino = be64_to_cpu(ino);                        /* swap from disk BE to CPU */
        ino &= 0x00ffffffffffffffULL;           /* top byte reserved */
    } else {
        uint32_t v = 0;
        memcpy(&v, &from->i4, sizeof(xfs_dir2_ino4_t));
        ino = be32_to_cpu(v);
    }
    return ino;
}

static char *
next_dentry_local(xfs_ino_t *ino)
{
    static xfs_dir2_sf_entry_t *sfe;
    static char sf_name_buf[256]; /* XFS max namelen is 255 */

    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;
    char *name = NULL;
    int namelen;

    if (xfs.dirpos >= xfs.dirmax && xfs.dirpos >= 0)
        return NULL;

    switch (xfs.dirpos) {
    case -2: /* "." (synthetic) */
        *ino    = 0;          /* or current inode, depending on your design */
        name    = ".";
        namelen = 1;
        break;

    case -1: /* ".." (from header parent) */
        *ino    = sf_parent_ino();  /* using hdr + i8count */
        name    = "..";
        namelen = 2;

        /* First on-disk entry starts here */
        sfe = xfs_sf_firstentry(sf);
        break;

    default:
        namelen = sfe->namelen;
        if (namelen > 255)
            namelen = 255;

        *ino = xfs_sf_ino_from_entry(sfe);

        memcpy(sf_name_buf, xfs_sf_name(sfe), namelen);
        sf_name_buf[namelen] = '\0';
        name = sf_name_buf;

        /* Advance to the next shortform entry */
        sfe = (xfs_dir2_sf_entry_t *)((char *)sfe +
                                      xfs_sf_entsize(hdr, sfe->namelen));
        break;
    }

    xfs.dirpos++;
    return name;
}

static char *
first_dentry_dir3(xfs_ino_t *ino)
{
    xfs.forw  = 0;
    xfs.dirpos = 0;

    /* Count entries and position at first DIR3 data entry */
    xfs.dirmax = xfs_count_dir3_entries();
    if (xfs.dirmax <= 0)
        return NULL;

    return next_dentry_dir3(ino);
}



static char *
next_dentry_dir3(xfs_ino_t *ino)
{
    int has_ftype = xfs.has_ftype;

#define d2u ((xfs_dir2_data_union_t *)dirbuf)

    /*
     * One iteration per entry handed back.  A bogus entry starts the next
     * iteration instead of recursing: a directory full of them used to
     * recurse once per entry, and the bootloader's stack has nothing
     * below it to catch that.
     */
    for (;;) {
        for (;;) {
            /* At end of this DIR3 data block? Go to next block. */
            if (xfs.blkoff >= xfs.dirbsize) {
                uint32_t magic;

                /* Align filepos down to the start of this block. */
                filepos &= ~(xfs.dirbsize - 1);

                /* Read block magic. */
                if (xfs_read(&magic, sizeof(magic)) != sizeof(magic)) {
                    printf("xfs: read error in DIR3 block header\n");
                    return NULL;
                }
                magic = be32_to_cpu(magic);

                if (magic != XFS_DIR3_DATA_MAGIC &&
                    magic != XFS_DIR3_BLOCK_MAGIC) {
                    printf("xfs: invalid DIR3 block magic: 0x%x\n", magic);
                    return NULL;
                }

                xfs.dir3_has_tail = (magic == XFS_DIR3_BLOCK_MAGIC);

                /* Skip full v5 header. */
                xfs.blkoff = sizeof(xfs_dir3_data_hdr_t);
                filepos   |= xfs.blkoff;
            }

            /*
             * Stop at the tail of a block-format directory (the v5 tail,
             * 12 bytes, not the v4 8-byte one).  A data block has no
             * tail, and its entries run to the end of the block.
             */
            if (xfs.dir3_has_tail
                && xfs.blkoff + (int) sizeof(xfs_dir3_block_tail_t)
                   >= xfs.dirbsize)
                return NULL;

            /* Read first 4 bytes of the next slot */
            if (xfs_read(dirbuf, 4) != 4) {
                /* This is end-of-dir, not an error */
                return NULL;
            }
            xfs.blkoff += 4;

            /* Free entry? */
            if (be16_to_cpu(d2u->unused.freetag) == XFS_DIR2_DATA_FREE_TAG) {
                uint16_t len = be16_to_cpu(d2u->unused.length);
                int skip = roundup8(len) - 4;   /* 4 already read */

                if (skip < 0)
                    skip = 0;

                filepos    += skip;
                xfs.blkoff += skip;
                continue;   /* look at next slot */
            }

            /* Used entry found. */
            break;
        }

        /* Read rest of fixed header: remaining 4 bytes of inumber + namelen. */
        if (xfs_read((char *)dirbuf + 4, 5) != 5) {
            printf("xfs: short read in dir3 entry header\n");
            return NULL;
        }
        xfs.blkoff += 5;

        *ino = be64_to_cpu(d2u->entry.inumber);
        int namelen = d2u->entry.namelen;

        /* Skip bogus entries. */
        if (*ino == 0 || namelen == 0) {
            xfs.dirpos++;
            continue;
        }

        /* Compute remaining size of this entry. */
        {
            int base = 8 + 1 + namelen + 2;
            if (has_ftype)
                base += 1;

            int entsize = roundup8(base);
            int toread  = entsize - 9;   /* 9 bytes already consumed */

            if (toread < 0)
                toread = 0;

            if (toread && xfs_read(dirbuf, toread) != toread) {
                printf("xfs: short read in dir3 filename\n");
                return NULL;
            }

            xfs.blkoff += toread;
        }

        /* dirbuf now starts with the filename bytes. */
        char *name = (char *)dirbuf;
        name[namelen] = 0;   /* overwrite filetype / padding / tag byte */

        xfs.dirpos++;

        return name;
    }

#undef d2u
}


/*
 * Initialize reading entries from a classic XFS DIR2 directory
 * (XFS v4, crc=0). Handles:
 *   - block format
 *   - leaf1 / leafn format
 *
 * Returns the first entry via next_dentry_dir2().
 */
static char *
first_dentry_dir2(xfs_ino_t *ino)
{
    int depth;

    xfs.forw = 0;
    filepos  = 0;

#ifdef DEBUG_XFS_2
    printf("first_dentry_dir2(): starting (di_format=%d)\n",
           icore.di_format);
#endif

    /* ----------------------------------------------------------
     * SHORTFORM (small) directory
     * ---------------------------------------------------------- */
    if (icore.di_format == XFS_DINODE_FMT_LOCAL)
    {
        xfs.dirmax  = inode->di_u.di_dir2sf.hdr.count;
        xfs.i8param = inode->di_u.di_dir2sf.hdr.i8count ? 0 : 4;
        xfs.dirpos  = -2;   /* ".", "..", then real entries */

#ifdef DEBUG_XFS_2
        printf("DIR2 shortform: dirmax=%d i8param=%d\n",
               xfs.dirmax, xfs.i8param);
#endif
        return next_dentry_dir2(ino);
    }

    /* ----------------------------------------------------------
     * EXTENTS or BTREE directory
     * Read first data block header
     * ---------------------------------------------------------- */
    if (xfs_read(dirbuf, sizeof(xfs_dir2_data_hdr_t)) != sizeof(xfs_dir2_data_hdr_t)) {
        printf("xfs: failed to read dir2 data header\n");
        return NULL;
    }

    uint32_t magic = be32_to_cpu(((xfs_dir2_data_hdr_t *)dirbuf)->magic);

#ifdef DEBUG_XFS_2
    printf("DIR2 header magic = 0x%x\n", magic);
#endif

    /* ----------------------------------------------------------
     * CASE 1: BLOCK-FORMAT DIRECTORY (single block)
     * ---------------------------------------------------------- */
    if (magic == XFS_DIR2_BLOCK_MAGIC)
    {
        xfs_dir2_block_tail_t *tail;

        /* Tail is located at end of block */
        filepos = xfs.dirbsize - sizeof(*tail);

        if (xfs_read(dirbuf, sizeof(*tail)) != sizeof(*tail)) {
            printf("xfs: failed to read dir2 block tail\n");
            return NULL;
        }

        tail = (xfs_dir2_block_tail_t *)dirbuf;
        xfs.dirmax = be32_to_cpu(tail->count) - be32_to_cpu(tail->stale);

#ifdef DEBUG_XFS_2
        printf("DIR2 block-format: dirmax=%d\n", xfs.dirmax);
#endif
    }

    /* ----------------------------------------------------------
     * CASE 2: LEAF1 / LEAFN directory
     * ---------------------------------------------------------- */
    else
    {
        /* Starting dablk for leaf / node search, per XFS rules */
        xfs.dablk = (1ULL << 35) >> xfs.blklog;

        for (depth = 0; depth <= XFS_DA_NODE_MAXDEPTH; depth++)
        {
            if (xfs_dabread() < 0) {
                printf("xfs: dir2 node block %u not found\n", xfs.dablk);
                return NULL;
            }

            xfs_dir2_leaf_hdr_t *lh = (xfs_dir2_leaf_hdr_t *)dirbuf;
            xfs_da_intnode_t    *n  = (xfs_da_intnode_t *)dirbuf;

            uint16_t m = be16_to_cpu(n->hdr.info.magic);

#ifdef DEBUG_XFS_2
            printf("DIR2 leaf/node scan: magic=0x%x\n", m);
#endif

            if (m == XFS_DIR2_LEAF1_MAGIC || m == XFS_DIR2_LEAFN_MAGIC)
            {
                xfs.dirmax = be16_to_cpu(lh->count) - be16_to_cpu(lh->stale);
                xfs.forw   = be32_to_cpu(lh->info.forw);

#ifdef DEBUG_XFS_2
                printf("DIR2 leaf-format: dirmax=%d forw=%u\n",
                       xfs.dirmax, xfs.forw);
#endif
                break;
            }

            /* Follow B-tree "before" pointer */
            xfs.dablk = be32_to_cpu(n->btree[0].before);
        }

        if (depth > XFS_DA_NODE_MAXDEPTH) {
            printf("xfs: dir2 node tree deeper than %d levels\n",
                   XFS_DA_NODE_MAXDEPTH);
            return NULL;
        }
    }

    /* --------------------------------------
     * Initialize data-block scan position
     * -------------------------------------- */
    xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
    filepos    = xfs.blkoff;
    xfs.dirpos = 0;

#ifdef DEBUG_XFS_2
    printf("DIR2 start: blkoff=%d dirpos=%d dirmax=%d\n",
           xfs.blkoff, xfs.dirpos, xfs.dirmax);
#endif

    return next_dentry_dir2(ino);
}

static char *
next_dentry_dir2(xfs_ino_t *ino)
{
    int namelen;
    int toread;
    char *name;

#define dau ((xfs_dir2_data_union_t *)dirbuf)

    /*
     * One iteration per entry handed back.  A bogus entry starts the next
     * iteration instead of recursing: a directory full of them used to
     * recurse once per entry, and the bootloader's stack has nothing
     * below it to catch that (see next_dentry_dir3()).
     */
    for (;;) {

    /* Loop until we find a valid (non-free) entry */
    for (;;) {

        /* --------------------------------------------------------
         * If we hit the end of a DIR2 block, move to the next one
         * -------------------------------------------------------- */
        if (xfs.blkoff >= xfs.dirbsize) {

            uint32_t magic;

            /* Align filepos to start of new block */
            filepos &= ~(xfs.dirbsize - 1);

            /* Read block magic */
            if (xfs_read(&magic, sizeof(magic)) != sizeof(magic)) {
                printf("xfs: read error in DIR2 block header\n");
                return NULL;
            }
            magic = be32_to_cpu(magic);

            /* Valid DIR2 magic values */
            if (magic != XFS_DIR2_DATA_MAGIC &&
                magic != XFS_DIR2_BLOCK_MAGIC)
            {
                printf("xfs: invalid DIR2 block magic: 0x%x\n", magic);
                return NULL;
            }

            /* Reset pointer just after header */
            xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
            filepos |= xfs.blkoff;
        }

        /* --------------------------------------------------------
         * Read first 4 bytes of entry
         *    free-entry: dau->unused.freetag == FREE_TAG
         *    used-entry: beginning of inode number (LSB)
         * -------------------------------------------------------- */
        if (xfs_read(dirbuf, 4) != 4) {
            printf("xfs: short read in dir2 entry\n");
            return NULL;
        }
        xfs.blkoff += 4;

        /* Free entry? */
        if (dau->unused.freetag == XFS_DIR2_DATA_FREE_TAG) {

            uint32_t len = be16_to_cpu(dau->unused.length);

            /* Length includes the 4 bytes we already consumed */
            toread = roundup8(len) - 4;

            xfs.blkoff += toread;
            filepos += toread;

            continue; /* skip free entry */
        }

        /* Otherwise this is a DIR2 data entry */
        break;
    }

    /* --------------------------------------------------------
     * Read rest of entry header (5 bytes):
     *   - namelen (1)
     *   - name[0] (1)
     *   - tag    (2)
     *   - padding (1)
     * -------------------------------------------------------- */
    if (xfs_read((char *)dirbuf + 4, 5) != 5) {
        printf("xfs: short read in dir2 entry header\n");
        return NULL;
    }
    xfs.blkoff += 5;

    *ino    = be64_to_cpu(dau->entry.inumber);
    namelen = dau->entry.namelen;

    /* Skip bogus entries */
    if (*ino == 0 || namelen == 0) {
        xfs.dirpos++;
        continue;
    }

    /* --------------------------------------------------------
     * Read filename + padding
     * -------------------------------------------------------- */
    toread = roundup8(namelen + 11) - 9;

    if (xfs_read(dirbuf, toread) != toread) {
        printf("xfs: short read in dir2 filename\n");
        return NULL;
    }

    name = (char *)dirbuf;
    name[namelen] = 0;

    /* Advance */
    xfs.blkoff += toread;
    xfs.dirpos++;

    return name;

    } /* outer for(;;) */

#undef dau
}

/* Dispatcher for first directory entry.
 * Chooses between:
 *   - local directory (shortform)
 *   - dir2 block/leaf formats (XFS v4)
 *   - dir3 block/leaf formats (XFS v5)
 */


static char *
first_dentry(xfs_ino_t *ino)
{
    xfs.forw = 0;
#ifdef DEBUG_XFS
   printf("first_dentry(): ino=%lu version=%d format=%d size=%lu is_v5=%d\n",
       (unsigned long long)*ino,
       inode->di_core.di_version,
       inode->di_core.di_format,
       (unsigned long long) be64_to_cpu(inode->di_core.di_size),
       xfs.is_v5);
#endif
    switch (icore.di_format)
    {
    /* ----------------------------------------------------
     *  SHORTFORM (LOCAL) DIRECTORIES
     * ---------------------------------------------------- */
    case XFS_DINODE_FMT_LOCAL:
        return first_dentry_local(ino);

    /* ----------------------------------------------------
     *  BLOCK / LEAF / B-TREE DIRECTORIES
     * ---------------------------------------------------- */
    case XFS_DINODE_FMT_EXTENTS:
    case XFS_DINODE_FMT_BTREE:
    {
        uint32_t magic;

        filepos = 0;

        /* Read the directory block header magic */
        if (xfs_read(&magic, sizeof(magic)) != sizeof(magic)) {
            printf("xfs: failed to read directory block header\n");
            return NULL;
        }
        magic = be32_to_cpu(magic);

#ifdef DEBUG_XFS_2
        printf("first_dentry(): directory block magic = 0x%x\n", magic);
#endif

        /* -----------------------------
         *  DIR3 (XFS v5 CRC-enabled)
         * ----------------------------- */
        if (magic == XFS_DIR3_DATA_MAGIC ||
            magic == XFS_DIR3_BLOCK_MAGIC ||
            magic == XFS_DIR3_LEAFN_MAGIC ||
            magic == XFS_DIR3_LEAF1_MAGIC)
        {
            return first_dentry_dir3(ino);
        }

        /* -----------------------------
         *  DIR2 (Classic XFS v4)
         * ----------------------------- */
        if (magic == XFS_DIR2_DATA_MAGIC ||
            magic == XFS_DIR2_BLOCK_MAGIC ||
            magic == XFS_DIR2_LEAFN_MAGIC ||
            magic == XFS_DIR2_LEAF1_MAGIC)
        {
            return first_dentry_dir2(ino);
        }

        printf("xfs: unsupported directory magic 0x%x\n", magic);
        return NULL;
    }

    default:
        printf("xfs: unsupported inode format %d\n", icore.di_format);
        return NULL;
    }
}



/*
 * Initialize an XFS partition starting at offset P_OFFSET; this is
 * sort-of the same idea as "mounting" it.  Read in the relevant
 * control structures and make them available to the user.  Returns 0
 * if successful, -1 on failure.
 */
static int
xfs_mount(long cons_dev, long p_offset, long quiet)
{
       xfs_sb_t super;

       partition_offset = p_offset;

       if (cons_read (cons_dev, &super, sizeof(super), partition_offset) != sizeof(super)) {
               if (!quiet)
                       printf("xfs_mount: read_disk_block failed!\n");
               return -1;
       } else if (be32_to_cpu(super.sb_magicnum) != XFS_SB_MAGIC) {
               if (!quiet)
                       printf("xfs_mount: Bad magic: %x\n",
                              be32_to_cpu(super.sb_magicnum));
               return -1;
       } else {
        unsigned int ver = be16_to_cpu(super.sb_versionnum) & XFS_SB_VERSION_NUMBITS;

    	/* Accept XFS v4 and v5 */
    if (ver == XFS_SB_VERSION_5) {
#ifdef DEBUG_XFS
        printf("xfs_mount: Detected XFS v5 filesystem (CRC-enabled)\n");
#endif
        xfs.is_v5 = 1;       /* new flag */
    }
    else if (ver == XFS_SB_VERSION_4) {
        xfs.is_v5 = 0;
    }
    else {
        if (!quiet)
            printf("xfs_mount: unsupported XFS version %u\n", ver);
        return -1;
    }

 
       }

       /*
        * The file type byte in directory entries is a feature bit, not a
        * property of the superblock version: a v4 filesystem made with
        * "mkfs.xfs -m ftype=1" carries it too, and every v5 filesystem
        * has it.  Entry sizes are computed from this, so getting it from
        * the version alone misparses every directory on such a v4.
        */
       if (xfs.is_v5)
               xfs.has_ftype = (be32_to_cpu(super.sb_features_incompat)
                                & XFS_SB_FEAT_INCOMPAT_FTYPE) != 0;
       else
               xfs.has_ftype = (be32_to_cpu(super.sb_features2)
                                & XFS_SB_VERSION2_FTYPE) != 0;

       /*
        * Likewise for the 64-bit extent counts: di_big_nextents only
        * exists where the filesystem says so.  Everywhere else those
        * bytes are di_pad[6] and di_flushiter.
        */
       xfs.has_nrext64 = xfs.is_v5
               && (be32_to_cpu(super.sb_features_incompat)
                   & XFS_SB_FEAT_INCOMPAT_NREXT64) != 0;

       xfs.bsize = be32_to_cpu(super.sb_blocksize);
       xfs.blklog = super.sb_blocklog;
       xfs.bdlog = xfs.blklog - SECTOR_BITS;
       xfs.rootino = be64_to_cpu(super.sb_rootino);
       xfs.isize = be16_to_cpu(super.sb_inodesize);
       xfs.agblocks = be32_to_cpu(super.sb_agblocks);
       xfs.agcount = be32_to_cpu(super.sb_agcount);
       xfs.dirbsize = xfs.bsize << super.sb_dirblklog;

       xfs.inopblog = super.sb_inopblog;
       xfs.agblklog = super.sb_agblklog;
       xfs.agnolog = xfs_highbit32 (be32_to_cpu(super.sb_agcount));

       /*
        * Everything below reads into fixed regions of FSYS_BUF, so the
        * geometry has to fit them.  These all come off the disk, and
        * di_read() hands sb_inodesize straight to devread().
        */
       if (xfs.blklog < SECTOR_BITS || xfs.blklog > 16
           || xfs.bsize != (1 << xfs.blklog)) {
               if (!quiet)
                       printf("xfs_mount: bad block size %d (log %d)\n",
                              xfs.bsize, xfs.blklog);
               return -1;
       }
       if (xfs.isize < 256 || xfs.isize > INODE_SIZE) {
               if (!quiet)
                       printf("xfs_mount: inode size %d out of range\n",
                              xfs.isize);
               return -1;
       }
       if (super.sb_dirblklog > 8
           || xfs.dirbsize < (int) sizeof(xfs_dir3_data_hdr_t)) {
               if (!quiet)
                       printf("xfs_mount: bad directory block size %d\n",
                              xfs.dirbsize);
               return -1;
       }
       /*
        * agblklog and inopblog are used as shift counts (fsb2daddr(),
        * the XFS_INO_*_BITS macros); an out-of-range value is undefined
        * behavior in C and would turn a corrupt superblock into garbage
        * inode/block addresses fed straight to devread().
        */
       if (xfs.agblklog < 0 || xfs.agblklog > 31
           || xfs.inopblog < 0 || xfs.inopblog > 31
           || xfs.agblklog + xfs.inopblog > 31) {
               if (!quiet)
                       printf("xfs_mount: bad agblklog/inopblog (%d/%d)\n",
                              xfs.agblklog, xfs.inopblog);
               return -1;
       }

       xfs.btnode_ptr0_off =
               ((xfs.bsize - sizeof(xfs_btree_block_t)) /
               (sizeof (xfs_bmbt_key_t) + sizeof (xfs_bmbt_ptr_t)))
                * sizeof(xfs_bmbt_key_t) + sizeof(xfs_btree_block_t);

#ifdef DEBUG_XFS
       printf("XFS: version   = %d\n",be16_to_cpu(super.sb_versionnum) & XFS_SB_VERSION_NUMBITS);
       printf("XFS: blocksize = %d\n",xfs.bsize);
#endif
       dev = cons_dev;
       xfsfs.blocksize = xfs.bsize;
       return 0;
}

static long
xfs_read (void *buf, long len)
{
       xad_t *xad;
       xfs_fileoff_t endofprev, endofcur, offset;
       xfs_filblks_t xadlen;
       int toread, startpos, endpos;

	if (icore.di_format == XFS_DINODE_FMT_LOCAL) {
    		char *dptr = xfs_dfork_dptr(); /* start of literal data/shortform dir */
    		memmove(buf, dptr + filepos, len);
    		filepos += len;
    		return len;
	}
       startpos = filepos;
       endpos = filepos + len;
       endofprev = (xfs_fileoff_t)-1;
       init_extents ();
       while (len > 0 && (xad = next_extent ())) {
               offset = xad->offset;
               xadlen = xad->len;
               if (isinxt (filepos >> xfs.blklog, offset, xadlen)) {
                       endofcur = (offset + xadlen) << xfs.blklog; 
                       toread = (endofcur >= endpos)
                                 ? len : (endofcur - filepos);
                       devread (fsb2daddr (xad->start),
                                filepos - (offset << xfs.blklog), toread, buf);
                       buf += toread;
                       len -= toread;
                       filepos += toread;
               } else if (offset > endofprev) {
                       toread = ((offset << xfs.blklog) >= endpos)
                                 ? len : ((offset - endofprev) << xfs.blklog);
                       len -= toread;
                       filepos += toread;
                       for (; toread; toread--) {
                               *(char*)buf++ = 0;
                       }
                       continue;
               }
               endofprev = offset + xadlen; 
       }

       return filepos - startpos;
}

/*
 * Read block number "blkno".
 */

static int
xfs_bread(int fd, long blkno, long nblks, char *buffer)
{
       long offset, nbytes;

       if (fd != 1) {
               printf("XFS error: bad file descriptor!\n");
               return -1;
       }

       offset = blkno * xfs.bsize;
       if (offset >= filemax)
               return 0;                       /* EOF */

       nbytes = nblks * xfs.bsize;
       if (offset + nbytes > filemax)
               nbytes = filemax - offset;      /* short read at EOF */

       /*
        * aboot asks for whole blocks, so the tail of the last one is not
        * backed by the file.  Zero the buffer rather than hand back
        * whatever was there.
        */
       memset(buffer, 0, nblks * xfs.bsize);

       filepos = offset;
       return xfs_read(buffer, nbytes);
}

/*
 * Unix-like open routine.  Returns a small integer 
 * (does not care what file, we say it's OK)
 */
static int xfs_open(const char *dirname)
{
       xfs_ino_t ino, parent_ino;
       xfs_fsize_t di_size;
       int di_mode;
       int cmp, n, link_count;
       static char linkbuf[MAXNAMELEN];
       char *rest, *name, ch;
       char namebuf[MAXNAMELEN];
       char restbuf[MAXNAMELEN];
       char *filename = namebuf;

       if (strlen(dirname) >= MAXNAMELEN) {
               printf("XFS error: path too long!\n");
               return -1;
       }
       strcpy(namebuf, dirname);


#ifdef DEBUG_XFS
       printf("xfs_open(): filename = %s\n", filename);
#endif


       parent_ino = ino = xfs.rootino;
       link_count = 0;
       for (;;) {
               di_read (ino);
               di_size = be64_to_cpu(icore.di_size);
               di_mode = be16_to_cpu(icore.di_mode);

#ifdef DEBUG_XFS
               printf("xfs_open(): di_mode = %o\n", di_mode);
#endif
               if ((di_mode & IFMT) == IFLNK) {
                       if (++link_count > MAX_LINK_COUNT) {
                               printf("XFS error: symlink loop!\n");
                               return -1;
                       }
                       /*
                        * filename may already point into linkbuf (a
                        * chained symlink), so the remaining path has to be
                        * saved off before xfs_read() overwrites linkbuf
                        * with the new target -- otherwise the append below
                        * copies clobbered bytes instead of the real
                        * remaining path.
                        */
                       strcpy(restbuf, filename);

                       if (di_size < (xfs_fsize_t)(sizeof(linkbuf) - 1)) {
                               filepos = 0;
                               filemax = di_size;
                               n = xfs_read (linkbuf, filemax);
                       } else {
                               printf("XFS error: bad file length!\n");
                               return -1;
                       }

                       ino = (linkbuf[0] == '/') ? xfs.rootino : parent_ino;
                       filename = restbuf;
                       while (n < (int)(sizeof(linkbuf) - 1)
                              && (linkbuf[n++] = *filename++));
                       linkbuf[n] = 0;
                       filename = linkbuf;
                       continue;
               }

               if (!*filename || isspace (*filename)) {
                       if (((di_mode & IFMT) != IFREG)
                           && ((di_mode & IFMT) != IFDIR)) {
                               printf("XFS error: bad file type!\n");
                               return -1;
                       }
                       filepos = 0;
                       filemax = di_size;
                       return 1;
               }

               if ((di_mode & IFMT) != IFDIR) {
                       printf("XFS error: bad file type!\n");
                       return -1;
               }

               for (; *filename == '/'; filename++);

               if (!strcmp(filename,"")) {
                       filepos = 0;
                       filemax = 0;
                       return 1;
               }

               for (rest = filename; (ch = *rest) && !isspace (ch) && ch != '/'; rest++);
               *rest = 0;
#ifdef DEBUG_XFS
               printf("xfs_open(): looking for '%s' in current directory\n", filename);
#endif

               name = first_dentry (&xfs.new_ino);
               if (name == NULL) {
                       *rest = ch;
                       return -1;
               }
               for (;;) {
#ifdef DEBUG_XFS
                       if (name)
                               printf("xfs_open(): candidate='%s' looking_for='%s'\n",
                                      name, filename);
                       else
                               printf("xfs_open(): candidate is NULL while looking_for='%s'\n",
                                      filename);


#endif
                       cmp = (!*filename) ? -1 : strcmp (filename, name);
                       if (cmp == 0) {
                               parent_ino = ino;
                               if (xfs.new_ino)
                                       ino = xfs.new_ino;
                               *(filename = rest) = ch;
                               break;
                       }
                       name = next_dentry (&xfs.new_ino);
                       if (name == NULL) {
                               *rest = ch;
                               return -1;
                       }
               }
       }
}

/*
 * Only one file is opened at any time, so close is a nop.
 */
static void xfs_close(int fd)
{
}

/*
 * Return the next directory entry.
 * Must have opened the directory with xfs_open()
 */
static const char *
xfs_readdir(int fd, int rewind)
{
#ifdef DEBUG_XFS
       printf("xfs_readdir(): fd=%d rewind=%d\n", fd, rewind);
#endif

       if (fd != 1)
               return NULL;
       if ((be16_to_cpu(icore.di_mode) & IFMT) != IFDIR) {
               printf("Not a directory!\n");
               return NULL;
       }
       if (rewind) {
#ifdef DEBUG_XFS
               printf("xfs_readdir(): calling first_dentry()\n");
#endif
       		return first_dentry (&xfs.new_ino);
       }
#ifdef DEBUG_XFS
       printf("xfs_readdir(): calling next_dentry()\n");
#endif
       return next_dentry (&xfs.new_ino);
}

/*
 * Get file status
 */
static int
xfs_fstat(int fd, struct stat* buf)
{
       if (fd != 1)
               return -1;

       memset(buf, 0, sizeof(struct stat));
       buf->st_mode   = be16_to_cpu(icore.di_mode);
       //buf->st_flags  = be16_to_cpu(icore.di_flags);
       buf->st_nlink  = be16_to_cpu(icore.di_onlink);
       buf->st_uid    = be32_to_cpu(icore.di_uid);
       buf->st_gid    = be32_to_cpu(icore.di_gid);
       buf->st_size   = be64_to_cpu(icore.di_size);
       buf->st_blocks = be64_to_cpu(icore.di_nblocks);
       buf->st_atime  = be32_to_cpu(icore.di_atime.t_sec);
       buf->st_mtime  = be32_to_cpu(icore.di_mtime.t_sec);
       buf->st_ctime  = be32_to_cpu(icore.di_ctime.t_sec);
       return 0;
}


