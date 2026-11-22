// Shared machinery for the Direct3D 8/9 bridges: guest wrapper objects,
// compile-time thunk generation from method signatures, buffer/rect locking
// through guest bounce buffers and vtable construction.
//
// Each bridge includes its DirectX header, defines BRIDGE_RESOURCE,
// BRIDGE_SURFACE, BRIDGE_VOLUME, BRIDGE_SWAPCHAIN, BRIDGE_VB and BRIDGE_IB,
// then includes this file, then defines wrap_by_iid() and its overrides,
// and finally includes its generated vtable table.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

extern "C" {
typedef uint32_t (*ApiFn)(uint32_t *a);
uint32_t api_register_dynamic(const char *label, ApiFn fn, int nargs, int flags);
void api_ret_double(double v);
uint32_t gheap_alloc(uint32_t size, int zero);
int gheap_free(uint32_t p);
void log_msg(int level, const char *fmt, ...);
extern int g_log_level;
void *wineless_window_sdl(uint32_t hwnd);
uint32_t wineless_window_from_sdl(void *sdl);
}

#define LOGE(...) log_msg(0, __VA_ARGS__)
#define LOGW(...) log_msg(1, __VA_ARGS__)
#define LOGI(...) log_msg(2, __VA_ARGS__)
#define LOGD(...) do { if (g_log_level >= 3) log_msg(3, __VA_ARGS__); } while (0)

static inline void *G2H(uint32_t a) { return (void *)(uintptr_t)a; }
static inline uint32_t H2G(const void *p) { return (uint32_t)(uintptr_t)p; }



// ---------------------------------------------------------------- wrappers
struct LockState {
    uint32_t guest = 0; // guest bounce buffer
    void *host = nullptr;
    uint32_t size = 0;
};

struct Info {
    D3DRESOURCETYPE kind = (D3DRESOURCETYPE)0;
    bool subresource = false; // surface/volume owned by a texture
    bool keep = false;        // never free the wrapper (see gen_release)
    uint32_t vb_size = 0;
    bool dynamic = false;
    uint32_t mirror = 0; // persistent guest copy for dynamic buffers
    std::map<uint32_t, LockState> locks; // key: subresource (face*256+level) or 0
};

struct GuestObj {
    uint32_t vtbl;
    uint32_t host_lo, host_hi;
    uint32_t info_lo, info_hi;
    uint32_t pad;
};

static std::recursive_mutex g_lock;
static std::unordered_map<void *, uint32_t> g_host2guest; // non-resource objects

// {7b1e4a52-5a57-4f8e-9a3c-0d1e2f3a4b5c} private-data tag holding the guest wrapper
static const GUID GUID_SawWrapper = {0x7b1e4a52, 0x5a57, 0x4f8e, {0x9a, 0x3c, 0x0d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c}};

static inline GuestObj *gobj(uint32_t g) { return (GuestObj *)G2H(g); }
static inline void *unwrap(uint32_t g) {
    if (!g) return nullptr;
    GuestObj *o = gobj(g);
    return (void *)(uintptr_t)((uint64_t)o->host_lo | ((uint64_t)o->host_hi << 32));
}
static inline Info *info_of(uint32_t g) {
    GuestObj *o = gobj(g);
    return (Info *)(uintptr_t)((uint64_t)o->info_lo | ((uint64_t)o->info_hi << 32));
}

template <class I> uint32_t vtable_of();
static uint32_t wrap_by_iid(REFIID iid, void *p); // per bridge

static uint32_t new_guest(uint32_t vtbl, void *host, D3DRESOURCETYPE kind) {
    uint32_t g = gheap_alloc(sizeof(GuestObj), 1);
    GuestObj *o = gobj(g);
    Info *inf = new Info();
    inf->kind = kind;
    o->vtbl = vtbl;
    o->host_lo = (uint32_t)(uintptr_t)host;
    o->host_hi = (uint32_t)((uintptr_t)host >> 32);
    o->info_lo = (uint32_t)(uintptr_t)inf;
    o->info_hi = (uint32_t)((uintptr_t)inf >> 32);
    return g;
}

