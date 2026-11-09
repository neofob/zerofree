# zerofree Modern Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite `zerofree` as a modern (C17, x86-64-v3/AVX2), single-threaded, batched-I/O, correct free-block zeroing tool for ext2/3/4 on Linux, with an in-repo test suite (unit + black-box).

**Architecture:** One pass over the block bitmap: maximal runs of consecutive free blocks are coalesced into ≤1 MiB batched `io_channel` I/O; dirty sub-runs within each chunk are coalesced into single writes (or a single kernel zero-offload call when fill=0). All threading is removed. The only extracted unit is the vectorized fill-compare predicate in `zerofree.h`, which gets real unit tests; everything else is pinned by black-box CLI tests.

**Tech Stack:** C (gnu17), GCC 13 with `-O3 -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic`, libext2fs (e2fsprogs ≥ 1.42, `EXT2_FLAG_64BITS`), Linux `fallocate(FALLOC_FL_ZERO_RANGE)` / `BLKZEROOUT`. Test harness: bash + python3 + mke2fs/debugfs/dumpe2fs/fsck.ext2.

**Spec:** This plan. (No separate design doc — the user opted out of the brainstorming Q&A and approved direction inline: remove `-t` entirely, batched run I/O, x86-64-v3 baseline, kernel zero-offload default-on with the remanence caveat documented, unit + black-box tests in the repo. This matches the decisions the user made on this same project in `session-ses_09ed.md`.)

**Bugs in the current `zerofree.c` that this rewrite must eliminate (deep-dive, 2026-11-08):**
- **B1 — silent skip of free blocks (thread mode):** `end_blk = part_size*(i+1)` omits the `s_first_data_block` offset that `start_blk` includes; on 1 KiB-block filesystems every thread stops one block early and boundary free blocks are never zeroed. Eliminated by removing threading (Task 3); the single pass covers `[s_first_data_block, blocks_count)` exactly.
- **B2 — `-t 0` → SIGFPE:** validation accepts 0 (`thread_count < 0`), then divides by zero. Eliminated by removing `-t`.
- **B3 — format-specifier UB:** `fprintf("USE %d threads\n", long)`; also old verbose final line uses `%u` for `unsigned long`. Fixed: `-t` gone; final line pinned to `%llu`.
- **B4 — swallowed I/O errors in thread mode:** threads set `*error`, `main` joins with `NULL`, exits 0 on failure. Fixed: single pass exits 1 with the pinned message on first I/O error (Review Focus #1).
- **B5 — unchecked `pthread_create` → barrier deadlock** if a spawn fails. Eliminated by removing threading.
- **B6 — no 64-bit filesystem support:** opened without `EXT2_FLAG_64BITS`, uses 32-bit `ext2fs_test_block_bitmap`. Fixed: pinned 64-bit API (Global Constraints); pinned by suite tests 21/22.
- **B7 — misc:** `bailout()` exits without `ext2fs_close`; unused variables; stray leading space in the discard error message; `-t` mode emits no progress at all. Fixed in the rewrite (close-on-error, clean build under `-Wpedantic`, normalized message, single progress path).

## Global Constraints

- CLI (pinned, byte-exact): `usage: %s [-n] [-v] [-d] [-f fillval] filesystem\n`; getopt string `"nvdf:"`; **`-t` is gone** — passing it must print usage and exit 1.
- Exit codes: 0 success; 1 any usage, I/O, or open error.
- Verbose stream (pinned byte-exact vs old single-thread mode): initial `fprintf(stderr, "\r%4.1f%%", 0.0)` when `-v`; re-emit only when `(int)(percent*10)` changes; final line on **stdout** `printf("\r%llu/%llu/%llu\n", modified, free_blk, total)`.
- Error messages (pinned): `error while reading block` / `error while writing block` / `error while discarding block` (stray leading space from old code is normalized away) / `out of memory (surely not?)` — each followed by exit 1.
- Semantics (pinned): only free blocks whose content differs from the fill value are written (sparse images stay sparse); dry-run counts but never writes/discards; discard mode discards every free block without reading and counts them all as modified.
- `CHUNK_BYTES = 1 << 20`; both buffers `aligned_alloc(4096, CHUNK_BYTES)`; `chunk_blocks = max(1, CHUNK_BYTES / blocksize)`.
- Open flags: `EXT2_FLAG_RW | EXT2_FLAG_64BITS`; use `blk64_t`, `io_channel_read_blk64`/`io_channel_write_blk64`, `ext2fs_test_block_bitmap2`, `ext2fs_blocks_count`, `ext2fs_free_blocks_count`.
- Offload contract: enabled iff `fill == 0 && !dryrun && !discard`; side fd `open(path, O_WRONLY|O_CLOEXEC)`; `S_ISREG` → `fallocate(FALLOC_FL_ZERO_RANGE)`; `S_ISBLK` → `BLKZEROOUT`; applied **only to dirty sub-runs**; first error in `{EOPNOTSUPP, ENOSYS, ENOTTY, EINVAL}` permanently falls back to pwrite for the run; any other error = write failure (message + exit 1). Remanence caveat documented in source header: ZERO_RANGE is metadata-level zeroing.
- C standard: `-std=gnu17` (gcc 13.3 here has no `gnu23`); `stdbool.h`/`stdint.h`/`restrict`; no pre-1.42 e2fsprogs compat shims — hard `#error` if `EXT2_FLAG_64BITS` missing.
- `sparsify.c` and its Makefile rule are **out of scope, untouched**.
- **No git commits** unless the user asks (standing instruction).

## Review Focus

Failure modes the tests don't (can't) exercise — each is pinned by code inspection at Task 3, Step 5:

