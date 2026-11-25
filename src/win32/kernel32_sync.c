// kernel32: threads, synchronization, TLS
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "win32.h"

// ---------------------------------------------------------------- critical sections
// Guest CRITICAL_SECTION (24 bytes): DebugInfo, LockCount, RecursionCount,
// OwningThread, LockSemaphore, SpinCount. We keep a host mutex index in DebugInfo.
#define MAX_CS 16384
static pthread_mutex_t *cs_table[MAX_CS];
static int cs_next = 1;
static pthread_mutex_t cs_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(uint32_t *cs) {
    uint32_t i = cs[0];
    if (i >= 0x10000 || i == 0 || !cs_table[i]) {
        // Not initialized via InitializeCriticalSection (or zeroed): lazily create.
        pthread_mutex_lock(&cs_alloc_lock);
        i = cs[0];
        if (i >= 0x10000 || i == 0 || !cs_table[i]) {
            pthread_mutex_t *m = malloc(sizeof *m);
            pthread_mutexattr_t at;
            pthread_mutexattr_init(&at);
            pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(m, &at);
            int k = cs_next;
            for (int n = 0; n < MAX_CS && cs_table[k]; n++) k = k + 1 >= MAX_CS ? 1 : k + 1;
            if (cs_table[k]) fatal("too many critical sections");
            cs_table[k] = m;
            cs_next = k + 1 >= MAX_CS ? 1 : k + 1;
            cs[0] = k;
            i = k;
        }
        pthread_mutex_unlock(&cs_alloc_lock);
    }
    return cs_table[i];
}

static void cs_init(uint32_t *cs) {
    memset(cs, 0, 24);
    cs[1] = 0xFFFFFFFF;
    cs_get(cs);
}

uint32_t api_k32_init_cs(uint32_t cs) { cs_init(G2H(cs)); return TRUE; }
API(kernel32, InitializeCriticalSection, 1) { cs_init(PTR(0)); return 0; }
API(kernel32, InitializeCriticalSectionAndSpinCount, 2) { cs_init(PTR(0)); return TRUE; }
API(kernel32, DeleteCriticalSection, 1) {
    uint32_t *cs = PTR(0);
    pthread_mutex_lock(&cs_alloc_lock);
    if (cs[0] && cs[0] < MAX_CS && cs_table[cs[0]]) {
        pthread_mutex_destroy(cs_table[cs[0]]);
        free(cs_table[cs[0]]);
        cs_table[cs[0]] = NULL;
    }
    cs[0] = 0;
    pthread_mutex_unlock(&cs_alloc_lock);
    return 0;
}
API(kernel32, EnterCriticalSection, 1) {
    uint32_t *cs = PTR(0);
    pthread_mutex_lock(cs_get(cs));
    cs[2]++;
    cs[3] = thread_current()->tid;
    __atomic_add_fetch(&cs[1], 1, __ATOMIC_SEQ_CST);
    return 0;
}
API(kernel32, TryEnterCriticalSection, 1) {
    uint32_t *cs = PTR(0);
    if (pthread_mutex_trylock(cs_get(cs))) return FALSE;
    cs[2]++;
    cs[3] = thread_current()->tid;
    __atomic_add_fetch(&cs[1], 1, __ATOMIC_SEQ_CST);
    return TRUE;
}
API(kernel32, LeaveCriticalSection, 1) {
    uint32_t *cs = PTR(0);
    __atomic_sub_fetch(&cs[1], 1, __ATOMIC_SEQ_CST);
    if (--cs[2] == 0) cs[3] = 0;
    pthread_mutex_unlock(cs_get(cs));
    return 0;
}

// ---------------------------------------------------------------- interlocked
API(kernel32, InterlockedIncrement, 1) { return __atomic_add_fetch((int32_t *)PTR(0), 1, __ATOMIC_SEQ_CST); }
API(kernel32, InterlockedDecrement, 1) { return __atomic_sub_fetch((int32_t *)PTR(0), 1, __ATOMIC_SEQ_CST); }
API(kernel32, InterlockedExchange, 2) { return __atomic_exchange_n((uint32_t *)PTR(0), ARG(1), __ATOMIC_SEQ_CST); }
API(kernel32, InterlockedExchangeAdd, 2) { return __atomic_fetch_add((uint32_t *)PTR(0), ARG(1), __ATOMIC_SEQ_CST); }
API(kernel32, InterlockedCompareExchange, 3) {
    uint32_t exp = ARG(2);
    __atomic_compare_exchange_n((uint32_t *)PTR(0), &exp, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return exp;
}

// ---------------------------------------------------------------- TLS
static uint8_t tls_used[TLS_MINIMUM_AVAILABLE];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t tls_alloc_slot(void) {
    pthread_mutex_lock(&tls_lock);
    for (int i = 0; i < TLS_MINIMUM_AVAILABLE; i++)
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            return i;
        }
    pthread_mutex_unlock(&tls_lock);
    return 0xFFFFFFFF;
}
API(kernel32, TlsAlloc, 0) { return tls_alloc_slot(); }
API(kernel32, TlsFree, 1) {
    if (ARG(0) >= TLS_MINIMUM_AVAILABLE) return FALSE;
    tls_used[ARG(0)] = 0;
    return TRUE;
}
API(kernel32, TlsGetValue, 1) {
    if (ARG(0) >= TLS_MINIMUM_AVAILABLE) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    set_last_error(0);
    return *teb_u32(thread_current(), TEB_TLS_SLOTS + 4 * ARG(0));
}
API(kernel32, TlsSetValue, 2) {
    if (ARG(0) >= TLS_MINIMUM_AVAILABLE) return FALSE;
    *teb_u32(thread_current(), TEB_TLS_SLOTS + 4 * ARG(0)) = ARG(1);
    return TRUE;
}

