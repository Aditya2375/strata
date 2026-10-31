# strata architecture

## On-disk layout

All integers are little-endian. Block size is 4096 everywhere.

| region | blocks | contents |
| --- | --- | --- |
| superblock | 1 | magic "STRA", sizes, region offsets, free counts, root + lost+found inode numbers |
| inode bitmap | computed | one bit per inode |
| data bitmap | computed | one bit per block |
| inode table | ninodes*128/4096 | 128-byte `dinode` records |
| data | rest | file and directory contents |

`dinode`: type, mode, uid, gid, nlink, size, mtime, ctime, allocated
block count, 12 direct pointers, 1 single-indirect pointer.
`dentry`: inode number, type, 119-byte name + NUL. A deleted entry is a
tombstone (ino = 0xFFFFFFFF) and is reused by later creates.

## Write ordering and crashes

Mounted images are fully cached in memory. Writes mutate the cache and
mark blocks dirty; `sync` writes dirty blocks back in ascending block
order and fsyncs. This means a crash leaves some prefix of the dirty
set on disk - metadata and data interleaved in block-address order -
which is exactly what the `crash K` test hook simulates (flush K
blocks, abort). Nothing about recovery relies on write *ordering*:
fsck treats the image as arbitrary partial state and rebuilds
consistency from the reachable set.

## fsck passes

1. superblock sanity (fatal if unusable)
2. inode bitmap vs inode type field; record blocks owned by each live
   inode (out-of-range pointers zeroed)
3. reachability walk from the root; orphan inodes are linked into
   /lost+found as `ino_N`
4. link counts (dirs: 2 + subdirectories; files: referencing entries);
   `.` and `..` entries verified or re-created
5. data bitmap vs blocks of reachable files (leaks freed, unmarked
   in-use blocks marked)
6. superblock free counts

## What the model test compares

`tests/model-test.py` plays random operation sequences against both
strata and a Python model whose files are keyed by inode (so hard
links share content). After every 25 operations it compares a
deterministic recursive dump: entry type, full path, byte size, and a
FNV-1a-64 hash of file contents.
