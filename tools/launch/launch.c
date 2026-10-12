#include "syscalls/syscalls.h"
#include "memory/memory.h"

int main(int argc, const char* argv[]){

    if (argc < 2){
        print("Usage: launch bundle.id");
        return -1;
    }

    const char* bundle_id = argv[1];
    size_t bsize = strlen(bundle_id)+1;

    if (bsize > 64){
        print("Bundle id too large");
        return -1;
    } 

    char *buf = zalloc(512);
    memcpy(buf, bundle_id, bsize);

    transformf("/apps/resolve", buf, 512);

    print("Resolved id to %s",buf);

    fs_stat out_stat = {};
    if (statf(buf, &out_stat)){
        exec(buf, 0, 0, EXEC_MODE_DEFAULT);
        return 0;
    }

    return -1;
}
