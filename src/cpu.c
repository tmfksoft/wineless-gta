#include "cpu.h"

#include <asm/ldt.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "log.h"
#include "lowmem.h"
#include "thread.h"

extern char host_exit[], host_dispatch[], sig_entry[];
uint64_t call32_raw(GuestCtx *ctx, uint32_t fn, uint32_t esp, uint32_t exit_stub);

#define STUB_AREA_SIZE 0x100000u
static uint8_t *stub_area;
static uint32_t stub_pos;
static pthread_mutex_t stub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pad_dispatch, exit_stub;
static int next_ldt = 0;
static pthread_mutex_t ldt_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t emit(const void *b, uint32_t n) {
    if (stub_pos + n > STUB_AREA_SIZE) fatal("cpu: stub area exhausted");
    uint32_t a = H2G(stub_area + stub_pos);
    memcpy(stub_area + stub_pos, b, n);
    stub_pos += (n + 3) & ~3u;
    return a;
}

uint32_t cpu_emit_code(const void *b, uint32_t n) {
    pthread_mutex_lock(&stub_lock);
    uint32_t a = emit(b, n);
    pthread_mutex_unlock(&stub_lock);
    return a;
}

// 64-bit code in low memory: movabs r11, target; jmp r11  (rax carries eax)
static uint32_t pad64(void *target) {
    uint8_t b[13] = {0x49, 0xBB};
    uint64_t t = (uint64_t)target;
    memcpy(b + 2, &t, 8);
    b[10] = 0x41; b[11] = 0xFF; b[12] = 0xE3;
    return emit(b, sizeof b);
}

// 32-bit code: ljmp 0x33:pad
static uint32_t ljmp64(uint32_t pad) {
    uint8_t b[7] = {0xEA};
    memcpy(b + 1, &pad, 4);
    b[5] = USER64_CS; b[6] = 0;
    return emit(b, sizeof b);
}

// 32-bit stub: push id; ljmp 0x33:dispatch  (12 bytes)
void cpu_write_api_stub(uint32_t addr, uint32_t id) {
    uint8_t *b = G2H(addr);
    b[0] = 0x68;
    memcpy(b + 1, &id, 4);
    b[5] = 0xEA;
    memcpy(b + 6, &pad_dispatch, 4);
    b[10] = USER64_CS;
    b[11] = 0;
}

uint32_t cpu_make_api_stub(uint32_t id) {
    uint8_t b[12] = {0};
    pthread_mutex_lock(&stub_lock);
    uint32_t a = emit(b, sizeof b);
    pthread_mutex_unlock(&stub_lock);
    cpu_write_api_stub(a, id);
    return a;
}

void cpu_init(void) {
    if (!(getauxval(AT_HWCAP2) & (1 << 1))) // HWCAP2_FSGSBASE
        fatal("cpu: kernel/CPU lacks FSGSBASE support (need Linux 5.9+ on x86-64)");
    uint32_t a = lm_alloc(STUB_AREA_SIZE, LM_GRAN, PAGE_EXECUTE_READWRITE, LM_TAG_SYSTEM);
    if (!a) fatal("cpu: cannot allocate stub area");
    stub_area = G2H(a);
    pad_dispatch = pad64(host_dispatch);
    exit_stub = ljmp64(pad64(host_exit));
}

uint16_t cpu_alloc_teb_selector(uint32_t teb) {
    pthread_mutex_lock(&ldt_lock);
    int idx = next_ldt++;
    pthread_mutex_unlock(&ldt_lock);
    if (idx >= 8192) fatal("cpu: out of LDT entries");
    struct user_desc d = {.entry_number = idx, .base_addr = teb, .limit = TEB_SIZE - 1,
                          .seg_32bit = 1, .useable = 1};
    if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) fatal("cpu: modify_ldt failed");
    return (uint16_t)((idx << 3) | 4 | 3);
}

uint64_t call32_this(GuestCtx *ctx, uint32_t fn, uint32_t self, int nargs, const uint32_t *args) {
    uint32_t saved = ctx->call_ecx;
    ctx->call_ecx = self;
    uint64_t r = call32(ctx, fn, nargs, args);
    ctx->call_ecx = saved;
    return r;
}

uint64_t call32(GuestCtx *ctx, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t saved_esp = ctx->guest_esp;
    uint32_t saved_eax = ctx->eax, saved_edx = ctx->edx, saved_pop = ctx->pop, saved_fret = ctx->fret;
    double saved_fval = ctx->fval;
    uint32_t saved_api = ctx->api_id;
    // Leave a gap below the live guest stack (the stub's id + return address live there).
    uint32_t esp = (saved_esp - 64) & ~15u;
    esp -= 4 * nargs;
    for (int i = 0; i < nargs; i++) ((uint32_t *)G2H(esp))[i] = args[i];
    Thread *th = ctx->thread;
    int d = th->nest_depth;
    if (d < 64) {
        th->nest[d].guest_esp = saved_esp;
        th->nest[d].prev_host_sp = ctx->host_sp;
        th->nest_depth = d + 1;
    }
    uint64_t r = call32_raw(ctx, fn, esp, exit_stub);
    th->nest_depth = d;
    ctx->guest_esp = saved_esp;
    ctx->eax = saved_eax; ctx->edx = saved_edx; ctx->pop = saved_pop; ctx->fret = saved_fret;
    ctx->fval = saved_fval;
    ctx->api_id = saved_api;
    return r;
}

