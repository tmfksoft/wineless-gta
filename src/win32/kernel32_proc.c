// kernel32: process, modules, memory, system information
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "../heap.h"
#include "../pe.h"
#include "../vfs.h"
#include "win32.h"

uint32_t g_guest_cmdline;
static uint32_t guest_env_a, guest_env_w;
static uint32_t unhandled_filter;
static struct timespec t0;

uint32_t win32_tick_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

size_t wlen(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
void w2a(const uint16_t *w, char *out, size_t outsz) {
    size_t i = 0;
    for (; w[i] && i < outsz - 1; i++) out[i] = w[i] < 256 ? (char)w[i] : '?';
    out[i] = 0;
}
void a2w(const char *s, uint16_t *out, size_t outcnt) {
    size_t i = 0;
    for (; s[i] && i < outcnt - 1; i++) out[i] = (uint8_t)s[i];
    out[i] = 0;
}

uint32_t copy_out_str(char *dst, uint32_t dstlen, const char *src) {
    uint32_t n = strlen(src);
    if (!dst || dstlen <= n) return n + 1;
    memcpy(dst, src, n + 1);
    return n;
}

uint32_t win32_errno(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    }
    return ERROR_ACCESS_DENIED;
}

void win32_init(const char *cmdline) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_guest_cmdline = gheap_strdup(cmdline);
    static const char *env[] = {
        "ALLUSERSPROFILE=C:\\ProgramData", "APPDATA=" VFS_ROAMING_VDIR,
        "COMPUTERNAME=LINUX", "HOMEDRIVE=C:", "HOMEPATH=\\Users\\Player",
        "NUMBER_OF_PROCESSORS=4", "OS=Windows_NT", "PATH=C:\\Windows\\System32;C:\\Windows",
        "PROCESSOR_ARCHITECTURE=x86", "SystemDrive=C:", "SystemRoot=C:\\Windows",
        "LOCALAPPDATA=" VFS_LOCAL_VDIR, "PROGRAMDATA=" VFS_PROGRAMDATA_VDIR,
        "TEMP=" VFS_TEMP_VDIR, "TMP=" VFS_TEMP_VDIR,
        "USERNAME=Player", "USERPROFILE=C:\\Users\\Player", "windir=C:\\Windows", NULL};
    size_t total = 1;
    for (int i = 0; env[i]; i++) total += strlen(env[i]) + 1;
    guest_env_a = gheap_alloc(total, 1);
    guest_env_w = gheap_alloc(total * 2, 1);
    char *pa = G2H(guest_env_a);
    uint16_t *pw = G2H(guest_env_w);
    for (int i = 0; env[i]; i++) {
        size_t l = strlen(env[i]) + 1;
        memcpy(pa, env[i], l);
        for (size_t k = 0; k < l; k++) pw[k] = (uint8_t)env[i][k];
        pa += l;
        pw += l;
    }
    user32_init();
}

void win32_process_exit(uint32_t code) {
    LOGI("process exit (%u)", code);
    user32_shutdown();
    fflush(NULL);
    _exit((int)code);
}

// ---------------------------------------------------------------- process
API(kernel32, ExitProcess, 1) { win32_process_exit(ARG(0)); }
API(kernel32, TerminateProcess, 2) {
    if (ARG(0) == PSEUDO_PROCESS) win32_process_exit(ARG(1));
    return FALSE;
}
API(kernel32, FatalAppExitA, 2) {
    LOGE("FatalAppExit: %s", STR(1));
    win32_process_exit(255);
}
API(kernel32, GetCommandLineA, 0) { return g_guest_cmdline; }
API(kernel32, GetStartupInfoA, 1) {
    uint8_t *si = PTR(0);
    memset(si, 0, 68);
    *(uint32_t *)si = 68;
    return 0;
}
API(kernel32, GetStartupInfoW, 1) {
    uint8_t *si = PTR(0);
    memset(si, 0, 68);
    *(uint32_t *)si = 68;
    return 0;
}
API(kernel32, GetEnvironmentStrings, 0) { return guest_env_a; }
// %NAME% -> value (unknown variables stay as written). Returns chars incl. NUL.
static size_t expand_env(const char *in, char *out, size_t n);
API(kernel32, ExpandEnvironmentStringsA, 3) {
    char buf[4096];
    size_t need = expand_env(STR(0), buf, sizeof buf);
    if (ARG(1) && ARG(2) >= need) memcpy(PTR(1), buf, need);
    return need;
}
API(kernel32, ExpandEnvironmentStringsW, 3) {
    char in[4096], buf[4096];
    w2a(WSTR(0), in, sizeof in);
    size_t need = expand_env(in, buf, sizeof buf);
    if (ARG(1) && ARG(2) >= need) a2w(buf, PTR(1), need);
    return need;
}

