// winmm: waveOut playback through our mixer, multimedia timers, and "no
// device" answers for MIDI/mixer/aux/MCI/mmio/waveIn and msacm32.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "win32.h"

#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_INVALHANDLE 5
#define WAVERR_BADFORMAT 32
#define WHDR_DONE 0x1
#define WHDR_PREPARED 0x2
#define WHDR_BEGINLOOP 0x4
#define WHDR_ENDLOOP 0x8
#define WHDR_INQUEUE 0x10
#define WOM_OPEN 0x3BB
#define WOM_CLOSE 0x3BC
#define WOM_DONE 0x3BD
#define CALLBACK_TYPEMASK 0x70000
#define CALLBACK_WINDOW 0x10000
#define CALLBACK_THREAD 0x20000
#define CALLBACK_FUNCTION 0x30000
#define CALLBACK_EVENT 0x50000
#define WAVE_FORMAT_QUERY 1
#define OUT_RATE 44100

void dsound_audio_start(void);
void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

// ---------------------------------------------------------------- waveOut
#define MAX_WAVE 8
#define MAX_HDRS 64
typedef struct {
    int used, paused;
    uint16_t channels, bits, block;
    uint32_t rate;
    uint32_t cb_type, cb, inst;
    uint32_t hdrs[MAX_HDRS]; // queued WAVEHDR guest pointers
    int nh;
    double pos;              // frame position inside hdrs[0]
    uint64_t played;         // frames played (for GetPosition)
    float volume;
} Wave;
static Wave waves[MAX_WAVE];
static pthread_mutex_t wave_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t wave_handle(int i) { return 0x5A0000 + i; }
static Wave *wave_get(uint32_t h) {
    uint32_t i = h - 0x5A0000;
    return i < MAX_WAVE && waves[i].used ? &waves[i] : NULL;
}

static void notify(Wave *w, uint32_t msg, uint32_t p1) {
    switch (w->cb_type) {
    case CALLBACK_FUNCTION: {
        // void CALLBACK waveOutProc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
        uint32_t args[5] = {wave_handle((int)(w - waves)), msg, w->inst, p1, 0};
        guest_callback_post(w->cb, 5, args);
        break;
    }
    case CALLBACK_WINDOW: user32_post(w->cb, msg, wave_handle((int)(w - waves)), p1); break;
    case CALLBACK_EVENT: {
        Object *ev = handle_get(w->cb, OBJ_EVENT);
        if (ev) obj_signal_event(ev, 1);
        break;
    }
    case CALLBACK_THREAD: LOGD("waveOut: thread callbacks not supported"); break;
    }
}

static inline float wsample(const Wave *w, const uint8_t *d, uint32_t frame, int ch) {
    int c = ch < w->channels ? ch : 0;
    if (w->bits == 16) return ((const int16_t *)d)[frame * w->channels + c] * (1.0f / 32768.0f);
    return (d[frame * w->channels + c] - 128) * (1.0f / 128.0f);
}

// Called from the mixer (audio thread).
void waveout_mix(float *out, int frames) {
    pthread_mutex_lock(&wave_lock);
    for (int wi = 0; wi < MAX_WAVE; wi++) {
        Wave *w = &waves[wi];
        if (!w->used || w->paused || !w->block) continue;
        double step = (double)w->rate / OUT_RATE;
        for (int i = 0; i < frames && w->nh; i++) {
            uint32_t *h = G2H(w->hdrs[0]);
            uint32_t nframes = h[1] / w->block;
            const uint8_t *d = G2H(h[0]);
            uint32_t f0 = (uint32_t)w->pos;
            if (f0 < nframes) {
                out[2 * i] += wsample(w, d, f0, 0) * w->volume;
                out[2 * i + 1] += wsample(w, d, f0, 1) * w->volume;
            }
            w->pos += step;
            w->played++;
            if ((uint32_t)w->pos >= nframes) {
                h[4] = (h[4] & ~WHDR_INQUEUE) | WHDR_DONE;
                uint32_t done = w->hdrs[0];
                memmove(w->hdrs, w->hdrs + 1, (--w->nh) * sizeof(uint32_t));
                w->pos = 0;
                notify(w, WOM_DONE, done);
            }
        }
    }
    pthread_mutex_unlock(&wave_lock);
}

