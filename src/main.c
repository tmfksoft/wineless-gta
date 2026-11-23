// Wineless GTA: run GTA III, Vice City and San Andreas on 64-bit Linux without Wine.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "cpu.h"
#include "handles.h"
#include "log.h"
#include "lowmem.h"
#include "asar.h"
#include "pe.h"
#include "thread.h"
#include "vfs.h"
#include "wl_boot.h"
#include "win32/win32.h"

// Executable to run: --exe, or detected in the game folder (see detect_exe).
static char exe_name_buf[256];
const char *g_exe_name = NULL;
bool g_is_gtasa = false;

// True if `path` is a 32-bit x86 Windows executable (not, say, this program
// copied in as gta3.exe so Steam launches it).
static bool is_win32_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[4096]; // the PE header can sit well past the DOS stub (gta-vc.exe: 0x880)
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 0x40 || h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe = *(uint32_t *)(h + 0x3c);
    return pe + 6 <= n && !memcmp(h + pe, "PE\0\0", 4) && *(uint16_t *)(h + pe + 4) == 0x14c;
}

// True if the PE has Steam's DRM wrapper (a ".bind" section).
static bool has_steam_drm(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[4096];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 0x40) return false;
    uint32_t pe = *(uint32_t *)(h + 0x3c);
    if (pe + 24 > n) return false;
    uint16_t nsect = *(uint16_t *)(h + pe + 6), optsz = *(uint16_t *)(h + pe + 20);
    for (int i = 0; i < nsect; i++) {
        uint32_t o = pe + 24 + optsz + i * 40;
        if (o + 8 <= n && !memcmp(h + o, ".bind", 5)) return true;
    }
    return false;
}

// Pick the game exe in `dir`: the known names first (gta_sa.exe, gta-vc.exe,
// gta3.exe), then any gta*.exe that isn't wrapped in Steam DRM.
static bool detect_exe(const char *dir) {
    static const char *known[] = {"gta_sa.exe", "gta-vc.exe", "gta3.exe"};
    DIR *d = opendir(dir);
    if (!d) return false;
    char found[3][256] = {{0}}, other[256] = "", drm[256] = "";
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".exe")) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (!is_win32_exe(full)) continue; // e.g. this launcher renamed to gta3.exe
        bool wrapped = has_steam_drm(full);
        for (int i = 0; i < 3; i++)
            if (!strcasecmp(de->d_name, known[i])) snprintf(found[i], sizeof found[i], "%s", de->d_name);
        if (!strncasecmp(de->d_name, "gta", 3)) {
            if (wrapped) snprintf(drm, sizeof drm, "%s", de->d_name);
            else if (!*other) snprintf(other, sizeof other, "%s", de->d_name);
        }
    }
    closedir(d);
    for (int i = 0; i < 3; i++) {
        if (!*found[i]) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, found[i]);
        if (has_steam_drm(full)) continue; // try something else first
        snprintf(exe_name_buf, sizeof exe_name_buf, "%s", found[i]);
        return true;
    }
    if (*other) {
        snprintf(exe_name_buf, sizeof exe_name_buf, "%s", other);
        return true;
    }
    static bool warned;
    if (*drm && !warned) {
        warned = true;
        LOGE("%s uses Steam's DRM wrapper, which isn't supported; put a DRM-free exe next to it", drm);
    }
    return false;
}

static bool find_exe(const char *dir, char *out, size_t n) {
    if (!g_exe_name) {
        if (!detect_exe(dir)) return false;
        g_exe_name = exe_name_buf;
    }
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    bool ok = false;
    while ((de = readdir(d)))
        if (!strcasecmp(de->d_name, g_exe_name)) {
            snprintf(out, n, "%s/%s", dir, de->d_name);
            ok = true;
            break;
        }
    closedir(d);
    return ok;
}

