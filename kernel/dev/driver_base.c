#include "driver_base.h"
#include "data_struct/hashmap.h"
#include "std/string.h"

uint64_t fd_id = 256;//First byte reserved

uint64_t reserve_fd_id(){
    return ++fd_id;
}

uint64_t reserve_fd_gid(const char *path){
    return chashmap_fnv1a64(path, strlen(path));
}
