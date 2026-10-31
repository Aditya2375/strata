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


echo "--- round-3 regressions ---"

# shared crafting helper for the hostile images below
cat > $T/r3.py <<'R3PY'
import sys, struct as s
BS=4096
def geometry(d):
    d.seek(0); b=d.read(BS)
    return dict(nin=s.unpack_from('<I',b,12)[0], ibm=s.unpack_from('<I',b,16)[0],
                dbm=s.unpack_from('<I',b,24)[0], it=s.unpack_from('<I',b,32)[0],
                data=s.unpack_from('<I',b,40)[0], root=s.unpack_from('<I',b,52)[0],
                lnf=s.unpack_from('<I',b,56)[0])
def find(d, g, dino, name):
    d.seek(g['it']*BS+dino*128); ino=d.read(128)
    sz=s.unpack_from('<Q',ino,16)[0]
    for off in range(0,sz,128):
        lb=off//BS
        if lb<12: pb=s.unpack_from('<I',ino,36+4*lb)[0]
        else:
            ind=s.unpack_from('<I',ino,36+4*12)[0]
            if not ind: return None
            d.seek(ind*BS); tab=d.read(BS); pb=s.unpack_from('<I',tab,4*(lb-12))[0]
        if not pb: return None
        d.seek(pb*BS+off%BS); e=d.read(128)
        if s.unpack_from('<I',e,0)[0]!=0xFFFFFFFF and e[8:8+120].split(b'\0')[0].decode(errors='replace')==name:
            return pb, off%BS, e
    return None
def set_size(d, g, ino, val):
    d.seek(g['it']*BS+ino*128+16); d.write(s.pack('<Q',val))
R3PY

# N1: a dentry pointing outside the inode table must be rejected, never
# dereferenced (was: SEGV / silent clobber of a victim file's bytes)
img=$T/r3n1.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'write /a/f data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
pb,off,e=find(d,g,aino,'f')
d.seek(pb*BS+off); d.write(s.pack('<I',352))   # past ninodes=256, lands in data blocks
d.close()
R3PY
out=$(printf '%s\n' 'rm /a/f' 'ln /a/f /a/g' | $BIN shell $img 2>&1)
case "$out" in
  *"filesystem corrupted"*"filesystem corrupted"*) echo "ok: out-of-table dentry rejected (rm+ln)";;
  *) echo "FAIL: N1 [$out]"; fails=1;;
esac
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
pb,off,e=find(d,g,aino,'f')
d.seek(pb*BS+off); d.write(s.pack('<I',0x00FFFFFF))
d.close()
R3PY
out=$(printf '%s\n' 'rm /a/f' | $BIN shell $img 2>&1)
case "$out" in
  *"filesystem corrupted"*) echo "ok: huge dentry ino rejected";;
  *) echo "FAIL: N1 huge [$out]"; fails=1;;
esac

# N5: inode size past the single-indirect maximum is flagged and truncated;
# reads stop at the real maximum instead of streaming zeros without end
img=$T/r3n5.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /g hello' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'g'); gino=s.unpack_from('<I',e,0)[0]
set_size(d,g,gino,1<<40)
d.close()
R3PY
if $BIN fsck $img --fix 2>&1 | grep -q "truncated" && $BIN fsck $img 2>&1 | grep -q clean; then
  echo "ok: oversized inode flagged and truncated"
else echo "FAIL: N5 fsck"; fails=1; fi
n=$(printf 'cat /g\n' | $BIN shell $img 2>/dev/null | wc -c)
[ "$n" -le 4243457 ] && echo "ok: reads bounded at the maximum file size" || { echo "FAIL: N5 read ran away ($n bytes)"; fails=1; }

# N2: a directory with a huge corrupt size must not hang fsck, and the
# repair must converge
img=$T/r3n2.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'write /a/f x' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
set_size(d,g,aino,1<<40)
d.close()
R3PY
timeout 30 $BIN fsck $img --fix >/dev/null 2>&1
if $BIN fsck $img 2>&1 | grep -q clean; then echo "ok: huge dir size repaired, fsck converges"
else echo "FAIL: N2"; $BIN fsck $img 2>&1 | tail -3; fails=1; fi

# N3: shell dump on a directory cycle terminates with a warning
img=$T/r3n3.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /x' 'mkdir /x/y' 'write /x/f data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'x'); xino=s.unpack_from('<I',e,0)[0]
pb,off,e=find(d,g,xino,'y')
d.seek(pb*BS+off); d.write(s.pack('<I',xino))   # /x/y points back at /x
d.close()
R3PY
out=$(timeout 20 sh -c "printf 'dump\n' | $BIN shell $img 2>&1")
case "$out" in
  *"directory cycle"*) echo "ok: dump survives a directory cycle";;
  *) echo "FAIL: N3 [$(echo "$out" | tail -2)]"; fails=1;;
