#include "kconsole.hpp"
#include "kconsole.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "memory/page_allocator.h"
#include "std/allocator.hpp"

KernelConsole *kconsole;
void kconsole_init(){
    kconsole = new KernelConsole();
}

extern "C" void kconsole_putc(char c) {
    if (!kconsole) kconsole_init();
    kconsole->put_char(c);
    gpu_flush();
}

extern "C" void kconsole_puts(const char *s) {
    if (!kconsole) kconsole_init();
    kconsole->put_string(s);
}

extern "C" void kconsole_clear() {
    if (!kconsole) kconsole_init();
    kconsole->clear();
}