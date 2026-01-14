#pragma once

#include "types.h"

typedef void (*dir_traverse)(const char *directory, const char* name);

void traverse_directory(char *directory, bool recursive, dir_traverse func);

char* get_current_dir();

char* gethomedir();