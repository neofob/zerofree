# Makefile to build zerofree and sparsify
# Tuan T. Pham

OBJS:=$(patsubst %.c,%.o,$(wildcard *.c))

LIBS=-lext2fs
ZF_CFLAGS=-std=gnu17 -O3 -march=x86-64-v3 -flto -Wall -Wextra

all: sparsify zerofree

%.o:%.c
	@gcc -O3 -c -o $@ $<

sparsify: sparsify.o
	@gcc -O3 -c -o sparsify.o sparsify.c

zerofree.o: zerofree.c
	@gcc $(ZF_CFLAGS) -c -o $@ $<

zerofree: zerofree.o
	@gcc $(ZF_CFLAGS) -o zerofree zerofree.o $(LIBS)

tags:$(wildcard *.c)
	@ctags *.c

clean:
	@rm -f $(OBJS) sparsify zerofree tags
