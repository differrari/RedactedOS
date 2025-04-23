ENTRY(_start)

SECTIONS {
    . = 0x41000000;
    kernel_start = .;
    .boot . : { boot.o(.text) }
    .text : { *(.text) }
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
}