// Guest address of a variable's value in the environment block (msvcrt getenv).
uint32_t win32_getenv(const char *name) {
    size_t nl = strlen(name);
    for (const char *p = G2H(guest_env_a); *p; p += strlen(p) + 1)
        if (!strncasecmp(p, name, nl) && p[nl] == '=') return H2G(p + nl + 1);
    return 0;
}
API(kernel32, GetEnvironmentStringsA, 0) { return guest_env_a; }
API(kernel32, GetEnvironmentStringsW, 0) { return guest_env_w; }
API(kernel32, FreeEnvironmentStringsA, 1) { return TRUE; }
API(kernel32, FreeEnvironmentStringsW, 1) { return TRUE; }
API(kernel32, SetEnvironmentVariableA, 2) {
    LOGD("SetEnvironmentVariableA(%s, %s)", STR(0), ARG(1) ? STR(1) : "(null)");
    return TRUE;
}
API(kernel32, GetEnvironmentVariableA, 3) {
    const char *name = STR(0);
    size_t nl = strlen(name);
    for (const char *p = G2H(guest_env_a); *p; p += strlen(p) + 1)
        if (!strncasecmp(p, name, nl) && p[nl] == '=') return copy_out_str(PTR(1), ARG(2), p + nl + 1);
    set_last_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
API(kernel32, GetEnvironmentVariableW, 3) {
    char name[256];
    w2a(WSTR(0), name, sizeof name);
    size_t nl = strlen(name);
    // Values come from the ASCII env block; widen on the way out. nSize/return
    // are in WCHARs, matching Win32 (chars excl. NUL on success, required size
    // incl. NUL if the buffer is too small).
    for (const char *p = G2H(guest_env_a); *p; p += strlen(p) + 1)
        if (!strncasecmp(p, name, nl) && p[nl] == '=') {
            const char *val = p + nl + 1;
            uint32_t vlen = (uint32_t)strlen(val);
            uint16_t *dst = ARG(1) ? PTR(1) : NULL;
            if (!dst || ARG(2) <= vlen) return vlen + 1;
            a2w(val, dst, ARG(2));
            return vlen;
        }
    set_last_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
API(kernel32, GetVersion, 0) { return 0x0A280105; } // 5.1 build 2600
API(kernel32, GetVersionExA, 1) {
    uint8_t *v = PTR(0);
    uint32_t sz = *(uint32_t *)v;
    memset(v + 4, 0, sz - 4);
    uint32_t *d = (uint32_t *)v;
    d[1] = 5; d[2] = 1; d[3] = 2600; d[4] = 2;
    strcpy((char *)v + 20, "Service Pack 3");
    if (sz >= 156) { *(uint16_t *)(v + 148) = 3; }
    return TRUE;
}
API(kernel32, GetVersionExW, 1) {
    uint8_t *v = PTR(0);
    uint32_t sz = *(uint32_t *)v;
    memset(v + 4, 0, sz - 4);
    uint32_t *d = (uint32_t *)v;
    d[1] = 5; d[2] = 1; d[3] = 2600; d[4] = 2;
    a2w("Service Pack 3", (uint16_t *)(v + 20), 128);
    if (sz >= 284) { *(uint16_t *)(v + 276) = 3; }
    return TRUE;
}
// Other processes: only this one exists.
// Other processes (e.g. the Steam client, whose pid Steam's DRM looks up) can be
// opened to see whether they are still running; nothing else.
static bool pid_alive(uint32_t pid) {
    char p[32];
    snprintf(p, sizeof p, "/proc/%u", pid);
    return access(p, F_OK) == 0;
}
API(kernel32, OpenProcess, 3) {
    if (ARG(2) == (uint32_t)getpid()) return PSEUDO_PROCESS;
    if (!ARG(2) || !pid_alive(ARG(2))) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    Object *o = obj_new(OBJ_PROCESS);
    o->process.pid = ARG(2);
    return handle_alloc(o);
}
API(kernel32, GetExitCodeProcess, 2) {
    Object *o = handle_get(ARG(0), OBJ_PROCESS);
    *(uint32_t *)PTR(1) = !o || pid_alive(o->process.pid) ? 259 /*STILL_ACTIVE*/ : 0;
    return TRUE;
}
// Our own process only (all a game can open).
static bool own_process(uint32_t h) { return h == PSEUDO_PROCESS || h == (uint32_t)getpid(); }
API(kernel32, ReadProcessMemory, 5) {
    if (!own_process(ARG(0)) || !lm_is_committed(ARG(1), ARG(3), false)) { set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    memcpy(PTR(2), PTR(1), ARG(3));
    if (ARG(4)) *(uint32_t *)PTR(4) = ARG(3);
    return TRUE;
}
API(kernel32, WriteProcessMemory, 5) {
    if (!own_process(ARG(0)) || !lm_is_committed(ARG(1), ARG(3), true)) { set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    memcpy(PTR(1), PTR(2), ARG(3));
    if (ARG(4)) *(uint32_t *)PTR(4) = ARG(3);
    return TRUE;
}
API(kernel32, GetCurrentProcess, 0) { return PSEUDO_PROCESS; }
API(kernel32, GetCurrentProcessId, 0) { return getpid(); }
API(kernel32, GetCurrentThread, 0) { return PSEUDO_THREAD; }
API(kernel32, GetCurrentThreadId, 0) { return thread_current()->tid; }
API(kernel32, SetUnhandledExceptionFilter, 1) {
    uint32_t prev = unhandled_filter;
    unhandled_filter = ARG(0);
    return prev;
}
uint32_t k32_unhandled_filter(void) { return unhandled_filter; }
API(kernel32, UnhandledExceptionFilter, 1) { return 1; }
API(kernel32, SetErrorMode, 1) {
    static uint32_t mode;
    uint32_t p = mode;
    mode = ARG(0);
    return p;
}
API(kernel32, GetLastError, 0) { return get_last_error(); }
API(kernel32, SetLastError, 1) { set_last_error(ARG(0)); return 0; }
API(kernel32, OutputDebugStringA, 1) { LOGD("OutputDebugString: %s", STR(0)); return 0; }
API(kernel32, SetConsoleCtrlHandler, 2) { return TRUE; }
API(kernel32, GetSystemDirectoryA, 2) { return copy_out_str(PTR(0), ARG(1), "C:\\Windows\\System32"); }
API(kernel32, GetWindowsDirectoryA, 2) { return copy_out_str(PTR(0), ARG(1), "C:\\Windows"); }

API(kernel32, GetSystemInfo, 1) {
    uint32_t *s = PTR(0);
    memset(s, 0, 36);
    s[1] = 4096;        // dwPageSize
    s[2] = 0x10000;     // lpMinimumApplicationAddress
    s[3] = 0x7FFEFFFF;  // lpMaximumApplicationAddress
    int n = get_nprocs();
    if (n > 32) n = 32;
    s[4] = n == 32 ? 0xFFFFFFFF : (1u << n) - 1;
    s[5] = n;
    s[6] = 586;         // PROCESSOR_INTEL_PENTIUM
    s[7] = 65536;       // dwAllocationGranularity
    ((uint16_t *)s)[16] = 6;
    return 0;
}
API(kernel32, IsProcessorFeaturePresent, 1) {
    switch (ARG(0)) {
    case 2: /* PF_COMPARE_EXCHANGE_DOUBLE */ case 3: /* MMX */ case 6: /* SSE */ case 10: /* SSE2 */
    case 13: /* SSE3 */ case 8: /* RDTSC */
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- modules
API(kernel32, GetModuleHandleA, 1) {
    uint32_t h = module_get(ARG(0) ? STR(0) : NULL, false);
    if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}
API(kernel32, GetModuleHandleW, 1) {
    char n[260];
    if (!ARG(0)) return g_exe.base;
    w2a(WSTR(0), n, sizeof n);
    uint32_t h = module_get(n, false);
    if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}
static uint32_t load_library(const char *n) {
    uint32_t h = module_get(n, true);
    if (!h) {
        LOGI("LoadLibrary(%s): not available", n);
        set_last_error(ERROR_MOD_NOT_FOUND);
    } else {
        LOGD("LoadLibrary(%s) = 0x%x", n, h);
    }
    return h;
}
API(kernel32, LoadLibraryA, 1) { return load_library(STR(0)); }
// LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800): proxies use it to reach the real DLL
static uint32_t load_library_ex(const char *n, uint32_t flags) {
    if ((flags & 0x800) && !strchr(n, '\\') && !strchr(n, '/')) {
        char p[512];
        snprintf(p, sizeof p, "C:\\Windows\\System32\\%s", n);
        return load_library(p);
    }
    return load_library(n);
}
API(kernel32, LoadLibraryExA, 3) { return load_library_ex(STR(0), ARG(2)); }
API(kernel32, LoadLibraryW, 1) {
    char n[260];
    w2a(WSTR(0), n, sizeof n);
    return load_library(n);
}
API(kernel32, FreeLibrary, 1) { return TRUE; } // modules stay loaded
API(kernel32, GetProcAddress, 2) {
    const char *name = ARG(1) >= 0x10000 ? STR(1) : NULL;
    int ord = ARG(1) < 0x10000 ? (int)ARG(1) : 0;
    uint32_t r = module_proc(ARG(0) ? ARG(0) : g_exe.base, name, ord);
    if (r && name) LOGT("GetProcAddress(%s) = 0x%x", name, r);
    if (!r) {
        const char *dll = module_name(ARG(0));
        if (name) LOGI("GetProcAddress(%s, %s): not found", dll ? dll : "?", name);
        else LOGI("GetProcAddress(%s, #%d): not found", dll ? dll : "?", ord);
        set_last_error(ERROR_PROC_NOT_FOUND);
    }
    return r;
}
static const char *module_vpath(uint32_t h) {
    Module *m = module_by_base(h ? h : g_exe.base);
    return m ? m->vpath : g_exe.vpath;
}
API(kernel32, GetModuleFileNameA, 3) {
    const char *buf = module_vpath(ARG(0));
    char *dst = PTR(1);
    uint32_t len = ARG(2), l = strlen(buf);
    if (!len) return 0;
    if (l >= len) { memcpy(dst, buf, len - 1); dst[len - 1] = 0; set_last_error(ERROR_INSUFFICIENT_BUFFER); return len; }
    memcpy(dst, buf, l + 1);
    return l;
}
API(kernel32, GetModuleFileNameW, 3) {
    const char *buf = module_vpath(ARG(0));
    uint32_t l = strlen(buf);
    if (!ARG(2)) return 0;
    if (l >= ARG(2)) l = ARG(2) - 1;
    uint16_t *dst = PTR(1);
    for (uint32_t i = 0; i < l; i++) dst[i] = (uint8_t)buf[i];
    dst[l] = 0;
    return l;
}

// ---------------------------------------------------------------- resources
static uint32_t make_hrsrc(uint32_t data, uint32_t size) {
    uint32_t h = gheap_alloc(8, 0);
    ((uint32_t *)G2H(h))[0] = data;
    ((uint32_t *)G2H(h))[1] = size;
    return h;
}
static bool native_module(uint32_t h) {
    Module *m = module_by_base(h);
    return m && !m->builtin;
}
API(kernel32, FindResourceA, 3) {
    uint32_t mod = ARG(0) ? ARG(0) : g_exe.base, size = 0;
    if (!native_module(mod)) return 0;
    uint32_t d = pe_find_resource(mod, ARG(2), ARG(1), &size);
    return d ? make_hrsrc(d, size) : 0;
}
API(kernel32, FindResourceW, 3) {
    uint32_t mod = ARG(0) ? ARG(0) : g_exe.base, size = 0;
    if (!native_module(mod)) return 0;
    uint32_t type = ARG(2), name = ARG(1);
    char tb[128], nb[128];
    if (type >= 0x10000) { w2a(G2H(type), tb, sizeof tb); type = gheap_strdup(tb); }
    if (name >= 0x10000) { w2a(G2H(name), nb, sizeof nb); name = gheap_strdup(nb); }
    uint32_t d = pe_find_resource(mod, type, name, &size);
    return d ? make_hrsrc(d, size) : 0;
}
API(kernel32, LoadResource, 2) { return ARG(1) ? ((uint32_t *)PTR(1))[0] : 0; }
API(kernel32, LockResource, 1) { return ARG(0); }
API(kernel32, SizeofResource, 2) { return ARG(1) ? ((uint32_t *)PTR(1))[1] : 0; }

// ---------------------------------------------------------------- memory
API(kernel32, VirtualAlloc, 4) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2), prot = ARG(3);
    uint32_t r = 0;
    if (type & MEM_RESERVE || !addr) {
        uint32_t a = addr & ~(LM_GRAN - 1);
        uint32_t sz = (addr + size) - a;
        r = lm_reserve(a, sz, LM_GRAN, LM_TAG_VIRTUAL, (type & MEM_TOP_DOWN) != 0);
        if (r && (type & MEM_COMMIT)) {
            if (!lm_commit(r, sz, prot)) { lm_release(r); r = 0; }
        }
        if (r && addr) r = addr & ~(LM_PAGE - 1);
    } else if (type & MEM_COMMIT) {
        if (lm_commit(addr, size, prot)) r = addr & ~(LM_PAGE - 1);
    }
    if (!r) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        LOGW("VirtualAlloc(0x%x, 0x%x, 0x%x, 0x%x) failed", addr, size, type, prot);
    }
    return r;
}
API(kernel32, VirtualFree, 3) {
    // MEM_DECOMMIT/MEM_RELEASE only operate on VirtualAlloc'd memory. Windows
    // rejects them (ERROR_INVALID_PARAMETER) and leaves memory intact otherwise;
    // some CRTs (VC6 small-block heap) call VirtualFree on plain heap pointers
    // and rely on that no-op, so decommitting anything else would destroy live
    // data (e.g. AoE's loaded SLP sprite buffers).
    if (lm_tag(ARG(0)) != LM_TAG_VIRTUAL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ARG(2) & MEM_RELEASE) return lm_release(ARG(0));
    if (ARG(2) & MEM_DECOMMIT) return lm_decommit(ARG(0), ARG(1));
    return FALSE;
}
API(kernel32, VirtualProtect, 4) {
    uint32_t old = 0;
    bool ok = lm_protect(ARG(0), ARG(1), ARG(2), &old);
    if (ARG(3)) *(uint32_t *)PTR(3) = old ? old : PAGE_EXECUTE_READWRITE;
    if (!ok) LOGD("VirtualProtect(0x%x, 0x%x, 0x%x) failed", ARG(0), ARG(1), ARG(2));
    return ok;
}
API(kernel32, VirtualQuery, 3) {
    LmQuery q;
    if (ARG(2) < 28 || !lm_query(ARG(0), &q)) return 0;
    uint32_t *m = PTR(1);
    m[0] = q.base; m[1] = q.alloc_base; m[2] = q.alloc_prot; m[3] = q.region_size;
    m[4] = q.state; m[5] = q.prot; m[6] = q.type;
    return 28;
}
API(kernel32, VirtualQueryEx, 4) {
    if (!own_process(ARG(0))) { set_last_error(ERROR_ACCESS_DENIED); return 0; }
    return api_kernel32_VirtualQuery(_args + 1);
}
API(kernel32, FlushInstructionCache, 3) { return TRUE; }
uint32_t k32_virtual_alloc(uint32_t *a) { return api_kernel32_VirtualAlloc(a); }
uint32_t k32_open_process(uint32_t *a) { return api_kernel32_OpenProcess(a); }

#define HEAP_ZERO_MEMORY 0x8
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10
static uint32_t next_heap = 0x00150000;
API(kernel32, HeapCreate, 3) { return next_heap += 0x10000; }
API(kernel32, HeapDestroy, 1) { return TRUE; }
API(kernel32, GetProcessHeap, 0) { return 0x00150000; }
API(kernel32, HeapAlloc, 3) {
    uint32_t p = gheap_alloc(ARG(2), ARG(1) & HEAP_ZERO_MEMORY);
    if (!p) LOGW("HeapAlloc(%u) failed", ARG(2));
    return p;
}
API(kernel32, HeapReAlloc, 4) {
    if ((ARG(1) & HEAP_REALLOC_IN_PLACE_ONLY) && ARG(3) > gheap_capacity(ARG(2))) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return gheap_realloc(ARG(2), ARG(3), ARG(1) & HEAP_ZERO_MEMORY);
}
API(kernel32, HeapFree, 3) { return gheap_free(ARG(2)); }
API(kernel32, HeapSize, 3) { return gheap_size(ARG(2)); }
API(kernel32, HeapValidate, 3) { return TRUE; }
API(kernel32, HeapCompact, 2) { return 0x100000; }
#define LMEM_ZEROINIT 0x40
API(kernel32, LocalAlloc, 2) { return gheap_alloc(ARG(1), ARG(0) & LMEM_ZEROINIT); }
API(kernel32, LocalFree, 1) { gheap_free(ARG(0)); return 0; }
API(kernel32, GlobalAlloc, 2) { return gheap_alloc(ARG(1), ARG(0) & LMEM_ZEROINIT); }
API(kernel32, GlobalFree, 1) { gheap_free(ARG(0)); return 0; }
API(kernel32, GlobalLock, 1) { return ARG(0); }
API(kernel32, GlobalUnlock, 1) { return TRUE; }

API(kernel32, GlobalMemoryStatus, 1) {
    uint32_t *m = PTR(0);
    struct sysinfo si;
    sysinfo(&si);
    uint64_t total = (uint64_t)si.totalram * si.mem_unit, avail = (uint64_t)si.freeram * si.mem_unit;
#define CLAMP2G(x) ((x) > 0x7FFFFFFFull ? 0x7FFFFFFFu : (uint32_t)(x))
    m[0] = 32;
    m[1] = total ? (uint32_t)(100 - avail * 100 / total) : 50;
    m[2] = CLAMP2G(total);
    m[3] = CLAMP2G(avail);
    m[4] = 0x7FFFFFFF;
    m[5] = 0x7FFFFFFF;
    m[6] = 0x7FFE0000;
    m[7] = 0x70000000;
    return 0;
}
API(kernel32, IsBadReadPtr, 2) { return !lm_is_committed(ARG(0), ARG(1), false); }
API(kernel32, IsBadWritePtr, 2) { return !lm_is_committed(ARG(0), ARG(1), true); }
API(kernel32, IsBadCodePtr, 1) { return !lm_is_committed(ARG(0), 1, false); }

// ---------------------------------------------------------------- time
static void unix_to_filetime(const struct timespec *ts, uint32_t *ft) {
    uint64_t v = ((uint64_t)ts->tv_sec + 11644473600ull) * 10000000ull + ts->tv_nsec / 100;
    ft[0] = (uint32_t)v;
    ft[1] = (uint32_t)(v >> 32);
}
API(kernel32, GetTickCount, 0) { return win32_tick_ms(); }
API(kernel32, QueryPerformanceFrequency, 1) {
    *(uint64_t *)PTR(0) = 10000000ull;
    return TRUE;
}
API(kernel32, QueryPerformanceCounter, 1) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    *(uint64_t *)PTR(0) = (uint64_t)t.tv_sec * 10000000ull + t.tv_nsec / 100;
    return TRUE;
}
static void fill_systemtime(uint16_t *st, const struct tm *tm, long ms) {
    st[0] = tm->tm_year + 1900; st[1] = tm->tm_mon + 1; st[2] = tm->tm_wday; st[3] = tm->tm_mday;
    st[4] = tm->tm_hour; st[5] = tm->tm_min; st[6] = tm->tm_sec; st[7] = (uint16_t)ms;
}
API(kernel32, GetLocalTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(PTR(0), &tm, ts.tv_nsec / 1000000);
    return 0;
}
API(kernel32, GetSystemTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(PTR(0), &tm, ts.tv_nsec / 1000000);
    return 0;
}
API(kernel32, GetSystemTimeAsFileTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unix_to_filetime(&ts, PTR(0));
    return 0;
}
API(kernel32, CompareFileTime, 2) {
    uint32_t *a = PTR(0), *b = PTR(1);
    uint64_t ta = a[0] | (uint64_t)a[1] << 32, tb = b[0] | (uint64_t)b[1] << 32;
    return ta < tb ? (uint32_t)-1 : ta > tb ? 1 : 0;
}
API(kernel32, FileTimeToLocalFileTime, 2) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int64_t v = *(int64_t *)PTR(0) + (int64_t)tm.tm_gmtoff * 10000000;
    *(int64_t *)PTR(1) = v;
    return TRUE;
}
// SystemTimeToTzSpecificLocalTime(tz (NULL = current), utc, local)
API(kernel32, SystemTimeToTzSpecificLocalTime, 3) {
    const uint16_t *u = PTR(1);
    struct tm tm = {.tm_year = u[0] - 1900, .tm_mon = u[1] - 1, .tm_mday = u[3], .tm_hour = u[4], .tm_min = u[5], .tm_sec = u[6]};
    time_t t = timegm(&tm);
    struct tm lt;
    localtime_r(&t, &lt);
    fill_systemtime(PTR(2), &lt, u[7]);
    return TRUE;
}
API(kernel32, FileTimeToSystemTime, 2) {
    uint64_t v = *(uint64_t *)PTR(0);
    time_t s = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&s, &tm);
    fill_systemtime(PTR(1), &tm, (v / 10000) % 1000);
    return TRUE;
}
API(kernel32, GetTimeZoneInformation, 1) {
    uint8_t *tz = PTR(0);
    memset(tz, 0, 172);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    *(int32_t *)tz = (int32_t)(-tm.tm_gmtoff / 60);
    return 0; // TIME_ZONE_ID_UNKNOWN
}
static uint32_t fmt_out(char *dst, uint32_t len, const char *s) {
    uint32_t n = strlen(s) + 1;
    if (!len) return n;
    if (len < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, s, n);
    return n;
}
API(kernel32, GetDateFormatA, 6) {
    uint16_t *st = PTR(2);
    char b[64];
    if (ARG(3)) LOGD("GetDateFormatA custom format '%s' ignored", STR(3));
    if (st) snprintf(b, sizeof b, "%02u/%02u/%04u", st[1], st[3], st[0]);
    else { time_t t = time(NULL); strftime(b, sizeof b, "%m/%d/%Y", localtime(&t)); }
    return fmt_out(PTR(4), ARG(5), b);
}
API(kernel32, GetTimeFormatA, 6) {
    uint16_t *st = PTR(2);
    char b[64];
    if (st) snprintf(b, sizeof b, "%02u:%02u:%02u", st[4], st[5], st[6]);
    else { time_t t = time(NULL); strftime(b, sizeof b, "%H:%M:%S", localtime(&t)); }
    return fmt_out(PTR(4), ARG(5), b);
}

