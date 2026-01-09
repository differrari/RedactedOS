#include "dwarf.h"
#include "types.h"
#include "console/kio.h"
#include "std/memory_access.h"
#include "std/string.h"

typedef struct {
    uint32_t unit_length;
    uint16_t version;
    uint8_t address_size;
    uint8_t segment_selector;
    uint32_t header_length;
    uint8_t minimum_instruction_length;
    uint8_t maximum_operations_per_instruction;
    uint8_t default_is_stmt;
    int8_t line_base;
    uint8_t line_range;
    uint8_t opcode_base;
    // uint8_t standard_opcode_lengths[];
    // uint8_t directory_entry_format_count;
    // uint8_t *directory_entry_format;
    // uint64_t directories_count;
    // char **directories;
	// uint8_t files_entry_format_count;
    // uint8_t *files_entry_format;
    // uint64_t files_count;
    // char **files;
}__attribute__((packed)) dwarf_debug_line_header;

typedef struct {
    uintptr_t address;
    uint32_t op_index;
    uint32_t file;
    uint32_t line;
    uint32_t column;
    bool is_stmt;
    bool basic_block;
    bool end_sequence;
    bool prologue_end;
    bool epilogue_begin;
    uint64_t isa;
    uint32_t discriminator;
} dwarf_debug_line_state_machine;

typedef enum {
    DW_LNS_copy            	 	= 0x1,
    DW_LNS_advance_pc        	= 0x2,
    DW_LNS_advance_line      	= 0x3,
    DW_LNS_set_file          	= 0x4,
    DW_LNS_set_column        	= 0x5,
    DW_LNS_negate_stmt       	= 0x6,
    DW_LNS_set_basic_block   	= 0x7,
    DW_LNS_const_add_pc      	= 0x8,
    DW_LNS_fixed_advance_pc  	= 0x9,
    DW_LNS_set_prologue_end  	= 0xA,
    DW_LNS_set_epilogue_begin	= 0xB,
    DW_LNS_set_isa           	= 0xC 
} DW_LNS_Opcode;

typedef enum {
    DW_LNE_end_sequence     	= 0x1,
    DW_LNE_set_address      	= 0x2,
    DW_LNE_rsvd             	= 0x3,
    DW_LNE_set_discriminator	= 0x4,
    DW_LNE_lo_user          	= 0x80,
    DW_LNE_hi_user          	= 0xFF
} DW_LNE_Opcode;

typedef enum {
	DW_LNCT_path				= 0x1,
	DW_LNCT_directory_index		= 0x2,
	DW_LNCT_timestamp			= 0x3,
	DW_LNCT_size				= 0x4,
	DW_LNCT_MD5					= 0x5,
} DW_LNCT;

typedef enum {
	DW_FORM_addr				= 0x01,
	DW_FORM_Reserved			= 0x02,
	DW_FORM_block2				= 0x03,
	DW_FORM_block4				= 0x04,
	DW_FORM_data2				= 0x05,
	DW_FORM_data4				= 0x06,
	DW_FORM_data8				= 0x07,
	DW_FORM_string				= 0x08,
	DW_FORM_block				= 0x09,
	DW_FORM_block1				= 0x0a,
	DW_FORM_data1				= 0x0b,
	DW_FORM_flag				= 0x0c,
	DW_FORM_sdata				= 0x0d,
	DW_FORM_strp				= 0x0e,
	DW_FORM_udata				= 0x0f,
	DW_FORM_ref_addr			= 0x10,
	DW_FORM_ref1				= 0x11,
	DW_FORM_ref2				= 0x12,
	DW_FORM_ref4				= 0x13,
	DW_FORM_ref8				= 0x14,
	DW_FORM_ref_udata			= 0x15,
	DW_FORM_indirect			= 0x16,
	DW_FORM_sec_offset			= 0x17,
	DW_FORM_exprloc				= 0x18,
	DW_FORM_flag_present		= 0x19,
	DW_FORM_strx				= 0x1a,
	DW_FORM_addrx				= 0x1b,
	DW_FORM_ref_sup4			= 0x1c,
	DW_FORM_strp_sup			= 0x1d,
	DW_FORM_data16				= 0x1e,
	DW_FORM_line_strp			= 0x1f,
	DW_FORM_ref_sig8			= 0x20,
	DW_FORM_implicit_const		= 0x21,
	DW_FORM_loclistx			= 0x22,
	DW_FORM_rnglistx			= 0x23,
	DW_FORM_ref_sup8			= 0x24,
	DW_FORM_strx1				= 0x25,
	DW_FORM_strx2				= 0x26,
	DW_FORM_strx3				= 0x27,
	DW_FORM_strx4				= 0x28,
	DW_FORM_addrx1				= 0x29,
	DW_FORM_addrx2				= 0x2a,
	DW_FORM_addrx3				= 0x2b,
	DW_FORM_addrx4				= 0x2c,
} DW_FORM;

