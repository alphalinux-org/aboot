override CPPFLAGS += -U_FORTIFY_SOURCE -I../include
override ASFLAGS += $(CPPFLAGS) -D__ASSEMBLY__

ifeq ($(TESTING),)
ifeq ($(FOREIGN),"yes")
override CFLAGS	+= -Os -Wall -ffreestanding
else
override CFLAGS += -Os -Wall -mno-fp-regs -ffreestanding
endif
else
override CFLAGS += -Os -g3 -Wall
endif

ifeq ($(TESTING),)
libaboot.a: vsprintf.o memcpy.o memset.o string.o _setjmp.o \
	_longjmp.o isolib.o __divqu.o __remqu.o __divlu.o \
	__remlu.o
	$(AR) cru $@ $?
else
libaboot.a: isolib.o
	$(AR) cru $@ $?
endif

clean:
	rm -f libaboot.a *.o

__divqu.o: divide.S
	$(CC) -DDIV -c -o $@ divide.S

__remqu.o: divide.S
	$(CC) -DREM -c -o $@ divide.S

__divlu.o: divide.S
	$(CC) -DDIV -DINTSIZE -c -o $@ divide.S

__remlu.o: divide.S
	$(CC) -DREM -DINTSIZE -c -o $@ divide.S

