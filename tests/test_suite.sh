#!/bin/bash
# Black-box test suite for zerofree. Usage: test_suite.sh /path/to/zerofree
set -u
BIN=${1:?usage: test_suite.sh /path/to/zerofree}
cd /tmp/opencode || exit 1
PASS=0; FAIL=0

ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# check_free_blocks <img> <blocksize> <expected_byte_hex>
check_free_blocks() {
python3 - "$1" "$2" "$3" <<'EOF'
import subprocess, sys
img, bs, byte = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
out = subprocess.run(['dumpe2fs', img], capture_output=True, text=True).stdout
ranges = []
for line in out.splitlines():
    line = line.strip()
    if line.startswith('Free blocks:'):
        for tok in line.split(':',1)[1].split(','):
            tok = tok.strip()
            if not tok: continue
            a,b = (tok.split('-') if '-' in tok else (tok,tok))
            ranges.append((int(a), int(b)))
want = bytes([byte])*bs
bad = []
with open(img,'rb') as f:
    for a,b in ranges:
        for blk in range(a, b+1):
            f.seek(blk*bs)
            if f.read(bs) != want:
                bad.append(blk)
if bad:
    print(f"  bad blocks ({len(bad)}): {bad[:10]}", file=sys.stderr)
    sys.exit(1)
EOF
}

# make_dirty_img <img> <blocksize> <size_kb>  -- junk-filled free space + /keep file
make_dirty_img() {
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1024 count="$3" status=none
    mke2fs -q -F -b "$2" -O ^resize_inode "$1"
    head -c 65536 /dev/urandom > keep.bin
    debugfs -w -R "write keep.bin keep" "$1" >/dev/null 2>&1
    FREE=$(dumpe2fs -h "$1" 2>/dev/null | awk -F: '/Free blocks/{gsub(/ /,"",$2); print $2}')
    JUNK_KB=$(( (FREE * $2 / 1024) - 256 ))
    tr '\0' '\252' < /dev/zero | dd of=junk.bin bs=1024 count=$JUNK_KB status=none
    debugfs -w -R "write junk.bin junk" "$1" >/dev/null 2>&1
    debugfs -w -R "rm junk" "$1" >/dev/null 2>&1
}

# modified_count <args...> -- runs $BIN -n -v <args>, prints "modified" field
modified_count() {
    "$BIN" -n -v "$@" 2>/dev/null | tr '\r' '\n' | tail -1 | cut -d/ -f1
}

### 1. basic zeroing, both block sizes; fsck clean; used data intact
for BS in 1024 4096; do
    make_dirty_img t.img $BS 8192
    "$BIN" t.img 2>/dev/null || { bad "bs=$BS run exit code"; continue; }
    check_free_blocks t.img $BS 00 && ok "bs=$BS free blocks zeroed" || bad "bs=$BS free blocks zeroed"
    fsck.ext2 -fn t.img >/dev/null 2>&1 && ok "bs=$BS fsck clean" || bad "bs=$BS fsck clean"
    rm -f keep.out; debugfs -R "dump /keep keep.out" t.img >/dev/null 2>&1
    cmp -s keep.bin keep.out && ok "bs=$BS used data intact" || bad "bs=$BS used data intact"
done

### 2. dry-run leaves image byte-identical, but reports work to do
make_dirty_img t.img 1024 8192
before=$(md5sum < t.img)
M=$(modified_count t.img)
after=$(md5sum < t.img)
[ "$before" = "$after" ] && ok "dry-run unchanged" || bad "dry-run unchanged"
[ "${M:-0}" -gt 0 ] && ok "dry-run reports modified>0 ($M)" || bad "dry-run reports modified>0 (got '$M')"
"$BIN" -n -d t.img 2>/dev/null
[ "$(md5sum < t.img)" = "$before" ] && ok "dry-run+discard unchanged" || bad "dry-run+discard unchanged"

### 3. write avoidance: second pass reports 0 modified
"$BIN" t.img 2>/dev/null
M=$(modified_count t.img)
[ "${M:-x}" = "0" ] && ok "second pass modifies 0" || bad "second pass modifies 0 (got '$M')"

