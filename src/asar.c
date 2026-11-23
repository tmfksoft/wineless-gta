// ASAR archive reader + aligned packer. See asar.h.
#include "asar.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DATA_ALIGN 4096u
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------- index

typedef struct {
    char *path;      // relative, lowercased, forward slashes, no leading slash
    uint64_t offset; // absolute byte offset in the archive file (files only)
    uint64_t size;
    int is_dir;
} Entry;

struct Asar {
    int fd;
    uint64_t data_start;
    Entry *ents;
    size_t n, cap;
};

static void norm_path(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        char c = in[i];
        if (c == '\\') c = '/';
        if (c == '/' && (j == 0 || out[j - 1] == '/')) continue; // collapse // and leading /
        out[j++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    while (j > 0 && out[j - 1] == '/') j--; // strip trailing /
    out[j] = 0;
}

static void add_entry(Asar *a, const char *path, uint64_t off, uint64_t size, int is_dir) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->ents = realloc(a->ents, a->cap * sizeof *a->ents);
    }
    Entry *e = &a->ents[a->n++];
    e->path = strdup(path);
    e->offset = off;
    e->size = size;
    e->is_dir = is_dir;
}

static int ent_cmp(const void *x, const void *y) {
    return strcmp(((const Entry *)x)->path, ((const Entry *)y)->path);
}

// ---------------------------------------------------------------- JSON (header)
// Minimal recursive-descent parser for the asar header shape:
//   node := '{' ( '"files"' ':' dirobj | '"size"' ':' num ',' '"offset"' ':' str | ... ) '}'

typedef struct { const char *p; const char *end; } Jp;

static void j_ws(Jp *j) { while (j->p < j->end && (*j->p==' '||*j->p=='\t'||*j->p=='\n'||*j->p=='\r')) j->p++; }
static int  j_ch(Jp *j, char c) { j_ws(j); if (j->p < j->end && *j->p == c) { j->p++; return 1; } return 0; }

// Parse a JSON string into buf (unescaped, truncated to bufsz). Returns 1 on ok.
static int j_str(Jp *j, char *buf, size_t bufsz) {
    j_ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    size_t k = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            if (e == 'u') { // \uXXXX — take low byte, skip 4 hex
                unsigned v = 0;
                for (int i = 0; i < 4 && j->p < j->end; i++) {
                    char h = *j->p++;
                    v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                c = (char)v;
            }
        }
        if (k + 1 < bufsz) buf[k++] = c;
    }
    if (j->p < j->end) j->p++; // closing quote
    buf[k] = 0;
    return 1;
}

// Skip one JSON value (string/number/object/array/literal).
static void j_skip(Jp *j);
static void j_skip_obj(Jp *j) {
    char tmp[8];
    if (!j_ch(j, '{')) return;
    if (j_ch(j, '}')) return;
    do { j_str(j, tmp, sizeof tmp); j_ch(j, ':'); j_skip(j); } while (j_ch(j, ','));
    j_ch(j, '}');
}
static void j_skip(Jp *j) {
    j_ws(j);
    if (j->p >= j->end) return;
    char c = *j->p;
    if (c == '"') { char t[8]; j_str(j, t, sizeof t); }
    else if (c == '{') j_skip_obj(j);
    else if (c == '[') { j->p++; if (!j_ch(j, ']')) { do j_skip(j); while (j_ch(j, ',')); j_ch(j, ']'); } }
    else while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']') j->p++;
}

static void parse_node(Asar *a, Jp *j, const char *prefix);

// Parse the object that is the value of a "files" key: { name: node, ... }
static void parse_files(Asar *a, Jp *j, const char *prefix) {
    if (!j_ch(j, '{')) return;
    if (j_ch(j, '}')) return;
    do {
        char name[512];
        if (!j_str(j, name, sizeof name)) break;
        j_ch(j, ':');
        char child[1024];
        if (prefix[0]) snprintf(child, sizeof child, "%s/%s", prefix, name);
        else snprintf(child, sizeof child, "%s", name);
        char low[1024];
        norm_path(child, low, sizeof low);
        parse_node(a, j, low);
    } while (j_ch(j, ','));
    j_ch(j, '}');
}

