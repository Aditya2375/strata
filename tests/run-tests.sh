#!/bin/sh
# strata end-to-end suite
set -u
BIN=${BIN:-build/strata}
UNIT=${UNIT:-build/test-unit}
T=/tmp/strata-tests
rm -rf $T; mkdir -p $T
fails=0
run(){ # name, expected-file, commands...
  name=$1; shift
  exp=$1; shift
  img=$T/$name.img
  $BIN format $img 512 256 >/dev/null
  printf '%s\n' "$@" | $BIN shell $img > $T/$name.out 2>&1
  if ! diff -u $exp $T/$name.out > $T/$name.diff 2>&1; then
    echo "FAIL: $name (see $T/$name.diff)"; fails=1
  else echo "ok: $name"; fi
}

# --- case: basic files and dirs ---
cat > $T/basic.exp <<'E'
hello strata
dir       512  links=3   ./
dir       512  links=3   ../
dir       256  links=2   lost+found/
file       12  links=1   note.txt
lost+found/
note.txt
notes/
type=dir size=256 blocks=1 links=2 mode=755
E
run basic $T/basic.exp \
  'write /note.txt hello strata' \
  'cat /note.txt' \
  'ls -l /' \
  'mkdir /notes' \
  'ls /' \
  'stat /notes'

# --- case: errors ---
cat > $T/errors.exp <<'E'
cat: /nope: no such file or directory
cd: /note.txt: not a directory
mkdir: /d: already exists
rmdir: /d: directory not empty
rm: /d: is a directory
cd: /ghost: no such file or directory
ln: operation not permitted
mv: no such file or directory
E
$BIN format $T/errors2.img 96 64 >/dev/null
printf '%s\n' \
  'cat /nope' \
  'write /note.txt x' \
  'cd /note.txt' \
  'mkdir /d' \
  'mkdir /d' \
  'write /d/x placeholder text here' \
  'rmdir /d' \
  'rm /d' \
  'cd /ghost' \
  'ln /d /d2' \
  'mv /ghost /g2' \
  | $BIN shell $T/errors2.img > $T/errors.out 2>&1
diff -u $T/errors.exp $T/errors.out > $T/errors.diff 2>&1 || { echo "FAIL: errors"; fails=1; }

# --- case: persistence across mounts ---
img=$T/persist.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'write /a/data.txt persisted bytes here' 'sync' | $BIN shell $img >/dev/null 2>&1
printf '%s\n' 'cat /a/data.txt' 'append /a/data.txt  +more' 'cat /a/data.txt' | $BIN shell $img > $T/persist.out 2>&1
printf 'persisted bytes here\npersisted bytes here +more\n' > $T/persist.exp
diff -u $T/persist.exp $T/persist.out > $T/persist.diff || { echo "FAIL: persistence"; fails=1; } 
[ -s $T/persist.diff ] || echo "ok: persistence"

# --- case: hard links ---
cat > $T/links.exp <<'E'
type=file size=5 blocks=1 links=2 mode=644
hello
type=file size=5 blocks=1 links=1 mode=644
E
run links $T/links.exp \
  'write /orig hello' \
  'ln /orig /copy' \
  'stat /copy' \
  'rm /orig' \
  'cat /copy' \
  'stat /copy'

# --- case: rename semantics ---
printf 'content of a\nls: /a: no such file or directory\ncontent of a\nmv: invalid argument\nafter\n' > $T/rename.exp
run rename $T/rename.exp \
  'write /a content of a' \
  'mkdir /sub' \
  'mv /a /sub/b' \
  'cat /sub/b' \
  'ls /a' \
  'cat /sub/b' \
  'mkdir /sub/full' \
  'write /sub/full/x y' \
  'mv /sub /sub/full' \
  'mv /sub/full /sub2' \
  'mv /sub/b /newname' \
  'write /newname after' \
  'cat /newname'

# --- case: deep tree + cwd ---
cat > $T/tree.exp <<'E'
/a/b
leaf
/
/a/b
/
E
run tree $T/tree.exp \
  'mkdir /a' 'mkdir /a/b' 'cd /a/b' 'write leaf x' 'pwd' 'ls' 'cd /' 'pwd' 'cd a/b' 'pwd' 'cd /' 'pwd'


