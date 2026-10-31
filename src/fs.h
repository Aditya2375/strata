/* strata - a small inode-based file system on a disk image */
#ifndef STRATA_FS_H
#define STRATA_FS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#ifndef EFSCORRUPT
#define EFSCORRUPT EUCLEAN
#endif

#define STRATA_BS        4096
#define STRATA_MAGIC     0x53545241u /* "STRA" */
#define STRATA_NDIRECT   12
#define STRATA_NAMEMAX   119
#define STRATA_MAX_INODES 65536
#define T_FREE 0
/* hard image size cap: the whole image is mapped into memory, so an
 * absurd nblocks must be refused at validation, not OOM-kill the host */
#define STRATA_MAX_BLOCKS (1u<<18) /* 1 GiB */
#define T_DIR  1
#define T_FILE 2

struct superblock {
    uint32_t magic, block_size, nblocks, ninodes;
    uint32_t ibmap_start, ibmap_blocks;
    uint32_t dbmap_start, dbmap_blocks;
    uint32_t itable_start, itable_blocks;
    uint32_t data_start;
    uint32_t free_inodes, free_blocks;
    uint32_t root_ino, lnf_ino; /* root and /lost+found */
    uint8_t pad[STRATA_BS - 15 * 4];
};

struct dinode { /* 128 bytes on disk */
    uint16_t type, mode;
    uint32_t uid, gid, nlink;
    uint64_t size;
    uint32_t mtime, ctime, blocks;
    uint32_t direct[STRATA_NDIRECT];
    uint32_t indirect;
    uint8_t pad[128 - 2 * 2 - 4 * 4 - 8 - 3 * 4 - STRATA_NDIRECT * 4 - 4];
};

struct dentry { /* 128 bytes inside a directory file */
    uint32_t ino;
    uint8_t type, pad[3];
    char name[STRATA_NAMEMAX + 1];
};

struct fs {
    int fd;
    struct superblock sb;
    uint8_t *img;            /* whole image, nblocks * STRATA_BS bytes */
    uint8_t *dirty;          /* one byte per block */
    int crash_armed;         /* test hook: sync flushes only first N dirty blocks */
    uint32_t crash_flush;
};

struct fs_stat {
    uint16_t type, mode;
    uint32_t uid, gid, nlink, blocks;
    uint64_t size;
    uint32_t mtime, ctime;
};

void  fs_iget(struct fs *f, uint32_t ino, struct dinode *out);
void  fs_iput(struct fs *f, uint32_t ino, const struct dinode *in);
int   mkfs(const char *path, uint32_t nblocks, uint32_t ninodes, uint32_t *actual_ninodes);
int   sb_validate(const struct superblock *sb, uint64_t file_bytes);
int   fs_mount(struct fs *f, const char *path);
int   fs_sync(struct fs *f);
void  fs_unmount(struct fs *f);
int   fs_resolve(struct fs *f, uint32_t cwd, const char *path, uint32_t *out);
int   fs_create(struct fs *f, uint32_t cwd, const char *path, uint16_t type, uint32_t *out);
int   fs_touch(struct fs *f, uint32_t cwd, const char *path);
int   fs_read(struct fs *f, uint32_t ino, uint64_t off, void *buf, uint64_t n, uint64_t *got);
int   fs_write(struct fs *f, uint32_t ino, uint64_t off, const void *buf, uint64_t n);
int   fs_truncate(struct fs *f, uint32_t ino, uint64_t size);
int   fs_mkdir(struct fs *f, uint32_t cwd, const char *path);
int   fs_unlink(struct fs *f, uint32_t cwd, const char *path);
int   fs_rmdir(struct fs *f, uint32_t cwd, const char *path);
int   fs_link(struct fs *f, uint32_t cwd, const char *src, const char *dst);
int   fs_rename(struct fs *f, uint32_t cwd, const char *src, const char *dst);
int   fs_stat(struct fs *f, uint32_t ino, struct fs_stat *st);
int   fs_readdir(struct fs *f, uint32_t ino, struct dentry *out, uint32_t max, uint32_t *count, int *truncated);
int   fsck(const char *path, int fix, FILE *log);
uint64_t fs_maxfile(void);
#endif