uint64_t decode_uleb128(uint8_t **p) {
    uint64_t result = 0;
    int shift = 0;
	uint8_t byte;

    do {
        byte = *(*p)++;
        result |= ((uint64_t)(byte & 0x7F)) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);

    return result;
}

int64_t decode_sleb128(uint8_t **p) {
    int64_t result = 0;
    int shift = 0;
    uint8_t byte;

    do {
        byte = *(*p)++;
        result |= ((int64_t)(byte & 0x7F)) << shift;
        shift += 7;
    } while (byte & 0x80);

    if ((shift < 64) && (byte & 0x40)) {
        result |= -((int64_t)1 << shift);
    }

    return result;
}

uintptr_t decode_address(uint8_t **p, uint8_t address_size) {
    uintptr_t result = 0;
    for (int i = 0; i < address_size; i++)
        result |= ((uintptr_t)(*(*p)++)) << (i * 8);
    return result;
}

uint64_t type_codes[256];
uint64_t form_codes[256];

uintptr_t dwarf_decode_entries(uintptr_t ptr, uintptr_t debug_line_str_base, size_t str_size, const char *array[]){
	uint8_t *p = (uint8_t*)ptr;
	uint8_t directory_entry_format_count = *p++;
	// kprintf("Directory formats %i at %x",directory_entry_format_count, ptr);
	
	for (uint8_t i = 0; i < directory_entry_format_count; i++){
		type_codes[i] = decode_uleb128(&p);
		form_codes[i] = decode_uleb128(&p);
		// kprintf("Type code %i Form code %i",type_codes[i],form_codes[i]);
	}
	uint64_t directories_count = decode_uleb128(&p);
	// kprintf("Directories %i", directories_count);
	for (uint64_t i = 0; i < directories_count; i++){
		for (uint8_t f = 0; f < directory_entry_format_count; f++) {
            // uint64_t type_code = type_codes[f];
            uint64_t form_code = form_codes[f];

			// kprintf("Format %x",form_code);

            const char *str = NULL;

			switch (form_code) {
				case DW_FORM_block: decode_uleb128(&p); break;
				case DW_FORM_block1: p += 1; break;
				case DW_FORM_block2: p += 2; break;
				case DW_FORM_block4: p += 4; break;
				case DW_FORM_data1: p += 1; break;
				case DW_FORM_data2: p += 2; break;
				case DW_FORM_data4: p += 4; break;
				case DW_FORM_data8: p += 8; break;
				case DW_FORM_data16: p += 16; break;
				case DW_FORM_flag: p += 1; break;
				case DW_FORM_line_strp: {
					// kprintf("Line strp");
					uint32_t offset = read_unaligned32(p);
					// kprintf("Offset %x",offset);
					p += 4;
					if (offset > str_size){
						kprintf("Trying to read outside of section %x",offset);
						return ptr;
					}
					str = (const char *)(debug_line_str_base + offset);
					// kprintf("Directory %s", str);
					if (array) array[i] = str;
					break;
				}
				case DW_FORM_sdata: decode_sleb128(&p); break;
				case DW_FORM_sec_offset: kprintf("Great, now I gotta decipher whatever the fuck a DW_FORM_sec_offset is"); return ptr;
				case DW_FORM_string: 
					str = (const char *)p;
					// kprintf("Directory %s", str);
					if (array) array[i] = str;
					p += strlen(str) + 1;
				break;
				case DW_FORM_strp: p += 4; break;
				case DW_FORM_strx: decode_uleb128(&p); break;
				case DW_FORM_strx1: p += 1; break;
				case DW_FORM_strx2: p += 2; break;
				case DW_FORM_strx3: p += 3; break;
				case DW_FORM_udata: decode_uleb128(&p); break;
				default: kprintf("Unknown form code %i",form_code); return ptr;
			}
        }
	}
	return (uintptr_t)p;
}

