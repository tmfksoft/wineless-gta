// libretro core entry points for Wineless.
//
// P2: boots a real guest on its own thread and streams its frames to the
// frontend. The guest runs its own main loop (never returns); at each frame
// present (the wl_present_hook, wired in the ddraw / d3d9 / d3d8 present
// chokepoints) it reads the just-rendered frame back to CPU pixels, hands it
// over, and sleeps to the next tick of its own 60 Hz clock (the frame cap).
// retro_run() on the frontend thread takes the newest frame if there is one and
// never waits for the guest, so the frontend holds 60 fps and real-time audio
// whatever the game's own frame rate.
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

// ---- frame hand-off state ----
//
// The guest and the frontend run on their own clocks. The guest renders as fast
// as it can, up to a 60 Hz cap enforced on its own thread (a software vsync in
// core_present); the frontend's retro_run never waits for it. Frames move through
// three buffers so neither side copies under the lock: the guest reads back into
// `back`, then swaps it with `latest`; retro_run swaps `latest` with `fb` when a
// new one is there. A frame the frontend has not taken yet is simply replaced.
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv_run = PTHREAD_COND_INITIALIZER; // retro_run -> a guest parked while paused
static uint32_t *back;        // guest thread's scratch: the frame being read back
static uint32_t *latest;      // newest complete guest frame, not yet taken
static uint32_t *fb;          // frontend's frame, fed to video_refresh
static int cur_w = WL_DEF_W, cur_h = WL_DEF_H;  // dims of `latest`
static int fb_w = WL_DEF_W, fb_h = WL_DEF_H;    // dims of `fb`
static int rep_w = WL_DEF_W, rep_h = WL_DEF_H;  // dims last reported to the frontend
static bool frame_ready;      // latest holds a frame retro_run has not taken
static bool have_frame;       // fb holds a valid frame to show
static bool guest_started;
static bool shutting_down;
static bool guest_exited;     // the game ended the process; retro_run asks the frontend to stop
static uint32_t guest_exit_code;
static uint64_t last_run_ns;  // when retro_run last ran (0 = never), under mx

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// The guest's frame cap. 60 for every game for now: the stream is 60 fps, so a
// guest frame faster than that would only be dropped.
#define WL_VSYNC_NS (1000000000ull / 60ull)
// retro_run silent for this long means the frontend is paused (or not started):
// park the guest at its next present, as the old blocking barrier used to.
#define WL_PAUSE_NS (250ull * 1000000ull)

// Present-path timing, logged every 5 s next to the bridge's own fps line, so a
// slow game says where its frame time goes: its own work, the readback, or the
// vsync wait. (The bridges no longer do a real D3D present in core mode.)
static struct {
    uint64_t window_start, last_present;
    unsigned presents, dropped;
    double game_ms, readback_ms, vsync_ms, worst_gap_ms;
} pt;

static void present_stats(uint64_t t) {
    if (!pt.window_start) { pt.window_start = t; return; }
    double win = (double)(t - pt.window_start) / 1e9;
    if (win < 5.0) return;
    if (pt.presents) {
        double n = pt.presents;
        LOGI("libretro: %.1f presents/s; per frame game %.1fms, readback %.1fms, vsync wait %.1fms; worst gap %.0fms; %u not taken",
             n / win, pt.game_ms / n, pt.readback_ms / n, pt.vsync_ms / n, pt.worst_gap_ms, pt.dropped);
    }
    uint64_t lp = pt.last_present;
    memset(&pt, 0, sizeof pt);
    pt.window_start = t;
    pt.last_present = lp;
}

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

// Publish one frame: fill `back` outside the lock, then swap it in as `latest`.
// Never waits for the frontend (that is what made the old barrier drag the
// frontend's loop down to the guest's speed). Called on the guest thread or the
// movie decode thread; the two don't present concurrently (the main thread is
// idle during intro movies).
static void publish_frame(wl_frame_reader reader, const uint32_t *src, uint32_t w, uint32_t h) {
    if ((!reader && !src) || !w || !h || !back) return;
    if (w > WL_MAX_W) w = WL_MAX_W;
    if (h > WL_MAX_H) h = WL_MAX_H;
    if (reader) {
        if (!reader(back, w)) return;                  // unreadable: keep showing the last frame
    } else {
        memcpy(back, src, (size_t)w * h * 4);
    }
    pthread_mutex_lock(&mx);
    uint32_t *t = latest; latest = back; back = t;
    if (frame_ready) pt.dropped++;
    cur_w = (int)w;
    cur_h = (int)h;
    frame_ready = true;
    pthread_mutex_unlock(&mx);
}

