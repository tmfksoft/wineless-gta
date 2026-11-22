// msvcrt.dll: the subset of the Microsoft C runtime that DLLs linked against
// the system CRT import (Microsoft's d3dx9_43 and D3DCompiler_43).
//
// Most functions map onto glibc, with guest memory addressed directly. The
// unusual ones:
//   _CIxxx       x87 "intrinsic" math: arguments arrive on the FPU stack, which
//                the host shares, so they are popped with fstpl and the result
//                pushed back by the dispatcher (api_ret_double).
//   _controlfp   translates MSVC's control-word flags to the x87 bits.
//   _setjmp3/longjmp  save guest registers; longjmp returns through a guest
//                trampoline that restores them.
//   printf family  a complete MSVC-style formatter (including floats and the
//                narrow/wide %s/%S swap) over 32-bit guest varargs.
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include "../cpu.h"
#include "../heap.h"
#include "../thread.h"
#include "../vfs.h"
#include "win32.h"

uint32_t win32_getenv(const char *name);

// ---------------------------------------------------------------- memory
API_C(msvcrt, malloc, 1) { return gheap_alloc(ARG(0) ? ARG(0) : 1, 0); }
API_C(msvcrt, calloc, 2) { uint32_t n = ARG(0) * ARG(1); return gheap_alloc(n ? n : 1, 1); }
API_C(msvcrt, realloc, 2) {
    if (!ARG(0)) return gheap_alloc(ARG(1) ? ARG(1) : 1, 0);
    if (!ARG(1)) { gheap_free(ARG(0)); return 0; }
    return gheap_realloc(ARG(0), ARG(1), 0);
}
API_C(msvcrt, free, 1) { if (ARG(0)) gheap_free(ARG(0)); return 0; }
API_CN(msvcrt, op_new, "??2@YAPAXI@Z", 1) { return gheap_alloc(ARG(0) ? ARG(0) : 1, 0); } // operator new
API_CN(msvcrt, op_delete, "??3@YAXPAX@Z", 1) { if (ARG(0)) gheap_free(ARG(0)); return 0; } // operator delete
API_N(msvcrt, type_info_dtor, "??1type_info@@UAE@XZ", 0) { return 0; }                           // ~type_info (thiscall)

API_C(msvcrt, memcpy, 3) { memcpy(PTR(0), PTR(1), ARG(2)); return ARG(0); }
API_C(msvcrt, memmove, 3) { memmove(PTR(0), PTR(1), ARG(2)); return ARG(0); }
API_C(msvcrt, memset, 3) { memset(PTR(0), (int)ARG(1), ARG(2)); return ARG(0); }

// ---------------------------------------------------------------- strings
static uint32_t gptr(const void *p) { return p ? H2G(p) : 0; }
API_C(msvcrt, strchr, 2) { return gptr(strchr(STR(0), (int)(ARG(1) & 0xFF))); }
API_C(msvcrt, strrchr, 2) { return gptr(strrchr(STR(0), (int)(ARG(1) & 0xFF))); }
API_C(msvcrt, strstr, 2) { return gptr(strstr(STR(0), STR(1))); }
API_C(msvcrt, strncmp, 3) { return (uint32_t)strncmp(STR(0), STR(1), ARG(2)); }
API_C(msvcrt, strlen, 1) { return (uint32_t)strlen(STR(0)); }
API_C(msvcrt, strcmp, 2) { return (uint32_t)strcmp(STR(0), STR(1)); }
API_C(msvcrt, strcpy, 2) { strcpy(PTR(0), STR(1)); return ARG(0); }
API_C(msvcrt, strncpy, 3) { strncpy(PTR(0), STR(1), ARG(2)); return ARG(0); }
API_C(msvcrt, strcat, 2) { strcat(PTR(0), STR(1)); return ARG(0); }
API_C(msvcrt, strncat, 3) { strncat(PTR(0), STR(1), ARG(2)); return ARG(0); }
API_C(msvcrt, memcmp, 3) { return (uint32_t)memcmp(PTR(0), PTR(1), ARG(2)); }
API_C(msvcrt, strpbrk, 2) { return gptr(strpbrk(STR(0), STR(1))); }
API_C(msvcrt, strspn, 2) { return (uint32_t)strspn(STR(0), STR(1)); }
API_C(msvcrt, strcspn, 2) { return (uint32_t)strcspn(STR(0), STR(1)); }
API_C(msvcrt, strtok, 2) { return gptr(strtok(ARG(0) ? PTR(0) : NULL, STR(1))); }
API_C(msvcrt, _stricmp, 2) { return (uint32_t)strcasecmp(STR(0), STR(1)); }
API_C(msvcrt, _strnicmp, 3) { return (uint32_t)strncasecmp(STR(0), STR(1), ARG(2)); }
API_C(msvcrt, _strdup, 1) { return ARG(0) ? gheap_strdup(STR(0)) : 0; }
API_C(msvcrt, _mbstrlen, 1) { return strlen(STR(0)); }
API_C(msvcrt, tolower, 1) { return (uint32_t)tolower((int)ARG(0)); }
API_C(msvcrt, toupper, 1) { return (uint32_t)toupper((int)ARG(0)); }
#define CTYPE(f) API_C(msvcrt, f, 1) { return (int)ARG(0) >= -1 && (int)ARG(0) < 256 && f((int)ARG(0)) ? 1 : 0; }
CTYPE(isalnum)
CTYPE(isalpha)
CTYPE(isdigit)
CTYPE(isspace)
CTYPE(isxdigit)
CTYPE(isupper)
CTYPE(islower)
CTYPE(iscntrl)
CTYPE(ispunct)
CTYPE(isprint)
CTYPE(isgraph)
API_C(msvcrt, isleadbyte, 1) { return 0; } // single-byte code page
API_C(msvcrt, iswalpha, 1) { return iswalpha(ARG(0) & 0xFFFF) ? 1 : 0; }
API_C(msvcrt, iswdigit, 1) { return iswdigit(ARG(0) & 0xFFFF) ? 1 : 0; }
API_C(msvcrt, iswpunct, 1) { return iswpunct(ARG(0) & 0xFFFF) ? 1 : 0; }
API_C(msvcrt, iswspace, 1) { return iswspace(ARG(0) & 0xFFFF) ? 1 : 0; }
API_C(msvcrt, atoi, 1) { return (uint32_t)atoi(STR(0)); }
API_C(msvcrt, atol, 1) { return (uint32_t)atoi(STR(0)); }
API_C(msvcrt, atof, 1) { api_ret_double(atof(STR(0))); return 0; }
API_C(msvcrt, strtod, 2) {
    char *end;
    double v = strtod(STR(0), &end);
    if (ARG(1)) *(uint32_t *)PTR(1) = H2G(end);
    api_ret_double(v);
    return 0;
}
static uint32_t to_radix(uint32_t v, bool neg, char *out, uint32_t radix) {
    char tmp[40];
    int n = 0;
    do { tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % radix]; v /= radix; } while (v);
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    return 0;
}
API_C(msvcrt, _itoa, 3) {
    int32_t v = (int32_t)ARG(0);
    bool neg = v < 0 && ARG(2) == 10;
    to_radix(neg ? (uint32_t)-v : (uint32_t)v, neg, PTR(1), ARG(2));
    return ARG(1);
}
API_C(msvcrt, _ultoa, 3) { to_radix(ARG(0), false, PTR(1), ARG(2)); return ARG(1); }
API_C(msvcrt, wcstombs, 3) {
    const uint16_t *w = WSTR(1);
    uint32_t n = 0;
    while (w[n] && (!ARG(0) || n < ARG(2))) {
        if (ARG(0)) ((char *)PTR(0))[n] = w[n] < 256 ? (char)w[n] : '?';
        n++;
    }
    if (ARG(0) && n < ARG(2)) ((char *)PTR(0))[n] = 0;
    return n;
}
API_C(msvcrt, wctomb, 2) {
    if (!ARG(0)) return 0;
    *(char *)PTR(0) = (ARG(1) & 0xFFFF) < 256 ? (char)ARG(1) : '?';
    return 1;
}

