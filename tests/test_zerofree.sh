#!/bin/bash
# Reproduction test for zerofree multi-thread partitioning bug.
# Creates a 1KiB-blocksize ext2 image (s_first_data_block=1), fills free
# space with 0xAA junk, deletes it, runs zerofree -t 4, then verifies that
# every free block is zero.
set -u
cd /tmp/opencode || exit 1

ZEROFREE=/home/tuan/src/zerofree/zerofree
IMG=test.img

rm -f $IMG junk.bin

# 4096 blocks of 1KiB => s_first_data_block = 1, single block group
dd if=/dev/zero of=$IMG bs=1024 count=4096 status=none
mke2fs -q -F -b 1024 -O ^resize_inode $IMG

# junk file filled with 0xAA covering most of the free space
FREE_BLOCKS=$(dumpe2fs -h $IMG 2>/dev/null | awk -F: '/Free blocks/{gsub(/ /,"",$2); print $2}')
JUNK_KB=$((FREE_BLOCKS - 64))   # leave room for indirect blocks/metadata
tr '\0' '\252' < /dev/zero | dd of=junk.bin bs=1024 count=$JUNK_KB status=none

debugfs -w -R "write junk.bin junk" $IMG >/dev/null 2>&1
debugfs -w -R "rm junk" $IMG >/dev/null 2>&1

FIRST_DATA_BLOCK=$(dumpe2fs -h $IMG 2>/dev/null | awk -F: '/First block/{gsub(/ /,"",$2); print $2}')
echo "First data block: $FIRST_DATA_BLOCK, free blocks: $FREE_BLOCKS"

# Run zerofree with 4 threads
$ZEROFREE -t 4 $IMG 2>/dev/null

# Independent verification: every free block must now be all-zero
python3 - "$IMG" <<'EOF'
import subprocess, sys

img = sys.argv[1]
# get free block ranges from dumpe2fs
out = subprocess.run(['dumpe2fs', img], capture_output=True, text=True).stdout
free_ranges = []
for line in out.splitlines():
    line = line.strip()
    if line.startswith('Free blocks:'):
        for tok in line.split(':',1)[1].split(','):
            tok = tok.strip()
            if not tok: continue
            if '-' in tok:
                a,b = tok.split('-'); free_ranges.append((int(a), int(b)))
            else:
                free_ranges.append((int(tok), int(tok)))

bs = 1024
bad = []
with open(img,'rb') as f:
    for a,b in free_ranges:
        for blk in range(a, b+1):
            f.seek(blk*bs)
            data = f.read(bs)
            if data != b'\0'*bs:
                bad.append(blk)

if bad:
    print(f"FAIL: {len(bad)} free block(s) NOT zeroed: {bad[:20]}")
    sys.exit(1)
print("PASS: all free blocks are zero")
EOF
