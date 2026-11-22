#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t base, size, entry;
    char path[1024];  // host path
    char vpath[512];  // virtual Windows path
} PeImage;

extern PeImage g_exe;

// A loaded module: the exe, a real DLL mapped from disk, or one of our builtin
// DLLs (a tiny fake image whose exports are API thunks).
typedef struct Module {
    char name[64];      // normalized: lowercase, no ".dll"
    char vpath[512];    // Windows path reported by GetModuleFileName
    uint32_t base, size, entry;
    bool builtin, is_exe;
    uint32_t exp_rva, exp_size;
    int tls_index;      // -1 if none
    uint32_t tls_start, tls_end, tls_zero, tls_callbacks;
} Module;

// Map the main executable and everything it imports. Needs the calling thread
// to be a guest thread already (DllMain runs during this).
bool pe_load_exe(const char *hostpath, const char *vpath, PeImage *out);

// Resource lookup in a mapped image. type/name: <0x10000 = integer id, else guest string ptr.
uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t *size);
void *pe_file_resource(const char *hostpath, uint32_t type, uint32_t *size); // malloc'd copy
uint32_t pe_section(uint32_t base, const char *name, uint32_t *size);

// LoadLibrary / GetModuleHandle. `name` may include a path. Returns HMODULE or 0.
uint32_t module_get(const char *name, bool load);
Module *module_by_base(uint32_t hmod);
const char *module_name(uint32_t hmod); // normalized name or NULL
// GetProcAddress on any module (builtin or native). `name` NULL -> by ordinal.
uint32_t module_proc(uint32_t hmod, const char *name, int ordinal);

// Static TLS and DLL thread notifications (thread.c calls these).
void pe_tls_setup_thread(uint32_t teb);
void pe_thread_attach(void);
