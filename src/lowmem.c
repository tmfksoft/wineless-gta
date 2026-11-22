#include "lowmem.h"

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "log.h"

#define NPAGES ((LM_HI - LM_LO) / LM_PAGE)
#define PIDX(a) (((a) - LM_LO) / LM_PAGE)

enum { PS_RESERVED = 1, PS_COMMITTED = 2 };

static uint8_t pstate[NPAGES];
static uint8_t pprot[NPAGES];    // windows prot (compressed: low byte of PAGE_*)
static uint8_t ptag[NPAGES];
static uint32_t pbase[NPAGES];   // allocation base for each reserved page
static uint32_t pallocsz[NPAGES]; // only valid at base page: reservation size
static pthread_mutex_t lm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_hint = 0x02000000u; // bottom-up search start

// DEP: a 32-bit exe not marked NX-compatible runs with DEP off on Windows, so
// any readable page is also executable (unpackers decrypt code in RW pages).
static bool dep_off;
void lm_set_dep(bool enabled) { dep_off = !enabled; }

static int linux_prot_strict(uint32_t wp);
static int linux_prot(uint32_t wp) {
    int p = linux_prot_strict(wp);
    return dep_off && (p & PROT_READ) ? p | PROT_EXEC : p;
}
static int linux_prot_strict(uint32_t wp) {
    switch (wp & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    }
    return PROT_READ | PROT_WRITE;
}

void lm_init(void) {
    void *p = mmap(G2H(LM_LO), LM_HI - LM_LO, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || p != G2H(LM_LO))
        fatal("lowmem: cannot reserve guest address space 0x%x-0x%x (something is mapped there)", LM_LO, LM_HI);
}

static bool range_free(uint32_t a, uint32_t size) {
    for (uint32_t i = PIDX(a), n = PIDX(a + size); i < n; i++)
        if (pstate[i]) return false;
    return true;
}

static uint32_t find_free(uint32_t size, uint32_t align, bool top_down) {
    if (top_down) {
        uint32_t a = (LM_HI - size) & ~(align - 1);
        while (a >= LM_LO) {
            uint32_t i = PIDX(a), n = PIDX(a + size), j;
            for (j = n; j > i; j--)
                if (pstate[j - 1]) break;
            if (j == i) return a;
            uint32_t blocked = LM_LO + (j - 1) * LM_PAGE;
            if (blocked < size + LM_LO) return 0;
            a = (blocked - size) & ~(align - 1);
        }
        return 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? next_hint : LM_LO;
        uint32_t a = (start + align - 1) & ~(align - 1);
        while ((uint64_t)a + size <= LM_HI) {
            uint32_t i = PIDX(a), n = PIDX(a + size), j;
            for (j = i; j < n; j++)
                if (pstate[j]) break;
            if (j == n) {
                next_hint = a + size;
                return a;
            }
            a = (LM_LO + (j + 1) * LM_PAGE + align - 1) & ~(align - 1);
        }
    }
    return 0;
}

uint32_t lm_reserve(uint32_t addr, uint32_t size, uint32_t align, int tag, bool top_down) {
    if (!size) return 0;
    if (align < LM_PAGE) align = LM_PAGE;
    size = (size + LM_PAGE - 1) & ~(LM_PAGE - 1);
    pthread_mutex_lock(&lm_lock);
    if (addr) {
        addr &= ~(LM_PAGE - 1);
        if (addr < LM_LO || (uint64_t)addr + size > LM_HI || !range_free(addr, size)) addr = 0;
    } else {
        addr = find_free(size, align, top_down);
    }
    if (addr) {
        for (uint32_t i = PIDX(addr), n = PIDX(addr + size); i < n; i++) {
            pstate[i] = PS_RESERVED;
            pprot[i] = PAGE_NOACCESS;
            ptag[i] = tag;
            pbase[i] = addr;
        }
        pallocsz[PIDX(addr)] = size;
    }
    pthread_mutex_unlock(&lm_lock);
    return addr;
}

bool lm_commit(uint32_t addr, uint32_t size, uint32_t winprot) {
    uint32_t a = addr & ~(LM_PAGE - 1);
    uint32_t e = (addr + size + LM_PAGE - 1) & ~(LM_PAGE - 1);
    if (a < LM_LO || e > LM_HI) return false;
    pthread_mutex_lock(&lm_lock);
    for (uint32_t i = PIDX(a), n = PIDX(e); i < n; i++)
        if (!pstate[i]) { pthread_mutex_unlock(&lm_lock); return false; }
    if (mprotect(G2H(a), e - a, linux_prot(winprot))) {
        pthread_mutex_unlock(&lm_lock);
        return false;
    }
    for (uint32_t i = PIDX(a), n = PIDX(e); i < n; i++) {
        pstate[i] = PS_RESERVED | PS_COMMITTED;
        pprot[i] = winprot & 0xff;
    }
    pthread_mutex_unlock(&lm_lock);
    return true;
}