// Parse one node object; `path` is its normalised path.
static void parse_node(Asar *a, Jp *j, const char *path) {
    if (!j_ch(j, '{')) return;
    uint64_t size = 0, offset = 0;
    int have_off = 0, is_dir = 0;
    if (!j_ch(j, '}')) {
        do {
            char key[32];
            j_str(j, key, sizeof key);
            j_ch(j, ':');
            if (!strcmp(key, "files")) { is_dir = 1; parse_files(a, j, path); }
            else if (!strcmp(key, "size")) { j_ws(j); size = strtoull(j->p, (char **)&j->p, 10); }
            else if (!strcmp(key, "offset")) { char o[32]; j_str(j, o, sizeof o); offset = strtoull(o, NULL, 10); have_off = 1; }
            else j_skip(j);
        } while (j_ch(j, ','));
        j_ch(j, '}');
    }
    if (is_dir) add_entry(a, path, 0, 0, 1);
    else if (have_off) add_entry(a, path, a->data_start + offset, size, 0);
    else add_entry(a, path, 0, size, 0); // e.g. unpacked/link — recorded, zero-length here
}

// ---------------------------------------------------------------- open / query

// A bundle is "<binary><asar><footer>"; the footer lets us find the asar start.
#define WL_BUNDLE_MAGIC "WLASARv1" // 8 bytes
#define WL_FOOTER_SIZE  24         // u64 asar_offset, u64 asar_size, 8-byte magic

// If `fd` ends with a bundle footer, return the embedded asar's byte offset;
// else 0 (a plain .asar reads from offset 0).
static uint64_t bundle_base(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < WL_FOOTER_SIZE) return 0;
    uint8_t f[WL_FOOTER_SIZE];
    if (pread(fd, f, WL_FOOTER_SIZE, st.st_size - WL_FOOTER_SIZE) != WL_FOOTER_SIZE) return 0;
    if (memcmp(f + 16, WL_BUNDLE_MAGIC, 8) != 0) return 0;
    uint64_t off, size;
    memcpy(&off, f, 8);
    memcpy(&size, f + 8, 8);
    if (off + size + WL_FOOTER_SIZE > (uint64_t)st.st_size) return 0; // sanity
    return off;
}

int asar_is_bundle(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint64_t b = bundle_base(fd);
    close(fd);
    return b != 0;
}

Asar *asar_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    uint64_t base = bundle_base(fd); // 0 for a plain .asar
    uint8_t pre[16];
    if (pread(fd, pre, 16, (off_t)base) != 16) { close(fd); return NULL; }
    // Pickle preamble: [u32 4][u32 P][u32 P][u32 jsonLen], json at 16, data after.
    uint32_t json_len;
    memcpy(&json_len, pre + 12, 4);
    if (json_len == 0 || json_len > (64u << 20)) { close(fd); return NULL; }
    char *json = malloc(json_len + 1);
    if (pread(fd, json, json_len, (off_t)base + 16) != (ssize_t)json_len) { free(json); close(fd); return NULL; }
    json[json_len] = 0;
    // Sanity: the header is a directory node (prevents a bogus footer match).
    if (strncmp(json, "{\"files\":{", 10) != 0) { free(json); close(fd); return NULL; }

    Asar *a = calloc(1, sizeof *a);
    a->fd = fd;
    a->data_start = base + 16 + align_up(json_len, 4);
    Jp j = { json, json + json_len };
    // The top node is { "files": { ... } }
    parse_node(a, &j, "");
    free(json);
    qsort(a->ents, a->n, sizeof *a->ents, ent_cmp);
    return a;
}

void asar_close(Asar *a) {
    if (!a) return;
    if (a->fd >= 0) close(a->fd);
    for (size_t i = 0; i < a->n; i++) free(a->ents[i].path);
    free(a->ents);
    free(a);
}

int asar_fd(const Asar *a) { return a ? a->fd : -1; }

static const Entry *find(const Asar *a, const char *relpath) {
    char low[1024];
    norm_path(relpath, low, sizeof low);
    Entry key = { .path = low };
    return bsearch(&key, a->ents, a->n, sizeof *a->ents, ent_cmp);
}