### 4. fill value
make_dirty_img t.img 1024 8192
"$BIN" -f 0xAA t.img 2>/dev/null
check_free_blocks t.img 1024 AA && ok "fillval 0xAA applied" || bad "fillval 0xAA applied"
M=$(modified_count -f 0xAA t.img)
[ "${M:-x}" = "0" ] && ok "fillval second pass modifies 0" || bad "fillval second pass modifies 0 (got '$M')"

### 5. discard punches holes on image files
make_dirty_img t.img 1024 8192
du_before=$(du -k t.img | cut -f1)
"$BIN" -d t.img 2>/dev/null || bad "discard run exit code"
du_after=$(du -k t.img | cut -f1)
check_free_blocks t.img 1024 00 && ok "discard: free blocks read zero" || bad "discard: free blocks read zero"
[ "$du_after" -lt "$du_before" ] && ok "discard: disk usage shrank ($du_before -> $du_after KiB)" || bad "discard: disk usage shrank ($du_before -> $du_after KiB)"
fsck.ext2 -fn t.img >/dev/null 2>&1 && ok "discard: fsck clean" || bad "discard: fsck clean"

### 6. -t option is rejected (removed feature)
make_dirty_img t.img 1024 8192
out=$("$BIN" -t 4 t.img 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "usage:"; then
    ok "-t rejected with usage"
else
    bad "-t rejected with usage (rc=$rc)"
fi

### 7. verbose final line format modified/free/total
make_dirty_img t.img 1024 8192
line=$("$BIN" -v t.img 2>/dev/null | tr '\r' '\n' | tail -1)
echo "$line" | grep -Eq '^[0-9]+/[0-9]+/[0-9]+$' && ok "verbose summary format ($line)" || bad "verbose summary format (got '$line')"

### 8. blocks dirty in exactly one byte (first byte / last byte) are detected
make_dirty_img t.img 1024 8192
"$BIN" t.img 2>/dev/null   # fully zeroed baseline
read -r B1 B2 <<EOF
$(python3 - t.img <<'PYEOF'
import subprocess, sys
out = subprocess.run(['dumpe2fs', sys.argv[1]], capture_output=True, text=True).stdout
for line in out.splitlines():
    line = line.strip()
    if line.startswith('Free blocks:'):
        toks = [t.strip() for t in line.split(':',1)[1].split(',') if t.strip()]
        for tok in toks:
            if '-' in tok:
                a,b = map(int, tok.split('-'))
                if b - a >= 4:
                    print(a+1, a+3); sys.exit(0)
PYEOF
)
EOF
printf '\377' | dd of=t.img bs=1 seek=$((B1*1024))        conv=notrunc status=none  # first byte
printf '\377' | dd of=t.img bs=1 seek=$((B2*1024 + 1023)) conv=notrunc status=none  # last byte
M=$(modified_count t.img)
[ "${M:-x}" = "2" ] && ok "single-byte-dirty detection (blocks $B1,$B2)" || bad "single-byte-dirty detection (expected 2, got '$M')"
"$BIN" t.img 2>/dev/null
check_free_blocks t.img 1024 00 && ok "single-byte-dirty blocks zeroed" || bad "single-byte-dirty blocks zeroed"

### 9. ext4 (offload) vs tmpfs (fallback) produce identical images
EXT4DIR=/home/tuan/zerofree-bench
if mkdir -p "$EXT4DIR" 2>/dev/null; then
    make_dirty_img t.img 1024 8192
    cp t.img "$EXT4DIR/t4.img" && sync
    "$BIN" t.img 2>/dev/null
    "$BIN" "$EXT4DIR/t4.img" 2>/dev/null
    cmp -s t.img "$EXT4DIR/t4.img" && ok "ext4/tmpfs runs byte-identical" || bad "ext4/tmpfs runs byte-identical"
    fsck.ext2 -fn "$EXT4DIR/t4.img" >/dev/null 2>&1 && ok "ext4 run fsck clean" || bad "ext4 run fsck clean"
    rm -f "$EXT4DIR/t4.img"
else
    bad "ext4 test dir unavailable"
fi

echo "----------------------------------------"
echo "RESULT: $PASS passed, $FAIL failed"
exit $((FAIL > 0))
