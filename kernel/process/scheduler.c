#include "scheduler.h"
#include "console/kio.h"
#include "ram_e.h"
#include "proc_allocator.h"
#include "gic.h"
#include "console/serial/uart.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);

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
    // kprintf_raw("Resuming e xecution of process %i at %h",current_proc, processes[current_proc].pc);
    restore_context(&processes[current_proc]);
}

void relocate_code(void* dst, void* src, uint32_t size, uint64_t src_data_base, uint64_t dst_data_base, uint32_t data_size) {
    uint32_t* src32 = (uint32_t*)src;
    uint32_t* dst32 = (uint32_t*)dst;
    uint64_t src_base = (uint64_t)src32;
    uint64_t dst_base = (uint64_t)dst32;
    uint32_t count = size / 4;

    kprintf_raw("Beginning translation from base address %h to new address %h", src_base, dst_base);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t instr = src32[i];
        uint32_t op = instr >> 26;

        if (op == 5 || op == 37) {
            int32_t offset = ((int32_t)instr << 6) >> 6;
            offset *= 4;
            // kprintf_raw("Offset %i",offset);
            // kprintf_raw("Address %h",(uint64_t)src_base+(i*4));
            uint64_t target = src_base + (i * 4) + offset;
            bool internal = (target >= src_base) && (target < src_base + size);
        
            if (!internal) {
                int64_t rel = (int64_t)(target - (dst_base + (i * 4))) >> 2;
                instr = (instr & 0xFC000000) | (rel & 0x03FFFFFF);
            }
        
            // kprintf_raw("Branch op %i to %h (%s)", op, target, (uint64_t)(internal ? "internal" : "external"));
        } else if ((instr >> 24) == 84) { // B.cond (untested)
            int32_t offset = ((int32_t)(instr >> 5) & 0x7FFFF);
            offset = (offset << 6) >> 6;
            offset *= 4;
            uint64_t target = src_base + (i * 4) + offset;
            bool internal = (target >= src_base) && (target < src_base + size);
            if (!internal) {
                int32_t rel = (int32_t)(target - (dst_base + (i * 4))) >> 2;
                instr = (instr & ~0xFFFFE0) | ((rel & 0x7FFFF) << 5);
                // kprintf_raw("Relocated conditional branch to %h\n", target);
            }
        } else if ((instr & 0x9F000000) == 0x90000000) {
            uint64_t immlo = (instr >> 29) & 0x3;
            uint64_t immhi = (instr >> 5) & 0x7FFFF;
            int64_t offset = ((int64_t)((immhi << 14) | (immlo << 12)) << 43) >> 43;

            uint64_t pc_page = (src_base + i * 4) & ~0xFFFULL;
            uint64_t target = pc_page + offset;

            kprintf_raw("Was at offset %i of original code, so at address %h and data started at %h",offset,target,src_data_base);
        
            // uint64_t target = (src_base & ~0xFFFULL) + ((i * 4 + offset) & ~0xFFFULL);
            bool internal = (target >= src_data_base) && (target < src_data_base + data_size);

            if (internal){
                uint64_t data_offset = target - src_data_base;
                uint64_t new_target = dst_data_base + data_offset;

                uint64_t dst_pc_page = (dst_base + i * 4) & ~0xFFFULL;
                int64_t new_offset = (int64_t)(new_target - dst_pc_page);
                
                uint64_t new_immhi = (new_offset >> 14) & 0x7FFFF;
                uint64_t new_immlo = (new_offset >> 12) & 0x3;
                
                instr = (instr & ~0x60000000) | (new_immlo << 29);
                instr = (instr & ~(0x7FFFF << 5)) | (new_immhi << 5);

                kprintf_raw("We're inside data stack, so new address is: %i",data_offset);

                immlo = (instr >> 29) & 0x3;
                immhi = (instr >> 5) & 0x7FFFF;
                offset = ((int64_t)((immhi << 14) | (immlo << 12)) << 43) >> 43;

                pc_page = (dst_base + i * 4) & ~0xFFFULL;
                target = pc_page + offset;

                kprintf_raw("Confirmation: New address is %h compared to calculated one %h",target, new_target);

            } else 
                kprintf_raw("[ERROR:] Symbol not supported yet.");
        
        }

        dst32[i] = instr;
    }

    kprintf_raw("Finished translation");
}


process_t* create_process(void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size) {
    if (proc_count >= MAX_PROCS) return 0;

    process_t* proc = &processes[proc_count];

    kprintf_raw("Code size %h. Data size %h", code_size, data_size);
    
    uint8_t* data_dest = (uint8_t*)alloc_proc_mem(data_size);
    if (!data_dest) return 0;

    for (uint64_t i = 0; i < data_size; i++){
        data_dest[i] = ((uint8_t *)data)[i];
    }

    uint64_t* code_dest = (uint64_t*)alloc_proc_mem(code_size);
    if (!code_dest) return 0;

    relocate_code(code_dest, func, code_size, (uint64_t)&data[0], (uint64_t)&data_dest[0], data_size);
    
    kprintf_raw("Code copied to %h", (uint64_t)code_dest);
    uint64_t stack_size = 0x1000;

    uint64_t stack = (uint64_t)alloc_proc_mem(stack_size);
    kprintf_raw("Stack size %h. Start %h", stack_size,stack);
    if (!stack) return 0;

    proc->sp = (stack + stack_size);
    
    proc->pc = (uint64_t)code_dest;
    kprintf_raw("Process allocated with address at %h, stack at %h",proc->pc, proc->sp);
    proc->spsr = 0;
    proc->state = READY;
    proc->id = proc_count++;
    
    return proc;
}

void start_scheduler(){
    disable_interrupt();
    timer_init(10);
    switch_proc(YIELD);
}

int get_current_proc(){
    return current_proc;
}
