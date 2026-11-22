// kernel32: assorted functions needed by newer MSVC runtimes, ASI plugins and
// third-party DLLs (Miles, SilentPatch, ...).
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

#include "../heap.h"
#include "../pe.h"
#include "../vfs.h"
#include "win32.h"

// The pointer-encoding functions just need to round-trip.
API(kernel32, EncodePointer, 1) { return ARG(0) ^ 0x5A57A5A5u; }
API(kernel32, DecodePointer, 1) { return ARG(0) ^ 0x5A57A5A5u; }
API(kernel32, EncodeSystemPointer, 1) { return ARG(0) ^ 0x5A57A5A5u; }
API(kernel32, DecodeSystemPointer, 1) { return ARG(0) ^ 0x5A57A5A5u; }
API(kernel32, IsDebuggerPresent, 0) { return FALSE; }
API(kernel32, DisableThreadLibraryCalls, 1) { return TRUE; }
API(kernel32, GetCommandLineW, 0) {
    static uint32_t w;
    if (!w) {
        const char *a = G2H(g_guest_cmdline);
        size_t n = strlen(a) + 1;
        w = gheap_alloc(n * 2, 0);
        a2w(a, G2H(w), n);
    }
    return w;
}
API(kernel32, MulDiv, 3) {
    int32_t c = (int32_t)ARG(2);
    if (!c) return (uint32_t)-1;
    int64_t r = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    r = r >= 0 ? (r + c / 2) / c : (r - c / 2) / c; // rounded like Windows
    return (uint32_t)(int32_t)r;
}

// Fls* are probed with GetProcAddress by newer CRTs; implement them on top of TLS.
uint32_t tls_alloc_slot(void);
API(kernel32, FlsAlloc, 1) { return tls_alloc_slot(); }
API(kernel32, FlsGetValue, 1) {
    if (ARG(0) >= TLS_MINIMUM_AVAILABLE) return 0;
    return *teb_u32(thread_current(), TEB_TLS_SLOTS + 4 * ARG(0));
}
API(kernel32, FlsSetValue, 2) {
    if (ARG(0) >= TLS_MINIMUM_AVAILABLE) return FALSE;
    *teb_u32(thread_current(), TEB_TLS_SLOTS + 4 * ARG(0)) = ARG(1);
    return TRUE;
}
API(kernel32, FlsFree, 1) { return TRUE; }