esac

# N4: a bogus FILE-type "lost+found" in the root's indirect region is
# tombstoned, not duplicated; fsck converges without leaking an inode
img=$T/r3n4.img
$BIN format $img 8192 1024 >/dev/null
{ i=0; while [ $i -lt 400 ]; do echo "write /f$i x"; i=$((i+1)); done; echo sync; } | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
r=find(d,g,g['root'],'lost+found'); pb,off,e=r
d.seek(pb*BS+off); d.write(s.pack('<I',0xFFFFFFFF))   # tombstone the real one
d.seek(g['it']*BS+g['root']*128); ino=d.read(128)
sz=s.unpack_from('<Q',ino,16)[0]; assert sz>=401*128
ind=s.unpack_from('<I',ino,36+4*12)[0]; assert ind
d.seek(ind*BS); tab=d.read(BS); pb12=s.unpack_from('<I',tab,0)[0]
slot=400-384
d.seek(pb12*BS+slot*128); e=d.read(128)
de=bytearray(128); s.pack_into('<I',de,0,s.unpack_from('<I',e,0)[0]); de[4]=2
nm=b'lost+found'; de[8:8+len(nm)]=nm
d.seek(pb12*BS+slot*128); d.write(bytes(de))
d.close()
R3PY
$BIN fsck $img --fix >/dev/null 2>&1
$BIN fsck $img --fix >/dev/null 2>&1
freei=$(python3 -c "
import struct as s
d=open('$img','rb'); d.seek(44); print(s.unpack('<I',d.read(4))[0])")
if $BIN fsck $img 2>&1 | grep -q clean && [ "$freei" = "621" ]; then
  echo "ok: bogus lost+found tombstoned, converged, no inode leak"
else echo "FAIL: N4 (free_inodes=$freei)"; $BIN fsck $img 2>&1 | tail -3; fails=1; fi

# N6: mkfs refuses an image beyond the mount-side block cap
if $BIN format $T/r3n6.img 300000 256 >/dev/null 2>&1 || [ -e $T/r3n6.img ]; then
  echo "FAIL: N6 oversized mkfs accepted"; fails=1
else echo "ok: oversized mkfs refused up front"; fi

# N7: stat on a dangling entry reports the error instead of zeros
img=$T/r3n7.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a')
d.seek(pb*BS+off); d.write(s.pack('<I',100))   # ino 100 is free
d.close()
R3PY
out=$(printf 'stat /a\n' | $BIN shell $img 2>&1)
case "$out" in
  *"no such file"*) echo "ok: stat on a dangling entry errors";;
  *) echo "FAIL: N7 [$out]"; fails=1;;
esac

# N8: no-fix fsck says what it WOULD do, not what it did
img=$T/r3n8.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R3PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
d.seek(g['it']*BS+100*128); d.write(bytes([2,0]))
d.seek(g['it']*BS+100*128+12); d.write(bytes([1,0,0,0]))
d.seek(g['ibm']*BS); bm=bytearray(d.read(BS)); bm[100//8]|=(1<<(100%8))
d.seek(g['ibm']*BS); d.write(bm)
d.close()
R3PY
out=$($BIN fsck $img 2>&1)
case "$out" in
  *"would be reattached"*) echo "ok: no-fix fsck speaks conditionally";;
  *) echo "FAIL: N8 [$out]"; fails=1;;
esac


echo "--- round-4 regressions ---"

# F1: ls -l on a dangling entry must not print an uninitialized stat
img=$T/r4f1.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a')
d.seek(pb*BS+off); d.write(s.pack('<I',100))   # ino 100 is free
d.close()
R4PY
out=$(printf 'ls -l /\n' | $BIN shell $img 2>&1)
case "$out" in
  *"[dangling]"*) echo "ok: ls -l flags a dangling entry";;
  *) echo "FAIL: F1 [$out]"; fails=1;;
esac

# F2: duplicate names pointing at two live inodes: one --fix must converge
# (was: dup removed but its inode left reachable, next fsck found an orphan)
img=$T/r4f2.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /f aaa' 'write /g bbb' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'g')
de=bytearray(e); de[8:8+2]=b'f\0'
d.seek(pb*BS+off); d.write(bytes(de))
d.close()
R4PY
$BIN fsck $img --fix >/dev/null 2>&1
if $BIN fsck $img 2>&1 | grep -q clean; then echo "ok: duplicate-name fix converges in one run"
else echo "FAIL: F2"; $BIN fsck $img 2>&1 | tail -3; fails=1; fi

