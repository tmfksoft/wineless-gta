#include "guest_printf.h"

#include <stdio.h>
#include <string.h>

int guest_vsnprintf(char *out, size_t outsz, const char *fmt, const uint32_t *args) {
    size_t o = 0;
    int ai = 0;
#define PUT(c) do { if (o + 1 < outsz) out[o] = (c); o++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        p++;
        while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
        if (*p == '*') { sl += snprintf(spec + sl, sizeof spec - sl, "%d", (int32_t)args[ai++]); p++; }
        else while (*p >= '0' && *p <= '9' && sl < 24) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') { sl += snprintf(spec + sl, sizeof spec - sl, "%d", (int32_t)args[ai++]); p++; }
            else while (*p >= '0' && *p <= '9' && sl < 28) spec[sl++] = *p++;
        }
        int is64 = 0;
        if (*p == 'h') p++;
        else if (*p == 'l') { p++; if (*p == 'l') { p++; is64 = 1; } }
        else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { p += 3; is64 = 1; }
        char c = *p;
        if (!c) break;
        char buf[512];
        int n = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (is64) {
                uint64_t v = args[ai] | ((uint64_t)args[ai + 1] << 32);
                ai += 2;
                spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = c; spec[sl] = 0;
                n = snprintf(buf, sizeof buf, spec, v);
            } else {
                spec[sl++] = c; spec[sl] = 0;
                if (c == 'd' || c == 'i') n = snprintf(buf, sizeof buf, spec, (int32_t)args[ai++]);
                else n = snprintf(buf, sizeof buf, spec, args[ai++]);
            }
            break;
        case 'c':
            spec[sl++] = 'c'; spec[sl] = 0;
            n = snprintf(buf, sizeof buf, spec, (int)(args[ai++] & 0xff));
            break;
        case 's': case 'S': {
            uint32_t sp = args[ai++];
            spec[sl++] = 's'; spec[sl] = 0;
            n = snprintf(buf, sizeof buf, spec, sp ? (const char *)(uintptr_t)sp : "(null)");
            break;
        }
        case 'p':
            n = snprintf(buf, sizeof buf, "%08X", args[ai++]);
            break;
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            double d;
            uint64_t v = args[ai] | ((uint64_t)args[ai + 1] << 32);
            ai += 2;
            memcpy(&d, &v, 8);
            spec[sl++] = c; spec[sl] = 0;
            n = snprintf(buf, sizeof buf, spec, d);
            break;
        }
        case '%': buf[0] = '%'; n = 1; break;
        default: buf[0] = '%'; buf[1] = c; n = 2; break;
        }
        if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
        for (int i = 0; i < n; i++) PUT(buf[i]);
    }
    if (outsz) out[o < outsz ? o : outsz - 1] = 0;
    return (int)(o < outsz ? o : outsz - 1);
#undef PUT
}
