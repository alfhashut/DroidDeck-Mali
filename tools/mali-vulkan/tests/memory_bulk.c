/* Compile only extracted transfer/submit helpers with in-memory RPC/Vulkan stubs.
 * Native mapped-memory switch and proxy profiling wrapper are real source. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "interop_protocol.h"
#include "renderer_protocol.h"
#include "normal_perf.h"
typedef int VkResult;
typedef void *VkQueue;
typedef void *VkFence;
typedef void *VkDevice;
typedef void *VkDeviceMemory;
typedef void *VkCommandBuffer;
typedef uint64_t VkDeviceSize;
typedef unsigned VkMemoryMapFlags;
#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_FEATURE_NOT_PRESENT (-8)
#define VK_ERROR_MEMORY_MAP_FAILED (-5)
#define VK_WHOLE_SIZE UINT64_MAX
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 1u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 2u
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 1
#define VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR 2
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE 3
typedef struct { int sType; const void *pNext; unsigned waitSemaphoreValueCount, signalSemaphoreValueCount; const uint64_t *pSignalSemaphoreValues; } VkTimelineSemaphoreSubmitInfoKHR;
typedef struct { int sType; const void *pNext; unsigned waitSemaphoreCount, commandBufferCount, signalSemaphoreCount; void **pCommandBuffers, **pSignalSemaphores; } VkSubmitInfo;
typedef struct { int sType; void *memory; uint64_t offset, size; } VkMappedMemoryRange;
struct proxy_instance { pthread_mutex_t lock; uint32_t wire_version; int fd, perf_enabled; struct dd_perf_rpc perf; } connection;
enum proxy_resource_kind { PROXY_MEMORY, PROXY_COMMAND, PROXY_SEMAPHORE, PROXY_FENCE };
struct proxy_logical;
struct proxy_resource {
    struct proxy_logical *owner; struct proxy_resource *next;
    uint32_t id, pool; enum proxy_resource_kind kind; int live;
    uint8_t *mirror; uint64_t allocation, map_offset, map_size;
    uint32_t staging_managed, staging_command;
};
struct proxy_logical { struct proxy_instance *owner; uint32_t id; struct proxy_resource *resources; uint8_t *write_request, *read_reply; uint32_t write_capacity, read_capacity; uint64_t map_alignment; atomic_int submit_failed; } device;
struct proxy_queue { uint32_t id; struct proxy_logical *owner; } queue;
struct native_memory { uint32_t id, ahb, type; uint64_t size, map_offset, map_size; void *map, *handle; } memories[3];
struct native_interop {
    struct { struct { uint32_t propertyFlags; } memoryTypes[1]; } properties;
    uint64_t atom;
    void (*UnmapMemory)(void *, void *);
    VkResult (*MapMemory)(void *, void *, uint64_t, uint64_t, unsigned, void **);
    VkResult (*FlushMappedMemoryRanges)(void *, unsigned, const VkMappedMemoryRange *);
    VkResult (*InvalidateMappedMemoryRanges)(void *, unsigned, const VkMappedMemoryRange *);
};
struct native_device { void *handle; struct native_interop interop; } native;
static int pending, fail_write, fail_read, bad_ack, bad_read, fail_alloc, disconnects;
static uint32_t pending_memory;
static unsigned writes, reads, submits, allocations, memory_maps, memory_unmaps;
static struct proxy_resource command, semaphore;
static struct native_memory *interop_find_memory(struct native_device *d, uint32_t id) {
    (void)d; for (unsigned i = 0; i < 3; ++i) if (memories[i].id == id && id) return &memories[i]; return NULL;
}
static int interop_memory_pending(struct native_device *d, struct native_memory *m) { (void)d; return pending || pending_memory == m->id; }
static int interop_range(uint64_t total, uint64_t offset, uint64_t size) { return size && offset <= total && size <= total - offset; }
static uint32_t native_transfer(uint32_t version, uint32_t op, const uint8_t *w, uint32_t bytes,
        uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    uint32_t minimum = op == MB_MEMORY_MAP ? 24u : op == MB_MEMORY_UNMAP ? 8u : 20u;
    if (bytes < minimum || (op != MB_MEMORY_WRITE && bytes != minimum) || mb_get_u32(w) != device.id) return MB_PROTOCOL_ERROR;
    struct { uint32_t wire_version; } session = {version}, *s = &session;
    struct native_device *d = &native; struct native_interop *v = &d->interop; uint32_t id = mb_get_u32(w + 4);
    switch (op) {
#include "native_memory.inc"
    default: return MB_PROTOCOL_ERROR;
    }
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
static VkResult rpc_exchange(struct proxy_instance *s, uint32_t op, const uint8_t *request,
        uint32_t bytes, uint8_t *reply, uint32_t *reply_bytes, uint32_t capacity) {
    /* The real upload helper holds this lock throughout the transfer. */
    assert(pthread_mutex_trylock(&s->lock) != 0);
    if (op == MB_RENDERER_SUBMIT) {
        for (struct proxy_resource *m = device.resources; m; m = m->next) if ((m->pool & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && (!m->staging_managed || m->staging_command == command.id)) {
            struct native_memory *n = interop_find_memory(&native, m->id);
            assert(n && !memcmp(n->map, m->mirror, (size_t)m->map_size));
        }
        ++submits; memset(reply, 0, MB_PREFIX_BYTES); *reply_bytes = MB_PREFIX_BYTES; return VK_SUCCESS;
    }
    assert(op == MB_MEMORY_WRITE || op == MB_MEMORY_READ || op == MB_MEMORY_MAP || op == MB_MEMORY_UNMAP);
    if (op == MB_MEMORY_MAP) ++memory_maps;
    if (op == MB_MEMORY_UNMAP) ++memory_unmaps;
    if (op == MB_MEMORY_WRITE && ++writes == (unsigned)fail_write) return VK_ERROR_DEVICE_LOST;
    if (op == MB_MEMORY_READ) {
        assert(bytes == 20 && capacity == MB_PREFIX_BYTES + mb_get_u32(request + 16));
        assert(capacity <= MB_CAP_MAX_PAYLOAD);
        if (++reads == (unsigned)fail_read) return VK_ERROR_DEVICE_LOST;
    }
    uint32_t extra = 0, count = 0; VkResult result = VK_SUCCESS;
    uint32_t status = native_transfer(s->wire_version, op, request, bytes, reply + MB_PREFIX_BYTES, &extra, &count, &result);
    assert(MB_PREFIX_BYTES + extra <= capacity);
    mb_put_u32(reply, status); mb_put_u32(reply + 4, (uint32_t)result); mb_put_u32(reply + 8, count);
    *reply_bytes = MB_PREFIX_BYTES + extra;
    if (op == MB_MEMORY_WRITE && bad_ack) {
        if (bad_ack == 1) *reply_bytes = 0;
        if (bad_ack == 2) *reply_bytes = 8;
        if (bad_ack == 3) mb_put_u32(reply + 8, 1);
    }
    if (op == MB_MEMORY_READ && bad_read) {
        if (bad_read == 1) *reply_bytes = 0;
        if (bad_read == 2) *reply_bytes = 8;
        if (bad_read == 3) mb_put_u32(reply + 8, 0);
        if (bad_read == 4) --*reply_bytes;
        if (bad_read == 5) ++*reply_bytes;
        if (bad_read == 6) return VK_ERROR_DEVICE_LOST; // Unexpected data on error.
    }
    return status == MB_OK ? result : VK_ERROR_INITIALIZATION_FAILED;
}
#include "metric.inc"
static int fake_shutdown(int fd, int how) { (void)fd; (void)how; ++disconnects; return 0; }
static void *test_realloc(void *ptr, size_t size) { ++allocations; return fail_alloc ? NULL : realloc(ptr, size); }
#define realloc test_realloc
#define shutdown fake_shutdown
#include "upload.inc"
#undef realloc
#undef shutdown
static struct proxy_resource *submit_find(struct proxy_logical *d, uintptr_t handle, enum proxy_resource_kind kind) {
    struct proxy_resource *r = (struct proxy_resource *)handle; return r && r->owner == d && r->live && r->kind == kind ? r : NULL;
}
static VkResult renderer_rpc(struct proxy_logical *d, uint32_t op, const uint8_t *args, uint32_t n, uint8_t *out, uint32_t expected) {
    (void)args; (void)n; (void)out; (void)expected; uint8_t reply[MB_PREFIX_BYTES]; uint32_t bytes = 0;
    pthread_mutex_lock(&d->owner->lock); VkResult r = rpc(d->owner, op, NULL, 0, reply, &bytes, sizeof(reply));
    pthread_mutex_unlock(&d->owner->lock); return r;
}
#include "map_rpc.inc"
static void submit_void_error(struct proxy_logical *d, const char *name, VkResult result) {
    (void)name; if (result != VK_SUCCESS) atomic_store(&d->submit_failed, result);
}
#include "mapping.inc"
#include "submit.inc"
#include "cleanup.inc"
static void setup(uint32_t version) {
    connection.wire_version = version; connection.perf_enabled = 1; assert(!pthread_mutex_init(&connection.lock, NULL));
    device.owner = &connection; device.id = 1; queue.owner = &device; queue.id = 1;
    native.interop.properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    command.id = 10; command.kind = PROXY_COMMAND; command.owner = &device; command.live = 1;
    semaphore.id = 11; semaphore.kind = PROXY_SEMAPHORE; semaphore.owner = &device; semaphore.live = 1;
}
static struct proxy_resource *mapping(unsigned i, uint64_t size, uint64_t base) {
    struct proxy_resource *m = calloc(1, sizeof(*m)); assert(m);
    m->owner = &device; m->id = i + 1; m->kind = PROXY_MEMORY; m->live = 1; m->pool = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m->allocation = base + size + 64; m->map_offset = base; m->map_size = size; m->mirror = malloc((size_t)size); assert(m->mirror);
    for (uint64_t n = 0; n < size; ++n) m->mirror[n] = (uint8_t)(n * 31 + i);
    m->next = device.resources; device.resources = m;
    memories[i] = (struct native_memory){.id = m->id, .size = m->allocation, .map_offset = base, .map_size = size, .map = calloc(1, (size_t)size)};
    assert(memories[i].map); return m;
}
static VkResult submit(void) {
    uint64_t value = submits + 1; void *c = &command, *s = &semaphore;
    VkTimelineSemaphoreSubmitInfoKHR timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline, .commandBufferCount = 1, .pCommandBuffers = &c, .signalSemaphoreCount = 1, .pSignalSemaphores = &s};
    return renderer_QueueSubmit(&queue, 1, &info, NULL);
}
static void finish(void) {
    proxy_free_resources(&device); assert(!device.resources && !device.write_request && !device.write_capacity);
    assert(!device.read_reply && !device.read_capacity);
    proxy_free_resources(&device); // Repeated cleanup also leaves no scratch buffer.
    for (unsigned i = 0; i < 3; ++i) free(memories[i].handle ? memories[i].handle : memories[i].map);
    assert(!pthread_mutex_destroy(&connection.lock));
}
static void bulk(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_WRITE_MAX * 4u + 19u, 257);
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 1) == VK_SUCCESS);
    assert(writes == 5 && !memcmp(m->mirror, memories[0].map, (size_t)m->map_size));
    assert(connection.perf.op[MB_MEMORY_WRITE].count == 5 && connection.perf.upload_bytes == m->map_size);
    assert(allocations == 1 && device.write_capacity == MB_MEMORY_WRITE_HEADER + MB_INTEROP_WRITE_MAX);
    uint8_t *request = device.write_request;
    memset(memories[0].map, 0, (size_t)m->map_size);
    assert(interop_copy_mapping(m, m->map_offset + 7, 511u * 1024u, 1) == VK_SUCCESS);
    assert(writes == 6 && allocations == 1 && device.write_request == request);
    assert(!memcmp(m->mirror + 7, (uint8_t *)memories[0].map + 7, 511u * 1024u));
    assert(((uint8_t *)memories[0].map)[6] == 0 && ((uint8_t *)memories[0].map)[7 + 511u * 1024u] == 0);
    finish();
}
static void ordering(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_WRITE_MAX, 0);
    assert(submit() == VK_SUCCESS); m->mirror[m->map_size - 1] ^= 0xff; assert(submit() == VK_SUCCESS);
    assert(submits == 2 && writes == 2 && connection.perf.upload_bytes == 2u * MB_INTEROP_WRITE_MAX);
    assert(connection.perf.op[MB_RENDERER_SUBMIT].count == 2);
    mapping(1, MB_INTEROP_WRITE_MAX + 1u, 128);
    assert(submit() == VK_SUCCESS && submits == 3 && writes == 5); finish();
}
static void persistent_staging(void) {
    setup(7);
    struct proxy_resource *ordinary = mapping(0, 512u * 1024u, 0);
    struct proxy_resource *stage = mapping(1, 230400, 0), *idle = mapping(2, 230400, 0);
    stage->allocation = stage->map_size; memories[1].size = stage->allocation;
    idle->allocation = idle->map_size; memories[2].size = idle->allocation;
    memcpy(memories[2].map, idle->mirror, idle->map_size); // unused slot's initial download
    connection.perf_enabled = 0;
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
    connection.perf_enabled = 1; connection.wire_version = 6;
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
    connection.wire_version = 7;
    --stage->map_size;
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    ++stage->map_size;
    stage->pool = 0;
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    stage->pool = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_ERROR_INITIALIZATION_FAILED);
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_SUCCESS);
    assert(proxy_DroidDeckStagingMALI(&device, idle, NULL) == VK_SUCCESS);
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_SUCCESS);
    assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_ERROR_INITIALIZATION_FAILED);
    assert(submit() == VK_SUCCESS && submits == 1 && writes == 2 && !stage->staging_command);
    assert(!memcmp(stage->mirror, memories[1].map, stage->map_size));
    // R must not upload either staging map, especially its pending predecessor U.
    pending_memory = stage->id;
    ordinary->mirror[123] ^= 0xff;
    assert(submit() == VK_SUCCESS && submits == 2 && writes == 3 && !reads);
    assert(!memcmp(stage->mirror, memories[1].map, stage->map_size));
    pending_memory = 0; stage->mirror[stage->map_size - 1] ^= 0xff;
    assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_SUCCESS);
    assert(submit() == VK_SUCCESS && submits == 3 && writes == 5);
    // Faulty arming cannot bypass the unchanged broker pending-memory guard.
    pending_memory = stage->id;
    assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_SUCCESS);
    assert(submit() == VK_ERROR_INITIALIZATION_FAILED && submits == 3 && stage->staging_command);
    pending_memory = 0; bad_ack = 3;
    assert(submit() == VK_ERROR_INITIALIZATION_FAILED && submits == 3 && stage->staging_command);
    bad_ack = 0; fail_write = (int)writes + 1;
    assert(submit() == VK_ERROR_DEVICE_LOST && submits == 3 && stage->staging_command);
    fail_write = 0;
    assert(submit() == VK_SUCCESS && submits == 4 && !stage->staging_command);
    assert(!memcmp(ordinary->mirror, memories[0].map, ordinary->map_size));
    assert(!memcmp(stage->mirror, memories[1].map, stage->map_size));
    finish();
}
static VkResult map_native(void *d, void *handle, uint64_t offset, uint64_t size, unsigned flags, void **out) {
    (void)d; assert(!offset && size == 230400 && !flags); *out = handle; return VK_SUCCESS;
}
static void unmap_native(void *d, void *handle) { (void)d; assert(handle == memories[1].handle && !pending_memory); }
static void persistent_map(void) {
    setup(7); mapping(0, 512u * 1024u, 0);
    struct proxy_resource *stage = mapping(1, 230400, 0);
    free(stage->mirror); stage->mirror = NULL;
    stage->allocation = 230400; memories[1].size = 230400;
    memories[1].handle = memories[1].map; memories[1].map = NULL;
    native.interop.MapMemory = map_native; native.interop.UnmapMemory = unmap_native;
    void *pointer = NULL;
    assert(proxy_MapMemory(&device, stage, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS);
    assert(pointer == stage->mirror && memory_maps == 1 && reads == 2 && !memory_unmaps);
    assert(proxy_DroidDeckStagingMALI(&device, stage, NULL) == VK_SUCCESS);
    for (unsigned frame = 0; frame < 40; ++frame) {
        // The caller's pool proved the previous U complete before this full copy.
        assert(!pending_memory);
        memset(pointer, (int)frame, 230400);
        assert(proxy_DroidDeckStagingMALI(&device, stage, &command) == VK_SUCCESS);
        assert(submit() == VK_SUCCESS);
        assert(!memcmp(pointer, memories[1].map, 230400));
        pending_memory = stage->id;
        assert(submit() == VK_SUCCESS); // R leaves this pending mapping untouched
        pending_memory = 0;
    }
    assert(memory_maps == 1 && reads == 2 && !memory_unmaps && writes == 120 && submits == 80);
    proxy_UnmapMemory(&device, stage);
    assert(memory_unmaps == 1 && !stage->mirror && !stage->map_size && !stage->staging_managed && !stage->staging_command);
    assert(!memories[1].map && !atomic_load(&device.submit_failed));
    finish();
}
static void read_bulk(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_READ_MAX * 4u + 19u, 257);
    for (uint64_t n = 0; n < m->map_size; ++n) ((uint8_t *)memories[0].map)[n] = (uint8_t)(n * 17 + 3);
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_SUCCESS);
    assert(reads == 5 && !writes && !memcmp(m->mirror, memories[0].map, (size_t)m->map_size));
    assert(connection.perf.op[MB_MEMORY_READ].count == 5 && connection.perf.download_bytes == m->map_size);
    assert(allocations == 1 && device.read_capacity == MB_PREFIX_BYTES + MB_INTEROP_READ_MAX);
    uint8_t *reply = device.read_reply;
    memset(m->mirror, 0, (size_t)m->map_size);
    assert(interop_copy_mapping(m, m->map_offset + 7, MB_INTEROP_READ_MAX, 0) == VK_SUCCESS);
    assert(reads == 6 && allocations == 1 && device.read_reply == reply);
    assert(!memcmp(m->mirror + 7, (uint8_t *)memories[0].map + 7, MB_INTEROP_READ_MAX));
    assert(!m->mirror[6] && !m->mirror[7 + MB_INTEROP_READ_MAX]);
    // Normal SHM client: 320 * 180 * 4 = 230400 bytes -> 57 old, 2 bulk reads.
    assert((230400u + MB_INTEROP_CHUNK - 1) / MB_INTEROP_CHUNK == 57);
    assert(interop_copy_mapping(m, m->map_offset, 230400, 0) == VK_SUCCESS && reads == 8);
    assert(!memcmp(m->mirror, memories[0].map, 230400));
    assert(interop_copy_mapping(m, m->map_offset + 3, 1, 0) == VK_SUCCESS && reads == 9);
    assert(!memcmp(m->mirror + 3, (uint8_t *)memories[0].map + 3, 1));
    assert(allocations == 1 && device.read_reply == reply);
    finish();
}
static void read_errors(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_READ_MAX + 1u, 64);
    memset(memories[0].map, 0xa7, (size_t)m->map_size);
    uint8_t before = m->mirror[0], last = m->mirror[m->map_size - 1];
    fail_alloc = 1;
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!reads && !device.read_reply && !device.read_capacity && m->mirror[0] == before);
    fail_alloc = 0; assert(interop_copy_mapping(m, m->map_offset, 1, 0) == VK_SUCCESS);
    uint8_t *old_reply = device.read_reply; uint32_t old_capacity = device.read_capacity;
    fail_alloc = 1;
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(reads == 1 && device.read_reply == old_reply && device.read_capacity == old_capacity);
    fail_alloc = 0; reads = 0; memset(&connection.perf, 0, sizeof(connection.perf));
    fail_read = 2;
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_ERROR_DEVICE_LOST);
    assert(reads == 2 && connection.perf.op[MB_MEMORY_READ].count == 2);
    assert(connection.perf.download_bytes == MB_INTEROP_READ_MAX);
    assert(m->mirror[MB_INTEROP_READ_MAX - 1] == 0xa7 && m->mirror[m->map_size - 1] == last);
    fail_read = 0; memset(m->mirror, 0x3c, (size_t)m->map_size);
    for (bad_read = 1; bad_read <= 6; ++bad_read) {
        assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_ERROR_INITIALIZATION_FAILED);
        for (uint64_t n = 0; n < m->map_size; ++n) assert(m->mirror[n] == 0x3c);
    }
    assert(disconnects == 6); bad_read = 0;
    pending = 1; assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_ERROR_INITIALIZATION_FAILED);
    assert(m->mirror[0] == 0x3c); pending = 0;
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_SUCCESS);
    assert(!memcmp(m->mirror, memories[0].map, (size_t)m->map_size)); finish();
}
static void errors(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_WRITE_MAX + 1u, 0);
    m->mirror[m->map_size - 1] = 0xa7;
    fail_alloc = 1; assert(submit() == VK_ERROR_OUT_OF_HOST_MEMORY && !writes && !submits && !device.write_request);
    fail_alloc = 0; assert(interop_copy_mapping(m, 0, 64, 1) == VK_SUCCESS);
    uint8_t *old_request = device.write_request; uint32_t old_capacity = device.write_capacity;
    fail_alloc = 1; assert(submit() == VK_ERROR_OUT_OF_HOST_MEMORY && writes == 1 && !submits);
    assert(device.write_request == old_request && device.write_capacity == old_capacity);
    writes = 0; memset(&connection.perf, 0, sizeof(connection.perf));
    fail_alloc = 0; fail_write = 2; assert(submit() == VK_ERROR_DEVICE_LOST && !submits && writes == 2);
    assert(connection.perf.upload_bytes == MB_INTEROP_WRITE_MAX && connection.perf.op[MB_MEMORY_WRITE].count == 2);
    assert(!memcmp(m->mirror, memories[0].map, MB_INTEROP_WRITE_MAX));
    assert(((uint8_t *)memories[0].map)[MB_INTEROP_WRITE_MAX] == 0);
    fail_write = 0; pending = 1; assert(submit() == VK_ERROR_INITIALIZATION_FAILED && !submits); pending = 0;
    for (bad_ack = 1; bad_ack <= 3; ++bad_ack) assert(submit() == VK_ERROR_INITIALIZATION_FAILED && !submits);
    assert(disconnects == 3); bad_ack = 0; assert(submit() == VK_SUCCESS && submits == 1); finish();
}
static void bounds(void) {
    setup(7); struct proxy_resource *m = mapping(0, MB_INTEROP_WRITE_MAX + 1u, 128);
    const uint64_t offsets[] = {127, 128, 128 + m->map_size, UINT64_MAX - 3, 129};
    const uint64_t sizes[] = {1, 0, 1, 8, m->map_size};
    for (unsigned i = 0; i < 5; ++i) {
        assert(interop_copy_mapping(m, offsets[i], sizes[i], 1) == VK_ERROR_INITIALIZATION_FAILED);
        assert(interop_copy_mapping(m, offsets[i], sizes[i], 0) == VK_ERROR_INITIALIZATION_FAILED);
    }
    assert(!writes && !reads && !allocations);
    assert(!mb_interop_mapped_range(UINT64_MAX, UINT64_MAX - 3, 8, UINT64_MAX - 3, 1));
    assert(mb_interop_mapped_range(UINT64_MAX, UINT64_MAX - 3, 3, UINT64_MAX - 2, 2));
    assert(mb_interop_mapped_range(64u * 1024u * 1024u, 0, 64u * 1024u * 1024u, 0, 64u * 1024u * 1024u));
    uint8_t *w = calloc(1, MB_MEMORY_WRITE_HEADER + MB_INTEROP_WRITE_MAX + 1u), reply[MB_INTEROP_CHUNK]; assert(w);
    mb_put_u32(w, device.id); mb_put_u32(w + 4, m->id); mb_put_u64(w + 8, 128); mb_put_u32(w + 16, 1);
    uint32_t extra = 0, count = 0; VkResult result = VK_SUCCESS;
#define REJECT(OP, BYTES) assert(native_transfer(7, OP, w, BYTES, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR)
    REJECT(MB_MEMORY_WRITE, 19); REJECT(MB_MEMORY_WRITE, 20); REJECT(MB_MEMORY_WRITE, 22);
    mb_put_u32(w + 16, 0); REJECT(MB_MEMORY_WRITE, 20);
    mb_put_u32(w + 16, MB_INTEROP_WRITE_MAX + 1u); REJECT(MB_MEMORY_WRITE, MB_MEMORY_WRITE_HEADER + MB_INTEROP_WRITE_MAX + 1u);
    mb_put_u32(w + 16, UINT32_MAX); REJECT(MB_MEMORY_WRITE, 20);
    mb_put_u32(w + 16, 8); mb_put_u64(w + 8, UINT64_MAX - 3); REJECT(MB_MEMORY_WRITE, 28);
    mb_put_u32(w + 16, 1); mb_put_u64(w + 8, 127); REJECT(MB_MEMORY_WRITE, 21);
    mb_put_u64(w + 8, m->map_offset + m->map_size); REJECT(MB_MEMORY_WRITE, 21);
    mb_put_u64(w + 8, 128); pending = 1; REJECT(MB_MEMORY_WRITE, 21); pending = 0;
    memories[0].ahb = 1; REJECT(MB_MEMORY_WRITE, 21); memories[0].ahb = 0;
    native.interop.properties.memoryTypes[0].propertyFlags = 0; REJECT(MB_MEMORY_WRITE, 21);
    native.interop.properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    void *map = memories[0].map; memories[0].map = NULL; REJECT(MB_MEMORY_WRITE, 21); memories[0].map = map;
    mb_put_u32(w + 4, 99); REJECT(MB_MEMORY_WRITE, 21); mb_put_u32(w + 4, m->id);
    mb_put_u32(w + 16, MB_INTEROP_READ_MAX + 1u); REJECT(MB_MEMORY_READ, 20);
    mb_put_u32(w + 16, UINT32_MAX); REJECT(MB_MEMORY_READ, 20);
    mb_put_u32(w + 16, 0); REJECT(MB_MEMORY_READ, 20);
    mb_put_u32(w + 16, 1); REJECT(MB_MEMORY_READ, 19); REJECT(MB_MEMORY_READ, 21);
    mb_put_u64(w + 8, UINT64_MAX - 3); mb_put_u32(w + 16, 8); REJECT(MB_MEMORY_READ, 20);
    mb_put_u64(w + 8, 127); mb_put_u32(w + 16, 1); REJECT(MB_MEMORY_READ, 20);
    mb_put_u64(w + 8, m->map_offset + m->map_size); REJECT(MB_MEMORY_READ, 20);
    mb_put_u64(w + 8, 128); pending = 1; REJECT(MB_MEMORY_READ, 20); pending = 0;
    memories[0].ahb = 1; REJECT(MB_MEMORY_READ, 20); memories[0].ahb = 0;
    native.interop.properties.memoryTypes[0].propertyFlags = 0; REJECT(MB_MEMORY_READ, 20);
    native.interop.properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    memories[0].map = NULL; REJECT(MB_MEMORY_READ, 20); memories[0].map = map;
    mb_put_u32(w + 4, 99); REJECT(MB_MEMORY_READ, 20); mb_put_u32(w + 4, m->id);
    // Allocation and mapped-range checks are independent.
    uint64_t allocation = memories[0].size; memories[0].size = 128; REJECT(MB_MEMORY_READ, 20);
    memories[0].size = allocation;
#undef REJECT
    free(w); finish();
}
static void legacy(void) {
    setup(6); struct proxy_resource *m = mapping(0, MB_INTEROP_WRITE_MAX, 64);
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 1) == VK_SUCCESS && writes == 128);
    assert(!memcmp(m->mirror, memories[0].map, (size_t)m->map_size));
    memset(m->mirror, 0, (size_t)m->map_size);
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_SUCCESS);
    assert(connection.perf.op[MB_MEMORY_READ].count == 128 && !memcmp(m->mirror, memories[0].map, (size_t)m->map_size));
    connection.wire_version = 7; memset(&connection.perf, 0, sizeof(connection.perf));
    memset(m->mirror, 0, (size_t)m->map_size);
    assert(interop_copy_mapping(m, m->map_offset, m->map_size, 0) == VK_SUCCESS);
    assert(connection.perf.op[MB_MEMORY_READ].count == 5 && !memcmp(m->mirror, memories[0].map, (size_t)m->map_size));
    uint8_t w[20] = {0}, reply[1]; uint32_t extra = 0, count = 0; VkResult result = VK_SUCCESS;
    mb_put_u32(w, device.id); mb_put_u32(w + 4, m->id); mb_put_u64(w + 8, 64); mb_put_u32(w + 16, MB_INTEROP_CHUNK + 1u);
    assert(native_transfer(6, MB_MEMORY_WRITE, w, 20u + MB_INTEROP_CHUNK + 1u, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    assert(native_transfer(6, MB_MEMORY_READ, w, 20, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    // Old v7 small read requests remain valid too (no oversized response).
    mb_put_u32(w + 16, 1);
    assert(native_transfer(7, MB_MEMORY_READ, w, 20, reply, &extra, &count, &result) == MB_OK && extra == 1 && count == 1);
    connection.wire_version = 5;
    assert(interop_copy_mapping(m, m->map_offset, 1, 0) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!mb_interop_write_size(7, UINT32_MAX, UINT32_MAX - 20u)); finish();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "bulk")) bulk();
    else if (!strcmp(argv[1], "read_bulk")) read_bulk();
    else if (!strcmp(argv[1], "read_errors")) read_errors();
    else if (!strcmp(argv[1], "ordering")) ordering();
    else if (!strcmp(argv[1], "persistent_staging")) persistent_staging();
    else if (!strcmp(argv[1], "persistent_map")) persistent_map();
    else if (!strcmp(argv[1], "errors")) errors();
    else if (!strcmp(argv[1], "bounds")) bounds();
    else if (!strcmp(argv[1], "legacy")) legacy();
    else return 1;
    printf("%s PASS\n", argv[1]); return 0;
}
