// user32: windows, messages, input state. First pass: a headless window model;
// the SDL-backed window and event translation plug in behind the same state.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "../heap.h"
#include "display.h"
#include "guest_printf.h"
#include "win32.h"

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

typedef struct {
    char name[64];
    uint32_t wndproc, style, hinst;
} WndClass;

typedef struct {
    uint32_t hwnd;
    uint32_t wndproc, style, exstyle, userdata, hinst, id;
    int32_t x, y, w, h;
    int visible;
    int dirty; // needs WM_PAINT (InvalidateRect until validated)
    char title[128];
    SDL_Window *sdl;
} Window;

static uint32_t desk_w(void);
static uint32_t desk_h(void);

static WndClass classes[16];
static int nclasses;
static Window windows[8];
static int nwindows;
static int cursor_count;
static uint32_t focus_hwnd, capture_hwnd;

void user32_init(void) {}

static void sdl_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    SDL_SetHint(SDL_HINT_APP_NAME, "GTA San Andreas");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    // HIDAPI joystick discovery runs on every event pump and has deadlocked here;
    // gamepads go through evdev instead (dinput_pad.c initializes them).
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    // A minimized window gets no swapchain images, which blocks Present forever.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        fatal("SDL_Init failed: %s", SDL_GetError());
    LOGI("SDL %d.%d.%d, video driver: %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
         SDL_GetCurrentVideoDriver());
    display_init();
}
static uint32_t desk_w(void) { uint32_t w, h; sdl_init_once(); display_desktop(&w, &h, NULL); return w; }
static uint32_t desk_h(void) { uint32_t w, h; sdl_init_once(); display_desktop(&w, &h, NULL); return h; }

// for the D3D9 bridge
void *wineless_window_sdl(uint32_t hwnd);
uint32_t wineless_window_from_sdl(void *sdl);

static Window *win_get(uint32_t h) {
    for (int i = 0; i < nwindows; i++)
        if (windows[i].hwnd == h) return &windows[i];
    return NULL;
}

void *wineless_window_sdl(uint32_t hwnd) {
    Window *w = win_get(hwnd);
    if (!w && nwindows) w = &windows[0];
    return w ? w->sdl : NULL;
}
uint32_t wineless_window_from_sdl(void *sdl) {
    for (int i = 0; i < nwindows; i++)
        if (windows[i].sdl == sdl) return windows[i].hwnd;
    return 0;
}

static uint32_t call_wndproc(Window *w, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!w || !w->wndproc) return 0;
    uint32_t args[4] = {w->hwnd, msg, wp, lp};
    return (uint32_t)call32(thread_current()->ctx, w->wndproc, 4, args);
}

