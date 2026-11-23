// dinput8: keyboard and mouse backed by SDL. Also provides the SDL scancode ->
// VK / scancode tables used by user32's message translation.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdlib.h>

#include "com.h"
#include "../heap.h"
#include "dinput_pad.h"
#include "win32.h"

#define DI_OK 0u
#define DI_NOEFFECT 1u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED 0x80004001u

typedef struct { SDL_Scancode sc; uint8_t vk; uint8_t dik; } KeyMap;
static const KeyMap keymap[] = {
    {SDL_SCANCODE_ESCAPE, 0x1B, 0x01}, {SDL_SCANCODE_1, '1', 0x02}, {SDL_SCANCODE_2, '2', 0x03},
    {SDL_SCANCODE_3, '3', 0x04}, {SDL_SCANCODE_4, '4', 0x05}, {SDL_SCANCODE_5, '5', 0x06},
    {SDL_SCANCODE_6, '6', 0x07}, {SDL_SCANCODE_7, '7', 0x08}, {SDL_SCANCODE_8, '8', 0x09},
    {SDL_SCANCODE_9, '9', 0x0A}, {SDL_SCANCODE_0, '0', 0x0B}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C},
    {SDL_SCANCODE_EQUALS, 0xBB, 0x0D}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E}, {SDL_SCANCODE_TAB, 0x09, 0x0F},
    {SDL_SCANCODE_Q, 'Q', 0x10}, {SDL_SCANCODE_W, 'W', 0x11}, {SDL_SCANCODE_E, 'E', 0x12},
    {SDL_SCANCODE_R, 'R', 0x13}, {SDL_SCANCODE_T, 'T', 0x14}, {SDL_SCANCODE_Y, 'Y', 0x15},
    {SDL_SCANCODE_U, 'U', 0x16}, {SDL_SCANCODE_I, 'I', 0x17}, {SDL_SCANCODE_O, 'O', 0x18},
    {SDL_SCANCODE_P, 'P', 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A}, {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B},
    {SDL_SCANCODE_RETURN, 0x0D, 0x1C}, {SDL_SCANCODE_LCTRL, 0xA2, 0x1D}, {SDL_SCANCODE_A, 'A', 0x1E},
    {SDL_SCANCODE_S, 'S', 0x1F}, {SDL_SCANCODE_D, 'D', 0x20}, {SDL_SCANCODE_F, 'F', 0x21},
    {SDL_SCANCODE_G, 'G', 0x22}, {SDL_SCANCODE_H, 'H', 0x23}, {SDL_SCANCODE_J, 'J', 0x24},
    {SDL_SCANCODE_K, 'K', 0x25}, {SDL_SCANCODE_L, 'L', 0x26}, {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27},
    {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29}, {SDL_SCANCODE_LSHIFT, 0xA0, 0x2A},
    {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B}, {SDL_SCANCODE_Z, 'Z', 0x2C}, {SDL_SCANCODE_X, 'X', 0x2D},
    {SDL_SCANCODE_C, 'C', 0x2E}, {SDL_SCANCODE_V, 'V', 0x2F}, {SDL_SCANCODE_B, 'B', 0x30},
    {SDL_SCANCODE_N, 'N', 0x31}, {SDL_SCANCODE_M, 'M', 0x32}, {SDL_SCANCODE_COMMA, 0xBC, 0x33},
    {SDL_SCANCODE_PERIOD, 0xBE, 0x34}, {SDL_SCANCODE_SLASH, 0xBF, 0x35}, {SDL_SCANCODE_RSHIFT, 0xA1, 0x36},
    {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37}, {SDL_SCANCODE_LALT, 0xA4, 0x38}, {SDL_SCANCODE_SPACE, 0x20, 0x39},
    {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A}, {SDL_SCANCODE_F1, 0x70, 0x3B}, {SDL_SCANCODE_F2, 0x71, 0x3C},
    {SDL_SCANCODE_F3, 0x72, 0x3D}, {SDL_SCANCODE_F4, 0x73, 0x3E}, {SDL_SCANCODE_F5, 0x74, 0x3F},
    {SDL_SCANCODE_F6, 0x75, 0x40}, {SDL_SCANCODE_F7, 0x76, 0x41}, {SDL_SCANCODE_F8, 0x77, 0x42},
    {SDL_SCANCODE_F9, 0x78, 0x43}, {SDL_SCANCODE_F10, 0x79, 0x44}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45},
    {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46}, {SDL_SCANCODE_KP_7, 0x67, 0x47}, {SDL_SCANCODE_KP_8, 0x68, 0x48},
    {SDL_SCANCODE_KP_9, 0x69, 0x49}, {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A}, {SDL_SCANCODE_KP_4, 0x64, 0x4B},
    {SDL_SCANCODE_KP_5, 0x65, 0x4C}, {SDL_SCANCODE_KP_6, 0x66, 0x4D}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E},
    {SDL_SCANCODE_KP_1, 0x61, 0x4F}, {SDL_SCANCODE_KP_2, 0x62, 0x50}, {SDL_SCANCODE_KP_3, 0x63, 0x51},
    {SDL_SCANCODE_KP_0, 0x60, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53}, {SDL_SCANCODE_NONUSBACKSLASH, 0xE2, 0x56},
    {SDL_SCANCODE_F11, 0x7A, 0x57}, {SDL_SCANCODE_F12, 0x7B, 0x58}, {SDL_SCANCODE_KP_ENTER, 0x0D, 0x9C},
    {SDL_SCANCODE_RCTRL, 0xA3, 0x9D}, {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0xB5}, {SDL_SCANCODE_PRINTSCREEN, 0x2C, 0xB7},
    {SDL_SCANCODE_RALT, 0xA5, 0xB8}, {SDL_SCANCODE_PAUSE, 0x13, 0xC5}, {SDL_SCANCODE_HOME, 0x24, 0xC7},
    {SDL_SCANCODE_UP, 0x26, 0xC8}, {SDL_SCANCODE_PAGEUP, 0x21, 0xC9}, {SDL_SCANCODE_LEFT, 0x25, 0xCB},
    {SDL_SCANCODE_RIGHT, 0x27, 0xCD}, {SDL_SCANCODE_END, 0x23, 0xCF}, {SDL_SCANCODE_DOWN, 0x28, 0xD0},
    {SDL_SCANCODE_PAGEDOWN, 0x22, 0xD1}, {SDL_SCANCODE_INSERT, 0x2D, 0xD2}, {SDL_SCANCODE_DELETE, 0x2E, 0xD3},
    {SDL_SCANCODE_LGUI, 0x5B, 0xDB}, {SDL_SCANCODE_RGUI, 0x5C, 0xDC}, {SDL_SCANCODE_APPLICATION, 0x5D, 0xDD},
};
#define NKEYS (sizeof keymap / sizeof keymap[0])

