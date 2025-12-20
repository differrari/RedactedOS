#pragma once
#include "string.h"

typedef struct {
    char *data;
    size_t length;
} string_slice;

string_slice make_string_slice(const char* buf, size_t start, size_t length);