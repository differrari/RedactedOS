#include "bootprocess_sm.hpp"
#include "bootprocess.h"
#include "../kprocess_loader.h"
#include "console/kio.h"

BootSM *state_machine;

extern "C" __attribute__((section(".text.kcoreprocesses"))) void eval_bootscreen() {
    while (1){
        state_machine->eval_state();
    }
}

extern "C" void init_bootprocess() {
    state_machine = new BootSM();
    create_kernel_process("bootsm",eval_bootscreen);
    state_machine->initialize();
}
