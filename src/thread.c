#include "thread.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "handles.h"
#include "heap.h"
#include "log.h"
#include "lowmem.h"
#include "pe.h"
#include "win32/win32.h"

uint32_t g_peb;
static __thread Thread *cur_thread;

Thread *thread_current(void) { return cur_thread; }

void set_last_error(uint32_t e) {
    if (cur_thread) *teb_u32(cur_thread, TEB_LAST_ERROR) = e;
}
uint32_t get_last_error(void) { return cur_thread ? *teb_u32(cur_thread, TEB_LAST_ERROR) : 0; }

void thread_init_process(void) {
    // PEB: BeingDebugged @2, ImageBaseAddress @8, ProcessHeap @0x18, OS version @0xA4..
    g_peb = lm_alloc(0x1000, LM_GRAN, PAGE_READWRITE, LM_TAG_SYSTEM);
    uint8_t *peb = G2H(g_peb);
    *(uint32_t *)(peb + 0x08) = 0x400000;
    *(uint32_t *)(peb + 0x18) = 0x00150000; // fake heap handle (see HeapCreate)
    *(uint32_t *)(peb + 0xA4) = 5;          // OSMajorVersion (XP)
    *(uint32_t *)(peb + 0xA8) = 1;
    *(uint16_t *)(peb + 0xAC) = 2600;
    *(uint32_t *)(peb + 0xB0) = 2;          // VER_PLATFORM_WIN32_NT
    *(uint32_t *)(peb + 0x64) = 1;          // NumberOfProcessors
}

// Signals with handlers must never run on a guest thread while fs points at the
// TEB unless they go through sig_entry, so block everything asynchronous here.
static void block_async_signals(void) {
    sigset_t s;
    sigfillset(&s);
    int keep[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGINT, SIGTERM, SIGQUIT, SIGUSR2};
    for (unsigned i = 0; i < sizeof keep / sizeof *keep; i++) sigdelset(&s, keep[i]);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
}

static Thread *thread_alloc(uint32_t stack_size) {
    Thread *t = calloc(1, sizeof *t);
    if (!stack_size) stack_size = 0x200000; // exe's SizeOfStackReserve
    // We commit the whole stack up front rather than growing it on guard-page
    // faults like Windows, so keep a 2 MB floor for exes that reserve less.
    if (stack_size < 0x200000) stack_size = 0x200000;
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    t->stack_size = stack_size;
    // guard page at the bottom stays reserved/uncommitted
    t->stack_base = lm_reserve(0, stack_size, LM_GRAN, LM_TAG_STACK, true);
    if (!t->stack_base || !lm_commit(t->stack_base + LM_PAGE, stack_size - LM_PAGE, PAGE_READWRITE))
        fatal("thread: cannot allocate guest stack");
    t->teb = lm_alloc(TEB_SIZE, LM_GRAN, PAGE_READWRITE, LM_TAG_SYSTEM);
    if (!t->teb) fatal("thread: cannot allocate TEB");
    *teb_u32(t, TEB_EXCEPTION_LIST) = 0xFFFFFFFF;
    *teb_u32(t, TEB_STACK_BASE) = t->stack_base + stack_size;
    *teb_u32(t, TEB_STACK_LIMIT) = t->stack_base + LM_PAGE;
    *teb_u32(t, TEB_SELF) = t->teb;
    *teb_u32(t, TEB_PID) = getpid();
    *teb_u32(t, TEB_PEB) = g_peb;
    t->ctx = (GuestCtx *)G2H(t->teb + CTX_OFF);
    t->ctx->thread = t;
    t->ctx->teb = t->teb;
    t->ctx->fs_sel = cpu_alloc_teb_selector(t->teb);
    t->ctx->guest_esp = t->stack_base + stack_size - 16;
    pe_tls_setup_thread(t->teb); // static TLS blocks of loaded modules
    pthread_mutex_init(&t->susp_lock, NULL);
    pthread_cond_init(&t->susp_cond, NULL);
    return t;
}

