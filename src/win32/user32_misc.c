// Smaller user32 pieces (keyboard text, clipboard, help, beeps) and the IME,
// GUID, shell file operation and internet-state calls games make along the way.
#include <SDL3/SDL.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../heap.h"
#include "../vfs.h"
#include "win32.h"

// ---------------------------------------------------------------- keyboard text
// ToAscii(vk, scan, keystate[256], out WORD*, flags): US layout
API(user32, ToAscii, 5) {
    uint32_t vk = ARG(0);
    const uint8_t *ks = PTR(2);
    bool shift = ks && (ks[0x10] & 0x80), caps = ks && (ks[0x14] & 1), ctrl = ks && (ks[0x11] & 0x80);
    uint32_t c = 0;
    if (vk >= 'A' && vk <= 'Z') c = (shift ^ caps) ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') c = shift ? (uint32_t)")!@#$%^&*("[vk - '0'] : vk;
    else if (vk >= 0x60 && vk <= 0x69) c = '0' + vk - 0x60;
    else
        switch (vk) {
        case 0x20: c = ' '; break;
        case 0x0D: c = '\r'; break;
        case 0x08: c = 8; break;
        case 0x09: c = '\t'; break;
        case 0x1B: c = 27; break;
        case 0xBA: c = shift ? ':' : ';'; break;
        case 0xBB: c = shift ? '+' : '='; break;
        case 0xBC: c = shift ? '<' : ','; break;
        case 0xBD: c = shift ? '_' : '-'; break;
        case 0xBE: c = shift ? '>' : '.'; break;
        case 0xBF: c = shift ? '?' : '/'; break;
        case 0xC0: c = shift ? '~' : '`'; break;
        case 0xDB: c = shift ? '{' : '['; break;
        case 0xDC: c = shift ? '|' : '\\'; break;
        case 0xDD: c = shift ? '}' : ']'; break;
        case 0xDE: c = shift ? '"' : '\''; break;
        case 0x6A: c = '*'; break;
        case 0x6B: c = '+'; break;
        case 0x6D: c = '-'; break;
        case 0x6E: c = '.'; break;
        case 0x6F: c = '/'; break;
        }
    if (ctrl && c >= 'a' && c <= 'z') c -= 'a' - 1;
    if (!c || (ARG(1) & 0x8000)) return 0; // nothing, or a key release
    *(uint16_t *)PTR(3) = c;
    return 1;
}

