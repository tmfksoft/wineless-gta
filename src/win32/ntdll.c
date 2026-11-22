// ntdll: the few native calls games and their DRM stubs make directly,
// answering as an ordinary process that isn't being debugged.
#include <unistd.h>

#include "win32.h"

#define STATUS_SUCCESS 0
#define STATUS_INVALID_INFO_CLASS 0xC0000003u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_PORT_NOT_SET 0xC0000353u

extern uint32_t g_peb;

// NtSetInformationThread(thread, class, info, len): ThreadHideFromDebugger (0x11) etc.
API(ntdll, NtSetInformationThread, 4) { return STATUS_SUCCESS; }
API(ntdll, ZwSetInformationThread, 4) { return STATUS_SUCCESS; }

// NtQueryInformationProcess(process, class, info, len, retlen)
static uint32_t query_process(uint32_t *a) {
    uint32_t cls = a[1], len = a[3];
    uint32_t *out = G2H(a[2]);
    uint32_t need;
    switch (cls) {
    case 0: // ProcessBasicInformation
        need = 24;
        if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
        memset(out, 0, need);
        out[1] = g_peb;
        out[4] = getpid();
        break;
    case 7: // ProcessDebugPort: none
        need = 4;
        if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
        out[0] = 0;
        break;
    case 0x1E: // ProcessDebugObjectHandle: none
        if (len >= 4) out[0] = 0;
        if (a[4]) *(uint32_t *)G2H(a[4]) = 4;
        return STATUS_PORT_NOT_SET;
    case 0x1F: // ProcessDebugFlags: 1 = not being debugged
        need = 4;
        if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
        out[0] = 1;
        break;
    default:
        LOGD("NtQueryInformationProcess(class %u) not supported", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
    if (a[4]) *(uint32_t *)G2H(a[4]) = need;
    return STATUS_SUCCESS;
}
API(ntdll, NtQueryInformationProcess, 5) { return query_process(_args); }
API(ntdll, ZwQueryInformationProcess, 5) { return query_process(_args); }
