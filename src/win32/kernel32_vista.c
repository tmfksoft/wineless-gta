// kernel32 functions from Vista and later that modern MSVC runtimes and plugin
// loaders (Ultimate ASI Loader, ...) import directly, plus module enumeration
// (Toolhelp / PSAPI) and a few process/thread queries.
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "../heap.h"
#include "../pe.h"
#include "../vfs.h"
#include "win32.h"

// ---------------------------------------------------------------- strings / locale
API(kernel32, LCMapStringEx, 9) {
    // (locale name, flags, src, srclen, dst, dstlen, version, reserved, sortHandle)
    uint32_t args[6] = {0x409, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)};
    extern uint32_t k32_lcmap_w(uint32_t *a);
    return k32_lcmap_w(args);
}
API(kernel32, CompareStringEx, 9) {
    uint32_t args[6] = {0x409, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)};
    extern uint32_t k32_compare_w(uint32_t *a);
    return k32_compare_w(args);
}
API(kernel32, CompareStringOrdinal, 5) {
    const uint16_t *a = PTR(0), *b = PTR(2);
    int32_t na = (int32_t)ARG(1), nb = (int32_t)ARG(3);
    if (na < 0) na = wlen(a);
    if (nb < 0) nb = wlen(b);
    for (int i = 0;; i++) {
        if (i >= na || i >= nb) return na < nb ? 1 : na > nb ? 3 : 2;
        uint32_t x = a[i], y = b[i];
        if (ARG(4)) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x != y) return x < y ? 1 : 3;
    }
}
API(kernel32, AreFileApisANSI, 0) { return TRUE; }
API(kernel32, GetConsoleOutputCP, 0) { return 437; }
API(kernel32, OutputDebugStringW, 1) {
    char b[1024];
    w2a(WSTR(0), b, sizeof b);
    LOGD("OutputDebugString: %s", b);
    return 0;
}
API(kernel32, FormatMessageA, 7) {
    // FORMAT_MESSAGE_FROM_SYSTEM: just describe the code
    char b[128];
    snprintf(b, sizeof b, "Error %u\r\n", ARG(2));
    if (ARG(0) & 0x100) { // ALLOCATE_BUFFER
        uint32_t p = gheap_strdup(b);
        *(uint32_t *)PTR(4) = p;
        return strlen(b);
    }
    if (!ARG(5)) return 0;
    snprintf(PTR(4), ARG(5), "%s", b);
    return strlen(STR(4));
}

// ---------------------------------------------------------------- time
static void ft_now(uint32_t *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t v = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
    ft[0] = (uint32_t)v;
    ft[1] = (uint32_t)(v >> 32);
}
API(kernel32, GetSystemTimePreciseAsFileTime, 1) { ft_now(PTR(0)); return 0; }
API(kernel32, GetTickCount64, 0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t ms = (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    api_ret_edx((uint32_t)(ms >> 32));
    return (uint32_t)ms;
}
API(kernel32, SystemTimeToFileTime, 2) {
    const uint16_t *st = PTR(0);
    struct tm tm = {0};
    tm.tm_year = st[0] - 1900; tm.tm_mon = st[1] - 1; tm.tm_mday = st[3];
    tm.tm_hour = st[4]; tm.tm_min = st[5]; tm.tm_sec = st[6];
    time_t t = timegm(&tm);
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)st[7] * 10000;
    uint32_t *ft = PTR(1);
    ft[0] = (uint32_t)v;
    ft[1] = (uint32_t)(v >> 32);
    return TRUE;
}
API(kernel32, GetProcessTimes, 5) {
    for (int i = 1; i <= 4; i++)
        if (ARG(i)) memset(PTR(i), 0, 8);
    if (ARG(1)) ft_now(PTR(1));
    return TRUE;
}
// Same shape as GetProcessTimes: (hThread, creation, exit, kernel, user).
// We don't track per-thread CPU time, so report zeros with a plausible
// creation time. AoE2 HD's steam_api.dll calls this during init.
API(kernel32, GetThreadTimes, 5) {
    for (int i = 1; i <= 4; i++)
        if (ARG(i)) memset(PTR(i), 0, 8);
    if (ARG(1)) ft_now(PTR(1));
    return TRUE;
}

