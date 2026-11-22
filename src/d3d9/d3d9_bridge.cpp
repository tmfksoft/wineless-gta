// Direct3D 9 bridge: exposes DXVK's 64-bit D3D9 objects to the 32-bit game.
//
// Every guest-visible interface is a small guest object { vtbl, host ptr, info }
// whose vtable slots are API thunks. Thunks are generated at compile time from
// the method signatures (see Conv<>): integers/enums/floats are reinterpreted,
// interface pointers unwrapped, interface out-params wrapped, plain data
// pointers passed through (guest memory is identity-mapped). Methods involving
// pointer-holding structs (present parameters, locks) are written by hand.
//
// Display: the game sees the monitor's real modes; its fullscreen requests are
// mapped onto our SDL window according to the display policy (display.c).

#include <d3d9.h>

#include <SDL3/SDL.h>

#include "../win32/display.h"
#include "../win32/gdi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
uint32_t d3d9_bridge_create(uint32_t sdk);
void input_debug_frame(unsigned frame);
void d3d9_movie_frame(const void *bgrx, uint32_t w, uint32_t h);
void d3d9_movie_end(void);
int d3d8_active(void);
void d3d8_movie_frame(const void *bgrx, uint32_t w, uint32_t h);
}

#define BRIDGE_RESOURCE IDirect3DResource9
#define BRIDGE_SURFACE IDirect3DSurface9
#define BRIDGE_VOLUME IDirect3DVolume9
#define BRIDGE_SWAPCHAIN IDirect3DSwapChain9
#define BRIDGE_VB IDirect3DVertexBuffer9
#define BRIDGE_IB IDirect3DIndexBuffer9
#include "../bridge/com_bridge.hpp"

// Resource whose concrete type is only known at runtime.
static uint32_t wrap_resource(IDirect3DResource9 *r) {
    if (!r) return 0;
    switch (r->GetType()) {
    case D3DRTYPE_TEXTURE: return wrap(static_cast<IDirect3DTexture9 *>(r));
    case D3DRTYPE_CUBETEXTURE: return wrap(static_cast<IDirect3DCubeTexture9 *>(r));
    case D3DRTYPE_VOLUMETEXTURE: return wrap(static_cast<IDirect3DVolumeTexture9 *>(r));
    case D3DRTYPE_SURFACE: return wrap(static_cast<IDirect3DSurface9 *>(r));
    case D3DRTYPE_VOLUME: return wrap(reinterpret_cast<IDirect3DVolume9 *>(r));
    case D3DRTYPE_VERTEXBUFFER: return wrap(static_cast<IDirect3DVertexBuffer9 *>(r));
    case D3DRTYPE_INDEXBUFFER: return wrap(static_cast<IDirect3DIndexBuffer9 *>(r));
    default: return 0;
    }
}
template <> uint32_t wrap<IDirect3DBaseTexture9>(IDirect3DBaseTexture9 *h) { return wrap_resource(h); }
template <> uint32_t wrap<IDirect3DResource9>(IDirect3DResource9 *h) { return wrap_resource(h); }

