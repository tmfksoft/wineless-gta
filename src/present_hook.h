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

// Set by the core; NULL in standalone. The bridges report how long the real D3D
// present took (ms) after the hook returned, for the core's frame-time log.
extern void (*wl_present_timing)(double present_ms);

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
// DirectInput wheel, in WHEEL_DELTA units (120 per notch).
void wl_mouse_wheel_di(int dz);

// ---- keyboard and mouse as a desk (user32.c), for frontends that send them ----
// When set (before the guest boots), the cursor is virtual: GetCursorPos /
// SetCursorPos / ClipCursor act on a position the core drives, rather than on
// SDL's pointer, which the libretro core never has. 0 in standalone.
extern int wl_core_mouse;
// Move the cursor to (x, y) in the game's own resolution. Posts WM_MOUSEMOVE and
// feeds the delta to DirectInput, so cursor-polling and DI-reading games agree.
void wl_cursor_set(int32_t x, int32_t y);
// Move the cursor by (dx, dy) game pixels (captured mouse): DirectInput gets the
// raw motion; the cursor is integrated and clamped to the game's window.
void wl_cursor_move(int dx, int dy);
// Mouse button 0=L, 1=R, 2=M, 3=X1, 4=X2 at the cursor: window message + DI.
void wl_cursor_button(int btn, int down);
// One wheel notch up (+1) or down (-1); horizontal when `horizontal`.
void wl_cursor_wheel(int notches, int horizontal);
// The game's resolution, for scaling the frontend's absolute pointer.
void wl_game_size(int32_t *w, int32_t *h);
// One typed character (a Unicode code point) as WM_CHAR, after its WM_KEYDOWN.
void wl_key_char(uint32_t codepoint);

// ---- the cursor, reported up to the frontend (user32.c; PXS-281) ----
// What the game wants from the mouse and what its cursor looks like, for a
// frontend that can capture the player's pointer or draw the cursor itself.
#define WL_CURSOR_MAX_SIDE 256
typedef struct {
    int relative;          // wants raw motion: DirectInput exclusive mouse, raw input
                           // with NOLEGACY/CAPTUREMOUSE, or hide + recentre every frame
    int visible;           // ShowCursor count >= 0 and a non-NULL SetCursor, or a shown D3D cursor
    uint32_t image_id;     // 0 = no image the core could read
    uint32_t w, h;
    int32_t hot_x, hot_y;
    const char *named;     // a CSS keyword for an IDC_* system cursor, or NULL
} wl_cursor_info;
// Fills `out`. When image_id is set and differs from `have_id`, also copies its
// w*h pixels (0xAARRGGBB, straight alpha, top row first) into `argb`, which holds
// WL_CURSOR_MAX_SIDE^2. Call from the frontend thread.
void wl_cursor_query(wl_cursor_info *out, uint32_t *argb, uint32_t have_id);
// The D3D hardware cursor's image (d3d9 bridge, SetCursorProperties).
void wl_cursor_hw_image(const uint32_t *argb, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y);
// +1 when a DirectInput mouse in exclusive mode is acquired, -1 when it is released (dinput8.c).
void wl_cursor_di_exclusive(int delta);

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