// ---------------------------------------------------------------- printf
// One formatter for the narrow and wide functions. In the wide ones %s and %c
// are wide and %S/%C narrow (MSVC semantics); args are 32-bit guest slots.
typedef struct { char *buf; size_t cap, len; } Out;
static void out_s(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        o->cap = (o->len + n + 1) * 2;
        o->buf = realloc(o->buf, o->cap);
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}
// Output is UTF-8-free: wide characters above 0xFF become '?' (the CRT's narrow
// conversion), except in wide output where they are kept by the caller via
// a parallel code-unit stream. Strings here are short ASCII diagnostics in practice.
static char *msvc_format(const void *fmtp, bool wide, const uint32_t *a) {
    Out o = {0};
    int k = 0;
    size_t i = 0;
#define FCH(j) (wide ? ((const uint16_t *)fmtp)[j] : ((const uint8_t *)fmtp)[j])
    for (;;) {
        uint32_t c = FCH(i);
        if (!c) break;
        if (c != '%') { char ch = c < 256 ? (char)c : '?'; out_s(&o, &ch, 1); i++; continue; }
        i++;
        char spec[32] = "%";
        int sl = 1;
        while (strchr("-+ #0", (int)FCH(i)) && FCH(i) && sl < 8) spec[sl++] = FCH(i++);
        int width = -1, prec = -1;
        if (FCH(i) == '*') { width = (int32_t)a[k++]; i++; }
        else { width = 0; bool any = false; while (FCH(i) >= '0' && FCH(i) <= '9') { width = width * 10 + FCH(i++) - '0'; any = true; } if (!any) width = -1; }
        if (FCH(i) == '.') {
            i++;
            if (FCH(i) == '*') { prec = (int32_t)a[k++]; i++; }
            else { prec = 0; while (FCH(i) >= '0' && FCH(i) <= '9') prec = prec * 10 + FCH(i++) - '0'; }
        }
        int len = 0; // 0 int, 1 short, 2 long (32-bit), 3 int64, 4 long double/double
        bool narrow_s = false, wide_s = false;
        for (;;) {
            uint32_t m = FCH(i);
            if (m == 'h') { len = 1; narrow_s = true; i++; }
            else if (m == 'l') { if (FCH(i + 1) == 'l') { len = 3; i += 2; } else { len = 2; wide_s = true; i++; } }
            else if (m == 'L') { len = 4; i++; }
            else if (m == 'w') { wide_s = true; i++; }
            else if (m == 'I' && FCH(i + 1) == '6' && FCH(i + 2) == '4') { len = 3; i += 3; }
            else if (m == 'I' && FCH(i + 1) == '3' && FCH(i + 2) == '2') { len = 0; i += 3; }
            else if (m == 'I') { i++; } // pointer-sized = 32-bit
            else break;
        }
        uint32_t conv = FCH(i);
        if (!conv) break;
        i++;
        char piece[512];
        int n = 0;
        if (width >= 0) sl += snprintf(spec + sl, sizeof spec - sl, "%d", width);
        if (prec >= 0) sl += snprintf(spec + sl, sizeof spec - sl, ".%d", prec);
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = a[k++];
            if (len == 3) v |= (uint64_t)a[k++] << 32;
            else if (conv == 'd' || conv == 'i') v = len == 1 ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(int64_t)(int32_t)v;
            else if (len == 1) v &= 0xFFFF;
            snprintf(spec + sl, sizeof spec - sl, "ll%c", (int)conv);
            n = snprintf(piece, sizeof piece, spec, (long long)v);
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A': {
            double d;
            memcpy(&d, &a[k], 8);
            k += 2;
            snprintf(spec + sl, sizeof spec - sl, "%c", (int)conv);
            n = snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 'c': case 'C': {
            uint32_t ch = a[k++] & 0xFFFF;
            snprintf(spec + sl, sizeof spec - sl, "c");
            n = snprintf(piece, sizeof piece, spec, ch < 256 ? (int)ch : '?');
            break;
        }
        case 's': case 'S': {
            uint32_t p = a[k++];
            bool w = conv == 'S' ? !wide : wide;
            if (narrow_s) w = false;
            if (wide_s) w = true;
            char tmp[1024];
            if (!p) snprintf(tmp, sizeof tmp, "(null)");
            else if (w) {
                const uint16_t *ws = G2H(p);
                size_t j = 0;
                for (; ws[j] && j + 1 < sizeof tmp && (prec < 0 || (int)j < prec); j++) tmp[j] = ws[j] < 256 ? ws[j] : '?';
                tmp[j] = 0;
            } else snprintf(tmp, sizeof tmp, "%.*s", prec < 0 ? 1023 : prec, (const char *)G2H(p));
            snprintf(spec + sl, sizeof spec - sl, "s");
            n = snprintf(piece, sizeof piece, spec, tmp);
            break;
        }
        case 'p': n = snprintf(piece, sizeof piece, "%08X", a[k++]); break;
        case 'n': *(uint32_t *)G2H(a[k++]) = (uint32_t)o.len; break;
        case '%': piece[0] = '%'; n = 1; break;
        default: piece[0] = '%'; piece[1] = conv; n = 2; break;
        }
        if (n > (int)sizeof piece - 1) n = sizeof piece - 1;
        if (n > 0) out_s(&o, piece, n);
    }
