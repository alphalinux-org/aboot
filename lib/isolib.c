/*
 * This code is based on the ISO filesystem support in MILO (by
 * Dave Rusling).
 *
 * This is a set of functions that provides minimal filesystem
 * functionality to the Linux bootstrapper.  All we can do is
 * open and read files... but that's all we need 8-)
 */
#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>
#include "iso.h"
#include "isolib.h"
#include "utils.h"

/* iso9660 support code */

#define MAX_OPEN_FILES 5

static struct inode_table_entry {
    struct iso_inode inode;
    int inumber;
    int free;
    unsigned short old_mode;
    unsigned size;
    int nlink;
    int mode;
    void *start;
} inode_table[MAX_OPEN_FILES];

static unsigned long root_inode = 0;
static struct isofs_super_block sb;
static char data_block[1024];
static char big_data_block[2048];

#ifndef S_IRWXUGO
# define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
# define S_IXUGO	(S_IXUSR|S_IXGRP|S_IXOTH)
# define S_IRUGO	(S_IRUSR|S_IRGRP|S_IROTH)
#endif

extern long iso_dev_read (void * buf, long offset, long size);

static int parse_rock_ridge_inode(struct iso_directory_record * de,
				  struct iso_inode * inode);
static char *get_rock_ridge_symlink(struct iso_inode *inode);
static int get_rock_ridge_filename(struct iso_directory_record * de,
				   char * retname,
				   struct iso_inode * inode);

int
isonum_711 (char * p)
{
	return (*p & 0xff);
}


int
isonum_712 (char * p)
{
	int val;

	val = *p;
	if (val & 0x80)
		val |= 0xffffff00;
	return val;
}


int
isonum_721 (char * p)
{
	return ((p[0] & 0xff) | ((p[1] & 0xff) << 8));
}

int
isonum_722 (char * p)
{
	return (((p[0] & 0xff) << 8) | (p[1] & 0xff));
}


int
isonum_723 (char * p)
{
	return isonum_721(p);
}


int
isonum_731 (char * p)
{
	return ((p[0] & 0xff)
		| ((p[1] & 0xff) << 8)
		| ((p[2] & 0xff) << 16)
		| ((p[3] & 0xff) << 24));
}


int
isonum_732 (char * p)
{
	return (((p[0] & 0xff) << 24)
		| ((p[1] & 0xff) << 16)
		| ((p[2] & 0xff) << 8)
		| (p[3] & 0xff));
}


int
isonum_733 (char * p)
{
	return isonum_731(p);
}


static int
iso_bmap (struct iso_inode *inode, int block)
{
	if (block < 0) {
		printf("iso_bmap: block<0");
		return 0;
	}
	return (inode->i_first_extent >> sb.s_blocksize_bits) + block;
}

static int
iso_breadi (struct iso_inode *ip, long blkno, long nblks, char * buffer)
{
	long i_size, abs_blkno;

	/* do some error checking */
	if (!ip || !ip->i_first_extent)
		return -1;

	i_size = ((struct inode_table_entry *) ip)->size;
	/* as in ext2.c - cons_read() doesn't really cope well with
           EOF conditions - actually it should be fixed */
	if ((blkno+nblks) * sb.s_blocksize > i_size)
		nblks = ((i_size + sb.s_blocksize)
			 / sb.s_blocksize) - blkno;

	/* figure out which iso block number(s) we're being asked for */
	abs_blkno = iso_bmap(ip, blkno);
	if (!abs_blkno)
		return -1;
	/* now try and read them (easy since ISO files are continguous) */
	return iso_dev_read(buffer, abs_blkno * sb.s_blocksize,
			    nblks * sb.s_blocksize);
}


/*
 * Release our hold on an inode.  Since this is a read-only application,
 * don't worry about putting back any changes...
 */
static void
iso_iput (struct iso_inode *ip)
{
	struct inode_table_entry *itp;

	/* Find and free the inode table slot we used... */
	itp = (struct inode_table_entry *) ip;

	itp->inumber = 0;
	itp->free = 1;
}

/*
 * Read the specified inode from the disk and return it to the user.
 * Returns NULL if the inode can't be read...
 *
 * Uses data_block
 */