// $XDG_DOCUMENTS_DIR from ~/.config/user-dirs.dirs, else ~/Documents
static void documents_dir(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, n, "%s/Documents", home);
    const char *e = getenv("XDG_DOCUMENTS_DIR");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    char cfg[PATH_MAX];
    const char *xch = getenv("XDG_CONFIG_HOME");
    if (xch && *xch) snprintf(cfg, sizeof cfg, "%s/user-dirs.dirs", xch);
    else snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(cfg, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "XDG_DOCUMENTS_DIR=\"");
        if (p != line) continue;
        p += strlen("XDG_DOCUMENTS_DIR=\"");
        char *q = strrchr(p, '"');
        if (!q) break;
        *q = 0;
        if (!strncmp(p, "$HOME", 5)) snprintf(out, n, "%s%s", home, p + 5);
        else snprintf(out, n, "%s", p);
        break;
    }
    fclose(f);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--exe NAME] [--game-dir DIR] [--user-dir DIR] [--display MODE] [-- game args]\n"
            "  --exe       game exe to run (default: gta_sa.exe, gta-vc.exe or gta3.exe found in the game folder)\n"
            "  --game-dir  folder containing the game (default: $WINELESS_GAME_DIR, the\n"
            "              folder this program is in, or the current folder)\n"
            "  --user-dir  where saves/settings go (default: the game's folder in Documents,\n"
            "              e.g. \"GTA Vice City User Files\")\n"
            "  --display   fullscreen (default) | borderless | windowed   (also $WINELESS_DISPLAY)\n"
            "  --install-steam-tool  register as a Steam compatibility tool (\"Wineless\")\n"
            "  --save      write the given params to ./wineless.config and exit (no launch)\n"
            "A wineless.config next to the binary can set exe, game-dir, user-dir, display\n"
            "and environment variables (key = value lines), for one-click drop-in launches.\n"
            "  --windowed  same as --display windowed\n"
            "  WINELESS_LOG=error|warn|info|debug|trace\n",
            argv0);
}

// The project was first called "San Andreas Wrapper": accept its SAW_* variable
// names as aliases for WINELESS_* (an explicit WINELESS_* setting wins).
extern char **environ;
static void legacy_env_aliases(void) {
    char *renamed[64];
    int n = 0;
    for (char **e = environ; *e && n < 64; e++)
        if (!strncmp(*e, "SAW_", 4) && strchr(*e, '=')) renamed[n++] = strdup(*e);
    for (int i = 0; i < n; i++) {
        char *eq = strchr(renamed[i], '=');
        *eq = 0;
        char name[256];
        snprintf(name, sizeof name, "WINELESS_%s", renamed[i] + 4);
        setenv(name, eq + 1, 0);
        fprintf(stderr, "wineless-gta: %s is deprecated, use %s\n", renamed[i], name);
        free(renamed[i]);
    }
}

// ---------------------------------------------------------------- Steam compatibility tool
// Steam runs a compatibility tool as `<tool> <verb> <linux path to exe> [args]`
// (see toolmanifest.vdf below), with the game folder as working directory.

// Launchers Steam starts instead of the game, and what their Play button runs.
static const struct { const char *launcher, *game; } launcher_redirects[] = {
    {"Launcher.exe", "AoK HD.exe"}, // Age of Empires II HD (splash with Play/Workshop links)
};

static void mkdirs_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

