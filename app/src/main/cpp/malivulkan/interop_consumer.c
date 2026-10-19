/* Isolated debug consumer, using the same platform SurfaceControl mechanism as
 * waylandcomp/src/sc_layer.c. It never starts that compositor or copies pixels. */
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <android/data_space.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "interop_consumer.h"
#define CLOG(...) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", __VA_ARGS__)
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *debug_window;
static uint64_t window_generation;
JNIEXPORT void JNICALL Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(JNIEnv *env, jclass type, jobject surface) {
    (void)type;
    ANativeWindow *next = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&window_lock); ANativeWindow *old = debug_window; debug_window = next; ++window_generation; pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}
/* C ABI declarations match sc_layer.c. The NDK SurfaceControl header contains
 * C++ references, and API-29 imports would break loading on the app's API-26 floor. */
typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;
typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
typedef void (*sc_complete)(void *, ASurfaceTransactionStats *);
struct consumer_api {
    ASurfaceControl *(*ASurfaceControl_createFromWindow)(ANativeWindow *, const char *);
    void (*ASurfaceControl_release)(ASurfaceControl *);
    ASurfaceTransaction *(*ASurfaceTransaction_create)(void);
    void (*ASurfaceTransaction_delete)(ASurfaceTransaction *);
    void (*ASurfaceTransaction_setBuffer)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int);
    void (*ASurfaceTransaction_setGeometry)(ASurfaceTransaction *, ASurfaceControl *, const ARect *, const ARect *, int32_t);
    void (*ASurfaceTransaction_setVisibility)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*ASurfaceTransaction_setBufferDataSpace)(ASurfaceTransaction *, ASurfaceControl *, int32_t);
    void (*ASurfaceTransaction_reparent)(ASurfaceTransaction *, ASurfaceControl *, ASurfaceControl *);
    void (*ASurfaceTransaction_setOnComplete)(ASurfaceTransaction *, void *, sc_complete);
    void (*ASurfaceTransaction_apply)(ASurfaceTransaction *);
    int (*ASurfaceTransactionStats_getPresentFenceFd)(ASurfaceTransactionStats *);
    int (*ASurfaceTransactionStats_getPreviousReleaseFenceFd)(ASurfaceTransactionStats *, ASurfaceControl *);
};
struct completion {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done, present, release;
    ASurfaceControl *control;
    struct consumer_api *api;
};
static void consumer_complete(void *context, ASurfaceTransactionStats *stats) {
    struct completion *c = context;
    int present = c->api->ASurfaceTransactionStats_getPresentFenceFd(stats);
    int release = c->api->ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, c->control);
    pthread_mutex_lock(&c->mutex); c->present = present; c->release = release; c->done = 1;
    pthread_cond_signal(&c->cond); pthread_mutex_unlock(&c->mutex);
}
static int wait_fd(int fd, int timeout) {
    if (fd < 0) return 0;
    struct pollfd p = {.fd = fd, .events = POLLIN}; int n;
    do { n = poll(&p, 1, timeout); } while (n < 0 && errno == EINTR);
    return n > 0 && (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL)) ? 0 : -1;
}
static void apply_wait(struct consumer_api *api, ASurfaceTransaction *t, struct completion *c) {
    c->done = 0; c->present = c->release = -1;
    api->ASurfaceTransaction_setOnComplete(t, c, consumer_complete);
    api->ASurfaceTransaction_apply(t); api->ASurfaceTransaction_delete(t);
    pthread_mutex_lock(&c->mutex);
    /* Must not destroy callback storage/AHB before completion. Same lifetime
     * constraint as sc_layer.c; surface destruction does not free these objects. */
    while (!c->done) pthread_cond_wait(&c->cond, &c->mutex);
    pthread_mutex_unlock(&c->mutex);
}
static int consumer_present(AHardwareBuffer *ahb, const AHardwareBuffer_Desc *desc, int producer_fd, unsigned preview_us) {
    /* Gate before acquisition even when the caller has not performed SYNC_WAIT. */
    if (wait_fd(producer_fd, 5000)) { CLOG("consumer blocked: producer sync not signaled"); return -1; }
    pthread_mutex_lock(&window_lock); ANativeWindow *window = debug_window;
    if (window) ANativeWindow_acquire(window);
    pthread_mutex_unlock(&window_lock);
    if (!window) { CLOG("AHB consumer unavailable: debug SurfaceView has no live native window"); return -1; }
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    struct consumer_api api = {0}; int result = -1;
    if (!library) goto window_out;
#define SC_LOAD(n) api.n = (__typeof__(api.n))dlsym(library, #n); if (!api.n) { CLOG("AHB presentation requires Android API 29 function " #n); goto library_out; }
    SC_LOAD(ASurfaceControl_createFromWindow)
    SC_LOAD(ASurfaceControl_release)
    SC_LOAD(ASurfaceTransaction_create)
    SC_LOAD(ASurfaceTransaction_delete)
    SC_LOAD(ASurfaceTransaction_setBuffer)
    SC_LOAD(ASurfaceTransaction_setGeometry)
    SC_LOAD(ASurfaceTransaction_setVisibility)
    SC_LOAD(ASurfaceTransaction_setBufferDataSpace)
    SC_LOAD(ASurfaceTransaction_reparent)
    SC_LOAD(ASurfaceTransaction_setOnComplete)
    SC_LOAD(ASurfaceTransaction_apply)
    SC_LOAD(ASurfaceTransactionStats_getPresentFenceFd)
    SC_LOAD(ASurfaceTransactionStats_getPreviousReleaseFenceFd)
#undef SC_LOAD
    /* Allocate retirement buffer BEFORE publishing; setBuffer never accepts NULL. */
    AHardwareBuffer_Desc blank_desc = {.width = 1, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    AHardwareBuffer *blank = NULL;
    if (AHardwareBuffer_allocate(&blank_desc, &blank)) goto library_out;
    ASurfaceControl *control = api.ASurfaceControl_createFromWindow(window, "DroidDeck Mali AHB diagnostic");
    if (!control) { AHardwareBuffer_release(blank); goto library_out; }
    struct completion c = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .control = control, .api = &api};
    AHardwareBuffer_acquire(ahb);
    int acquire = producer_fd >= 0 ? dup(producer_fd) : -1;
    if (producer_fd >= 0 && acquire < 0) goto control_out;
    ASurfaceTransaction *t = api.ASurfaceTransaction_create();
    if (!t) { if (acquire >= 0) close(acquire); goto control_out; }
    /* Platform takes ownership of acquire fd and its own reference to the exact AHB. */
    api.ASurfaceTransaction_setBuffer(t, control, ahb, acquire);
    ARect source = {0, 0, (int32_t)desc->width, (int32_t)desc->height};
    int32_t width = ANativeWindow_getWidth(window), height = ANativeWindow_getHeight(window);
    int32_t side = width < height ? width : height;
    if (side <= 0) side = 256;
    ARect destination = {0, 0, side, side};
    api.ASurfaceTransaction_setGeometry(t, control, &source, &destination, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    api.ASurfaceTransaction_setBufferDataSpace(t, control, ADATASPACE_SRGB);
    api.ASurfaceTransaction_setVisibility(t, control, 1 /* ASURFACE_TRANSACTION_VISIBILITY_SHOW */);
    apply_wait(&api, t, &c);
    result = wait_fd(c.present, 5000);
    CLOG("actual Vulkan AHB consumer transaction completed; present fence=%d result=%d", c.present, result);
    if (c.present < 0) CLOG("hardware present fence unavailable; SurfaceControl OnComplete confirms consumer completion");
    if (c.present >= 0) close(c.present);
    if (c.release >= 0) { (void)wait_fd(c.release, -1); close(c.release); }
    usleep(preview_us); /* Diagnostic preview, retained until real Android release. */
    t = api.ASurfaceTransaction_create();
    /* A transaction allocation failure cannot authorize releasing an active buffer. */
    while (!t) { usleep(10000); t = api.ASurfaceTransaction_create(); }
    api.ASurfaceTransaction_setBuffer(t, control, blank, -1);
    api.ASurfaceTransaction_setVisibility(t, control, 0 /* ASURFACE_TRANSACTION_VISIBILITY_HIDE */);
    api.ASurfaceTransaction_reparent(t, control, NULL);
    apply_wait(&api, t, &c);
    if (c.present >= 0) close(c.present);
    /* -1 means already released; otherwise wait before dropping our consumer ref. */
    if (c.release >= 0) {
        while (wait_fd(c.release, -1)) usleep(10000);
        close(c.release);
    }
    CLOG("AHB Android consumer released exact producer buffer; safe to destroy Vulkan allocation");
control_out:
    pthread_cond_destroy(&c.cond); pthread_mutex_destroy(&c.mutex);
    AHardwareBuffer_release(ahb); api.ASurfaceControl_release(control); AHardwareBuffer_release(blank);
library_out:
    dlclose(library);
window_out:
    ANativeWindow_release(window); return result;
}

int mb_consumer_present(AHardwareBuffer *ahb, const AHardwareBuffer_Desc *desc, int producer_fd) {
    return consumer_present(ahb, desc, producer_fd, 1000000);
}
int mb_consumer_present_renderer(AHardwareBuffer *ahb, const AHardwareBuffer_Desc *desc, int producer_fd) {
    return consumer_present(ahb, desc, producer_fd, 3000000);
}

/* Persistent counterpart: one SurfaceControl, one retained current producer.
 * apply_wait keeps callback storage alive, including during surface teardown. */
struct mb_consumer_session {
    struct consumer_api api;
    void *library;
    ANativeWindow *window;
    uint64_t generation;
    ASurfaceControl *control;
    AHardwareBuffer *blank, *current, *previous;
    struct completion completion;
    int closing, release_timeout;
};
struct mb_consumer_session *mb_consumer_session_open(void) {
    struct mb_consumer_session *s = calloc(1, sizeof(*s));
    if (!s) { CLOG("persistent consumer allocation failed errno=%d", errno); return NULL; }
    const char *stage = "live preview window";
    pthread_mutex_lock(&window_lock); s->window = debug_window; s->generation = window_generation;
    if (s->window) ANativeWindow_acquire(s->window);
    pthread_mutex_unlock(&window_lock);
    if (!s->window) goto fail;
    stage = "libandroid.so";
    s->library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!s->library) goto fail;
#define SESSION_LOAD(n) stage = #n; s->api.n = (__typeof__(s->api.n))dlsym(s->library, #n); if (!s->api.n) goto fail;
    SESSION_LOAD(ASurfaceControl_createFromWindow)
    SESSION_LOAD(ASurfaceControl_release)
    SESSION_LOAD(ASurfaceTransaction_create)
    SESSION_LOAD(ASurfaceTransaction_delete)
    SESSION_LOAD(ASurfaceTransaction_setBuffer)
    SESSION_LOAD(ASurfaceTransaction_setGeometry)
    SESSION_LOAD(ASurfaceTransaction_setVisibility)
    SESSION_LOAD(ASurfaceTransaction_setBufferDataSpace)
    SESSION_LOAD(ASurfaceTransaction_reparent)
    SESSION_LOAD(ASurfaceTransaction_setOnComplete)
    SESSION_LOAD(ASurfaceTransaction_apply)
    SESSION_LOAD(ASurfaceTransactionStats_getPresentFenceFd)
    SESSION_LOAD(ASurfaceTransactionStats_getPreviousReleaseFenceFd)
#undef SESSION_LOAD
    AHardwareBuffer_Desc d = {.width=1, .height=1, .layers=1, .format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    stage = "retirement AHardwareBuffer allocation";
    int ar = AHardwareBuffer_allocate(&d, &s->blank);
    if (ar) { CLOG("persistent retirement AHB Android result=%d", ar); goto fail; }
    stage = "SurfaceControl createFromWindow";
    s->control = s->api.ASurfaceControl_createFromWindow(s->window, "DroidDeck Mali persistent Gamescope");
    if (!s->control) goto fail;
    pthread_mutex_init(&s->completion.mutex, NULL);
    pthread_condattr_t attr; pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&s->completion.cond, &attr); pthread_condattr_destroy(&attr);
    s->completion.control = s->control; s->completion.api = &s->api;
    return s;
fail:
    CLOG("persistent consumer open failed stage=%s errno=%d", stage, errno);
    if (s->blank) AHardwareBuffer_release(s->blank);
    if (s->library) dlclose(s->library);
    if (s->window) ANativeWindow_release(s->window);
    free(s); return NULL;
}
/* Same budget on an in-flight replacement: a stop must not become stuck behind
 * a permanently missing previous-release callback/fence. Checkpoint 5 is unchanged. */
