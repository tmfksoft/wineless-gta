// Host D3D9 device used by the DirectDraw / legacy Direct3D layer.
#include <d3d9.h>

#include "dd_gfx.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "../win32/display.h"
#include "../present_hook.h"

extern "C" {
void log_msg(int level, const char *fmt, ...);
void *wineless_window_sdl(uint32_t hwnd);
}
#define LOGE(...) log_msg(0, __VA_ARGS__)
#define LOGI(...) log_msg(2, __VA_ARGS__)

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static uint32_t g_w, g_h;
static bool g_in_scene;
static std::recursive_mutex g_mx;

static const D3DFORMAT FMT[] = {D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5,
                                D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8};
static const uint32_t FMT_BPP[] = {2, 2, 2, 2, 4, 4};

static void default_states() {
    // Direct3D 5 device defaults that differ from D3D9's
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_dev->SetRenderState(D3DRS_DITHERENABLE, TRUE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
}

int ddg_device(uint32_t hwnd, uint32_t w, uint32_t h, int fullscreen_requested) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_d3d) g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { LOGE("ddraw: Direct3D 9 unavailable (is a Vulkan driver installed?)"); return 0; }
    wineless_set_game_resolution(hwnd, w, h, fullscreen_requested);
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; // the back buffer persists like a DirectDraw surface
    pp.hDeviceWindow = (HWND)wineless_window_sdl(hwnd);
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr;
    if (g_dev) {
        hr = g_dev->Reset(&pp);
    } else {
        hr = g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    }
    if (FAILED(hr)) { LOGE("ddraw: creating the D3D9 device failed (0x%x)", (unsigned)hr); return 0; }
    g_w = w;
    g_h = h;
    g_in_scene = false;
    default_states();
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    LOGI("ddraw: rendering %ux%u through Direct3D 9", w, h);
    return 1;
}

int ddg_ready(void) { return g_dev != nullptr; }

void debug_screenshot(IDirect3DDevice9 *dev, unsigned frame); // d3d9_bridge.cpp
extern "C" void input_debug_frame(unsigned frame);

void ddg_present(void) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return;
    if (g_in_scene) { g_dev->EndScene(); g_in_scene = false; }
    static unsigned frame;
    frame++;
    input_debug_frame(frame);
    static const char *shots = getenv("WINELESS_SHOT");
    for (const char *p = shots; p && *p;) {
        unsigned n = (unsigned)strtoul(p, (char **)&p, 10);
        if (n == frame) debug_screenshot(g_dev, frame);
        while (*p == ',') p++;
        if (!n) break;
    }
    // libretro core: read this frame back to CPU pixels, hand it to the frontend
    // and block to pace the guest. (ddg_rt_read re-locks g_mx; it is recursive.)
    if (wl_present_hook) wl_present_hook(ddg_rt_read, g_w, g_h);
    if (!wineless_window_presentable()) return;
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- textures
struct Tex { IDirect3DTexture9 *t; uint32_t w, h; int fmt; };

void *ddg_texture_create(uint32_t w, uint32_t h, int fmt) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return nullptr;
    Tex *t = new Tex{nullptr, w, h, fmt};
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, FMT[fmt], D3DPOOL_MANAGED, &t->t, nullptr))) {
        delete t;
        return nullptr;
    }
    return t;
}
void ddg_texture_upload(void *tv, const void *data, uint32_t pitch, uint32_t w, uint32_t h) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Tex *t = (Tex *)tv;
    if (!t) return;
    D3DLOCKED_RECT lr;
    if (FAILED(t->t->LockRect(0, &lr, nullptr, 0))) return;
    uint32_t row = (w < t->w ? w : t->w) * FMT_BPP[t->fmt];
    for (uint32_t y = 0; y < h && y < t->h; y++)
        memcpy((uint8_t *)lr.pBits + y * lr.Pitch, (const uint8_t *)data + y * pitch, row);
    t->t->UnlockRect(0);
}
void ddg_texture_free(void *tv) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Tex *t = (Tex *)tv;
    if (!t) return;
    if (g_dev) g_dev->SetTexture(0, nullptr);
    t->t->Release();
    delete t;
}
void ddg_set_texture(void *tv) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_dev) g_dev->SetTexture(0, tv ? ((Tex *)tv)->t : nullptr);
}

