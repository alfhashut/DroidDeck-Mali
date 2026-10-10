/* Proxy-local instrumentation only. Never part of any snapshot/wire ABI. */
#ifndef DD_NORMAL_MEMORY_PERF_H
#define DD_NORMAL_MEMORY_PERF_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { DD_WRITE_OTHER, DD_WRITE_STAGING, DD_WRITE_UNIFORM, DD_WRITE_ROLES };
enum { DD_WRITE_OUTSIDE, DD_WRITE_U, DD_WRITE_R, DD_WRITE_PHASES };
#ifdef __cplusplus
static thread_local unsigned dd_memory_submit_phase;
#else
static _Thread_local unsigned dd_memory_submit_phase;
#endif
struct dd_write_row {
    uint64_t calls, ok, bytes, acknowledged_bytes, wire_bytes, full, allocation, mapped_offset, mapped_size;
    uint64_t offset, length, min_length, max_length;
    uint32_t first_id, last_id; unsigned ids_changed;
    uint64_t modified, marked, merged, precise, fallback, avoided, fallback_reason[5];
};
struct dd_memory_profile {
    uint64_t report, lookups, visited, max_visit, submit_nodes, submit_live, submit_mapped, submits;
    uint64_t owned_records, live_index, active_records, retired_records;
    struct dd_write_row row[DD_WRITE_ROLES][DD_WRITE_PHASES];
};
static inline void dd_memory_ring(struct dd_memory_profile *p, uint32_t id,
        uint64_t allocation, uint64_t map_offset, uint64_t map_size,
        uint64_t modified, uint64_t marked, unsigned merged, uint64_t transmitted, unsigned fallback) {
    struct dd_write_row *r = &p->row[DD_WRITE_UNIFORM][dd_memory_submit_phase];
    if (!r->calls) r->first_id = id;
    r->last_id = id; r->allocation = allocation; r->mapped_offset = map_offset; r->mapped_size = map_size;
    r->modified += modified; r->marked += marked; r->merged += merged;
    r->precise += !fallback; r->fallback += !!fallback;
    r->avoided += transmitted <= map_size ? map_size - transmitted : 0;
    for (unsigned i = 0; i < 5; ++i) r->fallback_reason[i] += !!(fallback & (1u << i));
}
static inline void dd_memory_write(struct dd_memory_profile *p, unsigned role, uint32_t id,
        uint64_t allocation, uint64_t map_offset, uint64_t map_size,
        uint64_t offset, uint64_t length, int ok) {
    struct dd_write_row *r = &p->row[role][dd_memory_submit_phase];
    if (!r->calls) { r->first_id = id; r->min_length = length; }
    else { r->ids_changed |= r->last_id != id; if (length < r->min_length) r->min_length = length; }
    ++r->calls; r->ok += ok; r->bytes += length;
    if (ok) r->acknowledged_bytes += length;
    r->wire_bytes += length + 36;
    r->full += offset == map_offset && length == map_size;
    if (length > r->max_length) r->max_length = length;
    r->last_id = id; r->allocation = allocation; r->mapped_offset = map_offset; r->mapped_size = map_size;
    r->offset = offset; r->length = length;
}
static inline void dd_memory_report(struct dd_memory_profile *p, FILE *out, uint64_t now) {
    if (!p->report) { p->report = now; return; }
    if (now - p->report < UINT64_C(2000000000)) return;
    static const char *roles[] = {"other", "staging-SHM", "compositor-upload-ring"};
    static const char *phases[] = {"other", "U", "R"};
    uint64_t calls = 0, bytes = 0;
    for (unsigned a = 0; a < DD_WRITE_ROLES; ++a) for (unsigned b = 0; b < DD_WRITE_PHASES; ++b) {
        const struct dd_write_row *r = &p->row[a][b]; if (!r->calls && !r->precise && !r->fallback) continue;
        calls += r->calls; bytes += r->bytes;
        fprintf(out, "MaliPerf memory-write: purpose=%s submission=%s calls=%llu acknowledged=%llu requested-data-bytes=%llu acknowledged-data-bytes=%llu request-wire-bytes=%llu first/last-memory-ID=%u/%u IDs-changed=%u last-allocation=%llu last-mapped-offset/size=%llu/%llu last-write-offset/length=%llu/%llu length-min/max=%llu/%llu whole-map=%llu (window totals; requested bytes may be only partially sent on transport failure; wire includes 16+20 request headers, excludes reply; modified extents in client-writes, content-differences unknown)\n",
            roles[a], phases[b], (unsigned long long)r->calls, (unsigned long long)r->ok,
            (unsigned long long)r->bytes, (unsigned long long)r->acknowledged_bytes, (unsigned long long)r->wire_bytes, r->first_id, r->last_id, r->ids_changed,
            (unsigned long long)r->allocation, (unsigned long long)r->mapped_offset, (unsigned long long)r->mapped_size,
            (unsigned long long)r->offset, (unsigned long long)r->length,
            (unsigned long long)r->min_length, (unsigned long long)r->max_length, (unsigned long long)r->full);
        if (a == DD_WRITE_UNIFORM) fprintf(out,
            "MaliPerf ring-ranges: submission=%s logical-modified=%llu transmitted=%llu marked=%llu merged=%llu precise=%llu full-map-fallback=%llu bytes-avoided=%llu fallback-baseline/bounds/capacity/unfinished/unsealed=%llu/%llu/%llu/%llu/%llu (attempted epochs; transmitted bytes and ACKs above; zero-range sealed epochs are precise)\n",
            phases[b], (unsigned long long)r->modified, (unsigned long long)r->bytes,
            (unsigned long long)r->marked, (unsigned long long)r->merged,
            (unsigned long long)r->precise, (unsigned long long)r->fallback, (unsigned long long)r->avoided,
            (unsigned long long)r->fallback_reason[0], (unsigned long long)r->fallback_reason[1],
            (unsigned long long)r->fallback_reason[2], (unsigned long long)r->fallback_reason[3],
            (unsigned long long)r->fallback_reason[4]);
    }
    fprintf(out, "MaliPerf proxy-lifetime: write-calls=%llu requested-data-bytes=%llu handle-lookups=%llu nodes-visited=%llu lookup-avg/max=%.2f/%llu last-submit-nodes/live/mapped=%llu/%llu/%llu submits=%llu owned-records=%llu live-index=%llu active-traversal=%llu retired-records=%llu (lookups probe live hash entries; submit visits active nodes only; retired storage retained until device cleanup; registry gauges for snapshot device; independent 2s proxy window)\n",
        (unsigned long long)calls, (unsigned long long)bytes, (unsigned long long)p->lookups,
        (unsigned long long)p->visited, p->lookups ? p->visited / (double)p->lookups : 0, (unsigned long long)p->max_visit,
        (unsigned long long)p->submit_nodes, (unsigned long long)p->submit_live,
        (unsigned long long)p->submit_mapped, (unsigned long long)p->submits,
        (unsigned long long)p->owned_records, (unsigned long long)p->live_index,
        (unsigned long long)p->active_records, (unsigned long long)p->retired_records);
    memset(p, 0, sizeof(*p)); p->report = now;
}
#endif
