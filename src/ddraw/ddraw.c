// DirectDraw (IDirectDraw/2, surfaces 1-3, palettes, clippers) and Direct3D 5
// immediate mode (IDirect3D2, IDirect3DDevice2, viewports, materials,
// IDirect3DTexture2), rendered through a host D3D9 device (dd_gfx.cpp).
//
// Model: every surface keeps its pixels in guest memory, so Lock hands out a
// pointer. Textures are uploaded to the GPU when bound (16-bit and paletted
// formats converted, colour keys turned into alpha). The flippable back buffer
// is the D3D9 render target itself: Lock reads it back, Unlock/Blt write to it,
// Flip presents.
//
// IDirectDraw4/7 are only implemented as far as the GTA games use them.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../heap.h"
#include "../win32/com.h"
#include "../win32/win32.h"
#include "../win32/display.h"
#include "../win32/gdi.h"
#include "dd_gfx.h"

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_NOTLOCKED 0x887600E6u
#define DDERR_CANTCREATEDC 0x887602D2u
#define DDERR_NOPALETTEATTACHED 0x887601CCu
#define DDERR_NOCOLORKEY 0x887600D7u
#define DDERR_NOTFLIPPABLE 0x887600D6u

// DDSURFACEDESC flags / caps
#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_ZBUFFERBITDEPTH 0x40
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_CKSRCBLT 0x10000
#define DDSD_REFRESHRATE 0x40000
#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_TEXTURE 0x1000
#define DDSCAPS_3DDEVICE 0x2000
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_VISIBLE 0x8000
#define DDSCAPS_ZBUFFER 0x20000
#define DDSCAPS_LOCALVIDMEM 0x10000000
#define DDPF_ALPHAPIXELS 0x1
#define DDPF_PALETTEINDEXED8 0x20
#define DDPF_RGB 0x40
#define DDPF_ZBUFFER 0x400
#define DDLOCK_READONLY 0x10
#define DDLOCK_WRITEONLY 0x20
#define DDBLT_COLORFILL 0x400
#define DDBLT_KEYSRC 0x8000
#define DDBLT_KEYSRCOVERRIDE 0x10000
#define DDBLT_DEPTHFILL 0x2000000
#define DDBLTFAST_SRCCOLORKEY 0x1
#define DDCKEY_SRCBLT 0x8
#define DDBD_8 0x800
#define DDBD_16 0x400
#define DDBD_24 0x200
#define DDBD_32 0x100
#define SD_SIZE 108 // DDSURFACEDESC

typedef struct Palette {
    int refs;
    uint32_t e[256]; // PALETTEENTRY: r, g, b, flags
    uint32_t g;
} Palette;

typedef struct Surf {
    int refs;
    uint32_t w, h, pitch, bpp; // bpp in bytes
    uint32_t caps;
    uint32_t pf_flags, rmask, gmask, bmask, amask;
    uint32_t mem; // guest pixels
    bool primary, backbuffer, zbuf, rt;
    struct Surf *back, *zattach;
    uint32_t ck_lo, ck_hi;
    bool has_ck;
    Palette *pal;
    void *tex;
    bool dirty, tex_ck;
    uint32_t handle;
    bool locked;
    uint32_t lock_flags;
    uint32_t g_surf, g_tex;
} Surf;

// ---------------------------------------------------------------- global state
static uint32_t mode_w, mode_h, mode_bpp = 16;
static uint32_t coop_hwnd, coop_flags;
static Surf *primary, *backbuf;
static uint32_t vt_dd1, vt_dd2, vt_dd7, vt_surf, vt_tex, vt_pal, vt_clip, vt_d3d2, vt_dev2, vt_vp2, vt_mat2;
static uint32_t g_dd1, g_dd2, g_dd7, g_d3d2, g_dev2;
#define MAX_HANDLES 8192
static Surf *tex_handles[MAX_HANDLES];
static int n_tex_handles = 1;

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// ---------------------------------------------------------------- pixel formats
static void set_format(Surf *s, uint32_t bits) {
    s->pf_flags = DDPF_RGB;
    s->amask = 0;
    switch (bits) {
    case 8: s->pf_flags = DDPF_RGB | DDPF_PALETTEINDEXED8; s->rmask = s->gmask = s->bmask = 0; s->bpp = 1; break;
    case 16: s->rmask = 0xF800; s->gmask = 0x07E0; s->bmask = 0x001F; s->bpp = 2; break;
    case 24: s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; s->bpp = 3; break;
    default: s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; s->bpp = 4; break;
    }
}
static void read_pf(Surf *s, const uint32_t *pf) {
    // DDPIXELFORMAT: dwSize, dwFlags, dwFourCC, dwRGBBitCount, R, G, B, A masks
    s->pf_flags = pf[1];
    uint32_t bits = pf[3];
    if (pf[1] & DDPF_PALETTEINDEXED8) bits = 8;
    s->bpp = bits ? (bits + 7) / 8 : 2;
    s->rmask = pf[4];
    s->gmask = pf[5];
    s->bmask = pf[6];
    s->amask = (pf[1] & DDPF_ALPHAPIXELS) ? pf[7] : 0;
}
static void write_pf(const Surf *s, uint32_t *pf) {
    memset(pf, 0, 32);
    pf[0] = 32;
    pf[1] = s->pf_flags | (s->amask ? DDPF_ALPHAPIXELS : 0);
    pf[3] = s->bpp * 8;
    pf[4] = s->rmask;
    pf[5] = s->gmask;
    pf[6] = s->bmask;
    pf[7] = s->amask;
    if (s->zbuf) { pf[1] = DDPF_ZBUFFER; pf[3] = 16; pf[4] = 0; pf[5] = 0xFFFF; pf[6] = pf[7] = 0; }
}

static int shift_of(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int bits_of(uint32_t m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }
static uint32_t chan_to8(uint32_t v, uint32_t mask) {
    if (!mask) return 0xFF;
    int b = bits_of(mask);
    v = (v & mask) >> shift_of(mask);
    return b >= 8 ? v >> (b - 8) : (v * 255 + ((1u << b) - 1) / 2) / ((1u << b) - 1);
}
static uint32_t chan_from8(uint32_t v8, uint32_t mask) {
    if (!mask) return 0;
    int b = bits_of(mask);
    uint32_t v = b >= 8 ? v8 << (b - 8) : v8 >> (8 - b);
    return (v << shift_of(mask)) & mask;
}
static uint32_t px_get(const Surf *s, const uint8_t *p) {
    switch (s->bpp) {
    case 1: return p[0];
    case 2: return *(const uint16_t *)p;
    case 3: return p[0] | (p[1] << 8) | (p[2] << 16);
    default: return *(const uint32_t *)p;
    }
}
static void px_put(const Surf *s, uint8_t *p, uint32_t v) {
    switch (s->bpp) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: *(uint16_t *)p = (uint16_t)v; break;
    case 3: p[0] = v; p[1] = v >> 8; p[2] = v >> 16; break;
    default: *(uint32_t *)p = v; break;
    }
}
// pixel -> ARGB8888
static uint32_t px_to_argb(const Surf *s, uint32_t v) {
    if (s->pf_flags & DDPF_PALETTEINDEXED8) {
        uint32_t e = s->pal ? s->pal->e[v & 0xFF] : (v * 0x010101);
        return 0xFF000000u | ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
    }
    uint32_t a = s->amask ? chan_to8(v, s->amask) : 0xFF;
    return (a << 24) | (chan_to8(v, s->rmask) << 16) | (chan_to8(v, s->gmask) << 8) | chan_to8(v, s->bmask);
}
static uint32_t argb_to_px(const Surf *s, uint32_t c) {
    if (s->pf_flags & DDPF_PALETTEINDEXED8) return 0; // no reverse palette lookup
    return chan_from8((c >> 16) & 0xFF, s->rmask) | chan_from8((c >> 8) & 0xFF, s->gmask) | chan_from8(c & 0xFF, s->bmask) |
           (s->amask ? chan_from8(c >> 24, s->amask) : 0);
}

// ---------------------------------------------------------------- surfaces
static Surf *surf_of(uint32_t g) { return g ? com_host(g) : NULL; }

static uint32_t make_surf_obj(Surf *s) {
    s->g_surf = com_new(vt_surf, s, 0);
    return s->g_surf;
}

static Surf *surf_new(uint32_t w, uint32_t h, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->refs = 1;
    s->w = w;
    s->h = h;
    s->caps = caps;
    return s;
}
static void surf_alloc(Surf *s) {
    s->pitch = (s->w * s->bpp + 3) & ~3u;
    if (!s->mem && s->pitch && s->h) s->mem = gheap_alloc(s->pitch * s->h, 1);
}

// Copy the GPU back buffer into a surface's guest pixels.
static void rt_to_mem(Surf *s) {
    if (!ddg_ready() || !s->mem) return;
    uint32_t *tmp = malloc((size_t)s->w * s->h * 4);
    if (ddg_rt_read(tmp, s->w)) {
        for (uint32_t y = 0; y < s->h; y++) {
            uint8_t *row = (uint8_t *)G2H(s->mem) + y * s->pitch;
            for (uint32_t x = 0; x < s->w; x++) px_put(s, row + x * s->bpp, argb_to_px(s, tmp[y * s->w + x]));
        }
    }
    free(tmp);
}
// Write a rectangle of a surface's guest pixels to the GPU back buffer.
static void mem_to_rt(Surf *s, const int32_t *rect) {
    if (!ddg_ready() || !s->mem) return;
    uint32_t *tmp = malloc((size_t)s->w * s->h * 4);
    int32_t r[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    if (rect) memcpy(r, rect, sizeof r);
    for (int32_t y = r[1] < 0 ? 0 : r[1]; y < r[3] && y < (int32_t)s->h; y++) {
        const uint8_t *row = (const uint8_t *)G2H(s->mem) + y * s->pitch;
        for (int32_t x = r[0] < 0 ? 0 : r[0]; x < r[2] && x < (int32_t)s->w; x++)
            tmp[y * s->w + x] = px_to_argb(s, px_get(s, row + x * s->bpp));
    }
    ddg_rt_write(tmp, s->w, r);
    free(tmp);
}

// Upload a texture surface to its host texture (if its pixels changed).
static bool ck_enable;
static void tex_upload(Surf *s) {
    if (!s->mem || !ddg_ready()) return;
    bool want_ck = s->has_ck; // keyed texels become transparent
    bool direct = !want_ck && !(s->pf_flags & DDPF_PALETTEINDEXED8);
    int fmt = -1;
    if (direct) {
        if (s->bpp == 2 && s->rmask == 0xF800 && s->gmask == 0x07E0 && s->bmask == 0x1F) fmt = DDG_FMT_R5G6B5;
        else if (s->bpp == 2 && s->rmask == 0x7C00 && s->gmask == 0x03E0 && s->bmask == 0x1F)
            fmt = s->amask == 0x8000 ? DDG_FMT_A1R5G5B5 : DDG_FMT_X1R5G5B5;
        else if (s->bpp == 2 && s->rmask == 0x0F00 && s->gmask == 0x00F0 && s->bmask == 0x0F && s->amask == 0xF000)
            fmt = DDG_FMT_A4R4G4B4;
        else if (s->bpp == 4 && s->rmask == 0xFF0000 && s->gmask == 0xFF00 && s->bmask == 0xFF)
            fmt = s->amask ? DDG_FMT_A8R8G8B8 : DDG_FMT_X8R8G8B8;
    }
    if (fmt < 0) fmt = DDG_FMT_A8R8G8B8; // generic conversion
    if (s->tex && (s->tex_ck != want_ck)) { ddg_texture_free(s->tex); s->tex = NULL; }
    if (!s->tex) s->tex = ddg_texture_create(s->w, s->h, fmt);
    if (!s->tex) return;
    s->tex_ck = want_ck;
    if (fmt != DDG_FMT_A8R8G8B8 || (s->bpp == 4 && s->amask && !want_ck)) {
        ddg_texture_upload(s->tex, G2H(s->mem), s->pitch, s->w, s->h);
    } else {
        uint32_t *tmp = malloc((size_t)s->w * s->h * 4);
        for (uint32_t y = 0; y < s->h; y++) {
            const uint8_t *row = (const uint8_t *)G2H(s->mem) + y * s->pitch;
            for (uint32_t x = 0; x < s->w; x++) {
                uint32_t v = px_get(s, row + x * s->bpp);
                uint32_t c = px_to_argb(s, v);
                if (want_ck && v >= s->ck_lo && v <= s->ck_hi) c &= 0x00FFFFFF;
                tmp[y * s->w + x] = c;
            }
        }
        ddg_texture_upload(s->tex, tmp, s->w * 4, s->w, s->h);
        free(tmp);
    }
    s->dirty = false;
}

static void surf_unref(Surf *s) {
    if (--s->refs > 0) return;
    if (s->primary || s->backbuffer) return; // device-owned for the session
    if (s->tex) ddg_texture_free(s->tex);
    if (s->handle && s->handle < MAX_HANDLES) tex_handles[s->handle] = NULL;
    if (s->mem) gheap_free(s->mem);
    if (s->g_surf) gheap_free(s->g_surf);
    if (s->g_tex) gheap_free(s->g_tex);
    free(s);
}

static void fill_desc(const Surf *s, uint8_t *d, uint32_t surface_ptr) {
    uint32_t sz = *(uint32_t *)d;
    if (sz < SD_SIZE) sz = SD_SIZE;
    memset(d + 4, 0, sz - 4);
    uint32_t *u = (uint32_t *)d;
    u[0] = sz;
    u[1] = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    u[2] = s->h;
    u[3] = s->w;
    u[4] = s->pitch ? s->pitch : s->w * s->bpp;
    if (s->primary && s->back) { u[1] |= DDSD_BACKBUFFERCOUNT; u[5] = 1; }
    if (s->zbuf) { u[1] |= DDSD_ZBUFFERBITDEPTH; u[6] = 16; }
    if (surface_ptr) { u[1] |= DDSD_LPSURFACE; u[9] = surface_ptr; }
    if (s->has_ck) { u[1] |= DDSD_CKSRCBLT; u[16] = s->ck_lo; u[17] = s->ck_hi; }
    write_pf(s, u + 18);
    u[26] = s->caps;
}

// ---------------------------------------------------------------- IDirectDrawSurface 1/2/3
static uint32_t ok(uint32_t *a) { return DD_OK; }
static uint32_t notimpl(uint32_t *a) { return DDERR_UNSUPPORTED; }

static uint32_t s_qi(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    uint32_t d1;
    memcpy(&d1, G2H(a[1]), 4);
    uint32_t *out = G2H(a[2]);
    switch (d1) {
    case 0x6C14DB81: case 0x57805885: case 0xDA044E00: case 0x00000000: // Surface / 2 / 3 / IUnknown
        s->refs++;
        *out = s->g_surf;
        return DD_OK;
    case 0x2CDCD9E0: case 0x93281502: // IDirect3DTexture / 2
        if (!s->g_tex) s->g_tex = com_new(vt_tex, s, 0);
        s->refs++;
        *out = s->g_tex;
        return DD_OK;
    }
    LOGD("ddraw: surface QI {%08x} unsupported", d1);
    *out = 0;
    return E_NOINTERFACE;
}
static uint32_t s_addref(uint32_t *a) { return ++surf_of(a[0])->refs; }
static uint32_t s_release(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    int r = s->refs - 1;
    surf_unref(s);
    return r < 0 ? 0 : r;
}
static uint32_t s_add_attached(uint32_t *a) {
    Surf *s = surf_of(a[0]), *z = surf_of(a[1]);
    if (z && z->zbuf) { s->zattach = z; z->refs++; }
    return DD_OK;
}
static void clip_rect(const Surf *s, const int32_t *in, int32_t *r) {
    if (in) memcpy(r, in, 16);
    else { r[0] = 0; r[1] = 0; r[2] = s->w; r[3] = s->h; }
    if (r[0] < 0) r[0] = 0;
    if (r[1] < 0) r[1] = 0;
    if (r[2] > (int32_t)s->w) r[2] = s->w;
    if (r[3] > (int32_t)s->h) r[3] = s->h;
}
// Generic blit: CPU copy with optional stretching and source colour key.
static uint32_t do_blt(Surf *d, const int32_t *drect, Surf *s, const int32_t *srect, bool key, uint32_t klo, uint32_t khi) {
    int32_t dr[4], sr[4];
    clip_rect(d, drect, dr);
    clip_rect(s, srect, sr);
    int32_t dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !d->mem || !s->mem) return DD_OK;
    if (s->rt) rt_to_mem(s);
    if (d->rt && !(dw == (int32_t)d->w && dh == (int32_t)d->h && !key)) rt_to_mem(d);
    bool same = s->bpp == d->bpp && s->rmask == d->rmask && s->gmask == d->gmask && s->bmask == d->bmask &&
                (s->pf_flags & DDPF_PALETTEINDEXED8) == (d->pf_flags & DDPF_PALETTEINDEXED8);
    for (int32_t y = 0; y < dh; y++) {
        int32_t sy = sr[1] + y * sh / dh;
        uint8_t *drow = (uint8_t *)G2H(d->mem) + (dr[1] + y) * d->pitch;
        const uint8_t *srow = (const uint8_t *)G2H(s->mem) + sy * s->pitch;
        for (int32_t x = 0; x < dw; x++) {
            int32_t sx = sr[0] + x * sw / dw;
            uint32_t v = px_get(s, srow + sx * s->bpp);
            if (key && v >= klo && v <= khi) continue;
            px_put(d, drow + (dr[0] + x) * d->bpp, same ? v : argb_to_px(d, px_to_argb(s, v)));
        }
    }
    if (d->rt) mem_to_rt(d, dr);
    if (d->primary) ddg_present();
    d->dirty = true;
    return DD_OK;
}
static uint32_t s_blt(uint32_t *a) {
    Surf *d = surf_of(a[0]), *s = surf_of(a[2]);
    const int32_t *drect = a[1] ? G2H(a[1]) : NULL, *srect = a[3] ? G2H(a[3]) : NULL;
    uint32_t flags = a[4];
    uint32_t *fx = a[5] ? G2H(a[5]) : NULL;
    if (flags & DDBLT_COLORFILL) {
        uint32_t c = fx ? fx[20] : 0;
        int32_t r[4];
        clip_rect(d, drect, r);
        if (d->rt) { ddg_rt_fill(r, px_to_argb(d, c)); if (d->primary) ddg_present(); return DD_OK; }
        if (!d->mem) return DD_OK;
        for (int32_t y = r[1]; y < r[3]; y++)
            for (int32_t x = r[0]; x < r[2]; x++) px_put(d, (uint8_t *)G2H(d->mem) + y * d->pitch + x * d->bpp, c);
        d->dirty = true;
        return DD_OK;
    }
    if (flags & DDBLT_DEPTHFILL) {
        ddg_clear(0, NULL, 2 /*D3DCLEAR_ZBUFFER*/, 0, 1.0f);
        return DD_OK;
    }
    if (!s) return DDERR_INVALIDPARAMS;
    bool key = false;
    uint32_t lo = 0, hi = 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { key = true; lo = fx[23]; hi = fx[24]; }
    else if ((flags & DDBLT_KEYSRC) && s->has_ck) { key = true; lo = s->ck_lo; hi = s->ck_hi; }
    return do_blt(d, drect, s, srect, key, lo, hi);
}
static uint32_t s_bltfast(uint32_t *a) {
    Surf *d = surf_of(a[0]), *s = surf_of(a[3]);
    if (!s) return DDERR_INVALIDPARAMS;
    int32_t sr[4];
    clip_rect(s, a[4] ? G2H(a[4]) : NULL, sr);
    int32_t dr[4] = {(int32_t)a[1], (int32_t)a[2], (int32_t)a[1] + sr[2] - sr[0], (int32_t)a[2] + sr[3] - sr[1]};
    bool key = (a[5] & DDBLTFAST_SRCCOLORKEY) && s->has_ck;
    return do_blt(d, dr, s, sr, key, s->ck_lo, s->ck_hi);
}
static uint32_t s_flip(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->primary) return DDERR_NOTFLIPPABLE;
    ddg_present();
    return DD_OK;
}
static uint32_t s_get_attached(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    uint32_t caps = *(uint32_t *)G2H(a[1]);
    Surf *r = NULL;
    if ((caps & DDSCAPS_BACKBUFFER) && s->back) r = s->back;
    else if ((caps & DDSCAPS_ZBUFFER) && s->zattach) r = s->zattach;
    else if (caps & DDSCAPS_ZBUFFER && s->back && s->back->zattach) r = s->back->zattach;
    if (!r) { *(uint32_t *)G2H(a[2]) = 0; return DDERR_NOTFOUND; }
    r->refs++;
    *(uint32_t *)G2H(a[2]) = r->g_surf;
    return DD_OK;
}
static uint32_t s_enum_attached(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    Surf *list[2] = {s->back, s->zattach};
    uint32_t desc = gheap_alloc(SD_SIZE, 1);
    for (int i = 0; i < 2; i++) {
        if (!list[i]) continue;
        *(uint32_t *)G2H(desc) = SD_SIZE;
        fill_desc(list[i], G2H(desc), 0);
        list[i]->refs++;
        uint32_t args[3] = {list[i]->g_surf, desc, a[1]};
        if (!(uint32_t)call32(thread_current()->ctx, a[2], 3, args)) break;
    }
    gheap_free(desc);
    return DD_OK;
}
static uint32_t s_getcaps(uint32_t *a) { *(uint32_t *)G2H(a[1]) = surf_of(a[0])->caps; return DD_OK; }
static uint32_t s_getcolorkey(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->has_ck) return DDERR_NOCOLORKEY;
    uint32_t *k = G2H(a[2]);
    k[0] = s->ck_lo;
    k[1] = s->ck_hi;
    return DD_OK;
}
static uint32_t s_setcolorkey(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!(a[1] & DDCKEY_SRCBLT)) return DD_OK;
    if (!a[2]) { s->has_ck = false; s->dirty = true; return DD_OK; }
    uint32_t *k = G2H(a[2]);
    s->has_ck = true;
    s->ck_lo = k[0];
    s->ck_hi = k[1] < k[0] ? k[0] : k[1];
    s->dirty = true;
    return DD_OK;
}
static void dc_write(void *ctx, int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *argb) {
    Surf *s = ctx;
    if (!s->mem) return;
    if (s->rt) rt_to_mem(s);
    for (int32_t j = 0; j < h; j++) {
        if (y + j < 0 || y + j >= (int32_t)s->h) continue;
        uint8_t *row = (uint8_t *)G2H(s->mem) + (y + j) * s->pitch;
        for (int32_t i = 0; i < w; i++)
            if (x + i >= 0 && x + i < (int32_t)s->w) px_put(s, row + (x + i) * s->bpp, argb_to_px(s, argb[j * w + i]));
    }
    s->dirty = true;
    if (s->rt) {
        int32_t r[4] = {x, y, x + w, y + h};
        clip_rect(s, r, r);
        mem_to_rt(s, r);
        if (s->primary) ddg_present();
    }
}
static uint32_t s_getdc(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    GdiSurfaceDC d = {s, dc_write};
    uint32_t h = gdi_surface_dc(&d);
    if (!h) return DDERR_CANTCREATEDC;
    *(uint32_t *)G2H(a[1]) = h;
    return DD_OK;
}
static uint32_t s_releasedc(uint32_t *a) { gdi_surface_dc_free(a[1]); return DD_OK; }
static uint32_t s_getpalette(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->pal) return DDERR_NOPALETTEATTACHED;
    s->pal->refs++;
    *(uint32_t *)G2H(a[1]) = s->pal->g;
    return DD_OK;
}
static uint32_t s_getpf(uint32_t *a) { write_pf(surf_of(a[0]), G2H(a[1])); return DD_OK; }
static uint32_t s_getdesc(uint32_t *a) { fill_desc(surf_of(a[0]), G2H(a[1]), 0); return DD_OK; }
static uint32_t s_lock(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->mem) return DDERR_GENERIC;
    uint32_t flags = a[3];
    if (s->rt && !(flags & DDLOCK_WRITEONLY)) rt_to_mem(s);
    uint32_t p = s->mem;
    if (a[1]) {
        const int32_t *r = G2H(a[1]);
        p += r[1] * s->pitch + r[0] * s->bpp;
    }
    fill_desc(s, G2H(a[2]), p);
    s->locked = true;
    s->lock_flags = flags;
    return DD_OK;
}
static uint32_t s_unlock(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->locked) return DD_OK;
    s->locked = false;
    if (!(s->lock_flags & DDLOCK_READONLY)) {
        s->dirty = true;
        if (s->rt) mem_to_rt(s, NULL);
        if (s->primary) ddg_present();
    }
    return DD_OK;
}
static uint32_t s_setpalette(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    Palette *p = a[1] ? com_host(a[1]) : NULL;
    if (p) p->refs++;
    s->pal = p;
    s->dirty = true;
    return DD_OK;
}
static uint32_t s_getdd(uint32_t *a) { *(uint32_t *)G2H(a[1]) = g_dd2; return DD_OK; }
static uint32_t s_isLost(uint32_t *a) { return DD_OK; }

