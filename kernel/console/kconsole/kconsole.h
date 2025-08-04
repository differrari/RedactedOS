#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void kconsole_putc(char c);
void kconsole_puts(const char *s);
void kconsole_clear();
void kconsole_refresh();

#ifdef __cplusplus
}
#endif
