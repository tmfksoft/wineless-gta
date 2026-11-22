// ntdll: the few native calls games make directly, answering as an ordinary
// process that isn't being debugged.
#include <unistd.h>

#include "win32.h"

#define STATUS_SUCCESS 0
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_INVALID_INFO_CLASS 0xC0000003u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_PARAMETER 0xC000000Du
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

// NtReadVirtualMemory(process, base, buffer, len, retlen): our own process only
// (the native equivalent of ReadProcessMemory), a plain copy within our address space.
static uint32_t read_virtual_memory(uint32_t *a) {
    if (!(a[0] == PSEUDO_PROCESS || a[0] == (uint32_t)getpid()) || !lm_is_committed(a[1], a[3], false))
        return STATUS_ACCESS_VIOLATION;
    memcpy(G2H(a[2]), G2H(a[1]), a[3]);
    if (a[4]) *(uint32_t *)G2H(a[4]) = a[3];
    return STATUS_SUCCESS;
}
API(ntdll, NtReadVirtualMemory, 5) { return read_virtual_memory(_args); }
API(ntdll, ZwReadVirtualMemory, 5) { return read_virtual_memory(_args); }

// NtQueryVirtualMemory(process, base, class, out, outlen, retlen): the native
// VirtualQuery; we support MemoryBasicInformation (class 0), a 28-byte struct.
API(ntdll, NtQueryVirtualMemory, 6) {
    if (!(ARG(0) == PSEUDO_PROCESS || ARG(0) == (uint32_t)getpid())) return STATUS_ACCESS_VIOLATION;
    if (ARG(2) != 0) return STATUS_INVALID_INFO_CLASS; // only MemoryBasicInformation
    LmQuery q;
    if (ARG(4) < 28 || !lm_query(ARG(1), &q)) return STATUS_INVALID_PARAMETER;
    uint32_t *m = PTR(3);
    m[0] = q.base; m[1] = q.alloc_base; m[2] = q.alloc_prot; m[3] = q.region_size;
    m[4] = q.state; m[5] = q.prot; m[6] = q.type;
    if (ARG(5)) *(uint32_t *)PTR(5) = 28;
    return STATUS_SUCCESS;
}
API(ntdll, ZwQueryVirtualMemory, 6) { return api_ntdll_NtQueryVirtualMemory(_args); }

// NtProtectVirtualMemory(process, *base, *size, newprot, *oldprot): the native
// VirtualProtect (base and size are passed by pointer).
API(ntdll, NtProtectVirtualMemory, 5) {
    uint32_t base = *(uint32_t *)PTR(1), size = *(uint32_t *)PTR(2), old = 0;
    if (!lm_protect(base, size, ARG(3), &old)) return STATUS_ACCESS_VIOLATION;
    if (ARG(4)) *(uint32_t *)PTR(4) = old ? old : PAGE_EXECUTE_READWRITE;
    return STATUS_SUCCESS;
}
API(ntdll, ZwProtectVirtualMemory, 5) { return api_ntdll_NtProtectVirtualMemory(_args); }

#define STATUS_NO_MEMORY 0xC0000017u
extern uint32_t k32_virtual_alloc(uint32_t *a);
extern uint32_t k32_create_mapping(uint32_t *a);
extern uint32_t k32_map_view(uint32_t *a);

// NtAllocateVirtualMemory(process, *base, zerobits, *size, type, prot): native VirtualAlloc
// (base and size are passed by pointer, and the chosen base/size are written back).
API(ntdll, NtAllocateVirtualMemory, 6) {
    uint32_t base = *(uint32_t *)PTR(1), size = *(uint32_t *)PTR(3);
    uint32_t args[4] = {base, size, ARG(4), ARG(5)};
    uint32_t r = k32_virtual_alloc(args);
    if (!r) return STATUS_NO_MEMORY;
    *(uint32_t *)PTR(1) = r;
    *(uint32_t *)PTR(3) = (size + 0xfff) & ~0xfffu;
    return STATUS_SUCCESS;
}
API(ntdll, ZwAllocateVirtualMemory, 6) { return api_ntdll_NtAllocateVirtualMemory(_args); }

// NtFreeVirtualMemory(process, *base, *size, freetype): native VirtualFree
API(ntdll, NtFreeVirtualMemory, 4) {
    uint32_t base = *(uint32_t *)PTR(1), size = ARG(2) ? *(uint32_t *)PTR(2) : 0;
    if (ARG(3) & MEM_RELEASE) lm_release(base);
    else if (ARG(3) & MEM_DECOMMIT) lm_decommit(base, size);
    return STATUS_SUCCESS;
}
API(ntdll, ZwFreeVirtualMemory, 4) { return api_ntdll_NtFreeVirtualMemory(_args); }

