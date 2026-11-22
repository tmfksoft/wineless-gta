// steam_api.dll for Windows games, bridged to the Linux Steam client.
//
// Like Proton's lsteamclient: the real Linux steamclient.so (shipped with
// Steam) is loaded into this process, and the game's 32-bit MSVC calls on Steam
// interfaces are forwarded to its 64-bit Itanium-ABI interfaces. Steam does
// all the work (login, ownership, achievements, cloud, networking) exactly as
// for a native Linux game.
//
// Each Steam interface the game sees is a guest object whose vtable slots all
// land in steam_slot(), which looks up the method's signature (generated from
// the Steamworks SDK headers by scripts/gen_steam.py) and converts the call:
// thiscall `this` in ecx, 64-bit values split across two stack slots, CSteamID
// returned through a hidden pointer, strings copied into guest memory.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../heap.h"
#include "../pe.h"
#include "../thread.h"
#include "../vfs.h"
#include "../win32/com.h"
#include "../win32/win32.h"

typedef struct {
    const char *name;
    int host_idx; // index in the Linux vtable
    const char *sig;
} SteamMethod;
typedef struct {
    const char *cls, *version;
    const SteamMethod *m;
    int n;
} SteamIfaceDesc;
#include "steam_ifaces.inc"

#define NIFACES (int)(sizeof steam_ifaces / sizeof *steam_ifaces)

// ---------------------------------------------------------------- steamclient.so
typedef struct {
    int32_t user;
    int32_t callback;
    uint8_t *param;
    int32_t size;
} CallbackMsg;

static void *lib;
static void *(*p_CreateInterface)(const char *, int *);
static bool (*p_BGetCallback)(int32_t pipe, CallbackMsg *msg);
static bool (*p_FreeLastCallback)(int32_t pipe);
static bool (*p_GetAPICallResult)(int32_t pipe, uint64_t call, void *buf, int size, int expected, bool *failed);

static void *client; // ISteamClient017 (host)
static int32_t pipe_h, user_h;
static bool inited;

static bool load_client(void) {
    if (lib) return true;
    const char *home = getenv("HOME");
    const char *compat = getenv("STEAM_COMPAT_CLIENT_INSTALL_PATH"); // set when Steam runs us as a compat tool
    char paths[4][1024];
    int n = 0;
    if (compat) snprintf(paths[n++], sizeof *paths, "%s/linux64/steamclient.so", compat);
    if (home) {
        snprintf(paths[n++], sizeof *paths, "%s/.steam/sdk64/steamclient.so", home);
        snprintf(paths[n++], sizeof *paths, "%s/.local/share/Steam/linux64/steamclient.so", home);
        snprintf(paths[n++], sizeof *paths, "%s/.steam/steam/linux64/steamclient.so", home);
    }
    for (int i = 0; i < n && !lib; i++) {
        lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (lib) LOGI("steam: loaded %s", paths[i]);
        else LOGD("steam: %s: %s", paths[i], dlerror());
    }
    if (!lib) { LOGE("steam: steamclient.so not found (is Steam installed?)"); return false; }
    p_CreateInterface = dlsym(lib, "CreateInterface");
    p_BGetCallback = dlsym(lib, "Steam_BGetCallback");
    p_FreeLastCallback = dlsym(lib, "Steam_FreeLastCallback");
    p_GetAPICallResult = dlsym(lib, "Steam_GetAPICallResult");
    if (!p_CreateInterface || !p_BGetCallback || !p_FreeLastCallback || !p_GetAPICallResult) {
        LOGE("steam: steamclient.so is missing expected exports");
        dlclose(lib);
        lib = NULL;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- calling host methods
typedef uint64_t (*IntFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double);
typedef float (*FltFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double);
#define MAXI 14
#define MAXF 4

static void *vfunc(void *obj, int idx) { return (*(void ***)obj)[idx]; }

// SysV passes integer and float arguments in separate register files, so ints
// can be listed first and floats last regardless of their order in the method.
// A float argument travels in the low 32 bits of an xmm register.
static uint64_t invoke(void *fn, const uint64_t *in, const double *fl, bool want_float, float *fret) {
    if (want_float) {
        *fret = ((FltFn)fn)(in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7], in[8], in[9], in[10], in[11],
                            in[12], in[13], fl[0], fl[1], fl[2], fl[3]);
        return 0;
    }
    return ((IntFn)fn)(in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7], in[8], in[9], in[10], in[11], in[12],
                       in[13], fl[0], fl[1], fl[2], fl[3]);
}

