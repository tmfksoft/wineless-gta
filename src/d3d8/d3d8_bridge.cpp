// Direct3D 8 bridge: exposes DXVK's 64-bit D3D8 (implemented on top of its D3D9)
// to 32-bit games such as GTA III and Vice City. The machinery is shared with
// the D3D9 bridge (bridge/com_bridge.hpp); this file only has what differs.

#include <d3d8.h>

#include "../win32/display.h"
#include "../present_hook.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
uint32_t d3d8_bridge_create(uint32_t sdk);
void d3d8_movie_frame(const void *bgrx, uint32_t w, uint32_t h);
void d3d8_movie_end(void);
int d3d8_active(void);
}

#define BRIDGE_RESOURCE IDirect3DResource8
#define BRIDGE_SURFACE IDirect3DSurface8
#define BRIDGE_VOLUME IDirect3DVolume8
#define BRIDGE_SWAPCHAIN IDirect3DSwapChain8
#define BRIDGE_VB IDirect3DVertexBuffer8
#define BRIDGE_IB IDirect3DIndexBuffer8
#include "../bridge/com_bridge.hpp"

// ---------------------------------------------------------------- wrapping by type
static uint32_t wrap_resource(IDirect3DResource8 *r) {
    if (!r) return 0;
    switch (r->GetType()) {
    case D3DRTYPE_TEXTURE: return wrap(static_cast<IDirect3DTexture8 *>(r));
    case D3DRTYPE_CUBETEXTURE: return wrap(static_cast<IDirect3DCubeTexture8 *>(r));
    case D3DRTYPE_VOLUMETEXTURE: return wrap(static_cast<IDirect3DVolumeTexture8 *>(r));
    case D3DRTYPE_SURFACE: return wrap(reinterpret_cast<IDirect3DSurface8 *>(r));
    case D3DRTYPE_VOLUME: return wrap(reinterpret_cast<IDirect3DVolume8 *>(r));
    case D3DRTYPE_VERTEXBUFFER: return wrap(static_cast<IDirect3DVertexBuffer8 *>(r));
    case D3DRTYPE_INDEXBUFFER: return wrap(static_cast<IDirect3DIndexBuffer8 *>(r));
    default: return 0;
    }
}
template <> uint32_t wrap<IDirect3DBaseTexture8>(IDirect3DBaseTexture8 *h) { return wrap_resource(h); }
template <> uint32_t wrap<IDirect3DResource8>(IDirect3DResource8 *h) { return wrap_resource(h); }

static uint32_t wrap_by_iid(REFIID iid, void *p) {
    if (!p) return 0;
#define TRY(I) if (iid == IID_##I) return wrap((I *)p)
    TRY(IDirect3D8); TRY(IDirect3DDevice8); TRY(IDirect3DSwapChain8); TRY(IDirect3DSurface8);
    TRY(IDirect3DVolume8); TRY(IDirect3DVertexBuffer8); TRY(IDirect3DIndexBuffer8);
    TRY(IDirect3DTexture8); TRY(IDirect3DCubeTexture8); TRY(IDirect3DVolumeTexture8);
#undef TRY
    if (iid == IID_IDirect3DBaseTexture8 || iid == IID_IDirect3DResource8) return wrap_resource((IDirect3DResource8 *)p);
    return 0;
}

// ---------------------------------------------------------------- present parameters
struct GuestPP8 { // D3DPRESENT_PARAMETERS (D3D8) as laid out by 32-bit code
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    uint32_t hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
static_assert(sizeof(GuestPP8) == 52, "guest present parameters (d3d8)");

static uint32_t g_focus_hwnd;
static IDirect3DDevice8 *g_device;

// 16-bit display formats are presented as 32-bit: modern swapchains don't do 565.
static D3DFORMAT host_bb_format(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_UNKNOWN: return D3DFMT_X8R8G8B8;
    default: return f;
    }
}

