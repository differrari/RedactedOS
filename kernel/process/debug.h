#pragma once

#include "process.h"

void debug_load();//TODO: This doesn't belong here

void backtrace(uptr *ttbr, uintptr_t fp, uintptr_t elr, sizedptr debug_line, sizedptr debug_line_str);
void debug_snapshot(process_t *proc, thread_t *t);