#pragma once

#include "types.h"

typedef void (*toml_handler)(const char *key, char *value, size_t value_len, void *context);

void read_toml(char *info, size_t size, toml_handler on_kvp, void *context);