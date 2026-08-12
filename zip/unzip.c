/* unzip.c -- decompress files in gzip or pkzip format.
 * Copyright (C) 1992-1993 Jean-loup Gailly
 *
 * Adapted for Linux booting by Hannu Savolainen 1993
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License, see the file COPYING.
 *
 * The code in this file is derived from the file funzip.c written
 * and put in the public domain by Mark Adler.
 */

/*
 * This version can extract files in gzip or pkzip format.  For the
 * latter, only the first entry is extracted, and it has to be either
 * deflated or stored.
 */

#include "gzip.h"
#include "utils.h"	/* printf, for the DEBUG build */

/* PKZIP header definitions */
#define LOCSIG 0x04034b50L      /* four-byte lead-in (lsb first) */
#define LOCFLG 6                /* offset of bit flag */
#define  CRPFLG 1               /*  bit for encrypted entry */
#define  EXTFLG 8               /*  bit for extended local header */
#define LOCHOW 8                /* offset of compression method */
#define LOCTIM 10               /* file mod time (for decryption) */
#define LOCCRC 14               /* offset of crc */
#define LOCSIZ 18               /* offset of compressed size */
#define LOCLEN 22               /* offset of uncompressed length */
#define LOCFIL 26               /* offset of file name field length */
#define LOCEXT 28               /* offset of extra field length */
#define LOCHDR 30               /* size of local header, including sig */
#define EXTHDR 16               /* size of extended local header, inc sig */


/* Globals */

int pkzip = 0;    /* set for a pkzip file */
int extended = 0; /* set if extended local header */

/*
 * Unzip in to out.  This routine works on both gzip and pkzip files.
 *
 * IN assertions: the buffer inbuf contains already the beginning of
 *   the compressed data, from offsets inptr to insize-1 included.
 *   The magic header has already been checked. The output buffer is cleared.
 */
void unzip(int in, int out)
{
    unsigned long orig_crc = 0;       /* original crc */
    unsigned long orig_len = 0;       /* original uncompressed length */
    int n;
    unsigned char buf[EXTHDR];        /* extended local header */

    /* ifd = in;
    ofd = out; */

    updcrc(NULL, 0);           /* initialize crc */

    if (pkzip && !extended) {  /* crc and length at the end otherwise */
	orig_crc = LG(inbuf + LOCCRC);
	orig_len = LG(inbuf + LOCLEN);
    }

    /* Decompress */
    if (method == DEFLATED)  {

	int res = inflate();

	if (res == 3) {
	    unzip_error("out of memory");
	} else if (res != 0) {
	    unzip_error("invalid compressed format");
	}

    } else if (pkzip && method == STORED) {
	register unsigned long n = LG(inbuf + LOCLEN);

	if (n != LG(inbuf + LOCSIZ)) {
	    unzip_error("length mismatch");
	}
	while (n--) {
	    unsigned char c = get_byte();
	    put_char(c);
	}
    } else {
	unzip_error("internal error, invalid method");
    }

    /* Get the crc and original length */
#ifdef DEBUG
    printf("getting CRC and length\n");
#endif
    if (!pkzip) {
        /* crc32  (see algorithm.doc)
	 * uncompressed input size modulo 2^32
         */
	for (n = 0; n < 8; n++) {
	    buf[n] = get_byte(); /* may cause an error if EOF */
#ifdef DEBUG
	    printf("nonext: doing CRC, get_byte (ptr %d) returned %d\n",
		   inptr, buf[n]);
#endif
	}
	orig_crc = LG(buf);
	orig_len = LG(buf+4);

    } else if (extended) {  /* If extended header, check it */
	/* signature - 4bytes: 0x50 0x4b 0x07 0x08
	 * CRC-32 value
         * compressed size 4-bytes
         * uncompressed size 4-bytes
	 */
	for (n = 0; n < EXTHDR; n++) {
	    buf[n] = get_byte(); /* may cause an error if EOF */
#ifdef DEBUG
	    printf("ext: doing CRC, get_byte returned %d\n", buf[n]);
#endif
	}
	orig_crc = LG(buf+4);
	orig_len = LG(buf+12);
    }

    /* Validate decompression */
    if (orig_crc != updcrc(buf, 0)) {
	unzip_error("crc error");
    }
    if (orig_len != bytes_out) {
	unzip_error("length error");
    }

    /* Check if there are more entries in a pkzip file */
    if (pkzip && inptr + 4 < insize && LG(inbuf+inptr) == LOCSIG) {
	    unzip_error("zip file has more than one entry");
    }
    extended = pkzip = 0; /* for next file */
}
