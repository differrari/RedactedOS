#pragma once

#include "types.h"
#include "std/stringview.h"

typedef void (*toml_handler)(stringview key, stringview value, void *context);

void read_toml(char *info, toml_handler on_kvp, void *context);