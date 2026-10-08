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
#include "interop_consumer.h"
#define CLOG(...) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", __VA_ARGS__)
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *debug_window;
JNIEXPORT void JNICALL Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(JNIEnv *env, jclass type, jobject surface) {
    (void)type;
    ANativeWindow *next = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&window_lock); ANativeWindow *old = debug_window; debug_window = next; pthread_mutex_unlock(&window_lock);
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
int mb_consumer_present(AHardwareBuffer *ahb, const AHardwareBuffer_Desc *desc, int producer_fd) {
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
    usleep(1000000); /* Visible quadrants; not a normal compositor/session. */
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
