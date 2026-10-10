/* Pointer handles retain their storage until device cleanup. Only live records
 * participate in the index and active list. Caller holds the connection lock
 * for every lookup/mutation/traversal (cleanup has exclusive device ownership). */
#ifndef DD_RESOURCE_REGISTRY_H
#define DD_RESOURCE_REGISTRY_H
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
enum proxy_resource_kind { PROXY_POOL, PROXY_COMMAND, PROXY_EVENT, PROXY_FENCE, PROXY_BUFFER, PROXY_MEMORY, PROXY_IMAGE, PROXY_AHB, PROXY_SYNC, PROXY_SEMAPHORE, PROXY_VIEW, PROXY_SAMPLER, PROXY_SET_LAYOUT, PROXY_PIPELINE_LAYOUT, PROXY_DESCRIPTOR_POOL, PROXY_DESCRIPTOR_SET, PROXY_SHADER, PROXY_PIPELINE };
struct proxy_resource {
    VK_LOADER_DATA loader;
    struct proxy_logical *owner;
    struct proxy_resource *next; /* owning chain: never searched in the frame path */
    struct proxy_resource *active_next, *active_prev;
    struct proxy_resource *index_next, **index_link;
    uint32_t id, pool, image_id; /* image_id is view metadata for diagnostic logs */
    enum proxy_resource_kind kind;
    atomic_int live;
    uint8_t *mirror;
    uint64_t allocation, map_offset, map_size;
    /* Explicit normal staging opt-in; ordinary coherent mappings are unchanged. */
    uint32_t staging_managed, staging_command;
    /* Profiling metadata only; never used for validation or memory transport. */
    uint32_t profile_usage, profile_upload;
};
_Static_assert(offsetof(struct proxy_resource, loader) == 0, "command buffer dispatch word");

/* 256 bucket heads cost 2 KiB on this 64-bit path. Intrusive chaining imposes
 * no live-object cap or index allocation failure. Collisions visit live nodes
 * only: expected O(1), worst O(live), independent of retained history. */
#define PROXY_RESOURCE_BUCKETS 256u
struct proxy_registry {
    struct proxy_resource *owned, *active, *index[PROXY_RESOURCE_BUCKETS];
    size_t owned_count, indexed_count, active_count;
};
static inline size_t proxy_resource_bucket(uintptr_t handle) {
    /* Mix aligned pointer bits; do not dereference untrusted/stale handles. */
    uint64_t x = (uint64_t)handle;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33; x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return (size_t)x & (PROXY_RESOURCE_BUCKETS - 1u);
}
static inline void proxy_registry_insert(struct proxy_registry *r, struct proxy_resource *o) {
    o->live = 1;
    o->next = r->owned; r->owned = o; ++r->owned_count;
    o->active_next = r->active; o->active_prev = NULL;
    if (r->active) r->active->active_prev = o;
    r->active = o; ++r->active_count;
    struct proxy_resource **bucket = &r->index[proxy_resource_bucket((uintptr_t)o)];
    o->index_next = *bucket; o->index_link = bucket;
    if (*bucket) (*bucket)->index_link = &o->index_next;
    *bucket = o; ++r->indexed_count;
}
static inline struct proxy_resource *proxy_registry_find(struct proxy_registry *r,
        uintptr_t handle, enum proxy_resource_kind kind, uint64_t *visited) {
    for (struct proxy_resource *o = r->index[proxy_resource_bucket(handle)]; o; o = o->index_next) {
        ++*visited;
        if ((uintptr_t)o == handle && o->kind == kind && o->live) return o;
    }
    return NULL;
}
static inline void proxy_registry_retire(struct proxy_registry *r, struct proxy_resource *o) {
    if (!o->live) return;
    o->live = 0;
    *o->index_link = o->index_next;
    if (o->index_next) o->index_next->index_link = o->index_link;
    if (o->active_prev) o->active_prev->active_next = o->active_next;
    else r->active = o->active_next;
    if (o->active_next) o->active_next->active_prev = o->active_prev;
    o->index_link = NULL; o->index_next = o->active_next = o->active_prev = NULL;
    --r->indexed_count; --r->active_count;
    /* No free, ID reuse or pointer recycling: direct command handles and any
     * previously obtained local references remain allocated, with live=0. */
}
static inline void proxy_registry_retire_children(struct proxy_registry *r,
        struct proxy_resource *o, enum proxy_resource_kind child_kind) {
    proxy_registry_retire(r, o);
    for (struct proxy_resource *child = r->active, *next; child; child = next) {
        next = child->active_next;
        if (child->kind == child_kind && child->pool == o->id) proxy_registry_retire(r, child);
    }
}
static inline void proxy_registry_cleanup(struct proxy_registry *r) {
    for (struct proxy_resource *o = r->owned, *next; o; o = next) {
        next = o->next; free(o->mirror); free(o);
    }
    memset(r, 0, sizeof(*r));
}
#endif
