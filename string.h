#pragma once

#include "types.h"

typedef struct {
    char *data;
    uint32_t length;
} string;

string string_l(const char *literal);
string string_c(const char *array, uint32_t max_length);
bool string_equals(string a, string b);