// ---------------------------------------------------------------- crashes
static const char *api_name_for_id(uint32_t id);

void sig_handler_c(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    uint16_t cs = g[REG_CSGSFS] & 0xffff;
    int dump_only = sig == SIGUSR2;
    if (dump_only) LOGE("==== watchdog: dumping thread state");
    LOGE("==== signal %d (%s) at %s ip=0x%llx addr=%p", sig, strsignal(sig),
         cs == USER32_CS ? "GUEST" : "HOST", (unsigned long long)g[REG_RIP], si->si_addr);
    LOGE("eax=%08llx ebx=%08llx ecx=%08llx edx=%08llx esi=%08llx edi=%08llx ebp=%08llx esp=%08llx",
         g[REG_RAX] & 0xffffffff, g[REG_RBX] & 0xffffffff, g[REG_RCX] & 0xffffffff,
         g[REG_RDX] & 0xffffffff, g[REG_RSI] & 0xffffffff, g[REG_RDI] & 0xffffffff,
         g[REG_RBP] & 0xffffffff, g[REG_RSP] & 0xffffffff);
    Thread *t = thread_current();
    if (t && t->ctx->api_id) LOGE("last API: %s", api_name_for_id(t->ctx->api_id));
    uint32_t esp = cs == USER32_CS ? (uint32_t)g[REG_RSP] : (t ? t->ctx->guest_esp : 0);
    if (esp && lm_is_committed(esp, 64, false)) {
        uint32_t *s = G2H(esp);
        LOGE("guest stack @%08x: %08x %08x %08x %08x %08x %08x %08x %08x", esp, s[0], s[1], s[2],
             s[3], s[4], s[5], s[6], s[7]);
        // crude return-address scan: values that point into the exe's code
        char buf[512]; int n = 0;
        for (int i = 0; i < 256 && n < 480 && lm_is_committed(esp + i * 4, 4, false); i++) {
            uint32_t v = s[i];
            if (v >= 0x401000 && v < 0x857000) n += snprintf(buf + n, sizeof buf - n, " %08x", v);
        }
        LOGE("possible return addresses:%s", buf);
    }
    if (cs != USER32_CS) {
        // host frames: print as binary offsets for addr2line
        void *bt[24];
        int n = backtrace(bt, 24);
        char buf[1024];
        int o = 0;
        for (int i = 0; i < n && o < 1000; i++) {
            Dl_info di;
            if (dladdr(bt[i], &di) && di.dli_fname)
                o += snprintf(buf + o, sizeof buf - o, " %s+0x%lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                              (unsigned long)((char *)bt[i] - (char *)di.dli_fbase));
        }
        LOGE("host backtrace:%s", buf);
    }
    if (dump_only) return;
    signal(sig, SIG_DFL);
    _exit(128 + sig);
}

// ---------------------------------------------------------------- watchdog
// WINELESS_WATCHDOG=1: if the main guest thread makes no API calls for 5s, dump where it is.
volatile uint64_t g_api_calls_main;
static pid_t watch_tid;
static void *watchdog(void *arg) {
    uint64_t last = 0;
    int dumped = 0;
    int sample = !strcmp(getenv("WINELESS_WATCHDOG"), "sample");
    for (;;) {
        if (sample) {
            extern Thread *g_threads[];
            extern int g_nthreads;
            sleep(2);
            for (int i = 0; i < g_nthreads; i++) {
                syscall(SYS_tgkill, getpid(), g_threads[i]->tid, SIGUSR2);
                usleep(20000);
            }
            continue;
        }
        sleep(5);
        uint64_t now = g_api_calls_main;
        if (now == last && !dumped) {
            syscall(SYS_tgkill, getpid(), watch_tid, SIGUSR2);
            dumped = 1;
        } else if (now != last) {
            dumped = 0;
        }
        last = now;
    }
    return NULL;
}
void cpu_start_watchdog(void) {
    if (!getenv("WINELESS_WATCHDOG")) return;
    watch_tid = (pid_t)syscall(SYS_gettid);
    pthread_t t;
    pthread_create(&t, NULL, watchdog, NULL);
    pthread_detach(t);
}

#include "api.h"
static const char *api_name_for_id(uint32_t id) { return api_describe(id); }

void cpu_thread_sigaltstack(void) {
    stack_t ss = {.ss_size = 256 * 1024};
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, NULL);
}

void cpu_install_signal_handlers(void) {
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGUSR2};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        struct sigaction sa = {0};
        sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))(void *)sig_entry;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, NULL);
    }
}