int asar_stat(const Asar *a, const char *relpath, uint64_t *offset, uint64_t *size, int *is_dir) {
    const Entry *e = find(a, relpath);
    if (!e) { if (is_dir) *is_dir = 0; return 0; }
    if (offset) *offset = e->offset;
    if (size) *size = e->size;
    if (is_dir) *is_dir = e->is_dir;
    return 1;
}

int asar_list(const Asar *a, const char *reldir, asar_list_cb cb, void *ctx) {
    char low[1024];
    norm_path(reldir, low, sizeof low);
    size_t plen = strlen(low);
    if (plen && !find(a, reldir)) return -1; // a named dir must exist (root always matches)
    int count = 0;
    for (size_t i = 0; i < a->n; i++) {
        const char *p = a->ents[i].path;
        if (plen) {
            if (strncmp(p, low, plen) != 0 || p[plen] != '/') continue;
            p += plen + 1;
        }
        if (strchr(p, '/')) continue; // not an immediate child
        if (!*p) continue;
        cb(ctx, p, a->ents[i].is_dir, a->ents[i].size);
        count++;
    }
    return count;
}

void asar_foreach(const Asar *a, asar_each_cb cb, void *ctx) {
    for (size_t i = 0; i < a->n; i++) {
        const Entry *e = &a->ents[i];
        if (e->path[0]) cb(ctx, e->path, e->offset, e->size, e->is_dir);
    }
}

// ---------------------------------------------------------------- packer

typedef struct { char *buf; size_t n, cap; } Sbuf;
static void sb_add(Sbuf *s, const char *p, size_t n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; s->buf = realloc(s->buf, s->cap); }
    memcpy(s->buf + s->n, p, n); s->n += n; s->buf[s->n] = 0;
}
static void sb_str(Sbuf *s, const char *p) { sb_add(s, p, strlen(p)); }
static void sb_json_str(Sbuf *s, const char *p) {
    sb_add(s, "\"", 1);
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; sb_add(s, e, 2); }
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); sb_str(s, e); }
        else sb_add(s, (char *)&c, 1);
    }
    sb_add(s, "\"", 1);
}

typedef struct { char **paths; uint64_t *offs; size_t n, cap; } FileList;
static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Recursively emit the JSON "files" object for `hostdir` and assign aligned
// offsets to each file, recording (hostpath, offset) for the data pass.
static int pack_dir(const char *hostdir, Sbuf *json, uint64_t *cursor, FileList *fl) {
    DIR *d = opendir(hostdir);
    if (!d) return -1;
    char **names = NULL; size_t nn = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (nn == cap) { cap = cap ? cap * 2 : 32; names = realloc(names, cap * sizeof *names); }
        names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof *names, name_cmp);

    sb_str(json, "{\"files\":{");
    for (size_t i = 0; i < nn; i++) {
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", hostdir, names[i]);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (i) sb_add(json, ",", 1);
        sb_json_str(json, names[i]);
        sb_add(json, ":", 1);
        if (S_ISDIR(st.st_mode)) {
            pack_dir(full, json, cursor, fl);
        } else {
            uint64_t off = *cursor;
            char num[48];
            snprintf(num, sizeof num, "{\"size\":%lld,\"offset\":\"%llu\"}",
                     (long long)st.st_size, (unsigned long long)off);
            sb_str(json, num);
            *cursor = align_up(off + (uint64_t)st.st_size, DATA_ALIGN);
            if (fl->n == fl->cap) {
                fl->cap = fl->cap ? fl->cap * 2 : 256;
                fl->paths = realloc(fl->paths, fl->cap * sizeof *fl->paths);
                fl->offs = realloc(fl->offs, fl->cap * sizeof *fl->offs);
            }
            fl->paths[fl->n] = strdup(full);
            fl->offs[fl->n++] = off;
        }
    }
    sb_str(json, "}}");
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    return 0;
}