// Without a terminal (launched by Steam) keep a log in ~/.cache/wineless-gta.
static void log_to_cache_file(const char *exe_name) {
    if (isatty(2)) return;
    char dir[PATH_MAX], path[PATH_MAX + 300];
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/wineless-gta", xdg);
    else if (home) snprintf(dir, sizeof dir, "%s/.cache/wineless-gta", home);
    else return;
    mkdirs_p(dir);
    snprintf(path, sizeof path, "%s/%s.log", dir, exe_name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    dup2(fileno(f), 1);
    dup2(fileno(f), 2);
    fclose(f);
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

// --install-steam-tool: register this binary as a Steam compatibility tool.
static int install_steam_tool(void) {
    const char *home = getenv("HOME");
    char root[PATH_MAX], dir[PATH_MAX + 64], path[PATH_MAX + 128], self[PATH_MAX];
    if (!home) { fprintf(stderr, "HOME is not set\n"); return 1; }
    snprintf(path, sizeof path, "%s/.steam/root", home);
    if (!realpath(path, root)) snprintf(root, sizeof root, "%s/.local/share/Steam", home);
    snprintf(dir, sizeof dir, "%s/compatibilitytools.d/wineless", root);
    mkdirs_p(dir);
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return 1;
    self[l] = 0;
    // copy the binary so rebuilding/moving the original doesn't break Steam
    snprintf(path, sizeof path, "%s/wineless-gta", dir);
    FILE *in = fopen(self, "rb"), *out = fopen(path, "wb");
    if (!in || !out) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    if (fclose(out)) return 1;
    chmod(path, 0755);
    snprintf(path, sizeof path, "%s/compatibilitytool.vdf", dir);
    bool ok = write_file(path, "\"compatibilitytools\"\n{\n  \"compat_tools\"\n  {\n    \"wineless\"\n    {\n"
                                "      \"install_path\" \".\"\n      \"display_name\" \"Wineless\"\n"
                                "      \"from_oslist\" \"windows\"\n      \"to_oslist\" \"linux\"\n    }\n  }\n}\n");
    snprintf(path, sizeof path, "%s/toolmanifest.vdf", dir);
    ok = ok && write_file(path, "\"manifest\"\n{\n  \"version\" \"2\"\n  \"commandline\" \"/wineless-gta %verb%\"\n}\n");
    if (!ok) { fprintf(stderr, "cannot write the tool files in %s\n", dir); return 1; }
    printf("Installed the Wineless compatibility tool in %s\n"
           "Restart Steam, then pick it under a game's Properties > Compatibility.\n", dir);
    return 0;
}

// Handles Steam's verbs. Returns -1 to carry on and run the game (argv adjusted),
// otherwise an exit code.
static int steam_verb(int *argc, char ***argv, char *game_dir, size_t gdsz) {
    if (*argc < 2) return -1;
    const char *verb = (*argv)[1];
    if (!strcmp(verb, "getcompatpath") || !strcmp(verb, "getnativepath")) {
        if (*argc > 2) printf("%s\n", (*argv)[2]);
        return 0;
    }
    if (strcmp(verb, "run") && strcmp(verb, "waitforexitandrun")) return -1;
    if (*argc < 3) return 1;
    static char path[PATH_MAX], name[512];
    snprintf(path, sizeof path, "%s", (*argv)[2]);
    char *slash = strrchr(path, '/');
    if (!slash) return 1;
    *slash = 0;
    snprintf(name, sizeof name, "%s", slash + 1);
    snprintf(game_dir, gdsz, "%s", path);
    // Steam's install-script runner (DirectX/VC++ redistributables): nothing to install here
    if (!strcasecmp(name, "iscriptevaluator.exe")) return 0;
    const char *over = getenv("WINELESS_EXE");
    if (over && *over) snprintf(name, sizeof name, "%s", over);
    else
        for (size_t i = 0; i < sizeof launcher_redirects / sizeof *launcher_redirects; i++) {
            char alt[PATH_MAX + 512];
            snprintf(alt, sizeof alt, "%s/%s", path, launcher_redirects[i].game);
            if (!strcasecmp(name, launcher_redirects[i].launcher) && access(alt, F_OK) == 0) {
                snprintf(name, sizeof name, "%s", launcher_redirects[i].game);
                break;
            }
        }
    g_exe_name = name;
    log_to_cache_file(name);
    LOGI("steam: %s %s (app %s)%s", verb, name, getenv("SteamAppId") ? getenv("SteamAppId") : "?",
         strcmp(name, slash + 1) ? " in place of its launcher" : "");
    // the remaining arguments are the game's
    (*argv)[2] = "--";
    (*argv)++;
    (*argc)--;
    return -1;
}

// The directory the wineless-gta binary itself lives in.
static bool self_dir(char *out, size_t n) {
    char self[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return false;
    self[l] = 0;
    char *s = strrchr(self, '/');
    if (!s) return false;
    *s = 0;
    snprintf(out, n, "%s", self);
    return true;
}

// Resolve a config path value: ~ -> $HOME, absolute kept, else relative to the
// config file's directory (where the binary and config live).
static void resolve_path(const char *in, const char *base, char *out, size_t n) {
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0)) {
        const char *home = getenv("HOME");
        snprintf(out, n, "%s%s", home ? home : "", in + 1);
    } else if (in[0] == '/') {
        snprintf(out, n, "%s", in);
    } else {
        snprintf(out, n, "%s/%s", base, in);
    }
}

typedef struct { char exe[256], game_dir[PATH_MAX], user_dir[PATH_MAX]; } Config;

// Optional wineless.config next to the binary, so dropping the binary + a config
// into a game folder is a one-click launch. "key = value" lines, # or ; comments.
// Known keys: exe, game-dir, user-dir, display. Any other key sets an environment
// variable (a real env var or command-line flag still wins).
static void load_config(Config *c) {
    memset(c, 0, sizeof *c);
    char dir[PATH_MAX], path[PATH_MAX + 32];
    if (!self_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/wineless.config", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    LOGI("config: %s", path);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *key = line;
        while (isspace((unsigned char)*key)) key++;
        if (*key == '#' || *key == ';' || *key == 0 || *key == '\n') continue;
        char *eq = strchr(key, '=');
        if (!eq) continue;
        char *val = eq + 1;
        for (char *e = eq; e > key && isspace((unsigned char)e[-1]);) *--e = 0;
        *eq = 0;
        while (isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
        size_t vl = strlen(val);
        if (vl >= 2 && (*val == '"' || *val == '\'') && val[vl - 1] == *val) { val[vl - 1] = 0; val++; }
        if (!*key) continue;
        if (!strcasecmp(key, "exe")) snprintf(c->exe, sizeof c->exe, "%s", val);
        else if (!strcasecmp(key, "game-dir") || !strcasecmp(key, "game_dir")) resolve_path(val, dir, c->game_dir, sizeof c->game_dir);
        else if (!strcasecmp(key, "user-dir") || !strcasecmp(key, "user_dir")) resolve_path(val, dir, c->user_dir, sizeof c->user_dir);
        else if (!strcasecmp(key, "display")) setenv("WINELESS_DISPLAY", val, 0);
        else setenv(key, val, 0); // environment variable
    }
    fclose(f);
}

// Apply a wineless.config found at the root of a bundled .asar. Lowest precedence:
// every setenv uses overwrite=0, so a real env var (or anything set by the
// operator) wins. Only exe/display and WINELESS_*/DXVK_*/SDL_* env keys are
// honoured; game-dir/user-dir and arbitrary host env vars are ignored, so a
// packaged game can't influence the host process environment.
static void apply_bundle_config(const char *self) {
    char buf[8192];
    long n = asar_read_whole(self, "wineless.config", buf, sizeof buf - 1);
    if (n < 0) return;
    buf[n] = 0;
    LOGI("config: wineless.config from bundle");
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *key = line;
        while (isspace((unsigned char)*key)) key++;
        if (*key == '#' || *key == ';' || !*key) continue;
        char *eq = strchr(key, '=');
        if (!eq) continue;
        char *val = eq + 1;
        for (char *e = eq; e > key && isspace((unsigned char)e[-1]);) *--e = 0;
        *eq = 0;
        while (isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
        size_t vl = strlen(val);
        if (vl >= 2 && (*val == '"' || *val == '\'') && val[vl - 1] == *val) { val[vl - 1] = 0; val++; }
        if (!*key) continue;
        if (!strcasecmp(key, "exe")) setenv("WINELESS_EXE", val, 0);
        else if (!strcasecmp(key, "display")) setenv("WINELESS_DISPLAY", val, 0);
        else if (!strcasecmp(key, "game-dir") || !strcasecmp(key, "game_dir")) {} // the asar IS the game
        else if (!strcasecmp(key, "user-dir") || !strcasecmp(key, "user_dir")) {} // host-specific
        else if (!strncasecmp(key, "WINELESS_", 9) || !strncasecmp(key, "DXVK_", 5) || !strncasecmp(key, "SDL_", 4))
            setenv(key, val, 0);
        // else: ignored — no arbitrary host env injection from a packaged game
    }
}

// --save: write the resolved parameters to ./wineless.config so a later bare
// run (or a drop-in launch) reuses them. Keys match load_config's.
static int save_config(const char *exe, const char *game_dir, const char *user_dir) {
    char path[PATH_MAX];
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    snprintf(path, sizeof path, "%s/wineless.config", cwd);
    FILE *f = fopen(path, "w");
    if (!f) { LOGE("cannot write %s: %s", path, strerror(errno)); return 1; }
    fputs("# wineless.config - generated by --save\n", f);
    if (exe && *exe) fprintf(f, "exe = %s\n", exe);
    if (game_dir && *game_dir) fprintf(f, "game-dir = %s\n", game_dir);
    if (user_dir && *user_dir) fprintf(f, "user-dir = %s\n", user_dir);
    const char *disp = getenv("WINELESS_DISPLAY");
    if (disp && *disp) fprintf(f, "display = %s\n", disp);
    if (fclose(f)) { LOGE("cannot write %s", path); return 1; }
    LOGI("config: wrote %s", path);
    return 0;
}

int main(int argc, char **argv) {
    legacy_env_aliases();
    if (argc > 1 && !strcmp(argv[1], "--install-steam-tool")) return install_steam_tool();
    // Pack a game directory into an aligned .asar (files 4K-aligned for zero-copy
    // MapViewOfFile). No engine needed — pure packaging. Usage:
    //   wineless-gta --pack-asar <game-dir> <out.asar>
    if (argc == 4 && !strcmp(argv[1], "--pack-asar")) {
        int rc = asar_pack(argv[2], argv[3]);
        if (rc) fprintf(stderr, "wineless-gta: packing %s failed (%d)\n", argv[2], rc);
        else fprintf(stderr, "wineless-gta: packed %s -> %s\n", argv[2], argv[3]);
        return rc ? 1 : 0;
    }
    // Debug: dump an .asar's index, and optionally hexdump the head of one entry.
    //   wineless-gta --asar-list <file.asar> [relpath-to-read]
    if (argc >= 3 && !strcmp(argv[1], "--asar-list")) return asar_debug_list(argv[2], argc > 3 ? argv[3] : NULL);
    // Build a self-contained bundle (engine + game + locator footer):
    //   wineless-gta --make-bundle [base-binary] <game-dir|.asar> <out>
    // base-binary defaults to this executable; pass wineless_libretro.so to make
    // a self-contained core. Running the bundle boots the embedded game.
    if (argc >= 3 && !strcmp(argv[1], "--make-bundle")) {
        const char *base, *game, *out;
        if (argc == 5) { base = argv[2]; game = argv[3]; out = argv[4]; }
        else if (argc == 4) {
            static char self[PATH_MAX];
            ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
            if (l <= 0) { fprintf(stderr, "cannot find self\n"); return 1; }
            self[l] = 0; base = self; game = argv[2]; out = argv[3];
        } else { fprintf(stderr, "usage: --make-bundle [base] <game-dir|.asar> <out>\n"); return 1; }
        int rc = asar_make_bundle(base, game, out);
        if (!rc) fprintf(stderr, "wineless-gta: bundled %s + %s -> %s\n", base, game, out);
        return rc ? 1 : 0;
    }
    log_init();
    // Self-contained bundle: this binary has an .asar appended (see --make-bundle).
    // Apply its wineless.config and boot the embedded game.
    {
        char self[PATH_MAX];
        ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
        if (l > 0) {
            self[l] = 0;
            if (asar_is_bundle(self)) {
                LOGI("bundle: booting embedded game from %s", self);
                // wl_boot_game applies the bundle's wineless.config; a real
                // WINELESS_EXE env var (passed here) still overrides exe=.
                return wl_boot_game(self, getenv("WINELESS_EXE"), NULL);
            }
        }
    }
    char steam_game_dir[PATH_MAX] = "";
    int vr = steam_verb(&argc, &argv, steam_game_dir, sizeof steam_game_dir);
    if (vr >= 0) return vr;
    Config cfg;
    load_config(&cfg); // sets env vars; its exe/dirs are defaults below
    char game_dir[PATH_MAX] = "", user_dir[PATH_MAX] = "";
    // defaults: Steam-provided dir, else config; the CLI loop below overrides
    snprintf(game_dir, sizeof game_dir, "%s", *steam_game_dir ? steam_game_dir : cfg.game_dir);
    snprintf(user_dir, sizeof user_dir, "%s", cfg.user_dir);
    if (!g_exe_name && cfg.exe[0]) g_exe_name = strdup(cfg.exe);
    bool do_save = false;
    int i = 1;
    for (; i < argc; i++) {
        // options work with one or two dashes (-exe / --exe)
        const char *o = argv[i];
        if (o[0] == '-' && o[1] == '-' && o[2]) o++;
        if (!strcmp(o, "-game-dir") && i + 1 < argc) snprintf(game_dir, sizeof game_dir, "%s", argv[++i]);
        else if (!strcmp(o, "-user-dir") && i + 1 < argc) snprintf(user_dir, sizeof user_dir, "%s", argv[++i]);
        else if (!strcmp(o, "-display") && i + 1 < argc) setenv("WINELESS_DISPLAY", argv[++i], 1);
        else if (!strcmp(o, "-windowed")) setenv("WINELESS_DISPLAY", "windowed", 1);
        else if (!strcmp(o, "-exe") && i + 1 < argc) g_exe_name = argv[++i];
        else if (!strcmp(o, "-save")) do_save = true;
        else if (!strcmp(o, "-h") || !strcmp(o, "-help")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else break;
    }
    char exe[PATH_MAX];
    if (!*game_dir && getenv("WINELESS_GAME_DIR")) snprintf(game_dir, sizeof game_dir, "%s", getenv("WINELESS_GAME_DIR"));
    if (!*game_dir) {
        char self[PATH_MAX];
        ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
        if (l > 0) {
            self[l] = 0;
            *strrchr(self, '/') = 0;
            if (find_exe(self, exe, sizeof exe)) snprintf(game_dir, sizeof game_dir, "%s", self);
        }
    }
    if (!*game_dir) getcwd(game_dir, sizeof game_dir);
    char real[PATH_MAX];
    if (realpath(game_dir, real)) snprintf(game_dir, sizeof game_dir, "%s", real);
    // A game packed as an .asar: delegate to the shared asar-aware boot path.
    {
        size_t gl = strlen(game_dir);
        if (gl > 5 && !strcasecmp(game_dir + gl - 5, ".asar"))
            return wl_boot_game(game_dir, g_exe_name, *user_dir ? user_dir : NULL);
    }
    if (!find_exe(game_dir, exe, sizeof exe)) {
        if (g_exe_name) LOGE("%s not found in %s", g_exe_name, game_dir);
        else LOGE("no usable GTA III / Vice City / San Andreas exe in %s", game_dir);
        usage(argv[0]);
        return 1;
    }
    // Each game keeps its saves in its own Documents folder.
    g_is_gtasa = !strcasecmp(g_exe_name, "gta_sa.exe");
    LOGI("game: %s", g_exe_name);
    if (strcasestr(g_exe_name, "gta3")) g_user_subdir = "GTA3 User Files";
    else if (strcasestr(g_exe_name, "vc") || strcasestr(g_exe_name, "vice")) g_user_subdir = "GTA Vice City User Files";
    if (!*user_dir) {
        char docs[PATH_MAX];
        documents_dir(docs, sizeof docs);
        snprintf(user_dir, sizeof user_dir, "%s/%s", docs, g_user_subdir);
    }

    if (do_save) return save_config(g_exe_name, game_dir, user_dir);

    // Join any trailing game arguments into one string.
    char extra[4096] = "";
    for (int p = 0, first = 1; i < argc && p < (int)sizeof extra - 2; i++, first = 0)
        p += snprintf(extra + p, sizeof extra - p, "%s%s", first ? "" : " ", argv[i]);

    return wl_boot_game_resolved(game_dir, exe, user_dir, extra[0] ? extra : NULL);
}

// ---------------------------------------------------------------- shared boot
// Paths already resolved. Sets the DXVK/SDL environment, initialises the engine,
// loads the PE and runs it to completion on the current thread. Shared by main()
// and the libretro core (see wl_boot.h).
int wl_boot_game_resolved(const char *game_dir, const char *exe_host, const char *user_dir,
                          const char *extra_args) {
    // DXVK-native must be told which window system integration to use.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    // DXVK would otherwise write <exe>_d3d9.log into the current (game) folder.
    setenv("DXVK_LOG_PATH", "none", 0);
    // These games run exclusive fullscreen on Windows, where presents wait for
    // vsync; our swapchains are "windowed", so ask DXVK for vsync explicitly.
    setenv("DXVK_CONFIG", "d3d9.presentInterval = 1", 0);
    // Prefer X11 (XWayland on Wayland desktops): SDL's Wayland path goes through
    // libdecor, whose GTK plugin aborts on some GNOME setups. Override with
    // SDL_VIDEO_DRIVER=wayland.
    setenv("SDL_VIDEO_DRIVER", "x11,wayland", 0);

    // Build the game command line: "C:\GTA San Andreas\gta_sa.exe" args...
    char cmdline[4096];
    int n = snprintf(cmdline, sizeof cmdline, "\"%s\\%s\"", VFS_GAME_VDIR, g_exe_name);
    if (extra_args && *extra_args) snprintf(cmdline + n, sizeof cmdline - n, " %s", extra_args);

    lm_init();
    cpu_init();
    handles_init();
    api_init();
    thread_init_process();
    // Private per-game folder for AppData / ProgramData: $XDG_DATA_HOME/wineless-gta/<exe name>
    char data_dir[PATH_MAX];
    {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        char base[PATH_MAX];
        if (xdg && *xdg) snprintf(base, sizeof base, "%s", xdg);
        else snprintf(base, sizeof base, "%s/.local/share", home ? home : "/tmp");
        char prof[256];
        snprintf(prof, sizeof prof, "%s", g_exe_name);
        char *dot = strrchr(prof, '.');
        if (dot && !strcasecmp(dot, ".exe")) *dot = 0;
        snprintf(data_dir, sizeof data_dir, "%s/wineless-gta/%s", base, prof);
    }
    vfs_init(game_dir, user_dir, data_dir);
    win32_init(cmdline);

    cpu_install_signal_handlers();

    // The main thread must be a guest thread before loading: DLL initializers
    // (DllMain, TLS callbacks) run while the exe's imports are resolved.
    Thread *t = thread_setup_current(0x200000);
    g_main_thread = t;
    cpu_start_watchdog();
    char vexe[512];
    snprintf(vexe, sizeof vexe, "%s\\%s", VFS_GAME_VDIR, g_exe_name);
    // Resolve the exe through the VFS: for a normal dir this is game_dir/exe; for
    // an .asar it's the copy extracted to the temp code dir by vfs_init.
    char exe_real[PATH_MAX];
    const char *exe_path = vfs_to_host(vexe, exe_real, sizeof exe_real) ? exe_real : exe_host;
    if (!pe_load_exe(exe_path, vexe, &g_exe)) return 1;
    *(uint32_t *)G2H(g_peb + 0x08) = g_exe.base; // PEB.ImageBaseAddress

    LOGI("starting game (cmdline: %s)", cmdline);
    if (!setjmp(t->exit_jmp)) {
        t->exit_code = (uint32_t)call32(t->ctx, g_exe.entry, 0, NULL);
    }
    LOGI("game main thread returned %u", t->exit_code);
    win32_process_exit(t->exit_code);
    return (int)t->exit_code;
}

// Resolve paths the way main() does, then boot+run. Used by the libretro core,
// which has only a game dir / exe name / user dir to start from.
int wl_boot_game(const char *game_dir_in, const char *exe_name, const char *user_dir_in) {
    if (exe_name && *exe_name) g_exe_name = strdup(exe_name);
    char game_dir[PATH_MAX] = "", user_dir[PATH_MAX] = "", exe[PATH_MAX];
    if (game_dir_in && *game_dir_in) snprintf(game_dir, sizeof game_dir, "%s", game_dir_in);
    if (user_dir_in && *user_dir_in) snprintf(user_dir, sizeof user_dir, "%s", user_dir_in);

    if (!*game_dir && getenv("WINELESS_GAME_DIR")) snprintf(game_dir, sizeof game_dir, "%s", getenv("WINELESS_GAME_DIR"));
    if (!*game_dir) getcwd(game_dir, sizeof game_dir);
    char real[PATH_MAX];
    if (realpath(game_dir, real)) snprintf(game_dir, sizeof game_dir, "%s", real);

    // Game packed as an .asar (or appended to this binary as a bundle): detect the
    // exe from the archive (vfs_init extracts it), set the per-game folder, boot.
    size_t gl = strlen(game_dir);
    bool is_asar = (gl > 5 && !strcasecmp(game_dir + gl - 5, ".asar")) || asar_is_bundle(game_dir);
    if (is_asar) {
        if (asar_is_bundle(game_dir)) apply_bundle_config(game_dir); // exe/display/env (lowest precedence)
        if (!g_exe_name) {
            const char *envx = getenv("WINELESS_EXE");
            static char ebuf[256];
            if (envx && *envx) g_exe_name = strdup(envx);
            else if (asar_find_exe(game_dir, ebuf, sizeof ebuf)) g_exe_name = ebuf;
            else { LOGE("no .exe in %s", game_dir); return 1; }
        }
        g_is_gtasa = !strcasecmp(g_exe_name, "gta_sa.exe");
        LOGI("game (asar): %s", g_exe_name);
        if (strcasestr(g_exe_name, "gta3")) g_user_subdir = "GTA3 User Files";
        else if (strcasestr(g_exe_name, "vc") || strcasestr(g_exe_name, "vice")) g_user_subdir = "GTA Vice City User Files";
        if (!*user_dir) {
            char docs[PATH_MAX];
            documents_dir(docs, sizeof docs);
            snprintf(user_dir, sizeof user_dir, "%s/%s", docs, g_user_subdir);
        }
        return wl_boot_game_resolved(game_dir, "", user_dir, NULL);
    }

    if (!find_exe(game_dir, exe, sizeof exe)) {
        if (g_exe_name) LOGE("%s not found in %s", g_exe_name, game_dir);
        else LOGE("no usable game exe in %s", game_dir);
        return 1;
    }
    g_is_gtasa = !strcasecmp(g_exe_name, "gta_sa.exe");
    LOGI("game: %s", g_exe_name);
    if (strcasestr(g_exe_name, "gta3")) g_user_subdir = "GTA3 User Files";
    else if (strcasestr(g_exe_name, "vc") || strcasestr(g_exe_name, "vice")) g_user_subdir = "GTA Vice City User Files";
    if (!*user_dir) {
        char docs[PATH_MAX];
        documents_dir(docs, sizeof docs);
        snprintf(user_dir, sizeof user_dir, "%s/%s", docs, g_user_subdir);
    }
    return wl_boot_game_resolved(game_dir, exe, user_dir, NULL);
}
