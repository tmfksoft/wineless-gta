// dsound: DirectSound 8 implemented as a software mixer on top of SDL3 audio.
// Secondary buffers live in guest memory, so Lock() hands out direct pointers.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../heap.h"
#include "com.h"
#include "win32.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_NODRIVER 0x88780078u
#define DSERR_OUTOFMEMORY 0x8007000Eu

#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBCAPS_CTRL3D 0x10
#define DSBCAPS_CTRLFREQUENCY 0x20
#define DSBCAPS_CTRLPAN 0x40
#define DSBCAPS_CTRLVOLUME 0x80
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x20000
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define DSBPLAY_LOOPING 1
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER 2

#define OUT_RATE 44100
#define MAX_NOTIFY 64

typedef struct { float x, y, z; } Vec3;

typedef struct {
    uint32_t guest; // sample memory (guest)
    uint32_t size;
    int refs;
} SoundData;

typedef struct Buf {
    struct Buf *next;
    int refs;
    int primary;
    uint32_t flags;
    SoundData *data;
    uint16_t channels, bits, block;
    uint32_t rate, freq;
    int32_t volume, pan;
    int playing, looping;
    double pos; // in frames
    // Starvation guard for streamed buffers: frames played since the game last
    // wrote (Unlock) and how many writes it has made while playing. See mix().
    double stale;
    uint32_t refills;
    // 3D
    Vec3 p, v;
    float min_dist, max_dist;
    uint32_t mode;
    // notifications
    uint32_t notify_off[MAX_NOTIFY], notify_ev[MAX_NOTIFY];
    int nnotify;
    // guest interfaces
    uint32_t g_buf, g_3d, g_notify, g_listener;
} Buf;

static pthread_mutex_t ds_lock = PTHREAD_MUTEX_INITIALIZER;
static Buf *buffers;
static SDL_AudioStream *stream;
static Vec3 lis_pos, lis_front = {0, 0, 1}, lis_top = {0, 1, 0};
static float lis_rolloff = 1.0f, lis_distf = 1.0f;
static uint32_t vt_ds, vt_buf, vt_3d, vt_lis, vt_notify, g_ds;
static uint64_t frames_mixed;

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

// ---------------------------------------------------------------- mixer
static inline float sample_at(const Buf *b, const uint8_t *d, uint32_t frame, int ch) {
    int c = ch < b->channels ? ch : 0;
    if (b->bits == 16) return ((const int16_t *)d)[frame * b->channels + c] * (1.0f / 32768.0f);
    return (d[frame * b->channels + c] - 128) * (1.0f / 128.0f);
}

static void gains_for(const Buf *b, float *gl, float *gr) {
    float g = (b->flags & DSBCAPS_CTRLVOLUME) && b->volume < 0 ? powf(10.0f, b->volume / 2000.0f) : 1.0f;
    float l = 1.0f, r = 1.0f;
    if ((b->flags & DSBCAPS_CTRLPAN) && b->pan) {
        if (b->pan > 0) l = powf(10.0f, -b->pan / 2000.0f);
        else r = powf(10.0f, b->pan / 2000.0f);
    }
    if ((b->flags & DSBCAPS_CTRL3D) && b->mode != 2 /*DS3DMODE_DISABLE*/) {
        Vec3 rel = b->p;
        if (b->mode == 0) { rel.x -= lis_pos.x; rel.y -= lis_pos.y; rel.z -= lis_pos.z; }
        float dist = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z) * lis_distf;
        float mn = b->min_dist > 0 ? b->min_dist : 1.0f, mx = b->max_dist;
        float d = dist > mx ? mx : dist;
        float att = d <= mn ? 1.0f : mn / (mn + lis_rolloff * (d - mn));
        if (dist > mx && (b->flags & DSBCAPS_MUTE3DATMAXDISTANCE)) att = 0;
        g *= att;
        if (dist > 1e-4f) {
            Vec3 t = lis_top, f = lis_front;
            Vec3 right = {t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
            float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
            float x = rl > 0 ? (rel.x * right.x + rel.y * right.y + rel.z * right.z) / (rl * dist) : 0;
            l *= sqrtf((1 - x) * 0.5f) * 1.41421356f;
            r *= sqrtf((1 + x) * 0.5f) * 1.41421356f;
        }
    }
    *gl = g * l;
    *gr = g * r;
}

