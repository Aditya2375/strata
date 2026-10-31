/* strata shell + CLI. Usage:
 *   strata format IMG [nblocks] [ninodes]
 *   strata fsck IMG [--fix]
 *   strata shell IMG      (default when given just IMG)
 * REPL commands: ls cd pwd mkdir touch write append cat rm rmdir ln mv
 *                stat df fsck sync crash [K] help exit */
#include "fs.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define LINE 4096
static struct fs F;
static uint32_t CWD;
static const char *IMG_PATH;

static const char *ename(int e){
    switch(e){
    case ENOENT: return "no such file or directory";
    case EEXIST: return "already exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case ENOTEMPTY: return "directory not empty";
    case ENOSPC: return "no space left on device";
    case EFBIG: return "file too large";
    case ENAMETOOLONG: return "name too long";
    case EBUSY: return "device or resource busy";
    case EPERM: return "operation not permitted";
    case EINVAL: return "invalid argument";
    case EFSCORRUPT: return "filesystem corrupted";
    default: return "error";
    }
}

static void path_of(struct fs *f, uint32_t ino, char *out, size_t cap){
    /* reconstruct path by walking up via ".." */
    if(ino==f->sb.root_ino){ snprintf(out,cap,"/"); return; }
    char parts[64][STRATA_NAMEMAX+1]; int n=0;
    uint32_t cur=ino;
    while(cur!=f->sb.root_ino && n<64){
        struct dinode d; fs_iget(f,cur,&d);
        if(d.type!=T_DIR) break;
        struct dentry ents[256]; uint32_t cnt=0;
        if(fs_readdir(f,cur,ents,256,&cnt,NULL)) break;
        uint32_t par=UINT32_MAX;
        for(uint32_t i=0;i<cnt;i++) if(!strcmp(ents[i].name,"..")) par=ents[i].ino;
        if(par==UINT32_MAX) break;
        struct dentry pe[512]; uint32_t pc=0;
        if(fs_readdir(f,par,pe,512,&pc,NULL)) break;
        int found=0;
        for(uint32_t i=0;i<pc;i++)
            if(pe[i].ino==cur && strcmp(pe[i].name,".") && strcmp(pe[i].name,"..")){
                strncpy(parts[n++],pe[i].name,STRATA_NAMEMAX); found=1; break;
            }
        if(!found) break;
        cur=par;
    }
    if(cur!=f->sb.root_ino){ snprintf(out,cap,"?"); return; }
    size_t used=0; out[0]=0;
    for(int i=n-1;i>=0;i--){
        used+=snprintf(out+used,cap-used,"/%s",parts[i]);
        if(used>=cap) break;
    }
}

static int cmd_ls(char *arg){
    int longfmt=0;
    char *path=NULL;
    if(arg){
        char *save=NULL;
        for(char *t=strtok_r(arg," \t",&save);t;t=strtok_r(NULL," \t",&save)){
            if(!strcmp(t,"-l")) longfmt=1; else path=t;
        }
    }
    uint32_t ino;
    int r=path?fs_resolve(&F,CWD,path,&ino):0;
    if(!path) ino=CWD;
    if(r){ printf("ls: %s: %s\n",path,ename(-r)); return 1; }
    struct fs_stat st;
    if(fs_stat(&F,ino,&st)){ printf("ls: %s: %s\n",path?path:".",ename(ENOENT)); return 1; }
    if(st.type==T_FILE){
        if(longfmt) printf("file %8llu bytes  links=%u  %s\n",(unsigned long long)st.size,st.nlink,path?path:".");
        else printf("%s\n",path?(strrchr(path,'/')?strrchr(path,'/')+1:path):".");
        return 0;
    }
    struct dentry *ents=malloc(sizeof(struct dentry)*4096); uint32_t cnt; int trunc=0;
    int rr=fs_readdir(&F,ino,ents,4096,&cnt,&trunc);
    if(rr){ free(ents); printf("ls: %s: %s\n",path?path:".",ename(-rr)); return 1; }
    if(trunc) printf("ls: (listing truncated at %u entries)\n",cnt);
    /* sort by name for stable output */
    for(uint32_t i=1;i<cnt;i++){
        struct dentry k=ents[i]; int j=(int)i-1;
        while(j>=0 && strcmp(ents[j].name,k.name)>0){ ents[j+1]=ents[j]; j--; }
        ents[j+1]=k;
    }
    for(uint32_t i=0;i<cnt;i++){
        if(!longfmt && (!strcmp(ents[i].name,".")||!strcmp(ents[i].name,".."))) continue;
        if(longfmt){
            struct fs_stat es;
            if(fs_stat(&F,ents[i].ino,&es)){
                printf("??? %8s  links=%-3s %s%s [dangling]\n", "-", "-",
                       ents[i].name, ents[i].type==T_DIR?"/":"");
                continue;
            }
            printf("%s %8llu  links=%-3u %s%s\n",
                   ents[i].type==T_DIR?"dir ":"file",
                   (unsigned long long)es.size, es.nlink,
                   ents[i].name, ents[i].type==T_DIR?"/":"");
        } else printf("%s%s\n",ents[i].name,ents[i].type==T_DIR?"/":"");
    }
    free(ents);
    return 0;
}