// While the frontend is paused (or has not started running frames), hold the
// guest here so the game pauses with it, as it did under the blocking barrier.
static void park_while_paused(void) {
    pthread_mutex_lock(&mx);
    while (!shutting_down && (!last_run_ns || now_ns() - last_run_ns > WL_PAUSE_NS)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&cv_run, &mx, &ts);
    }
    pthread_mutex_unlock(&mx);
}

// The guest's present (d3d9 / d3d8 / ddraw): hand the frame over, then sleep to
// the next 60 Hz tick of the guest's own clock. That is the frame cap; a game
// slower than 60 never sleeps here and runs at whatever rate it manages.
static void core_present(wl_frame_reader reader, uint32_t w, uint32_t h) {
    static uint64_t next_vsync;
    uint64_t t0 = now_ns();
    if (pt.last_present) {
        double gap = (double)(t0 - pt.last_present) / 1e6;
        pt.game_ms += gap;
        if (gap > pt.worst_gap_ms) pt.worst_gap_ms = gap;
    }
    publish_frame(reader, NULL, w, h);
    uint64_t t1 = now_ns();
    pt.readback_ms += (double)(t1 - t0) / 1e6;

    park_while_paused();

    uint64_t t2 = now_ns();
    if (!next_vsync || t2 > next_vsync + WL_VSYNC_NS) next_vsync = t2; // late: re-phase, don't burst
    next_vsync += WL_VSYNC_NS;
    if (next_vsync > t2) {
        struct timespec ts = {(time_t)(next_vsync / 1000000000ull), (long)(next_vsync % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    uint64_t t3 = now_ns();
    pt.vsync_ms += (double)(t3 - t2) / 1e6;
    pt.presents++;
    pt.last_present = t3;
    present_stats(t3);
    // game_ms measures from here to the next present, so it excludes this wait.
}
// The game called ExitProcess (or its main thread returned). Exiting the host
// process from here would kill the frontend mid-stream: the client never hears
// the session stopped, the recording and saves are lost, and the node reads it
// as a crash. Instead flag it for retro_run, which asks the frontend to shut down
// (RETRO_ENVIRONMENT_SHUTDOWN), and park this guest thread for good; retro_deinit
// ends the process once the frontend has finished.
static void core_process_exit(uint32_t code) {
    pthread_mutex_lock(&mx);
    guest_exited = true;
    guest_exit_code = code;
    pthread_cond_broadcast(&cv_run);
    for (;;) pthread_cond_wait(&cv_run, &mx);
}

// Decoded intro movies pace themselves (dshow.c sleeps to the movie's rate).
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
    back   = calloc((size_t)WL_MAX_W * WL_MAX_H, 4);
    latest = calloc((size_t)WL_MAX_W * WL_MAX_H, 4);
    fb     = calloc((size_t)WL_MAX_W * WL_MAX_H, 4);
}
void retro_deinit(void) {
    // Unhook the present path and release the guest if it is parked there.
    pthread_mutex_lock(&mx);
    wl_present_hook = NULL;
    wl_submit_frame = NULL;
    shutting_down = true;
    pthread_cond_broadcast(&cv_run);
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

// ---- the cursor, reported up (PXS-281) ----
//
// What the game wants from the mouse (relative = capture the player's pointer),
// whether it shows a cursor, and what that looks like, so the client can capture
// or draw the cursor itself. A private environment call: Pixelstream's frontend
// (libretro-frontend src/input/cursor_state.h, which owns this contract) answers
// it; any other frontend says no, and the core stops asking.
#define RETRO_ENVIRONMENT_PIXELSTREAM_SET_CURSOR_STATE (0x5058 | RETRO_ENVIRONMENT_PRIVATE)
struct retro_pixelstream_cursor_state {
    unsigned version;     // 1
    unsigned mode;        // 0 unknown, 1 absolute, 2 relative
    bool visible;
    uint32_t image_id;    // 0 = no image
    unsigned width, height;
    int hot_x, hot_y;
    const uint32_t *argb; // width*height 0xAARRGGBB, straight alpha, top row first; read during the call
    const char *named;    // CSS keyword for a standard cursor, or NULL
};

// Sent on change only; the frontend repeats it for late joiners.
static void report_cursor(void) {
    static bool off, sent;
    static wl_cursor_info last;
    static uint32_t px[WL_CURSOR_MAX_SIDE * WL_CURSOR_MAX_SIDE];
    static uint32_t have_id; // the image px holds
    if (off || !env_cb) return;
    wl_cursor_info c;
    wl_cursor_query(&c, px, have_id);
    if (c.image_id) have_id = c.image_id;
    if (sent && c.relative == last.relative && c.visible == last.visible && c.image_id == last.image_id &&
        c.named == last.named)
        return;
    struct retro_pixelstream_cursor_state s = {
        1, c.relative ? 2u : 1u, c.visible != 0, c.image_id, c.w, c.h, c.hot_x, c.hot_y,
        c.image_id ? px : NULL, c.named,
    };
    if (!env_cb(RETRO_ENVIRONMENT_PIXELSTREAM_SET_CURSOR_STATE, &s)) {
        off = true;
        LOGI("libretro: frontend does not take cursor reports");
        return;
    }
    if (!sent || c.relative != last.relative)
        LOGI("libretro: cursor mode %s", c.relative ? "relative" : "absolute");
    last = c;
    sent = true;
}

// ---- the core loop ----

void retro_run(void) {
    if (input_poll_cb) input_poll_cb();
    poll_input();
    report_cursor();

    // Take the newest guest frame if there is one; never wait for it. With no new
    // frame the last one is shown again, so the frontend's loop (and the audio it
    // carries) stays at 60 whatever the game's own frame rate is.
    uint64_t t = now_ns();
    pthread_mutex_lock(&mx);
    if (guest_exited) {
        static bool asked;
        uint32_t code = guest_exit_code;
        pthread_mutex_unlock(&mx);
        if (!asked) {
            asked = true;
            LOGI("libretro: game exited (%u); asking the frontend to shut down", code);
            if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL)) {
                // A frontend that cannot shut down on request: end as before.
                LOGI("libretro: frontend has no SHUTDOWN; exiting");
                fflush(NULL);
                _exit((int)code);
            }
        }
        if (video_cb) video_cb(NULL, rep_w, rep_h, (size_t)rep_w * 4);
        return;
    }
    uint64_t prev_run = last_run_ns;
    last_run_ns = t;
    if (frame_ready) {
        uint32_t *sw = fb; fb = latest; latest = sw;
        fb_w = cur_w; fb_h = cur_h;
        have_frame = true;
        frame_ready = false;
    }
    pthread_cond_broadcast(&cv_run);
    pthread_mutex_unlock(&mx);
    int w = fb_w, h = fb_h;

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

    // Drain the guest's audio mixer (DirectSound + movies + waveout) by the wall
    // clock: the samples owed since the last retro_run, so a late frame costs no
    // audio and the mixer advances in real time, as the game's own clock does.
    // A long gap (paused, first run) restarts at one frame's worth rather than
    // dumping a backlog.
    if (audio_batch_cb) {
        static int16_t ab[2 * 2048];
        static double owed;
        double frame_samples = WL_SAMPLE_RATE / WL_FB_FPS;
        if (!prev_run || t - prev_run > WL_PAUSE_NS) {
            owed = 0.0;
            prev_run = t - WL_VSYNC_NS;
        }
        owed += (double)(t - prev_run) / 1e9 * WL_SAMPLE_RATE;
        if (owed > 4.0 * frame_samples) owed = 4.0 * frame_samples; // bound a hiccup's catch-up
        int want = (int)owed;
        if (want > 2048) want = 2048;
        if (want > 0) {
            int got = wl_audio_pull(ab, want);
            if (got > 0) audio_batch_cb(ab, (size_t)got);
            owed -= want;
        }
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
    wl_process_exit_hook = core_process_exit;
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