static void fire_notifications(Buf *b, uint32_t from, uint32_t to, int wrapped) {
    for (int i = 0; i < b->nnotify; i++) {
        uint32_t o = b->notify_off[i];
        int hit;
        if (o == 0xFFFFFFFF) hit = 0; // DSBPN_OFFSETSTOP handled on stop
        else if (!wrapped) hit = o >= from && o < to;
        else hit = o >= from || o < to;
        if (hit) {
            Object *ev = handle_get(b->notify_ev[i], OBJ_EVENT);
            if (ev) obj_signal_event(ev, 1);
        }
    }
}
static void notify_stop(Buf *b) {
    for (int i = 0; i < b->nnotify; i++)
        if (b->notify_off[i] == 0xFFFFFFFF) {
            Object *ev = handle_get(b->notify_ev[i], OBJ_EVENT);
            if (ev) obj_signal_event(ev, 1);
        }
}

// Extra stereo stream at OUT_RATE fed by the movie player (dshow.c).
#define MOVIE_RING (OUT_RATE * 2) // frames (2s)
static float movie_ring[MOVIE_RING * 2];
static uint32_t movie_rd, movie_wr; // frame indices (monotonic)

// Push stereo float frames at `rate`; resampled (linear) to the mixer rate.
static double movie_frac;
static float movie_prev[2];
void dsound_movie_push(const float *stereo, uint32_t frames, uint32_t rate) {
    pthread_mutex_lock(&ds_lock);
    double step = rate ? (double)rate / OUT_RATE : 1.0;
    // position runs over [-1, frames): -1 is the last frame of the previous chunk
    double pos = movie_frac - 1.0;
    while (pos < (double)frames - 1.0) {
        if (movie_wr - movie_rd >= MOVIE_RING) break; // full: drop
        int i0 = (int)floor(pos);
        float f = (float)(pos - i0);
        const float *a = i0 < 0 ? movie_prev : stereo + 2 * i0;
        const float *b = stereo + 2 * (i0 + 1);
        uint32_t k = (movie_wr++ % MOVIE_RING) * 2;
        movie_ring[k] = a[0] + (b[0] - a[0]) * f;
        movie_ring[k + 1] = a[1] + (b[1] - a[1]) * f;
        pos += step;
    }
    movie_frac = pos - ((double)frames - 1.0);
    if (frames) { movie_prev[0] = stereo[2 * (frames - 1)]; movie_prev[1] = stereo[2 * (frames - 1) + 1]; }
    pthread_mutex_unlock(&ds_lock);
}
void dsound_movie_reset(void) {
    pthread_mutex_lock(&ds_lock);
    movie_rd = movie_wr = 0;
    movie_frac = 0;
    movie_prev[0] = movie_prev[1] = 0;
    pthread_mutex_unlock(&ds_lock);
}
uint32_t dsound_movie_queued(void) { return movie_wr - movie_rd; }

void waveout_mix(float *out, int frames);

