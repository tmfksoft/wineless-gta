// Proof of concept: a 64-bit Linux process running 32-bit x86 code (CS=0x23),
// with a Windows-style TEB reachable via fs:, calling a stdcall "import" that
// is serviced by ordinary 64-bit C code, then returning to the host.
//
// Build: gcc -O2 -o modeswitch modeswitch.c
#define _GNU_SOURCE
#include <asm/ldt.h>
#include <asm/prctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

#define USER32_CS 0x23
#define USER64_CS 0x33
#define USER_DS   0x2b
#define TEB_SEL   ((0 << 3) | 4 | 3) // LDT entry 0, RPL 3

// ---- globals shared with the asm glue (all fixed addresses, non-TLS) ----
uint64_t g_host_sp;      // host stack to use while servicing guest calls
uint64_t g_host_fsbase;  // glibc's TLS base, restored on every 32->64 switch
uint32_t g_guest_esp;    // initial guest stack for call32
uint64_t g_exit64;       // low-memory 64-bit landing pad addresses
uint32_t g_exit32_stub;  // 32-bit return address that leaves guest mode

// guest_dispatch(id, esp) -> low32 = eax, high32 = bytes of args to pop (stdcall)
uint64_t guest_dispatch(uint32_t id, uint32_t *esp);

// call32(fn): run 32-bit function at fn on g_guest_esp; returns its eax.
uint32_t call32(uint32_t fn);

__asm__(
    ".text\n"
    ".globl call32\n"
    "call32:\n"
    "  push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15\n"
    "  push g_host_sp(%rip)\n"            // allow nesting (callbacks)
    "  mov %rsp, g_host_sp(%rip)\n"
    "  mov $" "0x2b" ", %eax; mov %eax, %ds; mov %eax, %es\n"
    "  mov $" "7" ", %eax; mov %eax, %fs\n"   // TEB selector -> fs base = TEB
    "  mov g_guest_esp(%rip), %esp\n"     // zero-extends into rsp
    "  mov g_exit32_stub(%rip), %eax\n"
    "  sub $4, %esp; mov %eax, (%rsp)\n"  // 32-bit return address
    "  pushq $0x23\n"
    "  push %rdi\n"
    "  lretq\n"                           // -> 32-bit mode at fn

    ".globl host_exit\n"                  // arrives in 64-bit mode, rsp=guest
    "host_exit:\n"
    "  mov %eax, %r12d\n"
    "  mov g_host_sp(%rip), %rsp\n"
    "  xor %eax, %eax; mov %eax, %fs\n"
    "  mov g_host_fsbase(%rip), %rax; wrfsbase %rax\n"
    "  mov %r12d, %eax\n"
    "  pop g_host_sp(%rip)\n"
    "  pop %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx\n"
    "  ret\n"

    ".globl host_dispatch\n"              // rsp=guest: [id][ret][args...]
    "host_dispatch:\n"
    "  mov %esp, %r13d\n"                 // guest esp (callee-saved in SysV)
    "  mov g_host_sp(%rip), %rsp\n"
    "  push %rsi; push %rdi\n"            // guest esi/edi are callee-saved in stdcall
    "  xor %eax, %eax; mov %eax, %fs\n"
    "  mov g_host_fsbase(%rip), %rax; wrfsbase %rax\n"
    "  mov (%r13), %edi\n"                // id
    "  lea 4(%r13), %rsi\n"               // &ret, args follow
    "  call guest_dispatch\n"
    "  pop %rdi; pop %rsi\n"
    "  mov %rax, %rdx; shr $32, %rdx\n"   // rdx = pop bytes
    "  mov 4(%r13), %ecx\n"               // return address
    "  lea 8(%r13,%rdx), %r13\n"          // new guest esp
    "  mov $7, %r8d; mov %r8d, %fs\n"
    "  mov %r13, %rsp\n"
    "  pushq $0x23\n"
    "  push %rcx\n"
    "  lretq\n");