// registry of guest threads (diagnostics)
Thread *g_threads[64];
int g_nthreads;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void bind_current(Thread *t) {
    t->tid = (uint32_t)syscall(SYS_gettid);
    *teb_u32(t, TEB_TID) = t->tid;
    t->pthread = pthread_self();
    cur_thread = t;
    pthread_mutex_lock(&reg_lock);
    if (g_nthreads < 64) g_threads[g_nthreads++] = t;
    pthread_mutex_unlock(&reg_lock);
    cpu_thread_sigaltstack();
    block_async_signals();
}

Thread *thread_setup_current(uint32_t stack_size) {
    Thread *t = thread_alloc(stack_size);
    bind_current(t);
    t->handle = handle_new_thread(t);
    return t;
}

static void *thread_main(void *arg) {
    Thread *t = arg;
    bind_current(t);
    pthread_mutex_lock(&t->susp_lock);
    while (t->suspended) pthread_cond_wait(&t->susp_cond, &t->susp_lock);
    pthread_mutex_unlock(&t->susp_lock);
    LOGD("thread %u start 0x%x(0x%x)", t->tid, t->start, t->param);
    if (!setjmp(t->exit_jmp)) {
        pe_thread_attach(); // DLL_THREAD_ATTACH for loaded DLLs
        uint32_t arg32 = t->param;
        t->exit_code = (uint32_t)call32(t->ctx, t->start, 1, &arg32);
    }
    LOGD("thread %u exit %u", t->tid, t->exit_code);
    handle_thread_finished(t);
    return NULL;
}

Thread *thread_spawn(uint32_t start, uint32_t param, uint32_t stack_size, int suspended) {
    Thread *t = thread_alloc(stack_size);
    t->start = start;
    t->param = param;
    t->suspended = suspended;
    t->handle = handle_new_thread(t);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pthread, &at, thread_main, t)) fatal("thread: pthread_create failed");
    pthread_attr_destroy(&at);
    return t;
}

void thread_exit_current(uint32_t code) {
    Thread *t = cur_thread;
    t->exit_code = code;
    longjmp(t->exit_jmp, 1);
}

// ---------------------------------------------------------------- callback worker
// Runs guest callbacks requested by host threads (multimedia timers, waveOut
// notifications): on Windows these run on system threads, so a dedicated guest
// thread is the closest equivalent.
typedef struct { uint32_t fn; int nargs; uint32_t args[6]; } CbItem;
#define CB_QUEUE 256
static CbItem cbq[CB_QUEUE];
static int cb_head, cb_tail, cb_started;
static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cb_cond = PTHREAD_COND_INITIALIZER;

static void *cb_main(void *arg) {
    Thread *t = thread_setup_current(0x40000);
    pe_thread_attach();
    for (;;) {
        pthread_mutex_lock(&cb_lock);
        while (cb_head == cb_tail) pthread_cond_wait(&cb_cond, &cb_lock);
        CbItem it = cbq[cb_head];
        cb_head = (cb_head + 1) % CB_QUEUE;
        pthread_mutex_unlock(&cb_lock);
        call32(t->ctx, it.fn, it.nargs, it.args);
    }
    return NULL;
}

void guest_callback_post(uint32_t fn, int nargs, const uint32_t *args) {
    pthread_mutex_lock(&cb_lock);
    if (!cb_started) {
        cb_started = 1;
        pthread_t th;
        pthread_create(&th, NULL, cb_main, NULL);
        pthread_detach(th);
    }
    int n = (cb_tail + 1) % CB_QUEUE;
    if (n != cb_head) {
        cbq[cb_tail].fn = fn;
        cbq[cb_tail].nargs = nargs > 6 ? 6 : nargs;
        for (int i = 0; i < cbq[cb_tail].nargs; i++) cbq[cb_tail].args[i] = args[i];
        cb_tail = n;
        pthread_cond_signal(&cb_cond);
    }
    pthread_mutex_unlock(&cb_lock);
}
