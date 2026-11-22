// kernel32: files, directories, file mappings, drives
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../vfs.h"
#include "win32.h"

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_ATTRIBUTE_READONLY 0x01
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

static Object *std_objs[3];

static Object *file_obj(uint32_t h) {
    if (h == 0xFFFFFFF6u) return std_objs[0]; // STD_INPUT_HANDLE used directly
    if (h == 0xFFFFFFF5u) return std_objs[1];
    if (h == 0xFFFFFFF4u) return std_objs[2];
    return handle_get(h, OBJ_FILE);
}

static uint32_t std_handle[3];
API(kernel32, GetStdHandle, 1) {
    int i = (int)(0xFFFFFFF6u - ARG(0)); // -10 -> 0, -11 -> 1, -12 -> 2
    if (i < 0 || i > 2) return INVALID_HANDLE_VALUE;
    if (!std_handle[i]) {
        Object *o = obj_new(OBJ_FILE);
        o->file.fd = i;
        o->file.is_console = 1;
        std_objs[i] = o;
        o->refs++;
        std_handle[i] = handle_alloc(o);
    }
    return std_handle[i];
}
API(kernel32, SetStdHandle, 2) { return TRUE; }
API(kernel32, SetHandleCount, 1) { return ARG(0); }

static uint32_t create_file(const char *name, uint32_t access, uint32_t disp, uint32_t flags) {
    char host[PATH_MAX];
    if (!strncmp(name, "\\\\.\\", 4) || !strcasecmp(name, "CONOUT$") || !strcasecmp(name, "CONIN$")) {
        LOGD("CreateFile(%s): device not supported", name);
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (!vfs_to_host(name, host, sizeof host)) {
        set_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    int of = 0;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) of = O_RDWR;
    else if (access & (GENERIC_WRITE | 0x2 /*FILE_WRITE_DATA*/ | 0x4)) of = O_WRONLY;
    else of = O_RDONLY;
    if (access & 0x10000000u) of = O_RDWR; // GENERIC_ALL
    struct stat st;
    bool existed = stat(host, &st) == 0;
    switch (disp) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    }
    if (existed && S_ISDIR(st.st_mode)) of = O_RDONLY | O_DIRECTORY;
    if ((of & O_CREAT) && (of & O_ACCMODE) == O_RDONLY) of = (of & ~O_ACCMODE) | O_RDWR;
    int fd = open(host, of | O_CLOEXEC, 0644);
    if (fd < 0) {
        uint32_t e = win32_errno(errno);
        if (errno == EEXIST) e = ERROR_FILE_EXISTS;
        set_last_error(e);
        LOGD("CreateFile(%s -> %s, disp %u) failed: %s", name, host, disp, strerror(errno));
        return INVALID_HANDLE_VALUE;
    }
    if (!existed) vfs_note_created(host, false);
    Object *o = obj_new(OBJ_FILE);
    o->file.fd = fd;
    o->file.path = strdup(host);
    o->file.delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    uint32_t h = handle_alloc(o);
    set_last_error((existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0);
    LOGD("CreateFile(%s) -> %s = 0x%x", name, host, h);
    return h;
}

API(kernel32, CreateFileA, 7) { return create_file(STR(0), ARG(1), ARG(4), ARG(5)); }
API(kernel32, CreateFileW, 7) {
    char n[PATH_MAX];
    w2a(WSTR(0), n, sizeof n);
    return create_file(n, ARG(1), ARG(4), ARG(5));
}

API(kernel32, CloseHandle, 1) {
    Object *o = handle_get(ARG(0), 0);
    if (o && o->type == OBJ_FILE && o->file.delete_on_close) vfs_note_deleted(o->file.path);
    if (!handle_close(ARG(0))) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

// OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent
static void complete_overlapped(uint32_t *ov, uint32_t n) {
    ov[0] = 0;
    ov[1] = n;
    if (ov[4]) {
        Object *ev = handle_get(ov[4], OBJ_EVENT);
        if (ev) obj_signal_event(ev, 1);
    }
}

API(kernel32, ReadFile, 5) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    uint32_t *ov = PTR(4);
    ssize_t r;
    if (ov) r = pread(o->file.fd, PTR(1), ARG(2), (off_t)ov[2] | ((off_t)ov[3] << 32));
    else r = read(o->file.fd, PTR(1), ARG(2));
    if (r < 0) {
        set_last_error(win32_errno(errno));
        if (ARG(3)) *(uint32_t *)PTR(3) = 0;
        return FALSE;
    }
    if (ARG(3)) *(uint32_t *)PTR(3) = (uint32_t)r;
    if (ov) {
        complete_overlapped(ov, (uint32_t)r);
        if (r == 0 && ARG(2)) { set_last_error(ERROR_HANDLE_EOF); return FALSE; }
    }
    return TRUE;
}

API(kernel32, WriteFile, 5) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    uint32_t *ov = PTR(4);
    ssize_t r;
    if (ov) r = pwrite(o->file.fd, PTR(1), ARG(2), (off_t)ov[2] | ((off_t)ov[3] << 32));
    else r = write(o->file.fd, PTR(1), ARG(2));
    if (r < 0) {
        set_last_error(win32_errno(errno));
        return FALSE;
    }
    if (ARG(3)) *(uint32_t *)PTR(3) = (uint32_t)r;
    if (ov) complete_overlapped(ov, (uint32_t)r);
    return TRUE;
}

