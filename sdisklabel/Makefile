override CFLAGS += -I../include $(CPPFLAGS)

all:	sdisklabel swriteboot

sdisklabel: sdisklabel.o library.o
	$(CC) $(LDFLAGS) $(CFLAGS) sdisklabel.o library.o -o sdisklabel

swriteboot: swriteboot.o library.o
	$(CC) $(LDFLAGS) $(CFLAGS) swriteboot.o library.o -o swriteboot

clean:
	rm -f sdisklabel swriteboot *.o
