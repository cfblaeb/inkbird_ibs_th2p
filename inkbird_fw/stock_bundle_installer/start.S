.syntax unified
.cpu cortex-m0
.thumb

.section .vectors, "a", %progbits
.global _vectors
_vectors:
    .word _stack_top
    .word Reset_Handler

.section .text.Reset_Handler, "ax", %progbits
.global Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    cpsid i
    ldr r0, =_stack_top
    msr msp, r0
    movs r0, #0
    msr psp, r0
    msr control, r0
    isb

    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
1:
    cmp r0, r1
    bcc 2f
    b 3f
2:
    str r2, [r0]
    adds r0, r0, #4
    b 1b
3:
    bl trace_reset_marker
    bl installer_main
4:
    b 4b

.size Reset_Handler, . - Reset_Handler
