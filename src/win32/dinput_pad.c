// Gamepads for DirectInput 8, backed by SDL3's gamepad API (evdev; HIDAPI off).
//
// Each SDL gamepad is presented the way an Xbox controller shows up through
// DirectInput on Windows: left stick X/Y, both triggers on Z, right stick
// Rx/Ry, buttons A B X Y LB RB Back Start LS RS, D-pad as POV 0. The game's own
// data format (SetDataFormat) is honoured, along with per-axis ranges and dead
// zones.
#include "dinput_pad.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../heap.h"
#include "win32.h"

#define DI_OK 0u
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_NOTFOUND 0x80070002u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_INVALIDPARAM 0x80070057u

#define DIDFT_AXIS 0x03
#define DIDFT_ABSAXIS 0x02
#define DIDFT_BUTTON 0x0C
#define DIDFT_PSHBUTTON 0x04
#define DIDFT_POV 0x10
#define DIDFT_ANYINSTANCE 0xFFFF
#define DIPH_DEVICE 0
#define DIPH_BYOFFSET 1
#define DIPH_BYID 2

enum { K_NONE, K_AXIS, K_BUTTON, K_POV };
#define NAXES 6
#define NBUTTONS 10

// DirectInput object GUIDs {A36D02xx-C9F3-11CF-BFC7-444553540000}
static const uint8_t DI_D4[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint32_t AXIS_GUID[NAXES] = {0xA36D02E0, 0xA36D02E1, 0xA36D02E2, 0xA36D02F4, 0xA36D02F5, 0xA36D02E3};
#define GUID_SLIDER 0xA36D02E4
#define GUID_BUTTON 0xA36D02F0
#define GUID_POV 0xA36D02F2
static const char *AXIS_NAME[NAXES] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation"};
static const char *BUTTON_NAME[NBUTTONS] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Left Stick", "Right Stick"};
static const SDL_GamepadButton BUTTON_SDL[NBUTTONS] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
// offsets in the standard c_dfDIJoystick layout (EnumObjects before SetDataFormat)
static const uint32_t STD_AXIS_OFS[NAXES] = {0, 4, 8, 12, 16, 20};

typedef struct { uint32_t ofs; int kind, idx; } FmtEntry;

struct PadDev {
    SDL_JoystickID id;
    SDL_Gamepad *pad;
    char name[128];
    uint16_t vid, pid;
    int index;
    int32_t rmin[NAXES], rmax[NAXES];
    uint32_t deadzone[NAXES]; // 0..10000
    FmtEntry fmt[256];
    int nfmt;
    uint32_t data_size;
};

static int pad_ready;
static SDL_JoystickID ids[16];
static int nids;

static void refresh(void) {
    int n = 0;
    SDL_JoystickID *list = SDL_GetGamepads(&n);
    nids = 0;
    for (int i = 0; i < n && nids < 16; i++) ids[nids++] = list[i];
    SDL_free(list);
}

// ---------------------------------------------------------------- debug virtual pad
// WINELESS_VIRTUAL_PAD=1 attaches an SDL virtual gamepad (for testing without one);
// WINELESS_PAD="15000:start,16000:a" presses its buttons at those times (ms).
static SDL_Joystick *vjoy;
static void attach_virtual_pad(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = 0x045E; // looks like an Xbox 360 pad
    d.product_id = 0x028E;
    d.name = "Wineless GTA virtual pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    vjoy = id ? SDL_OpenJoystick(id) : NULL;
    if (vjoy) { // a real pad rests with its triggers released
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    }
    LOGI("dinput: virtual gamepad %s", vjoy ? "attached" : SDL_GetError());
}

// ---- libretro core: drive the virtual pad from the frontend's RetroPad ----
// SDL's SetJoystickVirtual* lock the joystick subsystem internally, so these are
// safe to call from the frontend (retro_run) thread; the guest's event pump
// applies them via SDL_UpdateJoysticks.
#include "../present_hook.h"
int wl_core_pad = 0;

static SDL_GamepadButton wl_pad_sdl(int which) {
    switch (which) {
    case WL_PAD_A: return SDL_GAMEPAD_BUTTON_SOUTH;
    case WL_PAD_B: return SDL_GAMEPAD_BUTTON_EAST;
    case WL_PAD_X: return SDL_GAMEPAD_BUTTON_WEST;
    case WL_PAD_Y: return SDL_GAMEPAD_BUTTON_NORTH;
    case WL_PAD_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case WL_PAD_START: return SDL_GAMEPAD_BUTTON_START;
    case WL_PAD_LB: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case WL_PAD_RB: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case WL_PAD_LS: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case WL_PAD_RS: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case WL_PAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case WL_PAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case WL_PAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case WL_PAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return SDL_GAMEPAD_BUTTON_INVALID;
    }
}
void wl_pad_button(int which, int down) {
    if (!vjoy) return;
    SDL_GamepadButton b = wl_pad_sdl(which);
    if (b != SDL_GAMEPAD_BUTTON_INVALID) SDL_SetJoystickVirtualButton(vjoy, b, down ? true : false);
}
static int16_t clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
void wl_pad_axes(int lx, int ly, int rx, int ry, int lt, int rt) {
    if (!vjoy) return;
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX,  clamp16(lx));
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY,  clamp16(ly));
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTX, clamp16(rx));
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTY, clamp16(ry));
    // triggers: 0..32767 in -> -32768(released)..32767(pressed) axis
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  clamp16(lt * 2 - 32768));
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, clamp16(rt * 2 - 32768));
}

