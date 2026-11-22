// Mode-switch glue between 64-bit host code and 32-bit guest code.
//
// Invariant: while running guest code, fs selector = per-thread LDT entry
// (base = TEB). While running host code, fs base = glibc TCB (set with
// wrfsbase; the selector is left as-is, which is fine on FSGSBASE kernels).
// The per-thread GuestCtx lives at TEB+0x1000, so right after a far jump into
// 64-bit mode it is reachable as %fs:0x1000+off.

#define CTX 0x1000
#define C_HOST_SP     0x00
#define C_HOST_FSBASE 0x08
#define C_GUEST_ESP   0x10
#define C_FS_SEL      0x14
#define C_EAX         0x18
#define C_EDX         0x1c
#define C_POP         0x20
#define C_FRET        0x24
#define C_FVAL        0x28

    .text
    .code64

// uint64_t call32_raw(GuestCtx *ctx /*rdi*/, uint32_t fn /*esi*/, uint32_t esp /*edx*/,
//                     uint32_t exit_stub /*ecx*/)
    .globl call32_raw
    .type call32_raw,@function
call32_raw:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    push C_HOST_SP(%rdi)          // previous host_sp (nesting)
    push %rdi                     // ctx
    mov %rsp, C_HOST_SP(%rdi)
    rdfsbase %rax
    mov %rax, C_HOST_FSBASE(%rdi)
    mov $0x2b, %eax
    mov %eax, %ds
    mov %eax, %es
    mov C_FS_SEL(%rdi), %eax
    mov %eax, %fs                 // fs base := TEB
    mov %edx, %esp                // guest stack (args already copied)
    sub $4, %esp
    mov %ecx, (%rsp)              // return address -> exit stub
    mov 0x54(%rdi), %ecx          // ctx->call_ecx (`this` for thiscall)
    pushq $0x23
    push %rsi
    lretq                         // enter 32-bit mode at fn
    .size call32_raw, .-call32_raw

// Reached from the 32-bit exit stub via ljmp 0x33. rsp = guest stack.
    .globl host_exit
host_exit:
    rdfsbase %r14
    add $CTX, %r14
    mov %eax, C_EAX(%r14)
    mov %edx, C_EDX(%r14)
    mov C_HOST_SP(%r14), %rsp
    mov C_HOST_FSBASE(%r14), %rcx
    wrfsbase %rcx
    mov C_EDX(%r14), %edx
    shl $32, %rdx
    mov C_EAX(%r14), %eax
    or %rdx, %rax
    pop %rdi                      // ctx
    pop C_HOST_SP(%rdi)
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

// Reached from an API stub: push id; ljmp 0x33:pad -> here.
// Guest stack: [esp]=id, [esp+4]=return address, [esp+8...]=args.
    .globl host_dispatch
host_dispatch:
    mov %esp, %r13d
    rdfsbase %r14
    add $CTX, %r14
    mov %ebx, 0x40(%r14)          // guest callee-saved regs (for exception contexts)
    mov %ebp, 0x44(%r14)
    mov %esi, 0x48(%r14)
    mov %edi, 0x4c(%r14)
    mov %ecx, 0x50(%r14)          // thiscall `this`
    mov %eax, C_EAX(%r14)         // default return = guest's eax (transparent fns, e.g. _chkesp)
    mov C_HOST_SP(%r14), %rsp     // host_sp is 8 mod 16 (see call32_raw)
    sub $8, %rsp
    push %rsi                     // guest esi/edi are callee-saved for stdcall
    push %rdi
    mov %r13d, C_GUEST_ESP(%r14)
    mov C_HOST_FSBASE(%r14), %rax
    wrfsbase %rax
    mov (%r13), %edi              // id
    mov %r14, %rsi                // ctx
    call api_dispatch
    pop %rdi
    pop %rsi
    add $8, %rsp
    mov C_POP(%r14), %edx
    mov 4(%r13), %ecx             // return address
    lea 8(%r13,%rdx), %r13        // new guest esp
    cmpl $0, C_FRET(%r14)
    je 1f
    fldl C_FVAL(%r14)
1:
    mov C_FS_SEL(%r14), %eax
    mov %eax, %fs                 // fs base := TEB
    mov C_EAX(%r14), %eax
    mov C_EDX(%r14), %edx
    mov %r13, %rsp
    pushq $0x23
    push %rcx
    lretq

// Signal entry: kernel delivers in 64-bit mode with whatever fs base was live.
// If it is a TEB (below 2GB), switch to the host TLS before running C code.
// void sig_entry(int sig /*edi*/, siginfo_t * /*rsi*/, void *uc /*rdx*/)
    .globl sig_entry
    .type sig_entry,@function
sig_entry:
    .cfi_startproc
    push %rbx
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %rbx, 0
    push %r12
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r12, 0
    push %r13
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r13, 0
    rdfsbase %rbx
    xor %r12d, %r12d
    mov $0x80000000, %eax
    cmp %rax, %rbx
    jae 1f
    mov $1, %r12d
    mov CTX+C_HOST_FSBASE(%rbx), %rax
    wrfsbase %rax
1:
    call sig_handler_c
    test %r12d, %r12d
    jz 2f
    wrfsbase %rbx
2:
    pop %r13
    .cfi_adjust_cfa_offset -8
    pop %r12
    .cfi_adjust_cfa_offset -8
    pop %rbx
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
    .size sig_entry, .-sig_entry

    .section .note.GNU-stack,"",@progbits
