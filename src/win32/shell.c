// shlwapi path helpers and the bits of shell32 that plugins (SilentPatch) use.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../heap.h"
#include "../vfs.h"
#include "win32.h"

// ---------------------------------------------------------------- ANSI paths
static void path_append(char *p, const char *more) {
    size_t l = strlen(p);
    while (*more == '\\') more++;
    if (l && p[l - 1] != '\\' && *more) strcat(p, "\\");
    strcat(p, more);
}
API(shlwapi, PathAppendA, 2) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", STR(0));
    path_append(tmp, STR(1));
    if (strlen(tmp) >= 260) return FALSE;
    strcpy(PTR(0), tmp);
    return TRUE;
}
API(shlwapi, PathCombineA, 3) {
    char tmp[1024];
    if (ARG(2) && (STR(2)[0] == '\\' || (STR(2)[0] && STR(2)[1] == ':'))) snprintf(tmp, sizeof tmp, "%s", STR(2));
    else {
        snprintf(tmp, sizeof tmp, "%s", ARG(1) ? STR(1) : "");
        if (ARG(2)) path_append(tmp, STR(2));
    }
    if (strlen(tmp) >= 260) return 0;
    strcpy(PTR(0), tmp);
    return ARG(0);
}
API(shlwapi, PathRemoveFileSpecA, 1) {
    char *p = PTR(0), *bs = strrchr(p, '\\');
    if (!bs) { if (*p) { *p = 0; return TRUE; } return FALSE; }
    if (bs == p || (bs == p + 2 && p[1] == ':')) bs[1] = 0;
    else *bs = 0;
    return TRUE;
}
API(shlwapi, PathFileExistsA, 1) {
    extern uint32_t wineless_file_exists(const char *path);
    return wineless_file_exists(STR(0));
}

// ---------------------------------------------------------------- wide paths
static size_t wl(const uint16_t *s) { size_t n = 0; while (s[n]) n++; return n; }
static void wcat(uint16_t *d, const uint16_t *s) { size_t n = wl(d); while ((d[n++] = *s++)) {} }
API(shlwapi, PathAppendW, 2) {
    uint16_t *p = PTR(0);
    const uint16_t *more = WSTR(1);
    while (*more == '\\') more++;
    size_t l = wl(p);
    if (l + wl(more) + 2 > 260) return FALSE;
    if (l && p[l - 1] != '\\' && *more) { p[l] = '\\'; p[l + 1] = 0; }
    wcat(p, more);
    return TRUE;
}
API(shlwapi, PathRemoveFileSpecW, 1) {
    uint16_t *p = PTR(0), *bs = NULL;
    for (uint16_t *q = p; *q; q++)
        if (*q == '\\') bs = q;
    if (!bs) { if (*p) { *p = 0; return TRUE; } return FALSE; }
    if (bs == p || (bs == p + 2 && p[1] == ':')) bs[1] = 0;
    else *bs = 0;
    return TRUE;
}
API(shlwapi, PathRenameExtensionW, 2) {
    uint16_t *p = PTR(0), *dot = NULL;
    for (uint16_t *q = p; *q; q++) {
        if (*q == '.') dot = q;
        else if (*q == '\\') dot = NULL;
    }
    if (!dot) dot = p + wl(p);
    if ((size_t)(dot - p) + wl(WSTR(1)) + 1 > 260) return FALSE;
    *dot = 0;
    wcat(p, WSTR(1));
    return TRUE;
}

// ---------------------------------------------------------------- shell32
#define CSIDL_PERSONAL 0x05
#define CSIDL_APPDATA 0x1a
#define CSIDL_LOCAL_APPDATA 0x1c
#define CSIDL_COMMON_APPDATA 0x23
#define CSIDL_MYDOCUMENTS 0x05
static const char *csidl_path(uint32_t csidl) {
    switch (csidl & 0xff) {
    case CSIDL_PERSONAL: return VFS_DOCS_VDIR;
    case CSIDL_APPDATA: return VFS_ROAMING_VDIR;
    case CSIDL_LOCAL_APPDATA: return VFS_LOCAL_VDIR;
    case CSIDL_COMMON_APPDATA: return VFS_PROGRAMDATA_VDIR;
    }
    LOGD("SHGetFolderPath(csidl 0x%x): not available", csidl);
    return NULL;
}
API(shell32, SHGetFolderPathA, 5) {
    const char *p = csidl_path(ARG(1));
    if (!p) return 0x80070002; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    strcpy(PTR(4), p);
    return 0;
}
API(shell32, SHGetFolderPathW, 5) {
    const char *p = csidl_path(ARG(1));
    if (!p) return 0x80070002;
    a2w(p, PTR(4), 260);
    return 0;
}
// SHGetSpecialFolderPathA(hwnd, out, csidl, create)
API(shell32, SHGetSpecialFolderPathA, 4) {
    const char *p = csidl_path(ARG(2));
    if (!p) return FALSE;
    strcpy(PTR(1), p);
    return TRUE;
}
API(shell32, CommandLineToArgvW, 2) {
    // Split the command line (quotes respected) into a LocalAlloc'd argv block.
    const uint16_t *cl = WSTR(0);
    char a[4096];
    w2a(cl, a, sizeof a);
    char *args[64];
    int n = 0;
    static char buf[4096];
    char *o = buf;
    for (const char *p = a; *p && n < 64;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        args[n++] = o;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') q = !q;
            else *o++ = *p;
            p++;
        }
        *o++ = 0;
    }
    size_t total = 4 * (n + 1);
    for (int i = 0; i < n; i++) total += 2 * (strlen(args[i]) + 1);
    uint32_t blk = gheap_alloc(total, 1);
    uint32_t *vec = G2H(blk);
    uint32_t s = blk + 4 * (n + 1);
    for (int i = 0; i < n; i++) {
        vec[i] = s;
        a2w(args[i], G2H(s), strlen(args[i]) + 1);
        s += 2 * (strlen(args[i]) + 1);
    }
    vec[n] = 0;
    if (ARG(1)) *(uint32_t *)PTR(1) = n;
    return blk;
}
API(shell32, ShellExecuteA, 6) {
    LOGI("ShellExecuteA(%s %s): not supported", ARG(1) ? STR(1) : "", ARG(2) ? STR(2) : "");
    return 2; // SE_ERR_FILENOTFOUND
}

// newer CRT probes
API(kernel32, InitializeCriticalSectionEx, 3) {
    extern uint32_t api_k32_init_cs(uint32_t cs);
    return api_k32_init_cs(ARG(0));
}
