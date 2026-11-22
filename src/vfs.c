#include "vfs.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"

// ---------------------------------------------------------------- index
typedef struct Entry {
    struct Entry *next;
    char *lower;  // lowercase relative path, '/' separated
    char *actual; // real relative path
} Entry;

typedef struct {
    Entry **b;
    size_t nb, n;
} Index;

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void idx_grow(Index *ix) {
    size_t nb = ix->nb ? ix->nb * 2 : 4096;
    Entry **b = calloc(nb, sizeof *b);
    for (size_t i = 0; i < ix->nb; i++)
        for (Entry *e = ix->b[i], *nx; e; e = nx) {
            nx = e->next;
            size_t k = hash(e->lower) & (nb - 1);
            e->next = b[k];
            b[k] = e;
        }
    free(ix->b);
    ix->b = b;
    ix->nb = nb;
}

static Entry *idx_get(Index *ix, const char *lower) {
    if (!ix->nb) return NULL;
    for (Entry *e = ix->b[hash(lower) & (ix->nb - 1)]; e; e = e->next)
        if (!strcmp(e->lower, lower)) return e;
    return NULL;
}

static void idx_put(Index *ix, const char *actual) {
    char *lower = strdup(actual);
    for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);
    Entry *e = idx_get(ix, lower);
    if (e) {
        free(e->actual);
        e->actual = strdup(actual);
        free(lower);
        return;
    }
    if (ix->n + 1 > ix->nb / 2) idx_grow(ix);
    e = malloc(sizeof *e);
    e->lower = lower;
    e->actual = strdup(actual);
    size_t k = hash(lower) & (ix->nb - 1);
    e->next = ix->b[k];
    ix->b[k] = e;
    ix->n++;
}

static void idx_del(Index *ix, const char *lower) {
    if (!ix->nb) return;
    Entry **pp = &ix->b[hash(lower) & (ix->nb - 1)];
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->lower, lower)) {
            Entry *e = *pp;
            *pp = e->next;
            free(e->lower);
            free(e->actual);
            free(e);
            ix->n--;
            return;
        }
}

// ---------------------------------------------------------------- mounts
typedef struct {
    char vprefix[256]; // e.g. "c:\\gta san andreas" (lowercase)
    char root[PATH_MAX];
    bool indexed;
    Index ix;
} Mount;

static Mount mounts[6];
static Mount *game_mount, *user_mount;
static int nmounts;
static pthread_rwlock_t vfs_lock = PTHREAD_RWLOCK_INITIALIZER;
const char *g_user_subdir = "GTA San Andreas User Files";
static char cwd[512] = VFS_GAME_VDIR;

static void scan(Mount *m, const char *rel) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", m->root, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        idx_put(&m->ix, sub);
        int isdir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) { // follow symlinks
            struct stat st;
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s/%s", m->root, sub);
            isdir = !stat(full, &st) && S_ISDIR(st.st_mode);
        }
        if (isdir) scan(m, sub);
    }
    closedir(d);
}

static Mount *add_mount(const char *vprefix, const char *root, bool indexed) {
    Mount *m = &mounts[nmounts++];
    snprintf(m->vprefix, sizeof m->vprefix, "%s", vprefix);
    for (char *p = m->vprefix; *p; p++) *p = tolower((unsigned char)*p);
    snprintf(m->root, sizeof m->root, "%s", root);
    m->indexed = indexed;
    if (indexed) {
        scan(m, "");
        LOGI("vfs: %s -> %s (%zu entries indexed)", vprefix, root, m->ix.n);
    } else {
        LOGI("vfs: %s -> %s", vprefix, root);
    }
    return m;
}

static void mkdirs(const char *p) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
    mkdir(tmp, 0755);
}

void vfs_init(const char *game_dir, const char *user_dir, const char *data_dir) {
    char docs[PATH_MAX];
    snprintf(docs, sizeof docs, "%s", user_dir);
    char *slash = strrchr(docs, '/');
    if (slash && slash != docs) *slash = 0;
    mkdirs(user_dir);
    // Longest prefix first.
    char uv[256];
    snprintf(uv, sizeof uv, "%s\\%s", VFS_DOCS_VDIR, g_user_subdir);
    user_mount = add_mount(uv, user_dir, true);
    add_mount(VFS_DOCS_VDIR, docs, false);
    // AppData and ProgramData: indexed so case-insensitive lookups work
    char p[PATH_MAX];
    static const char *const sub[] = {"AppData/Roaming", "AppData/Local/Temp", "AppData/LocalLow", "ProgramData"};
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", data_dir, sub[i]);
        mkdirs(p);
    }
    snprintf(p, sizeof p, "%s/AppData", data_dir);
    add_mount(VFS_APPDATA_VDIR, p, true);
    snprintf(p, sizeof p, "%s/ProgramData", data_dir);
    add_mount(VFS_PROGRAMDATA_VDIR, p, true);
    // Steam's Windows client files (the genuine steamclient.dll that Steam's DRM
    // wrapper checks), as Proton provides them: from Steam's legacycompat folder.
    const char *home = getenv("HOME");
    char steam[PATH_MAX], real[PATH_MAX];
    snprintf(steam, sizeof steam, "%s/.steam/root/legacycompat", home ? home : "");
    if (realpath(steam, real)) add_mount(VFS_STEAM_VDIR, real, true);
    game_mount = add_mount(VFS_GAME_VDIR, game_dir, true);
}