static const SteamMethod *find_method(const SteamIfaceDesc *d, const char *name) {
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->m[i].name, name)) return &d->m[i];
    return NULL;
}
static const SteamIfaceDesc *desc_by_class(const char *cls) {
    for (int i = 0; i < NIFACES; i++)
        if (!strcmp(steam_ifaces[i].cls, cls)) return &steam_ifaces[i];
    return NULL;
}
static const SteamIfaceDesc *desc_by_version(const char *ver) {
    for (int i = 0; i < NIFACES; i++)
        if (!strcmp(steam_ifaces[i].version, ver)) return &steam_ifaces[i];
    return NULL;
}

// Host-side call with integer/pointer arguments only.
static uint64_t hcall(void *obj, const char *cls, const char *name, int n, const uint64_t *args) {
    const SteamMethod *m = find_method(desc_by_class(cls), name);
    uint64_t in[MAXI] = {(uint64_t)(uintptr_t)obj};
    for (int i = 0; i < n; i++) in[1 + i] = args[i];
    double fl[MAXF] = {0};
    return invoke(vfunc(obj, m->host_idx), in, fl, false, NULL);
}

// ---------------------------------------------------------------- guest interface objects
typedef struct {
    void *host;
    const SteamIfaceDesc *d;
    uint32_t guest;
} Iface;

static Iface ifaces[64];
static int nifaces;
static uint32_t vtables[NIFACES];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t sig_stack_dwords(const char *sig) {
    uint32_t n = sig[0] == 'S'; // hidden return pointer
    for (const char *p = sig + 1; *p; p++) n += (*p == 'q' || *p == 'd') ? 2 : 1;
    return n;
}

#define MAX_SLOTS 128
static uint32_t steam_slot(int slot, uint32_t *a);
// 128 distinct entry points so each vtable slot knows its index. Names use
// octal digits: slot_017 is slot 15 (C reads 0##n as an octal literal).
#define S1(n) static uint32_t slot_##n(uint32_t *a) { return steam_slot(0##n, a); }
#define S8(n) S1(n##0) S1(n##1) S1(n##2) S1(n##3) S1(n##4) S1(n##5) S1(n##6) S1(n##7)
S8(0) S8(1) S8(2) S8(3) S8(4) S8(5) S8(6) S8(7) S8(10) S8(11) S8(12) S8(13) S8(14) S8(15) S8(16) S8(17)
#define R1(n) slot_##n,
#define R8(n) R1(n##0) R1(n##1) R1(n##2) R1(n##3) R1(n##4) R1(n##5) R1(n##6) R1(n##7)
static const ApiFn slot_fns[MAX_SLOTS] = {R8(0) R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7)
                                          R8(10) R8(11) R8(12) R8(13) R8(14) R8(15) R8(16) R8(17)};

// Programs (Steam's DRM wrapper, some games) check that Steam interfaces and
// their vtables live inside steamclient.dll. When the genuine steamclient.dll
// is mapped as our module (see pe.c), carve them out of its .data section,
// as Proton's lsteamclient does. Its own code never runs, so .data is free.
static uint32_t dll_cur, dll_end;
static uint32_t dll_alloc(uint32_t size) {
    static bool tried;
    if (!tried) {
        tried = true;
        uint32_t base = module_get("steamclient.dll", false), sz = 0;
        uint32_t d = base ? pe_section(base, ".data", &sz) : 0;
        if (d && sz > 0x1000) {
            dll_cur = d + 16;
            dll_end = d + sz;
            LOGD("steam: interfaces go in steamclient.dll .data at 0x%x", d);
        }
    }
    size = (size + 15) & ~15u;
    if (!dll_cur || dll_cur + size > dll_end) return 0;
    uint32_t r = dll_cur;
    dll_cur += size;
    memset(G2H(r), 0, size);
    return r;
}

