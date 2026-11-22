#pragma once
#include <stdint.h>

#include "lowmem.h"

// An API implementation receives a pointer to its stack arguments (guest
// memory, 32-bit slots) and returns eax.
typedef uint32_t (*ApiFn)(uint32_t *a);

#define API_CDECL 1 // caller pops args
#define API_DATA 2  // a variable: fn(NULL) returns its guest address

typedef struct ApiDef {
    const char *dll;  // lowercase, no extension
    const char *name; // may be NULL for ordinal-only
    int ordinal;      // 0 if none
    ApiFn fn;
    uint8_t nargs;
    uint8_t flags;
} ApiDef;

#define API_SECTION __attribute__((used, section("wineless_api"), aligned(8)))

#define API_DEF_(dll, name, ord, nargs, flags)                                               \
    static uint32_t api_##dll##_##name(uint32_t *_args);                                         \
    static const ApiDef apidef_##dll##_##name API_SECTION = {#dll, #name, ord, api_##dll##_##name, \
                                                             nargs, flags};                   \
    static uint32_t api_##dll##_##name(uint32_t *_args)

// exports whose names aren't C identifiers (C++-mangled): separate C name
#define API_DEF_N_(dll, cname, ename, nargs, flags)                                            \
    static uint32_t api_##dll##_##cname(uint32_t *_args);                                        \
    static const ApiDef apidef_##dll##_##cname API_SECTION = {#dll, ename, 0, api_##dll##_##cname, \
                                                              nargs, flags};                    \
    static uint32_t api_##dll##_##cname(uint32_t *_args)
#define API_N(dll, cname, ename, nargs) API_DEF_N_(dll, cname, ename, nargs, 0)
#define API_CN(dll, cname, ename, nargs) API_DEF_N_(dll, cname, ename, nargs, API_CDECL)

// stdcall API with nargs 32-bit arguments
#define API(dll, name, nargs) API_DEF_(dll, name, 0, nargs, 0)
// exported by ordinal (and name)
#define API_ORD(dll, name, ord, nargs) API_DEF_(dll, name, ord, nargs, 0)
// cdecl
#define API_C(dll, name, nargs) API_DEF_(dll, name, 0, nargs, API_CDECL)

// exported variable: the body returns the variable's guest address
#define API_DATA_EXPORT(dll, name) API_DEF_(dll, name, 0, 0, API_DATA)

// argument helpers
#define ARG(i) (_args[i])
#define PTR(i) ((void *)(uintptr_t)_args[i])
#define STR(i) ((const char *)(uintptr_t)_args[i])
#define WSTR(i) ((const uint16_t *)(uintptr_t)_args[i])

void api_init(void);
// Guest address of a callable thunk for dll!name (or ordinal). If not
// implemented and `stub_missing`, returns a stub that aborts when called.
uint32_t api_resolve(const char *dll, const char *name, int ordinal, int stub_missing);
// Register a host function at runtime (COM vtables etc.); returns guest thunk.
uint32_t api_register_dynamic(const char *label, ApiFn fn, int nargs, int flags);
const char *api_describe(uint32_t id);
uint32_t api_bind_def_at(const ApiDef *d, uint32_t addr);

// Set extra return state for the current call.
void api_ret_edx(uint32_t edx);
void api_ret_double(double v);
// Override stack bytes popped (for varargs-like stdcall oddities).
void api_ret_pop(uint32_t bytes);

// Normalize "KERNEL32.dll" -> "kernel32"
void api_norm_dll(const char *in, char *out, int outsz);