#undef FCH
    if (!o.buf) out_s(&o, "", 0);
    return o.buf;
}
// MSVC _snprintf semantics: no terminator and -1 when the output doesn't fit.
static uint32_t put_narrow(char *dst, uint32_t n, const char *s) {
    size_t l = strlen(s);
    if (l < n) { memcpy(dst, s, l + 1); return l; }
    memcpy(dst, s, n);
    return (uint32_t)-1;
}
static uint32_t put_wide(uint16_t *dst, uint32_t n, const char *s) {
    size_t l = strlen(s);
    for (size_t j = 0; j < l && j < n; j++) dst[j] = (uint8_t)s[j];
    if (l < n) { dst[l] = 0; return l; }
    return (uint32_t)-1;
}
API_C(msvcrt, _snprintf, 3) {
    char *s = msvc_format(PTR(2), false, _args + 3);
    uint32_t r = put_narrow(PTR(0), ARG(1), s);
    free(s);
    return r;
}
API_C(msvcrt, sprintf, 2) {
    char *s = msvc_format(PTR(1), false, _args + 2);
    uint32_t r = put_narrow(PTR(0), 0x7FFFFFFFu, s);
    free(s);
    return r;
}
API_C(msvcrt, vsprintf, 3) {
    char *s = msvc_format(PTR(1), false, PTR(2));
    uint32_t r = put_narrow(PTR(0), 0x7FFFFFFFu, s);
    free(s);
    return r;
}
API_C(msvcrt, _vsnprintf, 4) {
    char *s = msvc_format(PTR(2), false, PTR(3));
    uint32_t r = put_narrow(PTR(0), ARG(1), s);
    free(s);
    return r;
}
API_C(msvcrt, _vsnwprintf, 4) {
    char *s = msvc_format(PTR(2), true, PTR(3));
    uint32_t r = put_wide(PTR(0), ARG(1), s);
    free(s);
    return r;
}

