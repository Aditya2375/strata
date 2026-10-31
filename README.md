# strata

A small inode-based file system in C, in the spirit of early Unix file
systems (and xv6's): a disk image file is the block device, with a
superblock, inode and block bitmaps, an inode table, and data blocks.
It comes with an interactive shell, a real `fsck` that finds and repairs
corruption, and a crash hook for power-fail testing.

**[Try it in your browser](https://aditya2375.github.io/strata/)** — the
same C code compiled to WebAssembly.

## Layout

```
block 0          superblock
block 1..        inode bitmap | data block bitmap | inode table | data blocks
```

4096-byte blocks. 128-byte inodes: type, mode, owner, link count, size,
timestamps, 12 direct block pointers + 1 single-indirect pointer
(max file ~4.1 MiB). Directories are files of 128-byte entries.
`mkfs` creates the root directory and `/lost+found`.

## Features

- Files: create, read, write, append, truncate (with block freeing)
- Directories: mkdir, rmdir, nested paths, `ls`, `cd`, `pwd`
- Hard links (shared inode, link counts), rename (incl. across
  directories, POSIX edge cases: self-rename is a no-op, renaming a
  directory into its own subtree is rejected, rename onto hard links
  to the same file is a no-op)
- Write-back cache with explicit `sync`; the shell's `crash K` hook
  flushes only the first K dirty blocks and aborts - a deterministic
  power-fail point
- `strata fsck IMG [--fix]`: checks bitmaps against actual use,
  reachability from the root (orphans are reattached under
  `/lost+found`), directory entry integrity (dangling entries,
  duplicate names, `.` and `..`), link counts, and free-space counts
- Honest errors: ENOENT, EEXIST, ENOTDIR, EISDIR, ENOTEMPTY, ENOSPC,
  EFBIG, EINVAL, EPERM

## Testing

`./tests/run-tests.sh` runs:

- **3,195 unit checks** (allocator cycles, a 2 MiB file crossing into
  indirect blocks with byte-exact read-back, partial block-boundary
  writes, truncation, tombstone reuse, deep nesting, ENOSPC fill)
- scripted shell cases: basic ops, error paths, persistence across
  mounts, hard links, rename semantics, deep trees
- a **crash-recovery sweep**: a scripted workload aborted at 9 different
  flush points; `fsck --fix` must repair every one and a second `fsck`
  must come back clean
- **fsck corruption-injection tests**: bitmap bits flipped both ways,
  a zeroed link count, an orphan inode, a corrupted `..` pointer -
  each repaired to a clean pass
- a **differential model test**: 8 random seeds x 600 operations
  (write/append/mkdir/rm/rmdir/mv/ln), comparing a deterministic
  full-tree dump (paths, sizes, FNV-1a content hashes) against a
  Python reference model after every 25 ops
- 300 random input-fuzz sessions
- everything above also runs under AddressSanitizer + UBSan in CI

The model tests caught real bugs before release: a namei failure path
that let a create write through an unresolved parent, a rename that
could delete the file it was renaming, a rename that could cycle a
directory into its own subtree, and an fsck encoder macro whose loop
variable shadowed the caller's.

## Honest limits

- No journal: a crash can lose recent writes. `fsck --fix` restores a
  consistent file system afterwards (that is the tested contract), but
  it cannot recover data that never reached the disk image.
- The whole image is cached in memory while mounted (like a ramdisk);
  `sync` flushes dirty blocks. Images are meant to be megabytes, not
  gigabytes.
- Single-indirect addressing only: files are capped at ~4.1 MiB.
- No permissions enforcement (mode/uid/gid are stored, not checked),
  no symlinks, no sparse-file detection beyond zero-filling.
- `fsck` does not attempt to recover orphaned *file contents* by
  scanning for data - orphaned inodes are reattached by inode number.
- Not a kernel file system: no FUSE mount, this runs in userspace on
  an image file.

## Build & use

```sh
make                        # builds build/strata
./build/strata format my.img 2048 1024     # image: 2048 blocks, 1024 inodes
./build/strata shell my.img                # REPL (also: ./build/strata my.img)
./build/strata fsck my.img --fix
make test
```

REPL commands: `ls [-l] cd pwd mkdir touch write append cat rm rmdir
ln mv stat df dump fsck sync crash [K] help exit`

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the on-disk
structures and the write-ordering argument.
