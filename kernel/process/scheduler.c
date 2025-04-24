#include "scheduler.h"
#include "console/kio.h"
#include "ram_e.h"
#include "proc_allocator.h"
#include "gic.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);
extern void restore_context(process_t* proc);
extern void restore_context_yield(process_t* proc);
extern void restore_pc_interrupt(process_t* proc);

#define MAX_PROCS 16
process_t processes[MAX_PROCS];
int current_proc = 0;
int proc_count = 0;

void save_context_registers(){
    save_context(&processes[current_proc]);
}

void save_return_address_interrupt(){
    save_pc_interrupt(&processes[current_proc]);
}

void restore_return_address_interrupt(){
    restore_pc_interrupt(&processes[current_proc]);
}

void switch_proc(ProcSwitchReason reason) {
    if (proc_count == 0)
        return;
    int next_proc = (current_proc + 1) % proc_count;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % proc_count;
        if (next_proc == current_proc)
            return;
    }

    current_proc = next_proc;
    if (reason == YIELD)
        restore_context_yield(&processes[current_proc]);
    else 
        restore_context(&processes[current_proc]);
}

void relocate_code(void* dst, void* src, uint32_t size) {
    uint32_t* src32 = (uint32_t*)src;
    uint32_t* dst32 = (uint32_t*)dst;
    uint64_t src_base = (uint64_t)src32;
    uint64_t dst_base = (uint64_t)dst32;
    uint32_t count = size / 4;

    printf("Beginning translation from base address %h to new address %h", src_base, dst_base);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t instr = src32[i];
        uint32_t op = instr >> 26;

        if (op == 5 || op == 37) {
            int32_t offset = ((int32_t)instr << 6) >> 6;
            offset *= 4;
            printf("Offset %i",offset);
            printf("Address %h",(uint64_t)src_base+(i*4));
            uint64_t target = src_base + (i * 4) + offset;
            bool internal = (target >= src_base) && (target < src_base + size);
        
            if (!internal) {
                int64_t rel = (int64_t)(target - (dst_base + (i * 4))) >> 2;
                instr = (instr & 0xFC000000) | (rel & 0x03FFFFFF);
            }
        
            printf("Branch op %i to %h (%s)", op, target, (uint64_t)(internal ? "internal" : "external"));
        } else if ((instr >> 24) == 84) { // B.cond (untested)
            int32_t offset = ((int32_t)(instr >> 5) & 0x7FFFF);
            offset = (offset << 6) >> 6;
            offset *= 4;
            uint64_t target = src_base + (i * 4) + offset;
            bool internal = (target >= src_base) && (target < src_base + size);
            if (!internal) {
                int32_t rel = (int32_t)(target - (dst_base + (i * 4))) >> 2;
                instr = (instr & ~0xFFFFE0) | ((rel & 0x7FFFF) << 5);
                printf("Relocated conditional branch to %h\n", target);
            } else {
                printf("Preserved internal conditional branch to %h\n", target);
            }
        } else if ((instr >> 24) == 169) { // ADRP. Same page for now
            uint64_t immhi = (instr >> 5) & 0x7FFFF;
            uint64_t immlo = (instr >> 29) & 0x3;
            int64_t offset = ((immhi << 14) | (immlo << 12));
            offset = (offset << 6) >> 6;
            uint64_t target = src_base + (i * 4) + offset;
            bool internal = (target >= src_base) && (target < src_base + size);
            if (!internal) {
                uint64_t new_target = dst_base + (i * 4) + offset;
                uint64_t new_offset = new_target - (dst_base & ~0xFFFULL);
                uint64_t new_immhi = (new_offset >> 14) & 0x7FFFF;
                uint64_t new_immlo = (new_offset >> 12) & 0x3;
                instr = (instr & ~0x60000000) | (new_immlo << 29);
                instr = (instr & ~(0x7FFFF << 5)) | (new_immhi << 5);
                printf("Relocated ADRP to %i\n", new_target);
            } else {
                printf("Preserved internal ADRP to %h\n", target);
            }
        }

        dst32[i] = instr;
    }

    printf("Finished translation");
}


process_t* create_process(void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size) {
    if (proc_count >= MAX_PROCS) return 0;

    process_t* proc = &processes[proc_count];

    printf("Code size %h", code_size);
    
    uint8_t* data_dest = (uint8_t*)alloc_proc_mem(data_size);
    if (!data_dest) return 0;

    for (uint64_t i = 0; i < data_size; i++){
        data_dest[i] = ((uint8_t *)data)[i];
    }

    uint64_t* code_dest = (uint64_t*)alloc_proc_mem(code_size);
    if (!code_dest) return 0;

    relocate_code(code_dest, func, code_size/*, (uint64_t)&func, (uint64_t)&code_dest*/);
    
    printf("Code copied to %h", (uint64_t)code_dest);
    uint64_t stack_size = 0x1000;

    printf("Stack size %h", stack_size);

    uint64_t stack = (uint64_t)alloc_proc_mem(stack_size);
    if (!stack) return 0;

    proc->sp = (stack + stack_size);
    proc->pc = (uint64_t)code_dest;
    proc->spsr = 0x3C5; // clean flags
    proc->state = READY;
    proc->id = proc_count++;
    
    return proc;
}

void start_scheduler(){
    timer_init(1);
    switch_proc(YIELD);
}

int get_current_proc(){
    return current_proc;
}

__attribute__((section(".rodata.proc1")))
static const char fmt[] = "Process %i";

__attribute__((section(".text.proc1")))
void proc_func() {
    int j = 0;
    while (1) {
        printf(fmt, j++);
    }
}

void default_processes(){
    extern uint8_t proc_1_start;
    extern uint8_t proc_1_end;
    extern uint8_t proc_1_rodata_start;
    extern uint8_t proc_1_rodata_end;

    create_process(proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
    create_process(proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
}