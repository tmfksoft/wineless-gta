#include "heap.h"

#include <pthread.h>
#include <string.h>

#include "log.h"
#include "lowmem.h"

#define HDR 16u
#define SLAB_SIZE 0x10000u
#define SLAB_HDR 64u
#define MAGIC_SMALL 0x5A57534Du // 'SAWM'
#define MAGIC_LARGE 0x5A57534Cu
#define MAX_SMALL 16384u

typedef struct {
    uint32_t size;  // requested size
    uint32_t magic;
    uint32_t cls;   // class index (small) or reservation size (large)
    uint32_t pad;
} BlockHdr;

static const uint32_t classes[] = {
    32,   48,   64,   80,   96,   112,  128,  160,  192,  224,  256,  320,  384,  448,
    512,  640,  768,  896,  1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120,
    6144, 7168, 8192, 10240, 12288, 14336, 16384 + HDR};
#define NCLS (sizeof classes / sizeof classes[0])

static uint32_t freelist[NCLS]; // guest addresses of free blocks (block start incl. header)
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static int class_for(uint32_t total) {
    for (unsigned i = 0; i < NCLS; i++)
        if (classes[i] >= total) return i;
    return -1;
}

static int refill(int c) {
    uint32_t slab = lm_alloc(SLAB_SIZE, SLAB_SIZE, PAGE_READWRITE, LM_TAG_HEAPSLAB);
    if (!slab) return 0;
    uint32_t sz = classes[c];
    for (uint32_t a = slab + SLAB_HDR; a + sz <= slab + SLAB_SIZE; a += sz) {
        *(uint32_t *)G2H(a) = freelist[c];
        freelist[c] = a;
    }
    return 1;
}

uint32_t gheap_alloc(uint32_t size, int zero) {
    uint32_t total = size + HDR;
    if (total < size) return 0;
    BlockHdr *h;
    if (size <= MAX_SMALL) {
        int c = class_for(total);
        pthread_mutex_lock(&heap_lock);
        if (!freelist[c] && !refill(c)) {
            pthread_mutex_unlock(&heap_lock);
            return 0;
        }
        uint32_t b = freelist[c];
        freelist[c] = *(uint32_t *)G2H(b);
        pthread_mutex_unlock(&heap_lock);
        h = G2H(b);
        h->magic = MAGIC_SMALL;
        h->cls = c;
    } else {
        uint32_t rs = (total + LM_PAGE - 1) & ~(LM_PAGE - 1);
        uint32_t b = lm_alloc(rs, LM_PAGE, PAGE_READWRITE, LM_TAG_HEAPLARGE);
        if (!b) return 0;
        h = G2H(b);
        h->magic = MAGIC_LARGE;
        h->cls = rs;
        zero = 0; // fresh pages are zero
    }
    h->size = size;
    h->pad = 0;
    if (zero) memset(h + 1, 0, size);
    return H2G(h + 1);
}

static BlockHdr *hdr_of(uint32_t p) {
    if (p < LM_LO + HDR || (p & 7)) return NULL;
    int tag = lm_tag(p);
    if (tag != LM_TAG_HEAPSLAB && tag != LM_TAG_HEAPLARGE) return NULL;
    BlockHdr *h = (BlockHdr *)G2H(p) - 1;
    if (h->magic != MAGIC_SMALL && h->magic != MAGIC_LARGE) return NULL;
    return h;
}

int gheap_free(uint32_t p) {
    if (!p) return 1;
    BlockHdr *h = hdr_of(p);
    if (!h) {
        LOGW("heap: free of invalid pointer 0x%x", p);
        return 0;
    }
    if (h->magic == MAGIC_LARGE) {
        h->magic = 0;
        return lm_release(H2G(h));
    }
    int c = h->cls;
    h->magic = 0;
    pthread_mutex_lock(&heap_lock);
    *(uint32_t *)h = freelist[c];
    freelist[c] = H2G(h);
    pthread_mutex_unlock(&heap_lock);
    return 1;
}

uint32_t gheap_size(uint32_t p) {
    BlockHdr *h = hdr_of(p);
    return h ? h->size : (uint32_t)-1;
}

uint32_t gheap_capacity(uint32_t p) {
    BlockHdr *h = hdr_of(p);
    if (!h) return 0;
    return h->magic == MAGIC_SMALL ? classes[h->cls] - HDR : h->cls - HDR;
}

uint32_t gheap_realloc(uint32_t p, uint32_t size, int zero) {
    if (!p) return gheap_alloc(size, zero);
    BlockHdr *h = hdr_of(p);
    if (!h) {
        LOGW("heap: realloc of invalid pointer 0x%x", p);
        return 0;
    }
    uint32_t cap = h->magic == MAGIC_SMALL ? classes[h->cls] - HDR : h->cls - HDR;
    if (size <= cap) {
        if (zero && size > h->size) memset((char *)G2H(p) + h->size, 0, size - h->size);
        h->size = size;
        return p;
    }
    uint32_t n = gheap_alloc(size, 0);
    if (!n) return 0;
    memcpy(G2H(n), G2H(p), h->size);
    if (zero) memset((char *)G2H(n) + h->size, 0, size - h->size);
    gheap_free(p);
    return n;
}

uint32_t gheap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t p = gheap_alloc(n, 0);
    if (p) memcpy(G2H(p), s, n);
    return p;
}