1. **I/O error mid-run (EIO)** — no fault-injection hook; verify the error path prints the pinned message, closes the fs (ignore close ret), frees buffers, exits 1. Old code leaked the open fs here.
2. **Inconsistent superblock** (`s_free_blocks_count == 0` while bitmap has free blocks) — percent math must not divide by zero: use `free_total ? free_total : free_blk` as denominator (free_blk ≥ 1 inside the loop → percent 100.0).
3. **Open failure of the offload side fd** (permissions race between ext2fs open and side open) — offload silently disabled, zero behavior change; no output, no exit.
4. **Block device without discard support** — `io_channel_discard` error → pinned discard error message + exit 1 (unchanged from old behavior).
5. **`-f` parsing** — `strtol(optarg, &end, 0)`: reject empty/garbage; value must be 0–255 after wrap to `unsigned int` (so `-f -1` → 0xFFFFFFFF → rejected); base-0 means `010` is octal 8 — same as old behavior, documented in a comment.

---

### Task 1: Baseline binary + black-box test suite (RED)

**Files:**
- Create: `tests/test_suite.sh`
- Scratch: `/tmp/opencode/zerofree.orig` (baseline, untracked)

**Interfaces:**
- Produces: `tests/test_suite.sh <binary>` → prints `PASS:`/`FAIL:` lines, final `RESULT: N passed, M failed`, exit 0 iff M==0. Test names below are the pinned contract Task 3 must satisfy.

- [x] **Step 1: Build and save the current (old) binary as baseline**

Run: `make clean && make && cp zerofree /tmp/opencode/zerofree.orig`
Expected: builds (with the known `%d` format warning — that is bug B3, keep the warning in mind); binary saved. This binary is the reference for the verbose byte-identity check (Task 3) and benchmarks (Task 4).

- [x] **Step 2: Write `tests/test_suite.sh`**

Self-contained: generates its own images under `/tmp/opencode` (or `$TMPDIR`), takes the binary path as `$1`. Helpers:
- `make_img <path> <bs> <size_kb> [extra mke2fs args...]`: `dd if=/dev/zero` then `mke2fs -q -F -b $bs -O ^resize_inode [extra]`. For **dirty** images, pre-fill the raw file with 0xAA (`tr '\0' '\252' < /dev/zero | dd iflag=fullblock`) **before** `mke2fs -E nodiscard` so every data block starts dirty — no debugfs bulk writes.
- `check_free_blocks <img> <bs> <hexbyte>`: python3 — parse `dumpe2fs` "Free blocks:" ranges, verify every block's bytes equal the expected byte; print offending blocks on failure.
- `modified_count <img...>`: run `"$BIN" -v <img>` (or given args), `tr '\r' '\n' | tail -1 | cut -d/ -f1`.
- Data-intact: write a 64KiB random `keep.bin` via `debugfs -w -R "write keep.bin keep"`, later extract with `debugfs -R "dump keep <out>"` and md5-compare.