static uint32_t vtable_for(const SteamIfaceDesc *d) {
    int i = (int)(d - steam_ifaces);
    if (vtables[i]) return vtables[i];
    if (d->n > MAX_SLOTS) fatal("steam: %s has too many methods", d->cls);
    ComMethod m[MAX_SLOTS];
    for (int k = 0; k < d->n; k++) m[k] = (ComMethod){slot_fns[k], (int)sig_stack_dwords(d->m[k].sig)};
    char label[64];
    snprintf(label, sizeof label, "%s", d->cls);
    // Thunks are named "<class>::<slot>"; the method name is logged on first use.
    vtables[i] = com_vtable(label, m, d->n);
    uint32_t in_dll = dll_alloc(4 * d->n);
    if (in_dll) {
        memcpy(G2H(in_dll), G2H(vtables[i]), 4 * d->n);
        vtables[i] = in_dll;
    }
    return vtables[i];
}

static uint32_t wrap(void *host, const SteamIfaceDesc *d) {
    if (!host || !d) return 0;
    pthread_mutex_lock(&mx);
    uint32_t g = 0;
    for (int i = 0; i < nifaces && !g; i++)
        if (ifaces[i].host == host && ifaces[i].d == d) g = ifaces[i].guest;
    if (!g && nifaces < 64) {
        Iface *f = &ifaces[nifaces++];
        f->host = host;
        f->d = d;
        uint32_t vt = vtable_for(d), at = dll_alloc(sizeof(ComObj));
        if (at) { // same layout as com_new
            ComObj *c = G2H(at);
            c->vtbl = vt;
            c->host_lo = (uint32_t)(uintptr_t)f;
            c->host_hi = (uint32_t)((uintptr_t)f >> 32);
            c->refs = 1;
            g = at;
        } else g = com_new(vt, f, 0);
        f->guest = g;
    }
    pthread_mutex_unlock(&mx);
    return g;
}

static uint32_t get_iface(const char *version) {
    if (!inited) return 0;
    const SteamIfaceDesc *d = desc_by_version(version);
    if (!d) { LOGI("steam: interface %s not supported", version); return 0; }
    if (!strcmp(d->cls, "ISteamClient")) return wrap(client, d);
    uint64_t args[3] = {(uint64_t)user_h, (uint64_t)pipe_h, (uint64_t)(uintptr_t)version};
    void *h = (void *)(uintptr_t)hcall(client, "ISteamClient", "GetISteamGenericInterface", 3, args);
    if (!h) LOGI("steam: Steam has no %s", version);
    return wrap(h, d);
}

// Strings returned by Steam live in its (64-bit) memory: copy them into guest
// memory. A ring of copies keeps each valid for a while, like Steam's own buffers.
static uint32_t guest_str(const char *s) {
    static uint32_t ring[256];
    static unsigned pos;
    if (!s) return 0;
    pthread_mutex_lock(&mx);
    unsigned i = pos++ % 256;
    if (ring[i]) gheap_free(ring[i]);
    ring[i] = gheap_strdup(s);
    uint32_t r = ring[i];
    pthread_mutex_unlock(&mx);
    return r;
}