// ---------------------------------------------------------------- IDirect3DTexture2
static uint32_t t_qi(uint32_t *a) { uint32_t b[3] = {surf_of(a[0])->g_surf, a[1], a[2]}; return s_qi(b); }
static uint32_t t_addref(uint32_t *a) { return ++surf_of(a[0])->refs; }
static uint32_t t_release(uint32_t *a) { return s_release(a); }
static uint32_t t_gethandle(uint32_t *a) {
    Surf *s = surf_of(a[0]);
    if (!s->handle) {
        if (n_tex_handles >= MAX_HANDLES) return DDERR_GENERIC;
        s->handle = n_tex_handles++;
        tex_handles[s->handle] = s;
    }
    *(uint32_t *)G2H(a[2]) = s->handle;
    return DD_OK;
}
static uint32_t t_palchanged(uint32_t *a) { surf_of(a[0])->dirty = true; return DD_OK; }
static uint32_t t_load(uint32_t *a) {
    Surf *d = surf_of(a[0]), *s = surf_of(a[1]);
    if (!s || !d->mem || !s->mem) return DDERR_INVALIDPARAMS;
    uint32_t rows = d->h < s->h ? d->h : s->h, row = (d->pitch < s->pitch ? d->pitch : s->pitch);
    for (uint32_t y = 0; y < rows; y++)
        memcpy((uint8_t *)G2H(d->mem) + y * d->pitch, (uint8_t *)G2H(s->mem) + y * s->pitch, row);
    if (s->pal && !d->pal) { d->pal = s->pal; d->pal->refs++; }
    if (s->has_ck) { d->has_ck = true; d->ck_lo = s->ck_lo; d->ck_hi = s->ck_hi; }
    d->dirty = true;
    return DD_OK;
}