const char *vfs_game_dir(void) { return game_mount->root; }
const char *vfs_user_dir(void) { return user_mount->root; }

// Build canonical absolute virtual path: backslashes, no "." / "..", no dup separators.
void vfs_full_path(const char *in, char *out, size_t outsz) {
    char buf[1024];
    if (isalpha((unsigned char)in[0]) && in[1] == ':')
        snprintf(buf, sizeof buf, "%s", in);
    else if (in[0] == '\\' || in[0] == '/')
        snprintf(buf, sizeof buf, "C:%s", in);
    else
        snprintf(buf, sizeof buf, "%s\\%s", cwd, in);
    for (char *p = buf; *p; p++)
        if (*p == '/') *p = '\\';
    // split into components after "X:"
    char *comps[128];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf + 2, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        // Win32 ignores trailing spaces and dots in path components
        // (gta.dat references "DATA\DEFAULT.IDE " with a trailing space).
        size_t tl = strlen(tok);
        while (tl && (tok[tl - 1] == ' ' || tok[tl - 1] == '.')) tok[--tl] = 0;
        if (!tl) continue;
        if (n < 128) comps[n++] = tok;
    }
    size_t o = snprintf(out, outsz, "%c:", toupper((unsigned char)buf[0]));
    if (!n) o += snprintf(out + o, outsz - o, "\\");
    for (int i = 0; i < n && o < outsz; i++) o += snprintf(out + o, outsz - o, "\\%s", comps[i]);
}

static Mount *find_mount(const char *full, const char **rest) {
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].vprefix);
        if (!strncasecmp(full, mounts[i].vprefix, l) && (full[l] == 0 || full[l] == '\\')) {
            *rest = full[l] ? full + l + 1 : full + l;
            return &mounts[i];
        }
    }
    return NULL;
}

// Resolve relative path (with '/') case-insensitively: longest indexed prefix.
static void resolve_rel(Mount *m, const char *rel, char *out, size_t outsz) {
    if (!m->indexed || !*rel) {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    char lower[1024];
    snprintf(lower, sizeof lower, "%s", rel);
    for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);
    // try full, then strip trailing components until a match is found
    size_t len = strlen(lower);
    for (;;) {
        Entry *e = idx_get(&m->ix, lower);
        if (e) {
            snprintf(out, outsz, "%s%s", e->actual, rel + len);
            return;
        }
        char *s = strrchr(lower, '/');
        if (!s) break;
        *s = 0;
        len = s - lower;
    }
    snprintf(out, outsz, "%s", rel);
}

bool vfs_to_host(const char *winpath, char *out, size_t outsz) {
    char full[1024];
    vfs_full_path(winpath, full, sizeof full);
    const char *rest;
    pthread_rwlock_rdlock(&vfs_lock);
    Mount *m = find_mount(full, &rest);
    if (!m) {
        pthread_rwlock_unlock(&vfs_lock);
        LOGD("vfs: '%s' is outside all mounts", winpath);
        return false;
    }
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", rest);
    for (char *p = rel; *p; p++)
        if (*p == '\\') *p = '/';
    char actual[1024];
    resolve_rel(m, rel, actual, sizeof actual);
    snprintf(out, outsz, "%s%s%s", m->root, *actual ? "/" : "", actual);
    pthread_rwlock_unlock(&vfs_lock);
    return true;
}

static Mount *mount_for_host(const char *hostpath, const char **rel) {
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].root);
        if (!strncmp(hostpath, mounts[i].root, l) && hostpath[l] == '/') {
            *rel = hostpath + l + 1;
            return &mounts[i];
        }
    }
    return NULL;
}

void vfs_note_created(const char *hostpath, bool is_dir) {
    const char *rel;
    pthread_rwlock_wrlock(&vfs_lock);
    Mount *m = mount_for_host(hostpath, &rel);
    if (m && m->indexed) {
        idx_put(&m->ix, rel);
        if (is_dir) scan(m, rel);
    }
    pthread_rwlock_unlock(&vfs_lock);
}

void vfs_note_deleted(const char *hostpath) {
    const char *rel;
    pthread_rwlock_wrlock(&vfs_lock);
    Mount *m = mount_for_host(hostpath, &rel);
    if (m && m->indexed) {
        char lower[1024];
        snprintf(lower, sizeof lower, "%s", rel);
        for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);
        idx_del(&m->ix, lower);
    }
    pthread_rwlock_unlock(&vfs_lock);
}

const char *vfs_get_cwd(void) { return cwd; }

bool vfs_set_cwd(const char *winpath) {
    char full[1024], host[PATH_MAX];
    vfs_full_path(winpath, full, sizeof full);
    if (!vfs_to_host(full, host, sizeof host)) return false;
    struct stat st;
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) return false;
    snprintf(cwd, sizeof cwd, "%s", full);
    size_t l = strlen(cwd);
    if (l > 3 && cwd[l - 1] == '\\') cwd[l - 1] = 0;
    return true;
}