uint32_t sdl_scancode_to_vk(SDL_Scancode sc) {
    for (unsigned i = 0; i < NKEYS; i++)
        if (keymap[i].sc == sc) {
            uint8_t vk = keymap[i].vk;
            // WM_KEYDOWN reports generic modifier VKs
            if (vk == 0xA0 || vk == 0xA1) return 0x10;
            if (vk == 0xA2 || vk == 0xA3) return 0x11;
            if (vk == 0xA4 || vk == 0xA5) return 0x12;
            return vk;
        }
    return 0;
}
// Win32 keystroke lParam bits 16-24: scan code plus the "extended key" flag,
// which RenderWare uses to tell the arrow block apart from the numpad.
uint32_t sdl_scancode_to_win_scan(SDL_Scancode sc) {
    for (unsigned i = 0; i < NKEYS; i++)
        if (keymap[i].sc == sc) return (keymap[i].dik & 0x7F) | (keymap[i].dik & 0x80 ? 0x100 : 0);
    return 0;
}

// ---------------------------------------------------------------- input state
static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t key_down[256]; // by DIK
static int32_t mouse_dx, mouse_dy, mouse_dz;
static uint8_t mouse_btn[8];
static int mouse_acquired, have_focus = 1;
static SDL_Window *last_window;

void input_sdl_event(const SDL_Event *e) {
    pthread_mutex_lock(&in_lock);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        for (unsigned i = 0; i < NKEYS; i++)
            if (keymap[i].sc == e->key.scancode) key_down[keymap[i].dik] = e->type == SDL_EVENT_KEY_DOWN ? 0x80 : 0;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_dx += (int32_t)e->motion.xrel;
        mouse_dy += (int32_t)e->motion.yrel;
        last_window = SDL_GetWindowFromID(e->motion.windowID);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        mouse_dz += (int32_t)(e->wheel.y * 120);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // DirectInput order: left, right, middle, x1, x2
        static const int map[] = {-1, 0, 2, 1, 3, 4};
        int b = e->button.button < 6 ? map[e->button.button] : -1;
        if (b >= 0) mouse_btn[b] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0x80 : 0;
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        have_focus = 1;
        last_window = SDL_GetWindowFromID(e->window.windowID);
        if (mouse_acquired && last_window) SDL_SetWindowRelativeMouseMode(last_window, true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (getenv("WINELESS_BACKGROUND")) break;
        have_focus = 0;
        memset(key_down, 0, sizeof key_down);
        memset(mouse_btn, 0, sizeof mouse_btn);
        last_window = SDL_GetWindowFromID(e->window.windowID);
        if (last_window) SDL_SetWindowRelativeMouseMode(last_window, false);
        break;
    }
    pthread_mutex_unlock(&in_lock);
}

