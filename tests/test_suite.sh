#!/bin/bash
# Black-box test suite for zerofree.
# Usage: test_suite.sh /path/to/zerofree
# Self-contained: generates its own filesystem images under $SCRATCH (tmpfs).
set -u
BIN=${1:?usage: test_suite.sh /path/to/zerofree}
# resolve to an absolute path: the suite cd's into $SCRATCH below
case $BIN in
	/*) ;;
	*) BIN=$(cd "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN") ;;
esac
SCRATCH=${SCRATCH:-/tmp/opencode}
mkdir -p "$SCRATCH" || exit 1
cd "$SCRATCH" || exit 1
PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

### --- helpers -----------------------------------------------------------

# make_clean_img <path> <blocksize> <size_kb> [extra mke2fs args...]
# Clean image: free blocks read back as zeros.
make_clean_img() {
    local path=$1 bs=$2 size_kb=$3; shift 3
    rm -f "$path"
    dd if=/dev/zero of="$path" bs=1024 count="$size_kb" status=none
    mke2fs -q -F -b "$bs" -O ^resize_inode "$@" "$path" >/dev/null 2>&1
}

# make_dirty_img <path> <blocksize> <size_kb> [extra mke2fs args...]
# Dirty image: raw file pre-filled with 0xAA, mke2fs without discard, so
# every free data block contains 0xAA (junk) and must be zeroed.
make_dirty_img() {
    local path=$1 bs=$2 size_kb=$3; shift 3
    rm -f "$path"
    tr '\0' '\252' < /dev/zero | \
        dd of="$path" bs=1M count=$((size_kb / 1024)) iflag=fullblock status=none
    mke2fs -q -F -b "$bs" -O ^resize_inode -E nodiscard "$@" "$path" >/dev/null 2>&1
}

# check_free_blocks <img> <blocksize> <expected_byte_hex>
check_free_blocks() {
python3 - "$1" "$2" "$3" <<'EOF'
import subprocess, sys
img, bs, byte = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
out = subprocess.run(['dumpe2fs', img], capture_output=True, text=True).stdout
ranges = []
for line in out.splitlines():
    # per-group range lines are indented; the top-level "Free blocks:"
    # summary line holds a count, not ranges -- skip it
    if line[:1] != ' ' or not line.strip().startswith('Free blocks:'):
        continue
    for tok in line.strip().split(':', 1)[1].split(','):
            tok = tok.strip()
            if not tok:
                continue
            a, b = (tok.split('-') if '-' in tok else (tok, tok))
            ranges.append((int(a), int(b)))
want = bytes([byte]) * bs
bad = []
with open(img, 'rb') as f:
    for a, b in ranges:
        for blk in range(a, b + 1):
            f.seek(blk * bs)
            if f.read(bs) != want:
                bad.append(blk)
if bad:
    print(f"  bad blocks ({len(bad)}): {bad[:10]}", file=sys.stderr)
    sys.exit(1)
EOF
}

# first_free_block_with_range_len_ge <img> <minlen>
# Prints "a b" (two block numbers inside a free range of length >= minlen).
pick_two_free_blocks() {
python3 - "$1" "$2" <<'EOF'
import subprocess, sys
img, minlen = sys.argv[1], int(sys.argv[2])
out = subprocess.run(['dumpe2fs', img], capture_output=True, text=True).stdout
for line in out.splitlines():
    # per-group range lines are indented; the top-level "Free blocks:"
    # summary line holds a count, not ranges -- skip it
    if line[:1] != ' ' or not line.strip().startswith('Free blocks:'):
        continue
    for tok in line.strip().split(':', 1)[1].split(','):
            tok = tok.strip()
            if not tok:
                continue
            a, b = (tok.split('-') if '-' in tok else (tok, tok))
            a, b = int(a), int(b)
            if b - a + 1 >= minlen:
                print(a + 1, a + 3)
                sys.exit(0)
sys.exit(1)
EOF
}

# modified_count <bin args...>  -> "modified" field of the -v summary line
modified_count() {
    "$BIN" -v "$@" 2>/dev/null | tr '\r' '\n' | tail -1 | cut -d/ -f1
}

# keep_file_md5 <img> -> md5 of the "keep" file stored inside <img>
keep_file_md5() {
    debugfs -R "dump keep $SCRATCH/keep.out" "$1" >/dev/null 2>&1
    md5sum "$SCRATCH/keep.out" | cut -d' ' -f1
}

du_kb() { du -k "$1" | cut -f1; }

### --- tests -------------------------------------------------------------

### 1-3: 1 KiB blocksize
make_dirty_img t.img 1024 8192
"$BIN" t.img 2>/dev/null || bad "run failed (bs=1024)"
check_free_blocks t.img 1024 00 && ok "bs=1024 free blocks zeroed" || bad "bs=1024 free blocks zeroed"
fsck.ext2 -fn t.img >/dev/null 2>&1 && ok "bs=1024 fsck clean" || bad "bs=1024 fsck clean"
head -c 65536 /dev/urandom > keep.bin
make_dirty_img t.img 1024 8192
debugfs -w -R "write keep.bin keep" t.img >/dev/null 2>&1
M_BEFORE=$(keep_file_md5 t.img)
"$BIN" t.img 2>/dev/null
M_AFTER=$(keep_file_md5 t.img)
[ "$M_BEFORE" = "$M_AFTER" ] && ok "bs=1024 used data intact" || bad "bs=1024 used data intact"

### 4-6: 4 KiB blocksize
make_dirty_img t.img 4096 8192
"$BIN" t.img 2>/dev/null || bad "run failed (bs=4096)"
check_free_blocks t.img 4096 00 && ok "bs=4096 free blocks zeroed" || bad "bs=4096 free blocks zeroed"
fsck.ext2 -fn t.img >/dev/null 2>&1 && ok "bs=4096 fsck clean" || bad "bs=4096 fsck clean"
make_dirty_img t.img 4096 8192
debugfs -w -R "write keep.bin keep" t.img >/dev/null 2>&1
M_BEFORE=$(keep_file_md5 t.img)
"$BIN" t.img 2>/dev/null
M_AFTER=$(keep_file_md5 t.img)
[ "$M_BEFORE" = "$M_AFTER" ] && ok "bs=4096 used data intact" || bad "bs=4096 used data intact"

### 7-8: dry-run
make_dirty_img t.img 1024 8192
MD5_BEFORE=$(md5sum t.img | cut -d' ' -f1)
"$BIN" -n t.img 2>/dev/null
MD5_AFTER=$(md5sum t.img | cut -d' ' -f1)
[ "$MD5_BEFORE" = "$MD5_AFTER" ] && ok "dry-run unchanged" || bad "dry-run unchanged"
M=$(modified_count -n t.img)
[ -n "$M" ] && [ "$M" -gt 0 ] 2>/dev/null && ok "dry-run reports modified>0 ($M)" || bad "dry-run reports modified>0 (got '$M')"

### 9: write-avoidance (second pass)
make_dirty_img t.img 1024 8192
"$BIN" t.img 2>/dev/null
M=$(modified_count t.img)
[ "$M" = "0" ] && ok "second pass modifies 0" || bad "second pass modifies 0 (got '$M')"

### 10-11: fill value
make_dirty_img t.img 1024 8192
"$BIN" -f 0xAA t.img 2>/dev/null
check_free_blocks t.img 1024 aa && ok "fillval 0xAA applied" || bad "fillval 0xAA applied"
M=$(modified_count -f 0xAA t.img)
[ "$M" = "0" ] && ok "fillval second pass modifies 0" || bad "fillval second pass modifies 0 (got '$M')"

### 12-15: discard
make_dirty_img t.img 1024 8192
"$BIN" -d t.img 2>/dev/null
check_free_blocks t.img 1024 00 && ok "discard: free blocks read zero" || bad "discard: free blocks read zero"
DU_BEFORE=$(du_kb t.img)
make_dirty_img t.img 1024 8192
DU_RAW=$(du_kb t.img)
"$BIN" -d t.img 2>/dev/null
DU_AFTER=$(du_kb t.img)
[ "$DU_AFTER" -lt "$DU_RAW" ] 2>/dev/null && ok "discard: disk usage shrank ($DU_RAW -> $DU_AFTER KiB)" || bad "discard: disk usage shrank ($DU_RAW -> $DU_AFTER KiB)"
fsck.ext2 -fn t.img >/dev/null 2>&1 && ok "discard: fsck clean" || bad "discard: fsck clean"
make_dirty_img t.img 1024 8192
MD5_BEFORE=$(md5sum t.img | cut -d' ' -f1)
"$BIN" -n -d t.img 2>/dev/null
MD5_AFTER=$(md5sum t.img | cut -d' ' -f1)
[ "$MD5_BEFORE" = "$MD5_AFTER" ] && ok "dry-run+discard unchanged" || bad "dry-run+discard unchanged"

### 16-18: CLI validation
make_dirty_img t.img 1024 8192
out=$("$BIN" -t 4 t.img 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "usage:"; then
    ok "-t rejected with usage"
else
    bad "-t rejected with usage (rc=$rc)"
fi
for F in 256 abc; do
    if "$BIN" -f "$F" t.img >/dev/null 2>&1; then
        bad "invalid -f rejected ($F)"
    else
        ok "invalid -f rejected ($F)"
    fi
done
out=$("$BIN" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "usage:"; then
    ok "no args usage"
else
    bad "no args usage (rc=$rc)"
fi

### 19: verbose summary format
make_dirty_img t.img 1024 8192
line=$("$BIN" -v t.img 2>/dev/null | tr '\r' '\n' | tail -1)
echo "$line" | grep -Eq '^[0-9]+/[0-9]+/[0-9]+$' && ok "verbose summary format ($line)" || bad "verbose summary format (got '$line')"

### 20: single-byte-dirty blocks (first byte / last byte) are detected
make_clean_img t.img 1024 8192
AB=$(pick_two_free_blocks t.img 5) || bad "single-byte-dirty detection (no free range)"
B1=${AB% *}; B2=${AB#* }
printf '\377' | dd of=t.img bs=1 seek=$((B1 * 1024))        conv=notrunc status=none
printf '\377' | dd of=t.img bs=1 seek=$((B2 * 1024 + 1023)) conv=notrunc status=none
M=$(modified_count t.img)
[ "$M" = "2" ] && ok "single-byte-dirty detection (blocks $B1,$B2)" || bad "single-byte-dirty detection (expected 2, got '$M')"
"$BIN" t.img 2>/dev/null
check_free_blocks t.img 1024 00 && ok "single-byte-dirty blocks zeroed" || bad "single-byte-dirty blocks zeroed"

### 21-22: 64-bit filesystem
make_dirty_img t64.img 1024 16384 -t ext4 -O 64bit
"$BIN" t64.img 2>/dev/null || bad "run failed (64bit)"
check_free_blocks t64.img 1024 00 && ok "64-bit fs zeroed" || bad "64-bit fs zeroed"
fsck.ext2 -fn t64.img >/dev/null 2>&1 && ok "64-bit fs fsck clean" || bad "64-bit fs fsck clean"

### 23-24: ext4 (offload-capable host fs) vs tmpfs (fallback) equivalence
EXT4DIR=/home/tuan/zerofree-bench
if mkdir -p "$EXT4DIR" 2>/dev/null && [ -w "$EXT4DIR" ]; then
    make_dirty_img t.img 1024 8192
    cp t.img "$EXT4DIR/t4.img" && sync
    "$BIN" t.img 2>/dev/null
    "$BIN" "$EXT4DIR/t4.img" 2>/dev/null
    cmp -s t.img "$EXT4DIR/t4.img" && ok "ext4 vs tmpfs runs byte-identical" || bad "ext4 vs tmpfs runs byte-identical"
    fsck.ext2 -fn "$EXT4DIR/t4.img" >/dev/null 2>&1 && ok "ext4 run fsck clean" || bad "ext4 run fsck clean"
    rm -f "$EXT4DIR/t4.img"
else
    bad "ext4 test dir unavailable"
    bad "ext4 run fsck clean"
fi

### 25: success exit code
make_dirty_img t.img 1024 8192
"$BIN" t.img 2>/dev/null && ok "success exit code 0" || bad "success exit code 0"

### --- summary -----------------------------------------------------------
rm -f t.img t64.img keep.bin keep.out
echo "----------------------------------------"
echo "RESULT: $PASS passed, $FAIL failed"
exit $((FAIL > 0))
