// libretro core entry points for Wineless.
//
// P2: boots a real guest on its own thread and streams its frames to the
// frontend via a present-barrier. The guest runs its own main loop (never
// returns); at each frame present (the wl_present_hook, wired in the ddraw /
// d3d9 / d3d8 present chokepoints) it reads the just-rendered frame back to CPU
// pixels, hands it over, and blocks so the frontend paces it. retro_run() on the
// frontend thread consumes the latest frame, emits it via video_refresh, and
// releases the guest for the next one.
//
// Input: the RetroPad drives a virtual Xbox pad (plus a few menu keys), and a
// frontend that sends a keyboard and mouse (RETRO_DEVICE_KEYBOARD via the
// keyboard callback, RETRO_DEVICE_MOUSE, RETRO_DEVICE_POINTER) drives the guest's
// keyboard and a virtual cursor, as window messages and DirectInput state both.
//
// Build: -DBUILD_LIBRETRO=ON produces wineless_libretro.so. The standalone
// wineless-gta executable does not include this file.

#include "libretro.h"
#include "../asar.h"
#include "../present_hook.h"
#include "../wl_boot.h"

#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void log_msg(int level, const char *fmt, ...);
#define LOGI(...) log_msg(2, __VA_ARGS__)
#define LOGE(...) log_msg(0, __VA_ARGS__)

// ---- Output defaults (until the guest's real resolution is known) ----
#define WL_DEF_W      640
#define WL_DEF_H      480
#define WL_MAX_W      2048
#define WL_MAX_H      1152
#define WL_FB_FPS     60.0
#define WL_SAMPLE_RATE 44100.0
#define WL_SAMPLE_RATE_I 44100
#define WL_MIN_FPS_I     50

// libretro frontend callbacks
static retro_environment_t   env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;

// ---- present-barrier state ----
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv_ready    = PTHREAD_COND_INITIALIZER; // guest -> frontend
static pthread_cond_t  cv_consumed = PTHREAD_COND_INITIALIZER; // frontend -> guest
static uint32_t *latest;      // guest writes the just-rendered frame here (w-packed)
static uint32_t *fb;          // frontend's copy, fed to video_refresh / duplicated
static int cur_w = WL_DEF_W, cur_h = WL_DEF_H;  // dims of the latest frame
static int rep_w = WL_DEF_W, rep_h = WL_DEF_H;  // dims last reported to the frontend
static bool frame_ready;      // latest holds an unconsumed frame
static bool have_frame;       // fb holds a valid frame to show
static bool guest_started;

// ---- guest thread ----
static pthread_t guest_tid;
static char g_game_dir[4096], g_exe_name[256], g_user_dir[4096];

static void *guest_main(void *arg) {
    (void)arg;
    // Runs the whole guest to completion on this thread (never returns until the
    // game exits). Paths empty -> wl_boot_game falls back to env/cwd.
    int rc = wl_boot_game(g_game_dir[0] ? g_game_dir : NULL,
                          g_exe_name[0] ? g_exe_name : NULL,
                          g_user_dir[0] ? g_user_dir : NULL);
    LOGI("libretro: guest exited (%d)", rc);
    return NULL;
}

// Publish one frame into `latest` and block until the frontend consumes it,
// pacing the producer to the frontend's retro_run cadence. The frame comes
// either from a reader callback (reads a GPU backbuffer) or a packed CPU buffer
// (decoded movies). Called on the guest thread or the movie decode thread; the
// two don't present concurrently (the main thread is idle during intro movies).
static void publish_frame(wl_frame_reader reader, const uint32_t *src, uint32_t w, uint32_t h) {
    if ((!reader && !src) || !w || !h) return;
    if (w > WL_MAX_W) w = WL_MAX_W;
    if (h > WL_MAX_H) h = WL_MAX_H;
    pthread_mutex_lock(&mx);
    if (reader) reader(latest, w);                     // read backbuffer into latest (w-packed)
    else        memcpy(latest, src, (size_t)w * h * 4); // copy packed CPU pixels
    cur_w = (int)w;
    cur_h = (int)h;
    frame_ready = true;
    pthread_cond_signal(&cv_ready);
    while (frame_ready)
        pthread_cond_wait(&cv_consumed, &mx);
    pthread_mutex_unlock(&mx);
}

