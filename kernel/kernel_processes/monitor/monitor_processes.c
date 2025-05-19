#include "monitor_processes.h"
#include "process/scheduler.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "input/input_dispatch.h"
#include "../windows/windows.h"
#include "graph/graphics.h"
#include "kstring.h"
#include "memory/kalloc.h"
#include "theme/theme.h"
#include "math/math.h"
#include "std/syscalls/syscalls.h"

__attribute__((section(".text.kcoreprocesses")))
char* parse_proc_state(int state){
    switch (state)
    {
    case STOPPED:
        return "Stopped";

    case READY:
    case RUNNING:
    case BLOCKED:
        return "Running";
    
    default:
        break;
    }
}

__attribute__((section(".text.kcoreprocesses")))
void print_process_info(){
    uint16_t proc_count = process_count();
    process_t *processes = get_all_processes();
    for (int i = 0; i < MAX_PROCS; i++){
        process_t *proc = &processes[i];
        if (proc->id != 0 && proc->state != STOPPED){
            printf("Process [%i]: %s [pid = %i | status = %s]",i,(uintptr_t)proc->name,proc->id,(uint64_t)parse_proc_state(proc->state));
            printf("Stack: %h (%h). SP: %h",proc->stack, proc->stack_size, proc->sp);
            printf("Flags: %h", proc->spsr);
            printf("PC: %h",proc->pc);
        }
    }
    for (int i = 0; i < 100000000; i++);
}

#define PROCS_PER_SCREEN 2

uint16_t scroll_index = 0;

__attribute__((section(".text.kcoreprocesses")))
void draw_process_view(){
    gpu_clear(BG_COLOR+0x112211);
    process_t *processes = get_all_processes();
    gpu_size screen_size = gpu_get_screen_size();
    gpu_point screen_middle = {screen_size.width / 2, screen_size.height / 2};

    for (int i = 0; i < PROCS_PER_SCREEN; i++) {
        int index = scroll_index + i;

        process_t *proc;
        while (index < MAX_PROCS){
            proc = &processes[index];
            if (proc->id == 0 || proc->state == STOPPED){ 
                printf("Process %i %s invalid",index, (uintptr_t)proc->name);
                index++;
            }
            else {
                printf("Process %i %s valid", index, (uint64_t)proc->name);
                break;
            }
        }

        printf("Settled on process %i",index);

        if (proc->id == 0 || proc->state == STOPPED) break;

        kstring name = string_l((const char*)(uintptr_t)proc->name);
        kstring state = string_l(parse_proc_state(proc->state));

        int scale = 2;
        uint32_t char_size = gpu_get_char_size(scale);
        int name_offset = (name.length / 2) * char_size;
        int state_offset = (state.length / 2) * char_size;

        int name_y = screen_middle.y - 100;
        int state_y = screen_middle.y - 60;
        int pc_y = screen_middle.y - 30;
        int stack_y = screen_middle.y;
        int stack_height = 100;
        int stack_width = 40;
        int flags_y = stack_y + stack_height + 10;

        int xo = (i * (screen_size.width / PROCS_PER_SCREEN)) + 50;

        gpu_draw_string(name, (gpu_point){xo, name_y}, scale, BG_COLOR);
        gpu_draw_string(state, (gpu_point){xo, state_y}, scale, BG_COLOR);
        
        kstring pc = string_from_hex(proc->pc);
        gpu_draw_string(pc, (gpu_point){xo, pc_y}, scale, BG_COLOR);
        temp_free(pc.data, pc.length);
        
        gpu_point stack_top = {xo, stack_y};
        gpu_draw_line(stack_top, (gpu_point){stack_top.x + stack_width, stack_top.y}, 0xFFFFFF);
        gpu_draw_line(stack_top, (gpu_point){stack_top.x, stack_top.y + stack_height}, 0xFFFFFF);
        gpu_draw_line((gpu_point){stack_top.x + stack_width, stack_top.y}, (gpu_point){stack_top.x + stack_width, stack_top.y + stack_height}, 0xFFFFFF);
        gpu_draw_line((gpu_point){stack_top.x, stack_top.y + stack_height}, (gpu_point){stack_top.x + stack_width, stack_top.y + stack_height}, 0xFFFFFF);

        int used_stack = proc->stack - proc->sp;
        int used_height = max((used_stack * stack_height) / proc->stack_size,1);

        gpu_fill_rect((gpu_rect){stack_top.x + 1, stack_top.y + stack_height - used_height + 1, stack_width - 2, used_height-1}, BG_COLOR);

        kstring flags = string_from_hex(proc->spsr);
        gpu_draw_string(flags, (gpu_point){xo, flags_y}, scale, BG_COLOR);
        temp_free(name.data, name.length);
        temp_free(state.data, state.length);
        temp_free(flags.data, flags.length);

    }
    print_process_info();
    gpu_flush();
}

__attribute__((section(".text.kcoreprocesses")))
void monitor_procs(){
    keypress kp = {
        .modifier = KEY_MOD_ALT | KEY_MOD_CMD,
        .keys[0] = 0x15//R
    };
    uint16_t shortcut = sys_subscribe_shortcut_current(kp);
    bool active = false;
    while (1){
        if (sys_shortcut_triggered_current(shortcut)){
            active = !active;
            if (active)
                pause_window_draw();
            else 
                resume_window_draw();
        }
        if (active)
            draw_process_view();
    }
}

process_t* start_process_monitor(){
    return create_kernel_process("procmonitor",monitor_procs);
}