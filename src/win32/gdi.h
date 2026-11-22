#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A DC that draws into something other than plain memory (a DirectDraw surface
// that may live on the GPU): BitBlt hands it blocks of pixels.
typedef struct {
    void *ctx;
    // Write a w x h block of ARGB pixels at (x, y).
    void (*write)(void *ctx, int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *argb);
} GdiSurfaceDC;

uint32_t gdi_surface_dc(const GdiSurfaceDC *s);

// A DC over pixels in (host) memory, e.g. a locked Direct3D surface.
enum { GDI_FMT_XRGB8888, GDI_FMT_RGB888, GDI_FMT_R5G6B5, GDI_FMT_X1R5G5B5 };
uint32_t gdi_memory_dc(void *bits, int w, int h, int pitch, int fmt);

// Release a DC made by either function.
void gdi_surface_dc_free(uint32_t hdc);

#ifdef __cplusplus
}
#endif
