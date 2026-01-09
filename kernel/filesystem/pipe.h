#pragma once

#include "types.h"
#include "files/fs.h"
#include "dev/driver_base.h"

typedef struct {
    file write_fd;
    file read_fd;
    uint16_t pid;
    system_module* write_mod;
} pipe_t;

typedef enum {
    PIPE_DEFAULT = 0,//Any writes to the source while the pipe is open get copied to the destination
    PIPE_FROM_BEGINNING = 1,//Read the source file when creating the pipe and copy its contents 
} PIPE_OPTIONS;

#ifdef __cplusplus
extern "C" {
#endif
FS_RESULT create_pipe(const char *source, const char* destination, PIPE_OPTIONS options, file *out_fd);
FS_RESULT close_pipe(file *fd);

void update_pipes(uint64_t mfid, const char *buf, size_t size);
void close_pipes_for_process(uint16_t pid);

#ifdef __cplusplus
}
#endif