// sscanf: each conversion is handed to glibc with a guest pointer as target
// (guest addresses are host addresses); MSVC's l means 32-bit for integers.
API_C(msvcrt, sscanf, 2) {
    const char *in = STR(0), *f = STR(1);
    const uint32_t *a = _args + 2;
    int k = 0, assigned = 0, pos = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)in[pos])) pos++; f++; continue; }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') f++;
            if (in[pos] != *f) break;
            pos++;
            f++;
            continue;
        }
        const char *st = f++;
        bool skip = *f == '*';
        if (skip) f++;
        while (isdigit((unsigned char)*f)) f++;
        bool l = false, ll = false, h = false;
        if (*f == 'h') { h = true; f++; }
        else if (*f == 'l') { f++; if (*f == 'l') { ll = true; f++; } else l = true; }
        else if (!strncmp(f, "I64", 3)) { ll = true; f += 3; }
        char conv = *f;
        if (conv == '[') { f++; if (*f == ']') f++; while (*f && *f != ']') f++; }
        if (!*f) break;
        f++;
        // rebuild a glibc spec: copy flags/width, set the length modifier explicitly
        char spec[128];
        int sl = 0;
        for (const char *p = st; p < f && sl < 100; p++) {
            if (*p == 'h' || *p == 'l' || (*p == 'I' && p[1] == '6')) {
                if (*p == 'I') p += 2;
                continue;
            }
            if (p == f - 1 || (conv == '[' && *p == '[')) {
                if (strchr("diouxX", conv)) { if (ll) { spec[sl++] = 'l'; spec[sl++] = 'l'; } else if (h) spec[sl++] = 'h'; }
                else if (strchr("eEfgG", conv) && l) spec[sl++] = 'l';
                if (conv == '[') { memcpy(spec + sl, p, f - p); sl += f - p; break; }
            }
            spec[sl++] = *p;
        }
        spec[sl] = 0;
        strcat(spec, "%n");
        int used = 0, got;
        if (skip) got = sscanf(in + pos, spec, &used) >= 0 && used ? 0 : -1;
        else if (conv == 'n') { *(uint32_t *)G2H(a[k++]) = pos; continue; }
        else got = sscanf(in + pos, spec, G2H(a[k++]), &used);
        if (got < 0 || (!skip && got < 1)) break;
        if (!skip && conv != 'n') assigned++;
        pos += used;
    }
    return assigned ? (uint32_t)assigned : (in[0] ? 0 : (uint32_t)-1);
}

// ---------------------------------------------------------------- math
static double dbl(const uint32_t *a) { double d; memcpy(&d, a, 8); return d; }
API_C(msvcrt, ceil, 2) { api_ret_double(ceil(dbl(_args))); return 0; }
API_C(msvcrt, floor, 2) { api_ret_double(floor(dbl(_args))); return 0; }
API_C(msvcrt, sqrt, 2) { api_ret_double(sqrt(dbl(_args))); return 0; }
API_C(msvcrt, fabs, 2) { api_ret_double(fabs(dbl(_args))); return 0; }
API_C(msvcrt, sin, 2) { api_ret_double(sin(dbl(_args))); return 0; }
API_C(msvcrt, cos, 2) { api_ret_double(cos(dbl(_args))); return 0; }
API_C(msvcrt, tan, 2) { api_ret_double(tan(dbl(_args))); return 0; }
API_C(msvcrt, asin, 2) { api_ret_double(asin(dbl(_args))); return 0; }
API_C(msvcrt, acos, 2) { api_ret_double(acos(dbl(_args))); return 0; }
API_C(msvcrt, atan, 2) { api_ret_double(atan(dbl(_args))); return 0; }
API_C(msvcrt, exp, 2) { api_ret_double(exp(dbl(_args))); return 0; }
API_C(msvcrt, log, 2) { api_ret_double(log(dbl(_args))); return 0; }
API_C(msvcrt, log10, 2) { api_ret_double(log10(dbl(_args))); return 0; }
API_C(msvcrt, pow, 4) { api_ret_double(pow(dbl(_args), dbl(_args + 2))); return 0; }
API_C(msvcrt, atan2, 4) { api_ret_double(atan2(dbl(_args), dbl(_args + 2))); return 0; }
API_C(msvcrt, fmod, 4) { api_ret_double(fmod(dbl(_args), dbl(_args + 2))); return 0; }
API_C(msvcrt, ldexp, 3) { api_ret_double(ldexp(dbl(_args), (int)ARG(2))); return 0; }
API_C(msvcrt, frexp, 3) {
    int e;
    api_ret_double(frexp(dbl(_args), &e));
    *(int32_t *)PTR(2) = e;
    return 0;
}
API_C(msvcrt, modf, 3) {
    double ip;
    api_ret_double(modf(dbl(_args), &ip));
    memcpy(PTR(2), &ip, 8);
    return 0;
}
API_C(msvcrt, _finite, 2) { return isfinite(dbl(_args)); }
API_C(msvcrt, _isnan, 2) { return isnan(dbl(_args)); }
API_C(msvcrt, _fpclass, 2) {
    double d = dbl(_args);
    switch (fpclassify(d)) {
    case FP_NAN: return 0x0002;                       // _FPCLASS_QNAN
    case FP_INFINITE: return signbit(d) ? 0x0004 : 0x0200;
    case FP_ZERO: return signbit(d) ? 0x0020 : 0x0040;
    case FP_SUBNORMAL: return signbit(d) ? 0x0010 : 0x0080;
    default: return signbit(d) ? 0x0008 : 0x0100;
    }
}

// x87 intrinsics: the argument(s) are on the guest's (= our) FPU stack.
static double fpu_pop(void) {
    double v;
    __asm__ volatile("fstpl %0" : "=m"(v));
    return v;
}
#define CI1(name, expr) API_C(msvcrt, name, 0) { double x = fpu_pop(); api_ret_double(expr); return 0; }
#define CI2(name, expr) API_C(msvcrt, name, 0) { double y = fpu_pop(), x = fpu_pop(); api_ret_double(expr); return 0; }
CI1(_CIsqrt, sqrt(x))
CI1(_CIsin, sin(x))
CI1(_CIcos, cos(x))
CI1(_CItan, tan(x))
CI1(_CIasin, asin(x))
CI1(_CIacos, acos(x))
CI1(_CIatan, atan(x))
CI1(_CIsinh, sinh(x))
CI1(_CIcosh, cosh(x))
CI1(_CItanh, tanh(x))
CI1(_CIexp, exp(x))
CI1(_CIlog, log(x))
CI2(_CIpow, pow(x, y))
CI2(_CIatan2, atan2(x, y))
CI2(_CIfmod, fmod(x, y))