static int one_cmd(char *line){
    line[strcspn(line,"\n")]=0;
    char *save=NULL;
    char *cmd=strtok_r(line," \t",&save);
    if(!cmd) return 0;
    char *rest=save;
    if(rest) rest+=strspn(rest," \t");
    if(!strcmp(cmd,"dump")){
        /* deterministic full-tree dump: "F path size fnv1a64" / "D path" */
        char stack[512][1152]; uint32_t istack[512]; int top=0;
        /* a corrupt image can turn the tree into a cyclic graph: never
         * descend into a directory inode twice */
        uint8_t *dseen=calloc(F.sb.ninodes,1);
        strcpy(stack[0],"/"); istack[0]=F.sb.root_ino;
        if(dseen) dseen[F.sb.root_ino]=1;
        while(top>=0){
            char dir[1152]; strcpy(dir,stack[top]);
            uint32_t ino=istack[top--];
            struct dentry *ents=malloc(sizeof(struct dentry)*4096); uint32_t cnt; int trunc=0;
            if(fs_readdir(&F,ino,ents,4096,&cnt,&trunc)){ free(ents); continue; }
            if(trunc) fprintf(stderr,"dump: %s: listing truncated at 4096 entries\n",dir);
            for(uint32_t i=1;i<cnt;i++){ struct dentry k=ents[i]; int j=(int)i-1; while(j>=0&&strcmp(ents[j].name,k.name)>0){ents[j+1]=ents[j];j--;} ents[j+1]=k; }
            for(uint32_t i=0;i<cnt;i++){
                if(!strcmp(ents[i].name,".")||!strcmp(ents[i].name,"..")) continue;
                char p[1152];
                size_t dl=strlen(dir);
                size_t nl=strlen(ents[i].name);
                if(dl+(dir[1]?1:0)+nl+1>sizeof p){
                    /* the stack bounds depth, but path LENGTH overflows
                     * first: never copy a path that does not fit whole */
                    fprintf(stderr,"dump: %s%s%s: path too long, entry skipped\n",
                            dir,dir[1]?"/":"",ents[i].name);
                    continue;
                }
                memcpy(p,dir,dl);
                if(dir[1]) p[dl++]='/';
                memcpy(p+dl,ents[i].name,nl+1);
                if(ents[i].type==T_DIR){
                    printf("D %s\n",p);
                    if(ents[i].ino<F.sb.ninodes && dseen && dseen[ents[i].ino])
                        fprintf(stderr,"dump: %s: directory cycle, subtree skipped\n",p);
                    else if(top<510){ strcpy(stack[++top],p); istack[top]=ents[i].ino; if(dseen&&ents[i].ino<F.sb.ninodes) dseen[ents[i].ino]=1; }
                    else fprintf(stderr,"dump: %s: tree too deep, subtree skipped\n",p);
                } else {
                    struct fs_stat st;
                    if(fs_stat(&F,ents[i].ino,&st)){
                        fprintf(stderr,"dump: %s: dangling entry, skipped\n",p);
                        continue;
                    }
                    uint64_t h=1469598103934665603ULL;
                    char buf[STRATA_BS]; uint64_t off=0,got;
                    while(fs_read(&F,ents[i].ino,off,buf,sizeof buf,&got)==0&&got){
                        for(uint64_t k=0;k<got;k++){ h^=(uint8_t)buf[k]; h*=1099511628211ULL; }
                        off+=got;
                    }
                    printf("F %s %llu %016llx\n",p,(unsigned long long)st.size,(unsigned long long)h);
                }
            }
            free(ents);
        }
        free(dseen);
        return 0;
    }
    if(!strcmp(cmd,"help")){
        printf("ls [-l] [p] cd p pwd mkdir p touch p write p TEXT append p TEXT\n"
               "cat p rm p rmdir p ln a b mv a b stat p df fsck sync crash [K] exit\n");
        return 0;
    }
    if(!strcmp(cmd,"exit")||!strcmp(cmd,"quit")) return -1;
    if(!strcmp(cmd,"ls")) return cmd_ls(rest);
    if(!strcmp(cmd,"pwd")){ char p[4096]; path_of(&F,CWD,p,sizeof p); printf("%s\n",p); return 0; }
    if(!strcmp(cmd,"cd")){
        if(!rest||!*rest){ CWD=F.sb.root_ino; return 0; }
        uint32_t ino; int r=fs_resolve(&F,CWD,rest,&ino);
        if(r){ printf("cd: %s: %s\n",rest,ename(-r)); return 1; }
        struct fs_stat st;
        if(fs_stat(&F,ino,&st)){ printf("cd: %s: no such file or directory\n",rest); return 1; }
        if(st.type!=T_DIR){ printf("cd: %s: not a directory\n",rest); return 1; }
        CWD=ino; return 0;
    }
    if(!strcmp(cmd,"mkdir")||!strcmp(cmd,"touch")||!strcmp(cmd,"rm")||!strcmp(cmd,"rmdir")||!strcmp(cmd,"cat")||!strcmp(cmd,"stat")){
        if(!rest||!*rest){ printf("%s: missing operand\n",cmd); return 1; }
        char *nl=strchr(rest,'\n'); if(nl) *nl=0;
        if(!strcmp(cmd,"mkdir")){ int r=fs_mkdir(&F,CWD,rest); if(r) printf("mkdir: %s: %s\n",rest,ename(-r)); return r?1:0; }
        if(!strcmp(cmd,"touch")){ int r=fs_create(&F,CWD,rest,T_FILE,NULL); if(r&&r!=-EEXIST){ printf("touch: %s: %s\n",rest,ename(-r)); return 1; } return 0; }
        if(!strcmp(cmd,"rm")){ int r=fs_unlink(&F,CWD,rest); if(r) printf("rm: %s: %s\n",rest,ename(-r)); return r?1:0; }
        if(!strcmp(cmd,"rmdir")){ int r=fs_rmdir(&F,CWD,rest); if(r) printf("rmdir: %s: %s\n",rest,ename(-r)); return r?1:0; }
        if(!strcmp(cmd,"cat")){
            uint32_t ino; int r=fs_resolve(&F,CWD,rest,&ino);
            if(r){ printf("cat: %s: %s\n",rest,ename(-r)); return 1; }
            struct fs_stat st;
            if(fs_stat(&F,ino,&st)){ printf("cat: %s: no such file or directory\n",rest); return 1; }
            if(st.type==T_DIR){ printf("cat: %s: is a directory\n",rest); return 1; }
            char buf[STRATA_BS]; uint64_t off=0, got;
            int last=-1;
            while(fs_read(&F,ino,off,buf,sizeof buf,&got)==0 && got){ fwrite(buf,1,got,stdout); last=(unsigned char)buf[got-1]; off+=got; }
            if(off && last!='\n') putchar('\n');
            if(st.size && off && (off==st.size)) fflush(stdout);
            return 0;
        }
        if(!strcmp(cmd,"stat")){
            uint32_t ino; int r=fs_resolve(&F,CWD,rest,&ino);
            if(r){ printf("stat: %s: %s\n",rest,ename(-r)); return 1; }
            struct fs_stat st;
            if((r=fs_stat(&F,ino,&st))){ printf("stat: %s: %s\n",rest,ename(-r)); return 1; }
            printf("type=%s size=%llu blocks=%u links=%u mode=%o\n",
                   st.type==T_DIR?"dir":"file",(unsigned long long)st.size,
                   st.blocks,st.nlink,st.mode);
            return 0;
        }
    }
    if(!strcmp(cmd,"write")||!strcmp(cmd,"append")){
        if(!rest){ printf("%s: missing operand\n",cmd); return 1; }
        char *sp=strchr(rest,' ');
        if(!sp){ printf("%s: usage: %s FILE TEXT\n",cmd,cmd); return 1; }
        *sp=0; char *text=sp+1;
        char *nl=strchr(text,'\n'); if(nl) *nl=0;
        uint32_t ino; int r=fs_resolve(&F,CWD,rest,&ino);
        if(r==-ENOENT){ r=fs_create(&F,CWD,rest,T_FILE,&ino); }
        if(r){ printf("%s: %s: %s\n",cmd,rest,ename(-r)); return 1; }
        struct fs_stat st; fs_stat(&F,ino,&st);
        if(st.type==T_DIR){ printf("%s: %s: is a directory\n",cmd,rest); return 1; }
        uint64_t off = !strcmp(cmd,"append") ? st.size : 0;
        r=fs_write(&F,ino,off,text,strlen(text));
        if(r){ printf("%s: %s: %s\n",cmd,rest,ename(-r)); return 1; }
        if(!strcmp(cmd,"write")) fs_truncate(&F,ino,off+strlen(text));
        return 0;
    }
    if(!strcmp(cmd,"ln")||!strcmp(cmd,"mv")){
        if(!rest){ printf("%s: missing operands\n",cmd); return 1; }
        char *sp=strchr(rest,' ');
        if(!sp){ printf("%s: usage: %s A B\n",cmd,cmd); return 1; }
        *sp=0; char *b=sp+1; while(*b==' ') b++;
        char *nl=strchr(b,'\n'); if(nl) *nl=0;
        int r = !strcmp(cmd,"ln") ? fs_link(&F,CWD,rest,b) : fs_rename(&F,CWD,rest,b);
        if(r) printf("%s: %s\n",cmd,ename(-r));
        return r?1:0;
    }
    if(!strcmp(cmd,"df")){
        printf("blocks: %u total, %u free   inodes: %u total, %u free\n",
               F.sb.nblocks,F.sb.free_blocks,F.sb.ninodes,F.sb.free_inodes);
        return 0;
    }
    if(!strcmp(cmd,"sync")){ int r=fs_sync(&F); printf(r?"sync: io error\n":"synced\n"); return r?1:0; }
    if(!strcmp(cmd,"fsck")){
        fs_sync(&F);
        int r=fsck(IMG_PATH,1,stdout);
        /* repairs may have changed blocks under our cache; remount */
        fs_unmount(&F);
        if(fs_mount(&F,IMG_PATH)){ printf("fsck: remount failed\n"); return 1; }
        CWD=F.sb.root_ino;
        return r;
    }
    if(!strcmp(cmd,"crash")){
        F.crash_armed=1;
        F.crash_flush = rest&&*rest ? (uint32_t)atoi(rest) : 0;
        fs_sync(&F);
        _exit(2);
    }
    printf("unknown command: %s (try help)\n",cmd);
    return 1;
}

