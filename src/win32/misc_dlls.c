// Small system DLLs: winmm, gdi32, ole32, ws2_32 (networking is disabled).
#include <arpa/inet.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "win32.h"

// ---------------------------------------------------------------- winmm
API(winmm, timeGetTime, 0) { return win32_tick_ms(); }
API(winmm, timeBeginPeriod, 1) { return 0; }
API(winmm, timeEndPeriod, 1) { return 0; }
API(winmm, timeGetDevCaps, 2) {
    uint32_t *c = PTR(0);
    if (ARG(1) >= 8) { c[0] = 1; c[1] = 1000000; }
    return 0;
}

// ---------------------------------------------------------------- gdi32
API(gdi32, GetDeviceCaps, 2) {
    switch (ARG(1)) {
    case 8: return 1920;  // HORZRES
    case 10: return 1080; // VERTRES
    case 12: return 32;   // BITSPIXEL
    case 88: case 90: return 96; // LOGPIXELSX/Y
    case 116: return 60;  // VREFRESH
    }
    return 0;
}

// ---------------------------------------------------------------- ole32
#define REGDB_E_CLASSNOTREG 0x80040154u
API(ole32, CoInitialize, 1) { return 0; }
API(ole32, CoInitializeEx, 2) { return 0; }
API(ole32, CoUninitialize, 0) { return 0; }
uint32_t dshow_create(const void *clsid, const void *iid, uint32_t *out);

API(ole32, CoCreateInstance, 5) {
    uint32_t hr = dshow_create(PTR(0), PTR(3), PTR(4));
    if (hr != 0xFFFFFFFF) return hr;
    uint32_t *g = PTR(0);
    LOGI("CoCreateInstance({%08x-...}): class not available", g ? g[0] : 0);
    if (ARG(4)) *(uint32_t *)PTR(4) = 0;
    return REGDB_E_CLASSNOTREG;
}
API(ole32, CoTaskMemFree, 1) { return 0; }

// ---------------------------------------------------------------- ws2_32 (by ordinal)
// Networking isn't needed for single player: every socket call fails cleanly.
#define WSAENETDOWN 10050
API_ORD(ws2_32, WSAStartup, 115, 2) {
    uint8_t *d = PTR(1);
    memset(d, 0, 400);
    *(uint16_t *)d = ARG(0);
    *(uint16_t *)(d + 2) = 0x0202;
    strcpy((char *)d + 4, "Wineless GTA sockets (disabled)");
    return 0;
}
API_ORD(ws2_32, WSACleanup, 116, 0) { return 0; }
API_ORD(ws2_32, WSAGetLastError, 111, 0) { return WSAENETDOWN; }
API_ORD(ws2_32, socket, 23, 3) { return 0xFFFFFFFF; }
API_ORD(ws2_32, closesocket, 3, 1) { return 0; }
API_ORD(ws2_32, connect, 4, 3) { return 0xFFFFFFFF; }
API_ORD(ws2_32, htons, 9, 1) { return htons((uint16_t)ARG(0)); }
API_ORD(ws2_32, ntohs, 15, 1) { return ntohs((uint16_t)ARG(0)); }
API_ORD(ws2_32, htonl, 8, 1) { return htonl(ARG(0)); }
API_ORD(ws2_32, inet_addr, 11, 1) {
    struct in_addr a;
    return inet_aton(STR(0), &a) ? a.s_addr : 0xFFFFFFFF;
}
API_ORD(ws2_32, recv, 16, 4) { return 0xFFFFFFFF; }
API_ORD(ws2_32, send, 19, 4) { return 0xFFFFFFFF; }
API_ORD(ws2_32, gethostbyname, 52, 1) { return 0; }

// more ws2_32 by ordinal: networking is unavailable
#define SOCKET_ERROR 0xFFFFFFFFu
API_ORD(ws2_32, accept, 1, 3) { return SOCKET_ERROR; }
API_ORD(ws2_32, bind, 2, 3) { return SOCKET_ERROR; }
API_ORD(ws2_32, getpeername, 5, 3) { return SOCKET_ERROR; }
API_ORD(ws2_32, getsockname, 6, 3) { return SOCKET_ERROR; }
API_ORD(ws2_32, getsockopt, 7, 5) { return SOCKET_ERROR; }
API_ORD(ws2_32, ioctlsocket, 10, 3) { return SOCKET_ERROR; }
API_ORD(ws2_32, inet_ntoa, 12, 1) { return 0; }
API_ORD(ws2_32, listen, 13, 2) { return SOCKET_ERROR; }
API_ORD(ws2_32, ntohl, 14, 1) { return ntohl(ARG(0)); }
API_ORD(ws2_32, recvfrom, 17, 6) { return SOCKET_ERROR; }
API_ORD(ws2_32, select, 18, 5) { return SOCKET_ERROR; }
API_ORD(ws2_32, sendto, 20, 6) { return SOCKET_ERROR; }
API_ORD(ws2_32, setsockopt, 21, 5) { return SOCKET_ERROR; }
API_ORD(ws2_32, shutdown, 22, 2) { return SOCKET_ERROR; }
API_ORD(ws2_32, WSASetLastError, 112, 1) { return 0; }
API_ORD(ws2_32, __WSAFDIsSet, 151, 2) { return 0; }
API(ws2_32, WSACreateEvent, 0) { return 0; }
API(ws2_32, WSACloseEvent, 1) { return TRUE; }
API(ws2_32, WSAResetEvent, 1) { return TRUE; }
API(ws2_32, WSASetEvent, 1) { return TRUE; }
API(ws2_32, WSAEventSelect, 3) { return SOCKET_ERROR; }
API(ws2_32, WSAWaitForMultipleEvents, 5) { return 0x102; } // WSA_WAIT_TIMEOUT
API(ws2_32, WSAGetOverlappedResult, 5) { return FALSE; }
API(ws2_32, WSARecv, 7) { return SOCKET_ERROR; }
API(ws2_32, WSARecvFrom, 9) { return SOCKET_ERROR; }
API(ws2_32, WSASend, 7) { return SOCKET_ERROR; }
API(ws2_32, WSASendTo, 9) { return SOCKET_ERROR; }
