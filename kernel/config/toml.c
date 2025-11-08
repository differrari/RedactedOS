#include "toml.h"
#include "std/string.h"
#include "console/kio.h"

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
                    while (*prop && (*prop <= ' ' || *prop > '~')) prop++;
                    size_t len = strlen(prop,0);
                    while (len && (*(prop + len - 1) <= ' ' || *(prop + len - 1) > '~')) len--;
                    size_t name_offset = 0;
                    if (*prop == '\"'){
                        bool multiline = len >= 3 && *(prop+1) == *(prop+2) && *(prop+2) == *(prop);
                        if (!multiline){
                            if (*(prop+len-1) != '\"'){
                                kprintf("Non-terminated string");
                                path = nl;
                                continue;
                            }
                            len -= 2;
                            prop += 1;
                        } else {
                            prop = (char*)(path + (prop -line.data) + 3);
                            do {
                                nl = seek_to(nl, '\"');
                                if (strlen(nl, 3) >= 2 && *nl == '\"' && *(nl+1) == '\"'){
                                    len = nl-prop-1;
                                    nl += 2;
                                    break;
                                }
                            } while (nl);
                        }
                    } else if (*prop == '['){
                        int depth = 0;
                        prop = (char*)(path + (prop -line.data) + 1);
                        nl = prop;
                        do {
                            if (*nl == '[') depth++;
                            if (*nl == ']'){
                                if (depth == 0) break;
                                else depth--;
                            }
                            nl++;
                        } while (*nl);
                        len = (nl-prop);
                    }
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