int main(int argc, char **argv){
    if(argc>=3 && !strcmp(argv[1],"format")){
        uint32_t nb = argc>3 ? (uint32_t)atoi(argv[3]) : 2048;
        uint32_t ni = argc>4 ? (uint32_t)atoi(argv[4]) : 1024;
        uint32_t real_ni=0;
        if(mkfs(argv[2],nb,ni,&real_ni)){ fprintf(stderr,"strata: format failed\n"); return 1; }
        printf("formatted %s: %u blocks (%u KiB), %u inodes\n",argv[2],nb,nb*4/1,real_ni);
        return 0;
    }
    if(argc>=3 && !strcmp(argv[1],"fsck")){
        int fix = argc>3 && !strcmp(argv[3],"--fix");
        return fsck(argv[2],fix,stdout);
    }
    const char *img = argc>=3 && !strcmp(argv[1],"shell") ? argv[2] : argc>=2 ? argv[1] : NULL;
    if(!img){ fprintf(stderr,"usage: strata format|fsck|shell IMG ...\n"); return 1; }
    IMG_PATH=img;
    int r=fs_mount(&F,img);
    if(r){ fprintf(stderr,"strata: cannot mount %s: %s\n",img,ename(-r)); return 1; }
    { struct fs_stat rst;
      if(fs_stat(&F,F.sb.root_ino,&rst) || rst.type!=T_DIR){
        fprintf(stderr,"strata: root inode is not a directory - run fsck, image needs repair\n");
        fs_unmount(&F); return 1;
      } }
    CWD=F.sb.root_ino;
    char line[LINE];
    int interactive=isatty(0);
    while(1){
        if(interactive){ char p[1024]; path_of(&F,CWD,p,sizeof p); printf("strata:%s> ",p); fflush(stdout); }
        if(!fgets(line,sizeof line,stdin)) break;
        if(one_cmd(line)==-1) break;
    }
    fs_unmount(&F);
    return 0;
}
