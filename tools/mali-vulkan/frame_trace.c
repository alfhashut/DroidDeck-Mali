/* Compiled once into maliahb, and once into the normal Gamescope TU.
 * Armed at startup only. A worker handles control/output off the hot path.
 * Records stop at capacity; never overwrite and silently lose frame identity.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "frame_trace.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef DD_FT_ROOT
#define DD_FT_ROOT "/storage/emulated/0/Download/DroidDeckFrameTrace"
#endif
#ifndef DD_FT_CAPACITY
#define DD_FT_CAPACITY 16384u
#endif
struct dd_ft_record {
    uint64_t wall, cpu, object, value;
    struct dd_ft_id id;
    uint32_t tid, stage;
    int32_t status;
};
static const char *dd_ft_names[] = {
    "client_commit", "client_select", "client_coalesced", "client_discarded",
    "frame_begin", "frame_end", "shm_import_begin", "shm_import_end",
    "ahb_acquire_begin", "ahb_acquire_end", "render_begin", "render_end",
    "wait_begin", "wait_end", "vk_submit_begin", "vk_submit_end",
    "publish", "outer_commit", "outer_release", "output_pool_blocked",
    "android_handoff_begin", "android_handoff_end", "android_submit", "android_repeat",
    "release_callback", "release_queued", "release_fence_pending",
    "release_fence_observed", "pool_return", "cancel_unsubmitted",
    "transaction_complete", "android_latch_time", "shm_copy_begin", "shm_copy_end",
    "shm_lookup_begin", "shm_lookup_end", "shm_source_begin", "shm_source_end",
    "staging_acquire_begin", "staging_acquire_end", "staging_create_begin", "staging_create_end",
    "staging_map_begin", "staging_map_end", "staging_validate_begin", "staging_validate_end",
    "shm_source_release_begin", "shm_source_release_end", "texture_create_begin", "texture_create_end",
    "upload_record_begin", "upload_record_end", "upload_prepare_begin", "upload_prepare_end",
    "upload_submit_begin", "upload_submit_end", "upload_finalize_begin", "upload_finalize_end",
    "upload_queue_begin", "upload_queue_end", "upload_wait_begin", "upload_wait_end",
    "shm_cleanup_begin", "shm_cleanup_end"
};
static pthread_mutex_t dd_ft_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t dd_ft_thread;
static struct dd_ft_record *dd_ft_records;
static unsigned dd_ft_used, dd_ft_enabled, dd_ft_running, dd_ft_stop, dd_ft_dropped;
static uint64_t dd_ft_run, dd_ft_start, dd_ft_end, dd_ft_last_run, dd_ft_ready_ns;
static unsigned dd_ft_cpu_stride = 8;
static const char *dd_ft_role;
static __thread uint32_t dd_ft_tid;
static uint64_t dd_ft_scope_sequence;
uint64_t dd_ft_new_scope(void) { return __atomic_add_fetch(&dd_ft_scope_sequence, 1, __ATOMIC_RELAXED); }
static uint64_t dd_ft_now(clockid_t clock) {
    struct timespec t;
    return clock_gettime(clock, &t) ? 0 : (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
#define DD_FT_ON (1u << 31)
int dd_ft_active(void) { return (__atomic_load_n(&dd_ft_enabled, __ATOMIC_ACQUIRE) & DD_FT_ON) != 0; }
void dd_ft_event(enum dd_ft_stage stage, struct dd_ft_id id,
                 uint64_t object, uint64_t value, int32_t status, int sample_cpu) {
    unsigned gate = __atomic_load_n(&dd_ft_enabled, __ATOMIC_ACQUIRE);
    do { if (!(gate & DD_FT_ON)) return; }
    while (!__atomic_compare_exchange_n(&dd_ft_enabled, &gate, gate+1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint64_t wall = dd_ft_now(CLOCK_MONOTONIC);
    /* Never wait behind journal I/O or another observer. Drop with accounting. */
    if (pthread_mutex_trylock(&dd_ft_lock)) {
        __atomic_add_fetch(&dd_ft_dropped, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&dd_ft_enabled, 1, __ATOMIC_RELEASE); return;
    }
    if (wall < dd_ft_start || wall >= dd_ft_end) {
        pthread_mutex_unlock(&dd_ft_lock); __atomic_sub_fetch(&dd_ft_enabled, 1, __ATOMIC_RELEASE); return;
    }
    if ((unsigned)stage >= DD_FT_COUNT || dd_ft_used == DD_FT_CAPACITY || !wall) {
        __atomic_add_fetch(&dd_ft_dropped, 1, __ATOMIC_RELAXED);
    } else {
        struct dd_ft_record *r = &dd_ft_records[dd_ft_used++];
        r->wall = wall; r->cpu = 0; r->id = id; r->stage = stage;
        r->object = object; r->value = value; r->status = status;
        if (!dd_ft_tid) dd_ft_tid = (uint32_t)syscall(SYS_gettid);
        r->tid = dd_ft_tid;
        uint64_t sequence = id.attempt ? id.attempt : id.frame ? id.frame : id.client ? id.client : id.fence ? id.fence : value;
        if (sample_cpu && sequence && sequence % dd_ft_cpu_stride == 0)
            r->cpu = dd_ft_now(CLOCK_THREAD_CPUTIME_ID);
    }
    pthread_mutex_unlock(&dd_ft_lock);
    __atomic_sub_fetch(&dd_ft_enabled, 1, __ATOMIC_RELEASE);
}
static void dd_ft_dump(int complete) {
    __atomic_fetch_and(&dd_ft_enabled, ~DD_FT_ON, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&dd_ft_enabled, __ATOMIC_ACQUIRE)) {
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&dd_ft_lock);
    char path[512];
    snprintf(path, sizeof(path), DD_FT_ROOT "/trace-%" PRIu64 "-%d-%s.jsonl", dd_ft_run, (int)getpid(), dd_ft_role);
    FILE *file = fopen(path, "wx");
    if (file) {
        fprintf(file, "{\"type\":\"header\",\"version\":1,\"run\":%" PRIu64 ",\"pid\":%d,\"role\":\"%s\",\"clock\":\"CLOCK_MONOTONIC\",\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"cpu_stride\":%u,\"capacity\":%u,\"ready_ns\":%" PRIu64 "}\n",
                dd_ft_run, (int)getpid(), dd_ft_role, dd_ft_start, dd_ft_end, dd_ft_cpu_stride, DD_FT_CAPACITY, dd_ft_ready_ns);
        for (unsigned i = 0; i < dd_ft_used; ++i) {
            const struct dd_ft_record *r = &dd_ft_records[i];
            fprintf(file, "{\"type\":\"event\",\"event_seq\":%u,\"stage\":\"%s\",\"mono_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"tid\":%u,\"client\":%" PRIu64 ",\"attempt\":%" PRIu64 ",\"scope\":%" PRIu64 ",\"frame\":%u,\"key\":%u,\"sync\":%u,\"fence\":%u,\"object\":%" PRIu64 ",\"value\":%" PRIu64 ",\"status\":%d}\n",
                    i+1, dd_ft_names[r->stage], r->wall, r->cpu, r->tid, r->id.client, r->id.attempt, r->id.scope,
                    r->id.frame, r->id.key, r->id.sync, r->id.fence, r->object, r->value, r->status);
        }
        fprintf(file, "{\"type\":\"footer\",\"count\":%u,\"dropped\":%u,\"complete\":%s}\n",
                dd_ft_used, __atomic_load_n(&dd_ft_dropped, __ATOMIC_RELAXED), complete ? "true" : "false");
        if (fclose(file)) fprintf(stderr, "DroidDeck frame trace output failed: %s\n", strerror(errno));
    } else fprintf(stderr, "DroidDeck frame trace open failed: %s\n", strerror(errno));
    dd_ft_last_run = dd_ft_run;
    pthread_mutex_unlock(&dd_ft_lock);
}
static void *dd_ft_worker(void *unused) {
    (void)unused;
    dd_ft_records = (struct dd_ft_record *)calloc(DD_FT_CAPACITY, sizeof(*dd_ft_records));
    if (!dd_ft_records) return NULL;
    char path[512]; snprintf(path, sizeof(path), DD_FT_ROOT "/ready-%d-%s", (int)getpid(), dd_ft_role);
    FILE *ready = fopen(path, "w");
    if (ready) { fprintf(ready, "version=1 pid=%d role=%s\n", (int)getpid(), dd_ft_role); fclose(ready); }
    while (!__atomic_load_n(&dd_ft_stop, __ATOMIC_ACQUIRE)) {
        uint64_t now = dd_ft_now(CLOCK_MONOTONIC);
        if (dd_ft_active() && now >= dd_ft_end) dd_ft_dump(1);
        if (access(DD_FT_ROOT "/armed", F_OK)) break;
        if (!dd_ft_active()) {
            uint64_t run = 0, start = 0, end = 0; unsigned stride = 0; char extra;
            FILE *control = fopen(DD_FT_ROOT "/control", "r");
            int n = control ? fscanf(control, "%" SCNu64 " %" SCNu64 " %" SCNu64 " %u %c", &run, &start, &end, &stride, &extra) : 0;
            if (control) fclose(control);
            if (n == 4 && run && run != dd_ft_last_run && stride && stride <= 64 &&
                    end > start && end-start <= UINT64_C(30000000000) && start <= now + UINT64_C(60000000000) && now < end) {
                pthread_mutex_lock(&dd_ft_lock);
                dd_ft_ready_ns = now; dd_ft_run = run; dd_ft_start = start; dd_ft_end = end; dd_ft_cpu_stride = stride; dd_ft_used = 0;
                __atomic_store_n(&dd_ft_dropped, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&dd_ft_enabled, DD_FT_ON, __ATOMIC_RELEASE);
                pthread_mutex_unlock(&dd_ft_lock);
            }
        }
        struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
    }
    if (dd_ft_active()) dd_ft_dump(dd_ft_now(CLOCK_MONOTONIC) >= dd_ft_end);
    unlink(path); free(dd_ft_records); dd_ft_records = NULL;
    return NULL;
}
void dd_ft_init(const char *role) {
    /* Startup-only arm marker. No polling thread exists in default sessions. */
    if (access(DD_FT_ROOT "/armed", F_OK)) return;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&dd_ft_running, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    dd_ft_role = role; __atomic_store_n(&dd_ft_stop, 0, __ATOMIC_RELEASE);
    if (pthread_create(&dd_ft_thread, NULL, dd_ft_worker, NULL)) __atomic_store_n(&dd_ft_running, 0, __ATOMIC_RELEASE);
}
void dd_ft_shutdown(void) {
    if (!__atomic_load_n(&dd_ft_running, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&dd_ft_stop, 1, __ATOMIC_RELEASE);
    pthread_join(dd_ft_thread, NULL);
    __atomic_store_n(&dd_ft_running, 0, __ATOMIC_RELEASE);
}
