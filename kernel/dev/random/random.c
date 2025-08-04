#include "math/rng.h"
#include "random.h"
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

bool rng_init_global() {
    uint64_t seed;
    asm volatile("mrs %0, cntvct_el0" : "=r"(seed));
    rng_seed(&global_rng, seed);
    kprintf("[RNG] Random init. seed: %i", seed);
    return true;
}

driver_module rng_module = {
    .name = "random",
    .mount = "random",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = rng_init_global,
    .open = rng_open,
    .fini = 0,
    .read = rng_read,
    .write = 0,
    .seek = 0,
    .readdir = 0,
};