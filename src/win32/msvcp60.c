// msvcp60: the sliver of the MSVC 6 C++ runtime (msvcp60.dll) that games touch.
// Morrowind is built on MSVC6's std::string, so this implements that class.
//
// VC6's std::string is a copy-on-write (reference-counted) string, but every
// function that manages the sharing — the copy constructor, destructor, _Grow,
// _Freeze, _Split, _Copy, _Tidy — is exported and called (only the field reads
// size()/_Ptr[i] are inlined into the game). So we can implement it as an always
// -unique string: the copy constructor deep-copies and the COW helpers collapse
// to plain capacity management. Layout matches VC6: { allocator, _Ptr, _Len,
// _Res }, 16 bytes; our buffer is [1-byte refcount = 0][chars..][NUL], _Ptr
// pointing at the chars.
#include <string.h>

#include "../cpu.h"
#include "../heap.h"
#include "../thread.h"
#include "win32.h"

#define SELF (thread_current()->ctx->g_ecx) // `this` for a thiscall method
// the mangled std::basic_string<char> class name, shared by every method symbol
#define BS "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
// In the free operators the std:: namespace is a back-reference, so the class
// name ends at the allocator without the trailing "std@@".
#define BS0 "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@"

typedef struct {
    uint32_t al, ptr, len, res;
} GStr;
static GStr *G(uint32_t p) { return (GStr *)G2H(p); }
static char *D(uint32_t ptr) { return (char *)G2H(ptr); }

// buffer for `cap` chars: [refcount][cap+1], returns the guest _Ptr (= chars)
static uint32_t s_newbuf(uint32_t cap) {
    uint32_t a = gheap_alloc(cap + 2, 0);
    ((uint8_t *)G2H(a))[0] = 0;
    ((char *)G2H(a))[1] = 0;
    return a + 1;
}
static void s_free(uint32_t ptr) { if (ptr) gheap_free(ptr - 1); }
static void s_init(GStr *s, uint32_t cap) { s->ptr = s_newbuf(cap); s->len = 0; s->res = cap; }
// ensure capacity for `n` chars, preserving the current contents
static void s_ensure(GStr *s, uint32_t n) {
    if (s->ptr && s->res >= n) return;
    uint32_t nb = s_newbuf(n), keep = s->len < n ? s->len : n;
    if (s->ptr && keep) memcpy(D(nb), D(s->ptr), keep);
    D(nb)[keep] = 0;
    s_free(s->ptr);
    s->ptr = nb;
    s->res = n;
    if (s->len > n) s->len = n;
}
static void s_set(GStr *s, const char *src, uint32_t n) {
    s_ensure(s, n);
    if (n) memcpy(D(s->ptr), src, n);
    D(s->ptr)[n] = 0;
    s->len = n;
}

// ---- constructors / destructor (thiscall; `this` in ecx) --------------------
API_N(msvcp60, str_ctor_default, "??_F" BS "QAEXXZ", 0) { s_init(G(SELF), 0); return SELF; }
API_N(msvcp60, str_ctor_copy, "??0" BS "QAE@ABV01@@Z", 1) {
    GStr *d = G(SELF), *s = G(ARG(0));
    s_init(d, s->len);
    if (s->len) memcpy(D(d->ptr), D(s->ptr), s->len);
    D(d->ptr)[s->len] = 0;
    d->len = s->len;
    return SELF;
}
API_N(msvcp60, str_ctor_cstr, "??0" BS "QAE@PBDABV?$allocator@D@1@@Z", 2) {
    uint32_t n = ARG(0) ? (uint32_t)strlen(STR(0)) : 0;
    s_init(G(SELF), n);
    s_set(G(SELF), ARG(0) ? STR(0) : "", n);
    return SELF;
}
API_N(msvcp60, str_dtor, "??1" BS "QAE@XZ", 0) { s_free(G(SELF)->ptr); G(SELF)->ptr = 0; return 0; }

