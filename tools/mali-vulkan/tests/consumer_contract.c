/* Actual consumer implementation with asynchronous SurfaceControl/FD mocks.
 * There is no Vulkan model in this test: it verifies native consumer ownership. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void *consumer_dlopen(const char *, int);
static void *consumer_dlsym(void *, const char *);
static int consumer_dlclose(void *);
#define dlopen consumer_dlopen
#define dlsym consumer_dlsym
#define dlclose consumer_dlclose
#define MB_SESSION_RELEASE_TIMEOUT_MS 200 /* never sleep five seconds in tests */
#include "../../../app/src/main/cpp/malivulkan/interop_consumer.c"
#undef dlopen
#undef dlsym
#undef dlclose
struct AHardwareBuffer { atomic_int refs; AHardwareBuffer_Desc desc; };
struct ANativeWindow { atomic_int refs; };
struct ASurfaceControl { AHardwareBuffer *current; };
struct ASurfaceTransaction { ASurfaceControl *control; AHardwareBuffer *next; int fd; void *context; sc_complete callback; };
struct ASurfaceTransactionStats { int present, release; };
static atomic_int callbacks, acquisitions, retiring, released, allocations, frees, acquired_fds, drop_surface;
static int missing_api;
static AHardwareBuffer *producer;
static int persistent;
static int release_scenario;
static ANativeWindow window;
int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority; (void)tag; (void)format; return 0;
}
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) {
    AHardwareBuffer *a = calloc(1, sizeof(*a)); assert(a); a->refs = 1; a->desc = *d; *out = a; ++allocations; return 0;
}
void AHardwareBuffer_acquire(AHardwareBuffer *a) { assert(a && a->refs); ++a->refs; }
void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a && a->refs); if (!--a->refs) { assert(a != producer); ++frees; free(a); } }
void ANativeWindow_acquire(ANativeWindow *w) { assert(w == &window && w->refs); ++w->refs; }
void ANativeWindow_release(ANativeWindow *w) { assert(w == &window && w->refs); --w->refs; }
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface) { (void)env; assert(surface); ++window.refs; return &window; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) { assert(w == &window); return 512; }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { assert(w == &window); return 256; }
static ASurfaceControl *fn_ASurfaceControl_createFromWindow(ANativeWindow *w, const char *name) { assert(w->refs && name); return calloc(1, sizeof(ASurfaceControl)); }
static void fn_ASurfaceControl_release(ASurfaceControl *c) { assert(c); if (c->current) AHardwareBuffer_release(c->current); free(c); }
static ASurfaceTransaction *fn_ASurfaceTransaction_create(void) { if (release_scenario == 4) return NULL; ASurfaceTransaction *t = calloc(1, sizeof(*t)); assert(t); t->fd = -1; return t; }
static void fn_ASurfaceTransaction_delete(ASurfaceTransaction *t) { free(t); }
static void fn_ASurfaceTransaction_setBuffer(ASurfaceTransaction *t, ASurfaceControl *c, AHardwareBuffer *a, int fd) {
    assert(a && a->refs); t->control = c; t->next = a; t->fd = fd;
    if (a->desc.width == 256) { assert(a->refs == 2); ++acquisitions; }
    else { assert(c->current && c->current->refs == 3); ++retiring; }
}
static void fn_ASurfaceTransaction_setGeometry(ASurfaceTransaction *t, ASurfaceControl *c, const ARect *src, const ARect *dst, int32_t transform) {
    assert(t && c && !transform && src->right == 256 && src->bottom == 256 && dst->right == 256 && dst->bottom == 256);
}
static void fn_ASurfaceTransaction_setVisibility(ASurfaceTransaction *t, ASurfaceControl *c, int8_t visibility) { assert(t && c && (visibility == 0 || visibility == 1)); }
static void fn_ASurfaceTransaction_setBufferDataSpace(ASurfaceTransaction *t, ASurfaceControl *c, int32_t space) { assert(t && c && space == ADATASPACE_SRGB); }
static void fn_ASurfaceTransaction_reparent(ASurfaceTransaction *t, ASurfaceControl *c, ASurfaceControl *parent) { assert(t && c && !parent); }
static void fn_ASurfaceTransaction_setOnComplete(ASurfaceTransaction *t, void *context, sc_complete cb) { t->context = context; t->callback = cb; }
struct callback_job { ASurfaceTransaction t; };
static void *callback_worker(void *arg) {
    struct callback_job *job = arg; ASurfaceTransaction *t = &job->t;
    usleep(10000);
    if (t->fd >= 0) { assert(wait_fd(t->fd, 0) == 0); close(t->fd); ++acquired_fds; }
    AHardwareBuffer *old = t->control->current;
    AHardwareBuffer_acquire(t->next); t->control->current = t->next;
    struct ASurfaceTransactionStats stats = {.present = -1, .release = -1}; int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    if (old) { assert(persistent || old == producer); AHardwareBuffer_release(old); stats.release = pipe_fds[0]; }
    else stats.present = pipe_fds[0];
    if (!old && drop_surface) Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(NULL, NULL, NULL);
    if (old && release_scenario == 3) { free(job); return NULL; } /* no OnComplete */
    ++callbacks; t->callback(t->context, &stats);
    /* The completion callback ran but its fence is still unsignaled. Consumer
     * must retain the exact AHB until this thread signals previous release. */
    if (old && release_scenario == 2) { free(job); return NULL; } /* never signal release */
    usleep(old && release_scenario == 1 ? 70000 : 20000);
    if (old) { assert(old->refs == 2); ++released; }
    assert(write(pipe_fds[1], "x", 1) == 1); close(pipe_fds[1]);
    free(job); return NULL;
}
static void fn_ASurfaceTransaction_apply(ASurfaceTransaction *t) {
    struct callback_job *job = malloc(sizeof(*job)); assert(job); job->t = *t;
    pthread_t thread_id; assert(pthread_create(&thread_id, NULL, callback_worker, job) == 0); assert(pthread_detach(thread_id) == 0);
}
static int fn_ASurfaceTransactionStats_getPresentFenceFd(ASurfaceTransactionStats *s) { return s->present; }
static int fn_ASurfaceTransactionStats_getPreviousReleaseFenceFd(ASurfaceTransactionStats *s, ASurfaceControl *c) { assert(c); return s->release; }
static void *consumer_dlopen(const char *name, int flags) { (void)flags; assert(!strcmp(name, "libandroid.so")); return (void *)1; }
static int consumer_dlclose(void *p) { assert(p); return 0; }
static void *consumer_dlsym(void *p, const char *name) {
    assert(p); if (missing_api) return NULL;
#define FN(n) if (!strcmp(name, #n)) return (void *)fn_##n;
    FN(ASurfaceControl_createFromWindow) FN(ASurfaceControl_release)
    FN(ASurfaceTransaction_create) FN(ASurfaceTransaction_delete) FN(ASurfaceTransaction_setBuffer)
    FN(ASurfaceTransaction_setGeometry) FN(ASurfaceTransaction_setVisibility) FN(ASurfaceTransaction_setBufferDataSpace)
    FN(ASurfaceTransaction_reparent) FN(ASurfaceTransaction_setOnComplete) FN(ASurfaceTransaction_apply)
    FN(ASurfaceTransactionStats_getPresentFenceFd) FN(ASurfaceTransactionStats_getPreviousReleaseFenceFd)
#undef FN
    return NULL;
}
static int fd_count(void) { DIR *d = opendir("/proc/self/fd"); assert(d); int n = 0; while (readdir(d)) ++n; closedir(d); return n; }
static void *signal_producer(void *arg) { int fd = *(int *)arg; usleep(50000); assert(!acquisitions); assert(write(fd, "x", 1) == 1); return NULL; }
static int release_wait_contract(const char *name) {
    int scenario = !strcmp(name, "delayed") ? 1 : !strcmp(name, "missing") || !strcmp(name, "swap") ? 2 : !strcmp(name, "callback") ? 3 : !strcmp(name, "allocation") ? 4 : 0;
    AHardwareBuffer_Desc desc = {.width=256, .height=256, .layers=1, .format=1};
    assert(AHardwareBuffer_allocate(&desc, &producer) == 0);
    AHardwareBuffer *other; assert(AHardwareBuffer_allocate(&desc, &other) == 0);
    persistent = 1;
    Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(NULL, NULL, (jobject)1);
    struct mb_consumer_session *session = mb_consumer_session_open(); assert(session);
    assert(mb_consumer_session_present(session, producer, &desc, -1) == 0);
    int released_before = released, acquired_before = acquisitions;
    release_scenario = scenario;
    if (!strcmp(name, "swap")) {
        int64_t begin = session_now_ms();
        assert(mb_consumer_session_present(session, other, &desc, -1) == 2);
        assert(session_now_ms() - begin < 1000 && session->release_timeout);
        assert(mb_consumer_session_owns(session, producer) && mb_consumer_session_owns(session, other));
        assert(producer->refs >= 2 && other->refs >= 2 && released == released_before);
        assert(mb_consumer_session_close(session) == 2);
        assert(mb_consumer_session_present(session, producer, &desc, -1) == -3);
        puts("swap timeout: both AHBs retained; no release, no reuse; bounded stop");
        return 0;
    }
    int64_t begin = session_now_ms();
    int result = mb_consumer_session_close(session);
    assert(session_now_ms() - begin < 1000);
    if (scenario < 2) {
        assert(result == 0 && producer->refs == 1 && released == released_before + 1);
        puts("Android release wait: outstanding at stop=1 released=1 timed out=0");
    } else {
        assert(result == 2 && session->release_timeout && session->current == producer);
        assert(producer->refs >= 2 && released == released_before);
        assert(mb_consumer_session_present(session, other, &desc, -1) == -3);
        assert(acquisitions == acquired_before); /* no frame submitted after stop */
        begin = session_now_ms();
        assert(mb_consumer_session_close(session) == 2 && session_now_ms() - begin < 50);
        assert(producer->refs >= 2 && released == released_before);
        puts("Android release wait TIMEOUT: retained producer, no reuse, no fake release; sticky failure");
    }
    return 0; /* test process exit reclaims intentionally retained mock parents */
}
int main(int argc, char **argv) {
    if (argc == 2) return release_wait_contract(argv[1]);
    AHardwareBuffer_Desc desc = {.width = 256, .height = 256, .layers = 1, .format = 1, .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
    assert(AHardwareBuffer_allocate(&desc, &producer) == 0);
    assert(mb_consumer_present(producer, &desc, -1) == -1); /* no consumer window */
    Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(NULL, NULL, (jobject)1);
    missing_api = 1; assert(mb_consumer_present(producer, &desc, -1) == -1); missing_api = 0;
    int before = fd_count(), p[2]; assert(pipe(p) == 0); pthread_t signal;
    assert(pthread_create(&signal, NULL, signal_producer, &p[1]) == 0);
    assert(mb_consumer_present(producer, &desc, p[0]) == 0); assert(pthread_join(signal, NULL) == 0);
    assert(producer->refs == 1 && window.refs == 1); close(p[0]); close(p[1]);
    assert(fd_count() == before);
    for (unsigned i = 0; i < 2; ++i) { assert(mb_consumer_present(producer, &desc, -1) == 0); assert(producer->refs == 1 && window.refs == 1); }
    drop_surface = 1; assert(mb_consumer_present(producer, &desc, -1) == 0);
    assert(producer->refs == 1 && !window.refs && acquisitions == 4 && released == 4 && callbacks == 8 && retiring == 4 && acquired_fds == 1);
    assert(fd_count() == before && allocations == frees + 1);
    persistent = 1; drop_surface = 0;
    Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(NULL, NULL, (jobject)1);
    AHardwareBuffer *other = NULL; assert(AHardwareBuffer_allocate(&desc, &other) == 0);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        struct mb_consumer_session *session = mb_consumer_session_open(); assert(session);
        for (unsigned frame = 0; frame < 12; ++frame) {
            AHardwareBuffer *next = frame % 2 ? other : producer;
            assert(mb_consumer_session_present(session, next, &desc, -1) == 0);
            assert(next->refs == 3);
            assert(mb_consumer_session_present(session, next, &desc, -1) == -3);
            assert((frame % 2 ? producer : other)->refs == 1);
        }
        assert(mb_consumer_session_close(session) == 0);
        assert(producer->refs == 1 && other->refs == 1 && window.refs == 1);
        assert(fd_count() == before);
    }
    struct mb_consumer_session *session = mb_consumer_session_open(); assert(session);
    assert(mb_consumer_session_present(session, producer, &desc, -1) == 0);
    Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeSetSurface(NULL, NULL, NULL);
    assert(mb_consumer_session_present(session, other, &desc, -1) == -3);
    assert(mb_consumer_session_close(session) == 0);
    assert(producer->refs == 1 && other->refs == 1 && !window.refs);
    AHardwareBuffer_release(other);
    assert(fd_count() == before && allocations == frees + 1);
    puts("persistent consumer: repeated swaps, same-buffer rejection, delayed release, preview loss, repeated stop, FD baseline PASS");
    AHardwareBuffer *owned = producer; producer = NULL; AHardwareBuffer_release(owned);
    assert(allocations == frees);
    puts("consumer: exact buffer, unsignaled producer wait, asynchronous present/release fences, repeated cycles, surface teardown and FD/reference balance PASS");
    return 0;
}
