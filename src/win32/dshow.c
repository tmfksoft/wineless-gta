// A stand-in DirectShow FilterGraph for the intro movies. The game renders a
// movie file, runs the graph and waits for EC_COMPLETE (delivered through the
// window message it registers with IMediaEventEx::SetNotifyWindow), stopping
// the graph early if a key is pressed.
//
// The MPEG-1 files are decoded with pl_mpeg on a playback thread (like
// DirectShow, so the movie keeps running while the game loads): video frames
// go to the back buffer through the game's D3D9 device (serialized with the
// game's own D3D calls by the bridge lock) and audio into the DirectSound mixer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define PL_MPEG_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../third_party/pl_mpeg.h"
#pragma GCC diagnostic pop

#include "../vfs.h"

#include "../heap.h"
#include "com.h"
#include "win32.h"

#define E_ABORT 0x80004004u
#define EC_COMPLETE 1

typedef struct {
    int refs;
    uint32_t state; // 0 stopped, 1 paused, 2 running
    uint32_t notify_hwnd, notify_msg, notify_lp;
    int complete_pending;
    uint32_t g_graph, g_control, g_event, g_video, g_seeking;
    // playback
    plm_t *plm;
    uint8_t *frame;  // BGRX
    int frame_dirty;
    char name[260];
    pthread_t th;
    volatile int run;
    int has_thread;
} Graph;

void d3d9_movie_frame(const void *bgrx, uint32_t w, uint32_t h);
void d3d9_movie_end(void);
void dsound_movie_push(const float *stereo, uint32_t frames, uint32_t rate);
void dsound_movie_reset(void);
void dsound_audio_start(void);

static Graph *playing; // graph whose movie is currently running

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void on_video(plm_t *p, plm_frame_t *f, void *user) {
    Graph *g = user;
    plm_frame_to_bgra(f, g->frame, f->width * 4);
    g->frame_dirty = 1;
}
static void on_audio(plm_t *p, plm_samples_t *smp, void *user) {
    dsound_movie_push(smp->interleaved, smp->count, (uint32_t)plm_get_samplerate(p));
}

static void complete(Graph *g);

static void *movie_thread(void *arg) {
    Graph *g = arg;
    double fps = plm_get_framerate(g->plm);
    double frame_t = fps > 1 ? 1.0 / fps : 1.0 / 30;
    double last = now_s();
    while (g->run) {
        double t = now_s(), dt = t - last;
        last = t;
        if (dt > 0.1) dt = 0.1;
        plm_decode(g->plm, dt);
        if (g->frame_dirty) {
            g->frame_dirty = 0;
            d3d9_movie_frame(g->frame, plm_get_width(g->plm), plm_get_height(g->plm));
        }
        if (plm_has_ended(g->plm)) {
            LOGI("dshow: '%s' finished", g->name);
            g->state = 0;
            complete(g);
            break;
        }
        double sleep = frame_t / 2 - (now_s() - t);
        if (sleep > 0.001) usleep((useconds_t)(sleep * 1e6));
    }
    return NULL;
}

static void stop_playback(Graph *g) {
    if (g->has_thread) {
        g->run = 0;
        pthread_join(g->th, NULL);
        g->has_thread = 0;
    }
    if (playing == g) {
        playing = NULL;
        dsound_movie_reset();
        d3d9_movie_end();
    }
}

void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

static Graph *gr(uint32_t *a) { return com_host(a[0]); }

