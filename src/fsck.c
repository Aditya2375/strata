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

/* mark blocks of one inode; repair out-of-range pointers */
static void mark_ino_blocks(ck *c, uint32_t ino, struct dinode *d){
    for(int i=0;i<STRATA_NDIRECT;i++){
        uint32_t b=d->direct[i];
        if(!b) continue;
        if(!okblk(c,b)){ report(c,1,"bad direct pointer zeroed",ino,b); d->direct[i]=0; d->blocks--; iputd(c,ino,d); continue; }
        csbit(c->used_blk,b);
    }
    uint32_t ib=d->indirect;
    if(ib){
        if(!okblk(c,ib)){ report(c,1,"bad indirect pointer zeroed",ino,ib); d->indirect=0; d->blocks--; iputd(c,ino,d); }
        else{
            csbit(c->used_blk,ib);
            uint32_t *tab=(uint32_t*)cblk(c,ib);
            int chg=0;
            for(uint32_t i=0;i<STRATA_BS/4;i++){
                uint32_t b=tab[i];
                if(!b) continue;
                if(!okblk(c,b)){ report(c,1,"bad indirect entry zeroed",ino,b); tab[i]=0; d->blocks--; chg=1; continue; }
                csbit(c->used_blk,b);
            }
            if(chg){ cwrite(c,ib); iputd(c,ino,d); }
        }
    }
}

/* directory walk: validate entries, record reachability */
static void walk_dir(ck *c, uint32_t ino){
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
        if(ch.type==T_DIR) walk_dir(c,e->ino);
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
    uint64_t off; uint32_t pb;
    if(tomb_off!=UINT64_MAX){ off=tomb_off; pb=tomb_pb; }
    else{
        off=d->size;
        uint32_t lb=(uint32_t)(off/STRATA_BS);
        if(lb<STRATA_NDIRECT) pb=d->direct[lb];
        else pb=0;
        if(!pb){
            uint8_t *dbm=cblk(c,c->sb.dbmap_start);
            pb=0;
            for(uint32_t b=c->sb.data_start;b<c->sb.nblocks;b++)
                if(!cbit(dbm,b)&&!cbit(c->used_blk,b)){ pb=b; csbit(dbm,b); csbit(c->used_blk,b);
                    cwrite(c,c->sb.dbmap_start+(b/8)/STRATA_BS); memset(cblk(c,b),0,STRATA_BS); break; }
            if(!pb || lb>=STRATA_NDIRECT) return 0;
            d->direct[lb]=pb; d->blocks++;
        }
        d->size+=128;
        iputd(c,ino,d);
    }
    struct dentry de; memset(&de,0,sizeof de);
    de.ino=target; de.type=T_DIR; strncpy(de.name,name,STRATA_NAMEMAX);
    memcpy(cblk(c,pb)+(off%STRATA_BS),&de,sizeof de);
    cwrite(c,pb);
    return 1;
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
    for(uint32_t b=0;b<c.sb.data_start;b++) csbit(c.used_blk,b);

    /* pass 1: inode truth + block ownership */
    uint8_t *ib=cblk(&c,c.sb.ibmap_start);
    for(uint32_t i=0;i<c.sb.ninodes;i++){
        struct dinode d; idata(&c,i,&d);
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
                fprintf(log,"fsck: FATAL: /lost+found missing; orphan ino=%u left detached\n",i);
                c.errors++; continue;
            }
            report(&c,1,"orphan inode reattached under /lost+found",i,0);
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
                uint64_t off=ld.size;
                uint32_t lb=(uint32_t)(off/STRATA_BS), pb=0;
                if(lb<STRATA_NDIRECT) pb=ld.direct[lb];
                else if(ld.indirect&&okblk(&c,ld.indirect)) pb=((uint32_t*)cblk(&c,ld.indirect))[lb-STRATA_NDIRECT];
                if(!pb){
                    /* allocate a fresh block for lost+found growth */
                    uint8_t *db=cblk(&c,c.sb.dbmap_start);
                    for(uint32_t b=c.sb.data_start;b<c.sb.nblocks;b++)
                        if(!cbit(db,b)&&!cbit(c.used_blk,b)){ pb=b; csbit(db,b); csbit(c.used_blk,b); cwrite(&c,c.sb.dbmap_start+(b/8)/STRATA_BS); memset(cblk(&c,b),0,STRATA_BS); break; }
                    if(pb && lb<STRATA_NDIRECT){ ld.direct[lb]=pb; ld.blocks++; }
                    else pb=0;
                }
                if(pb){
                    memcpy(cblk(&c,pb)+(off%STRATA_BS),&de,sizeof de); cwrite(&c,pb);
                    ld.size+=128; iputd(&c,lnf,&ld);
                } else { fprintf(log,"fsck: FATAL: could not grow /lost+found (orphan ino=%u still detached)\n",i); c.errors++; }
            }
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
    close(c.fd);
    /* exit nonzero while any problem remains: FATALs are never counted in
       fixed, and fixable problems stay unfixed without --fix */
    return c.errors>c.fixed ? 1 : 0;
}
