#include "bootprocess_sm.hpp"
#include "bootscreen.h"
#include "login_screen.h"
#include "../windows/dos.h"
#include "console/kio.h"
#include "input/input_dispatch.h"
#include "usb/usb.h"
#include "graph/graphics.h"

BootSM::BootSM(){

}

void BootSM::initialize(){
    disable_visual();
    usb_start_polling();
    gpu_size screen_size = gpu_get_screen_size();
    mouse_config((gpu_point){screen_size.width/2,screen_size.height/2}, screen_size);
    AdvanceToState(Bootscreen);
}

BootSM::BootStates BootSM::eval_state(){
    if (!current_proc || current_proc->state == process_t::process_state::STOPPED)
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
            current_proc = create_windowing_system();
        break;
    }
    current_state = next_state;
}

BootSM::BootStates BootSM::GetNextState(){
    switch (current_state){
        // case Login:
        //     return Desktop;
        default:
            return Desktop;
    }
}