#pragma once
// Minimal COM bridge: guest objects are { vtbl, host_ptr_lo, host_ptr_hi, refcount, ... }
// living in low memory; each vtable slot is an API thunk into host code.
#include <stdint.h>

#include "../api.h"

typedef struct {
    ApiFn fn;
    int nargs; // including `this`
} ComMethod;

#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu

typedef struct {
    uint32_t vtbl;
    uint32_t host_lo, host_hi;
    uint32_t refs;
} ComObj;

// Build a vtable of n guest thunks. `label` prefixes the thunk names for logs.
uint32_t com_vtable(const char *label, const ComMethod *m, int n);
// Allocate a guest object with `extra` bytes after the header. Refcount = 1.
uint32_t com_new(uint32_t vtbl, void *host, uint32_t extra);
static inline void *com_host(uint32_t obj) {
    ComObj *o = (ComObj *)(uintptr_t)obj;
    return (void *)(uintptr_t)((uint64_t)o->host_lo | ((uint64_t)o->host_hi << 32));
}
static inline ComObj *com_obj(uint32_t obj) { return (ComObj *)(uintptr_t)obj; }

// Generic helpers usable as vtable entries
uint32_t com_addref(uint32_t *a);
uint32_t com_release_nofree(uint32_t *a);
uint32_t com_qi_self(uint32_t *a); // QueryInterface that hands back `this`
uint32_t com_notimpl(uint32_t *a);

int guid_eq(const void *g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]);