// ---------------------------------------------------------------- system info
API(kernel32, GetNativeSystemInfo, 1) {
    extern uint32_t k32_get_system_info(uint32_t *a);
    uint32_t args[1] = {ARG(0)};
    return k32_get_system_info(args);
}
API(kernel32, IsWow64Process, 2) {
    if (ARG(1)) *(uint32_t *)PTR(1) = FALSE; // a plain 32-bit Windows, as far as the game knows
    return TRUE;
}
API(kernel32, GlobalMemoryStatusEx, 1) {
    uint32_t *m = PTR(0);
    struct sysinfo si;
    sysinfo(&si);
    uint64_t total = (uint64_t)si.totalram * si.mem_unit, avail = (uint64_t)si.freeram * si.mem_unit;
    m[1] = total ? (uint32_t)(100 - avail * 100 / total) : 50;
    memcpy(m + 2, &total, 8);
    memcpy(m + 4, &avail, 8);
    uint64_t pf = total * 2, va = 0x7FFE0000ull, vfree = 0x70000000ull, z = 0;
    memcpy(m + 6, &pf, 8);
    memcpy(m + 8, &pf, 8);
    memcpy(m + 10, &va, 8);
    memcpy(m + 12, &vfree, 8);
    memcpy(m + 14, &z, 8);
    return TRUE;
}
API(kernel32, SetPriorityClass, 2) { return TRUE; }
API(kernel32, GetPriorityClass, 1) { return 0x20; } // NORMAL_PRIORITY_CLASS
API(kernel32, SetThreadStackGuarantee, 1) { if (ARG(0)) *(uint32_t *)PTR(0) = 0; return TRUE; }
API(kernel32, SwitchToThread, 0) { sched_yield(); return TRUE; }
API(kernel32, GetThreadId, 1) {
    Object *o = handle_get(ARG(0), OBJ_THREAD);
    return o ? o->thread.t->tid : 0;
}
API(kernel32, GetProcessId, 1) { return ARG(0) == PSEUDO_PROCESS ? (uint32_t)getpid() : 0; }
API(kernel32, OpenThread, 3) {
    extern Thread *g_threads[];
    extern int g_nthreads;
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i]->tid == ARG(2)) return handle_dup(g_threads[i]->handle);
    return 0;
}
API(kernel32, OpenMutexW, 3) {
    char n[260];
    w2a(WSTR(2), n, sizeof n);
    Object *o = obj_find_named(n, OBJ_MUTEX);
    if (!o) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    return handle_alloc(o);
}
API(kernel32, GetProcessHandleCount, 2) { *(uint32_t *)PTR(1) = 64; return TRUE; }
API(kernel32, HeapLock, 1) { return TRUE; }
API(kernel32, HeapUnlock, 1) { return TRUE; }

// Vectored exception handlers: accepted (not called; hardware faults don't go
// through SEH yet).
API(kernel32, AddVectoredExceptionHandler, 2) { return 0x00BEEF00 + ARG(1); }
API(kernel32, RemoveVectoredExceptionHandler, 1) { return TRUE; }

