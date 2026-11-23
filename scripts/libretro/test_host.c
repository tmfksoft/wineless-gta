// Minimal libretro host for testing the Wineless core headlessly, including the
// input path (a real mp4 recording has no input source). It dlopens the core,
// feeds a scripted RetroPad, and dumps frames to BMP.
//
//   test_host <core.so> <game.exe> <out_dir> [frames]
//
// Input script: holds START+A in pulses after the menu should be up, to drive
// "START GAME". Dumps a frame every DUMP_EVERY frames.
//
// Build: cc -O2 -o test_host test_host.c -ldl   (run under Xvfb for DXVK).

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define DUMP_EVERY 150

static void *core;
static void (*p_set_environment)(retro_environment_t);
static void (*p_set_video)(retro_video_refresh_t);
static void (*p_set_audio_batch)(retro_audio_sample_batch_t);
static void (*p_set_input_poll)(retro_input_poll_t);
static void (*p_set_input_state)(retro_input_state_t);
static void (*p_init)(void);
static void (*p_run)(void);
static bool (*p_load_game)(const struct retro_game_info *);
static void (*p_get_av)(struct retro_system_av_info *);

static const char *g_out;
static unsigned g_frame;
static const char *g_game;

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true; // accept whatever (we expect XRGB8888)
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data; if (v) v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        const char **s = data; if (s) *s = "."; return true;
    }
    default: return false;
    }
}

static void save_bmp(const uint32_t *px, int w, int h, int pitch_px) {
    char path[512];
    snprintf(path, sizeof path, "%s/host_frame_%06u.bmp", g_out, g_frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = w * 3, pad = (4 - row % 4) % 4, img = (row + pad) * h;
    uint8_t hdr[54] = { 'B','M' };
    uint32_t v;
    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *r = px + (size_t)y * pitch_px;
        for (int x = 0; x < w; x++) {
            uint32_t p = r[x]; uint8_t bgr[3] = { p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff };
            fwrite(bgr, 1, 3, f);
        }
        static const uint8_t z[3] = { 0 };
        fwrite(z, 1, pad, f);
    }
    fclose(f);
    fprintf(stderr, "[host] dumped %s\n", path);
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (data && (g_frame % DUMP_EVERY) == 0)
        save_bmp(data, w, h, (int)(pitch / 4));
}
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; return frames; }
static void input_poll_cb(void) {}

// Scripted input: after the menu should be up (~frame 120), pulse START+A to
// drive "START GAME". id per RetroPad.
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)port; (void)idx;
    if (dev != RETRO_DEVICE_JOYPAD) return 0;
    bool pulse = g_frame > 120 && ((g_frame / 25) % 2 == 0); // 25 on, 25 off
    if (pulse && (id == RETRO_DEVICE_ID_JOYPAD_START || id == RETRO_DEVICE_ID_JOYPAD_A))
        return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <core.so> <game.exe> <out_dir> [frames]\n", argv[0]); return 2; }
    const char *so = argv[1];
    g_game = argv[2];
    g_out = argv[3];
    unsigned nframes = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 1500;

    core = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define SYM(v, n) *(void **)&v = dlsym(core, n); if (!v) { fprintf(stderr, "missing %s\n", n); return 1; }
    SYM(p_set_environment, "retro_set_environment");
    SYM(p_set_video, "retro_set_video_refresh");
    SYM(p_set_audio_batch, "retro_set_audio_sample_batch");
    SYM(p_set_input_poll, "retro_set_input_poll");
    SYM(p_set_input_state, "retro_set_input_state");
    SYM(p_init, "retro_init");
    SYM(p_run, "retro_run");
    SYM(p_load_game, "retro_load_game");
    SYM(p_get_av, "retro_get_system_av_info");

    p_set_environment(env_cb);
    p_set_video(video_cb);
    p_set_audio_batch(audio_batch_cb);
    p_set_input_poll(input_poll_cb);
    p_set_input_state(input_state_cb);
    p_init();

    struct retro_game_info gi = { .path = g_game };
    if (!p_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }
    struct retro_system_av_info av; p_get_av(&av);
    fprintf(stderr, "[host] av: %ux%u @ %.1ffps\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps);

    for (g_frame = 0; g_frame < nframes; g_frame++)
        p_run();
    fprintf(stderr, "[host] done %u frames\n", nframes);
    return 0;
}
