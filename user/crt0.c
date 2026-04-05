#include "syscalls/syscalls.h"

extern int main(int argc, char **argv);
//TODO extend user crt0 shutdown for cpp
//currently _start() just calls main() and then halt()
//if cpp is used in userland object teardown/cleanup must be handled
__attribute__((noreturn)) void _start(int argc, char **argv){
    int rc = main(argc, argv);
    halt(rc);
    __builtin_unreachable();
}