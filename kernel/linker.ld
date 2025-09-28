ENTRY(_start)

SECTIONS {
    . = LOAD_ADDR;
    kernel_start = .;
    .boot . : { ./build/boot.o(.text) }
    .text : { 
        *(EXCLUDE_FILE(*libshared.a:*.o) .text)
    }
    .data : { *(.data) }
    __bss_start = .;
    .bss :
    {
        bss = .;
        *(.bss)
    }
    . = ALIGN(0x1000);
    __bss_end = .;
    __bss_size = __bss_end - __bss_start;
    .vectors : { KEEP(*(.vectors)) }

    . = ALIGN(16);
    stack_bottom = .;
    .stack_fill : { FILL(0); . = . + 0x10000; }
    ksp = .;
    stack_top = .;

    . = ALIGN(0x1000);
    heap_bottom = .;
    .heap_fill : { FILL(0); . = . + 0xFF000; }
    heap_limit = .;

    . = ALIGN(0x200000);
    kcode_end = .;

    .shared : ALIGN(0x1000) {
        shared_start = .;
        *libshared.a:*(.text*)
        shared_code_end = .;
        . = ALIGN(0x1000);
        *libshared.a:*(.rodata*)
        shared_ro_end = .;
        . = ALIGN(0x1000);
        *libshared.a:*(.data*)
        . = ALIGN(0x1000);
        *libshared.a:*(.bss*)
        . = ALIGN(0x1000);
        shared_end = .;
    }

    . = ALIGN(0x200000);
    kfull_end = .;
}