API(winmm, waveOutGetNumDevs, 0) { return 1; }

// ---- legacy joystick (winmm) backed by the SDL gamepad (incl. the core's
// virtual pad). Games like Tomb Raider III poll joyGetPosEx. ----
int wl_joy_read(uint32_t axes[6], uint32_t *buttons, uint32_t *pov);
#define JOYERR_NOERROR   0
#define JOYERR_UNPLUGGED 167
API(winmm, joyGetNumDevs, 0) { return 16; } // count the driver *supports*
API(winmm, joyGetDevCapsA, 3) {
    uint32_t axes[6], buttons, pov;
    if (ARG(0) != 0 || !wl_joy_read(axes, &buttons, &pov)) return JOYERR_UNPLUGGED;
    uint8_t *c = PTR(1);
    uint32_t n = ARG(2);
    if (n) memset(c, 0, n);
    // JOYCAPS: wMid,wPid @0/2; szPname[32] @4; then ranges at @36...
    if (n >= 36 + 8 * 4 + 6 * 4 + 4 * 2 + 32 + 260) {
        uint32_t *r = (uint32_t *)(c + 36);
        r[0] = 0; r[1] = 65535; // X min/max
        r[2] = 0; r[3] = 65535; // Y
        r[4] = 0; r[5] = 65535; // Z
        uint16_t *w = (uint16_t *)(c + 36 + 6 * 4);
        w[0] = 10;   // wNumButtons
        w[1] = 10;   // wPeriodMin
        w[2] = 1000; // wPeriodMax
        uint32_t *r2 = (uint32_t *)(c + 36 + 6 * 4 + 6); // Rmin..Vmax
        r2[0] = 0; r2[1] = 65535; r2[2] = 0; r2[3] = 65535; r2[4] = 0; r2[5] = 65535;
        uint16_t *w2 = (uint16_t *)(c + 36 + 6 * 4 + 6 + 6 * 4);
        w2[0] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10; // wCaps: HAS Z/R/U/V/POV
        w2[1] = 6;  // wMaxAxes
        w2[2] = 6;  // wNumAxes
        w2[3] = 10; // wMaxButtons
    }
    return JOYERR_NOERROR;
}
API(winmm, joyGetDevCapsW, 3) { return api_winmm_joyGetDevCapsA(_args); }
API(winmm, joyGetPos, 2) {
    uint32_t axes[6], buttons, pov;
    if (ARG(0) != 0 || !wl_joy_read(axes, &buttons, &pov)) return JOYERR_UNPLUGGED;
    uint32_t *i = PTR(1); // JOYINFO: X,Y,Z,Buttons
    i[0] = axes[0]; i[1] = axes[1]; i[2] = axes[2]; i[3] = buttons;
    return JOYERR_NOERROR;
}
API(winmm, joyGetPosEx, 2) {
    uint32_t axes[6], buttons, pov;
    if (ARG(0) != 0 || !wl_joy_read(axes, &buttons, &pov)) return JOYERR_UNPLUGGED;
    uint32_t *x = PTR(1); // JOYINFOEX: dwSize,dwFlags,X,Y,Z,R,U,V,Buttons,ButtonNumber,POV,...
    x[2] = axes[0]; x[3] = axes[1]; x[4] = axes[2];
    x[5] = axes[3]; x[6] = axes[4]; x[7] = axes[5];
    x[8] = buttons;
    uint32_t nb = 0;
    for (int b = 0; b < 32; b++) if (buttons & (1u << b)) nb++;
    x[9] = nb;
    x[10] = pov;
    return JOYERR_NOERROR;
}
API(winmm, joySetCapture, 4) { return JOYERR_NOERROR; }
API(winmm, joyReleaseCapture, 1) { return JOYERR_NOERROR; }
API(winmm, joySetThreshold, 2) { return JOYERR_NOERROR; }
API(winmm, joyGetThreshold, 2) { if (PTR(1)) *(uint32_t *)PTR(1) = 0; return JOYERR_NOERROR; }
API(winmm, waveOutGetDevCapsA, 3) {
    uint8_t *c = PTR(1);
    uint32_t n = ARG(2) < 52 ? ARG(2) : 52;
    uint8_t caps[52] = {0};
    *(uint16_t *)caps = 1;            // wMid
    *(uint16_t *)(caps + 2) = 1;      // wPid
    *(uint32_t *)(caps + 4) = 0x0500; // driver version
    strcpy((char *)caps + 8, "Linux audio (Wineless GTA)");
    *(uint32_t *)(caps + 40) = 0xFFF; // all standard formats
    *(uint16_t *)(caps + 44) = 2;     // channels
    *(uint32_t *)(caps + 48) = 0x4 | 0x8; // WAVECAPS_VOLUME | LRVOLUME
    memcpy(c, caps, n);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutOpen, 6) {
    const uint8_t *wfx = PTR(2);
    uint16_t tag = *(uint16_t *)wfx, ch = *(uint16_t *)(wfx + 2), bits = *(uint16_t *)(wfx + 14);
    if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2) return WAVERR_BADFORMAT;
    if (ARG(5) & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    dsound_audio_start();
    pthread_mutex_lock(&wave_lock);
    int i = 0;
    while (i < MAX_WAVE && waves[i].used) i++;
    if (i == MAX_WAVE) { pthread_mutex_unlock(&wave_lock); return MMSYSERR_NODRIVER; }
    Wave *w = &waves[i];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->channels = ch;
    w->bits = bits;
    w->rate = *(uint32_t *)(wfx + 4);
    w->block = *(uint16_t *)(wfx + 12) ? *(uint16_t *)(wfx + 12) : ch * bits / 8;
    w->cb_type = ARG(5) & CALLBACK_TYPEMASK;
    w->cb = ARG(3);
    w->inst = ARG(4);
    w->volume = 1.0f;
    pthread_mutex_unlock(&wave_lock);
    if (ARG(0)) *(uint32_t *)PTR(0) = wave_handle(i);
    LOGD("waveOutOpen: %uHz %ubit %uch callback type 0x%x", w->rate, bits, ch, w->cb_type);
    notify(w, WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutClose, 1) {
    Wave *w = wave_get(ARG(0));
    if (!w) return MMSYSERR_INVALHANDLE;
    notify(w, WOM_CLOSE, 0);
    pthread_mutex_lock(&wave_lock);
    w->used = 0;
    pthread_mutex_unlock(&wave_lock);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutPrepareHeader, 3) {
    uint32_t *h = PTR(1);
    h[4] |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutUnprepareHeader, 3) {
    uint32_t *h = PTR(1);
    if (h[4] & WHDR_INQUEUE) return 33; // WAVERR_STILLPLAYING
    h[4] &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutWrite, 3) {
    Wave *w = wave_get(ARG(0));
    if (!w) return MMSYSERR_INVALHANDLE;
    uint32_t *h = PTR(1);
    pthread_mutex_lock(&wave_lock);
    if (w->nh < MAX_HDRS) {
        h[4] = (h[4] & ~WHDR_DONE) | WHDR_INQUEUE;
        w->hdrs[w->nh++] = ARG(1);
    }
    pthread_mutex_unlock(&wave_lock);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutReset, 1) {
    Wave *w = wave_get(ARG(0));
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&wave_lock);
    uint32_t done[MAX_HDRS];
    int n = w->nh;
    memcpy(done, w->hdrs, n * sizeof(uint32_t));
    w->nh = 0;
    w->pos = 0;
    w->played = 0;
    for (int i = 0; i < n; i++) {
        uint32_t *h = G2H(done[i]);
        h[4] = (h[4] & ~WHDR_INQUEUE) | WHDR_DONE;
    }
    pthread_mutex_unlock(&wave_lock);
    for (int i = 0; i < n; i++) notify(w, WOM_DONE, done[i]);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutPause, 1) { Wave *w = wave_get(ARG(0)); if (w) w->paused = 1; return w ? 0 : MMSYSERR_INVALHANDLE; }
