#include "present_hook.h"
// NULL in the standalone build; the libretro core assigns its barrier here.
void (*wl_present_hook)(wl_frame_reader, uint32_t, uint32_t) = 0;
void (*wl_submit_frame)(const uint32_t *, uint32_t, uint32_t) = 0;
