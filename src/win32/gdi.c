// GDI: bitmaps, memory DCs, fonts and text, clip rectangles, lines, palettes.
//
// Enough for games that draw their UI text with GDI onto surfaces (Age of
// Empires II HD) and blit .bmp files (Tomb Raider III). Text is rasterized
// with stb_truetype from the fonts a game registers (AddFontResourceEx) or the
// system's fonts, with GDI's metric rules (OS/2 win ascent/descent sizing).
// DC targets are plain pixel memory (bitmaps, locked Direct3D surfaces) or a
// write callback (DirectDraw surfaces).
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../heap.h"
#include "../vfs.h"
#include "gdi.h"
#include "win32.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------- objects
enum { K_BITMAP = 1, K_DC, K_FONT, K_REGION, K_PALETTE };

typedef struct {
    uint32_t w, h, bpp, stride; // a bottom-up DIB in guest memory
    uint32_t bits;
    uint32_t pal[256]; // ARGB
} Bitmap;

typedef struct { int32_t l, t, r, b; } Rect;

typedef struct Face Face;
typedef struct {
    int32_t height, width, escapement, orientation, weight;
    uint8_t italic, underline, strike, charset, outprec, clipprec, quality, pitch;
    uint16_t face[32];
} LogFont; // LOGFONTW without the padding

typedef struct {
    LogFont lf;
    Face *face;
    float sx, sy;
    int ascent, descent, int_lead, ext_lead, avgw, maxw;
    bool fake_bold, fake_italic;
} Font;

typedef struct { uint32_t n; uint32_t e[256]; } Palette; // PALETTEENTRY: r, g, b, flags

typedef struct {
    uint32_t bitmap, font;
    uint32_t text_color, bk_color, pen_color; // COLORREF 0x00BBGGRR
    int bk_mode; // 1 TRANSPARENT, 2 OPAQUE
    uint32_t align;
    int32_t cx, cy;
    bool has_clip;
    Rect clip;
    GdiSurfaceDC surf; // callback target
    uint8_t *mem;      // memory target
    int w, h, pitch, fmt;
} DC;

typedef struct {
    bool used;
    int kind;
    void *p;
} GdiObj;

#define GDI_BASE 0x00470000u
#define GDI_MAX 4096
static GdiObj objs[GDI_MAX];
static pthread_mutex_t gdi_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t obj_add(int kind, void *p) {
    pthread_mutex_lock(&gdi_lock);
    for (int i = 1; i < GDI_MAX; i++)
        if (!objs[i].used) {
            objs[i] = (GdiObj){true, kind, p};
            pthread_mutex_unlock(&gdi_lock);
            return GDI_BASE + i;
        }
    pthread_mutex_unlock(&gdi_lock);
    LOGE("gdi: out of handles");
    return 0;
}
static void *obj_get(uint32_t h, int kind) {
    if (h <= GDI_BASE || h >= GDI_BASE + GDI_MAX) return NULL;
    GdiObj *o = &objs[h - GDI_BASE];
    return o->used && o->kind == kind ? o->p : NULL;
}
static int obj_kind(uint32_t h) {
    if (h <= GDI_BASE || h >= GDI_BASE + GDI_MAX || !objs[h - GDI_BASE].used) return 0;
    return objs[h - GDI_BASE].kind;
}
static void obj_free(uint32_t h) {
    if (h > GDI_BASE && h < GDI_BASE + GDI_MAX) objs[h - GDI_BASE].used = false;
}

static DC *dc_new(void) {
    DC *d = calloc(1, sizeof *d);
    d->text_color = 0;
    d->bk_color = 0xFFFFFF;
    d->bk_mode = 2;
    return d;
}

uint32_t gdi_surface_dc(const GdiSurfaceDC *s) {
    DC *d = dc_new();
    d->surf = *s;
    return obj_add(K_DC, d);
}
uint32_t gdi_memory_dc(void *bits, int w, int h, int pitch, int fmt) {
    DC *d = dc_new();
    d->mem = bits;
    d->w = w;
    d->h = h;
    d->pitch = pitch;
    d->fmt = fmt;
    return obj_add(K_DC, d);
}
void gdi_surface_dc_free(uint32_t hdc) {
    DC *d = obj_get(hdc, K_DC);
    if (!d) return;
    obj_free(hdc);
    free(d);
}

// ---------------------------------------------------------------- pixel targets
typedef struct {
    uint8_t *row0; // top row
    int pitch, w, h, fmt;
    const uint32_t *pal; // 8-bit sources
} Target;
#define FMT_PAL8 100
#define FMT_PAL4 101
#define FMT_PAL1 102