// Wrap the result of QueryInterface/GetContainer according to the IID.
static uint32_t wrap_by_iid(REFIID iid, void *p) {
    if (!p) return 0;
#define TRY(I) if (iid == IID_##I) return wrap((I *)p)
    TRY(IDirect3D9); TRY(IDirect3DDevice9); TRY(IDirect3DSwapChain9); TRY(IDirect3DSurface9);
    TRY(IDirect3DVolume9); TRY(IDirect3DVertexBuffer9); TRY(IDirect3DIndexBuffer9);
    TRY(IDirect3DTexture9); TRY(IDirect3DCubeTexture9); TRY(IDirect3DVolumeTexture9);
    TRY(IDirect3DVertexDeclaration9); TRY(IDirect3DVertexShader9); TRY(IDirect3DPixelShader9);
    TRY(IDirect3DStateBlock9); TRY(IDirect3DQuery9);
#undef TRY
    if (iid == IID_IDirect3DBaseTexture9 || iid == IID_IDirect3DResource9)
        return wrap_resource((IDirect3DResource9 *)p);
    if (iid == IID_IUnknown) {
        // Ask the object for something more specific.
        IUnknown *u = (IUnknown *)p;
        IDirect3DResource9 *r = nullptr;
        if (SUCCEEDED(u->QueryInterface(IID_IDirect3DResource9, (void **)&r)) && r) {
            uint32_t g = wrap_resource(r);
            r->Release();
            return g;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- present parameters
struct GuestPP { // D3DPRESENT_PARAMETERS as laid out by 32-bit code
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    D3DSWAPEFFECT SwapEffect;
    uint32_t hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, PresentationInterval;
};
static_assert(sizeof(GuestPP) == 56, "guest present parameters");

static uint32_t g_focus_hwnd;
static IDirect3DDevice9 *g_device; // the game's device (host side), for movie playback

static D3DPRESENT_PARAMETERS pp_to_host(const GuestPP *g, uint32_t focus) {
    D3DPRESENT_PARAMETERS h = {};
    uint32_t dw, dh;
    display_desktop(&dw, &dh, nullptr);
    h.BackBufferWidth = g->BackBufferWidth ? g->BackBufferWidth : dw;
    h.BackBufferHeight = g->BackBufferHeight ? g->BackBufferHeight : dh;
    h.BackBufferFormat = g->BackBufferFormat;
    h.BackBufferCount = g->BackBufferCount;
    h.MultiSampleType = g->MultiSampleType;
    h.MultiSampleQuality = g->MultiSampleQuality;
    h.SwapEffect = g->SwapEffect;
    h.hDeviceWindow = (HWND)wineless_window_sdl(g->hDeviceWindow ? g->hDeviceWindow : focus);
    h.Windowed = TRUE; // fake fullscreen: always present into our window
    h.EnableAutoDepthStencil = g->EnableAutoDepthStencil;
    h.AutoDepthStencilFormat = g->AutoDepthStencilFormat;
    h.Flags = g->Flags;
    h.FullScreen_RefreshRateInHz = 0;
    h.PresentationInterval = g->PresentationInterval;
    if (h.BackBufferFormat == D3DFMT_UNKNOWN) h.BackBufferFormat = D3DFMT_X8R8G8B8;
    return h;
}
static void pp_to_guest(const D3DPRESENT_PARAMETERS &h, GuestPP *g, BOOL windowed) {
    g->BackBufferWidth = h.BackBufferWidth;
    g->BackBufferHeight = h.BackBufferHeight;
    g->BackBufferFormat = h.BackBufferFormat;
    g->BackBufferCount = h.BackBufferCount;
    g->MultiSampleType = h.MultiSampleType;
    g->MultiSampleQuality = h.MultiSampleQuality;
    g->SwapEffect = h.SwapEffect;
    g->hDeviceWindow = wineless_window_from_sdl(h.hDeviceWindow);
    g->Windowed = windowed;
    g->EnableAutoDepthStencil = h.EnableAutoDepthStencil;
    g->AutoDepthStencilFormat = h.AutoDepthStencilFormat;
    g->Flags = h.Flags;
    g->FullScreen_RefreshRateInHz = windowed ? 0 : 60;
    g->PresentationInterval = h.PresentationInterval;
}

// ---------------------------------------------------------------- IDirect3D9
// Modes offered to the game: the monitor's real resolutions (see display.c),
// one entry per size at the desktop refresh rate.
static void fake_mode(D3DDISPLAYMODE *m, D3DFORMAT f, int i = -1) {
    uint32_t w, h, hz;
    if (i < 0) display_desktop(&w, &h, &hz);
    else display_mode(i, &w, &h, &hz);
    m->Width = w;
    m->Height = h;
    m->RefreshRate = hz;
    m->Format = f == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : f;
}
// 32-bit only: the game saves its mode as an index, so keep the list short and stable.
static bool mode_format_ok(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8; }

OVERRIDE(IDirect3D9, GetAdapterCount, 1) { return 1; }
OVERRIDE(IDirect3D9, GetAdapterModeCount, 3) {
    return g[1] == 0 && mode_format_ok((D3DFORMAT)g[2]) ? (uint32_t)display_mode_count() : 0;
}
OVERRIDE(IDirect3D9, EnumAdapterModes, 5) {
    if (g[1] != 0 || g[3] >= (uint32_t)display_mode_count() || !mode_format_ok((D3DFORMAT)g[2]))
        return (uint32_t)D3DERR_INVALIDCALL;
    fake_mode((D3DDISPLAYMODE *)G2H(g[4]), (D3DFORMAT)g[2], (int)g[3]);
    return D3D_OK;
}
OVERRIDE(IDirect3D9, GetAdapterDisplayMode, 3) {
    if (g[1] != 0) return (uint32_t)D3DERR_INVALIDCALL;
    fake_mode((D3DDISPLAYMODE *)G2H(g[2]), D3DFMT_X8R8G8B8);
    return D3D_OK;
}
OVERRIDE(IDirect3D9, CheckDeviceType, 6) {
    return (uint32_t)SELF(IDirect3D9)->CheckDeviceType(g[1], (D3DDEVTYPE)g[2], D3DFMT_X8R8G8B8,
                                                        (D3DFORMAT)g[4], TRUE);
}
OVERRIDE(IDirect3D9, GetAdapterMonitor, 2) { return 0x00050001; }
OVERRIDE(IDirect3D9, CreateDevice, 7) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GuestPP *gpp = (GuestPP *)G2H(g[5]);
    g_focus_hwnd = g[3];
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g[3]);
    wineless_set_game_resolution(g[3], pp.BackBufferWidth, pp.BackBufferHeight, !gpp->Windowed);
    LOGI("d3d9: CreateDevice %ux%u fmt %d windowed(requested)=%d behaviour 0x%x", gpp->BackBufferWidth,
         gpp->BackBufferHeight, gpp->BackBufferFormat, gpp->Windowed, g[4]);
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = SELF(IDirect3D9)->CreateDevice(g[1], (D3DDEVTYPE)g[2], (HWND)wineless_window_sdl(g[3]), g[4], &pp, &dev);
    if (SUCCEEDED(hr)) {
        pp_to_guest(pp, gpp, gpp->Windowed);
        g_device = dev;
    } else {
        LOGE("d3d9: CreateDevice failed 0x%x", (unsigned)hr);
    }
    *(uint32_t *)G2H(g[6]) = wrap(dev);
    return (uint32_t)hr;
}

// ---------------------------------------------------------------- IDirect3DDevice9
OVERRIDE(IDirect3DDevice9, GetDisplayMode, 3) {
    fake_mode((D3DDISPLAYMODE *)G2H(g[2]), D3DFMT_X8R8G8B8);
    return D3D_OK;
}
OVERRIDE(IDirect3DDevice9, GetCreationParameters, 2) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = SELF(IDirect3DDevice9)->GetCreationParameters(&p);
    uint32_t *o = (uint32_t *)G2H(g[1]);
    o[0] = p.AdapterOrdinal;
    o[1] = p.DeviceType;
    o[2] = g_focus_hwnd;
    o[3] = p.BehaviorFlags;
    return (uint32_t)hr;
}
OVERRIDE(IDirect3DDevice9, Reset, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GuestPP *gpp = (GuestPP *)G2H(g[1]);
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g_focus_hwnd);
    wineless_set_game_resolution(g_focus_hwnd, pp.BackBufferWidth, pp.BackBufferHeight, !gpp->Windowed);
    LOGI("d3d9: Reset %ux%u windowed(requested)=%d", gpp->BackBufferWidth, gpp->BackBufferHeight, gpp->Windowed);
    HRESULT hr = SELF(IDirect3DDevice9)->Reset(&pp);
    if (SUCCEEDED(hr)) pp_to_guest(pp, gpp, gpp->Windowed);
    return (uint32_t)hr;
}
// Debug: WINELESS_SHOT=100,500 dumps the back buffer to wineless_shot_<frame>.bmp (cwd or $WINELESS_SHOT_DIR)
void debug_screenshot(IDirect3DDevice9 *dev, unsigned frame) {
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            const char *dir = getenv("WINELESS_SHOT_DIR");
            char path[512];
            snprintf(path, sizeof path, "%s/wineless_shot_%u.bmp", dir ? dir : ".", frame);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint32_t w = d.Width, h = d.Height, row = w * 3, pad = (4 - row % 4) % 4, img = (row + pad) * h;
                uint8_t hdr[54] = {'B', 'M'};
                uint32_t v;
                v = 54 + img; memcpy(hdr + 2, &v, 4);
                v = 54; memcpy(hdr + 10, &v, 4);
                v = 40; memcpy(hdr + 14, &v, 4);
                memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
                hdr[26] = 1; hdr[28] = 24;
                memcpy(hdr + 34, &img, 4);
                fwrite(hdr, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const uint8_t *src = (const uint8_t *)lr.pBits + y * lr.Pitch;
                    for (uint32_t x = 0; x < w; x++) fwrite(src + x * 4, 1, 3, f);
                    static const uint8_t z[3] = {0};
                    fwrite(z, 1, pad, f);
                }
                fclose(f);
                LOGI("d3d9: screenshot %s", path);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
}