static void mix(float *out, int frames) {
    memset(out, 0, frames * 2 * sizeof(float));
    pthread_mutex_lock(&ds_lock);
    for (int i = 0; i < frames && movie_rd != movie_wr; i++, movie_rd++) {
        uint32_t k = (movie_rd % MOVIE_RING) * 2;
        out[2 * i] += movie_ring[k];
        out[2 * i + 1] += movie_ring[k + 1];
    }
    for (Buf *b = buffers; b; b = b->next) {
        if (b->primary || !b->playing || !b->data || !b->block) continue;
        const uint8_t *d = G2H(b->data->guest);
        uint32_t nframes = b->data->size / b->block;
        if (!nframes) continue;
        double step = (double)(b->freq ? b->freq : b->rate) / OUT_RATE;
        float gl, gr;
        gains_for(b, &gl, &gr);
        uint32_t start_byte = (uint32_t)b->pos * b->block;
        int wrapped = 0;
        // A looping buffer the game streams into (refilled while playing) that it
        // has not written for a whole lap holds nothing but audio already heard.
        // Real DirectSound would replay it, but a game that stalls for most of a
        // second (AoE2 HD on a slow node) then repeats the same chunk over and
        // over. Play silence instead, keep the cursor moving so the game's
        // position reads and notifications stay right, and resume on its next write.
        // A buffer written once and looped (an ambient loop) never counts as streamed.
        int starved = b->looping && b->refills >= 2 && b->stale >= nframes;
        b->stale += step * frames;
        if (starved) {
            b->pos += step * frames;
            while (b->pos >= nframes) { b->pos -= nframes; wrapped = 1; }
            if (b->nnotify) fire_notifications(b, start_byte, (uint32_t)b->pos * b->block, wrapped);
            continue;
        }
        for (int i = 0; i < frames; i++) {
            uint32_t i0 = (uint32_t)b->pos;
            float fr = (float)(b->pos - i0);
            uint32_t i1 = i0 + 1 >= nframes ? (b->looping ? 0 : i0) : i0 + 1;
            float l = sample_at(b, d, i0, 0), r = sample_at(b, d, i0, 1);
            l += (sample_at(b, d, i1, 0) - l) * fr;
            r += (sample_at(b, d, i1, 1) - r) * fr;
            out[2 * i] += l * gl;
            out[2 * i + 1] += r * gr;
            b->pos += step;
            if (b->pos >= nframes) {
                if (b->looping) { b->pos -= nframes; wrapped = 1; }
                else { b->pos = 0; b->playing = 0; notify_stop(b); break; }
            }
        }
        if (b->nnotify) fire_notifications(b, start_byte, (uint32_t)b->pos * b->block, wrapped);
    }
    frames_mixed += frames;
    pthread_mutex_unlock(&ds_lock);
    waveout_mix(out, frames);
    for (int i = 0; i < frames * 2; i++) {
        float s = out[i];
        out[i] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
    }
}

static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    float buf[1024 * 2];
    while (additional > 0) {
        int frames = additional / 8;
        if (frames > 1024) frames = 1024;
        if (frames <= 0) frames = 1;
        mix(buf, frames);
        SDL_PutAudioStreamData(s, buf, frames * 8);
        additional -= frames * 8;
    }
}

static void *null_audio_thread(void *arg) {
    float buf[441 * 2];
    for (;;) {
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
        mix(buf, 441);
    }
    return NULL;
}

static void audio_start(void);
void dsound_audio_start(void) { audio_start(); }

// Pause/resume the audio device on focus change, so a looping buffer doesn't
// get stuck repeating while the game is paused in the background.
void dsound_set_paused(int paused) {
    if (!stream) return;
    if (paused) SDL_PauseAudioStreamDevice(stream);
    else SDL_ResumeAudioStreamDevice(stream);
}

// ---- libretro core audio bridge ----
// When set, the core owns audio output and pulls the mixer itself (see
// wl_audio_pull); dsound must not also drive mix() from SDL or the null thread.
int wl_core_audio = 0;
int wl_audio_rate(void) { return OUT_RATE; }
int wl_audio_pull(short *out, int frames) {
    if (frames < 0) frames = 0;
    if (frames > 2048) frames = 2048;
    float buf[2048 * 2];
    mix(buf, frames);
    for (int i = 0; i < frames * 2; i++) {
        float s = buf[i];
        s = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
        out[i] = (short)(s * 32767.0f);
    }
    return frames;
}

static void audio_start(void) {
    static int started;
    if (started) return;
    started = 1;
    if (wl_core_audio) {
        LOGI("dsound: libretro owns audio output (core pulls the mixer)");
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    }
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream);
        LOGI("dsound: SDL audio output via '%s'", SDL_GetCurrentAudioDriver());
    } else {
        LOGW("dsound: no audio device (%s); mixing silently", SDL_GetError());
        pthread_t t;
        pthread_create(&t, NULL, null_audio_thread, NULL);
        pthread_detach(t);
    }
}

// ---------------------------------------------------------------- buffers
static Buf *buf_of(uint32_t g) { return com_host(g); }

static uint32_t make_guest(uint32_t vt, Buf *b) { return com_new(vt, b, 0); }