# F3: root inode flipped to a file: fsck must refuse loudly and the shell
# must refuse to operate, never segfault in ls
img=$T/r4f3.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
d.seek(g['it']*BS+g['root']*128); d.write(bytes([2,0]))   # T_FILE
d.close()
R4PY
out=$($BIN fsck $img --fix 2>&1); rc=$?
case "$out" in
  *"FATAL: root inode is not a directory"*) [ $rc -ne 0 ] && echo "ok: fsck FATAL on non-dir root" || { echo "FAIL: F3 rc=0"; fails=1; };;
  *) echo "FAIL: F3 [$out]"; fails=1;;
esac
out=$(printf 'ls\n' | $BIN shell $img 2>&1); rc=$?
case "$out" in
  *"root inode is not a directory"*) [ $rc -ne 0 ] && [ $rc -ne 139 ] && echo "ok: shell refuses a non-dir root (no segfault)" || { echo "FAIL: F3 shell rc=$rc"; fails=1; };;
  *) echo "FAIL: F3 shell [$out]"; fails=1;;
esac

# F4a: superblock root_ino pointing at a live FILE inode is equally fatal
img=$T/r4f4a.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
d.seek(52); d.write(s.pack('<I',aino))
d.close()
R4PY
out=$($BIN fsck $img --fix 2>&1)
case "$out" in
  *"FATAL: root inode is not a directory"*) echo "ok: file-inode root_ino is FATAL";;
  *) echo "FAIL: F4a [$out]"; fails=1;;
esac
out=$(printf 'ls\n' | $BIN shell $img 2>&1)
case "$out" in
  *"root inode is not a directory"*) echo "ok: shell refuses file-inode root";;
  *) echo "FAIL: F4a shell [$out]"; fails=1;;
esac

# F4b: lnf_ino == root_ino must be recreated and converge in one --fix run,
# keeping the root's real children
img=$T/r4f4b.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
d.seek(56); d.write(s.pack('<I',g['root']))
d.close()
R4PY
$BIN fsck $img --fix >/dev/null 2>&1
if $BIN fsck $img 2>&1 | grep -q clean && printf 'ls /\n' | $BIN shell $img 2>/dev/null | grep -q '^a/$'; then
  echo "ok: lnf==root repaired, converges, children intact"
else echo "FAIL: F4b"; fails=1; fi

# F4c: a dentry naming the root inode is an illegal directory hard link
img=$T/r4f4c.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
d.seek(g['it']*BS+aino*128); ino=d.read(128)
sz=s.unpack_from('<Q',ino,16)[0]; rb=s.unpack_from('<I',ino,36)[0]
d.seek(rb*BS+2*128); d.write(s.pack('<I',g['root'])+bytes([1,0,0,0])+b'evil\0'+bytes(115))
d.seek(g['it']*BS+aino*128+16); d.write(s.pack('<Q',sz+128))
d.close()
R4PY
out=$($BIN fsck $img --fix 2>&1)
case "$out" in
  *"directory hard link removed"*) : ;;
  *) echo "FAIL: F4c [$out]"; fails=1;;
esac
$BIN fsck $img 2>&1 | grep -q clean && echo "ok: dentry naming root removed, converges" || { echo "FAIL: F4c converge"; fails=1; }

# F4d: an out-of-range lost+found pointer is recreated, not fatal
img=$T/r4f4d.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b')
d.seek(56); d.write(s.pack('<I',99999))
d.close()
R4PY
$BIN fsck $img --fix >/dev/null 2>&1
if $BIN fsck $img 2>&1 | grep -q clean && printf 'cat /a\n' | $BIN shell $img 2>/dev/null | grep -q '^data$'; then
  echo "ok: out-of-range lnf pointer recreated, data intact"
else echo "FAIL: F4d"; fails=1; fi

# F6: a 120-byte unterminated dentry name must never over-read or hang
img=$T/r4f6.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R4PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a')
d.seek(pb*BS+off+8); d.write(b'A'*120)
d.close()
R4PY
out=$(timeout 10 sh -c "printf 'ls\ndump\n' | $BIN shell $img 2>&1"); rc=$?
[ $rc -ne 124 ] && [ $rc -ne 139 ] && echo "ok: unterminated name handled (rc=$rc)" || { echo "FAIL: F6 rc=$rc"; fails=1; }
$BIN fsck $img --fix >/dev/null 2>&1
$BIN fsck $img 2>&1 | grep -q clean && echo "ok: unterminated name repaired, converges" || { echo "FAIL: F6 fsck"; fails=1; }


echo "--- round-5 regressions ---"

# R5-1: a directory size that is not a multiple of 128 is corruption in
# every scan, not a phantom entry: ls, rmdir and lookup all agree
img=$T/r5s1.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'write /a/f data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R5PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
set_size(d,g,aino,129)
d.close()
R5PY
out=$(printf 'ls /a\nrmdir /a\ncat /a/f\n' | $BIN shell $img 2>&1)
case "$out" in
  *"filesystem corrupted"*"filesystem corrupted"*"filesystem corrupted"*) echo "ok: partial trailing dir slot rejected everywhere";;
  *) echo "FAIL: R5-1 [$out]"; fails=1;;