void pad_debug_tick(uint32_t now) {
    static const char *spec;
    static int init, n;
    static struct { uint32_t at; int button, state; } ev[64];
    if (!init) {
        init = 1;
        spec = getenv("WINELESS_PAD");
        static const struct { const char *name; SDL_GamepadButton b; } names[] = {
            {"a", SDL_GAMEPAD_BUTTON_SOUTH}, {"b", SDL_GAMEPAD_BUTTON_EAST}, {"x", SDL_GAMEPAD_BUTTON_WEST},
            {"y", SDL_GAMEPAD_BUTTON_NORTH}, {"back", SDL_GAMEPAD_BUTTON_BACK}, {"start", SDL_GAMEPAD_BUTTON_START},
            {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {"up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
            {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
        for (const char *p = spec; p && *p && n < 64;) {
            uint32_t t = strtoul(p, (char **)&p, 10);
            if (*p++ != ':') break;
            const char *e = strchr(p, ',');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == l && !strncmp(p, names[i].name, l)) {
                    ev[n].at = t;
                    ev[n].button = names[i].b;
                    ev[n++].state = 0;
                }
            p += l;
            if (*p == ',') p++;
        }
    }
    if (!vjoy) return;
    for (int i = 0; i < n; i++) {
        if (ev[i].state == 0 && now >= ev[i].at) {
            ev[i].state = 1;
            SDL_SetJoystickVirtualButton(vjoy, ev[i].button, true);
            LOGI("debug: pad button %d down (%.1fs)", ev[i].button, now / 1000.0);
        } else if (ev[i].state == 1 && now >= ev[i].at + 150) {
            ev[i].state = 2;
            SDL_SetJoystickVirtualButton(vjoy, ev[i].button, false);
        }
    }
}

// ---------------------------------------------------------------- enumeration
void pad_init(void) {
    if (pad_ready) return;
    pad_ready = 1;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0"); // HIDAPI discovery deadlocked in the event pump
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        LOGW("dinput: gamepad support unavailable: %s", SDL_GetError());
        return;
    }
    if (wl_core_pad || getenv("WINELESS_VIRTUAL_PAD")) attach_virtual_pad();
    refresh();
    LOGI("dinput: %d gamepad(s) connected", nids);
    for (int i = 0; i < nids; i++) LOGI("dinput:   %s", SDL_GetGamepadNameForID(ids[i]));
}

int pad_count(void) {
    if (!pad_ready) return 0;
    SDL_UpdateGamepads();
    refresh();
    return nids;
}

static void put_guid(uint8_t *g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t *d4) {
    memcpy(g, &d1, 4);
    memcpy(g + 4, &d2, 2);
    memcpy(g + 6, &d3, 2);
    memcpy(g + 8, d4, 8);
}
static const uint8_t INST_D4[8] = {'W', 'G', 'T', 'A', 'P', 'A', 'D', 0};
static const uint8_t PIDVID_D4[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
#define DEVTYPE (0x15 | (2 << 8) | 0x10000) // DI8DEVTYPE_GAMEPAD, STANDARD, HID

// DIDEVICEINSTANCEA (580 bytes) for pad i
void pad_instance(int i, uint8_t *di) {
    uint32_t sz = *(uint32_t *)di;
    memset(di + 4, 0, (sz ? sz : 580) - 4);
    SDL_JoystickID id = ids[i];
    uint16_t vid = SDL_GetGamepadVendorForID(id), pid = SDL_GetGamepadProductForID(id);
    put_guid(di + 4, 0x5A57D000u + i, 0, 0x11F0, INST_D4);
    put_guid(di + 20, (uint32_t)vid | ((uint32_t)pid << 16), 0, 0, PIDVID_D4);
    *(uint32_t *)(di + 36) = DEVTYPE;
    const char *name = SDL_GetGamepadNameForID(id);
    snprintf((char *)di + 40, 260, "%s", name ? name : "Gamepad");
    snprintf((char *)di + 300, 260, "%s", name ? name : "Gamepad");
    *(uint16_t *)(di + 576) = 1; // HID usage page: generic desktop
    *(uint16_t *)(di + 578) = 5; // usage: game pad
}

int pad_find(const uint8_t *guid) {
    uint32_t d1;
    memcpy(&d1, guid, 4);
    if (memcmp(guid + 8, INST_D4, 8) || (d1 & 0xFFFFF000u) != 0x5A57D000u) return -1;
    int i = d1 & 0xFFF;
    return i < nids ? i : -1;
}

PadDev *pad_open(int i) {
    PadDev *p = calloc(1, sizeof *p);
    p->id = ids[i];
    p->index = i;
    p->pad = SDL_OpenGamepad(p->id);
    snprintf(p->name, sizeof p->name, "%s", SDL_GetGamepadNameForID(p->id) ? SDL_GetGamepadNameForID(p->id) : "Gamepad");
    p->vid = SDL_GetGamepadVendorForID(p->id);
    p->pid = SDL_GetGamepadProductForID(p->id);
    for (int a = 0; a < NAXES; a++) { p->rmin[a] = 0; p->rmax[a] = 65535; }
    LOGI("dinput: opened gamepad '%s' (%04x:%04x)", p->name, p->vid, p->pid);
    return p;
}

// ---------------------------------------------------------------- state
static float axis_value(PadDev *p, int a) {
    SDL_Gamepad *g = p->pad;
    switch (a) {
    case 0: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    case 1: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    case 2: // both triggers share Z like the Xbox controller's DirectInput driver
        return (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) -
                SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 32767.0f;
    case 3: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    case 4: return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
    default: return 0;
    }
}
static int32_t axis_scaled(PadDev *p, int a) {
    float v = axis_value(p, a);
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    float dz = p->deadzone[a] / 10000.0f;
    if (fabsf(v) < dz) v = 0;
    else if (dz > 0) v = (v > 0 ? v - dz : v + dz) / (1 - dz);
    return p->rmin[a] + (int32_t)lroundf((v + 1) * 0.5f * (float)(p->rmax[a] - p->rmin[a]));
}
static uint32_t pov_value(PadDev *p) {
    SDL_Gamepad *g = p->pad;
    int u = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP), d = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    int l = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT), r = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (u && r) return 4500;
    if (d && r) return 13500;
    if (d && l) return 22500;
    if (u && l) return 31500;
    if (u) return 0;
    if (r) return 9000;
    if (d) return 18000;
    if (l) return 27000;
    return 0xFFFFFFFF;
}

static int axis_for_guid(uint32_t d1) {
    for (int a = 0; a < NAXES; a++)
        if (AXIS_GUID[a] == d1) return a;
    return -1;
}

// Build offset -> object mapping from the game's DIDATAFORMAT.
uint32_t pad_setformat(PadDev *p, const uint32_t *fmt) {
    uint32_t nobj = fmt[4], odf = fmt[5];
    p->data_size = fmt[3];
    p->nfmt = 0;
    int used_axis[NAXES] = {0}, next_button = 0, next_pov = 0;
    for (uint32_t i = 0; i < nobj && p->nfmt < 256; i++) {
        const uint32_t *o = G2H(odf + 16 * i);
        uint32_t guidp = o[0], ofs = o[1], type = o[2];
        uint32_t inst = (type >> 8) & 0xFFFF;
        int kind = K_NONE, idx = -1;
        uint32_t d1 = 0;
        if (guidp) memcpy(&d1, G2H(guidp), 4);
        if (guidp && d1 == GUID_BUTTON) kind = K_BUTTON;
        else if (guidp && d1 == GUID_POV) kind = K_POV;
        else if (guidp && (axis_for_guid(d1) >= 0 || d1 == GUID_SLIDER)) kind = K_AXIS;
        else if (!guidp) kind = (type & DIDFT_AXIS) ? K_AXIS : (type & DIDFT_BUTTON) ? K_BUTTON : (type & DIDFT_POV) ? K_POV : K_NONE;
        if (kind == K_AXIS) {
            if (guidp && axis_for_guid(d1) >= 0) idx = axis_for_guid(d1);
            else if (guidp && d1 == GUID_SLIDER) idx = -1; // no sliders on a gamepad
            else if (inst != DIDFT_ANYINSTANCE) idx = inst < NAXES ? (int)inst : -1;
            else for (int a = 0; a < NAXES; a++) if (!used_axis[a]) { idx = a; break; }
            if (idx >= 0) used_axis[idx] = 1;
        } else if (kind == K_BUTTON) {
            idx = inst != DIDFT_ANYINSTANCE ? (int)inst : next_button++;
            if (idx >= NBUTTONS) idx = -1;
        } else if (kind == K_POV) {
            idx = inst != DIDFT_ANYINSTANCE ? (int)inst : next_pov++;
            if (idx >= 1) idx = -1;
        }
        p->fmt[p->nfmt++] = (FmtEntry){ofs, idx >= 0 ? kind : (kind == K_POV ? K_POV : K_NONE), idx};
    }
    LOGD("dinput: pad data format %u bytes, %u objects", p->data_size, nobj);
    return DI_OK;
}

uint32_t pad_getstate(PadDev *p, uint32_t n, uint8_t *out) {
    if (!p->pad || !SDL_GamepadConnected(p->pad)) return DIERR_INPUTLOST;
    SDL_UpdateGamepads();
    memset(out, 0, n);
    for (int i = 0; i < p->nfmt; i++) {
        FmtEntry *e = &p->fmt[i];
        if (e->ofs >= n) continue;
        switch (e->kind) {
        case K_AXIS:
            if (e->ofs + 4 <= n) *(int32_t *)(out + e->ofs) = axis_scaled(p, e->idx);
            break;
        case K_BUTTON:
            out[e->ofs] = SDL_GetGamepadButton(p->pad, BUTTON_SDL[e->idx]) ? 0x80 : 0;
            break;
        case K_POV:
            if (e->ofs + 4 <= n) *(uint32_t *)(out + e->ofs) = e->idx == 0 ? pov_value(p) : 0xFFFFFFFF;
            break;
        }
    }
    return DI_OK;
}

uint32_t pad_getcaps(PadDev *p, uint32_t *c) {
    uint32_t sz = c[0];
    memset(c + 1, 0, sz - 4);
    c[1] = 0x1 | 0x2;     // DIDC_ATTACHED | DIDC_POLLEDDEVICE
    c[2] = DEVTYPE;
    c[3] = NAXES - 1;     // X Y Z Rx Ry (Rz unused on a gamepad, still reported in objects)
    c[4] = NBUTTONS;
    c[5] = 1;
    return DI_OK;
}

static int find_axis(PadDev *p, uint32_t obj, uint32_t how, int *out) {
    if (how == DIPH_BYOFFSET) {
        for (int i = 0; i < p->nfmt; i++)
            if (p->fmt[i].ofs == obj && p->fmt[i].kind == K_AXIS) { *out = p->fmt[i].idx; return 1; }
        for (int a = 0; a < NAXES; a++)
            if (!p->nfmt && STD_AXIS_OFS[a] == obj) { *out = a; return 1; }
        return 0;
    }
    if (how == DIPH_BYID) {
        uint32_t inst = (obj >> 8) & 0xFFFF;
        if ((obj & DIDFT_AXIS) && inst < NAXES) { *out = inst; return 1; }
    }
    return 0;
}

uint32_t pad_setprop(PadDev *p, uint32_t prop, const uint32_t *h) {
    uint32_t obj = h[2], how = h[3];
    int a0 = 0, a1 = NAXES, a;
    if (how != DIPH_DEVICE) {
        if (!find_axis(p, obj, how, &a)) return prop == 4 || prop == 5 ? DIERR_NOTFOUND : DI_OK;
        a0 = a;
        a1 = a + 1;
    }
    for (a = a0; a < a1; a++) {
        if (prop == 4) { p->rmin[a] = (int32_t)h[4]; p->rmax[a] = (int32_t)h[5]; } // DIPROP_RANGE
        else if (prop == 5) p->deadzone[a] = h[4] > 10000 ? 10000 : h[4];      // DIPROP_DEADZONE
    }
    return DI_OK;
}

uint32_t pad_getprop(PadDev *p, uint32_t prop, uint32_t *h) {
    int a = 0;
    switch (prop) {
    case 4: // DIPROP_RANGE
        if (h[3] != DIPH_DEVICE && !find_axis(p, h[2], h[3], &a)) return DIERR_NOTFOUND;
        h[4] = (uint32_t)p->rmin[a];
        h[5] = (uint32_t)p->rmax[a];
        return DI_OK;
    case 5: // DIPROP_DEADZONE
        if (h[3] != DIPH_DEVICE && !find_axis(p, h[2], h[3], &a)) return DIERR_NOTFOUND;
        h[4] = p->deadzone[a];
        return DI_OK;
    case 24: // DIPROP_VIDPID
        h[4] = (uint32_t)p->vid | ((uint32_t)p->pid << 16);
        return DI_OK;
    case 19: // DIPROP_PRODUCTNAME (WCHAR[260])
        a2w(p->name, (uint16_t *)(h + 4), 260);
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

// DIDEVICEOBJECTINSTANCEA (316 bytes)
static void fill_object(PadDev *p, uint8_t *o, int kind, int idx) {
    memset(o, 0, 316);
    *(uint32_t *)o = 316;
    uint32_t d1 = kind == K_AXIS ? AXIS_GUID[idx] : kind == K_BUTTON ? GUID_BUTTON : GUID_POV;
    put_guid(o + 4, d1, 0xC9F3, 0x11CF, DI_D4);
    uint32_t ofs = kind == K_AXIS ? STD_AXIS_OFS[idx] : kind == K_POV ? 32 : 48 + idx;
    for (int i = 0; i < p->nfmt; i++)
        if (p->fmt[i].kind == kind && p->fmt[i].idx == idx) { ofs = p->fmt[i].ofs; break; }
    *(uint32_t *)(o + 20) = ofs;
    uint32_t type = kind == K_AXIS ? DIDFT_ABSAXIS : kind == K_BUTTON ? DIDFT_PSHBUTTON : DIDFT_POV;
    *(uint32_t *)(o + 24) = type | ((uint32_t)idx << 8);
    snprintf((char *)o + 32, 260, "%s", kind == K_AXIS ? AXIS_NAME[idx] : kind == K_BUTTON ? BUTTON_NAME[idx] : "Hat Switch");
    *(uint16_t *)(o + 304) = 1; // usage page
}

uint32_t pad_enumobjects(PadDev *p, uint32_t cb, uint32_t ref, uint32_t flags) {
    uint32_t o = gheap_alloc(316, 1);
    GuestCtx *ctx = thread_current()->ctx;
    int kinds[3] = {K_AXIS, K_BUTTON, K_POV}, counts[3] = {NAXES - 1, NBUTTONS, 1};
    uint32_t masks[3] = {DIDFT_AXIS, DIDFT_BUTTON, DIDFT_POV};
    for (int k = 0; k < 3; k++) {
        if (flags && !(flags & masks[k])) continue;
        for (int i = 0; i < counts[k]; i++) {
            fill_object(p, G2H(o), kinds[k], i);
            uint32_t args[2] = {o, ref};
            if (!(uint32_t)call32(ctx, cb, 2, args)) { gheap_free(o); return DI_OK; } // DIENUM_STOP
        }
    }
    gheap_free(o);
    return DI_OK;
}

uint32_t pad_getobjectinfo(PadDev *p, uint8_t *o, uint32_t obj, uint32_t how) {
    int kind = K_NONE, idx = -1;
    if (how == DIPH_BYOFFSET) {
        for (int i = 0; i < p->nfmt; i++)
            if (p->fmt[i].ofs == obj && p->fmt[i].kind != K_NONE) { kind = p->fmt[i].kind; idx = p->fmt[i].idx; break; }
    } else if (how == DIPH_BYID) {
        idx = (obj >> 8) & 0xFFFF;
        kind = (obj & DIDFT_AXIS) ? K_AXIS : (obj & DIDFT_BUTTON) ? K_BUTTON : (obj & DIDFT_POV) ? K_POV : K_NONE;
        if ((kind == K_AXIS && idx >= NAXES) || (kind == K_BUTTON && idx >= NBUTTONS) || (kind == K_POV && idx >= 1))
            kind = K_NONE;
    }
    if (kind == K_NONE || idx < 0) return DIERR_NOTFOUND;
    fill_object(p, o, kind, idx);
    return DI_OK;
}

void pad_getinfo(PadDev *p, uint8_t *di) { pad_instance(p->index, di); }
int pad_connected(PadDev *p) { return p->pad && SDL_GamepadConnected(p->pad); }
