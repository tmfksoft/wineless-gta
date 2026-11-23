#pragma once
// Entry points to boot and run a guest, shared by the standalone CLI (main.c)
// and the libretro core (src/libretro/). Defined in main.c.
//
// Both must be called on the thread that will become the guest's main thread:
// they call thread_setup_current() and then run the guest inline via call32(),
// which does not return until the game exits.

#ifdef __cplusplus
extern "C" {
#endif

// Resolve game dir / exe / user dir the way the CLI does (honouring
// $WINELESS_GAME_DIR, the exe-detection rules and the per-game user folder),
// set the g_exe_name / g_is_gtasa / g_user_subdir globals, then boot and run.
// Any argument may be NULL/empty to fall back to the usual defaults.
// Returns the guest process exit code (or 1 on a setup failure).
int wl_boot_game(const char *game_dir_in, const char *exe_name, const char *user_dir_in);

// Lower half: paths already resolved. exe_host is the real filesystem path to
// the .exe; game_dir/user_dir are host paths. Sets the DXVK/SDL environment
// defaults, initialises the engine, loads the PE and runs it. g_exe_name must
// already be set. Returns the guest process exit code.
int wl_boot_game_resolved(const char *game_dir, const char *exe_host, const char *user_dir,
                          const char *extra_args);

#ifdef __cplusplus
}
#endif
