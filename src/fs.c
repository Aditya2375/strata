/* strata core: whole-image page cache over an image file, inode + directory
 * layer, write-back with explicit sync points. Crash hook: fs.crash_armed
 * makes fs_sync flush only the first crash_flush dirty blocks, then the
 * caller aborts - a deterministic power-fail point for fsck testing. */
#include "fs.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t g32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t g64(const uint8_t *p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
static void p16(uint8_t *p,uint16_t v){p[0]=v&0xff;p[1]=(v>>8)&0xff;}
static void p32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(v>>(8*i))&0xff;}
static void p64(uint8_t *p,uint64_t v){for(int i=0;i<8;i++)p[i]=(v>>(8*i))&0xff;}

static uint8_t *blk(struct fs *f, uint32_t b){ return f->img + (size_t)b * STRATA_BS; }
static void touch(struct fs *f, uint32_t b){ f->dirty[b] = 1; }
static uint32_t now32(void){ return (uint32_t)time(NULL); }

/* ---- bitmap helpers ---- */
static int bit_get(const uint8_t *bm, uint32_t i){ return (bm[i>>3]>>(i&7))&1; }
static void bit_set(uint8_t *bm, uint32_t i){ bm[i>>3]|=(uint8_t)(1u<<(i&7)); }
static void bit_clr(uint8_t *bm, uint32_t i){ bm[i>>3]&=(uint8_t)~(1u<<(i&7)); }
static uint8_t *ibmap(struct fs *f){ return blk(f, f->sb.ibmap_start); }
static uint8_t *dbmap(struct fs *f){ return blk(f, f->sb.data_start ? f->sb.dbmap_start : 0); }

static void sb_save(struct fs *f){ memcpy(blk(f,0), &f->sb, sizeof f->sb); touch(f,0); }

static uint32_t balloc(struct fs *f){
    for(uint32_t b=f->sb.data_start;b<f->sb.nblocks;b++){
        if(!bit_get(dbmap(f),b)){
            bit_set(dbmap(f),b);
            touch(f,f->sb.dbmap_start + (b/8)/STRATA_BS);
            memset(blk(f,b),0,STRATA_BS);
            touch(f,b);
            f->sb.free_blocks--; sb_save(f);
            return b;
        }
    }
    return 0;
}
static void bfree(struct fs *f, uint32_t b){
    if(b<f->sb.data_start||b>=f->sb.nblocks) return; /* corrupt pointer: nothing to free */
    if(!bit_get(dbmap(f),b)) return;
    bit_clr(dbmap(f),b);
    touch(f,f->sb.dbmap_start + (b/8)/STRATA_BS);
    f->sb.free_blocks++; sb_save(f);
}
static uint32_t ialloc(struct fs *f, uint16_t type){
    for(uint32_t i=0;i<f->sb.ninodes;i++){
        if(!bit_get(ibmap(f),i)){
            bit_set(ibmap(f),i);
            touch(f,f->sb.ibmap_start + (i/8)/STRATA_BS);
            struct dinode d; memset(&d,0,sizeof d);
            d.type=type; d.mode=(type==T_DIR)?0755:0644; d.nlink=(type==T_DIR)?2:1;
            d.mtime=d.ctime=now32();
            fs_iput(f,i,&d);
            f->sb.free_inodes--; sb_save(f);
            return i;
        }
    }
    return UINT32_MAX;
}
static void ifree(struct fs *f, uint32_t i){
    if(i>=f->sb.ninodes) return; /* corrupt pointer: never clear a bit outside the inode map */
    if(!bit_get(ibmap(f),i)) return;
    bit_clr(ibmap(f),i);
    touch(f,f->sb.ibmap_start + (i/8)/STRATA_BS);
    struct dinode d; memset(&d,0,sizeof d);
    fs_iput(f,i,&d);
    f->sb.free_inodes++; sb_save(f);
}

/* ---- inode I/O ---- */
void fs_iget(struct fs *f, uint32_t ino, struct dinode *out){
    if(ino>=f->sb.ninodes){ memset(out,0,sizeof *out); return; } /* corrupt pointer: report as free */
    uint64_t off=(uint64_t)f->sb.itable_start*STRATA_BS + (uint64_t)ino*128;
    const uint8_t *p=f->img+off;
    memset(out,0,sizeof *out);
    out->type=g16(p); out->mode=g16(p+2);
    out->uid=g32(p+4); out->gid=g32(p+8); out->nlink=g32(p+12);
    out->size=g64(p+16);
    out->mtime=g32(p+24); out->ctime=g32(p+28); out->blocks=g32(p+32);
    for(int i=0;i<STRATA_NDIRECT;i++) out->direct[i]=g32(p+36+i*4);
    out->indirect=g32(p+36+STRATA_NDIRECT*4);
}
void fs_iput(struct fs *f, uint32_t ino, const struct dinode *in){
    if(ino>=f->sb.ninodes) return; /* corrupt pointer: never write outside the inode table */
    uint64_t off=(uint64_t)f->sb.itable_start*STRATA_BS + (uint64_t)ino*128;
    uint8_t *p=f->img+off;
    p16(p,in->type); p16(p+2,in->mode);
    p32(p+4,in->uid); p32(p+8,in->gid); p32(p+12,in->nlink);
    p64(p+16,in->size);
    p32(p+24,in->mtime); p32(p+28,in->ctime); p32(p+32,in->blocks);
    for(int i=0;i<STRATA_NDIRECT;i++) p32(p+36+i*4,in->direct[i]);
    p32(p+36+STRATA_NDIRECT*4,in->indirect);
    touch(f,f->sb.itable_start + (ino*128)/STRATA_BS);
}

uint64_t fs_maxfile(void){ return (uint64_t)(STRATA_NDIRECT + STRATA_BS/4) * STRATA_BS; }

/* ---- block mapping ---- */
static int okblk_rt(struct fs *f, uint32_t b){ return b>=f->sb.data_start && b<f->sb.nblocks; }

static uint32_t bmap(struct fs *f, struct dinode *ip, uint32_t ino, uint32_t lb, int alloc, int *err){
    *err=0;
    if(lb < STRATA_NDIRECT){
        if(ip->direct[lb] && !okblk_rt(f,ip->direct[lb])){ *err=EFSCORRUPT; return 0; }
        if(!ip->direct[lb]){
            if(!alloc) return 0;
            ip->direct[lb]=balloc(f);
            if(!ip->direct[lb]){*err=ENOSPC;return 0;}
            ip->blocks++;
        }
        return ip->direct[lb];
    }
    lb -= STRATA_NDIRECT;
    if(lb >= STRATA_BS/4){ *err=EFBIG; return 0; }
    if(ip->indirect && !okblk_rt(f,ip->indirect)){ *err=EFSCORRUPT; return 0; }
    if(!ip->indirect){
        if(!alloc) return 0;
        ip->indirect=balloc(f);
        if(!ip->indirect){*err=ENOSPC;return 0;}
        ip->blocks++;
        fs_iput(f,ino,ip);
    }
    uint32_t *tab=(uint32_t*)blk(f,ip->indirect);
    if(tab[lb] && !okblk_rt(f,tab[lb])){ *err=EFSCORRUPT; return 0; }
    if(!tab[lb]){
        if(!alloc) return 0;
        tab[lb]=balloc(f);
        if(!tab[lb]){*err=ENOSPC;return 0;}
        touch(f,ip->indirect);
        ip->blocks++;
    }
    return tab[lb];
}

static void free_blocks(struct fs *f, struct dinode *ip, uint64_t keep_lbs){
    for(uint32_t i=0;i<STRATA_NDIRECT;i++){
        if(i>=keep_lbs && ip->direct[i]){ bfree(f,ip->direct[i]); ip->direct[i]=0; ip->blocks--; }
    }
    if(ip->indirect){
        if(!okblk_rt(f,ip->indirect)) return; /* corrupt pointer: leave the blocks marked used */
        uint32_t *tab=(uint32_t*)blk(f,ip->indirect);
        uint32_t start = keep_lbs>STRATA_NDIRECT ? keep_lbs-STRATA_NDIRECT : 0;
        for(uint32_t i=start;i<STRATA_BS/4;i++)
            if(tab[i]){ bfree(f,tab[i]); tab[i]=0; ip->blocks--; }
        touch(f,ip->indirect);
        if(keep_lbs<=STRATA_NDIRECT){ bfree(f,ip->indirect); ip->indirect=0; ip->blocks--; }
    }
}

/* ---- mount / sync ---- */
int mkfs(const char *path, uint32_t nblocks, uint32_t ninodes){
    if(ninodes<64||ninodes>STRATA_MAX_INODES) return -1;
    if(nblocks==0||nblocks>STRATA_MAX_BLOCKS) return -1; /* match the mount-side cap */
    if(ninodes%32) ninodes=(ninodes+31)&~31u;
    uint32_t itable_blocks=(ninodes*128 + STRATA_BS-1)/STRATA_BS;
    uint32_t ibmap_blocks=(ninodes/8 + STRATA_BS-1)/STRATA_BS;
    uint32_t dbmap_blocks=(nblocks/8 + STRATA_BS-1)/STRATA_BS;
    uint32_t meta=1+ibmap_blocks+dbmap_blocks+itable_blocks;
    if(nblocks < meta+8) return -1;
    int fd=open(path,O_RDWR|O_CREAT|O_TRUNC,0644);
    if(fd<0) return -1;
    if(ftruncate(fd,(off_t)nblocks*STRATA_BS)){ close(fd); return -1; }
    uint8_t *img=calloc((size_t)nblocks,STRATA_BS);
    if(!img){ close(fd); return -1; }
    struct superblock sb; memset(&sb,0,sizeof sb);
    sb.magic=STRATA_MAGIC; sb.block_size=STRATA_BS; sb.nblocks=nblocks; sb.ninodes=ninodes;
    sb.ibmap_start=1; sb.ibmap_blocks=ibmap_blocks;
    sb.dbmap_start=1+ibmap_blocks; sb.dbmap_blocks=dbmap_blocks;
    sb.itable_start=1+ibmap_blocks+dbmap_blocks; sb.itable_blocks=itable_blocks;
    sb.data_start=meta;
    sb.free_inodes=ninodes; sb.free_blocks=nblocks-meta;
    sb.root_ino=0; sb.lnf_ino=1;
    memcpy(img,&sb,sizeof sb);
    /* reserved blocks marked used */
    uint8_t *db=img+(size_t)sb.dbmap_start*STRATA_BS;
    for(uint32_t b=0;b<meta;b++) bit_set(db,b);
    uint8_t *ib=img+(size_t)sb.ibmap_start*STRATA_BS;
    bit_set(ib,0); bit_set(ib,1);
    sb.free_inodes-=2; memcpy(img,&sb,sizeof sb);
    /* root dir: . .. lost+found ; lost+found dir: . .. */
    struct dinode root; memset(&root,0,sizeof root);
    root.type=T_DIR; root.mode=0755; root.nlink=3; root.mtime=root.ctime=now32();
    struct dinode lnf=root; lnf.nlink=2;
    uint8_t *it=img+(size_t)sb.itable_start*STRATA_BS;
    uint8_t tmp[128];
    #define ENC(p,d) do{ uint8_t *q=(p); p16(q,(d).type);p16(q+2,(d).mode);p32(q+4,(d).uid);p32(q+8,(d).gid);p32(q+12,(d).nlink);p64(q+16,(d).size);p32(q+24,(d).mtime);p32(q+28,(d).ctime);p32(q+32,(d).blocks);for(int i=0;i<STRATA_NDIRECT;i++)p32(q+36+i*4,(d).direct[i]);p32(q+36+STRATA_NDIRECT*4,(d).indirect);}while(0)
    /* allocate two data blocks for the two dirs */
    uint32_t rb=meta, lb=meta+1;
    bit_set(db,rb); bit_set(db,lb);
    sb.free_blocks-=2; memcpy(img,&sb,sizeof sb);
    root.direct[0]=rb; root.blocks=1; root.size=3*128;
    lnf.direct[0]=lb; lnf.blocks=1; lnf.size=2*128;
    ENC(it,root); ENC(it+128,lnf);
    struct dentry de; memset(&de,0,sizeof de);
    uint8_t *rdir=img+(size_t)rb*STRATA_BS;
    de.ino=0;de.type=T_DIR;strcpy(de.name,".");memcpy(rdir,&de,sizeof de);
    de.ino=0;de.type=T_DIR;strcpy(de.name,"..");memcpy(rdir+128,&de,sizeof de);
    de.ino=1;de.type=T_DIR;strcpy(de.name,"lost+found");memcpy(rdir+256,&de,sizeof de);
    uint8_t *ldir=img+(size_t)lb*STRATA_BS;
    de.ino=1;de.type=T_DIR;strcpy(de.name,".");memcpy(ldir,&de,sizeof de);
    de.ino=0;de.type=T_DIR;strcpy(de.name,"..");memcpy(ldir+128,&de,sizeof de);
    ssize_t w=pwrite(fd,img,(size_t)nblocks*STRATA_BS,0);
    int ok=(w==(ssize_t)((size_t)nblocks*STRATA_BS));
    fsync(fd); free(img); close(fd);
    (void)tmp;
    return ok?0:-1;
}

/* Validate on-disk geometry before any field is trusted: regions must be
 * ordered and non-overlapping inside the image, every index must fit, and
 * the backing file must actually be large enough to hold nblocks. */
int sb_validate(const struct superblock *sb, uint64_t file_bytes){
    if(sb->magic!=STRATA_MAGIC || sb->block_size!=STRATA_BS) return -EFSCORRUPT;
    if(sb->nblocks>STRATA_MAX_BLOCKS) return -EFSCORRUPT; /* image too large */
    if(sb->ninodes<2 || sb->ninodes>STRATA_MAX_INODES) return -EFSCORRUPT;
    if(sb->ibmap_start<1) return -EFSCORRUPT;
    if((uint64_t)sb->ibmap_start+sb->ibmap_blocks > sb->dbmap_start) return -EFSCORRUPT;
    if((uint64_t)sb->dbmap_start+sb->dbmap_blocks > sb->itable_start) return -EFSCORRUPT;
    if((uint64_t)sb->itable_start+sb->itable_blocks > sb->data_start) return -EFSCORRUPT;
    if(sb->data_start>=sb->nblocks) return -EFSCORRUPT;
    if((uint64_t)sb->ibmap_blocks*STRATA_BS*8 < sb->ninodes) return -EFSCORRUPT;
    if((uint64_t)sb->dbmap_blocks*STRATA_BS*8 < sb->nblocks) return -EFSCORRUPT;
    if((uint64_t)sb->itable_blocks*STRATA_BS < (uint64_t)sb->ninodes*128) return -EFSCORRUPT;
    if(sb->root_ino>=sb->ninodes || sb->lnf_ino>=sb->ninodes) return -EFSCORRUPT;
    if(file_bytes < (uint64_t)sb->nblocks*STRATA_BS) return -EFSCORRUPT;
    return 0;
}

int fs_mount(struct fs *f, const char *path){
    memset(f,0,sizeof *f);
    f->fd=open(path,O_RDWR);
    if(f->fd<0) return -errno;
    struct superblock sb;
    struct stat st;
    if(pread(f->fd,&sb,sizeof sb,0)!=(ssize_t)sizeof sb ||
       fstat(f->fd,&st)!=0 || !S_ISREG(st.st_mode) ||
       sb_validate(&sb,(uint64_t)st.st_size)){
        close(f->fd); return -EFSCORRUPT;
    }
    f->sb=sb;
    f->img=malloc((size_t)sb.nblocks*STRATA_BS);
    f->dirty=calloc(sb.nblocks,1);
    if(!f->img||!f->dirty){ close(f->fd); free(f->img); free(f->dirty); return -ENOMEM; }
    size_t want=(size_t)sb.nblocks*STRATA_BS;
    size_t off=0;
    while(off<want){
        ssize_t r=pread(f->fd,f->img+off,want-off,off);
        if(r<=0){ fs_unmount(f); return -EIO; }
        off+=(size_t)r;
    }
    return 0;
}

int fs_sync(struct fs *f){
    uint32_t flushed=0;
    for(uint32_t b=0;b<f->sb.nblocks;b++){
        if(!f->dirty[b]) continue;
        if(f->crash_armed && flushed>=f->crash_flush) return -100-flushed; /* crash point */
        if(pwrite(f->fd,blk(f,b),STRATA_BS,(off_t)b*STRATA_BS)!=STRATA_BS) return -EIO;
        f->dirty[b]=0;
        flushed++;
    }
    return fsync(f->fd);
}

void fs_unmount(struct fs *f){
    if(f->fd>=0){ fs_sync(f); close(f->fd); }
    free(f->img); free(f->dirty);
    memset(f,0,sizeof *f); f->fd=-1;
}

/* ---- directories ---- */
static int dir_scan(struct fs *f, struct dinode *dp, uint32_t dino,
                    const char *name, uint32_t *ent_off, struct dentry *out){
    if(dp->type!=T_DIR) return -ENOTDIR;
    if(dp->size % 128) return -EFSCORRUPT;
    for(uint64_t off=0; off<dp->size; off+=128){
        uint32_t lb=(uint32_t)(off/STRATA_BS);
        int err; uint32_t pb=bmap(f,dp,dino,lb,0,&err);
        if(!pb) return -EFSCORRUPT;
        struct dentry *de=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
        if(de->ino==UINT32_MAX) continue; /* removed marker */
        if(name && strncmp(de->name,name,STRATA_NAMEMAX+1)==0){
            /* a hostile image can point a dentry outside the inode table;
               never hand that ino to fs_iget/fs_iput/ifree */
            if(de->ino >= f->sb.ninodes) return -EFSCORRUPT;
            if(ent_off) *ent_off=(uint32_t)off;
            if(out) *out=*de;
            return 0;
        }
        if(!name && out && off/128 < 100000) {} /* unused mode */
    }
    return -ENOENT;
}

static int dir_add(struct fs *f, struct dinode *dp, uint32_t dino, const char *name, uint32_t ino, uint8_t type){
    if(strlen(name)>STRATA_NAMEMAX) return -ENAMETOOLONG;
    struct dentry tmp;
    if(dir_scan(f,dp,dino,name,NULL,&tmp)==0) return -EEXIST;
    struct dentry de; memset(&de,0,sizeof de);
    de.ino=ino; de.type=type; strcpy(de.name,name);
    /* reuse a removed slot if any */
    for(uint64_t off=0; off<dp->size; off+=128){
        int err; uint32_t pb=bmap(f,dp,dino,(uint32_t)(off/STRATA_BS),0,&err);
        if(!pb) return -EFSCORRUPT;
        struct dentry *slot=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
        if(slot->ino==UINT32_MAX){ *slot=de; touch(f,pb); return 0; }
    }
    int err; uint32_t pb=bmap(f,dp,dino,(uint32_t)(dp->size/STRATA_BS),1,&err);
    if(!pb) return err?-err:-ENOSPC;
    memcpy(blk(f,pb)+(dp->size%STRATA_BS),&de,sizeof de);
    touch(f,pb);
    dp->size+=128;
    fs_iput(f,dino,dp);
    return 0;
}

static int dir_remove(struct fs *f, struct dinode *dp, uint32_t dino, const char *name, struct dentry *out){
    uint32_t off; struct dentry de;
    int r=dir_scan(f,dp,dino,name,&off,&de);
    if(r) return r;
    int err; uint32_t pb=bmap(f,dp,dino,off/STRATA_BS,0,&err);
    if(!pb) return -EFSCORRUPT;
    struct dentry *slot=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
    slot->ino=UINT32_MAX; /* tombstone */
    touch(f,pb);
    if(out) *out=de;
    return 0;
}

/* ---- path resolution ---- */
static int namei(struct fs *f, uint32_t cwd, const char *path, uint32_t *out,
                 char *last, uint32_t *parent){
    uint32_t cur = (path[0]=='/') ? f->sb.root_ino : cwd;
    uint32_t par = cur;
    if(parent) *parent=UINT32_MAX;
    char buf[1024];
    if(strlen(path)>=sizeof buf) return -ENAMETOOLONG;
    strcpy(buf,path);
    char *save=NULL;
    char *tok=strtok_r(buf,"/",&save);
    const char *prev=NULL;
    if(last) last[0]=0;
    while(tok){
        if(strlen(tok)>STRATA_NAMEMAX) return -ENAMETOOLONG;
        if(!strcmp(tok,".")){ prev=tok; tok=strtok_r(NULL,"/",&save); continue; }
        struct dinode d; fs_iget(f,cur,&d);
        if(d.type!=T_DIR) return -ENOTDIR;
        if(!strcmp(tok,"..")){
            struct dentry de;
            if(dir_scan(f,&d,cur,"..",NULL,&de)) return -EFSCORRUPT;
            cur=de.ino; prev=tok; tok=strtok_r(NULL,"/",&save); continue;
        }
        par=cur; prev=tok;
        char *next=strtok_r(NULL,"/",&save);
        if(!next){
            /* last component: always scan so callers learn existence */
            if(last){ strncpy(last,tok,STRATA_NAMEMAX); last[STRATA_NAMEMAX]=0; }
            if(parent) *parent=par;
            struct dentry de;
            int r=dir_scan(f,&d,cur,tok,NULL,&de);
            if(r) return r;
            if(out) *out=de.ino;
            return 0;
        }
        struct dentry de;
        int r=dir_scan(f,&d,cur,tok,NULL,&de);
        if(r) return r;
        cur=de.ino;
        tok=next;
    }
    if(!prev){ /* path was "/" or "" */
        if(out) *out=cur;
        if(last) last[0]=0;
        if(parent) *parent=cur;
        return 0;
    }
    if(out) *out=cur;
    return 0;
}

int fs_resolve(struct fs *f, uint32_t cwd, const char *path, uint32_t *out){
    return namei(f,cwd,path,out,NULL,NULL);
}

/* ---- file ops ---- */
int fs_create(struct fs *f, uint32_t cwd, const char *path, uint16_t type, uint32_t *out){
    char last[STRATA_NAMEMAX+1]; uint32_t par;
    int r=namei(f,cwd,path,NULL,last,&par);
    if(r==0) return -EEXIST;
    if(r!=-ENOENT) return r;
    if(!last[0]||par==UINT32_MAX) return last[0]?-ENOENT:-EEXIST;
    struct dinode pd; fs_iget(f,par,&pd);
    if(pd.type!=T_DIR) return -ENOTDIR;
    uint32_t ino=ialloc(f,type);
    if(ino==UINT32_MAX) return -ENOSPC;
    int e=dir_add(f,&pd,par,last,ino,(uint8_t)type);
    if(e){ ifree(f,ino); return e; }
    if(out) *out=ino;
    return 0;
}

int fs_read(struct fs *f, uint32_t ino, uint64_t off, void *buf, uint64_t n, uint64_t *got){
    struct dinode d; fs_iget(f,ino,&d);
    if(d.type!=T_FILE && d.type!=T_DIR) return -EINVAL;
    /* a hostile image can carry a size far past the single-indirect limit;
       clamp to the real maximum or reads stream sparse zeros for days */
    uint64_t sz=d.size, mx=fs_maxfile();
    if(sz>mx) sz=mx;
    if(off>=sz){ *got=0; return 0; }
    if(off+n>sz) n=sz-off;
    uint8_t *dst=buf; uint64_t done=0;
    while(done<n){
        uint64_t pos=off+done;
        int err; uint32_t pb=bmap(f,&d,ino,(uint32_t)(pos/STRATA_BS),0,&err);
        if(err){ *got=done; return -err; }
        uint32_t chunk=(uint32_t)(STRATA_BS-(pos%STRATA_BS));
        if(chunk>n-done) chunk=(uint32_t)(n-done);
        if(pb) memcpy(dst+done,blk(f,pb)+(pos%STRATA_BS),chunk);
        else memset(dst+done,0,chunk);
        done+=chunk;
    }
    *got=done;
    return 0;
}

int fs_write(struct fs *f, uint32_t ino, uint64_t off, const void *buf, uint64_t n){
    struct dinode d; fs_iget(f,ino,&d);
    if(d.type!=T_FILE) return d.type==T_DIR?-EISDIR:-EINVAL;
    if(off+n>fs_maxfile()) return -EFBIG;
    const uint8_t *src=buf; uint64_t done=0;
    while(done<n){
        uint64_t pos=off+done;
        int err; uint32_t pb=bmap(f,&d,ino,(uint32_t)(pos/STRATA_BS),1,&err);
        if(!pb){ fs_iput(f,ino,&d); return err?-err:-ENOSPC; }
        uint32_t chunk=(uint32_t)(STRATA_BS-(pos%STRATA_BS));
        if(chunk>n-done) chunk=(uint32_t)(n-done);
        memcpy(blk(f,pb)+(pos%STRATA_BS),src+done,chunk);
        touch(f,pb);
        done+=chunk;
    }
    if(off+n>d.size) d.size=off+n;
    d.mtime=now32();
    fs_iput(f,ino,&d);
    return 0;
}

int fs_truncate(struct fs *f, uint32_t ino, uint64_t size){
    struct dinode d; fs_iget(f,ino,&d);
    if(d.type!=T_FILE) return -EISDIR;
    if(size>fs_maxfile()) return -EFBIG;
    if(size<d.size){
        uint64_t keep=(size+STRATA_BS-1)/STRATA_BS;
        free_blocks(f,&d,keep);
        /* zero the tail of the last kept block */
        if(size%STRATA_BS){
            int err; uint32_t pb=bmap(f,&d,ino,(uint32_t)(size/STRATA_BS),0,&err);
            if(pb){ memset(blk(f,pb)+(size%STRATA_BS),0,STRATA_BS-(size%STRATA_BS)); touch(f,pb); }
        }
    }
    d.size=size; d.mtime=now32();
    fs_iput(f,ino,&d);
    return 0;
}

int fs_mkdir(struct fs *f, uint32_t cwd, const char *path){
    uint32_t ino;
    int r=fs_create(f,cwd,path,T_DIR,&ino);
    if(r) return r;
    struct dinode d; fs_iget(f,ino,&d);
    /* parent for ".." */
    char last[STRATA_NAMEMAX+1]; uint32_t par;
    r=namei(f,cwd,path,NULL,last,&par);
    if(r){ free_blocks(f,&d,0); ifree(f,ino); return r; }
    int e=dir_add(f,&d,ino,".",ino,T_DIR);
    if(!e) e=dir_add(f,&d,ino,"..",par,T_DIR);
    if(e){
        /* roll back so a failed mkdir never leaves a half-initialized
         * directory: drop the parent's entry, free the inode's blocks */
        struct dinode pd; fs_iget(f,par,&pd);
        dir_remove(f,&pd,par,last,NULL);
        free_blocks(f,&d,0); ifree(f,ino);
        return e;
    }
    fs_iget(f,par,&d);
    d.nlink++; fs_iput(f,par,&d);
    return 0;
}

int fs_unlink(struct fs *f, uint32_t cwd, const char *path){
    char last[STRATA_NAMEMAX+1]; uint32_t par;
    int r=namei(f,cwd,path,NULL,last,&par);
    if(r||!last[0]||par==UINT32_MAX) return r?r:-ENOENT;
    struct dinode pd; fs_iget(f,par,&pd);
    struct dentry de;
    r=dir_remove(f,&pd,par,last,&de);
    if(r) return r;
    if(de.type==T_DIR){ dir_add(f,&pd,par,last,de.ino,de.type); return -EISDIR; }
    struct dinode d; fs_iget(f,de.ino,&d);
    if(--d.nlink==0){ free_blocks(f,&d,0); ifree(f,de.ino); }
    else fs_iput(f,de.ino,&d);
    return 0;
}