void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t user32_main_hwnd(void);

static uint32_t dik_to_vk(unsigned dik) {
    for (unsigned i = 0; i < NKEYS; i++)
        if (keymap[i].dik == dik) return sdl_scancode_to_vk(keymap[i].sc);
    return 0;
}

// Debug: WINELESS_KEYS="18000:1c,19500:d0" presses DIK code 0x1c (Enter) at 18.0s and
// Down at 19.5s (milliseconds since start), each held for 120ms. Polled once per
// presented frame. WINELESS_CLOSE=<ms> requests a window close at that time.
#define MAX_DBG_KEYS 256
void input_debug_frame(unsigned frame) {
    static unsigned at[MAX_DBG_KEYS], key[MAX_DBG_KEYS], state[MAX_DBG_KEYS];
    static int nk, init;
    static unsigned close_at;
    if (!init) {
        init = 1;
        const char *spec = getenv("WINELESS_KEYS");
        for (const char *p = spec; p && *p && nk < MAX_DBG_KEYS;) {
            unsigned t = strtoul(p, (char **)&p, 10);
            if (*p != ':') break;
            key[nk] = strtoul(p + 1, (char **)&p, 16) & 0xff;
            at[nk++] = t;
            while (*p == ',') p++;
        }
        const char *c = getenv("WINELESS_CLOSE");
        close_at = c ? strtoul(c, NULL, 10) : 0;
    }
    uint32_t now = win32_tick_ms();
    pad_debug_tick(now);
    if (close_at && now >= close_at) {
        close_at = 0;
        LOGI("debug: requesting close (frame %u)", frame);
        SDL_Event ev = {.type = SDL_EVENT_QUIT};
        SDL_PushEvent(&ev); // same path as the user closing the window
    }
    pthread_mutex_lock(&in_lock);
    for (int i = 0; i < nk; i++) {
        unsigned k = key[i];
        uint32_t vk = dik_to_vk(k), lp = 1 | ((k & 0x7f) << 16) | (k & 0x80 ? 0x01000000u : 0);
        if (state[i] == 0 && now >= at[i]) {
            state[i] = 1;
            key_down[k] = 0x80;
            if (vk) user32_post(user32_main_hwnd(), 0x0100, vk, lp);
            LOGI("debug: key 0x%x down (%.1fs, frame %u)", k, now / 1000.0, frame);
        } else if (state[i] == 1 && now >= at[i] + 120) {
            state[i] = 2;
            key_down[k] = 0;
            if (vk) user32_post(user32_main_hwnd(), 0x0101, vk, lp | 0xC0000000u);
        }
    }
    pthread_mutex_unlock(&in_lock);
}

// libretro core: drive a DirectInput key from the frontend's input. Sets the
// DirectInput poll state (key_down) AND posts the window message on edges —
// menus read WM_KEYDOWN/WM_KEYUP, gameplay polls GetDeviceState, so both are
// needed. Called once per frame from retro_run with the current state; the
// window queue is already cross-thread safe (the movie thread posts too).
void wl_key_set(unsigned dik, int down) {
    dik &= 0xff;
    down = down ? 1 : 0;
    static uint8_t prev[256];
    if (prev[dik] == down) return; // edge-triggered
    prev[dik] = down;
    pthread_mutex_lock(&in_lock);
    key_down[dik] = down ? 0x80 : 0;
    pthread_mutex_unlock(&in_lock);
    uint32_t vk = dik_to_vk(dik);
    if (!vk) return;
    uint32_t lp = 1 | ((dik & 0x7f) << 16) | (dik & 0x80 ? 0x01000000u : 0);
    if (down) user32_post(user32_main_hwnd(), 0x0100 /*WM_KEYDOWN*/, vk, lp);
    else      user32_post(user32_main_hwnd(), 0x0101 /*WM_KEYUP*/,  vk, lp | 0xC0000000u);
}

