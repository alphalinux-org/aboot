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

#define isspace(c) ((c) == 0x10)

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
       long pos = sector * SECT_SIZE;
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

#ifdef __alpha__ /* take care of alignment*/
 static long FSYS_BUF[32768/sizeof(long)];
 #define dirbuf                ((long *)FSYS_BUF)
 #define filebuf       ((long *)FSYS_BUF + 4096/sizeof(long))
 #define inode         ((xfs_dinode_t *)((long *)FSYS_BUF + 8192/sizeof(long)))
#else
 static char FSYS_BUF[32768];
 #define dirbuf                ((char *)FSYS_BUF)
 #define filebuf       ((char *)FSYS_BUF + 4096)
 #define inode         ((xfs_dinode_t *)((char *)FSYS_BUF + 8192))
#endif

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

/* Are we dealing with dir3-style shortform (v5 + ftype)? */
static inline int
xfs_sf_is_dir3(void)
{
    /* Good enough approximation for aboot: v5 filesystem ⇒ dir3 shortform */
    return xfs.is_v5;   /* or (xfs.is_v5 && icore.di_version >= 3) if you prefer */
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

/* XFS is big endian, alpha is little endian */
#define le16(x) __swab16(x)
#define le32(x) __swab32(x)
#define le64(x) __swab64(x)

static xfs_fsblock_t
xt_start (xfs_bmbt_rec_32_t *r)
{
       return (((xfs_fsblock_t)(le32 (r->l1) & mask32lo(9))) << 43) | 
              (((xfs_fsblock_t)le32 (r->l2)) << 11) |
              (((xfs_fsblock_t)le32 (r->l3)) >> 21);
}

static xfs_fileoff_t
xt_offset (xfs_bmbt_rec_32_t *r)
{
       return (((xfs_fileoff_t)le32 (r->l0) &
               mask32lo(31)) << 23) |
               (((xfs_fileoff_t)le32 (r->l1)) >> 9);
}

static xfs_filblks_t
xt_len (xfs_bmbt_rec_32_t *r)
{
       return le32(r->l3) & mask32lo(21);
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
       return ((xfs_fsblock_t)agno*xfs.agblocks + agbno) << xfs.bdlog;
}

static xfs_daddr_t
fsb2daddr (xfs_fsblock_t fsbno)
{
       return agb2daddr ((xfs_agnumber_t)(fsbno >> xfs.agblklog),
                        (xfs_agblock_t)(fsbno & mask32lo(xfs.agblklog)));
}

#undef offsetof
#define offsetof(t,m)  ((int)&(((t *)0)->m))

static inline int
btroot_maxrecs (void)
{
       int tmp = icore.di_forkoff ? (icore.di_forkoff << 3) : xfs.isize;

       return (tmp - sizeof(xfs_bmdr_block_t) - offsetof(xfs_dinode_t, di_u)) /
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
           le16(inode->di_core.di_magic),
           inode->di_core.di_version,
           inode->di_core.di_format,
           (unsigned long long) le64(inode->di_core.di_size),
           le16(inode->di_core.di_flags),
           xfs.is_v5);
#endif

    /* -------------------------------
     * Basic sanity checks
     * ------------------------------- */
    if (le16(inode->di_core.di_magic) != XFS_DINODE_MAGIC) {
        printf("XFS: bad inode magic 0x%x for ino %lu\n",
               le16(inode->di_core.di_magic),
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

    		xfs.ptr0 = *(xfs_bmbt_ptr_t *)(dfork +
                 sizeof(xfs_bmdr_block_t) +
                 btroot_maxrecs() * sizeof(xfs_bmbt_key_t));
#ifdef DEBUG_XFS
	   printf("di_read(): ino=%lu di_version=%u di_format=%u di_size=%lu di_nextents=%u is_v5=%d\n",
	   (unsigned long long)ino,
           inode->di_core.di_version,
           inode->di_core.di_format,
           (unsigned long long)le64(inode->di_core.di_size),
           le32(inode->di_core.di_nextents),
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
    		//xfs.nextents = le32(icore.di_nextents);
		xfs_dinode_disk_t *dip = (xfs_dinode_disk_t *)inode;

		/* This is what older XFS used for data extents (still correct
		 * on non-NREXT64 filesystems and v1/v2 inodes).
		*/
		uint32_t ne32 = le32(icore.di_nextents);

		/* On v5 filesystems with NREXT64, the 8 bytes where older
		 * layouts had (di_pad[6] + di_flushiter) are now a union that
		 * may contain di_big_nextents (64-bit data extent count).
		 *
		 * Our xfs_dinode_disk_t still has di_pad[6] + di_flushiter,
		 * but that's exactly the same 8-byte region, so we can just
		 * reinterpret it as a BE64 big extent counter.
		 */
		uint64_t be_big = 0;
		memcpy(&be_big, dip->di_pad, sizeof(be_big)); /* di_pad[0..5] + di_flushiter */
		uint64_t big = le64(be_big);

		uint32_t ne;
		if (big != 0) {
			/* NREXT64 case: di_big_nextents is valid and non-zero.
			 * We only need 32 bits in the bootloader; directories on
			 * a boot partition won't have anywhere near 2^32 extents.
			 */
			ne = (uint32_t)big;

		} else {
			 /* Legacy case: use 32-bit di_nextents */
			ne = ne32;
		}
		xfs.xt = (xfs_bmbt_rec_32_t *)xfs_dfork_dptr();
		xfs.nextents = ne;
               break;
       case XFS_DINODE_FMT_BTREE:
               ptr0 = xfs.ptr0;
               for (;;) {
                       xfs.daddr = fsb2daddr (le64(ptr0));
                       devread (xfs.daddr, 0,
                                sizeof(xfs_btree_lblock_t), (char *)&h);
                       if (!h.bb_level) {
                               xfs.nextents = le16(h.bb_numrecs);
                               xfs.next = fsb2daddr (le64(h.bb_rightsib));
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
                       xfs.nextents = le16(h.bb_numrecs);
                       xfs.next = fsb2daddr (le64(h.bb_rightsib));
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
static void
xfs_dabread (void)
{
       xad_t *xad;
       xfs_fileoff_t offset;;

       init_extents ();
       while ((xad = next_extent ())) {
               offset = xad->offset;
               if (isinxt (xfs.dablk, offset, xad->len)) {
                       devread (fsb2daddr (xad->start + xfs.dablk - offset),
                                0, 100, dirbuf);
                       break;
               }
       }
}


/* Get parent inode from shortform header (v4 & v5). */

static inline xfs_ino_t
sf_parent_ino(void)
{
    xfs_dir2_sf_t     *sf  = xfs_sf_dir();
    xfs_dir2_sf_hdr_t *hdr = &sf->hdr;

    return xfs_dir2_sf_get_ino(hdr, &hdr->parent);
}


static void debug_dump_sf_bytes(void)
{
    unsigned char *p = (unsigned char *)xfs_sf_dir();
    int i;

    printf("SF RAW DUMP (first 128 bytes):\n");

    for (i = 0; i < 128; i++) {
        printf("%02x ", p[i]);
        if ((i % 16) == 15)
            printf("\n");
    }
    printf("\n");
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
    int has_ftype = xfs.is_v5;
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
    magic = le32(magic);

    if (magic != XFS_DIR3_DATA_MAGIC && magic != XFS_DIR3_BLOCK_MAGIC) {
        printf("xfs: unsupported DIR3 block magic: 0x%x\n", magic);
        return 0;
    }

    /* Skip v5 data header */
    filepos    = sizeof(xfs_dir3_data_hdr_t);
    xfs.blkoff = filepos;

    /* --- Find block tail to know where the leaf area starts --- */
    {
        long saved_pos  = filepos;
        long saved_off  = xfs.blkoff;

        /* Tail is at end of block */
        filepos = xfs.dirbsize - sizeof(xfs_dir3_block_tail_t);
        if (xfs_read(&tail, sizeof(tail)) != sizeof(tail)) {
            printf("xfs: failed to read dir3 block tail\n");
            return 0;
        }

        /* Convert endianness */
        uint32_t leaf_count = le32(tail.count);
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
        if (le16(u.unused.freetag) == XFS_DIR2_DATA_FREE_TAG) {
            uint16_t len = le16(u.unused.length);
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
            xfs_dabread();

#define h ((xfs_dir2_leaf_hdr_t *)dirbuf)
            xfs.dirmax = le16(h->count) - le16(h->stale);
            xfs.forw   = le32(h->info.forw);
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
           (long long)le64(icore.di_size),
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

    /* On v5/ftype=1 shortform dirs, name[namelen] is ftype, so skip it. */
    if (xfs.is_v5)
        p++;

    if (hdr->i8count) {
        /* 8-byte inode */
        memcpy(&ino, p, sizeof(xfs_dir2_ino8_t));
        ino = le64(ino) & 0x00ffffffffffffffULL;
    } else {
        uint32_t v32;
        memcpy(&v32, p, sizeof(xfs_dir2_ino4_t));
        ino = le32(v32);
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
    return xfs.is_v5 ? xfs_dir3_sf_entsize(hdr, namelen)
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
        return (xfs_ino_t)le64(*(xfs_ino_t *)p);
    else
        return (xfs_ino_t)le32(*(uint32_t *)p);
}



/* Decode an xfs_dir2_inou_t -> xfs_ino_t (v4/v5) */
static inline xfs_ino_t
xfs_dir2_sf_get_ino(xfs_dir2_sf_hdr_t *hdr, xfs_dir2_inou_t *from)
{
    xfs_ino_t ino = 0;

    if (hdr->i8count) {
        /* 64-bit inode stored big-endian in from->i8.i[8] */
        memcpy(&ino, &from->i8, sizeof(xfs_dir2_ino8_t));
        ino = le64(ino);                        /* swap from disk BE to CPU */
        ino &= 0x00ffffffffffffffULL;           /* top byte reserved */
    } else {
        uint32_t v = 0;
        memcpy(&v, &from->i4, sizeof(xfs_dir2_ino4_t));
        ino = le32(v);
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
    int has_ftype = xfs.is_v5;   /* ftype present on your fs */

#define d2u ((xfs_dir2_data_union_t *)dirbuf)

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
            magic = le32(magic);

            if (magic != XFS_DIR3_DATA_MAGIC &&
                magic != XFS_DIR3_BLOCK_MAGIC) {
                printf("xfs: invalid DIR3 block magic: 0x%x\n", magic);
                return NULL;
            }

            /* Skip full v5 header. */
            xfs.blkoff = sizeof(xfs_dir3_data_hdr_t);
            filepos   |= xfs.blkoff;
        }
	/* Stop when we reach the tail (v5 tail, 12 bytes, not the v4 8-byte one) */
if (xfs.blkoff + sizeof(xfs_dir3_block_tail_t) >= xfs.dirbsize)
    return NULL;

/* Read first 4 bytes of the next slot */
if (xfs_read(dirbuf, 4) != 4) {
    /* This is end-of-dir, not an error */
    return NULL;
}
        xfs.blkoff += 4;

        /* Free entry? */
        if (le16(d2u->unused.freetag) == XFS_DIR2_DATA_FREE_TAG) {
            uint16_t len = le16(d2u->unused.length);
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

    *ino = le64(d2u->entry.inumber);
    int namelen = d2u->entry.namelen;

    /* Skip bogus entries. */
    if (*ino == 0 || namelen == 0) {
        xfs.dirpos++;
        return next_dentry_dir3(ino);
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

#undef d2u
    return name;
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

    uint32_t magic = le32(((xfs_dir2_data_hdr_t *)dirbuf)->magic);

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
        xfs.dirmax = le32(tail->count) - le32(tail->stale);

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

        for (;;)
        {
            xfs_dabread();

            xfs_dir2_leaf_hdr_t *lh = (xfs_dir2_leaf_hdr_t *)dirbuf;
            xfs_da_intnode_t    *n  = (xfs_da_intnode_t *)dirbuf;

            uint16_t m = le16(n->hdr.info.magic);

#ifdef DEBUG_XFS_2
            printf("DIR2 leaf/node scan: magic=0x%x\n", m);
#endif

            if (m == XFS_DIR2_LEAF1_MAGIC || m == XFS_DIR2_LEAFN_MAGIC)
            {
                xfs.dirmax = le16(lh->count) - le16(lh->stale);
                xfs.forw   = le32(lh->info.forw);

#ifdef DEBUG_XFS_2
                printf("DIR2 leaf-format: dirmax=%d forw=%u\n",
                       xfs.dirmax, xfs.forw);
#endif
                break;
            }

            /* Follow B-tree "before" pointer */
            xfs.dablk = le32(n->btree[0].before);
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
            magic = le32(magic);

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

            uint32_t len = le16(dau->unused.length);

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

    *ino    = le64(dau->entry.inumber);
    namelen = dau->entry.namelen;

    /* Skip bogus entries */
    if (*ino == 0 || namelen == 0) {
        xfs.dirpos++;
        return next_dentry_dir2(ino);
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
    xfs.blkoff += toread + 5;
    xfs.dirpos++;

#undef dau

    return name;
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
       (unsigned long long) le64(inode->di_core.di_size),
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
        magic = le32(magic);

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
               printf("xfs_mount: read_disk_block failed!\n");
               return -1;
       } else if (le32(super.sb_magicnum) != XFS_SB_MAGIC) {
               printf("xfs_mount: Bad magic: %x\n", super.sb_magicnum);
               return -1;
       } else {
        unsigned int ver = le16(super.sb_versionnum) & XFS_SB_VERSION_NUMBITS;

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
        printf("xfs_mount: unsupported XFS version %u\n", ver);
        return -1;
    }

 
       }

       xfs.bsize = le32 (super.sb_blocksize);
       xfs.blklog = super.sb_blocklog;
       xfs.bdlog = xfs.blklog - SECTOR_BITS;
       xfs.rootino = le64 (super.sb_rootino);
       xfs.isize = le16 (super.sb_inodesize);
       xfs.agblocks = le32 (super.sb_agblocks);
       xfs.dirbsize = xfs.bsize << super.sb_dirblklog;

       xfs.inopblog = super.sb_inopblog;
       xfs.agblklog = super.sb_agblklog;
       xfs.agnolog = xfs_highbit32 (le32(super.sb_agcount));

       xfs.btnode_ptr0_off =
               ((xfs.bsize - sizeof(xfs_btree_block_t)) /
               (sizeof (xfs_bmbt_key_t) + sizeof (xfs_bmbt_ptr_t)))
                * sizeof(xfs_bmbt_key_t) + sizeof(xfs_btree_block_t);

#ifdef DEBUG_XFS
       printf("XFS: version   = %d\n",le16(super.sb_versionnum) & XFS_SB_VERSION_NUMBITS);
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
       if (fd == 1) {
               /*
                * Duh. XFS doesn't read past EOF
                * aboot does just that by trying to read nblks*blksize,
                * where nblks*blksize > filesize
                */
               memset(buffer,0,nblks*xfs.bsize);
               long nbytes = xfs_read(buffer, nblks*xfs.bsize);
               if (nbytes == le64(icore.di_size))
                       return nblks*xfs.bsize;
               return (int)nbytes;
       }
       printf("XFS error: bad file descriptor!\n");
       return -1;
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
       char linkbuf[xfs.bsize];
       char *rest, *name, ch;
       char namebuf[MAXNAMELEN];
       strncpy(namebuf,dirname,MAXNAMELEN);
       char *filename = namebuf;


#ifdef DEBUG_XFS
       printf("xfs_open(): filename = %s\n", filename);
#endif


       parent_ino = ino = xfs.rootino;
       link_count = 0;
       for (;;) {
               di_read (ino);
               di_size = le64 (icore.di_size);
               di_mode = le16 (icore.di_mode);

#ifdef DEBUG_XFS
               printf("xfs_open(): di_mode = %o\n", di_mode);
#endif
               if ((di_mode & IFMT) == IFLNK) {
                       if (++link_count > MAX_LINK_COUNT) {
                               printf("XFS error: symlink loop!\n");
                               return 0;
                       }
                       if (di_size < xfs.bsize - 1) {
                               filepos = 0;
                               filemax = di_size;
                               n = xfs_read (linkbuf, filemax);
                       } else {
                               printf("XFS error: bad file length!\n");
                               return 0;
                       }

                       ino = (linkbuf[0] == '/') ? xfs.rootino : parent_ino;
                       while (n < (xfs.bsize - 1) && (linkbuf[n++] = *filename++));
                       linkbuf[n] = 0;
                       filename = linkbuf;
                       continue;
               }

               if (!*filename || isspace (*filename)) {
                       if (((di_mode & IFMT) != IFREG)
                           && ((di_mode & IFMT) != IFDIR)) {
                               printf("XFS error: bad file type!\n");
                               return 0;
                       }
                       filepos = 0;
                       filemax = di_size;
                       return 1;
               }

               if ((di_mode & IFMT) != IFDIR) {
                       printf("XFS error: bad file type!\n");
                       return 0;
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
       if ((le16 (icore.di_mode) & IFMT) != IFDIR) {
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
       buf->st_mode   = le16(icore.di_mode);
       //buf->st_flags  = le16(icore.di_flags);
       buf->st_nlink  = le16(icore.di_onlink);
       buf->st_uid    = le32(icore.di_uid);
       buf->st_gid    = le32(icore.di_gid);
       buf->st_size   = le64(icore.di_size);
       buf->st_blocks = le64(icore.di_nblocks);
       buf->st_atime  = le32(icore.di_atime.t_sec);
       buf->st_mtime  = le32(icore.di_mtime.t_sec);
       buf->st_ctime  = le32(icore.di_ctime.t_sec);
       return 0;
}