static struct iso_inode *
iso_iget (int ino)
{
	int i;
	struct iso_inode *inode;
	struct inode_table_entry *itp;
	struct iso_directory_record * raw_inode;
	unsigned char *pnt = NULL;
	void *cpnt = NULL;
	int high_sierra;
	int block;

#ifdef DEBUG_ISO
	printf("iso_iget(ino=%d)\n", ino);
#endif

	/* find a free inode to play with */
	inode = NULL;
	itp = NULL;
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		if (inode_table[i].free) {
			itp = &(inode_table[i]);
			inode = &(itp->inode);
			break;
		}
	}
	if ((inode == NULL) || (itp == NULL)) {
		printf("iso9660 (iget): no free inodes\n");
		return (NULL);
	}

	block = ino >> sb.s_blocksize_bits;
	if (iso_dev_read(data_block, block * sb.s_blocksize, sb.s_blocksize)
	    != sb.s_blocksize) {
		printf("iso9660: unable to read i-node block");
		return NULL;
	}

	pnt = ((unsigned char *) data_block + (ino & (sb.s_blocksize - 1)));
	raw_inode = ((struct iso_directory_record *) pnt);
	high_sierra = sb.s_high_sierra;

	if ((ino & (sb.s_blocksize - 1)) + *pnt > sb.s_blocksize){
		int frag1, offset;

		offset = (ino & (sb.s_blocksize - 1));
		frag1 = sb.s_blocksize - offset;
		cpnt = big_data_block;
		memcpy(cpnt, data_block + offset, frag1);
		offset += *pnt - sb.s_blocksize; /* DUH! pnt would get
                                                    wiped out by the
                                                    iso_dev_read here. */
		if (iso_dev_read(data_block, ++block * sb.s_blocksize,
				 sb.s_blocksize)
		    != sb.s_blocksize) {
			printf("unable to read i-node block");
			return NULL;
		}
		memcpy((char *)cpnt+frag1, data_block, offset);
		pnt = ((unsigned char *) cpnt);
		raw_inode = ((struct iso_directory_record *) pnt);
	}

	if (raw_inode->flags[-high_sierra] & 2) {
		itp->mode = S_IRUGO | S_IXUGO | S_IFDIR;
		itp->nlink = 1; /* Set to 1.  We know there are 2, but
				   the find utility tries to optimize
				   if it is 2, and it screws up.  It is
				   easier to give 1 which tells find to
				   do it the hard way. */
	} else {
		itp->mode = sb.s_mode; /* Everybody gets to read the file. */
		itp->nlink = 1;
		itp->mode |= S_IFREG;
		/*
		 * If there are no periods in the name, then set the
		 * execute permission bit
		 */
		for(i=0; i< raw_inode->name_len[0]; i++)
			if(raw_inode->name[i]=='.' || raw_inode->name[i]==';')
				break;
		if(i == raw_inode->name_len[0] || raw_inode->name[i] == ';')
			itp->mode |= S_IXUGO; /* execute permission */
	}

	itp->size = isonum_733 (raw_inode->size);

	/* There are defective discs out there - we do this to protect
	   ourselves.  A cdrom will never contain more than 700Mb */
	if((itp->size < 0 || itp->size > 700000000) &&
	   sb.s_cruft == 'n')
	{
		printf("Warning: defective cdrom.  "
		       "Enabling \"cruft\" mount option.\n");
		sb.s_cruft = 'y';
	}

	/*
	 * Some dipshit decided to store some other bit of information
	 * in the high byte of the file length.  Catch this and
	 * holler.  WARNING: this will make it impossible for a file
	 * to be > 16Mb on the CDROM!!!
	 */
	if(sb.s_cruft == 'y' &&
	   itp->size & 0xff000000)
	{
		itp->size &= 0x00ffffff;
	}

	if (raw_inode->interleave[0]) {
		printf("Interleaved files not (yet) supported.\n");
		itp->size = 0;
	}

	/* I have no idea what file_unit_size is used for, so
	   we will flag it for now */
	if (raw_inode->file_unit_size[0] != 0){
		printf("File unit size != 0 for ISO file (%d).\n", ino);
	}

	/* I have no idea what other flag bits are used for, so
	   we will flag it for now */
#ifdef DEBUG_ISO
	if ((raw_inode->flags[-high_sierra] & ~2)!= 0){
		printf("Unusual flag settings for ISO file (%d %x).\n",
		       ino, raw_inode->flags[-high_sierra]);
	}
#endif

	inode->i_first_extent = (isonum_733 (raw_inode->extent) +
				 isonum_711 (raw_inode->ext_attr_length))
					 << sb.s_log_zone_size;

	/* Now we check the Rock Ridge extensions for further info */

	if (sb.s_rock)
		parse_rock_ridge_inode(raw_inode,inode);

	/* Will be used for previous directory */
	inode->i_backlink = 0xffffffff;
	switch (sb.s_conversion) {
	      case 'a':
		inode->i_file_format = ISOFS_FILE_UNKNOWN; /* File type */
		break;
	      case 'b':
		inode->i_file_format = ISOFS_FILE_BINARY; /* File type */
		break;
	      case 't':
		inode->i_file_format = ISOFS_FILE_TEXT; /* File type */
		break;
	      case 'm':
		inode->i_file_format = ISOFS_FILE_TEXT_M; /* File type */
		break;
	}

	/* keep our inode table correct */
	itp->free = 0;
	itp->inumber = ino;

	/* return a pointer to it */
	return inode;
}


/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use iso_match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, iso_match returns 1 for success, 0 for failure.
 */
static int
iso_match (int len, const char *name, const char *compare, int dlen)
{
	if (!compare)
		return 0;

#ifdef DEBUG_ISO
	printf("iso_match: comparing %d chars of %s with %s\n",
	       dlen, name, compare);
#endif

	/* check special "." and ".." files */
	if (dlen == 1) {
		/* "." */
		if (compare[0] == 0) {
			if (!len)
				return 1;
			compare = ".";
		} else if (compare[0] == 1) {
			compare = "..";
			dlen = 2;
		}
	}
	if (dlen != len)
		return 0;
	return !memcmp(name, compare, len);
}

/*
 * Find an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as an inode number). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * uses data_block
 */
