#pragma once

#include "types.h"
#include "std/string.h"

typedef struct {
    bool valid;
    string name;
    string author;
    string version;
} package_info;

#ifdef __cplusplus
extern "C" {
#endif
package_info parse_package_info(char *info, size_t size);
#ifdef __cplusplus
}
#endif