#pragma once
#include "string.h"

typedef struct {
    char *data;
    size_t length;
} stringview;

stringview delimited_stringview(const char* buf, size_t start, size_t length);