// ---------------------------------------------------------------- palettes / clippers
static uint32_t p_addref(uint32_t *a) { return ++((Palette *)com_host(a[0]))->refs; }
static uint32_t p_release(uint32_t *a) { Palette *p = com_host(a[0]); return p->refs > 1 ? --p->refs : 0; }
static uint32_t p_getcaps(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 0x4 | 0x40; return DD_OK; } // 8BIT | ALLOW256
static uint32_t p_getentries(uint32_t *a) {
    Palette *p = com_host(a[0]);
    for (uint32_t i = 0; i < a[3] && a[2] + i < 256; i++) ((uint32_t *)G2H(a[4]))[i] = p->e[a[2] + i];
    return DD_OK;
}
static uint32_t p_setentries(uint32_t *a) {
    Palette *p = com_host(a[0]);
    for (uint32_t i = 0; i < a[3] && a[2] + i < 256; i++) p->e[a[2] + i] = ((uint32_t *)G2H(a[4]))[i];
    if (primary && primary->pal == p && primary->bpp == 1 && primary->mem) mem_to_rt(backbuf ? backbuf : primary, NULL);
    return DD_OK;
}
static uint32_t c_addref(uint32_t *a) { return 1; }
static uint32_t c_gethwnd(uint32_t *a) { *(uint32_t *)G2H(a[1]) = coop_hwnd; return DD_OK; }

// ---------------------------------------------------------------- IDirectDraw / 2 / 7
static uint32_t dd_qi(uint32_t *a) {
    uint32_t d1;
    memcpy(&d1, G2H(a[1]), 4);
    uint32_t *out = G2H(a[2]);
    switch (d1) {
    case 0x6C14DB80: case 0x00000000: *out = g_dd1; return DD_OK; // IDirectDraw / IUnknown
    case 0xB3A6F3E0: *out = g_dd2; return DD_OK;                  // IDirectDraw2
    case 0x15E65EC0: *out = g_dd7; return DD_OK;                  // IDirectDraw7
    case 0x3BBA0080: case 0x6AAE1EC1: *out = g_d3d2; return DD_OK; // IDirect3D / 2
    }
    LOGI("ddraw: IDirectDraw QI {%08x} unsupported", d1);
    *out = 0;
    return E_NOINTERFACE;
}
static uint32_t dd_addref(uint32_t *a) { return 2; }
static uint32_t dd_release(uint32_t *a) { return 1; }

static uint32_t dd_create_palette(uint32_t *a) {
    Palette *p = calloc(1, sizeof *p);
    p->refs = 1;
    if (a[2]) memcpy(p->e, G2H(a[2]), 256 * 4);
    p->g = com_new(vt_pal, p, 0);
    *(uint32_t *)G2H(a[3]) = p->g;
    return DD_OK;
}
static uint32_t dd_create_clipper(uint32_t *a) {
    *(uint32_t *)G2H(a[2]) = com_new(vt_clip, NULL, 0);
    return DD_OK;
}

static bool want_fullscreen(void) { return (coop_flags & 0x1) != 0; } // DDSCL_FULLSCREEN

static uint32_t dd_create_surface(uint32_t *a) {
    const uint32_t *d = G2H(a[1]);
    uint32_t flags = d[1], caps = (flags & DDSD_CAPS) ? d[26] : 0;
    Surf *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        uint32_t w = mode_w, h = mode_h;
        if (!w) { display_desktop(&w, &h, NULL); }
        s = surf_new(w, h, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY);
        s->primary = s->rt = true;
        set_format(s, mode_bpp);
        surf_alloc(s);
        primary = s;
        if (!ddg_device(coop_hwnd, w, h, want_fullscreen())) return DDERR_GENERIC;
        if ((caps & DDSCAPS_FLIP) && (flags & DDSD_BACKBUFFERCOUNT) && d[5]) {
            Surf *b = surf_new(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | (caps & DDSCAPS_3DDEVICE));
            b->backbuffer = b->rt = true;
            set_format(b, mode_bpp);
            surf_alloc(b);
            make_surf_obj(b);
            s->back = b;
            backbuf = b;
        }
        LOGI("ddraw: primary surface %ux%u, %u-bit%s", w, h, mode_bpp, s->back ? " with back buffer" : "");
    } else {
        uint32_t w = (flags & DDSD_WIDTH) ? d[3] : mode_w, h = (flags & DDSD_HEIGHT) ? d[2] : mode_h;
        s = surf_new(w, h, caps ? caps : DDSCAPS_OFFSCREENPLAIN);
        if (caps & DDSCAPS_ZBUFFER) {
            s->zbuf = true;
            s->bpp = 2;
        } else {
            if (flags & DDSD_PIXELFORMAT) read_pf(s, d + 18);
            else set_format(s, mode_bpp);
            surf_alloc(s);
        }
        if (flags & DDSD_CKSRCBLT) { s->has_ck = true; s->ck_lo = d[16]; s->ck_hi = d[17] < d[16] ? d[16] : d[17]; }
        if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) s->caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        s->dirty = true;
        LOGD("ddraw: surface %ux%u caps 0x%x bpp %u masks %x/%x/%x/%x", w, h, caps, s->bpp * 8, s->rmask, s->gmask,
             s->bmask, s->amask);
    }
    *(uint32_t *)G2H(a[2]) = make_surf_obj(s);
    return DD_OK;
}

