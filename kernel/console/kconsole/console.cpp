#include "kconsole.hpp"
#include "kconsole.h"

KernelConsole kconsole;

extern "C" void kconsole_putc(char c) {
    kconsole.put_char(c);
}

extern "C" void kconsole_puts(const char *s) {
    kconsole.put_string(s);
}

extern "C" void kconsole_clear() {
    kconsole.clear();
}