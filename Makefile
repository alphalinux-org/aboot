#
# aboot/Makefile
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (c) 1995, 1996 by David Mosberger (davidm@cs.arizona.edu)
#

# location of linux kernel sources (must be absolute path):
KSRC		= /usr/src/linux
VMLINUX		= $(KSRC)/vmlinux
VMLINUXGZ	= $(KSRC)/arch/alpha/boot/vmlinux.gz

# for userspace testing
#TESTING	= yes

# for boot testing
#CFGDEFS       	= -DDEBUG_ISO -DDEBUG_ROCK -DDEBUG_EXT2 -DDEBUG_XFS -DDEBUG

# root, aka prefix
root		=
bindir		= $(root)/sbin
bootdir		= $(root)/boot
mandir         = $(root)/usr/share/man

export

#
# There shouldn't be any need to change anything below this line.
#
LOADADDR	= 20000000

ABOOT_LDFLAGS = -static -N -Taboot.lds --relax

ifeq ($(TESTING),)
override CPPFLAGS	+= $(CFGDEFS) -U_FORTIFY_SOURCE -Iinclude
override CFLAGS		+= $(CPPFLAGS) -Os -Wall -ffreestanding -mno-fp-regs -msmall-data -msmall-text
else
override CPPFLAGS	+= -DTESTING $(CFGDEFS) -U_FORTIFY_SOURCE -Iinclude
override CFLAGS		+= $(CPPFLAGS) -O -g3 -Wall
endif

override ASFLAGS	+= $(CPPFLAGS)


.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<
.S.s:
	$(CC) $(ASFLAGS) -D__ASSEMBLY__ -E -o $*.o $<
.S.o:
	$(CC) $(ASFLAGS) -D__ASSEMBLY__ -c -o $*.o $<

NET_OBJS = net.o
DISK_OBJS = disk.o fs/ext2.o fs/ufs.o fs/dummy.o fs/iso.o fs/xfs.o
ifeq ($(TESTING),)
ABOOT_OBJS = \
	head.o aboot.o cons.o utils.o \
	zip/misc.o zip/unzip.o zip/inflate.o
else
ABOOT_OBJS = aboot.o zip/misc.o zip/unzip.o zip/inflate.o
endif
LIBS	= lib/libaboot.a

ifeq ($(TESTING),)
all:	diskboot
else
all:	aboot
endif

diskboot:	bootlx sdisklabel/sdisklabel sdisklabel/swriteboot \
		tools/e2writeboot tools/isomarkboot tools/abootconf \
		tools/elfencap

netboot: vmlinux.bootp

bootloader.h: net_aboot.nh b2c
	./b2c net_aboot.nh bootloader.h bootloader

netabootwrap: netabootwrap.c bootloader.h
	$(CC) $@.c $(CFLAGS) -o $@


bootlx:	aboot tools/objstrip
	tools/objstrip -vb aboot bootlx

install-man: 
	make -C doc/man install

install-man-gz:
	make -C doc/man install-gz

install: tools/abootconf tools/e2writeboot tools/isomarkboot \
	sdisklabel/swriteboot install-man
	install -d $(bindir) $(bootdir)
	install -c tools/abootconf $(bindir)
	install -c tools/e2writeboot $(bindir)
	install -c tools/isomarkboot $(bindir)
	install -c sdisklabel/swriteboot $(bindir)
	install -c bootlx $(bootdir)

installondisk:	bootlx sdisklabel/swriteboot
	sdisklabel/swriteboot -vf0 /dev/sda bootlx vmlinux.gz

ifeq ($(TESTING),)
aboot:	$(ABOOT_OBJS) $(DISK_OBJS) $(LIBS)
	$(LD) $(ABOOT_LDFLAGS) $(ABOOT_OBJS) $(DISK_OBJS) -o $@ $(LIBS)
else
aboot:	$(ABOOT_OBJS) $(DISK_OBJS) $(LIBS)
	$(CC) $(ABOOT_OBJS) $(DISK_OBJS) -o $@ $(LIBS)
endif

vmlinux.bootp: net_aboot.nh $(VMLINUXGZ) net_pad
	cat net_aboot.nh $(VMLINUXGZ) net_pad > $@

net_aboot.nh: net_aboot tools/objstrip
	strip net_aboot
	tools/objstrip -vb net_aboot $@

net_aboot: $(ABOOT_OBJS) $(ABOOT_OBJS) $(NET_OBJS) $(LIBS)
	$(LD) $(ABOOT_LDFLAGS) $(ABOOT_OBJS) $(NET_OBJS) -o $@ $(LIBS)

net_pad:
	dd if=/dev/zero of=$@ bs=512 count=1

clean:	sdisklabel/clean tools/clean lib/clean
	rm -f aboot abootconf net_aboot net_aboot.nh net_pad vmlinux.bootp \
		$(ABOOT_OBJS) $(DISK_OBJS) $(NET_OBJS) bootlx \
		include/ksize.h vmlinux.nh b2c bootloader.h netabootwrap

distclean: clean
	find . -name \*~ | xargs rm -f

lib/%:
	make -C lib $* CPPFLAGS="$(CPPFLAGS)" TESTING="$(TESTING)"

tools/%:
	make -C tools $* CPPFLAGS="$(CPPFLAGS)"

sdisklabel/%:
	make -C sdisklabel $* CPPFLAGS="$(CPPFLAGS)"

vmlinux.nh: $(VMLINUX) tools/objstrip
	tools/objstrip -vb $(VMLINUX) vmlinux.nh

include/ksize.h: vmlinux.nh
	echo "#define KERNEL_SIZE `ls -l vmlinux.nh | awk '{print $$5}'` > $@

dep:
