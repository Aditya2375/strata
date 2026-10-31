/* strata fsck: offline consistency checker and repair tool.
 * Passes: superblock sanity, bitmap truth (inodes + blocks), reachability
 * from the root (orphans reattached under /lost+found), directory entry
 * integrity (. and .., dangling entries), link counts, free counts. */
#include "fs.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef struct {
    int fd, fix, errors, fixed;
    FILE *log;
    struct superblock sb;
    uint8_t *img;
    uint8_t *used_ino, *used_blk;   /* computed truth */
    uint8_t *reach;                 /* reachable from root */
    uint32_t *owner;                /* first inode claiming each data block */
    uint8_t *dir_seen;              /* a directory inode already has a dentry */
} ck;

static uint32_t cg32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t cg64(const uint8_t *p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
static uint8_t *cblk(ck *c, uint32_t b){ return c->img + (size_t)b*STRATA_BS; }
static void cwrite(ck *c, uint32_t b){
    if(c->fix && pwrite(c->fd, cblk(c,b), STRATA_BS, (off_t)b*STRATA_BS)!=STRATA_BS) fprintf(c->log,"fsck: write error on block %u\n",b);
}
static int cbit(const uint8_t *bm, uint32_t i){ return (bm[i>>3]>>(i&7))&1; }
static void csbit(uint8_t *bm, uint32_t i){ bm[i>>3]|=(uint8_t)(1u<<(i&7)); }
static void ccbit(uint8_t *bm, uint32_t i){ bm[i>>3]&=(uint8_t)~(1u<<(i&7)); }

static void report(ck *c, int fixable, const char *what, uint32_t a, uint32_t b){
    c->errors++;
    if(fixable && c->fix) c->fixed++;
    fprintf(c->log, "fsck: %s%s (ino=%u blk=%u)%s\n", fixable?"fixable:":"FATAL: ",
            what, a, b, (fixable&&c->fix)?" [fixed]":"");
}

static void idata(ck *c, uint32_t ino, struct dinode *d){
    const uint8_t *p=c->img+(uint64_t)c->sb.itable_start*STRATA_BS+(uint64_t)ino*128;
    memset(d,0,sizeof *d);
    d->type=(uint16_t)(p[0]|p[1]<<8); d->mode=(uint16_t)(p[2]|p[3]<<8);
    d->uid=cg32(p+4); d->gid=cg32(p+8); d->nlink=cg32(p+12); d->size=cg64(p+16);
    d->mtime=cg32(p+24); d->ctime=cg32(p+28); d->blocks=cg32(p+32);
    for(int i=0;i<STRATA_NDIRECT;i++) d->direct[i]=cg32(p+36+i*4);
    d->indirect=cg32(p+36+STRATA_NDIRECT*4);
}
static void iputd(ck *c, uint32_t ino, const struct dinode *d){
    uint8_t *p=c->img+(uint64_t)c->sb.itable_start*STRATA_BS+(uint64_t)ino*128;
    p[0]=d->type&0xff;p[1]=d->type>>8;p[2]=d->mode&0xff;p[3]=d->mode>>8;
    #define P32(o,v) do{uint32_t _v=(v);uint32_t _o=(uint32_t)(o);for(int _i=0;_i<4;_i++)p[_o+_i]=(uint8_t)(_v>>(8*_i));}while(0)
    P32(4,d->uid);P32(8,d->gid);P32(12,d->nlink);
    for(int i=0;i<8;i++)p[16+i]=(d->size>>(8*i))&0xff;
    P32(24,d->mtime);P32(28,d->ctime);P32(32,d->blocks);
    for(int i=0;i<STRATA_NDIRECT;i++)P32(36+i*4,d->direct[i]);
    P32(36+STRATA_NDIRECT*4,d->indirect);
    #undef P32
    cwrite(c, c->sb.itable_start + (ino*128)/STRATA_BS);
}

static int okblk(ck *c, uint32_t b){ return b>=c->sb.data_start && b<c->sb.nblocks; }

/* mark blocks of one inode; repair out-of-range pointers and duplicate
 * ownership (two inodes sharing a block: the second pointer is zeroed,
 * which stops rm-of-one corrupting the other at the cost of the second
 * file's data) */
static void claim_blk(ck *c, uint32_t ino, uint32_t b){
    csbit(c->used_blk,b);
    if(c->owner[b]==UINT32_MAX) c->owner[b]=ino;
}
static int dup_blk(ck *c, uint32_t b){
    return c->owner[b]!=UINT32_MAX; /* any prior owner conflicts */
}
static void mark_ino_blocks(ck *c, uint32_t ino, struct dinode *d){
    for(int i=0;i<STRATA_NDIRECT;i++){
        uint32_t b=d->direct[i];
        if(!b) continue;
        if(!okblk(c,b)){ report(c,1,"bad direct pointer zeroed",ino,b); d->direct[i]=0; d->blocks--; iputd(c,ino,d); continue; }
        if(dup_blk(c,b)){ report(c,1,"duplicate block ownership: duplicate pointer zeroed",ino,b); d->direct[i]=0; d->blocks--; iputd(c,ino,d); continue; }
        claim_blk(c,ino,b);
    }
    uint32_t ib=d->indirect;
    if(ib){
        if(!okblk(c,ib)){ report(c,1,"bad indirect pointer zeroed",ino,ib); d->indirect=0; d->blocks--; iputd(c,ino,d); }
        else if(dup_blk(c,ib)){ report(c,1,"duplicate block ownership: duplicate indirect zeroed",ino,ib); d->indirect=0; d->blocks--; iputd(c,ino,d); }
        else{
            claim_blk(c,ino,ib);
            uint32_t *tab=(uint32_t*)cblk(c,ib);
            int chg=0;
            for(uint32_t i=0;i<STRATA_BS/4;i++){
                uint32_t b=tab[i];
                if(!b) continue;
                if(!okblk(c,b)){ report(c,1,"bad indirect entry zeroed",ino,b); tab[i]=0; d->blocks--; chg=1; continue; }
                if(dup_blk(c,b)){ report(c,1,"duplicate block ownership: duplicate pointer zeroed",ino,b); tab[i]=0; d->blocks--; chg=1; continue; }
                claim_blk(c,ino,b);
            }
            if(chg){ cwrite(c,ib); iputd(c,ino,d); }
        }
    }
}

/* directory walk: validate entries, record reachability */
/* iterative tree walk: a deep but valid directory tree must not overflow
 * the C stack (30k nested dirs are creatable through the shell), so the
 * pending set lives on the heap */
static void walk_one(ck *c, uint32_t ino, uint32_t **stack, size_t *top, size_t *cap){
    if(c->reach[ino]) return;
    c->reach[ino]=1;
    struct dinode d; idata(c,ino,&d);
    if(d.type!=T_DIR) return;
    if(d.size%128){ report(c,1,"directory size truncated to entry boundary",ino,0); d.size-=d.size%128; iputd(c,ino,&d); }
    int seen_dot=0, seen_dotdot=0;
    for(uint64_t off=0; off<d.size; off+=128){
        uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
        if(lb<STRATA_NDIRECT) pb=d.direct[lb];
        else if(d.indirect && okblk(c,d.indirect)) pb=((uint32_t*)cblk(c,d.indirect))[lb-STRATA_NDIRECT];
        if(!okblk(c,pb)){ report(c,0,"directory has unusable data block",ino,pb); return; }
        struct dentry *e=(struct dentry*)(cblk(c,pb)+(off%STRATA_BS));
        if(e->ino==UINT32_MAX) continue;
        int bad=0;
        if(e->name[0]==0 || e->name[STRATA_NAMEMAX]!=0) bad=1;
        if(!bad && e->ino>=c->sb.ninodes) bad=2;
        if(!bad){ struct dinode t; idata(c,e->ino,&t); if(t.type==T_FREE) bad=3; }
        if(bad){
            report(c,1,"dangling directory entry removed",e->ino,pb);
            e->ino=UINT32_MAX; cwrite(c,pb);
            continue;
        }
        if(!strcmp(e->name,".")){
            seen_dot=1;
            if(e->ino!=ino){ report(c,1,"'.' entry corrected",ino,e->ino); e->ino=ino; e->type=T_DIR; cwrite(c,pb); }
            continue;
        }
        if(!strcmp(e->name,"..")){
            seen_dotdot=1; continue;
        }
        struct dinode ch; idata(c,e->ino,&ch);
        if(ch.type==T_DIR){
            if(c->dir_seen[e->ino]){
                /* a second dentry for a directory inode: strata has no
                 * directory hard links, so this is corruption, and letting
                 * fsck bake it into nlink would hide it forever */
                report(c,1,"directory hard link removed",e->ino,pb);
                e->ino=UINT32_MAX; cwrite(c,pb);
                continue;
            }
            c->dir_seen[e->ino]=1;
            if(!c->reach[e->ino]){
                if(*top==*cap){
                    size_t ncap=*cap?*cap*2:256;
                    uint32_t *ns=realloc(*stack,ncap*sizeof *ns);
                    if(!ns){ fprintf(c->log,"fsck: FATAL: out of memory walking directories\n"); c->errors++; return; }
                    *stack=ns; *cap=ncap;
                }
                (*stack)[(*top)++]=e->ino;
            }
        }
        else c->reach[e->ino]=1;
        /* duplicate name check against earlier entries in this dir */
        for(uint64_t o2=0; o2<off; o2+=128){
            uint32_t lb2=(uint32_t)(o2/STRATA_BS), pb2=0;
            if(lb2<STRATA_NDIRECT) pb2=d.direct[lb2];
            else if(d.indirect && okblk(c,d.indirect)) pb2=((uint32_t*)cblk(c,d.indirect))[lb2-STRATA_NDIRECT];
            if(!okblk(c,pb2)) break;
            struct dentry *e2=(struct dentry*)(cblk(c,pb2)+(o2%STRATA_BS));
            if(e2->ino==UINT32_MAX) continue;
            if(!strcmp(e2->name,e->name)){
                report(c,1,"duplicate directory entry removed",e->ino,pb);
                e->ino=UINT32_MAX; cwrite(c,pb);
                break;
            }
        }
    }
    (void)seen_dot; (void)seen_dotdot;
}

static void walk_dir(ck *c, uint32_t ino){
    size_t top=0, cap=256;
    uint32_t *stack=malloc(cap*sizeof *stack);
    if(!stack){ fprintf(c->log,"fsck: FATAL: out of memory walking directories\n"); c->errors++; return; }
    stack[top++]=ino;
    while(top) walk_one(c, stack[--top], &stack, &top, &cap);
    free(stack);
}

static int dir_append(ck *c, uint32_t dino, struct dinode *dd, const struct dentry *de);

/* ensure dir `ino` has a live entry named `name` pointing at `target`; fix or insert */
static int ensure_entry(ck *c, uint32_t ino, struct dinode *d, const char *name, uint32_t target){
    uint64_t tomb_off=UINT64_MAX; uint32_t tomb_pb=0;
    for(uint64_t off=0; off<d->size; off+=128){
        uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
        if(lb<STRATA_NDIRECT) pb=d->direct[lb];
        else if(d->indirect && okblk(c,d->indirect)) pb=((uint32_t*)cblk(c,d->indirect))[lb-STRATA_NDIRECT];
        if(!okblk(c,pb)) break;
        struct dentry *e=(struct dentry*)(cblk(c,pb)+(off%STRATA_BS));
        if(e->ino==UINT32_MAX){ if(tomb_off==UINT64_MAX){ tomb_off=off; tomb_pb=pb; } continue; }
        if(!strcmp(e->name,name)){
            if(e->ino!=target || e->type!=T_DIR){ e->ino=target; e->type=T_DIR; cwrite(c,pb); }
            return 1;
        }
    }
    /* insert into a tombstone slot, or grow the directory */
    struct dentry de; memset(&de,0,sizeof de);
    de.ino=target; de.type=T_DIR; strncpy(de.name,name,STRATA_NAMEMAX);
    if(tomb_off!=UINT64_MAX){
        memcpy(cblk(c,tomb_pb)+(tomb_off%STRATA_BS),&de,sizeof de);
        cwrite(c,tomb_pb);
        return 1;
    }
    return dir_append(c,ino,d,&de);
}

/* count dirents pointing at each inode + subdir counts for link checking */
static void count_links(ck *c, uint32_t *refs, uint32_t *subdirs, uint32_t *dotdot, uint32_t *parent, uint32_t *dot){
    for(uint32_t i=0;i<c->sb.ninodes;i++){
        if(!c->reach[i]) continue;
        struct dinode d; idata(c,i,&d);
        if(d.type!=T_DIR) continue;
        for(uint64_t off=0; off<d.size; off+=128){
            uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
            if(lb<STRATA_NDIRECT) pb=d.direct[lb];
            else if(d.indirect && okblk(c,d.indirect)) pb=((uint32_t*)cblk(c,d.indirect))[lb-STRATA_NDIRECT];
            if(!okblk(c,pb)) continue;
            struct dentry *e=(struct dentry*)(cblk(c,pb)+(off%STRATA_BS));
            if(e->ino==UINT32_MAX||e->ino>=c->sb.ninodes) continue;
            if(!strcmp(e->name,".")||!strcmp(e->name,"..")){
                if(!strcmp(e->name,"..")) dotdot[i]=e->ino;
                else dot[i]=e->ino;
                continue;
            }
            refs[e->ino]++;
            struct dinode ch; idata(c,e->ino,&ch);
            if(ch.type==T_DIR){ subdirs[i]++; parent[e->ino]=i; }
        }
    }
}

/* allocate one fresh zeroed data block; 0 when the image is full */
static uint32_t ck_balloc(ck *c){
    uint8_t *db=cblk(c,c->sb.dbmap_start);
    for(uint32_t b=c->sb.data_start;b<c->sb.nblocks;b++)
        if(!cbit(db,b)&&!cbit(c->used_blk,b)){
            csbit(db,b); csbit(c->used_blk,b);
            cwrite(c,c->sb.dbmap_start+(b/8)/STRATA_BS);
            memset(cblk(c,b),0,STRATA_BS);
            return b;
        }
    return 0;
}

/* append one entry to a directory, growing it by one block when the tail
 * block is full - including through the single-indirect table, so a long
 * /lost+found (or any long directory) is not stuck at the direct-block
 * limit. Returns 0 when no block could be allocated. */
static int dir_append(ck *c, uint32_t dino, struct dinode *dd, const struct dentry *de){
    uint64_t off=dd->size;
    uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
    if(lb<STRATA_NDIRECT){
        pb=dd->direct[lb];
        if(!pb){
            pb=ck_balloc(c);
            if(!pb) return 0;
            dd->direct[lb]=pb; dd->blocks++;
        }
    } else {
        uint32_t slot=lb-STRATA_NDIRECT;
        if(slot>=STRATA_BS/4) return 0; /* past single-indirect capacity */
        if(!dd->indirect || !okblk(c,dd->indirect)){
            uint32_t ib=ck_balloc(c);
            if(!ib) return 0;
            dd->indirect=ib; dd->blocks++;
        }
        uint32_t *tab=(uint32_t*)cblk(c,dd->indirect);
        pb=tab[slot];
        if(!pb){
            pb=ck_balloc(c);
            if(!pb) return 0;
            tab[slot]=pb; dd->blocks++;
            cwrite(c,dd->indirect);
        }
    }
    memcpy(cblk(c,pb)+(off%STRATA_BS),de,sizeof *de); cwrite(c,pb);
    dd->size+=128; iputd(c,dino,dd);
    return 1;
}

/* recreate a missing or clobbered /lost+found: fresh empty dir linked
 * from the root. Returns the inode, or UINT32_MAX on failure. */
static uint32_t recreate_lnf(ck *c){
    if(!c->reach[c->sb.root_ino]) return UINT32_MAX;
    uint32_t nino=c->sb.lnf_ino;
    if(c->used_ino[nino]) for(nino=0;nino<c->sb.ninodes&&c->used_ino[nino];nino++);
    if(nino>=c->sb.ninodes) return UINT32_MAX;
    uint8_t *dbm=cblk(c,c->sb.dbmap_start);
    uint32_t pb=0;
    for(uint32_t b=c->sb.data_start;b<c->sb.nblocks;b++)
        if(!cbit(dbm,b)&&!cbit(c->used_blk,b)){ pb=b; csbit(dbm,b); csbit(c->used_blk,b); cwrite(c,c->sb.dbmap_start+(b/8)/STRATA_BS); memset(cblk(c,b),0,STRATA_BS); break; }
    if(!pb) return UINT32_MAX;
    struct dentry *dd=(struct dentry*)cblk(c,pb);
    memset(dd,0,STRATA_BS);
    dd[0].ino=nino; dd[0].type=T_DIR; strcpy(dd[0].name,".");
    dd[1].ino=c->sb.root_ino; dd[1].type=T_DIR; strcpy(dd[1].name,"..");
    cwrite(c,pb);
    struct dinode nd; memset(&nd,0,sizeof nd);
    nd.type=T_DIR; nd.mode=0755; nd.nlink=2; nd.size=2*128; nd.blocks=1; nd.direct[0]=pb;
    iputd(c,nino,&nd);
    csbit(c->used_ino,nino);
    uint8_t *ibm=cblk(c,c->sb.ibmap_start);
    csbit(ibm,nino); cwrite(c,c->sb.ibmap_start+(nino/8)/STRATA_BS);
    /* link it from the root, reusing a dangling "lost+found" entry when
     * one is there instead of adding a duplicate name */
    struct dinode rd; idata(c,c->sb.root_ino,&rd);
    int linked=0;
    for(uint64_t off=0; off<rd.size && !linked; off+=128){
        uint32_t lb=(uint32_t)(off/STRATA_BS);
        uint32_t rpb= lb<STRATA_NDIRECT ? rd.direct[lb] : 0;
        if(!rpb) break;
        struct dentry *e=(struct dentry*)(cblk(c,rpb)+(off%STRATA_BS));
        if(e->ino!=UINT32_MAX && !strncmp(e->name,"lost+found",STRATA_NAMEMAX+1)){
            e->ino=nino; e->type=T_DIR; cwrite(c,rpb); linked=1;
        }
    }
    if(!linked){
        struct dentry le; memset(&le,0,sizeof le);
        le.ino=nino; le.type=T_DIR; strncpy(le.name,"lost+found",STRATA_NAMEMAX);
        if(!dir_append(c,c->sb.root_ino,&rd,&le)) return UINT32_MAX;
    }
    rd.nlink++; iputd(c,c->sb.root_ino,&rd);
    c->sb.lnf_ino=nino;
    memcpy(cblk(c,0),&c->sb,sizeof c->sb); cwrite(c,0);
    c->reach[nino]=1;
    c->dir_seen[nino]=1;
    return nino;
}

/* does /lost+found already contain an entry with this name? */
static int lnf_has(ck *c, const struct dinode *ld, const char *nm){
    for(uint64_t off=0; off<ld->size; off+=128){
        uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
        if(lb<STRATA_NDIRECT) pb=ld->direct[lb];
        else if(ld->indirect&&okblk(c,ld->indirect)) pb=((uint32_t*)cblk(c,ld->indirect))[lb-STRATA_NDIRECT];
        if(!pb||!okblk(c,pb)) continue;
        struct dentry *e=(struct dentry*)(cblk(c,pb)+(off%STRATA_BS));
        if(e->ino!=UINT32_MAX && !strncmp(e->name,nm,STRATA_NAMEMAX)) return 1;
    }
    return 0;
}

int fsck(const char *path, int fix, FILE *log){
    ck c; memset(&c,0,sizeof c);
    c.fix=fix; c.log=log; c.fd=open(path,O_RDWR);
    if(c.fd<0){ fprintf(log,"fsck: cannot open %s\n",path); return 1; }
    struct stat st;
    if(pread(c.fd,&c.sb,sizeof c.sb,0)!=(ssize_t)sizeof c.sb ||
       fstat(c.fd,&st)!=0 || !S_ISREG(st.st_mode) ||
       sb_validate(&c.sb,(uint64_t)st.st_size)){
        fprintf(log,"fsck: FATAL: bad superblock geometry, cannot proceed\n"); close(c.fd); return 1;
    }
    size_t sz=(size_t)c.sb.nblocks*STRATA_BS;
    c.img=malloc(sz);
    if(!c.img || pread(c.fd,c.img,sz,0)!=(ssize_t)sz){ fprintf(log,"fsck: short read\n"); free(c.img); close(c.fd); return 1; }
    c.used_ino=calloc(c.sb.ninodes,1);
    c.reach=calloc(c.sb.ninodes,1);
    c.used_blk=calloc(c.sb.nblocks,1);
    c.owner=malloc(c.sb.nblocks*sizeof(uint32_t));
    for(uint32_t b=0;b<c.sb.nblocks;b++) c.owner[b]=UINT32_MAX;
    c.dir_seen=calloc(c.sb.ninodes,1);
    for(uint32_t b=0;b<c.sb.data_start;b++) csbit(c.used_blk,b);

    /* pass 1: inode truth + block ownership */
    uint8_t *ib=cblk(&c,c.sb.ibmap_start);
    for(uint32_t i=0;i<c.sb.ninodes;i++){
        struct dinode d; idata(&c,i,&d);
        if(d.type!=T_FREE && d.type!=T_FILE && d.type!=T_DIR){
            report(&c,1,"unknown inode type: inode freed",i,d.type);
            if(c.fix){
                struct dinode z; memset(&z,0,sizeof z); iputd(&c,i,&z);
                ccbit(ib,i); cwrite(&c,c.sb.ibmap_start+(i/8)/STRATA_BS);
            }
            continue;
        }
        int used=(d.type!=T_FREE), bit=cbit(ib,i);
        if(used && !bit){ report(&c,1,"inode marked free but in use",i,0); csbit(ib,i); cwrite(&c,c.sb.ibmap_start+(i/8)/STRATA_BS); }
        if(!used && bit){ report(&c,1,"inode bitmap set for free inode",i,0); ccbit(ib,i); cwrite(&c,c.sb.ibmap_start+(i/8)/STRATA_BS); }
        if(used){ c.used_ino[i]=1; mark_ino_blocks(&c,i,&d); }
    }

    /* pass 2: reachability from root */
    if(c.sb.root_ino>=c.sb.ninodes){ fprintf(log,"fsck: FATAL: root inode out of range\n"); goto out; }
    walk_dir(&c,c.sb.root_ino);

    /* pass 3: orphans -> /lost+found */
    for(int progress=1;progress;){
        progress=0;
        for(uint32_t i=0;i<c.sb.ninodes;i++){
            if(!c.used_ino[i]||c.reach[i]) continue;
            /* find or recreate lost+found */
            uint32_t lnf=c.sb.lnf_ino;
            struct dinode ld; idata(&c,lnf,&ld);
            if(!c.reach[lnf]||ld.type!=T_DIR){
                if(!c.fix || (lnf=recreate_lnf(&c))==UINT32_MAX){
                    fprintf(log,"fsck: FATAL: /lost+found missing; orphan ino=%u left detached\n",i);
                    c.errors++; continue;
                }
                report(&c,1,"/lost+found recreated",lnf,0);
                idata(&c,lnf,&ld);
            }
            if(c.fix){
                char nm[32]; snprintf(nm,sizeof nm,"ino_%u",i);
                /* a previous reattach (or a user file) may already hold the
                 * name; suffix rather than duplicate it and ping-pong */
                { struct dinode ld0; idata(&c,lnf,&ld0);
                  if(lnf_has(&c,&ld0,nm)){
                    int ok=0;
                    for(char s='a';s<='z'&&!ok;s++){ snprintf(nm,sizeof nm,"ino_%u%c",i,s); ok=!lnf_has(&c,&ld0,nm); }
                    if(!ok){ fprintf(log,"fsck: FATAL: /lost+found name conflict for orphan ino=%u\n",i); c.errors++; continue; }
                  } }
                struct dentry de; memset(&de,0,sizeof de);
                de.ino=i; idata(&c,i,&ld); de.type=(uint8_t)ld.type;
                strncpy(de.name,nm,STRATA_NAMEMAX);
                idata(&c,lnf,&ld);
                /* report only after the entry is really written: printing
                 * "reattached [fixed]" before a FATAL would claim a fix
                 * that never happened */
                if(!dir_append(&c,lnf,&ld,&de)){
                    fprintf(log,"fsck: FATAL: could not grow /lost+found (orphan ino=%u still detached)\n",i);
                    c.errors++; continue;
                }
            }
            report(&c,1,"orphan inode reattached under /lost+found",i,0);
            { struct dinode od; idata(&c,i,&od); if(od.type==T_DIR) c.dir_seen[i]=1; }
            walk_dir(&c,i); /* children become reachable too */
            progress=1;
        }
    }

    /* pass 4: link counts */
    uint32_t *refs=calloc(c.sb.ninodes,4), *subs=calloc(c.sb.ninodes,4), *dd=calloc(c.sb.ninodes,4), *par=calloc(c.sb.ninodes,4), *dot=calloc(c.sb.ninodes,4);
    count_links(&c,refs,subs,dd,par,dot);
    for(uint32_t i=0;i<c.sb.ninodes;i++){
        if(!c.reach[i]) continue;
        struct dinode d; idata(&c,i,&d);
        uint32_t want = (d.type==T_DIR) ? 2+subs[i] : refs[i];
        if(d.type==T_DIR && i==c.sb.root_ino) want=2+subs[i]; /* "." counts */
        if(d.nlink!=want){ report(&c,1,"link count corrected",i,d.nlink); d.nlink=want; iputd(&c,i,&d); }
        if(d.type==T_DIR){
            uint32_t wantdd = (i==c.sb.root_ino) ? i : par[i];
            if(dd[i]!=wantdd){
                report(&c,1,"'..' entry corrected",i,dd[i]);
                if(c.fix && !ensure_entry(&c,i,&d,"..",wantdd)){
                    fprintf(c.log,"fsck: FATAL: no room to restore dotdot in ino=%u\n",i);
                    c.errors++;
                }
            }
            if(dot[i]!=i){
                report(&c,1,"'.' entry corrected",i,dot[i]);
                if(c.fix && !ensure_entry(&c,i,&d,".",i)){
                    fprintf(c.log,"fsck: FATAL: no room to restore dot in ino=%u\n",i);
                    c.errors++;
                }
            }
        }
    }
    free(refs); free(subs); free(dd); free(par); free(dot);

    /* pass 5: data bitmap vs reachable blocks */
    memset(c.used_blk,0,c.sb.nblocks);
    for(uint32_t b=0;b<c.sb.data_start;b++) csbit(c.used_blk,b);
    for(uint32_t i=0;i<c.sb.ninodes;i++){
        if(!c.reach[i]) continue;
        struct dinode d; idata(&c,i,&d);
        for(int j=0;j<STRATA_NDIRECT;j++) if(d.direct[j]&&okblk(&c,d.direct[j])) csbit(c.used_blk,d.direct[j]);
        if(d.indirect&&okblk(&c,d.indirect)){
            csbit(c.used_blk,d.indirect);
            uint32_t *tab=(uint32_t*)cblk(&c,d.indirect);
            for(uint32_t j=0;j<STRATA_BS/4;j++) if(tab[j]&&okblk(&c,tab[j])) csbit(c.used_blk,tab[j]);
        }
    }
    uint8_t *dbm=cblk(&c,c.sb.dbmap_start);
    for(uint32_t b=c.sb.data_start;b<c.sb.nblocks;b++){
        int used=cbit(c.used_blk,b), bit=cbit(dbm,b);
        if(used&&!bit){ report(&c,1,"block in use but bitmap clear",0,b); csbit(dbm,b); cwrite(&c,c.sb.dbmap_start+(b/8)/STRATA_BS); }
        if(!used&&bit){ report(&c,1,"leaked block freed",0,b); ccbit(dbm,b); cwrite(&c,c.sb.dbmap_start+(b/8)/STRATA_BS); }
    }

    /* pass 6: free counts in superblock */
    uint32_t fi=0, fb=0;
    for(uint32_t i=0;i<c.sb.ninodes;i++){ struct dinode d; idata(&c,i,&d); if(d.type==T_FREE) fi++; }
    for(uint32_t b=c.sb.data_start;b<c.sb.nblocks;b++) if(!cbit(dbm,b)) fb++;
    if(c.sb.free_inodes!=fi||c.sb.free_blocks!=fb){
        report(&c,1,"superblock free counts corrected",fi,fb);
        c.sb.free_inodes=fi; c.sb.free_blocks=fb;
        memcpy(cblk(&c,0),&c.sb,sizeof c.sb); cwrite(&c,0);
    }

    if(!c.errors) fprintf(log,"fsck: clean\n");
    else fprintf(log,"fsck: %d problem(s), %d fixed\n",c.errors,c.fixed);
    if(fix) fsync(c.fd);
out:
    free(c.img); free(c.used_ino); free(c.reach); free(c.used_blk);
    free(c.owner); free(c.dir_seen);
    close(c.fd);
    /* exit nonzero while any problem remains: FATALs are never counted in
       fixed, and fixable problems stay unfixed without --fix */
    return c.errors>c.fixed ? 1 : 0;
}