static void mode_desc(uint8_t *d, uint32_t w, uint32_t h, uint32_t bpp, uint32_t hz) {
    memset(d, 0, SD_SIZE);
    uint32_t *u = (uint32_t *)d;
    Surf tmp = {0};
    tmp.w = w; tmp.h = h;
    set_format(&tmp, bpp);
    u[0] = SD_SIZE;
    u[1] = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    u[2] = h;
    u[3] = w;
    u[4] = w * tmp.bpp;
    u[6] = hz;
    write_pf(&tmp, u + 18);
}
// Modes for the legacy interfaces: the usual resolutions the monitor supports plus
// the desktop size, at most 16. Games of that era keep fixed-size mode tables
// (Tomb Raider III overflows past 37 entries counting 16- and 32-bit), and drivers
// of the time listed about a dozen resolutions.
#define MAX_LEGACY_MODES 16
static int legacy_modes(uint32_t (*out)[2]) {
    static const uint16_t common[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},
                                         {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},
                                         {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                         {2560, 1440}, {3840, 2160}};
    uint32_t dw, dh, hz;
    display_desktop(&dw, &dh, &hz);
    int n = 0, total = display_mode_count();
    for (unsigned c = 0; c < sizeof common / sizeof *common && n < MAX_LEGACY_MODES - 1; c++)
        for (int i = 0; i < total; i++) {
            uint32_t w, h;
            display_mode(i, &w, &h, &hz);
            if (w == common[c][0] && h == common[c][1] && !(w == dw && h == dh)) {
                out[n][0] = w;
                out[n][1] = h;
                n++;
                break;
            }
        }
    // the desktop resolution, in order
    int at = n;
    for (int i = 0; i < n; i++)
        if (out[i][0] > dw || (out[i][0] == dw && out[i][1] > dh)) { at = i; break; }
    memmove(out + at + 1, out + at, (n - at) * sizeof *out);
    out[at][0] = dw;
    out[at][1] = dh;
    return n + 1;
}
static uint32_t dd_enum_modes(uint32_t *a) {
    const uint32_t *filt = a[2] ? G2H(a[2]) : NULL;
    uint32_t desc = gheap_alloc(SD_SIZE, 1);
    static const uint32_t depths[] = {8, 16, 32};
    uint32_t modes[MAX_LEGACY_MODES][2], hz;
    int n = legacy_modes(modes);
    display_desktop(&(uint32_t){0}, &(uint32_t){0}, &hz);
    for (int di = 0; di < 3; di++)
        for (int i = 0; i < n; i++) {
            uint32_t w = modes[i][0], h = modes[i][1];
            if (filt) {
                if ((filt[1] & DDSD_WIDTH) && filt[3] != w) continue;
                if ((filt[1] & DDSD_HEIGHT) && filt[2] != h) continue;
                if ((filt[1] & DDSD_PIXELFORMAT) && filt[21] != depths[di]) continue;
            }
            mode_desc(G2H(desc), w, h, depths[di], hz);
            uint32_t args[2] = {desc, a[3]};
            if (!(uint32_t)call32(thread_current()->ctx, a[4], 2, args)) goto done; // DDENUMRET_CANCEL
        }
done:
    gheap_free(desc);
    return DD_OK;
}
static uint32_t dd_getcaps(uint32_t *a) {
    for (int k = 1; k <= 2; k++) {
        uint32_t *c = a[k] ? G2H(a[k]) : NULL;
        if (!c) continue;
        uint32_t sz = c[0];
        memset(c + 1, 0, sz - 4);
        if (k == 2) continue; // HEL: nothing emulated
        c[1] = 0x1 | 0x40 | 0x200 | 0x400000 | 0x4000000; // 3D, BLT, BLTSTRETCH, COLORKEY, BLTCOLORFILL
        c[2] = 0x80000000; // DDCAPS2_FLIPNOVSYNC-ish: no restrictions
        c[3] = 0x200;      // DDCKEYCAPS_SRCBLT
        c[14] = DDBD_16 | DDBD_24 | DDBD_32;
        c[15] = c[16] = 0x10000000; // 256MB video memory
        if (sz > 132)
            c[33] = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_PRIMARYSURFACE |
                    DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY |
                    DDSCAPS_COMPLEX | DDSCAPS_FRONTBUFFER | DDSCAPS_LOCALVIDMEM;
    }
    return DD_OK;
}
static uint32_t dd_get_mode(uint32_t *a) {
    uint32_t w = mode_w, h = mode_h, hz = 60;
    if (!w) display_desktop(&w, &h, &hz);
    mode_desc(G2H(a[1]), w, h, mode_w ? mode_bpp : 32, hz);
    return DD_OK;
}
static uint32_t dd_set_coop(uint32_t *a) {
    coop_hwnd = a[1];
    coop_flags = a[2];
    return DD_OK;
}
static uint32_t set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    // Reject an implausible mode rather than resizing the render target to it.
    // A game can compute a garbage resolution when it reads an uninitialised
    // setting (e.g. a clean, config-less environment); passing it on corrupts the
    // surface and, with a real windowing backend, triggers a failed mode switch.
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        LOGI("ddraw: ignoring implausible SetDisplayMode %ux%u %u-bit", w, h, bpp);
        return DD_OK;
    }
    LOGI("ddraw: SetDisplayMode %ux%u %u-bit", w, h, bpp);
    mode_w = w;
    mode_h = h;
    mode_bpp = bpp;
    if (ddg_ready()) ddg_device(coop_hwnd, w, h, want_fullscreen());
    return DD_OK;
}
static uint32_t dd1_set_mode(uint32_t *a) { return set_mode(a[1], a[2], a[3]); }
static uint32_t dd2_set_mode(uint32_t *a) { return set_mode(a[1], a[2], a[3]); }
static uint32_t dd_restore_mode(uint32_t *a) { return DD_OK; }
static uint32_t dd_wait_vb(uint32_t *a) { return DD_OK; }
static uint32_t dd_get_vidmem(uint32_t *a) {
    if (a[2]) *(uint32_t *)G2H(a[2]) = 0x10000000;
    if (a[3]) *(uint32_t *)G2H(a[3]) = 0x10000000;
    return DD_OK;
}
static uint32_t dd_get_fourcc(uint32_t *a) { if (a[1]) *(uint32_t *)G2H(a[1]) = 0; return DD_OK; }
static uint32_t dd_get_monitor_freq(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 60; return DD_OK; }
static uint32_t dd_get_vb_status(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 0; return DD_OK; }
static uint32_t dd_get_scanline(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 0; return DD_OK; }

// ---------------------------------------------------------------- Direct3D 5
// D3DDEVICEDESC (DirectX 5 size: 204 bytes)
#define DEVDESC_SIZE 204
static void fill_primcaps(uint32_t *p) {
    p[0] = 56;
    p[1] = 0x2 | 0x10 | 0x20 | 0x40;           // MISCCAPS: MASKZ, CULLNONE, CULLCW, CULLCCW
    p[2] = 0x1 | 0x10 | 0x80 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x80000; // RASTERCAPS: DITHER, ZTEST, FOGVERTEX, ..
    p[3] = 0xFF;                               // ZCMPCAPS all
    p[4] = p[5] = 0x1FFF;                      // blend caps
    p[6] = 0xFF;                               // ALPHACMPCAPS
    p[7] = 0x1 | 0x8 | 0x200 | 0x4000 | 0x8000 | 0x80000; // SHADECAPS
    p[8] = 0x1 | 0x4 | 0x8 | 0x20 | 0x40;      // TEXTURECAPS: PERSPECTIVE, ALPHA, TRANSPARENCY, SQUAREONLY? (no), ..
    p[9] = 0x3F;                               // TEXTUREFILTERCAPS
    p[10] = 0xFF;                              // TEXTUREBLENDCAPS
    p[11] = 0x1F;                              // TEXTUREADDRESSCAPS
}
static void fill_devdesc(uint8_t *d, bool hal) {
    memset(d, 0, DEVDESC_SIZE);
    uint32_t *u = (uint32_t *)d;
    u[0] = DEVDESC_SIZE;
    if (!hal) return;
    u[1] = 0x7FF;            // all D3DDD_ flags
    u[2] = 2;                // D3DCOLOR_RGB
    u[3] = 0x1 | 0x40 | 0x200 | 0x400 | 0x800 | 0x80000 | 0x1000; // FLOATTLVERTEX, TLVERTEXSYSTEMMEMORY, TEXTUREVIDEOMEMORY, DRAWPRIMTLVERTEX, CANRENDERAFTERFLIP, HWRASTERIZATION, TEXTURENONLOCALVIDMEM
    u[4] = 8; u[5] = 1;      // dtcTransformCaps {size, CLIP}
    u[6] = TRUE;             // bClipping
    u[7] = 16; u[8] = 0x7; u[9] = 1; u[10] = 8; // lighting caps
    fill_primcaps(u + 11);   // line caps @44
    fill_primcaps(u + 25);   // tri caps @100
    u[39] = DDBD_16 | DDBD_32;          // render depths
    u[40] = DDBD_16 | DDBD_24 | DDBD_32; // z depths
    u[41] = 0;
    u[42] = 65535;           // max vertex count
    u[43] = 1; u[44] = 1; u[45] = 2048; u[46] = 2048; // texture size limits
}

static const uint8_t HAL_GUID[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};

static uint32_t d3d_qi(uint32_t *a) { uint32_t b[3] = {g_dd2, a[1], a[2]}; return dd_qi(b); }
static const uint8_t RGB_GUID[16] = {0x60, 0x5C, 0x66, 0xA4, 0x73, 0x26, 0xCF, 0x11, 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};

// Like Windows: "RGB Emulation" (software) first, then the HAL. Games store the
// index of the chosen device (Tomb Raider III's config.txt "D3D 1" is the HAL).
// Both are served by the same hardware device.
static uint32_t d3d_enum_devices(uint32_t *a) {
    static const struct { const uint8_t *guid; const char *desc, *name; bool hal; } devs[] = {
        {RGB_GUID, "Microsoft Direct3D RGB Software Emulation", "RGB Emulation", false},
        {HAL_GUID, "Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL", true}};
    for (unsigned i = 0; i < 2; i++) {
        uint32_t guid = gheap_alloc(16, 0), desc = gheap_strdup(devs[i].desc), name = gheap_strdup(devs[i].name);
        uint32_t hw = gheap_alloc(256, 1), hel = gheap_alloc(256, 1);
        memcpy(G2H(guid), devs[i].guid, 16);
        fill_devdesc(G2H(hw), devs[i].hal);
        fill_devdesc(G2H(hel), true);
        if (devs[i].hal) fill_devdesc(G2H(hel), false);
        uint32_t args[6] = {guid, desc, name, hw, hel, a[2]};
        uint32_t r = (uint32_t)call32(thread_current()->ctx, a[1], 6, args);
        uint32_t fr[] = {guid, desc, name, hw, hel};
        for (int k = 0; k < 5; k++) gheap_free(fr[k]);
        if (!r) break; // D3DENUMRET_CANCEL
    }
    return DD_OK;
}
static uint32_t d3d_find_device(uint32_t *a) {
    uint8_t *r = G2H(a[2]); // D3DFINDDEVICERESULT: size, guid, hw desc, sw desc
    memcpy(r + 4, HAL_GUID, 16);
    fill_devdesc(r + 20, true);
    fill_devdesc(r + 20 + DEVDESC_SIZE, false);
    return DD_OK;
}

// viewports, materials
typedef struct { uint32_t x, y, w, h; float minz, maxz; uint32_t bg_material; } Viewport;
typedef struct { float diffuse[4]; uint32_t handle; } Material;
#define MAX_MATERIALS 1024
static Material *materials[MAX_MATERIALS];
static int n_materials = 1;
static Viewport *cur_vp;

static uint32_t d3d_create_viewport(uint32_t *a) {
    Viewport *v = calloc(1, sizeof *v);
    v->w = mode_w ? mode_w : 640;
    v->h = mode_h ? mode_h : 480;
    v->maxz = 1;
    *(uint32_t *)G2H(a[1]) = com_new(vt_vp2, v, 0);
    return DD_OK;
}
static uint32_t d3d_create_material(uint32_t *a) {
    Material *m = calloc(1, sizeof *m);
    *(uint32_t *)G2H(a[1]) = com_new(vt_mat2, m, 0);
    return DD_OK;
}
static uint32_t d3d_create_device(uint32_t *a) {
    if (!ddg_ready()) return DDERR_GENERIC;
    *(uint32_t *)G2H(a[3]) = g_dev2;
    LOGI("ddraw: Direct3D 5 HAL device created");
    return DD_OK;
}

static void apply_viewport(Viewport *v) {
    if (v) ddg_viewport(v->x, v->y, v->w, v->h, v->minz, v->maxz);
}
static uint32_t vp_init(uint32_t *a) { return DD_OK; }
static uint32_t vp_get(uint32_t *a) {
    Viewport *v = com_host(a[0]);
    uint32_t *o = G2H(a[1]);
    o[1] = v->x; o[2] = v->y; o[3] = v->w; o[4] = v->h;
    o[5] = f2u(v->w / 2.0f); o[6] = f2u(v->h / 2.0f); o[7] = f2u(1); o[8] = f2u(1);
    o[9] = f2u(v->minz); o[10] = f2u(v->maxz);
    return DD_OK;
}
static uint32_t vp_set(uint32_t *a) { // D3DVIEWPORT and D3DVIEWPORT2 agree on x/y/w/h/minz/maxz
    Viewport *v = com_host(a[0]);
    const uint32_t *i = G2H(a[1]);
    v->x = i[1]; v->y = i[2]; v->w = i[3]; v->h = i[4];
    v->minz = u2f(i[9]);
    v->maxz = u2f(i[10]);
    if (v->maxz <= v->minz) { v->minz = 0; v->maxz = 1; }
    if (v == cur_vp) apply_viewport(v);
    return DD_OK;
}
static uint32_t vp_set_bg(uint32_t *a) { ((Viewport *)com_host(a[0]))->bg_material = a[1]; return DD_OK; }
static uint32_t vp_get_bg(uint32_t *a) {
    *(uint32_t *)G2H(a[1]) = ((Viewport *)com_host(a[0]))->bg_material;
    if (a[2]) *(uint32_t *)G2H(a[2]) = TRUE;
    return DD_OK;
}
static uint32_t vp_clear(uint32_t *a) {
    Viewport *v = com_host(a[0]);
    uint32_t color = 0;
    if (v->bg_material && v->bg_material < MAX_MATERIALS && materials[v->bg_material]) {
        float *c = materials[v->bg_material]->diffuse;
        color = (uint32_t)(fminf(fmaxf(c[3], 0), 1) * 255) << 24 | (uint32_t)(fminf(fmaxf(c[0], 0), 1) * 255) << 16 |
                (uint32_t)(fminf(fmaxf(c[1], 0), 1) * 255) << 8 | (uint32_t)(fminf(fmaxf(c[2], 0), 1) * 255);
    }
    ddg_clear(a[1], G2H(a[2]), a[3] & 3, color, 1.0f);
    return DD_OK;
}
static uint32_t mat_set(uint32_t *a) {
    Material *m = com_host(a[0]);
    memcpy(m->diffuse, (uint8_t *)G2H(a[1]) + 4, 16);
    return DD_OK;
}
static uint32_t mat_get(uint32_t *a) {
    Material *m = com_host(a[0]);
    memcpy((uint8_t *)G2H(a[1]) + 4, m->diffuse, 16);
    return DD_OK;
}
static uint32_t mat_handle(uint32_t *a) {
    Material *m = com_host(a[0]);
    if (!m->handle && n_materials < MAX_MATERIALS) { m->handle = n_materials++; materials[m->handle] = m; }
    *(uint32_t *)G2H(a[2]) = m->handle;
    return DD_OK;
}

// ---------------------------------------------------------------- IDirect3DDevice2
static uint32_t rstate[256];
static Surf *cur_tex;
static bool game_alphatest;

