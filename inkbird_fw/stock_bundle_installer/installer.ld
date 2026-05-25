MEMORY
{
  sram  (rwx) : ORIGIN = 0x1fff1838, LENGTH = 0x0b7c8
}

OUTPUT_ARCH(arm)
ENTRY(_vectors)

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        KEEP(*(.text.Reset_Handler))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > sram

    .data : {
        *(.data .data.*)
        . = ALIGN(4);
    } > sram

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > sram

    _stack_top = ORIGIN(sram) + LENGTH(sram);

    /DISCARD/ : {
        *(.comment)
        *(.ARM.attributes)
        *(.note*)
    }
}