// ---- assign / append / operator= --------------------------------------------
API_N(msvcp60, str_assign_cstr, "?assign@" BS "QAEAAV12@PBDI@Z", 2) {
    s_set(G(SELF), PTR(0), ARG(1));
    return SELF;
}
API_N(msvcp60, str_assign_sub, "?assign@" BS "QAEAAV12@ABV12@II@Z", 3) {
    GStr *d = G(SELF), *s = G(ARG(0));
    uint32_t off = ARG(1) < s->len ? ARG(1) : s->len, n = ARG(2);
    if (n > s->len - off) n = s->len - off;
    s_set(d, D(s->ptr) + off, n);
    return SELF;
}
API_N(msvcp60, str_assign_eq, "??4" BS "QAEAAV01@PBD@Z", 1) {
    s_set(G(SELF), ARG(0) ? STR(0) : "", ARG(0) ? (uint32_t)strlen(STR(0)) : 0);
    return SELF;
}
API_N(msvcp60, str_append_cstr, "?append@" BS "QAEAAV12@PBDI@Z", 2) {
    GStr *d = G(SELF);
    uint32_t old = d->len, n = ARG(1);
    s_ensure(d, old + n);
    if (n) memcpy(D(d->ptr) + old, PTR(0), n);
    d->len = old + n;
    D(d->ptr)[d->len] = 0;
    return SELF;
}
API_N(msvcp60, str_append_ch, "?append@" BS "QAEAAV12@ID@Z", 2) {
    GStr *d = G(SELF);
    uint32_t old = d->len, n = ARG(0);
    s_ensure(d, old + n);
    memset(D(d->ptr) + old, (int)ARG(1), n);
    d->len = old + n;
    D(d->ptr)[d->len] = 0;
    return SELF;
}
API_N(msvcp60, str_insert, "?insert@" BS "QAEAAV12@IPBDI@Z", 3) {
    GStr *d = G(SELF);
    uint32_t off = ARG(0) < d->len ? ARG(0) : d->len, n = ARG(2);
    s_ensure(d, d->len + n);
    memmove(D(d->ptr) + off + n, D(d->ptr) + off, d->len - off);
    if (n) memcpy(D(d->ptr) + off, PTR(1), n);
    d->len += n;
    D(d->ptr)[d->len] = 0;
    return SELF;
}
API_N(msvcp60, str_erase, "?erase@" BS "QAEAAV12@II@Z", 2) {
    GStr *d = G(SELF);
    uint32_t off = ARG(0) < d->len ? ARG(0) : d->len, n = ARG(1);
    if (n > d->len - off) n = d->len - off;
    memmove(D(d->ptr) + off, D(d->ptr) + off + n, d->len - off - n);
    d->len -= n;
    D(d->ptr)[d->len] = 0;
    return SELF;
}

// ---- lookups ----------------------------------------------------------------
API_N(msvcp60, str_c_str, "?c_str@" BS "QBEPBDXZ", 0) { return G(SELF)->ptr; }
API_N(msvcp60, str_max_size, "?max_size@" BS "QBEIXZ", 0) { return 0x7FFFFFFEu; }
API_N(msvcp60, str_find, "?find@" BS "QBEIPBDII@Z", 3) {
    GStr *d = G(SELF);
    const char *hay = D(d->ptr), *ned = PTR(0);
    uint32_t off = ARG(1), n = ARG(2);
    if (off > d->len || n > d->len - off) {
        if (n == 0 && off <= d->len) return off;
        return 0xFFFFFFFFu;
    }
    for (uint32_t i = off; i + n <= d->len; i++)
        if (!memcmp(hay + i, ned, n)) return i;
    return n == 0 ? off : 0xFFFFFFFFu;
}
API_N(msvcp60, str_find_first_of, "?find_first_of@" BS "QBEIPBDII@Z", 3) {
    GStr *d = G(SELF);
    const char *hay = D(d->ptr), *set = PTR(0);
    for (uint32_t i = ARG(1); i < d->len; i++)
        if (memchr(set, hay[i], ARG(2))) return i;
    return 0xFFFFFFFFu;
}
API_N(msvcp60, str_find_last_of, "?find_last_of@" BS "QBEIPBDII@Z", 3) {
    GStr *d = G(SELF);
    const char *hay = D(d->ptr), *set = PTR(0);
    uint32_t start = ARG(1) < d->len ? ARG(1) : (d->len ? d->len - 1 : 0);
    for (uint32_t i = start + 1; i-- > 0;)
        if (memchr(set, hay[i], ARG(2))) return i;
    return 0xFFFFFFFFu;
}

