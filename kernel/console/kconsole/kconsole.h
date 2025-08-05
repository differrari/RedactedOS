#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void kconsole_putc(char c);
void kconsole_puts(const char *s);
void kconsole_clear();

#ifdef __cplusplus
}
#endif
