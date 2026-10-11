#pragma once

#include "types.h"
#include "files/system_module.h"

typedef struct process_t process_t;

typedef union {
    struct {
        u64 type: 8;
        u64 id: 56;
    };
    u64 resource;
} resource;

typedef enum {
    resource_invalid,
    resource_process,
    resource_file,
} resource_types;

resource resource_map_process(process_t *p);
resource resource_map_file(system_module *mod, file *fd);