OVERRIDE(IDirect3DDevice9, Present, 5) {
    static unsigned frame;
    static double t_last;
    static unsigned f_last;
    IDirect3DDevice9 *dev = SELF(IDirect3DDevice9);
    frame++;
    static const char *shots = getenv("WINELESS_SHOT");
    if (shots) {
        for (const char *p = shots; *p;) {
            unsigned n = (unsigned)strtoul(p, (char **)&p, 10);
            if (n == frame) debug_screenshot(dev, frame);
            while (*p == ',') p++;
            if (!n) break;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    if (t - t_last >= 5.0) {
        if (t_last > 0) LOGI("d3d9: frame %u, %.1f fps", frame, (frame - f_last) / (t - t_last));
        t_last = t;
        f_last = frame;
    }
    static bool skipping;
    bool can = wineless_window_presentable();
    if (can == skipping) {
        skipping = !can;
        LOGI("d3d9: window %s; %s presenting", can ? "visible" : "minimized/hidden", can ? "resuming" : "pausing");
    }
    if (!can) {
        // minimized/hidden: presenting would block; act like a backgrounded Windows game
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, nullptr);
        return D3D_OK;
    }
    return (uint32_t)dev->Present((const RECT *)G2H(g[1]), (const RECT *)G2H(g[2]), nullptr, nullptr);
}
OVERRIDE(IDirect3DDevice9, CreateAdditionalSwapChain, 3) {
    GuestPP *gpp = (GuestPP *)G2H(g[1]);
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g_focus_hwnd);
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT hr = SELF(IDirect3DDevice9)->CreateAdditionalSwapChain(&pp, &sc);
    if (SUCCEEDED(hr)) pp_to_guest(pp, gpp, gpp->Windowed);
    *(uint32_t *)G2H(g[2]) = wrap(sc);
    return (uint32_t)hr;
}