// _ftol: pop a double off the FPU stack, return (long long) truncated toward
// zero in edx:eax — MSVC's helper for a float/double -> integer cast.
API_C(msvcrt, _ftol, 0) {
    int64_t v = (int64_t)fpu_pop();
    api_ret_edx((uint32_t)((uint64_t)v >> 32));
    return (uint32_t)v;
}

// _controlfp(new, mask): MSVC flag layout <-> x87 control word bits
static uint32_t cw_to_msvc(uint16_t cw) {
    uint32_t r = 0;
    if (cw & 0x01) r |= 0x10;      // _EM_INVALID
    if (cw & 0x02) r |= 0x80000;   // _EM_DENORMAL
    if (cw & 0x04) r |= 0x08;      // _EM_ZERODIVIDE
    if (cw & 0x08) r |= 0x04;      // _EM_OVERFLOW
    if (cw & 0x10) r |= 0x02;      // _EM_UNDERFLOW
    if (cw & 0x20) r |= 0x01;      // _EM_INEXACT
    switch (cw & 0xC00) { case 0x400: r |= 0x100; break; case 0x800: r |= 0x200; break; case 0xC00: r |= 0x300; break; }
    switch (cw & 0x300) { case 0x000: r |= 0x20000; break; case 0x200: r |= 0x10000; break; } // _PC_24/_PC_53 (_PC_64 = 0)
    return r;
}
static uint16_t msvc_to_cw(uint32_t m, uint16_t cw) {
    cw &= ~0xF3F;
    if (m & 0x10) cw |= 0x01;
    if (m & 0x80000) cw |= 0x02;
    if (m & 0x08) cw |= 0x04;
    if (m & 0x04) cw |= 0x08;
    if (m & 0x02) cw |= 0x10;
    if (m & 0x01) cw |= 0x20;
    switch (m & 0x300) { case 0x100: cw |= 0x400; break; case 0x200: cw |= 0x800; break; case 0x300: cw |= 0xC00; break; }
    switch (m & 0x30000) { case 0x20000: break; case 0x10000: cw |= 0x200; break; default: cw |= 0x300; break; }
    return cw;
}
API_C(msvcrt, _controlfp, 2) {
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    uint32_t cur = cw_to_msvc(cw);
    if (ARG(1)) {
        cur = (cur & ~ARG(1)) | (ARG(0) & ARG(1));
        cw = msvc_to_cw(cur, cw);
        __asm__ volatile("fldcw %0" : : "m"(cw));
    }
    return cur;
}
API_C(msvcrt, _clearfp, 0) {
    uint16_t sw;
    __asm__ volatile("fnstsw %0; fnclex" : "=m"(sw));
    uint32_t r = 0;
    if (sw & 0x01) r |= 0x10;
    if (sw & 0x02) r |= 0x80000;
    if (sw & 0x04) r |= 0x08;
    if (sw & 0x08) r |= 0x04;
    if (sw & 0x10) r |= 0x02;
    if (sw & 0x20) r |= 0x01;
    return r;
}

// ---------------------------------------------------------------- setjmp / longjmp
// MSVC jmp_buf: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, Cookie, ...
API_C(msvcrt, _setjmp3, 2) {
    GuestCtx *ctx = thread_current()->ctx;
    uint32_t *jb = PTR(0), *sp = G2H(ctx->guest_esp);
    jb[0] = ctx->g_ebp;
    jb[1] = ctx->g_ebx;
    jb[2] = ctx->g_edi;
    jb[3] = ctx->g_esi;
    jb[4] = ctx->guest_esp + 8; // esp as the caller sees it after the call
    jb[5] = sp[1];              // return address
    jb[6] = *(uint32_t *)G2H(ctx->teb); // fs:[0], the SEH chain
    jb[7] = ARG(1) ? _args[2] : 0xFFFFFFFF;
    jb[8] = 0x56433230; // 'VC20'
    jb[9] = 0;
    return 0;
}
// Return into a guest trampoline that loads the saved registers and jumps.
API_C(msvcrt, longjmp, 2) {
    static uint32_t tramp;
    if (!tramp) {
        static const uint8_t code[] = {
            0x89, 0xC1,                               // mov ecx, eax (jmp_buf)
            0x89, 0xD0,                               // mov eax, edx (value)
            0x85, 0xC0, 0x75, 0x05,                   // test eax, eax; jnz +5
            0xB8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1
            0x8B, 0x29,                               // mov ebp, [ecx]
            0x8B, 0x59, 0x04,                         // mov ebx, [ecx+4]
            0x8B, 0x79, 0x08,                         // mov edi, [ecx+8]
            0x8B, 0x71, 0x0C,                         // mov esi, [ecx+12]
            0x8B, 0x51, 0x18,                         // mov edx, [ecx+24]
            0x64, 0x89, 0x15, 0x00, 0x00, 0x00, 0x00, // mov fs:[0], edx
            0x8B, 0x61, 0x10,                         // mov esp, [ecx+16]
            0xFF, 0x61, 0x14};                        // jmp [ecx+20]
        tramp = cpu_emit_code(code, sizeof code);
    }
    GuestCtx *ctx = thread_current()->ctx;
    ((uint32_t *)G2H(ctx->guest_esp))[1] = tramp; // our "return address"
    api_ret_edx(ARG(1));
    return ARG(0);
}