static bool bitmap_target(Bitmap *b, Target *t) {
    t->row0 = (uint8_t *)G2H(b->bits) + (size_t)(b->h - 1) * b->stride;
    t->pitch = -(int)b->stride;
    t->w = b->w;
    t->h = b->h;
    t->pal = b->pal;
    switch (b->bpp) {
    case 32: t->fmt = GDI_FMT_XRGB8888; break;
    case 24: t->fmt = GDI_FMT_RGB888; break;
    case 16: t->fmt = GDI_FMT_X1R5G5B5; break;
    case 8: t->fmt = FMT_PAL8; break;
    case 4: t->fmt = FMT_PAL4; break;
    default: t->fmt = FMT_PAL1; break;
    }
    return true;
}
static bool dc_target(DC *d, Target *t) {
    if (d->mem) {
        *t = (Target){d->mem, d->pitch, d->w, d->h, d->fmt, NULL};
        return true;
    }
    Bitmap *b = obj_get(d->bitmap, K_BITMAP);
    return b && bitmap_target(b, t);
}
// 0x00RRGGBB
static uint32_t px_get(const Target *t, int x, int y) {
    const uint8_t *r = t->row0 + (ptrdiff_t)y * t->pitch;
    switch (t->fmt) {
    case GDI_FMT_XRGB8888: return ((const uint32_t *)r)[x] & 0xFFFFFF;
    case GDI_FMT_RGB888: return r[x * 3] | r[x * 3 + 1] << 8 | r[x * 3 + 2] << 16;
    case GDI_FMT_R5G6B5: {
        uint16_t v = ((const uint16_t *)r)[x];
        uint32_t R = v >> 11, G = (v >> 5) & 63, B = v & 31;
        return (R << 3 | R >> 2) << 16 | (G << 2 | G >> 4) << 8 | (B << 3 | B >> 2);
    }
    case GDI_FMT_X1R5G5B5: {
        uint16_t v = ((const uint16_t *)r)[x];
        uint32_t R = (v >> 10) & 31, G = (v >> 5) & 31, B = v & 31;
        return (R << 3 | R >> 2) << 16 | (G << 3 | G >> 2) << 8 | (B << 3 | B >> 2);
    }
    case FMT_PAL8: return t->pal[r[x]] & 0xFFFFFF;
    case FMT_PAL4: return t->pal[(r[x >> 1] >> ((x & 1) ? 0 : 4)) & 15] & 0xFFFFFF;
    default: return t->pal[(r[x >> 3] >> (7 - (x & 7))) & 1] & 0xFFFFFF;
    }
}
static int nearest_pal(const uint32_t *pal, int n, uint32_t c) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < n; i++) {
        int dr = (int)((pal[i] >> 16) & 255) - (int)((c >> 16) & 255), dg = (int)((pal[i] >> 8) & 255) - (int)((c >> 8) & 255),
            db = (int)(pal[i] & 255) - (int)(c & 255);
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}
static void px_put(const Target *t, int x, int y, uint32_t c) {
    uint8_t *r = t->row0 + (ptrdiff_t)y * t->pitch;
    uint32_t R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
    switch (t->fmt) {
    // GDI is alpha-unaware, but games draw text onto an A8R8G8B8 surface and then
    // composite it with alpha (Age of Empires II HD draws white glyphs + black
    // shadow and tints the result). Carry the drawn luminance into the alpha so
    // white text is opaque and the black shadow fades; opaque X8R8G8B8 ignores it.
    case GDI_FMT_XRGB8888: {
        uint32_t a = R > G ? R : G;
        if (B > a) a = B;
        ((uint32_t *)r)[x] = (a << 24) | (c & 0xFFFFFF);
        break;
    }
    case GDI_FMT_RGB888: r[x * 3] = B; r[x * 3 + 1] = G; r[x * 3 + 2] = R; break;
    case GDI_FMT_R5G6B5: ((uint16_t *)r)[x] = (R >> 3) << 11 | (G >> 2) << 5 | B >> 3; break;
    case GDI_FMT_X1R5G5B5: ((uint16_t *)r)[x] = (R >> 3) << 10 | (G >> 3) << 5 | B >> 3; break;
    case FMT_PAL8: r[x] = nearest_pal(t->pal, 256, c); break;
    default: break; // drawing into 1/4-bit bitmaps is not supported
    }
}
static uint32_t cref_rgb(uint32_t c) { return (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// Clip box for a DC: target bounds intersected with the clip rectangle.
static Rect dc_clip(DC *d, const Target *t) {
    Rect r = {0, 0, t->w, t->h};
    if (d->has_clip) {
        if (d->clip.l > r.l) r.l = d->clip.l;
        if (d->clip.t > r.t) r.t = d->clip.t;
        if (d->clip.r < r.r) r.r = d->clip.r;
        if (d->clip.b < r.b) r.b = d->clip.b;
    }
    return r;
}
static void fill(const Target *t, Rect clip, Rect r, uint32_t rgb) {
    if (r.l < clip.l) r.l = clip.l;
    if (r.t < clip.t) r.t = clip.t;
    if (r.r > clip.r) r.r = clip.r;
    if (r.b > clip.b) r.b = clip.b;
    for (int y = r.t; y < r.b; y++)
        for (int x = r.l; x < r.r; x++) px_put(t, x, y, rgb);
}

// ---------------------------------------------------------------- strings
// Windows-1252 bytes 0x80-0x9F; the rest of the code page equals Latin-1.
static const uint16_t cp1252_hi[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                       0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                       0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
// Narrow string (n < 0: NUL-terminated) -> UTF-16 in a malloc'd buffer.
static uint16_t *widen(const char *s, int n, int *outn) {
    if (n < 0) n = strlen(s);
    uint16_t *w = malloc((n + 1) * 2);
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        w[i] = c >= 0x80 && c < 0xA0 ? cp1252_hi[c - 0x80] : c;
    }
    w[n] = 0;
    *outn = n;
    return w;
}

// ---------------------------------------------------------------- font files
// A face in a font file, with what GDI matches on (family name, bold, italic).
struct Face {
    char path[1024];
    int index;     // face number inside a .ttc
    char family[96]; // lowercase
    bool bold, italic;
    bool game;     // registered by the game (preferred)
    // loaded on first use
    bool loaded, bad;
    uint8_t *data;
    size_t size;
    stbtt_fontinfo info;
    int upem, win_asc, win_desc, hhea_asc, hhea_desc, hhea_gap, avg_w, xmin, xmax, weight;
};
static Face **faces; // individually allocated: fonts keep pointers to them
static int nfaces, capfaces;
static bool system_scanned;

static uint8_t *map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    void *p = MAP_FAILED;
    if (!fstat(fd, &st) && st.st_size > 12) p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = st.st_size;
    return p;
}

static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

// nameID from the Microsoft/Unicode name records, as lowercase ASCII-ish UTF-8.
static bool font_name(const stbtt_fontinfo *fi, int id, char *out, size_t n) {
    int len = 0;
    const char *s = stbtt_GetFontNameString(fi, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                            STBTT_MS_LANG_ENGLISH, id);
    if (!s) s = stbtt_GetFontNameString(fi, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_SYMBOL,
                                        STBTT_MS_LANG_ENGLISH, id);
    if (!s) return false;
    size_t o = 0;
    for (int i = 0; i + 1 < len && o + 1 < n; i += 2) {
        uint16_t c = be16((const uint8_t *)s + i);
        out[o++] = c < 128 ? tolower(c) : '?';
    }
    out[o] = 0;
    return o > 0;
}

static void add_faces(const char *path, bool game) {
    size_t size;
    uint8_t *d = map_file(path, &size);
    if (!d) return;
    int n = stbtt_GetNumberOfFonts(d);
    for (int i = 0; i < n && i < 16; i++) {
        int off = stbtt_GetFontOffsetForIndex(d, i);
        stbtt_fontinfo fi;
        if (off < 0 || !stbtt_InitFont(&fi, d, off)) continue;
        Face f = {0};
        snprintf(f.path, sizeof f.path, "%s", path);
        f.index = i;
        f.game = game;
        if (!font_name(&fi, 1, f.family, sizeof f.family)) continue;
        char sub[64] = "";
        font_name(&fi, 2, sub, sizeof sub);
        f.bold = strstr(sub, "bold") || strstr(sub, "black") || strstr(sub, "heavy");
        f.italic = strstr(sub, "italic") || strstr(sub, "oblique");
        uint32_t os2 = stbtt__find_table(d, off, "OS/2");
        if (os2 && os2 + 64 <= size) {
            uint16_t sel = be16(d + os2 + 62);
            f.italic |= sel & 1;
            f.bold |= (sel & 0x20) != 0;
        }
        if (nfaces == capfaces) faces = realloc(faces, (capfaces = capfaces ? capfaces * 2 : 64) * sizeof *faces);
        faces[nfaces] = malloc(sizeof f);
        *faces[nfaces++] = f;
    }
    munmap(d, size);
}

static void scan_dir(const char *dir, int depth) {
    if (depth > 6) return;
    DIR *dd = opendir(dir);
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(p, depth + 1);
        else {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf") || !strcasecmp(ext, ".ttc")))
                add_faces(p, false);
        }
    }
    closedir(dd);
}