Tests (exact names, in order):
1. `bs=1024 free blocks zeroed` — dirty 8 MiB image, run, `check_free_blocks .. 00`
2. `bs=1024 fsck clean` — `fsck.ext2 -fn`
3. `bs=1024 used data intact` — keep-file md5 equal
4. `bs=4096 free blocks zeroed`
5. `bs=4096 fsck clean`
6. `bs=4096 used data intact`
7. `dry-run unchanged` — `md5` of image identical after `"$BIN" -n`
8. `dry-run reports modified>0`
9. `second pass modifies 0` — after a real run, `modified_count` == 0
10. `fillval 0xAA applied` — `"$BIN" -f 0xAA`; `check_free_blocks .. aa`
11. `fillval second pass modifies 0`
12. `discard: free blocks read zero` — `"$BIN" -d`
13. `discard: disk usage shrank` — `du -k` before > after (hole-punching must actually deallocate; old per-1KiB-block punch fails this on 4 KiB host blocks)
14. `discard: fsck clean`
15. `dry-run+discard unchanged` — image md5 identical after `-n -d`
16. `-t rejected with usage` — rc≠0 and stderr matches `usage:`
17. `invalid -f rejected` — `-f 256`, `-f abc`, `-f` (empty) each rc≠0
18. `no args usage` — rc≠0, stderr matches `usage:`
19. `verbose summary format` — final line matches `^[0-9]+/[0-9]+/[0-9]+$`
20. `single-byte-dirty detection` — fully zero image; pick two free blocks from a range of length ≥ 5 (blocks a+1 and a+3); `printf '\377' | dd` corrupt first byte of one and last byte of the other; `modified_count` must be exactly `2`; then real run zeroes them (`check_free_blocks .. 00`)
21. `64-bit fs zeroed` — `make_img .. 1024 16384 -t ext4 -O 64bit` dirty image; run; `check_free_blocks .. 00`
22. `64-bit fs fsck clean`
23. `ext4 vs tmpfs runs byte-identical` — same dirty image copied to `/home/tuan/zerofree-bench/` (ext4) and tmpfs; run on both; `cmp` the results (pins offload-vs-fallback equivalence)
24. `ext4 run fsck clean`
25. `success exit code 0`

Suite must be idempotent (re-runnable) and print a final `RESULT:` line.

- [x] **Step 3: Run the suite against the old binary and record the RED state**

