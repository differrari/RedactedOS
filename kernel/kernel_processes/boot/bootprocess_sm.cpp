#include "bootprocess_sm.hpp"
#include "bootscreen.h"
#include "login_screen.h"
#include "../windows/windows.h"
#include "console/kio.h"

BootSM::BootSM(){

}

void BootSM::initialize(){
    AdvanceToState(Bootscreen);
}

BootSM::BootStates BootSM::eval_state(){
    //Check current process to see if it's terminated
    if (current_proc->state == process_t::process_state::STOPPED)
        AdvanceToState(GetNextState());

    return current_state;
}

void BootSM::AdvanceToState(BootStates next_state){
    switch (next_state){
        case Bootscreen:
            current_proc = start_bootscreen();
        break;
        case Login:
            current_proc = present_login();
        break;
        case Desktop:
            current_proc = start_windows();
        break;
    }
    current_state = next_state;
}

BootSM::BootStates BootSM::GetNextState(){
    switch (current_state){
        case Login:
            return Desktop;
        default:
            return Login;
    }
}