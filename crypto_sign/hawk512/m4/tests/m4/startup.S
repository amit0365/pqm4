/*
 * Minimal Cortex-M4 startup for the QEMU mps2-an386 board.
 *
 * Provides:
 *   - Full 16-slot ARMv7-M system vector table (offsets 0..0x3C)
 *     including SysTick_Handler at offset 0x3C (vector 15).
 *   - Reset_Handler: copies .data from flash to RAM, zeros .bss,
 *     calls main(), then halts via semihosting SYS_EXIT.
 *   - Default HardFault handler that BKPTs (visible to QEMU).
 *   - SysTick_Handler: weak alias to a default that increments
 *     `systick_wrap_count` (defined in bench code) so that a
 *     long-running benchmark can recover a 56-bit virtual cycle
 *     count: wrap_count << 24 + (SYST_RVR - SYST_CVR).
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .extern _stack_top
    .extern _sdata
    .extern _edata
    .extern _sidata
    .extern _sbss
    .extern _ebss
    .extern main

    .section .vector_table, "a"
    .align  2
__vectors:
    .word   _stack_top              @ 0x00  Initial SP
    .word   Reset_Handler           @ 0x04  Reset
    .word   HardFault_Handler       @ 0x08  NMI
    .word   HardFault_Handler       @ 0x0C  HardFault
    .word   HardFault_Handler       @ 0x10  MemManage
    .word   HardFault_Handler       @ 0x14  BusFault
    .word   HardFault_Handler       @ 0x18  UsageFault
    .word   0                       @ 0x1C  Reserved
    .word   0                       @ 0x20  Reserved
    .word   0                       @ 0x24  Reserved
    .word   0                       @ 0x28  Reserved
    .word   SVC_Handler             @ 0x2C  SVCall
    .word   HardFault_Handler       @ 0x30  Debug Monitor
    .word   0                       @ 0x34  Reserved
    .word   PendSV_Handler          @ 0x38  PendSV
    .word   SysTick_Handler         @ 0x3C  SysTick

    .text

    .global Reset_Handler
    .type   Reset_Handler, %function
    .thumb_func
Reset_Handler:
    @ Copy .data from flash to RAM.
    @ (No FPU enable: build uses soft-float, no FPU instructions emitted.)
    ldr     r0, =_sidata
    ldr     r1, =_sdata
    ldr     r2, =_edata
1:  cmp     r1, r2
    bge     2f
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       1b
2:
    @ Zero .bss.
    ldr     r0, =_sbss
    ldr     r1, =_ebss
3:  cmp     r0, r1
    bge     4f
    movs    r2, #0
    str     r2, [r0], #4
    b       3b
4:
    @ Call main().
    bl      main

    @ main() returned: r0 = exit code. Semihosting SYS_EXIT.
    cbz     r0, exit_ok
    mov     r1, r0
    movs    r0, #0x18
    bkpt    #0xAB
    b       hang
exit_ok:
    movs    r0, #0x18
    ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
    bkpt    #0xAB
hang:
    b       hang
    .size   Reset_Handler, .-Reset_Handler

    .global HardFault_Handler
    .type   HardFault_Handler, %function
    .thumb_func
HardFault_Handler:
    bkpt    #0xFE
    b       HardFault_Handler
    .size   HardFault_Handler, .-HardFault_Handler

    @ Default weak stubs — tests/benches that need real handlers
    @ override these by providing a non-weak definition of the symbol.

    .weak   SysTick_Handler
    .type   SysTick_Handler, %function
    .thumb_func
SysTick_Handler:
    bx      lr
    .size   SysTick_Handler, .-SysTick_Handler

    .weak   SVC_Handler
    .type   SVC_Handler, %function
    .thumb_func
SVC_Handler:
    bx      lr
    .size   SVC_Handler, .-SVC_Handler

    .weak   PendSV_Handler
    .type   PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    bx      lr
    .size   PendSV_Handler, .-PendSV_Handler
