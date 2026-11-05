@ Entry, exit, exception vectors and the context switch.
@ While a game thread runs, the SVC-mode stack pointer points at that
@ thread's saved context, so an SVC or fault saves straight into it.

    .arm
    .syntax unified
    .fpu vfp

    .equ MODE_USR, 0x10
    .equ MODE_ABT, 0x17
    .equ MODE_UND, 0x1B
    .equ MODE_SVC, 0x13
    .equ MODE_IRQ, 0x12
    .equ CTX_PC,    60
    .equ CTX_CPSR,  64
    .equ CTX_FPSCR, 68
    .equ CTX_D,     72
    .equ CTX_TLS,   200

    .section .text.entry, "ax", %progbits
    .global job_start
    .type   job_start, %function
job_start:
    cpsid   if
    push    {r4-r12, lr}
    ldr     r1, =host_sp
    str     sp, [r1]
    mrs     r2, cpsr
    ldr     r1, =host_cpsr
    str     r2, [r1]
    mov     r4, r0
    ldr     r5, =host_banked
    cps     #MODE_SVC
    stmia   r5!, {sp, lr}
    cps     #MODE_ABT
    stmia   r5!, {sp, lr}
    ldr     sp, =fault_frame
    cps     #MODE_UND
    stmia   r5!, {sp, lr}
    ldr     sp, =fault_frame
    cps     #MODE_IRQ
    stmia   r5!, {sp, lr}
    ldr     sp, =irq_scratch
    cps     #MODE_SVC
    ldr     sp, =sup_stack_top
    bl      mmu_enter
    mov     r0, r4
    bl      job_main
    .global sup_yield
    .type   sup_yield, %function
sup_yield:
    cpsid   if
    cps     #MODE_SVC
    ldr     sp, =sup_stack_top
    bl      mmu_leave
    ldr     r5, =host_banked
    cps     #MODE_SVC
    ldmia   r5!, {sp, lr}
    cps     #MODE_ABT
    ldmia   r5!, {sp, lr}
    cps     #MODE_UND
    ldmia   r5!, {sp, lr}
    cps     #MODE_IRQ
    ldmia   r5!, {sp, lr}
    ldr     r1, =host_cpsr
    ldr     r1, [r1]
    msr     cpsr_c, r1
    ldr     r1, =host_sp
    ldr     sp, [r1]
    pop     {r4-r12, lr}
    bx      lr
    .ltorg

    .global sup_resume
    .type   sup_resume, %function
sup_resume:
    ldr     r1, =sup_current_ctx
    str     r0, [r1]
    mov     sp, r0
    ldr     r1, [sp, #CTX_TLS]
    mcr     p15, 0, r1, c13, c0, 3
    add     r2, sp, #CTX_D
    vldmia  r2, {d0-d15}
    ldr     r1, [sp, #CTX_FPSCR]
    vmsr    fpscr, r1
    ldr     r0, [sp, #CTX_CPSR]
    msr     spsr_cxsf, r0
    ldr     lr, [sp, #CTX_PC]
    clrex
    ldmia   sp, {r0-r14}^
    nop
    movs    pc, lr

    .global vector_table
    .global vector_table_end
    .balign 4
vector_table:
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    ldr     pc, [pc, #0x18]
    .word   v_other, v_undef, v_svc, v_pabt, v_dabt, v_other, v_irq, v_other
vector_table_end:

v_svc:
    stmia   sp, {r0-r14}^
    nop
    str     lr, [sp, #CTX_PC]
    mrs     r0, spsr
    str     r0, [sp, #CTX_CPSR]
    vmrs    r1, fpscr
    str     r1, [sp, #CTX_FPSCR]
    add     r2, sp, #CTX_D
    vstmia  r2, {d0-d15}
    mov     r0, sp
    ldr     sp, =sup_stack_top
    bl      trap_svc
    b       sup_resume

v_irq:
    sub     lr, lr, #4
    str     r0, [sp]
    ldr     r0, =sup_current_ctx
    ldr     r0, [r0]
    stmia   r0, {r0-r14}^
    nop
    str     lr, [r0, #CTX_PC]
    mrs     r1, spsr
    str     r1, [r0, #CTX_CPSR]
    ldr     r1, [sp]
    str     r1, [r0]
    vmrs    r1, fpscr
    str     r1, [r0, #CTX_FPSCR]
    add     r2, r0, #CTX_D
    vstmia  r2, {d0-d15}
    cps     #MODE_SVC
    ldr     sp, =sup_stack_top
    bl      trap_irq
    b       sup_resume

v_dabt:
    sub     lr, lr, #8
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #0
    b       fault_common
v_pabt:
    sub     lr, lr, #4
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #1
    b       fault_common
v_undef:
    sub     lr, lr, #4
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #2
    b       fault_common
v_other:
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #5
fault_common:
    str     lr, [sp, #CTX_PC]
    mrs     r1, spsr
    str     r1, [sp, #CTX_CPSR]
    mov     r1, sp
    cps     #MODE_SVC
    ldr     sp, =sup_stack_top
    bl      trap_fault
1:  b       1b
    .ltorg

    .section .sup_state, "aw", %nobits
    .balign 8
host_sp:     .space 4
host_cpsr:   .space 4
host_banked: .space 32
irq_scratch: .space 8
    .global sup_current_ctx
sup_current_ctx: .space 4
    .balign 8
    .global fault_frame
fault_frame: .space 256
    .balign 8
sup_stack:   .space 0x8000
    .global sup_stack_top
sup_stack_top:
