#pragma once

#include "process.h"
#include "debug/inspect.h"

void debug_load();//TODO: This doesn't belong here

void backtrace(uptr *ttbr, uintptr_t fp, uintptr_t elr, sizedptr debug_line, sizedptr debug_line_str);
void debug_snapshot(process_t *proc, thread_t *t);

bool set_inspect(debug_inspect_types types, process_t *inspector, thread_t *inspector_thread, u16 inspected_pid, u16 inspected_tid);