static void update_blend(void) {
    bool tex = cur_tex != NULL;
    bool tex_alpha = tex && (cur_tex->amask || cur_tex->tex_ck || cur_tex->has_ck);
    uint32_t mode = rstate[21] ? rstate[21] : 2;
    uint32_t cop = 4 /*MODULATE*/, aop = 2 /*SELECTARG1*/, aa1 = 2 /*TEXTURE*/;
    switch (mode) {
    case 1: case 5: case 7: cop = 2; aop = 2; break;           // DECAL, DECALMASK, COPY
    case 2: case 6: cop = 4; aop = tex_alpha ? 2 : 3; break;   // MODULATE (alpha from texture if it has one)
    case 3: cop = 13 /*BLENDTEXTUREALPHA*/; aop = 3; break;     // DECALALPHA
    case 4: cop = 4; aop = 4; break;                           // MODULATEALPHA
    case 8: cop = 7 /*ADD*/; aop = 3; break;                   // ADD
    }
    if (!tex) { cop = 3 /*SELECTARG2*/; aop = 3; }
    ddg_tss(0, 1 /*COLOROP*/, cop);
    ddg_tss(0, 2 /*COLORARG1*/, aa1);
    ddg_tss(0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
    ddg_tss(0, 4 /*ALPHAOP*/, aop);
    ddg_tss(0, 5 /*ALPHAARG1*/, 2);
    ddg_tss(0, 6 /*ALPHAARG2*/, 0);
    // colour-keyed textures: keyed texels have alpha 0, discard them
    bool ck = ck_enable && tex && cur_tex->has_ck;
    ddg_rs(15 /*ALPHATESTENABLE*/, game_alphatest || ck);
    if (ck && !game_alphatest) { ddg_rs(25 /*ALPHAFUNC*/, 5 /*GREATER*/); ddg_rs(24 /*ALPHAREF*/, 0); }
    else { ddg_rs(25, rstate[25] ? rstate[25] : 8); ddg_rs(24, rstate[24]); }
}

static uint32_t filter_of(uint32_t f) { return (f == 2 || f == 4 || f == 5 || f == 6) ? 2 /*LINEAR*/ : 1 /*POINT*/; }

static void set_rs(uint32_t s, uint32_t v) {
    if (s < 256) rstate[s] = v;
    switch (s) {
    case 1: // TEXTUREHANDLE
        cur_tex = v && v < MAX_HANDLES ? tex_handles[v] : NULL;
        if (cur_tex && (cur_tex->dirty || !cur_tex->tex)) tex_upload(cur_tex);
        ddg_set_texture(cur_tex ? cur_tex->tex : NULL);
        update_blend();
        break;
    case 3: ddg_sampler(1, v); ddg_sampler(2, v); break; // TEXTUREADDRESS -> U and V
    case 44: ddg_sampler(1, v); break;
    case 45: ddg_sampler(2, v); break;
    case 5: case 6: ddg_rs(128 /*WRAP0*/, (rstate[5] ? 1 : 0) | (rstate[6] ? 2 : 0)); break;
    case 17: ddg_sampler(5 /*MAGFILTER*/, filter_of(v)); break;
    case 18: ddg_sampler(6 /*MINFILTER*/, filter_of(v)); break;
    case 21: update_blend(); break; // TEXTUREMAPBLEND
    case 15: game_alphatest = v != 0; update_blend(); break;
    case 24: case 25: if (!(ck_enable && cur_tex && cur_tex->has_ck)) ddg_rs(s, v); break;
    case 41: ck_enable = v != 0; update_blend(); break; // COLORKEYENABLE
    case 43: ddg_sampler(4 /*BORDERCOLOR*/, v); break;
    case 46: ddg_sampler(8 /*MIPMAPLODBIAS*/, v); break;
    // directly equivalent states
    case 7: case 8: case 9: case 14: case 16: case 19: case 20: case 22: case 23: case 26: case 27: case 28:
    case 29: case 34: case 35: case 36: case 37: case 38: case 48: case 52: case 53: case 54: case 55: case 56:
    case 57: case 58: case 59:
        ddg_rs(s, v);
        break;
    default: break; // legacy states with no D3D9 equivalent (stipple, ROP2, ...)
    }
}

static uint32_t dev_getcaps(uint32_t *a) {
    if (a[1]) fill_devdesc(G2H(a[1]), true);
    if (a[2]) fill_devdesc(G2H(a[2]), false);
    return DD_OK;
}
static uint32_t dev_enum_texfmts(uint32_t *a) {
    // Same order as typical Windows HAL drivers (and Wine): games store the index
    // of the chosen format (Tomb Raider III's "D3DTF 1" means A1R5G5B5).
    static const uint32_t fmts[][6] = {{16, 0x7C00, 0x03E0, 0x1F, 0, DDPF_RGB},
                                       {16, 0x7C00, 0x03E0, 0x1F, 0x8000, DDPF_RGB | DDPF_ALPHAPIXELS},
                                       {16, 0x0F00, 0x00F0, 0x0F, 0xF000, DDPF_RGB | DDPF_ALPHAPIXELS},
                                       {16, 0xF800, 0x07E0, 0x1F, 0, DDPF_RGB},
                                       {32, 0xFF0000, 0xFF00, 0xFF, 0, DDPF_RGB},
                                       {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, DDPF_RGB | DDPF_ALPHAPIXELS},
                                       {8, 0, 0, 0, 0, DDPF_RGB | DDPF_PALETTEINDEXED8}};
    uint32_t desc = gheap_alloc(SD_SIZE, 1);
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        uint32_t *u = G2H(desc);
        memset(u, 0, SD_SIZE);
        u[0] = SD_SIZE;
        u[1] = DDSD_CAPS | DDSD_PIXELFORMAT;
        u[18] = 32; u[19] = fmts[i][5]; u[21] = fmts[i][0];
        u[22] = fmts[i][1]; u[23] = fmts[i][2]; u[24] = fmts[i][3]; u[25] = fmts[i][4];
        u[26] = DDSCAPS_TEXTURE;
        uint32_t args[2] = {desc, a[2]};
        if (!(uint32_t)call32(thread_current()->ctx, a[1], 2, args)) break;
    }
    gheap_free(desc);
    return DD_OK;
}
static uint32_t dev_begin_scene(uint32_t *a) { ddg_begin_scene(); return DD_OK; }
static uint32_t dev_end_scene(uint32_t *a) { ddg_end_scene(); return DD_OK; }
static uint32_t dev_get_d3d(uint32_t *a) { *(uint32_t *)G2H(a[1]) = g_d3d2; return DD_OK; }
static uint32_t dev_set_viewport(uint32_t *a) {
    cur_vp = a[1] ? com_host(a[1]) : NULL;
    apply_viewport(cur_vp);
    return DD_OK;
}
static uint32_t dev_get_viewport(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 0; return cur_vp ? DD_OK : DDERR_GENERIC; }
static uint32_t dev_get_rt(uint32_t *a) {
    if (!backbuf) return DDERR_GENERIC;
    backbuf->refs++;
    *(uint32_t *)G2H(a[1]) = backbuf->g_surf;
    return DD_OK;
}
static uint32_t dev_get_rs(uint32_t *a) { *(uint32_t *)G2H(a[2]) = a[1] < 256 ? rstate[a[1]] : 0; return DD_OK; }
static uint32_t dev_set_rs(uint32_t *a) { set_rs(a[1], a[2]); return DD_OK; }
static uint32_t dev_light_state(uint32_t *a) { return DD_OK; }
static uint32_t dev_set_transform(uint32_t *a) {
    // D3D5 transform states: WORLD 1, VIEW 2, PROJECTION 3 (D3D9 WORLD is 256)
    uint32_t t = a[1] == 1 ? 256 : a[1];
    extern void ddg_transform(uint32_t t, const float *m);
    ddg_transform(t, G2H(a[2]));
    return DD_OK;
}

static uint32_t prim_count(uint32_t prim, uint32_t n) {
    switch (prim) {
    case 1: return n;          // POINTLIST
    case 2: return n / 2;      // LINELIST
    case 3: return n ? n - 1 : 0; // LINESTRIP
    case 4: return n / 3;      // TRIANGLELIST
    case 5: case 6: return n >= 2 ? n - 2 : 0; // STRIP, FAN
    }
    return 0;
}
#define FVF_TL (0x004 | 0x040 | 0x080 | 0x100) // XYZRHW | DIFFUSE | SPECULAR | TEX1
#define FVF_L (0x002 | 0x040 | 0x080 | 0x100)  // XYZ | DIFFUSE | SPECULAR | TEX1

// Convert LVERTEX (x y z reserved color spec tu tv) to the FVF layout (drop reserved).
static void *lvert_convert(const uint8_t *v, uint32_t n) {
    uint8_t *o = malloc((size_t)n * 28);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(o + i * 28, v + i * 32, 12);
        memcpy(o + i * 28 + 12, v + i * 32 + 16, 16);
    }
    return o;
}
static void before_draw(void) {
    if (cur_tex && (cur_tex->dirty || !cur_tex->tex)) {
        tex_upload(cur_tex);
        ddg_set_texture(cur_tex->tex);
    }
}
static uint32_t dev_draw(uint32_t *a) {
    uint32_t prim = a[1], vt = a[2], n = a[4];
    before_draw();
    if (vt == 3) ddg_draw(prim, prim_count(prim, n), G2H(a[3]), 32, FVF_TL);
    else if (vt == 2) {
        void *c = lvert_convert(G2H(a[3]), n);
        ddg_draw(prim, prim_count(prim, n), c, 28, FVF_L);
        free(c);
    } else LOGD("ddraw: DrawPrimitive with untransformed D3DVERTEX not supported");
    return DD_OK;
}
static uint32_t dev_draw_indexed(uint32_t *a) {
    uint32_t prim = a[1], vt = a[2], nv = a[4], ni = a[6];
    before_draw();
    if (vt == 3) ddg_draw_indexed(prim, nv, prim_count(prim, ni), G2H(a[5]), G2H(a[3]), 32, FVF_TL);
    else if (vt == 2) {
        void *c = lvert_convert(G2H(a[3]), nv);
        ddg_draw_indexed(prim, nv, prim_count(prim, ni), G2H(a[5]), c, 28, FVF_L);
        free(c);
    }
    return DD_OK;
}
// Begin/Vertex/End immediate mode
static uint8_t imm_buf[65536 * 32];
static uint32_t imm_n, imm_prim, imm_vt;
static uint32_t dev_begin(uint32_t *a) { imm_prim = a[1]; imm_vt = a[2]; imm_n = 0; return DD_OK; }
static uint32_t dev_vertex(uint32_t *a) {
    if (imm_n < 65536) memcpy(imm_buf + imm_n++ * 32, G2H(a[1]), 32);
    return DD_OK;
}
static uint32_t dev_end(uint32_t *a) {
    before_draw();
    if (imm_vt == 3) ddg_draw(imm_prim, prim_count(imm_prim, imm_n), imm_buf, 32, FVF_TL);
    return DD_OK;
}
static uint32_t dev_stats(uint32_t *a) { memset((uint8_t *)G2H(a[1]) + 4, 0, 16); return DD_OK; }
static uint32_t dev_clipstatus(uint32_t *a) { return DD_OK; }
static uint32_t dev_swap_handles(uint32_t *a) { return DD_OK; }