extern char host_exit[], host_dispatch[];

// ---- tiny x86 emitter for low-memory stubs ----
static uint8_t *low; static size_t lowpos;
static uint32_t emit(const void *b, size_t n) {
    uint32_t a = (uint32_t)(uintptr_t)(low + lowpos);
    memcpy(low + lowpos, b, n); lowpos += n; return a;
}
// 64-bit landing pad in low memory: movabs r11, target; jmp r11
// (r11 is scratch in both ABIs; rax must survive because it carries eax)
static uint32_t pad64(void *target) {
    uint8_t b[13] = {0x49, 0xBB}; uint64_t t = (uint64_t)target;
    memcpy(b + 2, &t, 8); b[10] = 0x41; b[11] = 0xFF; b[12] = 0xE3; return emit(b, 13);
}
// 32-bit: [push id;] ljmp 0x33:pad
static uint32_t stub32(int has_id, uint32_t id, uint32_t pad) {
    uint8_t b[12]; size_t n = 0;
    if (has_id) { b[n++] = 0x68; memcpy(b + n, &id, 4); n += 4; }
    b[n++] = 0xEA; memcpy(b + n, &pad, 4); n += 4; b[n++] = USER64_CS; b[n++] = 0;
    return emit(b, n);
}

// ---- the "Win32 API" implemented natively ----
uint64_t guest_dispatch(uint32_t id, uint32_t *esp) {
    uint32_t *args = esp + 1;
    switch (id) {
    case 1: // int __stdcall FakeApi(uint32 a, uint32 tebself)
        printf("  [host] FakeApi(a=0x%x, teb->Self=0x%x) called from 32-bit 0x%x\n",
               args[0], args[1], esp[0]);
        return (uint64_t)(args[0] + 1) | (8ull << 32);
    }
    fprintf(stderr, "unimplemented import %u\n", id); exit(1);
}

int main(void) {
    // Low 4GB region for TEB, stubs, guest stack, "game code".
    low = mmap((void *)0x10000000, 0x100000, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (low == MAP_FAILED) { perror("mmap"); return 1; }

    // Fake TEB: NT_TIB.Self at +0x18
    uint32_t teb = (uint32_t)(uintptr_t)low; lowpos = 0x1000;
    *(uint32_t *)(low + 0x18) = teb;

    struct user_desc d = {.entry_number = 0, .base_addr = teb, .limit = 0xfff,
                          .seg_32bit = 1, .useable = 1};
    if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) { perror("modify_ldt"); return 1; }
    syscall(SYS_arch_prctl, ARCH_GET_FS, &g_host_fsbase);

    g_exit32_stub = stub32(0, 0, pad64(host_exit));
    uint32_t disp = pad64(host_dispatch);
    uint32_t imp_thunk = stub32(1, 1, disp);

    // IAT slot, like a PE import address table entry
    uint32_t iat = emit(&imp_thunk, 4);

    // "Game" code (32-bit):
    //   mov eax, fs:[0x18] ; push eax ; push 0x1233 ; call [iat] ; ret
    uint8_t code[] = {0x64, 0xA1, 0x18, 0, 0, 0, 0x50, 0x68, 0x33, 0x12, 0, 0,
                      0xFF, 0x15, 0, 0, 0, 0, 0xC3};
    memcpy(code + 14, &iat, 4);
    uint32_t fn = emit(code, sizeof code);

    g_guest_esp = (uint32_t)(uintptr_t)(low + 0x100000 - 16);

    printf("host: entering 32-bit code at 0x%x (TEB 0x%x)\n", fn, teb);
    uint32_t r = call32(fn);
    errno = 42; // errno lives in glibc TLS (fs:) -- proves fs base was restored
    printf("host: back in 64-bit, eax=0x%x, TLS ok (errno=%d)\n", r, errno);
    return r == 0x1234 ? 0 : 2;
}
