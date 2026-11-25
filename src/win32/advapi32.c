// advapi32: an in-memory registry with the few values the game looks for.
// "Shell Folders\Personal" points at the virtual Documents folder, which the
// VFS maps to the user's real XDG Documents directory.
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

#include "../vfs.h"
#include "win32.h"

#define HKEY_CLASSES_ROOT 0x80000000u
#define HKEY_CURRENT_USER 0x80000001u
#define HKEY_LOCAL_MACHINE 0x80000002u
#define HKEY_USERS 0x80000003u
#define REG_SZ 1
#define REG_DWORD 4

typedef struct RegVal {
    char *key;  // lowercase full key path
    char *name; // lowercase value name ("" = default)
    uint32_t type, size;
    uint8_t *data;
} RegVal;

static RegVal *vals;
static int nvals, capvals;
static char **keys; // existing keys (lowercase)
static int nkeys, capkeys;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void lower(char *s) {
    for (; *s; s++) *s = tolower((unsigned char)*s);
}

static void add_key(const char *k) {
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i], k)) return;
    if (nkeys == capkeys) keys = realloc(keys, (capkeys = capkeys ? capkeys * 2 : 32) * sizeof *keys);
    keys[nkeys++] = strdup(k);
    // parents exist too
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", k);
    char *bs = strrchr(tmp, '\\');
    if (bs) { *bs = 0; add_key(tmp); }
}

static bool key_exists(const char *k) {
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i], k)) return true;
    return false;
}

static RegVal *find_val(const char *k, const char *n) {
    for (int i = 0; i < nvals; i++)
        if (!strcmp(vals[i].key, k) && !strcmp(vals[i].name, n)) return &vals[i];
    return NULL;
}

static void set_val(const char *kin, const char *nin, uint32_t type, const void *data, uint32_t size) {
    char k[512], n[256];
    snprintf(k, sizeof k, "%s", kin);
    snprintf(n, sizeof n, "%s", nin ? nin : "");
    lower(k);
    lower(n);
    add_key(k);
    RegVal *v = find_val(k, n);
    if (!v) {
        if (nvals == capvals) vals = realloc(vals, (capvals = capvals ? capvals * 2 : 32) * sizeof *vals);
        v = &vals[nvals++];
        v->key = strdup(k);
        v->name = strdup(n);
    } else {
        free(v->data);
    }
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    memcpy(v->data, data, size);
}

static void set_sz(const char *k, const char *n, const char *s) { set_val(k, n, REG_SZ, s, strlen(s) + 1); }

// What a running Windows Steam publishes: where steamclient.dll is and Steam's
// pid. Games and Steam's DRM wrapper use it to reach the client; our builtin
// steamclient.dll bridges to the Linux Steam (src/steam).
static void steam_registry(void) {
    const char *home = getenv("HOME");
    char p[1024];
    snprintf(p, sizeof p, "%s/.steam/steam.pid", home ? home : "");
    FILE *f = fopen(p, "r");
    uint32_t pid = 0;
    if (f) { if (fscanf(f, "%u", &pid) != 1) pid = 0; fclose(f); }
    snprintf(p, sizeof p, "/proc/%u", pid);
    struct stat st;
    if (!pid || stat(p, &st)) return; // Steam isn't running
    const char *sk = "hkey_current_user\\software\\valve\\steam";
    set_sz(sk, "SteamPath", "C:\\Program Files (x86)\\Steam");
    set_sz(sk, "SteamExe", "C:\\Program Files (x86)\\Steam\\steam.exe");
    const char *ap = "hkey_current_user\\software\\valve\\steam\\activeprocess";
    set_sz(ap, "SteamClientDll", "C:\\Program Files (x86)\\Steam\\steamclient.dll");
    set_sz(ap, "SteamClientDll64", "C:\\Program Files (x86)\\Steam\\steamclient64.dll");
    set_val(ap, "pid", REG_DWORD, &pid, 4);
    uint32_t user = 1; // a logged-in user (the real account id isn't known before connecting)
    set_val(ap, "ActiveUser", REG_DWORD, &user, 4);
    const char *lm = "hkey_local_machine\\software\\valve\\steam";
    set_sz(lm, "InstallPath", "C:\\Program Files (x86)\\Steam");
}

__attribute__((constructor)) static void reg_defaults(void) {
    const char *sf = "hkey_current_user\\software\\microsoft\\windows\\currentversion\\explorer\\shell folders";
    set_sz(sf, "Personal", VFS_DOCS_VDIR);
    set_sz(sf, "AppData", VFS_ROAMING_VDIR);
    set_sz(sf, "Local AppData", VFS_LOCAL_VDIR);
    set_sz(sf, "Common AppData", VFS_PROGRAMDATA_VDIR);
    set_sz(sf, "My Music", "C:\\Users\\Player\\Music");
    set_sz(sf, "My Pictures", "C:\\Users\\Player\\Pictures");
    const char *usf = "hkey_current_user\\software\\microsoft\\windows\\currentversion\\explorer\\user shell folders";
    set_sz(usf, "Personal", VFS_DOCS_VDIR);
    const char *dx = "hkey_local_machine\\software\\microsoft\\directx";
    set_sz(dx, "Version", "4.09.00.0904");
    add_key("hkey_local_machine\\software\\microsoft\\direct3d");
    const char *rs = "hkey_local_machine\\software\\rockstar games\\gta san andreas\\installation";
    set_sz(rs, "ExePath", VFS_GAME_VDIR "\\gta_sa.exe");
    set_sz(rs, "Installed", "1");
    steam_registry();
}

static const char *root_name(uint32_t h) {
    switch (h) {
    case HKEY_CLASSES_ROOT: return "hkey_classes_root";
    case HKEY_CURRENT_USER: return "hkey_current_user";
    case HKEY_LOCAL_MACHINE: return "hkey_local_machine";
    case HKEY_USERS: return "hkey_users";
    }
    return NULL;
}

static bool key_path(uint32_t h, const char *sub, char *out, size_t n) {
    const char *base = root_name(h);
    if (!base) {
        Object *o = handle_get(h, OBJ_REGKEY);
        if (!o) return false;
        base = o->regkey.path;
    }
    if (sub && *sub) snprintf(out, n, "%s\\%s", base, sub[0] == '\\' ? sub + 1 : sub);
    else snprintf(out, n, "%s", base);
    lower(out);
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
    return true;
}

static uint32_t open_key(uint32_t h, const char *sub, uint32_t *out, bool create) {
    char p[512];
    if (!key_path(h, sub, p, sizeof p)) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_lock);
    bool ex = key_exists(p);
    if (!ex && create) add_key(p);
    pthread_mutex_unlock(&reg_lock);
    if (!ex && !create) {
        LOGD("RegOpenKey(%s): not found", p);
        return ERROR_FILE_NOT_FOUND;
    }
    Object *o = obj_new(OBJ_REGKEY);
    o->regkey.path = strdup(p);
    *out = handle_alloc(o);
    return ERROR_SUCCESS;
}

API(advapi32, RegOpenKeyA, 3) { return open_key(ARG(0), ARG(1) ? STR(1) : NULL, PTR(2), false); }
API(advapi32, RegOpenKeyExA, 5) { return open_key(ARG(0), ARG(1) ? STR(1) : NULL, PTR(4), false); }
API(advapi32, RegCreateKeyA, 3) { return open_key(ARG(0), ARG(1) ? STR(1) : NULL, PTR(2), true); }
API(advapi32, RegCreateKeyExA, 9) {
    uint32_t r = open_key(ARG(0), ARG(1) ? STR(1) : NULL, PTR(7), true);
    if (ARG(8)) *(uint32_t *)PTR(8) = 2; // REG_OPENED_EXISTING_KEY
    return r;
}
API(advapi32, RegCloseKey, 1) {
    Object *o = handle_get(ARG(0), OBJ_REGKEY);
    if (o) { free(o->regkey.path); o->regkey.path = NULL; handle_close(ARG(0)); }
    return ERROR_SUCCESS;
}
API(advapi32, RegQueryValueExA, 6) {
    char p[512], n[256];
    if (!key_path(ARG(0), NULL, p, sizeof p)) return ERROR_INVALID_HANDLE;
    snprintf(n, sizeof n, "%s", ARG(1) ? STR(1) : "");
    lower(n);
    pthread_mutex_lock(&reg_lock);
    RegVal *v = find_val(p, n);
    uint32_t r = ERROR_SUCCESS;
    if (!v) {
        r = ERROR_FILE_NOT_FOUND;
        LOGD("RegQueryValueEx(%s\\%s): not found", p, n);
    } else {
        if (ARG(3)) *(uint32_t *)PTR(3) = v->type;
        uint32_t *len = PTR(5);
        if (ARG(4)) {
            if (!len || *len < v->size) r = ERROR_MORE_DATA;
            else memcpy(PTR(4), v->data, v->size);
        }
        if (len) *len = v->size;
        LOGD("RegQueryValueEx(%s\\%s) = %s", p, n, v->type == REG_SZ ? (char *)v->data : "<bin>");
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}
// Wide version: string values are stored narrow, so they are widened on the way out.
API(advapi32, RegQueryValueExW, 6) {
    char p[512], n[256];
    if (!key_path(ARG(0), NULL, p, sizeof p)) return ERROR_INVALID_HANDLE;
    if (ARG(1)) w2a(WSTR(1), n, sizeof n);
    else n[0] = 0;
    lower(n);
    pthread_mutex_lock(&reg_lock);
    RegVal *v = find_val(p, n);
    uint32_t r = ERROR_SUCCESS;
    if (!v) {
        r = ERROR_FILE_NOT_FOUND;
        LOGD("RegQueryValueExW(%s\\%s): not found", p, n);
    } else {
        bool str = v->type == REG_SZ || v->type == 2 /*REG_EXPAND_SZ*/;
        uint32_t need = str ? (uint32_t)(strlen((char *)v->data) + 1) * 2 : v->size;
        if (ARG(3)) *(uint32_t *)PTR(3) = v->type;
        uint32_t *len = PTR(5);
        if (ARG(4)) {
            if (!len || *len < need) r = ERROR_MORE_DATA;
            else if (str) a2w((char *)v->data, PTR(4), need / 2);
            else memcpy(PTR(4), v->data, v->size);
        }
        if (len) *len = need;
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}
API(advapi32, RegSetValueExA, 6) {
    char p[512];
    if (!key_path(ARG(0), NULL, p, sizeof p)) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_lock);
    set_val(p, ARG(1) ? STR(1) : "", ARG(3), PTR(4), ARG(5));
    pthread_mutex_unlock(&reg_lock);
    return ERROR_SUCCESS;
}
API(advapi32, RegSetValueA, 5) {
    char p[512];
    if (!key_path(ARG(0), ARG(1) ? STR(1) : NULL, p, sizeof p)) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_lock);
    set_val(p, "", REG_SZ, PTR(3), strlen(STR(3)) + 1);
    pthread_mutex_unlock(&reg_lock);
    return ERROR_SUCCESS;
}
API(advapi32, RegDeleteKeyA, 2) { return ERROR_SUCCESS; }
API(advapi32, RegDeleteValueA, 2) { return ERROR_SUCCESS; }
API(advapi32, RegEnumKeyExA, 8) { return ERROR_NO_MORE_ITEMS; }
API(advapi32, GetUserNameA, 2) {
    uint32_t *len = PTR(1);
    if (*len < 7) { *len = 7; return FALSE; }
    strcpy(PTR(0), "Player");
    *len = 7;
    return TRUE;
}