// Present-barrier hooks (see present_hook.h). GPU backbuffer path and the
// direct-pixel path (decoded movies) both funnel through publish_frame.
static void core_present(wl_frame_reader reader, uint32_t w, uint32_t h) {
    publish_frame(reader, NULL, w, h);
}
static void core_submit(const uint32_t *xrgb, uint32_t w, uint32_t h) {
    publish_frame(NULL, xrgb, w, h);
}

// ---- mandatory libretro API ----

// ---- keyboard (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK) ----

// libretro RETROK_* -> DirectInput DIK (= the Windows set-1 scan code, 0x80 for
// the extended keys). 0 = no Windows key for it.
static uint8_t retrok_to_dik(unsigned k) {
    if (k >= RETROK_a && k <= RETROK_z) {
        static const uint8_t letters[26] = {
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return letters[k - RETROK_a];
    }
    if (k >= RETROK_1 && k <= RETROK_9) return (uint8_t)(0x02 + (k - RETROK_1));
    if (k >= RETROK_F1 && k <= RETROK_F10) return (uint8_t)(0x3B + (k - RETROK_F1));
    switch (k) {
    case RETROK_0: return 0x0B;
    case RETROK_ESCAPE: return 0x01;
    case RETROK_MINUS: return 0x0C;
    case RETROK_EQUALS: return 0x0D;
    case RETROK_BACKSPACE: return 0x0E;
    case RETROK_TAB: return 0x0F;
    case RETROK_LEFTBRACKET: return 0x1A;
    case RETROK_RIGHTBRACKET: return 0x1B;
    case RETROK_RETURN: return 0x1C;
    case RETROK_LCTRL: return 0x1D;
    case RETROK_SEMICOLON: return 0x27;
    case RETROK_QUOTE: return 0x28;
    case RETROK_BACKQUOTE: return 0x29;
    case RETROK_LSHIFT: return 0x2A;
    case RETROK_BACKSLASH: return 0x2B;
    case RETROK_COMMA: return 0x33;
    case RETROK_PERIOD: return 0x34;
    case RETROK_SLASH: return 0x35;
    case RETROK_RSHIFT: return 0x36;
    case RETROK_KP_MULTIPLY: return 0x37;
    case RETROK_LALT: return 0x38;
    case RETROK_SPACE: return 0x39;
    case RETROK_CAPSLOCK: return 0x3A;
    case RETROK_NUMLOCK: return 0x45;
    case RETROK_SCROLLOCK: return 0x46;
    case RETROK_KP7: return 0x47;
    case RETROK_KP8: return 0x48;
    case RETROK_KP9: return 0x49;
    case RETROK_KP_MINUS: return 0x4A;
    case RETROK_KP4: return 0x4B;
    case RETROK_KP5: return 0x4C;
    case RETROK_KP6: return 0x4D;
    case RETROK_KP_PLUS: return 0x4E;
    case RETROK_KP1: return 0x4F;
    case RETROK_KP2: return 0x50;
    case RETROK_KP3: return 0x51;
    case RETROK_KP0: return 0x52;
    case RETROK_KP_PERIOD: return 0x53;
    case RETROK_OEM_102: return 0x56;
    case RETROK_F11: return 0x57;
    case RETROK_F12: return 0x58;
    case RETROK_KP_ENTER: return 0x9C;
    case RETROK_RCTRL: return 0x9D;
    case RETROK_KP_DIVIDE: return 0xB5;
    case RETROK_PRINT: return 0xB7;
    case RETROK_RALT: return 0xB8;
    case RETROK_PAUSE: return 0xC5;
    case RETROK_HOME: return 0xC7;
    case RETROK_UP: return 0xC8;
    case RETROK_PAGEUP: return 0xC9;
    case RETROK_LEFT: return 0xCB;
    case RETROK_RIGHT: return 0xCD;
    case RETROK_END: return 0xCF;
    case RETROK_DOWN: return 0xD0;
    case RETROK_PAGEDOWN: return 0xD1;
    case RETROK_INSERT: return 0xD2;
    case RETROK_DELETE: return 0xD3;
    case RETROK_MENU: return 0xDD;
    default: return 0;
    }
}

// Held by the keyboard and by the RetroPad's menu overlay (poll_input), by DIK.
// A key is down for the guest while either holds it, so a pad press can't release
// a typed key or the reverse. Both are written on the frontend thread only: the
// keyboard callback runs inside input_poll_cb, which retro_run calls.
static uint8_t kb_down[256], pad_down[256];

static void apply_key(unsigned dik) { wl_key_set(dik, kb_down[dik] || pad_down[dik]); }

static void RETRO_CALLCONV keyboard_event(bool down, unsigned keycode, uint32_t character,
                                          uint16_t key_modifiers) {
    (void)key_modifiers;
    uint8_t dik = retrok_to_dik(keycode);
    if (dik) {
        kb_down[dik] = down;
        apply_key(dik);
    }
    // WM_CHAR after its WM_KEYDOWN, in order. Auto-repeat presses come through
    // here too (with the key already down), which is how a held key types.
    if (down && character) wl_key_char(character);
}

void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    bool no_rom = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
    struct retro_keyboard_callback kcb = {keyboard_event};
    cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kcb);
}
void retro_set_video_refresh(retro_video_refresh_t cb)      { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)        { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)           { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)         { input_state_cb = cb; }