static void free_guest(uint32_t g) {
    Info *inf = info_of(g);
    if (inf) {
        if (inf->mirror) gheap_free(inf->mirror);
        for (auto &l : inf->locks)
            if (l.second.guest) gheap_free(l.second.guest);
        delete inf;
    }
    gheap_free(g);
}

template <class I> static uint32_t wrap(I *host) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    // resources (and D3D8 surfaces, which aren't resources there) carry their
    // wrapper in private data, so identity survives the host object's lifetime
    if constexpr (std::is_base_of_v<BRIDGE_RESOURCE, I> || std::is_same_v<I, BRIDGE_SURFACE>) {
        uint32_t g = 0;
        DWORD sz = sizeof g;
        if (SUCCEEDED(host->GetPrivateData(GUID_SawWrapper, &g, &sz)) && g) return g;
        D3DRESOURCETYPE kind;
        if constexpr (std::is_same_v<I, BRIDGE_SURFACE>) kind = D3DRTYPE_SURFACE;
        else kind = host->GetType();
        g = new_guest(vtable_of<I>(), host, kind);
        host->SetPrivateData(GUID_SawWrapper, &g, sizeof g, 0);
        if constexpr (std::is_same_v<I, BRIDGE_SURFACE>) info_of(g)->keep = true;
        if constexpr (std::is_same_v<I, BRIDGE_VB>) {
            D3DVERTEXBUFFER_DESC d;
            host->GetDesc(&d);
            info_of(g)->vb_size = d.Size;
            info_of(g)->dynamic = (d.Usage & D3DUSAGE_DYNAMIC) != 0;
        }
        if constexpr (std::is_same_v<I, BRIDGE_IB>) {
            D3DINDEXBUFFER_DESC d;
            host->GetDesc(&d);
            info_of(g)->vb_size = d.Size;
            info_of(g)->dynamic = (d.Usage & D3DUSAGE_DYNAMIC) != 0;
        }
        return g;
    } else {
        auto it = g_host2guest.find((void *)host);
        if (it != g_host2guest.end()) return it->second;
        uint32_t g = new_guest(vtable_of<I>(), host, (D3DRESOURCETYPE)0);
        g_host2guest[(void *)host] = g;
        if constexpr (std::is_same_v<I, BRIDGE_SWAPCHAIN> || std::is_same_v<I, BRIDGE_VOLUME>)
            info_of(g)->keep = true;
        return g;
    }
}


// ---------------------------------------------------------------- argument conversion
struct BadStruct {};
template <class T> struct is_bad_struct : std::false_type {};
template <> struct is_bad_struct<D3DPRESENT_PARAMETERS> : std::true_type {};
template <> struct is_bad_struct<D3DLOCKED_RECT> : std::true_type {};
template <> struct is_bad_struct<D3DLOCKED_BOX> : std::true_type {};
template <> struct is_bad_struct<D3DDEVICE_CREATION_PARAMETERS> : std::true_type {};
template <> struct is_bad_struct<RGNDATA> : std::true_type {};

