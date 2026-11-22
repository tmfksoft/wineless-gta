#include "handles.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "thread.h"

#define MAX_HANDLES 65536
#define HBASE 0x100

static Object *table[MAX_HANDLES];
static int next_free = 1;
static pthread_mutex_t olock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ocond = PTHREAD_COND_INITIALIZER;

void obj_lock(void) { pthread_mutex_lock(&olock); }
void obj_unlock(void) { pthread_mutex_unlock(&olock); }

void handles_init(void) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_destroy(&ocond);
    pthread_cond_init(&ocond, &ca);
}

Object *obj_new(int type) {
    Object *o = calloc(1, sizeof *o);
    o->type = type;
    o->refs = 1;
    return o;
}

static void obj_unref(Object *o) {
    if (--o->refs > 0) return;
    switch (o->type) {
    case OBJ_FILE:
        if (o->file.fd > 2) close(o->file.fd);
        if (o->file.delete_on_close && o->file.path) unlink(o->file.path);
        free(o->file.path);
        break;
    case OBJ_FIND: free(o->find.e); break;
    case OBJ_MAPPING: if (o->mapping.fd >= 0) close(o->mapping.fd); break;
    }
    free(o->name);
    free(o);
}

static uint32_t idx2h(int i) { return HBASE + i * 4; }
static int h2idx(uint32_t h) {
    if (h < HBASE || (h & 3)) return -1;
    uint32_t i = (h - HBASE) / 4;
    return i < MAX_HANDLES ? (int)i : -1;
}

uint32_t handle_alloc(Object *o) {
    pthread_mutex_lock(&olock);
    for (int n = 0; n < MAX_HANDLES; n++) {
        int i = next_free;
        next_free = next_free + 1 >= MAX_HANDLES ? 1 : next_free + 1;
        if (!table[i]) {
            table[i] = o;
            pthread_mutex_unlock(&olock);
            return idx2h(i);
        }
    }
    pthread_mutex_unlock(&olock);
    LOGE("handle table full");
    return 0;
}

Object *handle_get(uint32_t h, int type) {
    if (h == PSEUDO_THREAD) {
        Thread *t = thread_current();
        h = t ? t->handle : 0;
    }
    int i = h2idx(h);
    if (i < 0) return NULL;
    Object *o = table[i];
    if (!o || (type && o->type != type)) return NULL;
    return o;
}

bool handle_close(uint32_t h) {
    if (h == PSEUDO_PROCESS || h == PSEUDO_THREAD) return true;
    int i = h2idx(h);
    if (i < 0) return false;
    pthread_mutex_lock(&olock);
    Object *o = table[i];
    table[i] = NULL;
    if (o) obj_unref(o);
    pthread_mutex_unlock(&olock);
    return o != NULL;
}

uint32_t handle_dup(uint32_t h) {
    Object *o = handle_get(h, 0);
    if (!o) return 0;
    pthread_mutex_lock(&olock);
    o->refs++;
    pthread_mutex_unlock(&olock);
    return handle_alloc(o);
}

Object *obj_find_named(const char *name, int type) {
    if (!name) return NULL;
    Object *r = NULL;
    pthread_mutex_lock(&olock);
    for (int i = 1; i < MAX_HANDLES && !r; i++)
        if (table[i] && table[i]->name && table[i]->type == type && !strcmp(table[i]->name, name)) {
            r = table[i];
            r->refs++;
        }
    pthread_mutex_unlock(&olock);
    return r;
}

uint32_t handle_new_thread(Thread *t) {
    Object *o = obj_new(OBJ_THREAD);
    o->thread.t = t;
    return handle_alloc(o);
}

void handle_thread_finished(Thread *t) {
    pthread_mutex_lock(&olock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (table[i] && table[i]->type == OBJ_THREAD && table[i]->thread.t == t) table[i]->thread.finished = 1;
    pthread_cond_broadcast(&ocond);
    pthread_mutex_unlock(&olock);
}

// try to acquire; olock held
static int try_acquire(Object *o, uint32_t tid) {
    switch (o->type) {
    case OBJ_EVENT:
        if (!o->event.signaled) return 0;
        if (!o->event.manual) o->event.signaled = 0;
        return 1;
    case OBJ_SEMAPHORE:
        if (o->sem.count <= 0) return 0;
        o->sem.count--;
        return 1;
    case OBJ_MUTEX:
        if (o->mutex.count && o->mutex.owner != tid) return 0;
        o->mutex.owner = tid;
        o->mutex.count++;
        return 1;
    case OBJ_THREAD:
        return o->thread.finished;
    case OBJ_FILE:
        return 1;
    }
    return 1;
}

uint32_t obj_wait(uint32_t h, uint32_t timeout_ms) {
    Object *o = handle_get(h, 0);
    if (!o) return WAIT_FAILED;
    Thread *self = thread_current();
    uint32_t tid = self ? self->tid : 0;
    struct timespec dl;
    if (timeout_ms != INFINITE) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += timeout_ms / 1000;
        dl.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&olock);
    uint32_t r = WAIT_OBJECT_0;
    while (!try_acquire(o, tid)) {
        if (timeout_ms == 0) { r = WAIT_TIMEOUT; break; }
        if (timeout_ms == INFINITE) pthread_cond_wait(&ocond, &olock);
        else if (pthread_cond_timedwait(&ocond, &olock, &dl) == ETIMEDOUT) {
            if (!try_acquire(o, tid)) r = WAIT_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&olock);
    return r;
}

void obj_signal_event(Object *o, int set) {
    pthread_mutex_lock(&olock);
    o->event.signaled = set != 0;
    pthread_cond_broadcast(&ocond);
    pthread_mutex_unlock(&olock);
    if (set == 2) { // PulseEvent: let waiters run, then reset (approximation)
        sched_yield();
        pthread_mutex_lock(&olock);
        o->event.signaled = 0;
        pthread_mutex_unlock(&olock);
    }
}

int obj_release_semaphore(Object *o, int n, int *prev) {
    pthread_mutex_lock(&olock);
    if (prev) *prev = o->sem.count;
    if (o->sem.count + n > o->sem.max) {
        pthread_mutex_unlock(&olock);
        return 0;
    }
    o->sem.count += n;
    pthread_cond_broadcast(&ocond);
    pthread_mutex_unlock(&olock);
    return 1;
}

int obj_release_mutex(Object *o) {
    Thread *self = thread_current();
    pthread_mutex_lock(&olock);
    if (!o->mutex.count || o->mutex.owner != (self ? self->tid : 0)) {
        pthread_mutex_unlock(&olock);
        return 0;
    }
    if (--o->mutex.count == 0) o->mutex.owner = 0;
    pthread_cond_broadcast(&ocond);
    pthread_mutex_unlock(&olock);
    return 1;
}
