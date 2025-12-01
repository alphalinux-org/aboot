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

#include <asm/system.h>
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

static long dev = -1;
static long partition_offset;
static long filepos;
static long filemax; /* filelen */

static long xfs_read (void *buf, long len);

#define isspace(c) ((c) == 0x10)

static int
devread(long sector, long start, long length, void *buf)
{
       long pos = sector * SECT_SIZE;
       pos += partition_offset + start;
#ifdef DEBUG_XFS
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
};

static struct xfs_info xfs;

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

static int
di_read (xfs_ino_t ino)
{
       xfs_agino_t agino;
       xfs_agnumber_t agno;
       xfs_agblock_t agbno;
       xfs_daddr_t daddr;
       int offset;

       agno = ino2agno (ino);
       agino = ino2agino (ino);
       agbno = agino2agbno (agino);
       offset = ino2offset (ino);
       daddr = agb2daddr (agno, agbno);

       devread (daddr, offset*xfs.isize, xfs.isize, (char *)inode);

       xfs.ptr0 = *(xfs_bmbt_ptr_t *)
                   (inode->di_u.di_c + sizeof(xfs_bmdr_block_t)
                   + btroot_maxrecs ()*sizeof(xfs_bmbt_key_t));

       return 1;
}

static void
init_extents (void)
{
       xfs_bmbt_ptr_t ptr0;
       xfs_btree_lblock_t h;

       switch (icore.di_format) {
       case XFS_DINODE_FMT_EXTENTS:
               xfs.xt = inode->di_u.di_bmx;
               xfs.nextents = le32 (icore.di_nextents);
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

static inline xfs_ino_t
sf_ino (char *sfe, int namelen)
{
       void *p = sfe + namelen + 3;
#ifdef __alpha__
       xfs_ino_t ino = 0;
       if (xfs.i8param == 0) {
               memcpy(&ino, p, sizeof(xfs_dir2_ino8_t));
               return le64(ino);
       } else {
               memcpy(&ino, p, sizeof(xfs_dir2_ino4_t));
               return le32(ino);
       }
#else
       /* unaligned access */
       return (xfs.i8param == 0)
               ? le64(*(xfs_ino_t *)p) : le32(*(uint32_t *)p);
#endif
}

static inline xfs_ino_t
sf_parent_ino (void)
{
#ifdef __alpha__
       void *p = &inode->di_u.di_dir2sf.hdr.parent;
       xfs_ino_t ino = 0;
       if (xfs.i8param == 0) {
               memcpy(&ino, p, sizeof(xfs_dir2_ino8_t));
               return le64(ino);
       } else {
               memcpy(&ino, p, sizeof(xfs_dir2_ino4_t));
               return le32(ino);
       }
#else
       /* unaligned access */
       return (xfs.i8param == 0)
               ? le64(*(xfs_ino_t *)(&inode->di_u.di_dir2sf.hdr.parent))
               : le32(*(uint32_t *)(&inode->di_u.di_dir2sf.hdr.parent));
#endif
}

static inline int
roundup8 (int n)
{
       return ((n+7)&~7);
}

static char *
next_dentry (xfs_ino_t *ino)
{
       int namelen;
       int toread;
       static xfs_dir2_sf_entry_t *sfe;
       char *name = NULL;

       if (xfs.dirpos >= xfs.dirmax) {
               if (xfs.forw == 0)
                       return NULL;
               xfs.dablk = xfs.forw;
               xfs_dabread ();
#define h      ((xfs_dir2_leaf_hdr_t *)dirbuf)
               xfs.dirmax = le16 (h->count) - le16 (h->stale);
               xfs.forw = le32 (h->info.forw);
#undef h
               xfs.dirpos = 0;
       }

       switch (icore.di_format) {
       case XFS_DINODE_FMT_LOCAL:
               switch (xfs.dirpos) {
               case -2:
                       *ino = 0;
                       name = ".";
                       namelen = 1;
                       break;
               case -1: /* ".." */
                       *ino = sf_parent_ino ();
                       name = "..";
                       namelen = 2;
                       sfe = (xfs_dir2_sf_entry_t *)
                               (inode->di_u.di_c 
                                + sizeof(xfs_dir2_sf_hdr_t)
                                - xfs.i8param);
                       break;
               default:
                       namelen = sfe->namelen;
                       *ino = sf_ino ((char *)sfe, namelen);
                       name = sfe->name;
                       name[namelen] = 0;
                       sfe = (xfs_dir2_sf_entry_t *)
                                 ((char *)sfe + namelen + 11 - xfs.i8param);
               }
               break;
       case XFS_DINODE_FMT_BTREE:
       case XFS_DINODE_FMT_EXTENTS:
#define dau    ((xfs_dir2_data_union_t *)dirbuf)
               for (;;) {
                       if (xfs.blkoff >= xfs.dirbsize) {
                               xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
                               filepos &= ~(xfs.dirbsize - 1);
                               filepos |= xfs.blkoff;
                       }
                       xfs.blkoff += 4;
                       if (dau->unused.freetag == XFS_DIR2_DATA_FREE_TAG) {
                               toread = roundup8 (le16(dau->unused.length)) - 4;
                               xfs.blkoff += toread;
                               filepos += toread;
                               continue;
                       }
                       break;
               }
               xfs_read ((char *)dirbuf + 4, 5);
               *ino = le64 (dau->entry.inumber);
               namelen = dau->entry.namelen;
#undef dau
               toread = roundup8 (namelen + 11) - 9;
               xfs_read (dirbuf, toread);
               name = (char *)dirbuf;
               name[namelen] = 0;
               xfs.blkoff += toread + 5;
       }
       ++xfs.dirpos;

       return name;
}

static char *
first_dentry (xfs_ino_t *ino)
{
       xfs.forw = 0;
       switch (icore.di_format) {
       case XFS_DINODE_FMT_LOCAL:
               xfs.dirmax = inode->di_u.di_dir2sf.hdr.count;
               xfs.i8param = inode->di_u.di_dir2sf.hdr.i8count ? 0 : 4;
               xfs.dirpos = -2;
               break;
       case XFS_DINODE_FMT_EXTENTS:
       case XFS_DINODE_FMT_BTREE:
               filepos = 0;
               xfs_read (dirbuf, sizeof(xfs_dir2_data_hdr_t));
               if (((xfs_dir2_data_hdr_t *)dirbuf)->magic == le32(XFS_DIR2_BLOCK_MAGIC)) {
#define tail           ((xfs_dir2_block_tail_t *)dirbuf)
                       filepos = xfs.dirbsize - sizeof(*tail);
                       xfs_read (dirbuf, sizeof(*tail));
                       xfs.dirmax = le32 (tail->count) - le32 (tail->stale);
#undef tail
               } else {
                       xfs.dablk = (1ULL << 35) >> xfs.blklog;
#define h              ((xfs_dir2_leaf_hdr_t *)dirbuf)
#define n              ((xfs_da_intnode_t *)dirbuf)
                       for (;;) {
                               xfs_dabread ();
                               if ((n->hdr.info.magic == le16(XFS_DIR2_LEAFN_MAGIC))
                                   || (n->hdr.info.magic == le16(XFS_DIR2_LEAF1_MAGIC))) {
                                       xfs.dirmax = le16 (h->count) - le16 (h->stale);
                                       xfs.forw = le32 (h->info.forw);
                                       break;
                               }
                               xfs.dablk = le32 (n->btree[0].before);
                       }
#undef n
#undef h
               }
               xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
               filepos = xfs.blkoff;
               xfs.dirpos = 0;
       }
       return next_dentry (ino);
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
       } else if ((le16(super.sb_versionnum) & XFS_SB_VERSION_NUMBITS) != XFS_SB_VERSION_4) {
               printf("xfs_mount: Bad version: %x\n", super.sb_versionnum);
               return -1;
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

#ifdef DEBUG
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
               memmove (buf, inode->di_u.di_c + filepos, len);
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

               name = first_dentry (&xfs.new_ino);
               for (;;) {
#ifdef DEBUG
                       printf("xfs_open(): found %s\n", name);
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
       if (fd != 1)
               return NULL;
       if ((le16 (icore.di_mode) & IFMT) != IFDIR) {
               printf("Not a directory!\n");
               return NULL;
       }
       if (rewind)
               return first_dentry (&xfs.new_ino);
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
       buf->st_flags  = le16(icore.di_flags);
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


