#include "monitor_processes.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "keyboard_input.h"
#include "input_keycodes.h"
#include "syscalls/syscalls.h"
#include "input/input_dispatch.h"
#include "ui/draw/draw.h"
#include "std/string.h"
#include "theme/theme.h"
#include "math/math.h"
#include "syscalls/syscalls.h"
#include "memory/memory_types.h"

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
    if (!info) return 0;
    uint64_t size = info->size;
    if (info->next)
        size += calc_heap((uintptr_t)info->next);
    return size;
}

char *procname;

void print_process_info(){
    process_t *processes = get_all_processes();
    for (int i = 0; i < MAX_PROCS; i++){
        process_t *proc = &processes[i];
        if (proc->id != 0 && proc->state != STOPPED && (!procname || strcmp_case(procname,proc->name,true) == 0)){
            print("Process [%i]: %s [pid = %i | status = %s]",i,(uintptr_t)proc->name,proc->id,(uintptr_t)parse_proc_state(proc->state));
            print("Stack: %x (%x). SP: %x",proc->stack, proc->stack_size, proc->sp);
            print("Heap: %x (%x)",proc->heap, calc_heap(proc->heap_phys));
            print("Flags: %x", proc->spsr);
            print("PC: %x",proc->pc);
        }
    }
}

#define PROCS_PER_SCREEN 2

uint16_t scroll_index = 0;

draw_ctx ctx;

void draw_memory(char *name,int x, int y, int width, int full_height, int used, int size){
    int height = full_height - (fb_get_char_size(2)*2) - 10;
    gpu_point stack_top = {x, y};
    fb_draw_line(&ctx,stack_top.x, stack_top.y, stack_top.x + width, stack_top.y, 0xFFFFFFFF);
    fb_draw_line(&ctx,stack_top.x, stack_top.y, stack_top.x, stack_top.y + height, 0xFFFFFFFF);
    fb_draw_line(&ctx,stack_top.x + width, stack_top.y, stack_top.x + width, stack_top.y + height, 0xFFFFFFFF);
    fb_draw_line(&ctx,stack_top.x, stack_top.y + height, stack_top.x + width, stack_top.y + height, 0xFFFFFFFF);
    
    int used_height = max((used * height) / size,1);

    fb_fill_rect(&ctx,stack_top.x + 1, stack_top.y + height - used_height + 1, width - 2, used_height-1, system_theme.bg_color);

    string str = string_format("%s\n%x",(uintptr_t)name, used);
    fb_draw_string(&ctx,str.data, stack_top.x, stack_top.y + height + 5, 2, system_theme.bg_color);
    free_sized(str.data,str.mem_length);
}

void draw_process_view(){
    fb_clear(&ctx,system_theme.bg_color+0x112211);
    process_t *processes = get_all_processes();
    gpu_size screen_size = (gpu_size){ctx.width,ctx.height};
    gpu_point screen_middle = {screen_size.width / 2, screen_size.height / 2};

    sys_focus_current();
    
    kbd_event ev;
    if (read_event(&ev)){
        if (ev.key == KEY_LEFT)
            scroll_index = max(scroll_index - 1, 0);
        if (ev.key == KEY_RIGHT)
            scroll_index = min(scroll_index + 1,MAX_PROCS);
    }

    for (int i = 0; i < PROCS_PER_SCREEN; i++) {
        int index = scroll_index;
        int valid_count = 0;

        process_t *proc = NULL;
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

        if (proc == NULL || proc->id == 0 || valid_count < i || proc->state == STOPPED) break;

        string name = string_from_literal((const char*)(uintptr_t)proc->name);
        string state = string_from_literal(parse_proc_state(proc->state));

        int scale = 2;

        int name_y = screen_middle.y - 100;
        int state_y = screen_middle.y - 60;
        int pc_y = screen_middle.y - 30;
        int stack_y = screen_middle.y;
        int stack_height = 130;
        int stack_width = 40;
        int flags_y = stack_y + stack_height + 10;

        int xo = (i * (screen_size.width / PROCS_PER_SCREEN)) + 50;

        fb_draw_string(&ctx,name.data, xo, name_y, scale, system_theme.bg_color);
        fb_draw_string(&ctx,state.data, xo, state_y, scale, system_theme.bg_color);
        
        string pc = string_from_hex(proc->pc);
        fb_draw_string(&ctx,pc.data, xo, pc_y, scale, system_theme.bg_color);
        free_sized(pc.data, pc.mem_length);
        
        draw_memory("Stack", xo, stack_y, stack_width, stack_height, proc->stack - proc->sp, proc->stack_size);
        uint64_t heap = calc_heap(proc->heap_phys);
        uint64_t heap_limit = ((heap + 0xFFF) & ~0xFFF);
        draw_memory("Heap", xo + stack_width + 50, stack_y, stack_width, stack_height, heap, heap_limit);

        string flags = string_format("Flags: %x", proc->spsr);
        fb_draw_string(&ctx, flags.data, xo, flags_y, scale, system_theme.bg_color);
        free_sized(name.data, name.mem_length);
        free_sized(state.data, state.mem_length);
        free_sized(flags.data, flags.mem_length);

    }
    commit_draw_ctx(&ctx);
}

bool visual;
void show_help(char* name){
    print("Usage: %s [gui] <filter>",name);
}

bool parse_args(int argc, char* argv[]){
    for (int i = 1; i < argc; i++){
        if (strcmp(argv[i],"gui") == 0){ 
            visual = true;
            request_draw_ctx(&ctx);
        }
        else if (strcmp(argv[i],"-help") == 0 || strcmp(argv[i],"-h") == 0){ 
            show_help(argv[0]);
            return false;
        }
        else if (strlen(argv[i])) procname = argv[i];
    }
    return true;
}

int monitor_procs(int argc, char* argv[]){
    visual = false;
    if (!parse_args(argc, argv)) return 0;
    while (1){
        if (visual)
            draw_process_view();
        else {
            print_process_info();
            msleep(5000);
        }
        kbd_event ev;
        if (read_event(&ev) && ev.key == KEY_ESC) return 0;
    }
    return 1;
}