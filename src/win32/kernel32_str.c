// kernel32: strings, code pages, locale. ANSI is treated as Latin-1/CP1252.
#include <ctype.h>
#include <stdio.h>
#include <strings.h>

#include "win32.h"

API(kernel32, lstrlenA, 1) { return ARG(0) ? strlen(STR(0)) : 0; }
API(kernel32, lstrlenW, 1) { return ARG(0) ? wlen(WSTR(0)) : 0; }
API(kernel32, lstrcpyA, 2) { strcpy(PTR(0), STR(1)); return ARG(0); }
API(kernel32, lstrcpynA, 3) {
    char *d = PTR(0);
    if (!ARG(2)) return ARG(0);
    strncpy(d, STR(1), ARG(2) - 1);
    d[ARG(2) - 1] = 0;
    return ARG(0);
}
API(kernel32, lstrcatA, 2) { strcat(PTR(0), STR(1)); return ARG(0); }
API(kernel32, lstrcmpA, 2) { int r = strcmp(STR(0), STR(1)); return r < 0 ? -1 : r > 0; }
API(kernel32, lstrcmpiA, 2) { int r = strcasecmp(STR(0), STR(1)); return r < 0 ? -1 : r > 0; }

API(kernel32, GetACP, 0) { return 1252; }
API(kernel32, GetOEMCP, 0) { return 437; }
API(kernel32, IsValidCodePage, 1) { return ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001 || ARG(0) == 28591; }
API(kernel32, GetCPInfo, 2) {
    uint8_t *ci = PTR(1);
    memset(ci, 0, 20);
    *(uint32_t *)ci = 1; // MaxCharSize
    ci[4] = '?';
    return TRUE;
}
API(kernel32, IsDBCSLeadByte, 1) { return FALSE; }

API(kernel32, MultiByteToWideChar, 6) {
    const uint8_t *s = PTR(2);
    int32_t n = (int32_t)ARG(3);
    uint16_t *d = PTR(4);
    uint32_t dl = ARG(5);
    if (n < 0) n = strlen((const char *)s) + 1;
    if (!dl) return n;
    if ((uint32_t)n > dl) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int32_t i = 0; i < n; i++) d[i] = s[i];
    return n;
}
API(kernel32, WideCharToMultiByte, 8) {
    const uint16_t *s = PTR(2);
    int32_t n = (int32_t)ARG(3);
    char *d = PTR(4);
    uint32_t dl = ARG(5);
    if (n < 0) n = wlen(s) + 1;
    if (!dl) return n;
    if ((uint32_t)n > dl) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    int used = 0;
    for (int32_t i = 0; i < n; i++) {
        if (s[i] < 256) d[i] = (char)s[i];
        else { d[i] = ARG(6) ? *(char *)PTR(6) : '?'; used = 1; }
    }
    if (ARG(7)) *(uint32_t *)PTR(7) = used;
    return n;
}

API(kernel32, GetUserDefaultLCID, 0) { return 0x409; }
API(kernel32, GetSystemDefaultLCID, 0) { return 0x409; }
API(kernel32, GetUserDefaultLangID, 0) { return 0x409; }
API(kernel32, GetSystemDefaultLangID, 0) { return 0x409; }
API(kernel32, GetThreadLocale, 0) { return 0x409; }
API(kernel32, IsValidLocale, 2) { return TRUE; }
API(kernel32, EnumSystemLocalesA, 2) { return TRUE; }
API(kernel32, EnumSystemLocalesW, 2) { return TRUE; }

static const char *locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x0001: return "0409";          // LOCALE_ILANGUAGE
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";           // LOCALE_SABBREVLANGNAME
    case 0x0005: return "1";             // LOCALE_ICOUNTRY
    case 0x0006: return "United States";
    case 0x0007: return "USA";           // LOCALE_SABBREVCTRYNAME
    case 0x000B: return "437";           // LOCALE_IDEFAULTCODEPAGE
    case 0x000C: return ",";             // LOCALE_SLIST
    case 0x000D: return "1";             // LOCALE_IMEASURE
    case 0x000E: return ".";             // LOCALE_SDECIMAL
    case 0x000F: return ",";             // LOCALE_STHOUSAND
    case 0x0010: return "3;0";           // LOCALE_SGROUPING
    case 0x0011: return "2";             // LOCALE_IDIGITS
    case 0x0012: return "1";             // LOCALE_ILZERO
    case 0x0014: return "$";             // LOCALE_SCURRENCY
    case 0x0015: return "USD";
    case 0x0016: return ".";             // LOCALE_SMONDECIMALSEP
    case 0x0017: return ",";             // LOCALE_SMONTHOUSANDSEP
    case 0x0018: return "3;0";
    case 0x0019: return "2";             // LOCALE_ICURRDIGITS
    case 0x001A: return "2";
    case 0x001B: return "0";             // LOCALE_ICURRENCY
    case 0x001C: return "0";             // LOCALE_INEGCURR
    case 0x001D: return "/";             // LOCALE_SDATE
    case 0x001E: return ":";             // LOCALE_STIME
    case 0x001F: return "M/d/yyyy";      // LOCALE_SSHORTDATE
    case 0x0020: return "dddd, MMMM dd, yyyy";
    case 0x0021: return "0";
    case 0x0022: return "0";
    case 0x0023: return "0";             // LOCALE_ITIME
    case 0x0024: return "0";
    case 0x0025: return "0";
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x0050: return "";              // LOCALE_SPOSITIVESIGN
    case 0x0051: return "-";
    case 0x0052: return "0";
    case 0x0053: return "1";
    case 0x0054: return "1";
    case 0x0055: return "0";
    case 0x0056: return "1";
    case 0x0057: return "0";
    case 0x1001: return "English";       // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States"; // LOCALE_SENGCOUNTRY
    case 0x1003: return "h:mm:ss tt";
    case 0x1004: return "1252";          // LOCALE_IDEFAULTANSICODEPAGE
    case 0x1009: return "1";
    case 0x100C: return "0";             // LOCALE_IFIRSTDAYOFWEEK (Monday=0)
    case 0x1014: return "2";             // LOCALE_INEGNUMBER
    case 0x1015: return "2";
    }
    if ((type & 0xFFFF) >= 0x2A && (type & 0xFFFF) <= 0x4F) {
        static const char *days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
                                     "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        static const char *mons[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                     "September", "October", "November", "December", "",
                                     "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", ""};
        uint32_t t = type & 0xFFFF;
        if (t <= 0x37) return days[t - 0x2A];
        if (t <= 0x51) return mons[t - 0x38];
    }
    LOGD("GetLocaleInfo: unknown type 0x%x", type);
    return "";
}
API(kernel32, GetLocaleInfoA, 4) {
    const char *s = locale_info(ARG(1));
    uint32_t n = strlen(s) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(PTR(2), s, n);
    return n;
}
API(kernel32, GetLocaleInfoW, 4) {
    const char *s = locale_info(ARG(1));
    uint32_t n = strlen(s) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    a2w(s, PTR(2), n);
    return n;
}

