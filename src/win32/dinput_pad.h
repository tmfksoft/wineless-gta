#pragma once
#include <stdint.h>

typedef struct PadDev PadDev;

void pad_init(void);
int pad_count(void);                      // connected gamepads (refreshed)
void pad_instance(int i, uint8_t *didi);  // fill DIDEVICEINSTANCEA
int pad_find(const uint8_t *guid);        // instance GUID -> index, -1 if not ours
PadDev *pad_open(int i);
int pad_connected(PadDev *p);
uint32_t pad_setformat(PadDev *p, const uint32_t *fmt);
uint32_t pad_getstate(PadDev *p, uint32_t n, uint8_t *out);
uint32_t pad_getcaps(PadDev *p, uint32_t *caps);
uint32_t pad_setprop(PadDev *p, uint32_t prop, const uint32_t *hdr);
uint32_t pad_getprop(PadDev *p, uint32_t prop, uint32_t *hdr);
uint32_t pad_enumobjects(PadDev *p, uint32_t cb, uint32_t ref, uint32_t flags);
uint32_t pad_getobjectinfo(PadDev *p, uint8_t *obj, uint32_t which, uint32_t how);
void pad_getinfo(PadDev *p, uint8_t *didi);
void pad_debug_tick(uint32_t now_ms);
