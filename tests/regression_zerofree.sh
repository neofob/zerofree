#!/bin/bash
# Regression matrix: block sizes 1024/4096, thread counts 1(default),1,2,3,4,7,16
set -u
cd /tmp/opencode || exit 1
ZEROFREE=/home/tuan/src/zerofree/zerofree
FAILED=0

check_img() {  # $1=img $2=blocksize
python3 - "$1" "$2" <<'EOF'
import subprocess, sys
img, bs = sys.argv[1], int(sys.argv[2])
out = subprocess.run(['dumpe2fs', img], capture_output=True, text=True).stdout
free_ranges = []
for line in out.splitlines():
    line = line.strip()
    if line.startswith('Free blocks:'):
        for tok in line.split(':',1)[1].split(','):
            tok = tok.strip()
            if not tok: continue
            a,b = (tok.split('-') if '-' in tok else (tok,tok))
            free_ranges.append((int(a), int(b)))
bad = []
with open(img,'rb') as f:
    for a,b in free_ranges:
        for blk in range(a, b+1):
            f.seek(blk*bs)
            if f.read(bs) != b'\0'*bs:
                bad.append(blk)
sys.exit(1 if bad else 0)
EOF
}

make_dirty_img() {  # $1=img $2=blocksize $3=size_kb
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1024 count="$3" status=none
    mke2fs -q -F -b "$2" -O ^resize_inode "$1"
    FREE=$(dumpe2fs -h "$1" 2>/dev/null | awk -F: '/Free blocks/{gsub(/ /,"",$2); print $2}')
    JUNK_KB=$(( (FREE * $2 / 1024) - 256 ))
    tr '\0' '\252' < /dev/zero | dd of=junk.bin bs=1024 count=$JUNK_KB status=none
    debugfs -w -R "write junk.bin junk" "$1" >/dev/null 2>&1
    debugfs -w -R "rm junk" "$1" >/dev/null 2>&1
}

for BS in 1024 4096; do
    for T in default 1 2 3 4 7 16; do
        make_dirty_img r.img $BS 8192
        if [ "$T" = default ]; then
            $ZEROFREE r.img 2>/dev/null
        else
            $ZEROFREE -t $T r.img 2>/dev/null
        fi
        if check_img r.img $BS; then
            echo "PASS bs=$BS threads=$T"
        else
            echo "FAIL bs=$BS threads=$T"
            FAILED=1
        fi
    done
done
# dry-run must not modify anything
make_dirty_img r.img 1024 8192
before=$(md5sum < r.img)
$ZEROFREE -n -t 4 r.img 2>/dev/null
after=$(md5sum < r.img)
[ "$before" = "$after" ] && echo "PASS dry-run unchanged" || { echo "FAIL dry-run modified image"; FAILED=1; }
# fsck sanity
fsck.ext2 -fn r.img >/dev/null 2>&1 && echo "PASS fsck clean" || { echo "FAIL fsck"; FAILED=1; }
exit $FAILED
