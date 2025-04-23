ENTRY(_start)

SECTIONS {
    . = 0x40000000;
    kernel_start = .;
    .boot . : { boot.o(.text) }
    .text : { *(.text) }
    .data : { *(.data) }
    .bss : { *(.bss COMMON) }
    .vectors : { KEEP(*(.vectors)) }

    . = ALIGN(16);
    stack_bottom = .;
    . = . + 0x10000; /* 64KB stack */
    stack_top = .;

    . = ALIGN(4096);
    heap_bottom = .;
    . = . + 0x1000000; /* 16MB heap */
    heap_limit = .;
}