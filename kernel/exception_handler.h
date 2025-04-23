#pragma once

#include "types.h"

void set_exception_vectors();
void sync_el1_handler();
void irq_el1_handler();
void fiq_el1_handler();
void error_el1_handler();
void panic(const char* msg);
void panic_with_info(const char* msg, uint64_t info);