static void buf_unref(Buf *b) {
    pthread_mutex_lock(&ds_lock);
    if (--b->refs > 0) { pthread_mutex_unlock(&ds_lock); return; }
    for (Buf **pp = &buffers; *pp; pp = &(*pp)->next)
        if (*pp == b) { *pp = b->next; break; }
    pthread_mutex_unlock(&ds_lock);
    if (b->data && --b->data->refs == 0) {
        gheap_free(b->data->guest);
        free(b->data);
    }
    uint32_t gs[] = {b->g_buf, b->g_3d, b->g_notify, b->g_listener};
    for (int i = 0; i < 4; i++)
        if (gs[i]) gheap_free(gs[i]);
    free(b);
}

static uint32_t b_addref(uint32_t *a) { return ++buf_of(a[0])->refs; }
static uint32_t b_release(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    int r = b->refs - 1;
    buf_unref(b);
    return r;
}

static const uint8_t D4_DS[8] = {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static int iid_is(const void *g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t *d4) { return guid_eq(g, d1, d2, d3, d4); }

static uint32_t b_qi(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    const void *iid = G2H(a[1]);
    uint32_t *out = G2H(a[2]);
    static const uint8_t d4_b8[8] = {0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E};
    static const uint8_t d4_notify[8] = {0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16};
    static const uint8_t d4_unk[8] = {0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    uint32_t r = 0;
    if (iid_is(iid, 0x279AFA85, 0x4981, 0x11CE, D4_DS) || iid_is(iid, 0x6825A449, 0x7524, 0x4D82, d4_b8) ||
        iid_is(iid, 0, 0, 0, d4_unk))
        r = b->g_buf;
    else if (iid_is(iid, 0x279AFA86, 0x4981, 0x11CE, D4_DS) && (b->flags & DSBCAPS_CTRL3D) && !b->primary) {
        if (!b->g_3d) b->g_3d = make_guest(vt_3d, b);
        r = b->g_3d;
    } else if (iid_is(iid, 0x279AFA84, 0x4981, 0x11CE, D4_DS) && b->primary) {
        if (!b->g_listener) b->g_listener = make_guest(vt_lis, b);
        r = b->g_listener;
    } else if (iid_is(iid, 0xB0210783, 0x89CD, 0x11D0, d4_notify)) {
        if (!b->g_notify) b->g_notify = make_guest(vt_notify, b);
        r = b->g_notify;
    }
    if (!r) {
        uint32_t d1;
        memcpy(&d1, iid, 4);
        LOGD("dsound: buffer QI {%08x} unsupported", d1);
        *out = 0;
        return E_NOINTERFACE;
    }
    b->refs++;
    *out = r;
    return S_OK;
}

static uint32_t b_getcaps(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    uint32_t *c = G2H(a[1]);
    c[1] = b->flags | 0x8; // LOCSOFTWARE
    c[2] = b->data ? b->data->size : 0;
    c[3] = 0;
    c[4] = 0;
    return DS_OK;
}
static uint32_t b_getpos(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    pthread_mutex_lock(&ds_lock);
    uint32_t size = b->data ? b->data->size : 0;
    uint32_t play = b->block ? (uint32_t)b->pos * b->block : 0;
    // write cursor leads by ~15ms like a typical software buffer
    uint32_t lead = b->block ? ((b->freq ? b->freq : b->rate) * 15 / 1000) * b->block : 0;
    uint32_t write = size ? (play + (b->playing ? lead : 0)) % size : 0;
    pthread_mutex_unlock(&ds_lock);
    if (a[1]) *(uint32_t *)G2H(a[1]) = play;
    if (a[2]) *(uint32_t *)G2H(a[2]) = write;
    return DS_OK;
}
static uint32_t b_getformat(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    uint8_t w[18] = {0};
    *(uint16_t *)w = 1;
    *(uint16_t *)(w + 2) = b->channels;
    *(uint32_t *)(w + 4) = b->rate;
    *(uint32_t *)(w + 8) = b->rate * b->block;
    *(uint16_t *)(w + 12) = b->block;
    *(uint16_t *)(w + 14) = b->bits;
    if (a[1]) memcpy(G2H(a[1]), w, a[2] < 18 ? a[2] : 18);
    if (a[3]) *(uint32_t *)G2H(a[3]) = 18;
    return DS_OK;
}
static uint32_t b_getvolume(uint32_t *a) { *(int32_t *)G2H(a[1]) = buf_of(a[0])->volume; return DS_OK; }
static uint32_t b_getpan(uint32_t *a) { *(int32_t *)G2H(a[1]) = buf_of(a[0])->pan; return DS_OK; }
static uint32_t b_getfreq(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    *(uint32_t *)G2H(a[1]) = b->freq ? b->freq : b->rate;
    return DS_OK;
}
static uint32_t b_getstatus(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    *(uint32_t *)G2H(a[1]) = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0);
    return DS_OK;
}
static uint32_t b_initialize(uint32_t *a) { return DS_OK; }
static uint32_t b_lock(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    if (b->primary || !b->data) return DSERR_INVALIDPARAM;
    uint32_t size = b->data->size, off = a[1], bytes = a[2], flags = a[7];
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        uint32_t w;
        uint32_t lead = ((b->freq ? b->freq : b->rate) * 15 / 1000) * b->block;
        w = ((uint32_t)b->pos * b->block + lead) % size;
        off = w;
    }
    if (flags & DSBLOCK_ENTIREBUFFER) { off = 0; bytes = size; }
    if (off >= size || bytes > size) return DSERR_INVALIDPARAM;
    uint32_t n1 = bytes, n2 = 0;
    if (off + bytes > size) { n1 = size - off; n2 = bytes - n1; }
    if (a[3]) *(uint32_t *)G2H(a[3]) = b->data->guest + off;
    if (a[4]) *(uint32_t *)G2H(a[4]) = n1;
    if (a[5]) *(uint32_t *)G2H(a[5]) = n2 ? b->data->guest : 0;
    if (a[6]) *(uint32_t *)G2H(a[6]) = n2;
    return DS_OK;
}
static uint32_t b_play(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    pthread_mutex_lock(&ds_lock);
    if (!b->playing) { b->stale = 0; b->refills = 0; }
    b->playing = 1;
    b->looping = (a[3] & DSBPLAY_LOOPING) != 0;
    pthread_mutex_unlock(&ds_lock);
    return DS_OK;
}
static uint32_t b_setpos(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    pthread_mutex_lock(&ds_lock);
    if (b->block && b->data) b->pos = (a[1] % b->data->size) / b->block;
    pthread_mutex_unlock(&ds_lock);
    return DS_OK;
}
static void set_format(Buf *b, const uint8_t *w) {
    b->channels = *(uint16_t *)(w + 2);
    b->rate = *(uint32_t *)(w + 4);
    b->bits = *(uint16_t *)(w + 14);
    b->block = *(uint16_t *)(w + 12);
    if (!b->block) b->block = b->channels * b->bits / 8;
}
static uint32_t b_setformat(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    if (a[1]) set_format(b, G2H(a[1]));
    LOGD("dsound: primary format %uHz %ubit %uch", b->rate, b->bits, b->channels);
    return DS_OK;
}
static uint32_t b_setvolume(uint32_t *a) {
    int32_t v = (int32_t)a[1];
    buf_of(a[0])->volume = v < -10000 ? -10000 : v > 0 ? 0 : v;
    return DS_OK;
}
static uint32_t b_setpan(uint32_t *a) {
    int32_t v = (int32_t)a[1];
    buf_of(a[0])->pan = v < -10000 ? -10000 : v > 10000 ? 10000 : v;
    return DS_OK;
}
static uint32_t b_setfreq(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    b->freq = a[1]; // 0 = DSBFREQUENCY_ORIGINAL
    return DS_OK;
}
static uint32_t b_stop(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    pthread_mutex_lock(&ds_lock);
    int was = b->playing;
    b->playing = 0;
    if (was) notify_stop(b);
    pthread_mutex_unlock(&ds_lock);
    return DS_OK;
}
static uint32_t b_unlock(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    pthread_mutex_lock(&ds_lock);
    b->stale = 0;
    if (b->playing && b->refills < 0xFFFFFFFFu) b->refills++;
    pthread_mutex_unlock(&ds_lock);
    return DS_OK;
}
static uint32_t b_restore(uint32_t *a) { return DS_OK; }
static uint32_t b_setfx(uint32_t *a) { return DSERR_CONTROLUNAVAIL; }
static uint32_t b_acquire(uint32_t *a) { return DS_OK; }