static uint32_t steam_slot(int slot, uint32_t *a) {
    GuestCtx *ctx = thread_current()->ctx;
    Iface *f = com_host(ctx->g_ecx);
    const SteamMethod *m = &f->d->m[slot];
    const char *sig = m->sig;
    uint64_t in[MAXI] = {(uint64_t)(uintptr_t)f->host};
    double fl[MAXF] = {0};
    int ni = 1, nf = 0, k = 0;
    uint32_t retp = 0;
    if (sig[0] == 'S') retp = a[k++];
    uint32_t ver_arg = 0;
    bool refuse = sig[0] == 'x';
    for (const char *p = sig + 1; *p; p++) {
        switch (*p) {
        case 'q': in[ni++] = a[k] | (uint64_t)a[k + 1] << 32; k += 2; break;
        case 'd': { double d; memcpy(&d, &a[k], 8); fl[nf++] = d; k += 2; break; }
        case 'f': { uint64_t bits = a[k++]; double d; memcpy(&d, &bits, 8); fl[nf++] = d; break; }
        case 'p': ver_arg = a[k]; in[ni++] = a[k++]; break;
        case 'x': refuse = true; k++; break;
        default: in[ni++] = a[k++]; break;
        }
        if (ni > MAXI || nf > MAXF) fatal("steam: %s::%s has too many arguments", f->d->cls, m->name);
    }
    api_ret_pop(k * 4);
    static bool seen[NIFACES][MAX_SLOTS];
    int di = (int)(f->d - steam_ifaces);
    if (!seen[di][slot]) {
        seen[di][slot] = true;
        LOGD("steam: first call to %s::%s", f->d->cls, m->name);
    }
    if (refuse) {
        LOGI("steam: %s::%s is not supported (takes a callback or pointer array)", f->d->cls, m->name);
        if (retp) memset(G2H(retp), 0, 8);
        return retp;
    }
    void *fn = vfunc(f->host, m->host_idx);
    float fr = 0;
    uint64_t r = invoke(fn, in, fl, sig[0] == 'f', &fr);
    switch (sig[0]) {
    case 'v': return 0;
    case 'b': return (uint32_t)(r & 0xFF);
    case 'q': api_ret_edx((uint32_t)(r >> 32)); return (uint32_t)r;
    case 'S': memcpy(G2H(retp), &r, 8); return retp;
    case 'f': api_ret_double(fr); return 0;
    case 'z':
        LOGT("steam: %s::%s -> \"%s\"", f->d->cls, m->name, r ? (const char *)(uintptr_t)r : "(null)");
        return guest_str((const char *)(uintptr_t)r);
    case 'I': {
        const char *ver = ver_arg ? (const char *)G2H(ver_arg) : NULL;
        const SteamIfaceDesc *id = ver ? desc_by_version(ver) : NULL;
        if (r && ver && !id) LOGI("steam: %s::%s wants unsupported interface %s", f->d->cls, m->name, ver);
        return wrap((void *)(uintptr_t)r, id);
    }
    default: return (uint32_t)r;
    }
}

// ---------------------------------------------------------------- callbacks
// Guest CCallbackBase (MSVC): vtbl; uint8 flags @4; int iCallback @8.
// MSVC orders the overloaded virtuals in reverse: [0] Run(void*, bool, SteamAPICall_t),
// [1] Run(void*), [2] GetCallbackSizeBytes().
#define CB_FLAG_REGISTERED 1
#define CB_FLAG_GAMESERVER 2
typedef struct { uint32_t obj; } GuestCb;
typedef struct { uint32_t obj; uint64_t call; } GuestCallResult;
static GuestCb cbs[1024];
static int ncbs;
static GuestCallResult crs[1024];
static int ncrs;

static uint32_t cb_vfn(uint32_t obj, int i) { return ((uint32_t *)G2H(*(uint32_t *)G2H(obj)))[i]; }
static int cb_id(uint32_t obj) { return *(int32_t *)G2H(obj + 8); }

API_C(steam_api, SteamAPI_RegisterCallback, 2) {
    uint32_t obj = ARG(0);
    *(uint8_t *)G2H(obj + 4) |= CB_FLAG_REGISTERED;
    *(int32_t *)G2H(obj + 8) = (int32_t)ARG(1);
    pthread_mutex_lock(&mx);
    if (ncbs < 1024) cbs[ncbs++].obj = obj;
    pthread_mutex_unlock(&mx);
    return 0;
}
API_C(steam_api, SteamAPI_UnregisterCallback, 1) {
    uint32_t obj = ARG(0);
    *(uint8_t *)G2H(obj + 4) &= ~CB_FLAG_REGISTERED;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < ncbs; i++)
        if (cbs[i].obj == obj) cbs[i--] = cbs[--ncbs];
    pthread_mutex_unlock(&mx);
    return 0;
}
API_C(steam_api, SteamAPI_RegisterCallResult, 3) {
    pthread_mutex_lock(&mx);
    if (ncrs < 1024) crs[ncrs++] = (GuestCallResult){ARG(0), ARG(1) | (uint64_t)ARG(2) << 32};
    pthread_mutex_unlock(&mx);
    return 0;
}
API_C(steam_api, SteamAPI_UnregisterCallResult, 3) {
    uint64_t call = ARG(1) | (uint64_t)ARG(2) << 32;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < ncrs; i++)
        if (crs[i].obj == ARG(0) && crs[i].call == call) crs[i--] = crs[--ncrs];
    pthread_mutex_unlock(&mx);
    return 0;
}

