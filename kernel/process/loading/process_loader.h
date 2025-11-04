#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "types.h"
#include "process/process.h"

process_t* create_process(const char *name, const char *bundle, sizedptr text, uintptr_t text_va, sizedptr data, uintptr_t data_va, sizedptr rodata, uintptr_t rodata_va, sizedptr bss, uintptr_t bss_va, uintptr_t entry);
void translate_enable_verbose();
void decode_instruction(uint32_t instruction);
#ifdef __cplusplus
}
#endif