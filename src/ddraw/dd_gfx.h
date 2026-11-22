#pragma once
// Host-side rendering for the DirectDraw / legacy Direct3D layer (ddraw.c):
// a thin C API over a DXVK D3D9 device.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DDG_FMT_R5G6B5, DDG_FMT_X1R5G5B5, DDG_FMT_A1R5G5B5, DDG_FMT_A4R4G4B4, DDG_FMT_A8R8G8B8, DDG_FMT_X8R8G8B8 };

// (Re)create the device for a w x h back buffer presented into the game window.
int ddg_device(uint32_t hwnd, uint32_t w, uint32_t h, int fullscreen_requested);
int ddg_ready(void);
void ddg_present(void);

void *ddg_texture_create(uint32_t w, uint32_t h, int fmt);
void ddg_texture_upload(void *tex, const void *data, uint32_t pitch, uint32_t w, uint32_t h);
void ddg_texture_free(void *tex);
void ddg_set_texture(void *tex); // NULL unbinds

void ddg_rs(uint32_t state, uint32_t value);       // D3D9 render state
void ddg_sampler(uint32_t state, uint32_t value);  // D3D9 sampler state, stage 0
void ddg_tss(uint32_t stage, uint32_t state, uint32_t value);

void ddg_begin_scene(void);
void ddg_end_scene(void);
void ddg_draw(uint32_t prim, uint32_t nprims, const void *verts, uint32_t stride, uint32_t fvf);
void ddg_draw_indexed(uint32_t prim, uint32_t nverts, uint32_t nprims, const uint16_t *idx, const void *verts,
                      uint32_t stride, uint32_t fvf);
void ddg_clear(uint32_t nrects, const int32_t *rects, uint32_t flags, uint32_t color, float z);
void ddg_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz);

// Back buffer access as 32-bit XRGB pixels (rect: x0,y0,x1,y1 in back-buffer pixels).
int ddg_rt_read(uint32_t *dst, uint32_t dst_pitch_px);
void ddg_rt_write(const uint32_t *src, uint32_t src_pitch_px, const int32_t *rect);
void ddg_rt_fill(const int32_t *rect, uint32_t xrgb);

#ifdef __cplusplus
}
#endif