// ---------------------------------------------------------------- classes/windows
API(user32, RegisterClassA, 1) {
    uint32_t *wc = PTR(0);
    if (nclasses == 16) return 0;
    WndClass *c = &classes[nclasses++];
    c->style = wc[0];
    c->wndproc = wc[1];
    c->hinst = wc[4];
    snprintf(c->name, sizeof c->name, "%s", wc[9] >= 0x10000 ? (const char *)G2H(wc[9]) : "#atom");
    LOGD("RegisterClassA(%s, wndproc=0x%x)", c->name, c->wndproc);
    return 0xC000 + nclasses;
}
API(user32, RegisterClassW, 1) {
    uint32_t *wc = PTR(0);
    if (nclasses == 16) return 0;
    WndClass *c = &classes[nclasses++];
    c->style = wc[0];
    c->wndproc = wc[1];
    c->hinst = wc[4];
    if (wc[9] >= 0x10000) w2a(G2H(wc[9]), c->name, sizeof c->name);
    else snprintf(c->name, sizeof c->name, "#atom");
    LOGD("RegisterClassW(%s, wndproc=0x%x)", c->name, c->wndproc);
    return 0xC000 + nclasses;
}
API(user32, UnregisterClassA, 2) { return TRUE; }
API(user32, CreateWindowExA, 12) {
    const char *cls = ARG(1) >= 0x10000 ? STR(1) : NULL;
    WndClass *c = NULL;
    for (int i = 0; i < nclasses; i++)
        if ((cls && !strcasecmp(classes[i].name, cls)) || (!cls && ARG(1) == 0xC001u + i)) c = &classes[i];
    if (!c || nwindows == 8) {
        LOGW("CreateWindowExA: unknown class %s", cls ? cls : "(atom)");
        return 0;
    }
    Window *w = &windows[nwindows++];
    memset(w, 0, sizeof *w);
    w->hwnd = 0x00010010 + nwindows * 0x10;
    w->wndproc = c->wndproc;
    w->exstyle = ARG(0);
    w->style = ARG(3);
    w->x = (int32_t)ARG(4) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(4);
    w->y = (int32_t)ARG(5) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(5);
    w->w = (int32_t)ARG(6) == (int32_t)0x80000000 ? 640 : (int32_t)ARG(6);
    w->h = (int32_t)ARG(7) == (int32_t)0x80000000 ? 480 : (int32_t)ARG(7);
    w->hinst = ARG(10);
    snprintf(w->title, sizeof w->title, "%s", ARG(2) ? STR(2) : "");
    if (!(w->style & 0x40000000)) { // not WS_CHILD: a real top-level window
        // The real size/fullscreen state is set when the game creates its D3D device
        // (wineless_set_game_resolution); until then a hidden window of the requested size.
        sdl_init_once();
        w->x = w->y = 0;
        w->sdl = SDL_CreateWindow(w->title, w->w, w->h, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!w->sdl) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_StartTextInput(w->sdl); // typed characters for WM_CHAR
    }
    LOGI("CreateWindowExA('%s', class %s, %dx%d style 0x%x) -> 0x%x", w->title, c->name, w->w, w->h, w->style, w->hwnd);
    // CREATESTRUCTA for WM_NCCREATE / WM_CREATE
    uint32_t cs = gheap_alloc(48, 1);
    uint32_t *p = G2H(cs);
    p[0] = ARG(11); p[1] = ARG(10); p[2] = ARG(9); p[3] = ARG(8);
    p[4] = w->h; p[5] = w->w; p[6] = w->y; p[7] = w->x;
    p[8] = w->style; p[9] = ARG(2); p[10] = ARG(1); p[11] = w->exstyle;
    call_wndproc(w, 0x0081 /*WM_NCCREATE*/, 0, cs);
    call_wndproc(w, 0x0001 /*WM_CREATE*/, 0, cs);
    gheap_free(cs);
    return w->hwnd;
}
API(user32, DestroyWindow, 1) {
    Window *w = win_get(ARG(0));
    if (w) call_wndproc(w, 0x0002 /*WM_DESTROY*/, 0, 0);
    return TRUE;
}
API(user32, ShowWindow, 2) {
    Window *w = win_get(ARG(0));
    int prev = w ? w->visible : 0;
    if (w) {
        w->visible = ARG(1) != 0;
        if (w->sdl) {
            if (w->visible) { SDL_ShowWindow(w->sdl); SDL_RaiseWindow(w->sdl); }
            else SDL_HideWindow(w->sdl);
        }
    }
    return prev;
}
// Painting: InvalidateRect marks a window; UpdateWindow sends WM_PAINT at once,
// and the message loop produces it when the queue is empty, until the window is
// validated (BeginPaint, ValidateRect, DefWindowProc).
API(user32, UpdateWindow, 1) {
    Window *w = win_get(ARG(0));
    if (w && w->dirty) call_wndproc(w, 0x000F /*WM_PAINT*/, 0, 0);
    return TRUE;
}
API(user32, IsIconic, 1) { return FALSE; }
API(user32, IsZoomed, 1) { return FALSE; }
API(user32, IsWindowVisible, 1) { Window *w = win_get(ARG(0)); return w && w->visible; }
API(user32, GetMenu, 1) { return 0; }
API(user32, SetWindowTextA, 2) {
    Window *w = win_get(ARG(0));
    if (w) {
        snprintf(w->title, sizeof w->title, "%s", STR(1));
        if (w->sdl) SDL_SetWindowTitle(w->sdl, w->title);
    }
    return TRUE;
}
API(user32, GetWindowLongA, 2) {
    Window *w = win_get(ARG(0));
    if (!w) return 0;
    switch ((int32_t)ARG(1)) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_STYLE: return w->style;
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_ID: return w->id;
    }
    return 0;
}
API(user32, SetWindowLongA, 3) {
    Window *w = win_get(ARG(0));
    if (!w) return 0;
    uint32_t *f = NULL;
    switch ((int32_t)ARG(1)) {
    case GWL_WNDPROC: f = &w->wndproc; break;
    case GWL_STYLE: f = &w->style; break;
    case GWL_EXSTYLE: f = &w->exstyle; break;
    case GWL_USERDATA: f = &w->userdata; break;
    case GWL_ID: f = &w->id; break;
    default: return 0;
    }
    uint32_t prev = *f;
    *f = ARG(2);
    return prev;
}
API(user32, SetWindowPos, 7) {
    Window *w = win_get(ARG(0));
    uint32_t flags = ARG(6);
    if (w) {
        if (!(flags & 0x2 /*SWP_NOMOVE*/)) { w->x = ARG(2); w->y = ARG(3); }
        // size is fixed at 640x480 for now
    }
    return TRUE;
}
API(user32, AdjustWindowRect, 3) { return TRUE; }
API(user32, AdjustWindowRectEx, 4) { return TRUE; }
static void put_rect(uint32_t *r, int32_t l, int32_t t, int32_t rr, int32_t b) { r[0] = l; r[1] = t; r[2] = rr; r[3] = b; }
API(user32, GetWindowRect, 2) {
    Window *w = win_get(ARG(0));
    if (!w) { put_rect(PTR(1), 0, 0, desk_w(), desk_h()); return TRUE; }
    put_rect(PTR(1), w->x, w->y, w->x + w->w, w->y + w->h);
    return TRUE;
}
API(user32, GetClientRect, 2) {
    Window *w = win_get(ARG(0));
    if (!w) return FALSE;
    put_rect(PTR(1), 0, 0, w->w, w->h);
    return TRUE;
}
API(user32, ClientToScreen, 2) {
    Window *w = win_get(ARG(0));
    int32_t *pt = PTR(1);
    if (w) { pt[0] += w->x; pt[1] += w->y; }
    return TRUE;
}
API(user32, ScreenToClient, 2) {
    Window *w = win_get(ARG(0));
    int32_t *pt = PTR(1);
    if (w) { pt[0] -= w->x; pt[1] -= w->y; }
    return TRUE;
}
API(user32, GetWindowPlacement, 2) {
    uint32_t *wp = PTR(1);
    Window *w = win_get(ARG(0));
    wp[1] = 0; wp[2] = 1; // flags, showCmd=SW_SHOWNORMAL
    wp[3] = wp[4] = wp[5] = wp[6] = 0;
    if (w) put_rect(wp + 7, w->x, w->y, w->x + w->w, w->y + w->h);
    return TRUE;
}
API(user32, SetWindowPlacement, 2) { return TRUE; }
API(user32, GetWindowTextA, 3) {
    Window *w = win_get(ARG(0));
    if (!ARG(2)) return 0;
    snprintf(PTR(1), ARG(2), "%s", w ? w->title : "");
    return strlen(STR(1));
}
API(user32, GetWindowTextW, 3) {
    Window *w = win_get(ARG(0));
    if (!ARG(2)) return 0;
    a2w(w ? w->title : "", PTR(1), ARG(2));
    return wlen(WSTR(1));
}
API(user32, GetWindowTextLengthW, 1) { Window *w = win_get(ARG(0)); return w ? strlen(w->title) : 0; }
API(user32, GetWindowTextLengthA, 1) { Window *w = win_get(ARG(0)); return w ? strlen(w->title) : 0; }
// The real window's size follows the display policy; only remember the request.
API(user32, MoveWindow, 6) {
    Window *w = win_get(ARG(0));
    if (w && !w->sdl) { w->x = ARG(1); w->y = ARG(2); w->w = ARG(3); w->h = ARG(4); }
    return TRUE;
}
API(user32, BringWindowToTop, 1) {
    Window *w = win_get(ARG(0));
    if (w && w->sdl) SDL_RaiseWindow(w->sdl);
    return TRUE;
}
API(user32, ValidateRect, 2) {
    Window *w = win_get(ARG(0));
    if (w) w->dirty = 0;
    return TRUE;
}
API(user32, GetLastActivePopup, 1) { return ARG(0); }
API(user32, IsWindow, 1) { return win_get(ARG(0)) != NULL; }
API(user32, GetTopWindow, 1) { return nwindows ? windows[0].hwnd : 0; }
API(user32, GetWindow, 2) { return 0; }
API(user32, GetWindowThreadProcessId, 2) {
    if (ARG(1)) *(uint32_t *)PTR(1) = getpid();
    return g_main_thread ? g_main_thread->tid : 0;
}
API(user32, GetWindowLongW, 2) { return api_user32_GetWindowLongA(_args); }
API(user32, SetWindowLongW, 3) { return api_user32_SetWindowLongA(_args); }
API(user32, SendMessageW, 4) { return call_wndproc(win_get(ARG(0)), ARG(1), ARG(2), ARG(3)); }
API(user32, PostThreadMessageA, 4) { return TRUE; }
API(user32, LoadIconW, 2) { return 0x00020000 + (ARG(1) & 0xffff); }
API(user32, MapWindowPoints, 4) { return 0; }
API(user32, IsDlgButtonChecked, 2) { return 0; }
API(user32, DialogBoxParamW, 5) {
    LOGW("DialogBoxParamW: dialogs not supported, returning IDOK");
    return 1;
}
// Wide windows share the ANSI path with the class name and title narrowed
// (CREATESTRUCT then carries the narrow copies).
API(user32, CreateWindowExW, 12) {
    uint32_t a[12];
    memcpy(a, _args, sizeof a);
    char buf[256];
    uint32_t cls = 0, title = 0;
    if (ARG(1) >= 0x10000) { w2a(WSTR(1), buf, sizeof buf); a[1] = cls = gheap_strdup(buf); }
    if (ARG(2)) { w2a(WSTR(2), buf, sizeof buf); a[2] = title = gheap_strdup(buf); }
    uint32_t r = api_user32_CreateWindowExA(a);
    if (cls) gheap_free(cls);
    if (title) gheap_free(title);
    return r;
}
// EnumDisplaySettings: ENUM_CURRENT/REGISTRY_SETTINGS (-1/-2) give the desktop
// mode, indexes 0.. the display's modes (32-bit). `wide` shifts the fields
// after DEVMODEW's longer device and form names.
static uint32_t enum_display_settings(uint32_t idx, uint8_t *dm, bool wide) {
    uint32_t w, h, hz;
    if (idx == 0xFFFFFFFFu || idx == 0xFFFFFFFEu) display_desktop(&w, &h, &hz);
    else if ((int)idx < display_mode_count()) display_mode(idx, &w, &h, &hz);
    else return FALSE;
    int k = wide ? 32 : 0;   // after dmDeviceName
    int k2 = wide ? 64 : 0;  // after dmFormName
    *(uint16_t *)(dm + 36 + k) = wide ? 220 : 156;            // dmSize
    *(uint32_t *)(dm + 40 + k) = 0x40000 | 0x80000 | 0x100000 | 0x400000; // dmFields
    *(uint32_t *)(dm + 104 + k2) = 32;                       // dmBitsPerPel
    *(uint32_t *)(dm + 108 + k2) = w;                        // dmPelsWidth
    *(uint32_t *)(dm + 112 + k2) = h;
    *(uint32_t *)(dm + 116 + k2) = 0;                        // dmDisplayFlags
    *(uint32_t *)(dm + 120 + k2) = hz;                       // dmDisplayFrequency
    return TRUE;
}
API(user32, EnumDisplaySettingsA, 3) { return enum_display_settings(ARG(1), PTR(2), false); }
API(user32, EnumDisplaySettingsW, 3) { return enum_display_settings(ARG(1), PTR(2), true); }