static int
iso_find_entry (struct iso_inode *dir, const char *name, int namelen,
		unsigned long *ino, unsigned long *ino_back)
{
	unsigned long bufsize = sb.s_blocksize;
	unsigned char bufbits = sb.s_blocksize_bits;
	unsigned int block, f_pos, offset, inode_number = 0; /*shut up, gcc*/
	void * cpnt = NULL;
	unsigned int old_offset;
	unsigned int backlink;
	int dlen, match, i;
	struct iso_directory_record * de;
	char c;
	struct inode_table_entry *itp = (struct inode_table_entry *) dir;

	*ino = 0;
	if (!dir) return -1;

	if (!(block = dir->i_first_extent)) return -1;

	f_pos = 0;
	offset = 0;
	block = iso_bmap(dir,f_pos >> bufbits);
	if (!block) return -1;

	if (iso_dev_read(data_block, block * sb.s_blocksize, sb.s_blocksize)
	    != sb.s_blocksize) return -1;

	while (f_pos < itp->size) {
		de = (struct iso_directory_record *) (data_block + offset);
		backlink = itp->inumber;
		inode_number = (block << bufbits) + (offset & (bufsize - 1));

		/* If byte is zero, this is the end of file, or time to move to
		   the next sector. Usually 2048 byte boundaries. */

		if (*((unsigned char *) de) == 0) {
			offset = 0;

			/* round f_pos up to the nearest blocksize */
			f_pos = ((f_pos & ~(ISOFS_BLOCK_SIZE - 1))
				 + ISOFS_BLOCK_SIZE);
			block = iso_bmap(dir,f_pos>>bufbits);
			if (!block) return -1;
			if (iso_dev_read(data_block,
					 block * sb.s_blocksize,
					 sb.s_blocksize)
				!= sb.s_blocksize) return -1;
  			continue; /* Will kick out if past end of directory */
		}

		old_offset = offset;
		offset += *((unsigned char *) de);
		f_pos += *((unsigned char *) de);

		/* Handle case where the directory entry spans two blocks.
		   Usually 1024 byte boundaries */
		if (offset >= bufsize) {
		        unsigned int frag1;
			frag1 = bufsize - old_offset;
			cpnt = big_data_block;
			memcpy(cpnt, data_block + old_offset, frag1);

			de = (struct iso_directory_record *) cpnt;
			offset = f_pos & (bufsize - 1);
			block = iso_bmap(dir,f_pos>>bufbits);
			if (!block) return -1;
                        if (iso_dev_read(data_block,
					 block * sb.s_blocksize,
					 sb.s_blocksize)
				!= sb.s_blocksize) return 0;
			memcpy((char *)cpnt+frag1, data_block, offset);
		}

		/* Handle the '.' case */

		if (de->name[0]==0 && de->name_len[0]==1) {
		  inode_number = itp->inumber;
		  backlink = 0;
		}

		/* Handle the '..' case */

		if (de->name[0]==1 && de->name_len[0]==1) {

		  if((int) sb.s_firstdatazone != itp->inumber)
  		    inode_number = (isonum_733(de->extent) +
			  	    isonum_711(de->ext_attr_length))
				    << sb.s_log_zone_size;
		  else
		    inode_number = itp->inumber;
		  backlink = 0;
		}

		{
		  /* should be sufficient, since get_rock_ridge_filename
		   * truncates at 254 chars */
		  char retname[256];
		  dlen = get_rock_ridge_filename(de, retname, dir);
		  if (dlen) {
		    strcpy(de->name, retname);
		  } else {
		    dlen = isonum_711((char *) de->name_len);
		    if(sb.s_mapping == 'n') {
		      for (i = 0; i < dlen; i++) {
			c = de->name[i];
			if (c >= 'A' && c <= 'Z') c |= 0x20;  /* lower case */
			if (c == ';' && i == dlen-2
			    && de->name[i+1] == '1') {
			  dlen -= 2;
			  break;
			}
			if (c == ';') c = '.';
			de->name[i] = c;
		      }
		      /* This allows us to match with and without a trailing
			 period.  */
		      if(de->name[dlen-1] == '.' && namelen == dlen-1)
			dlen--;
		    }
		  }
		}
		/*
		 * Skip hidden or associated files unless unhide is set
		 */
		match = 0;
		if(   !(de->flags[-sb.s_high_sierra] & 5)
		   || sb.s_unhide == 'y' )
		{
		  match = iso_match(namelen, name, de->name, dlen);
		}

		if (cpnt) cpnt = NULL;

		if (match) {
		  if ((int) inode_number == -1) {
		    /* Should never happen */
		    printf("iso9660: error inode_number = -1\n");
		    return -1;
		  }

		  *ino = inode_number;
		  *ino_back = backlink;
#ifdef DEBUG_ISO
		  printf("iso_find_entry returning successfully (ino = %d)\n",
			 inode_number);
#endif
		  return 0;
		}
	}
#ifdef DEBUG_ISO
		  printf("iso_find_entry returning unsuccessfully (ino = %d)\n",
			 inode_number);
#endif
	return -1;
}


/*
 *  Look up name in the current directory and return its corresponding
 *  inode if it can be found.
 *
 */
struct iso_inode *
iso_lookup(struct iso_inode *dir, const char *name)
{
	struct inode_table_entry *itp = (struct inode_table_entry *) dir;
	unsigned long ino, ino_back;
	struct iso_inode *result = NULL;
	int first, last;

#ifdef DEBUG_ISO
	printf("iso_lookup: %s\n", name);
#endif

	/* is the current inode a directory? */
	if (!S_ISDIR(itp->mode)) {
#ifdef DEBUG_ISO
		printf("iso_lookup: inode %d not a directory\n", itp->inumber);
#endif
		iso_iput(dir);
		return NULL;
	}

	/* work through the name finding each directory in turn */
	ino = 0;
	first = last = 0;
	while (last < (int) strlen(name)) {
		if (name[last] == '/') {
			if (iso_find_entry(dir, &name[first], last - first,
					   &ino, &ino_back))
				return NULL;
			/* throw away the old directory inode, we
			   don't need it anymore */
			iso_iput(dir);

			if (!(dir = iso_iget(ino)))
				return NULL;
			first = last + 1;
			last = first;
		} else
			last++;
	}
	{
		int rv;
		if ((rv = iso_find_entry(dir, &name[first], last - first, &ino, &ino_back))) {
			iso_iput(dir);
			return NULL;
		}
	}
	if (!(result = iso_iget(ino))) {
		iso_iput(dir);
		return NULL;
	}
	/*
	 * We need this backlink for the ".." entry unless the name
	 * that we are looking up traversed a mount point (in which
	 * case the inode may not even be on an iso9660 filesystem,
	 * and writing to u.isofs_i would only cause memory
	 * corruption).
	 */
	result->i_backlink = ino_back;

	iso_iput(dir);
	return result;
}

/* follow a symbolic link, returning the inode of the file it points to */
static struct iso_inode *
iso_follow_link(struct iso_inode *from, const char *basename)
{
	struct inode_table_entry *itp = (struct inode_table_entry *)from;
	struct iso_inode *root = iso_iget(root_inode);
	/* HK: iso_iget expects an "int" but root_inode is "long" ?? */
	struct iso_inode *result = NULL;
	char *linkto;

#ifdef DEBUG_ISO
	printf("iso_follow_link(%s): ",basename);
#endif

	if (!S_ISLNK(itp->mode)) /* Hey, that's not a link! */
		return NULL;

	if (!itp->size)
		return NULL;

	if (!(linkto = get_rock_ridge_symlink(from)))
		return NULL;

	linkto[itp->size]='\0';
#ifdef DEBUG_ISO
	printf("%s->%s\n",basename,linkto ? linkto : "[failed]");
#endif

	/* Resolve relative links. */

	if (linkto[0] !='/') {
		const char *end = strrchr(basename, '/');
		if (end) {
			char fullname[(end - basename + 1) + strlen(linkto) + 1];
			strncpy(fullname, basename, end - basename + 1);
			fullname[end - basename + 1] = '\0';
			strcat(fullname, linkto);
#ifdef DEBUG_ISO
			printf("resolved to %s\n", fullname);
#endif
			result = iso_lookup(root,fullname);
		} else {
			/* Assume it's in the root */
			result = iso_lookup(root,linkto);
		}
	} else {
		result = iso_lookup(root,linkto);
	}
	free(linkto);
	iso_iput(root);
	return result;
}

