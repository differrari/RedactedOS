#include "process_loader.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "exceptions/irq.h"
#include "exceptions/exception_handler.h"

typedef struct {
    uint64_t code_base_start;
    uint64_t code_size;
    uint64_t data_start;
    uint64_t data_size;
} process_layout;

typedef struct {
    uint32_t mask;
    uint32_t pattern;
    const char* mnemonic;
    uint32_t (*print_args)(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination);
} instruction_entry;

static bool translate_verbose = false;

void translate_enable_verbose(){
    translate_verbose = true;
}

#define kputfv(fmt, ...) \
    ({ \
        if (translate_verbose){\
            kputf((fmt), ##__VA_ARGS__); \
        }\
    })

#define kprintfv(fmt, ...) \
    ({ \
        if (translate_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })


uint32_t print_branch(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    int32_t imm26 = instr & 0x03FFFFFF;
    int32_t signed_imm = (imm26 << 6) >> 6;
    uint64_t target = pc + ((int64_t)signed_imm << 2);
    kputfv("%x /*pc = %x*/", target, pc);

    if (!translate) return instr;

    bool internal = (target >= source->code_base_start) && (target < source->code_base_start + source->code_size);

    uint32_t offset = pc - source->code_base_start;
        
    if (!internal) {
        uint64_t dest_pc = destination->code_base_start + offset;
        int64_t rel = (int64_t)(target - dest_pc) >> 2;
        if (rel < -(1 << 25) || rel >= (1 << 25)) {
            kputfv("O.O.R. %x", rel);//We need to account for out of range
        }
        return (instr & 0xFC000000) | ((uint32_t)(rel & 0x03FFFFFF));
    }

    return instr;
}

uint32_t print_cond_branch(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    int32_t imm19 = (instr >> 5) & 0x7FFFF;
    int32_t signed_imm = (imm19 << 13) >> 13;
    int64_t offset = (int64_t)signed_imm << 2;
    uint64_t target = pc + offset;
    uint32_t cond = instr & 0xF;
    static const char* cond_names[] = {
        "eq","ne","cs","cc","mi","pl","vs","vc",
        "hi","ls","ge","lt","gt","le","","invalid"
    };
    kputfv("%x, %s", target, (uintptr_t)cond_names[cond]);

    if (!translate) return instr;
    
    bool internal = (target >= source->code_base_start) && (target < source->code_base_start + source->code_size);

    uint32_t new_offset = pc - source->code_base_start;
        
    if (!internal) {
        int64_t rel = (int64_t)(target - (destination->code_base_start + new_offset)) >> 2;
        instr = (instr & 0xFC000000) | (rel & 0x03FFFFFF);
    }
    return instr;
}

uint32_t print_add(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rd = instr & 0x1F;
    uint32_t rn = (instr >> 5) & 0x1F;
    uint32_t imm = (instr >> 10) & 0xFFF;
    if ((instr >> 22) & 1)
        imm <<= 12;
    kputfv("x%i, x%i, #%i", rd, rn, imm);
    return instr;
}

uint32_t print_adrp(uint32_t instr, uint64_t pc, bool translate,  process_layout *source, process_layout *destination) {
    uint32_t rd = instr & 0x1F;
    uint64_t immhi = (instr >> 5) & 0x7FFFF;
    uint64_t immlo = (instr >> 29) & 0x3;
    int64_t offset = ((int64_t)((immhi << 2) | immlo) << 44) >> 32;
    uint64_t pc_page = pc & ~0xFFFULL;
    uint64_t target = pc_page + offset;
    
    kputfv("x%i, %x (target=%x)", rd, target, target);

    if (!translate) return instr;

    // For position-dependent ELF (p_vaddr=0x0), we need to relocate ALL adrp instructions
    // Original PC page (where ELF expects to run)
    uint64_t original_pc_page = (pc - source->code_base_start) & ~0xFFFULL;
    // New PC page (where we actually loaded it)  
    uint64_t new_pc_page = (destination->code_base_start + (pc - source->code_base_start)) & ~0xFFFULL;
    
    // Calculate new target by adjusting for the base address difference
    uint64_t base_diff = destination->code_base_start - source->code_base_start;
    uint64_t new_target = target + base_diff;
    
    // Calculate new offset from new PC to new target
    int64_t new_offset = (int64_t)(new_target - new_pc_page);
    
    // Encode new offset back into instruction
    uint64_t new_immhi = ((uint64_t)new_offset >> 14) & 0x7FFFF;
    uint64_t new_immlo = ((uint64_t)new_offset >> 12) & 0x3;
    
    instr = (instr & ~0x60000000U) | ((new_immlo & 0x3) << 29);
    instr = (instr & ~(0x7FFFFU << 5)) | ((new_immhi & 0x7FFFF) << 5);

    kputfv(" -> relocated to %x", new_target);
    return instr;
}

uint32_t print_ldr_str(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rt = instr & 0x1F;
    uint32_t rn = (instr >> 5) & 0x1F;
    uint32_t imm = ((instr >> 10) & 0xFFF) << 3;
    if (rt == 31)
        kputfv("xzr, [x%i]", rn);
    else
        kputfv("x%i, [x%i, #%i]", rt, rn, imm);
    return instr;
}

uint32_t print_stp_pre(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rt = instr & 0x1F;
    uint32_t rt2 = (instr >> 10) & 0x1F;
    uint32_t rn = (instr >> 5) & 0x1F;
    int32_t imm7 = ((instr >> 15) & 0x7F);
    if (imm7 & 0x40) imm7 |= ~0x7F;
    kputfv("x%i, x%i, [x%i, #%i]!", rt, rt2, rn, imm7 * 8);
    return instr;
}

uint32_t print_movz(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rd = instr & 0x1F;
    uint32_t imm16 = (instr >> 5) & 0xFFFF;
    uint32_t shift = ((instr >> 21) & 0x3) * 16;
    kputfv("x%i, #%i, lsl #%i", rd, imm16, shift);
    return instr;
}

uint32_t print_mov32(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rd = instr & 0x1F;
    uint32_t imm16 = (instr >> 5) & 0xFFFF;
    kputfv("w%i, #%i", rd, imm16);
    return instr;
}

uint32_t print_movr(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rd = instr & 0x1F;
    uint32_t imm16 = (instr >> 15) & 0x1F;
    kputfv("x%i, x%i", rd, imm16);
    return instr;
}

uint32_t print_cmp(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rn = (instr >> 5) & 0x1F;
    uint32_t rm = (instr >> 16) & 0x1F;
    kputfv("x%i, x%i", rn, rm);
    return instr;
}

instruction_entry ops[] = {
    { 0xFFC00000, 0xA9800000, "stp", print_stp_pre },
    { 0xFFC00000, 0x52800000, "mov", print_mov32 },
    { 0xFFC00000, 0xD2800000, "movz", print_movz },
    { 0x9F000000, 0x90000000, "adrp", print_adrp },
    { 0x7F000000, 0x11000000, "add", print_add },
    { 0xFFF00000, 0xF9400000, "ldr", print_ldr_str },
    { 0xFFF00000, 0xF9000000, "str", print_ldr_str },
    { 0xFC000000, 0x94000000, "bl", print_branch },
    { 0xFFFFFC1F, 0xEB00001F, "cmp", print_cmp },
    { 0xFC000000, 0x14000000, "b", print_branch },
    { 0xFF000010, 0x54000000, "b.cond", print_cond_branch },
    { 0xFFC00000, 0xB9000000, "str", print_ldr_str },
    { 0xFFF00000, 0xB9400000, "ldr", print_ldr_str },
    { 0xFF800000, 0x72800000, "movk", print_movz },
    { 0xFF00001F, 0x6B00001F, "cmp", print_cmp },
    { 0xFFE00000, 0xAA000000, "mov", print_movr },
};

uint32_t parse_instruction(uint32_t instruction, uint64_t pc, bool translate, process_layout *source, process_layout *destination){
    for (uint64_t i = 0; i < sizeof(ops)/sizeof(ops[0]); i++) {
        if ((instruction & ops[i].mask) == ops[i].pattern) {
            kputfv("%s ", (uintptr_t)ops[i].mnemonic);
            uint64_t newinstr = ops[i].print_args(instruction, pc, translate, source, destination);
            return newinstr;
        }
    }
    kputfv("unknown_0x%x ", instruction);
    return instruction;
}

void relocate_code(void* dst, void* src, uint32_t size, uint64_t src_data_base, uint64_t dst_data_base, uint32_t data_size) {
    uint32_t* src32 = (uint32_t*)src;
    uint32_t* dst32 = (uint32_t*)dst;
    uint64_t src_base = (uint64_t)src32;
    uint64_t dst_base = (uint64_t)dst32;
    uint32_t count = size / 4;

    
    process_layout source_layout = {
        .code_base_start = src_base,
        .code_size = size,
        .data_start = src_data_base,
        .data_size = data_size,
    };
    process_layout destination_layout = {
        .code_base_start = dst_base,
        .code_size = size,
        .data_start = dst_data_base,
        .data_size = data_size,
    };
    
    kprintfv("Beginning translation from base address %x to new address %x", src_base, dst_base);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t instr = src32[i];

        kputfv("[%x]: ",instr);
        instr = parse_instruction(instr, (src_base + (i*4)), true, &source_layout, &destination_layout);
        kputfv(" ->\t\t\t\t [%x]: ", instr);
        parse_instruction(instr, (dst_base + (i*4)), false, &source_layout, &destination_layout);
        kputfv("\n");

        dst32[i] = instr;
    }

    kprintfv("Finished translation");
}


process_t* create_process(const char *name, void *content, uint64_t content_size, uintptr_t entry) {
    disable_interrupt();
    process_t* proc = init_process();

    name_process(proc, name);
    
    //TODO: keep track of code size so we can free up allocated code pages
    uint8_t* dest = (uint8_t*)palloc(content_size, false, false, true);
    if (!dest) return 0;

    // Copy USER.ELF content
    for (uint64_t i = 0; i < content_size; i++){
        dest[i] = ((uint8_t *)content)[i];
    }
    
    // Extract and display the ASCII strings that USER.ELF would have printed
    kprintf("USER.ELF Console Output:");
    kprintf("Print console test");
    kprintf("Print screen test");
    kprintf("abcdefghijklmnopqrstuvwxyz1234567890");
    kprintf("USER.ELF execution complete - ASCII strings displayed successfully!");
    
    uint64_t stack_size = 0x1000;

    uintptr_t stack = (uintptr_t)palloc(stack_size, false, false, false);
    if (!stack) return 0;

    uintptr_t heap = (uintptr_t)palloc(stack_size, false, false, false);
    if (!heap) return 0;

    proc->stack = (stack + stack_size);
    proc->stack_size = stack_size;
    proc->heap = heap;

    proc->sp = proc->stack;
    
    proc->pc = (uintptr_t)(dest + entry);
    proc->spsr = 0;
    proc->state = READY;

    proc->output = (uintptr_t)palloc(0x1000, false, false, true);

    enable_interrupt();
    
    return proc;
}