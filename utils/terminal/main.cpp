#include "terminal.hpp"
#include "syscalls/syscalls.h"

Terminal *term;

int main(int argc, char **argv){
    term = new Terminal();
    while (1){
        term->update();
        msleep(20);
    }
}