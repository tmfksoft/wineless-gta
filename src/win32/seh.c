// Structured exception handling for software exceptions: RaiseException walks
// the guest's fs:[0] handler chain like Windows; RtlUnwind runs the unwind
// pass. (Hardware faults are not translated into SEH yet.)
#include <stdlib.h>

#include "win32.h"

#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2
#define EXCEPTION_EXIT_UNWIND 0x4
#define CONTEXT_FULL 0x10007
#define REC_SIZE 80
#define CTX_SIZE 716
#define SCRATCH (REC_SIZE + CTX_SIZE + 64)

uint32_t k32_unhandled_filter(void);

// Reserve room below the current guest stack for exception records, and make
// nested call32()s start below it.
static uint32_t scratch_push(GuestCtx *ctx, uint32_t *saved) {
    *saved = ctx->guest_esp;
    uint32_t area = (ctx->guest_esp - 0x200 - SCRATCH) & ~15u;
    ctx->guest_esp = area - 16;
    memset(G2H(area), 0, SCRATCH);
    return area;
}

static void fill_context(GuestCtx *ctx, uint32_t c, uint32_t eip, uint32_t esp) {
    uint32_t *r = G2H(c);
    r[0] = CONTEXT_FULL;
    r[0x9c / 4] = ctx->g_edi;
    r[0xa0 / 4] = ctx->g_esi;
    r[0xa4 / 4] = ctx->g_ebx;
    r[0xb4 / 4] = ctx->g_ebp;
    r[0xb8 / 4] = eip;
    r[0xbc / 4] = 0x23;
    r[0xc0 / 4] = 0x202;
    r[0xc4 / 4] = esp;
    r[0xc8 / 4] = 0x2b;
    r[0x98 / 4] = r[0x94 / 4] = 0x2b;
    r[0x90 / 4] = ctx->fs_sel;
}

// Returns 1 if a handler asked to continue execution.
static int dispatch(GuestCtx *ctx, uint32_t rec, uint32_t context) {
    uint32_t teb = ctx->teb;
    uint32_t frame = *(uint32_t *)G2H(teb);
    int guard = 0;
    while (frame != 0xFFFFFFFF && frame && guard++ < 1000) {
        if (!lm_is_committed(frame, 8, false)) break;
        uint32_t handler = ((uint32_t *)G2H(frame))[1];
        uint32_t args[4] = {rec, frame, context, 0};
        LOGD("seh: frame 0x%x handler 0x%x", frame, handler);
        uint32_t r = (uint32_t)call32(ctx, handler, 4, args);
        if (r == 0) return 1;              // ExceptionContinueExecution
        frame = *(uint32_t *)G2H(frame);   // ContinueSearch / nested
    }
    return 0;
}

