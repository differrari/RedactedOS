#include "monitor_processes.h"
#include "process/scheduler.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "input/input_dispatch.h"
#include "../windows/windows.h"
#include "graph/graphics.h"
#include "std/string.h"
#include "memory/talloc.h"
#include "theme/theme.h"
#include "math/math.h"
#include "syscalls/syscalls.h"
#include "memory/memory_types.h"

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
        return "Invalid";
    }
}

uint64_t calc_heap(uintptr_t ptr){
    mem_page *info = (mem_page*)ptr;
    uint64_t size = info->size;
    if (info->next)
        size += calc_heap((uintptr_t)info->next);
    return size;
}

__attribute__((section(".text.kcoreprocesses")))
void print_process_info(){
    process_t *processes = get_all_processes();
    for (int i = 0; i < MAX_PROCS; i++){
        process_t *proc = &processes[i];
        if (proc->id != 0 && proc->state != STOPPED){
            printf("Process [%i]: %s [pid = %i | status = %s]",i,(uintptr_t)proc->name,proc->id,(uintptr_t)parse_proc_state(proc->state));
            printf("Stack: %x (%x). SP: %x",proc->stack, proc->stack_size, proc->sp);
            printf("Heap: %x (%x)",proc->heap, calc_heap(proc->heap));
            printf("Flags: %x", proc->spsr);
            printf("PC: %x",proc->pc);
        }
    }
    sleep(1000);
}

#define PROCS_PER_SCREEN 2

uint16_t scroll_index = 0;


void draw_memory(char *name,int x, int y, int width, int full_height, int used, int size){
    int height = full_height - (gpu_get_char_size(2)*2) - 10;
    gpu_point stack_top = {x, y};
    gpu_draw_line(stack_top, (gpu_point){stack_top.x + width, stack_top.y}, 0xFFFFFF);
    gpu_draw_line(stack_top, (gpu_point){stack_top.x, stack_top.y + height}, 0xFFFFFF);
    gpu_draw_line((gpu_point){stack_top.x + width, stack_top.y}, (gpu_point){stack_top.x + width, stack_top.y + height}, 0xFFFFFF);
    gpu_draw_line((gpu_point){stack_top.x, stack_top.y + height}, (gpu_point){stack_top.x + width, stack_top.y + height}, 0xFFFFFF);

    int used_height = max((used * height) / size,1);

    gpu_fill_rect((gpu_rect){{stack_top.x + 1, stack_top.y + height - used_height + 1}, {width - 2, used_height-1}}, BG_COLOR);

    string str = string_format("%s\n%x",(uintptr_t)name, used);
    gpu_draw_string(str, (gpu_point){stack_top.x, stack_top.y + height + 5}, 2, BG_COLOR);
    free(str.data,str.mem_length);
}

__attribute__((section(".text.kcoreprocesses")))
void draw_process_view(){
    gpu_clear(BG_COLOR+0x112211);
    process_t *processes = get_all_processes();
    gpu_size screen_size = gpu_get_screen_size();
    gpu_point screen_middle = {screen_size.width / 2, screen_size.height / 2};

    sys_focus_current();

    keypress kp;
    while (sys_read_input_current(&kp)){
        if (kp.keys[0] == KEY_LEFT)
            scroll_index = max(scroll_index - 1, 0);
        if (kp.keys[0] == KEY_RIGHT)
            scroll_index = min(scroll_index + 1,MAX_PROCS);
    }

    for (int i = 0; i < PROCS_PER_SCREEN; i++) {
        int index = scroll_index;
        int valid_count = 0;

        process_t *proc;
        while (index < MAX_PROCS) {
            proc = &processes[index];
            if (proc->id != 0 && proc->state != STOPPED) {
                if (valid_count == i + scroll_index) {
                    break;
                }
                valid_count++;
            }
            index++;
        }

        if (proc->id == 0 || valid_count < i || proc->state == STOPPED) break;

        string name = string_l((const char*)(uintptr_t)proc->name);
        string state = string_l(parse_proc_state(proc->state));

        int scale = 2;

        int name_y = screen_middle.y - 100;
        int state_y = screen_middle.y - 60;
        int pc_y = screen_middle.y - 30;
        int stack_y = screen_middle.y;
        int stack_height = 130;
        int stack_width = 40;
        int flags_y = stack_y + stack_height + 10;

        int xo = (i * (screen_size.width / PROCS_PER_SCREEN)) + 50;

        gpu_draw_string(name, (gpu_point){xo, name_y}, scale, BG_COLOR);
        gpu_draw_string(state, (gpu_point){xo, state_y}, scale, BG_COLOR);
        
        string pc = string_from_hex(proc->pc);
        gpu_draw_string(pc, (gpu_point){xo, pc_y}, scale, BG_COLOR);
        free(pc.data, pc.mem_length);
        
        draw_memory("Stack", xo, stack_y, stack_width, stack_height, proc->stack - proc->sp, proc->stack_size);
        uint64_t heap = calc_heap(proc->heap);
        uint64_t heap_limit = ((heap + 0xFFF) & ~0xFFF);
        draw_memory("Heap", xo + stack_width + 50, stack_y, stack_width, stack_height, heap, heap_limit);

        string flags = string_format("Flags: %x", proc->spsr);
        gpu_draw_string(flags, (gpu_point){xo, flags_y}, scale, BG_COLOR);
        free(name.data, name.mem_length);
        free(state.data, state.mem_length);
        free(flags.data, flags.mem_length);

    }
    gpu_flush();
    print_process_info();
}

__attribute__((section(".text.kcoreprocesses")))
void monitor_procs(){
    keypress kp = {
        .modifier = KEY_MOD_LALT,
        .keys[0] = 0x15//R
    };
    uint16_t shortcut = sys_subscribe_shortcut_current(kp);
    bool active = false;
    while (1){
        if (sys_shortcut_triggered_current(shortcut)){
            if (active)
                pause_window_draw();
            else 
                resume_window_draw();
            active = !active;
        }
        if (active)
            draw_process_view();
    }
}

process_t* start_process_monitor(){
#if QEMU
    return create_kernel_process("procmonitor",monitor_procs);
#else 
    //TODO: disabled process monitor since shortcuts seem broken on rpi
    return 0x0;//create_kernel_process("procmonitor",monitor_procs);
#endif
}