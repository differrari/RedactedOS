#include "syscalls/syscalls.h"
#include "environment/env_types.h"

#include "data/serialize/binary_serial.h"

typedef struct {
    i32 number;
    string_slice name;
    string_slice type;
    i64 size;
    string_slice modified;
}__attribute__((packed)) test;

int main(int argc, const char* argv[]){
    print("This should display in raw text");
    env_display_type disp = env_display_document;
    swritef("/environment/display", &disp, sizeof(disp));

    structdef defintions[5] = {
        { .type = binary_type_i32,      .name = string_from_literal("number") },
        { .type = binary_type_string,   .name = string_from_literal("name") },
        { .type = binary_type_string,   .name = string_from_literal("type") },
        { .type = binary_type_i64,      .name = string_from_literal("size") },
        { .type = binary_type_string,   .name = string_from_literal("modified") },
    };

    sizedptr serialized_struct = bin_ser_emit_structure(defintions, N_ARR(defintions));

    for (size_t i = 0; i < serialized_struct.size; i++){
        print("%x",((char*)serialized_struct.ptr)[i]);
    }

    swritef("/environment/data_structure", (char*)serialized_struct.ptr, serialized_struct.size);

    binary_serializer serializer = make_binary_serializer((char*)serialized_struct.ptr,serialized_struct.size);

    test tesdata = {
        .number = 1,
        .name = SLICE_LIT("Numero 1"),
        .type = SLICE_LIT("dir"),
        .size = 6,
        .modified = SLICE_LIT("A while ago"),
    };

    print("Address of name %x",&tesdata.name);

    buffer buf = bin_ser_serialize(&serializer, &tesdata, sizeof(tesdata), 1);

    swritef("/environment/data", buf.buffer, buf.buffer_size);

    print("Serialized data");

    for (size_t i = 0; i < buf.buffer_size; i++){
        print("%x",((char*)buf.buffer)[i]);
    }
    
    // /environment/data_structure 
    // /environment/data
    print("\[");
    print("This will eventually not be displayed, and a table will be displayed instead");
    while (1){}
}
