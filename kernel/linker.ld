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

    . = ALIGN(4096);
    proc_1_rodata_start = .;
    .rodata.proc1 : { KEEP(*(.rodata.proc1)) }
    proc_1_rodata_end = .;
    proc_1_start = .;
    .text.proc1 : { KEEP(*(.text.proc1)) }
    proc_1_end = .;

    . = ALIGN(0x200000);
    kcode_end = .;
}