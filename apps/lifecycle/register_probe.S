.syntax unified
.cpu cortex-m4
.thumb

.extern os_reclaim_count
.extern os_yield

.macro probe_prologue
    push    {r3-r11, lr}
    movs    r4, #0x44
    movs    r5, #0x55
    movs    r6, #0x66
    movs    r7, #0x77
    movs    r0, #0x88
    mov     r8, r0
    movs    r0, #0x99
    mov     r9, r0
    movs    r0, #0xaa
    mov     r10, r0
    movs    r0, #0xbb
    mov     r11, r0
.endm

.macro verify_registers failure
    cmp     r4, #0x44
    bne     \failure
    cmp     r5, #0x55
    bne     \failure
    cmp     r6, #0x66
    bne     \failure
    cmp     r7, #0x77
    bne     \failure
    mov     r0, r8
    cmp     r0, #0x88
    bne     \failure
    mov     r0, r9
    cmp     r0, #0x99
    bne     \failure
    mov     r0, r10
    cmp     r0, #0xaa
    bne     \failure
    mov     r0, r11
    cmp     r0, #0xbb
    bne     \failure
.endm

.section .text.lifecycle_yield_register_probe,"ax",%progbits
.align 2
.global lifecycle_yield_register_probe
.type lifecycle_yield_register_probe,%function
.thumb_func
lifecycle_yield_register_probe:
    probe_prologue
    bl      os_yield
    verify_registers .Lyield_failure
    movs    r0, #1
    b       .Lyield_done
.Lyield_failure:
    movs    r0, #0
.Lyield_done:
    pop     {r3-r11, pc}
.size lifecycle_yield_register_probe, .-lifecycle_yield_register_probe

.section .text.lifecycle_preempt_register_probe,"ax",%progbits
.align 2
.global lifecycle_preempt_register_probe
.type lifecycle_preempt_register_probe,%function
.thumb_func
lifecycle_preempt_register_probe:
    probe_prologue
.Lwait_for_reclaim:
    bl      os_reclaim_count
    cmp     r0, #0
    beq     .Lwait_for_reclaim
    verify_registers .Lpreempt_failure
    movs    r0, #1
    b       .Lpreempt_done
.Lpreempt_failure:
    movs    r0, #0
.Lpreempt_done:
    pop     {r3-r11, pc}
.size lifecycle_preempt_register_probe, .-lifecycle_preempt_register_probe