static int vk_down(uint32_t vk) {
    for (unsigned i = 0; i < NKEYS; i++) {
        uint8_t k = keymap[i].vk;
        int match = k == vk || (vk == 0x10 && (k == 0xA0 || k == 0xA1)) || (vk == 0x11 && (k == 0xA2 || k == 0xA3)) ||
                    (vk == 0x12 && (k == 0xA4 || k == 0xA5));
        if (match && key_down[keymap[i].dik]) return 1;
    }
    if (vk == 0x01) return mouse_btn[0] != 0;
    if (vk == 0x02) return mouse_btn[1] != 0;
    if (vk == 0x04) return mouse_btn[2] != 0;
    return 0;
}
API(user32, GetKeyState, 1) { return vk_down(ARG(0)) ? 0xFF80 : 0; }
API(user32, GetAsyncKeyState, 1) { return vk_down(ARG(0)) ? 0x8000 : 0; }
API(user32, GetKeyboardState, 1) {
    uint8_t *k = PTR(0);
    for (int i = 0; i < 256; i++) k[i] = vk_down(i) ? 0x80 : 0;
    return TRUE;
}

// ---------------------------------------------------------------- devices
enum { DEV_KEYBOARD, DEV_MOUSE, DEV_PAD };
typedef struct { int type; uint32_t data_size; PadDev *pad; } Dev;

