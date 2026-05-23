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

void print_data(structdef field, sizedptr data){
    if (!data.ptr || !data.size) return;
    switch (field.type) {
    case binary_type_i8: print("%S: %i",field.name,*(i8*)data.ptr); break;
    case binary_type_i16: print("%S: %i",field.name,*(i16*)data.ptr); break;
    case binary_type_i32: print("%S: %i",field.name,*(i32*)data.ptr); break;  
    case binary_type_i64: print("%S: %i",field.name,*(i64*)data.ptr); break;
    case binary_type_float: print("%S: %f",field.name,*(float*)data.ptr); break;
    case binary_type_double: print("%S: %f",field.name,*(double*)data.ptr); break;
    case binary_type_string: print("%S: %v",field.name,data); break;
    default: return;
    }
}

void read_deser_data(structdef field, sizedptr data, bool is_alloc){
    print_data(field, data);
    if (is_alloc) release((void*)data.ptr);
}

void structured(){
    set_environment_config((env_config){env_display_document,env_behavior_wipe});

    structdef defintions[5] = {
        { .type = binary_type_i32,      .name = string_from_literal("number") },
        { .type = binary_type_string,   .name = string_from_literal("name") },
        { .type = binary_type_string,   .name = string_from_literal("type") },
        { .type = binary_type_i64,      .name = string_from_literal("size") },
        { .type = binary_type_string,   .name = string_from_literal("modified") },
    };

    set_environment_structure(defintions, N_ARR(defintions));

    binary_serializer serializer = make_binary_serializer(defintions, N_ARR(defintions));

    test tesdata = {
        .number = 1,
        .name = SLICE_LIT("Numero 1"),
        .type = SLICE_LIT("dir"),
        .size = 6,
        .modified = SLICE_LIT("A while ago"),
    };

    // print("Address of name %x",&tesdata.name);

    buffer buf = bin_ser_serialize(&serializer, &tesdata, sizeof(tesdata), 1);

    send_environment_data(buf.buffer,buf.buffer_size);

    msleep(3000);
    
    tesdata.modified = SLICE_LIT("Just now");

    buf = bin_ser_serialize(&serializer, &tesdata, sizeof(tesdata), 1);

    send_environment_data(buf.buffer,buf.buffer_size);
}

void tui(){
    put("{wipe}");
    put("{cursor:10,10}");
    for (int i = 0; i < 5; i++){
        put("{cursor:10,%i}{bg:fcba03 *}",10+i);
        put("{cursor:20,%i}{bg:fcba03 *}",10+i);
    }
    put("{cursor:9,17}{bg:fcba03 *}");
    put("{cursor:21,17}{bg:fcba03 *}");
    put("{cursor:10,18}");
    for (int i = 0; i <= 10; i++){
        put("{bg:fcba03 *}");
    }
}

int main(int argc, const char* argv[]){
    print("This should display in raw text");
    
    tui();
    
    // while (1){}

    return 0;
}