// NtCreateSection(out, access, objattr, *maxsize, pageprot, allocattr, file): a section object,
// built on our Win32 file-mapping (FileHandle NULL => pagefile-backed, i.e. anonymous).
API(ntdll, NtCreateSection, 7) {
    uint32_t sizeLo = ARG(3) ? *(uint32_t *)PTR(3) : 0, sizeHi = ARG(3) ? ((uint32_t *)PTR(3))[1] : 0;
    uint32_t args[6] = {ARG(6) ? ARG(6) : INVALID_HANDLE_VALUE, 0, ARG(4), sizeHi, sizeLo, 0};
    uint32_t h = k32_create_mapping(args);
    if (!h) return STATUS_NO_MEMORY;
    if (ARG(0)) *(uint32_t *)PTR(0) = h;
    return STATUS_SUCCESS;
}
API(ntdll, ZwCreateSection, 7) { return api_ntdll_NtCreateSection(_args); }

// NtMapViewOfSection(section, process, *base, zerobits, commit, *offset, *viewsize, inherit,
// alloctype, prot): native MapViewOfFile.
API(ntdll, NtMapViewOfSection, 10) {
    uint64_t off = ARG(5) ? *(uint64_t *)PTR(5) : 0;
    uint32_t size = ARG(6) ? *(uint32_t *)PTR(6) : 0;
    uint32_t faccess = ARG(9) == PAGE_READONLY ? 4 /*FILE_MAP_READ*/ : 2 /*FILE_MAP_WRITE*/;
    uint32_t args[5] = {ARG(0), faccess, (uint32_t)(off >> 32), (uint32_t)off, size};
    uint32_t va = k32_map_view(args);
    if (!va) return STATUS_NO_MEMORY;
    if (ARG(2)) *(uint32_t *)PTR(2) = va;
    if (ARG(6)) *(uint32_t *)PTR(6) = size;
    return STATUS_SUCCESS;
}
API(ntdll, ZwMapViewOfSection, 10) { return api_ntdll_NtMapViewOfSection(_args); }

API(ntdll, NtUnmapViewOfSection, 2) { lm_release(ARG(1)); return STATUS_SUCCESS; }
API(ntdll, ZwUnmapViewOfSection, 2) { return api_ntdll_NtUnmapViewOfSection(_args); }

API(ntdll, NtClose, 1) { handle_close(ARG(0)); return STATUS_SUCCESS; }
API(ntdll, ZwClose, 1) { return api_ntdll_NtClose(_args); }

API(ntdll, NtFlushInstructionCache, 3) { return STATUS_SUCCESS; }
API(ntdll, ZwFlushInstructionCache, 3) { return STATUS_SUCCESS; }

#define STATUS_INVALID_CID 0xC000000Bu
extern uint32_t k32_open_process(uint32_t *a);

// NtOpenProcess(*handle, access, objattr, clientid): native OpenProcess; the pid is
// CLIENT_ID.UniqueProcess (the first field ClientId points at).
API(ntdll, NtOpenProcess, 4) {
    uint32_t pid = ARG(3) ? *(uint32_t *)PTR(3) : 0;
    uint32_t args[3] = {ARG(1), 0, pid};
    uint32_t h = k32_open_process(args);
    if (!h) return STATUS_INVALID_CID;
    if (ARG(0)) *(uint32_t *)PTR(0) = h;
    return STATUS_SUCCESS;
}
API(ntdll, ZwOpenProcess, 4) { return api_ntdll_NtOpenProcess(_args); }

// NtQuerySystemInformation(class, info, len, retlen)
static uint32_t query_system(uint32_t *a) {
    uint32_t cls = a[0], len = a[2];
    switch (cls) {
    case 0x23: { // SystemKernelDebuggerInformation: none present (true here)
        if (len < 2) return STATUS_INFO_LENGTH_MISMATCH;
        uint8_t *o = G2H(a[1]);
        o[0] = 0; // KernelDebuggerEnabled
        o[1] = 1; // KernelDebuggerNotPresent
        if (a[3]) *(uint32_t *)G2H(a[3]) = 2;
        return STATUS_SUCCESS;
    }
    default:
        LOGD("NtQuerySystemInformation(class %u) not supported", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}
API(ntdll, NtQuerySystemInformation, 4) { return query_system(_args); }
API(ntdll, ZwQuerySystemInformation, 4) { return query_system(_args); }
