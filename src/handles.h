#pragma once
#include <stdbool.h>
#include <stdint.h>

struct Thread;

enum {
    OBJ_NONE,
    OBJ_FILE,
    OBJ_EVENT,
    OBJ_MUTEX,
    OBJ_SEMAPHORE,
    OBJ_THREAD,
    OBJ_FIND,
    OBJ_MAPPING,
    OBJ_REGKEY,
    OBJ_TOKEN, // access token (no state)
    OBJ_PROCESS, // another running process (by Linux pid)
};

typedef struct FindEntry {
    char name[260];
    int is_dir;
    uint64_t size;
    int64_t mtime;
} FindEntry;

typedef struct Object {
    int type;
    int refs;
    char *name; // named kernel objects
    union {
        struct { int fd; int is_console; char *path; int delete_on_close;
                 int is_arc; uint64_t arc_off, arc_size, arc_pos; } file;
        struct { int manual; int signaled; } event;
        struct { uint32_t owner; int count; } mutex;
        struct { int count, max; } sem;
        struct { struct Thread *t; int finished; } thread;
        struct { FindEntry *e; int n, pos; } find;
        struct { int fd; uint32_t size; uint32_t prot; int is_arc; uint64_t arc_base; } mapping;
        struct { char *path; } regkey;
        struct { uint32_t pid; } process;
    };
} Object;

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define PSEUDO_PROCESS 0xFFFFFFFFu
#define PSEUDO_THREAD 0xFFFFFFFEu

void handles_init(void);
uint32_t handle_alloc(Object *o);    // takes ownership of one reference
Object *handle_get(uint32_t h, int type); // borrowed; NULL if wrong type (type 0 = any)
bool handle_close(uint32_t h);
uint32_t handle_dup(uint32_t h);
Object *obj_new(int type);
Object *obj_find_named(const char *name, int type);

uint32_t handle_new_thread(struct Thread *t);
void handle_thread_finished(struct Thread *t);

// Waits (global lock + condvar; good enough for a handful of game threads)
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
uint32_t obj_wait(uint32_t h, uint32_t timeout_ms);
void obj_signal_event(Object *o, int set); // set=1 SetEvent, 0 ResetEvent, 2 PulseEvent
int obj_release_semaphore(Object *o, int n, int *prev);
int obj_release_mutex(Object *o);
void obj_lock(void);
void obj_unlock(void);