API(winmm, waveOutRestart, 1) { Wave *w = wave_get(ARG(0)); if (w) w->paused = 0; return w ? 0 : MMSYSERR_INVALHANDLE; }
API(winmm, waveOutGetPosition, 3) {
    Wave *w = wave_get(ARG(0));
    if (!w) return MMSYSERR_INVALHANDLE;
    uint32_t *t = PTR(1); // MMTIME {wType, value}
    uint64_t frames = (uint64_t)(w->played * ((double)w->rate / OUT_RATE));
    if (t[0] == 2 /*TIME_SAMPLES*/) t[1] = (uint32_t)frames;
    else { t[0] = 4; t[1] = (uint32_t)(frames * w->block); } // TIME_BYTES
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutGetID, 2) { if (ARG(1)) *(uint32_t *)PTR(1) = 0; return 0; }
API(winmm, waveOutSetVolume, 2) {
    Wave *w = wave_get(ARG(0));
    if (w) w->volume = (ARG(1) & 0xFFFF) / 65535.0f;
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutGetVolume, 2) {
    Wave *w = wave_get(ARG(0));
    uint32_t v = w ? (uint32_t)(w->volume * 65535) : 0xFFFF;
    if (ARG(1)) *(uint32_t *)PTR(1) = v | (v << 16);
    return MMSYSERR_NOERROR;
}
API(winmm, waveOutMessage, 4) { return MMSYSERR_NODRIVER; }