const char *files[256];

debug_line_info dwarf_decode_lines(uintptr_t ptr, size_t size, uintptr_t debug_line_str_base, size_t str_size, uintptr_t address){

	uintptr_t end_section = ptr + size;

	while (ptr < end_section) {
		dwarf_debug_line_state_machine state = (dwarf_debug_line_state_machine) {
			.address = 0,
			.op_index = 0,
			.file = 1,
			.line = 1,
			.column = 0,
			.is_stmt = 0,
			.basic_block = false,
			.end_sequence = false,
			.prologue_end = false,
			.epilogue_begin = false,
			.isa = 0,
			.discriminator = 0
		};
		dwarf_debug_line_header *hdr = (dwarf_debug_line_header*)ptr;

		state.is_stmt = hdr->default_is_stmt;

		if (hdr->version != 5) {
			kprintf("Only DWARF version 5 is supported");
			return (debug_line_info){};
		}

		// kprintf("Header version -> %i",hdr->version);

		// kprintf("Header is %x bytes",sizeof(dwarf_debug_line_header));

		uintptr_t file_ptr = dwarf_decode_entries(ptr + sizeof(dwarf_debug_line_header) + hdr->opcode_base - 1, debug_line_str_base, str_size, 0);
		if (file_ptr != ptr){
			// kprintf("Now files %x",file_ptr);
			dwarf_decode_entries(file_ptr, debug_line_str_base, str_size, files);
		}

		// for (int i = 0; i < 256; i++){
		// 	if (files[i]) kprintf("File [%i] = %s",i,files[i]);
		// }

		ptr = (uintptr_t)&hdr->header_length + sizeof(hdr->header_length) + hdr->header_length;
		
		uint8_t *end = (uint8_t*)&hdr->unit_length + sizeof(hdr->unit_length) + hdr->unit_length;
		uint8_t *p = (uint8_t*)ptr;

		// kprintf("Program starts at %x, ends at %x",p,end);
		
		dwarf_debug_line_state_machine previous_state = state;

		while (p < end) {
			uint8_t opcode = *p++;
			if (p == end) break;
			// kprintf("OP: %i. P %x %x",opcode,p,end);
			
			bool emit_row = false;

			if (opcode == 0) {//Extended
				uint64_t len = decode_uleb128(&p);
				uint8_t ex_opcode = *p++;

				// kprintf("EOP: %i",ex_opcode);

				switch (ex_opcode) {
					case DW_LNE_end_sequence:
						state.end_sequence = true;
						emit_row = true;
						break;

					case DW_LNE_set_address:
						// kprintf("Address changed by DW_LNE_set_address from %x",state.address);
						state.address = decode_address(&p, hdr->address_size);
						// kprintf(" to %x",state.address);
						break;

					case DW_LNE_set_discriminator:
						state.discriminator = decode_uleb128(&p);
						break;

					default:
						kprintf("[DWARF ERROR] UNKNOWN EXTENDED OPCODE %i with length %i at %x",ex_opcode, len, p);
						return (debug_line_info){};
				}
			} else if (opcode < hdr->opcode_base) { //Standard
				switch (opcode) {
					case DW_LNS_copy:
						emit_row = true;
						break;

					case DW_LNS_advance_pc: {
						uint64_t operand = decode_uleb128(&p);
						// kprintf("Advancing DW_LNS_advance_pc by %x",operand);
						state.address += operand * hdr->minimum_instruction_length;
						break;
					}

					case DW_LNS_advance_line: {
						int64_t line_inc = decode_sleb128(&p);
						state.line += line_inc;
						break;
					}

					case DW_LNS_set_file:
						state.file = decode_uleb128(&p);
						// kprintf("SET FILE INSTRUCTION %i",state.file);
						break;

					case DW_LNS_set_column:
						state.column = decode_uleb128(&p);
						break;

					case DW_LNS_negate_stmt:
						state.is_stmt = !state.is_stmt;
						break;

					case DW_LNS_set_basic_block:
						state.basic_block = true;
						break;

					case DW_LNS_const_add_pc: {
						uint8_t adjusted = 255 - hdr->opcode_base;
						uint64_t addr_inc = (adjusted / hdr->line_range) * hdr->minimum_instruction_length;
						state.address += addr_inc;
						// kprintf("Advancing DW_LNS_const_add_pc by %x. New %x",addr_inc,state.address);
						break;
					}

					case DW_LNS_fixed_advance_pc: {
						uint16_t advance = *(uint16_t *)p;
						p += 2;
						state.address += advance;
						// kprintf("Advancing DW_LNS_fixed_advance_pc by %x. New %x",advance,state.address);
						break;
					}

					case DW_LNS_set_prologue_end:
						state.prologue_end = true;
						break;

					case DW_LNS_set_epilogue_begin:
						state.epilogue_begin = true;
						break;

					case DW_LNS_set_isa:
						state.isa = decode_uleb128(&p);
						break;

					default:
						kprintf("[DWARF ERROR] UNKNOWN STANDARD OPCODE %i",opcode);
						continue;
				}

			} else { //Special
				uint8_t adj = opcode - hdr->opcode_base;//146 - 13 = 133
				// kprintf("Special opcode %i - %i = %i",opcode)
				uint8_t op_adv = adj/hdr->line_range;//47/14 = 9.xxx
				state.line += hdr->line_base + (adj % hdr->line_range);
				// kprintf("Advancing by special by %x",op_adv);
				state.address += op_adv * hdr->minimum_instruction_length;
				state.basic_block = false;
				state.prologue_end = false;
				state.epilogue_begin = false;
				state.discriminator = 0;
				emit_row = true;
			}

			if (emit_row) {
				// kprintf("Address %#x line %i of file %s", state.address, state.line, files[state.file]);

				if (state.end_sequence) {
					// kprintf(">>>>>>Resetting state");
					state = (dwarf_debug_line_state_machine) {
						.address = 0,
						.op_index = 0,
						.file = 1,
						.line = 1,
						.column = 0,
						.is_stmt = hdr->default_is_stmt,
						.basic_block = false,
						.end_sequence = false,
						.prologue_end = false,
						.epilogue_begin = false,
						.isa = 0,
						.discriminator = 0
					};
				}

				if (state.address == address) {
				    return (debug_line_info){
						.address = address,
						.line = state.line,
						.column = state.column,
						.file = files[state.file]
					};
				} else if (state.address > address && previous_state.address && previous_state.address < address) {
				    return (debug_line_info){
						.address = address,
						.line = previous_state.line,
						.column = previous_state.column,
						.file = files[previous_state.file]
					};
				}

				previous_state = state;

				state.basic_block = false;
				state.prologue_end = false;
				state.epilogue_begin = false;
				state.discriminator = 0;
			}
		}
		ptr = (uintptr_t)p;
	}

	
	return (debug_line_info){};
}
