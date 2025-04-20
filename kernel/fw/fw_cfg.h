#pragma once

#include "types.h"
#include "string.h"

struct fw_cfg_file {
    uint32_t size;
    uint16_t selector;
    uint16_t reserved;
    char name[56];
}__attribute__((packed));

bool fw_cfg_check();
bool fw_find_file(string search, struct fw_cfg_file *file);
void fw_cfg_dma_write(void* dest, uint32_t size, uint32_t ctrl);