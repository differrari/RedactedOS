#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "types.h"
#include "process/process.h"

typedef struct {
    sizedptr file_cpy;
    sizedptr virt_mem;
    uint8_t permissions;
} program_load_data;

process_t* create_process(const char *name, const char *bundle, program_load_data *data, size_t data_count, uintptr_t entry);
void translate_enable_verbose();
void decode_instruction(uint32_t instruction);
#ifdef __cplusplus
}
#endif