int fs_rmdir(struct fs *f, uint32_t cwd, const char *path){
    char last[STRATA_NAMEMAX+1]; uint32_t par;
    int r=namei(f,cwd,path,NULL,last,&par);
    if(r||!last[0]||par==UINT32_MAX) return r?r:-ENOENT;
    struct dinode pd; fs_iget(f,par,&pd);
    struct dentry de;
    r=dir_scan(f,&pd,par,last,NULL,&de);
    if(r) return r;
    if(de.ino==f->sb.lnf_ino) return -EPERM; /* /lost+found is fsck's */
    if(de.type!=T_DIR) return -ENOTDIR;
    struct dinode d; fs_iget(f,de.ino,&d);
    for(uint64_t off=0; off<d.size; off+=128){
        int err; uint32_t pb=bmap(f,&d,de.ino,(uint32_t)(off/STRATA_BS),0,&err);
        if(!pb) return -EFSCORRUPT;
        struct dentry *e=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
        if(e->ino==UINT32_MAX) continue;
        if(strcmp(e->name,".")&&strcmp(e->name,"..")) return -ENOTEMPTY;
    }
    dir_remove(f,&pd,par,last,NULL);
    pd.nlink--; fs_iput(f,par,&pd);
    free_blocks(f,&d,0); ifree(f,de.ino);
    return 0;
}

int fs_link(struct fs *f, uint32_t cwd, const char *src, const char *dst){
    uint32_t sino;
    int r=namei(f,cwd,src,&sino,NULL,NULL);
    if(r) return r;
    struct dinode d; fs_iget(f,sino,&d);
    if(d.type==T_DIR) return -EPERM;
    char last[STRATA_NAMEMAX+1]; uint32_t par;
    r=namei(f,cwd,dst,NULL,last,&par);
    if(r==0) return -EEXIST;
    if(r!=-ENOENT) return r;
    if(!last[0]||par==UINT32_MAX) return -ENOENT;
    struct dinode pd; fs_iget(f,par,&pd);
    r=dir_add(f,&pd,par,last,sino,d.type);
    if(r) return r;
    d.nlink++; fs_iput(f,sino,&d);
    return 0;
}

