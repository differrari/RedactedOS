ENTRY(_start)

SECTIONS {
    . = 0x41000000;
    kernel_start = .;
    .boot . : { boot.o(.text) }
    .text : { 
        *(EXCLUDE_FILE(shared/*.o) .text .text.[!p]*)
    }
    .data : { *(.data) }
    .bss : { *(.bss COMMON) }
    .vectors : { KEEP(*(.vectors)) }

    . = ALIGN(16);
    stack_bottom = .;
    .stack_fill : { FILL(0); . = . + 0x10000; }
    stack_top = .;

    . = ALIGN(4096);
    heap_bottom = .;
    .heap_fill : { FILL(0); . = . + 0x1000000; }
    heap_limit = .;

    .proc1 : ALIGN(8) {
        proc_1_rodata_start = .;
        KEEP(*(.rodata.proc1))
        KEEP(*(.data.proc1))
        proc_1_rodata_end = .;
        proc_1_start = .;
        KEEP(*(.text.proc1))
        proc_1_end = .;
    }

    .kbootscreen : ALIGN(8) {
        kbootscreen_rodata_start = .;
        KEEP(*(.rodata.kbootscreen))
        KEEP(*(.data.kbootscreen))
        kbootscreen_rodata_end = .;
        kbootscreen_start = .;
        KEEP(*(.text.kbootscreen))
        kbootscreen_end = .;
    }
    . = ALIGN(0x200000);
    kcode_end = .;

    . = . + 0x200000;

    .shared : ALIGN(4096) {
        shared_start = .;
        KEEP(*(.shared))
        KEEP(*(.text.shared))
        KEEP(*shared/*.o(.text*))

        . = ALIGN(8);
        BYTE(0);

        shared_end = .;
    }

    . = ALIGN(0x200000);
    kfull_end = .;
}