static void scan_system_fonts(void) {
    if (system_scanned) return;
    system_scanned = true;
    const char *home = getenv("HOME");
    char p[1024];
    scan_dir("/usr/share/fonts", 0);
    scan_dir("/usr/local/share/fonts", 0);
    if (home) {
        snprintf(p, sizeof p, "%s/.local/share/fonts", home);
        scan_dir(p, 0);
        snprintf(p, sizeof p, "%s/.fonts", home);
        scan_dir(p, 0);
    }
    LOGD("gdi: %d font faces indexed", nfaces);
}

// Fonts a game registers from its own folder (AddFontResourceEx).
static uint32_t add_font(const char *winpath) {
    char host[4096];
    if (!vfs_to_host(winpath, host, sizeof host)) return 0;
    pthread_mutex_lock(&gdi_lock);
    int before = nfaces;
    bool dup = false;
    for (int i = 0; i < nfaces && !dup; i++) dup = faces[i]->game && !strcmp(faces[i]->path, host);
    if (!dup) add_faces(host, true);
    int added = nfaces - before;
    pthread_mutex_unlock(&gdi_lock);
    LOGD("AddFontResource: %s (%d faces)", host, added);
    return dup ? 1 : added;
}
API(gdi32, AddFontResourceExA, 3) { return add_font(STR(0)); }
API(gdi32, AddFontResourceA, 1) { return add_font(STR(0)); }
API(gdi32, RemoveFontResourceExA, 3) { return TRUE; }

static bool face_load(Face *f) {
    if (f->loaded) return !f->bad;
    f->loaded = true;
    f->data = map_file(f->path, &f->size);
    int off = f->data ? stbtt_GetFontOffsetForIndex(f->data, f->index) : -1;
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off)) { f->bad = true; return false; }
    uint8_t *d = f->data;
    uint32_t head = stbtt__find_table(d, off, "head"), hhea = stbtt__find_table(d, off, "hhea"),
             os2 = stbtt__find_table(d, off, "OS/2");
    f->upem = head ? be16(d + head + 18) : 2048;
    if (head) { f->xmin = (int16_t)be16(d + head + 36); f->xmax = (int16_t)be16(d + head + 40); }
    if (hhea) {
        f->hhea_asc = (int16_t)be16(d + hhea + 4);
        f->hhea_desc = (int16_t)be16(d + hhea + 6);
        f->hhea_gap = (int16_t)be16(d + hhea + 8);
    }
    f->win_asc = f->hhea_asc;
    f->win_desc = -f->hhea_desc;
    f->weight = f->bold ? 700 : 400;
    if (os2 && os2 + 78 <= f->size) {
        f->avg_w = (int16_t)be16(d + os2 + 2);
        f->weight = be16(d + os2 + 4);
        f->win_asc = be16(d + os2 + 74);
        f->win_desc = be16(d + os2 + 76);
    }
    if (f->win_asc + f->win_desc <= 0) { f->win_asc = f->upem * 8 / 10; f->win_desc = f->upem * 2 / 10; }
    return true;
}

// Windows face names with no Linux file, mapped to common metric-compatible fonts.
static const char *const aliases[][4] = {
    {"arial", "liberation sans", "arimo", "dejavu sans"},
    {"times new roman", "liberation serif", "tinos", "dejavu serif"},
    {"courier new", "liberation mono", "cousine", "dejavu sans mono"},
    {"courier", "liberation mono", "dejavu sans mono", NULL},
    {"tahoma", "dejavu sans", "liberation sans", NULL},
    {"verdana", "dejavu sans", "liberation sans", NULL},
    {"segoe ui", "noto sans", "dejavu sans", "liberation sans"},
    {"palatino linotype", "tex gyre pagella", "urw palladio l", "liberation serif"},
    {"ms sans serif", "liberation sans", "dejavu sans", NULL},
    {"ms shell dlg", "liberation sans", "dejavu sans", NULL},
    {"ms shell dlg 2", "liberation sans", "dejavu sans", NULL},
    {"microsoft sans serif", "liberation sans", "dejavu sans", NULL},
    {"system", "dejavu sans", "liberation sans", NULL},
    {"arial narrow", "liberation sans narrow", "nimbus sans narrow", "liberation sans"},
};

static Face *find_family(const char *fam, bool bold, bool italic) {
    Face *best = NULL;
    int bs = -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nfaces; i++) {
            Face *f = faces[i];
            if (strcmp(f->family, fam)) continue;
            int s = (f->bold == bold) * 2 + (f->italic == italic) + f->game * 4;
            if (s > bs) { bs = s; best = f; }
        }
        if (best || system_scanned) break;
        scan_system_fonts(); // the game's own fonts didn't have it
    }
    return best;
}

static Face *match_face(const LogFont *lf, bool *fake_bold, bool *fake_italic) {
    char fam[96];
    int n = 0;
    for (; n < 31 && lf->face[n]; n++) fam[n] = lf->face[n] < 128 ? tolower(lf->face[n]) : '?';
    fam[n] = 0;
    bool bold = lf->weight >= 600, italic = lf->italic;
    pthread_mutex_lock(&gdi_lock);
    Face *f = *fam ? find_family(fam, bold, italic) : NULL;
    for (size_t a = 0; !f && a < sizeof aliases / sizeof *aliases; a++)
        if (!strcmp(fam, aliases[a][0]))
            for (int k = 1; k < 4 && !f && aliases[a][k]; k++) f = find_family(aliases[a][k], bold, italic);
    static const char *const fallback[] = {"liberation sans", "dejavu sans", "noto sans", "arimo", "free sans"};
    for (size_t k = 0; !f && k < sizeof fallback / sizeof *fallback; k++) f = find_family(fallback[k], bold, italic);
    if (!f && nfaces) f = faces[0];
    if (f && !face_load(f)) f = NULL;
    pthread_mutex_unlock(&gdi_lock);
    if (f) {
        *fake_bold = bold && !f->bold;
        *fake_italic = italic && !f->italic;
        if (*fam && strcmp(fam, f->family)) LOGD("gdi: font '%s' -> '%s'", fam, f->family);
    } else LOGE("gdi: no usable font for '%s'", fam);
    return f;
}

