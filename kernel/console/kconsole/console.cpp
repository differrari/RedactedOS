#include "kconsole.hpp"
#include "kconsole.h"
#include "graph/graphics.h"

KernelConsole kconsole;

extern "C" void kconsole_putc(char c) {
    kconsole.put_char(c);
    gpu_flush();
}

extern "C" void kconsole_puts(const char *s) {
    kconsole.put_string(s);
}

extern "C" void kconsole_clear() {
    kconsole.clear();
}

extern "C" void kconsole_refresh(){
    kconsole.refresh_input();
    gpu_flush();
}