// ---------------------------------------------------------------- CRT startup / exit
API_C(msvcrt, _initterm, 2) {
    GuestCtx *ctx = thread_current()->ctx;
    for (uint32_t p = ARG(0); p < ARG(1); p += 4) {
        uint32_t fn = *(uint32_t *)G2H(p);
        if (fn) call32(ctx, fn, 0, NULL);
    }
    return 0;
}
// atexit functions of DLLs: they would only run at process exit, which we don't do.
API_C(msvcrt, _onexit, 1) { return ARG(0); }
API_C(msvcrt, __dllonexit, 3) { return ARG(0); }
API_C(msvcrt, _lock, 1) { return 0; }
API_C(msvcrt, _unlock, 1) { return 0; }
API_C(msvcrt, __set_app_type, 1) { return 0; } // console/GUI hint; harmless to ignore
API_C(msvcrt, __setusermatherr, 1) { return 0; } // custom _matherr hook; we don't call it

// __getmainargs(argc, argv, env, wildcard, startinfo): MSVC CRT argv setup.
// We give argv[0] = the program path and an empty environment; games that read
// the command line use GetCommandLineA, which we answer in full.
API_C(msvcrt, __getmainargs, 5) {
    static uint32_t argv_arr, env_arr;
    if (!argv_arr) {
        argv_arr = gheap_alloc(8, 1); // [argv[0], NULL]
        ((uint32_t *)G2H(argv_arr))[0] = g_guest_cmdline;
        env_arr = gheap_alloc(4, 1); // [NULL]
    }
    if (ARG(0)) *(uint32_t *)G2H(ARG(0)) = 1;
    if (ARG(1)) *(uint32_t *)G2H(ARG(1)) = argv_arr;
    if (ARG(2)) *(uint32_t *)G2H(ARG(2)) = env_arr;
    return 0;
}
API_C(msvcrt, _amsg_exit, 1) { fatal("msvcrt: runtime error R60%02u", ARG(0)); }
API_C(msvcrt, abort, 0) { fatal("msvcrt: abort()"); }
API_C(msvcrt, exit, 1) { win32_process_exit(ARG(0)); }
API_C(msvcrt, _purecall, 0) { fatal("msvcrt: pure virtual function call"); }
API_CN(msvcrt, terminate, "?terminate@@YAXXZ", 0) { fatal("msvcrt: terminate()"); }
API_C(msvcrt, _XcptFilter, 2) { return 0; } // EXCEPTION_CONTINUE_SEARCH
// C++ exceptions: not implemented yet. The frame handler lets other handlers
// run (non-C++ exceptions pass through such frames); a throw is reported.
API_C(msvcrt, __CxxFrameHandler, 4) { return 1; } // ExceptionContinueSearch
API(msvcrt, _CxxThrowException, 2) { fatal("msvcrt: C++ exception thrown (not supported yet)"); }

// ---------------------------------------------------------------- misc
API_C(msvcrt, getenv, 1) { return win32_getenv(STR(0)); }
static uint32_t rand_seed = 1;
API_C(msvcrt, srand, 1) { rand_seed = ARG(0); return 0; }
API_C(msvcrt, rand, 0) { rand_seed = rand_seed * 214013 + 2531011; return (rand_seed >> 16) & 0x7FFF; }
API_C(msvcrt, setlocale, 2) {
    static uint32_t c;
    if (!c) c = gheap_strdup("C");
    return c;
}
API_C(msvcrt, _strdate, 1) {
    time_t t = time(NULL);
    strftime(PTR(0), 9, "%m/%d/%y", localtime(&t));
    return ARG(0);
}
API_C(msvcrt, _strtime, 1) {
    time_t t = time(NULL);
    strftime(PTR(0), 9, "%H:%M:%S", localtime(&t));
    return ARG(0);
}
API_C(msvcrt, _tempnam, 2) {
    static unsigned n;
    char b[512];
    snprintf(b, sizeof b, "C:\\Users\\Player\\AppData\\Local\\Temp\\%s%u", ARG(1) ? STR(1) : "t", ++n);
    return gheap_strdup(b);
}
static __thread GuestCtx *qs_ctx;
static __thread uint32_t qs_cmp;
static int qs_thunk(const void *x, const void *y) {
    uint32_t args[2] = {H2G(x), H2G(y)};
    return (int)(uint32_t)call32(qs_ctx, qs_cmp, 2, args);
}
API_C(msvcrt, qsort, 4) {
    GuestCtx *pc = qs_ctx;
    uint32_t pf = qs_cmp;
    qs_ctx = thread_current()->ctx;
    qs_cmp = ARG(3);
    qsort(PTR(0), ARG(1), ARG(2), qs_thunk);
    qs_ctx = pc;
    qs_cmp = pf;
    return 0;
}

// errno, per thread in guest memory
API_C(msvcrt, _errno, 0) {
    static __thread uint32_t e;
    if (!e) e = gheap_alloc(4, 1);
    return e;
}