static const uint8_t D4_A[8] = {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
static const uint8_t D4_SEEK[8] = {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60};
static const uint8_t D4_UNK[8] = {0xC0, 0, 0, 0, 0, 0, 0, 0x46};

static uint32_t g_qi(uint32_t *a) {
    Graph *g = gr(a);
    const void *iid = G2H(a[1]);
    uint32_t r = 0;
    if (guid_eq(iid, 0x56a868a9, 0x0ad4, 0x11ce, D4_A) || guid_eq(iid, 0, 0, 0, D4_UNK)) r = g->g_graph;
    else if (guid_eq(iid, 0x56a868b1, 0x0ad4, 0x11ce, D4_A)) r = g->g_control;
    else if (guid_eq(iid, 0x56a868c0, 0x0ad4, 0x11ce, D4_A) || guid_eq(iid, 0x56a868b6, 0x0ad4, 0x11ce, D4_A)) r = g->g_event;
    else if (guid_eq(iid, 0x56a868b4, 0x0ad4, 0x11ce, D4_A)) r = g->g_video;
    else if (guid_eq(iid, 0x36b73880, 0xc2c8, 0x11cf, D4_SEEK)) r = g->g_seeking;
    if (!r) {
        *(uint32_t *)G2H(a[2]) = 0;
        return E_NOINTERFACE;
    }
    g->refs++;
    *(uint32_t *)G2H(a[2]) = r;
    return S_OK;
}
static uint32_t g_addref(uint32_t *a) { return ++gr(a)->refs; }
static uint32_t g_release(uint32_t *a) {
    Graph *g = gr(a);
    int r = --g->refs;
    if (r == 0) {
        stop_playback(g);
        if (g->plm) plm_destroy(g->plm);
        free(g->frame);
        uint32_t objs[] = {g->g_graph, g->g_control, g->g_event, g->g_video, g->g_seeking};
        for (int i = 0; i < 5; i++) gheap_free(objs[i]);
        free(g);
    }
    return r;
}
static uint32_t ok(uint32_t *a) { return S_OK; }
static uint32_t notimpl(uint32_t *a) { return E_NOTIMPL; }
static uint32_t get_zero(uint32_t *a) {
    if (a[1]) *(uint32_t *)G2H(a[1]) = 0;
    return S_OK;
}

// IGraphBuilder
static uint32_t gb_renderfile(uint32_t *a) {
    Graph *g = gr(a);
    char host[1024];
    w2a(G2H(a[1]), g->name, sizeof g->name);
    if (g->plm) { plm_destroy(g->plm); g->plm = NULL; }
    if (vfs_to_host(g->name, host, sizeof host)) g->plm = plm_create_with_filename(host);
    if (!g->plm || !plm_get_width(g->plm)) {
        LOGW("dshow: cannot play '%s'; skipping it", g->name);
        if (g->plm) { plm_destroy(g->plm); g->plm = NULL; }
        return S_OK; // Run() will then report completion immediately
    }
    int w = plm_get_width(g->plm), h = plm_get_height(g->plm);
    free(g->frame);
    g->frame = calloc((size_t)w * h, 4);
    plm_set_video_decode_callback(g->plm, on_video, g);
    plm_set_audio_decode_callback(g->plm, on_audio, g);
    plm_set_audio_enabled(g->plm, 1);
    plm_set_audio_stream(g->plm, 0);
    plm_set_audio_lead_time(g->plm, 0.15);
    plm_set_loop(g->plm, 0);
    LOGI("dshow: playing '%s' (%dx%d, %.0f fps, %d Hz audio)", g->name, w, h, plm_get_framerate(g->plm),
         plm_get_samplerate(g->plm));
    return S_OK;
}
// IMediaControl
static void complete(Graph *g) {
    g->complete_pending = 1;
    if (g->notify_hwnd && g->notify_msg) user32_post(g->notify_hwnd, g->notify_msg, 0, g->notify_lp);
}
static uint32_t mc_run(uint32_t *a) {
    Graph *g = gr(a);
    g->state = 2;
    if (!g->plm) {
        complete(g);
        return S_OK;
    }
    if (playing && playing != g) stop_playback(playing);
    if (g->has_thread) return S_OK; // already running
    dsound_audio_start(); // GTA III opens its sound only after the intro movies
    playing = g;
    g->run = 1;
    g->has_thread = pthread_create(&g->th, NULL, movie_thread, g) == 0;
    return S_OK;
}
static uint32_t mc_pause(uint32_t *a) {
    Graph *g = gr(a);
    g->state = 1;
    stop_playback(g); // the game only pauses movies on its way out
    return S_OK;
}
static uint32_t mc_stop(uint32_t *a) {
    Graph *g = gr(a);
    g->state = 0;
    stop_playback(g);
    return S_OK;
}

static uint32_t mc_getstate(uint32_t *a) {
    *(uint32_t *)G2H(a[2]) = gr(a)->state;
    return S_OK;
}

// IMediaEventEx
static uint32_t me_getevent(uint32_t *a) {
    Graph *g = gr(a);
    if (!g->complete_pending) return E_ABORT;
    g->complete_pending = 0;
    *(uint32_t *)G2H(a[1]) = EC_COMPLETE;
    *(uint32_t *)G2H(a[2]) = 0;
    *(uint32_t *)G2H(a[3]) = 0;
    return S_OK;
}
static uint32_t me_wait(uint32_t *a) {
    if (a[2]) *(uint32_t *)G2H(a[2]) = EC_COMPLETE;
    return S_OK;
}
static uint32_t me_setnotify(uint32_t *a) {
    Graph *g = gr(a);
    g->notify_hwnd = a[1];
    g->notify_msg = a[2];
    g->notify_lp = a[3];
    if (g->complete_pending && g->notify_hwnd) user32_post(g->notify_hwnd, g->notify_msg, 0, g->notify_lp);
    return S_OK;
}

// AddSourceFilter(file, name, IBaseFilter**): building graphs from filters and
// pins isn't supported; report an unplayable file so the game skips it.
static uint32_t gb_addsource(uint32_t *a) {
    char name[512];
    w2a(G2H(a[1]), name, sizeof name);
    LOGI("dshow: AddSourceFilter('%s') not supported; skipping", name);
    if (a[3]) *(uint32_t *)G2H(a[3]) = 0;
    return 0x80040240u; // VFW_E_UNKNOWN_FILE_TYPE
}
static uint32_t gb_notfound(uint32_t *a) { return 0x80040216u; } // VFW_E_NOT_FOUND

static uint32_t vt_graph, vt_control, vt_event, vt_video, vt_seeking;

static void build(void) {
    if (vt_graph) return;
    ComMethod gb[18] = {{g_qi, 3}, {g_addref, 1}, {g_release, 1}, {ok, 3} /*AddFilter*/, {ok, 2} /*RemoveFilter*/,
                        {notimpl, 2} /*EnumFilters*/, {gb_notfound, 3} /*FindFilterByName*/, {ok, 4} /*ConnectDirect*/,
                        {ok, 2} /*Reconnect*/, {ok, 2} /*Disconnect*/, {ok, 1} /*SetDefaultSyncSource*/,
                        {ok, 3} /*Connect*/, {ok, 2} /*Render*/, {gb_renderfile, 3}, {gb_addsource, 4},
                        {ok, 2} /*SetLogFile*/, {ok, 1} /*Abort*/, {ok, 1} /*ShouldOperationContinue*/};
    vt_graph = com_vtable("IGraphBuilder", gb, 18);
    ComMethod mc[16] = {{g_qi, 3}, {g_addref, 1}, {g_release, 1}, {notimpl, 2}, {notimpl, 4}, {notimpl, 6},
                        {notimpl, 9}, {mc_run, 1}, {mc_pause, 1}, {mc_stop, 1}, {mc_getstate, 3},
                        {notimpl, 2}, {notimpl, 3}, {notimpl, 2}, {notimpl, 2}, {mc_stop, 1}};
    vt_control = com_vtable("IMediaControl", mc, 16);
    ComMethod me[16] = {{g_qi, 3}, {g_addref, 1}, {g_release, 1}, {notimpl, 2}, {notimpl, 4}, {notimpl, 6},
                        {notimpl, 9}, {get_zero, 2}, {me_getevent, 5}, {me_wait, 3}, {ok, 2}, {ok, 2},
                        {ok, 4}, {me_setnotify, 4}, {ok, 2}, {get_zero, 2}};
    vt_event = com_vtable("IMediaEventEx", me, 16);
    ComMethod vw[46] = {{g_qi, 3}, {g_addref, 1}, {g_release, 1}, {notimpl, 2}, {notimpl, 4}, {notimpl, 6},
                        {notimpl, 9}};
    for (int i = 7; i <= 32; i++) vw[i] = (ComMethod){(i & 1) ? ok : get_zero, 2}; // put_X / get_X pairs
    vw[33] = (ComMethod){get_zero, 2};
    vw[34] = (ComMethod){ok, 2};
    vw[35] = (ComMethod){get_zero, 2};
    vw[36] = (ComMethod){ok, 2};
    vw[37] = (ComMethod){ok, 2};
    vw[38] = (ComMethod){ok, 5};
    vw[39] = (ComMethod){ok, 5}; // SetWindowPosition
    vw[40] = (ComMethod){ok, 5};
    vw[41] = (ComMethod){ok, 3};
    vw[42] = (ComMethod){ok, 3};
    vw[43] = (ComMethod){ok, 5};
    vw[44] = (ComMethod){ok, 2};
    vw[45] = (ComMethod){get_zero, 2};
    vt_video = com_vtable("IVideoWindow", vw, 46);
    ComMethod ms[3] = {{g_qi, 3}, {g_addref, 1}, {g_release, 1}};
    ComMethod msf[20] = {0};
    memcpy(msf, ms, sizeof ms);
    vt_seeking = com_vtable("IMediaSeeking", msf, 20);
}

// Returns HRESULT, or 0xFFFFFFFF if the class isn't ours.
uint32_t dshow_create(const void *clsid, const void *iid, uint32_t *out) {
    static const uint8_t d4_fg[8] = {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
    if (!guid_eq(clsid, 0xe436ebb3, 0x524f, 0x11ce, d4_fg)) return 0xFFFFFFFF;
    build();
    Graph *g = calloc(1, sizeof *g);
    g->g_graph = com_new(vt_graph, g, 0);
    g->g_control = com_new(vt_control, g, 0);
    g->g_event = com_new(vt_event, g, 0);
    g->g_video = com_new(vt_video, g, 0);
    g->g_seeking = com_new(vt_seeking, g, 0);
    uint32_t args[3] = {g->g_graph, H2G(iid), H2G(out)};
    uint32_t hr = g_qi(args);
    if (hr != S_OK) free(g);
    return hr;
}