static uint16_t ctype1(uint32_t c) {
    uint16_t f = 0;
    if (c < 128) {
        if (isupper(c)) f |= 0x001;
        if (islower(c)) f |= 0x002;
        if (isdigit(c)) f |= 0x004;
        if (isspace(c)) f |= 0x008;
        if (ispunct(c)) f |= 0x010;
        if (iscntrl(c)) f |= 0x020;
        if (c == ' ' || c == '\t') f |= 0x040;
        if (isxdigit(c)) f |= 0x080;
        if (isalpha(c)) f |= 0x100;
    } else if (c >= 0xC0 && c < 0x100 && c != 0xD7 && c != 0xF7) {
        f = 0x100 | (c < 0xE0 ? 0x001 : 0x002);
    } else if (c >= 0xA0 && c < 0xC0) {
        f = 0x010;
    } else if (c < 0xA0) {
        f = 0x020;
    } else {
        f = 0x100;
    }
    return f;
}
API(kernel32, GetStringTypeA, 5) {
    const uint8_t *s = PTR(2);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen((const char *)s) + 1;
    uint16_t *o = PTR(4);
    for (int32_t i = 0; i < n; i++) o[i] = ARG(1) == 1 ? ctype1(s[i]) : 0;
    return TRUE;
}
API(kernel32, GetStringTypeW, 4) {
    const uint16_t *s = PTR(1);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) n = wlen(s) + 1;
    uint16_t *o = PTR(3);
    for (int32_t i = 0; i < n; i++) o[i] = ARG(0) == 1 ? ctype1(s[i]) : 0;
    return TRUE;
}

static uint32_t map_char(uint32_t c, uint32_t flags) {
    if (flags & 0x100) { // LCMAP_LOWERCASE
        if (c < 128) return tolower(c);
        if (c >= 0xC0 && c < 0xDF && c != 0xD7) return c + 0x20;
    }
    if (flags & 0x200) { // LCMAP_UPPERCASE
        if (c < 128) return toupper(c);
        if (c >= 0xE0 && c < 0xFF && c != 0xF7) return c - 0x20;
    }
    return c;
}
API(kernel32, LCMapStringA, 6) {
    const uint8_t *s = PTR(2);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen((const char *)s) + 1;
    if (!ARG(5)) return n;
    if (ARG(5) < (uint32_t)n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    uint8_t *d = PTR(4);
    for (int32_t i = 0; i < n; i++) d[i] = (uint8_t)map_char(s[i], ARG(1));
    return n;
}
API(kernel32, LCMapStringW, 6) {
    const uint16_t *s = PTR(2);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = wlen(s) + 1;
    if (!ARG(5)) return n;
    if (ARG(5) < (uint32_t)n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    uint16_t *d = PTR(4);
    for (int32_t i = 0; i < n; i++) d[i] = (uint16_t)map_char(s[i], ARG(1));
    return n;
}

static int cmp_common(const uint16_t *a, int na, const uint16_t *b, int nb, int icase) {
    for (int i = 0;; i++) {
        if (i >= na || i >= nb) return na < nb ? 1 : na > nb ? 3 : 2;
        uint32_t x = a[i], y = b[i];
        if (icase) { x = map_char(x, 0x100); y = map_char(y, 0x100); }
        if (x != y) return x < y ? 1 : 3;
    }
}
API(kernel32, CompareStringA, 6) {
    const uint8_t *sa = PTR(2), *sb = PTR(4);
    int na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = strlen((const char *)sa);
    if (nb < 0) nb = strlen((const char *)sb);
    uint16_t wa[1024], wb[1024];
    if (na > 1024) na = 1024;
    if (nb > 1024) nb = 1024;
    for (int i = 0; i < na; i++) wa[i] = sa[i];
    for (int i = 0; i < nb; i++) wb[i] = sb[i];
    return cmp_common(wa, na, wb, nb, ARG(1) & 1);
}
API(kernel32, CompareStringW, 6) {
    const uint16_t *sa = PTR(2), *sb = PTR(4);
    int na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = wlen(sa);
    if (nb < 0) nb = wlen(sb);
    return cmp_common(sa, na, sb, nb, ARG(1) & 1);
}

// shared with the *Ex variants (kernel32_vista.c)
uint32_t k32_lcmap_w(uint32_t *a) { return api_kernel32_LCMapStringW(a); }
uint32_t k32_compare_w(uint32_t *a) { return api_kernel32_CompareStringW(a); }