// ---------------------------------------------------------------- 3D buffer / listener / notify
static uint32_t d3_getall(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    uint32_t *p = G2H(a[1]);
    memcpy(p + 1, &b->p, 12);
    memcpy(p + 4, &b->v, 12);
    p[7] = 360; p[8] = 360;
    float o[3] = {0, 0, 1};
    memcpy(p + 9, o, 12);
    p[12] = 0;
    memcpy(p + 13, &b->min_dist, 4);
    memcpy(p + 14, &b->max_dist, 4);
    p[15] = b->mode;
    return DS_OK;
}
static uint32_t d3_setall(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    uint32_t *p = G2H(a[1]);
    memcpy(&b->p, p + 1, 12);
    memcpy(&b->v, p + 4, 12);
    memcpy(&b->min_dist, p + 13, 4);
    memcpy(&b->max_dist, p + 14, 4);
    b->mode = p[15];
    return DS_OK;
}
static uint32_t d3_ok(uint32_t *a) { return DS_OK; }
static uint32_t d3_getf_min(uint32_t *a) { memcpy(G2H(a[1]), &buf_of(a[0])->min_dist, 4); return DS_OK; }
static uint32_t d3_getf_max(uint32_t *a) { memcpy(G2H(a[1]), &buf_of(a[0])->max_dist, 4); return DS_OK; }
static uint32_t d3_getmode(uint32_t *a) { *(uint32_t *)G2H(a[1]) = buf_of(a[0])->mode; return DS_OK; }
static uint32_t d3_getpos(uint32_t *a) { memcpy(G2H(a[1]), &buf_of(a[0])->p, 12); return DS_OK; }
static uint32_t d3_getvel(uint32_t *a) { memcpy(G2H(a[1]), &buf_of(a[0])->v, 12); return DS_OK; }
static uint32_t d3_setmax(uint32_t *a) { buf_of(a[0])->max_dist = f32(a[1]); return DS_OK; }
static uint32_t d3_setmin(uint32_t *a) { buf_of(a[0])->min_dist = f32(a[1]); return DS_OK; }
static uint32_t d3_setmode(uint32_t *a) { buf_of(a[0])->mode = a[1]; return DS_OK; }
static uint32_t d3_setpos(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    b->p = (Vec3){f32(a[1]), f32(a[2]), f32(a[3])};
    return DS_OK;
}
static uint32_t d3_setvel(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    b->v = (Vec3){f32(a[1]), f32(a[2]), f32(a[3])};
    return DS_OK;
}
static uint32_t d3_getcone(uint32_t *a) {
    if (a[1]) *(uint32_t *)G2H(a[1]) = 360;
    if (a[2]) *(uint32_t *)G2H(a[2]) = 360;
    return DS_OK;
}
static uint32_t d3_getconeo(uint32_t *a) { float o[3] = {0, 0, 1}; memcpy(G2H(a[1]), o, 12); return DS_OK; }
static uint32_t d3_getconev(uint32_t *a) { *(int32_t *)G2H(a[1]) = 0; return DS_OK; }