// GetKeyNameText(lParam, buf, size): names of keys by scan code (bit 24 = extended)
static const char *key_name(uint32_t lp) {
    uint32_t sc = (lp >> 16) & 0xFF;
    bool ext = (lp >> 24) & 1;
    static const char *const names[0x59] = {
        [0x01] = "Esc", [0x02] = "1", [0x03] = "2", [0x04] = "3", [0x05] = "4", [0x06] = "5", [0x07] = "6",
        [0x08] = "7", [0x09] = "8", [0x0A] = "9", [0x0B] = "0", [0x0C] = "-", [0x0D] = "=", [0x0E] = "Backspace",
        [0x0F] = "Tab", [0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R", [0x14] = "T", [0x15] = "Y",
        [0x16] = "U", [0x17] = "I", [0x18] = "O", [0x19] = "P", [0x1A] = "[", [0x1B] = "]", [0x1C] = "Enter",
        [0x1D] = "Ctrl", [0x1E] = "A", [0x1F] = "S", [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H",
        [0x24] = "J", [0x25] = "K", [0x26] = "L", [0x27] = ";", [0x28] = "'", [0x29] = "`", [0x2A] = "Shift",
        [0x2B] = "\\", [0x2C] = "Z", [0x2D] = "X", [0x2E] = "C", [0x2F] = "V", [0x30] = "B", [0x31] = "N",
        [0x32] = "M", [0x33] = ",", [0x34] = ".", [0x35] = "/", [0x36] = "Right Shift", [0x37] = "Num *",
        [0x38] = "Alt", [0x39] = "Space", [0x3A] = "Caps Lock", [0x3B] = "F1", [0x3C] = "F2", [0x3D] = "F3",
        [0x3E] = "F4", [0x3F] = "F5", [0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9", [0x44] = "F10",
        [0x45] = "Pause", [0x46] = "Scroll Lock", [0x47] = "Num 7", [0x48] = "Num 8", [0x49] = "Num 9",
        [0x4A] = "Num -", [0x4B] = "Num 4", [0x4C] = "Num 5", [0x4D] = "Num 6", [0x4E] = "Num +", [0x4F] = "Num 1",
        [0x50] = "Num 2", [0x51] = "Num 3", [0x52] = "Num 0", [0x53] = "Num Del", [0x57] = "F11", [0x58] = "F12"};
    if (ext)
        switch (sc) {
        case 0x1C: return "Num Enter";
        case 0x1D: return "Right Ctrl";
        case 0x35: return "Num /";
        case 0x38: return "Right Alt";
        case 0x47: return "Home";
        case 0x48: return "Up";
        case 0x49: return "Page Up";
        case 0x4B: return "Left";
        case 0x4D: return "Right";
        case 0x4F: return "End";
        case 0x50: return "Down";
        case 0x51: return "Page Down";
        case 0x52: return "Insert";
        case 0x53: return "Delete";
        case 0x5B: return "Left Windows";
        case 0x5C: return "Right Windows";
        case 0x5D: return "Application";
        }
    return sc < 0x59 ? names[sc] : NULL;
}
API(user32, GetKeyNameTextA, 3) {
    const char *n = key_name(ARG(0));
    if (!n || !ARG(2)) return 0;
    snprintf(PTR(1), ARG(2), "%s", n);
    return strlen(STR(1));
}
API(user32, GetKeyNameTextW, 3) {
    const char *n = key_name(ARG(0));
    if (!n || !ARG(2)) return 0;
    a2w(n, PTR(1), ARG(2));
    return wlen(WSTR(1));
}
API(user32, GetCaretBlinkTime, 0) { return 530; }
API(user32, MessageBeep, 1) { return TRUE; }
API(user32, WinHelpA, 4) { return FALSE; }

// ---------------------------------------------------------------- clipboard (text only, via SDL)
#define CF_TEXT 1
#define CF_UNICODETEXT 13
API(user32, OpenClipboard, 1) { return TRUE; }
API(user32, CloseClipboard, 0) { return TRUE; }
API(user32, EmptyClipboard, 0) { return TRUE; }
API(user32, IsClipboardFormatAvailable, 1) {
    return (ARG(0) == CF_TEXT || ARG(0) == CF_UNICODETEXT) && SDL_HasClipboardText();
}
// The returned "handle" is GlobalAlloc memory (GlobalLock returns it as-is).
API(user32, GetClipboardData, 1) {
    if (ARG(0) != CF_TEXT && ARG(0) != CF_UNICODETEXT) return 0;
    char *t = SDL_GetClipboardText();
    if (!t) return 0;
    size_t n = strlen(t);
    uint32_t h;
    if (ARG(0) == CF_TEXT) {
        h = gheap_alloc(n + 1, 1);
        // keep ASCII and Latin-1; replace other characters
        char *o = G2H(h);
        for (const uint8_t *p = (const uint8_t *)t; *p; p++) {
            if (*p < 0x80) *o++ = *p;
            else if ((*p & 0xE0) == 0xC0 && p[1]) { uint32_t c = (*p & 31) << 6 | (p[1] & 63); *o++ = c < 256 ? c : '?'; p++; }
            else if (*p >= 0xC0) *o++ = '?';
        }
        *o = 0;
    } else {
        h = gheap_alloc((n + 1) * 2, 1);
        uint16_t *o = G2H(h);
        for (const uint8_t *p = (const uint8_t *)t; *p;) {
            uint32_t c = *p++;
            if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
            else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p++; }
            *o++ = c;
        }
        *o = 0;
    }
    SDL_free(t);
    return h;
}
API(user32, SetClipboardData, 2) {
    if (ARG(0) == CF_TEXT && ARG(1)) SDL_SetClipboardText(STR(1));
    else if (ARG(0) == CF_UNICODETEXT && ARG(1)) {
        char b[4096];
        w2a(WSTR(1), b, sizeof b);
        SDL_SetClipboardText(b);
    }
    return ARG(1);
}

// ---------------------------------------------------------------- imm32: no input method active
API(imm32, ImmGetContext, 1) { return 0; }
API(imm32, ImmReleaseContext, 2) { return TRUE; }
API(imm32, ImmAssociateContext, 2) { return 0; }
API(imm32, ImmGetDefaultIMEWnd, 1) { return 0; }
API(imm32, ImmNotifyIME, 4) { return FALSE; }
API(imm32, ImmGetOpenStatus, 1) { return FALSE; }
API(imm32, ImmSetOpenStatus, 2) { return TRUE; }

// ---------------------------------------------------------------- ole32 / wininet
API(ole32, CoCreateGuid, 1) {
    uint8_t *g = PTR(0);
    if (getrandom(g, 16, 0) != 16)
        for (int i = 0; i < 16; i++) g[i] = rand();
    g[7] = (g[7] & 0x0F) | 0x40; // version 4 (Data3 is little-endian)
    g[8] = (g[8] & 0x3F) | 0x80;
    return 0;
}
// Networking goes through Steam; report a LAN connection.
API(wininet, InternetGetConnectedState, 2) {
    if (ARG(0)) *(uint32_t *)PTR(0) = 0x02; // INTERNET_CONNECTION_LAN
    return TRUE;
}

// ---------------------------------------------------------------- shell32: SHFileOperationA
#define FO_MOVE 1
#define FO_COPY 2
#define FO_DELETE 3
#define FO_RENAME 4

static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)flag; (void)f;
    remove(p);
    vfs_note_deleted(p);
    return 0;
}
static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    if (!out) { if (in) fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    fclose(in);
    ok &= fclose(out) == 0;
    vfs_note_created(to, false);
    return ok;
}
// SHFILEOPSTRUCTA: hwnd, wFunc, pFrom (double-NUL list), pTo, fFlags, fAnyOperationsAborted, ...
API(shell32, SHFileOperationA, 1) {
    uint32_t *op = PTR(0);
    uint32_t func = op[1];
    const char *from = G2H(op[2]), *to = op[3] ? G2H(op[3]) : NULL;
    int err = 0;
    for (const char *f = from; f && *f; f += strlen(f) + 1) {
        char hf[4096], ht[4096];
        if (!vfs_to_host(f, hf, sizeof hf)) { err = 2; continue; }
        LOGD("SHFileOperation(%u, %s%s%s)", func, f, to ? " -> " : "", to ? to : "");
        struct stat st;
        if (stat(hf, &st)) { err = 2; continue; }
        if (func == FO_DELETE) {
            if (S_ISDIR(st.st_mode)) nftw(hf, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
            else rm_entry(hf, &st, 0, NULL);
        } else if (to && vfs_to_host(to, ht, sizeof ht)) {
            struct stat dst;
            if (!stat(ht, &dst) && S_ISDIR(dst.st_mode)) { // into a folder: keep the name
                const char *base = strrchr(hf, '/');
                strncat(ht, base ? base : "/", sizeof ht - strlen(ht) - 1);
            }
            if (func == FO_COPY) { if (S_ISDIR(st.st_mode) || !copy_file(hf, ht)) err = 2; }
            else if (rename(hf, ht)) err = 2;
            else { vfs_note_deleted(hf); vfs_note_created(ht, S_ISDIR(st.st_mode)); }
        } else err = 2;
    }
    op[5] = 0; // fAnyOperationsAborted
    return err;
}

// DPI awareness: we render into an SDL/DXVK surface and scale ourselves, so DPI
// awareness is a no-op. Report success / "aware" so games don't re-scale.
API(user32, SetProcessDPIAware, 0) { return TRUE; }
API(user32, SetProcessDpiAwarenessContext, 1) { return TRUE; }
API(user32, IsProcessDPIAware, 0) { return TRUE; }
API(user32, GetDpiForSystem, 0) { return 96; }
API(user32, GetDpiForWindow, 1) { return 96; }
