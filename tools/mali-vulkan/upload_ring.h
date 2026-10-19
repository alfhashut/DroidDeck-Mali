/* Explicit compositor-writer contract, NOT generic Vulkan dirty tracking.
 * All functions run under the proxy connection lock. Storage is bounded;
 * bookkeeping overflow selects a full upload, never rejects a valid buffer. */
#ifndef DD_UPLOAD_RING_H
#define DD_UPLOAD_RING_H
#include <stdint.h>
#include <string.h>
enum { DD_RING_PROBE, DD_RING_REGISTER, DD_RING_RESERVE, DD_RING_MODIFIED, DD_RING_SEAL };
enum { DD_RING_BASELINE = 1, DD_RING_BOUNDS = 2, DD_RING_CAPACITY = 4,
       DD_RING_UNFINISHED = 8, DD_RING_UNSEALED = 16 };
#define DD_RING_RANGES 32u
struct dd_ring_range { uint64_t offset, length; };
struct dd_upload_ring {
    uint64_t size, generation, sealed_generation, flight_generation, marks, modified;
    uint32_t count, pending_count, uncertain, sealed_command, flight_command;
    struct dd_ring_range ranges[DD_RING_RANGES], pending[DD_RING_RANGES];
};
static inline int dd_ring_valid(const struct dd_upload_ring *r, uint64_t offset, uint64_t length) {
    return length && offset < r->size && length <= r->size - offset;
}
static inline void dd_ring_init(struct dd_upload_ring *r, uint64_t size) {
    memset(r, 0, sizeof(*r)); r->size = size; r->uncertain = DD_RING_BASELINE;
}
static inline void dd_ring_change(struct dd_upload_ring *r) {
    if (r->generation != UINT64_MAX) ++r->generation;
    else r->uncertain |= DD_RING_UNSEALED; /* saturated: retain full fallback */
}
static inline int dd_ring_reserve(struct dd_upload_ring *r, uint64_t offset, uint64_t length) {
    dd_ring_change(r);
    if (!dd_ring_valid(r, offset, length)) { r->uncertain |= DD_RING_BOUNDS; return 0; }
    if (r->pending_count == DD_RING_RANGES) { r->uncertain |= DD_RING_CAPACITY; return 0; }
    struct dd_ring_range range = {offset, length};
    r->pending[r->pending_count++] = range; return 1;
}
static inline int dd_ring_modified(struct dd_upload_ring *r, uint64_t offset, uint64_t length) {
    dd_ring_change(r);
    if (!dd_ring_valid(r, offset, length)) { r->uncertain |= DD_RING_BOUNDS; return 0; }
    unsigned i;
    for (i = 0; i < r->pending_count; ++i)
        if (r->pending[i].offset == offset && r->pending[i].length == length) break;
    if (i == r->pending_count) { r->uncertain |= DD_RING_UNFINISHED; return 0; }
    r->pending[i] = r->pending[--r->pending_count];
    ++r->marks; r->modified += length;
    uint64_t end = offset + length;
    /* Unsorted bounded set: restarting after a merge handles bridging ranges. */
    for (i = 0; i < r->count;) {
        uint64_t a = r->ranges[i].offset, b = a + r->ranges[i].length;
        if (end < a || offset > b) { ++i; continue; }
        if (a < offset) offset = a;
        if (b > end) end = b;
        r->ranges[i] = r->ranges[--r->count]; i = 0;
    }
    if (r->count == DD_RING_RANGES) { r->uncertain |= DD_RING_CAPACITY; return 0; }
    struct dd_ring_range range = {offset, end - offset};
    r->ranges[r->count++] = range; return 1;
}
static inline void dd_ring_seal(struct dd_upload_ring *r, uint32_t command) {
    r->sealed_command = command; r->sealed_generation = r->generation;
}
static inline unsigned dd_ring_fallback(const struct dd_upload_ring *r, uint32_t command) {
    return r->uncertain | (r->pending_count ? DD_RING_UNFINISHED : 0) |
        ((r->sealed_command != command || r->sealed_generation != r->generation) ? DD_RING_UNSEALED : 0);
}
static inline void dd_ring_uploaded(struct dd_upload_ring *r, uint32_t command) {
    r->flight_command = command; r->flight_generation = r->generation;
}
static inline void dd_ring_submitted(struct dd_upload_ring *r, uint32_t command) {
    /* Called ONLY after every upload ACK AND real QueueSubmit success. A newer
     * store/seal must survive; conservatively retain the entire older set too. */
    if (r->flight_command != command) return;
    if (r->generation != UINT64_MAX && r->generation == r->flight_generation && r->sealed_command == command && !r->pending_count) {
        r->count = 0; r->marks = r->modified = 0; r->uncertain = 0;
    }
    r->flight_command = 0; r->sealed_command = 0;
}
#endif