// ---------------------------------------------------------------- IDirect3DSwapChain9
OVERRIDE(IDirect3DSwapChain9, Present, 6) {
    return (uint32_t)SELF(IDirect3DSwapChain9)->Present((const RECT *)G2H(g[1]), (const RECT *)G2H(g[2]), nullptr, nullptr, g[5]);
}
OVERRIDE(IDirect3DSwapChain9, GetDisplayMode, 2) {
    fake_mode((D3DDISPLAYMODE *)G2H(g[1]), D3DFMT_X8R8G8B8);
    return D3D_OK;
}
OVERRIDE(IDirect3DSwapChain9, GetPresentParameters, 2) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = SELF(IDirect3DSwapChain9)->GetPresentParameters(&pp);
    if (SUCCEEDED(hr)) pp_to_guest(pp, (GuestPP *)G2H(g[1]), FALSE);
    return (uint32_t)hr;
}

OVERRIDE(IDirect3DSurface9, LockRect, 4) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DSurface9 *s = SELF(IDirect3DSurface9);
    const RECT *r = (const RECT *)G2H(g[2]);
    D3DLOCKED_RECT hl;
    HRESULT hr = s->LockRect(&hl, r, g[3]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    s->GetDesc(&d);
    hr = lock_rect_common(g[0], g[3] & D3DLOCK_READONLY ? 0x10000 : 0, hl, r, d, g[3], (uint32_t *)G2H(g[1]));
    return (uint32_t)hr;
}
OVERRIDE(IDirect3DSurface9, UnlockRect, 1) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Info *inf = info_of(g[0]);
    bool ro = inf->locks.count(0x10000) != 0;
    unlock_common(g[0], ro ? 0x10000 : 0, ro);
    return (uint32_t)SELF(IDirect3DSurface9)->UnlockRect();
}
OVERRIDE(IDirect3DTexture9, LockRect, 5) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DTexture9 *t = SELF(IDirect3DTexture9);
    const RECT *r = (const RECT *)G2H(g[3]);
    D3DLOCKED_RECT hl;
    HRESULT hr = t->LockRect(g[1], &hl, r, g[4]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    t->GetLevelDesc(g[1], &d);
    return (uint32_t)lock_rect_common(g[0], g[1] | (g[4] & D3DLOCK_READONLY ? 0x10000 : 0), hl, r, d, g[4],
                                      (uint32_t *)G2H(g[2]));
}
OVERRIDE(IDirect3DTexture9, UnlockRect, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Info *inf = info_of(g[0]);
    bool ro = inf->locks.count(g[1] | 0x10000) != 0;
    unlock_common(g[0], g[1] | (ro ? 0x10000 : 0), ro);
    return (uint32_t)SELF(IDirect3DTexture9)->UnlockRect(g[1]);
}
OVERRIDE(IDirect3DCubeTexture9, LockRect, 6) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DCubeTexture9 *t = SELF(IDirect3DCubeTexture9);
    const RECT *r = (const RECT *)G2H(g[4]);
    D3DLOCKED_RECT hl;
    HRESULT hr = t->LockRect((D3DCUBEMAP_FACES)g[1], g[2], &hl, r, g[5]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    t->GetLevelDesc(g[2], &d);
    return (uint32_t)lock_rect_common(g[0], g[1] * 256 + g[2], hl, r, d, g[5], (uint32_t *)G2H(g[3]));
}
OVERRIDE(IDirect3DCubeTexture9, UnlockRect, 3) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    unlock_common(g[0], g[1] * 256 + g[2], false);
    return (uint32_t)SELF(IDirect3DCubeTexture9)->UnlockRect((D3DCUBEMAP_FACES)g[1], g[2]);
}