#ifndef MB_SESSION_RELEASE_TIMEOUT_MS
#define MB_SESSION_RELEASE_TIMEOUT_MS 5000
#endif
static int64_t session_now_ms(void);
static int session_release_wait(int, int64_t);
static int session_apply(struct mb_consumer_session *, ASurfaceTransaction *, int64_t);
static int session_completion(struct mb_consumer_session *s, ASurfaceTransaction *t) {
    int64_t deadline = session_now_ms() + MB_SESSION_RELEASE_TIMEOUT_MS;
    if (!session_apply(s, t, deadline)) {
        s->release_timeout = 1;
        CLOG("Android release wait TIMEOUT during swap: OnComplete=0; retaining current and previous AHBs/callback storage");
        return 2;
    }
    int result = session_release_wait(s->completion.present, deadline) ? 2 : 0;
    if (result) CLOG("persistent present fence timeout/error; result=VK_TIMEOUT");
    if (s->completion.present >= 0) { close(s->completion.present); s->completion.present = -1; }
    /* Even after a timeout, never grant reuse before Android's actual release.
     * Failure retains all callback/control/AHB objects in the native broker. */
    if (s->completion.release >= 0) {
        if (session_release_wait(s->completion.release, deadline)) {
            s->release_timeout = 1;
            CLOG("Android release wait TIMEOUT during swap: OnComplete=1 release fence=%d; retaining current and previous AHBs", s->completion.release);
            return 2;
        }
        close(s->completion.release); s->completion.release = -1;
    }
    return result;
}
int mb_consumer_session_present(struct mb_consumer_session *s, AHardwareBuffer *ahb, const AHardwareBuffer_Desc *d, int fd) {
    if (!s || !ahb || s->closing || s->release_timeout || s->current == ahb) return -3;
    pthread_mutex_lock(&window_lock); int visible = debug_window == s->window && s->generation == window_generation; pthread_mutex_unlock(&window_lock);
    if (!visible) { CLOG("persistent preview disappeared"); return -3; }
    if (wait_fd(fd, 5000)) { CLOG("persistent producer SYNC_FD timeout/error; result=VK_TIMEOUT"); return 2; }
    int acquire = fd >= 0 ? dup(fd) : -1;
    if (fd >= 0 && acquire < 0) { CLOG("persistent producer FD dup failed errno=%d", errno); return -1; }
    ASurfaceTransaction *t = s->api.ASurfaceTransaction_create();
    if (!t) { CLOG("persistent transaction allocation failed errno=%d", errno); if (acquire >= 0) close(acquire); return -1; }
    AHardwareBuffer_acquire(ahb);
    s->api.ASurfaceTransaction_setBuffer(t, s->control, ahb, acquire);
    ARect src = {0, 0, (int32_t)d->width, (int32_t)d->height};
    int32_t w = ANativeWindow_getWidth(s->window), h = ANativeWindow_getHeight(s->window), side = w < h ? w : h;
    if (side <= 0) side = 256;
    ARect dst = {0, 0, side, side};
    s->api.ASurfaceTransaction_setGeometry(t, s->control, &src, &dst, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    s->api.ASurfaceTransaction_setBufferDataSpace(t, s->control, ADATASPACE_SRGB);
    s->api.ASurfaceTransaction_setVisibility(t, s->control, 1);
    s->previous = s->current; s->current = ahb;
    int result = session_completion(s, t);
    if (!s->release_timeout && s->previous) { AHardwareBuffer_release(s->previous); s->previous = NULL; }
    return result;
}
int mb_consumer_session_owns(struct mb_consumer_session *s, AHardwareBuffer *ahb) {
    return s && ahb && (s->current == ahb || s->previous == ahb);
}
/* One total monotonic budget for transaction allocation, OnComplete and fences.
 * Test builds override this constant; production always uses five seconds. */
static int64_t session_now_ms(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int session_remaining_ms(int64_t deadline) {
    int64_t left = deadline - session_now_ms();
    return left > 0 ? (int)left : 0;
}
static int session_release_wait(int fd, int64_t deadline) {
    if (fd < 0) return 0;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int n;
    do { n = poll(&p, 1, session_remaining_ms(deadline)); }
    while (n < 0 && errno == EINTR && session_remaining_ms(deadline));
    return n > 0 && (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL)) ? 0 : -1;
}
static int session_apply(struct mb_consumer_session *s, ASurfaceTransaction *t, int64_t deadline) {
    struct completion *c = &s->completion;
    c->done = 0; c->present = c->release = -1;
    s->api.ASurfaceTransaction_setOnComplete(t, c, consumer_complete);
    s->api.ASurfaceTransaction_apply(t); s->api.ASurfaceTransaction_delete(t);
    pthread_mutex_lock(&c->mutex);
    struct timespec until = {.tv_sec = deadline / 1000, .tv_nsec = (deadline % 1000) * 1000000};
    while (!c->done && session_now_ms() < deadline)
        if (pthread_cond_timedwait(&c->cond, &c->mutex, &until)) break;
    int done = c->done;
    pthread_mutex_unlock(&c->mutex);
    return done;
}
int mb_consumer_session_close(struct mb_consumer_session *s) {
    if (!s) return 0;
    if (s->release_timeout) return 2; /* sticky failure, never restart the budget */
    s->closing = 1;
    int64_t deadline = session_now_ms() + MB_SESSION_RELEASE_TIMEOUT_MS;
    if (s->current) {
        s->completion.done = 0; s->completion.present = s->completion.release = -1;
        ASurfaceTransaction *t = s->api.ASurfaceTransaction_create();
        while (!t && session_remaining_ms(deadline)) { usleep(1000); t = s->api.ASurfaceTransaction_create(); }
        if (!t) goto timeout;
        s->api.ASurfaceTransaction_setBuffer(t, s->control, s->blank, -1);
        s->api.ASurfaceTransaction_setVisibility(t, s->control, 0);
        s->api.ASurfaceTransaction_reparent(t, s->control, NULL);
        struct completion *c = &s->completion; s->closing = 1;
        if (!session_apply(s, t, deadline)) goto timeout;
        if (session_release_wait(c->release, deadline)) goto timeout;
        if (c->release >= 0) { close(c->release); c->release = -1; }
        /* The release acknowledgement, rather than present-fence availability,
         * authorizes dropping the producer reference at stop. */
        if (c->present >= 0) { close(c->present); c->present = -1; }
        AHardwareBuffer_release(s->current);
    }
    pthread_cond_destroy(&s->completion.cond); pthread_mutex_destroy(&s->completion.mutex);
    s->api.ASurfaceControl_release(s->control); AHardwareBuffer_release(s->blank);
    ANativeWindow_release(s->window); dlclose(s->library); free(s);
    return 0;
timeout:
    s->release_timeout = 1;
    pthread_mutex_lock(&s->completion.mutex);
    CLOG("Android release wait TIMEOUT: OnComplete=%d stopping=%d release fence=%d present fence=%d; retaining callback storage, control and producer AHB",
        s->completion.done, s->closing, s->completion.release, s->completion.present);
    /* No callback can use these FDs once OnComplete has returned them. Keep the
     * release fence and all callback/AHB storage alive; close the unrelated FD. */
    if (s->completion.done && s->completion.present >= 0) { close(s->completion.present); s->completion.present = -1; }
    pthread_mutex_unlock(&s->completion.mutex);
    return 2; /* VK_TIMEOUT: caller must retain the consumer and native parents */
}
