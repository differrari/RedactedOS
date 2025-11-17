#include "package_info.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "console/kio.h"
#include "config/toml.h"

static inline void handle_package_kvp(const char *key, char *value, size_t value_len, void* ctx){
    package_info *pkg_info = (package_info*)ctx;
    if (strstart("app_name", key, true) == 8) 
        pkg_info->name = string_from_literal_length(value, value_len);
    if (strstart("app_author", key, true) == 10)
        pkg_info->author = string_from_literal_length(value, value_len);
    if (strstart("app_version", key, true) == 11)
        pkg_info->version = string_from_literal_length(value, value_len);
}

package_info parse_package_info(char *info, size_t size){
    package_info pkg_info = {};
    read_toml(info, size, handle_package_kvp, (void*)&pkg_info);
    return pkg_info;
}