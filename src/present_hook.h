#pragma once
#include <stdint.h>
// A hook called from the guest thread at each frame present. In the standalone
// build it is NULL and presents go to the window as usual. The libretro core
// sets it to read the just-rendered frame back to CPU pixels, hand it to the
// frontend, and block there to pace the guest (the "present-barrier").
//
// Compiled into both the executable and the core (this is not under src/libretro).

#ifdef __cplusplus
extern "C" {
#endif

// Fills dst (w*h XRGB8888, dst_pitch_px pixels per row) with the current frame.
// Returns non-zero on success. (e.g. ddraw's ddg_rt_read, or a d3d9 readback.)
typedef int (*wl_frame_reader)(uint32_t *dst, uint32_t dst_pitch_px);

// Set by the core; NULL in standalone. Called at present with the current
// frame's reader and dimensions.
extern void (*wl_present_hook)(wl_frame_reader reader, uint32_t w, uint32_t h);

// Direct-pixel variant for frames already in CPU memory as XRGB8888 (0xXXRRGGBB,
// tightly packed w*h) — e.g. decoded intro movies, which render on their own
// thread outside the guest's Present path. Set by the core; NULL in standalone.
extern void (*wl_submit_frame)(const uint32_t *xrgb, uint32_t w, uint32_t h);

// ---- audio bridge (dsound.c) ----
// When non-zero, dsound does NOT open its own output (SDL device / null thread);
// the libretro core becomes the sole consumer of the mixer and pulls samples in
// retro_run. Set by the core before the guest boots; 0 in standalone.
extern int wl_core_audio;
// Pull up to `frames` mixed interleaved-stereo s16 samples at wl_audio_rate().
// Returns frames produced. Advances the mixer, so call exactly once per frame.
int wl_audio_pull(short *out, int frames);
int wl_audio_rate(void);

// ---- input bridge (dinput8.c) ----
// Set a DirectInput keyboard key (by DIK scancode) down/up. Thread-safe. The
// guest polls this state via GetDeviceState. Used by the core to drive the guest
// from libretro input.
void wl_key_set(unsigned dik, int down);
// Relative mouse motion (camera/aim) and mouse buttons (0=L,1=R,2=M).
void wl_mouse_move(int dx, int dy);
void wl_mouse_button(int btn, int down);

// ---- native virtual gamepad (dinput_pad.c) ----
// When set (before the guest boots), a virtual Xbox-style pad is attached so the
// game sees a real controller with analog sticks. The core drives it from the
// libretro RetroPad. 0 in standalone.
extern int wl_core_pad;
// Neutral button ids (mapped to SDL gamepad buttons inside dinput_pad.c).
enum {
    WL_PAD_A, WL_PAD_B, WL_PAD_X, WL_PAD_Y, WL_PAD_BACK, WL_PAD_START,
    WL_PAD_LB, WL_PAD_RB, WL_PAD_LS, WL_PAD_RS,
    WL_PAD_UP, WL_PAD_DOWN, WL_PAD_LEFT, WL_PAD_RIGHT
};
void wl_pad_button(int which, int down);
// Sticks -32768..32767 (Y positive = down); triggers 0..32767.
void wl_pad_axes(int lx, int ly, int rx, int ry, int lt, int rt);

#ifdef __cplusplus
}
#endif