// ---------------------------------------------------------------- fonts
static uint32_t font_create(const LogFont *lf) {
    Font *f = calloc(1, sizeof *f);
    f->lf = *lf;
    f->face = match_face(lf, &f->fake_bold, &f->fake_italic);
    if (f->face) {
        Face *fc = f->face;
        int h = lf->height ? lf->height : -12;
        float s = h < 0 ? (float)-h / fc->upem : (float)h / (fc->win_asc + fc->win_desc);
        f->sy = f->sx = s;
        f->ascent = (int)lroundf(fc->win_asc * s);
        f->descent = (int)lroundf(fc->win_desc * s);
        int em = (int)lroundf(fc->upem * s);
        f->int_lead = f->ascent + f->descent - em > 0 ? f->ascent + f->descent - em : 0;
        int ext = (fc->hhea_asc - fc->hhea_desc + fc->hhea_gap) - (fc->win_asc + fc->win_desc);
        f->ext_lead = ext > 0 ? (int)lroundf(ext * s) : 0;
        int avg = fc->avg_w;
        if (avg <= 0) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&fc->info, 'x', &adv, &lsb);
            avg = adv;
        }
        f->avgw = (int)lroundf(avg * s);
        if (lf->width > 0 && f->avgw > 0) {
            f->sx = s * lf->width / f->avgw;
            f->avgw = lf->width;
        }
        f->maxw = (int)lroundf((fc->xmax - fc->xmin) * f->sx);
        LOGD("gdi: font %s h %d w %d wt %d -> asc %d desc %d intlead %d avgw %d", fc->family, lf->height,
             lf->width, lf->weight, f->ascent, f->descent, f->int_lead, f->avgw);
    }
    return obj_add(K_FONT, f);
}

static void logfont_from_a(const uint8_t *a, LogFont *lf) {
    memset(lf, 0, sizeof *lf);
    memcpy(lf, a, 28);
    for (int i = 0; i < 32 && a[28 + i]; i++) lf->face[i] = a[28 + i];
}
static void logfont_from_w(const uint8_t *w, LogFont *lf) {
    memset(lf, 0, sizeof *lf);
    memcpy(lf, w, 28);
    memcpy(lf->face, w + 28, 64);
    lf->face[31] = 0;
}

API(gdi32, CreateFontIndirectA, 1) {
    LogFont lf;
    logfont_from_a(PTR(0), &lf);
    return font_create(&lf);
}
API(gdi32, CreateFontIndirectW, 1) {
    LogFont lf;
    logfont_from_w(PTR(0), &lf);
    return font_create(&lf);
}
static uint32_t create_font_args(uint32_t *a, bool wide) {
    LogFont lf = {(int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4],
                  a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], {0}};
    if (a[13]) {
        if (wide) {
            const uint16_t *s = G2H(a[13]);
            for (int i = 0; i < 31 && s[i]; i++) lf.face[i] = s[i];
        } else {
            const char *s = G2H(a[13]);
            for (int i = 0; i < 31 && s[i]; i++) lf.face[i] = (uint8_t)s[i];
        }
    }
    return font_create(&lf);
}
API(gdi32, CreateFontA, 14) { return create_font_args(_args, false); }
API(gdi32, CreateFontW, 14) { return create_font_args(_args, true); }

// Stock fonts (GetStockObject 10..17) are created on first use.
#define STOCK_BASE 0x8000u
static uint32_t stock_font(uint32_t i) {
    static uint32_t made[8];
    if (i < 10 || i > 17) return 0;
    if (!made[i - 10]) {
        static const char *const names[8] = {"Courier New", "Courier New", "Arial", "System", "Arial", "System", "Courier New", "Tahoma"};
        LogFont lf = {0};
        lf.height = i == 13 ? 16 : -11;
        lf.weight = i == 13 ? 700 : 400;
        for (int k = 0; names[i - 10][k]; k++) lf.face[k] = names[i - 10][k];
        made[i - 10] = font_create(&lf);
    }
    return made[i - 10];
}

static Font *dc_font(DC *d) {
    Font *f = obj_get(d->font, K_FONT);
    if (!f) f = obj_get(stock_font(13), K_FONT); // SYSTEM_FONT is the default
    return f && f->face ? f : NULL;
}

// ---------------------------------------------------------------- text
static int glyph_adv(Font *f, int g) {
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->face->info, g, &adv, &lsb);
    return (int)lroundf(adv * f->sx) + f->fake_bold;
}
static int text_width(Font *f, const uint16_t *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph_adv(f, stbtt_FindGlyphIndex(&f->face->info, s[i]));
    return w;
}

// Draw a run with its pen starting at x and baseline at y. Returns its width.
static int draw_run(DC *d, const Target *t, Rect clip, Font *f, int x, int y, const uint16_t *s, int n, uint32_t rgb) {
    stbtt_fontinfo *fi = &f->face->info;
    bool aa = f->lf.quality != 3; // NONANTIALIASED_QUALITY
    int pen = x;
    uint8_t *buf = NULL;
    size_t bufsz = 0;
    for (int i = 0; i < n; i++) {
        int g = stbtt_FindGlyphIndex(fi, s[i]);
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(fi, g, f->sx, f->sy, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            if ((size_t)(gw * gh) > bufsz) buf = realloc(buf, bufsz = gw * gh);
            stbtt_MakeGlyphBitmap(fi, buf, gw, gh, gw, f->sx, f->sy, g);
            for (int bold = 0; bold <= f->fake_bold; bold++)
                for (int yy = 0; yy < gh; yy++) {
                    int py = y + y0 + yy;
                    if (py < clip.t || py >= clip.b) continue;
                    // synthetic italic: shear right above the baseline
                    int shear = f->fake_italic ? (-(y0 + yy)) / 4 : 0;
                    for (int xx = 0; xx < gw; xx++) {
                        int c = buf[yy * gw + xx];
                        int px = pen + x0 + xx + bold + shear;
                        if (!c || px < clip.l || px >= clip.r) continue;
                        if (!aa || c == 255) {
                            if (aa || c >= 128) px_put(t, px, py, rgb);
                            continue;
                        }
                        uint32_t o = px_get(t, px, py), r = 0;
                        for (int sh = 0; sh < 24; sh += 8) {
                            int a = (o >> sh) & 255, b = (rgb >> sh) & 255;
                            r |= (uint32_t)(a + (b - a) * c / 255) << sh;
                        }
                        px_put(t, px, py, r);
                    }
                }
        }
        pen += glyph_adv(f, g);
    }
    free(buf);
    int w = pen - x;
    int em = f->ascent + f->descent - f->int_lead;
    int th = em / 14 > 1 ? em / 14 : 1;
    if (f->lf.underline) fill(t, clip, (Rect){x, y + f->descent / 2, x + w, y + f->descent / 2 + th}, rgb);
    if (f->lf.strike) fill(t, clip, (Rect){x, y - em / 3, x + w, y - em / 3 + th}, rgb);
    (void)d;
    return w;
}

#define TA_UPDATECP 1
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24