OVERRIDE(IDirect3DVertexBuffer9, Lock, 5) { return lock_buffer<IDirect3DVertexBuffer9>(g); }
OVERRIDE(IDirect3DVertexBuffer9, Unlock, 1) { return unlock_buffer<IDirect3DVertexBuffer9>(g); }
OVERRIDE(IDirect3DIndexBuffer9, Lock, 5) { return lock_buffer<IDirect3DIndexBuffer9>(g); }
OVERRIDE(IDirect3DIndexBuffer9, Unlock, 1) { return unlock_buffer<IDirect3DIndexBuffer9>(g); }

// Volumes are not used by the game; fail cleanly.
OVERRIDE(IDirect3DVolume9, LockBox, 4) { return (uint32_t)D3DERR_INVALIDCALL; }
OVERRIDE(IDirect3DVolumeTexture9, LockBox, 5) { return (uint32_t)D3DERR_INVALIDCALL; }

OVERRIDE(IDirect3DSurface9, GetContainer, 3) { return get_container<IDirect3DSurface9>(g); }
OVERRIDE(IDirect3DVolume9, GetContainer, 3) { return get_container<IDirect3DVolume9>(g); }

// ---------------------------------------------------------------- hardware cursor
// Games set a Direct3D 9 hardware cursor (SetCursorProperties + ShowCursor);
// DXVK's cursor path isn't wired up for us, so build a real SDL cursor from the
// cursor bitmap. Age of Empires II HD relies on this for its custom cursors.
static SDL_Cursor *g_hw_cursor;
static bool g_hw_cursor_shown;
extern "C" int wineless_d3d9_cursor_shown(void) { return g_hw_cursor_shown; }

