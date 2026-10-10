/* Normal-only attribution. Optional metadata on opcode 63; no new opcode. */
#ifndef DD_NORMAL_WAIT_PERF_H
#define DD_NORMAL_WAIT_PERF_H
#include <stdint.h>
#include <stdio.h>
#define DD_WAIT_REQUEST_BYTES 32u
#define DD_WAIT_SAMPLE_BYTES 72u
#define DD_WAIT_MATERIAL_NS UINT64_C(1000000)
/* 24 descriptor sets rotate once/frame, with two submissions/frame. Retain
 * more than that 48-submission distance, including intervening upload work. */
#define DD_WAIT_HISTORY 64u
#define DD_PERF_DESTROY_KINDS 10u
/* These names describe actual source actions, not guessed GPU dependencies. */
enum dd_wait_reason {
    DD_WAIT_UNATTRIBUTED, DD_WAIT_SHM_STAGING_REUSE, DD_WAIT_DESCRIPTOR_REUSE,
    DD_WAIT_OUTPUT_COMPLETION, DD_WAIT_OUTPUT_RETIRE, DD_WAIT_REASONS
};
static const char *const dd_wait_names[DD_WAIT_REASONS] = {
    "unattributed-device-wait", "shm-staging-slot-reuse", "descriptor-set-reuse",
    "output-producer-completion", "output-command-retirement"
};
/* Preserve metadata reason ID 1 and snapshot layout for existing assets. */
#define DD_WAIT_SHM_STAGING_DESTROY DD_WAIT_SHM_STAGING_REUSE
struct dd_wait_sample {
    uint64_t before, wall_ns, cpu_ns, query_ns, last_signal;
    uint32_t command, image, buffer, memory, descriptor_set, fence, before_result, producer_found;
};
struct dd_wait_producer {
    uint64_t value;
    uint32_t semaphore, command, image, buffer, memory, descriptor_set, fence;
};
struct dd_wait_stats {
    uint64_t count, samples, rtt_ns, rtt_max, wall_ns, wall_max, cpu_ns, off_cpu_ns, query_ns;
    uint64_t satisfied, brief, material, unknown, failed;
    uint64_t target;
    uint32_t semaphore, detail, result;
    struct dd_wait_sample last;
};
/* Explicit LE scalars; never send native structs, pointers or handles. */
static inline void dd_wait_put(uint8_t *w, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) w[i] = (uint8_t)(v >> (i * 8));
}
static inline uint64_t dd_wait_get(const uint8_t *w, unsigned bytes) {
    uint64_t v = 0; for (unsigned i = 0; i < bytes; ++i) v |= (uint64_t)w[i] << (i * 8); return v;
}
#define DD_WAIT_FIELDS(U64, U32) \
    U64(before, 0) U64(wall_ns, 8) U64(cpu_ns, 16) U64(query_ns, 24) U64(last_signal, 32) \
    U32(command, 40) U32(image, 44) U32(buffer, 48) U32(memory, 52) U32(descriptor_set, 56) \
    U32(fence, 60) U32(before_result, 64) U32(producer_found, 68)
