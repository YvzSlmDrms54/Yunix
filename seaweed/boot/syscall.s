/* seaweed/boot/syscall.s - the doors between ring 3 and the kernel.
 *
 *   syscall_stub   where "int $0x80" lands. Saves the program's registers
 *                  in the same layout as every other interrupt (struct
 *                  registers), calls syscall_handler, and returns with
 *                  whatever the handler left in the saved EAX.
 *
 *   user_enter     drops from the kernel into a ring 3 program, and
 *                  returns - later - with its exit code.
 *
 *   user_leave     ends the program from inside the kernel (exit, or a
 *                  crash) and makes user_enter return.
 */

.section .text

.extern syscall_handler
.extern tss_set_kernel_stack

.global syscall_stub
.type syscall_stub, @function
syscall_stub:
    push $0                 /* no error code */
    push $0x80              /* interrupt number */
    pusha
    mov %ds, %ax
    push %eax

    /* Kernel data segments in ds and es only. fs and gs stay as they are:
     * the kernel never uses them, and a program keeps its thread-local
     * storage segment there - overwriting gs would lose it. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es

    push %esp
    call syscall_handler
    add $4, %esp

    pop %eax
    mov %ax, %ds
    mov %ax, %es

    popa                    /* EAX comes back as the handler set it */
    add $8, %esp
    iret

/* --- entering and leaving ring 3 --- */

.section .bss
.align 4
saved_esp:
    .skip 4

.section .text

/* uint32_t user_enter(uint32_t entry, uint32_t user_stack)
 *
 * Works like setjmp: it saves the registers C expects to survive a call,
 * and the stack pointer, so that user_leave can come back here from deep
 * inside an interrupt handler later. */
.global user_enter
.type user_enter, @function
user_enter:
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov %esp, saved_esp

    /* Interrupts from ring 3 must land on a kernel stack. Use this one,
     * just below what we saved: anything pushed there is thrown away
     * when user_leave resets ESP. */
    push %esp
    call tss_set_kernel_stack
    add $4, %esp

    mov 20(%esp), %ecx      /* entry: 4 saved registers + return address */
    mov 24(%esp), %edx      /* the program's stack */

    mov $0x23, %ax          /* user data segment, privilege 3 */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs            /* the program may point fs/gs at its own */
    mov %ax, %gs            /* thread-local storage later */

    /* iret pops EIP, CS, EFLAGS - and, because CS says ring 3, also ESP
     * and SS. So build that frame and let iret do the jump. */
    push $0x23              /* SS */
    push %edx               /* ESP */
    push $0x202             /* EFLAGS: interrupts on (bit 9), bit 1 always 1 */
    push $0x1B              /* CS: user code, privilege 3 */
    push %ecx               /* EIP */

    /* The Linux ABI says EDX holds a function for atexit, or 0. Clear
     * everything so no kernel values leak into the program. */
    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    xor %ebp, %ebp
    iret

/* void user_leave(uint32_t exit_code) - does not return. */
.global user_leave
.type user_leave, @function
user_leave:
    mov 4(%esp), %eax       /* the exit code, as user_enter's result */
    mov saved_esp, %esp     /* back onto user_enter's frame */

    mov $0x10, %cx
    mov %cx, %ds
    mov %cx, %es
    mov %cx, %fs
    mov %cx, %gs

    pop %edi
    pop %esi
    pop %ebx
    pop %ebp

    /* We came here from an interrupt or a system call, both of which
     * turned interrupts off. The kernel code we return to had them on. */
    sti
    ret

.section .note.GNU-stack,"",@progbits
