#include "bootprocess_sm.hpp"
#include "bootprocess.h"
#include "../kprocess_loader.h"
#include "console/kio.h"

BootSM *state_machine;

//TODO: This is overengineered, just use C
extern "C" int eval_bootscreen(int argc, char* argv[]) {
    while (1){
        state_machine->eval_state();
    }
    return 1;
}

extern "C" void init_bootprocess() {
    state_machine = new BootSM();
    create_kernel_process("bootsm",eval_bootscreen, 0, 0);
    state_machine->initialize();
}
