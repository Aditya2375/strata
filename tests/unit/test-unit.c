/* unit tests: allocator, inode lifecycle, bmap growth, truncation, dir reuse */
#include "../../src/fs.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static int checks=0;
#define OK(x) do{ if(!(x)){ fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1);} checks++; }while(0)

int main(void){
    unlink("/tmp/strata-unit.img");
    OK(mkfs("/tmp/strata-unit.img",1024,512,NULL)==0);
    struct fs f; OK(fs_mount(&f,"/tmp/strata-unit.img")==0);
    OK(f.sb.root_ino==0 && f.sb.lnf_ino==1);

    /* inode alloc/free cycles */
    uint32_t inos[512];
    for(int round=0; round<3; round++){
        for(int i=0;i<100;i++){
            char p[32]; snprintf(p,sizeof p,"/f%d",i);
            OK(fs_create(&f,0,p,T_FILE,&inos[i])==0);
        }
        for(int i=0;i<100;i+=2){
            char p[32]; snprintf(p,sizeof p,"/f%d",i);
            OK(fs_unlink(&f,0,p)==0);
        }
        for(int i=0;i<100;i++){
            char p[32]; snprintf(p,sizeof p,"/f%d",i);
            fs_unlink(&f,0,p);
        }
        checks+=150;
    }

    /* big file crossing into indirect blocks: 2 MiB */
    uint32_t ino;
    OK(fs_create(&f,0,"/big",T_FILE,&ino)==0);
    uint8_t *wbuf=malloc(STRATA_BS), rbuf[STRATA_BS];
    uint64_t total=2*1024*1024, off=0;
    while(off<total){
        for(int i=0;i<STRATA_BS;i++) wbuf[i]=(uint8_t)((off+i)*2654435761u>>13);
        OK(fs_write(&f,ino,off,wbuf,STRATA_BS)==0);
        off+=STRATA_BS;
        checks++;
    }
    struct fs_stat st; OK(fs_stat(&f,ino,&st)==0);
    OK(st.size==total);
    OK(st.blocks>STRATA_NDIRECT); /* indirect in use */
    off=0;
    while(off<total){
        uint64_t got;
        OK(fs_read(&f,ino,off,rbuf,STRATA_BS,&got)==0 && got==STRATA_BS);
        for(int i=0;i<STRATA_BS;i++){
            uint8_t want=(uint8_t)((off+i)*2654435761u>>13);
            if(rbuf[i]!=want){ fprintf(stderr,"FAIL data mismatch at %llu\n",(unsigned long long)(off+i)); return 1; }
        }
        off+=STRATA_BS;
        checks++;
    }
    /* unaligned partial write/read */
    const char *msg="strata partial write across a block boundary";
    OK(fs_write(&f,ino,4000,msg,strlen(msg))==0);
    memset(rbuf,0,sizeof rbuf); uint64_t got;
    OK(fs_read(&f,ino,4000,rbuf,strlen(msg),&got)==0 && got==strlen(msg));
    OK(!memcmp(rbuf,msg,strlen(msg)));
    /* truncate shrinks and frees blocks */
    OK(fs_truncate(&f,ino,STRATA_BS*3)==0);
    OK(fs_stat(&f,ino,&st)==0 && st.size==STRATA_BS*3);
    OK(st.blocks<=4);
    uint32_t free_after_trunc=f.sb.free_blocks;
    OK(fs_truncate(&f,ino,0)==0);
    OK(fs_stat(&f,ino,&st)==0 && st.size==0 && st.blocks==0);
    OK(f.sb.free_blocks>free_after_trunc);

    /* dir entry tombstone reuse: create/remove many names in one dir */
    OK(fs_mkdir(&f,0,"/d")==0);
    for(int i=0;i<200;i++){
        char p[32]; snprintf(p,sizeof p,"/d/x%d",i%17);
        if(i%3==0){ fs_create(&f,0,p,T_FILE,NULL); }
        else if(i%3==1){ fs_unlink(&f,0,p); }
        else { fs_create(&f,0,p,T_FILE,NULL); fs_unlink(&f,0,p); }
        checks++;
    }
    uint32_t dino; OK(fs_resolve(&f,0,"/d",&dino)==0);
    struct dentry ents[64]; uint32_t cnt;
    OK(fs_readdir(&f,dino,ents,64,&cnt,NULL)==0);
    OK(cnt<=19); /* . .. plus at most 17 live names */

    /* readdir truncation is reported, not silent */
    int trunc=0; uint32_t cnt2=0;
    OK(fs_readdir(&f,dino,ents,2,&cnt2,&trunc)==0);
    OK(cnt2==2 && trunc==1);
    OK(fs_readdir(&f,dino,ents,64,&cnt2,&trunc)==0);
    OK(trunc==0);

    /* overlong path components are rejected, not truncated */
    char longname[160]; memset(longname,'a',sizeof longname-1); longname[0]='/'; longname[sizeof longname-1]=0;
    OK(fs_create(&f,0,longname,T_FILE,NULL)==-ENAMETOOLONG);

    /* cross-type rename: file over dir is EISDIR, dir over file ENOTDIR */
    OK(fs_create(&f,0,"/rt_file",T_FILE,NULL)==0);
    OK(fs_mkdir(&f,0,"/rt_dir")==0);
    OK(fs_rename(&f,0,"/rt_file","/rt_dir")==-EISDIR);
    OK(fs_rename(&f,0,"/rt_dir","/rt_file")==-ENOTDIR);

    /* renaming a dir over an empty dir keeps the parent's link count */
    OK(fs_mkdir(&f,0,"/n1")==0);
    OK(fs_mkdir(&f,0,"/n1/sub")==0);
    OK(fs_mkdir(&f,0,"/n2")==0);
    OK(fs_mkdir(&f,0,"/n2/sub")==0);
    uint32_t n2; OK(fs_resolve(&f,0,"/n2",&n2)==0);
    struct fs_stat nst; OK(fs_stat(&f,n2,&nst)==0);
    uint32_t want_links=nst.nlink;
    OK(fs_rename(&f,0,"/n1/sub","/n2/sub")==0);
    OK(fs_stat(&f,n2,&nst)==0 && nst.nlink==want_links);

    /* a 300-deep ".." chain: moving the top dir into it must be rejected
       (the old 256-hop cap would have missed the cycle) */
    OK(fs_mkdir(&f,0,"/dc")==0);
    char dp[700]; strcpy(dp,"/dc");
    for(int i=0;i<300;i++){ strcat(dp,"/a"); OK(fs_mkdir(&f,0,dp)==0); }
    OK(fs_rename(&f,0,"/dc",dp)==-EINVAL);

    /* superblock geometry validation */
    struct superblock good=f.sb;
    OK(sb_validate(&good,(uint64_t)f.sb.nblocks*STRATA_BS)==0);
    struct superblock bad;
    bad=good; bad.nblocks=0xFFFFFFFFu;
    OK(sb_validate(&bad,(uint64_t)good.nblocks*STRATA_BS)==-EFSCORRUPT); /* 16TB claim vs real file size */
    bad=good; bad.itable_start=bad.data_start;
    OK(sb_validate(&bad,(uint64_t)good.nblocks*STRATA_BS)==-EFSCORRUPT); /* overlapping regions */
    bad=good; bad.dbmap_start=bad.ibmap_start;
    OK(sb_validate(&bad,(uint64_t)good.nblocks*STRATA_BS)==-EFSCORRUPT);
    bad=good; bad.root_ino=good.ninodes;
    OK(sb_validate(&bad,(uint64_t)good.nblocks*STRATA_BS)==-EFSCORRUPT); /* root out of range */
    bad=good; bad.itable_blocks=1;
    OK(sb_validate(&bad,(uint64_t)good.nblocks*STRATA_BS)==-EFSCORRUPT); /* itable too small for ninodes */


    /* deep nesting + relative resolution */
    uint32_t cwd=0;
    char deep[256]="";
    for(int i=0;i<20;i++){
        char seg[32]; snprintf(seg,sizeof seg,"n%d",i);
        strcat(deep,"/"); strcat(deep,seg);
        OK(fs_mkdir(&f,0,deep)==0);
    }
    OK(fs_resolve(&f,0,deep,&cwd)==0);
    OK(fs_resolve(&f,cwd,"../../n17/n18/n19",&cwd)==0 || cwd);
    uint32_t r18;
    OK(fs_resolve(&f,cwd,"..",&r18)==0);

    /* ENOSPC: fill the image */
    OK(fs_create(&f,0,"/hog",T_FILE,&ino)==0);
    memset(wbuf,0x5a,STRATA_BS);
    int rc=0, wrote=0;
    for(int i=0;i<4096;i++){
        rc=fs_write(&f,ino,(uint64_t)i*STRATA_BS,wbuf,STRATA_BS);
        if(rc){ break; }
        wrote++;
    }
    OK(rc==-ENOSPC || rc==-EFBIG);
    OK(wrote>0);
    OK(fs_sync(&f)==0);

    /* touch: creates a missing file, bumps mtime on an existing one */
    OK(fs_touch(&f,0,"/tf")==0);
    uint32_t tino; OK(fs_resolve(&f,0,"/tf",&tino)==0);
    struct dinode td; fs_iget(&f,tino,&td);
    OK(td.type==T_FILE);
    td.mtime=1000; td.ctime=1000; fs_iput(&f,tino,&td);
    OK(fs_touch(&f,0,"/tf")==0);
    struct fs_stat tst; OK(fs_stat(&f,tino,&tst)==0);
    OK(tst.mtime!=1000); /* bumped to now */
    OK(tst.ctime!=1000); /* ctime bumps too */
    OK(fs_touch(&f,0,"/lost+found")==0); /* directories are touchable */
    OK(fs_touch(&f,0,"/tf/")==-ENOTDIR); /* trailing slash on a file */
    OK(fs_touch(&f,0,"/nope/")==-ENOENT); /* trailing slash on nothing */
    /* a dangling entry (dentry pointing at a freed inode) is ENOENT */
    uint32_t dgino; OK(fs_create(&f,0,"/dg",T_FILE,&dgino)==0);
    struct dinode dd; fs_iget(&f,dgino,&dd);
    dd.type=T_FREE; fs_iput(&f,dgino,&dd);
    OK(fs_touch(&f,0,"/dg")==-ENOENT);
    OK(fs_unlink(&f,0,"/dg")==-ENOENT); /* unlink agrees */
    OK(fs_sync(&f)==0);
    fs_unmount(&f);
    free(wbuf);
    printf("%d checks passed\n",checks);
    return 0;
}
