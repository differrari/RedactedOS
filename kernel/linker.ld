ENTRY(_start)

SECTIONS {
    . = LOAD_ADDR;
    kernel_start = .;
    .boot . : { ./build/boot.o(.text) }
    .text : { KEEP(*(.text)) }
    .data : { *(.data) }
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(0x1000);

    .vectors : { KEEP(*(.vectors)) }

    . = ALIGN(16);
    ksp = . + 0x10000;

    kcode_end = . + 0x10000;
}