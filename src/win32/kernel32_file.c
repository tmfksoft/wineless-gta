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
    // Game dir served from an .asar: stream a read-only data file straight from
    // the archive (no extraction). Checked before vfs_to_host so big files don't
    // trigger the on-demand extraction that vfs_to_host does for other callers.
    // (Extracted exe/dll are real files and fall through to the normal path.)
    if (vfs_game_is_archive() && !(access & GENERIC_WRITE) &&
        disp != CREATE_NEW && disp != CREATE_ALWAYS) {
        uint64_t aoff, asize; int aisdir;
        if (vfs_archive_entry(name, &aoff, &asize, &aisdir) && !aisdir) {
            Object *o = obj_new(OBJ_FILE);
            o->file.fd = -1;
            o->file.is_arc = 1;
            o->file.arc_off = aoff;
            o->file.arc_size = asize;
            o->file.arc_pos = 0;
            o->file.path = strdup(name);
            set_last_error(0);
            LOGD("CreateFile(%s) -> asar @%llu size %llu", name,
                 (unsigned long long)aoff, (unsigned long long)asize);
            return handle_alloc(o);
        }
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
    if (o->file.is_arc) {
        uint64_t fpos = ov ? (((uint64_t)ov[3] << 32) | ov[2]) : o->file.arc_pos;
        uint64_t avail = fpos < o->file.arc_size ? o->file.arc_size - fpos : 0;
        uint32_t want = ARG(2) < avail ? ARG(2) : (uint32_t)avail;
        ssize_t r = want ? pread(vfs_game_archive_fd(), PTR(1), want, (off_t)(o->file.arc_off + fpos)) : 0;
        if (r < 0) { set_last_error(win32_errno(errno)); if (ARG(3)) *(uint32_t *)PTR(3) = 0; return FALSE; }
        if (!ov) o->file.arc_pos = fpos + (uint64_t)r;
        if (ARG(3)) *(uint32_t *)PTR(3) = (uint32_t)r;
        if (ov) complete_overlapped(ov, (uint32_t)r);
        return TRUE;
    }
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

static uint64_t arc_seek(Object *o, int64_t off, uint32_t whence) {
    int64_t base = whence % 3 == 0 ? 0 : whence % 3 == 1 ? (int64_t)o->file.arc_pos : (int64_t)o->file.arc_size;
    int64_t np = base + off;
    if (np < 0) np = 0;
    o->file.arc_pos = (uint64_t)np;
    return (uint64_t)np;
}

API(kernel32, SetFilePointer, 4) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    int32_t *hi = PTR(2);
    off_t off = hi ? (off_t)(((uint64_t)(uint32_t)*hi << 32) | ARG(1)) : (off_t)(int32_t)ARG(1);
    if (o->file.is_arc) {
        uint64_t np = arc_seek(o, off, ARG(3));
        if (hi) *hi = (int32_t)(np >> 32);
        set_last_error(0);
        return (uint32_t)np;
    }
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
    if (o->file.is_arc) {
        uint64_t np = arc_seek(o, off, ARG(4));
        if (ARG(3)) *(int64_t *)PTR(3) = (int64_t)np;
        return TRUE;
    }
    static const int wh[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    off_t r = lseek(o->file.fd, off, wh[ARG(4) % 3]);
    if (r < 0) { set_last_error(win32_errno(errno)); return FALSE; }
    if (ARG(3)) *(int64_t *)PTR(3) = r;
    return TRUE;
}
API(kernel32, GetFileSize, 2) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    if (o->file.is_arc) {
        if (ARG(1)) *(uint32_t *)PTR(1) = (uint32_t)(o->file.arc_size >> 32);
        return (uint32_t)o->file.arc_size;
    }
    struct stat st;
    if (fstat(o->file.fd, &st)) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    if (ARG(1)) *(uint32_t *)PTR(1) = (uint32_t)((uint64_t)st.st_size >> 32);
    return (uint32_t)st.st_size;
}
// BY_HANDLE_FILE_INFORMATION: attrs, 3x FILETIME, volume serial, size, links,
// 64-bit file index. Goldberg's steam_api reads this for its settings files.
API(kernel32, GetFileInformationByHandle, 2) {
    Object *o = file_obj(ARG(0));
    if (!o || !ARG(1)) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    uint8_t *b = PTR(1);
    memset(b, 0, 0x34);
    uint64_t size = 0, ino = 0, serial = 0, mtime = 0;
    uint32_t links = 1, attr = 0x80; // FILE_ATTRIBUTE_NORMAL
    if (o->file.is_arc) {
        size = o->file.arc_size;
        attr = 0x01; // READONLY
    } else {
        struct stat st;
        if (fstat(o->file.fd, &st)) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
        size = (uint64_t)st.st_size;
        ino = (uint64_t)st.st_ino;
        serial = (uint32_t)st.st_dev;
        links = (uint32_t)st.st_nlink;
        mtime = (uint64_t)st.st_mtime;
        attr = S_ISDIR(st.st_mode) ? 0x10 /*DIRECTORY*/ : 0x80;
    }
    uint64_t ft = (mtime + 11644473600ull) * 10000000ull;
    *(uint32_t *)(b + 0x00) = attr;
    *(uint32_t *)(b + 0x04) = (uint32_t)ft;         *(uint32_t *)(b + 0x08) = (uint32_t)(ft >> 32); // creation
    *(uint32_t *)(b + 0x0C) = (uint32_t)ft;         *(uint32_t *)(b + 0x10) = (uint32_t)(ft >> 32); // access
    *(uint32_t *)(b + 0x14) = (uint32_t)ft;         *(uint32_t *)(b + 0x18) = (uint32_t)(ft >> 32); // write
    *(uint32_t *)(b + 0x1C) = (uint32_t)serial;
    *(uint32_t *)(b + 0x20) = (uint32_t)(size >> 32); // nFileSizeHigh
    *(uint32_t *)(b + 0x24) = (uint32_t)size;          // nFileSizeLow
    *(uint32_t *)(b + 0x28) = links;
    *(uint32_t *)(b + 0x2C) = (uint32_t)(ino >> 32);   // nFileIndexHigh
    *(uint32_t *)(b + 0x30) = (uint32_t)ino;           // nFileIndexLow
    return TRUE;
}
API(kernel32, GetFileSizeEx, 2) {
    Object *o = file_obj(ARG(0));
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->file.is_arc) { *(int64_t *)PTR(1) = (int64_t)o->file.arc_size; return TRUE; }
    struct stat st;
    if (fstat(o->file.fd, &st)) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
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

