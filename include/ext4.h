/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */

#ifndef _EXT4_H_
#define _EXT4_H_


/* --- ext4 superblock feature flags we care about --- */

/* Incompatible features (s_feature_incompat) */
#ifndef EXT4_FEATURE_INCOMPAT_FILETYPE
#define EXT4_FEATURE_INCOMPAT_FILETYPE      0x0002
#endif
#ifndef EXT4_FEATURE_INCOMPAT_RECOVER
#define EXT4_FEATURE_INCOMPAT_RECOVER       0x0004
#endif
#ifndef EXT4_FEATURE_INCOMPAT_META_BG
#define EXT4_FEATURE_INCOMPAT_META_BG       0x0010
#endif
#ifndef EXT4_FEATURE_INCOMPAT_EXTENTS
#define EXT4_FEATURE_INCOMPAT_EXTENTS       0x0040
#endif
#ifndef EXT4_FEATURE_INCOMPAT_64BIT
#define EXT4_FEATURE_INCOMPAT_64BIT         0x0080
#endif
#ifndef EXT4_FEATURE_INCOMPAT_MMP
#define EXT4_FEATURE_INCOMPAT_MMP           0x0100
#endif
#ifndef EXT4_FEATURE_INCOMPAT_FLEX_BG
#define EXT4_FEATURE_INCOMPAT_FLEX_BG       0x0200
#endif

/* RO-compat features (s_feature_ro_compat) */
#ifndef EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_LARGE_FILE
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_BTREE_DIR
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR    0x0004
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_HUGE_FILE
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x0008
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_GDT_CSUM
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x0010
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_DIR_NLINK
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK    0x0020
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x0040
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_BIGALLOC
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC     0x0200
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_QUOTA
#define EXT4_FEATURE_RO_COMPAT_QUOTA        0x0100
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_METADATA_CSUM
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#endif

#ifndef EXT4_FEATURE_INCOMPAT_CSUM_SEED
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000
#endif

/*
 * Inode flags
 */
#define EXT4_EXTENTS_FL	0x00080000 /* Inode uses extents */

/*
 * ext4_inode has i_block array (60 bytes total).
 * The first 12 bytes store ext4_extent_header;
 * the remainder stores an array of ext4_extent.
 */

/*
 * This is the extent on-disk structure.
 * It's used at the bottom of the tree.
 */
struct ext4_extent {
	__le32	ee_block;	/* first logical block extent covers */
	__le16	ee_len;		/* number of blocks covered by extent */
	__le16	ee_start_hi;	/* high 16 bits of physical block */
	__le32	ee_start_lo;	/* low 32 bits of physical block */
};

/*
 * This is index on-disk structure.
 * It's used at all the levels except the bottom.
 */
struct ext4_extent_idx {
	__le32	ei_block;	/* index covers logical blocks from 'block' */
	__le32	ei_leaf_lo;	/* pointer to the physical block of the next *
				 * level. leaf or next index could be there */
	__le16	ei_leaf_hi;	/* high 16 bits of physical block */
	__u16	ei_unused;
};

/*
 * Each block (leaves and indexes), even inode-stored has header.
 */
struct ext4_extent_header {
	__le16	eh_magic;	/* probably will support different formats */
	__le16	eh_entries;	/* number of valid entries */
	__le16	eh_max;		/* capacity of store in entries */
	__le16	eh_depth;	/* has tree real underlying blocks? */
	__le32	eh_generation;	/* generation of the tree */
};

#define EXT4_EXT_MAGIC	0xf30a

/* An ee_len above this marks the extent uninitialized. */
#define EXT_INIT_MAX_LEN	32768

#endif
