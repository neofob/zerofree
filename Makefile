# Makefile to build zerofree and sparsify
# Tuan T. Pham

OBJS:=$(patsubst %.c,%.o,$(wildcard *.c))

LIBS=-lext2fs

all: sparsify zerofree

%.o:%.c
	@gcc -g -march=native -std=c11 -c -o $@ $<

sparsify: sparsify.o
	@gcc -c -o sparsify.o sparsify.c

zerofree: zerofree.o
	@gcc -g -O3 -march=native -mtune=native -mavx2 -mavx512f -mavx512bw -ffast-math -funroll-loops -std=c11 -o zerofree zerofree.o $(LIBS)

tags:$(wildcard *.c)
	@ctags *.c
clean:
	@rm -f $(OBJS) sparsify zerofree tags