// ---------------------------------------------------------------- multimedia timers
#define MAX_TIMERS 16
typedef struct {
    int used;
    uint32_t delay, cb, user, flags;
    double next;
} Timer;
static Timer timers[MAX_TIMERS];
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static int timer_thread_started;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void *timer_main(void *arg) {
    for (;;) {
        double now = now_ms(), soonest = now + 5;
        pthread_mutex_lock(&timer_lock);
        for (int i = 0; i < MAX_TIMERS; i++) {
            Timer *t = &timers[i];
            if (!t->used || now < t->next) {
                if (t->used && t->next < soonest) soonest = t->next;
                continue;
            }
            uint32_t id = i + 1;
            if (t->flags & 0x10) { // TIME_CALLBACK_EVENT_SET
                Object *ev = handle_get(t->cb, OBJ_EVENT);
                if (ev) obj_signal_event(ev, 1);
            } else if (t->flags & 0x20) { // TIME_CALLBACK_EVENT_PULSE
                Object *ev = handle_get(t->cb, OBJ_EVENT);
                if (ev) obj_signal_event(ev, 2);
            } else {
                uint32_t args[5] = {id, 0, t->user, 0, 0};
                guest_callback_post(t->cb, 5, args);
            }
            if (t->flags & 1) { // TIME_PERIODIC
                t->next += t->delay ? t->delay : 1;
                if (t->next < now) t->next = now + (t->delay ? t->delay : 1);
            } else {
                t->used = 0;
            }
        }
        pthread_mutex_unlock(&timer_lock);
        double wait = soonest - now_ms();
        if (wait > 0) usleep((useconds_t)(wait * 1000));
    }
    return NULL;
}

