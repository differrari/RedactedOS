ENTRY(_start)
HIGH_VA = 0xFFFF800000000000;
KERNEL_IMAGE_VA_BASE = 0xFFFFC00000000000;
KIMG_VA = KERNEL_IMAGE_VA_BASE;

SECTIONS {
    . = LOAD_ADDR;
    __boot_start = .;
    .boot : {
        KEEP(*(.boot.entry .boot.entry.*))
        KEEP(*(.boot.vectors .boot.vectors.*))
        *(.boot.text .boot.text.*)
        *(.boot.rodata .boot.rodata.*)
        *(.boot.data .boot.data.*)
    }

    .boot.bss (NOLOAD) : {
        . = ALIGN(16);
        __boot_bss_start = .;
        *(.boot.bss .boot.bss.*)
        . = ALIGN(16);
        boot_stack = .;
        . = . + 0x20000;
        boot_stack_top = .;
        . = ALIGN(16);
        __boot_bss_end = .;
    }

    .boot.const : {
        . = ALIGN(8);
        __boot_kimg_pa_start = .;
        QUAD(__kernel_lma)
        __boot_kimg_pa_end = .;
        QUAD(__kernel_lma_end)
    }
    . = ALIGN(0x1000);
    
    __kernel_lma = .;
    . = KERNEL_IMAGE_VA_BASE + __kernel_lma;
    __kernel_va_start = .;
    kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_IMAGE_VA_BASE) {
        __text_start = .;
        KEEP(*(.text .text.*))
        __text_end = .;
    }
    . = ALIGN(0x1000);

    .rodata : AT(ADDR(.rodata) - KERNEL_IMAGE_VA_BASE) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }
    . = ALIGN(0x1000);

    .data : AT(ADDR(.data) - KERNEL_IMAGE_VA_BASE) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }
    . = ALIGN(0x1000);

    .bss : AT(ADDR(.bss) - KERNEL_IMAGE_VA_BASE) {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(0x1000);

    .kstack (NOLOAD) : AT(ADDR(.kstack) - KERNEL_IMAGE_VA_BASE) {
        . = ALIGN(16);
        __kstack_bottom = .;
        . = . + 0x10000;
        ksp = .;
        __kstack_top = .;
    }
    . = ALIGN(0x1000);

    .vectors : AT(ADDR(.vectors) - KERNEL_IMAGE_VA_BASE) {
        __vectors_start = .;
        KEEP(*(.vectors))
        __vectors_end = .;
    }

    . = ALIGN(16);
    kcode_end = .;

    __kernel_va_end = .;
    __kernel_lma_end = __kernel_va_end - KERNEL_IMAGE_VA_BASE;
}