static uint32_t fmt_out_w(uint16_t *dst, uint32_t len, const char *s) {
    uint32_t n = strlen(s) + 1;
    if (!len) return n;
    if (len < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    a2w(s, dst, n);
    return n;
}
API(kernel32, GetDateFormatW, 6) {
    uint16_t *st = PTR(2);
    char b[64];
    if (st) snprintf(b, sizeof b, "%02u/%02u/%04u", st[1], st[3], st[0]);
    else { time_t t = time(NULL); strftime(b, sizeof b, "%m/%d/%Y", localtime(&t)); }
    return fmt_out_w(PTR(4), ARG(5), b);
}
API(kernel32, GetTimeFormatW, 6) {
    uint16_t *st = PTR(2);
    char b[64];
    if (st) snprintf(b, sizeof b, "%02u:%02u:%02u", st[4], st[5], st[6]);
    else { time_t t = time(NULL); strftime(b, sizeof b, "%H:%M:%S", localtime(&t)); }
    return fmt_out_w(PTR(4), ARG(5), b);
}

uint32_t k32_get_system_info(uint32_t *a) { return api_kernel32_GetSystemInfo(a); }

static size_t expand_env(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n;) {
        const char *e = *p == '%' ? strchr(p + 1, '%') : NULL;
        if (e && e > p + 1) {
            char name[256];
            snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
            uint32_t v = win32_getenv(name);
            if (v) {
                o += snprintf(out + o, n - o, "%s", (const char *)G2H(v));
                if (o >= n) o = n - 1;
                p = e + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o + 1;
}
