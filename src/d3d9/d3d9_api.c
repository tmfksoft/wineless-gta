// d3d9.dll exports; the interfaces themselves live in d3d9_bridge.cpp.
#include "../win32/win32.h"

uint32_t d3d9_bridge_create(uint32_t sdk);

API(d3d9, Direct3DCreate9, 1) {
    LOGI("Direct3DCreate9(sdk %u)", ARG(0));
    uint32_t r = d3d9_bridge_create(ARG(0));
    return r;
}
API(d3d9, D3DPERF_BeginEvent, 2) { return 0; }
API(d3d9, D3DPERF_EndEvent, 0) { return 0; }
API(d3d9, D3DPERF_SetMarker, 2) { return 0; }
API(d3d9, D3DPERF_GetStatus, 0) { return 0; }
API(d3d9, D3DPERF_SetOptions, 1) { return 0; }
API(d3d9, D3DPERF_QueryRepeatFrame, 0) { return FALSE; }
// Debug-build mute toggle; a no-op in retail. AoE2 HD GetProcAddress's it every
// frame, so exporting it stops the "not found" log spam.
API(d3d9, DebugSetMute, 0) { return 0; } // S_OK
