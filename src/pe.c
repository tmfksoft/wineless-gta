// PE image loader: the main exe, real DLLs from disk (relocations, imports,
// exports/forwarders, static TLS, DllMain) and builtin DLLs, all in one
// module table. Which implementation a DLL gets is decided like Wine's DLL
// overrides: builtin when we implement it, otherwise the file next to the game;
// WINELESS_DLL_OVERRIDES="name=native,other=builtin" changes that per DLL.
#include "pe.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "cpu.h"
#include "log.h"
#include "lowmem.h"
#include "thread.h"
#include "vfs.h"

PeImage g_exe;

#pragma pack(push, 1)
typedef struct { uint32_t va, size; } DataDir;
typedef struct {
    uint16_t magic; uint8_t lmaj, lmin;
    uint32_t code_size, init_size, uninit_size, entry, code_base, data_base, image_base;
    uint32_t sect_align, file_align;
    uint16_t osmaj, osmin, imgmaj, imgmin, submaj, submin;
    uint32_t win32ver, image_size, headers_size, checksum;
    uint16_t subsystem, dllchars;
    uint32_t stack_res, stack_commit, heap_res, heap_commit, loader_flags, ndirs;
    DataDir dirs[16];
} OptHdr32;
typedef struct {
    char name[8];
    uint32_t vsize, va, raw_size, raw_ptr, reloc_ptr, line_ptr;
    uint16_t nreloc, nline;
    uint32_t chars;
} SectHdr;
typedef struct { uint32_t oft, ts, fwd, name, ft; } ImportDesc;
typedef struct {
    uint32_t chars, ts; uint16_t maj, min, nnamed, nid;
} ResDir;
typedef struct { uint32_t name, off; } ResEnt;
typedef struct { uint32_t data, size, cp, res; } ResData;
typedef struct {
    uint32_t chars, ts; uint16_t maj, min;
    uint32_t name, base, nfuncs, nnames, funcs, names, ords;
} ExportDir;
typedef struct { uint32_t start, end, index, callbacks, zero, chars; } TlsDir;
#pragma pack(pop)

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RELOC = 5, DIR_TLS = 9 };

#define MAX_MODULES 128
static Module mods[MAX_MODULES];
static int nmods;
static pthread_mutex_t mod_lock;
static int ntls; // static TLS slots handed out
// Like the Windows loader, DLL initializers run only after the whole import
// graph has been mapped and bound (dependencies first). ASI loaders rely on
// this: they patch the exe's import table from DllMain.
static Module *init_queue[MAX_MODULES];
static int ninit, init_head, load_depth;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init_lock(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mod_lock, &a);
}
static void lock(void) { pthread_once(&once, init_lock); pthread_mutex_lock(&mod_lock); }
static void unlock(void) { pthread_mutex_unlock(&mod_lock); }

static OptHdr32 *opt_of(uint32_t base) {
    uint8_t *img = G2H(base);
    return (OptHdr32 *)(img + *(uint32_t *)(img + 0x3c) + 24);
}

static void norm_name(const char *in, char *out, int outsz) {
    const char *bs = strrchr(in, '\\'), *fs = strrchr(in, '/');
    if (fs > bs) bs = fs;
    api_norm_dll(bs ? bs + 1 : in, out, outsz);
}

static Module *find_loaded(const char *norm) {
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, norm)) return &mods[i];
    return NULL;
}

Module *module_by_base(uint32_t hmod) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].base == hmod) return &mods[i];
    return NULL;
}

const char *module_name(uint32_t hmod) {
    Module *m = module_by_base(hmod);
    return m ? m->name : NULL;
}

// ---------------------------------------------------------------- builtin DLLs
extern const ApiDef __start_wineless_api[], __stop_wineless_api[];

static bool dll_has_impl(const char *dll) {
    for (const ApiDef *d = __start_wineless_api; d < __stop_wineless_api; d++)
        if (!strcmp(d->dll, dll)) return true;
    return false;
}

static const ApiDef *sort_defs[2048];
static int cmp_def_name(const void *x, const void *y) {
    const ApiDef *a = *(const ApiDef *const *)x, *b = *(const ApiDef *const *)y;
    return strcmp(a->name, b->name);
}

