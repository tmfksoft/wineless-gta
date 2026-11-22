#include "com.h"

#include <stdio.h>
#include <string.h>

#include "../heap.h"
#include "../log.h"
#include "../lowmem.h"

uint32_t com_vtable(const char *label, const ComMethod *m, int n) {
    uint32_t vt = gheap_alloc(4 * n, 1);
    uint32_t *v = G2H(vt);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%d", label, i);
        // A NULL fn has an unknown arg count, so calling it is fatal (and logged by name).
        v[i] = api_register_dynamic(name, m[i].fn, m[i].nargs, 0);
    }
    return vt;
}

uint32_t com_new(uint32_t vtbl, void *host, uint32_t extra) {
    uint32_t o = gheap_alloc(sizeof(ComObj) + extra, 1);
    ComObj *c = G2H(o);
    c->vtbl = vtbl;
    c->host_lo = (uint32_t)(uintptr_t)host;
    c->host_hi = (uint32_t)((uintptr_t)host >> 32);
    c->refs = 1;
    return o;
}

uint32_t com_addref(uint32_t *a) { return ++com_obj(a[0])->refs; }
uint32_t com_release_nofree(uint32_t *a) {
    ComObj *o = com_obj(a[0]);
    return o->refs ? --o->refs : 0;
}
uint32_t com_qi_self(uint32_t *a) {
    *(uint32_t *)G2H(a[2]) = a[0];
    com_obj(a[0])->refs++;
    return S_OK;
}
uint32_t com_notimpl(uint32_t *a) { return E_NOTIMPL; }

int guid_eq(const void *g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
    const uint8_t *p = g;
    uint32_t a;
    uint16_t b, c;
    memcpy(&a, p, 4);
    memcpy(&b, p + 4, 2);
    memcpy(&c, p + 6, 2);
    return a == d1 && b == d2 && c == d3 && !memcmp(p + 8, d4, 8);
}
