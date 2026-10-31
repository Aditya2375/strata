/* emscripten wrapper: paste the CLI into this translation unit and expose
 * a tiny session API to JavaScript. The image lives in MEMFS. */
#define main strata_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
int strata_boot(void){
    static int booted=0;
    if(booted) fs_unmount(&F);   /* reset: release the previous image's memory */
    booted=1;
    unlink("/fs.img");
    if(mkfs("/fs.img",1024,256,NULL)) return 1;
    IMG_PATH="/fs.img";
    if(fs_mount(&F,IMG_PATH)) return 2;
    CWD=F.sb.root_ino;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void strata_exec(const char *line){
    char buf[4096];
    strncpy(buf,line,sizeof buf-1); buf[sizeof buf-1]=0;
    /* crash calls _exit and exit ends the REPL: in the browser both kill the
     * emscripten runtime and wedge the session, so refuse them instead */
    const char *p=buf; while(*p==' '||*p=='\t') p++;
    char w0[16]={0}; size_t n=0;
    while(p[n]&&p[n]!=' '&&p[n]!='\t'&&p[n]!='\n'&&n<sizeof w0-1){ w0[n]=p[n]; n++; }
    if(!strcmp(w0,"crash")||!strcmp(w0,"exit")||!strcmp(w0,"quit")){
        printf("that command ends the session and is disabled in the browser demo\n");
        fflush(stdout);
        return;
    }
    one_cmd(buf);
    fflush(stdout);
    fs_sync(&F);
}
