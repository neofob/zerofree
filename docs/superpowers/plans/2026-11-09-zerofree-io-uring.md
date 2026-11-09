# zerofree io_uring Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace zerofree's synchronous single-queue data path with a multi-buffered, in-order io_uring engine (queue depth 8, overlapped read/compare/write) plus a silent pread/pwrite fallback, with byte-identical observable behavior.

**Architecture:** New `zio.c`/`zio.h` engine owns the data fd and all data I/O: an 8-slot read pipeline (primes up to 8 chunks ahead via `io_uring_submit`-style ring plumbing, consumed strictly in submission order) and fire-and-forget writes of dirty sub-runs from the pre-filled buffer, drained at end of pass. A new `runscan.h` word-scan replaces the per-block bitmap test as the run iterator feeding the pipeline. `zerofree.c` keeps main/offload/pass; libext2fs keeps the fs fd for metadata; offload (fallocate/BLKZEROOUT) stays synchronous.

**Tech Stack:** C17 (`-std=gnu17`), GCC 13, `-O3 -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic`, raw `io_uring_setup`/`io_uring_enter` syscalls (no liburing), libext2fs (e2fsprogs ≥ 1.42), Linux `fallocate(FALLOC_FL_ZERO_RANGE)` / `BLKZEROOUT`. Tests: C unit tests + the existing bash black-box suite.