static const uint8_t SYS_D4[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_UNPLUGGED 0x80040209u

static uint32_t dev_getcaps(uint32_t *a) {
    Dev *d = com_host(a[0]);
    uint32_t *c = G2H(a[1]);
    if (d->type == DEV_PAD) return pad_getcaps(d->pad, c);
    uint32_t sz = c[0];
    memset(c + 1, 0, sz - 4);
    c[1] = 1 | 4; // DIDC_ATTACHED | DIDC_EMULATED
    if (d->type == DEV_KEYBOARD) { c[2] = 0x13 | (4 << 8); c[4] = 128; }
    else { c[2] = 0x12 | (2 << 8); c[3] = 3; c[4] = 5; }
    return DI_OK;
}
static uint32_t dev_ok(uint32_t *a) { return DI_OK; }
static uint32_t dev_getprop(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) return pad_getprop(d->pad, a[1], G2H(a[2]));
    return DIERR_UNSUPPORTED;
}
static uint32_t dev_setprop(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) return pad_setprop(d->pad, a[1], G2H(a[2]));
    return DI_OK;
}
static uint32_t dev_acquire(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_MOUSE) {
        mouse_acquired = 1;
        if (last_window && have_focus) SDL_SetWindowRelativeMouseMode(last_window, true);
    }
    if (d->type == DEV_PAD && !pad_connected(d->pad)) return DIERR_UNPLUGGED;
    return DI_OK;
}
static uint32_t dev_unacquire(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_MOUSE) {
        mouse_acquired = 0;
        if (last_window) SDL_SetWindowRelativeMouseMode(last_window, false);
    }
    return DI_OK;
}
static uint32_t dev_getstate(uint32_t *a) {
    Dev *d = com_host(a[0]);
    uint32_t n = a[1];
    uint8_t *out = G2H(a[2]);
    if (d->type == DEV_PAD) return pad_getstate(d->pad, n, out);
    memset(out, 0, n);
    pthread_mutex_lock(&in_lock);
    if (d->type == DEV_KEYBOARD) {
        memcpy(out, key_down, n < 256 ? n : 256);
    } else {
        int32_t s[3] = {mouse_dx, mouse_dy, mouse_dz};
        mouse_dx = mouse_dy = mouse_dz = 0;
        memcpy(out, s, n < 12 ? n : 12);
        if (n > 12) memcpy(out + 12, mouse_btn, n - 12 < 8 ? n - 12 : 8);
    }
    pthread_mutex_unlock(&in_lock);
    return DI_OK;
}
static uint32_t dev_getdata(uint32_t *a) {
    // Buffered data isn't produced; report zero items.
    if (a[3]) *(uint32_t *)G2H(a[3]) = 0;
    return DI_OK;
}
static uint32_t dev_setformat(uint32_t *a) {
    Dev *d = com_host(a[0]);
    uint32_t *fmt = G2H(a[1]);
    d->data_size = fmt[3];
    LOGD("dinput: SetDataFormat(type %d, data size %u)", d->type, d->data_size);
    if (d->type == DEV_PAD) return pad_setformat(d->pad, fmt);
    return DI_OK;
}
static void fill_sys_instance(int type, uint8_t *di) {
    uint32_t sz = *(uint32_t *)di;
    memset(di + 4, 0, (sz ? sz : 580) - 4);
    uint32_t d1 = type == DEV_KEYBOARD ? 0x6F1D2B61 : 0x6F1D2B60;
    memcpy(di + 4, &d1, 4); memcpy(di + 8, "\xA0\xD5\xCF\x11", 4); memcpy(di + 12, SYS_D4, 8);
    memcpy(di + 20, di + 4, 16);
    *(uint32_t *)(di + 36) = type == DEV_KEYBOARD ? 0x13 | (4 << 8) : 0x12 | (2 << 8);
    strcpy((char *)di + 40, type == DEV_KEYBOARD ? "Keyboard" : "Mouse");
    strcpy((char *)di + 300, type == DEV_KEYBOARD ? "Keyboard" : "Mouse");
}
static uint32_t dev_getinfo(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) pad_getinfo(d->pad, G2H(a[1]));
    else fill_sys_instance(d->type, G2H(a[1]));
    return DI_OK;
}
static uint32_t dev_poll(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) return pad_connected(d->pad) ? DI_OK : DIERR_INPUTLOST;
    return DI_NOEFFECT;
}
static uint32_t dev_enumobjects(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) return pad_enumobjects(d->pad, a[1], a[2], a[3]);
    return DI_OK;
}
static uint32_t dev_getobjectinfo(uint32_t *a) {
    Dev *d = com_host(a[0]);
    if (d->type == DEV_PAD) return pad_getobjectinfo(d->pad, G2H(a[1]), a[2], a[3]);
    return 0x80070002u; // DIERR_NOTFOUND
}
static uint32_t dev_ff(uint32_t *a) { return DIERR_UNSUPPORTED; }
static uint32_t dev_enumeffects(uint32_t *a) { return DI_OK; }

static uint32_t dev_vtbl;
static uint32_t make_dev(int type, PadDev *pad) {
    if (!dev_vtbl) {
        ComMethod m[32] = {
            {com_qi_self, 3}, {com_addref, 1}, {com_release_nofree, 1},
            {dev_getcaps, 2},       // GetCapabilities
            {dev_enumobjects, 4},   // EnumObjects
            {dev_getprop, 3},       // GetProperty
            {dev_setprop, 3},       // SetProperty
            {dev_acquire, 1},       // Acquire
            {dev_unacquire, 1},     // Unacquire
            {dev_getstate, 3},      // GetDeviceState
            {dev_getdata, 5},       // GetDeviceData
            {dev_setformat, 2},     // SetDataFormat
            {dev_ok, 2},            // SetEventNotification
            {dev_ok, 3},            // SetCooperativeLevel
            {dev_getobjectinfo, 4}, // GetObjectInfo
            {dev_getinfo, 2},       // GetDeviceInfo
            {dev_ok, 3},            // RunControlPanel
            {dev_ok, 4},            // Initialize
            {dev_ff, 5},            // CreateEffect (no force feedback)
            {dev_enumeffects, 4},   // EnumEffects
            {dev_ff, 3},            // GetEffectInfo
            {dev_ff, 2},            // GetForceFeedbackState
            {dev_ff, 2},            // SendForceFeedbackCommand
            {dev_enumeffects, 4},   // EnumCreatedEffectObjects
            {dev_ff, 2},            // Escape
            {dev_poll, 1},          // Poll
        };
        dev_vtbl = com_vtable("IDirectInputDevice8A", m, 32);
    }
    Dev *d = calloc(1, sizeof *d);
    d->type = type;
    d->pad = pad;
    return com_new(dev_vtbl, d, 0);
}

