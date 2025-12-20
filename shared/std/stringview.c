#include "stringview.h"
#include "math/math.h"

stringview delimited_stringview(const char* buf, size_t start, size_t length){
    if (strlen(buf) <= start) return (stringview){};
    length = min(strlen(buf+start),length);
    return (stringview){.data = (char*)buf + start, .length = length };
}