**Spec:** `docs/superpowers/specs/2026-11-09-zerofree-io-uring-design.md` (argues from the 2026-11-08 rewrite plan's pinned contract, which stays in force).

## Global Constraints

- **No git commits** (standing instruction). Task checkpoints are verification runs, not commits.
- `sparsify.c` and its Makefile rule: **out of scope, untouched**.
- CLI pinned: usage `usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n`, getopt `"nvdf:"`, `-t` rejected with usage + exit 1.
- Pinned messages (verbatim): `error while reading block` / `error while writing block` / `error while discarding block` / `out of memory (surely not?)` / `failed to open filesystem` (full form `%s: failed to open filesystem %s\n`); all errors exit 1.
- Verbose stream byte-exact vs the pre-change binary: initial `fprintf(stderr, "\r%4.1f%%", 0.0)` under `-v`; tick only when `(int)(percent*10)` changes; final line on **stdout** `printf("\r%llu/%llu/%llu\n", modified, free_blk, total)`. Per-block `progress()` calls stay (spec decision 11).
- Semantics pinned: only dirty free sub-runs are written; second pass modifies 0; dry-run counts, never writes; discard reads nothing; counters `modified`/`free_blk`/`total` unchanged in meaning.
- Offload contract: enabled iff `fill == 0 && !dryrun && !discard`; side fd `O_WRONLY|O_CLOEXEC`; `S_ISREG`→`FALLOC_FL_ZERO_RANGE`, `S_ISBLK`→`BLKZEROOUT`; dirty sub-runs only; `{EOPNOTSUPP,ENOSYS,ENOTTY,EINVAL}`→permanent fallback to the write path; any other error = failure. **Stays synchronous, unchanged.**
- `zf_block_is_filled` (zerofree.h), its unit test (`tests/unit_filled`), and AVX2 build flags: **untouched**.
- 64-bit fs support via `blk64_t` / `EXT2_FLAG_64BITS` (e2fsprogs ≥ 1.42 `#error` guard stays).
- Engine constants: `ZIO_MAX_SLOTS 8`, `ZIO_CHUNK_BYTES (1<<22)` (4 MiB), buffers 4096-aligned, ring 256 entries.
- I/O protocol (spec, pinned): read `res == 0` → read error (no resubmit); `0 < res < requested` → bounded resubmit of remainder (reads and writes symmetric); CQE reaping loops until the expected CQE is seen (`io_uring_enter(GETEVENTS)` may return early on kernel 7.2); `io_uring_enter` errors other than `EINTR`/`EAGAIN` → fail the run with the pinned message for that op class.
- Fallback: `io_uring_setup` failure (any errno) or `ZEROFREE_NO_URING=1` → synchronous pread/pwrite engine for the whole run, silently, with identical behavior.
- Bench harness: 4 GiB image at `/home/tuan/zfbench` (4K blocks); scenarios S1 clean pass, S2 dirty→0 (offload), S3 dirty→0xBB (write path); before/after numbers reported.

## Review Focus

Failure modes no task's tests can exercise — each is pinned by the named inspection step.

1. **io_uring_setup fails (EPERM under seccomp, ENOSYS pre-5.1)** — the run must fall back to pread/pwrite and complete identically; no leaked fd/mmap on the fallback path. Pinned: Task 4, Step 6 (inspection) + Task 5, Step 6 (sync/uring suite equivalence).
2. **Write CQE error surfaces only at `zio_drain`** — must print `error while writing block` and exit 1 (suite cannot inject EIO on image files). Pinned: Task 4, Step 6 (inspection).
3. **`EINTR` on `io_uring_enter` mid-run** — retry, never abort. Pinned: Task 4, Step 6 (inspection).
4. **Short/zero-length read results** — `res == 0` must not resubmit (no spin); `0 < res < len` must terminate. Pinned: Task 3, Step 3 (sync loop bounds) + Task 4, Step 6 (inspection).
5. **Fallback byte-equivalence** — `ZEROFREE_NO_URING=1` output byte-identical to the pre-change binary (`zerofree.pre`). Pinned: Task 5, Step 6 (cmp check).

---

### Task 1: Record official baseline numbers

**Files:**
- Create: `/home/tuan/zfbench/baseline.txt` (outside repo; bench artifact)

**Interfaces:**
- Produces: `baseline.txt` with S1/S2/S3 times for the **pre-change** binary, used by Task 6 for the before/after table.

- [ ] **Step 1: Save the pre-change binary and confirm the build is clean**

Run (in `/home/tuan/src/zerofree`): `make clean && make && cp zerofree zerofree.pre`
Expected: clean build (no warnings), `zerofree.pre` saved. Keep `zerofree.pre` for Task 5's equivalence check — do not delete it.

- [ ] **Step 2: Record S1–S3 with `zerofree.pre`**

The 4 GiB dirty image is rebuilt for each scenario (`/home/tuan/zfbench`, 4K blocks, junk = 0xAA):

```sh
cd /home/tuan/zfbench
mk() { tr '\0' '\252' < /dev/zero | dd of=b2.img bs=1M count=4096 iflag=fullblock status=none
       mke2fs -q -F -b 4096 -O ^resize_inode -E nodiscard b2.img; }
{ echo "S1 clean pass (read+compare, cached)"; mk; /home/tuan/src/zerofree/zerofree.pre -v b2.img 2>/dev/null >/dev/null
  /usr/bin/time -f %e /home/tuan/src/zerofree/zerofree.pre -v b2.img 2>/dev/null | tail -1
  echo "S2 dirty->0 (offload)"; mk
  /usr/bin/time -f %e /home/tuan/src/zerofree/zerofree.pre -v b2.img 2>/dev/null | tail -1
  echo "S3 dirty->0xBB (write path)"; mk
  /usr/bin/time -f %e /home/tuan/src/zerofree/zerofree.pre -f 0xBB -v b2.img 2>/dev/null | tail -1
} > baseline.txt 2>&1
cat baseline.txt
```

Note: `tr '\0' '\252'` yields 0xAA (252 is octal) — that is the intended junk byte.

Expected: three plausible seconds-values (S1 ≈ 1.1–1.3 s, S2 ≈ 1.5–2.5 s, S3 ≈ 1.2–1.4 s on this machine; exact values vary — record whatever is measured).

- [ ] **Step 3: Checkpoint**

Run: `cat /home/tuan/zfbench/baseline.txt`
Expected: S1/S2/S3 lines present with numeric times.

---

### Task 2: Bitmap run iterator (`runscan.h`) + unit test

**Files:**
- Create: `runscan.h` (static-inline run iterator, no dependencies)
- Test: `tests/run_scan.c`
- Modify: `Makefile` (add `tests/run_scan` rule and add it to the `unit` target)

**Interfaces:**
- Produces (exact, used by Task 5 in `zerofree.c`):

```c
struct run_it {
	const unsigned char *map;	/* block bitmap bytes (ext2 bit order) */
	uint64_t lo, hi;		/* scan range [lo, hi) in blocks */
	uint64_t pos;			/* private: next block to examine */
};
void run_it_init(struct run_it *it, const unsigned char *map,
		 uint64_t lo, uint64_t hi);
/* Next maximal run of FREE blocks (bit clear) at/after it->pos, fully
 * inside [lo, hi). Returns 0 and sets *start/*len, or 1 when done. */
int next_run(struct run_it *it, uint64_t *start, uint64_t *len);
```

- Bit order is the ext2 one, LSB-first per byte: block `b` is used iff
  `map[b >> 3] & (1u << (b & 7))`. This must match
  `ext2fs_test_block_bitmap2` semantics exactly (Task 5 depends on it).

- [ ] **Step 1: Write the failing unit test `tests/run_scan.c`**

The test builds bitmaps, runs `next_run` to exhaustion, and compares the
run list against a reference per-block scan (the exact bit test above).
Cases, each asserting an exact `(start, len)` list:

1. `all free` — 4096 zeroed bytes → single run `(0, 4096)`.
2. `all used` — 0xFF bytes → no runs.
3. `single free block` at 100 → `(100, 1)`.
4. `run across word boundary` — blocks 60..70 free → `(60, 11)`.
5. `unaligned lo/hi` — range `[100, 1000)`, free run 500..599 → `(500, 100)`; nothing outside the range reported even if the underlying bytes have free bits (set bits clear at 50 and at 2000 — must not appear).
6. `hi exactly word-aligned` — range `[0, 128)`, all free → `(0, 128)` (guards the `1ULL << 64` edge).
7. `random` — 3 seeded PRNGs × 64 KiB random bitmaps, range the full
   extent plus a mid-range `[37, 5000)` window; compare full run lists
   against the reference scan.

Print `PASS: run_scan` on success, `FAIL: ...` + exit 1 otherwise.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=gnu17 -O3 -I. -o tests/run_scan tests/run_scan.c && ./tests/run_scan`
Expected: link failure (`run_it_init`/`next_run` undefined) — the header does not exist yet.

- [ ] **Step 3: Implement `runscan.h`**

Static-inline functions, C17, `#include <stdint.h>`/`<string.h>` only.
Per-word algorithm (word `w` covers blocks `base = w*64 .. base+63`):
load 8 bytes with `memcpy` (map pointer need not be aligned), mask to the
valid window `[max(pos, lo), min(base+64, hi))` — with `hi - base == 64`
meaning "all ones" (never compute `1ULL << 64`), free bits = `~bits &
mask`; if none, advance to the next word; else `start = base +
__builtin_ctzll(free)`, then find `end` by scanning from `start`'s word
for the first used bit (`__builtin_ctzll` on `bits << (start - base_w)`),
stepping through all-used-free words until a used bit or `hi`. Set
`it->pos = end`; return 0. When `pos >= hi`, return 1.

Reference skeleton (verified against the tests):

```c
static inline int next_run(struct run_it *it, uint64_t *start, uint64_t *len)
{
	uint64_t hi = it->hi, pos;
	while (1) {
		uint64_t w = it->pos >> 6, base = w << 6;
		uint64_t lo_b = it->pos > base ? it->pos : base;
		uint64_t hi_b = hi < base + 64 ? hi : base + 64;
		uint64_t bits, free_bits, mask;
		if (lo_b >= hi_b)
			return 1;
		memcpy(&bits, it->map + (w << 3), 8);
		mask = (hi_b - base == 64 ? ~0ULL
				       : (1ULL << (hi_b - base)) - 1) &
			 (~0ULL << (lo_b - base));
		free_bits = ~bits & mask;
		if (!free_bits) {
			it->pos = hi_b;
			continue;
		}
		pos = base + __builtin_ctzll(free_bits);	/* first free */
		/* now find the first USED bit at/after pos, word by word */
		w = pos >> 6;
		while (1) {
			uint64_t b2, m, shift = (w << 6) < pos ?
						 pos - (w << 6) : 0;
			memcpy(&b2, it->map + (w << 3), 8);
			m = b2 & (~0ULL << shift);
			if (hi < (w << 6) + 64)
				m &= (1ULL << (hi - (w << 6))) - 1;
			if (m) {
				*start = pos;
				*len = (w << 6) + __builtin_ctzll(m) - pos;
				it->pos = pos + *len;
				return 0;
			}
			if (hi <= (w << 6) + 64) {	/* run runs to hi */
				*start = pos;
				*len = hi - pos;
				it->pos = hi;
				return 0;
			}
			w++;
		}
	}
}
```

(Shifts are all provably in [0,63]; the `hi - w*64` in the inner mask is
in [1,63] whenever the condition holds. If a test still disagrees, fix
the implementation to the contract above — never the tests.)

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=gnu17 -O3 -I. -o tests/run_scan tests/run_scan.c && ./tests/run_scan`
Expected: `PASS: run_scan`.

- [ ] **Step 5: Wire into the Makefile**

Add to `Makefile` (near the `tests/unit_filled` rule, same style):

```make
tests/run_scan: tests/run_scan.c runscan.h
	@gcc $(ZF_CFLAGS) -I. -o tests/run_scan tests/run_scan.c
```

and change the `unit` target to:

```make
unit: tests/unit_filled tests/run_scan
	@./tests/unit_filled
	@./tests/run_scan
```

Run: `make unit`
Expected: both unit tests run and pass.

- [ ] **Step 6: Checkpoint**

Run: `make unit && git diff --stat` (inspection only — no commit)
Expected: unit green; only `runscan.h`, `tests/run_scan.c`, `Makefile` changed.

---

### Task 3: `zio` engine — interface + synchronous (pread/pwrite) mode + unit test

**Files:**
- Create: `zio.h`, `zio.c`
- Test: `tests/zio_test.c`
- Modify: `Makefile` (add `tests/zio_test` rule + `unit` target line)

**Interfaces:**
- Produces (exact; Task 4 fills in the uring half, Task 5 consumes):

```c
/* zio.h */
#define ZIO_MAX_SLOTS 8
#define ZIO_CHUNK_BYTES ((size_t)1 << 22)	/* 4 MiB */

struct zio {
	int		fd;		/* -1 until zio_open succeeds */
	bool		use_uring;
	size_t		bs;
	unsigned char	*restrict fillbuf;	/* ZIO_CHUNK_BYTES, pre-filled */
	unsigned char	*restrict bufs[ZIO_MAX_SLOTS];
	/* private engine state follows (ring, slots, seq counters) */
};

/* Open the data fd (rdonly ? O_RDONLY : O_RDWR, both |O_CLOEXEC),
 * allocate buffers (8 slot bufs + fillbuf in uring mode; 1 slot buf +
 * fillbuf in sync mode; all 4096-aligned), memset fillbuf with fill,
 * and try io_uring_setup unless ZEROFREE_NO_URING=1 (env checked here).
 * 0 = ok (use_uring set accordingly); -1 = fd open failed; -2 = OOM.
 * uring init failure is NOT an error: use_uring stays false. */
int zio_open(struct zio *z, const char *path, size_t bs,
	     uint8_t fill, bool rdonly);

/* Free buffers, tear down the ring if any, close the fd.
 * No-op if fd == -1. Never fails. */
void zio_close(struct zio *z);

/* Prime a read of blocks [blk, blk+count) (count*bs <= ZIO_CHUNK_BYTES).
 * uring: submit now (next in-order seq). sync: no-op.
 * Must not be called more than ZIO_MAX_SLOTS ahead of zio_read. */
void zio_prime(struct zio *z, uint64_t blk, unsigned int count);

/* Wait for the next in-order primed read to complete; return its buffer,
 * or NULL on error (having printed "error while reading block\n" to
 * stderr). sync mode: performs the pread here (blocking loop).
 * Protocol: res==0 -> error; 0<res<len -> bounded remainder resubmit. */
unsigned char *zio_read(struct zio *z);

/* Write fillbuf[0 .. count*bs) to blocks [blk, blk+count).
 * uring: submit, return 0 immediately (completion checked at drain).
 * sync: blocking pwrite loop. Returns 1 on error (having printed
 * "error while writing block\n"). */
int zio_flush_dirty(struct zio *z, uint64_t blk, unsigned int count);

/* Wait for all in-flight writes; 0 ok, 1 on error (message printed).
 * sync: no-op, returns 0. */
int zio_drain(struct zio *z);
```

- [ ] **Step 1: Write the failing unit test `tests/zio_test.c`**

Usage: `tests/zio_test.c` compiles to `tests/zio_test`; run as
`./tests/zio_test sync` or `./tests/zio_test uring` — the arg selects the
engine by `setenv("ZEROFREE_NO_URING", "1", 1)` (sync) or
`unsetenv` (uring) **before** `zio_open`. Scratch file under
`$TMPDIR` (default `/tmp`), e.g. `zio_test.img`, 9.5 MiB (2432 blocks at
4096), deleted at exit.

Pattern function (deterministic, different per offset so any ordering
mistake is visible): `pat(off) = (uint8_t)((off * 2654435761u) >> 24) ^
(uint8_t)(off >> 8)`.

Test sequence:
1. Fill the file with `pat` (pwrite loop or `ftruncate`+write), `fsync`.
2. `zio_open(&z, path, 4096, 0x5A, false)` → assert 0; `z.fd >= 0`.
   In sync mode assert `z.use_uring == false`; in uring mode assert
   `z.use_uring == true` (this machine has io_uring; if the test host
   ever lacks it, uring mode should `exit 77`-style SKIP, not FAIL —
   print `SKIP: zio_test (no io_uring)` and exit 0).
3. Prime+read three chunks in order: `(0, 1024)`, `(1024, 1024)`,
   `(2048, 384)` — the third is the non-1024 tail (1.5 MiB). After each
   `zio_read`, `memcmp` the buffer against `pat` over `count*4096`
   bytes at the chunk's file offset. Assert the buffer pointers for the
   three reads are **three distinct** allocations (uring mode); in sync
mode `zio_read` legitimately returns the same buffer each time — the
per-read `memcmp` before the next read (already the test structure) is
what pins correctness there.
4. `zio_flush_dirty(&z, 100, 50)` → 0; `zio_drain(&z)` → 0; verify on
   disk that file bytes `[409600, 409600+204800)` are all `0x5A` and
   that byte 409600-4096 and 409600+204800 are still `pat` (sub-run
   bounds).
5. `zio_close(&z)`; rerun `fopen`-level byte check of one untouched
   region (e.g. block 0) to confirm no stray writes.
6. Print `PASS: zio_test (<mode>)` / `FAIL: ...` + exit 1.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=gnu17 -O3 -I. -o tests/zio_test tests/zio_test.c zio.c`
Expected: compile failure — `zio.h`/`zio.c` do not exist.

- [ ] **Step 3: Implement `zio.h` + the synchronous half of `zio.c`**

- `zio_open`: open fd (`O_CLOEXEC | (rdonly ? O_RDONLY : O_RDWR)`),
  `fstat` sanity not needed; allocate `fillbuf` and
  (uring-mode: 8 × `bufs[i]`; sync-mode: only `bufs[0]`) via
  `aligned_alloc(4096, ZIO_CHUNK_BYTES)`; `memset(fillbuf, fill, ...)`;
  `use_uring = false` in this task (Task 4 adds the uring attempt after
  buffer allocation). Return codes per the interface.
- `zio_prime`: no-op (record nothing).
- `zio_read`: `len = <count of the pending chunk>` — the engine must
  remember geometry: keep `pending_blk/pending_count` set by `zio_prime`
  (sync mode: last prime is the read to perform; uring mode Task 4 uses
  per-slot geometry). Loop:
  `r = pread(fd, buf+done, len-done, off+done)`; `r < 0`: `EINTR` retry,
  else print `error while reading block\n`, return NULL; `r == 0`: same
  error (pinned protocol); else `done += r`. Returns `buf`.
- `zio_flush_dirty`: pwrite loop from `fillbuf`, same error discipline,
  prints `error while writing block\n` on failure, returns 1.
- `zio_drain`: `return 0`.
- `zio_close`: free bufs/fillbuf, `close(fd)` if `fd >= 0`, `fd = -1`.
- Zero private state beyond `pending_blk/pending_count/done` for this
  task (Task 4 adds ring/slot state).

- [ ] **Step 4: Run test to verify it passes (sync mode)**

Run: `./tests/zio_test sync`
Expected: `PASS: zio_test (sync)`.

- [ ] **Step 5: Wire into the Makefile**

```make
tests/zio_test: tests/zio_test.c zio.c zio.h
	@gcc $(ZF_CFLAGS) -I. -o tests/zio_test tests/zio_test.c zio.c
```

Extend the `unit` target:

```make
unit: tests/unit_filled tests/run_scan tests/zio_test
	@./tests/unit_filled
	@./tests/run_scan
	@./tests/zio_test sync
	@./tests/zio_test uring
```

Note: `./tests/zio_test uring` in the `unit` target will SKIP (exit 0,
`SKIP: zio_test (no io_uring)`) until Task 4 lands — acceptable.

Run: `make unit`
Expected: all pass (uring line prints SKIP).

- [ ] **Step 6: Checkpoint**

Run: `make unit && git diff --stat` (inspection only)
Expected: unit green; new files `zio.h`, `zio.c`, `tests/zio_test.c`; Makefile modified.

---

### Task 4: `zio` engine — io_uring half

**Files:**
- Modify: `zio.c` (ring plumbing + uring paths for the four verbs), `zio.h` (private state comment only if signatures change — they must not)

**Interfaces:**
- Consumes: Task 3's `zio.h` API unchanged; `zio_open`'s `ZEROFREE_NO_URING` check.
- Produces: `./tests/zio_test uring` passing; `use_uring == true` on this host.

- [ ] **Step 1: Implement the uring attempt in `zio_open`**

After buffer allocation, unless `ZEROFREE_NO_URING` is set:
`io_uring_setup(256, &params)` via
`syscall(SYS_io_uring_setup, ...)` (include `<linux/io_uring.h>`,
`<sys/syscall.h>`; use `SYS_io_uring_setup`/`SYS_io_uring_enter`).
On success: mmap the CQ ring, SQ ring, and SQE array per `params`
offsets/sizes (standard three `mmap`s, `MAP_SHARED|MAP_POPULATE`); set
`use_uring = true`. On **any** failure: close nothing (nothing is open),
leave `use_uring = false`, no message. Ring state lives in a
`struct ring` member of `struct zio` (private to `zio.c`); all pointers
NULL when unused so `zio_close` teardown is a uniform "unmap if
non-NULL, close if `fd >= 0`".

- [ ] **Step 2: Implement uring `zio_prime` / `zio_read`**

Slot model (private): `struct slot { unsigned char *buf; uint64_t blk,
count, done_bytes; bool in_flight; int res; } slots[ZIO_MAX_SLOTS]`,
indexed by `seq & (ZIO_MAX_SLOTS-1)`; `next_seq` (assigned in
`zio_prime`), `want_seq` (next to consume in `zio_read`),
`in_flight` counter. CQE `user_data`: reads = `seq`; writes =
`seq | (1ULL << 63)` (write seqs are a separate counter).

- `zio_prime`: `seq = next_seq++`; fill the slot (geometry, `done_bytes
  = 0`); `sqe = get_sqe()`; if NULL → `io_uring_enter(pending, 0, 0)`
  and retry once; issue `IORING_OP_READ` with `offset = blk*bs`,
  `len = count*bs`, `addr = buf`, `user_data = seq`; `in_flight++`.
  **Submission policy (pinned):** SQEs are submitted eagerly — after
  writing each SQE, `io_uring_enter(k, 0, 0, NULL, 0)` where `k` is the
  number of SQEs written since the last enter (a `pending` counter).
  `zio_read`/`zio_drain` then only reap, with `to_submit = 0`.
- `zio_read`: `seq = want_seq++`; loop reaping:
  `io_uring_enter(0, 1, IORING_ENTER_GETEVENTS)` in a loop until slot
  `seq` is no longer in-flight (reaping must not stop at the first CQE —
  `enter` can return early, and CQEs arrive out of order; route each CQE
  by the write-tag bit: read CQE marks its slot complete with `res`,
  write CQE handled by the write bookkeeping). Then: `res < 0` → print
  `error while reading block\n`, return NULL; `res == 0` → same error;
  `res < count*bs - done_bytes` → advance `done_bytes`, resubmit
  remainder for the same seq (SQE `offset += done_bytes`,
  `addr = buf + done_bytes`, `len` remainder), continue the reap loop;
  complete → return `buf`. `io_uring_enter` returning -1: `EINTR`/
  `EAGAIN` → retry; anything else → read error path (same message).
- **Short-read termination**: `res > 0` strictly advances `done_bytes`
  each iteration, so the loop terminates; the `res == 0` case errors
  instead of spinning (pinned protocol).

- [ ] **Step 3: Implement uring `zio_flush_dirty` / `zio_drain`**

- `zio_flush_dirty`: issue `IORING_OP_WRITE` per sub-run
  (`addr = fillbuf`, `offset = blk*bs`, `len = count*bs`,
  `user_data = wseq | (1ULL << 63)`); `get_sqe == NULL` →
  `io_uring_enter(submit, 0)`, retry. Return 0. Track in-flight writes
  (counter + the `addr/offset` needed to resubmit a short remainder).
- `zio_drain`: reap in a loop until in-flight writes are 0
  (`io_uring_enter(0, 1, IORING_ENTER_GETEVENTS)`; ignore/complete read
  CQEs — none should be pending). Write CQE: `res < 0` → error;
  `0 < res < len` → resubmit remainder (same bounded protocol); done.
  Any error → print `error while writing block\n`, return 1.
- Write-completion bookkeeping is private to `zio.c` (a small array of
  pending-write records keyed by `wseq`); the header API does not
  change.

- [ ] **Step 4: Run the unit test in both modes**

Run: `make unit`
Expected: `PASS: zio_test (sync)`, `PASS: zio_test (uring)`, run_scan
and unit_filled still pass.

- [ ] **Step 5: Sanity-check dependencies**

Run: `ldd tests/zio_test | grep -i uring; echo "rc=$? (expect 1: no liburing)"`
Expected: no `liburing` in the dependency list (raw syscalls only);
the uring half of the unit test passed in Step 4, which only happens
through the real ring.

- [ ] **Step 6: Review-focus inspection (no test can cover these)**

Read `zio.c` and confirm, stating each finding:
1. Every `io_uring_setup` failure path leaves no mmap/fd behind and
   `use_uring == false`; `zio_close` is idempotent-safe in both states
   (Review Focus 1).
2. A write CQE with `res < 0` arriving only during `zio_drain` produces
   `error while writing block` and return 1 (Review Focus 2).
3. No `io_uring_enter` call site exits on `EINTR`; non-`EINTR`/
   `EAGAIN` failures map to the op class's pinned message (Review Focus 3).
4. Both read and write remainder-resubmit loops terminate: `res > 0`
   advances the offset; `res == 0` errors instead of resubmitting
   (Review Focus 4).

- [ ] **Step 7: Checkpoint**

Run: `make unit && make`
Expected: all unit tests green (including `zio_test uring`); zerofree
binary still builds (integration happens in Task 5).

---

### Task 5: Integrate into `zerofree.c` + Makefile; full suite in both modes

**Files:**
- Modify: `zerofree.c` (ctx, main, offload-adjacent `flush_dirty`, `zf_pass`), `Makefile`

**Interfaces:**
- Consumes: `zio.h` (Task 3/4), `runscan.h` (Task 2).
- Produces: the shipped `zerofree` binary; `make test` runs the suite twice.

- [ ] **Step 1: Update `struct zf_ctx` and buffer ownership**

- Replace `buf`/`fillbuf`/`chunk_blocks` fields: add `struct zio zio;`
  (first member, or wherever fits), keep `bs`, set
  `chunk_blocks = max(1, ZIO_CHUNK_BYTES / bs)`. `fill`, `dryrun`,
  `discard`, `verbose`, counters, `zero_fd`, `method` unchanged.
- `main`: remove both `aligned_alloc` calls and the `memset(fillbuf,
  ...)`. After `ext2fs_read_block_bitmap` succeeds, and when
  `!discard`: `rc = zio_open(&c.zio, fs_path, c.bs, c.fill, c.dryrun)`;
  `rc == -1` → `fprintf(stderr, "%s: failed to open filesystem %s\n",
  argv[0], fs_path)`, close `c.zero_fd` if `>= 0`, `ext2fs_close(c.fs)`, return 1; `rc == -2` →
  `fprintf(stderr, "%s: out of memory (surely not?)\n", argv[0])`, close
  `c.zero_fd` if `>= 0`, `ext2fs_close(c.fs)`, return 1.
- `finish()`: replace `free(c->buf); free(c->fillbuf);` with
  `zio_close(&c.zio);` (no-op when discard mode never opened it).
- Final success path: same replacement before the `ext2fs_close` return
  check.

- [ ] **Step 2: Replace the I/O calls in the pass**

- `flush_dirty()`: `io_channel_write_blk64(...)` becomes
  `zio_flush_dirty(&c->zio, blk, count)`. The offload
  (`ZERO_RANGE`/`ZERO_BLKZEROOUT`) logic and its permanent-fallback
  handling are **byte-for-byte unchanged**.
- `read_piece()` goes away: the pass calls `zio_read` directly and prints
  nothing itself (`zio_read` owns the pinned message).

- [ ] **Step 3: Restructure `zf_pass` around the pipeline**

Keep `zf_pass`'s signature, verbose open/final prints, and the
`free_blk`/`modified`/`progress` bookkeeping loops **exactly as today**
(spec decision 11: per-block). Split into two mode functions, both fed
by the Task 2 run iterator initialized with
`run_it_init(&it, c->fs->block_map->map, s_first_data_block,
ext2fs_blocks_count(c->fs->super))`:

- `zf_pass_discard(c, &it)`: `while (next_run(...))`: `io_channel_discard`
  (unless dryrun) on the **whole run**, per-block counting/progress loop
  as today, pinned discard error on failure.
- `zf_pass_fill(c, &it)`: the pipeline. Keep a run cursor
  (`have_run`, `run_pos`, `run_end`) and an 8-slot work-item ring
  `win[ZIO_MAX_SLOTS]` of `{uint64_t blk; unsigned int count;}` with
  `head`/`nwin` indices:
  - `window_fill`: while `nwin < ZIO_MAX_SLOTS`, pull the next chunk
    (`min(chunk_blocks, run_end - run_pos)`; advance the run cursor,
    fetching the next run when the current one is exhausted) and
    `zio_prime(c.zio, chunk.blk, chunk.count)`, storing the work item.
    Stop (and remember "no more runs") when the iterator is exhausted.
  - consume loop while `nwin > 0`: `buf = zio_read(&c.zio)` (NULL →
    return 1); the existing dirty-sub-run scan over `buf` using
    `zf_block_is_filled(buf + nb * c->bs, c->bs, c.fill)` and, per
    maximal dirty sub-run, `if (!c->dryrun && flush_dirty(c, blk + nb,
    i - nb)) return 1;` with `modified`/`free_blk`/`progress` exactly as
    today; then rotate `head`, decrement `nwin`, `window_fill`.
  - invariants (comment them in the code): at most `ZIO_MAX_SLOTS` primes
    ahead of the consume point; consumption is in submission order, which
    `zio_read` enforces; a prime never crosses a run boundary, the window
    does.
  - after the loop: `if (zio_drain(&c.zio)) return 1;`
  - dry-run: `zio_open`'s `rdonly` fd makes writes impossible by
    construction; the existing `!c->dryrun` guards around `flush_dirty`
    stay.

- [ ] **Step 4: Makefile**

```make
ZEROFREE_OBJS = zerofree.o zio.o

zerofree.o: zerofree.c zerofree.h zio.h runscan.h
zio.o: zio.c zio.h

zerofree: $(ZEROFREE_OBJS)
	@gcc $(ZF_CFLAGS) -o zerofree $(ZEROFREE_OBJS) $(LIBS)

test: zerofree unit
	@bash tests/test_suite.sh ./zerofree
	@ZEROFREE_NO_URING=1 bash tests/test_suite.sh ./zerofree
```

(sparsify rules untouched; `unit` target as left by Task 4.)

- [ ] **Step 5: Run everything**

Run: `make clean && make && make test`
Expected: clean build; `RESULT: 25 passed, 0 failed` printed **twice**
(uring run, then `ZEROFREE_NO_URING=1` run).

- [ ] **Step 6: Fallback byte-equivalence vs the pre-change binary**

Build a small dirty image (suite helper style: 8 MiB, 1K blocks, 0xAA
junk + `mke2fs -E nodiscard`) plus one 4K-block image. For each:
`./zerofree.pre -v img > out.pre 2>&1` then
`ZEROFREE_NO_URING=1 ./zerofree -v img > out.new 2>&1` on fresh copies,
and `cmp out.pre out.new`. Repeat for one `-f 0xBB` run and one `-n` run.
Expected: identical streams (same progress ticks, same final
`modified/free/total` line) — Review Focus 5.

- [ ] **Step 7: Checkpoint**

Run: `make test` (again, from clean)
Expected: green twice; no warnings in the build log.

---

### Task 6: Bench before/after + final report

**Files:**
- Create: `/home/tuan/zfbench/after.txt` (bench artifact, outside repo)

**Interfaces:**
- Consumes: Task 1's `baseline.txt`, the shipped `zerofree`.

- [ ] **Step 1: Re-run S1–S3 with the new binary**

Same script as Task 1, Step 2, with `./zerofree` instead of
`zerofree.pre`, output to `/home/tuan/zfbench/after.txt`. Additionally
record the `ZEROFREE_NO_URING=1` S1 time (fallback sanity) in the same
file.

- [ ] **Step 2: Verify acceptance**

- All unit tests + suite ×2 green (Task 5 output, re-run from clean).
- `fsck.ext2 -fn` clean on the S1–S3 images after each run.
- `ldd zerofree`: no new dependencies (no liburing).
Expected: S1 ≤ ~0.9 s and S3 ≤ ~1.0 s (the ~1.5–2× goal for cached
passes); S2 dominated by NVMe read bandwidth and ≤ baseline. If any
scenario regresses >10%, stop and investigate before reporting (spec
anti-goals).

- [ ] **Step 3: Write the final summary**

Report to the user: before/after table (S1/S2/S3, plus sync-fallback S1),
what changed per file, the review-focus findings from Task 4 Step 6, and
the remaining known limits (single-threaded compare, no O_DIRECT —
spec non-goals, revisit only if a future bench justifies it). No git
commit (standing instruction).
