#include "terminal.hpp"

Terminal *term;

int main(int argc, char **argv){
    term = new Terminal();
    while (1){
        term->update();
    }
}