void retro_init(void) {
    latest = calloc((size_t)WL_MAX_W * WL_MAX_H, 4);
    fb     = calloc((size_t)WL_MAX_W * WL_MAX_H, 4);
}
void retro_deinit(void) {
    // Unhook the present-barrier and release the guest if it is blocked there.
    pthread_mutex_lock(&mx);
    wl_present_hook = NULL;
    wl_submit_frame = NULL;
    frame_ready = false;
    pthread_cond_signal(&cv_consumed);
    pthread_mutex_unlock(&mx);

    // The guest runs an infinite game loop on its own thread that cannot be
    // unwound cleanly. retro_deinit means the session is over (the frontend has
    // already finalised its output), so running the host's C++ static destructors
    // now — while the guest thread is still executing inside DXVK/SDL — just races
    // into a crash. Terminate the process immediately with a clean status instead.
    if (guest_started) {
        fflush(NULL);
        _exit(0);
    }
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Wineless";
    info->library_version  = "0.2-p2";
    info->valid_extensions = "exe";
    info->need_fullpath    = true;
    info->block_extract    = true;
}

static void fill_av_info(struct retro_system_av_info *info, int w, int h) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = w;
    info->geometry.base_height  = h;
    info->geometry.max_width    = WL_MAX_W;
    info->geometry.max_height   = WL_MAX_H;
    info->geometry.aspect_ratio = (float)w / (float)h;
    info->timing.fps            = WL_FB_FPS;
    info->timing.sample_rate    = WL_SAMPLE_RATE;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    fill_av_info(info, rep_w, rep_h);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) {}

// Drive the native virtual gamepad from the RetroPad (port 0), so the game sees a
// real Xbox-style controller with analog sticks and triggers. RetroPad face
// buttons use the standard positional (Nintendo<->Xbox) swap so a physical pad
// maps 1:1. This gives true analog movement / camera for the GTA-era games.
static int16_t joy(unsigned id)  { return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id); }
static int16_t axis(unsigned idx, unsigned ax) {
    return input_state_cb(0, RETRO_DEVICE_ANALOG, idx, ax);
}

// ---- mouse ----
//
// Two ways in, and the frontend sends whichever the player is using:
//  - absolute (RTS, point-and-click): RETRO_DEVICE_POINTER, -32767..32767 over the
//    picture. Applied only when it changes, because a frontend with no mouse
//    reports a constant (0,0) and that must not park the cursor mid-screen;
//  - captured (FPS): RETRO_DEVICE_MOUSE X/Y deltas, raw to DirectInput and
//    integrated into the cursor.
// Buttons and the wheel come from RETRO_DEVICE_MOUSE either way.
static int16_t mouse(unsigned id) { return input_state_cb(0, RETRO_DEVICE_MOUSE, 0, id); }

