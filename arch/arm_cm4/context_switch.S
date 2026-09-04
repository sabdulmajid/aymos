.syntax unified
.cpu cortex-m4
.thumb

.extern os_arch_has_active_context
.extern os_kernel_panic
.extern os_pendsv_switch
.extern os_svc_dispatch

.section .text.SVC_Handler,"ax",%progbits
.align 2
.global SVC_Handler
.type SVC_Handler,%function
.thumb_func
SVC_Handler:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    mov     r1, lr
    b       os_svc_dispatch
.size SVC_Handler, .-SVC_Handler

.section .text.PendSV_Handler,"ax",%progbits
.align 2
.global PendSV_Handler
.type PendSV_Handler,%function
.thumb_func
PendSV_Handler:
    mrs     r2, primask
    cpsid   i
    ldr     r3, =os_arch_has_active_context
    ldr     r3, [r3]
    cbz     r3, .Lfirst_dispatch
    mrs     r0, psp
    stmdb   r0!, {r4-r11}
    b       .Lselect
.Lfirst_dispatch:
    movs    r0, #0
.Lselect:
    push    {r2, lr}
    bl      os_pendsv_switch
    pop     {r2, lr}
    cbz     r0, .Lbad_context
    ldmia   r0!, {r4-r11}
    msr     psp, r0
    movs    r1, #2
    msr     control, r1
    isb
    msr     primask, r2
    ldr     lr, =0xFFFFFFFD
    bx      lr
.Lbad_context:
    ldr     r0, =.Lpanic_reason
    bl      os_kernel_panic
    b       .
.size PendSV_Handler, .-PendSV_Handler

.section .rodata.context_switch,"a",%progbits
.align 2
.Lpanic_reason:
    .asciz "ARCH_PSP"