// Interlocked singly-linked lists (SLIST_HEADER: {next, depth|sequence})
API(kernel32, InitializeSListHead, 1) { memset(PTR(0), 0, 8); return 0; }
API(kernel32, InterlockedFlushSList, 1) {
    return __atomic_exchange_n((uint32_t *)PTR(0), 0, __ATOMIC_SEQ_CST);
}
API(kernel32, InterlockedPushEntrySList, 2) {
    uint32_t *head = PTR(0), *e = PTR(1);
    uint32_t old = __atomic_load_n(head, __ATOMIC_SEQ_CST);
    do { e[0] = old; } while (!__atomic_compare_exchange_n(head, &old, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return old;
}
API(kernel32, InterlockedPopEntrySList, 1) {
    uint32_t *head = PTR(0);
    uint32_t old = __atomic_load_n(head, __ATOMIC_SEQ_CST);
    while (old && !__atomic_compare_exchange_n(head, &old, *(uint32_t *)G2H(old), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    return old;
}

// Activation contexts (manifests): accepted and ignored.
API(kernel32, CreateActCtxW, 1) { return 0x00ACC000; }
API(kernel32, CreateActCtxA, 1) { return 0x00ACC000; }
API(kernel32, ActivateActCtx, 2) { if (ARG(1)) *(uint32_t *)PTR(1) = 1; return TRUE; }
API(kernel32, DeactivateActCtx, 2) { return TRUE; }
API(kernel32, ReleaseActCtx, 1) { return 0; }

API(kernel32, GetModuleHandleExW, 3) {
    // flags: 2 = UNCHANGED_REFCOUNT, 4 = FROM_ADDRESS
    uint32_t h = 0;
    if (ARG(0) & 4) {
        LmQuery q;
        if (lm_query(ARG(1), &q) && q.alloc_base && module_by_base(q.alloc_base)) h = q.alloc_base;
    } else if (!ARG(1)) {
        h = g_exe.base;
    } else {
        char n[260];
        w2a(WSTR(1), n, sizeof n);
        h = module_get(n, false);
    }
    if (ARG(2)) *(uint32_t *)PTR(2) = h;
    return h != 0;
}
API(kernel32, GetModuleHandleExA, 3) {
    uint32_t h = 0;
    if (ARG(0) & 4) {
        LmQuery q;
        if (lm_query(ARG(1), &q) && q.alloc_base && module_by_base(q.alloc_base)) h = q.alloc_base;
    } else {
        h = module_get(ARG(1) ? STR(1) : NULL, false);
    }
    if (ARG(2)) *(uint32_t *)PTR(2) = h;
    return h != 0;
}
API(kernel32, LoadLibraryExW, 3) {
    char n[260], p[512];
    w2a(WSTR(0), n, sizeof n);
    if ((ARG(2) & 0x800) && !strchr(n, '\\') && !strchr(n, '/')) { // LOAD_LIBRARY_SEARCH_SYSTEM32
        snprintf(p, sizeof p, "C:\\Windows\\System32\\%s", n);
        snprintf(n, sizeof n, "%s", p);
    }
    uint32_t h = module_get(n, true);
    if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}

// Local/Global memory: fixed blocks, handle == pointer
API(kernel32, LocalLock, 1) { return ARG(0); }
API(kernel32, LocalUnlock, 1) { return TRUE; }
API(kernel32, LocalReAlloc, 3) { return gheap_realloc(ARG(0), ARG(1), ARG(2) & 0x40); }
API(kernel32, LocalSize, 1) { return gheap_size(ARG(0)); }
API(kernel32, GlobalReAlloc, 3) { return gheap_realloc(ARG(0), ARG(1), ARG(2) & 0x40); }
API(kernel32, GlobalHandle, 1) { return ARG(0); }
API(kernel32, GlobalSize, 1) { return gheap_size(ARG(0)); }

// ---------------------------------------------------------------- INI files
static bool ini_get(const char *file, const char *sect, const char *key, char *out, size_t outsz) {
    char host[1024];
    char path[512];
    // relative names are looked up in the Windows directory on real Windows;
    // games pass full paths or files next to the exe, so try the cwd.
    snprintf(path, sizeof path, "%s", file);
    if (!vfs_to_host(path, host, sizeof host)) return false;
    FILE *f = fopen(host, "r");
    if (!f) return false;
    char line[1024];
    bool in = false, found = false;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) *e = 0;
            in = !strcasecmp(p + 1, sect);
            continue;
        }
        if (!in || *p == ';' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(k, key)) {
            snprintf(out, outsz, "%s", v);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}
API(kernel32, GetPrivateProfileIntA, 4) {
    char v[256];
    if (!ini_get(STR(3), STR(0), STR(1), v, sizeof v)) return ARG(2);
    return (uint32_t)strtol(v, NULL, 0);
}
API(kernel32, GetPrivateProfileIntW, 4) {
    char f[260], s[128], k[128], v[256];
    w2a(WSTR(3), f, sizeof f);
    w2a(WSTR(0), s, sizeof s);
    w2a(WSTR(1), k, sizeof k);
    if (!ini_get(f, s, k, v, sizeof v)) return ARG(2);
    return (uint32_t)strtol(v, NULL, 0);
}
API(kernel32, GetPrivateProfileStringA, 6) {
    char v[1024];
    const char *res = ARG(1) && ARG(0) && ini_get(STR(5), STR(0), STR(1), v, sizeof v) ? v : (ARG(2) ? STR(2) : "");
    char *dst = PTR(3);
    uint32_t n = ARG(4);
    if (!n) return 0;
    size_t l = strlen(res);
    if (l >= n) l = n - 1;
    memcpy(dst, res, l);
    dst[l] = 0;
    return l;
}
API(kernel32, GetPrivateProfileSectionW, 4) {
    // Only used to probe for a section; report it empty.
    uint16_t *dst = PTR(1);
    if (ARG(2) >= 2) dst[0] = dst[1] = 0;
    return 0;
}
API(kernel32, GetProfileStringA, 5) {
    char *dst = PTR(3);
    const char *def = ARG(2) ? STR(2) : "";
    if (!ARG(4)) return 0;
    snprintf(dst, ARG(4), "%s", def);
    return strlen(dst);
}
API(kernel32, WritePrivateProfileStringA, 4) { return TRUE; }

// ---------------------------------------------------------------- misc
API(kernel32, FindFirstFileExA, 6) {
    // FindExInfoStandard + FindExSearchNameMatch only
    extern uint32_t wineless_find_first(const char *pattern, void *data);
    return wineless_find_first(STR(0), PTR(2));
}
API(kernel32, OpenFile, 3) {
    LOGI("OpenFile(%s): not supported", STR(0));
    return 0xFFFFFFFF;
}
API(kernel32, CreateProcessA, 10) {
    LOGW("CreateProcessA(%s, %s): not supported", ARG(0) ? STR(0) : "", ARG(1) ? STR(1) : "");
    set_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
API(kernel32, GetConsoleCP, 0) { return 437; }
API(kernel32, GetConsoleMode, 2) { return FALSE; }
API(kernel32, WriteConsoleW, 5) {
    char b[1024];
    uint32_t n = ARG(2) < 1023 ? ARG(2) : 1023;
    const uint16_t *w = PTR(1);
    for (uint32_t i = 0; i < n; i++) b[i] = w[i] < 256 ? (char)w[i] : '?';
    b[n] = 0;
    fputs(b, stderr);
    if (ARG(3)) *(uint32_t *)PTR(3) = ARG(2);
    return TRUE;
}
API(kernel32, WriteConsoleA, 5) {
    char b[1024];
    uint32_t n = ARG(2) < 1023 ? ARG(2) : 1023;
    const char *s = PTR(1);
    for (uint32_t i = 0; i < n; i++) b[i] = s[i];
    b[n] = 0;
    fputs(b, stderr);
    if (ARG(3)) *(uint32_t *)PTR(3) = ARG(2);
    return TRUE;
}
API(kernel32, ReadConsoleW, 5) { return FALSE; }
API(kernel32, GetProcessAffinityMask, 3) {
    if (ARG(1)) *(uint32_t *)PTR(1) = 1;
    if (ARG(2)) *(uint32_t *)PTR(2) = 1;
    return TRUE;
}
API(kernel32, SetThreadAffinityMask, 2) { return 1; }
API(kernel32, GetThreadContext, 2) { return FALSE; }
API(kernel32, OpenEventA, 3) {
    Object *o = obj_find_named(STR(2), OBJ_EVENT);
    if (!o) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    return handle_alloc(o);
}
API(kernel32, OpenFileMappingA, 3) {
    Object *o = obj_find_named(STR(2), OBJ_MAPPING);
    if (!o) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    return handle_alloc(o);
}
API(kernel32, OpenMutexA, 3) {
    Object *o = obj_find_named(STR(2), OBJ_MUTEX);
    if (!o) { set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    return handle_alloc(o);
}

// ole32
API(ole32, StringFromGUID2, 3) {
    const uint8_t *g = PTR(0);
    char b[64];
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9], g[10], g[11],
             g[12], g[13], g[14], g[15]);
    uint32_t n = strlen(b) + 1;
    if (ARG(2) < n) return 0;
    a2w(b, PTR(1), n);
    return n;
}

uint32_t k32_get_profile_string(uint32_t *a) { return api_kernel32_GetPrivateProfileStringA(a); }
