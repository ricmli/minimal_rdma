.PHONY: clean

CFLAGS  := -Wall -g
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs

APPS    := server client frame_tx frame_rx

all: ${APPS}


clean:
	rm -f ${APPS}

