ENTRY(_start)

SECTIONS {
    . = LOAD_ADDR;
    kernel_start = .;
    .boot . : { boot.o(.text) }
    .text : { 
        *(EXCLUDE_FILE(shared/*.o) .text .text.[!p]*)
    }
    .data : { *(.data) }
    __bss_start = .;
    .bss :
    {
        bss = .;
        *(.bss)
    }
    . = ALIGN(4096); /* align to page size */
    __bss_end = .;
    __bss_size = __bss_end - __bss_start;
    .vectors : { KEEP(*(.vectors)) }

    . = ALIGN(16);
    stack_bottom = .;
    .stack_fill : { FILL(0); . = . + 0x10000; }
    ksp = .;
    stack_top = .;

    . = ALIGN(4096);
    heap_bottom = .;
    .heap_fill : { FILL(0); . = . + 0x2000000; }
    heap_limit = .;

    .kcoreprocesses : ALIGN(8) {
        kcoreprocesses_rodata_start = .;
        KEEP(*(.rodata.kcoreprocesses))
        KEEP(*(.data.kcoreprocesses))
        kcoreprocesses_rodata_end = .;
        kcoreprocesses_start = .;
        KEEP(*(.text.kcoreprocesses))
        kcoreprocesses_end = .;
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