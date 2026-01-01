CC=gcc
CFLAGS=-Wall -Iincludes -Wextra -std=c99 -ggdb
LDLIBS=-lcrypto
VPATH=src
all: rserver
rserver: rserver.c 
clean:
	rm -rf rserver *.o
.PHONY : clean all