/*
 * look if the driver can tell the multi session redirection value
 */
static inline unsigned int
iso_get_last_session (void)
{
#ifdef DEBUG_ISO
	printf("iso_get_last_session() called\n");
#endif
	return 0;
}


int
iso_read_super (void *data, int silent)
{
	static int first_time = 1;
	int high_sierra;
	unsigned int iso_blknum, vol_desc_start;
	char rock = 'y';
	int i;

	struct iso_volume_descriptor *vdp;
	struct hs_volume_descriptor *hdp;

	struct iso_primary_descriptor *pri = NULL;
	struct hs_primary_descriptor *h_pri = NULL;

	struct iso_directory_record *rootp;

	/* Initialize the inode table */
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		inode_table[i].free = 1;
		inode_table[i].inumber = 0;
	}

#ifdef DEBUG_ISO
	printf("iso_read_super() called\n");
#endif

	/* set up the block size */
	sb.s_blocksize = 1024;
	sb.s_blocksize_bits = 10;

	sb.s_high_sierra = high_sierra = 0; /* default is iso9660 */

	vol_desc_start = iso_get_last_session();

	for (iso_blknum = vol_desc_start+16; iso_blknum < vol_desc_start+100;
	     iso_blknum++) {
#ifdef DEBUG_ISO
		printf("iso_read_super: iso_blknum=%d\n", iso_blknum);
#endif
		if (iso_dev_read(data_block, iso_blknum * 2048,
				 sb.s_blocksize) != sb.s_blocksize)
		{
			printf("iso_read_super: bread failed, dev "
			       "iso_blknum %d\n", iso_blknum);
			return -1;
		}
		vdp = (struct iso_volume_descriptor *)data_block;
		hdp = (struct hs_volume_descriptor *)data_block;

		if (strncmp (hdp->id, HS_STANDARD_ID, sizeof hdp->id) == 0) {
			if (isonum_711 (hdp->type) != ISO_VD_PRIMARY)
				return -1;
			if (isonum_711 (hdp->type) == ISO_VD_END)
				return -1;

			sb.s_high_sierra = 1;
			high_sierra = 1;
			rock = 'n';
			h_pri = (struct hs_primary_descriptor *)vdp;
			break;
		}

		if (strncmp (vdp->id, ISO_STANDARD_ID, sizeof vdp->id) == 0) {
			if (isonum_711 (vdp->type) != ISO_VD_PRIMARY)
				return -1;
			if (isonum_711 (vdp->type) == ISO_VD_END)
				return -1;

			pri = (struct iso_primary_descriptor *)vdp;
			break;
		}
	}
	if(iso_blknum == vol_desc_start + 100) {
		if (!silent)
			printf("iso: Unable to identify CD-ROM format.\n");
		return -1;
	}

	if (high_sierra) {
		rootp = (struct iso_directory_record *)
			h_pri->root_directory_record;
#if 0
//See http://www.y-adagio.com/public/standards/iso_cdromr/sect_1.htm
//Section 4.16 and 4.17 for explanation why this check is invalid
		if (isonum_723 (h_pri->volume_set_size) != 1) {
			printf("Multi-volume disks not (yet) supported.\n");
			return -1;
		};
#endif
		sb.s_nzones = isonum_733 (h_pri->volume_space_size);
		sb.s_log_zone_size =
			isonum_723 (h_pri->logical_block_size);
		sb.s_max_size = isonum_733(h_pri->volume_space_size);
	} else {
		rootp = (struct iso_directory_record *)
			pri->root_directory_record;
#if 0
//See http://www.y-adagio.com/public/standards/iso_cdromr/sect_1.htm
//Section 4.16 and 4.17 for explanation why this check is invalid
		if (isonum_723 (pri->volume_set_size) != 1) {
			printf("Multi-volume disks not (yet) supported.\n");
			return -1;
		}
#endif
		sb.s_nzones = isonum_733 (pri->volume_space_size);
		sb.s_log_zone_size = isonum_723 (pri->logical_block_size);
		sb.s_max_size = isonum_733(pri->volume_space_size);
	}

	sb.s_ninodes = 0; /* No way to figure this out easily */

	/* RDE: convert log zone size to bit shift */

	switch (sb.s_log_zone_size) {
	      case  512: sb.s_log_zone_size =  9; break;
	      case 1024: sb.s_log_zone_size = 10; break;
	      case 2048: sb.s_log_zone_size = 11; break;

	      default:
		printf("Bad logical zone size %ld\n", sb.s_log_zone_size);
		return -1;
	}

	/* RDE: data zone now byte offset! */

	sb.s_firstdatazone = (isonum_733( rootp->extent)
					 << sb.s_log_zone_size);
	/*
	 * The CDROM is read-only, has no nodes (devices) on it, and
	 * since all of the files appear to be owned by root, we
	 * really do not want to allow suid.  (suid or devices will
	 * not show up unless we have Rock Ridge extensions).
	 */
	if (first_time) {
		first_time = 0;
		printf("iso: Max size:%ld   Log zone size:%ld\n",
		       sb.s_max_size, 1UL << sb.s_log_zone_size);
		printf("iso: First datazone:%ld   Root inode number %d\n",
		       sb.s_firstdatazone >> sb.s_log_zone_size,
		       isonum_733 (rootp->extent) << sb.s_log_zone_size);
		if (high_sierra)
			printf("iso: Disc in High Sierra format.\n");
	}

	/* set up enough so that it can read an inode */

	sb.s_mapping = 'n';
	sb.s_rock = (rock == 'y' ? 1 : 0);
	sb.s_conversion = 'b';
	sb.s_cruft = 'n';
	sb.s_unhide = 'n';
	/*
	 * It would be incredibly stupid to allow people to mark every file
	 * on the disk as suid, so we merely allow them to set the default
	 * permissions.
	 */
	sb.s_mode = S_IRUGO & 0777;

	/* return successfully */
	root_inode = isonum_733 (rootp->extent) << sb.s_log_zone_size;
	/* HK: isonum_733 returns an "int" but root_inode is a long ? */
	return 0;
}