// ---- legacy 16-bit-origin file API (OpenFile family) ----------------------
// AoE and other late-90s titles open files with CreateFile but read/seek/close
// them with these. On Win32 an HFILE is just a file HANDLE, so they map onto
// the same OBJ_FILE handles. HFILE_ERROR is -1; our handles are never -1.
#define HFILE_ERROR 0xFFFFFFFFu
// OpenFile/_lopen style bits: low 2 = OF_READ(0)/OF_WRITE(1)/OF_READWRITE(2).
#define OF_WRITE 0x0001
#define OF_READWRITE 0x0002
#define OF_CREATE 0x1000
#define OF_EXIST 0x4000
#define OF_DELETE 0x0200
static uint32_t lopen_access(uint32_t style) {
    uint32_t rw = style & 0x3;
    if (rw == OF_WRITE) return GENERIC_WRITE;
    if (rw == OF_READWRITE) return GENERIC_READ | GENERIC_WRITE;
    return GENERIC_READ;
}
API(kernel32, _lopen, 2) { return create_file(STR(0), lopen_access(ARG(1)), OPEN_EXISTING, 0); }
API(kernel32, _lcreat, 2) {
    // iAttribute bit 0 = read-only; otherwise create/truncate read-write.
    return create_file(STR(0), GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, 0);
}
// OpenFile(name, OFSTRUCT*, style): fills szPathName@4..131, cBytes@0. OF_EXIST
// just tests existence (open then close); OF_DELETE unlinks; OF_PARSE only fills
// the struct. Otherwise open per the access bits.
API(kernel32, OpenFile, 3) {
    uint8_t *of = PTR(1);
    if (of) {
        memset(of, 0, 136);
        of[0] = 136;         // cBytes
        of[1] = 1;           // fFixedDisk
        snprintf((char *)of + 4, 128, "%s", STR(0));
    }
    uint32_t style = ARG(2);
    if (style & OF_DELETE) {
        char host[PATH_MAX];
        if (vfs_to_host(STR(0), host, sizeof host) && unlink(host) == 0) { vfs_note_deleted(host); return 0; }
        return HFILE_ERROR;
    }
    uint32_t disp = (style & OF_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING;
    uint32_t h = create_file(STR(0), lopen_access(style), disp, 0);
    if (style & (OF_EXIST | 0x0100 /*OF_PARSE*/)) { // test-only: close and report
        if (h == INVALID_HANDLE_VALUE) return HFILE_ERROR;
        handle_close(h);
        return (style & OF_EXIST) ? 0 : h;
    }
    return h == INVALID_HANDLE_VALUE ? HFILE_ERROR : h;
}
API(kernel32, _lread, 3) {
    Object *o = file_obj(ARG(0));
    if (!o) return HFILE_ERROR;
    ssize_t r = read(o->file.fd, PTR(1), ARG(2));
    if (r < 0) { set_last_error(win32_errno(errno)); return HFILE_ERROR; }
    return (uint32_t)r;
}
API(kernel32, _hread, 3) { return api_kernel32__lread(_args); }
API(kernel32, _lwrite, 3) {
    Object *o = file_obj(ARG(0));
    if (!o) return HFILE_ERROR;
    ssize_t r = write(o->file.fd, PTR(1), ARG(2));
    if (r < 0) { set_last_error(win32_errno(errno)); return HFILE_ERROR; }
    return (uint32_t)r;
}
API(kernel32, _hwrite, 3) { return api_kernel32__lwrite(_args); }
API(kernel32, _llseek, 3) {
    Object *o = file_obj(ARG(0));
    if (!o) return HFILE_ERROR;
    static const int wh[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    off_t r = lseek(o->file.fd, (off_t)(int32_t)ARG(1), wh[ARG(2) % 3]);
    if (r < 0) { set_last_error(win32_errno(errno)); return HFILE_ERROR; }
    return (uint32_t)r;
}
API(kernel32, _lclose, 1) { return handle_close(ARG(0)) ? 0 : HFILE_ERROR; }

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
// mkdir(host), creating missing parent directories on ENOENT. Games installed
// into a writable folder often assume an intermediate dir (e.g. C:\game\Logs)
// already exists; when C:\game is served from a read-only .asar, our extracted
// code dir doesn't have it, so a non-recursive mkdir of Logs\<timestamp> fails
// and the game treats that as fatal. Creating parents fixes the asar case and is
// harmless for a real folder.
static int mkdir_parents(const char *host) {
    if (!mkdir(host, 0755)) return 0;
    if (errno != ENOENT) return -1;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", host);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
    return mkdir(host, 0755);
}
API(kernel32, CreateDirectoryA, 2) {
    char host[PATH_MAX];
    if (!vfs_to_host(STR(0), host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (mkdir_parents(host)) {
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

// A game file that lives only in the .asar.
static uint32_t archive_attrs(const char *name) {
    uint64_t off, size; int isdir;
    if (!vfs_archive_entry(name, &off, &size, &isdir)) return INVALID_FILE_ATTRIBUTES;
    return isdir ? FILE_ATTRIBUTE_DIRECTORY : (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
}
API(kernel32, GetFileAttributesA, 1) {
    char host[PATH_MAX];
    struct stat st;
    if (!vfs_to_host(STR(0), host, sizeof host)) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    if (stat(host, &st)) {
        uint32_t a = archive_attrs(STR(0));
        if (a != INVALID_FILE_ATTRIBUTES) return a;
        set_last_error(win32_errno(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
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

// Collect matching .asar entries into a find object (used for the archive game dir).
typedef struct { Object *o; int cap; const char *mask; } FindArc;
static void find_arc_add(void *v, const char *name, int is_dir, uint64_t size) {
    FindArc *fa = v;
    if (!wild_match(fa->mask, name)) return;
    if (fa->o->find.n == fa->cap) {
        fa->cap = fa->cap ? fa->cap * 2 : 32;
        fa->o->find.e = realloc(fa->o->find.e, fa->cap * sizeof *fa->o->find.e);
    }
    FindEntry *e = &fa->o->find.e[fa->o->find.n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir;
    e->size = size;
    e->mtime = 0;
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

    Object *o = obj_new(OBJ_FIND);
    int cap = 0;
    // Game dir served from an .asar: enumerate the archive (its index has every
    // file, including the extracted exe/dll).
    if (vfs_game_is_archive()) {
        FindArc fa = { o, 0, mask };
        int n = vfs_archive_list(dir, find_arc_add, &fa);
        if (n >= 0) {
            if (!o->find.n) { free(o->find.e); free(o); set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
            fill_find_data(data, &o->find.e[0]);
            o->find.pos = 1;
            return handle_alloc(o);
        }
    }

    DIR *d = opendir(host);
    if (!d) { free(o); set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
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
API(kernel32, GetDiskFreeSpaceExA, 4) {
    struct statvfs sv;
    uint64_t freeb = 0x80000000ull, total = 0x100000000ull;
    if (!statvfs(vfs_user_dir(), &sv)) { freeb = (uint64_t)sv.f_bavail * sv.f_frsize; total = (uint64_t)sv.f_blocks * sv.f_frsize; }
    if (ARG(1)) *(uint64_t *)PTR(1) = freeb; // lpFreeBytesAvailable
    if (ARG(2)) *(uint64_t *)PTR(2) = total; // lpTotalNumberOfBytes
    if (ARG(3)) *(uint64_t *)PTR(3) = freeb; // lpTotalNumberOfFreeBytes
    return TRUE;
}
API(kernel32, GetDiskFreeSpaceExW, 4) { return api_kernel32_GetDiskFreeSpaceExA(_args); }
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
    if (f && f->file.is_arc) {
        // Archive-backed file: map a window of the shared archive fd.
        o->mapping.fd = dup(vfs_game_archive_fd());
        o->mapping.is_arc = 1;
        o->mapping.arc_base = f->file.arc_off;
        o->mapping.size = ARG(4) ? ARG(4) : (uint32_t)f->file.arc_size;
    } else {
        o->mapping.fd = f ? dup(f->file.fd) : -1;
        o->mapping.size = ARG(4);
    }
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
        if (o->mapping.is_arc) size = (uint32_t)(o->mapping.size - off);
        else {
            struct stat st;
            size = o->mapping.fd >= 0 && !fstat(o->mapping.fd, &st) ? (uint32_t)(st.st_size - off) : o->mapping.size;
        }
    }
    uint64_t file_off = o->mapping.arc_base + off; // arc_base is 0 for non-archive
    // LM_TAG_MAPPED (not VIRTUAL) so VirtualFree won't decommit a view: on Windows
    // mapped views are freed only by UnmapViewOfFile, and VirtualFree(MEM_DECOMMIT)
    // on one fails harmlessly. AoE memory-maps its DRS archives and then calls
    // VirtualFree(MEM_DECOMMIT) on sprite pointers inside a view; decommitting those
    // would unmap live file data and crash when the sprite is read.
    uint32_t va = lm_reserve(0, size, LM_GRAN, LM_TAG_MAPPED, false);
    if (!va) return 0;
    if (o->mapping.fd >= 0) {
        int prot = (ARG(1) & 2) ? PROT_READ | PROT_WRITE : PROT_READ;
        // An archive entry is zero-copy mmap'd only when its data is page-aligned
        // (our packer aligns to 4K). Otherwise — or if mmap fails — copy the bytes
        // into committed memory. Archive mappings are MAP_PRIVATE (read-only fd).
        bool can_mmap = !o->mapping.is_arc || (file_off % 4096 == 0);
        void *p = MAP_FAILED;
        if (can_mmap) {
            int mf = (o->mapping.is_arc ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED;
            p = mmap(G2H(va), size, prot, mf, o->mapping.fd, (off_t)file_off);
        }
        if (p == MAP_FAILED) {
            if (!o->mapping.is_arc) { lm_release(va); return 0; }
            lm_commit(va, size, PAGE_READWRITE);
            if (pread(o->mapping.fd, G2H(va), size, (off_t)file_off) < 0) { lm_release(va); return 0; }
        } else {
            lm_commit(va, size, (ARG(1) & 2) ? PAGE_READWRITE : PAGE_READONLY);
        }
    } else {
        lm_commit(va, size, PAGE_READWRITE);
    }
    LOGD("MapViewOfFile(h=0x%x off=0x%llx sz=0x%x) = 0x%x", ARG(0), (unsigned long long)off, size, va);
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
    if (mkdir_parents(host)) { set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : win32_errno(errno)); return FALSE; }
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
uint32_t k32_map_view(uint32_t *a) { return api_kernel32_MapViewOfFile(a); }
