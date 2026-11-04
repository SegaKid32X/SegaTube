/* SegaTube 68000 program: code in ROM at 0x880800,
 * variables in work RAM (telemetry lives at 0xFFFF00, stack below it). */
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    .text 0x880800 : {
        KEEP(*(.text.entry))
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        . = ALIGN(4);
    }
    __data_load = LOADADDR(.text) + SIZEOF(.text);
    .data 0xFF0000 : AT(__data_load) {
        __data_start = .;
        *(.ramtext)                 /* the EverDrive driver: runs with RV = 1 (md_ed.c) */
        *(.data) *(.data.*)
        . = ALIGN(4);
        __data_end = .;
    }
    .bss : {
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    __stack = 0xFFFE00;
    /DISCARD/ : { *(.comment) *(.note*) }
}