API(kernel32, GetOverlappedResult, 4) {
    uint32_t *ov = PTR(1);
    if (ARG(2)) *(uint32_t *)PTR(2) = ov[1];
    return TRUE;
}

API(kernel32, SetFilePointer, 4) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    int32_t *hi = PTR(2);
    off_t off = hi ? (off_t)(((uint64_t)(uint32_t)*hi << 32) | ARG(1)) : (off_t)(int32_t)ARG(1);
    static const int wh[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    off_t r = lseek(o->file.fd, off, wh[ARG(3) % 3]);
    if (r < 0) { set_last_error(win32_errno(errno)); return 0xFFFFFFFF; }
    if (hi) *hi = (int32_t)(r >> 32);
    set_last_error(0);
    return (uint32_t)r;
}
API(kernel32, SetFilePointerEx, 5) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    off_t off = (off_t)((uint64_t)ARG(1) | ((uint64_t)ARG(2) << 32));
    static const int wh[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    off_t r = lseek(o->file.fd, off, wh[ARG(4) % 3]);
    if (r < 0) { set_last_error(win32_errno(errno)); return FALSE; }
    if (ARG(3)) *(int64_t *)PTR(3) = r;
    return TRUE;
}
API(kernel32, GetFileSize, 2) {
    Object *o = file_obj(ARG(0));
    struct stat st;
    if (!o || fstat(o->file.fd, &st)) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    if (ARG(1)) *(uint32_t *)PTR(1) = (uint32_t)((uint64_t)st.st_size >> 32);
    return (uint32_t)st.st_size;
}
API(kernel32, GetFileSizeEx, 2) {
    Object *o = file_obj(ARG(0));
    struct stat st;
    if (!o || fstat(o->file.fd, &st)) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *(int64_t *)PTR(1) = st.st_size;
    return TRUE;
}
API(kernel32, GetFileType, 1) {
    Object *o = file_obj(ARG(0));
    if (!o) return 0;
    return o->file.is_console ? 2 : 1; // FILE_TYPE_CHAR / FILE_TYPE_DISK
}
API(kernel32, FlushFileBuffers, 1) { return file_obj(ARG(0)) != NULL; }
API(kernel32, SetEndOfFile, 1) {
    Object *o = file_obj(ARG(0));
    if (!o) return FALSE;
    off_t cur = lseek(o->file.fd, 0, SEEK_CUR);
    return ftruncate(o->file.fd, cur) == 0;
}

