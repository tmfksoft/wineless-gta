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
// Not yet (P3): input and audio are still stubbed (silence; no input routing).
//
// Build: -DBUILD_LIBRETRO=ON produces wineless_libretro.so. The standalone
// wineless-gta executable does not include this file.

#include "libretro.h"
#include "../present_hook.h"
#include "../wl_boot.h"

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

void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    bool no_rom = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
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

// Map the RetroPad (port 0) onto the guest's keyboard + mouse. Digital buttons
// and the left stick drive movement/action keys; the right stick drives the
// mouse (camera/aim); the triggers are the mouse buttons (fire/aim). This is a
// sensible default for the GTA-era games (arrow-key movement, mouse look); the
// exact key choices can be refined per game.
//
// DIK scancodes: Up C8 Down D0 Left CB Right CD, Enter 1C, Esc 01, Space 39,
// LShift 2A, LCtrl 1D, Tab 0F, F 21.
static int16_t joy(unsigned id)  { return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id); }
static int16_t axis(unsigned idx, unsigned ax) {
    return input_state_cb(0, RETRO_DEVICE_ANALOG, idx, ax);
}

static void poll_input(void) {
    if (!input_state_cb) return;

    // ---- keys: digital buttons + left stick -> movement/action ----
    static const struct { unsigned id; unsigned dik; } btn[] = {
        { RETRO_DEVICE_ID_JOYPAD_UP,    0xC8 }, // Up
        { RETRO_DEVICE_ID_JOYPAD_DOWN,  0xD0 }, // Down
        { RETRO_DEVICE_ID_JOYPAD_LEFT,  0xCB }, // Left
        { RETRO_DEVICE_ID_JOYPAD_RIGHT, 0xCD }, // Right
        { RETRO_DEVICE_ID_JOYPAD_A,     0x1C }, // Enter  (select / enter-exit vehicle)
        { RETRO_DEVICE_ID_JOYPAD_START, 0x1C }, // Enter  (menu start)
        { RETRO_DEVICE_ID_JOYPAD_B,     0x39 }, // Space  (sprint / brake)
        { RETRO_DEVICE_ID_JOYPAD_Y,     0x2A }, // LShift (jump)
        { RETRO_DEVICE_ID_JOYPAD_X,     0x1D }, // LCtrl  (action/fire alt)
        { RETRO_DEVICE_ID_JOYPAD_SELECT,0x01 }, // Esc    (back / pause)
        { RETRO_DEVICE_ID_JOYPAD_L,     0x0F }, // Tab
        { RETRO_DEVICE_ID_JOYPAD_R,     0x21 }, // F
    };
    static const unsigned diks[] = { 0xC8,0xD0,0xCB,0xCD,0x1C,0x39,0x2A,0x1D,0x01,0x0F,0x21 };
    uint8_t want[256] = { 0 };
    for (unsigned i = 0; i < sizeof btn / sizeof *btn; i++)
        if (joy(btn[i].id)) want[btn[i].dik] = 1;

    const int TH = 12000; // left-stick -> arrow threshold
    int16_t lx = axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    int16_t ly = axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    if (lx < -TH) want[0xCB] = 1;  if (lx > TH) want[0xCD] = 1;
    if (ly < -TH) want[0xC8] = 1;  if (ly > TH) want[0xD0] = 1;

    for (unsigned i = 0; i < sizeof diks / sizeof *diks; i++)
        wl_key_set(diks[i], want[diks[i]]);

    // ---- right stick -> relative mouse (camera / aim) ----
    const int DZ = 6000;
    int16_t rx = axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    int16_t ry = axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    int mdx = (rx > DZ || rx < -DZ) ? rx / 1800 : 0;
    int mdy = (ry > DZ || ry < -DZ) ? ry / 1800 : 0;
    if (mdx || mdy) wl_mouse_move(mdx, mdy);

    // ---- triggers -> mouse buttons (R2/R = fire, L2/L already Tab/F above) ----
    wl_mouse_button(0, joy(RETRO_DEVICE_ID_JOYPAD_R2) != 0);  // left = fire
    wl_mouse_button(1, joy(RETRO_DEVICE_ID_JOYPAD_L2) != 0);  // right = aim
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
    if (game && game->path && game->path[0]) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", game->path);
        char *d = dirname(tmp);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", d);
        snprintf(tmp, sizeof tmp, "%s", game->path);
        char *b = basename(tmp);
        snprintf(g_exe_name, sizeof g_exe_name, "%s", b);
    }

    // Engage the present-barrier + audio bridge, then boot the guest on its own
    // thread. wl_core_audio must be set before the guest starts so dsound does
    // not open its own output.
    wl_core_audio = 1;
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