int
iso_bread (int fd, long blkno, long nblks, char * buffer)
{
	struct iso_inode *inode;

	/* find the inode for this file */
	inode = &inode_table[fd].inode;
	return iso_breadi(inode, blkno, nblks, buffer);
}


int
iso_open (const char *filename)
{
	struct iso_inode *inode, *oldinode;
	struct iso_inode *root;

	/* get the root directory */
	root = iso_iget(root_inode);
/* HK: iso_iget expects an "int" but root_inode is "long" ?? */
	if (!root) {
		printf("iso9660: get root inode failed\n");
		return -1;
	}

	/* lookup the file */
	inode = iso_lookup(root, filename);

#ifdef DEBUG_ISO
	if (inode && S_ISLNK(((struct inode_table_entry *)inode)->mode)) {
		printf("%s is a link (len %u)\n",filename,
		       ((struct inode_table_entry *)inode)->size);
	} else {
		printf("%s is not a link\n",filename);
	}
#endif

	while ((inode) && (S_ISLNK(((struct inode_table_entry *)inode)->mode))) {
		oldinode = inode;
		inode = iso_follow_link(oldinode, filename);
		iso_iput(oldinode);
	}
	if (inode == NULL)
		return -1;
	else {
		struct inode_table_entry * itp =
			(struct inode_table_entry *) inode;
		return (itp - inode_table);
	}
}


void
iso_close (int fd)
{
	iso_iput(&inode_table[fd].inode);
}


long
iso_map (int fd, long block)
{
	return iso_bmap(&inode_table[fd].inode, block) * sb.s_blocksize;
}

#include <sys/stat.h>
int
iso_fstat (int fd, struct stat * buf)
{
	struct inode_table_entry * ip = inode_table + fd;

	if (fd >= MAX_OPEN_FILES)
		return -1;

	memset(buf, 0, sizeof(struct stat));
	/* fill in relevant fields */
	buf->st_ino   = ip->inumber;
	buf->st_mode  = ip->mode;
	buf->st_nlink = ip->nlink;
	buf->st_size  = ip->size;
	return 0;
}

/*
 * NOTE: mixing calls to this and calls to any other function that reads from
 * the filesystem will clobber the buffers and cause much wailing and gnashing
 * of teeth.
 *
 * Sorry this function is so ugly. It was written as a way for me to
 * learn how the ISO filesystem stuff works.
 *
 * Will Woods, 2/2001
 *
 * Uses data_block
 */