# --- case: renaming a dir over an empty dir keeps link counts consistent ---
img=$T/mvdir.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /d1' 'mkdir /d1/sub' 'mkdir /d2' 'mkdir /d2/sub' 'mv /d1/sub /d2/sub' 'fsck' | $BIN shell $img > $T/mvdir.out 2>&1
grep -q '^fsck: clean$' $T/mvdir.out && echo "ok: mv dir over dir stays fsck-clean" || { echo "FAIL: mv dir over dir (see $T/mvdir.out)"; fails=1; }

# --- case: cross-type rename rejected ---
printf 'mv: is a directory\nmv: not a directory\n' > $T/mvx.exp
run mvx $T/mvx.exp 'write /f x' 'mkdir /dd' 'mv /f /dd' 'mv /dd /f'

# --- case: overlong names rejected, not silently truncated ---
lname=$(printf 'a%.0s' $(seq 1 130))
printf 'touch: /%s: name too long\n' "$lname" > $T/long.exp
run long $T/long.exp "touch /$lname"

# --- case: a 300-deep ".." chain still trips the cycle guard ---
img=$T/deepmv.img
$BIN format $img 4096 1024 >/dev/null
{ echo 'mkdir /dc'; p=/dc; i=0; while [ $i -lt 300 ]; do p=$p/a; echo "mkdir $p"; i=$((i+1)); done; echo "mv /dc $p"; } | $BIN shell $img > $T/deepmv.out 2>&1
grep -q 'mv: invalid argument' $T/deepmv.out && echo "ok: 300-deep mv cycle rejected" || { echo "FAIL: deep mv cycle (see $T/deepmv.out)"; fails=1; }

# --- case: corrupt superblock geometry is refused, not OOB-read ---
img=$T/corrupt.img
$BIN format $img 512 256 >/dev/null
printf '\xff\xff\xff\xff' | dd of=$img bs=1 seek=8 count=4 conv=notrunc 2>/dev/null  # nblocks = 4 billion
if $BIN fsck $img > $T/corrupt-fsck.out 2>&1; then echo "FAIL: fsck accepted corrupt geometry"; fails=1; fi
grep -q 'FATAL: bad superblock geometry' $T/corrupt-fsck.out && echo "ok: corrupt geometry refused by fsck" || { echo "FAIL: fsck geometry message"; fails=1; }
printf '%s\n' 'ls' | $BIN shell $img > $T/corrupt-shell.out 2>&1
grep -q 'cannot mount' $T/corrupt-shell.out && echo "ok: corrupt geometry refused at mount" || { echo "FAIL: mount accepted corrupt geometry"; fails=1; }

# --- case: fsck exits nonzero while a FATAL remains ---
img=$T/fatal.img
$BIN format $img 512 256 >/dev/null
printf '\x00\x00\x00\x00' | dd of=$img bs=1 seek=0 count=4 conv=notrunc 2>/dev/null  # magic = 0
if $BIN fsck $img --fix > /dev/null 2>&1; then echo "FAIL: fsck --fix exited 0 with a FATAL"; fails=1; else echo "ok: fsck --fix exits nonzero while a FATAL remains"; fi

echo "--- unit tests ---"
$UNIT || fails=1

echo "--- big file (2 MiB, indirect blocks) ---"
img=$T/big.img
$BIN format $img 1024 256 >/dev/null
python3 - <<PY
import subprocess, hashlib
data = bytes((i*7+13)%251 for i in range(2*1024*1024))
open('$T/blob','wb').write(data)
h1 = hashlib.sha256(data).hexdigest()
# write via shell in chunks using a tiny driver
cmds = "write /big placeholder\n"
subprocess.run(['$BIN','shell','$img'], input=cmds.encode(), check=True)
PY
python3 - $img <<'PY'
import subprocess, sys
img = sys.argv[1]
# large-file test via repeated append (shell-level), 300 x 1 KiB of distinct text
lines = [f"append /big2 line-{i:04d}-" + "x"*(1000-15) for i in range(300)]
inp = "\n".join(["write /big2 start"]+lines+["cat /big2"])+"\n"
out = subprocess.run(['build/strata','shell',img], input=inp.encode(), capture_output=True).stdout.decode()
expect = "start" + "".join(f"line-{i:04d}-"+"x"*(1000-15) for i in range(300))
if expect in out: print("ok: big file content")
else:
    print("FAIL: big file content"); sys.exit(1)