bool lm_decommit(uint32_t addr, uint32_t size) {
    uint32_t a = addr & ~(LM_PAGE - 1);
    uint32_t e = (addr + size + LM_PAGE - 1) & ~(LM_PAGE - 1);
    if (a < LM_LO || e > LM_HI) return false;
    pthread_mutex_lock(&lm_lock);
    mmap(G2H(a), e - a, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    for (uint32_t i = PIDX(a), n = PIDX(e); i < n; i++)
        if (pstate[i]) { pstate[i] = PS_RESERVED; pprot[i] = PAGE_NOACCESS; }
    pthread_mutex_unlock(&lm_lock);
    return true;
}

bool lm_release(uint32_t base) {
    if (base < LM_LO || base >= LM_HI) return false;
    pthread_mutex_lock(&lm_lock);
    uint32_t i0 = PIDX(base);
    if (!pstate[i0] || pbase[i0] != base) { pthread_mutex_unlock(&lm_lock); return false; }
    uint32_t size = pallocsz[i0];
    mmap(G2H(base), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    for (uint32_t i = i0, n = PIDX(base + size); i < n; i++) {
        pstate[i] = 0; pprot[i] = 0; ptag[i] = 0; pbase[i] = 0;
    }
    pallocsz[i0] = 0;
    if (base < next_hint) next_hint = base;
    pthread_mutex_unlock(&lm_lock);
    return true;
}

bool lm_protect(uint32_t addr, uint32_t size, uint32_t winprot, uint32_t *oldprot) {
    uint32_t a = addr & ~(LM_PAGE - 1);
    uint32_t e = (addr + size + LM_PAGE - 1) & ~(LM_PAGE - 1);
    if (a < LM_LO || e > LM_HI || e <= a) return false;
    pthread_mutex_lock(&lm_lock);
    for (uint32_t i = PIDX(a), n = PIDX(e); i < n; i++)
        if (!(pstate[i] & PS_COMMITTED)) { pthread_mutex_unlock(&lm_lock); return false; }
    if (oldprot) *oldprot = pprot[PIDX(a)];
    bool ok = mprotect(G2H(a), e - a, linux_prot(winprot)) == 0;
    if (ok)
        for (uint32_t i = PIDX(a), n = PIDX(e); i < n; i++) pprot[i] = winprot & 0xff;
    pthread_mutex_unlock(&lm_lock);
    return ok;
}

uint32_t lm_alloc(uint32_t size, uint32_t align, uint32_t winprot, int tag) {
    uint32_t a = lm_reserve(0, size, align, tag, false);
    if (a && !lm_commit(a, size, winprot)) {
        lm_release(a);
        return 0;
    }
    return a;
}

bool lm_query(uint32_t addr, LmQuery *q) {
    if (addr >= LM_HI) return false;
    uint32_t a = addr & ~(LM_PAGE - 1);
    memset(q, 0, sizeof *q);
    q->base = a;
    pthread_mutex_lock(&lm_lock);
    if (a < LM_LO) {
        q->region_size = LM_LO - a;
        q->state = MEM_FREE;
        q->prot = PAGE_NOACCESS;
        pthread_mutex_unlock(&lm_lock);
        return true;
    }
    uint32_t i = PIDX(a);
    uint8_t st = pstate[i], pr = pprot[i];
    uint32_t ab = pbase[i];
    uint32_t j = i;
    while (j < NPAGES && pstate[j] == st && pprot[j] == pr && pbase[j] == ab) j++;
    q->region_size = (j - i) * LM_PAGE;
    if (!st) {
        q->state = MEM_FREE;
        q->prot = PAGE_NOACCESS;
    } else {
        q->alloc_base = ab;
        q->alloc_prot = ptag[i] == LM_TAG_IMAGE ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READWRITE;
        q->state = (st & PS_COMMITTED) ? MEM_COMMIT : MEM_RESERVE;
        q->prot = (st & PS_COMMITTED) ? pr : 0;
        q->type = ptag[i] == LM_TAG_IMAGE ? MEM_IMAGE : MEM_PRIVATE;
    }
    pthread_mutex_unlock(&lm_lock);
    return true;
}

int lm_tag(uint32_t addr) {
    if (addr < LM_LO || addr >= LM_HI) return LM_TAG_NONE;
    return ptag[PIDX(addr)];
}

bool lm_is_committed(uint32_t addr, uint32_t size, bool write) {
    if (!size) return true;
    if (addr < LM_LO || (uint64_t)addr + size > LM_HI) return false;
    for (uint32_t i = PIDX(addr), n = PIDX(addr + size - 1); i <= n; i++) {
        if (!(pstate[i] & PS_COMMITTED)) return false;
        int lp = linux_prot(pprot[i]);
        if (!(lp & PROT_READ) || (write && !(lp & PROT_WRITE))) return false;
    }
    return true;
}
