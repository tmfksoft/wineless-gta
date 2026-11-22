// version.dll: file version information read from a PE file's VS_VERSIONINFO
// resource (RT_VERSION).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "../pe.h"
#include "../vfs.h"
#include "win32.h"

#define RT_VERSION 16

static void *read_version(const char *winpath, uint32_t *size) {
    char host[4096];
    if (!vfs_to_host(winpath, host, sizeof host)) return NULL;
    void *r = pe_file_resource(host, RT_VERSION, size);
    if (!r) LOGD("GetFileVersionInfo(%s): no version resource", winpath);
    return r;
}

// The buffer holds the resource followed by as much space again, where
// VerQueryValueA puts narrow copies of string values (as Windows does).
API(version, GetFileVersionInfoSizeA, 2) {
    if (ARG(1)) *(uint32_t *)PTR(1) = 0;
    uint32_t size;
    void *r = read_version(STR(0), &size);
    if (!r) { set_last_error(1813 /*ERROR_RESOURCE_TYPE_NOT_FOUND*/); return 0; }
    free(r);
    return size * 2 + 4;
}
API(version, GetFileVersionInfoA, 4) {
    uint32_t size;
    void *r = read_version(STR(0), &size);
    if (!r) { set_last_error(1813); return FALSE; }
    uint32_t n = size < ARG(2) ? size : ARG(2);
    memset(PTR(3), 0, ARG(2));
    memcpy(PTR(3), r, n);
    free(r);
    return TRUE;
}

// VS_VERSIONINFO nodes: wLength, wValueLength, wType, szKey (UTF-16), pad to 4,
// value, pad to 4, children.
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static bool key_eq(const uint16_t *k, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower(k[i]) != tolower((unsigned char)s[i])) return false;
    return k[n] == 0;
}

// VerQueryValueA(block, subblock, &ptr, &len)
API(version, VerQueryValueA, 4) {
    uint8_t *base = PTR(0);
    const char *path = STR(1);
    uint32_t node = 0;
    uint16_t total = *(uint16_t *)base;
    while (*path == '\\') path++;
    while (*path) {
        const char *e = strchr(path, '\\');
        size_t seg = e ? (size_t)(e - path) : strlen(path);
        // children of `node`
        uint16_t nlen = *(uint16_t *)(base + node), vlen = *(uint16_t *)(base + node + 2), type = *(uint16_t *)(base + node + 4);
        uint32_t k = node + 6;
        while (*(uint16_t *)(base + k)) k += 2;
        uint32_t c = align4(k + 2) + (type == 1 ? vlen * 2 : vlen);
        c = align4(c);
        uint32_t found = 0;
        while (c < node + nlen && c < total) {
            uint16_t clen = *(uint16_t *)(base + c);
            if (!clen) break;
            if (key_eq((uint16_t *)(base + c + 6), path, seg)) { found = c; break; }
            c = align4(c + clen);
        }
        if (!found) return FALSE;
        node = found;
        path += seg;
        while (*path == '\\') path++;
    }
    uint16_t vlen = *(uint16_t *)(base + node + 2), type = *(uint16_t *)(base + node + 4);
    uint32_t k = node + 6;
    while (*(uint16_t *)(base + k)) k += 2;
    uint32_t v = align4(k + 2);
    if (type == 1 && vlen) {
        // a string: hand back a narrow copy from the spare half of the buffer
        char *dst = (char *)base + total + node; // node offsets are distinct, so copies never overlap
        for (uint16_t i = 0; i < vlen; i++) dst[i] = (char)*(uint16_t *)(base + v + i * 2);
        *(uint32_t *)PTR(2) = H2G(dst);
        if (ARG(3)) *(uint32_t *)PTR(3) = vlen;
    } else {
        *(uint32_t *)PTR(2) = H2G(base + v);
        if (ARG(3)) *(uint32_t *)PTR(3) = vlen;
    }
    return vlen > 0 || node == 0;
}