// Build a real (tiny) PE image for a builtin DLL: DOS/PE headers, an export
// directory naming every function we implement, and the API thunks inside the
// image. Code that walks from a function address back to the module's "MZ"
// header, or parses export tables itself, then works like on Windows.
static Module *add_builtin(const char *norm) {
    if (nmods == MAX_MODULES) return NULL;
    int n = 0, maxord = 0;
    for (const ApiDef *d = __start_wineless_api; d < __stop_wineless_api && n < 2048; d++)
        if (!strcmp(d->dll, norm)) {
            sort_defs[n++] = d;
            if (d->ordinal > maxord) maxord = d->ordinal;
        }
    qsort(sort_defs, n, sizeof *sort_defs, cmp_def_name);
    // ordinals: explicit ones are kept, the rest follow the highest explicit one
    int nfuncs = maxord, ord_of[2048];
    for (int i = 0; i < n; i++) ord_of[i] = sort_defs[i]->ordinal ? sort_defs[i]->ordinal : ++nfuncs;

    // layout (RVAs): headers @0, export data @0x1000, thunks after it
    uint32_t exp = 0x1000, funcs = exp + 40, names = funcs + 4 * nfuncs, ords = names + 4 * n;
    uint32_t strs = ords + 2 * n, sz_strs = strlen(norm) + 5;
    for (int i = 0; i < n; i++) sz_strs += strlen(sort_defs[i]->name) + 1;
    uint32_t thunks = (strs + sz_strs + 15) & ~15u;
    uint32_t size = (thunks + 16 * n + 0xFFF) & ~0xFFFu;
    uint32_t base = lm_alloc(size, LM_GRAN, PAGE_EXECUTE_READWRITE, LM_TAG_IMAGE);
    if (!base) return NULL;
    uint8_t *img = G2H(base);

    // DOS + PE headers
    img[0] = 'M'; img[1] = 'Z';
    *(uint32_t *)(img + 0x3c) = 0x40;
    memcpy(img + 0x40, "PE\0\0", 4);
    uint8_t *fh = img + 0x44;
    *(uint16_t *)(fh + 0) = 0x14c;      // i386
    *(uint16_t *)(fh + 2) = 1;          // sections
    *(uint16_t *)(fh + 16) = 0xE0;      // optional header size
    *(uint16_t *)(fh + 18) = 0x2102;    // DLL | 32BIT | EXECUTABLE
    OptHdr32 *oh = (OptHdr32 *)(fh + 20);
    oh->magic = 0x10b;
    oh->image_base = base;
    oh->sect_align = 0x1000;
    oh->file_align = 0x200;
    oh->osmaj = 5; oh->osmin = 1; oh->submaj = 5; oh->submin = 1;
    oh->image_size = size;
    oh->headers_size = 0x1000;
    oh->subsystem = 2;
    oh->ndirs = 16;
    oh->dirs[DIR_EXPORT].va = exp;
    oh->dirs[DIR_EXPORT].size = strs + sz_strs - exp;
    SectHdr *sh = (SectHdr *)((uint8_t *)oh + 0xE0);
    memcpy(sh->name, ".text", 5);
    sh->va = 0x1000;
    sh->vsize = size - 0x1000;
    sh->chars = 0x60000020;

    // export directory
    ExportDir *ed = (ExportDir *)(img + exp);
    char *str = (char *)img + strs;
    ed->name = strs;
    str += sprintf(str, "%s.dll", norm) + 1;
    ed->base = 1;
    ed->nfuncs = nfuncs;
    ed->nnames = n;
    ed->funcs = funcs;
    ed->names = names;
    ed->ords = ords;
    for (int i = 0; i < n; i++) {
        uint32_t t = thunks + 16 * i;
        if (sort_defs[i]->flags & API_DATA) t = api_bind_def_at(sort_defs[i], 0) - base; // variable elsewhere
        else api_bind_def_at(sort_defs[i], base + t);
        ((uint32_t *)(img + funcs))[ord_of[i] - 1] = t;
        ((uint32_t *)(img + names))[i] = (uint32_t)((uint8_t *)str - img);
        ((uint16_t *)(img + ords))[i] = (uint16_t)(ord_of[i] - 1);
        str += sprintf(str, "%s", sort_defs[i]->name) + 1;
    }

    Module *m = &mods[nmods++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", norm);
    snprintf(m->vpath, sizeof m->vpath, "C:\\Windows\\System32\\%s.dll", norm);
    m->base = base;
    m->size = size;
    m->builtin = true;
    m->exp_rva = exp;
    m->exp_size = oh->dirs[DIR_EXPORT].size;
    m->tls_index = -1;
    LOGD("module: builtin %s -> 0x%x (%d exports)", norm, base, n);
    return m;
}

// ---------------------------------------------------------------- overrides / search
static char override_for(const char *norm) {
    const char *o = getenv("WINELESS_DLL_OVERRIDES");
    if (!o) return 0;
    size_t l = strlen(norm);
    for (const char *p = o; *p;) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        if ((size_t)(eq - p) == l && !strncasecmp(p, norm, l)) return tolower((unsigned char)eq[1]); // 'n' / 'b'
        p = strchr(eq, ',');
        if (!p) break;
        p++;
    }
    return 0;
}

// Find a DLL file the way Windows would (application dir, then current dir).
// Fills host and Windows paths. `name` may be a path.
// Folders inside the game folder where games keep redistributable DLLs (Age of
// Empires II HD: Microsoft's d3dx9/D3DCompiler). Only searched for DLLs we have
// no builtin for, so e.g. a dsound.dll there doesn't replace ours.
static const char *const redist_dirs[] = {"D3D9_DLLBACKUP"};