static uint32_t l_getall(uint32_t *a) {
    uint32_t *p = G2H(a[1]);
    memcpy(p + 1, &lis_pos, 12);
    memset(p + 4, 0, 12);
    memcpy(p + 7, &lis_distf, 4);
    memcpy(p + 8, &lis_rolloff, 4);
    float df = 1.0f;
    memcpy(p + 9, &df, 4);
    memcpy(p + 10, &lis_front, 12);
    memcpy(p + 13, &lis_top, 12);
    return DS_OK;
}
static uint32_t l_setall(uint32_t *a) {
    uint32_t *p = G2H(a[1]);
    memcpy(&lis_pos, p + 1, 12);
    memcpy(&lis_distf, p + 7, 4);
    memcpy(&lis_rolloff, p + 8, 4);
    memcpy(&lis_front, p + 10, 12);
    memcpy(&lis_top, p + 13, 12);
    return DS_OK;
}
static uint32_t l_getf(uint32_t *a) { float one = 1.0f; memcpy(G2H(a[1]), &one, 4); return DS_OK; }
static uint32_t l_getorient(uint32_t *a) { memcpy(G2H(a[1]), &lis_front, 12); memcpy(G2H(a[2]), &lis_top, 12); return DS_OK; }
static uint32_t l_getpos(uint32_t *a) { memcpy(G2H(a[1]), &lis_pos, 12); return DS_OK; }
static uint32_t l_getvel(uint32_t *a) { memset(G2H(a[1]), 0, 12); return DS_OK; }
static uint32_t l_setdist(uint32_t *a) { lis_distf = f32(a[1]); return DS_OK; }
static uint32_t l_setroll(uint32_t *a) { lis_rolloff = f32(a[1]); return DS_OK; }
static uint32_t l_setorient(uint32_t *a) {
    lis_front = (Vec3){f32(a[1]), f32(a[2]), f32(a[3])};
    lis_top = (Vec3){f32(a[4]), f32(a[5]), f32(a[6])};
    return DS_OK;
}
static uint32_t l_setpos(uint32_t *a) { lis_pos = (Vec3){f32(a[1]), f32(a[2]), f32(a[3])}; return DS_OK; }
static uint32_t l_setf(uint32_t *a) { return DS_OK; }
static uint32_t l_setvel(uint32_t *a) { return DS_OK; }