API(user32, GetDesktopWindow, 0) { return 0x00010000; }
API(user32, FindWindowA, 2) { return 0; }
API(user32, SetForegroundWindow, 1) { return TRUE; }
API(user32, GetForegroundWindow, 0) { return nwindows ? windows[0].hwnd : 0; }
API(user32, GetActiveWindow, 0) { return nwindows ? windows[0].hwnd : 0; }
API(user32, SetFocus, 1) { uint32_t p = focus_hwnd; focus_hwnd = ARG(0); return p; }
API(user32, GetFocus, 0) { return focus_hwnd; }
static SDL_Window *main_sdl(void) { return nwindows ? windows[0].sdl : NULL; }
// Hide the OS cursor when the game hid it (ShowCursor count < 0) or set a NULL
// cursor: games like Age of Empires II HD draw their own cursor and hide the
// system one, so we must not leave the Linux pointer on top.
int wineless_d3d9_cursor_shown(void); // d3d9 bridge: a hardware cursor overlay is active
static bool cursor_null;
static void apply_cursor_visibility(void) {
    if (wineless_d3d9_cursor_shown()) return; // the D3D9 hardware cursor owns visibility
    if (cursor_count < 0 || cursor_null) SDL_HideCursor();
    else SDL_ShowCursor();
}
API(user32, SetCapture, 1) { uint32_t p = capture_hwnd; capture_hwnd = ARG(0); return p; }
API(user32, ReleaseCapture, 0) { capture_hwnd = 0; return TRUE; }
// ClipCursor(rect): confine the pointer to the window (edge scrolling, own
// cursor); NULL releases it.
API(user32, ClipCursor, 1) {
    SDL_Window *w = main_sdl();
    if (w) SDL_SetWindowMouseGrab(w, ARG(0) != 0);
    return TRUE;
}
API(user32, SetCursor, 1) {
    cursor_null = ARG(0) == 0;
    apply_cursor_visibility();
    return 0x00030000;
}
API(user32, SwapMouseButton, 1) { return FALSE; } // we never swap the buttons
API(user32, GetDoubleClickTime, 0) { return 500; }
API(user32, SetDoubleClickTime, 1) { return TRUE; }
API(user32, ShowCursor, 1) {
    cursor_count += ARG(0) ? 1 : -1;
    apply_cursor_visibility();
    return cursor_count;
}
static void mouse_to_game(Window *w, float x, float y, int32_t *gx, int32_t *gy);
// The game window sits at screen (0,0) in the game's view, so screen = client.
API(user32, GetCursorPos, 1) {
    int32_t *p = PTR(0);
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    mouse_to_game(nwindows ? &windows[0] : NULL, x, y, &p[0], &p[1]);
    return TRUE;
}
API(user32, SetCursorPos, 2) {
    Window *w = nwindows ? &windows[0] : NULL;
    if (!w || !w->sdl || !(SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_INPUT_FOCUS)) return TRUE;
    int ww, wh;
    SDL_GetWindowSize(w->sdl, &ww, &wh);
    float x = (float)(int32_t)ARG(0), y = (float)(int32_t)ARG(1);
    if (w->w > 0 && w->h > 0) { x = x * ww / w->w; y = y * wh / w->h; }
    SDL_WarpMouseInWindow(w->sdl, x, y);
    return TRUE;
}
API(user32, LoadIconA, 2) { return 0x00020000 + (ARG(1) & 0xffff); }
API(user32, LoadCursorA, 2) { return 0x00030000 + (ARG(1) & 0xffff); }
API(user32, SetTimer, 4) { return ARG(1) ? ARG(1) : 1; }
API(user32, KillTimer, 2) { return TRUE; }
API(user32, GetSystemMetrics, 1) {
    switch (ARG(0)) {
    case 0: case 16: case 78: return desk_w(); // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 17: case 79: return desk_h();
    }
    return 0;
}
API(user32, SystemParametersInfoA, 4) {
    if (ARG(2)) {
        switch (ARG(0)) {
        case 0x10: /* SPI_GETSCREENSAVEACTIVE */ case 0x3A: /* SPI_GETSTICKYKEYS */ case 0x34: /* FILTERKEYS */
        case 0x3C: /* SPI_GETTOGGLEKEYS */ break;
        default: break;
        }
    }
    return TRUE;
}

API(user32, SystemParametersInfoW, 4) { return api_user32_SystemParametersInfoA(_args); }

// ---------------------------------------------------------------- messages
// ---------------------------------------------------------------- message queue
typedef struct { uint32_t hwnd, msg, wp, lp, time, x, y; } Msg;
#define QSIZE 1024
static Msg queue[QSIZE];
static int qhead, qtail;
static int quit_posted;
static uint32_t quit_code;

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER; // movie thread posts too

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&q_lock);
    int n = (qtail + 1) % QSIZE;
    if (n != qhead) { // full: drop
        queue[qtail] = (Msg){hwnd, msg, wp, lp, win32_tick_ms(), 0, 0};
        qtail = n;
    }
    pthread_mutex_unlock(&q_lock);
}

void dsound_set_paused(int paused);
uint32_t sdl_scancode_to_vk(SDL_Scancode sc);
uint32_t sdl_scancode_to_win_scan(SDL_Scancode sc);
void input_sdl_event(const SDL_Event *e); // dinput8.c

// gta_sa.exe 1.0 US: the audio engine's event volume table (loaded from
// AUDIO\CONFIG\EVENTVOL.DAT). The game's shutdown plays a sound and reads it
// unconditionally, so closing during the logos/movies (before it is loaded)
// crashes. Nothing can be lost at that point, so exit directly instead.
#define GTASA_EVENT_VOLUMES 0x00BD00F8u

extern bool g_is_gtasa;
static void request_close(uint32_t hwnd) {
    if (g_is_gtasa && *(volatile uint32_t *)G2H(GTASA_EVENT_VOLUMES) == 0) {
        LOGI("close requested before the game finished starting; exiting directly");
        win32_process_exit(0);
    }
    post(hwnd, 0x0010 /*WM_CLOSE*/, 0, 0);
}