// _acmdln: the CRT's char* holding the ANSI command line. Point it at ours
// (the CRT and some programs parse it directly instead of GetCommandLineA).
API_DATA_EXPORT(msvcrt, _acmdln) {
    static uint32_t p;
    if (!p) { p = gheap_alloc(4, 0); *(uint32_t *)G2H(p) = g_guest_cmdline; }
    return p;
}

// _access(path, mode): 0 if the file is accessible, -1 otherwise. Windows' mode
// bits (0=exist, 2=write, 4=read) match POSIX R_OK/W_OK/F_OK, so pass them through.
API_C(msvcrt, _access, 2) {
    char host[2048];
    if (!vfs_to_host(STR(0), host, sizeof host)) return (uint32_t)-1;
    return access(host, ARG(1)) == 0 ? 0 : (uint32_t)-1;
}
// _stat(path, struct _stat*): MSVC's 32-bit-time struct _stat is 36 bytes;
// st_mode is at +6, st_size at +20 (the fields games actually read).
API_C(msvcrt, _stat, 2) {
    char host[2048];
    if (!vfs_to_host(STR(0), host, sizeof host)) return (uint32_t)-1;
    struct stat st;
    if (stat(host, &st) != 0) return (uint32_t)-1;
    uint8_t *o = PTR(1);
    memset(o, 0, 36);
    *(uint16_t *)(o + 6) = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x1B6; // _S_IFDIR/_S_IFREG|rw
    *(uint16_t *)(o + 8) = 1;                             // st_nlink
    *(uint32_t *)(o + 20) = (uint32_t)st.st_size;         // st_size
    *(uint32_t *)(o + 24) = (uint32_t)st.st_atime;
    *(uint32_t *)(o + 28) = (uint32_t)st.st_mtime;
    *(uint32_t *)(o + 32) = (uint32_t)st.st_ctime;
    return 0;
}

// ---------------------------------------------------------------- stdio FILE*
// Games treat FILE* as opaque (no getc/putc macros here), so each open is a
// 32-byte guest cookie whose first word indexes a host FILE* in this table.
static FILE *g_files[512];
static pthread_mutex_t g_files_lk = PTHREAD_MUTEX_INITIALIZER;
#define FILE_MAGIC 0x4C494657u // 'WFIL' — marks our cookies apart from _iob[]
static FILE *file_of(uint32_t gf) {
    if (!gf) return NULL;
    uint32_t *p = G2H(gf);
    if (p[0] != FILE_MAGIC) return NULL; // a std stream (_iob) or not ours
    return p[1] && p[1] <= 512 ? g_files[p[1] - 1] : NULL;
}
static uint32_t file_new(FILE *f) {
    pthread_mutex_lock(&g_files_lk);
    uint32_t gf = 0;
    for (int i = 0; i < 512; i++)
        if (!g_files[i]) {
            g_files[i] = f;
            gf = gheap_alloc(32, 1);
            ((uint32_t *)G2H(gf))[0] = FILE_MAGIC;
            ((uint32_t *)G2H(gf))[1] = i + 1;
            break;
        }
    pthread_mutex_unlock(&g_files_lk);
    if (!gf) fclose(f);
    return gf;
}
static int file_no(uint32_t gf); // the std-stream helper, defined below
API_C(msvcrt, fopen, 2) {
    char host[2048];
    if (!vfs_to_host(STR(0), host, sizeof host)) return 0;
    char mode[8];
    int m = 0;
    for (const char *p = STR(1); *p && m < 7; p++)
        if (*p != 't') mode[m++] = *p; // POSIX fopen rejects 't'
    mode[m] = 0;
    FILE *f = fopen(host, mode);
    return f ? file_new(f) : 0;
}
API_C(msvcrt, fclose, 1) {
    uint32_t gf = ARG(0);
    FILE *f = file_of(gf);
    if (!f) return (uint32_t)-1; // EOF
    pthread_mutex_lock(&g_files_lk);
    g_files[((uint32_t *)G2H(gf))[1] - 1] = NULL;
    pthread_mutex_unlock(&g_files_lk);
    int r = fclose(f);
    gheap_free(gf);
    return r ? (uint32_t)-1 : 0;
}
API_C(msvcrt, fread, 4) {
    FILE *f = file_of(ARG(3));
    return f ? (uint32_t)fread(PTR(0), ARG(1), ARG(2), f) : 0;
}
static void diag_write(const void *buf, size_t n);
API_C(msvcrt, fwrite, 4) {
    FILE *f = file_of(ARG(3));
    if (f) return (uint32_t)fwrite(PTR(0), ARG(1), ARG(2), f);
    int fd = file_no(ARG(3)); // a std stream (stdout/stderr)?
    if (fd == 1 || fd == 2) diag_write(PTR(0), (size_t)ARG(1) * ARG(2));
    return ARG(2);
}
API_C(msvcrt, fseek, 3) {
    FILE *f = file_of(ARG(0));
    return f ? (uint32_t)fseek(f, (int32_t)ARG(1), ARG(2)) : (uint32_t)-1;
}
API_C(msvcrt, ftell, 1) {
    FILE *f = file_of(ARG(0));
    return f ? (uint32_t)ftell(f) : (uint32_t)-1;
}
API_C(msvcrt, fgets, 3) {
    FILE *f = file_of(ARG(2));
    return f && fgets(PTR(0), (int)ARG(1), f) ? ARG(0) : 0;
}
// Low-level fd I/O: host fds are small ints, so pass them straight through.
API_C(msvcrt, _open, 3) {
    char host[2048];
    if (!vfs_to_host(STR(0), host, sizeof host)) return (uint32_t)-1;
    int f = ARG(1) & 3; // O_RDONLY/WRONLY/RDWR share the low bits
    if (ARG(1) & 0x100) f |= O_CREAT;
    if (ARG(1) & 0x200) f |= O_TRUNC;
    if (ARG(1) & 0x008) f |= O_APPEND;
    return (uint32_t)open(host, f, 0644);
}
API_C(msvcrt, _close, 1) { return (uint32_t)close((int)ARG(0)); }
API_C(msvcrt, _write, 3) { return (uint32_t)write((int)ARG(0), PTR(1), ARG(2)); }

