#define _POSIX_C_SOURCE 200809L
#include "normal_ownership.h"
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define NORMAL_MAX_BUFFERS 64
static pthread_mutex_t normal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t normal_changed = PTHREAD_COND_INITIALIZER;
static uint32_t normal_sequence;
static struct record { struct mb_normal_state state; AHardwareBuffer *buffer; } records[NORMAL_MAX_BUFFERS];
static uint64_t normal_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static struct record *find(uint32_t key) {
    if (!key) return NULL;
    for (unsigned i = 0; i < NORMAL_MAX_BUFFERS; ++i) if (records[i].state.key == key) return &records[i];
    return NULL;
}
static void collect(struct record *r) {
    if (r && r->state.retired == 2 && !r->state.owned && !r->state.consumers) {
        AHardwareBuffer_release(r->buffer); memset(r, 0, sizeof(*r));
    }
}
uint32_t mb_normal_register(AHardwareBuffer *buffer) {
    if (!buffer) return 0;
    pthread_mutex_lock(&normal_lock);
    uint32_t key = 0;
    if (normal_sequence != UINT32_MAX) for (unsigned i = 0; i < NORMAL_MAX_BUFFERS; ++i) if (!records[i].state.key) {
        key = ++normal_sequence; records[i].state.key = key; records[i].buffer = buffer;
        AHardwareBuffer_acquire(buffer); break;
    }
    pthread_mutex_unlock(&normal_lock); return key;
}
AHardwareBuffer *mb_normal_import(uint32_t key, uint32_t width, uint32_t height) {
    pthread_mutex_lock(&normal_lock);
    struct record *r = find(key); AHardwareBuffer *buffer = NULL;
    if (r && !r->state.retired && !r->state.consumers) {
        AHardwareBuffer_Desc desc; AHardwareBuffer_describe(r->buffer, &desc);
        if (desc.width == width && desc.height == height) {
            buffer = r->buffer; ++r->state.consumers; AHardwareBuffer_acquire(buffer);
        }
    }
    pthread_mutex_unlock(&normal_lock); return buffer;
}
int mb_normal_publish(uint32_t key, uint32_t frame, uint32_t sync, uint32_t fence) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    int ok = r && !r->state.retired && r->state.consumers && !r->state.owned && frame && sync && fence;
    if (ok) { r->state.owned = 1; r->state.submitted = 0; r->state.frame = frame; r->state.sync = sync; r->state.fence = fence; r->state.published_ns = normal_now(); }
    pthread_mutex_unlock(&normal_lock); return ok;
}
int mb_normal_submit(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    /* Re-presenting the currently held image is legal; never permits a producer write. */
    /* Retirement stops the producer. Already queued commits may still reach Android
     * while the broker waits for their real release. */
    int ok = r && r->state.retired < 2 && r->state.owned && r->state.consumers;
    if (ok && !r->state.submitted) {
        r->state.submitted = 1; ++r->state.presented; r->state.submitted_ns = normal_now();
        r->state.timing.queue_ns += r->state.submitted_ns - r->state.published_ns;
    }
    pthread_mutex_unlock(&normal_lock); return ok;
}
void mb_normal_present_timing(uint32_t key, uint64_t ns) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r) { ++r->state.timing.present_calls; r->state.timing.present_ns += ns; }
    pthread_mutex_unlock(&normal_lock);
}
void mb_normal_release(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r && r->state.owned && r->state.submitted) {
        r->state.timing.held_ns += normal_now() - r->state.submitted_ns;
        r->state.owned = 0; r->state.submitted = 0; ++r->state.released; collect(r); pthread_cond_broadcast(&normal_changed);
    }
    pthread_mutex_unlock(&normal_lock);
}
void mb_normal_not_submitted(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r && !r->state.submitted) { r->state.owned = 0; collect(r); pthread_cond_broadcast(&normal_changed); }
    pthread_mutex_unlock(&normal_lock);
}
void mb_normal_detach(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r) {
        if (r->state.consumers) --r->state.consumers;
        /* Resource destruction proves an unpublished commit cannot reach Android.
         * A submitted allocation still requires its actual release callback. */
        if (!r->state.submitted) r->state.owned = 0;
        collect(r); pthread_cond_broadcast(&normal_changed);
    }
    pthread_mutex_unlock(&normal_lock);
}
int mb_normal_owned(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key); int held = r && r->state.owned;
    pthread_mutex_unlock(&normal_lock); return held;
}
int mb_normal_snapshot(uint32_t key, struct mb_normal_state *state) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r) *state = r->state; else memset(state, 0, sizeof(*state));
    pthread_mutex_unlock(&normal_lock); return !!r;
}
void mb_normal_retire(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r) { r->state.retired = 1; collect(r); }
    pthread_mutex_unlock(&normal_lock);
}
void mb_normal_forget(uint32_t key) {
    pthread_mutex_lock(&normal_lock); struct record *r = find(key);
    if (r) { r->state.retired = 2; collect(r); }
    pthread_mutex_unlock(&normal_lock);
}
int mb_normal_wait(const uint32_t *keys, unsigned count, unsigned timeout_ms) {
    /* A monotonic deadline bounds the entire wait, including EINTR/spurious wakeups.
     * Short realtime condition waits keep wall-clock adjustments from extending it. */
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    int64_t deadline = (int64_t)start.tv_sec * 1000000000LL + start.tv_nsec + (int64_t)timeout_ms * 1000000LL;
    pthread_mutex_lock(&normal_lock);
    for (;;) {
        int held = 0;
        for (unsigned i = 0; i < count; ++i) { struct record *r = find(keys[i]); held |= r && r->state.owned; }
        if (!held) { pthread_mutex_unlock(&normal_lock); return 1; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t left = deadline - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
        if (left <= 0) { pthread_mutex_unlock(&normal_lock); return 0; }
        if (left > 10000000LL) left = 10000000LL;
        clock_gettime(CLOCK_REALTIME, &now); now.tv_nsec += left;
        now.tv_sec += now.tv_nsec / 1000000000LL; now.tv_nsec %= 1000000000LL;
        pthread_cond_timedwait(&normal_changed, &normal_lock, &now);
    }
}
