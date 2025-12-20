#include "string_slice.h"
#include "math/math.h"

string_slice make_string_slice(const char* buf, size_t start, size_t length){
    if (strlen(buf) <= start) return (string_slice){};
    length = min(strlen(buf+start),length);
    return (string_slice){.data = (char*)buf + start, .length = length };
}
