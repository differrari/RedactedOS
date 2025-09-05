#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
void panic(const char* msg, uint64_t info);
#ifdef __cplusplus
}
#endif

void set_exception_vectors();
void fiq_el1_handler();
void error_el1_handler();
void handle_exception(const char* type, uint64_t info);