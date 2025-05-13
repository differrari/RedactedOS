#include "bootprocess_sm.hpp"
#include "bootprocess.h"
#include "../kprocess_loader.h"
#include "console/kio.h"

BootSM state_machine;

__attribute__((section(".text.kcoreprocesses")))
extern "C" void eval_bootscreen() {
    while (1){
        state_machine.eval_state();
    }
}

extern "C" void init_bootprocess() {
    state_machine.initialize();
    create_kernel_process(eval_bootscreen);
}
