
CFG_FILE=\"/etc/gpsmcr.conf\"

CONF=-DCFG_FILE=$(CFG_FILE) -pthread -Wall
#CONF=-DCFG_FILE=$(CFG_FILE) -D_DEBUG -pthread -Wall

COMMON=ourtypes.h gpsmcr.h gpsmcrlib.h gpsmcrinit.h

all: gpsmcr gmtool

install: all
	install -o root -g root -m 0700 gpsmcr /usr/local/bin/gpsmcr
	install -o root -g root -m 0700 gmtool /usr/local/bin/gmtool

gpsmcrlib.o: gpsmcrlib.c $(COMMON)
	gcc -c gpsmcrlib.c

gpsmcrinit.o: gpsmcrinit.c $(COMMON)
	gcc -c gpsmcrinit.c

chrony.o: chrony.c $(COMMON)
	gcc -c chrony.c

gpsmcr: gpsmcr.c gpsmcrlib.o gpsmcrinit.o chrony.o $(COMMON) 
	gcc $(CONF) -o gpsmcr gpsmcr.c gpsmcrlib.o gpsmcrinit.o chrony.o -lpthread -lrt

gmtool: gmtool.c gmtool.h gpsmcrlib.o $(COMMON)
	gcc $(CONF) -o gmtool gmtool.c gpsmcrlib.o -lrt

clean: 
	rm *.o
	rm gpsmcr gmtool

.PHONY: all