// ---------------------------------------------------------------- security: a standard, non-elevated user
API(advapi32, OpenProcessToken, 3) { *(uint32_t *)PTR(2) = handle_alloc(obj_new(OBJ_TOKEN)); return TRUE; }
API(advapi32, OpenThreadToken, 4) { set_last_error(1008 /*ERROR_NO_TOKEN*/); return FALSE; }
// GetTokenInformation(token, class, buf, len, retlen)
API(advapi32, GetTokenInformation, 5) {
    static const uint8_t user_sid[] = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0xE9, 3, 0, 0};
    static const uint8_t medium_sid[] = {1, 1, 0, 0, 0, 0, 0, 16, 0, 0x20, 0, 0}; // S-1-16-8192
    uint8_t tmp[64] = {0};
    uint32_t need;
    switch (ARG(1)) {
    case 1: // TokenUser: SID_AND_ATTRIBUTES { PSID, attrs } + SID
    case 25: { // TokenIntegrityLevel: same layout
        const uint8_t *sid = ARG(1) == 1 ? user_sid : medium_sid;
        size_t sl = ARG(1) == 1 ? sizeof user_sid : sizeof medium_sid;
        need = 8 + sl;
        if (ARG(4)) *(uint32_t *)PTR(4) = need;
        if (ARG(3) < need) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        uint32_t *o = PTR(2);
        o[0] = ARG(2) + 8;
        o[1] = ARG(1) == 25 ? 0x20 /*SE_GROUP_INTEGRITY*/ : 0;
        memcpy((uint8_t *)PTR(2) + 8, sid, sl);
        return TRUE;
    }
    case 18: *(uint32_t *)tmp = 1; need = 4; break; // TokenElevationType: default
    case 20: need = 4; break;                       // TokenElevation: not elevated
    case 8: *(uint32_t *)tmp = 1; need = 4; break;  // TokenType: primary
    default:
        LOGD("GetTokenInformation(class %u) not supported", ARG(1));
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ARG(4)) *(uint32_t *)PTR(4) = need;
    if (ARG(3) < need) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(PTR(2), tmp, need);
    return TRUE;
}
API(advapi32, LookupPrivilegeValueA, 3) { uint32_t *l = PTR(2); l[0] = 20; l[1] = 0; return TRUE; }
API(advapi32, LookupPrivilegeValueW, 3) { uint32_t *l = PTR(2); l[0] = 20; l[1] = 0; return TRUE; }
API(advapi32, AdjustTokenPrivileges, 6) {
    set_last_error(1300 /*ERROR_NOT_ALL_ASSIGNED*/);
    return TRUE;
}
// SystemFunction036 == RtlGenRandom: fill the buffer with random bytes.
// (hThread/handle-less; args are buffer, length.) Used by newer CRT/steam_api
// to seed their RNG.
API(advapi32, SystemFunction036, 2) {
    uint32_t len = ARG(1);
    if (len && ARG(0)) {
        uint8_t *p = PTR(0);
        for (uint32_t i = 0; i < len; i++) p[i] = (uint8_t)rand();
    }
    return TRUE;
}