#define STEAM_API_CALL_COMPLETED 703 // SteamAPICallCompleted_t (k_iSteamUtilsCallbacks + 3)

static void deliver_call_result(GuestCtx *ctx, uint64_t call) {
    uint32_t obj = 0;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < ncrs; i++)
        if (crs[i].call == call) { obj = crs[i].obj; crs[i] = crs[--ncrs]; break; }
    pthread_mutex_unlock(&mx);
    if (!obj) return;
    int size = (int)(uint32_t)call32_this(ctx, cb_vfn(obj, 2), obj, 0, NULL);
    if (size <= 0 || size > 65536) return;
    uint32_t buf = gheap_alloc(size, 1);
    bool failed = false;
    if (!p_GetAPICallResult(pipe_h, call, G2H(buf), size, cb_id(obj), &failed)) failed = true;
    uint32_t args[4] = {buf, failed, (uint32_t)call, (uint32_t)(call >> 32)};
    call32_this(ctx, cb_vfn(obj, 0), obj, 4, args);
    gheap_free(buf);
}

API_C(steam_api, SteamAPI_RunCallbacks, 0) {
    if (!inited) return 0;
    GuestCtx *ctx = thread_current()->ctx;
    CallbackMsg msg;
    while (p_BGetCallback(pipe_h, &msg)) {
        if (msg.callback == STEAM_API_CALL_COMPLETED && msg.size >= 8) {
            uint64_t call;
            memcpy(&call, msg.param, 8);
            deliver_call_result(ctx, call);
        }
        uint32_t targets[64];
        int nt = 0;
        pthread_mutex_lock(&mx);
        for (int i = 0; i < ncbs && nt < 64; i++)
            if (cb_id(cbs[i].obj) == msg.callback && !(*(uint8_t *)G2H(cbs[i].obj + 4) & CB_FLAG_GAMESERVER))
                targets[nt++] = cbs[i].obj;
        pthread_mutex_unlock(&mx);
        if (nt) {
            uint32_t buf = gheap_alloc(msg.size > 0 ? msg.size : 4, 1);
            for (int i = 0; i < nt; i++) {
                // the guest may change the data; give each listener a fresh copy
                memcpy(G2H(buf), msg.param, msg.size);
                uint32_t args[1] = {buf};
                call32_this(ctx, cb_vfn(targets[i], 1), targets[i], 1, args);
            }
            gheap_free(buf);
        }
        p_FreeLastCallback(pipe_h);
    }
    return 0;
}

// ---------------------------------------------------------------- init / accessors
static uint32_t requested_appid;

static void ensure_appid(void) {
    if (getenv("SteamAppId")) return;
    uint32_t id = requested_appid;
    char host[4096];
    if (!id && vfs_to_host("C:\\game\\steam_appid.txt", host, sizeof host)) {
        FILE *f = fopen(host, "r");
        if (f) { if (fscanf(f, "%u", &id) != 1) id = 0; fclose(f); }
    }
    if (!id) return;
    char v[16];
    snprintf(v, sizeof v, "%u", id);
    setenv("SteamAppId", v, 1);
    setenv("SteamGameId", v, 1);
    LOGI("steam: app id %u (not launched through Steam)", id);
}

// Outside Steam, a game asks to be relaunched through it. Running under the
// Wineless compatibility tool Steam has already set SteamAppId; otherwise use
// the id the game names, as steam_appid.txt would for a native game.
API_C(steam_api, SteamAPI_RestartAppIfNecessary, 1) {
    requested_appid = ARG(0);
    return 0;
}

