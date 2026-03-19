#include "process_loader.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "exceptions/irq.h"
#include "exceptions/exception_handler.h"
#include "std/memory.h"
#include "memory/mmu.h"
#include "memory/talloc.h"
#include "sysregs.h"
#include "memory/addr.h"

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
    uint32_t (*reloc)(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination);
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
    kputfv("%llx /*pc = %llx*/", target, pc);

    if (!translate) return instr;

    bool internal = (target >= source->code_base_start) && (target < source->code_base_start + source->code_size);

    uint64_t offset = pc - source->code_base_start;
        
    if (!internal) {
        uint64_t dest_pc = destination->code_base_start + offset;
        int64_t rel = (int64_t)(target - dest_pc) >> 2;
        if (rel < -(1 << 25) || rel >= (1 << 25)) {
            kputfv("O.O.R. %llx", rel);//We need to account for out of range
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
    kputfv("%llx, %s", target, cond_names[cond]);

    if (!translate) return instr;
    
    bool internal = (target >= source->code_base_start) && (target < source->code_base_start + source->code_size);

    uint32_t new_offset = pc - source->code_base_start;
        
    if (!internal) {
        int64_t rel = (int64_t)(target - (destination->code_base_start + new_offset)) >> 2;
        instr = (instr & ~(0x7FFFFu << 5)) | ((uint32_t)(rel & 0x7FFFF) << 5);
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
    int64_t imm21 = (int64_t)((immhi << 2) | immlo);
    imm21 = (imm21 << 43) >> 43;//b
    int64_t offset = imm21 << 12;
    kputfv("x%i, %llx", rd, (pc & ~0xFFFUL) + offset);

    if (!translate) return instr;

    uint64_t pc_page = pc & ~0xFFFULL;
    uint64_t target = pc_page + offset;

    uint64_t pc_offset = pc - source->code_base_start;

    bool internal = (target >= source->data_start) && (target < source->data_start + source->data_size);

    if (internal){
        uint64_t data_offset = target - source->data_start;
        uint64_t new_target = destination->data_start + data_offset;

        uint64_t dst_pc_page = (destination->code_base_start + pc_offset) & ~0xFFFULL;
        int64_t new_offset = (int64_t)(new_target - dst_pc_page);
        
        uint64_t new_immhi = (new_offset >> 14) & 0x7FFFF;
        uint64_t new_immlo = (new_offset >> 12) & 0x3;
        
        instr = (instr & ~0x60000000) | (new_immlo << 29);
        instr = (instr & ~(0x7FFFF << 5)) | (new_immhi << 5);

    }
    return instr;
}

uint32_t print_ldr_str(uint32_t instr, uint64_t pc, bool translate, process_layout *source, process_layout *destination) {
    uint32_t rt = instr & 0x1F;
    uint32_t rn = (instr >> 5) & 0x1F;
    uint32_t imm = ((instr >> 10) & 0xFFF) << (instr >> 30) & 0x3;
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
    uint32_t imm16 = (instr >> 16) & 0x1F;
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

void decode_instruction(uint32_t instruction){
    kprintf("Instruction code %x",instruction);
    for (uint64_t i = 0; i < N_ARR(ops); i++) {
        if ((instruction & ops[i].mask) == ops[i].pattern) {
            kputf("%s ", ops[i].mnemonic);
            break;
        }
    }
}

uint32_t parse_instruction(uint32_t instruction, uint64_t pc, bool translate, process_layout *source, process_layout *destination){
    for (uint64_t i = 0; i < N_ARR(ops); i++) {
        if ((instruction & ops[i].mask) == ops[i].pattern) {
            kputfv("%s ", ops[i].mnemonic);
            uint32_t newinstr = ops[i].reloc(instruction, pc, translate, source, destination);
            return newinstr;
        }
    }
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
    
    kprintfv("Beginning translation from base address %llx to new address %llx", src_base, dst_base);
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

size_t map_section(process_t *proc, kaddr_t base, uaddr_t off, program_load_data data){
    // kprintf("Copying %llx from %llx to %llx, representing %llx",data.file_cpy.size,data.file_cpy.ptr,base + (data.virt_mem.ptr - off), data.virt_mem.size);
    if (data.file_cpy.size) memcpy((void*)(uintptr_t)base + ((uintptr_t)data.virt_mem.ptr - (uintptr_t)off), (void*)data.file_cpy.ptr, data.file_cpy.size);
    return data.virt_mem.size;
}

process_t* create_process(const char *name, const char *bundle, program_load_data *data, size_t data_count, uintptr_t entry, bool allow_rwx) {

    process_t* proc = init_process();

    name_process(proc, name);

    proc->bundle = (char*)bundle;
    

    uaddr_t min_addr = UINT64_MAX;
    uaddr_t max_addr = 0;
    
    for (size_t i = 0; i < data_count; i++){
        uaddr_t s0 = (uaddr_t)data[i].virt_mem.ptr;
        uaddr_t s1 = (uaddr_t)(data[i].virt_mem.ptr + data[i].virt_mem.size);
        if (s0 < min_addr) min_addr = s0;
        if (s1 > max_addr) max_addr = s1;
    } 
    uaddr_t min_map = min_addr & ~(GRANULE_4KB - 1);
    uaddr_t max_map = (max_addr + (GRANULE_4KB - 1)) & ~(GRANULE_4KB - 1);

    size_t code_size = max_map -min_map;

    // kprintf("Code takes %x from %x to %x",code_size, min_addr, max_addr);

    uintptr_t *ttbr = mmu_new_ttbr();

    memset(&proc->mm, 0, sizeof(proc->mm));
    proc->mm.ttbr0 = ttbr;
    proc->mm.ttbr0_phys = pt_va_to_pa(ttbr);

    paddr_t dest = palloc_inner(code_size, MEM_PRIV_USER, MEM_RW, true, false);
    if (!dest) {
        reset_process(proc);
        return 0;
    }
    
    // kprintf("Allocated space for process between %x and %x",dest,dest+((code_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)));
    
    for (uaddr_t va = min_map; va < max_map; va += GRANULE_4KB) {
        bool any = 0;
        bool rw = 0;
        bool ex = 0;

        for (size_t s = 0; s < data_count; s++){
            uaddr_t s0 = (uaddr_t)data[s].virt_mem.ptr;
            uaddr_t s1 = (uaddr_t)(data[s].virt_mem.ptr + data[s].virt_mem.size);
            if (va + GRANULE_4KB <= s0) continue;
            if (va >= s1) continue;
            any = true;
            if (data[s].permissions & MEM_RW) rw = true;
            if (data[s].permissions & MEM_EXEC) ex = true;
        }

        if (!any) continue;
        if (rw && ex && !allow_rwx) {
            //kprintf("WX overlap at page %llx", va);
            if (dest) pfree((void*)dmap_pa_to_kva(dest), code_size);
            reset_process(proc);
            return 0;
        }
        if (rw && !allow_rwx) ex = false;

        uint8_t attr = MEM_NORM;
        if (rw) attr |= MEM_RW;
        if (ex) attr |= MEM_EXEC;
        mmu_map_4kb((uint64_t*)ttbr, (uint64_t)va, (paddr_t)(dest + (va - min_map)), MAIR_IDX_NORMAL, attr, MEM_PRIV_USER);
    }
    memset((void*)dmap_pa_to_kva(dest), 0, code_size);

    for (size_t i = 0; i < data_count; i++) {
        uint8_t prot = MEM_NORM;
        if (data[i].permissions & MEM_RW) prot |= MEM_RW;
        if (data[i].permissions & MEM_EXEC) prot |= MEM_EXEC;
        mm_add_vma(&proc->mm, data[i].virt_mem.ptr, data[i].virt_mem.ptr + data[i].virt_mem.size, prot, VMA_KIND_ELF, 0);
    }
    for (size_t i = 0; i < data_count; i++)
        map_section(proc, dmap_pa_to_kva(dest), min_map, data[i]);

    proc->va = min_map;
    proc->code = dest;
    proc->code_size = code_size;

    uint64_t stack_max_size = 0x800000; //TODO it shouldnt be fix
    uaddr_t stack_top = 0x00007FFFFFFFF000ULL;
    uaddr_t stack_limit = stack_top - stack_max_size;
    uaddr_t stack_commit = stack_top;
    uaddr_t mmap_top = stack_limit - PAGE_SIZE;

    uaddr_t mmap_bottom = (max_map + (PAGE_SIZE*4) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (mmap_bottom >= mmap_top) {
        reset_process(proc);
        return 0;
    }
    proc->heap_phys = 0;

    proc->mm.mmap_bottom = mmap_bottom;
    proc->mm.mmap_top = mmap_top;
    proc->mm.mmap_cursor = mmap_top;
    proc->mm.stack_top = stack_top;
    proc->mm.stack_limit = stack_limit;
    proc->mm.stack_commit = stack_commit;


    uint64_t total_pages = get_total_user_ram() / PAGE_SIZE;
    if (!total_pages) total_pages = 1;

    proc->mm.cap_stack_pages = stack_max_size / PAGE_SIZE;
    proc->mm.cap_anon_pages = total_pages / 2;
    if (proc->mm.cap_anon_pages < 128) proc->mm.cap_anon_pages = 128;

    mm_add_vma(&proc->mm, proc->mm.stack_limit, proc->mm.stack_top, MEM_RW, VMA_KIND_STACK, VMA_FLAG_DEMAND);

    proc->stack = stack_top;
    proc->stack_phys = 0;
    proc->stack_size = stack_max_size;
    proc->mm.rss_stack_pages = 0;

    proc->sp = proc->stack;

    proc->output = (kaddr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!proc->output) {
        reset_process(proc);
        return 0;
    }
    proc->output_size = 0;

    proc->pc = (uintptr_t)(entry);
    kprintf("User process %s allocated at %llx entry=%llx stack=%llx-%llx (phys=%llx-%llx) anon=%llx (phys=%llx)", name, proc, (uint64_t)proc->pc, (uint64_t)proc->mm.stack_limit, (uint64_t)proc->mm.stack_top, (uint64_t)proc->stack_phys, (uint64_t)proc->stack_phys, (uint64_t)proc->mm.mmap_bottom, (uint64_t)proc->heap_phys);
    proc->spsr = 0;
    proc->state = BLOCKED;
    
    return proc;
}