API(winmm, timeSetEvent, 5) {
    pthread_mutex_lock(&timer_lock);
    int i = 0;
    while (i < MAX_TIMERS && timers[i].used) i++;
    if (i == MAX_TIMERS) { pthread_mutex_unlock(&timer_lock); return 0; }
    timers[i] = (Timer){1, ARG(0), ARG(2), ARG(3), ARG(4), now_ms() + ARG(0)};
    if (!timer_thread_started) {
        timer_thread_started = 1;
        pthread_t th;
        pthread_create(&th, NULL, timer_main, NULL);
        pthread_detach(th);
    }
    pthread_mutex_unlock(&timer_lock);
    LOGD("timeSetEvent(%u ms, cb 0x%x, flags 0x%x) -> %d", ARG(0), ARG(2), ARG(4), i + 1);
    return i + 1;
}
API(winmm, timeKillEvent, 1) {
    pthread_mutex_lock(&timer_lock);
    if (ARG(0) >= 1 && ARG(0) <= MAX_TIMERS) timers[ARG(0) - 1].used = 0;
    pthread_mutex_unlock(&timer_lock);
    return 0;
}

// ---------------------------------------------------------------- not available
API(winmm, waveInGetNumDevs, 0) { return 0; }
API(winmm, waveInOpen, 6) { return MMSYSERR_NODRIVER; }
API(winmm, waveInClose, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, waveInStart, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, waveInReset, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, waveInAddBuffer, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, waveInPrepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, waveInUnprepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutGetNumDevs, 0) { return 0; }
API(winmm, midiOutGetDevCapsA, 3) { return MMSYSERR_BADDEVICEID; }
API(winmm, midiOutOpen, 5) { return MMSYSERR_NODRIVER; }
API(winmm, midiOutClose, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutReset, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutShortMsg, 2) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutLongMsg, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutPrepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutUnprepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiOutSetVolume, 2) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamOpen, 6) { return MMSYSERR_NODRIVER; }
API(winmm, midiStreamClose, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamOut, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamPause, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamRestart, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamStop, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamPosition, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, midiStreamProperty, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, mixerGetNumDevs, 0) { return 0; }
API(winmm, mixerOpen, 5) { return MMSYSERR_NODRIVER; }
API(winmm, mixerClose, 1) { return MMSYSERR_INVALHANDLE; }
API(winmm, mixerGetLineInfoA, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, mixerGetLineControlsA, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, mixerGetControlDetailsA, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, mixerSetControlDetails, 3) { return MMSYSERR_INVALHANDLE; }
API(winmm, auxGetNumDevs, 0) { return 0; }
API(winmm, auxGetDevCapsA, 3) { return MMSYSERR_BADDEVICEID; }
API(winmm, auxGetVolume, 2) { return MMSYSERR_BADDEVICEID; }
API(winmm, auxSetVolume, 2) { return MMSYSERR_BADDEVICEID; }
API(winmm, mciSendCommandA, 4) { return 263; } // MCIERR_INVALID_DEVICE_NAME
API(winmm, mciSendStringA, 4) { return 263; }
API(winmm, mmioOpenA, 3) { return 0; }
API(winmm, mmioClose, 2) { return 0; }
API(winmm, mmioRead, 3) { return (uint32_t)-1; }
API(winmm, mmioSeek, 3) { return (uint32_t)-1; }
API(winmm, mmioDescend, 4) { return 265; }
API(winmm, mmioAscend, 3) { return 265; }
API(winmm, PlaySoundA, 3) { return FALSE; }
API(winmm, sndPlaySoundA, 2) { return FALSE; }

// msacm32: one codec, MS-ADPCM -> 16-bit PCM (Tomb Raider III's music in
// cdaudio.wad). Anything else is "not possible", so Miles (GTA III / Vice City)
// keeps using its own decoders.
#define ACMERR_NOTPOSSIBLE 512
#define WAVE_FORMAT_ADPCM 2