static uint32_t di_createdevice(uint32_t *a) {
    const uint8_t *g = G2H(a[1]);
    uint32_t d1;
    memcpy(&d1, g, 4);
    uint32_t *out = G2H(a[2]);
    if (d1 == 0x6F1D2B61) { *out = make_dev(DEV_KEYBOARD, NULL); LOGD("dinput: keyboard created"); return DI_OK; }
    if (d1 == 0x6F1D2B60) { *out = make_dev(DEV_MOUSE, NULL); LOGD("dinput: mouse created"); return DI_OK; }
    int pi = pad_find(g);
    if (pi >= 0) { *out = make_dev(DEV_PAD, pad_open(pi)); return DI_OK; }
    LOGI("dinput: CreateDevice for unknown device {%08x}", d1);
    *out = 0;
    return DIERR_DEVICENOTREG;
}

// EnumDevices(dwDevType, callback, pvRef, dwFlags)
// classes: 0 all, 1 device, 2 pointer, 3 keyboard, 4 game controllers; or a DI8DEVTYPE_*
static uint32_t di_enumdevices(uint32_t *a) {
    uint32_t type = a[1], cb = a[2], ref = a[3];
    bool want_kb = type == 0 || type == 3 || type == 0x13;
    bool want_mouse = type == 0 || type == 2 || type == 0x12;
    bool want_pad = type == 0 || type == 4 || (type >= 0x14 && type <= 0x18);
    uint32_t di = gheap_alloc(580, 1);
    GuestCtx *ctx = thread_current()->ctx;
    int found = 0;
    bool stop = false;
    for (int i = 0; i < 2 && !stop; i++) {
        if ((i == 0 && !want_kb) || (i == 1 && !want_mouse)) continue;
        *(uint32_t *)G2H(di) = 580;
        fill_sys_instance(i == 0 ? DEV_KEYBOARD : DEV_MOUSE, G2H(di));
        uint32_t args[2] = {di, ref};
        stop = !(uint32_t)call32(ctx, cb, 2, args); // DIENUM_STOP
    }
    int n = want_pad && !stop ? pad_count() : 0;
    for (int i = 0; i < n && !stop; i++) {
        *(uint32_t *)G2H(di) = 580;
        pad_instance(i, G2H(di));
        uint32_t args[2] = {di, ref};
        found++;
        stop = !(uint32_t)call32(ctx, cb, 2, args);
    }
    gheap_free(di);
    LOGD("dinput: EnumDevices(type %u): %d game controller(s)", type, found);
    return DI_OK;
}
static uint32_t di_status(uint32_t *a) {
    // GetDeviceStatus(guid): attached?
    const uint8_t *g = G2H(a[1]);
    if (pad_find(g) >= 0) return pad_count() > pad_find(g) ? DI_OK : 1; // DI_NOTATTACHED
    return DI_OK;
}
static uint32_t di_ok(uint32_t *a) { return DI_OK; }

static uint32_t di_vtbl;
API(dinput8, DirectInput8Create, 5) {
    if (!di_vtbl) {
        ComMethod m[11] = {
            {com_qi_self, 3}, {com_addref, 1}, {com_release_nofree, 1},
            {di_createdevice, 4}, // CreateDevice
            {di_enumdevices, 5},  // EnumDevices
            {di_status, 2},       // GetDeviceStatus
            {di_ok, 3},           // RunControlPanel
            {di_ok, 3},           // Initialize
        };
        di_vtbl = com_vtable("IDirectInput8A", m, 11);
    }
    pad_init();
    *(uint32_t *)PTR(3) = com_new(di_vtbl, NULL, 0);
    return DI_OK;
}

// dinput.dll (DirectX 3-7): IDirectInputA/2A/7A and their devices start with the
// same methods in the same order as the DirectInput 8 interfaces, so the same
// objects serve both.
API(dinput, DirectInputCreateA, 4) {
    LOGD("dinput: DirectInputCreateA(version 0x%x)", ARG(1));
    uint32_t args[5] = {ARG(0), ARG(1), 0, ARG(2), ARG(3)};
    return api_dinput8_DirectInput8Create(args);
}
API(dinput, DirectInputCreateEx, 5) { return api_dinput8_DirectInput8Create(_args); }