// ---- COW helpers: always-unique, so these are plain capacity ops ------------
API_N(msvcp60, str_Grow, "?_Grow@" BS "AAE_NI_N@Z", 2) {
    GStr *s = G(SELF);
    if (ARG(0) == 0) { s_ensure(s, 0); D(s->ptr)[0] = 0; s->len = 0; return 0; }
    s_ensure(s, ARG(0));
    return 1;
}
API_N(msvcp60, str_Copy, "?_Copy@" BS "AAEXI@Z", 1) {
    GStr *s = G(SELF);
    s_ensure(s, ARG(0) < s->len ? s->len : ARG(0));
    return 0;
}
API_N(msvcp60, str_Eos, "?_Eos@" BS "AAEXI@Z", 1) {
    GStr *s = G(SELF);
    s->len = ARG(0);
    D(s->ptr)[ARG(0)] = 0;
    return 0;
}
API_N(msvcp60, str_Freeze, "?_Freeze@" BS "AAEXXZ", 0) { return 0; } // already unique
API_N(msvcp60, str_Split, "?_Split@" BS "AAEXXZ", 0) { return 0; }  // already unique
API_N(msvcp60, str_Tidy, "?_Tidy@" BS "AAEX_N@Z", 1) {
    GStr *s = G(SELF);
    if (ARG(0)) s_free(s->ptr);
    s_init(s, 0);
    return 0;
}

// ---- free operators (cdecl; "YA" mangling). operator+ returns by value, so
// the hidden return-object pointer is the first argument. --------------------
static int streq(GStr *a, const char *b, uint32_t bl) {
    return a->len == bl && !memcmp(D(a->ptr), b, bl);
}
API_CN(msvcp60, str_concat, "??H" "std@@YA?AV" BS0 "0@ABV10@0@Z", 3) {
    GStr *r = G(ARG(0)), *a = G(ARG(1)), *b = G(ARG(2));
    s_init(r, a->len + b->len);
    memcpy(D(r->ptr), D(a->ptr), a->len);
    memcpy(D(r->ptr) + a->len, D(b->ptr), b->len);
    r->len = a->len + b->len;
    D(r->ptr)[r->len] = 0;
    return ARG(0);
}
API_CN(msvcp60, str_eq_ss, "??8" "std@@YA_NABV" BS0 "0@0@Z", 2) {
    GStr *b = G(ARG(1));
    return streq(G(ARG(0)), D(b->ptr), b->len);
}
API_CN(msvcp60, str_eq_sc, "??8" "std@@YA_NABV" BS0 "0@PBD@Z", 2) {
    return streq(G(ARG(0)), STR(1), (uint32_t)strlen(STR(1)));
}
API_CN(msvcp60, str_ne_ss, "??9" "std@@YA_NABV" BS0 "0@0@Z", 2) {
    GStr *b = G(ARG(1));
    return !streq(G(ARG(0)), D(b->ptr), b->len);
}
API_CN(msvcp60, str_ne_sc, "??9" "std@@YA_NABV" BS0 "0@PBD@Z", 2) {
    return !streq(G(ARG(0)), STR(1), (uint32_t)strlen(STR(1)));
}
API_CN(msvcp60, str_lt_ss, "??M" "std@@YA_NABV" BS0 "0@0@Z", 2) {
    GStr *a = G(ARG(0)), *b = G(ARG(1));
    uint32_t m = a->len < b->len ? a->len : b->len;
    int c = memcmp(D(a->ptr), D(b->ptr), m);
    return c < 0 || (c == 0 && a->len < b->len);
}

// range/length errors: the error path only, which we don't support — report it.
API_CN(msvcp60, Xlen, "?_Xlen@std@@YAXXZ", 0) { fatal("msvcp60: std::string length_error"); }
API_CN(msvcp60, Xran, "?_Xran@std@@YAXXZ", 0) { fatal("msvcp60: std::string out_of_range"); }

// static data: npos (size_t -1) and the shared null character _Nullstr returns.
API_DEF_N_(msvcp60, npos, "?npos@" BS "2IB", 0, API_DATA) {
    static uint32_t p;
    if (!p) { p = gheap_alloc(4, 0); *(uint32_t *)G2H(p) = 0xFFFFFFFFu; }
    return p;
}
API_DEF_N_(msvcp60, nullstr_C, "?_C@?1??_Nullstr@" BS "CAPBDXZ@4DB", 0, API_DATA) {
    static uint32_t p;
    if (!p) { p = gheap_alloc(1, 1); }
    return p;
}

// ---- iostream / locale static-init objects (no-op; games only link them) ----
API_N(msvcp60, ios_base_Init_ctor, "??0Init@ios_base@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, ios_base_Init_dtor, "??1Init@ios_base@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, Winit_ctor, "??0_Winit@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, Winit_dtor, "??1_Winit@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, Lockit_ctor, "??0_Lockit@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, Lockit_dtor, "??1_Lockit@std@@QAE@XZ", 0) { return 0; }
