CC=gcc
CFLAGS=-O3 -march=native -fstrict-aliasing -Wall
#CFLAGS=-g3 -O0 -fstrict-aliasing -Wall -pg --coverage

all: dump
clean:
	rm dump
dump: Makefile readtrace.h dump.c
	$(CC) $(CFLAGS) -o dump dump.c
