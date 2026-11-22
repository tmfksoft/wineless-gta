#pragma once
// Guest-visible address space: [LM_LO, LM_HI), tracked per 4K page.
// gta_sa.exe is not LARGE_ADDRESS_AWARE, so we stay below 2GB like Windows.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_LO 0x00010000u
#define LM_HI 0x7FFE0000u
#define LM_PAGE 4096u
#define LM_GRAN 65536u // Windows allocation granularity

// Windows page protection constants
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_IMAGE 0x1000000
#define MEM_TOP_DOWN 0x100000

enum { LM_TAG_NONE, LM_TAG_VIRTUAL, LM_TAG_IMAGE, LM_TAG_HEAPSLAB, LM_TAG_HEAPLARGE, LM_TAG_SYSTEM, LM_TAG_STACK };

void lm_init(void);
// Data Execution Prevention as the exe asks for (DllCharacteristics NX_COMPAT).
void lm_set_dep(bool enabled);

// Reserve [addr, addr+size) (addr=0: pick, aligned to `align`). Returns 0 on failure.
uint32_t lm_reserve(uint32_t addr, uint32_t size, uint32_t align, int tag, bool top_down);
bool lm_commit(uint32_t addr, uint32_t size, uint32_t winprot);
bool lm_decommit(uint32_t addr, uint32_t size);
bool lm_release(uint32_t base); // whole reservation starting at base
bool lm_protect(uint32_t addr, uint32_t size, uint32_t winprot, uint32_t *oldprot);

// reserve+commit convenience for host-side allocations of guest memory
uint32_t lm_alloc(uint32_t size, uint32_t align, uint32_t winprot, int tag);

typedef struct {
    uint32_t base, alloc_base, alloc_prot, region_size, state, prot, type;
} LmQuery;
bool lm_query(uint32_t addr, LmQuery *q);

int lm_tag(uint32_t addr);
bool lm_is_committed(uint32_t addr, uint32_t size, bool write);

static inline void *G2H(uint32_t a) { return (void *)(uintptr_t)a; }
static inline uint32_t H2G(const void *p) { return (uint32_t)(uintptr_t)p; }