static void poll_mouse(void) {
    static int16_t last_px, last_py;
    int16_t px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    int16_t py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    if (px != last_px || py != last_py) {
        last_px = px;
        last_py = py;
        int32_t gw, gh;
        wl_game_size(&gw, &gh);
        wl_cursor_set((int32_t)(((int64_t)px + 32767) * (gw - 1) / 65534),
                      (int32_t)(((int64_t)py + 32767) * (gh - 1) / 65534));
    }

    int dx = mouse(RETRO_DEVICE_ID_MOUSE_X), dy = mouse(RETRO_DEVICE_ID_MOUSE_Y);
    if (dx || dy) wl_cursor_move(dx, dy);

    static const unsigned ids[5] = {RETRO_DEVICE_ID_MOUSE_LEFT, RETRO_DEVICE_ID_MOUSE_RIGHT,
                                    RETRO_DEVICE_ID_MOUSE_MIDDLE, RETRO_DEVICE_ID_MOUSE_BUTTON_4,
                                    RETRO_DEVICE_ID_MOUSE_BUTTON_5};
    static bool held[5];
    for (int b = 0; b < 5; b++) {
        bool now = mouse(ids[b]) != 0;
        if (now != held[b]) {
            held[b] = now;
            wl_cursor_button(b, now);
        }
    }

    if (mouse(RETRO_DEVICE_ID_MOUSE_WHEELUP))         wl_cursor_wheel(1, 0);
    if (mouse(RETRO_DEVICE_ID_MOUSE_WHEELDOWN))       wl_cursor_wheel(-1, 0);
    if (mouse(RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP))   wl_cursor_wheel(1, 1);
    if (mouse(RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN)) wl_cursor_wheel(-1, 1);
}

static void poll_input(void) {
    if (!input_state_cb) return;

    static const struct { unsigned id; int pad; } bmap[] = {
        { RETRO_DEVICE_ID_JOYPAD_B,      WL_PAD_A },     // bottom face -> Xbox A
        { RETRO_DEVICE_ID_JOYPAD_A,      WL_PAD_B },     // right face  -> Xbox B
        { RETRO_DEVICE_ID_JOYPAD_Y,      WL_PAD_X },     // left face   -> Xbox X
        { RETRO_DEVICE_ID_JOYPAD_X,      WL_PAD_Y },     // top face    -> Xbox Y
        { RETRO_DEVICE_ID_JOYPAD_SELECT, WL_PAD_BACK },
        { RETRO_DEVICE_ID_JOYPAD_START,  WL_PAD_START },
        { RETRO_DEVICE_ID_JOYPAD_L,      WL_PAD_LB },
        { RETRO_DEVICE_ID_JOYPAD_R,      WL_PAD_RB },
        { RETRO_DEVICE_ID_JOYPAD_L3,     WL_PAD_LS },
        { RETRO_DEVICE_ID_JOYPAD_R3,     WL_PAD_RS },
        { RETRO_DEVICE_ID_JOYPAD_UP,     WL_PAD_UP },
        { RETRO_DEVICE_ID_JOYPAD_DOWN,   WL_PAD_DOWN },
        { RETRO_DEVICE_ID_JOYPAD_LEFT,   WL_PAD_LEFT },
        { RETRO_DEVICE_ID_JOYPAD_RIGHT,  WL_PAD_RIGHT },
    };
    for (unsigned i = 0; i < sizeof bmap / sizeof *bmap; i++)
        wl_pad_button(bmap[i].pad, joy(bmap[i].id) != 0);

    int lx = axis(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X);
    int ly = axis(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y);
    int rx = axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    int ry = axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    int lt = joy(RETRO_DEVICE_ID_JOYPAD_L2) ? 32767 : 0;
    int rt = joy(RETRO_DEVICE_ID_JOYPAD_R2) ? 32767 : 0;
    wl_pad_axes(lx, ly, rx, ry, lt, rt);

    // The GTA-era menus are keyboard-driven (they ignore the pad), so overlay a
    // few keys for menu use: d-pad -> arrows, START -> Enter (accept), SELECT ->
    // Esc (back). Face buttons stay pad-only so in-game actions don't also fire
    // menu keys. (d-pad also moves in-game, redundant with the left stick.)
    pad_down[0xC8] = joy(RETRO_DEVICE_ID_JOYPAD_UP)    != 0;
    pad_down[0xD0] = joy(RETRO_DEVICE_ID_JOYPAD_DOWN)  != 0;
    pad_down[0xCB] = joy(RETRO_DEVICE_ID_JOYPAD_LEFT)  != 0;
    pad_down[0xCD] = joy(RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0;
    pad_down[0x1C] = joy(RETRO_DEVICE_ID_JOYPAD_START) != 0; // Enter
    pad_down[0x01] = joy(RETRO_DEVICE_ID_JOYPAD_SELECT) != 0; // Esc
    static const uint8_t overlay[] = {0xC8, 0xD0, 0xCB, 0xCD, 0x1C, 0x01};
    for (unsigned i = 0; i < sizeof overlay; i++) apply_key(overlay[i]);

    poll_mouse();
}

// ---- the core loop ----

void retro_run(void) {
    if (input_poll_cb) input_poll_cb();
    poll_input();

    // Wait briefly for a fresh guest frame; if none (guest loading / slow),
    // duplicate the last frame so the stream keeps flowing.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 20 * 1000 * 1000;              // ~20ms (one 50fps frame)
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }

    int w = rep_w, h = rep_h;
    pthread_mutex_lock(&mx);
    while (!frame_ready) {
        if (pthread_cond_timedwait(&cv_ready, &mx, &ts) == ETIMEDOUT) break;
    }
    if (frame_ready) {
        w = cur_w; h = cur_h;
        memcpy(fb, latest, (size_t)w * h * 4);
        have_frame = true;
        frame_ready = false;                      // release the guest
        pthread_cond_signal(&cv_consumed);
    }
    pthread_mutex_unlock(&mx);

    // If the guest's resolution changed, tell the frontend before presenting.
    if (have_frame && (w != rep_w || h != rep_h)) {
        struct retro_system_av_info av;
        fill_av_info(&av, w, h);
        if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        rep_w = w; rep_h = h;
    }

    if (have_frame && video_cb)
        video_cb(fb, w, h, (size_t)w * 4);
    else if (video_cb)
        video_cb(NULL, rep_w, rep_h, (size_t)rep_w * 4); // dupe (frontend repeats)

    // Drain the guest's audio mixer (DirectSound + movies + waveout) and hand it
    // to the frontend. One pull per frame keeps A/V in step: ~sample_rate/fps
    // samples each retro_run, whether or not a new video frame arrived.
    if (audio_batch_cb) {
        static int16_t ab[2 * 2048];
        int want = (int)(WL_SAMPLE_RATE / WL_FB_FPS);
        int got = wl_audio_pull(ab, want);
        if (got > 0) audio_batch_cb(ab, (size_t)got);
    }
}

