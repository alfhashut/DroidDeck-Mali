/* CPU-only mapping ownership. Kept independent of GNU FD transport helpers. */
#ifndef DD_SHARED_STAGING_TYPES_H
#define DD_SHARED_STAGING_TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#define MB_SHARED_VERSION 1u
#define MB_SHARED_MAX_BYTES (16u * 1024u * 1024u)
#define MB_SHARED_MAX_REGIONS 3u /* per connection, across logical devices */
#define MB_SHARED_CONTROL_BYTES 64u
struct mb_shared_region { void *map; uint64_t bytes, sequence; };
_Static_assert(__atomic_always_lock_free(8, 0), "shared sequence must be lock-free across processes");
static inline size_t mb_shared_file_bytes(uint64_t bytes) {
    return bytes && bytes <= MB_SHARED_MAX_BYTES ? (size_t)((bytes + 63u) & ~UINT64_C(63)) + MB_SHARED_CONTROL_BYTES : 0;
}
static inline uint64_t *mb_shared_sequence(void *map, uint64_t bytes) {
    return (uint64_t *)((uint8_t *)map + ((bytes + 63u) & ~UINT64_C(63)));
}
static inline void mb_shared_release(struct mb_shared_region *r) {
    if (r->map) munmap(r->map, mb_shared_file_bytes(r->bytes));
    memset(r, 0, sizeof(*r));
}
#endif