static D3DPRESENT_PARAMETERS pp_to_host(const GuestPP8 *g, uint32_t focus) {
    D3DPRESENT_PARAMETERS h = {};
    uint32_t dw, dh;
    display_desktop(&dw, &dh, nullptr);
    h.BackBufferWidth = g->BackBufferWidth ? g->BackBufferWidth : dw;
    h.BackBufferHeight = g->BackBufferHeight ? g->BackBufferHeight : dh;
    h.BackBufferFormat = host_bb_format(g->BackBufferFormat);
    h.BackBufferCount = g->BackBufferCount;
    h.MultiSampleType = g->MultiSampleType;
    h.SwapEffect = g->SwapEffect;
    h.hDeviceWindow = (HWND)wineless_window_sdl(g->hDeviceWindow ? g->hDeviceWindow : focus);
    h.Windowed = TRUE; // fullscreen is handled by our window (display.c)
    h.EnableAutoDepthStencil = g->EnableAutoDepthStencil;
    h.AutoDepthStencilFormat = g->AutoDepthStencilFormat;
    h.Flags = g->Flags;
    h.FullScreen_RefreshRateInHz = 0;
    // windowed swapchains in D3D8 must use the default interval
    h.FullScreen_PresentationInterval = 0;
    return h;
}
static void pp_to_guest(const D3DPRESENT_PARAMETERS &h, GuestPP8 *g) {
    g->BackBufferWidth = h.BackBufferWidth;
    g->BackBufferHeight = h.BackBufferHeight;
    g->BackBufferCount = h.BackBufferCount;
}

// ---------------------------------------------------------------- IDirect3D8
// D3D8 enumerates all modes in one list (no format argument): the monitor's
// resolutions at 32-bit, then the same at 16-bit (games of this era want those).
static const D3DFORMAT MODE_FORMATS[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};
static uint32_t mode_count() { return display_mode_count() * 2; }
static void fake_mode(D3DDISPLAYMODE *m, int i = -1) {
    uint32_t w, h, hz;
    if (i < 0) {
        display_desktop(&w, &h, &hz);
        m->Format = D3DFMT_X8R8G8B8;
    } else {
        int n = display_mode_count();
        display_mode(i % n, &w, &h, &hz);
        m->Format = MODE_FORMATS[i / n];
    }
    m->Width = w;
    m->Height = h;
    m->RefreshRate = hz;
}

OVERRIDE(IDirect3D8, GetAdapterCount, 1) { return 1; }
OVERRIDE(IDirect3D8, GetAdapterModeCount, 2) { return g[1] == 0 ? mode_count() : 0; }
OVERRIDE(IDirect3D8, EnumAdapterModes, 4) {
    if (g[1] != 0 || g[2] >= mode_count()) return (uint32_t)D3DERR_INVALIDCALL;
    fake_mode((D3DDISPLAYMODE *)G2H(g[3]), (int)g[2]);
    return D3D_OK;
}
OVERRIDE(IDirect3D8, GetAdapterDisplayMode, 3) {
    if (g[1] != 0) return (uint32_t)D3DERR_INVALIDCALL;
    fake_mode((D3DDISPLAYMODE *)G2H(g[2]));
    return D3D_OK;
}
OVERRIDE(IDirect3D8, CheckDeviceType, 6) {
    return (uint32_t)SELF(IDirect3D8)->CheckDeviceType(g[1], (D3DDEVTYPE)g[2], D3DFMT_X8R8G8B8,
                                                        host_bb_format((D3DFORMAT)g[4]), TRUE);
}
OVERRIDE(IDirect3D8, CheckDeviceFormat, 7) {
    // answer as if the display were 32-bit
    return (uint32_t)SELF(IDirect3D8)->CheckDeviceFormat(g[1], (D3DDEVTYPE)g[2], D3DFMT_X8R8G8B8, g[4],
                                                          (D3DRESOURCETYPE)g[5], (D3DFORMAT)g[6]);
}
OVERRIDE(IDirect3D8, CheckDepthStencilMatch, 6) {
    return (uint32_t)SELF(IDirect3D8)->CheckDepthStencilMatch(g[1], (D3DDEVTYPE)g[2], D3DFMT_X8R8G8B8,
                                                               host_bb_format((D3DFORMAT)g[4]), (D3DFORMAT)g[5]);
}
OVERRIDE(IDirect3D8, GetAdapterMonitor, 2) { return 0x00050001; }
OVERRIDE(IDirect3D8, CreateDevice, 7) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GuestPP8 *gpp = (GuestPP8 *)G2H(g[5]);
    g_focus_hwnd = g[3];
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g[3]);
    wineless_set_game_resolution(g[3], pp.BackBufferWidth, pp.BackBufferHeight, !gpp->Windowed);
    LOGI("d3d8: CreateDevice %ux%u fmt %d windowed(requested)=%d behaviour 0x%x", gpp->BackBufferWidth,
         gpp->BackBufferHeight, gpp->BackBufferFormat, gpp->Windowed, g[4]);
    IDirect3DDevice8 *dev = nullptr;
    HRESULT hr = SELF(IDirect3D8)->CreateDevice(g[1], (D3DDEVTYPE)g[2], (HWND)wineless_window_sdl(g[3]), g[4], &pp, &dev);
    if (SUCCEEDED(hr)) {
        pp_to_guest(pp, gpp);
        g_device = dev;
    } else {
        LOGE("d3d8: CreateDevice failed 0x%x", (unsigned)hr);
    }
    *(uint32_t *)G2H(g[6]) = wrap(dev);
    return (uint32_t)hr;
}