// ---------------------------------------------------------------- SRW locks / condition variables / init once
// SRW lock = one guest dword; implemented with atomics and short sleeps.
static void srw_wait(void) { sched_yield(); }
API(kernel32, AcquireSRWLockExclusive, 1) {
    uint32_t *l = PTR(0), z = 0;
    while (!__atomic_compare_exchange_n(l, &z, 0x80000000u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { z = 0; srw_wait(); }
    return 0;
}
API(kernel32, TryAcquireSRWLockExclusive, 1) {
    uint32_t *l = PTR(0), z = 0;
    return __atomic_compare_exchange_n(l, &z, 0x80000000u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
API(kernel32, ReleaseSRWLockExclusive, 1) { __atomic_store_n((uint32_t *)PTR(0), 0, __ATOMIC_RELEASE); return 0; }
API(kernel32, AcquireSRWLockShared, 1) {
    uint32_t *l = PTR(0);
    for (;;) {
        uint32_t v = __atomic_load_n(l, __ATOMIC_RELAXED);
        if (!(v & 0x80000000u) && __atomic_compare_exchange_n(l, &v, v + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        srw_wait();
    }
    return 0;
}
API(kernel32, TryAcquireSRWLockShared, 1) {
    uint32_t *l = PTR(0), v = __atomic_load_n(l, __ATOMIC_RELAXED);
    return !(v & 0x80000000u) && __atomic_compare_exchange_n(l, &v, v + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
API(kernel32, ReleaseSRWLockShared, 1) { __atomic_sub_fetch((uint32_t *)PTR(0), 1, __ATOMIC_RELEASE); return 0; }
API(kernel32, InitializeSRWLock, 1) { *(uint32_t *)PTR(0) = 0; return 0; }

// Condition variable = guest dword used as a generation counter.
API(kernel32, InitializeConditionVariable, 1) { *(uint32_t *)PTR(0) = 0; return 0; }
API(kernel32, WakeAllConditionVariable, 1) { __atomic_add_fetch((uint32_t *)PTR(0), 1, __ATOMIC_RELEASE); return 0; }
API(kernel32, WakeConditionVariable, 1) { __atomic_add_fetch((uint32_t *)PTR(0), 1, __ATOMIC_RELEASE); return 0; }
API(kernel32, SleepConditionVariableSRW, 4) {
    uint32_t *cv = PTR(0), *lock = PTR(1), ms = ARG(2), shared = ARG(3) & 1;
    uint32_t gen = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    if (shared) __atomic_sub_fetch(lock, 1, __ATOMIC_RELEASE);
    else __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    uint32_t start = win32_tick_ms();
    bool woke = false;
    while (!(woke = __atomic_load_n(cv, __ATOMIC_ACQUIRE) != gen)) {
        if (ms != 0xFFFFFFFF && win32_tick_ms() - start >= ms) break;
        usleep(200);
    }
    if (shared) {
        for (;;) {
            uint32_t v = __atomic_load_n(lock, __ATOMIC_RELAXED);
            if (!(v & 0x80000000u) && __atomic_compare_exchange_n(lock, &v, v + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            srw_wait();
        }
    } else {
        uint32_t z = 0;
        while (!__atomic_compare_exchange_n(lock, &z, 0x80000000u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { z = 0; srw_wait(); }
    }
    if (!woke) { set_last_error(0x5B4); return FALSE; } // ERROR_TIMEOUT
    return TRUE;
}

// INIT_ONCE: dword, 0 = not started, 1 = in progress, 2 = done
API(kernel32, InitOnceBeginInitialize, 4) {
    uint32_t *io = PTR(0), z = 0;
    if (ARG(3)) *(uint32_t *)PTR(3) = 0;
    for (;;) {
        if (__atomic_compare_exchange_n(io, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            if (ARG(2)) *(uint32_t *)PTR(2) = TRUE; // caller must initialize
            return TRUE;
        }
        if (z == 2) {
            if (ARG(2)) *(uint32_t *)PTR(2) = FALSE;
            return TRUE;
        }
        z = 0;
        sched_yield();
    }
}
API(kernel32, InitOnceComplete, 3) {
    __atomic_store_n((uint32_t *)PTR(0), (ARG(1) & 4) ? 0 : 2, __ATOMIC_RELEASE); // INIT_ONCE_INIT_FAILED
    return TRUE;
}
API(kernel32, InitOnceExecuteOnce, 4) {
    uint32_t *io = PTR(0), z = 0;
    for (;;) {
        if (__atomic_compare_exchange_n(io, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            uint32_t args[3] = {ARG(0), ARG(2), ARG(3)};
            uint32_t ok = (uint32_t)call32(thread_current()->ctx, ARG(1), 3, args);
            __atomic_store_n(io, ok ? 2 : 0, __ATOMIC_RELEASE);
            return ok;
        }
        if (z == 2) return TRUE;
        z = 0;
        sched_yield();
    }
}

// ---------------------------------------------------------------- files
static uint32_t file_attr_data(const char *name, uint8_t *d) {
    char host[1024];
    struct stat st;
    if (!vfs_to_host(name, host, sizeof host) || stat(host, &st)) { set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    memset(d, 0, 36);
    *(uint32_t *)d = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    memcpy(d + 4, &ft, 8);
    memcpy(d + 12, &ft, 8);
    memcpy(d + 20, &ft, 8);
    *(uint32_t *)(d + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
    *(uint32_t *)(d + 32) = (uint32_t)st.st_size;
    return TRUE;
}
API(kernel32, GetFileAttributesExA, 3) { return file_attr_data(STR(0), PTR(2)); }
API(kernel32, GetFileAttributesExW, 3) {
    char n[1024];
    w2a(WSTR(0), n, sizeof n);
    return file_attr_data(n, PTR(2));
}
API(kernel32, GetTempPathW, 2) {
    char t[512];
    snprintf(t, sizeof t, "%s\\", VFS_TEMP_VDIR);
    uint32_t n = strlen(t);
    if (ARG(0) <= n) return n + 1;
    a2w(t, PTR(1), n + 1);
    return n;
}
API(kernel32, GetShortPathNameA, 3) {
    uint32_t n = strlen(STR(0));
    if (ARG(2) <= n) return n + 1;
    memmove(PTR(1), PTR(0), n + 1);
    return n;
}
API(kernel32, MoveFileExW, 3) {
    char a1[1024], a2[1024], h1[1024], h2[1024];
    w2a(WSTR(0), a1, sizeof a1);
    if (!ARG(1)) return FALSE;
    w2a(WSTR(1), a2, sizeof a2);
    if (!vfs_to_host(a1, h1, sizeof h1) || !vfs_to_host(a2, h2, sizeof h2) || rename(h1, h2)) return FALSE;
    vfs_note_deleted(h1);
    vfs_note_created(h2, false);
    return TRUE;
}
API(kernel32, GetPrivateProfileStringW, 6) {
    // Wide INI reads: delegate to the ANSI version through temporaries.
    char f[512], s[128], k[128], d[256], out[1024];
    w2a(WSTR(5), f, sizeof f);
    if (ARG(0)) w2a(WSTR(0), s, sizeof s);
    if (ARG(1)) w2a(WSTR(1), k, sizeof k);
    if (ARG(2)) w2a(WSTR(2), d, sizeof d);
    uint32_t gs = ARG(0) ? gheap_strdup(s) : 0, gk = ARG(1) ? gheap_strdup(k) : 0, gd = ARG(2) ? gheap_strdup(d) : 0;
    uint32_t gf = gheap_strdup(f), gout = gheap_alloc(sizeof out, 1);
    extern uint32_t k32_get_profile_string(uint32_t *a);
    uint32_t args[6] = {gs, gk, gd, gout, sizeof out, gf};
    uint32_t n = k32_get_profile_string(args);
    if (ARG(4)) a2w(G2H(gout), PTR(3), ARG(4));
    uint32_t gl[] = {gs, gk, gd, gf, gout};
    for (int i = 0; i < 5; i++) if (gl[i]) gheap_free(gl[i]);
    return n < ARG(4) ? n : (ARG(4) ? ARG(4) - 1 : 0);
}
API(kernel32, SetEnvironmentVariableW, 2) { return TRUE; }
API(kernel32, CreateFileMappingW, 6) {
    extern uint32_t k32_create_mapping(uint32_t *a);
    uint32_t args[6] = {ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0};
    return k32_create_mapping(args);
}
API(kernel32, CancelIo, 1) { return TRUE; }

// ---------------------------------------------------------------- module enumeration
// Toolhelp snapshot: modules only.
#define TH32CS_SNAPMODULE 0x8
API(kernel32, CreateToolhelp32Snapshot, 2) {
    Object *o = obj_new(OBJ_FIND); // reuse: e[] holds module bases in .size
    int cap = 128;
    o->find.e = calloc(cap, sizeof *o->find.e);
    for (uint32_t b = 0x10000; b < 0x80000000u && o->find.n < cap; b += 0x10000) {
        Module *m = module_by_base(b);
        if (!m) continue;
        FindEntry *e = &o->find.e[o->find.n++];
        snprintf(e->name, sizeof e->name, "%s", m->vpath);
        e->size = m->base;
        e->mtime = m->size;
    }
    return handle_alloc(o);
}
static uint32_t fill_module_entry_w(Object *o, uint8_t *me) {
    if (!o || o->find.pos >= o->find.n) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    FindEntry *e = &o->find.e[o->find.pos++];
    // MODULEENTRY32W: dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage,
    // modBaseAddr, modBaseSize, hModule, szModule[256], szExePath[260]
    uint32_t *d = (uint32_t *)me;
    d[1] = 1; d[2] = getpid(); d[3] = d[4] = 1;
    d[5] = (uint32_t)e->size; d[6] = (uint32_t)e->mtime; d[7] = (uint32_t)e->size;
    const char *base = strrchr(e->name, '\\');
    a2w(base ? base + 1 : e->name, (uint16_t *)(me + 32), 256);
    a2w(e->name, (uint16_t *)(me + 32 + 512), 260);
    return TRUE;
}
API(kernel32, Module32FirstW, 2) {
    Object *o = handle_get(ARG(0), OBJ_FIND);
    if (o) o->find.pos = 0;
    return fill_module_entry_w(o, PTR(1));
}
API(kernel32, Module32NextW, 2) { return fill_module_entry_w(handle_get(ARG(0), OBJ_FIND), PTR(1)); }
static uint32_t fill_module_entry_a(Object *o, uint8_t *me) {
    if (!o || o->find.pos >= o->find.n) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    FindEntry *e = &o->find.e[o->find.pos++];
    uint32_t *d = (uint32_t *)me; // MODULEENTRY32 with narrow names
    d[1] = 1; d[2] = getpid(); d[3] = d[4] = 1;
    d[5] = (uint32_t)e->size; d[6] = (uint32_t)e->mtime; d[7] = (uint32_t)e->size;
    const char *base = strrchr(e->name, '\\');
    snprintf((char *)me + 32, 256, "%s", base ? base + 1 : e->name);
    snprintf((char *)me + 32 + 256, 260, "%s", e->name);
    return TRUE;
}
API(kernel32, Module32First, 2) {
    Object *o = handle_get(ARG(0), OBJ_FIND);
    if (o) o->find.pos = 0;
    return fill_module_entry_a(o, PTR(1));
}
API(kernel32, Module32Next, 2) { return fill_module_entry_a(handle_get(ARG(0), OBJ_FIND), PTR(1)); }
// Processes: just this one.
static uint32_t fill_process_entry(uint8_t *pe, bool first, bool wide) {
    if (!first) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    uint32_t *d = (uint32_t *)pe;
    memset(pe + 4, 0, 32);
    d[2] = getpid();
    d[5] = 1;
    d[7] = 8;
    const char *name = strrchr(g_exe.vpath, '\\');
    name = name ? name + 1 : g_exe.vpath;
    if (wide) a2w(name, (uint16_t *)(pe + 36), 260);
    else snprintf((char *)pe + 36, 260, "%s", name);
    return TRUE;
}
API(kernel32, Process32First, 2) { return fill_process_entry(PTR(1), true, false); }
API(kernel32, Process32Next, 2) { return fill_process_entry(PTR(1), false, false); }
API(kernel32, Process32FirstW, 2) { return fill_process_entry(PTR(1), true, true); }
API(kernel32, Process32NextW, 2) { return fill_process_entry(PTR(1), false, true); }
// Heaps aren't enumerable.
API(kernel32, Heap32ListFirst, 2) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
API(kernel32, Heap32ListNext, 2) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
API(kernel32, Heap32First, 3) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
API(kernel32, Heap32Next, 1) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
API(kernel32, CheckRemoteDebuggerPresent, 2) { *(uint32_t *)PTR(1) = FALSE; return TRUE; }
API(kernel32, Thread32First, 2) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
API(kernel32, Thread32Next, 2) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }

API(kernel32, K32EnumProcessModules, 4) {
    uint32_t *out = PTR(1), cap = ARG(2) / 4, n = 0;
    for (uint32_t b = 0x10000; b < 0x80000000u; b += 0x10000)
        if (module_by_base(b)) { if (n < cap) out[n] = b; n++; }
    *(uint32_t *)PTR(3) = n * 4;
    return TRUE;
}
API(kernel32, K32GetModuleInformation, 4) {
    Module *m = module_by_base(ARG(1));
    if (!m) return FALSE;
    uint32_t *mi = PTR(2);
    mi[0] = m->base;
    mi[1] = m->size;
    mi[2] = m->entry;
    return TRUE;
}
API(kernel32, K32GetProcessMemoryInfo, 3) {
    uint32_t *pm = PTR(1);
    memset(pm + 1, 0, ARG(2) - 4);
    return TRUE;
}
// GetModuleBaseName/GetModuleFileNameEx(process, module (0 = exe), buf, size)
static uint32_t module_name_out(uint32_t mod, bool base, bool wide, uint32_t buf, uint32_t size) {
    Module *m = module_by_base(mod ? mod : g_exe.base);
    if (!m || !size) return 0;
    const char *p = m->vpath;
    if (base) { const char *b = strrchr(p, '\\'); if (b) p = b + 1; }
    if (wide) { a2w(p, G2H(buf), size); return wlen(G2H(buf)); }
    snprintf(G2H(buf), size, "%s", p);
    return strlen(G2H(buf));
}
API(kernel32, K32GetModuleBaseNameA, 4) { return module_name_out(ARG(1), true, false, ARG(2), ARG(3)); }
API(kernel32, K32GetModuleBaseNameW, 4) { return module_name_out(ARG(1), true, true, ARG(2), ARG(3)); }
API(kernel32, K32GetModuleFileNameExA, 4) { return module_name_out(ARG(1), false, false, ARG(2), ARG(3)); }
API(kernel32, K32GetModuleFileNameExW, 4) { return module_name_out(ARG(1), false, true, ARG(2), ARG(3)); }
API(psapi, GetModuleBaseNameA, 4) { return module_name_out(ARG(1), true, false, ARG(2), ARG(3)); }
API(psapi, GetModuleBaseNameW, 4) { return module_name_out(ARG(1), true, true, ARG(2), ARG(3)); }
API(psapi, GetModuleFileNameExA, 4) { return module_name_out(ARG(1), false, false, ARG(2), ARG(3)); }
API(psapi, GetModuleFileNameExW, 4) { return module_name_out(ARG(1), false, true, ARG(2), ARG(3)); }
// GetMappedFileName(process, address, buf, size): the image containing the
// address, as an NT device path (C: is \Device\HarddiskVolume1).
static Module *module_containing(uint32_t addr) {
    for (uint32_t b = addr & ~0xFFFFu; b >= 0x10000 && b + 0x10000 > b; b -= 0x10000) {
        Module *m = module_by_base(b);
        if (m) return addr < m->base + m->size ? m : NULL;
    }
    return NULL;
}
static uint32_t mapped_name(uint32_t addr, bool wide, uint32_t buf, uint32_t size) {
    Module *m = module_containing(addr);
    if (!m || !size) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char p[600];
    snprintf(p, sizeof p, "\\Device\\HarddiskVolume1%s", m->vpath + 2); // drop "C:"
    if (wide) { a2w(p, G2H(buf), size); return wlen(G2H(buf)); }
    snprintf(G2H(buf), size, "%s", p);
    return strlen(G2H(buf));
}
API(psapi, GetMappedFileNameA, 4) { return mapped_name(ARG(1), false, ARG(2), ARG(3)); }
API(psapi, GetMappedFileNameW, 4) { return mapped_name(ARG(1), true, ARG(2), ARG(3)); }
API(kernel32, K32GetMappedFileNameA, 4) { return mapped_name(ARG(1), false, ARG(2), ARG(3)); }
API(kernel32, K32GetMappedFileNameW, 4) { return mapped_name(ARG(1), true, ARG(2), ARG(3)); }
// QueryDosDevice(name, buf, size): "C:" -> "\Device\HarddiskVolume1" (double-NUL list)
API(kernel32, QueryDosDeviceA, 3) {
    const char *t = (!ARG(0) || !strcasecmp(STR(0), "C:")) ? "\\Device\\HarddiskVolume1" : NULL;
    if (!t) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    uint32_t n = strlen(t) + 2;
    if (ARG(2) < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(PTR(1), t, n - 1);
    ((char *)PTR(1))[n - 1] = 0;
    return n;
}
API(kernel32, QueryDosDeviceW, 3) {
    char n[16] = "";
    if (ARG(0)) w2a(WSTR(0), n, sizeof n);
    if (ARG(0) && strcasecmp(n, "C:")) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    const char *t = "\\Device\\HarddiskVolume1";
    uint32_t len = strlen(t) + 2;
    if (ARG(2) < len) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    a2w(t, PTR(1), len - 1);
    ((uint16_t *)PTR(1))[len - 1] = 0;
    return len;
}
API(psapi, EnumProcesses, 3) {
    if (ARG(1) >= 4) *(uint32_t *)PTR(0) = getpid();
    *(uint32_t *)PTR(2) = 4;
    return TRUE;
}
API(psapi, EnumProcessModules, 4) { return api_kernel32_K32EnumProcessModules(_args); }
API(psapi, GetModuleInformation, 4) { return api_kernel32_K32GetModuleInformation(_args); }
API(psapi, GetProcessMemoryInfo, 3) { return api_kernel32_K32GetProcessMemoryInfo(_args); }

// ---------------------------------------------------------------- not available
API(kernel32, CreateJobObjectW, 2) { return 0; }
API(kernel32, AssignProcessToJobObject, 2) { return FALSE; }
API(kernel32, SetInformationJobObject, 4) { return FALSE; }
API(kernel32, CreateProcessW, 10) { set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API(kernel32, WaitNamedPipeW, 2) { set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
API(kernel32, GetNamedPipeServerProcessId, 2) { return FALSE; }
API(kernel32, ReadDirectoryChangesW, 8) { set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API(kernel32, SetThreadContext, 2) { return FALSE; }
API(kernel32, QueueUserAPC, 3) { return 0; }
API(kernel32, EnumResourceNamesW, 4) { return FALSE; }
API(kernel32, CreateThreadpoolTimer, 3) { return 0; }
API(kernel32, SetThreadpoolTimer, 4) { return 0; }
API(kernel32, WaitForThreadpoolTimerCallbacks, 2) { return 0; }
API(kernel32, FreeLibraryAndExitThread, 2) { thread_exit_current(ARG(1)); }