OVERRIDE(IDirect3DDevice9, SetCursorProperties, 4) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DSurface9 *surf = static_cast<IDirect3DSurface9 *>(unwrap(g[3]));
    if (!surf) return D3D_OK;
    D3DSURFACE_DESC d;
    surf->GetDesc(&d);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(surf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        SDL_Surface *s = SDL_CreateSurface(d.Width, d.Height, SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            for (uint32_t y = 0; y < d.Height; y++)
                memcpy((uint8_t *)s->pixels + y * s->pitch, (uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (size_t)d.Width * 4);
            SDL_Cursor *c = SDL_CreateColorCursor(s, (int)g[1], (int)g[2]);
            SDL_DestroySurface(s);
            if (c) {
                if (g_hw_cursor) SDL_DestroyCursor(g_hw_cursor);
                g_hw_cursor = c;
                if (g_hw_cursor_shown) SDL_SetCursor(g_hw_cursor);
            }
        }
        surf->UnlockRect();
    }
    return D3D_OK;
}
OVERRIDE(IDirect3DDevice9, ShowCursor, 2) {
    bool prev = g_hw_cursor_shown;
    g_hw_cursor_shown = g[1] != 0;
    if (g_hw_cursor_shown) {
        if (g_hw_cursor) SDL_SetCursor(g_hw_cursor);
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
    return prev ? TRUE : FALSE;
}
OVERRIDE(IDirect3DDevice9, SetCursorPosition, 4) { return 0; } // the real pointer is already tracked

// GDI on a surface (games drawing UI text with TextOut/DrawText): lock it on the
// host and hand out a DC over the locked pixels; ReleaseDC unlocks.
OVERRIDE(IDirect3DSurface9, GetDC, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DSurface9 *s = SELF(IDirect3DSurface9);
    D3DSURFACE_DESC d;
    s->GetDesc(&d);
    int fmt;
    switch (d.Format) {
    case D3DFMT_X8R8G8B8: case D3DFMT_A8R8G8B8: fmt = GDI_FMT_XRGB8888; break;
    case D3DFMT_R8G8B8: fmt = GDI_FMT_RGB888; break;
    case D3DFMT_R5G6B5: fmt = GDI_FMT_R5G6B5; break;
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: fmt = GDI_FMT_X1R5G5B5; break;
    default: LOGI("d3d9: GetDC on format %u not supported", (unsigned)d.Format); return (uint32_t)D3DERR_INVALIDCALL;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = s->LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) { LOGD("d3d9: GetDC: LockRect failed 0x%x", (unsigned)hr); return (uint32_t)hr; }
    *(uint32_t *)G2H(g[1]) = gdi_memory_dc(lr.pBits, d.Width, d.Height, lr.Pitch, fmt);
    return D3D_OK;
}
OVERRIDE(IDirect3DSurface9, ReleaseDC, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    gdi_surface_dc_free(g[1]);
    return (uint32_t)SELF(IDirect3DSurface9)->UnlockRect();
}

// ---------------------------------------------------------------- movie frames
// The intro movies are decoded by dshow.c; frames are drawn straight into the
// back buffer with the game's device (the game itself doesn't render meanwhile).
static IDirect3DSurface9 *g_movie_surf;
static uint32_t g_movie_w, g_movie_h;

void d3d9_movie_frame(const void *bgrx, uint32_t w, uint32_t h) {
    if (!g_device && d3d8_active()) { // D3D8 game (GTA III / Vice City)
        d3d8_movie_frame(bgrx, w, h);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DDevice9 *dev = g_device;
    if (!dev || !wineless_window_presentable()) return;
    if (g_movie_surf && (g_movie_w != w || g_movie_h != h)) {
        g_movie_surf->Release();
        g_movie_surf = nullptr;
    }
    if (!g_movie_surf) {
        // D3DPOOL_DEFAULT is required as a StretchRect source; released in d3d9_movie_end
        // so it never blocks a Reset.
        if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_movie_surf, nullptr)))
            return;
        g_movie_w = w;
        g_movie_h = h;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g_movie_surf->LockRect(&lr, nullptr, D3DLOCK_DISCARD))) return;
    for (uint32_t y = 0; y < h; y++)
        std::memcpy((uint8_t *)lr.pBits + y * lr.Pitch, (const uint8_t *)bgrx + y * w * 4, w * 4);
    g_movie_surf->UnlockRect();

    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    // letterbox/pillarbox to keep the movie's aspect ratio
    RECT dst;
    if ((uint64_t)d.Width * h > (uint64_t)d.Height * w) {
        uint32_t dw = (uint32_t)((uint64_t)d.Height * w / h);
        dst = {(LONG)((d.Width - dw) / 2), 0, (LONG)((d.Width + dw) / 2), (LONG)d.Height};
    } else {
        uint32_t dh = (uint32_t)((uint64_t)d.Width * h / w);
        dst = {0, (LONG)((d.Height - dh) / 2), (LONG)d.Width, (LONG)((d.Height + dh) / 2)};
    }
    dev->StretchRect(g_movie_surf, nullptr, bb, &dst, D3DTEXF_LINEAR);
    bb->Release();
    // Debug: WINELESS_MOVIE_SHOT=200,800 saves movie frames (numbered across all movies)
    static unsigned mframe;
    static const char *mshots = getenv("WINELESS_MOVIE_SHOT");
    mframe++;
    for (const char *p = mshots; p && *p;) {
        unsigned n = (unsigned)strtoul(p, (char **)&p, 10);
        if (n == mframe) debug_screenshot(dev, 100000 + mframe);
        while (*p == ',') p++;
        if (!n) break;
    }
    dev->Present(nullptr, nullptr, nullptr, nullptr);
}

void d3d9_movie_end(void) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (g_movie_surf) {
        g_movie_surf->Release();
        g_movie_surf = nullptr;
    }
}

#include "d3d9_vtables.inc"

// ---------------------------------------------------------------- entry point
uint32_t d3d9_bridge_create(uint32_t sdk) {
    IDirect3D9 *d3d = Direct3DCreate9(sdk);
    if (!d3d) {
        LOGE("d3d9: Direct3DCreate9 failed (is a Vulkan driver installed?)");
        return 0;
    }
    return wrap(d3d);
}
