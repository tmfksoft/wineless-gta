// Display modes and fullscreen handling.
//
// The game always believes it owns an exclusive fullscreen display. What
// actually happens depends on WINELESS_DISPLAY / --display:
//   fullscreen (default) real fullscreen at the game's resolution (SDL mode
//                        switch on X11, compositor-emulated on Wayland)
//   borderless           desktop-sized borderless window, DXVK scales
//   windowed             a normal window at the game's resolution
// D3D9 itself always runs "windowed" into our SDL window.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <strings.h>

#include "display.h"
#include "win32.h"

typedef struct { int w, h; } Res;
static Res modes[64];
static int nmodes;
static int desk_w = 800, desk_h = 600, desk_hz = 60;
static int policy = DISPLAY_FULLSCREEN;
static int inited;
// What display_apply last did, so it can be redone when the window changes monitor.
static SDL_DisplayID applied_display;
static uint32_t applied_w, applied_h;
static int applied_fs;

static void add_mode(int w, int h) {
    if (w < 640 || h < 480) return;
    for (int i = 0; i < nmodes; i++)
        if (modes[i].w == w && modes[i].h == h) return;
    if (nmodes < 64) modes[nmodes++] = (Res){w, h};
}

static int cmp_res(const void *a, const void *b) {
    const Res *x = a, *y = b;
    return x->w != y->w ? x->w - y->w : x->h - y->h;
}

void display_init(void) {
    if (inited) return;
    inited = 1;
    const char *p = getenv("WINELESS_DISPLAY");
    if (p && !strcasecmp(p, "borderless")) policy = DISPLAY_BORDERLESS;
    else if (p && !strcasecmp(p, "windowed")) policy = DISPLAY_WINDOWED;

    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = d ? SDL_GetDesktopDisplayMode(d) : NULL;
    if (dm) {
        desk_w = dm->w;
        desk_h = dm->h;
        desk_hz = dm->refresh_rate > 1 ? (int)(dm->refresh_rate + 0.5f) : 60;
    }
    int n = 0;
    SDL_DisplayMode **list = d ? SDL_GetFullscreenDisplayModes(d, &n) : NULL;
    for (int i = 0; i < n; i++) add_mode(list[i]->w, list[i]->h);
    SDL_free(list);
    add_mode(desk_w, desk_h);
    add_mode(640, 480);
    add_mode(800, 600); // the game refuses to start without it
    qsort(modes, nmodes, sizeof *modes, cmp_res);
    static const char *names[] = {"fullscreen", "borderless", "windowed"};
    LOGI("display: desktop %dx%d@%d, %d modes, policy %s", desk_w, desk_h, desk_hz, nmodes, names[policy]);
}

int display_mode_count(void) { display_init(); return nmodes; }
void display_mode(int i, uint32_t *w, uint32_t *h, uint32_t *hz) {
    display_init();
    *w = modes[i].w;
    *h = modes[i].h;
    *hz = desk_hz;
}
void display_desktop(uint32_t *w, uint32_t *h, uint32_t *hz) {
    display_init();
    *w = desk_w;
    *h = desk_h;
    if (hz) *hz = desk_hz;
}
int display_policy(void) { display_init(); return policy; }

void display_apply(SDL_Window *win, uint32_t w, uint32_t h, int game_wants_fullscreen) {
    display_init();
    if (!win) return;
    applied_w = w;
    applied_h = h;
    applied_fs = game_wants_fullscreen;
    // The user's --display policy drives the real window; the game's own
    // windowed/fullscreen request only chooses a real mode switch vs scaling.
    if (policy == DISPLAY_WINDOWED) {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowBordered(win, true);
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        LOGI("display: windowed %ux%u", w, h);
    } else if (policy == DISPLAY_BORDERLESS || !game_wants_fullscreen) {
        // Desktop-sized, DXVK scales the game's image. Used for borderless, and
        // for fullscreen policy when the game runs windowed itself (AoE2 HD).
        SDL_SetWindowFullscreenMode(win, NULL); // desktop fullscreen
        SDL_SetWindowFullscreen(win, true);
        LOGI("display: desktop fullscreen, game renders %ux%u (scaled)", w, h);
    } else {
        SDL_DisplayMode mode;
        SDL_DisplayID d = SDL_GetDisplayForWindow(win);
        if (SDL_GetClosestFullscreenDisplayMode(d, w, h, (float)desk_hz, false, &mode) && mode.w == (int)w &&
            mode.h == (int)h) {
            SDL_SetWindowFullscreenMode(win, &mode);
            LOGI("display: fullscreen %dx%d@%.0f", mode.w, mode.h, mode.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(win, NULL);
            LOGI("display: no %ux%u mode, using desktop fullscreen (scaled)", w, h);
        }
        SDL_SetWindowFullscreen(win, true);
    }
    SDL_SyncWindow(win);
    applied_display = SDL_GetDisplayForWindow(win);
    SDL_RaiseWindow(win);
    if (policy != DISPLAY_WINDOWED && game_wants_fullscreen && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS))
        SDL_SetWindowMouseGrab(win, true);
}

// The window was moved to another monitor (e.g. Shift+Super+Left in GNOME). A
// fullscreen mode switch belongs to one monitor (XWayland only emulates it on the
// monitor it was set on), so set the game's mode up again on the new one.
void display_window_moved(SDL_Window *win) {
    if (!win || policy == DISPLAY_WINDOWED || !applied_fs || !applied_w) return;
    SDL_DisplayID d = SDL_GetDisplayForWindow(win);
    if (!d || d == applied_display) return;
    const char *name = SDL_GetDisplayName(d);
    LOGI("display: window moved to display %s; re-applying %ux%u", name ? name : "?", applied_w, applied_h);
    display_apply(win, applied_w, applied_h, 1);
}