static bool find_dll_file_ex(const char *name, char *host, size_t hostsz, char *vpath, size_t vsz, bool redist);
static bool find_dll_file(const char *name, char *host, size_t hostsz, char *vpath, size_t vsz) {
    return find_dll_file_ex(name, host, hostsz, vpath, vsz, false);
}
static bool find_dll_file_ex(const char *name, char *host, size_t hostsz, char *vpath, size_t vsz, bool redist) {
    char want[512];
    snprintf(want, sizeof want, "%s", name);
    const char *base = strrchr(want, '\\');
    if (!base) base = strrchr(want, '/');
    if (!strchr(base ? base : want, '.')) strncat(want, ".dll", sizeof want - strlen(want) - 1);
    char cand[8][600];
    int n = 0;
    if (strchr(want, '\\') || strchr(want, '/') || (want[0] && want[1] == ':')) {
        snprintf(cand[n++], sizeof cand[0], "%s", want);
    } else {
        char exedir[512];
        snprintf(exedir, sizeof exedir, "%s", g_exe.vpath);
        char *bs = strrchr(exedir, '\\');
        if (bs) *bs = 0;
        if (redist) {
            for (unsigned i = 0; i < sizeof redist_dirs / sizeof *redist_dirs; i++)
                snprintf(cand[n++], sizeof cand[0], "%s\\%s\\%s", exedir, redist_dirs[i], want);
        } else {
            snprintf(cand[n++], sizeof cand[0], "%s\\%s", exedir, want);
            snprintf(cand[n++], sizeof cand[0], "%s\\%s", vfs_get_cwd(), want);
        }
    }
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (vfs_to_host(cand[i], host, hostsz) && !stat(host, &st) && S_ISREG(st.st_mode)) {
            vfs_full_path(cand[i], vpath, vsz);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- exports
static uint32_t resolve_import(const char *dll, const char *name, int ord);

static uint32_t export_lookup(Module *m, const char *name, int ordinal) {
    if (m->builtin) return api_resolve(m->name, name, ordinal, 0);
    if (!m->exp_rva) return 0;
    uint8_t *img = G2H(m->base);
    ExportDir *ed = (ExportDir *)(img + m->exp_rva);
    uint32_t *funcs = (uint32_t *)(img + ed->funcs);
    uint32_t idx = 0xFFFFFFFF;
    if (name) {
        uint32_t *names = (uint32_t *)(img + ed->names);
        uint16_t *ords = (uint16_t *)(img + ed->ords);
        uint32_t lo = 0, hi = ed->nnames;
        while (lo < hi) { // names are sorted
            uint32_t mid = (lo + hi) / 2;
            int c = strcmp((const char *)img + names[mid], name);
            if (!c) { idx = ords[mid]; break; }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        if (idx == 0xFFFFFFFF) // tolerate unsorted tables
            for (uint32_t i = 0; i < ed->nnames; i++)
                if (!strcmp((const char *)img + names[i], name)) { idx = ords[i]; break; }
    } else if ((uint32_t)ordinal >= ed->base) {
        idx = ordinal - ed->base;
    }
    if (idx == 0xFFFFFFFF || idx >= ed->nfuncs || !funcs[idx]) return 0;
    uint32_t rva = funcs[idx];
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {
        // forwarder "DLL.Func" or "DLL.#12"
        char fwd[256];
        snprintf(fwd, sizeof fwd, "%s", (const char *)img + rva);
        char *dot = strrchr(fwd, '.');
        if (!dot) return 0;
        *dot = 0;
        const char *fn = dot + 1;
        return fn[0] == '#' ? resolve_import(fwd, NULL, atoi(fn + 1)) : resolve_import(fwd, fn, 0);
    }
    return m->base + rva;
}

uint32_t module_proc(uint32_t hmod, const char *name, int ordinal) {
    lock();
    Module *m = module_by_base(hmod);
    uint32_t r = m ? export_lookup(m, name, ordinal) : 0;
    unlock();
    return r;
}

// Resolve one import for a native image: builtin thunk, native export, or a
// stub that reports the missing function by name if it is ever called.
static uint32_t resolve_import(const char *dll, const char *name, int ord) {
    uint32_t h = module_get(dll, true);
    Module *m = h ? module_by_base(h) : NULL;
    uint32_t r = m ? export_lookup(m, name, ord) : 0;
    if (!r) {
        if (name) LOGD("pe: import %s!%s not available", dll, name);
        else LOGD("pe: import %s!#%d not available", dll, ord);
        r = api_resolve(dll, name, ord, 1);
    }
    return r;
}

// ---------------------------------------------------------------- TLS
static void tls_alloc_block(Module *m, uint32_t teb) {
    uint32_t *teb_tls = (uint32_t *)G2H(teb + TEB_TLS_POINTER);
    if (!*teb_tls) {
        *teb_tls = lm_alloc(0x1000, LM_GRAN, PAGE_READWRITE, LM_TAG_SYSTEM); // up to 1024 slots
    }
    uint32_t tsz = m->tls_end - m->tls_start, total = tsz + m->tls_zero;
    uint32_t blk = lm_alloc(total ? total : 4, LM_PAGE, PAGE_READWRITE, LM_TAG_SYSTEM);
    if (tsz) memcpy(G2H(blk), G2H(m->tls_start), tsz);
    ((uint32_t *)G2H(*teb_tls))[m->tls_index] = blk;
}

void pe_tls_setup_thread(uint32_t teb) {
    lock();
    for (int i = 0; i < nmods; i++)
        if (mods[i].tls_index >= 0) tls_alloc_block(&mods[i], teb);
    unlock();
}

static void call_tls_callbacks(Module *m, uint32_t reason) {
    if (!m->tls_callbacks) return;
    Thread *t = thread_current();
    for (uint32_t *cb = G2H(m->tls_callbacks); *cb; cb++) {
        uint32_t args[3] = {m->base, reason, 0};
        LOGD("pe: TLS callback 0x%x(%s, %u)", *cb, m->name, reason);
        call32(t->ctx, *cb, 3, args);
    }
}

void pe_thread_attach(void) {
    Thread *t = thread_current();
    lock();
    int n = nmods;
    unlock();
    for (int i = 0; i < n; i++) {
        Module *m = &mods[i];
        if (m->builtin) continue;
        call_tls_callbacks(m, 2 /*DLL_THREAD_ATTACH*/);
        if (!m->is_exe && m->entry) {
            uint32_t args[3] = {m->base, 2, 0};
            call32(t->ctx, m->entry, 3, args);
        }
    }
}

// ---------------------------------------------------------------- mapping
static uint8_t *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc(st.st_size);
    if (read(fd, f, st.st_size) != st.st_size) { close(fd); free(f); return NULL; }
    close(fd);
    *len = st.st_size;
    return f;
}

static void run_pending_inits(void);

static Module *map_image_ex(const char *hostpath, const char *vpath, bool is_exe, bool inert);

// Give each section the protection the Windows loader would (after imports are
// bound): VirtualQuery then reports per-section regions the way programs that
// inspect their own memory layout expect.
static void protect_sections(uint32_t base) {
    uint8_t *img = G2H(base);
    uint32_t pe = *(uint32_t *)(img + 0x3c);
    uint16_t nsec = *(uint16_t *)(img + pe + 6), ohsz = *(uint16_t *)(img + pe + 20);
    SectHdr *sh = (SectHdr *)(img + pe + 24 + ohsz);
    uint32_t old;
    lm_protect(base, 0x1000, PAGE_READONLY, &old); // headers
    for (int i = 0; i < nsec; i++) {
        uint32_t c = sh[i].chars, len = sh[i].vsize ? sh[i].vsize : sh[i].raw_size;
        len = (len + 0xFFF) & ~0xFFFu;
        if (!len || (sh[i].va & 0xFFF)) continue; // unaligned layouts keep RWX
        bool x = c & 0x20000000, r = c & 0x40000000, w = c & 0x80000000;
        uint32_t prot = x ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                          : (w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS);
        if (prot == PAGE_NOACCESS) prot = PAGE_READONLY;
        lm_protect(base + sh[i].va, len, prot, &old);
    }
}
static Module *map_image(const char *hostpath, const char *vpath, bool is_exe) {
    return map_image_ex(hostpath, vpath, is_exe, false);
}
// inert: lay the image out in memory (sections, relocations) without binding
// imports, TLS or running any of its code.
static Module *map_image_ex(const char *hostpath, const char *vpath, bool is_exe, bool inert) {
    size_t len;
    uint8_t *f = read_file(hostpath, &len);
    if (!f) { LOGE("pe: cannot read %s", hostpath); return NULL; }
    uint32_t pe = len > 0x40 ? *(uint32_t *)(f + 0x3c) : 0;
    if (len < 0x40 || f[0] != 'M' || f[1] != 'Z' || pe + 0x100 > len || memcmp(f + pe, "PE\0\0", 4) ||
        *(uint16_t *)(f + pe + 4) != 0x14c) {
        LOGE("pe: %s is not a 32-bit x86 PE image", vpath);
        free(f);
        return NULL;
    }
    uint16_t nsect = *(uint16_t *)(f + pe + 6), optsz = *(uint16_t *)(f + pe + 20);
    uint16_t chars = *(uint16_t *)(f + pe + 22);
    OptHdr32 *oh = (OptHdr32 *)(f + pe + 24);
    uint32_t pref = oh->image_base, size = oh->image_size;
    bool relocs_stripped = (chars & 1) || !oh->dirs[DIR_RELOC].va;
    uint32_t base = lm_reserve(pref, size, LM_PAGE, LM_TAG_IMAGE, false);
    if (!base) {
        if (relocs_stripped) {
            LOGE("pe: %s needs 0x%x-0x%x, which is taken, and has no relocations", vpath, pref, pref + size);
            free(f);
            return NULL;
        }
        base = lm_reserve(0, size, LM_GRAN, LM_TAG_IMAGE, true);
        if (!base) { free(f); return NULL; }
    }
    if (is_exe) {
        lm_set_dep(oh->dllchars & 0x100); // IMAGE_DLLCHARACTERISTICS_NX_COMPAT
        if (!(oh->dllchars & 0x100)) LOGD("pe: DEP off (exe isn't NX-compatible)");
    }
    lm_commit(base, size, PAGE_EXECUTE_READWRITE);
    uint8_t *img = G2H(base);
    memcpy(img, f, oh->headers_size < len ? oh->headers_size : len);
    SectHdr *sh = (SectHdr *)(f + pe + 24 + optsz);
    for (int i = 0; i < nsect; i++) {
        // Windows maps a section into its VirtualSize rounded up to SectionAlignment, and
        // copies SizeOfRawData file bytes into it (truncating only if raw_size exceeds that
        // extent), then zero-fills the remainder. Copying min(raw_size, vsize) instead drops
        // the file bytes between an unaligned VirtualSize and SizeOfRawData, so our image can
        // differ from Windows' for any PE where raw_size > vsize.
        uint32_t valign = oh->sect_align ? oh->sect_align : 0x1000;
        uint32_t vext = sh[i].vsize ? ((sh[i].vsize + valign - 1) & ~(valign - 1)) : sh[i].raw_size;
        uint32_t n = sh[i].raw_size < vext ? sh[i].raw_size : vext;
        if (sh[i].va + n > size) n = sh[i].va < size ? size - sh[i].va : 0; // never past the image
        if (sh[i].raw_ptr + n > len) n = sh[i].raw_ptr < len ? len - sh[i].raw_ptr : 0;
        if (sh[i].raw_ptr && n) memcpy(img + sh[i].va, f + sh[i].raw_ptr, n);
        LOGD("pe: %s section %-8.8s va=%08x size=%08x", vpath, sh[i].name, base + sh[i].va, sh[i].vsize);
    }
    free(f);
    oh = opt_of(base);
    // Keep everything writable/executable: games patch their own code.

    // relocations
    int32_t delta = (int32_t)(base - pref);
    if (delta && oh->dirs[DIR_RELOC].va) {
        uint8_t *p = img + oh->dirs[DIR_RELOC].va, *end = p + oh->dirs[DIR_RELOC].size;
        int nrel = 0;
        while (p + 8 <= end) {
            uint32_t page = *(uint32_t *)p, bsz = *(uint32_t *)(p + 4);
            if (bsz < 8) break;
            for (uint16_t *e = (uint16_t *)(p + 8); (uint8_t *)e < p + bsz; e++) {
                int type = *e >> 12;
                if (type == 3) { *(uint32_t *)(img + page + (*e & 0xfff)) += delta; nrel++; }
                else if (type != 0) LOGW("pe: %s: unsupported relocation type %d", vpath, type);
            }
            p += bsz;
        }
        LOGD("pe: %s relocated by %+d (%d fixups)", vpath, delta, nrel);
    }
    oh->image_base = base;

    Module *m = &mods[nmods++];
    memset(m, 0, sizeof *m);
    norm_name(vpath, m->name, sizeof m->name);
    if (is_exe) { char *d = strrchr(m->name, '.'); if (d && !strcmp(d, ".exe")) *d = 0; }
    snprintf(m->vpath, sizeof m->vpath, "%s", vpath);
    m->base = base;
    m->size = size;
    m->entry = oh->entry ? base + oh->entry : 0;
    m->is_exe = is_exe;
    m->exp_rva = oh->dirs[DIR_EXPORT].va;
    m->exp_size = oh->dirs[DIR_EXPORT].size;
    m->tls_index = -1;
    if (inert) {
        m->entry = 0;
        protect_sections(base);
        LOGI("pe: %s at 0x%x-0x%x (mapped as data)", vpath, base, base + size);
        return m;
    }
    if (oh->dirs[DIR_TLS].va) {
        TlsDir *td = (TlsDir *)(img + oh->dirs[DIR_TLS].va);
        m->tls_index = ntls++;
        m->tls_start = td->start;
        m->tls_end = td->end;
        m->tls_zero = td->zero;
        m->tls_callbacks = td->callbacks;
        if (td->index) *(uint32_t *)G2H(td->index) = m->tls_index;
        Thread *t = thread_current();
        if (t) tls_alloc_block(m, t->teb);
        LOGD("pe: %s uses static TLS (slot %d, %u bytes)", vpath, m->tls_index, td->end - td->start + td->zero);
    }

    // imports (may recursively load more DLLs)
    load_depth++;
    int nimp = 0;
    if (oh->dirs[DIR_IMPORT].va) {
        for (ImportDesc *d = (ImportDesc *)(img + oh->dirs[DIR_IMPORT].va); d->name; d++) {
            char dll[128];
            snprintf(dll, sizeof dll, "%s", (const char *)img + d->name);
            uint32_t *ilt = (uint32_t *)(img + (d->oft ? d->oft : d->ft));
            uint32_t *iat = (uint32_t *)(img + d->ft);
            for (int k = 0; ilt[k]; k++) {
                if (ilt[k] & 0x80000000) iat[k] = resolve_import(dll, NULL, ilt[k] & 0xffff);
                else iat[k] = resolve_import(dll, (const char *)img + ilt[k] + 2, 0);
                nimp++;
            }
        }
    }
    load_depth--;
    protect_sections(base);
    if (!is_exe && ninit < MAX_MODULES) init_queue[ninit++] = m; // after its dependencies
    LOGI("pe: %s at 0x%x-0x%x (%d imports)", vpath, base, base + size, nimp);
    return m;
}

// Run TLS callbacks and DllMain(DLL_PROCESS_ATTACH) for a freshly mapped DLL.
static bool attach_dll(Module *m) {
    Thread *t = thread_current();
    if (!t) return true;
    call_tls_callbacks(m, 1);
    if (!m->entry) return true;
    uint32_t args[3] = {m->base, 1 /*DLL_PROCESS_ATTACH*/, 0};
    LOGD("pe: DllMain(%s, PROCESS_ATTACH)", m->name);
    uint32_t ok = (uint32_t)call32(t->ctx, m->entry, 3, args);
    if (!ok) LOGW("pe: DllMain of %s returned FALSE", m->name);
    return ok != 0;
}

// Run DllMain/TLS callbacks for everything mapped since the last call, once
// the outermost load has finished binding.
static void run_pending_inits(void) {
    if (load_depth) return;
    // FIFO drained one at a time: DllMain may LoadLibrary and append more.
    while (init_head < ninit) {
        Module *m = init_queue[init_head++];
        if (m && !attach_dll(m)) LOGW("pe: %s failed to initialize", m->vpath);
    }
    init_head = ninit = 0;
}

bool pe_load_exe(const char *hostpath, const char *vpath, PeImage *out) {
    lock();
    // Fill g_exe early so DLL searches during import resolution know the app dir.
    snprintf(g_exe.vpath, sizeof g_exe.vpath, "%s", vpath);
    snprintf(g_exe.path, sizeof g_exe.path, "%s", hostpath);
    Module *m = map_image(hostpath, vpath, true);
    if (m) {
        // DLL initializers may look at the exe (GetModuleHandle(NULL), entry point)
        out->base = g_exe.base = m->base;
        out->size = g_exe.size = m->size;
        out->entry = g_exe.entry = m->entry;
        run_pending_inits();
    }
    unlock();
    if (!m) return false;
    snprintf(out->path, sizeof out->path, "%s", hostpath);
    snprintf(out->vpath, sizeof out->vpath, "%s", vpath);
    call_tls_callbacks(m, 1);
    return true;
}

// Windows "KnownDLLs": always the system copy, never one from the game folder.
static bool known_dll(const char *norm) {
    static const char *k[] = {"kernel32", "kernelbase", "ntdll", "user32", "gdi32", "advapi32", "ole32",
                              "oleaut32", "shell32", "shlwapi", "msvcrt", "ws2_32", "imm32", "comdlg32",
                              "rpcrt4", "sechost", "combase", "setupapi"};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcmp(k[i], norm)) return true;
    return false;
}
// Game-shipped runtime DLLs we deliberately replace (unless overridden).
// Built-ins that replace the game's own copy: steam_api and steamclient bridge
// to the Linux Steam client (the Windows ones can't find a Windows Steam).
static bool prefer_builtin(const char *norm) {
    return !strcmp(norm, "eax") || !strcmp(norm, "vorbisfile") || !strcmp(norm, "steam_api") ||
           !strcmp(norm, "steamclient");
}

static bool is_system_path(const char *name) {
    if (!strchr(name, '\\') && !strchr(name, '/')) return false;
    char full[1024];
    vfs_full_path(name, full, sizeof full);
    return !strncasecmp(full, "C:\\Windows\\", 11);
}

static Module *find_mod(const char *norm, int want_builtin /* 1 builtin, 0 native, -1 any (native first) */) {
    Module *b = NULL;
    for (int i = 0; i < nmods; i++) {
        if (mods[i].is_exe || strcmp(mods[i].name, norm)) continue;
        if (mods[i].builtin) { if (!b) b = &mods[i]; }
        else if (want_builtin != 1) return &mods[i];
    }
    return want_builtin == 0 ? NULL : b;
}

// LoadLibrary / GetModuleHandle, following Windows' rules closely enough for
// proxy DLLs (ASI loaders, wrappers): a bare name prefers a copy in the game
// folder, while a path into C:\Windows (how proxies reach the "real" DLL) or
// a KnownDLL always gets our builtin implementation.
uint32_t module_get(const char *namein, bool load) {
    if (!namein) return g_exe.base;
    char norm[64];
    norm_name(namein, norm, sizeof norm);
    bool sys = is_system_path(namein);
    bool has_path = strchr(namein, '\\') || strchr(namein, '/') || (namein[0] && namein[1] == ':');
    lock();
    Module *m = NULL;
    if (sys || known_dll(norm)) {
        m = find_mod(norm, 1);
    } else if (has_path) {
        char full[1024];
        vfs_full_path(namein, full, sizeof full);
        for (int i = 0; i < nmods && !m; i++)
            if (!mods[i].builtin && !strcasecmp(mods[i].vpath, full)) m = &mods[i];
        if (!m) m = find_mod(norm, 0); // same DLL already loaded under a bare name
    } else {
        m = find_mod(norm, -1);
    }
    if (!m) { // the exe by name ("gta_sa.exe" / "gta_sa")
        char nn[64];
        snprintf(nn, sizeof nn, "%s", norm);
        char *d = strrchr(nn, '.');
        if (d && !strcmp(d, ".exe")) *d = 0;
        for (int i = 0; i < nmods && !m; i++)
            if (mods[i].is_exe && !strcmp(mods[i].name, nn)) m = &mods[i];
    }
    if (m) { unlock(); return m->base; }

    static const char *core[] = {"kernel32", "ntdll", "user32", "gdi32", "advapi32", "ole32",
                                 "winmm", "ws2_32", "vorbisfile", "eax"};
    bool is_core = false;
    for (unsigned i = 0; i < sizeof core / sizeof *core; i++)
        if (!strcmp(core[i], norm)) is_core = true;
    if (!load && !is_core) { unlock(); return 0; }

    char ov = override_for(norm);
    bool have_builtin = dll_has_impl(norm);
    char host[1024], vpath[512];
    bool want_native = false;
    if (load && !sys && !known_dll(norm) && ov != 'b' && nmods < MAX_MODULES &&
        find_dll_file(namein, host, sizeof host, vpath, sizeof vpath)) {
        want_native = ov == 'n' || !have_builtin || !prefer_builtin(norm);
        // GTA:SA's classic ASI loader replaces vorbisFile.dll and keeps the real
        // one as vorbisHooked.dll: use it so mods (SilentPatch etc.) load.
        if (!want_native && !strcmp(norm, "vorbisfile")) {
            char h2[1024], v2[512];
            if (find_dll_file("vorbishooked.dll", h2, sizeof h2, v2, sizeof v2)) {
                LOGI("pe: vorbisFile.dll is an ASI loader (vorbisHooked.dll present); loading it");
                want_native = true;
            }
        }
        if (want_native && have_builtin) LOGI("pe: using %s from the game folder instead of the builtin one", vpath);
    } else if (load && !sys && !known_dll(norm) && !have_builtin && nmods < MAX_MODULES &&
               find_dll_file_ex(namein, host, sizeof host, vpath, sizeof vpath, true)) {
        LOGI("pe: loading %s", vpath);
        want_native = true;
    }
    if (want_native) {
        Module *nm = map_image(host, vpath, false);
        if (nm) {
            uint32_t base = nm->base;
            run_pending_inits();
            unlock();
            return base;
        }
    }
    uint32_t r = 0;
    if (have_builtin && (!has_path || sys || !load || known_dll(norm) || !find_mod(norm, 0))) {
        Module *b = find_mod(norm, 1);
        char gh[1024], gv[512];
        if (!b && !strcmp(norm, "steamclient") &&
            find_dll_file(VFS_STEAM_VDIR "\\steamclient.dll", gh, sizeof gh, gv, sizeof gv)) {
            // Like Proton: the genuine steamclient.dll provides the module (its
            // address range, file and headers), our builtin provides the code.
            b = map_image_ex(gh, gv, false, true);
            if (b) {
                snprintf(b->name, sizeof b->name, "steamclient");
                b->builtin = true;
            }
        }
        if (!b) {
            b = add_builtin(norm);
            // loaded by a full path outside System32 (steamclient.dll from the
            // registry): report that path, as the real DLL's would be
            if (b && has_path && !sys) vfs_full_path(namein, b->vpath, sizeof b->vpath);
        }
        r = b ? b->base : 0;
    }
    unlock();
    return r;
}

// ---------------------------------------------------------------- resources
static int res_name_match(uint8_t *rsrc, ResEnt *e, uint32_t want) {
    if (want < 0x10000) return !(e->name & 0x80000000) && (e->name & 0xffff) == want;
    if (!(e->name & 0x80000000)) return 0;
    uint16_t *s = (uint16_t *)(rsrc + (e->name & 0x7fffffff));
    const char *w = G2H(want);
    if (w[0] == '#') return 0;
    size_t len = s[0];
    if (strlen(w) != len) return 0;
    for (size_t i = 0; i < len; i++)
        if (toupper(s[1 + i]) != toupper((unsigned char)w[i])) return 0;
    return 1;
}

static ResEnt *res_find(uint8_t *rsrc, uint32_t diroff, uint32_t want) {
    ResDir *d = (ResDir *)(rsrc + diroff);
    ResEnt *e = (ResEnt *)(d + 1);
    // 0xFFFFFFFF = first entry (any language); ids < 0x10000; else a "#123" or name string
    if (want != 0xFFFFFFFF && want >= 0x10000 && ((const char *)G2H(want))[0] == '#')
        want = atoi((const char *)G2H(want) + 1);
    for (int i = 0; i < d->nnamed + d->nid; i++)
        if (want == 0xFFFFFFFF || res_name_match(rsrc, &e[i], want)) return &e[i];
    return NULL;
}

// A resource of a PE file on disk (first name and language for `type`), copied
// into a malloc'd buffer. For GetFileVersionInfo on files that aren't loaded.
void *pe_file_resource(const char *hostpath, uint32_t type, uint32_t *size) {
    FILE *f = fopen(hostpath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = n > 0x200 ? malloc(n) : NULL;
    bool ok = file && fread(file, 1, n, f) == (size_t)n;
    fclose(f);
    void *out = NULL;
    uint32_t pe = ok ? *(uint32_t *)(file + 0x3c) : 0;
    if (ok && file[0] == 'M' && file[1] == 'Z' && pe + 24 + sizeof(OptHdr32) < (uint32_t)n && !memcmp(file + pe, "PE\0\0", 4)) {
        uint16_t nsec = *(uint16_t *)(file + pe + 6), ohsz = *(uint16_t *)(file + pe + 20);
        OptHdr32 *oh = (OptHdr32 *)(file + pe + 24);
        SectHdr *sh = (SectHdr *)(file + pe + 24 + ohsz);
        uint32_t rva = oh->dirs[2].va, rsz = oh->dirs[2].size;
        // lay the resource section out at its RVA-relative offsets
        uint8_t *rsrc = NULL;
        for (int i = 0; i < nsec && !rsrc && rva; i++)
            if (rva >= sh[i].va && rva < sh[i].va + (sh[i].vsize > sh[i].raw_size ? sh[i].vsize : sh[i].raw_size)) {
                uint32_t span = (sh[i].vsize > sh[i].raw_size ? sh[i].vsize : sh[i].raw_size) - (rva - sh[i].va);
                if (span < rsz) span = rsz;
                rsrc = calloc(1, span);
                uint32_t from = sh[i].raw_ptr + (rva - sh[i].va), avail = sh[i].raw_size - (rva - sh[i].va);
                if (from + avail <= (uint32_t)n) memcpy(rsrc, file + from, avail < span ? avail : span);
                ResEnt *t = res_find(rsrc, 0, type);
                ResEnt *nm = t && (t->off & 0x80000000) ? res_find(rsrc, t->off & 0x7fffffff, 0xFFFFFFFF) : NULL;
                ResEnt *l = nm && (nm->off & 0x80000000) ? res_find(rsrc, nm->off & 0x7fffffff, 0xFFFFFFFF) : NULL;
                if (l && !(l->off & 0x80000000) && l->off + sizeof(ResData) <= span) {
                    ResData *rd = (ResData *)(rsrc + l->off);
                    if (rd->data >= rva && rd->data - rva + rd->size <= span) {
                        out = malloc(rd->size);
                        memcpy(out, rsrc + (rd->data - rva), rd->size);
                        *size = rd->size;
                    }
                }
                free(rsrc);
            }
    }
    free(file);
    return out;
}

// Address range of a section of a loaded image (0 if absent).
uint32_t pe_section(uint32_t base, const char *name, uint32_t *size) {
    uint8_t *img = G2H(base);
    uint32_t pe = *(uint32_t *)(img + 0x3c);
    uint16_t nsec = *(uint16_t *)(img + pe + 6), ohsz = *(uint16_t *)(img + pe + 20);
    SectHdr *sh = (SectHdr *)(img + pe + 24 + ohsz);
    for (int i = 0; i < nsec; i++)
        if (!strncmp(sh[i].name, name, 8)) {
            if (size) *size = sh[i].vsize > sh[i].raw_size ? sh[i].vsize : sh[i].raw_size;
            return base + sh[i].va;
        }
    return 0;
}

uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t *size) {
    uint8_t *img = G2H(base);
    uint32_t pe = *(uint32_t *)(img + 0x3c);
    OptHdr32 *oh = (OptHdr32 *)(img + pe + 24);
    if (!oh->dirs[2].va) return 0;
    uint8_t *rsrc = img + oh->dirs[2].va;
    ResEnt *t = res_find(rsrc, 0, type);
    if (!t || !(t->off & 0x80000000)) return 0;
    ResEnt *n = res_find(rsrc, t->off & 0x7fffffff, name);
    if (!n || !(n->off & 0x80000000)) return 0;
    ResEnt *l = res_find(rsrc, n->off & 0x7fffffff, 0xFFFFFFFF); // first language
    if (!l || (l->off & 0x80000000)) return 0;
    ResData *rd = (ResData *)(rsrc + l->off);
    if (size) *size = rd->size;
    return base + rd->data;
}