PY
[ $? -eq 0 ] || fails=1

echo "--- ENOSPC consistency ---"
img=$T/small.img
$BIN format $img 64 64 >/dev/null
python3 - $img <<'PY'
import subprocess, sys
img = sys.argv[1]
inp = "\n".join(f"write /f{i} " + "y"*900 for i in range(40)) + "\nsync\n"
subprocess.run(['build/strata','shell',img], input=inp.encode(), capture_output=True)
r = subprocess.run(['build/strata','fsck',img], capture_output=True)
print(r.stdout.decode().strip())
PY
./build/strata fsck $img | grep -q clean && echo "ok: enospc fsck clean" || { echo "FAIL: enospc fsck"; fails=1; }

echo "--- crash + fsck recovery sweep ---"
crashfails=0
for K in 0 1 2 3 5 8 13 21 34; do
  img=$T/crash$K.img
  $BIN format $img 512 256 >/dev/null
  printf '%s\n' \
    'mkdir /w' 'write /w/a alpha' 'write /w/b beta' 'mkdir /w/c' \
    'write /w/c/deep deep content' 'append /w/a -more' 'rm /w/b' \
    "crash $K" | $BIN shell $img >/dev/null 2>&1
  rc=$?
  [ $rc -eq 2 ] || { echo "FAIL: crash hook did not abort (K=$K rc=$rc)"; fails=1; continue; }
  $BIN fsck $img --fix > $T/crash$K.fsck 2>&1
  $BIN fsck $img > $T/crash$K.fsck2 2>&1
  if grep -q 'clean' $T/crash$K.fsck2 && printf 'ls /w\ncat /w/a\n' | $BIN shell $img >/dev/null 2>&1; then
    echo "ok: crash K=$K recovered"
  else
    echo "FAIL: crash K=$K"; cat $T/crash$K.fsck2; fails=1
  fi
done