// ---------------------------------------------------------------- IDirect3DDevice8
OVERRIDE(IDirect3DDevice8, GetDisplayMode, 2) {
    fake_mode((D3DDISPLAYMODE *)G2H(g[1]));
    return D3D_OK;
}
OVERRIDE(IDirect3DDevice8, GetCreationParameters, 2) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = SELF(IDirect3DDevice8)->GetCreationParameters(&p);
    uint32_t *o = (uint32_t *)G2H(g[1]);
    o[0] = p.AdapterOrdinal;
    o[1] = p.DeviceType;
    o[2] = g_focus_hwnd;
    o[3] = p.BehaviorFlags;
    return (uint32_t)hr;
}
OVERRIDE(IDirect3DDevice8, Reset, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GuestPP8 *gpp = (GuestPP8 *)G2H(g[1]);
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g_focus_hwnd);
    wineless_set_game_resolution(g_focus_hwnd, pp.BackBufferWidth, pp.BackBufferHeight, !gpp->Windowed);
    LOGI("d3d8: Reset %ux%u windowed(requested)=%d", gpp->BackBufferWidth, gpp->BackBufferHeight, gpp->Windowed);
    HRESULT hr = SELF(IDirect3DDevice8)->Reset(&pp);
    if (SUCCEEDED(hr)) pp_to_guest(pp, gpp);
    return (uint32_t)hr;
}
// Debug: WINELESS_SHOT=100,500 saves the back buffer as wineless_shot_<frame>.bmp
static void debug_screenshot(IDirect3DDevice8 *dev, unsigned frame) {
    IDirect3DSurface8 *bb = nullptr, *img = nullptr;
    if (FAILED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (SUCCEEDED(dev->CreateImageSurface(d.Width, d.Height, d.Format, &img)) &&
        SUCCEEDED(dev->CopyRects(bb, nullptr, 0, img, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            const char *dir = getenv("WINELESS_SHOT_DIR");
            char path[512];
            snprintf(path, sizeof path, "%s/wineless_shot_%u.bmp", dir ? dir : ".", frame);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint32_t w = d.Width, h = d.Height, row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
                uint8_t hdr[54] = {'B', 'M'};
                uint32_t v = 54 + size;
                memcpy(hdr + 2, &v, 4);
                v = 54; memcpy(hdr + 10, &v, 4);
                v = 40; memcpy(hdr + 14, &v, 4);
                memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
                hdr[26] = 1; hdr[28] = 24;
                memcpy(hdr + 34, &size, 4);
                fwrite(hdr, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const uint8_t *src = (const uint8_t *)lr.pBits + y * lr.Pitch;
                    for (uint32_t x = 0; x < w; x++) fwrite(src + x * 4, 1, 3, f);
                    static const uint8_t z[3] = {0};
                    fwrite(z, 1, pad, f);
                }
                fclose(f);
                LOGI("d3d8: screenshot %s", path);
            }
            img->UnlockRect();
        }
    }
    if (img) img->Release();
    bb->Release();
}

// libretro core: read the current back buffer into an XRGB8888 CPU buffer.
// D3D8 uses CreateImageSurface + CopyRects (there is no GetRenderTargetData).
static int d3d8_rt_read(uint32_t *dst, uint32_t dst_pitch_px) {
    IDirect3DDevice8 *dev = g_device;
    if (!dev) return 0;
    IDirect3DSurface8 *bb = nullptr, *img = nullptr;
    if (FAILED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    int ok = 0;
    if (SUCCEEDED(dev->CreateImageSurface(d.Width, d.Height, d.Format, &img)) &&
        SUCCEEDED(dev->CopyRects(bb, nullptr, 0, img, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (uint32_t y = 0; y < d.Height; y++) {
                const uint8_t *src = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
                uint32_t *out = dst + (size_t)y * dst_pitch_px;
                if (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) {
                    std::memcpy(out, src, (size_t)d.Width * 4);
                } else if (d.Format == D3DFMT_R5G6B5) {
                    const uint16_t *s = (const uint16_t *)src;
                    for (uint32_t x = 0; x < d.Width; x++) {
                        uint16_t p = s[x];
                        uint32_t r = (p >> 11) & 0x1f, gg = (p >> 5) & 0x3f, b = p & 0x1f;
                        out[x] = ((r << 3 | r >> 2) << 16) | ((gg << 2 | gg >> 4) << 8) | (b << 3 | b >> 2);
                    }
                } else if (d.Format == D3DFMT_X1R5G5B5 || d.Format == D3DFMT_A1R5G5B5) {
                    const uint16_t *s = (const uint16_t *)src;
                    for (uint32_t x = 0; x < d.Width; x++) {
                        uint16_t p = s[x];
                        uint32_t r = (p >> 10) & 0x1f, gg = (p >> 5) & 0x1f, b = p & 0x1f;
                        out[x] = ((r << 3 | r >> 2) << 16) | ((gg << 3 | gg >> 2) << 8) | (b << 3 | b >> 2);
                    }
                } else {
                    img->UnlockRect(); img->Release(); bb->Release();
                    return 0;
                }
            }
            img->UnlockRect();
            ok = 1;
        }
    }
    if (img) img->Release();
    bb->Release();
    return ok;
}

OVERRIDE(IDirect3DDevice8, Present, 5) {
    static unsigned frame;
    static double t_last;
    static unsigned f_last;
    frame++;
    static const char *shots = getenv("WINELESS_SHOT");
    for (const char *p = shots; p && *p;) {
        unsigned n = (unsigned)strtoul(p, (char **)&p, 10);
        if (n == frame) debug_screenshot(SELF(IDirect3DDevice8), frame);
        while (*p == ',') p++;
        if (!n) break;
    }
    // libretro core: hand this frame to the frontend and pace the guest.
    if (wl_present_hook) {
        IDirect3DSurface8 *bb = nullptr;
        uint32_t w = 0, h = 0;
        if (SUCCEEDED(SELF(IDirect3DDevice8)->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            w = d.Width; h = d.Height;
            bb->Release();
        }
        wl_present_hook(d3d8_rt_read, w, h);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    if (t - t_last >= 5.0) {
        if (t_last > 0) LOGI("d3d8: frame %u, %.1f fps", frame, (frame - f_last) / (t - t_last));
        t_last = t;
        f_last = frame;
    }
    if (!wineless_window_presentable()) {
        struct timespec sl = {0, 16000000};
        nanosleep(&sl, nullptr);
        return D3D_OK;
    }
    if (!wl_present_timing)
        return (uint32_t)SELF(IDirect3DDevice8)->Present((const RECT *)G2H(g[1]), (const RECT *)G2H(g[2]), nullptr, nullptr);
    // libretro core: report how long the real present takes, for its frame-time log.
    struct timespec p0, p1;
    clock_gettime(CLOCK_MONOTONIC, &p0);
    HRESULT phr = SELF(IDirect3DDevice8)->Present((const RECT *)G2H(g[1]), (const RECT *)G2H(g[2]), nullptr, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &p1);
    wl_present_timing((p1.tv_sec - p0.tv_sec) * 1e3 + (p1.tv_nsec - p0.tv_nsec) / 1e6);
    return (uint32_t)phr;
}
OVERRIDE(IDirect3DDevice8, CreateAdditionalSwapChain, 3) {
    GuestPP8 *gpp = (GuestPP8 *)G2H(g[1]);
    D3DPRESENT_PARAMETERS pp = pp_to_host(gpp, g_focus_hwnd);
    IDirect3DSwapChain8 *sc = nullptr;
    HRESULT hr = SELF(IDirect3DDevice8)->CreateAdditionalSwapChain(&pp, &sc);
    if (SUCCEEDED(hr)) pp_to_guest(pp, gpp);
    *(uint32_t *)G2H(g[2]) = wrap(sc);
    return (uint32_t)hr;
}
OVERRIDE(IDirect3DSwapChain8, Present, 5) {
    return (uint32_t)SELF(IDirect3DSwapChain8)->Present((const RECT *)G2H(g[1]), (const RECT *)G2H(g[2]), nullptr, nullptr);
}

// ---------------------------------------------------------------- locking
OVERRIDE(IDirect3DSurface8, LockRect, 4) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DSurface8 *s = SELF(IDirect3DSurface8);
    const RECT *r = (const RECT *)G2H(g[2]);
    D3DLOCKED_RECT hl;
    HRESULT hr = s->LockRect(&hl, r, g[3]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    s->GetDesc(&d);
    return (uint32_t)lock_rect_common(g[0], g[3] & D3DLOCK_READONLY ? 0x10000 : 0, hl, r, d, g[3], (uint32_t *)G2H(g[1]));
}
OVERRIDE(IDirect3DSurface8, UnlockRect, 1) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    bool ro = info_of(g[0])->locks.count(0x10000) != 0;
    unlock_common(g[0], ro ? 0x10000 : 0, ro);
    return (uint32_t)SELF(IDirect3DSurface8)->UnlockRect();
}
OVERRIDE(IDirect3DTexture8, LockRect, 5) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DTexture8 *t = SELF(IDirect3DTexture8);
    const RECT *r = (const RECT *)G2H(g[3]);
    D3DLOCKED_RECT hl;
    HRESULT hr = t->LockRect(g[1], &hl, r, g[4]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    t->GetLevelDesc(g[1], &d);
    return (uint32_t)lock_rect_common(g[0], g[1] | (g[4] & D3DLOCK_READONLY ? 0x10000 : 0), hl, r, d, g[4],
                                      (uint32_t *)G2H(g[2]));
}
OVERRIDE(IDirect3DTexture8, UnlockRect, 2) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    bool ro = info_of(g[0])->locks.count(g[1] | 0x10000) != 0;
    unlock_common(g[0], g[1] | (ro ? 0x10000 : 0), ro);
    return (uint32_t)SELF(IDirect3DTexture8)->UnlockRect(g[1]);
}
OVERRIDE(IDirect3DCubeTexture8, LockRect, 6) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DCubeTexture8 *t = SELF(IDirect3DCubeTexture8);
    const RECT *r = (const RECT *)G2H(g[4]);
    D3DLOCKED_RECT hl;
    HRESULT hr = t->LockRect((D3DCUBEMAP_FACES)g[1], g[2], &hl, r, g[5]);
    if (FAILED(hr)) return (uint32_t)hr;
    D3DSURFACE_DESC d;
    t->GetLevelDesc(g[2], &d);
    return (uint32_t)lock_rect_common(g[0], g[1] * 256 + g[2], hl, r, d, g[5], (uint32_t *)G2H(g[3]));
}
OVERRIDE(IDirect3DCubeTexture8, UnlockRect, 3) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    unlock_common(g[0], g[1] * 256 + g[2], false);
    return (uint32_t)SELF(IDirect3DCubeTexture8)->UnlockRect((D3DCUBEMAP_FACES)g[1], g[2]);
}
OVERRIDE(IDirect3DVertexBuffer8, Lock, 5) { return lock_buffer<IDirect3DVertexBuffer8>(g); }
OVERRIDE(IDirect3DVertexBuffer8, Unlock, 1) { return unlock_buffer<IDirect3DVertexBuffer8>(g); }
OVERRIDE(IDirect3DIndexBuffer8, Lock, 5) { return lock_buffer<IDirect3DIndexBuffer8>(g); }
OVERRIDE(IDirect3DIndexBuffer8, Unlock, 1) { return unlock_buffer<IDirect3DIndexBuffer8>(g); }
OVERRIDE(IDirect3DVolume8, LockBox, 4) { return (uint32_t)D3DERR_INVALIDCALL; }
OVERRIDE(IDirect3DVolumeTexture8, LockBox, 5) { return (uint32_t)D3DERR_INVALIDCALL; }
OVERRIDE(IDirect3DSurface8, GetContainer, 3) { return get_container<IDirect3DSurface8>(g); }
OVERRIDE(IDirect3DVolume8, GetContainer, 3) { return get_container<IDirect3DVolume8>(g); }

// ---------------------------------------------------------------- movie frames
// D3D8 has no StretchRect: copy the frame into a 32-bit image surface and
// CopyRects it, centred, into the back buffer.
int d3d8_active(void) { return g_device != nullptr; }

void d3d8_movie_frame(const void *bgrx, uint32_t w, uint32_t h) {
    // libretro core: intro movie frame (already XRGB8888) — stream it directly.
    if (wl_submit_frame) {
        wl_submit_frame((const uint32_t *)bgrx, w, h);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    IDirect3DDevice8 *dev = g_device;
    if (!dev || !wineless_window_presentable()) return;
    IDirect3DSurface8 *bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    uint32_t cw = w < d.Width ? w : d.Width, ch = h < d.Height ? h : d.Height;
    IDirect3DSurface8 *img = nullptr;
    if (SUCCEEDED(dev->CreateImageSurface(cw, ch, d.Format, &img))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(img->LockRect(&lr, nullptr, 0))) {
            const uint8_t *src = (const uint8_t *)bgrx + ((h - ch) / 2) * w * 4 + ((w - cw) / 2) * 4;
            for (uint32_t y = 0; y < ch; y++) std::memcpy((uint8_t *)lr.pBits + y * lr.Pitch, src + y * w * 4, cw * 4);
            img->UnlockRect();
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            POINT pt = {(LONG)((d.Width - cw) / 2), (LONG)((d.Height - ch) / 2)};
            dev->CopyRects(img, nullptr, 0, bb, &pt);
        }
        img->Release();
    }
    bb->Release();
    dev->Present(nullptr, nullptr, nullptr, nullptr);
}
void d3d8_movie_end(void) {}

#include "d3d8_vtables.inc"

// ---------------------------------------------------------------- entry point
uint32_t d3d8_bridge_create(uint32_t sdk) {
    IDirect3D8 *d3d = Direct3DCreate8(sdk);
    if (!d3d) {
        LOGE("d3d8: Direct3DCreate8 failed (is a Vulkan driver installed?)");
        return 0;
    }
    return wrap(d3d);
}