// ---------------------------------------------------------------- vtables / entry points
static void build_vtables(void) {
    if (vt_surf) return;
    ComMethod dd[24] = {
        {dd_qi, 3}, {dd_addref, 1}, {dd_release, 1}, {ok, 1}, {dd_create_clipper, 4}, {dd_create_palette, 5},
        {dd_create_surface, 4}, {notimpl, 3}, {dd_enum_modes, 5}, {notimpl, 5}, {ok, 1}, {dd_getcaps, 3},
        {dd_get_mode, 2}, {dd_get_fourcc, 3}, {notimpl, 2}, {dd_get_monitor_freq, 2}, {dd_get_scanline, 2},
        {dd_get_vb_status, 2}, {ok, 2}, {dd_restore_mode, 1}, {dd_set_coop, 3}, {dd1_set_mode, 4}, {dd_wait_vb, 3},
        {dd_get_vidmem, 4}};
    vt_dd1 = com_vtable("IDirectDraw", dd, 23);
    dd[21] = (ComMethod){dd2_set_mode, 6};
    vt_dd2 = com_vtable("IDirectDraw2", dd, 24);
    ComMethod dd7[30] = {{dd_qi, 3}, {dd_addref, 1}, {dd_release, 1}, [11] = {dd_getcaps, 3},
                         [20] = {dd_set_coop, 3}, [23] = {dd_get_vidmem, 4}};
    vt_dd7 = com_vtable("IDirectDraw7", dd7, 30);
    ComMethod s[40] = {
        {s_qi, 3}, {s_addref, 1}, {s_release, 1}, {s_add_attached, 2}, {notimpl, 2}, {s_blt, 6}, {notimpl, 4},
        {s_bltfast, 6}, {ok, 3}, {s_enum_attached, 3}, {notimpl, 4}, {s_flip, 3}, {s_get_attached, 3}, {ok, 2},
        {s_getcaps, 2}, {notimpl, 2}, {s_getcolorkey, 3}, {s_getdc, 2}, {ok, 2}, {notimpl, 3}, {s_getpalette, 2},
        {s_getpf, 2}, {s_getdesc, 2}, {ok, 3}, {s_isLost, 1}, {s_lock, 5}, {s_releasedc, 2}, {ok, 1}, {ok, 2},
        {s_setcolorkey, 3}, {notimpl, 3}, {s_setpalette, 2}, {s_unlock, 2}, {notimpl, 6}, {notimpl, 2},
        {notimpl, 3}, {s_getdd, 2}, {ok, 2}, {ok, 2}, {notimpl, 3}};
    vt_surf = com_vtable("IDirectDrawSurface3", s, 40);
    ComMethod t[6] = {{t_qi, 3}, {t_addref, 1}, {t_release, 1}, {t_gethandle, 3}, {t_palchanged, 3}, {t_load, 2}};
    vt_tex = com_vtable("IDirect3DTexture2", t, 6);
    ComMethod p[7] = {{com_qi_self, 3}, {p_addref, 1}, {p_release, 1}, {p_getcaps, 2}, {p_getentries, 5}, {ok, 4},
                      {p_setentries, 5}};
    vt_pal = com_vtable("IDirectDrawPalette", p, 7);
    ComMethod c[9] = {{com_qi_self, 3}, {c_addref, 1}, {c_addref, 1}, {notimpl, 4}, {c_gethwnd, 2}, {ok, 3},
                      {ok, 2}, {ok, 3}, {ok, 3}};
    vt_clip = com_vtable("IDirectDrawClipper", c, 9);
    ComMethod d3[9] = {{d3d_qi, 3}, {dd_addref, 1}, {dd_release, 1}, {d3d_enum_devices, 3}, {notimpl, 3},
                       {d3d_create_material, 3}, {d3d_create_viewport, 3}, {d3d_find_device, 3},
                       {d3d_create_device, 4}};
    vt_d3d2 = com_vtable("IDirect3D2", d3, 9);
    ComMethod dv[33] = {
        {com_qi_self, 3}, {dd_addref, 1}, {dd_release, 1}, {dev_getcaps, 3}, {dev_swap_handles, 3}, {dev_stats, 2},
        {ok, 2}, {ok, 2}, {notimpl, 4}, {dev_enum_texfmts, 3}, {dev_begin_scene, 1}, {dev_end_scene, 1},
        {dev_get_d3d, 2}, {dev_set_viewport, 2}, {dev_get_viewport, 2}, {ok, 3}, {dev_get_rt, 2}, {dev_begin, 4},
        {notimpl, 6}, {dev_vertex, 2}, {ok, 2}, {dev_end, 2}, {dev_get_rs, 3}, {dev_set_rs, 3},
        {dev_light_state, 3}, {dev_light_state, 3}, {dev_set_transform, 3}, {notimpl, 3}, {notimpl, 3},
        {dev_draw, 6}, {dev_draw_indexed, 8}, {dev_clipstatus, 2}, {dev_clipstatus, 2}};
    vt_dev2 = com_vtable("IDirect3DDevice2", dv, 33);
    ComMethod vp[19] = {{com_qi_self, 3}, {com_addref, 1}, {com_release_nofree, 1}, {vp_init, 2}, {vp_get, 2},
                        {vp_set, 2}, {notimpl, 5}, {notimpl, 3}, {vp_set_bg, 2}, {vp_get_bg, 3}, {ok, 2},
                        {notimpl, 3}, {vp_clear, 4}, {ok, 2}, {ok, 2}, {notimpl, 4}, {vp_get, 2}, {vp_set, 2}};
    vt_vp2 = com_vtable("IDirect3DViewport2", vp, 18);
    ComMethod mt[6] = {{com_qi_self, 3}, {com_addref, 1}, {com_release_nofree, 1}, {mat_set, 2}, {mat_get, 2},
                       {mat_handle, 3}};
    vt_mat2 = com_vtable("IDirect3DMaterial2", mt, 6);
    g_dd1 = com_new(vt_dd1, NULL, 0);
    g_dd2 = com_new(vt_dd2, NULL, 0);
    g_dd7 = com_new(vt_dd7, NULL, 0);
    g_d3d2 = com_new(vt_d3d2, NULL, 0);
    g_dev2 = com_new(vt_dev2, NULL, 0);
}

API(ddraw, DirectDrawCreate, 3) {
    build_vtables();
    *(uint32_t *)PTR(1) = g_dd1;
    return DD_OK;
}
API(ddraw, DirectDrawCreateEx, 4) {
    build_vtables();
    *(uint32_t *)PTR(1) = g_dd7;
    return DD_OK;
}
// DirectDrawEnumerateA(callback, ctx): the primary display driver (GUID NULL)
API(ddraw, DirectDrawEnumerateA, 2) {
    uint32_t desc = gheap_strdup("Primary Display Driver"), name = gheap_strdup("display");
    uint32_t args[4] = {0, desc, name, ARG(1)};
    call32(thread_current()->ctx, ARG(0), 4, args);
    gheap_free(desc);
    gheap_free(name);
    return DD_OK;
}

// d3d8.dll: GTA III / Vice City render with D3D8 (d3d8/d3d8_bridge.cpp).
uint32_t d3d8_bridge_create(uint32_t sdk);
API(d3d8, Direct3DCreate8, 1) {
    LOGI("Direct3DCreate8(sdk %u)", ARG(0));
    return d3d8_bridge_create(ARG(0));
}
// dpnhpast.dll (DirectPlay NAT helper) only needs to exist (GTA:SA's DirectX check).
API(dpnhpast, DllCanUnloadNow, 0) { return S_FALSE; }