static uint32_t n_setpositions(uint32_t *a) {
    Buf *b = buf_of(a[0]);
    uint32_t n = a[1];
    uint32_t *p = G2H(a[2]);
    pthread_mutex_lock(&ds_lock);
    b->nnotify = 0;
    for (uint32_t i = 0; i < n && i < MAX_NOTIFY; i++) {
        b->notify_off[i] = p[2 * i];
        b->notify_ev[i] = p[2 * i + 1];
        b->nnotify++;
    }
    pthread_mutex_unlock(&ds_lock);
    return DS_OK;
}

static void build_vtables(void) {
    if (vt_buf) return;
    ComMethod mb[24] = {
        {b_qi, 3}, {b_addref, 1}, {b_release, 1}, {b_getcaps, 2}, {b_getpos, 3}, {b_getformat, 4},
        {b_getvolume, 2}, {b_getpan, 2}, {b_getfreq, 2}, {b_getstatus, 2}, {b_initialize, 3},
        {b_lock, 8}, {b_play, 4}, {b_setpos, 2}, {b_setformat, 2}, {b_setvolume, 2}, {b_setpan, 2},
        {b_setfreq, 2}, {b_stop, 1}, {b_unlock, 5}, {b_restore, 1}, {b_setfx, 4}, {b_acquire, 4},
        {NULL, 0}};
    vt_buf = com_vtable("IDirectSoundBuffer8", mb, 24);
    ComMethod m3[21] = {
        {b_qi, 3}, {b_addref, 1}, {b_release, 1}, {d3_getall, 2}, {d3_getcone, 3}, {d3_getconeo, 2},
        {d3_getconev, 2}, {d3_getf_max, 2}, {d3_getf_min, 2}, {d3_getmode, 2}, {d3_getpos, 2},
        {d3_getvel, 2}, {d3_setall, 3}, {d3_ok, 4}, {d3_ok, 5}, {d3_ok, 3}, {d3_setmax, 3},
        {d3_setmin, 3}, {d3_setmode, 3}, {d3_setpos, 5}, {d3_setvel, 5}};
    vt_3d = com_vtable("IDirectSound3DBuffer", m3, 21);
    ComMethod ml[18] = {
        {b_qi, 3}, {b_addref, 1}, {b_release, 1}, {l_getall, 2}, {l_getf, 2}, {l_getf, 2},
        {l_getorient, 3}, {l_getpos, 2}, {l_getf, 2}, {l_getvel, 2}, {l_setall, 3}, {l_setdist, 3},
        {l_setf, 3}, {l_setorient, 8}, {l_setpos, 5}, {l_setroll, 3}, {l_setvel, 5}, {d3_ok, 1}};
    vt_lis = com_vtable("IDirectSound3DListener", ml, 18);
    ComMethod mn[4] = {{b_qi, 3}, {b_addref, 1}, {b_release, 1}, {n_setpositions, 3}};
    vt_notify = com_vtable("IDirectSoundNotify", mn, 4);
}

