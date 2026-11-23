#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Virtual Windows layout presented to the game:
//   C:\game                                              -> game dir (indexed)
//   C:\Users\Player\Documents\<game> User Files       -> user files dir (indexed)
//   C:\Users\Player\Documents                            -> parent of user files dir
//   C:\Users\Player\AppData, C:\ProgramData              -> per-game folders under
//                                                           $XDG_DATA_HOME/wineless-gta/<game>
#define VFS_GAME_VDIR "C:\\game"
#define VFS_DOCS_VDIR "C:\\Users\\Player\\Documents"
#define VFS_APPDATA_VDIR "C:\\Users\\Player\\AppData"
#define VFS_ROAMING_VDIR VFS_APPDATA_VDIR "\\Roaming"
#define VFS_LOCAL_VDIR VFS_APPDATA_VDIR "\\Local"
#define VFS_TEMP_VDIR VFS_LOCAL_VDIR "\\Temp"
#define VFS_PROGRAMDATA_VDIR "C:\\ProgramData"
#define VFS_STEAM_VDIR "C:\\Program Files (x86)\\Steam" // Steam's legacycompat folder
// Name of the game's folder in Documents ("GTA San Andreas User Files", ...)
extern const char *g_user_subdir;

// data_dir: this game's private folder (holds AppData and ProgramData)
void vfs_init(const char *game_dir, const char *user_dir, const char *data_dir);

// Map a Windows path (absolute or relative to the virtual cwd) to a host path.
// Case-insensitive via the index. Returns false if outside all mounts.
bool vfs_to_host(const char *winpath, char *out, size_t outsz);

// Keep the index in sync after the game changes the file system.
void vfs_note_created(const char *hostpath, bool is_dir);
void vfs_note_deleted(const char *hostpath);

const char *vfs_get_cwd(void); // virtual, no trailing backslash
bool vfs_set_cwd(const char *winpath);
// Canonical absolute virtual path (for GetFullPathName)
void vfs_full_path(const char *winpath, char *out, size_t outsz);

const char *vfs_game_dir(void);
const char *vfs_user_dir(void);

// ---- ASAR game mount ----
// True if the game dir is served from an .asar archive (vfs_init was given one).
bool vfs_game_is_archive(void);
// The archive's fd (for pread/mmap). -1 if not an archive mount.
int  vfs_game_archive_fd(void);
// Resolve a Windows path to an archive entry. On a file hit sets *off (absolute
// in the archive) and *size; on a dir hit sets *is_dir. Returns true on any hit.
bool vfs_archive_entry(const char *winpath, uint64_t *off, uint64_t *size, int *is_dir);
// Enumerate immediate children of an archive directory for FindFirstFile. cb gets
// each child's name + whether it's a dir. Returns child count, or -1 if not an
// archive dir.
typedef void (*vfs_arc_list_cb)(void *ctx, const char *name, int is_dir, uint64_t size);
int vfs_archive_list(const char *windir, vfs_arc_list_cb cb, void *ctx);