Run: `bash tests/test_suite.sh /tmp/opencode/zerofree.orig`
Expected: mixed — RED (expected failures on old code): `16` (old accepts `-t`), `13` (old's per-block 1 KiB hole punch doesn't shrink `du`), `21/22` (old can't open 64-bit fs: no `EXT2_FLAG_64BITS`). Everything else should pass — anything else failing means the test is wrong; fix the test, not the expectation.

- [x] **Step 4: Record results** in this file under "RED baseline" (append a dated note with the pass/fail list).

**RED baseline (2026-11-08, vs `/tmp/opencode/zerofree.orig`):** 25 passed, 2 failed —
- FAIL `discard: disk usage shrank (7620 -> 7620 KiB)` (old per-1KiB hole punch can't deallocate 4 KiB host blocks) — expected RED, fixed by run-sized discards.
- FAIL `-t rejected with usage (rc=0)` (old binary accepts `-t`) — expected RED, fixed by removing `-t`.
- All other 25 pass on the old binary, including 21/22 (`64-bit fs zeroed` / `fsck clean`): caveat — the 16 MiB test image's block counts still fit in 32 bits, so the old 32-bit API works there by luck; tests 21/22 are **guards** (the new code must also open and zero 64-bit-feature images), not discriminators for bug B6. True B6 exposure (block counts > 2³²) needs a multi-TiB filesystem and cannot be image-tested.

### Task 2: `zf_block_is_filled` unit-tested compare (TDD)

**Files:**
- Create: `tests/unit_filled.c`
- Create: `zerofree.h`

**Interfaces:**
- Produces: `static inline bool zf_block_is_filled(const unsigned char *restrict p, size_t n, uint8_t fill)` in `zerofree.h` — true iff all `n` bytes of `p` equal `fill`; `n == 0` → true. Consumed by `zerofree.c` (Task 3) and the unit test.

- [x] **Step 1: Write `tests/unit_filled.c` (failing first)**

Hand-rolled harness (`CHECK(cond)` macro printing name, counting failures; `main` returns nonzero on failure). Cases, exact:
- `n=1024` all-`0x00`, fill `0x00` → true
- `n=1024` all-`0xFF`, fill `0xFF` → true
- first byte dirty (rest fill) → false
- middle byte dirty (off = n/2) → false
- last byte dirty (off = n-1) → false
- single non-fill byte inside all-fill (off = 37, n = 4096) → false
- `n=1` equal → true; `n=1` different → false
- `n=0` → true
- `n=1000` (not multiple of 8) all-fill → true; same with byte at `n-1` dirty → false
- `n=65536` all-fill (crosses many 512B sub-chunks) → true; byte at 511 dirty → false
- patterned fill `0x5A`, `n=4096` → true; one byte off → false
- **fuzz:** seeded LCG, 20000 rounds: random `n` in `[0, 8192]`, random fill; buffer = fill with 0/1 random bytes flipped; compare result against a naive byte loop. All must agree.

- [x] **Step 2: Verify it fails**

Run: `gcc -std=gnu17 -O3 -march=x86-64-v3 -Wall -Wextra -Wpedantic -I. -o tests/unit_filled tests/unit_filled.c`
Expected: **compile error** — `zerofree.h: No such file`. (RED: the unit under test doesn't exist yet.)

- [x] **Step 3: Implement `zerofree.h`**

`zf_block_is_filled`: O(1) first-byte bail (`p[0] != fill → false`), then 512-byte sub-chunks: accumulate `uint64_t w` loaded via `memcpy(&w, ...)` (strict-aliasing safe), `acc |= w ^ fill8` where `fill8` splats the byte; return false at first nonzero sub-chunk (early exit granularity 512B), true when exhausted. `static inline`, `restrict` on `p`. Under `-march=x86-64-v3` this auto-vectorizes to AVX2 `vpxor`/`vpor`.

- [x] **Step 4: Verify it passes + AVX2 was actually emitted**

Run: compile (as Step 2) and `./tests/unit_filled`; then
`objdump -d tests/unit_filled | sed -n '/zf_block_is_filled/,/ret/p' | grep -cE 'vpor|vpxor'`
Expected: all unit tests pass; grep count > 0 (vector ops present in the predicate — proof the AVX2 target is in effect).

### Task 3: Rewrite `zerofree.c` + Makefile (GREEN)

**Files:**
- Rewrite: `zerofree.c`
- Modify: `Makefile` (zerofree rule + new `test`/`unit` targets; **sparsify rule untouched**)

**Interfaces:**
- Consumes: `zf_block_is_filled` from `zerofree.h`; Global Constraints above (pinned CLI/messages/semantics).
- Produces: `./zerofree` binary satisfying every `tests/test_suite.sh` test (names pinned in Task 1).

- [x] **Step 1: Rewrite `zerofree.c`**

Structure (implementer's discretion on internal naming; algorithm pinned):
- Header: license header + changelog entry (2026-11-08: modern x86-64 rewrite — threaded mode removed, run-batched I/O, vectorized fill compare, zero-offload) + the ZERO_RANGE remanence caveat paragraph.
- `#define _GNU_SOURCE`; includes `errno.h fcntl.h linux/fs.h stdbool stdint stdio stdlib string sys/ioctl sys/stat unistd ext2fs/ext2fs.h`; `#ifndef EXT2_FLAG_64BITS #error`.
- Context struct carrying: `ext2_filsys *fs`, the two aligned buffers, `blocksize`, `chunk_blocks`, `fill`/`fill8`, flags `dryrun/discard/verbose`, counters `free_blk/free_total/modified/total`, `old_percent`, offload state (`zero_fd`, `zero_supported`, method enum `{PWRITE, ZERO_RANGE, BLKZEROOUT}`).
- `progress()`: exact pinned stream behavior; denominator guard per Review Focus #2.
- `offload_init()`: only when `fill==0 && !dryrun && !discard`; `open(O_WRONLY|O_CLOEXEC)`; `fstat` → `S_ISREG`/`S_ISBLK` selects method; any open/stat failure → disabled, silent.
- `zero_range()`: `fallocate` or `BLKZEROOUT` (`struct { __u64 length; __u64 start; }`); unsupported-error set → permanently `PWRITE`; other errno → return error to caller (→ pinned write-error message + exit 1).
- `flush_dirty()`: one call per dirty sub-run; offload when supported, else `io_channel_write_blk64` from the pre-filled buffer.
- Pass loop: for `blk` in `[s_first_data_block, blocks_count)`: skip used (`ext2fs_test_block_bitmap2`); on free: count/progress; find maximal run; discard mode → single uncapped `io_channel_discard` per run (count every block modified); else process run in ≤`chunk_blocks` pieces: one `io_channel_read_blk64` per piece, per-block `zf_block_is_filled`, coalesce dirty sub-runs, `flush_dirty` each.
- Error handling per Review Focus #1: pinned message, `ext2fs_close` (ret ignored), free buffers, exit 1.
- `main`: pinned getopt/usage; `-f` validation (Review Focus #5) with `printf("fillval = %u\n", fillval)`; `ext2fs_check_if_mounted` + mounted-rw refusal (unchanged messages); `ext2fs_open` with pinned flags; `aligned_alloc` both buffers (OOM → pinned message, exit 1); `memset` fill buffer; `ext2fs_read_block_bitmap`; pass; `ext2fs_close` (error → message, exit 1); free; return 0.
- No `pthread.h` anywhere; no `-t` in the getopt string.

- [x] **Step 2: Update `Makefile`**

`ZF_CFLAGS=-std=gnu17 -O3 -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic`; `zerofree.o`/`zerofree` rules use it (`-lext2fs`); add `test:` target (`unit` + `bash tests/test_suite.sh ./zerofree`) and `unit:` target (build `tests/unit_filled` with `$(ZF_CFLAGS) -I.` and run it); `sparsify` rule and `LIBS` line otherwise unchanged; `clean` removes `tests/unit_filled`.

- [x] **Step 3: Clean build**

Run: `make clean && make 2>&1`
Expected: zero warnings, zero output; `zerofree` exists.

- [x] **Step 4: Full suite GREEN**

Run: `make test`
Expected: **all 25 suite tests + unit tests pass** (Task 1 RED items 13/16/21/22 now green).

- [x] **Step 5: Verbose byte-identity vs the old single-thread binary**

Run: create one dirty image twice (a.img, b.img, same seed); `/tmp/opencode/zerofree.orig -n -v a.img > o.out 2> o.err`; `./zerofree -n -v b.img > n.out 2> n.err`; `cmp o.out n.out && cmp o.err n.err`. Also repeat with `-d -n -v`.
Expected: both streams byte-identical (same percent cadence, same final `modified/free/total`). Any diff → investigate before proceeding (this is the compatibility contract).

- [x] **Step 6: Review-Focus code inspection**

Walk the five Review Focus items against the final source; confirm each in one line each in the session report. No code change expected; if something is off, fix and re-run Task 3 Steps 3–5.

### Task 4: Benchmarks + final verification

**Files:** scratch under `/home/tuan/zerofree-bench/` (ext4) and `/tmp/opencode/` (tmpfs); no repo files.

- [x] **Step 1: Build 1 GiB dirty benchmark image** (both storage classes)

`tr '\0' '\252' < /dev/zero | dd iflag=fullblock bs=1M count=1024` then `mke2fs -q -F -b 1024 -O ^resize_inode -E nodiscard` → ~1.03M dirty free blocks. Verify dirty count with `zerofree.orig -n -v` (expect modified ≈ 1.03M). Keep pristine copy; work on fresh copies per run.

- [x] **Step 2: ext4 benchmark (old vs new, 3 runs each)**

Zeroing: `time` old on one copy, new on another, then `sync -f` each and time that separately (old defers ~1 GiB writeback; new's offload should not). Scan-only: on a zeroed image, `time ... -n`. Discard: old vs new + `du -k` of results (old leaves ~1 GiB allocated; new should be ~1 MiB). Record `/usr/bin/time -f "%es maxrss %MkB"`.

- [x] **Step 3: tmpfs benchmark (parity check)**

1 GiB dirty image in `/tmp/opencode` (2.4 G free: pristine + one work copy). Old vs new zeroing, 3 runs each; record that offload silently falls back (tmpfs) and timings are ≈ parity or better (level-1 compare gain is small on tmpfs — report honestly).

- [x] **Step 4: Correctness at scale**

After the new binary's 1 GiB ext4 run: `zerofree -n -v` second pass reports `0/…`; `fsck.ext2 -fn` clean; `check_free_blocks` equivalent sample check on the 1 GiB image (python over dumpe2fs ranges).

- [x] **Step 5: Cleanup + report**

Remove bench images and `/home/tuan/zerofree-bench`. Final report to user: bug list (B1–B7 from the deep-dive, with which ones are eliminated by the rewrite vs explicitly fixed), what changed, test results, benchmark tables, honest caveats (offload remanence; `BLKZEROOUT` branch compile-verified only without root; fadvise deliberately not included — measured as net loss on the previous run of this project; sparsify untouched; no commits made).

---

## Final results (completed 2026-11-09)

**Build:** `make clean && make` → zero warnings under `-std=gnu17 -O3 -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic`. AVX2 confirmed in `zf_block_is_filled` (objdump: `vpbroadcast`, 34× `vpor`, 32× `vpxor`).

**Tests:**
- Unit: 16/16 (incl. 20000-round seeded-LCG fuzz vs naive loop).
- Suite: 27/27 GREEN (RED baseline was 25/27: `discard: disk usage shrank` 7620→7620 and `-t rejected` were the 2 failures on the old binary).
- Verbose byte-identity vs old single-thread binary: stdout **and** stderr byte-identical for `-n -v` and `-d -n -v` (final lines `7598/7662/8192`, `7662/7662/8192`).
- Review-Focus probes (runtime where possible): corrupted `s_free_blocks_count=0` → no div-by-zero, `100.0%`, rc 0; `-f` parsing: `010`→8, `255`→255, `0`→0 accepted; `-1`, `0x100`, `""` rejected (rc 1).
- Scale correctness (1 GiB ext4, new binary): python checker over all 1,031,867 free blocks → 0 bad; `fsck.ext2 -fn` clean; second pass `0/1031867/1048576`.

**Benchmarks** (1 GiB dirty image, 1 KiB blocks, 1,031,803 dirty free blocks, quiet-start design: sync → cp → sync → measure):

| metric (ext4) | old | new |
|---|---|---|
| zeroing (5-run range) | 1.56–1.70 s | 0.24–0.30 s |
| `sync -f` (5-run range) | 0.27–1.63 s | 0.01 s |
| end-to-end (median) | ~3.04 s | ~0.28 s (**~11×**) |
| scan-only `-n` | 0.93 s | 0.20 s (**4.7×**) |
| discard + `du` | 2.95 s, 1,032,604 KiB | 1.37 s, 1,048 KiB (**~1000× less disk**) |
| maxrss | ~2.1 MB | ~4.2 MB (2×1 MiB batch buffers) |

| metric (tmpfs) | old | new |
|---|---|---|
| zeroing (3-run range) | 1.36–1.55 s | 0.40–0.49 s (**~3.4×**) |
| `sync -f` | 0.00 s | 0.00 s |

strace probe on tmpfs: exactly one `fallocate(FALLOC_FL_ZERO_RANGE)` → `EOPNOTSUPP` → permanent pwrite fallback (by design); the tmpfs gain is from batched I/O + vectorized compare, not offload.

**Caveats:** offload remanence (ZERO_RANGE is metadata-level; BLKZEROOUT device-dependent — documented in source header); BLKZEROOUT branch compile-verified only (no root/raw device); `posix_fadvise` deliberately omitted (measured net loss in the prior session); 64-bit suite tests are guards (counts fit in 32 bits — true >2³² exposure needs a >128 TiB image); sparsify untouched; no git commits made.

**Suite fix made during Task 4:** the dumpe2fs parsers in `tests/test_suite.sh` were counting the top-level `Free blocks:` *summary* line as a 1-block range (harmless over-check); both helpers now only parse the indented per-group range lines.
