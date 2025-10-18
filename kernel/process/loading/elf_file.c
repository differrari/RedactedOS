#include "elf_file.h"
#include "console/kio.h"
#include "process_loader.h"
#include "dwarf.h"
#include "memory/page_allocator.h"
#include "std/memory.h"
#include "exceptions/irq.h"

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

process_t* load_elf_file(const char *name, const char *bundle, void* file, size_t filesize){
    elf_header *header = (elf_header*)file;

    if (header->magic[0] != 0x7f){
        kprintf("Failed to read header file");
        return 0;
    }

    // kprintf("ELF FILE VERSION %x HEADER VERSION %x (%x)",header->elf_version,header->header_version,header->header_size);
    // kprintf("There are %i program headers",header->program_header_num_entries);
    // kprintf("FILE %i for %x",header->type, header->instruction_set);
    // kprintf("ENTRY %x - %i",header->program_entry_offset);
    // kprintf("HEADER %x - %i * %i vs %i",header->program_header_offset, header->program_header_entry_size,header->program_header_num_entries,sizeof(elf_program_header));
    elf_program_header* first_program_header = (elf_program_header*)((uint8_t *)file + header->program_header_offset);
    // kprintf("VA: %x",first_program_header->p_vaddr);
    // kprintf("program takes up %x, begins at %x, and is %b, %b",first_program_header->p_filez, first_program_header->p_offset, first_program_header->segment_type, first_program_header->flags);
    // kprintf("SECTION %x - %i * %i",header->section_header_offset, header->section_entry_size,header->section_num_entries);
    // kprintf("First instruction %x", *(uint64_t*)(file + header->program_entry_offset));

    // kprintf("Sections %i. String at %i. Offset %x",header->section_num_entries,header->string_table_section_index,header->section_header_offset);

    elf_section_header *sections = (elf_section_header*)(file + header->section_header_offset);
    // kprintf("String table %s",file + sections[header->string_table_section_index].sh_offset);

    sizedptr debug_line = {};
    sizedptr debug_line_str = {};

    for (int i = 1; i < header->section_num_entries; i++){
        // kprintf("Offset %i",sections[i].sh_name);
        char *section_name = (char*)(file + sections[header->string_table_section_index].sh_offset + sections[i].sh_name);
        // kprintf("%i. %s. Starts at %x. Virt %x Align %x",i, section_name, sections[i].sh_offset,sections[i].sh_addr,sections[i].sh_addralign);
        // kprintf("Flags %b",sections[i].sh_flags);
        if (strcmp(".debug_line", section_name, true) == 0){
            debug_line = (sizedptr){(uintptr_t)file + sections[i].sh_offset,sections[i].sh_size};
        }
        if (strcmp(".debug_line_str", file + sections[header->string_table_section_index].sh_offset + sections[i].sh_name, true) == 0) {
            debug_line_str = (sizedptr){(uintptr_t)file + sections[i].sh_offset,sections[i].sh_size};
        }
        //.got/.got.plt = unresolved addresses to be determined by dynamic linking
    }

    disable_interrupt();

    process_t *proc = create_process(name, bundle, (void*)file, filesize, header->program_entry_offset, first_program_header->p_vaddr);
    
    if (debug_line.ptr && debug_line.size){ 
        proc->debug_lines.size = debug_line.size;
        void* dl = palloc(debug_line.size, MEM_PRIV_SHARED, MEM_RO, true);
        memcpy(dl, (void*)debug_line.ptr, debug_line.size);
        proc->debug_lines.ptr = (uintptr_t)dl;
    }
    if (debug_line_str.ptr && debug_line_str.size){ 
        proc->debug_line_str.size = debug_line_str.size;
        void* dls = palloc(debug_line_str.size, MEM_PRIV_SHARED, MEM_RO, true);
        memcpy(dls, (void*)debug_line_str.ptr, debug_line_str.size);
        proc->debug_line_str.ptr = (uintptr_t)dls;
    }

    enable_interrupt();

    return proc;
}