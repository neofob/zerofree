# Makefile to build zerofree and sparsify
# Tuan T. Pham

OBJS:=$(patsubst %.c,%.o,$(wildcard *.c))

LIBS=-lext2fs

# zerofree: modern single-threaded tool -- C17, x86-64-v3 (AVX2) baseline,
# LTO, no dependencies beyond libext2fs (e2fsprogs >= 1.43).
ZF_CFLAGS=-std=gnu17 -O3 -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic

all: sparsify zerofree

%.o:%.c
	@gcc -g -c -o $@ $<

sparsify: sparsify.o
	@gcc -c -o sparsify.o sparsify.c

ZEROFREE_OBJS = zerofree.o zio.o

zerofree.o: zerofree.c zerofree.h zio.h runscan.h
	@gcc $(ZF_CFLAGS) -c -o zerofree.o zerofree.c

zio.o: zio.c zio.h
	@gcc $(ZF_CFLAGS) -c -o zio.o zio.c

zerofree: $(ZEROFREE_OBJS)
	@gcc $(ZF_CFLAGS) -o zerofree $(ZEROFREE_OBJS) $(LIBS)

unit: tests/unit_filled tests/run_scan tests/zio_test
	@./tests/unit_filled
	@./tests/run_scan
	@./tests/zio_test sync
	@./tests/zio_test uring

tests/unit_filled: tests/unit_filled.c zerofree.h
	@gcc $(ZF_CFLAGS) -I. -o tests/unit_filled tests/unit_filled.c

tests/run_scan: tests/run_scan.c runscan.h
	@gcc $(ZF_CFLAGS) -I. -o tests/run_scan tests/run_scan.c

tests/zio_test: tests/zio_test.c zio.c zio.h
	@gcc $(ZF_CFLAGS) -I. -o tests/zio_test tests/zio_test.c zio.c

test: zerofree unit
	@bash tests/test_suite.sh ./zerofree
	@ZEROFREE_NO_URING=1 bash tests/test_suite.sh ./zerofree

tags:$(wildcard *.c)
	@ctags *.c
clean:
	@rm -f $(OBJS) sparsify zerofree tags tests/run_scan tests/zio_test
