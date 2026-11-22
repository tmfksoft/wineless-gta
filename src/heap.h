#pragma once
#include <stdint.h>

// Guest heap: every block lives below 2GB. 16-byte header per block holds the
// exact requested size (HeapSize must return it) and the size class.
uint32_t gheap_alloc(uint32_t size, int zero);
uint32_t gheap_realloc(uint32_t p, uint32_t size, int zero);
int gheap_free(uint32_t p);
uint32_t gheap_size(uint32_t p); // (uint32_t)-1 if invalid
uint32_t gheap_capacity(uint32_t p); // usable bytes without moving (0 if invalid)

// host helper: strdup into guest memory
uint32_t gheap_strdup(const char *s);