static uint32_t text_out(uint32_t hdc, int x, int y, const uint16_t *s, int n) {
    DC *d = obj_get(hdc, K_DC);
    Target t;
    if (!d || !dc_target(d, &t)) return TRUE; // window DCs: nothing to draw into
    Font *f = dc_font(d);
    if (!f) return FALSE;
    if (d->align & TA_UPDATECP) { x = d->cx; y = d->cy; }
    int w = text_width(f, s, n);
    if ((d->align & TA_CENTER) == TA_CENTER) x -= w / 2;
    else if (d->align & TA_RIGHT) x -= w;
    int base = (d->align & TA_BASELINE) == TA_BASELINE ? y : (d->align & TA_BOTTOM) ? y - f->descent : y + f->ascent;
    Rect clip = dc_clip(d, &t);
    if (d->bk_mode == 2) fill(&t, clip, (Rect){x, base - f->ascent, x + w, base + f->descent}, cref_rgb(d->bk_color));
    draw_run(d, &t, clip, f, x, base, s, n, cref_rgb(d->text_color));
    if (d->align & TA_UPDATECP) d->cx = x + w;
    return TRUE;
}
API(gdi32, TextOutW, 5) { return text_out(ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), WSTR(3), (int)ARG(4)); }
API(gdi32, TextOutA, 5) {
    int n;
    uint16_t *w = widen(STR(3), (int)ARG(4), &n);
    uint32_t r = text_out(ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), w, n);
    free(w);
    return r;
}
// ExtTextOut(hdc, x, y, options, rect, str, n, dx): ETO_OPAQUE fills the rect, ETO_CLIPPED clips
static uint32_t ext_text_out(uint32_t *a, const uint16_t *s, int n) {
    DC *d = obj_get(a[0], K_DC);
    Target t;
    if (d && a[4] && dc_target(d, &t) && (a[3] & 2)) {
        const int32_t *r = G2H(a[4]);
        fill(&t, dc_clip(d, &t), (Rect){r[0], r[1], r[2], r[3]}, cref_rgb(d->bk_color));
    }
    if (!d || !n) return TRUE;
    bool saved = d->has_clip;
    Rect sc = d->clip;
    if (a[4] && (a[3] & 4)) {
        const int32_t *r = G2H(a[4]);
        Rect c = {r[0], r[1], r[2], r[3]};
        if (d->has_clip) {
            if (sc.l > c.l) c.l = sc.l;
            if (sc.t > c.t) c.t = sc.t;
            if (sc.r < c.r) c.r = sc.r;
            if (sc.b < c.b) c.b = sc.b;
        }
        d->clip = c;
        d->has_clip = true;
    }
    int mode = d->bk_mode;
    if (a[3] & 2) d->bk_mode = 1;
    uint32_t r = text_out(a[0], (int32_t)a[1], (int32_t)a[2], s, n);
    d->bk_mode = mode;
    d->has_clip = saved;
    d->clip = sc;
    return r;
}
API(gdi32, ExtTextOutW, 8) { return ext_text_out(_args, WSTR(5), (int)ARG(6)); }
API(gdi32, ExtTextOutA, 8) {
    int n = 0;
    uint16_t *w = ARG(5) ? widen(STR(5), (int)ARG(6), &n) : NULL;
    uint32_t r = ext_text_out(_args, w, n);
    free(w);
    return r;
}

static uint32_t text_extent(uint32_t hdc, const uint16_t *s, int n, int32_t *size) {
    DC *d = obj_get(hdc, K_DC);
    Font *f = d ? dc_font(d) : obj_get(stock_font(13), K_FONT);
    if (!f || !f->face) { size[0] = n * 8; size[1] = 16; return TRUE; }
    size[0] = text_width(f, s, n);
    size[1] = f->ascent + f->descent;
    return TRUE;
}
API(gdi32, GetTextExtentPoint32W, 4) { return text_extent(ARG(0), WSTR(1), (int)ARG(2), PTR(3)); }
API(gdi32, GetTextExtentPoint32A, 4) {
    int n;
    uint16_t *w = widen(STR(1), (int)ARG(2), &n);
    uint32_t r = text_extent(ARG(0), w, n, PTR(3));
    free(w);
    return r;
}
API(gdi32, GetTextExtentPointW, 4) { return text_extent(ARG(0), WSTR(1), (int)ARG(2), PTR(3)); }

static bool text_metrics(uint32_t hdc, uint8_t *tm, bool wide) {
    DC *d = obj_get(hdc, K_DC);
    Font *f = d ? dc_font(d) : obj_get(stock_font(13), K_FONT);
    if (!f || !f->face) return false;
    memset(tm, 0, wide ? 60 : 56);
    int32_t *v = (int32_t *)tm;
    v[0] = f->ascent + f->descent;
    v[1] = f->ascent;
    v[2] = f->descent;
    v[3] = f->int_lead;
    v[4] = f->ext_lead;
    v[5] = f->avgw;
    v[6] = f->maxw;
    v[7] = f->fake_bold ? 700 : f->face->weight;
    v[8] = 0;
    v[9] = v[10] = 96;
    int o = 44;
    if (wide) {
        uint16_t *c = (uint16_t *)(tm + 44);
        c[0] = 32; c[1] = 0xFFFC; c[2] = 0x1F; c[3] = 32;
        o = 52;
    } else {
        tm[44] = 32; tm[45] = 255; tm[46] = 0x1F; tm[47] = 32;
        o = 48;
    }
    tm[o] = f->lf.italic ? 255 : 0;
    tm[o + 1] = f->lf.underline ? 255 : 0;
    tm[o + 2] = f->lf.strike ? 255 : 0;
    tm[o + 3] = 0x07 | 0x20; // variable pitch, vector, TrueType; FF_SWISS
    tm[o + 4] = f->lf.charset;
    return true;
}
API(gdi32, GetTextMetricsW, 2) { return text_metrics(ARG(0), PTR(1), true); }
API(gdi32, GetTextMetricsA, 2) { return text_metrics(ARG(0), PTR(1), false); }

// DrawText(hdc, str, n, rect, format)
#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_BOTTOM 0x8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_NOCLIP 0x100
#define DT_EXTERNALLEADING 0x200
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800