API_C(steam_api, SteamAPI_Init, 0) {
    if (inited) return 1;
    ensure_appid();
    if (!load_client()) return 0;
    int rc = 0;
    client = p_CreateInterface("SteamClient017", &rc);
    if (!client) { LOGE("steam: steamclient.so has no SteamClient017"); return 0; }
    // Right after a previous game process ended Steam can briefly refuse a pipe.
    for (int tries = 0; tries < 20 && !(pipe_h = (int32_t)hcall(client, "ISteamClient", "CreateSteamPipe", 0, NULL));
         tries++)
        usleep(100000);
    if (!pipe_h) { LOGE("steam: could not connect to Steam (is it running?)"); return 0; }
    uint64_t a[1] = {(uint64_t)pipe_h};
    user_h = (int32_t)hcall(client, "ISteamClient", "ConnectToGlobalUser", 1, a);
    if (!user_h) {
        LOGE("steam: Steam refused the connection (not logged in, or the game isn't owned)");
        hcall(client, "ISteamClient", "BReleaseSteamPipe", 1, a);
        pipe_h = 0;
        return 0;
    }
    inited = true;
    LOGI("steam: connected (pipe %d, user %d)", pipe_h, user_h);
    return 1;
}

API_C(steam_api, SteamAPI_Shutdown, 0) {
    if (!inited) return 0;
    uint64_t a[2] = {(uint64_t)pipe_h, (uint64_t)user_h};
    hcall(client, "ISteamClient", "ReleaseUser", 2, a);
    hcall(client, "ISteamClient", "BReleaseSteamPipe", 1, a);
    inited = false;
    return 0;
}

API_C(steam_api, SteamAPI_IsSteamRunning, 0) { return inited || load_client(); }
API_C(steam_api, SteamAPI_GetHSteamUser, 0) { return (uint32_t)user_h; }
API_C(steam_api, SteamAPI_GetHSteamPipe, 0) { return (uint32_t)pipe_h; }
API_C(steam_api, SteamAPI_WriteMiniDump, 3) { return 0; }
API_C(steam_api, SteamAPI_SetMiniDumpComment, 1) { return 0; }
API_C(steam_api, SteamAPI_SetTryCatchCallbacks, 1) { return 0; }

API_C(steam_api, SteamClient, 0) { return get_iface("SteamClient017"); }
API_C(steam_api, SteamUser, 0) { return get_iface("SteamUser018"); }
API_C(steam_api, SteamFriends, 0) { return get_iface("SteamFriends015"); }
API_C(steam_api, SteamUtils, 0) { return get_iface("SteamUtils007"); }
API_C(steam_api, SteamMatchmaking, 0) { return get_iface("SteamMatchMaking009"); }
API_C(steam_api, SteamUserStats, 0) { return get_iface("STEAMUSERSTATS_INTERFACE_VERSION011"); }
API_C(steam_api, SteamApps, 0) { return get_iface("STEAMAPPS_INTERFACE_VERSION007"); }
API_C(steam_api, SteamNetworking, 0) { return get_iface("SteamNetworking005"); }
API_C(steam_api, SteamRemoteStorage, 0) { return get_iface("STEAMREMOTESTORAGE_INTERFACE_VERSION013"); }
API_C(steam_api, SteamUGC, 0) { return get_iface("STEAMUGC_INTERFACE_VERSION007"); }
API_C(steam_api, SteamScreenshots, 0) { return get_iface("STEAMSCREENSHOTS_INTERFACE_VERSION002"); }
API_C(steam_api, SteamHTTP, 0) { return get_iface("STEAMHTTP_INTERFACE_VERSION002"); }

// ---------------------------------------------------------------- steamclient.dll
// What Steam's DRM wrapper and some games load directly (the registry's
// ActiveProcess\SteamClientDll points here). CreateInterface hands out the same
// bridged interfaces, created by the Linux steamclient.so.
API_C(steamclient, CreateInterface, 2) {
    const char *ver = STR(0);
    if (ARG(1)) *(uint32_t *)PTR(1) = 1; // k_EIfaceNotFound until found
    if (!load_client()) return 0;
    const SteamIfaceDesc *d = desc_by_version(ver);
    if (!d) { LOGI("steamclient: CreateInterface(%s) not supported", ver); return 0; }
    int rc = 0;
    void *h = p_CreateInterface(ver, &rc);
    LOGI("steamclient: CreateInterface(%s) = %p", ver, h);
    if (!h) return 0;
    if (ARG(1)) *(uint32_t *)PTR(1) = 0;
    return wrap(h, d);
}