// _splitpath(path, drive, dir, fname, ext): split a path; any output may be NULL.
API_C(msvcrt, _splitpath, 5) {
    const char *p = STR(0);
    char *drive = ARG(1) ? PTR(1) : NULL, *dir = ARG(2) ? PTR(2) : NULL;
    char *fn = ARG(3) ? PTR(3) : NULL, *ext = ARG(4) ? PTR(4) : NULL;
    if (drive) drive[0] = 0;
    if (dir) dir[0] = 0;
    if (fn) fn[0] = 0;
    if (ext) ext[0] = 0;
    if (p[0] && p[1] == ':') {
        if (drive) { drive[0] = p[0]; drive[1] = ':'; drive[2] = 0; }
        p += 2;
    }
    const char *slash = NULL, *dot = NULL;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') slash = q;
    if (slash) {
        size_t n = slash - p + 1;
        if (dir) { memcpy(dir, p, n); dir[n] = 0; }
        p = slash + 1;
    }
    for (const char *q = p; *q; q++) if (*q == '.') dot = q;
    if (dot) {
        if (fn) { size_t n = dot - p; memcpy(fn, p, n); fn[n] = 0; }
        if (ext) strcpy(ext, dot);
    } else if (fn) strcpy(fn, p);
    return 0;
}
// _makepath(path, drive, dir, fname, ext): the inverse of _splitpath.
API_C(msvcrt, _makepath, 5) {
    char *out = PTR(0);
    out[0] = 0;
    if (ARG(1) && STR(1)[0]) strcpy(out, STR(1));
    if (ARG(2) && STR(2)[0]) strcat(out, STR(2));
    if (ARG(3)) strcat(out, STR(3));
    if (ARG(4) && STR(4)[0]) {
        if (STR(4)[0] != '.') strcat(out, ".");
        strcat(out, STR(4));
    }
    return 0;
}

// clock(): processor time in CLOCKS_PER_SEC units; Windows uses 1000 (ms).
API_C(msvcrt, clock, 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
API_C(msvcrt, time, 1) {
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0)) *(uint32_t *)PTR(0) = t;
    return t;
}
// __chkesp: MSVC debug stack-pointer check, called after every call. We skip the
// check; it must leave eax (the just-returned value) untouched, so return the
// guest's entry eax (saved into ctx->eax by the dispatch).
API_C(msvcrt, _chkesp, 0) { return thread_current()->ctx->eax; }

// __p__fmode / __p__commode: pointers to the CRT's _fmode / _commode globals,
// which MSVC startup reads and writes. Zero-initialised = _O_TEXT / default.
API_C(msvcrt, __p__fmode, 0) {
    static uint32_t p;
    if (!p) p = gheap_alloc(4, 1);
    return p;
}
API_C(msvcrt, __p__commode, 0) {
    static uint32_t p;
    if (!p) p = gheap_alloc(4, 1);
    return p;
}

// ---------------------------------------------------------------- stdio (diagnostics only)
// _iob[0..2] are stdin/stdout/stderr (32-byte FILE each); writes go to our stderr.
static uint32_t iob;
API_DATA_EXPORT(msvcrt, _iob) {
    if (!iob) iob = gheap_alloc(20 * 32, 1);
    return iob;
}
static int file_no(uint32_t f) {
    if (!iob) api_msvcrt__iob(NULL);
    return f >= iob && f < iob + 20 * 32 ? (int)((f - iob) / 32) : -1;
}
// __pioinfo: per-handle info blocks the CRT's inline code peeks at; all zero (no flags).
API_DATA_EXPORT(msvcrt, __pioinfo) {
    static uint32_t p;
    if (!p) {
        p = gheap_alloc(64 * 4, 1);
        uint32_t blk = gheap_alloc(32 * 0x40, 1);
        for (int i = 0; i < 64; i++) ((uint32_t *)G2H(p))[i] = blk;
    }
    return p;
}
API_DATA_EXPORT(msvcrt, __badioinfo) {
    static uint32_t p;
    if (!p) p = gheap_alloc(0x40, 1);
    return p;
}
static void diag_write(const void *buf, size_t n) {
    if (n) fprintf(stderr, "[guest] %.*s%s", (int)n, (const char *)buf, ((const char *)buf)[n - 1] == '\n' ? "" : "\n");
}
API_C(msvcrt, fflush, 1) {
    FILE *f = file_of(ARG(0));
    return f ? (uint32_t)fflush(f) : 0;
}
API_C(msvcrt, _fileno, 1) { return (uint32_t)file_no(ARG(0)); }
API_C(msvcrt, _isatty, 1) { return 0; }
API_C(msvcrt, _lseeki64, 4) { api_ret_edx(0xFFFFFFFF); return 0xFFFFFFFF; }