int asar_pack(const char *srcdir, const char *outpath) {
    Sbuf json = {0};
    FileList fl = {0};
    uint64_t cursor = 0;
    if (pack_dir(srcdir, &json, &cursor, &fl) != 0) { free(json.buf); return 1; }

    // Pad the JSON (trailing spaces are ignored by parsers) so the data section
    // begins on a DATA_ALIGN boundary: data_start = 16 + align4(jsonLen).
    while ((16 + align_up(json.n, 4)) % DATA_ALIGN != 0) sb_add(&json, " ", 1);
    uint32_t json_len = (uint32_t)json.n;
    uint32_t json_pad = (uint32_t)align_up(json_len, 4);
    // Chromium Pickle layout (matches @electron/asar exactly):
    //   @0  = 4                 (size pickle payload size)
    //   @4  = 8 + json_pad      (header buffer length = what the reader reads next)
    //   @8  = 4 + json_pad      (header pickle payload size)
    //   @12 = json_len          (the JSON string length)
    //   @16 = json (+pad to 4) ; data section begins at 16 + json_pad
    uint64_t data_start = 16 + json_pad;

    int out = open(outpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { free(json.buf); free(fl.paths); free(fl.offs); return 2; }
    uint8_t pre[16];
    uint32_t v;
    v = 4;            memcpy(pre + 0, &v, 4);
    v = 8 + json_pad; memcpy(pre + 4, &v, 4);
    v = 4 + json_pad; memcpy(pre + 8, &v, 4);
    v = json_len;     memcpy(pre + 12, &v, 4);
    int ok = write(out, pre, 16) == 16;
    ok = ok && write(out, json.buf, json_len) == (ssize_t)json_len;
    for (uint32_t i = json_len; ok && i < json_pad; i++) ok = write(out, " ", 1) == 1; // pad json to 4

    // Data section: each file at data_start + its assigned (aligned) offset.
    char copybuf[1 << 16];
    for (size_t i = 0; ok && i < fl.n; i++) {
        if (lseek(out, (off_t)(data_start + fl.offs[i]), SEEK_SET) < 0) { ok = 0; break; }
        int in = open(fl.paths[i], O_RDONLY | O_CLOEXEC);
        if (in < 0) { ok = 0; break; }
        ssize_t r;
        while (ok && (r = read(in, copybuf, sizeof copybuf)) > 0) ok = write(out, copybuf, r) == r;
        close(in);
    }
    close(out);
    free(json.buf);
    for (size_t i = 0; i < fl.n; i++) free(fl.paths[i]);
    free(fl.paths); free(fl.offs);
    return ok ? 0 : 3;
}

long asar_read_whole(const char *path, const char *relpath, char *buf, size_t cap) {
    Asar *a = asar_open(path);
    if (!a) return -1;
    uint64_t off, size; int isdir;
    long r = -1;
    if (asar_stat(a, relpath, &off, &size, &isdir) && !isdir) {
        size_t n = size < cap ? (size_t)size : cap;
        ssize_t got = pread(a->fd, buf, n, (off_t)off);
        r = got < 0 ? -1 : (long)got;
    }
    asar_close(a);
    return r;
}

// ---------------------------------------------------------------- bundle maker

static int copy_fd(int src, int dst) {
    char b[1 << 16];
    ssize_t r;
    while ((r = read(src, b, sizeof b)) > 0)
        if (write(dst, b, r) != r) return -1;
    return r < 0 ? -1 : 0;
}

int asar_make_bundle(const char *base_bin, const char *game, const char *out) {
    char tmp[4096];
    const char *asar_path;
    int made_tmp = 0;
    size_t gl = strlen(game);
    struct stat gst;
    if (gl > 5 && strcasecmp(game + gl - 5, ".asar") == 0) {
        asar_path = game;
    } else if (stat(game, &gst) == 0 && S_ISDIR(gst.st_mode)) {
        snprintf(tmp, sizeof tmp, "%s.pack.asar", out);
        if (asar_pack(game, tmp) != 0) { fprintf(stderr, "asar: packing %s failed\n", game); return 1; }
        asar_path = tmp;
        made_tmp = 1;
    } else {
        fprintf(stderr, "asar: %s is not a directory or .asar\n", game);
        return 1;
    }

    int rc = 1;
    int bi = open(base_bin, O_RDONLY | O_CLOEXEC);
    int oo = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    int ai = open(asar_path, O_RDONLY | O_CLOEXEC);
    if (bi >= 0 && oo >= 0 && ai >= 0 && copy_fd(bi, oo) == 0) {
        off_t base_size = lseek(oo, 0, SEEK_CUR);
        // Pad to a 4K boundary so the embedded asar's (already 4K-aligned) data
        // stays page-aligned in the bundle — keeps MapViewOfFile zero-copy.
        off_t pad = (DATA_ALIGN - (base_size % DATA_ALIGN)) % DATA_ALIGN;
        if (pad) {
            static const char z[DATA_ALIGN] = { 0 };
            if (write(oo, z, pad) == pad) base_size += pad;
        }
        struct stat ast;
        fstat(ai, &ast);
        if (copy_fd(ai, oo) == 0) {
            uint8_t f[WL_FOOTER_SIZE];
            uint64_t off = (uint64_t)base_size, size = (uint64_t)ast.st_size;
            memcpy(f, &off, 8);
            memcpy(f + 8, &size, 8);
            memcpy(f + 16, WL_BUNDLE_MAGIC, 8);
            if (write(oo, f, WL_FOOTER_SIZE) == WL_FOOTER_SIZE) rc = 0;
        }
    }
    if (bi >= 0) close(bi);
    if (oo >= 0) close(oo);
    if (ai >= 0) close(ai);
    if (made_tmp) unlink(tmp);
    if (rc == 0) chmod(out, 0755);
    return rc;
}

// ---------------------------------------------------------------- debug

struct ExeFind { char top[512]; char any[512]; };
static void exe_scan(void *v, const char *rel, uint64_t off, uint64_t size, int is_dir) {
    (void)off; (void)size;
    struct ExeFind *f = v;
    size_t l = strlen(rel);
    if (is_dir || l < 4 || strcasecmp(rel + l - 4, ".exe")) return;
    if (!f->any[0]) snprintf(f->any, sizeof f->any, "%s", rel);
    if (!strchr(rel, '/') && !f->top[0]) snprintf(f->top, sizeof f->top, "%s", rel);
}
int asar_find_exe(const char *path, char *out, size_t outsz) {
    Asar *a = asar_open(path);
    if (!a) return 0;
    struct ExeFind f = {{0}, {0}};
    asar_foreach(a, exe_scan, &f);
    asar_close(a);
    const char *pick = f.top[0] ? f.top : f.any[0] ? f.any : NULL;
    if (!pick) return 0;
    size_t j = 0;
    for (size_t i = 0; pick[i] && j + 1 < outsz; i++) out[j++] = pick[i] == '/' ? '\\' : pick[i];
    out[j] = 0;
    return 1;
}

int asar_debug_list(const char *path, const char *relpath) {
    Asar *a = asar_open(path);
    if (!a) { fprintf(stderr, "asar: cannot open %s\n", path); return 1; }
    fprintf(stderr, "asar %s: data_start=%llu, %zu entries\n",
            path, (unsigned long long)a->data_start, a->n);
    for (size_t i = 0; i < a->n; i++) {
        const Entry *e = &a->ents[i];
        if (e->is_dir) fprintf(stderr, "  <dir>  %s\n", e->path);
        else fprintf(stderr, "  %8llu @%-10llu %s%s\n", (unsigned long long)e->size,
                     (unsigned long long)e->offset, e->path,
                     (e->offset % DATA_ALIGN) ? " (unaligned)" : "");
    }
    if (relpath) {
        uint64_t off, size; int dir;
        if (asar_stat(a, relpath, &off, &size, &dir) && !dir) {
            uint8_t buf[64];
            uint64_t n = size < sizeof buf ? size : sizeof buf;
            ssize_t r = pread(a->fd, buf, n, (off_t)off);
            fprintf(stderr, "read %s (%lld bytes) head: ", relpath, (long long)r);
            for (ssize_t i = 0; i < r; i++)
                fprintf(stderr, "%c", (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
            fprintf(stderr, "\n");
        } else fprintf(stderr, "entry '%s' not found or is a dir\n", relpath);
    }
    asar_close(a);
    return 0;
}
