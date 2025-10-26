#include "package_info.h"
#include "std/string.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"

package_info parse_package_info(char *info, size_t size){
    const char *path = info;
    package_info pkg_info = {};
    do {
        const char *nl = seek_to(path, '\n');
        string line = string_from_literal_length(path, nl-path-(nl > path && *(nl-1) == '\n'));
        const char *prop = seek_to(line.data, '=');
        string prop_name = string_from_literal_length(line.data, prop-line.data-(prop > line.data && *(prop-1) == '='));
        if (*prop == '\"') prop++;
        size_t len = strlen(prop,0);
        if (*(prop + len - 1) == '\"') len--;
        if (strcmp("app_name", prop_name.data, true) == 0)
            pkg_info.name = string_from_literal_length(prop, len);
        if (strcmp("app_author", prop_name.data, true) == 0)
            pkg_info.author = string_from_literal_length(prop, len);
        if (strcmp("app_version", prop_name.data, true) == 0)
            pkg_info.version = string_from_literal_length(prop, len);
        free(line.data, line.mem_length);
        free(prop_name.data, prop_name.mem_length);
        path = nl;
    } while (*path);
    return pkg_info;
}