// Mouse position in the game's coordinates: the real window can be larger than
// the game's resolution (borderless/scaled fullscreen), so scale.
static void mouse_to_game(Window *w, float x, float y, int32_t *gx, int32_t *gy) {
    int ww = 0, wh = 0;
    if (w && w->sdl) SDL_GetWindowSize(w->sdl, &ww, &wh);
    *gx = ww > 0 && w->w > 0 ? (int32_t)(x * w->w / ww) : (int32_t)x;
    *gy = wh > 0 && w->h > 0 ? (int32_t)(y * w->h / wh) : (int32_t)y;
}
// MK_* flags for mouse messages' wParam
static uint32_t mouse_keys(void) {
    SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
    SDL_Keymod m = SDL_GetModState();
    return (b & SDL_BUTTON_LMASK ? 0x1 : 0) | (b & SDL_BUTTON_RMASK ? 0x2 : 0) | (m & SDL_KMOD_SHIFT ? 0x4 : 0) |
           (m & SDL_KMOD_CTRL ? 0x8 : 0) | (b & SDL_BUTTON_MMASK ? 0x10 : 0);
}
static uint32_t mouse_lparam(Window *w, float x, float y) {
    int32_t gx, gy;
    mouse_to_game(w, x, y, &gx, &gy);
    return ((uint32_t)gy << 16) | ((uint32_t)gx & 0xffff);
}