char *iso_readdir_i(int fd, int rewind) {
    struct inode_table_entry *itp = &(inode_table[fd]);
    struct iso_directory_record *dirent = 0;
    unsigned int fraglen = 0, block, dirent_len, name_len = 0, oldoffset;
    static unsigned int blockoffset = 0, diroffset = 0;

    if (!S_ISDIR(itp->mode)) {
	printf("Not a directory\n");
	return NULL;
    }

    /* Initial read to this directory, get the first block */
    if (rewind) {
	blockoffset = diroffset = 0;
	block = iso_bmap(&itp->inode,0);
#ifdef DEBUG_ISO
	printf("fd #%d, inode %d, first_extent %d, block %u\n",
	       fd,itp->inumber,itp->inode.i_first_extent,block);
#endif
	if (!block) return NULL;

	if (iso_dev_read(data_block, block * sb.s_blocksize, sb.s_blocksize)
	    != sb.s_blocksize) return NULL;
    }

    /* keep doing this until we get a filename or we fail */
    while (!name_len) {
	/* set up our dirent pointer into the block of data we've read */
	dirent = (struct iso_directory_record *) (data_block + blockoffset);
	dirent_len = isonum_711(dirent->length);
#ifdef DEBUG_ISO
	printf("diroffset=%u, blockoffset=%u, length=%u\n",
	       diroffset,blockoffset,dirent_len);
#endif

	/* End of directory listing or end of sector */
	if (dirent_len == 0) {
	    /* round diroffset up to the nearest blocksize */
	    diroffset = ((diroffset & ~(ISOFS_BLOCK_SIZE - 1))
			 + ISOFS_BLOCK_SIZE);
#ifdef DEBUG_ISO
	    printf("dirent_len == 0. diroffset=%u, itp->size=%u. ",
		   diroffset,itp->size);
#endif
	    if (diroffset >= itp->size) {
#ifdef DEBUG_ISO
		printf("End of directory.\n");
#endif
		return NULL;
	    } else {
#ifdef DEBUG_ISO
		printf("End of sector. Need next block.\n");
#endif
		/* Get the next block. */
		block = iso_bmap(&itp->inode, diroffset>>sb.s_blocksize_bits);
		if (!block) return NULL;
		if (iso_dev_read(data_block, block * sb.s_blocksize, sb.s_blocksize)
		    != sb.s_blocksize) return NULL;

		/* set the offsets and the pointers properly */
		blockoffset = 0;
		dirent = (struct iso_directory_record *) data_block;
		dirent_len = isonum_711(dirent->length);
#ifdef DEBUG_ISO
		printf("diroffset=%u, blockoffset=%u, length=%u\n",
		       diroffset,blockoffset,dirent_len);
#endif
	    }
	}

	/* update the offsets for the next read */
	oldoffset = blockoffset;
	blockoffset += dirent_len;
	diroffset += dirent_len;

	/*
	 * directory entry spans two blocks -
	 * get next block and glue the two halves together
	 */
	if (blockoffset >= sb.s_blocksize) {
	    fraglen = sb.s_blocksize - oldoffset;
#ifdef DEBUG_ISO
	    printf("fragmented block: blockoffset = %u, fraglen = %u\n",
		   blockoffset, fraglen);
#endif
	    /* copy the fragment at end of old block to front of new buffer */
	    memcpy(big_data_block, data_block + oldoffset, fraglen);

	    /* read the next block into the buffer after the old fragment */
	    block = iso_bmap(&itp->inode, diroffset >> sb.s_blocksize_bits);
	    if (!block) return NULL;
	    if (iso_dev_read(big_data_block + fraglen, block * sb.s_blocksize, sb.s_blocksize)
		!= sb.s_blocksize) return NULL;
#ifdef DEBUG_ISO
	    printf("read %u bytes from offset %u\n",
		   sb.s_blocksize, block * sb.s_blocksize);
#endif

	    blockoffset = 0;
	    dirent = (struct iso_directory_record *) big_data_block;
	}

	/*
	 * Everything's cool, let's get the filename.
	 * First we need to figure out the length.
	 */
	name_len = isonum_711((char *) dirent->name_len);
#ifdef DEBUG_ISO
	if (name_len==0) printf("dirent->name_len = 0, skipping.\n");
#endif

	/* skip '.' and '..' */
	if (name_len == 1) {
	    if (dirent->name[0] == (char)0) name_len = 0;
	    if (dirent->name[0] == (char)1) name_len = 0;
	}


	if (sb.s_unhide == 'n') {
	    /* sb.s_high_sierra is the offset for the position of the flags.
	       this adjusts for differences between iso9660 and high sierra.

	       if bit 0 (exists) or bit 2 (associated) are set, we ignore
	       this record. */
	    if (dirent->flags[-sb.s_high_sierra] & 5) name_len = 0;
	}

	/* if we have a real filename here.. */
	if (name_len) {
	    /* should be sufficient, since get_rock_ridge_filename truncates
	       at 254 characters */
	    char rrname[256];
	    if ((name_len = get_rock_ridge_filename(dirent, rrname, &itp->inode))) {
		rrname[name_len] = '\0';
		/* it's okay if rrname is longer than dirent->name, because
		   we're just overwriting parts of the now-useless dirent */
		strcpy(dirent->name, rrname);
	    } else {
	        int i;
	        char c;
		if (sb.s_mapping == 'n') { /* downcase the name */
		    for (i = 0; i < name_len; i++) {
			c = dirent->name[i];

                        /* lower case */
			if ((c >= 'A') && (c <= 'Z')) c |= 0x20;

			/* Drop trailing '.;1' */
			if ((c == '.') && (i == name_len-3) &&
			    (dirent->name[i+1] == ';') &&
                            (dirent->name[i+2] == '1')) {
			    name_len -= 3 ; break;
			}

			/* Drop trailing ';1' */
			if ((c == ';') && (i == name_len-2) &&
                            (dirent->name[i+1] == '1')) {
			    name_len -= 2; break;
			}

			/* convert ';' to '.' */
			if (c == ';')
			    c = '.';

			dirent->name[i] = c;
		    }
		    dirent->name[name_len] = '\0';
		}
	    }
	}
	/* now that we're done using it, and it's smaller than a full block,
	 * copy big_data_block back into data_block */
	if (fraglen) {
            int len = sb.s_blocksize - dirent_len;
	    memcpy(data_block, big_data_block + dirent_len, len);
#ifdef DEBUG_ISO
	    printf("copied %u bytes of data back to data_block\n", len);
#endif
	    blockoffset = 0;
	    fraglen = 0;
	}
    }
    return dirent->name;
}

/**********************************************************************
 *
 * Rock Ridge functions and definitions, from the Linux kernel source.
 * linux/fs/isofs/rock.c, (c) 1992, 1993 Eric Youngdale.
 *
 **********************************************************************/

#define SIG(A,B) ((A << 8) | B)

/* This is a way of ensuring that we have something in the system
   use fields that is compatible with Rock Ridge */
#define CHECK_SP(FAIL)	       			\
      if(rr->u.SP.magic[0] != 0xbe) FAIL;	\
      if(rr->u.SP.magic[1] != 0xef) FAIL;

/* We define a series of macros because each function must do exactly the
   same thing in certain places.  We use the macros to ensure that everything
   is done correctly */

#define CONTINUE_DECLS \
  int cont_extent = 0, cont_offset = 0, cont_size = 0;   \
  void * buffer = 0

#define CHECK_CE	       			\
      {cont_extent = isonum_733(rr->u.CE.extent); \
      cont_offset = isonum_733(rr->u.CE.offset); \
      cont_size = isonum_733(rr->u.CE.size);}

#define SETUP_ROCK_RIDGE(DE,CHR,LEN)	      		      	\
  {LEN= sizeof(struct iso_directory_record) + DE->name_len[0];	\
  if(LEN & 1) LEN++;						\
  CHR = ((unsigned char *) DE) + LEN;				\
  LEN = *((unsigned char *) DE) - LEN;}

#define MAYBE_CONTINUE(LABEL) \
  {if (buffer) free(buffer); \
  if (cont_extent){ \
    buffer = malloc(cont_size); \
    if (!buffer) goto out; \
    if (iso_dev_read(buffer, cont_extent * ISOFS_BLOCK_SIZE + cont_offset, cont_size) \
	  == cont_size) { \
        chr = (unsigned char *) buffer; \
        len = cont_size; \
        cont_extent = cont_size = cont_offset = 0; \
        goto LABEL; \
    }; \
    printf("Unable to read rock-ridge attributes\n");    \
  }}

int get_rock_ridge_filename(struct iso_directory_record * de,
			    char * retname,
			    struct iso_inode * inode)
{
  int len;
  unsigned char * chr;
  int retnamlen = 0, truncate=0;
  int cont_extent = 0, cont_offset = 0, cont_size = 0;
  void *buffer = 0;

  /* No rock ridge? well then... */
  if (!sb.s_rock) return 0;
  *retname = '\0';