API(kernel32, RaiseException, 4) {
    GuestCtx *ctx = thread_current()->ctx;
    uint32_t *sp = G2H(ctx->guest_esp); // [id][ret][args]
    uint32_t ret = sp[1], caller_esp = ctx->guest_esp + 8 + 16;
    uint32_t saved;
    uint32_t area = scratch_push(ctx, &saved);
    uint32_t rec = area, context = area + REC_SIZE + 16;
    uint32_t *r = G2H(rec);
    r[0] = ARG(0);
    r[1] = ARG(1) & EXCEPTION_NONCONTINUABLE;
    r[3] = ret;
    uint32_t n = ARG(2) > 15 ? 15 : ARG(2);
    r[4] = n;
    for (uint32_t i = 0; i < n && ARG(3); i++) r[5 + i] = ((uint32_t *)PTR(3))[i];
    fill_context(ctx, context, ret, caller_esp);
    LOGD("RaiseException(0x%08x) from 0x%x", ARG(0), ret);
    // MSVC C++ throw: params = magic, object, ThrowInfo -> name the thrown type
    if (ARG(0) == 0xE06D7363 && n >= 3 && g_log_level >= LOG_DEBUG) {
        uint32_t obj = r[6], ti = r[7];
        const char *type = "?";
        if (ti && lm_is_committed(ti, 16, false)) {
            uint32_t cta = ((uint32_t *)G2H(ti))[3];
            if (cta && lm_is_committed(cta, 8, false) && ((uint32_t *)G2H(cta))[0]) {
                uint32_t ct = ((uint32_t *)G2H(cta))[1];
                uint32_t td = ct && lm_is_committed(ct, 8, false) ? ((uint32_t *)G2H(ct))[1] : 0;
                if (td && lm_is_committed(td, 12, false)) type = (const char *)G2H(td + 8);
            }
        }
        // CryptoPP::Exception (MSVC): vptr, int errorType, std::string m_what
        // (buf[16] @8, size @0x18, capacity @0x1C; heap ptr when capacity >= 16).
        char msg[256] = "";
        if (obj && lm_is_committed(obj, 0x20, false)) {
            uint32_t *o = G2H(obj);
            uint32_t size = o[6], cap = o[7], src = cap >= 16 ? o[2] : obj + 8;
            if (size < sizeof msg && lm_is_committed(src, size, false)) {
                memcpy(msg, G2H(src), size);
                msg[size] = 0;
            }
        }
        LOGD("  C++ exception of type %s: \"%s\"", type, msg);
    }

    if (dispatch(ctx, rec, context) && !(r[1] & EXCEPTION_NONCONTINUABLE)) {
        ctx->guest_esp = saved;
        return 0;
    }
    // Nobody handled it: the unhandled exception filter, then terminate.
    uint32_t filter = k32_unhandled_filter();
    if (filter) {
        uint32_t ptrs = area + REC_SIZE; // EXCEPTION_POINTERS {rec, ctx}
        ((uint32_t *)G2H(ptrs))[0] = rec;
        ((uint32_t *)G2H(ptrs))[1] = context;
        uint32_t a1[1] = {ptrs};
        int32_t fr = (int32_t)(uint32_t)call32(ctx, filter, 1, a1);
        if (fr == -1) { ctx->guest_esp = saved; return 0; } // EXCEPTION_CONTINUE_EXECUTION
    }
    LOGE("unhandled exception 0x%08x raised at 0x%08x; terminating", ARG(0), ret);
    win32_process_exit(0xC0000000u | 0x409);
}

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue)
API(kernel32, RtlUnwind, 4) {
    GuestCtx *ctx = thread_current()->ctx;
    uint32_t *sp = G2H(ctx->guest_esp);
    uint32_t ret = sp[1], target = ARG(0);
    uint32_t saved;
    uint32_t area = scratch_push(ctx, &saved);
    uint32_t rec = ARG(2), context = area + REC_SIZE + 16;
    if (!rec) {
        rec = area;
        ((uint32_t *)G2H(rec))[0] = 0xC0000027; // STATUS_UNWIND
        ((uint32_t *)G2H(rec))[3] = ret;
    }
    ((uint32_t *)G2H(rec))[1] |= EXCEPTION_UNWINDING | (target ? 0 : EXCEPTION_EXIT_UNWIND);
    fill_context(ctx, context, ret, ctx->guest_esp + 8 + 16);
    uint32_t *teb0 = G2H(ctx->teb);
    uint32_t frame = *teb0;
    int guard = 0;
    while (frame != 0xFFFFFFFF && frame && frame != target && guard++ < 1000) {
        if (!lm_is_committed(frame, 8, false)) break;
        uint32_t handler = ((uint32_t *)G2H(frame))[1];
        uint32_t args[4] = {rec, frame, context, 0};
        LOGD("seh: unwinding frame 0x%x (handler 0x%x)", frame, handler);
        call32(ctx, handler, 4, args);
        frame = *(uint32_t *)G2H(frame);
        *teb0 = frame; // pop
    }
    ctx->guest_esp = saved;
    return ARG(3);
}
