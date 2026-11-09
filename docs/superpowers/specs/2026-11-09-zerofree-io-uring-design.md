# zerofree: io_uring Data I/O Engine — Design

Date: 2026-11-09
Supersedes the I/O layer of `docs/superpowers/plans/2026-11-08-zerofree-modern-rewrite.md` (that plan's pinned CLI/behavior contract and standing constraints remain in force).

## Intent and why

zerofree is **I/O-architecture bound**, not CPU bound. Measured on the target
hardware (i7-1265U, WD SN850 NVMe, kernel 7.2, 4 GiB ext4 image, 4K blocks,
~1.03 M free blocks) on 2026-11-09:

| Workload | Time |
|---|---|
| bare `pread` loop, cached | 0.54 s (7.9 GB/s) — the floor for reading 4 GiB |
| zerofree clean pass (read+compare, cached) | 1.16 s |
| zerofree dirty→0 (cold, fallocate offload) | 2.11 s |
| zerofree dirty→0xBB (read+compare+write, cached) | 1.21 s |
| cold single-queue NVMe read (`O_DIRECT` dd) | 3.69 s (1.1 GB/s) vs 7.2 GB/s device sequential |

The compare loop (AVX2, verified optimal in the prior session: OR-reduce
zero-test at 31–34 GiB/s, POPCNT proven 7× worse by disassembly) costs
~0.15 s per 4 GiB. It is not the bottleneck and is **out of scope**.

The bottleneck is the data path: synchronous single-queue `pread`/`pwrite`
through libext2fs's unix io manager. On real NVMe, queue depth 1 leaves
~85% of the device's sequential read bandwidth on the table, and the
read → compare → write pipeline is strictly serialized so a dirty pass pays
the full read-copy *then* the full write-copy.

The user has approved (2026-11-09, explicit choice over the synchronous-only
alternative): **replace the data path with an io_uring engine**, with a
pread/pwrite fallback, keeping libext2fs for metadata and keeping the entire
CLI/behavior contract byte-identical.

## Goals

1. **Cold NVMe**: approach device sequential bandwidth on the read stream
   (target ≥ 3 GB/s on the 4 GiB cold-read class, vs 1.1 GB/s today) via
   read queue depth 4–8 and read/write overlap.
2. **Dirty cached passes** (read+compare+write, read-only clean pass):
   ~1.5–2× via overlapping the copy work (writes of chunk *i* in flight
   while chunk *i+1* is read/compared) and lower per-block overhead.
3. **Portability**: identical behavior when io_uring is unavailable
   (old kernel, seccomp) — silent fallback to the existing synchronous
   `pread`/`pwrite` semantics. `ZEROFREE_NO_URING=1` env var forces the
   sync path for testing.
4. **Same contract**: `tests/test_suite.sh` (25 tests) passes unchanged,
   in **both** engine modes.

## Non-goals (this iteration)

- **No changes to** `zf_block_is_filled` (zerofree.h), its unit tests, or the
  AVX2 build flags.
- **No O_DIRECT.** It saves one page-cache copy (~15% on cold reads) but adds
  three failure modes (alignment, host-fs support, mixed buffered/offload
  coherence story) on top of a code path that already reaches high bandwidth
  via queue depth. Measure after the engine lands; add in a follow-up only if
  a bench shows >10%.
- **No threading** — io_uring provides the overlap on one thread.
- **No liburing dependency** — raw `io_uring_setup` / `io_uring_enter`
  syscalls, ~100 lines of ring plumbing.
- **No O_DIRECT/O_ASYNC on the metadata path** — libext2fs's io cache keeps
  using the fs fd exactly as today.
- **No change to discard mode** — `io_channel_discard` per run stays.
- **sparsify.c and its Makefile rule: out of scope, untouched** (standing
  constraint from the 2026-11-08 plan).

## Anti-goals (things that would make this a failure)

- Any change in observable behavior: CLI, usage string, error messages,
  exit codes, verbose stream format (`\r%4.1f%%` progress at 0.1%
  granularity, final `\r%llu/%llu/%llu\n` on stdout), `modified`/`free`
  counters, sparseness semantics (only dirty sub-runs written), second-pass
  modifies-0 property.
- New hard dependencies (no liburing, no kernel > 5.1 *requirement*).
- Behavior changes on non-x86-64 (the tool already requires x86-64-v3 to
  build; the engine itself must stay arch-neutral C).
- Silent data corruption on I/O error: every engine error must abort with
  the same pinned message as today, close everything, exit 1.

## Constraints (inherited, in force)

- C17 (`-std=gnu17`), GCC 13, `-O3 -march=x86-64-v3 -flto -Wall -Wextra
  -Wpedantic`; e2fsprogs ≥ 1.42 (`EXT2_FLAG_64BITS`), 64-bit fs support.
- Pinned error messages: `error while reading block` /
  `error while writing block` / `error while discarding block` /
  `out of memory (surely not?)`; usage string and getopt table unchanged.
- Offload contract unchanged: enabled iff `fill == 0 && !dryrun &&
  !discard`; side fd `O_WRONLY|O_CLOEXEC`; `S_ISREG` →
  `fallocate(FALLOC_FL_ZERO_RANGE)`, `S_ISBLK` → `BLKZEROOUT`; applied only
  to dirty sub-runs; `{EOPNOTSUPP,ENOSYS,ENOTTY,EINVAL}` → permanent
  fallback to the write path; any other error = failure.
- **No git commits unless the user asks** (standing instruction).
- Bench harness: 4 GiB image at `/home/tuan/zfbench` (4K blocks); report
  clean-pass, dirty→0, dirty→0xBB times before/after.

## Architecture

```
 zerofree
 ├─ metadata : libext2fs, fs fd, io cache           (unchanged)
 ├─ data     : zio engine, its own fd               (new)
 │    ├─ reads  : io_uring, up to 8 in flight, 4 MiB chunks,
 │    │           multi-buffered, consumed strictly in submission order
 │    ├─ writes : io_uring, dirty sub-runs from the pre-filled buffer,
 │    │           submitted while the next chunks are still being read
 │    └─ fallback: pread/pwrite, same loop skeleton (uring absent,
 │                 ZEROFREE_NO_URING=1, or any ring-init failure)
 └─ offload  : fallocate/BLKZEROOUT on the side fd  (unchanged,
               synchronous — metadata-only ops, no data transfer)
```

Key decisions (designer's calls where marked):

1. **Separate data fd.** Open `path` ourselves: `O_RDWR|O_CLOEXEC` for
   real runs, `O_RDONLY|O_CLOEXEC` for dry-run. Discard mode needs no
   data fd at all (it never reads) — the engine is not initialized there.
   The fs fd keeps serving libext2fs metadata. Three fds total, each with
   a single role. Coherency argument (updated from the existing comment):
   our fd touches only free data blocks, forward-only; the offload side
   fd zeroes a range only after it was read; metadata never passes through
   our fd; `ext2fs_close` writes back only dirty metadata.
2. **Chunk size 4 MiB** (was 1 MiB). Doubles bytes per syscall/CQE; on
   this CPU, cached pread throughput is flat between 1 and 4 MiB
   (7.92/7.79 GB/s), and larger IOs are generally friendlier to NVMe.
   Buffer cost: 8 read slots × 4 MiB = 32 MiB + 4 MiB fill buffer ≈ 36 MiB
   RSS. Both buffers 4096-aligned. `chunk_blocks = 4 MiB / blocksize`
   (1024 for 4K, 4096 for 1K blocks).
3. **Engine interface: four verbs, one pass skeleton.** The verbs:
   - `zio_prime(c, blk, count)` — non-blocking read submission (sync:
     no-op);
   - `zio_read(c, blk, count)` — wait until the *next in-order* primed
     chunk has completed and its buffer is ready (sync: blocking pread;
     uring: reap CQEs until this slot completed);
   - `zio_flush_dirty(c, blk, count)` — sync: pwrite; uring: submit,
     returns immediately;
   - `zio_drain(c)` — wait for all in-flight writes (sync: no-op).
   The pass loop is written once and works in both modes, which makes the
   fallback trivially testable and keeps the dirty-sub-run logic (the only
   non-trivial state) in exactly one place.

I/O protocol (pinned, from builder review 2026-11-09):

- `res == 0` on a read: the pinned read error (not a resubmit — the
  bitmap masks EOF-like geometry, so 0 bytes is always anomalous).
  `0 < res < requested`: resubmit the remainder, bounded loop.
- Short writes are handled symmetrically (resubmit remainder / error).
- CQE reaping is a **loop until the expected CQE is seen**: on kernel
  7.2, `io_uring_enter(GETEVENTS)` can return before the CQE is visible.
- Any `io_uring_enter` error other than `EINTR`/`EAGAIN`: pinned failure
  message for the in-flight op class + exit 1.
- Opening our own data fd fails (permissions race after `ext2fs_open`
  succeeded): print the existing pinned `failed to open filesystem`
  message and exit 1 — both engine modes depend on that fd.
4. **Lookahead pipeline (this is what buys the queue depth).** Chunk
   geometry is only known once the bitmap scan has reached the chunk, so
   the pass loop runs the bitmap scan **up to 8 chunks ahead** of the
   consumption point. The structure is an 8-slot circular pipeline:
   - the bitmap word-scan (decision 10) acts as a run/chunk iterator;
   - slot 0 holds the chunk about to be processed; slots 1..7 hold
     upcoming chunks, already primed (uring) with their reads in flight.
     A single prime never crosses a run boundary (reads are
     per-run-contiguous), but the *window* does: when a run is
     exhausted the iterator hands over the next run's first chunk.
   - processing slot 0: `zio_read` → find dirty sub-runs →
     `zio_flush_dirty` per sub-run → rotate the ring, prime the newly
     opened slot 7.
   With three verbs alone the engine would submit a read only when the
   loop advanced to it — depth 1, and the design's headline win would be
   gone. The iterator + pipeline is mandatory, not an optimization.
5. **In-order read consumption.** io_uring CQEs may complete out of order;
   each slot carries its sequence number in `user_data`. The consumer
   processes slots in submission order (an in-order pointer + completion
   marks). This keeps the pass loop identical to the sync path and means
   a write for chunk *i* is never submitted before chunk *i*'s read
   completed — no same-region read-after-write hazards are possible
   anyway (forward-only scan), but the discipline removes the question.
6. **Submission policy.** Submit the initial window (up to 8 reads) before
   the first consume. After consuming chunk *i*: submit the write(s) for
   its dirty sub-runs, then refill a read slot if the lookahead window has
   room. `get_sqe == NULL` → `io_uring_enter` to free space. Short reads
   (res < requested): resubmit the remainder for that slot (bounded loop,
   same as today's io manager retry behavior); `res < 0` → pinned error
   message, drain, close, exit 1. `EINTR`/`EAGAIN` on enter → retry.
7. **Writes share the ring with reads.** Dirty sub-runs are block-aligned
   (offsets/lengths multiples of blocksize ≥ 1 KiB), sourced from
   `fillbuf + subrun_start*bs`. Sub-runs within a chunk are disjoint and
   ordered. At end of pass, `zio_drain` reaps all write CQEs; any error
   aborts with the pinned write message. (No fsync — today's `pwrite`
   path has the same durability semantics; the tool never fsyncs.)
8. **Offload stays synchronous** (fallocate/BLKZEROOUT per dirty sub-run).
   These are metadata-only operations with no data transfer; queuing them
   in the ring (`IORING_OP_FALLOCATE`) buys nothing and complicates the
   error-classification fallback (EOPNOTSUPP→permanent fallback logic)
   that is already correct as-is.
9. **Ring sizing.** 256 entries (SQ and CQ headroom above the 8-read +
   burst-write working set); `features` unused; no registered buffers
   (buffers are per-slot, plain `aligned_alloc`).
10. **Bitmap run-scan by word.** Replace the per-block
   `ext2fs_test_block_bitmap2` calls (a non-inlined library function —
   ~1 B calls on a 1K-block terabyte filesystem) with a scan over
   `c->fs->block_map->map` in 64-bit words: free = bit clear; mask words
   against `[s_first_data_block, blocks_count)`; use `__builtin_ctzll` /
   leading-zero to jump run-to-run. Semantics identical: a block is free
   iff its bit is clear in the block group bitmap. (The struct and its
   `map` member are in the public e2fsprogs header; this is not a
   private-ABI poke, but it is a version assumption — noted in a comment,
   pinned by the ≥1.42 `#error` guard.)
11. **Per-block counters and progress stay.** The `free_blk++`/
    `modified++` loops and the per-block `progress()` calls (fill and
    discard modes alike) are kept exactly as today: the 2026-11-08 plan
    pinned the verbose stream **byte-exact** and verified it with `cmp`,
    and on small images (e.g. the suite's 8 MiB test files, where one
    4 MiB chunk ≈ 50% of the file) per-chunk ticks would print values that
    skip several 0.1% steps. The cost is negligible — an inlined guarded
    float compare per block, ~1–2 ms per million blocks. The engine's
    per-chunk work (compare, flush) is what gets batched; the bookkeeping
    loop does not change.
12. **Files.** Engine in new `zio.c` / `zio.h` (ring plumbing + both
    modes); `zerofree.c` keeps main/offload/pass; `zerofree.h` untouched
    except the comment about chunk size if referenced. Makefile: add
    `zio.o` to the `zerofree` link; `zerofree.o` depends on `zerofree.h`
    and `zio.h`. `tests/unit_filled` rule untouched.
13. **Testing.**
    - `tests/test_suite.sh` runs **twice** in `make test`: default (uring)
      and with `ZEROFREE_NO_URING=1` (sync). Suite is self-contained and
      fast (~seconds), so running it twice is cheap.
    - New unit test: in-order consumption of the uring read pipeline is
      exercised implicitly by the black-box suite (byte-exact free-block
      checks in both modes); no fault injection for CQE errors — the
      error path is pinned by code inspection (same convention as the
      2026-11-08 plan's Review Focus).
    - Bench: 4 GiB image scenarios (clean pass, dirty→0, dirty→0xBB)
      before/after, reported in the final summary.

## What is left to the builder

- Exact ring plumbing details (SQE/CQE layout, mmap sizing, `enter`
  flags) per the Linux `io_uring` syscall ABI — any correct implementation
  of the interface in §3–5 works.
- Where to place `aligned_alloc` failures (pinned `out of memory` message).
- Fine-grained retry bounds for short-read resubmission.
- Whether 256 ring entries is oversized — any size ≥ working-set works.
- Code organization inside `zio.c`.

## Review focus (things the suite cannot exercise)

1. **uring init failure mid-run is impossible by construction** (init
   happens once, before the pass; any failure picks sync mode for the
   whole run) — verify no half-initialized ring state on the fallback
   path.
2. **Drain at end of pass**: a write CQE error that arrives only at drain
   time must still produce the pinned message + exit 1 (the suite can't
   trigger EIO on image files).
3. **`EINTR` handling** on `io_uring_enter` — retry, never abort.
4. **Short-read loop termination** — must not spin on a 0-byte resubmit
   (EOF-like condition at filesystem end is masked by the bitmap, but the
   loop needs a bound).
5. **Fallback equivalence** — `ZEROFREE_NO_URING=1` output byte-identical
   to the pre-change binary on the same image (spot-check the final line
   and a dirty→0xBB run).
