ENTRY(_boot)
SECTIONS
{
    . = 0x80000;                 /* Pi firmware loads kernel8.img here (64-bit) */
    .text :
    {
        KEEP(*(.text.boot))      /* _start MUST be first -> lands at 0x80000 */
        KEEP(*(.vectors))        /* FreeRTOS vector table (.balign 2048 inside) */
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
    __bss_size = (__bss_end - __bss_start) >> 3;   /* size in 8-byte units */
}