  len = sizeof(struct iso_directory_record) + isonum_711((char *) de->name_len);
  if (len & 1) len++;
  chr = ((unsigned char *) de) + len;
  len = *((unsigned char *) de) - len;
  {
    struct rock_ridge * rr;
    int sig;

  repeat:
    while (len > 1){ /* There may be one byte for padding somewhere */
      rr = (struct rock_ridge *) chr;
      if (rr->len == 0) break; /* Something got screwed up here */

      sig = (chr[0] << 8) + chr[1];
      chr += rr->len;
      len -= rr->len;

      switch(sig){
      case SIG('R','R'):
	  if((rr->u.RR.flags[0] & RR_NM) == 0) goto out;
	  break;
      case SIG('S','P'):
	  if (rr->u.SP.magic[0] != 0xbe ||
	      rr->u.SP.magic[1] != 0xef)
	      goto out;
	  break;
      case SIG('C','E'):
	  cont_extent = isonum_733(rr->u.CE.extent);
	  cont_offset = isonum_733(rr->u.CE.offset);
	  cont_size = isonum_733(rr->u.CE.size);
	  break;
      case SIG('N','M'):
	  if (truncate) break;
        /*
	 * If the flags are 2 or 4, this indicates '.' or '..'.
	 * We don't want to do anything with this, because it
	 * screws up the code that calls us.  We don't really
	 * care anyways, since we can just use the non-RR
	 * name.
	 */
	if (rr->u.NM.flags & 6) {
	    break;
	}

	if (rr->u.NM.flags & ~1) {
	  printf("Unsupported NM flag settings (%d)\n",rr->u.NM.flags);
	  break;
	};
	if((strlen(retname) + rr->len - 5) >= 254) {
	    int i = 254-strlen(retname);
	    strncat(retname, rr->u.NM.name, i);
	    retnamlen += i;
	    truncate = 1;
	    break;
	};
	strncat(retname, rr->u.NM.name, rr->len - 5);
	retnamlen += rr->len - 5;
	break;
      case SIG('R','E'):
	  goto out;
      default:
	  break;
      }
    };
  }
  if (buffer) free(buffer);
  if (cont_extent) { /* we had a continued record */
      buffer = malloc(cont_size);
      if (!buffer) goto out;
      if (iso_dev_read(buffer, cont_extent * ISOFS_BLOCK_SIZE + cont_offset, cont_size)
	  != cont_size) goto out;
      chr = buffer + cont_offset;
      len = cont_size;
      cont_extent = cont_size = cont_offset = 0;
      goto repeat;
  }
  return retnamlen; /* If 0, this file did not have a NM field */
 out:
  if (buffer) free(buffer);
  return 0;
}

static int parse_rock_ridge_inode(struct iso_directory_record * de,
				  struct iso_inode * inode){
  int len;
  unsigned char *chr;
  int symlink_len = 0;
  struct inode_table_entry *itp = (struct inode_table_entry *) inode;
  CONTINUE_DECLS;

#ifdef DEBUG_ROCK
  printf("parse_rock_ridge_inode(%u)\n",itp->inumber);
#endif

  if (!sb.s_rock) return 0;

  SETUP_ROCK_RIDGE(de, chr, len);
 repeat:
  {
    int sig;
    /* struct iso_inode * reloc; */
    struct rock_ridge * rr;
    int rootflag;

    while (len > 1){ /* There may be one byte for padding somewhere */
      rr = (struct rock_ridge *) chr;
      if (rr->len == 0) goto out; /* Something got screwed up here */
      sig = (chr[0] << 8) + chr[1];
      chr += rr->len;
      len -= rr->len;

      switch(sig){
      case SIG('R','R'):
#ifdef DEBUG_ROCK
  printf("RR ");
#endif
	if((rr->u.RR.flags[0] &
 	    (RR_PX | RR_TF | RR_SL | RR_CL)) == 0) goto out;
	break;
      case SIG('S','P'):
#ifdef DEBUG_ROCK
  printf("SP ");
#endif
	CHECK_SP(goto out);
	break;
      case SIG('C','E'):
#ifdef DEBUG_ROCK
  printf("CE ");
#endif
	CHECK_CE;
	break;
      case SIG('E','R'):
#ifdef DEBUG_ROCK
	printf("ISO 9660 Extensions: ");
	{ int p;
	  for(p=0;p<rr->u.ER.len_id;p++) printf("%c",rr->u.ER.data[p]);
	};
	printf("\n");
#endif
	break;
      case SIG('P','X'):
#ifdef DEBUG_ROCK
  printf("PX ");
#endif
	itp->mode  = isonum_733(rr->u.PX.mode);
	itp->nlink = isonum_733(rr->u.PX.n_links);
	/* Ignore uid and gid. We're only a simple bootloader, after all. */
	break;
      case SIG('P','N'):
	/* Ignore device files. */
	break;
      case SIG('T','F'):
        /* create/modify/access times are uninteresting to us. */
	break;
      case SIG('S','L'):
#ifdef DEBUG_ROCK
  printf("SL ");
#endif
	{int slen;
	 struct SL_component * slp;
	 struct SL_component * oldslp;
	 slen = rr->len - 5;
	 slp = &rr->u.SL.link;
	 itp->size = symlink_len;
	 while (slen > 1){
	   rootflag = 0;
	   switch(slp->flags &~1){
	   case 0:
	     itp->size += slp->len;
	     break;
	   case 2:
	     itp->size += 1;
	     break;
	   case 4:
	     itp->size += 2;
	     break;
	   case 8:
	     rootflag = 1;
	     itp->size += 1;
	     break;
	   default:
	     printf("Symlink component flag not implemented\n");
	   };
	   slen -= slp->len + 2;
	   oldslp = slp;
	   slp = (struct SL_component *) (((char *) slp) + slp->len + 2);

	   if(slen < 2) {
	     if(    ((rr->u.SL.flags & 1) != 0)
		    && ((oldslp->flags & 1) == 0) ) itp->size += 1;
	     break;
	   }

	   /*
	    * If this component record isn't continued, then append a '/'.
	    */
	   if(   (!rootflag)
		 && ((oldslp->flags & 1) == 0) ) itp->size += 1;
	 }
	}
	symlink_len = itp->size;
	break;
      case SIG('R','E'):
	printf("Attempt to read inode for relocated directory\n");
	goto out;
      case SIG('C','L'):
#ifdef DEBUG_ROCK
  printf("CL(!) ");
#endif
        /* I'm unsure as to the function of this signature.
	   We'll ignore it and hope that everything will be OK.
	*/
#if 0
#ifdef DEBUG
	printf("RR CL (%x)\n",inode->i_ino);
#endif
	inode->inode.first_extent = isonum_733(rr->u.CL.location);
	reloc = iso_iget(inode->i_sb,
			 (inode->u.isofs_i.i_first_extent <<
			  inode -> i_sb -> u.isofs_sb.s_log_zone_size));
	if (!reloc)
		goto out;
	inode->mode = reloc->mode;
	inode->nlink = reloc->nlink;
	inode->size = reloc->size;
	iso_iput(reloc);
#endif /* 0 */
	break;
      default:
	break;
      }
    };
  }
  MAYBE_CONTINUE(repeat);
#ifdef DEBUG_ROCK
  printf("\nparse_rock_ridge_inode(): ok\n");
#endif
  return 1;
 out:
  if(buffer) free(buffer);
#ifdef DEBUG_ROCK
  printf("\nparse_rock_ridge_inode(): failed\n");
#endif
  return 0;
}

