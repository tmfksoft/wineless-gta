#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Read-only ASAR archive support (Electron's format): one uncompressed data blob
// + a JSON index of {name -> {size, offset}}. We pack with each file's data
// 4096-aligned so MapViewOfFile can map it zero-copy; stock (unaligned) asars
// still read fine (reads are offset-based; only mmap falls back to a copy).

typedef struct Asar Asar;

// Open and index an .asar. Transparently handles a *bundle* — an .asar appended
// to a binary with a footer (see asar_make_bundle) — by reading the archive at
// the footer's offset. NULL on failure.
Asar *asar_open(const char *path);
void  asar_close(Asar *a);

// True if `path` carries an appended-asar footer (i.e. it is a bundle).
int asar_is_bundle(const char *path);

// Build a self-contained bundle: base_bin + the game's asar + a locator footer,
// written to `out` (made executable). `game` may be a directory (packed first)
// or an existing .asar. Returns 0 on success.
int asar_make_bundle(const char *base_bin, const char *game, const char *out);

// Read a whole entry (e.g. "wineless.config") into buf. Returns its length, or
// -1 if absent. Convenience over asar_open/asar_stat for small config files.
long asar_read_whole(const char *path, const char *relpath, char *buf, size_t cap);

// The archive's own fd, for pread()/mmap() by callers. -1 if closed.
int asar_fd(const Asar *a);

// Resolve a relative path (either slash style, case-insensitive). On a file hit,
// sets *offset (absolute in the archive file) and *size and returns 1. On a
// directory hit returns 1 with *is_dir=1. Returns 0 if not found.
int asar_stat(const Asar *a, const char *relpath, uint64_t *offset, uint64_t *size, int *is_dir);

// List immediate children of a directory ("" = root). cb is called per child.
// Returns the child count, or -1 if reldir is not a directory.
typedef void (*asar_list_cb)(void *ctx, const char *name, int is_dir, uint64_t size);
int asar_list(const Asar *a, const char *reldir, asar_list_cb cb, void *ctx);

// Iterate every entry (files and dirs) with its full relative path + location.
typedef void (*asar_each_cb)(void *ctx, const char *relpath, uint64_t offset, uint64_t size, int is_dir);
void asar_foreach(const Asar *a, asar_each_cb cb, void *ctx);

// Pack a directory tree into an aligned .asar (standard, stock-tool readable).
// Returns 0 on success, non-zero on error.
int asar_pack(const char *srcdir, const char *outpath);

// Debug: open an .asar, print its index, and (if relpath) read + hexdump the head
// of that entry. Returns 0 on success. Exercises the reader end to end.
int asar_debug_list(const char *path, const char *relpath);

// Find a bootable .exe in the archive (prefers a top-level one). On success writes
// its relative path (backslash-separated, Windows style) to out and returns 1.
int asar_find_exe(const char *path, char *out, size_t outsz);