static inline void dd_wait_encode(uint8_t *w, const struct dd_wait_sample *s) {
#define W64(n, o) dd_wait_put(w + o, s->n, 8);
#define W32(n, o) dd_wait_put(w + o, s->n, 4);
    DD_WAIT_FIELDS(W64, W32)
#undef W64
#undef W32
}
static inline void dd_wait_decode(const uint8_t *w, struct dd_wait_sample *s) {
#define R64(n, o) s->n = dd_wait_get(w + o, 8);
#define R32(n, o) s->n = (uint32_t)dd_wait_get(w + o, 4);
    DD_WAIT_FIELDS(R64, R32)
#undef R64
#undef R32
}
#undef DD_WAIT_FIELDS
static inline void dd_wait_add(struct dd_wait_stats *p, uint64_t rtt, uint32_t sem,
        uint64_t target, uint32_t detail, uint32_t result, const struct dd_wait_sample *s) {
    ++p->count; p->rtt_ns += rtt; if (rtt > p->rtt_max) p->rtt_max = rtt;
    p->semaphore = sem; p->target = target; p->detail = detail; p->result = result;
    p->failed += result != 0;
    struct dd_wait_sample empty = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; p->last = s ? *s : empty;
    if (!s) { ++p->unknown; return; }
    ++p->samples; p->wall_ns += s->wall_ns; p->cpu_ns += s->cpu_ns; p->query_ns += s->query_ns;
    p->off_cpu_ns += s->wall_ns > s->cpu_ns ? s->wall_ns - s->cpu_ns : 0;
    if (s->wall_ns > p->wall_max) p->wall_max = s->wall_ns;
    if (s->before_result != 0) ++p->unknown;
    else if (s->before >= target) ++p->satisfied;
    else if (s->wall_ns < DD_WAIT_MATERIAL_NS) ++p->brief;
    else ++p->material;
}
static inline void dd_wait_merge(struct dd_wait_stats *p, const struct dd_wait_stats *q) {
    if (!q->count) return;
#define SUM(n) p->n += q->n;
    SUM(count) SUM(samples) SUM(rtt_ns) SUM(wall_ns) SUM(cpu_ns) SUM(off_cpu_ns) SUM(query_ns)
    SUM(satisfied) SUM(brief) SUM(material) SUM(unknown) SUM(failed)
#undef SUM
    if (q->rtt_max > p->rtt_max) p->rtt_max = q->rtt_max;
    if (q->wall_max > p->wall_max) p->wall_max = q->wall_max;
    /* Within a monotonic timeline, retain the greatest observed target. */
    if (q->target >= p->target) {
        p->target = q->target; p->semaphore = q->semaphore; p->detail = q->detail;
        p->result = q->result; p->last = q->last;
    }
}
static inline void dd_wait_report(FILE *out, unsigned reason, const struct dd_wait_stats *frame,
        const struct dd_wait_stats *outside, unsigned frames) {
    struct dd_wait_stats p = *frame; dd_wait_merge(&p, outside);
    if (!p.count) return;
    fprintf(out, "MaliPerf wait: wait-reason=%s count=%llu frame-count=%llu outside-count=%llu per-frame=%.2f RTT-total/avg/max=%.3f/%.3f/%.3fms native-wait-total/avg/max=%.3f/%.3f/%.3fms native-CPU=%.3fms native-off-CPU-est=%.3fms counter-query=%.3fms samples=%llu already-satisfied=%llu briefly-blocked=%llu materially-blocked=%llu unknown=%llu failed=%llu last-target=%llu before=%llu before-result=0x%x semaphore=%u detail=%u latest-submitted=%llu producer-found=%u command=%u image=%u buffer=%u memory=%u descriptor-set=%u fence=%u result=0x%x (blocked classes use counter-before and 1ms native-wall threshold; off-CPU includes scheduling, not GPU timestamps)\n",
        dd_wait_names[reason], (unsigned long long)p.count, (unsigned long long)frame->count,
        (unsigned long long)outside->count, frame->count / (double)(frames ? frames : 1),
        p.rtt_ns / 1e6, p.rtt_ns / (double)p.count / 1e6, p.rtt_max / 1e6,
        p.wall_ns / 1e6, p.samples ? p.wall_ns / (double)p.samples / 1e6 : 0, p.wall_max / 1e6,
        p.cpu_ns / 1e6, p.off_cpu_ns / 1e6, p.query_ns / 1e6, (unsigned long long)p.samples,
        (unsigned long long)p.satisfied, (unsigned long long)p.brief, (unsigned long long)p.material,
        (unsigned long long)p.unknown, (unsigned long long)p.failed, (unsigned long long)p.target,
        (unsigned long long)p.last.before, p.last.before_result, p.semaphore, p.detail,
        (unsigned long long)p.last.last_signal, p.last.producer_found, p.last.command, p.last.image,
        p.last.buffer, p.last.memory, p.last.descriptor_set, p.last.fence, p.result);
}
#endif