static uint32_t draw_text(uint32_t hdc, const uint16_t *src, int n, int32_t *rc, uint32_t fmt) {
    DC *d = obj_get(hdc, K_DC);
    Font *f = d ? dc_font(d) : obj_get(stock_font(13), K_FONT);
    if (!f || !f->face) return 0;
    // drop '&' prefixes ("&&" is a literal '&')
    uint16_t *s = malloc((n + 1) * 2);
    int len = 0;
    for (int i = 0; i < n; i++) {
        if (!(fmt & DT_NOPREFIX) && src[i] == '&') {
            if (i + 1 < n && src[i + 1] == '&') i++;
            else continue;
        }
        s[len++] = src[i];
    }
    int lh = f->ascent + f->descent + (fmt & DT_EXTERNALLEADING ? f->ext_lead : 0);
    int maxw = rc[2] - rc[0];
    // split into lines
    int starts[512], lens[512], nl = 0;
    for (int i = 0; i <= len && nl < 512;) {
        int j = i;
        if (fmt & DT_SINGLELINE) j = len;
        else
            while (j < len && s[j] != '\n' && s[j] != '\r') j++;
        // word wrap within [i, j)
        int a = i;
        while (nl < 512) {
            int e = j;
            if ((fmt & DT_WORDBREAK) && text_width(f, s + a, j - a) > maxw) {
                int lastsp = -1, k = a;
                for (; k < j; k++) {
                    if (s[k] == ' ') lastsp = k;
                    if (text_width(f, s + a, k + 1 - a) > maxw) break;
                }
                e = lastsp > a ? lastsp : (k > a ? k : a + 1);
            }
            starts[nl] = a;
            lens[nl++] = e - a;
            a = e;
            while (a < j && s[a] == ' ' && (fmt & DT_WORDBREAK)) a++;
            if (a >= j) break;
        }
        if (j >= len) break;
        i = j + (s[j] == '\r' && j + 1 < len && s[j + 1] == '\n' ? 2 : 1);
    }
    int widest = 0;
    for (int i = 0; i < nl; i++) {
        int w = text_width(f, s + starts[i], lens[i]);
        if (w > widest) widest = w;
    }
    int total = nl * lh;
    if (g_log_level >= LOG_DEBUG) {
        char t[64];
        int j = 0;
        for (; j < len && j < 63; j++) t[j] = s[j] < 128 ? s[j] : '?';
        t[j] = 0;
        LOGD("gdi: DrawText '%s' rect %d,%d-%d,%d fmt 0x%x font %s/%d lines %d", t, rc[0], rc[1], rc[2], rc[3], fmt,
             f->face->family, f->lf.height, nl);
    }
    if (fmt & DT_CALCRECT) {
        if (!(fmt & DT_WORDBREAK) || widest < maxw) rc[2] = rc[0] + widest;
        rc[3] = rc[1] + total;
        free(s);
        return total;
    }
    Target t;
    if (d && dc_target(d, &t)) {
        int y = rc[1];
        // One line of text is placed vertically even without DT_SINGLELINE, and
        // then bounded only by the DC's clipping: Age of Empires II HD centres its
        // titles on a thin rect inside a taller clip region.
        bool valign = (fmt & DT_SINGLELINE) || nl == 1;
        if (valign) {
            if (fmt & DT_BOTTOM) y = rc[3] - lh;
            else if (fmt & DT_VCENTER) y = rc[1] + (rc[3] - rc[1] - lh) / 2;
        }
        bool clip_v = !(valign && (fmt & (DT_VCENTER | DT_BOTTOM)) && lh > rc[3] - rc[1]);
        Rect clip = dc_clip(d, &t);
        if (!(fmt & DT_NOCLIP)) {
            if (rc[0] > clip.l) clip.l = rc[0];
            if (rc[2] < clip.r) clip.r = rc[2];
            if (clip_v && rc[1] > clip.t) clip.t = rc[1];
            if (clip_v && rc[3] < clip.b) clip.b = rc[3];
        }
        for (int i = 0; i < nl; i++, y += lh) {
            int w = text_width(f, s + starts[i], lens[i]);
            int x = fmt & DT_RIGHT ? rc[2] - w : fmt & DT_CENTER ? rc[0] + (maxw - w) / 2 : rc[0];
            if (d->bk_mode == 2) fill(&t, clip, (Rect){x, y, x + w, y + lh}, cref_rgb(d->bk_color));
            draw_run(d, &t, clip, f, x, y + f->ascent, s + starts[i], lens[i], cref_rgb(d->text_color));
        }
    }
    free(s);
    if ((fmt & DT_SINGLELINE || nl == 1) && (fmt & (DT_VCENTER | DT_BOTTOM)))
        return (fmt & DT_BOTTOM) ? rc[3] - rc[1] : (rc[3] - rc[1] + lh) / 2;
    return total;
}
API(user32, DrawTextW, 5) {
    const uint16_t *s = WSTR(1);
    int n = (int32_t)ARG(2) < 0 ? (int)wlen(s) : (int)ARG(2);
    return draw_text(ARG(0), s, n, PTR(3), ARG(4));
}
API(user32, DrawTextA, 5) {
    int n;
    uint16_t *w = widen(STR(1), (int32_t)ARG(2), &n);
    uint32_t r = draw_text(ARG(0), w, n, PTR(3), ARG(4));
    free(w);
    return r;
}