int fs_rename(struct fs *f, uint32_t cwd, const char *src, const char *dst){
    char slast[STRATA_NAMEMAX+1]; uint32_t spar;
    int r=namei(f,cwd,src,NULL,slast,&spar);
    if(r) return r;
    uint32_t sino;
    if(namei(f,cwd,src,&sino,NULL,NULL)) return -ENOENT;
    if(!slast[0]||spar==UINT32_MAX){
        /* the source path ends in "." or ".." (or is "/"): POSIX rejects
         * renaming those - EINVAL for dot/dotdot, EBUSY for the root */
        return sino==f->sb.root_ino?-EBUSY:-EINVAL;
    }
    if(sino==f->sb.lnf_ino) return -EPERM; /* /lost+found is fsck's */
    struct dinode sd; fs_iget(f,sino,&sd);
    char dlast[STRATA_NAMEMAX+1]; uint32_t dpar;
    r=namei(f,cwd,dst,NULL,dlast,&dpar);
    size_t dlen=strlen(dst);
    int dst_slash = dlen>1 && dst[dlen-1]=='/'; /* trailing slash: dst must name a directory */
    if(r==0 && !dlast[0]) return -EBUSY; /* renaming onto "."/".." or "/" */
    /* rename onto the same name in the same directory is a no-op */
    if(r==0 && spar!=UINT32_MAX){
        uint32_t dino2;
        if(namei(f,cwd,dst,&dino2,NULL,NULL)==0 && dino2==sino) return 0;
    }
    /* reject moving a directory into its own subtree (would create a cycle) */
    if(sd.type==T_DIR){
        /* the receiving directory is dpar in both cases; if it sits inside
         * src the move would create a cycle */
        if(r==-ENOENT && dpar==UINT32_MAX) return -ENOENT;
        if(r && r!=-ENOENT) return r;
        uint32_t cur=dpar; int stopped=0;
        /* bound by the inode count, not a constant: a ".." walk longer than
         * ninodes revisits an inode, which means the chain itself is broken */
        for(uint32_t hops=0; hops<f->sb.ninodes; hops++){
            if(cur==sino) return -EINVAL;
            if(cur==f->sb.root_ino){ stopped=1; break; }
            struct dinode d; fs_iget(f,cur,&d);
            if(d.type!=T_DIR){ stopped=1; break; }
            struct dentry de;
            if(dir_scan(f,&d,cur,"..",NULL,&de)){ stopped=1; break; }
            if(de.ino==cur){ stopped=1; break; }
            cur=de.ino;
        }
        if(!stopped) return -EFSCORRUPT;
    }
    uint32_t replaced_ino=UINT32_MAX; uint8_t replaced_type=0;
    if(r==0){
        if(!dlast[0]) return -EBUSY; /* rename over "/" */
        struct dinode pd; fs_iget(f,dpar,&pd);
        struct dentry de;
        if(dir_scan(f,&pd,dpar,dlast,NULL,&de)) return -EFSCORRUPT;
        if(de.ino==f->sb.lnf_ino) return -EPERM; /* /lost+found is fsck's */
        if(sd.type!=T_DIR && de.type==T_DIR) return dst_slash?-ENOTDIR:-EISDIR;
        if(sd.type==T_DIR && de.type!=T_DIR) return -ENOTDIR;
        if(de.type==T_DIR){
            struct dinode dd; fs_iget(f,de.ino,&dd);
            for(uint64_t off=0; off<dd.size; off+=128){
                int err; uint32_t pb=bmap(f,&dd,de.ino,(uint32_t)(off/STRATA_BS),0,&err);
                if(!pb) return -EFSCORRUPT;
                struct dentry *e=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
                if(e->ino==UINT32_MAX) continue;
                if(strcmp(e->name,".")&&strcmp(e->name,"..")) return -ENOTEMPTY;
            }
        }
        dir_remove(f,&pd,dpar,dlast,NULL);
        replaced_ino=de.ino; replaced_type=de.type;
        r=-ENOENT; /* dst slot now free */
    }
    if(r!=-ENOENT) return r;
    if(!dlast[0]||dpar==UINT32_MAX) return -ENOENT;
    if(dst_slash && sd.type!=T_DIR) return -ENOTDIR; /* "mv file newdir/" */
    struct dinode spd; fs_iget(f,spar,&spd);
    struct dentry de;
    r=dir_remove(f,&spd,spar,slast,&de);
    if(r) return r;
    struct dinode dpd; fs_iget(f,dpar,&dpd);
    r=dir_add(f,&dpd,dpar,dlast,de.ino,de.type);
    if(r){ dir_add(f,&spd,spar,slast,de.ino,de.type); return r; }
    if(de.type==T_DIR){
        /* fix ".." when moved across dirs */
        struct dinode md; fs_iget(f,de.ino,&md);
        uint32_t off; struct dentry dotdot;
        if(dir_scan(f,&md,de.ino,"..",&off,&dotdot)==0 && dotdot.ino!=dpar){
            int err; uint32_t pb=bmap(f,&md,de.ino,off/STRATA_BS,0,&err);
            if(pb){ ((struct dentry*)(blk(f,pb)+(off%STRATA_BS)))->ino=dpar; touch(f,pb); }
            fs_iget(f,spar,&spd); spd.nlink--; fs_iput(f,spar,&spd);
            dpd.nlink++; fs_iput(f,dpar,&dpd);
        }
    }
    if(replaced_ino!=UINT32_MAX){
        struct dinode rd; fs_iget(f,replaced_ino,&rd);
        if(replaced_type==T_DIR){
            rd.nlink-=2; free_blocks(f,&rd,0); ifree(f,replaced_ino);
            /* the replaced directory also removed one subdirectory link
             * from the receiving parent */
            dpd.nlink--; fs_iput(f,dpar,&dpd);
        }
        else if(--rd.nlink==0){ free_blocks(f,&rd,0); ifree(f,replaced_ino); }
        else fs_iput(f,replaced_ino,&rd);
    }
    return 0;
}

int fs_stat(struct fs *f, uint32_t ino, struct fs_stat *st){
    struct dinode d; fs_iget(f,ino,&d);
    if(d.type==T_FREE) return -ENOENT;
    st->type=d.type; st->mode=d.mode; st->uid=d.uid; st->gid=d.gid;
    st->nlink=d.nlink; st->blocks=d.blocks; st->size=d.size;
    st->mtime=d.mtime; st->ctime=d.ctime;
    return 0;
}

int fs_readdir(struct fs *f, uint32_t ino, struct dentry *out, uint32_t max, uint32_t *count, int *truncated){
    struct dinode d; fs_iget(f,ino,&d);
    if(d.type!=T_DIR) return -ENOTDIR;
    if(truncated) *truncated=0;
    uint32_t n=0;
    uint64_t off=0;
    for(; off<d.size && n<max; off+=128){
        int err; uint32_t pb=bmap(f,&d,ino,(uint32_t)(off/STRATA_BS),0,&err);
        if(!pb) return -EFSCORRUPT;
        struct dentry *e=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
        if(e->ino==UINT32_MAX) continue;
        out[n++]=*e;
    }
    /* if the caller's buffer filled, say whether entries were left out */
    if(truncated && off<d.size){
        for(; off<d.size; off+=128){
            int err; uint32_t pb=bmap(f,&d,ino,(uint32_t)(off/STRATA_BS),0,&err);
            if(!pb) return -EFSCORRUPT;
            struct dentry *e=(struct dentry*)(blk(f,pb)+(off%STRATA_BS));
            if(e->ino!=UINT32_MAX){ *truncated=1; break; }
        }
    }
    *count=n;
    return 0;
}
