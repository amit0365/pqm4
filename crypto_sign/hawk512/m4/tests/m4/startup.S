/*
 * Minimal Cortex-M4 startup for the QEMU mps2-an386 board.
 *
 * Provides:
 *   - Vector table with initial SP and Reset_Handler entry
 *   - Reset_Handler: copies .data from flash to RAM, zeros .bss,
 *     calls main(), then halts via semihosting SYS_EXIT.
 *   - Default HardFault handler that BKPTs (visible to QEMU).
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
    @ rest of vector table not required for this harness

    .text

    .global Reset_Handler
    .type   Reset_Handler, %function
    .thumb_func
Reset_Handler:
    @ Copy .data from flash to RAM.
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

    @ main() returned: r0 has exit code. Issue semihosting SYS_EXIT.
    @ ARM semihosting v2 SYS_EXIT_EXTENDED (0x20): TARGET_SIG_APP_EXIT, code.
    @ For ARMv7-M with v1 compat, SYS_EXIT (0x18) takes ADP_Stopped_ApplicationExit
    @ (0x20026) as r1 to signal clean exit. We pass r0 (main's return) through.
    @ Simplest: use semihosting SYS_EXIT with status from r0.
    @ Spec for AArch32: r0=0x18, r1=0x20026 means ApplicationExit (clean).
    @ Non-zero exit on failure: r0=0x18, r1=non-zero-code (legacy form).
    cbz     r0, exit_ok
    @ Failure exit: code in r0
    mov     r1, r0
    movs    r0, #0x18
    bkpt    #0xAB
    b       hang
exit_ok:
    movs    r0, #0x18
    ldr     r1, =0x20026
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
