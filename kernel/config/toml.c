#include "toml.h"
#include "std/string.h"

void read_toml(char *info, size_t size, toml_handler on_kvp, void *context){
    const char *path = info;
    do {
        const char *nl = seek_to(path, '\n');
        size_t line_len = nl-path-(nl > path && *(nl-1) == '\n');
        if (line_len){
            string line = string_from_literal_length(path, line_len);
            char *comm = (char*)seek_to(line.data, '#');
            if (strlen(comm,0) == 0){
                char *prop = (char*)seek_to(line.data, '=');
                if (prop && prop > line.data){
                    string prop_name = string_from_literal_length(line.data, prop-line.data-(prop > line.data && *(prop-1) == '='));
                    while (*prop <= ' ' || *prop > '~' || *prop == '\"') prop++;
                    size_t len = strlen(prop,0);
                    while (*(prop + len - 1) <= ' ' || *(prop + len - 1) > '~' || *(prop + len - 1) == '\"') len--;
                    size_t name_offset = 0;
                    while (prop_name.data[name_offset] <= ' ' || prop_name.data[name_offset] > '~' || prop_name.data[name_offset] == '\"') name_offset++;
                    on_kvp(prop_name.data + name_offset, prop, len, context);
                    free(line.data, line.mem_length);
                    free(prop_name.data, prop_name.mem_length);
                }
            }
        }
        path = nl;
    } while (*path);
}