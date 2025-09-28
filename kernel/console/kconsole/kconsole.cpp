#include "kconsole.hpp"
#include "graph/graphics.h"

draw_ctx* KernelConsole::get_ctx(){
    return gpu_get_ctx();
}

void KernelConsole::flush(draw_ctx *ctx){
    gpu_flush();
}

bool KernelConsole::screen_ready(){
    return gpu_ready();
}
