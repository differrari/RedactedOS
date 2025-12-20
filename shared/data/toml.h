#pragma once

#include "types.h"
#include "std/string_slice.h"

typedef void (*toml_handler)(string_slice key, string_slice value, void *context);

void read_toml(char *info, toml_handler on_kvp, void *context);