template <class T, class = void> struct Conv {
    static_assert(sizeof(T) == 0, "no automatic conversion for this parameter type; write an override");
};
template <class T> struct Conv<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> {
    T v;
    explicit Conv(uint32_t g) : v(static_cast<T>(g)) {}
    T get() { return v; }
    void post() {}
};
template <> struct Conv<float> {
    float v;
    explicit Conv(uint32_t g) { std::memcpy(&v, &g, 4); }
    float get() { return v; }
    void post() {}
};
// interface pointer in
template <class I> struct Conv<I *, std::enable_if_t<std::is_base_of_v<IUnknown, I>>> {
    I *v;
    explicit Conv(uint32_t g) : v(static_cast<I *>(unwrap(g))) {}
    I *get() { return v; }
    void post() {}
};
// interface pointer out
template <class I> struct Conv<I **, std::enable_if_t<std::is_base_of_v<IUnknown, I>>> {
    uint32_t g;
    I *tmp = nullptr;
    explicit Conv(uint32_t g_) : g(g_) {}
    I **get() { return g ? &tmp : nullptr; }
    void post() {
        if (g) *(uint32_t *)G2H(g) = wrap<I>(tmp);
    }
};
// void** in generated thunks only ever means "HANDLE *pSharedHandle"
template <> struct Conv<void **> {
    explicit Conv(uint32_t g) {
        if (g && *(uint32_t *)G2H(g)) LOGW("d3d9: shared handles are not supported");
    }
    void **get() { return nullptr; }
    void post() {}
};
// plain data pointers (identity mapped)
template <class T>
struct Conv<T *, std::enable_if_t<!std::is_base_of_v<IUnknown, std::remove_cv_t<T>> &&
                                  !std::is_pointer_v<T>>> {
    static_assert(!is_bad_struct<std::remove_cv_t<T>>::value, "pointer-holding struct needs an override");
    T *v;
    explicit Conv(uint32_t g) : v(static_cast<T *>(G2H(g))) {}
    T *get() { return v; }
    void post() {}
};
// const GUID& etc.
template <class T> struct Conv<T &> {
    T *v;
    explicit Conv(uint32_t g) : v(static_cast<T *>(G2H(g))) {}
    T &get() { return *v; }
    void post() {}
};

template <class R> static uint32_t ret_conv(R r) {
    if constexpr (std::is_same_v<R, float>) {
        api_ret_double(r);
        return 0;
    } else {
        static_assert(std::is_integral_v<R> || std::is_enum_v<R>, "return type needs an override");
        return (uint32_t)r;
    }
}

template <class T> struct MfpTraits;
template <class C, class R, class... A> struct MfpTraits<R (C::*)(A...)> {
    using Class = C;
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr int nargs = 1 + sizeof...(A);
};

template <auto MFP> struct Gen {
    using T = MfpTraits<decltype(MFP)>;
    using C = typename T::Class;
    using R = typename T::Ret;
    template <size_t... I> static uint32_t call(uint32_t *g, std::index_sequence<I...>) {
        C *self = static_cast<C *>(unwrap(g[0]));
        std::tuple<Conv<std::tuple_element_t<I, typename T::Args>>...> cv{
            Conv<std::tuple_element_t<I, typename T::Args>>(g[1 + I])...};
        if constexpr (std::is_void_v<R>) {
            (self->*MFP)(std::get<I>(cv).get()...);
            (std::get<I>(cv).post(), ...);
            return 0;
        } else {
            R r = (self->*MFP)(std::get<I>(cv).get()...);
            (std::get<I>(cv).post(), ...);
            return ret_conv(r);
        }
    }
    static uint32_t fn(uint32_t *g) {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        return call(g, std::make_index_sequence<std::tuple_size_v<typename T::Args>>{});
    }
};

// Hand-written thunks are registered by specializing Override.
template <auto MFP> struct Override {
    static constexpr ApiFn fn = nullptr;
    static constexpr int nargs = 0;
};
#define OVERRIDE(I, M, N)                                                                  \
    static uint32_t ov_##I##_##M(uint32_t *g);                                             \
    template <> struct Override<&I::M> {                                                   \
        static constexpr ApiFn fn = ov_##I##_##M;                                          \
        static constexpr int nargs = N;                                                    \
    };                                                                                     \
    static uint32_t ov_##I##_##M(uint32_t *g)

#define SELF(I) static_cast<I *>(unwrap(g[0]))

