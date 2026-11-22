#pragma once
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>

#include "cpu.h"

// Windows TEB offsets we fill in
#define TEB_EXCEPTION_LIST 0x00
#define TEB_STACK_BASE 0x04
#define TEB_STACK_LIMIT 0x08
#define TEB_SELF 0x18
#define TEB_PID 0x20
#define TEB_TID 0x24
#define TEB_TLS_POINTER 0x2C
#define TEB_PEB 0x30
#define TEB_LAST_ERROR 0x34
#define TEB_TLS_SLOTS 0xE10
#define TLS_MINIMUM_AVAILABLE 64

typedef struct Thread {
    GuestCtx *ctx;
    uint32_t teb;
    uint32_t stack_base, stack_size; // guest stack reservation
    uint32_t tid;
    pthread_t pthread;
    jmp_buf exit_jmp;     // ExitThread unwinds to here
    uint32_t exit_code;
    uint32_t start, param;
    int suspended;
    pthread_mutex_t susp_lock;
    pthread_cond_t susp_cond;
    int priority;
    uint32_t handle;      // handle object for this thread (for waits)
    // nested host->guest calls; lets us recover when guest code unwinds past
    // them (SEH catch blocks jump out of callbacks without returning)
    struct { uint32_t guest_esp; uint64_t prev_host_sp; } nest[64];
    int nest_depth;
} Thread;

extern uint32_t g_peb;

void thread_init_process(void);
Thread *thread_current(void);
// Create guest thread state for the calling pthread (main thread) or a new one.
Thread *thread_setup_current(uint32_t stack_size);
// CreateThread backend. Returns Thread (already running unless suspended).
Thread *thread_spawn(uint32_t start, uint32_t param, uint32_t stack_size, int suspended);
_Noreturn void thread_exit_current(uint32_t code);

static inline uint32_t *teb_u32(Thread *t, uint32_t off) { return (uint32_t *)(uintptr_t)(t->teb + off); }
// Queue a stdcall guest callback to run on the callback worker thread.
void guest_callback_post(uint32_t fn, int nargs, const uint32_t *args);
void set_last_error(uint32_t e);
uint32_t get_last_error(void);