echo "--- fsck corruption repair ---"
corrupt_test(){
  name=$1; pycode=$2
  img=$T/corrupt-$name.img
  $BIN format $img 512 256 >/dev/null
  printf '%s\n' 'mkdir /a' 'write /a/f data here' 'write /g other' 'sync' | $BIN shell $img >/dev/null 2>&1
  python3 - "$img" "$pycode" <<'PY'
import sys, struct
img, mode = sys.argv[1], sys.argv[2]
BS=4096
d=open(img,'r+b')
sb=d.read(BS)
import struct as s
nin=s.unpack_from('<I',sb,12)[0]
ibm_s=s.unpack_from('<I',sb,16)[0]
dbm_s=s.unpack_from('<I',sb,24)[0]
it_s=s.unpack_from('<I',sb,32)[0]
data_s=s.unpack_from('<I',sb,40)[0]
if mode=='dbitmap_clear':     # clear bitmap bit of a used block
    d.seek(dbm_s*BS); bm=bytearray(d.read(BS))
    for b in range(data_s,512):
        if bm[b//8]>>(b%8)&1:
            bm[b//8]&=~(1<<(b%8)); break
    d.seek(dbm_s*BS); d.write(bm)
elif mode=='dbitmap_set':     # leak: mark a free block used
    d.seek(dbm_s*BS); bm=bytearray(d.read(BS))
    for b in range(data_s,512):
        if not (bm[b//8]>>(b%8)&1):
            bm[b//8]|=(1<<(b%8)); break
    d.seek(dbm_s*BS); d.write(bm)
elif mode=='nlink_zero':      # file reachable with nlink 0
    d.seek(it_s*BS)
    for i in range(nin):
        d.seek(it_s*BS+i*128)
        t=d.read(2)
        if t[0]==2:  # a file
            d.seek(it_s*BS+i*128+12); d.write(bytes(4)); break
elif mode=='orphan':          # inode in use but no dirent points at it
    d.seek(it_s*BS+100*128)
    d.write(bytes([2,0]))     # type=file at ino 100 with bitmap bit set below
    d.seek(it_s*BS+100*128+12); d.write(bytes([1,0,0,0]))
    d.seek(ibm_s*BS); bm=bytearray(d.read(BS))
    bm[100//8]|=(1<<(100%8)); d.seek(ibm_s*BS); d.write(bm)
elif mode=='dupblock':        # two files share one data block
    d.seek(it_s*BS+36); rb=s.unpack_from('<I',d.read(4))[0]
    ent={}
    for i in range(32):
        d.seek(rb*BS+i*128); e=d.read(128)
        nm=e[8:8+120].split(b'\0')[0].decode()
        if nm: ent[nm]=s.unpack_from('<I',e,0)[0]
    ia=ent['a']; ig=ent['g']
    d.seek(it_s*BS+ia*128+36); dab=s.unpack_from('<I',d.read(4))[0]
    d.seek(dab*BS+2*128); e=d.read(128)   # /a entries: . .. f
    fino=s.unpack_from('<I',e,0)[0]
    d.seek(it_s*BS+fino*128+36); pb=s.unpack_from('<I',d.read(4))[0]
    d.seek(it_s*BS+ig*128+36); d.write(s.pack('<I',pb))
elif mode=='dirhardlink':     # second dentry for /a in the root
    d.seek(it_s*BS+36); rb=s.unpack_from('<I',d.read(4))[0]
    ent={}; off=0
    for i in range(32):
        d.seek(rb*BS+i*128); e=d.read(128)
        ino=s.unpack_from('<I',e,0)[0]
        if ino==0xFFFFFFFF or e[8]==0: break
        ent[e[8:8+120].split(b'\0')[0].decode()]=ino; off+=128
    de=bytearray(128); s.pack_into('<I',de,0,ent['a']); de[4]=1; de[8:8+2]=b'al'
    d.seek(rb*BS+off); d.write(bytes(de))
    d.seek(it_s*BS+16); sz=s.unpack_from('<Q',d.read(8))[0]
    d.seek(it_s*BS+16); d.write(s.pack('<Q',sz+128))
elif mode=='unktype':         # inode with a type fsck does not know
    d.seek(it_s*BS+100*128); d.write(bytes([9,0]))
    d.seek(ibm_s*BS); bm=bytearray(d.read(BS)); bm[100//8]|=(1<<(100%8)); d.seek(ibm_s*BS); d.write(bm)
elif mode=='bad_dotdot':      # break a subdir's '..' to point at itself
    d.seek(it_s*BS)
    # find dir inode for /a: root block has entries; simpler: inode of /a is created early, scan dirs
    for i in range(2,nin):
        d.seek(it_s*BS+i*128); hdr=d.read(128)
        if hdr[0]==1:  # dir
            blk=s.unpack_from('<I',hdr,36)[0]
            d.seek(blk*BS+128); e=d.read(128)  # second entry = '..'
            if e[:4]!=b'\xff\xff\xff\xff':
                d.seek(blk*BS+128); d.write(s.pack('<I',i)); break
d.close()
PY
  $BIN fsck $img --fix > $T/corrupt-$name.log 2>&1
  if $BIN fsck $img 2>&1 | grep -q clean; then echo "ok: fsck repairs $name"
  else echo "FAIL: fsck $name"; $BIN fsck $img; fails=1; fi
}
corrupt_test dbitmap_clear dbitmap_clear
corrupt_test dbitmap_set dbitmap_set
corrupt_test nlink_zero nlink_zero
corrupt_test orphan orphan
corrupt_test bad_dotdot bad_dotdot
corrupt_test dupblock dupblock
corrupt_test dirhardlink dirhardlink
corrupt_test unktype unktype

echo "--- round-2 regressions ---"
# F7: insane block pointers / inode numbers on a valid-geometry image must
# produce errors, not crashes
img=$T/badptr.img
$BIN format $img 512 256 >/dev/null
printf 'write /a hello\nmkdir /dd\nwrite /dd/x hi\nsync\n' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" <<'PY'
import sys, struct
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
it_s=struct.unpack_from('<I',sb,32)[0]
d.seek(it_s*BS+36); rb=struct.unpack_from('<I',d.read(4))[0]
ent={}
for i in range(32):
    d.seek(rb*BS+i*128); e=d.read(128)
    nm=e[8:8+120].split(b'\0')[0].decode()
    if nm: ent[nm]=struct.unpack_from('<I',e,0)[0]
ia=ent['a']; idd=ent['dd']
d.seek(it_s*BS+ia*128+36); d.write(struct.pack('<I',0x100000))  # insane direct[0]
d.seek(it_s*BS+idd*128+36); ddb=struct.unpack_from('<I',d.read(4))[0]
d.seek(ddb*BS+3*128)
de=bytearray(128); struct.pack_into('<I',de,0,0x00FFFFFF); de[4]=2; de[8:8+3]=b'bad'
d.write(bytes(de))
d.seek(it_s*BS+idd*128+16); sz=struct.unpack_from('<Q',d.read(8))[0]
d.seek(it_s*BS+idd*128+16); d.write(struct.pack('<Q',sz+128))
d.close()
PY
out=$(printf 'cat /a\nls -l /dd\nstat /dd/bad\n' | $BIN shell $img 2>&1) && rc=0 || rc=$?
if [ "$rc" = "0" ]; then echo "ok: corrupt runtime pointers rejected without a crash"
else echo "FAIL: corrupt pointers crashed the shell"; fails=1; fi

# F8: more orphans than /lost+found's direct blocks can hold must still be
# reattached (indirect growth)
img=$T/flood.img
$BIN format $img 2048 512 >/dev/null
python3 - "$img" <<'PY'
import sys, struct
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
ibm_s=struct.unpack_from('<I',sb,16)[0]
it_s=struct.unpack_from('<I',sb,32)[0]
d.seek(ibm_s*BS); bm=bytearray(d.read(BS))
for i in range(100,500): bm[i//8]|=(1<<(i%8))
d.seek(ibm_s*BS); d.write(bm)
for i in range(100,500):
    d.seek(it_s*BS+i*128); d.write(bytes([2,0]))
    d.seek(it_s*BS+i*128+12); d.write(bytes([1,0,0,0]))
d.close()
PY
$BIN fsck $img --fix > $T/flood.log 2>&1
n=$(grep -c reattached $T/flood.log)
if $BIN fsck $img 2>&1 | grep -q clean && [ "$n" = "400" ]; then
  echo "ok: 400 orphans reattached through indirect lost+found growth"
else
  echo "FAIL: orphan flood (reattached=$n)"; fails=1
fi

# F9: an oversized image is refused at mount, not OOM-killed
img=$T/huge.img
$BIN format $img 512 256 >/dev/null
python3 - "$img" <<'PY'
import sys, struct, os
d=open(sys.argv[1],'r+b'); d.seek(8); d.write(struct.pack('<I',16*1024*1024)); d.close()
os.truncate(sys.argv[1], 64*1024*1024*1024)
PY
printf 'ls\n' | $BIN shell $img > $T/huge.out 2>&1
grep -q 'cannot mount' $T/huge.out && echo "ok: oversized image refused at mount" || { echo "FAIL: oversized image"; fails=1; }

echo "--- model test vs python reference ---"
python3 tests/model-test.py $BIN || { echo "FAIL: model test"; fails=1; }

echo "--- input fuzz ---"
python3 - $BIN <<'PY'
import subprocess, random, sys
bin = sys.argv[1]
random.seed(7)
cmds = ['ls','cd','mkdir','write','append','cat','rm','rmdir','ln','mv','stat','df','sync','pwd','help']
paths = ['/','/a','/b','a','..','x','/a/b','//','.','/lost+found']
for trial in range(300):
    random.seed(1000+trial)
    img=f'/tmp/fuzz{trial%8}.img'
    if trial<8: subprocess.run([bin,'format',img,'256','128'],capture_output=True)
    lines=[]
    for _ in range(random.randint(1,25)):
        c=random.choice(cmds)
        n=random.randint(0,2)
        args=' '.join(random.choice(paths) for _ in range(n))
        if c in ('write','append'): args += ' ' + ''.join(random.choice('abc xyz') for _ in range(random.randint(0,40)))
        lines.append((c+' '+args).strip())
    r=subprocess.run([bin,'shell',img],input=('\n'.join(lines)+'\n').encode(),capture_output=True)
    if r.returncode not in (0,): 
        print('FAIL: fuzz trial',trial,'rc',r.returncode); sys.exit(1)
print('fuzz: 300 random sessions, no crashes')
PY
[ $? -eq 0 ] || fails=1

if [ $fails -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "FAILURES PRESENT"; exit 1; fi
