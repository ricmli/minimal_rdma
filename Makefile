.PHONY: clean

CFLAGS  := -Wall -g
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs

APPS    := server client

all: ${APPS}


clean:
	rm -f ${APPS}

