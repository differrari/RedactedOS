#include "elf_file.h"
#include "console/kio.h"
#include "process_loader.h"
#include "dwarf.h"
#include "memory/page_allocator.h"
#include "memory/talloc.h"
#include "std/memory.h"
#include "exceptions/irq.h"
#include "sysregs.h"

typedef struct elf_header {
    char magic[4];//should be " ELF"
    uint8_t architecture;
    uint8_t endianness;
    uint8_t header_version;
    uint8_t ABI;
    uint64_t padding;
    uint16_t type;//1 relocatable, 2 executable, 3 shared, 4 core
    uint16_t instruction_set;//Expect 0xB7 = arm
    uint16_t elf_version;
    uint64_t program_entry_offset;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;//
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_num_entries;
    uint16_t section_entry_size;
    uint16_t section_num_entries;
    uint16_t string_table_section_index;
} elf_header;

typedef struct elf_program_header {
    uint32_t segment_type;
    uint32_t flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filez;
    uint64_t p_memsz;
    uint64_t alignment;
} elf_program_header;

typedef struct elf_section_header
{
  uint32_t	sh_name;		/* Section name (string tbl index) */
  uint32_t	sh_type;		/* Section type */
  uint64_t	sh_flags;		/* Section flags */
  uint64_t	sh_addr;		/* Section virtual addr at execution */
  uint64_t	sh_offset;		/* Section file offset */
  uint64_t	sh_size;		/* Section size in bytes */
  uint32_t	sh_link;		/* Link to another section */
  uint32_t	sh_info;		/* Additional section information */
  uint64_t	sh_addralign;		/* Section alignment */
  uint64_t	sh_entsize;		/* Entry size if section holds table */
} elf_section_header;

void get_elf_debug_info(process_t* proc, void* file, size_t filesize){
    if (!proc) return;
    if (!file) return;
    if (filesize < sizeof(elf_header)) return;

     elf_header *header = (elf_header*)file;

    if (header->magic[0] != 0x7f){
        kprintf("Failed to read header file");
        return;
    }
    if (header->magic[1] != 'E') return;
    if (header->magic[2] != 'L') return;
    if (header->magic[3] != 'F') return;

    if (header->section_entry_size != sizeof(elf_section_header)) return;
    if (header->section_num_entries == 0) return;
    if (header->section_header_offset >= filesize) return;

    size_t sh_total = (size_t)header->section_num_entries * sizeof(elf_section_header);
    if (header->section_header_offset + sh_total > filesize) return;
    if (header->string_table_section_index >= header->section_num_entries) return;

    elf_section_header *sections = (elf_section_header*)(file + header->section_header_offset);

    if (sections[header->string_table_section_index].sh_offset >= filesize) return;
    if (sections[header->string_table_section_index].sh_offset + sections[header->string_table_section_index].sh_size > filesize) return;

    sizedptr debug_line = {};
    sizedptr debug_line_str = {};

    for (int i = 1; i < header->section_num_entries; i++){
        if (sections[i].sh_name >= sections[header->string_table_section_index].sh_size) continue;

        char *section_name = (char*)(file + sections[header->string_table_section_index].sh_offset + sections[i].sh_name);
        if (!debug_line.ptr && strcmp_case(".debug_line", section_name,true) == 0){
            if (sections[i].sh_offset < filesize && sections[i].sh_offset + sections[i].sh_size <= filesize){
                debug_line = (sizedptr){(uintptr_t)file + sections[i].sh_offset,sections[i].sh_size};
            }
        }

        if (!debug_line_str.ptr && strcmp_case(".debug_line_str", section_name,true) == 0){
            if (sections[i].sh_offset < filesize && sections[i].sh_offset + sections[i].sh_size <= filesize){
                debug_line_str = (sizedptr){(uintptr_t)file + sections[i].sh_offset,sections[i].sh_size};
            }
        }
        if (debug_line.ptr && debug_line_str.ptr) break;
    }
    
    if (debug_line.ptr && debug_line.size){ 
        proc->debug_lines.size = debug_line.size;
        void* dl = palloc(debug_line.size, MEM_PRIV_KERNEL, MEM_RO, true);
        memcpy(dl, (void*)debug_line.ptr, debug_line.size);
        proc->debug_lines.ptr = (uintptr_t)dl;
    }
    if (debug_line_str.ptr && debug_line_str.size){ 
        proc->debug_line_str.size = debug_line_str.size;
        void* dls = palloc(debug_line_str.size, MEM_PRIV_KERNEL, MEM_RO, true);
        memcpy(dls, (void*)debug_line_str.ptr, debug_line_str.size);
        proc->debug_line_str.ptr = (uintptr_t)dls;
    }
}

uint8_t elf_to_red_permissions(uint8_t flags){
    uint8_t mem = 0;
    if ((flags >> 0) & 1) mem |= MEM_EXEC;
    if ((flags >> 1) & 1) mem |= MEM_RW;
    return mem;
}

