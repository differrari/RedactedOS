#pragma once
#include "string.h"

typedef struct {
    char *data;
    size_t length;
} string_slice;

string_slice make_string_slice(const char* buf, size_t start, size_t length);
static inline string string_from_slice(string_slice slice){
    return string_from_literal_length(slice.data, slice.length);
}