static void pump_sdl(void) {
    if (!nwindows || !windows[0].sdl) return;
    uint32_t hwnd = windows[0].hwnd;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        input_sdl_event(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            request_close(hwnd);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            // keep the pointer from wandering onto (and focusing) other windows
            if (display_policy() != DISPLAY_WINDOWED) SDL_SetWindowMouseGrab(windows[0].sdl, true);
            dsound_set_paused(0);
            post(hwnd, 0x001C /*WM_ACTIVATEAPP*/, 1, 0);
            post(hwnd, 0x0006 /*WM_ACTIVATE*/, 1, 0);
            post(hwnd, 0x0007 /*WM_SETFOCUS*/, 0, 0);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            display_window_moved(windows[0].sdl);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            SDL_SetWindowMouseGrab(windows[0].sdl, false);
            // WINELESS_BACKGROUND=1: keep the game running (don't tell it it lost focus)
            if (getenv("WINELESS_BACKGROUND")) break;
            dsound_set_paused(1); // silence a looping buffer while paused in the background
            post(hwnd, 0x0006, 0, 0);
            post(hwnd, 0x001C, 0, 0);
            post(hwnd, 0x0008 /*WM_KILLFOCUS*/, 0, 0);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint32_t vk = sdl_scancode_to_vk(e.key.scancode);
            if (!vk) break;
            int up = e.type == SDL_EVENT_KEY_UP;
            int sys = (e.key.mod & SDL_KMOD_ALT) || vk == 0x12;
            uint32_t lp = 1 | (sdl_scancode_to_win_scan(e.key.scancode) << 16) | (up ? 0xC0000000u : 0) |
                          (e.key.repeat ? 0x40000000u : 0);
            post(hwnd, (sys ? 0x0104 : 0x0100) + up, vk, lp);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: { // UTF-8 -> one WM_CHAR per UTF-16 unit
            const uint8_t *t = (const uint8_t *)e.text.text;
            while (*t) {
                uint32_t c = *t++;
                if (c >= 0xF0) { c = (c & 7) << 18 | (t[0] & 63) << 12 | (t[1] & 63) << 6 | (t[2] & 63); t += 3; }
                else if (c >= 0xE0) { c = (c & 15) << 12 | (t[0] & 63) << 6 | (t[1] & 63); t += 2; }
                else if (c >= 0xC0) { c = (c & 31) << 6 | (t[0] & 63); t += 1; }
                if (c >= 0x10000) {
                    c -= 0x10000;
                    post(hwnd, 0x0102, 0xD800 | (c >> 10), 1);
                    post(hwnd, 0x0102, 0xDC00 | (c & 0x3FF), 1);
                } else post(hwnd, 0x0102, c, 1);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            post(hwnd, 0x0200, mouse_keys(), mouse_lparam(&windows[0], e.motion.x, e.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            static const uint32_t base[] = {0, 0x0201, 0x0207, 0x0204};
            if (e.button.button >= 1 && e.button.button <= 3)
                post(hwnd, base[e.button.button] + (e.type == SDL_EVENT_MOUSE_BUTTON_UP), mouse_keys(),
                     mouse_lparam(&windows[0], e.button.x, e.button.y));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            post(hwnd, 0x020A, ((uint32_t)((int)(e.wheel.y * 120)) << 16) | mouse_keys(),
                 mouse_lparam(&windows[0], e.wheel.mouse_x, e.wheel.mouse_y));
            break;
        }
    }
}

// The game's view of its window always matches the resolution it picked; the
// real SDL window follows the display policy (see display.c).
void wineless_set_game_resolution(uint32_t hwnd, uint32_t w, uint32_t h, int game_wants_fullscreen) {
    Window *win = win_get(hwnd);
    if (!win && nwindows) win = &windows[0];
    if (!win) return;
    win->w = w;
    win->h = h;
    win->x = win->y = 0;
    display_apply(win->sdl, w, h, game_wants_fullscreen);
}

// Presenting to a minimized/hidden window blocks in vkAcquireNextImage, so the
// D3D9 bridge skips presents while this returns 0 (the game keeps running).
int wineless_window_presentable(void) {
    if (!nwindows || !windows[0].sdl) return 1;
    SDL_WindowFlags f = SDL_GetWindowFlags(windows[0].sdl);
    return !(f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED));
}

// Leave fullscreen and shut SDL down so a real (X11) mode switch is undone.
void user32_shutdown(void) {
    for (int i = 0; i < nwindows; i++)
        if (windows[i].sdl) SDL_SetWindowFullscreen(windows[i].sdl, false);
    SDL_Quit();
}

uint32_t user32_main_hwnd(void) { return nwindows ? windows[0].hwnd : 0; }
void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post(hwnd, msg, wp, lp); }

static int take_msg(uint32_t *m, int remove) {
    pthread_mutex_lock(&q_lock);
    int r = 0;
    if (qhead == qtail) {
        if (quit_posted) {
            memset(m, 0, 28);
            m[1] = 0x0012; // WM_QUIT
            m[2] = quit_code;
            if (remove) quit_posted = 0;
            r = 1;
        }
    } else {
        Msg *q = &queue[qhead];
        m[0] = q->hwnd; m[1] = q->msg; m[2] = q->wp; m[3] = q->lp; m[4] = q->time; m[5] = q->x; m[6] = q->y;
        if (remove) qhead = (qhead + 1) % QSIZE;
        r = 1;
    }
    pthread_mutex_unlock(&q_lock);
    if (!r)
        for (int i = 0; i < nwindows; i++)
            if (windows[i].dirty) { // WM_PAINT stays pending until the window is validated
                memset(m, 0, 28);
                m[0] = windows[i].hwnd;
                m[1] = 0x000F;
                m[4] = win32_tick_ms();
                return 1;
            }
    return r;
}

void input_debug_frame(unsigned frame);

// Debug: WINELESS_CLICKS="2000:400:300,2500:400:350" left-clicks at (x,y) in the
// game's resolution at the given time (ms). For scripting through menus in tests.
static void clicks_debug(void) {
    static unsigned at[64], cx[64], cy[64], state[64];
    static int nc, init;
    if (!init) {
        init = 1;
        for (const char *p = getenv("WINELESS_CLICKS"); p && *p && nc < 64;) {
            at[nc] = strtoul(p, (char **)&p, 10);
            if (*p != ':') break;
            cx[nc] = strtoul(p + 1, (char **)&p, 10);
            if (*p != ':') break;
            cy[nc] = strtoul(p + 1, (char **)&p, 10);
            nc++;
            while (*p == ',') p++;
        }
    }
    uint32_t now = win32_tick_ms();
    for (int i = 0; i < nc; i++) {
        uint32_t lp = (cy[i] << 16) | (cx[i] & 0xffff);
        if (!state[i] && now >= at[i]) {
            state[i] = 1;
            Window *w = nwindows ? &windows[0] : NULL;
            if (w && w->sdl && w->w > 0) { // move the real pointer too (GetCursorPos)
                int ww, wh;
                SDL_GetWindowSize(w->sdl, &ww, &wh);
                SDL_WarpMouseInWindow(w->sdl, (float)cx[i] * ww / w->w, (float)cy[i] * wh / w->h);
            }
            post(windows[0].hwnd, 0x0200 /*WM_MOUSEMOVE*/, 0, lp);
            post(windows[0].hwnd, 0x0201 /*WM_LBUTTONDOWN*/, 1, lp);
            LOGI("debug: click at %u,%u (%.1fs)", cx[i], cy[i], now / 1000.0);
        } else if (state[i] == 1 && now >= at[i] + 80) {
            state[i] = 2;
            post(windows[0].hwnd, 0x0202 /*WM_LBUTTONUP*/, 0, lp);
        }
    }
}

API(user32, PeekMessageA, 5) {
    static unsigned n;
    input_debug_frame(++n); // debug key/close injection (time based)
    clicks_debug();
    pump_sdl();
    return take_msg(PTR(0), ARG(4) & 1);
}
API(user32, GetMessageA, 4) {
    for (;;) {
        pump_sdl();
        if (take_msg(PTR(0), 1)) return ((uint32_t *)PTR(0))[1] != 0x0012;
        SDL_WaitEventTimeout(NULL, 10);
    }
}
API(user32, PeekMessageW, 5) { return api_user32_PeekMessageA(_args); }
API(user32, WaitMessage, 0) {
    for (;;) {
        pump_sdl();
        pthread_mutex_lock(&q_lock);
        int have = qhead != qtail || quit_posted;
        pthread_mutex_unlock(&q_lock);
        if (have) return TRUE;
        SDL_WaitEventTimeout(NULL, 10);
    }
}
// Typed characters arrive as SDL text events (posted as WM_CHAR by pump_sdl);
// here only the control keys that Windows turns into characters.
API(user32, TranslateMessage, 1) {
    uint32_t *m = PTR(0);
    if (m[1] != 0x0100 /*WM_KEYDOWN*/) return FALSE;
    uint32_t ch = m[2] == 0x0D ? 0x0D : m[2] == 0x08 ? 0x08 : m[2] == 0x1B ? 0x1B : m[2] == 0x09 ? 0x09 : 0;
    if (!ch) return FALSE;
    post(m[0], 0x0102 /*WM_CHAR*/, ch, m[3]);
    return TRUE;
}
API(user32, DispatchMessageA, 1) {
    uint32_t *m = PTR(0);
    return call_wndproc(win_get(m[0]), m[1], m[2], m[3]);
}
API(user32, SendMessageA, 4) { return call_wndproc(win_get(ARG(0)), ARG(1), ARG(2), ARG(3)); }
API(user32, PostMessageA, 4) { post(ARG(0), ARG(1), ARG(2), ARG(3)); return TRUE; }
API(user32, PostQuitMessage, 1) { quit_posted = 1; quit_code = ARG(0); return 0; }
API(user32, DefWindowProcA, 4) {
    if (ARG(1) == 0x0081) return TRUE; // WM_NCCREATE
    if (ARG(1) == 0x000F) {            // WM_PAINT: validate
        Window *w = win_get(ARG(0));
        if (w) w->dirty = 0;
    }
    if (ARG(1) == 0x0010) {            // WM_CLOSE
        Window *w = win_get(ARG(0));
        if (w) call_wndproc(w, 0x0002, 0, 0);
    }
    return 0;
}
API(user32, MessageBoxA, 4) {
    LOGW("MessageBox [%s]: %s", ARG(2) ? STR(2) : "", ARG(1) ? STR(1) : "");
    return 1; // IDOK
}
API(user32, MessageBoxW, 4) {
    char t[512], c[128];
    w2a(WSTR(1), t, sizeof t);
    w2a(WSTR(2), c, sizeof c);
    LOGW("MessageBox [%s]: %s", c, t);
    return 1;
}
API(user32, DialogBoxParamA, 5) {
    LOGW("DialogBoxParamA(template %u): dialogs not supported, returning IDOK", ARG(1));
    return 1;
}
API(user32, EndDialog, 2) { return TRUE; }
API(user32, GetDlgItem, 2) { return 0; }

// ---------------------------------------------------------------- keyboard
API(user32, GetKeyboardLayout, 1) { return 0x04090409; }
API(user32, MapVirtualKeyA, 2) {
    // VK -> scan code table for the keys the game asks about (US layout)
    static const uint8_t vk2sc[256] = {
        [0x08] = 0x0E, [0x09] = 0x0F, [0x0D] = 0x1C, [0x10] = 0x2A, [0x11] = 0x1D, [0x12] = 0x38,
        [0x13] = 0x45, [0x14] = 0x3A, [0x1B] = 0x01, [0x20] = 0x39, [0x21] = 0x49, [0x22] = 0x51,
        [0x23] = 0x4F, [0x24] = 0x47, [0x25] = 0x4B, [0x26] = 0x48, [0x27] = 0x4D, [0x28] = 0x50,
        [0x2D] = 0x52, [0x2E] = 0x53, ['0'] = 0x0B, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04,
        ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07, ['7'] = 0x08, ['8'] = 0x09, ['9'] = 0x0A,
        ['A'] = 0x1E, ['B'] = 0x30, ['C'] = 0x2E, ['D'] = 0x20, ['E'] = 0x12, ['F'] = 0x21,
        ['G'] = 0x22, ['H'] = 0x23, ['I'] = 0x17, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26,
        ['M'] = 0x32, ['N'] = 0x31, ['O'] = 0x18, ['P'] = 0x19, ['Q'] = 0x10, ['R'] = 0x13,
        ['S'] = 0x1F, ['T'] = 0x14, ['U'] = 0x16, ['V'] = 0x2F, ['W'] = 0x11, ['X'] = 0x2D,
        ['Y'] = 0x15, ['Z'] = 0x2C, [0x60] = 0x52, [0x61] = 0x4F, [0x62] = 0x50, [0x63] = 0x51,
        [0x64] = 0x4B, [0x65] = 0x4C, [0x66] = 0x4D, [0x67] = 0x47, [0x68] = 0x48, [0x69] = 0x49,
        [0x6A] = 0x37, [0x6B] = 0x4E, [0x6D] = 0x4A, [0x6E] = 0x53, [0x6F] = 0x35, [0x70] = 0x3B,
        [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F, [0x75] = 0x40, [0x76] = 0x41,
        [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x7A] = 0x57, [0x7B] = 0x58, [0x90] = 0x45,
        [0x91] = 0x46, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D, [0xA3] = 0x1D, [0xA4] = 0x38,
        [0xA5] = 0x38, [0xBA] = 0x27, [0xBB] = 0x0D, [0xBC] = 0x33, [0xBD] = 0x0C, [0xBE] = 0x34,
        [0xBF] = 0x35, [0xC0] = 0x29, [0xDB] = 0x1A, [0xDC] = 0x2B, [0xDD] = 0x1B, [0xDE] = 0x28};
    uint32_t code = ARG(0), type = ARG(1);
    if (type == 0) return code < 256 ? vk2sc[code] : 0; // MAPVK_VK_TO_VSC
    if (type == 1) {                                     // MAPVK_VSC_TO_VK
        for (int i = 0; i < 256; i++)
            if (vk2sc[i] == code && i != 0xA0 && i != 0xA2 && i != 0xA4) return i;
        return 0;
    }
    if (type == 2) return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') ? code : 0;
    return 0;
}

// ---------------------------------------------------------------- strings
API_C(user32, wsprintfA, 2) {
    char buf[1025];
    int n = guest_vsnprintf(buf, sizeof buf, STR(1), _args + 2);
    memcpy(PTR(0), buf, n + 1);
    return n;
}

// ---------------------------------------------------------------- more user32 (plugins, newer runtimes)
// Window procedure chaining: plugins subclass the game window with
// SetWindowLong(GWL_WNDPROC) and forward with CallWindowProc.
API(user32, CallWindowProcA, 5) {
    if (!ARG(0)) return 0;
    uint32_t args[4] = {ARG(1), ARG(2), ARG(3), ARG(4)};
    return (uint32_t)call32(thread_current()->ctx, ARG(0), 4, args);
}
API(user32, CallWindowProcW, 5) { return api_user32_CallWindowProcA(_args); }
API(user32, DefWindowProcW, 4) { return api_user32_DefWindowProcA(_args); }
API(user32, DispatchMessageW, 1) { return api_user32_DispatchMessageA(_args); }
API(user32, PostMessageW, 4) { post(ARG(0), ARG(1), ARG(2), ARG(3)); return TRUE; }
API(user32, GetMessageW, 4) { return api_user32_GetMessageA(_args); }
API(user32, SendMessageTimeoutW, 7) {
    uint32_t r = call_wndproc(win_get(ARG(0)), ARG(1), ARG(2), ARG(3));
    if (ARG(6)) *(uint32_t *)PTR(6) = r;
    return TRUE;
}
API(user32, IsWindowUnicode, 1) { return FALSE; }
API(user32, SetWindowTextW, 2) {
    Window *w = win_get(ARG(0));
    if (w) {
        w2a(WSTR(1), w->title, sizeof w->title);
        if (w->sdl) SDL_SetWindowTitle(w->sdl, w->title);
    }
    return TRUE;
}
API(user32, InternalGetWindowText, 3) {
    Window *w = win_get(ARG(0));
    if (!ARG(2)) return 0;
    a2w(w ? w->title : "", PTR(1), ARG(2));
    return wlen(PTR(1));
}
static uint32_t next_msg_id = 0xC100;
API(user32, RegisterWindowMessageW, 1) { return next_msg_id++; }
API(user32, RegisterWindowMessageA, 1) { return next_msg_id++; }
API(user32, GetAncestor, 2) { return win_get(ARG(0)) ? ARG(0) : 0; }
API(user32, IsChild, 2) { return FALSE; }
API(user32, EnumWindows, 2) {
    for (int i = 0; i < nwindows; i++) {
        uint32_t args[2] = {windows[i].hwnd, ARG(1)};
        if (!(uint32_t)call32(thread_current()->ctx, ARG(0), 2, args)) break;
    }
    return TRUE;
}
API(user32, GetClassLongW, 2) { return 0; }
API(user32, SetClassLongW, 3) { return 0; }
API(user32, EnableWindow, 2) { return FALSE; }
API(user32, GetGUIThreadInfo, 2) { return FALSE; }
API(user32, GetLastInputInfo, 1) { ((uint32_t *)PTR(0))[1] = win32_tick_ms(); return TRUE; }
API(user32, TrackMouseEvent, 1) { return TRUE; }
API(user32, ChangeDisplaySettingsW, 2) { return 0; } // DISP_CHANGE_SUCCESSFUL
API(user32, ChangeDisplaySettingsA, 2) { return 0; }

// one fake monitor: the desktop
#define FAKE_MONITOR 0x00050001
API(user32, MonitorFromWindow, 2) { return FAKE_MONITOR; }
API(user32, MonitorFromPoint, 3) { return FAKE_MONITOR; }
API(user32, MonitorFromRect, 2) { return FAKE_MONITOR; }
static uint32_t monitor_info(uint32_t *mi, bool wide) {
    uint32_t w = desk_w(), h = desk_h();
    mi[1] = 0; mi[2] = 0; mi[3] = w; mi[4] = h;     // rcMonitor
    mi[5] = 0; mi[6] = 0; mi[7] = w; mi[8] = h;     // rcWork
    mi[9] = 1;                                      // MONITORINFOF_PRIMARY
    if (mi[0] >= 72) {                              // MONITORINFOEX: szDevice
        if (wide) a2w("\\\\.\\DISPLAY1", (uint16_t *)(mi + 10), 32);
        else snprintf((char *)(mi + 10), 32, "\\\\.\\DISPLAY1");
    }
    return TRUE;
}
API(user32, GetMonitorInfoW, 2) { return monitor_info(PTR(1), true); }
API(user32, GetMonitorInfoA, 2) { return monitor_info(PTR(1), false); }

// painting / GDI handles: fakes good enough for code that never really draws
API(user32, GetDC, 1) { return 0x00DC0001; }
API(user32, ReleaseDC, 2) { return 1; }
API(user32, WindowFromDC, 1) { return nwindows ? windows[0].hwnd : 0; }
API(user32, BeginPaint, 2) {
    Window *w = win_get(ARG(0));
    if (w) w->dirty = 0;
    memset(PTR(1), 0, 64);
    ((uint32_t *)PTR(1))[0] = 0x00DC0001;
    return 0x00DC0001;
}
API(user32, EndPaint, 2) { return TRUE; }
API(user32, InvalidateRect, 3) {
    Window *w = win_get(ARG(0));
    if (w) w->dirty = 1;
    else if (!ARG(0))
        for (int i = 0; i < nwindows; i++) windows[i].dirty = 1;
    return TRUE;
}
API(user32, LoadCursorW, 2) { return 0x00030000 + (ARG(1) & 0xffff); }
API(user32, LoadImageW, 6) { return 0; }
API(user32, DestroyIcon, 1) { return TRUE; }
API(user32, DrawIconEx, 9) { return TRUE; }
API(user32, LoadMenuA, 2) { return 0; }
API(user32, SetMenu, 2) { return TRUE; }
API(user32, DestroyMenu, 1) { return TRUE; }
API(user32, GetDlgItemTextW, 4) { if (ARG(3)) *(uint16_t *)PTR(2) = 0; return 0; }
API(user32, SetDlgItemTextW, 3) { return TRUE; }
API(user32, RegisterClassExW, 1) { return 0; }

// Windows hooks: handles are returned but hooks aren't invoked.
API(user32, SetWindowsHookExW, 4) { return 0x00400001; }
API(user32, SetWindowsHookExA, 4) { return 0x00400001; }
API(user32, UnhookWindowsHookEx, 1) { return TRUE; }
API(user32, CallNextHookEx, 4) { return 0; }

API(user32, SetRect, 5) {
    uint32_t *r = PTR(0);
    r[0] = ARG(1); r[1] = ARG(2); r[2] = ARG(3); r[3] = ARG(4);
    return TRUE;
}
API(user32, OffsetRect, 3) {
    int32_t *r = PTR(0);
    r[0] += (int32_t)ARG(1); r[2] += (int32_t)ARG(1);
    r[1] += (int32_t)ARG(2); r[3] += (int32_t)ARG(2);
    return TRUE;
}
API(user32, CharLowerBuffW, 2) {
    uint16_t *s = PTR(0);
    for (uint32_t i = 0; i < ARG(1); i++)
        if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 0xC0 && s[i] <= 0xDE && s[i] != 0xD7)) s[i] += 32;
    return ARG(1);
}
API(user32, MapVirtualKeyW, 2) { return api_user32_MapVirtualKeyA(_args); }
