#pragma once
#include "string.h"

typedef struct {
    char *data;
    size_t length;
} string_slice;

string_slice make_string_slice(const char* buf, size_t start, size_t length);
static inline string_slice slice_from_lit(const char* buf){
    return make_string_slice(buf, 0, strlen(buf));
}
static inline string string_from_slice(string_slice slice){
    return string_from_literal_length(slice.data, slice.length);
}

static inline bool slices_equal(string_slice sl1, string_slice sl2, bool case_insensitive){
    if (sl1.length != sl2.length) return false;
    for (size_t i = 0; i < sl1.length; i++)
        if (sl1.data[i] != sl2.data[i] && (!case_insensitive || tolower(sl1.data[i]) != tolower(sl2.data[i]))) return false;
    return true;
}

static inline bool slice_lit_match(string_slice sl, const char *lit, bool case_insensitive){
    return slices_equal(sl, make_string_slice(lit,0,strlen(lit)), case_insensitive);
}