typedef struct {
    uint16_t ch, block, spb, ncoef;
    int16_t coef[32][2];
    uint32_t rate;
} AdpcmFmt;

// WAVEFORMATEX(+ADPCMWAVEFORMAT) -> parsed MS-ADPCM format; false if not MS-ADPCM.
static bool adpcm_parse(const uint8_t *wfx, AdpcmFmt *f) {
    if (*(const uint16_t *)wfx != WAVE_FORMAT_ADPCM) return false;
    memset(f, 0, sizeof *f);
    f->ch = *(const uint16_t *)(wfx + 2);
    memcpy(&f->rate, wfx + 4, 4);
    f->block = *(const uint16_t *)(wfx + 12);
    uint16_t cb = *(const uint16_t *)(wfx + 16);
    if (f->ch < 1 || f->ch > 2 || f->block < 7 * f->ch) return false;
    f->spb = cb >= 4 ? *(const uint16_t *)(wfx + 18) : (f->block - 7 * f->ch) * 2 / f->ch + 2;
    f->ncoef = cb >= 4 ? *(const uint16_t *)(wfx + 20) : 7;
    static const int16_t std[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    if (f->ncoef > 32) f->ncoef = 32;
    for (int i = 0; i < f->ncoef; i++) {
        if (cb >= 4 + 4 * (i + 1)) {
            f->coef[i][0] = *(const int16_t *)(wfx + 22 + i * 4);
            f->coef[i][1] = *(const int16_t *)(wfx + 24 + i * 4);
        } else if (i < 7) {
            f->coef[i][0] = std[i][0];
            f->coef[i][1] = std[i][1];
        }
    }
    return true;
}

static int16_t clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// Decode one block into f->spb frames of interleaved 16-bit samples.
static void adpcm_block(const AdpcmFmt *f, const uint8_t *in, int16_t *out) {
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    int ch = f->ch, c1[2], c2[2], delta[2], s1[2], s2[2];
    const uint8_t *p = in;
    for (int c = 0; c < ch; c++) {
        int pi = *p++;
        if (pi >= f->ncoef) pi = 0;
        c1[c] = f->coef[pi][0];
        c2[c] = f->coef[pi][1];
    }
    for (int c = 0; c < ch; c++, p += 2) delta[c] = *(const int16_t *)p;
    for (int c = 0; c < ch; c++, p += 2) s1[c] = *(const int16_t *)p;
    for (int c = 0; c < ch; c++, p += 2) s2[c] = *(const int16_t *)p;
    for (int c = 0; c < ch; c++) { out[c] = s2[c]; out[ch + c] = s1[c]; }
    int total = (f->spb - 2) * ch;
    for (int i = 0; i < total; i++) {
        int c = i % ch;
        int nib = (i & 1) ? (p[i >> 1] & 15) : (p[i >> 1] >> 4);
        int sn = nib >= 8 ? nib - 16 : nib;
        int pred = (s1[c] * c1[c] + s2[c] * c2[c]) / 256 + sn * delta[c];
        int16_t v = clamp16(pred);
        s2[c] = s1[c];
        s1[c] = v;
        delta[c] = adapt[nib] * delta[c] / 256;
        if (delta[c] < 16) delta[c] = 16;
        out[2 * ch + i] = v;
    }
}

typedef struct { AdpcmFmt f; } AcmStream;
#define ACM_MAX 64
static AcmStream *acm_streams[ACM_MAX]; // handle = 0xACC00000 + index

API(msacm32, acmGetVersion, 0) { return 0x04000000; }

// One driver: the Microsoft ADPCM codec.
#define ACM_DRIVER_ID 0xACD00001u
#define ACM_DRIVER_HANDLE 0xACE00001u
API(msacm32, acmDriverEnum, 3) {
    uint32_t args[3] = {ACM_DRIVER_ID, ARG(1), 0x1 /*ACMDRIVERDETAILS_SUPPORTF_CODEC*/};
    call32(thread_current()->ctx, ARG(0), 3, args);
    return 0;
}
API(msacm32, acmDriverDetailsA, 3) {
    if (ARG(0) != ACM_DRIVER_ID && ARG(0) != ACM_DRIVER_HANDLE) return 0x5; // MMSYSERR_INVALHANDLE
    uint8_t *d = PTR(1);
    uint32_t sz = *(uint32_t *)d;
    if (sz > 920) sz = 920;
    uint8_t t[920] = {0};
    uint32_t *u = (uint32_t *)t;
    u[0] = sz;
    memcpy(t + 4, "audc", 4);
    *(uint16_t *)(t + 12) = 1;  // MM_MICROSOFT
    *(uint16_t *)(t + 14) = 33; // MM_MSFT_ACM_MSADPCM
    u[4] = 0x04000000;
    u[5] = 0x04000000;
    u[6] = 0x1; // codec
    u[7] = 2;   // format tags: PCM, MS-ADPCM
    strcpy((char *)t + 40, "MS-ADPCM");
    strcpy((char *)t + 72, "Microsoft ADPCM CODEC");
    memcpy(d + 4, t + 4, sz - 4);
    return 0;
}
API(msacm32, acmDriverOpen, 3) { *(uint32_t *)PTR(0) = ACM_DRIVER_HANDLE; return 0; }
API(msacm32, acmDriverClose, 2) { return 0; }
API(msacm32, acmDriverID, 3) { *(uint32_t *)PTR(1) = ACM_DRIVER_ID; return 0; }
// acmMetrics(hao, metric, out)
API(msacm32, acmMetrics, 3) {
    uint32_t v;
    switch (ARG(1)) {
    case 1: case 2: case 4: case 5: v = 1; break; // driver counts (one codec)
    case 3: case 6: case 7: v = 0; break;         // converters / filters / disabled
    case 10: v = 2; break;                        // ACM_METRIC_COUNT_LOCAL_DRIVERS etc.
    case 50: v = 50; break;                       // ACM_METRIC_MAX_SIZE_FORMAT (ADPCMWAVEFORMAT)
    case 51: v = 0; break;                        // ACM_METRIC_MAX_SIZE_FILTER
    default: return 0x8; // MMSYSERR_NOTSUPPORTED
    }
    *(uint32_t *)PTR(2) = v;
    return 0;
}
// acmFormatSuggest(had, src, dst, cbDst, flags): MS-ADPCM -> 16-bit PCM
API(msacm32, acmFormatSuggest, 5) {
    AdpcmFmt f;
    if (!adpcm_parse(PTR(1), &f) || ARG(3) < 16) return ACMERR_NOTPOSSIBLE;
    uint8_t *d = PTR(2);
    memset(d, 0, ARG(3) < 18 ? ARG(3) : 18);
    *(uint16_t *)d = 1;
    *(uint16_t *)(d + 2) = f.ch;
    memcpy(d + 4, &f.rate, 4);
    uint32_t avg = f.rate * f.ch * 2;
    memcpy(d + 8, &avg, 4);
    *(uint16_t *)(d + 12) = f.ch * 2;
    *(uint16_t *)(d + 14) = 16;
    return 0;
}
// acmStreamOpen(phas, had, src, dst, filter, callback, instance, flags)
API(msacm32, acmStreamOpen, 8) {
    AdpcmFmt f;
    const uint8_t *dst = PTR(3);
    if (!adpcm_parse(PTR(2), &f) || *(const uint16_t *)dst != 1 || *(const uint16_t *)(dst + 14) != 16 ||
        *(const uint16_t *)(dst + 2) != f.ch) {
        LOGD("acm: conversion from format %u not supported", *(const uint16_t *)PTR(2));
        return ACMERR_NOTPOSSIBLE;
    }
    if (ARG(7) & 1) return 0; // ACM_STREAMOPENF_QUERY
    int i = 1;
    while (i < ACM_MAX && acm_streams[i]) i++;
    if (i == ACM_MAX) return ACMERR_NOTPOSSIBLE;
    AcmStream *s = malloc(sizeof *s);
    s->f = f;
    acm_streams[i] = s;
    *(uint32_t *)PTR(0) = 0xACC00000u + i;
    LOGD("acm: MS-ADPCM stream %u Hz %u ch, block %u", f.rate, f.ch, f.block);
    return 0;
}
static AcmStream *acm_get(uint32_t h) {
    uint32_t i = h - 0xACC00000u;
    return i < ACM_MAX ? acm_streams[i] : NULL;
}
API(msacm32, acmStreamClose, 2) {
    AcmStream *s = acm_get(ARG(0));
    if (s) { acm_streams[ARG(0) - 0xACC00000u] = NULL; free(s); }
    return 0;
}
// acmStreamSize(has, cbInput, pcbOutput, flags): 0 = from source size, 1 = from destination size
API(msacm32, acmStreamSize, 4) {
    AcmStream *s = acm_get(ARG(0));
    if (!s) return ACMERR_NOTPOSSIBLE;
    uint32_t n = ARG(1), frame = s->f.ch * 2, blockout = s->f.spb * frame;
    if (ARG(3) & 1) *(uint32_t *)PTR(2) = (n / blockout) * s->f.block;
    else *(uint32_t *)PTR(2) = ((n + s->f.block - 1) / s->f.block) * blockout;
    return 0;
}
#define ACMSTREAMHEADER_STATUSF_DONE 0x10000
#define ACMSTREAMHEADER_STATUSF_PREPARED 0x20000
API(msacm32, acmStreamPrepareHeader, 3) { ((uint32_t *)PTR(1))[1] |= ACMSTREAMHEADER_STATUSF_PREPARED; return 0; }
API(msacm32, acmStreamUnprepareHeader, 3) { ((uint32_t *)PTR(1))[1] &= ~ACMSTREAMHEADER_STATUSF_PREPARED; return 0; }
API(msacm32, acmStreamReset, 2) { return 0; }
// acmStreamConvert(has, ACMSTREAMHEADER*, flags): whole blocks only
API(msacm32, acmStreamConvert, 3) {
    AcmStream *s = acm_get(ARG(0));
    uint32_t *h = PTR(1);
    if (!s) return ACMERR_NOTPOSSIBLE;
    uint32_t frame = s->f.ch * 2, blockout = s->f.spb * frame;
    uint32_t nblk = h[4] / s->f.block, room = h[8] / blockout;
    if (nblk > room) nblk = room;
    const uint8_t *src = G2H(h[3]);
    int16_t *dst = G2H(h[7]);
    for (uint32_t i = 0; i < nblk; i++) adpcm_block(&s->f, src + i * s->f.block, dst + i * blockout / 2);
    h[5] = nblk * s->f.block;
    h[9] = nblk * blockout;
    h[1] |= ACMSTREAMHEADER_STATUSF_DONE;
    return 0;
}

// The rest of the msacm32 API: no drivers to enumerate or configure.
API(msacm32, acmDriverAddA, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmDriverAddW, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmDriverDetailsW, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmDriverMessage, 4) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmDriverPriority, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmDriverRemove, 2) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterChooseA, 1) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterChooseW, 1) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterDetailsA, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterDetailsW, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterEnumA, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterEnumW, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterTagDetailsA, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterTagDetailsW, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterTagEnumA, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFilterTagEnumW, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatChooseA, 1) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatChooseW, 1) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatDetailsA, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatDetailsW, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatEnumA, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatEnumW, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatTagDetailsA, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatTagDetailsW, 3) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatTagEnumA, 5) { return ACMERR_NOTPOSSIBLE; }
API(msacm32, acmFormatTagEnumW, 5) { return ACMERR_NOTPOSSIBLE; }
