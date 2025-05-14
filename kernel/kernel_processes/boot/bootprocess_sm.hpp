#pragma once

#include "types.h"
#include "process.h"

class BootSM {

    enum BootStates {
        Bootscreen,
        Login,
        Desktop
    };

public:
    BootSM();

    void initialize();

    BootStates eval_state();

private:
    BootStates current_state;
    process_t* current_proc;

    void AdvanceToState(BootStates next_state);
    BootStates GetNextState();

};