// ---------------------------------------------------------------- IDirectSound8
static uint32_t ds_createbuffer(uint32_t *a) {
    uint32_t *desc = G2H(a[1]);
    uint32_t flags = desc[1], bytes = desc[2], wfx = desc[4];
    Buf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->flags = flags;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->max_dist = 1e9f;
    b->min_dist = 1.0f;
    if (wfx) set_format(b, G2H(wfx));
    else { b->channels = 2; b->rate = 44100; b->bits = 16; b->block = 4; }
    if (!b->primary) {
        if (!bytes) { free(b); return DSERR_INVALIDPARAM; }
        b->data = calloc(1, sizeof *b->data);
        b->data->size = bytes;
        b->data->refs = 1;
        b->data->guest = gheap_alloc(bytes, 1);
        if (!b->data->guest) { free(b->data); free(b); return DSERR_OUTOFMEMORY; }
        if (b->bits == 8) memset(G2H(b->data->guest), 0x80, bytes);
    }
    b->g_buf = make_guest(vt_buf, b);
    pthread_mutex_lock(&ds_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&ds_lock);
    LOGD("dsound: CreateSoundBuffer flags 0x%x bytes %u %uHz %ubit %uch -> 0x%x", flags, bytes, b->rate, b->bits,
         b->channels, b->g_buf);
    *(uint32_t *)G2H(a[2]) = b->g_buf;
    return DS_OK;
}
static uint32_t ds_getcaps(uint32_t *a) {
    uint32_t *c = G2H(a[1]);
    memset(c + 1, 0, 92);
    c[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800; // PRIMARY*/CONTINUOUSRATE/SECONDARY*
    c[2] = 100;
    c[3] = 200000;
    c[4] = 1;
    for (int i = 5; i <= 16; i++) c[i] = 128;
    return DS_OK;
}
static uint32_t ds_duplicate(uint32_t *a) {
    Buf *o = buf_of(a[1]);
    if (o->primary) return DSERR_INVALIDPARAM;
    Buf *b = calloc(1, sizeof *b);
    *b = *o;
    b->next = NULL;
    b->refs = 1;
    b->playing = 0;
    b->pos = 0;
    b->nnotify = 0;
    b->g_3d = b->g_notify = b->g_listener = 0;
    o->data->refs++;
    b->g_buf = make_guest(vt_buf, b);
    pthread_mutex_lock(&ds_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&ds_lock);
    *(uint32_t *)G2H(a[2]) = b->g_buf;
    return DS_OK;
}
static uint32_t ds_ok(uint32_t *a) { return DS_OK; }
static uint32_t ds_getspeaker(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 4; return DS_OK; } // DSSPEAKER_STEREO
static uint32_t ds_verify(uint32_t *a) { *(uint32_t *)G2H(a[1]) = 1; return DS_OK; }

static uint32_t create_ds(uint32_t *out) {
    build_vtables();
    if (!g_ds) {
        ComMethod m[12] = {{com_qi_self, 3}, {com_addref, 1}, {com_release_nofree, 1}, {ds_createbuffer, 4},
                           {ds_getcaps, 2}, {ds_duplicate, 3}, {ds_ok, 3}, {ds_ok, 1}, {ds_getspeaker, 2},
                           {ds_ok, 2}, {ds_ok, 2}, {ds_verify, 2}};
        vt_ds = com_vtable("IDirectSound8", m, 12);
        g_ds = com_new(vt_ds, NULL, 0);
        audio_start();
    }
    com_obj(g_ds)->refs++;
    *out = g_ds;
    return DS_OK;
}

API_ORD(dsound, DirectSoundCreate8, 11, 3) { return create_ds(PTR(1)); }
API_ORD(dsound, DirectSoundCreate, 1, 3) { return create_ds(PTR(1)); }
API_ORD(dsound, DirectSoundEnumerateA, 2, 2) {
    // LPDSENUMCALLBACKA(GUID*, desc, module, ctx)
    uint32_t desc = gheap_strdup("Primary Sound Driver"), mod = gheap_strdup("");
    uint32_t args[4] = {0, desc, mod, ARG(1)};
    call32(thread_current()->ctx, ARG(0), 4, args);
    gheap_free(desc);
    gheap_free(mod);
    return DS_OK;
}
// EAX.DLL ordinal 6: EAXDirectSoundCreate -> plain DirectSound (no EAX effects)
API_ORD(eax, EAXDirectSoundCreate, 6, 3) { return create_ds(PTR(1)); }
