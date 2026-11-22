// vorbisfile.dll replacement backed by a static libvorbisfile.
//
// The game's OggVorbis_File is treated as opaque storage: we keep the real
// (64-bit) decoder state on the host and store a handle in the guest struct.
// Its I/O callbacks are 32-bit game functions, so libvorbisfile's reads go
// through call32() into a guest bounce buffer.
#include <pthread.h>
#include <stdlib.h>
#include <vorbis/vorbisfile.h>

#include "../heap.h"
#include "win32.h"

#define VF_MAGIC 0x5641574Fu // 'OWAV'
#define BOUNCE 65536
#define MAX_VF 256

typedef struct {
    OggVorbis_File ovf;
    uint32_t ds;                       // guest datasource
    uint32_t cb_read, cb_seek, cb_close, cb_tell;
    uint32_t bounce;                   // guest buffer for reads
    uint32_t info;                     // guest vorbis_info copy
    int used;
} VF;

static VF *vfs_tab[MAX_VF];
static pthread_mutex_t vf_lock = PTHREAD_MUTEX_INITIALIZER;

static GuestCtx *ctx(void) { return thread_current()->ctx; }

static size_t h_read(void *ptr, size_t size, size_t nmemb, void *dsv) {
    VF *v = dsv;
    size_t want = size * nmemb, got = 0;
    while (got < want) {
        uint32_t chunk = want - got > BOUNCE ? BOUNCE : (uint32_t)(want - got);
        uint32_t args[4] = {v->bounce, 1, chunk, v->ds};
        uint32_t n = (uint32_t)call32(ctx(), v->cb_read, 4, args);
        if (n > chunk) n = chunk;
        memcpy((char *)ptr + got, G2H(v->bounce), n);
        got += n;
        if (n < chunk) break;
    }
    return size ? got / size : 0;
}
static int h_seek(void *dsv, ogg_int64_t off, int whence) {
    VF *v = dsv;
    uint32_t args[4] = {v->ds, (uint32_t)off, (uint32_t)((uint64_t)off >> 32), (uint32_t)whence};
    return (int32_t)(uint32_t)call32(ctx(), v->cb_seek, 4, args);
}
static int h_close(void *dsv) {
    VF *v = dsv;
    uint32_t args[1] = {v->ds};
    return (int32_t)(uint32_t)call32(ctx(), v->cb_close, 1, args);
}
static long h_tell(void *dsv) {
    VF *v = dsv;
    uint32_t args[1] = {v->ds};
    return (long)(int32_t)(uint32_t)call32(ctx(), v->cb_tell, 1, args);
}

static VF *get(uint32_t gvf) {
    uint32_t *g = G2H(gvf);
    if (!gvf || g[0] != VF_MAGIC || g[1] >= MAX_VF) return NULL;
    return vfs_tab[g[1]];
}

// int ov_open_callbacks(void *ds, OggVorbis_File *vf, const char *initial, long ibytes, ov_callbacks cb)
API_C(vorbisfile, ov_open_callbacks, 8) {
    VF *v = calloc(1, sizeof *v);
    v->ds = ARG(0);
    v->cb_read = ARG(4);
    v->cb_seek = ARG(5);
    v->cb_close = ARG(6);
    v->cb_tell = ARG(7);
    v->bounce = gheap_alloc(BOUNCE, 0);
    ov_callbacks cb = {h_read, v->cb_seek ? h_seek : NULL, v->cb_close ? h_close : NULL, v->cb_tell ? h_tell : NULL};
    int r = ov_open_callbacks(v, &v->ovf, ARG(2) ? (const char *)PTR(2) : NULL, (int32_t)ARG(3), cb);
    if (r < 0) {
        LOGD("ov_open_callbacks failed: %d", r);
        gheap_free(v->bounce);
        free(v);
        return (uint32_t)r;
    }
    pthread_mutex_lock(&vf_lock);
    int idx = -1;
    for (int i = 0; i < MAX_VF; i++)
        if (!vfs_tab[i]) { idx = i; break; }
    if (idx >= 0) vfs_tab[idx] = v;
    pthread_mutex_unlock(&vf_lock);
    if (idx < 0) {
        ov_clear(&v->ovf);
        gheap_free(v->bounce);
        free(v);
        return (uint32_t)OV_EFAULT;
    }
    uint32_t *g = PTR(1);
    g[0] = VF_MAGIC;
    g[1] = idx;
    return 0;
}

API_C(vorbisfile, ov_clear, 1) {
    VF *v = get(ARG(0));
    if (!v) return (uint32_t)-1;
    ov_clear(&v->ovf); // calls the guest close callback
    uint32_t *g = PTR(0);
    pthread_mutex_lock(&vf_lock);
    vfs_tab[g[1]] = NULL;
    pthread_mutex_unlock(&vf_lock);
    g[0] = 0;
    gheap_free(v->bounce);
    if (v->info) gheap_free(v->info);
    free(v);
    return 0;
}

// vorbis_info* ov_info(vf, link) -> 32-bit copy {version, channels, rate, upper, nominal, lower, window, codec_setup}
API_C(vorbisfile, ov_info, 2) {
    VF *v = get(ARG(0));
    if (!v) return 0;
    vorbis_info *vi = ov_info(&v->ovf, (int32_t)ARG(1));
    if (!vi) return 0;
    if (!v->info) v->info = gheap_alloc(32, 1);
    int32_t *o = G2H(v->info);
    o[0] = vi->version;
    o[1] = vi->channels;
    o[2] = (int32_t)vi->rate;
    o[3] = (int32_t)vi->bitrate_upper;
    o[4] = (int32_t)vi->bitrate_nominal;
    o[5] = (int32_t)vi->bitrate_lower;
    o[6] = (int32_t)vi->bitrate_window;
    o[7] = 0;
    return v->info;
}

// long ov_read(vf, char *buffer, int length, int bigendianp, int word, int sgned, int *bitstream)
API_C(vorbisfile, ov_read, 7) {
    VF *v = get(ARG(0));
    if (!v) return (uint32_t)OV_EINVAL;
    int bs = 0;
    long r = ov_read(&v->ovf, PTR(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), &bs);
    if (ARG(6)) *(int32_t *)PTR(6) = bs;
    return (uint32_t)(int32_t)r;
}

API_C(vorbisfile, ov_time_total, 2) {
    VF *v = get(ARG(0));
    api_ret_double(v ? ov_time_total(&v->ovf, (int32_t)ARG(1)) : -1.0);
    return 0;
}
API_C(vorbisfile, ov_time_tell, 1) {
    VF *v = get(ARG(0));
    api_ret_double(v ? ov_time_tell(&v->ovf) : -1.0);
    return 0;
}
// int ov_time_seek(vf, double s): the double takes two stack slots
API_C(vorbisfile, ov_time_seek, 3) {
    VF *v = get(ARG(0));
    if (!v) return (uint32_t)OV_EINVAL;
    double s;
    uint64_t bits = (uint64_t)ARG(1) | ((uint64_t)ARG(2) << 32);
    memcpy(&s, &bits, 8);
    return (uint32_t)ov_time_seek(&v->ovf, s);
}
