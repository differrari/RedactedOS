#include "syscalls/syscalls.h"
#include "files/system_module.h"
#include "files/helpers.h"

extern void refresh_apps();
extern system_module apps_mod;

extern void refresh_tools();
extern system_module tools_mod;

int main(int argc, char* argv[]){
    refresh_apps();
    load_fsmodule(&apps_mod, true);

    refresh_tools();
    load_fsmodule(&tools_mod, true);

    while (true){}
    
    return 0;
}