// ---------------------------------------------------------------- threads
#define CREATE_SUSPENDED 4
API(kernel32, CreateThread, 6) {
    Thread *t = thread_spawn(ARG(2), ARG(3), ARG(1), (ARG(4) & CREATE_SUSPENDED) != 0);
    if (ARG(5)) {
        // tid is assigned when the pthread starts; give a provisional unique id
        *(uint32_t *)PTR(5) = t->handle;
    }
    LOGD("CreateThread(start=0x%x, param=0x%x, flags=0x%x) -> 0x%x", ARG(2), ARG(3), ARG(4), t->handle);
    return handle_dup(t->handle);
}
// QueueUserWorkItem(Function, Context, Flags): run Function(Context) on a worker
// thread. We don't pool, we just spawn a thread (the callback is a standard
// THREAD_START_ROUTINE, same ABI as CreateThread). Goldberg's steam_api uses this
// on its networking path. Returns TRUE on success.
API(kernel32, QueueUserWorkItem, 3) {
    Thread *t = thread_spawn(ARG(0), ARG(1), 0, 0);
    if (!t) { set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return TRUE;
}
API(kernel32, ResumeThread, 1) {
    Object *o = handle_get(ARG(0), OBJ_THREAD);
    if (!o) return 0xFFFFFFFF;
    Thread *t = o->thread.t;
    pthread_mutex_lock(&t->susp_lock);
    int prev = t->suspended;
    if (t->suspended > 0) t->suspended--;
    pthread_cond_broadcast(&t->susp_cond);
    pthread_mutex_unlock(&t->susp_lock);
    return prev;
}
API(kernel32, SuspendThread, 1) {
    // Real suspension of a running pthread is not supported; the game only uses this
    // around thread priority/sync tricks. Report success.
    LOGW("SuspendThread(0x%x): ignored", ARG(0));
    return 0;
}
API(kernel32, TerminateThread, 2) {
    LOGW("TerminateThread(0x%x): ignored", ARG(0));
    return TRUE;
}
API(kernel32, ExitThread, 1) { thread_exit_current(ARG(0)); }
API(kernel32, GetExitCodeThread, 2) {
    Object *o = handle_get(ARG(0), OBJ_THREAD);
    if (!o) return FALSE;
    *(uint32_t *)PTR(1) = o->thread.finished ? o->thread.t->exit_code : 259; // STILL_ACTIVE
    return TRUE;
}
API(kernel32, SetThreadPriority, 2) {
    Object *o = handle_get(ARG(0), OBJ_THREAD);
    if (o) o->thread.t->priority = (int32_t)ARG(1);
    return TRUE;
}
API(kernel32, GetThreadPriority, 1) {
    Object *o = handle_get(ARG(0), OBJ_THREAD);
    return o ? (uint32_t)o->thread.t->priority : 0;
}
API(kernel32, Sleep, 1) {
    if (ARG(0) == 0) sched_yield();
    else {
        struct timespec ts = {ARG(0) / 1000, (ARG(0) % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    return 0;
}
API(kernel32, SleepEx, 2) {
    struct timespec ts = {ARG(0) / 1000, (ARG(0) % 1000) * 1000000L};
    if (ARG(0) == 0) sched_yield();
    else nanosleep(&ts, NULL);
    return 0;
}

// ---------------------------------------------------------------- kernel objects
API(kernel32, WaitForSingleObject, 2) {
    uint32_t r = obj_wait(ARG(0), ARG(1));
    if (r == WAIT_FAILED) set_last_error(ERROR_INVALID_HANDLE);
    return r;
}
API(kernel32, WaitForSingleObjectEx, 3) {
    uint32_t r = obj_wait(ARG(0), ARG(1));
    if (r == WAIT_FAILED) set_last_error(ERROR_INVALID_HANDLE);
    return r;
}
API(kernel32, WaitForMultipleObjects, 4) {
    // Simple polling implementation (rarely used by the game).
    uint32_t n = ARG(0), *hs = PTR(1), all = ARG(2), timeout = ARG(3);
    uint32_t start = win32_tick_ms();
    for (;;) {
        if (all) {
            uint32_t i;
            for (i = 0; i < n; i++)
                if (obj_wait(hs[i], 0) != WAIT_OBJECT_0) break;
            if (i == n) return WAIT_OBJECT_0;
        } else {
            for (uint32_t i = 0; i < n; i++)
                if (obj_wait(hs[i], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
        }
        if (timeout != INFINITE && win32_tick_ms() - start >= timeout) return WAIT_TIMEOUT;
        usleep(500);
    }
}

static uint32_t named_or_new(Object *o, const char *name) {
    if (name) {
        Object *ex = obj_find_named(name, o->type);
        if (ex) {
            free(o);
            set_last_error(ERROR_ALREADY_EXISTS);
            return handle_alloc(ex);
        }
        o->name = strdup(name);
    }
    set_last_error(0);
    return handle_alloc(o);
}

API(kernel32, CreateEventA, 4) {
    Object *o = obj_new(OBJ_EVENT);
    o->event.manual = ARG(1) != 0;
    o->event.signaled = ARG(2) != 0;
    return named_or_new(o, ARG(3) ? STR(3) : NULL);
}
API(kernel32, CreateEventW, 4) {
    Object *o = obj_new(OBJ_EVENT);
    o->event.manual = ARG(1) != 0;
    o->event.signaled = ARG(2) != 0;
    char n[260];
    if (ARG(3)) w2a(WSTR(3), n, sizeof n);
    return named_or_new(o, ARG(3) ? n : NULL);
}
API(kernel32, CreateMutexW, 3) {
    Object *o = obj_new(OBJ_MUTEX);
    if (ARG(1)) { o->mutex.owner = thread_current()->tid; o->mutex.count = 1; }
    char n[260];
    if (ARG(2)) w2a(WSTR(2), n, sizeof n);
    return named_or_new(o, ARG(2) ? n : NULL);
}
API(kernel32, SetEvent, 1) {
    Object *o = handle_get(ARG(0), OBJ_EVENT);
    if (!o) return FALSE;
    obj_signal_event(o, 1);
    return TRUE;
}
API(kernel32, ResetEvent, 1) {
    Object *o = handle_get(ARG(0), OBJ_EVENT);
    if (!o) return FALSE;
    obj_signal_event(o, 0);
    return TRUE;
}
API(kernel32, PulseEvent, 1) {
    Object *o = handle_get(ARG(0), OBJ_EVENT);
    if (!o) return FALSE;
    obj_signal_event(o, 2);
    return TRUE;
}
API(kernel32, CreateSemaphoreA, 4) {
    Object *o = obj_new(OBJ_SEMAPHORE);
    o->sem.count = (int32_t)ARG(1);
    o->sem.max = (int32_t)ARG(2);
    return named_or_new(o, ARG(3) ? STR(3) : NULL);
}
API(kernel32, CreateSemaphoreW, 4) {
    Object *o = obj_new(OBJ_SEMAPHORE);
    o->sem.count = (int32_t)ARG(1);
    o->sem.max = (int32_t)ARG(2);
    char n[260];
    if (ARG(3)) w2a(WSTR(3), n, sizeof n);
    return named_or_new(o, ARG(3) ? n : NULL);
}
API(kernel32, ReleaseSemaphore, 3) {
    Object *o = handle_get(ARG(0), OBJ_SEMAPHORE);
    if (!o) return FALSE;
    int prev;
    int ok = obj_release_semaphore(o, (int32_t)ARG(1), &prev);
    if (ARG(2)) *(int32_t *)PTR(2) = prev;
    return ok;
}
API(kernel32, CreateMutexA, 3) {
    Object *o = obj_new(OBJ_MUTEX);
    if (ARG(1)) {
        o->mutex.owner = thread_current()->tid;
        o->mutex.count = 1;
    }
    return named_or_new(o, ARG(2) ? STR(2) : NULL);
}
API(kernel32, ReleaseMutex, 1) {
    Object *o = handle_get(ARG(0), OBJ_MUTEX);
    return o ? obj_release_mutex(o) : FALSE;
}
API(kernel32, DuplicateHandle, 7) {
    uint32_t h = ARG(1);
    if (h == PSEUDO_THREAD) h = thread_current()->handle;
    uint32_t n = handle_dup(h);
    if (ARG(3)) *(uint32_t *)PTR(3) = n;
    return n != 0;
}
