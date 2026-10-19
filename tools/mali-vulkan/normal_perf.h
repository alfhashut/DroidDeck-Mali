/* Local instrumentation only: no new wire opcode, Vulkan feature or handle. */
#ifndef DD_NORMAL_PERF_H
#define DD_NORMAL_PERF_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "normal_wait_perf.h"
#define DD_PERF_OPS 92u
#define DD_PERF_TARGET_NS UINT64_C(16666666)
#define DD_PERF_INTERVAL_NS UINT64_C(2000000000)
#define DD_PERF_SAMPLES 256u
struct dd_perf_op { uint64_t count, ns, worst; };
struct dd_perf_rpc {
    struct dd_perf_op op[DD_PERF_OPS]; uint64_t upload_bytes, download_bytes;
    struct dd_wait_stats wait[DD_WAIT_REASONS];
    uint64_t renderer_destroy_kind[DD_PERF_DESTROY_KINDS];
};
static inline uint64_t dd_perf_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
/* Sample once per frame/report, not per RPC. This thread's execution time;
 * wall minus CPU also includes scheduling delays and work in other processes. */
static inline uint64_t dd_perf_cpu_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static inline void dd_perf_add(struct dd_perf_rpc *p, unsigned op, uint64_t ns) {
    if (op >= DD_PERF_OPS) return;
    ++p->op[op].count; p->op[op].ns += ns;
    if (ns > p->op[op].worst) p->op[op].worst = ns;
}
static inline void dd_perf_merge(struct dd_perf_rpc *p, const struct dd_perf_rpc *q) {
    for (unsigned i = 0; i < DD_PERF_OPS; ++i) {
        p->op[i].count += q->op[i].count; p->op[i].ns += q->op[i].ns;
        if (q->op[i].worst > p->op[i].worst) p->op[i].worst = q->op[i].worst;
    }
    p->upload_bytes += q->upload_bytes; p->download_bytes += q->download_bytes;
    for (unsigned i = 0; i < DD_WAIT_REASONS; ++i) dd_wait_merge(&p->wait[i], &q->wait[i]);
    for (unsigned i = 0; i < DD_PERF_DESTROY_KINDS; ++i) p->renderer_destroy_kind[i] += q->renderer_destroy_kind[i];
}
/* Nearest rank p95. Sort a bounded copy at report time, never in the frame path.
 * At 30 or 60 Hz a two-second window fits without dropping samples. */
static inline uint64_t dd_perf_p95(const uint64_t *samples, unsigned count) {
    uint64_t sorted[DD_PERF_SAMPLES];
    if (count > DD_PERF_SAMPLES) count = DD_PERF_SAMPLES;
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = i;
        while (j && sorted[j - 1] > samples[i]) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = samples[i];
    }
    return count ? sorted[(count * 95u + 99u) / 100u - 1u] : 0;
}
#endif