// ---------------------------------------------------------------- IUnknown (all interfaces)
static uint32_t gen_qi(uint32_t *g) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IUnknown *self = static_cast<IUnknown *>(unwrap(g[0]));
    const IID &iid = *(const IID *)G2H(g[1]);
    void *out = nullptr;
    HRESULT hr = self->QueryInterface(iid, &out);
    uint32_t w = SUCCEEDED(hr) ? wrap_by_iid(iid, out) : 0;
    if (SUCCEEDED(hr) && !w) {
        ((IUnknown *)out)->Release();
        hr = E_NOINTERFACE;
        LOGD("d3d9: QueryInterface for unsupported IID {%08x}", iid.Data1);
    }
    if (g[2]) *(uint32_t *)G2H(g[2]) = w;
    return (uint32_t)hr;
}
static uint32_t gen_addref(uint32_t *g) { return static_cast<IUnknown *>(unwrap(g[0]))->AddRef(); }
// DXVK lets device-owned objects (back buffers, the auto depth buffer, the
// implicit swapchain) drop to a public refcount of 0 while they stay alive and
// get handed out again later, so wrappers of surfaces and swapchains are kept.
// Everything the game creates and destroys itself is freed at 0.
static uint32_t gen_release(uint32_t *g) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    uint32_t guest = g[0];
    void *host = unwrap(guest);
    Info *inf = info_of(guest);
    ULONG r = static_cast<IUnknown *>(host)->Release();
    if (r == 0 && !inf->keep) {
        if (inf->kind == 0) g_host2guest.erase(host);
        free_guest(guest);
    }
    return r;
}


// ---------------------------------------------------------------- locking
// The game can't address host memory, so every lock hands out a guest bounce
// buffer that is copied into the real mapping on unlock.
static uint32_t block_rows(D3DFORMAT f, uint32_t rows) {
    switch (f) {
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return (rows + 3) / 4;
    default: return rows;
    }
}

static HRESULT lock_rect_common(uint32_t guest, uint32_t key, const D3DLOCKED_RECT &hl, const RECT *rect,
                                const D3DSURFACE_DESC &desc, DWORD flags, uint32_t *out) {
    uint32_t rows = rect ? (uint32_t)(rect->bottom - rect->top) : desc.Height;
    rows = block_rows(desc.Format, rows);
    uint32_t size = (uint32_t)hl.Pitch * rows;
    Info *inf = info_of(guest);
    LockState &ls = inf->locks[key];
    if (ls.guest) gheap_free(ls.guest);
    ls.guest = gheap_alloc(size ? size : 1, 0);
    ls.host = hl.pBits;
    ls.size = size;
    if (!ls.guest) return D3DERR_OUTOFVIDEOMEMORY;
    if (!(flags & D3DLOCK_DISCARD)) std::memcpy(G2H(ls.guest), hl.pBits, size);
    out[0] = (uint32_t)hl.Pitch;
    out[1] = ls.guest;
    return D3D_OK;
}
static void unlock_common(uint32_t guest, uint32_t key, bool readonly) {
    Info *inf = info_of(guest);
    auto it = inf->locks.find(key);
    if (it == inf->locks.end()) return;
    if (!readonly && it->second.host) std::memcpy(it->second.host, G2H(it->second.guest), it->second.size);
    gheap_free(it->second.guest);
    inf->locks.erase(it);
}


