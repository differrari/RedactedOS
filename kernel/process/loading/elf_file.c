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

    if (header->magic[0] != 0x7f){
        kprintf("Failed to read header file");
        return 0;
    }

    elf_program_header* first_program_header = (elf_program_header*)((uint8_t *)file + header->program_header_offset);
    
    // Calculate entry offset relative to the first loadable segment
    uint64_t entry_offset = header->program_entry_offset - first_program_header->p_vaddr;
    
    return create_process(name, (void*)(file + first_program_header->p_offset), 
                         first_program_header->p_filez, entry_offset);
}