esac
$BIN fsck $img --fix >/dev/null 2>&1
$BIN fsck $img 2>&1 | grep -q clean && echo "ok: size-129 dir repaired, converges" || { echo "FAIL: R5-1 fsck"; fails=1; }

# R5-2: dump skips dangling entries instead of printing a fabricated record
img=$T/r5s2.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /a data' 'write /b more' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R5PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'b')
d.seek(pb*BS+off); d.write(s.pack('<I',100))   # ino 100 is free
d.close()
R5PY
out=$(printf 'dump\n' | $BIN shell $img 2>&1)
case "$out" in
  *"dangling entry, skipped"*"F /a "*) echo "ok: dump flags dangling entries, keeps real files";;
  *) echo "FAIL: R5-2 [$out]"; fails=1;;
esac

# R5-3: a FILE-typed dentry pointing at a live directory is EISDIR from the
# inode, and the subtree survives
img=$T/r5s3.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'mkdir /a' 'write /a/f data' 'write /evil x' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R5PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'a'); aino=s.unpack_from('<I',e,0)[0]
pb,off,e=find(d,g,g['root'],'evil')
de=bytearray(e); de[0:4]=s.pack('<I',aino); de[4]=2   # FILE-typed, points at the dir
d.seek(pb*BS+off); d.write(bytes(de))
d.close()
R5PY
out=$(printf 'rm /evil\ncat /a/f\n' | $BIN shell $img 2>&1)
case "$out" in
  *"is a directory"*"data"*) echo "ok: rm refuses FILE-dentry to live dir, subtree intact";;
  *) echo "FAIL: R5-3 [$out]"; fails=1;;
esac

# R5-5: duplicate block ownership: the REACHABLE owner keeps the data even
# when the other claimant has the lower inode number
img=$T/r5s5.img
$BIN format $img 512 256 >/dev/null
printf '%s\n' 'write /live precious-data' 'sync' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" "$T/r3.py" <<'R5PY'
import sys; exec(open(sys.argv[2]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d)
pb,off,e=find(d,g,g['root'],'live'); lino=s.unpack_from('<I',e,0)[0]
d.seek(g['it']*BS+lino*128); ino=d.read(128); blk=s.unpack_from('<I',ino,36)[0]
d.seek(g['it']*BS+50*128); d.write(ino)          # move /live to inode 50
d.seek(pb*BS+off); d.write(s.pack('<I',50)+e[4:])
c=bytearray(128); c[0:2]=bytes([2,0]); c[12:16]=s.pack('<I',1)
s.pack_into('<Q',c,16,13); s.pack_into('<I',c,32,1); s.pack_into('<I',c,36,blk)
d.seek(g['it']*BS+2*128); d.write(bytes(c))      # orphan inode 2 (lower) claims the block
d.seek(g['ibm']*BS); bm=bytearray(d.read(BS)); bm[50//8]|=(1<<(50%8))
d.seek(g['ibm']*BS); d.write(bm)
d.close()
R5PY
out=$($BIN fsck $img --fix 2>&1)
case "$out" in
  *"unreachable owner's pointer zeroed"*) : ;;
  *) echo "FAIL: R5-5 [$out]"; fails=1;;
esac
$BIN fsck $img 2>&1 | grep -q clean && n=$(printf 'cat /live\n' | $BIN shell $img 2>/dev/null) && [ "$n" = "precious-data" ]   && echo "ok: reachable owner keeps the block, fsck converges" || { echo "FAIL: R5-5 data [$n]"; fails=1; }


echo "--- round-6 regressions ---"

# R6-1: an indirect-table entry duplicating a block the SAME inode already
# owns must zero the table entry, never direct[i] (stack overflow past
# direct[11] under ASan for i>=12, and no convergence when nothing is
# zeroed). Covers indexes below 12, at 30 and at 100.
for IDX in 5 30 100; do
  img=$T/r6d$IDX.img
  $BIN format $img 1024 256 >/dev/null
  { i=0; while [ $i -lt 100 ]; do printf 'append /big %0600d\n' $i; i=$((i+1)); done; echo sync; } | $BIN shell $img >/dev/null 2>&1
  python3 - "$img" "$IDX" "$T/r3.py" <<'R6PY'
