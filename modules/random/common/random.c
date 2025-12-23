#include "math/rng.h"
#include "random/random.h"
#include "dev/driver_base.h"
#include "console/kio.h"

rng_t global_rng;
uint64_t rng_fid = 0;

FS_RESULT rng_open(const char *path, file *fd){
    fd->id = rng_fid++;
    fd->size = 0;
    return FS_RESULT_SUCCESS;
}

size_t rng_read(file *fd, char* buf, size_t count, file_offset offset){
    rng_fill_buf(&global_rng, (void*)buf, count);
    return count;
}

void rng_init_random(rng_t *rng){
    uint64_t seed;
    asm volatile("mrs %0, cntvct_el0" : "=r"(seed));
    rng_seed(rng, seed);
}

bool rng_init_global() {
    rng_init_random(&global_rng);
    return true;
}

system_module rng_module = {
    .name = "random",
    .mount = "/random",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = rng_init_global,
    .open = rng_open,
    .close = 0,
    .fini = 0,
    .read = rng_read,
    .write = 0,
    .sread = 0,
    .swrite = 0,//TODO implement simple io
    .readdir = 0,
};