API(kernel32, DeleteFileA, 1) {
    char host[PATH_MAX];
    if (!vfs_to_host(STR(0), host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (unlink(host)) { set_last_error(win32_errno(errno)); return FALSE; }
    vfs_note_deleted(host);
    return TRUE;
}
API(kernel32, MoveFileA, 2) {
    char a1[PATH_MAX], a2[PATH_MAX];
    if (!vfs_to_host(STR(0), a1, sizeof a1) || !vfs_to_host(STR(1), a2, sizeof a2)) return FALSE;
    if (rename(a1, a2)) { set_last_error(win32_errno(errno)); return FALSE; }
    vfs_note_deleted(a1);
    vfs_note_created(a2, false);
    return TRUE;
}
API(kernel32, CopyFileA, 3) {
    char a1[PATH_MAX], a2[PATH_MAX];
    if (!vfs_to_host(STR(0), a1, sizeof a1) || !vfs_to_host(STR(1), a2, sizeof a2)) {
        set_last_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (ARG(2) && access(a2, F_OK) == 0) { set_last_error(ERROR_FILE_EXISTS); return FALSE; }
    FILE *in = fopen(a1, "rb");
    if (!in) { set_last_error(win32_errno(errno)); return FALSE; }
    FILE *out = fopen(a2, "wb");
    if (!out) { set_last_error(win32_errno(errno)); fclose(in); return FALSE; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    fclose(in);
    ok &= fclose(out) == 0;
    vfs_note_created(a2, false);
    return ok;
}
// GetTempFileNameA(dir, prefix, unique, out)
API(kernel32, GetTempFileNameA, 4) {
    static unsigned counter;
    unsigned u = ARG(2) ? ARG(2) : (unsigned)((getpid() << 4) + ++counter) & 0xFFFF;
    char name[PATH_MAX];
    snprintf(name, sizeof name, "%s%s%.3s%04X.tmp", STR(0),
             STR(0)[0] && STR(0)[strlen(STR(0)) - 1] != '\\' ? "\\" : "", ARG(1) ? STR(1) : "", u);
    strcpy(PTR(3), name);
    if (!ARG(2)) {
        char host[PATH_MAX];
        if (vfs_to_host(name, host, sizeof host)) {
            FILE *f = fopen(host, "wb");
            if (f) { fclose(f); vfs_note_created(host, false); }
        }
    }
    return u;
}
API(kernel32, CreateDirectoryA, 2) {
    char host[PATH_MAX];
    if (!vfs_to_host(STR(0), host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (mkdir(host, 0755)) {
        set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : win32_errno(errno));
        return FALSE;
    }
    vfs_note_created(host, true);
    return TRUE;
}
API(kernel32, RemoveDirectoryA, 1) {
    char host[PATH_MAX];
    if (!vfs_to_host(STR(0), host, sizeof host)) return FALSE;
    if (rmdir(host)) { set_last_error(win32_errno(errno)); return FALSE; }
    vfs_note_deleted(host);
    return TRUE;
}

static uint32_t attrs_of(const struct stat *st) {
    uint32_t a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

uint32_t wineless_file_exists(const char *path) {
    char host[PATH_MAX];
    struct stat st;
    return vfs_to_host(path, host, sizeof host) && !stat(host, &st);
}

API(kernel32, GetFileAttributesA, 1) {
    char host[PATH_MAX];
    struct stat st;
    if (!vfs_to_host(STR(0), host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    if (stat(host, &st)) { set_last_error(win32_errno(errno)); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st);
}
API(kernel32, SetFileAttributesA, 2) { return TRUE; }

// ---------------------------------------------------------------- find
static bool wild_match(const char *p, const char *s) {
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return true;
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return true;
            for (; *s; s++)
                if (wild_match(p, s)) return true;
            return wild_match(p, s);
        }
        if (!*s) return *p == '.' && !p[1]; // "name." matches "name"
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        p++;
        s++;
    }
    return !*s;
}

static void fill_find_data(uint8_t *fd, const FindEntry *e) {
    memset(fd, 0, 320);
    *(uint32_t *)fd = e->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    uint64_t ft = ((uint64_t)e->mtime + 11644473600ull) * 10000000ull;
    memcpy(fd + 4, &ft, 8);
    memcpy(fd + 12, &ft, 8);
    memcpy(fd + 20, &ft, 8);
    *(uint32_t *)(fd + 28) = (uint32_t)(e->size >> 32);
    *(uint32_t *)(fd + 32) = (uint32_t)e->size;
    snprintf((char *)fd + 44, 260, "%s", e->name);
}

uint32_t wineless_find_first(const char *pattern, void *data) {
    char pat[PATH_MAX], dir[PATH_MAX], host[PATH_MAX];
    snprintf(pat, sizeof pat, "%s", pattern);
    for (char *p = pat; *p; p++)
        if (*p == '/') *p = '\\';
    char *bs = strrchr(pat, '\\');
    const char *mask;
    if (bs) { *bs = 0; snprintf(dir, sizeof dir, "%s", *pat ? pat : "\\"); mask = bs + 1; }
    else { snprintf(dir, sizeof dir, "."); mask = pat; }
    if (!vfs_to_host(dir, host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    DIR *d = opendir(host);
    if (!d) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    Object *o = obj_new(OBJ_FIND);
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!wild_match(mask, de->d_name)) continue;
        if (o->find.n == cap) {
            cap = cap ? cap * 2 : 32;
            o->find.e = realloc(o->find.e, cap * sizeof *o->find.e);
        }
        FindEntry *e = &o->find.e[o->find.n++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", host, de->d_name);
        struct stat st;
        if (!stat(full, &st)) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        } else {
            e->is_dir = 0; e->size = 0; e->mtime = 0;
        }
    }
    closedir(d);
    if (!o->find.n) {
        free(o->find.e);
        free(o);
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    fill_find_data(data, &o->find.e[0]);
    o->find.pos = 1;
    return handle_alloc(o);
}
API(kernel32, FindFirstFileA, 2) { return wineless_find_first(STR(0), PTR(1)); }
API(kernel32, FindNextFileA, 2) {
    Object *o = handle_get(ARG(0), OBJ_FIND);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->find.pos >= o->find.n) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    fill_find_data(PTR(1), &o->find.e[o->find.pos++]);
    return TRUE;
}
API(kernel32, FindClose, 1) { return handle_close(ARG(0)); }

// ---------------------------------------------------------------- paths
API(kernel32, GetFullPathNameA, 4) {
    char full[1024];
    vfs_full_path(STR(0), full, sizeof full);
    uint32_t n = strlen(full);
    if (ARG(1) <= n) return n + 1;
    char *buf = PTR(2);
    memcpy(buf, full, n + 1);
    if (ARG(3)) {
        char *bs = strrchr(buf, '\\');
        *(uint32_t *)PTR(3) = bs && bs[1] ? H2G(bs + 1) : 0;
    }
    return n;
}
API(kernel32, GetCurrentDirectoryA, 2) { return copy_out_str(PTR(1), ARG(0), vfs_get_cwd()); }
API(kernel32, SetCurrentDirectoryA, 1) {
    if (!vfs_set_cwd(STR(0))) {
        set_last_error(ERROR_PATH_NOT_FOUND);
        LOGD("SetCurrentDirectory(%s) failed", STR(0));
        return FALSE;
    }
    return TRUE;
}
API(kernel32, GetTempPathA, 2) {
    char t[512];
    snprintf(t, sizeof t, "%s\\", VFS_TEMP_VDIR);
    return copy_out_str(PTR(1), ARG(0), t);
}

// ---------------------------------------------------------------- drives
API(kernel32, GetDriveTypeA, 1) {
    const char *r = ARG(0) ? STR(0) : "C:\\";
    if ((r[0] == 'C' || r[0] == 'c') && r[1] == ':') return 3; // DRIVE_FIXED
    return 1;                                                   // DRIVE_NO_ROOT_DIR
}
API(kernel32, GetLogicalDriveStringsA, 2) {
    static const char s[] = "C:\\\0";
    if (ARG(0) < sizeof s) return sizeof s;
    memcpy(PTR(1), s, sizeof s);
    return sizeof s - 1;
}
API(kernel32, GetLogicalDrives, 0) { return 1u << 2; }
API(kernel32, GetVolumeInformationA, 8) {
    if (ARG(1)) copy_out_str(PTR(1), ARG(2), "GTASA");
    if (ARG(3)) *(uint32_t *)PTR(3) = 0x5A5A1234;
    if (ARG(4)) *(uint32_t *)PTR(4) = 255;
    if (ARG(5)) *(uint32_t *)PTR(5) = 0x3;
    if (ARG(6)) copy_out_str(PTR(6), ARG(7), "NTFS");
    return TRUE;
}
API(kernel32, GetDiskFreeSpaceA, 5) {
    struct statvfs sv;
    uint64_t freeb = 0x40000000ull, total = 0x80000000ull;
    if (!statvfs(vfs_user_dir(), &sv)) { freeb = (uint64_t)sv.f_bavail * sv.f_frsize; total = (uint64_t)sv.f_blocks * sv.f_frsize; }
    // report 4K clusters, capped so 32-bit math in the game cannot overflow
    uint64_t fc = freeb / 4096, tc = total / 4096;
    if (fc > 0x7FFFF) fc = 0x7FFFF;
    if (tc > 0x7FFFF) tc = 0x7FFFF;
    if (ARG(1)) *(uint32_t *)PTR(1) = 8;
    if (ARG(2)) *(uint32_t *)PTR(2) = 512;
    if (ARG(3)) *(uint32_t *)PTR(3) = (uint32_t)fc;
    if (ARG(4)) *(uint32_t *)PTR(4) = (uint32_t)tc;
    return TRUE;
}

// ---------------------------------------------------------------- file mappings
API(kernel32, CreateFileMappingA, 6) {
    Object *f = ARG(0) == INVALID_HANDLE_VALUE ? NULL : file_obj(ARG(0));
    Object *o = obj_new(OBJ_MAPPING);
    o->mapping.fd = f ? dup(f->file.fd) : -1;
    o->mapping.size = ARG(4);
    o->mapping.prot = ARG(2);
    if (ARG(5)) o->name = strdup(STR(5));
    return handle_alloc(o);
}
API(kernel32, MapViewOfFile, 5) {
    Object *o = handle_get(ARG(0), OBJ_MAPPING);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return 0; }
    uint64_t off = ((uint64_t)ARG(2) << 32) | ARG(3);
    uint32_t size = ARG(4);
    if (!size) {
        struct stat st;
        size = o->mapping.fd >= 0 && !fstat(o->mapping.fd, &st) ? (uint32_t)(st.st_size - off) : o->mapping.size;
    }
    uint32_t va = lm_reserve(0, size, LM_GRAN, LM_TAG_VIRTUAL, false);
    if (!va) return 0;
    if (o->mapping.fd >= 0) {
        int prot = (ARG(1) & 2) ? PROT_READ | PROT_WRITE : PROT_READ;
        void *p = mmap(G2H(va), size, prot, MAP_SHARED | MAP_FIXED, o->mapping.fd, (off_t)off);
        if (p == MAP_FAILED) { lm_release(va); return 0; }
        lm_commit(va, size, (ARG(1) & 2) ? PAGE_READWRITE : PAGE_READONLY);
        // lm_commit re-protects; mapping stays file-backed
    } else {
        lm_commit(va, size, PAGE_READWRITE);
    }
    return va;
}
API(kernel32, UnmapViewOfFile, 1) { return lm_release(ARG(0)); }

// ---------------------------------------------------------------- wide-character variants
// Paths are converted to ANSI (Latin-1); everything the games use is ASCII.
static void fill_find_data_w(uint8_t *fw, const FindEntry *e) {
    uint8_t a[320];
    fill_find_data(a, e);
    memset(fw, 0, 592);
    memcpy(fw, a, 44);                              // attributes, times, sizes, reserved
    a2w(e->name, (uint16_t *)(fw + 44), 260);       // cFileName
}
API(kernel32, FindFirstFileW, 2) {
    char pat[PATH_MAX];
    w2a(WSTR(0), pat, sizeof pat);
    uint8_t a[320];
    uint32_t h = wineless_find_first(pat, a);
    if (h != INVALID_HANDLE_VALUE) {
        Object *o = handle_get(h, OBJ_FIND);
        fill_find_data_w(PTR(1), &o->find.e[0]);
    }
    return h;
}
API(kernel32, FindFirstFileExW, 6) {
    char pat[PATH_MAX];
    w2a(WSTR(0), pat, sizeof pat);
    uint8_t a[320];
    uint32_t h = wineless_find_first(pat, a);
    if (h != INVALID_HANDLE_VALUE) {
        Object *o = handle_get(h, OBJ_FIND);
        fill_find_data_w(PTR(2), &o->find.e[0]);
    }
    return h;
}
API(kernel32, FindNextFileW, 2) {
    Object *o = handle_get(ARG(0), OBJ_FIND);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->find.pos >= o->find.n) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    fill_find_data_w(PTR(1), &o->find.e[o->find.pos++]);
    return TRUE;
}
API(kernel32, GetFileAttributesW, 1) {
    char n[PATH_MAX], host[PATH_MAX];
    struct stat st;
    w2a(WSTR(0), n, sizeof n);
    if (!vfs_to_host(n, host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    if (stat(host, &st)) { set_last_error(win32_errno(errno)); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st);
}
API(kernel32, GetCurrentDirectoryW, 2) {
    const char *c = vfs_get_cwd();
    uint32_t n = strlen(c);
    if (ARG(0) <= n) return n + 1;
    a2w(c, PTR(1), n + 1);
    return n;
}
API(kernel32, SetCurrentDirectoryW, 1) {
    char n[PATH_MAX];
    w2a(WSTR(0), n, sizeof n);
    if (!vfs_set_cwd(n)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    return TRUE;
}
API(kernel32, GetSystemDirectoryW, 2) {
    static const char d[] = "C:\\Windows\\System32";
    if (ARG(1) <= sizeof d - 1) return sizeof d;
    a2w(d, PTR(0), sizeof d);
    return sizeof d - 1;
}
API(kernel32, GetWindowsDirectoryW, 2) {
    static const char d[] = "C:\\Windows";
    if (ARG(1) <= sizeof d - 1) return sizeof d;
    a2w(d, PTR(0), sizeof d);
    return sizeof d - 1;
}
API(kernel32, CreateDirectoryW, 2) {
    char n[PATH_MAX], host[PATH_MAX];
    w2a(WSTR(0), n, sizeof n);
    if (!vfs_to_host(n, host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (mkdir(host, 0755)) { set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : win32_errno(errno)); return FALSE; }
    vfs_note_created(host, true);
    return TRUE;
}
API(kernel32, DeleteFileW, 1) {
    char n[PATH_MAX], host[PATH_MAX];
    w2a(WSTR(0), n, sizeof n);
    if (!vfs_to_host(n, host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (unlink(host)) { set_last_error(win32_errno(errno)); return FALSE; }
    vfs_note_deleted(host);
    return TRUE;
}
API(kernel32, GetFullPathNameW, 4) {
    char n[PATH_MAX], full[1024];
    w2a(WSTR(0), n, sizeof n);
    vfs_full_path(n, full, sizeof full);
    uint32_t l = strlen(full);
    if (ARG(1) <= l) return l + 1;
    uint16_t *buf = PTR(2);
    a2w(full, buf, l + 1);
    if (ARG(3)) {
        uint16_t *bs = NULL;
        for (uint16_t *q = buf; *q; q++) if (*q == '\\') bs = q;
        *(uint32_t *)PTR(3) = bs && bs[1] ? H2G(bs + 1) : 0;
    }
    return l;
}

uint32_t k32_create_mapping(uint32_t *a) { return api_kernel32_CreateFileMappingA(a); }