// Vertex/index buffers: dynamic buffers keep a persistent guest mirror (the game
// only ever writes through it, so it stays authoritative); static buffers get a
// temporary copy of the locked range.
template <class B> static uint32_t lock_buffer(uint32_t *g) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    B *b = static_cast<B *>(unwrap(g[0]));
    Info *inf = info_of(g[0]);
    uint32_t off = g[1], size = g[2], flags = g[4];
    if (off == 0 && size == 0) size = inf->vb_size;
    if (off + size > inf->vb_size) size = inf->vb_size > off ? inf->vb_size - off : 0;
    void *hp = nullptr;
    using LockPtr = std::tuple_element_t<2, typename MfpTraits<decltype(&B::Lock)>::Args>;
    HRESULT hr = b->Lock(off, size, reinterpret_cast<LockPtr>(&hp), flags);
    if (FAILED(hr)) return (uint32_t)hr;
    uint32_t gp;
    LockState &ls = inf->locks[0];
    if (inf->dynamic) {
        if (!inf->mirror) inf->mirror = gheap_alloc(inf->vb_size ? inf->vb_size : 1, 0);
        gp = inf->mirror + off;
        ls.guest = 0;
    } else {
        if (ls.guest) gheap_free(ls.guest);
        ls.guest = gheap_alloc(size ? size : 1, 0);
        if (!(flags & D3DLOCK_DISCARD)) std::memcpy(G2H(ls.guest), hp, size);
        gp = ls.guest;
    }
    ls.host = hp;
    ls.size = size;
    inf->locks[1].size = off; // remember offset for unlock
    inf->locks[1].guest = flags;
    *(uint32_t *)G2H(g[3]) = gp;
    return D3D_OK;
}
template <class B> static uint32_t unlock_buffer(uint32_t *g) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    B *b = static_cast<B *>(unwrap(g[0]));
    Info *inf = info_of(g[0]);
    auto it = inf->locks.find(0);
    if (it != inf->locks.end()) {
        uint32_t off = inf->locks[1].size, flags = inf->locks[1].guest;
        LockState &ls = it->second;
        if (!(flags & D3DLOCK_READONLY) && ls.host) {
            const void *src = inf->dynamic ? G2H(inf->mirror + off) : G2H(ls.guest);
            std::memcpy(ls.host, src, ls.size);
        }
        if (ls.guest) gheap_free(ls.guest);
        inf->locks.erase(0);
        inf->locks.erase(1);
    }
    return (uint32_t)b->Unlock();
}

// GetContainer(riid, void**)
template <class I> static uint32_t get_container(uint32_t *g) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    const IID &iid = *(const IID *)G2H(g[1]);
    void *out = nullptr;
    HRESULT hr = static_cast<I *>(unwrap(g[0]))->GetContainer(iid, &out);
    uint32_t w = SUCCEEDED(hr) ? wrap_by_iid(iid, out) : 0;
    if (g[2]) *(uint32_t *)G2H(g[2]) = w;
    return (uint32_t)(SUCCEEDED(hr) && !w ? E_NOINTERFACE : hr);
}

// ---------------------------------------------------------------- vtables
template <auto MFP> static void slot(ApiFn *fns, int *nargs, int i, const char *name) {
    if (!strcmp(name, "QueryInterface")) { fns[i] = gen_qi; nargs[i] = 3; return; }
    if (!strcmp(name, "AddRef")) { fns[i] = gen_addref; nargs[i] = 1; return; }
    if (!strcmp(name, "Release")) { fns[i] = gen_release; nargs[i] = 1; return; }
    if constexpr (Override<MFP>::fn != nullptr) {
        fns[i] = Override<MFP>::fn;
        nargs[i] = Override<MFP>::nargs;
    } else {
        fns[i] = Gen<MFP>::fn;
        nargs[i] = MfpTraits<decltype(MFP)>::nargs;
    }
}

#define VTABLE_BEGIN(I)                                                                    \
    template <> uint32_t vtable_of<I>() {                                                  \
        static uint32_t vt = 0;                                                            \
        if (vt) return vt;                                                                 \
        ApiFn fns[128];                                                                    \
        int nargs[128];                                                                    \
        const char *names[128];                                                            \
        int n = 0;
#define M(I, name)                                                                         \
    names[n] = #name;                                                                      \
    slot<&I::name>(fns, nargs, n, #name);                                                  \
    n++;
#define VTABLE_END(I, count)                                                               \
    static_assert(count <= 128, "vtable too big");                                         \
    vt = gheap_alloc(4 * n, 1);                                                            \
    for (int i = 0; i < n; i++) {                                                          \
        char label[96];                                                                    \
        snprintf(label, sizeof label, "%s::%s", #I, names[i]);                             \
        ((uint32_t *)G2H(vt))[i] = api_register_dynamic(label, fns[i], nargs[i], 0);       \
    }                                                                                      \
    return vt;                                                                             \
    }