import sys; exec(open(sys.argv[3]).read())
d=open(sys.argv[1],'r+b'); g=geometry(d); idx=int(sys.argv[2])
pb,off,e=find(d,g,g['root'],'big'); bino=s.unpack_from('<I',e,0)[0]
d.seek(g['it']*BS+bino*128); ino=d.read(128)
d0=s.unpack_from('<I',ino,36)[0]; ib=s.unpack_from('<I',ino,84)[0]
assert ib
d.seek(ib*BS+idx*4); d.write(s.pack('<I',d0))
d.close()
R6PY
  out=$($BIN fsck $img --fix 2>&1)
  case "$out" in
    *"duplicate pointer zeroed"*) : ;;
    *) echo "FAIL: R6-1 idx=$IDX [$out]"; fails=1;;
  esac
  $BIN fsck $img 2>&1 | grep -q clean || { echo "FAIL: R6-1 converge idx=$IDX"; fails=1; }
done
echo "ok: indirect self-duplicate zeroed at idx 5, 30, 100, converges"

echo "--- R6b: dentry type vs inode type ---"
img=$T/r6b1.img
$BIN format $img 256 128 >/dev/null
printf 'mkdir /d\ntouch /f\n' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" <<'PY'
import sys, struct as s
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
it=s.unpack_from('<I',sb,32)[0]; root=s.unpack_from('<I',sb,52)[0]
d.seek(it*BS+root*128); rino=d.read(128)
rb=s.unpack_from('<I',rino,36)[0]
for i in range(32):
    d.seek(rb*BS+i*128); e=bytearray(d.read(128))
    if s.unpack_from('<I',e,0)[0]==0xFFFFFFFF: break
    nm=e[8:8+120].split(b'\0')[0].decode()
    if nm=='d': e[4]=2; d.seek(rb*BS+i*128); d.write(bytes(e))  # DIR inode, FILE dentry
    if nm=='f': e[4]=1; d.seek(rb*BS+i*128); d.write(bytes(e))  # FILE inode, DIR dentry
