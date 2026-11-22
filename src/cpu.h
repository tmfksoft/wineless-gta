#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER32_CS 0x23
#define USER64_CS 0x33
#define USER_DS 0x2b

// Per-thread switch state, placed at TEB + CTX_OFF so the asm glue can find it
// through the fs base while in 64-bit mode right after a far jump.
#define TEB_SIZE 0x2000
#define CTX_OFF 0x1000

typedef struct GuestCtx {
    uint64_t host_sp;     // 0x00 host stack pointer to use while servicing the guest
    uint64_t host_fsbase; // 0x08 glibc TLS base for this thread
    uint32_t guest_esp;   // 0x10 guest esp at the last 32->64 transition
    uint32_t fs_sel;      // 0x14 LDT selector whose base is this thread's TEB
    uint32_t eax, edx;    // 0x18 0x1c return registers to the guest
    uint32_t pop;         // 0x20 bytes of stack args to pop (stdcall)
    uint32_t fret;        // 0x24 nonzero -> push fval on x87 stack
    double fval;          // 0x28
    struct Thread *thread;// 0x30
    uint32_t teb;         // 0x38
    uint32_t api_id;      // 0x3c id of the API being serviced (diagnostics)
    uint32_t g_ebx, g_ebp, g_esi, g_edi; // 0x40 guest callee-saved regs at API entry
    uint32_t g_ecx;       // 0x50 guest ecx at API entry (`this` for thiscall methods)
    uint32_t call_ecx;    // 0x54 ecx to load when call32 enters guest code
} GuestCtx;

_Static_assert(offsetof(GuestCtx, guest_esp) == 0x10, "ctx layout");
_Static_assert(offsetof(GuestCtx, fval) == 0x28, "ctx layout");
_Static_assert(offsetof(GuestCtx, thread) == 0x30, "ctx layout");
_Static_assert(offsetof(GuestCtx, g_ebx) == 0x40, "ctx layout");
_Static_assert(offsetof(GuestCtx, g_ecx) == 0x50, "ctx layout");
_Static_assert(offsetof(GuestCtx, call_ecx) == 0x54, "ctx layout");

void cpu_init(void);
// Copy 32-bit guest code into the executable stub area; returns its address.
uint32_t cpu_emit_code(const void *code, uint32_t n);

// Allocate an LDT data segment based at `teb`; returns selector.
uint16_t cpu_alloc_teb_selector(uint32_t teb);

// Call 32-bit function `fn` with stdcall/cdecl args on the current thread's guest
// stack. Returns edx:eax. Safe to nest from inside an API implementation.
uint64_t call32(GuestCtx *ctx, uint32_t fn, int nargs, const uint32_t *args);
// Same for an MSVC thiscall method: `self` in ecx, args on the stack (callee pops).
uint64_t call32_this(GuestCtx *ctx, uint32_t fn, uint32_t self, int nargs, const uint32_t *args);

// Emit 32-bit stub `push id; ljmp 0x33:dispatch` and return its guest address.
uint32_t cpu_make_api_stub(uint32_t id);
// Write the same 12-byte stub at a given (writable) guest address.
void cpu_write_api_stub(uint32_t addr, uint32_t id);

// Implemented by api.c
void api_dispatch(uint32_t id, GuestCtx *ctx);

// Install crash diagnostics (signal handlers that fix up fs first).
void cpu_install_signal_handlers(void);
void cpu_thread_sigaltstack(void);
void cpu_start_watchdog(void);
extern volatile uint64_t g_api_calls_main;
extern struct Thread *g_main_thread;
