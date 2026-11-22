#include "api.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "log.h"
#include "thread.h"

struct Thread *g_main_thread;

extern const ApiDef __start_wineless_api[], __stop_wineless_api[];

typedef struct {
    const ApiDef *def; // NULL -> unimplemented
    char label[96];    // "dll!name"
    ApiFn fn;
    uint8_t nargs, flags;
    uint32_t stub;
    uint64_t calls;
} ApiEntry;

#define MAX_ENTRIES 32768 // fixed so dispatch can read without locking
static ApiEntry entries[MAX_ENTRIES];
static uint32_t nentries;
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;

void api_norm_dll(const char *in, char *out, int outsz) {
    int i = 0;
    for (; in[i] && i < outsz - 1; i++) out[i] = tolower((unsigned char)in[i]);
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot && (!strcmp(dot, ".dll") || !strcmp(dot, ".drv"))) *dot = 0;
}

static uint32_t add_entry_at(const ApiDef *def, const char *label, ApiFn fn, int nargs, int flags, uint32_t at);
static uint32_t add_entry(const ApiDef *def, const char *label, ApiFn fn, int nargs, int flags) {
    return add_entry_at(def, label, fn, nargs, flags, 0);
}
static uint32_t add_entry_at(const ApiDef *def, const char *label, ApiFn fn, int nargs, int flags, uint32_t at) {
    pthread_mutex_lock(&api_lock);
    if (nentries == MAX_ENTRIES) fatal("api: too many entries");
    uint32_t id = nentries++;
    ApiEntry *e = &entries[id];
    memset(e, 0, sizeof *e);
    e->def = def;
    snprintf(e->label, sizeof e->label, "%s", label);
    e->fn = fn;
    e->nargs = nargs;
    e->flags = flags;
    if (at) {
        cpu_write_api_stub(at, id);
        e->stub = at;
    } else {
        e->stub = cpu_make_api_stub(id);
    }
    pthread_mutex_unlock(&api_lock);
    return id;
}

// Place the thunk for `d` at `addr` (inside a builtin DLL's image) and return it.
uint32_t api_bind_def_at(const ApiDef *d, uint32_t addr) {
    if (d->flags & API_DATA) return d->fn(NULL);
    char label[96];
    if (d->name) snprintf(label, sizeof label, "%s!%s", d->dll, d->name);
    else snprintf(label, sizeof label, "%s!#%d", d->dll, d->ordinal);
    add_entry_at(d, label, d->fn, d->nargs, d->flags, addr);
    return addr;
}

void api_init(void) {
    add_entry(NULL, "<none>", NULL, 0, 0); // id 0 = "no API"
    size_t n = __stop_wineless_api - __start_wineless_api;
    LOGD("api: %zu implementations registered", n);
}

static const ApiDef *find_def(const char *dll, const char *name, int ordinal) {
    for (const ApiDef *d = __start_wineless_api; d < __stop_wineless_api; d++) {
        if (strcmp(d->dll, dll)) continue;
        if (name && !strcmp(d->name, name)) return d;
        if (!name && ordinal && d->ordinal == ordinal) return d;
    }
    return NULL;
}

uint32_t api_resolve(const char *dllin, const char *name, int ordinal, int stub_missing) {
    char dll[64];
    api_norm_dll(dllin, dll, sizeof dll);
    const ApiDef *d = find_def(dll, name, ordinal);
    // reuse an existing thunk
    pthread_mutex_lock(&api_lock);
    for (uint32_t i = 1; i < nentries; i++) {
        ApiEntry *e = &entries[i];
        if ((d && e->def == d) ||
            (!d && !e->def && !e->fn && strncmp(e->label, dll, strlen(dll)) == 0 &&
             e->label[strlen(dll)] == '!' &&
             (name ? !strcmp(e->label + strlen(dll) + 1, name)
                   : atoi(e->label + strlen(dll) + 2) == ordinal))) {
            uint32_t s = e->stub;
            pthread_mutex_unlock(&api_lock);
            return s;
        }
    }
    pthread_mutex_unlock(&api_lock);
    char label[96];
    if (name) snprintf(label, sizeof label, "%s!%s", dll, name);
    else snprintf(label, sizeof label, "%s!#%d", dll, ordinal);
    if (d && (d->flags & API_DATA)) return d->fn(NULL);
    if (!d) {
        if (!stub_missing) return 0;
        LOGD("api: no implementation for %s (stubbed)", label);
        return entries[add_entry(NULL, label, NULL, 0, 0)].stub;
    }
    return entries[add_entry(d, label, d->fn, d->nargs, d->flags)].stub;
}

uint32_t api_register_dynamic(const char *label, ApiFn fn, int nargs, int flags) {
    return entries[add_entry(NULL, label, fn, nargs, flags)].stub;
}

const char *api_describe(uint32_t id) { return id < nentries ? entries[id].label : "?"; }

static __thread GuestCtx *cur_ctx;
void api_ret_edx(uint32_t edx) { cur_ctx->edx = edx; }
void api_ret_double(double v) { cur_ctx->fret = 1; cur_ctx->fval = v; }
void api_ret_pop(uint32_t bytes) { cur_ctx->pop = bytes; }

void api_dispatch(uint32_t id, GuestCtx *ctx) {
    GuestCtx *prev = cur_ctx;
    cur_ctx = ctx;
    // If guest code jumped out of a host->guest callback (SEH unwinding), the
    // host frames of that call are dead: reclaim their stack for future calls.
    Thread *th = ctx->thread;
    while (th->nest_depth > 0 && ctx->guest_esp > th->nest[th->nest_depth - 1].guest_esp) {
        th->nest_depth--;
        ctx->host_sp = th->nest[th->nest_depth].prev_host_sp;
        LOGD("api: guest unwound past a callback (depth %d)", th->nest_depth);
    }
    uint32_t *sp = G2H(ctx->guest_esp); // [0]=id [1]=ret [2..]=args
    if (id >= nentries) fatal("api: bad id %u from 0x%x", id, sp[1]);
    ApiEntry *e = &entries[id];
    ctx->api_id = id;
    if (!e->fn) fatal("UNIMPLEMENTED %s called from 0x%08x", e->label, sp[1]);
    ctx->pop = (e->flags & API_CDECL) ? 0 : e->nargs * 4;
    ctx->fret = 0;
    ctx->edx = 0;
    e->calls++;
    if (ctx->thread == g_main_thread) g_api_calls_main++;
    uint32_t *args = sp + 2;
    if (g_log_level >= LOG_TRACE) {
        char buf[256];
        int n = 0;
        for (int i = 0; i < e->nargs && n < 200; i++)
            n += snprintf(buf + n, sizeof buf - n, "%s%x", i ? ", " : "", args[i]);
        LOGT("-> %s(%s) from %08x", e->label, buf, sp[1]);
    }
    ctx->eax = e->fn(args);
    LOGT("<- %s = %x", e->label, ctx->eax);
    cur_ctx = prev;
}
