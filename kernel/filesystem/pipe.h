#pragma once

#include "types.h"
#include "files/fs.h"
#include "files/system_module.h"
#include "files/module_loader.h"
#include "files/pipes.h"

typedef struct {
    file write_fd;
    file read_fd;
    uint16_t pid;
    system_module* write_mod;
} pipe_t;

#ifdef __cplusplus
extern "C" {
#endif
FS_RESULT create_pipe(module_root *src_root, const char *source, module_root *dst_root, const char* destination, pipe_options options, file *out_fd);
FS_RESULT close_pipe(file *fd);

void update_pipes(uint64_t mfid, const char *buf, size_t size);
void close_pipes_for_process(uint16_t pid);

#ifdef __cplusplus
}
#endif