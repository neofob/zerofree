# Makefile to build zerofree and sparsify
# Tuan T. Pham

OBJS:=$(patsubst %.c,%.o,$(wildcard *.c))

LIBS=-lext2fs -lpthread

all: sparsify zerofree

%.o:%.c
	@gcc -g -c -o $@ $<

sparsify: sparsify.o
	@gcc -c -o sparsify.o sparsify.c

zerofree: zerofree.o
	@gcc -g -o zerofree zerofree.o $(LIBS)

tags:$(wildcard *.c)
	@ctags *.c
clean:
	@rm -f $(OBJS) sparsify zerofree tags
