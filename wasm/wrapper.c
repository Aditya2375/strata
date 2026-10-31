/* emscripten wrapper: paste the CLI into this translation unit and expose
 * a tiny session API to JavaScript. The image lives in MEMFS. */
#define main strata_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
int strata_boot(void){
    unlink("/fs.img");
    if(mkfs("/fs.img",1024,256)) return 1;
    IMG_PATH="/fs.img";
    if(fs_mount(&F,IMG_PATH)) return 2;
    CWD=F.sb.root_ino;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void strata_exec(const char *line){
    char buf[4096];
    strncpy(buf,line,sizeof buf-1); buf[sizeof buf-1]=0;
    one_cmd(buf);
    fs_sync(&F);
}
