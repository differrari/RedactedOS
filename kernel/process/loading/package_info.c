#include "package_info.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

package_info parse_package_info(char *info, size_t size){
    const char *path = info;
    package_info pkg_info = {};
    do {
        const char *nl = seek_to(path, '\n');
        string line = string_from_literal_length(path, nl-path-(nl > path && *(nl-1) == '\n'));
        char *prop = (char*)seek_to(line.data, '=');
        string prop_name = string_from_literal_length(line.data, prop-line.data-(prop > line.data && *(prop-1) == '='));
        while (*prop <= ' ' || *prop > '~' || *prop == '\"') prop++;
        size_t len = strlen(prop,0);
        while (*(prop + len - 1) <= ' ' || *(prop + len - 1) > '~' || *(prop + len - 1) == '\"') len--;
        size_t name_offset = 0;
        while (prop_name.data[name_offset] <= ' ' || prop_name.data[name_offset] > '~' || prop_name.data[name_offset] == '\"') name_offset++;
        if (strstart("app_name", prop_name.data + name_offset, true) == 8)
            pkg_info.name = string_from_literal_length(prop, len);
        if (strstart("app_author", prop_name.data + name_offset, true) == 10)
            pkg_info.author = string_from_literal_length(prop, len);
        if (strstart("app_version", prop_name.data + name_offset, true) == 11)
            pkg_info.version = string_from_literal_length(prop, len);
        free(line.data, line.mem_length);
        free(prop_name.data, prop_name.mem_length);
        path = nl;
    } while (*path);
    return pkg_info;
}