// ---------------------------------------------------------------- DC state
#define CLR_INVALID 0xFFFFFFFFu
API(gdi32, SetTextColor, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return 0;
    uint32_t o = d->text_color;
    d->text_color = ARG(1) & 0xFFFFFF;
    return o;
}
API(gdi32, GetTextColor, 1) { DC *d = obj_get(ARG(0), K_DC); return d ? d->text_color : 0; }
API(gdi32, SetBkColor, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return 0xFFFFFF;
    uint32_t o = d->bk_color;
    d->bk_color = ARG(1) & 0xFFFFFF;
    return o;
}
API(gdi32, SetBkMode, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return 2;
    int o = d->bk_mode;
    d->bk_mode = ARG(1) == 1 ? 1 : 2;
    return o;
}
API(gdi32, SetTextAlign, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return 0;
    uint32_t o = d->align;
    d->align = ARG(1);
    return o;
}
API(gdi32, MoveToEx, 4) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return TRUE;
    if (ARG(3)) { int32_t *p = PTR(3); p[0] = d->cx; p[1] = d->cy; }
    d->cx = (int32_t)ARG(1);
    d->cy = (int32_t)ARG(2);
    return TRUE;
}
API(gdi32, LineTo, 3) {
    DC *d = obj_get(ARG(0), K_DC);
    Target t;
    if (!d) return TRUE;
    int x1 = (int32_t)ARG(1), y1 = (int32_t)ARG(2);
    if (dc_target(d, &t) && d->pen_color != CLR_INVALID) {
        Rect c = dc_clip(d, &t);
        int x = d->cx, y = d->cy, dx = abs(x1 - x), dy = -abs(y1 - y), sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
        int err = dx + dy;
        uint32_t rgb = cref_rgb(d->pen_color);
        while (x != x1 || y != y1) { // GDI excludes the end point
            if (x >= c.l && x < c.r && y >= c.t && y < c.b) px_put(&t, x, y, rgb);
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    }
    d->cx = x1;
    d->cy = y1;
    return TRUE;
}

// ---------------------------------------------------------------- regions / clipping
API(gdi32, CreateRectRgn, 4) {
    Rect *r = malloc(sizeof *r);
    *r = (Rect){(int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3)};
    return obj_add(K_REGION, r);
}
API(gdi32, CreateRectRgnIndirect, 1) {
    const int32_t *s = PTR(0);
    Rect *r = malloc(sizeof *r);
    *r = (Rect){s[0], s[1], s[2], s[3]};
    return obj_add(K_REGION, r);
}
static uint32_t select_clip(DC *d, uint32_t hrgn) {
    Rect *r = obj_get(hrgn, K_REGION);
    d->has_clip = r != NULL;
    if (r) d->clip = *r;
    return r ? 2 /*SIMPLEREGION*/ : 1 /*NULLREGION*/;
}
API(gdi32, SelectClipRgn, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    return d ? select_clip(d, ARG(1)) : 1;
}
API(gdi32, IntersectClipRect, 5) {
    DC *d = obj_get(ARG(0), K_DC);
    if (!d) return 1;
    Rect c = {(int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4)};
    if (d->has_clip) {
        if (d->clip.l > c.l) c.l = d->clip.l;
        if (d->clip.t > c.t) c.t = d->clip.t;
        if (d->clip.r < c.r) c.r = d->clip.r;
        if (d->clip.b < c.b) c.b = d->clip.b;
    }
    d->clip = c;
    d->has_clip = true;
    return 2;
}

// ---------------------------------------------------------------- brushes
static const uint32_t sys_colors[31] = {
    0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
    0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xFF9933, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000,
    0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xFF9933, 0xF0F0F0};
API(user32, GetSysColor, 1) { return ARG(0) < 31 ? sys_colors[ARG(0)] : 0; }
static uint32_t solid_brushes[256];
static int n_solid;
API(gdi32, CreateSolidBrush, 1) {
    if (n_solid < 256) solid_brushes[n_solid++] = ARG(0) & 0xFFFFFF;
    return 0x00B00000u + n_solid - 1;
}
API(user32, GetSysColorBrush, 1) { return ARG(0) + 1; }
// COLORREF for a brush handle; false for NULL_BRUSH / unknown
static bool brush_color(uint32_t h, uint32_t *c) {
    static const uint32_t stock[5] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
    if (h >= STOCK_BASE && h < STOCK_BASE + 5) { *c = stock[h - STOCK_BASE]; return true; }
    if (h >= 1 && h <= 31) { *c = sys_colors[h - 1]; return true; } // COLOR_xxx + 1
    if (h >= 0x00B00000u && h < 0x00B00000u + (uint32_t)n_solid) { *c = solid_brushes[h - 0x00B00000u]; return true; }
    return false;
}
API(user32, FillRect, 3) {
    DC *d = obj_get(ARG(0), K_DC);
    Target t;
    uint32_t c;
    if (!d || !dc_target(d, &t) || !brush_color(ARG(2), &c)) return TRUE;
    const int32_t *r = PTR(1);
    fill(&t, dc_clip(d, &t), (Rect){r[0], r[1], r[2], r[3]}, cref_rgb(c));
    return TRUE;
}

// ---------------------------------------------------------------- palettes
API(gdi32, CreatePalette, 1) {
    const uint16_t *lp = PTR(0); // LOGPALETTE: version, count, entries
    Palette *p = calloc(1, sizeof *p);
    p->n = lp[1] > 256 ? 256 : lp[1];
    memcpy(p->e, lp + 2, p->n * 4);
    return obj_add(K_PALETTE, p);
}
API(gdi32, CreateHalftonePalette, 1) {
    Palette *p = calloc(1, sizeof *p);
    p->n = 256;
    for (int i = 0; i < 216; i++) p->e[i] = (i % 6) * 51 | ((i / 6) % 6) * 51 << 8 | (i / 36) * 51 << 16;
    for (int i = 216; i < 256; i++) p->e[i] = (i - 216) * 6 * 0x010101u;
    return obj_add(K_PALETTE, p);
}
API(gdi32, GetPaletteEntries, 4) {
    Palette *p = obj_get(ARG(0), K_PALETTE);
    if (!p) return 0;
    if (!ARG(3)) return p->n;
    uint32_t n = 0;
    for (; n < ARG(2) && ARG(1) + n < p->n; n++) ((uint32_t *)PTR(3))[n] = p->e[ARG(1) + n];
    return n;
}
API(gdi32, SetPaletteEntries, 4) {
    Palette *p = obj_get(ARG(0), K_PALETTE);
    if (!p) return 0;
    uint32_t n = 0;
    for (; n < ARG(2) && ARG(1) + n < p->n; n++) p->e[ARG(1) + n] = ((uint32_t *)PTR(3))[n];
    return n;
}
API(gdi32, ResizePalette, 2) {
    Palette *p = obj_get(ARG(0), K_PALETTE);
    if (!p) return FALSE;
    uint32_t n = ARG(1) > 256 ? 256 : ARG(1);
    for (uint32_t i = p->n; i < n; i++) p->e[i] = 0;
    p->n = n;
    return TRUE;
}
API(gdi32, GetNearestPaletteIndex, 2) {
    Palette *p = obj_get(ARG(0), K_PALETTE);
    if (!p || !p->n) return CLR_INVALID;
    uint32_t pal[256];
    for (uint32_t i = 0; i < p->n; i++) pal[i] = cref_rgb(p->e[i] & 0xFFFFFF);
    return nearest_pal(pal, p->n, cref_rgb(ARG(1)));
}
// The 20 static colours of a 256-colour display, the rest black.
API(gdi32, GetSystemPaletteEntries, 4) {
    static const uint32_t sys20[20] = {0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000,
                                       0xC0C0C0, 0xC0DCC0, 0xF0CAA6, 0xF0FBFF, 0xA4A0A0, 0x808080, 0x0000FF,
                                       0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF};
    if (!ARG(3)) return 256;
    for (uint32_t i = 0; i < ARG(2); i++) {
        uint32_t k = ARG(1) + i, c = 0;
        if (k < 10) c = sys20[k];
        else if (k >= 246 && k < 256) c = sys20[k - 236];
        ((uint32_t *)PTR(3))[i] = c; // PALETTEENTRY r,g,b == COLORREF layout
    }
    return ARG(2);
}
API(gdi32, SelectPalette, 3) { return 0; }
API(gdi32, RealizePalette, 1) { return 0; }

// ---------------------------------------------------------------- objects: select / get / delete
API(gdi32, GetStockObject, 1) { return STOCK_BASE + ARG(0); }

API(gdi32, SelectObject, 2) {
    DC *d = obj_get(ARG(0), K_DC);
    uint32_t h = ARG(1);
    if (!d) return STOCK_BASE; // window DCs: accept anything
    if (h >= STOCK_BASE + 10 && h <= STOCK_BASE + 17) h = stock_font(h - STOCK_BASE);
    switch (obj_kind(h)) {
    case K_BITMAP: { uint32_t o = d->bitmap; d->bitmap = h; return o ? o : GDI_BASE; }
    case K_FONT: { uint32_t o = d->font; d->font = h; return o ? o : stock_font(13); }
    case K_REGION: return select_clip(d, h);
    }
    if (h == STOCK_BASE + 6) { d->pen_color = 0xFFFFFF; return STOCK_BASE + 7; } // WHITE_PEN
    if (h == STOCK_BASE + 7) { d->pen_color = 0; return STOCK_BASE + 7; }        // BLACK_PEN
    if (h == STOCK_BASE + 8) { d->pen_color = CLR_INVALID; return STOCK_BASE + 7; } // NULL_PEN
    return STOCK_BASE; // brushes etc.
}

static uint32_t get_object(uint32_t h, uint32_t size, uint8_t *out, bool wide) {
    if (h >= STOCK_BASE + 10 && h <= STOCK_BASE + 17) h = stock_font(h - STOCK_BASE);
    switch (obj_kind(h)) {
    case K_BITMAP: {
        Bitmap *b = obj_get(h, K_BITMAP);
        if (!out) return 24;
        uint8_t bm[24] = {0};
        uint32_t *u = (uint32_t *)bm;
        u[1] = b->w;
        u[2] = b->h;
        u[3] = b->stride;
        *(uint16_t *)(bm + 16) = 1;
        *(uint16_t *)(bm + 18) = b->bpp;
        u[5] = b->bits;
        uint32_t n = size < 24 ? size : 24;
        memcpy(out, bm, n);
        return n;
    }
    case K_FONT: {
        Font *f = obj_get(h, K_FONT);
        uint32_t need = wide ? 92 : 60;
        if (!out) return need;
        uint8_t lf[92] = {0};
        memcpy(lf, &f->lf, 28);
        if (wide) memcpy(lf + 28, f->lf.face, 64);
        else for (int i = 0; i < 32; i++) lf[28 + i] = f->lf.face[i] < 256 ? f->lf.face[i] : '?';
        uint32_t n = size < need ? size : need;
        memcpy(out, lf, n);
        return n;
    }
    case K_PALETTE: {
        Palette *p = obj_get(h, K_PALETTE);
        if (out && size >= 2) *(uint16_t *)out = p->n;
        return 2;
    }
    }
    return 0;
}
API(gdi32, GetObjectA, 3) { return get_object(ARG(0), ARG(1), PTR(2), false); }
API(gdi32, GetObjectW, 3) { return get_object(ARG(0), ARG(1), PTR(2), true); }

API(gdi32, DeleteObject, 1) {
    uint32_t h = ARG(0);
    switch (obj_kind(h)) {
    case K_BITMAP: {
        Bitmap *b = obj_get(h, K_BITMAP);
        gheap_free(b->bits);
        free(b);
        break;
    }
    case K_FONT: case K_REGION: case K_PALETTE: free(objs[h - GDI_BASE].p); break;
    default: return TRUE; // stock objects, brushes
    }
    obj_free(h);
    return TRUE;
}

API(gdi32, CreateCompatibleDC, 1) { return obj_add(K_DC, dc_new()); }
API(gdi32, DeleteDC, 1) {
    DC *d = obj_get(ARG(0), K_DC);
    if (d && !d->surf.write && !d->mem) { obj_free(ARG(0)); free(d); }
    return TRUE;
}

// ---------------------------------------------------------------- bitmaps
// LoadImageA(hinst, name, type, cx, cy, flags): bitmaps from files only.
API(user32, LoadImageA, 6) {
    if (ARG(2) != 0 /*IMAGE_BITMAP*/ || !(ARG(5) & 0x10 /*LR_LOADFROMFILE*/)) return 0;
    char host[4096];
    if (!vfs_to_host(PTR(1), host, sizeof host)) return 0;
    FILE *f = fopen(host, "rb");
    if (!f) { LOGD("LoadImageA: %s not found", (char *)PTR(1)); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    bool ok = n > 54 && fread(d, 1, n, f) == (size_t)n && d[0] == 'B' && d[1] == 'M';
    fclose(f);
    uint32_t off, hsz, bpp, comp, ncol;
    int32_t w, h;
    if (ok) {
        memcpy(&off, d + 10, 4);
        memcpy(&hsz, d + 14, 4);
        memcpy(&w, d + 18, 4);
        memcpy(&h, d + 22, 4);
        bpp = *(uint16_t *)(d + 28);
        memcpy(&comp, d + 30, 4);
        memcpy(&ncol, d + 46, 4);
        ok = w > 0 && h != 0 && comp == 0 && (bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
    }
    if (!ok) { free(d); LOGI("LoadImageA: unsupported bitmap %s", (char *)PTR(1)); return 0; }
    bool topdown = h < 0;
    if (topdown) h = -h;
    Bitmap *b = calloc(1, sizeof *b);
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    b->stride = ((w * bpp + 31) / 32) * 4;
    if (bpp <= 8) {
        if (!ncol) ncol = 1u << bpp;
        for (uint32_t i = 0; i < ncol && i < 256 && 14 + hsz + i * 4 + 4 <= (uint32_t)n; i++) {
            uint32_t c;
            memcpy(&c, d + 14 + hsz + i * 4, 4);
            b->pal[i] = 0xFF000000u | (c & 0xFFFFFF);
        }
    }
    b->bits = gheap_alloc(b->stride * h, 1);
    for (int32_t y = 0; y < h; y++) {
        size_t src = off + (size_t)(topdown ? h - 1 - y : y) * b->stride;
        if (src + b->stride <= (size_t)n) memcpy((uint8_t *)G2H(b->bits) + (size_t)y * b->stride, d + src, b->stride);
    }
    free(d);
    uint32_t hb = obj_add(K_BITMAP, b);
    if (!hb) { gheap_free(b->bits); free(b); return 0; }
    LOGD("LoadImageA: %s %dx%d %u-bit", (char *)PTR(1), w, h, bpp);
    return hb;
}

// BitBlt(dst, x, y, w, h, src, sx, sy, rop): SRCCOPY between bitmaps, memory and surface DCs.
API(gdi32, BitBlt, 9) {
    DC *dst = obj_get(ARG(0), K_DC), *src = obj_get(ARG(5), K_DC);
    int32_t x = ARG(1), y = ARG(2), w = ARG(3), h = ARG(4), sx = ARG(6), sy = ARG(7);
    Target st, dt;
    if (!dst || !src || !dc_target(src, &st)) return FALSE;
    if (sx < 0) { x -= sx; w += sx; sx = 0; }
    if (sy < 0) { y -= sy; h += sy; sy = 0; }
    if (sx + w > st.w) w = st.w - sx;
    if (sy + h > st.h) h = st.h - sy;
    if (w <= 0 || h <= 0) return TRUE;
    uint32_t *tmp = malloc((size_t)w * h * 4);
    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++) tmp[j * w + i] = 0xFF000000u | px_get(&st, sx + i, sy + j);
    if (dst->surf.write) dst->surf.write(dst->surf.ctx, x, y, w, h, tmp);
    else if (dc_target(dst, &dt)) {
        Rect c = dc_clip(dst, &dt);
        for (int32_t j = 0; j < h; j++)
            for (int32_t i = 0; i < w; i++)
                if (x + i >= c.l && x + i < c.r && y + j >= c.t && y + j < c.b) px_put(&dt, x + i, y + j, tmp[j * w + i]);
    }
    free(tmp);
    return TRUE;
}
