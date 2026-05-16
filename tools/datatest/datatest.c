#include "syscalls/syscalls.h"
#include "environment/env_types.h"

int main(int argc, const char* argv[]){
    print("This should display in raw text");
    env_display_type disp = env_display_document;
    swritef("/environment/display", &disp, sizeof(disp));
    // /environment/data_structure 
    // /environment/data
    print("\[");
    print("This will eventually not be displayed, and a table will be displayed instead");
    while (1){}
}