// ---------------------------------------------------------------- state / drawing
void ddg_rs(uint32_t s, uint32_t v) { std::lock_guard<std::recursive_mutex> lk(g_mx); if (g_dev) g_dev->SetRenderState((D3DRENDERSTATETYPE)s, v); }
void ddg_sampler(uint32_t s, uint32_t v) { std::lock_guard<std::recursive_mutex> lk(g_mx); if (g_dev) g_dev->SetSamplerState(0, (D3DSAMPLERSTATETYPE)s, v); }
void ddg_tss(uint32_t st, uint32_t s, uint32_t v) { std::lock_guard<std::recursive_mutex> lk(g_mx); if (g_dev) g_dev->SetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)s, v); }

void ddg_begin_scene(void) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_dev && !g_in_scene) { g_dev->BeginScene(); g_in_scene = true; }
}
void ddg_end_scene(void) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_dev && g_in_scene) { g_dev->EndScene(); g_in_scene = false; }
}
void ddg_draw(uint32_t prim, uint32_t nprims, const void *verts, uint32_t stride, uint32_t fvf) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev || !nprims) return;
    if (!g_in_scene) { g_dev->BeginScene(); g_in_scene = true; }
    g_dev->SetFVF(fvf);
    g_dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)prim, nprims, verts, stride);
}
void ddg_draw_indexed(uint32_t prim, uint32_t nverts, uint32_t nprims, const uint16_t *idx, const void *verts,
                      uint32_t stride, uint32_t fvf) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev || !nprims) return;
    if (!g_in_scene) { g_dev->BeginScene(); g_in_scene = true; }
    g_dev->SetFVF(fvf);
    g_dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)prim, 0, nverts, nprims, idx, D3DFMT_INDEX16, verts, stride);
}
void ddg_clear(uint32_t n, const int32_t *rects, uint32_t flags, uint32_t color, float z) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_dev) g_dev->Clear(n, (const D3DRECT *)rects, flags, color, z, 0);
}
void ddg_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return;
    if (x + w > g_w) w = g_w > x ? g_w - x : 0;
    if (y + h > g_h) h = g_h > y ? g_h - y : 0;
    D3DVIEWPORT9 vp = {x, y, w, h, minz, maxz};
    g_dev->SetViewport(&vp);
}

// ---------------------------------------------------------------- back buffer access
int ddg_rt_read(uint32_t *dst, uint32_t dst_pitch_px) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return 0;
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    int ok = 0;
    if (SUCCEEDED(g_dev->CreateOffscreenPlainSurface(g_w, g_h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(g_dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (uint32_t y = 0; y < g_h; y++)
                memcpy(dst + y * dst_pitch_px, (uint8_t *)lr.pBits + y * lr.Pitch, g_w * 4);
            sys->UnlockRect();
            ok = 1;
        }
    }
    if (sys) sys->Release();
    bb->Release();
    return ok;
}

void ddg_rt_write(const uint32_t *src, uint32_t src_pitch_px, const int32_t *rect) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return;
    RECT r = {0, 0, (LONG)g_w, (LONG)g_h};
    if (rect) r = {rect[0], rect[1], rect[2], rect[3]};
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)g_w) r.right = g_w;
    if (r.bottom > (LONG)g_h) r.bottom = g_h;
    if (r.right <= r.left || r.bottom <= r.top) return;
    uint32_t w = r.right - r.left, h = r.bottom - r.top;
    IDirect3DSurface9 *tmp = nullptr, *bb = nullptr;
    if (FAILED(g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &tmp, nullptr))) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tmp->LockRect(&lr, nullptr, 0))) {
        for (uint32_t y = 0; y < h; y++)
            memcpy((uint8_t *)lr.pBits + y * lr.Pitch, src + (r.top + y) * src_pitch_px + r.left, w * 4);
        tmp->UnlockRect();
        if (SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            g_dev->StretchRect(tmp, nullptr, bb, &r, D3DTEXF_NONE);
            bb->Release();
        }
    }
    tmp->Release();
}

void ddg_rt_fill(const int32_t *rect, uint32_t xrgb) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_dev) return;
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    RECT r;
    if (rect) r = {rect[0], rect[1], rect[2], rect[3]};
    g_dev->ColorFill(bb, rect ? &r : nullptr, xrgb);
    bb->Release();
}

extern "C" void ddg_transform(uint32_t t, const float *m) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_dev) g_dev->SetTransform((D3DTRANSFORMSTATETYPE)t, (const D3DMATRIX *)m);
}