/* Returns the name of the file that this inode is symlinked to.  This is
   in malloc memory, so we have to free it when we're done */

static char * get_rock_ridge_symlink(struct iso_inode *inode)
{
  int blocksize = ISOFS_BLOCK_SIZE;
  int blockbits = ISOFS_BLOCK_BITS;
  char * rpnt = NULL;
  unsigned char * pnt;
  struct iso_directory_record * raw_inode;
  struct inode_table_entry *itp = (struct inode_table_entry *)inode;
  CONTINUE_DECLS;
  int block, blockoffset;
  int sig;
  int rootflag;
  int len;
  unsigned char * chr, * buf = NULL;
  struct rock_ridge * rr;

#ifdef DEBUG_ROCK
  printf("get_rock_ridge_symlink(%u): link is %u bytes long\n",itp->inumber, itp->size);
#endif

  if (!sb.s_rock) goto out;

  block = itp->inumber >> blockbits;
  blockoffset = itp->inumber & (blocksize - 1);

  buf=malloc(blocksize);

  if (iso_dev_read(buf, block << blockbits, blocksize) != blocksize)
	  goto out_noread;

  pnt = ((unsigned char *) buf) + blockoffset;

  raw_inode = ((struct iso_directory_record *) pnt);

  /*
   * If we go past the end of the buffer, there is some sort of error.
   */
  if (blockoffset + *pnt > blocksize)
	goto out_bad_span;

  /* Now test for possible Rock Ridge extensions which will override some of
     these numbers in the inode structure. */

  SETUP_ROCK_RIDGE(raw_inode, chr, len);

 repeat:
  while (len > 1){ /* There may be one byte for padding somewhere */
    rr = (struct rock_ridge *) chr;
    if (rr->len == 0) goto out; /* Something got screwed up here */
    sig = (chr[0] << 8) + chr[1];
    chr += rr->len;
    len -= rr->len;

#ifdef DEBUG_ROCK
    printf("%c%c ",chr[0],chr[1]);
#endif
    switch(sig){
    case SIG('R','R'):
      if((rr->u.RR.flags[0] & RR_SL) == 0) goto out;
      break;
    case SIG('S','P'):
      CHECK_SP(goto out);
      break;
    case SIG('S','L'):
      {int slen;
       struct SL_component * oldslp;
       struct SL_component * slp;
       slen = rr->len - 5;
       slp = &rr->u.SL.link;
       while (slen > 1){
	 if (!rpnt){
	   rpnt = malloc (itp->size +1);
	   if (!rpnt) goto out;
	   *rpnt = 0;
	 };
	 rootflag = 0;
	 switch(slp->flags &~1){
	 case 0:
	   strncat(rpnt,slp->text, slp->len);
	   break;
	 case 2:
	   strcat(rpnt,".");
	   break;
	 case 4:
	   strcat(rpnt,"..");
	   break;
	 case 8:
	   rootflag = 1;
	   strcat(rpnt,"/");
	   break;
	 default:
#ifdef DEBUG_ROCK
	   printf("Symlink component flag not implemented (%d)\n",slen);
#endif
	   break;
	 };
	 slen -= slp->len + 2;
	 oldslp = slp;
	 slp = (struct SL_component *) (((char *) slp) + slp->len + 2);

	 if(slen < 2) {
	   /*
	    * If there is another SL record, and this component record
	    * isn't continued, then add a slash.
	    */
	   if(    ((rr->u.SL.flags & 1) != 0)
	       && ((oldslp->flags & 1) == 0) ) strcat(rpnt,"/");
	   break;
	 }

	 /*
	  * If this component record isn't continued, then append a '/'.
	  */
	 if(   (!rootflag)
	    && ((oldslp->flags & 1) == 0) ) strcat(rpnt,"/");

       };
       break;
     case SIG('C','E'):
       CHECK_CE; /* This tells is if there is a continuation record */
       break;
     default:
       break;
     }
    };
  };
  MAYBE_CONTINUE(repeat);

 out_freebh:
#ifdef DEBUG_ROCK
  printf("\nget_rock_ridge_symlink() exiting\n");
#endif
        if (buf)
		free(buf);
	return rpnt;

	/* error exit from macro */
out:
#ifdef DEBUG_ROCK
	printf("abort");
#endif
	if(buffer)
		free(buffer);
	if(rpnt)
		free(rpnt);
	rpnt = NULL;
	goto out_freebh;
out_noread:
	printf("unable to read block");
	goto out_freebh;
out_bad_span:
	printf("symlink spans iso9660 blocks\n");
	goto out_freebh;
}