process_t* load_elf_file(const char *name, const char *bundle, void* file, size_t filesize){
    if (!file) return 0;
    if (filesize < sizeof(elf_header)) return 0;

    elf_header *header = (elf_header*)file;

    if (header->magic[0] != 0x7f){
        kprintf("Failed to read header file %x",header->magic[0]);
        return 0;
    }

    if (header->magic[1] != 'E') return 0;
    if (header->magic[2] != 'L') return 0;
    if (header->magic[3] != 'F') return 0;
    // kprintf("ELF FILE VERSION %x HEADER VERSION %x (%x)",header->elf_version,header->header_version,header->header_size);
    // kprintf("There are %i program headers",header->program_header_num_entries);
    // kprintf("FILE %i for %x",header->type, header->instruction_set);
    // kprintf("ENTRY %x - %i",header->program_entry_offset);
    // kprintf("HEADER %x - %i * %i vs %i",header->program_header_offset, header->program_header_entry_size,header->program_header_num_entries,sizeof(elf_program_header));
    // elf_program_header* first_program_header = (elf_program_header*)((uint8_t *)file + header->program_header_offset);
    // kprintf("VA: %x",first_program_header->p_vaddr);
    // kprintf("program takes up %x, begins at %x, and is %b, %b",first_program_header->p_filez, first_program_header->p_offset, first_program_header->segment_type, first_program_header->flags);
    // kprintf("SECTION %x - %i * %i",header->section_header_offset, header->section_entry_size,header->section_num_entries);
    // kprintf("First instruction %x", *(uint64_t*)(file + header->program_entry_offset));

    // kprintf("Sections %i. String at %i. Offset %x",header->section_num_entries,header->string_table_section_index,header->section_header_offset);

    if (header->program_header_entry_size != sizeof(elf_program_header)) return 0;
    if (header->program_header_num_entries == 0) return 0;
    if (header->program_header_offset >= filesize) return 0;

    size_t ph_total = (size_t)header->program_header_num_entries * sizeof(elf_program_header);
    if (header->program_header_offset + ph_total > filesize) return 0;

    elf_program_header* program_headers = (elf_program_header*)(file + header->program_header_offset);

    bool use_sections = false;
    for (int i = 0; i < header->program_header_num_entries; i++) {
        if (program_headers[i].segment_type != 1) continue;
        if (program_headers[i].p_memsz == 0) continue;
        if ((elf_to_red_permissions(program_headers[i].flags) & (MEM_RW | MEM_EXEC)) == (MEM_RW | MEM_EXEC)){
            use_sections = true;
            break;
        }
    }

    if (use_sections){ //should this fallback be kept or should it be removed?
        kprintf("ELF has RWX PT_LOAD, using fallback");
        if (header->section_entry_size == sizeof(elf_section_header) && header->section_num_entries && header->section_header_offset < filesize) {
            size_t sh_total = (size_t)header->section_num_entries * sizeof(elf_section_header);
            if (header->section_header_offset + sh_total <= filesize) {
                elf_section_header *sections = (elf_section_header*)(file + header->section_header_offset);
                size_t sec_count = 0;
                for (int i = 1; i < header->section_num_entries; i++) {
                    if (!(sections[i].sh_flags & 2)) continue;
                    if (!sections[i].sh_size) continue;
                    if (sections[i].sh_type != 8) {
                        if (sections[i].sh_offset >= filesize) continue;
                        if (sections[i].sh_offset + sections[i].sh_size > filesize) continue;
                    }
                    sec_count++;
                }
                if (sec_count) {
                    program_load_data *data = (program_load_data*)talloc(sec_count * sizeof(program_load_data));
                    if (data){
                        size_t di = 0;
                        for (int i = 1; i < header->section_num_entries; i++) {
                            if (!(sections[i].sh_flags & 2)) continue;
                            if (!sections[i].sh_size) continue;
                            if (sections[i].sh_type != 8) {
                                if (sections[i].sh_offset >= filesize) continue;
                                if (sections[i].sh_offset + sections[i].sh_size > filesize) continue;
                            }

                            uint8_t perm = 0;
                            if (sections[i].sh_flags & 4) perm |= MEM_EXEC;
                            if (sections[i].sh_flags & 1) perm |= MEM_RW;

                            sizedptr file_cpy = {};
                            if (sections[i].sh_type != 8) {
                                file_cpy.ptr = (uintptr_t)file + sections[i].sh_offset;
                                file_cpy.size = sections[i].sh_size;
                            }

                            data[di] = (program_load_data) {
                                .permissions = perm,
                                .file_cpy = file_cpy,
                                .virt_mem = (sizedptr){sections[i].sh_addr,sections[i].sh_size}
                            };
                            di++;
                        }

                        if (di) {
                            process_t *proc = create_process(name, bundle, data, di, header->program_entry_offset, true);
                            temp_free(data, sec_count * sizeof(program_load_data));
                            if (!proc) return 0;

                            proc->PROC_X0 = 1;

                            size_t blen = strlen(bundle);
                            size_t nlen = strlen(name);
                            size_t plen = blen + nlen + 5;
                            uintptr_t sp = proc->stack - (plen+1);
                            paddr_t sp_phys = proc->stack_phys - (plen+1);

                            char *nargvals = (char*)PHYS_TO_VIRT_P(sp_phys);

                            memcpy(nargvals, bundle, blen);
                            *(char*)(nargvals + blen) = '/';
                            memcpy(nargvals + blen + 1, name, nlen);
                            memcpy(nargvals + blen+  1+  nlen, ".elf", 4);

                            *(char*)(nargvals + plen) = 0;

                            uintptr_t pad = sp & 15;
                            sp -= pad;
                            sp_phys -= pad;
                            sp -= 2 * sizeof(uintptr_t);
                            sp_phys -= 2 * sizeof(uintptr_t);
                            *(uintptr_t*)PHYS_TO_VIRT_P(sp_phys) = sp + 2 * sizeof(uintptr_t) + pad;

                            *(uintptr_t*)PHYS_TO_VIRT_P(sp_phys + sizeof(uintptr_t)) = 0;

                            proc->PROC_X1 = sp;
                            proc->sp = sp;

                            get_elf_debug_info(proc, file, filesize);

                            return proc;
                        }
                        temp_free(data, sec_count * sizeof(program_load_data));
                    }
                }
            }
        }
    }
    size_t load_count = 0;
    for (int i = 0; i < header->program_header_num_entries; i++) {
        if (program_headers[i].segment_type != 1) continue;
        if (program_headers[i].p_memsz == 0) continue;
        if (program_headers[i].p_offset > filesize) continue;
        if (program_headers[i].p_filez > filesize) continue;
        if (program_headers[i].p_offset + program_headers[i].p_filez > filesize) continue;
        load_count++;
    }
    if (load_count == 0) return 0;

    program_load_data* data = (program_load_data*)talloc(load_count * sizeof(program_load_data));
    if (!data) return 0;

    size_t di = 0;
    for (int i = 0; i < header->program_header_num_entries; i++){
        // kprintf("Load to %llx (%llx + %llx) for %llx (%llx) %b at %x", program_headers[i].p_vaddr, program_headers[i].p_offset, program_headers[i].p_offset, program_headers[i].p_memsz, program_headers[i].p_filez, program_headers[i].flags, program_headers[i].alignment);
        if (program_headers[i].segment_type != 1) continue;
        if (program_headers[i].p_memsz == 0) continue;
        if (program_headers[i].p_offset > filesize) continue;
        if (program_headers[i].p_filez > filesize) continue;
        if (program_headers[i].p_offset + program_headers[i].p_filez > filesize) continue;

        data[di] = (program_load_data){
            .permissions = elf_to_red_permissions(program_headers[i].flags),
            .file_cpy = (sizedptr){(uintptr_t)file + program_headers[i].p_offset,program_headers[i].p_filez},
            .virt_mem = (sizedptr){program_headers[i].p_vaddr,program_headers[i].p_memsz}
        };
        di++;
    }

    if (di == 0) {
        temp_free(data, load_count * sizeof(program_load_data));
        return 0;
    }

    process_t *proc = create_process(name, bundle, data, di, header->program_entry_offset, false);
    temp_free(data, load_count * sizeof(program_load_data));
    if (!proc) return 0;

    proc->PROC_X0 = 1;
    
    size_t blen = strlen(bundle);
    size_t nlen = strlen(name);
    size_t plen = blen + nlen + 1 + 4;
    uintptr_t sp = proc->stack - (plen+1);
    paddr_t sp_phys = proc->stack_phys - (plen+1);
    
    char *nargvals = (char*)PHYS_TO_VIRT_P(sp_phys);
    
    memcpy(nargvals, bundle, blen);
    *(char*)(nargvals+blen) = '/';
    memcpy(nargvals + blen + 1, name, nlen);
    memcpy(nargvals + blen+  1+  nlen, ".elf", 4);
    
    *(char*)(nargvals+plen) = 0;
    
    uintptr_t pad = sp & 15;
    sp -= pad;
    sp_phys -= pad;
    sp -= 2 * sizeof(uintptr_t);
    sp_phys -= 2 * sizeof(uintptr_t);
    *(uintptr_t*)PHYS_TO_VIRT_P(sp_phys) = sp + 2 * sizeof(uintptr_t) + pad;
    
    *(uintptr_t*)PHYS_TO_VIRT_P(sp_phys + sizeof(uintptr_t)) = 0;
    
    proc->PROC_X1 = sp;
    proc->sp = sp;
    
    get_elf_debug_info(proc, file, filesize);

    return proc;
}