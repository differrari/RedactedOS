ENTRY(main)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x1000;

    .text : {
        *(.text .text.*)
    }:text

    .rodata : {
        *(.rodata .rodata.*)
    } :text
    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    } :data

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.comment .note .eh_frame)
    }
}