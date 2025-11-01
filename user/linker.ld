ENTRY(main)

SECTIONS {
    . = 0x1000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    }

    .data : {
        *(.data .data.*)
    }

    /DISCARD/ : {
        *(.comment .note .eh_frame)
    }
}