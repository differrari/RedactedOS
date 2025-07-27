#pragma once

#include "std/string.h"
#include "types.h"

class FSDriver {
public:
    virtual bool init(uint32_t partition_sector) = 0;
    virtual void* read_file(char *path, size_t size) = 0;
    virtual string_list* list_contents(char *path) = 0;
};