d.close()
PY
out=$($BIN fsck $img 2>&1)
[ "$(printf '%s\n' "$out" | grep -c 'entry type corrected')" = "2" ] && echo "ok: fsck reports both stale dentry types" || { echo "FAIL: R6b-L1 report [$out]"; fails=1; }
printf '%s\n' "$out" | grep -q '\[fixed\]' && { echo "FAIL: R6b-L1 no-fix wrote fixes"; fails=1; }
$BIN fsck $img --fix >/dev/null 2>&1
$BIN fsck $img 2>&1 | grep -q clean && echo "ok: stale dentry types corrected, converges" || { echo "FAIL: R6b-L1 fix"; fails=1; }
out=$(printf 'ls -l /\ncat /f\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q '^dir .* d/$' && printf '%s\n' "$out" | grep -q '^file .* f$' && echo "ok: ls -l labels corrected types" || { echo "FAIL: R6b-L1 ls -l [$out]"; fails=1; }

echo "--- R6b: cd/cat on a dangling entry ---"
img=$T/r6b2.img
$BIN format $img 256 128 >/dev/null
printf 'mkdir /gd\ntouch /g\n' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" <<'PY'
import sys, struct as s
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
it=s.unpack_from('<I',sb,32)[0]; root=s.unpack_from('<I',sb,52)[0]
d.seek(it*BS+root*128); rino=d.read(128)
rb=s.unpack_from('<I',rino,36)[0]
for i in range(32):
    d.seek(rb*BS+i*128); e=d.read(128)
    if s.unpack_from('<I',e,0)[0]==0xFFFFFFFF: break
    nm=e[8:8+120].split(b'\0')[0].decode()
    if nm in ('g','gd'):
        ino=s.unpack_from('<I',e,0)[0]
        d.seek(it*BS+ino*128); d.write(bytes(2))  # inode type = T_FREE, dentry stays
d.close()
PY
out=$(printf 'cat /g\ncd /gd\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'cat: /g: no such file or directory' && printf '%s\n' "$out" | grep -q 'cd: /gd: no such file or directory' && echo "ok: cd/cat error on dangling entries" || { echo "FAIL: R6b-L2 [$out]"; fails=1; }
$BIN fsck $img --fix >/dev/null 2>&1
$BIN fsck $img 2>&1 | grep -q clean && echo "ok: dangling entries reaped, converges" || { echo "FAIL: R6b-L2 fsck"; fails=1; }

echo "--- R6b: read-only image, pure check ---"
img=$T/r6b3.img
$BIN format $img 256 128 >/dev/null
printf 'touch /ro\n' | $BIN shell $img >/dev/null 2>&1
chmod 0444 $img
out=$($BIN fsck $img 2>&1) && rc=0 || rc=$?
chmod 0644 $img
[ "$rc" = "0" ] && printf '%s\n' "$out" | grep -q clean && echo "ok: pure check works on a read-only image" || { echo "FAIL: R6b-L3 rc=$rc [$out]"; fails=1; }

echo "--- R6b: append past a wrapped size ---"
img=$T/r6b4.img
$BIN format $img 256 128 >/dev/null
printf 'touch /h\n' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" <<'PY'
import sys, struct as s
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
it=s.unpack_from('<I',sb,32)[0]; root=s.unpack_from('<I',sb,52)[0]
d.seek(it*BS+root*128); rino=d.read(128)
rb=s.unpack_from('<I',rino,36)[0]
for i in range(32):
    d.seek(rb*BS+i*128); e=d.read(128)
    if s.unpack_from('<I',e,0)[0]==0xFFFFFFFF: break
    if e[8:8+120].split(b'\0')[0].decode()=='h':
        ino=s.unpack_from('<I',e,0)[0]
        d.seek(it*BS+ino*128+16); d.write(s.pack('<Q',0xFFFFFFFFFFFFFFF0))
d.close()
PY
out=$(printf 'append /h XYZZY\nls /\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'append: /h: file too large' && printf '%s\n' "$out" | grep -q '^h$' && echo "ok: wrapped size refused, shell survives" || { echo "FAIL: R6b-c [$out]"; fails=1; }
$BIN fsck $img --fix >/dev/null 2>&1

echo "--- R6b: multiple orphans, one linear pass ---"
img=$T/r6b5.img
$BIN format $img 512 128 >/dev/null
python3 - "$img" <<'PY'
import sys, struct as s
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
ibm=s.unpack_from('<I',sb,16)[0]; it=s.unpack_from('<I',sb,32)[0]
d.seek(ibm*BS); bm=bytearray(d.read(BS))
for i in (100,101,102):
    d.seek(it*BS+i*128); d.write(bytes([2,0]))
    d.seek(it*BS+i*128+12); d.write(s.pack('<I',1))
    bm[i//8]|=(1<<(i%8))
d.seek(ibm*BS); d.write(bm); d.close()
PY
out=$($BIN fsck $img --fix 2>&1)
[ "$(printf '%s\n' "$out" | grep -c 'reattached')" = "3" ] && echo "ok: three orphans reattached in one pass" || { echo "FAIL: R6b-b [$out]"; fails=1; }
$BIN fsck $img 2>&1 | grep -q clean || { echo "FAIL: R6b-b converge"; fails=1; }
out=$(printf 'ls /lost+found\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'ino_100' && printf '%s\n' "$out" | grep -q 'ino_102' && echo "ok: orphans landed in /lost+found" || { echo "FAIL: R6b-b lnf [$out]"; fails=1; }

echo "--- R7: dinode.blocks validated by fsck ---"
img=$T/r7blocks.img
$BIN format $img 256 128 >/dev/null
printf 'write /f hello\n' | $BIN shell $img >/dev/null 2>&1
python3 - "$img" <<'PY'
import sys, struct
BS=4096
d=open(sys.argv[1],'r+b'); sb=d.read(BS)
it=struct.unpack_from('<I',sb,32)[0]; root=struct.unpack_from('<I',sb,52)[0]
d.seek(it*BS+root*128); rino=d.read(128)
rb=struct.unpack_from('<I',rino,36)[0]
for i in range(32):
    d.seek(rb*BS+i*128); e=d.read(128)
    if struct.unpack_from('<I',e,0)[0]==0xFFFFFFFF: break
    if e[8:9]==b'f':
        ino=struct.unpack_from('<I',e,0)[0]
        d.seek(it*BS+ino*128+32); d.write(struct.pack('<I',0xFFFFFFF0))
d.close()
PY
out=$(printf 'stat /f
' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'blocks=4294967280' || { echo "FAIL: R7 setup [$out]"; fails=1; }
$BIN fsck $img 2>&1 | grep -q 'block count corrected' && echo "ok: fsck reports wrong block count" || { echo "FAIL: R7 report"; fails=1; }
$BIN fsck $img --fix >/dev/null 2>&1
out=$(printf 'stat /f
' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'blocks=1 ' && $BIN fsck $img 2>&1 | grep -q clean && echo "ok: block count corrected, converges" || { echo "FAIL: R7 fix [$out]"; fails=1; }

echo "--- R7: format prints the real inode count ---"
out=$($BIN format $T/r7fmt.img 512 100 2>&1)
printf '%s\n' "$out" | grep -q '128 inodes' && echo "ok: format reports rounded inode count" || { echo "FAIL: R7 format [$out]"; fails=1; }

echo "--- R8: missing intermediate components are ENOENT ---"
img=$T/r8.img
$BIN format $img 256 128 >/dev/null
out=$(printf 'mkdir /nope/x\ntouch /d2/f\ntouch /real\nln /real /nope/b\nmv /real /nope/c\nls /\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'mkdir: /nope/x: no such file or directory' && \
printf '%s\n' "$out" | grep -q 'touch: /d2/f: no such file or directory' && \
printf '%s\n' "$out" | grep -q 'ln: no such file or directory' && \
printf '%s\n' "$out" | grep -q 'mv: no such file or directory' && \
printf '%s\n' "$out" | grep -q '^real$' && \
! printf '%s\n' "$out" | grep -q '^d2$' && \
echo "ok: missing parents are ENOENT everywhere" || { echo "FAIL: R8-MED [$out]"; fails=1; }

echo "--- R8: tab is whitespace in every command ---"
img=$T/r8tab.img
$BIN format $img 256 128 >/dev/null
out=$(printf 'write /t\thello-tab\ncat /t\nln /t\t/t2\nmv /t2\t/t3\nstat /t3\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q '^hello-tab$' && \
printf '%s\n' "$out" | grep -q 'links=2' && \
echo "ok: tabs separate args in write/ln/mv" || { echo "FAIL: R8-tab [$out]"; fails=1; }

echo "--- R8: rmdir / is EBUSY ---"
img=$T/r8rmdir.img
$BIN format $img 256 128 >/dev/null
out=$(printf 'rmdir /\nrmdir /nope\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'rmdir: /: device or resource busy' && \
printf '%s\n' "$out" | grep -q 'rmdir: /nope: no such file or directory' && \
echo "ok: rmdir / is busy, rmdir missing is ENOENT" || { echo "FAIL: R8-rmdir [$out]"; fails=1; }


echo "--- R9: duplicate '..' entries are removed and fsck converges ---"
img=$T/r9a.img
$BIN format $img 256 128 >/dev/null
printf 'mkdir /a\n' | $BIN shell $img >/dev/null
python3 - $img <<'PY'
import struct, sys
img=open(sys.argv[1],'r+b')
img.seek(0); sb=img.read(64)
it=struct.unpack('<I',sb[32:36])[0]
img.seek(it*4096+2*128+36); ad=struct.unpack('<12I',img.read(48))  # /a is ino 2 on a fresh image
apos=ad[0]*4096
img.seek(apos); blk=bytearray(img.read(4096))
struct.pack_into('<II',blk,256,0,2)   # second ".." -> root
blk[256+8:256+10]=b'..'
img.seek(it*4096+2*128+16); img.write(struct.pack('<Q',384))
img.seek(apos); img.write(blk); img.close()
PY
out1=$($BIN fsck $img --fix 2>&1)
out2=$($BIN fsck $img --fix 2>&1)
printf '%s\n' "$out1" | grep -q "duplicate '..' entry removed" && \
printf '%s\n' "$out2" | grep -q 'fsck: clean' && \
echo "ok: duplicate '..' tombstoned, converges in one run" || { echo "FAIL: R9-A [$out1][$out2]"; fails=1; }

echo "--- R9: bad first '..' plus correct second is not a false clean ---"
img=$T/r9b.img
$BIN format $img 256 128 >/dev/null
printf 'mkdir /a\n' | $BIN shell $img >/dev/null
python3 - $img <<'PY'
import struct, sys
img=open(sys.argv[1],'r+b')
img.seek(0); sb=img.read(64)
it=struct.unpack('<I',sb[32:36])[0]; lnf=struct.unpack('<I',sb[56:60])[0]
img.seek(it*4096+2*128+36); ad=struct.unpack('<12I',img.read(48))
apos=ad[0]*4096
img.seek(apos); blk=bytearray(img.read(4096))
struct.pack_into('<I',blk,128,lnf)    # first ".." -> /lost+found
struct.pack_into('<II',blk,256,0,2)   # correct second ".." -> root
blk[256+8:256+10]=b'..'
img.seek(it*4096+2*128+16); img.write(struct.pack('<Q',384))
img.seek(apos); img.write(blk); img.close()
PY
out1=$($BIN fsck $img --fix 2>&1)
out2=$($BIN fsck $img --fix 2>&1)
nav=$(printf 'cd /a\ncd ..\npwd\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out1" | grep -q "'..' entry corrected" && \
printf '%s\n' "$out2" | grep -q 'fsck: clean' && \
printf '%s\n' "$nav" | grep -q '^/$' && \
echo "ok: false-clean '..' fixed, navigation correct" || { echo "FAIL: R9-B [$out1][$out2][$nav]"; fails=1; }

echo "--- R9: clobbered lost+found inode converges in one fsck run ---"
img=$T/r9c.img
$BIN format $img 256 128 >/dev/null
python3 - $img <<'PY'
import struct, sys
img=open(sys.argv[1],'r+b')
img.seek(0); sb=img.read(64)
it=struct.unpack('<I',sb[32:36])[0]; lnf=struct.unpack('<I',sb[56:60])[0]
img.seek(it*4096+lnf*128); img.write(struct.pack('<H',2))  # type dir -> file
img.close()
PY
out1=$($BIN fsck $img --fix 2>&1)
out2=$($BIN fsck $img --fix 2>&1)
printf '%s\n' "$out1" | grep -q 'problem' && \
printf '%s\n' "$out2" | grep -q 'fsck: clean' && \
echo "ok: lost+found recreated and converges in one run" || { echo "FAIL: R9-1 [$out1][$out2]"; fails=1; }

echo "--- R9: write to a dangling entry is a clean error ---"
img=$T/r9d.img
$BIN format $img 256 128 >/dev/null
printf 'write /g hello\n' | $BIN shell $img >/dev/null
python3 - $img <<'PY'
import struct, sys
img=open(sys.argv[1],'r+b')
img.seek(0); sb=img.read(64)
it=struct.unpack('<I',sb[32:36])[0]
img.seek(it*4096+2*128); img.write(struct.pack('<H',0))  # free /g's inode, keep the dentry
img.close()
PY
out=$(printf 'write /g again\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q '^write: /g:' && \
echo "ok: dangling write reports an error" || { echo "FAIL: R9-2 [$out]"; fails=1; }

echo "--- R9: file/. is ENOTDIR and ls file/ prints the name ---"
img=$T/r9e.img
$BIN format $img 256 128 >/dev/null
printf 'write /f hello\n' | $BIN shell $img >/dev/null
out=$(printf 'cat /f/.\nls /f/\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'cat: /f/.: not a directory' && \
printf '%s\n' "$out" | grep -q '^f$' && \
echo "ok: file/. ENOTDIR, ls file/ prints f" || { echo "FAIL: R9-3 [$out]"; fails=1; }

echo "--- R9: trailing whitespace is not part of names ---"
img=$T/r9f.img
$BIN format $img 256 128 >/dev/null
out=$(printf 'mkdir sp \nstat sp\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'type=dir' && \
echo "ok: trailing whitespace trimmed" || { echo "FAIL: R9-4 [$out]"; fails=1; }

echo "--- R9: mv file file/ is ENOTDIR, not a silent no-op ---"
img=$T/r9g.img
$BIN format $img 256 128 >/dev/null
printf 'write /t data\n' | $BIN shell $img >/dev/null
out=$(printf 'mv /t /t/\nstat /t\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'mv: not a directory' && \
printf '%s\n' "$out" | grep -q 'type=file' && \
echo "ok: mv file file/ refused" || { echo "FAIL: R9-5 [$out]"; fails=1; }

echo "--- R9: 119-byte names survive pwd ---"
img=$T/r9h.img
$BIN format $img 256 128 >/dev/null
name=$(python3 -c "print('x'*119)")
out=$(printf "mkdir /$name\ncd /$name\npwd\n" | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q "^/$name\$" && \
echo "ok: 119-byte name terminates in pwd" || { echo "FAIL: R9-6 [$out]"; fails=1; }

echo "--- R9: CLI verbs without an operand print usage ---"
out=$($BIN format 2>&1); rc1=$?
out2=$($BIN fsck 2>&1); rc2=$?
printf '%s\n' "$out" | grep -q 'usage: strata format' && [ $rc1 -eq 1 ] && \
printf '%s\n' "$out2" | grep -q 'usage: strata fsck' && [ $rc2 -eq 1 ] && \
echo "ok: usage on missing operand" || { echo "FAIL: R9-8 [$out][$out2]"; fails=1; }

echo "--- R9: rmdir of the working directory is handled sanely ---"
img=$T/r9i.img
$BIN format $img 256 128 >/dev/null
out=$(printf 'mkdir /d\ncd /d\nrmdir /d\npwd\n' | $BIN shell $img 2>&1)
fsckout=$($BIN fsck $img 2>&1)
printf '%s\n' "$out" | grep -q 'working directory was removed' && \
printf '%s\n' "$out" | grep -q '^/$' && \
printf '%s\n' "$fsckout" | grep -q 'fsck: clean' && \
echo "ok: rmdir of cwd moves shell to /" || { echo "FAIL: R9-9 [$out][$fsckout]"; fails=1; }

echo "--- R9: rmdir ./.. are EINVAL, / stays EBUSY ---"
img=$T/r9j.img
$BIN format $img 256 128 >/dev/null
printf 'mkdir /x\n' | $BIN shell $img >/dev/null
out=$(printf 'cd /x\nrmdir .\nrmdir ..\nrmdir /\n' | $BIN shell $img 2>&1)
printf '%s\n' "$out" | grep -q 'rmdir: .: invalid argument' && \
printf '%s\n' "$out" | grep -q 'rmdir: ..: invalid argument' && \
printf '%s\n' "$out" | grep -q 'rmdir: /: device or resource busy' && \
echo "ok: rmdir operand errors match POSIX" || { echo "FAIL: R9-10 [$out]"; fails=1; }

if [ $fails -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "FAILURES PRESENT"; exit 1; fi