// ---- content lifecycle ----

bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb && !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        LOGE("libretro: frontend does not support XRGB8888");
        return false;
    }

    // Derive game dir + exe from --game <path/to/game.exe>; else wl_boot_game
    // falls back to $WINELESS_GAME_DIR / $WINELESS_EXE / cwd.
    // Self-contained bundle: this .so has a game appended (see --make-bundle).
    // Boot from the .so itself, no --game needed.
    Dl_info info;
    if (dladdr((void *)retro_load_game, &info) && info.dli_fname && asar_is_bundle(info.dli_fname)) {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", info.dli_fname);
        g_exe_name[0] = 0;
        LOGI("libretro: self-contained bundle %s", info.dli_fname);
    } else if (game && game->path && game->path[0]) {
        const char *p = game->path;
        size_t l = strlen(p);
        if (l > 5 && strcasecmp(p + l - 5, ".asar") == 0) {
            // A whole game packed as one .asar — wl_boot_game mounts it and finds
            // the exe inside. (WINELESS_EXE can still pin a specific one.)
            snprintf(g_game_dir, sizeof g_game_dir, "%s", p);
            g_exe_name[0] = 0;
        } else {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s", p);
            snprintf(g_game_dir, sizeof g_game_dir, "%s", dirname(tmp));
            snprintf(tmp, sizeof tmp, "%s", p);
            snprintf(g_exe_name, sizeof g_exe_name, "%s", basename(tmp));
        }
    }

    // Engage the present-barrier + audio bridge, then boot the guest on its own
    // thread. wl_core_audio must be set before the guest starts so dsound does
    // not open its own output.
    wl_core_audio = 1;
    wl_core_pad = 1;
    wl_core_mouse = 1;
    wl_present_hook = core_present;
    wl_submit_frame = core_submit;
    if (pthread_create(&guest_tid, NULL, guest_main, NULL) != 0) {
        LOGE("libretro: failed to start guest thread: %s", strerror(errno));
        wl_present_hook = NULL;
        wl_submit_frame = NULL;
        return false;
    }
    guest_started = true;
    LOGI("libretro: guest thread started (game_dir='%s' exe='%s')",
         g_game_dir, g_exe_name);
    return true;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) {
    (void)t; (void)i; (void)n; return false;
}
void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// ---- memory / save state: unsupported (live Win32 process) ----
size_t retro_serialize_size(void) { return 0; }
bool   retro_serialize(void *d, size_t s)   { (void)d; (void)s; return false; }
bool   retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
