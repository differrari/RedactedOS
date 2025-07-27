#pragma once

class FSDriver {
public:
    virtual bool init(uint32_t partition_sector) = 0;
    virtual void* read_file(char *path) = 0;
    virtual string_list* list_contents(char *path) = 0;
};