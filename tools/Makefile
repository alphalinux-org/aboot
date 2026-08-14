override CFLAGS += -I. -I../include $(CPPFLAGS)
override PGMS += e2writeboot isomarkboot abootconf elfencap objstrip

EXEC_PREFIX = /usr

all:	$(PGMS)

install:	$(PGMS)
	install -c -o root -g root -m 755 $(PGMS) $(EXEC_PREFIX)/bin

clean:
	rm -f *~ *.o *.a core $(PGMS) isolib.o

# isolib.c is shared with the bootloader, but the two want different flags,
# so build a copy here rather than linking the bootloader's object.
isolib.o:	../lib/isolib.c
	$(CC) $(CFLAGS) -c -o $@ $<

isomarkboot:	isomarkboot.o isolib.o
e2writeboot:	e2writeboot.o e2lib.o bio.o

e2writeboot.o:	e2lib.h
e2lib.o: e2lib.h
