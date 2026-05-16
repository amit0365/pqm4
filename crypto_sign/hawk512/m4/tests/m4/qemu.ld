/*
 * Minimal Cortex-M4 linker script for the QEMU mps2-an386 board.
 * Used by the asm cross-check harness in this directory.
 *
 * Memory map (mps2-an386):
 *   0x00000000 — 16M  ZBT SSRAM3 (treated as flash; ROM image loaded here)
 *   0x20000000 — 4M   SRAM
 *
 * Vector table goes at the very start of "flash"; QEMU loads the ELF
 * and the M4 reset sequence reads the initial stack pointer from
 * [0x00000000] and the reset handler from [0x00000004], so we KEEP
 * the .vector_table section first.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

_stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .vector_table : {
        KEEP(*(.vector_table))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* .data initialized in flash, copied to RAM by Reset_Handler */
    _sidata = LOADADDR(.data);
    .data : {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ : { *(.ARM.*) *(.note*) *(.comment) }
}
