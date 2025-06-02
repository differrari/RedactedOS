#include "elf_file.h"
#include "console/kio.h"
#include "process_loader.h"

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

process_t* load_elf_file(const char *name, void* file){
    elf_header *header = (elf_header*)file;

    kprintf("ELF FILE VERSION %h HEADER VERSION %h (%h)",header->elf_version,header->header_version,header->header_size);
    kprintf("FILE %i for %h",header->type, header->instruction_set);
    kprintf("ENTRY %h - %i",header->program_entry_offset);
    kprintf("HEADER %h - %i * %i vs %i",header->program_header_offset, header->program_header_entry_size,header->program_header_num_entries,sizeof(elf_program_header));
    elf_program_header* first_program_header = (elf_program_header*)((uint8_t *)file + header->program_header_offset);
    kprintf("program takes up %h, begins at %h, and is %b, %b",first_program_header->p_filez, first_program_header->p_offset, first_program_header->segment_type, first_program_header->flags);
    kprintf("SECTION %h - %i * %i",header->section_header_offset, header->section_entry_size,header->section_num_entries);
    kprintf("First instruction %h", (uint64_t)*((uint8_t *)file + 0x1000));

    return create_process(name, (void*)(file + first_program_header->p_offset), first_program_header->p_filez, header->program_entry_offset);
}