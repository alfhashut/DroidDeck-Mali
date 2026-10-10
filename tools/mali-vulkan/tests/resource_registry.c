/* Only the actual small registry/create/destroy helpers are compiled here. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
#define MB_FEATURE(name) uint32_t name;
#include "features_fields.def"
#undef MB_FEATURE
} VkPhysicalDeviceFeatures;
#include "protocol.h"
#include "submit_protocol.h"
#include "interop_protocol.h"
#include "renderer_protocol.h"
#include "normal_memory_perf.h"
typedef int VkResult;
typedef uintptr_t VK_LOADER_DATA;
typedef void *VkDevice, *VkCommandPool, *VkCommandBuffer, *VkEvent, *VkFence;
typedef void *VkBuffer, *VkImage, *VkDeviceMemory, *VkImageView, *VkDescriptorPool, *VkDescriptorSet;
typedef int VkAllocationCallbacks;
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#define LOG(...) ((void)0)
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_FEATURE_NOT_PRESENT (-8)
#define VK_NULL_HANDLE NULL
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO 34
#define VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT 8
#include "interop_test_api.h"
typedef struct { unsigned sType; const void *pNext; VkDescriptorPool descriptorPool; uint32_t descriptorSetCount; const void **pSetLayouts; } VkDescriptorSetAllocateInfo;
static unsigned allocated, freed, allocation_calls, fail_allocation;
static void *tracked_calloc(size_t n, size_t size) {
    if (++allocation_calls==fail_allocation) return NULL;
    void *p=calloc(n, size); assert(p); ++allocated; return p;
}
static void tracked_free(void *p) { if (p) { ++freed; free(p); } }
#define calloc tracked_calloc
#define free tracked_free
#include "resource_registry.h"
struct proxy_instance { pthread_mutex_t lock; int perf_enabled, fd; uint32_t wire_version; struct dd_memory_profile memory_perf; } connection;
struct proxy_logical { struct proxy_instance *owner; struct proxy_registry registry; int fd;
    uint8_t *write_request, *read_reply; uint32_t write_capacity, read_capacity; } device;
static unsigned rpc_calls, next_id=1, errors, bad_ids, disconnected;
static VkResult rpc_result;
static void set_loader_magic_value(struct proxy_resource *o) { o->loader=1234; }
static VkResult submit_rpc(struct proxy_logical *d, uint32_t op, const uint32_t *args, unsigned n, uint64_t timeout, uint32_t *id) {
    (void)d; (void)op; (void)args; (void)n; (void)timeout; ++rpc_calls;
    if (rpc_result==VK_SUCCESS && id) *id=next_id++;
    return rpc_result;
}
static VkResult interop_rpc(struct proxy_logical *d, uint32_t op, const uint8_t *args, uint32_t n, uint8_t *out, uint32_t expected) {
    (void)d; (void)op; (void)args; (void)n; ++rpc_calls;
    if (rpc_result==VK_SUCCESS) for (unsigned i=0; i<expected; i+=4) mb_put_u32(out+i, bad_ids ? 0 : next_id++);
    return rpc_result;
}
#define renderer_rpc interop_rpc
static void submit_void_error(struct proxy_logical *d, const char *name, VkResult r) {
    (void)d; (void)name; errors+=r!=VK_SUCCESS;
}
static int test_shutdown(int fd, int how) { (void)fd; (void)how; ++disconnected; return 0; }
#define shutdown test_shutdown
#include "lifecycle.inc"

static void setup(void) {
    assert(!pthread_mutex_init(&connection.lock, NULL));
    connection.perf_enabled=1; device.owner=&connection;
}
static struct proxy_resource *create(enum proxy_resource_kind kind) {
    struct proxy_resource *o=NULL; uint32_t args[]={77,0,0};
    assert(submit_new(&device, MB_FENCE_CREATE, kind, args, 3, &o)==VK_SUCCESS && o);
    return o;
}
static void check_invariants(void) {
    size_t active=0, indexed=0;
    for (struct proxy_resource *o=device.registry.active, *prev=NULL; o; prev=o, o=o->active_next) {
        assert(o->live && o->owner==&device && o->active_prev==prev && o->index_link && *o->index_link==o);
        ++active;
    }
    for (unsigned i=0; i<PROXY_RESOURCE_BUCKETS; ++i)
        for (struct proxy_resource *o=device.registry.index[i]; o; o=o->index_next) { assert(o->live); ++indexed; }
    assert(active==device.registry.active_count && indexed==device.registry.indexed_count && active==indexed);
}
static void finish(void) {
    check_invariants(); proxy_free_resources(&device);
    assert(!device.registry.owned && !device.registry.active && !device.registry.owned_count);
    assert(!device.registry.indexed_count && !device.registry.active_count);
    for (unsigned i=0; i<PROXY_RESOURCE_BUCKETS; ++i) assert(!device.registry.index[i]);
    assert(allocated==freed);
    proxy_free_resources(&device); assert(allocated==freed);
    assert(!pthread_mutex_destroy(&connection.lock));
}
static void stress(void) {
    setup(); struct proxy_resource *stable[76];
    uint64_t baseline_probes[76];
    for (unsigned i=0; i<76; ++i) stable[i]=create(PROXY_BUFFER);
    for (unsigned i=0; i<76; ++i) {
        uint64_t before=connection.memory_perf.visited;
        assert(submit_find(&device, (uintptr_t)stable[i], PROXY_BUFFER)==stable[i]);
        baseline_probes[i]=connection.memory_perf.visited-before;
    }
    for (unsigned frame=0; frame<10000; ++frame) {
        struct proxy_resource *o=create(PROXY_IMAGE);
        assert(submit_find(&device, (uintptr_t)o, PROXY_IMAGE)==o);
        assert(!submit_find(&device, (uintptr_t)o, PROXY_MEMORY));
        struct proxy_logical other={.owner=&connection};
        assert(!submit_find(&other, (uintptr_t)o, PROXY_IMAGE));
        submit_retire(&device, o);
        assert(!submit_find(&device, (uintptr_t)o, PROXY_IMAGE) && !o->live);
        assert(o->owner==&device && o->loader==1234); // No reclaimed dangling storage.
        next_id=o->id; // Simulate broker ID reuse; local pointer handles must differ.
        struct proxy_resource *replacement=create(PROXY_IMAGE);
        assert(replacement!=o && replacement->id==o->id);
        assert(submit_find(&device, (uintptr_t)replacement, PROXY_IMAGE)==replacement);
        assert(!submit_find(&device, (uintptr_t)o, PROXY_IMAGE));
        submit_retire(&device, replacement);
        for (unsigned i=0; i<76; ++i) {
            uint64_t before=connection.memory_perf.visited;
            assert(submit_find(&device, (uintptr_t)stable[i], PROXY_BUFFER)==stable[i]);
            assert(connection.memory_perf.visited-before==baseline_probes[i]);
        }
        assert(device.registry.active_count==76 && device.registry.indexed_count==76);
        assert(connection.memory_perf.max_visit<=77);
        if (!(frame%1000)) check_invariants();
    }
    assert(device.registry.owned_count==20076);
    assert(!submit_find(&device, (uintptr_t)1, PROXY_IMAGE)); // No incoming-handle dereference.
    finish();
}
static void collisions(void) {
    setup(); struct proxy_resource *chain[3]={0}; unsigned count=0;
    size_t bucket=0;
    while (count<3) {
        struct proxy_resource *o=create(PROXY_BUFFER);
        if (!count) bucket=proxy_resource_bucket((uintptr_t)o);
        if (proxy_resource_bucket((uintptr_t)o)==bucket) chain[count++]=o;
        else submit_retire(&device, o);
    }
    // Intrusive index back-links must survive middle, head and tail removal.
    submit_retire(&device, chain[1]); check_invariants();
    assert(submit_find(&device, (uintptr_t)chain[0], PROXY_BUFFER)==chain[0]);
    assert(submit_find(&device, (uintptr_t)chain[2], PROXY_BUFFER)==chain[2]);
    submit_retire(&device, chain[2]); submit_retire(&device, chain[0]);
    submit_retire(&device, chain[0]); assert(!device.registry.active_count);
    finish();
}
static void lifecycle(void) {
    setup(); struct proxy_resource *pool=create(PROXY_POOL), *cmd=create(PROXY_COMMAND), *other=create(PROXY_COMMAND);
    cmd->pool=pool->id; other->pool=pool->id+1;
    assert(interop_command(cmd)==cmd);
    rpc_result=VK_ERROR_DEVICE_LOST; proxy_DestroyCommandPool(&device, pool, NULL);
    assert(pool->live && cmd->live && other->live); // Native rejection/pending use does not retire.
    rpc_result=VK_SUCCESS; proxy_DestroyCommandPool(&device, pool, NULL);
    assert(!pool->live && !cmd->live && other->live && !interop_command(cmd));
    struct proxy_resource *p2=create(PROXY_POOL); other->pool=p2->id;
    VkCommandBuffer handle=other;
    proxy_FreeCommandBuffers(&device, p2, 1, &handle); assert(!other->live);
    struct proxy_resource *dp=create(PROXY_DESCRIPTOR_POOL), *set=create(PROXY_DESCRIPTOR_SET), *unrelated=create(PROXY_DESCRIPTOR_SET);
    set->pool=dp->id; unrelated->pool=dp->id+1;
    proxy_DestroyDescriptorPool(&device, dp, NULL); assert(!set->live && !dp->live && unrelated->live);
    struct proxy_resource *view=create(PROXY_VIEW); proxy_DestroyImageView(&device, view, NULL); assert(!view->live);
    struct proxy_resource *buffer=create(PROXY_BUFFER); proxy_DestroyBuffer(&device, buffer, NULL); assert(!buffer->live);
    struct proxy_resource *image=create(PROXY_IMAGE); proxy_DestroyImage(&device, image, NULL); assert(!image->live);
    struct proxy_resource *memory=create(PROXY_MEMORY); memory->mirror=calloc(1, 16);
    unsigned before=rpc_calls; proxy_FreeMemory(&device, memory, NULL); assert(memory->live && rpc_calls==before);
    free(memory->mirror); memory->mirror=NULL; proxy_FreeMemory(&device, memory, NULL); assert(!memory->live);
    struct proxy_resource *event=create(PROXY_EVENT), *fence=create(PROXY_FENCE);
    assert(proxy_GetEventStatus(&device, event)==VK_SUCCESS && proxy_GetFenceStatus(&device, fence)==VK_SUCCESS);
    proxy_DestroyEvent(&device, event, NULL); proxy_DestroyFence(&device, fence, NULL);
    assert(proxy_GetEventStatus(&device, event)==VK_ERROR_INITIALIZATION_FAILED && proxy_GetFenceStatus(&device, fence)==VK_ERROR_INITIALIZATION_FAILED);
    before=rpc_calls; proxy_DestroyImage(&device, image, NULL); assert(rpc_calls==before);
    assert(errors>=3); finish();
}
static void errors_case(void) {
    setup(); struct proxy_resource *out=(void *)1; uint32_t args[]={1};
    unsigned before=rpc_calls;
    fail_allocation=allocation_calls+1;
    assert(submit_new(&device, MB_FENCE_CREATE, PROXY_FENCE, args, 1, &out)==VK_ERROR_OUT_OF_HOST_MEMORY && !out && rpc_calls==before);
    fail_allocation=0; rpc_result=VK_ERROR_DEVICE_LOST;
    assert(submit_new(&device, MB_FENCE_CREATE, PROXY_FENCE, args, 1, &out)==rpc_result && !out);
    assert(interop_new(&device, MB_IMAGE_CREATE, PROXY_IMAGE, NULL, 0, &out)==rpc_result && !out);
    assert(renderer_new(&device, MB_RENDERER_VIEW, PROXY_VIEW, NULL, 0, &out)==rpc_result && !out);
    assert(!device.registry.owned_count && allocated==freed);
    rpc_result=VK_SUCCESS; bad_ids=1;
    assert(interop_new(&device, MB_IMAGE_CREATE, PROXY_IMAGE, NULL, 0, &out)==VK_ERROR_INITIALIZATION_FAILED && !out);
    assert(renderer_new(&device, MB_RENDERER_VIEW, PROXY_VIEW, NULL, 0, &out)==VK_ERROR_INITIALIZATION_FAILED && !out);
    assert(disconnected==2 && !device.registry.owned_count && allocated==freed);
    bad_ids=0;
    assert(interop_new(&device, MB_IMAGE_CREATE, PROXY_IMAGE, NULL, 0, &out)==VK_SUCCESS && submit_find(&device, (uintptr_t)out, PROXY_IMAGE)==out);
    assert(renderer_new(&device, MB_RENDERER_VIEW, PROXY_VIEW, NULL, 0, &out)==VK_SUCCESS && submit_find(&device, (uintptr_t)out, PROXY_VIEW)==out);
    struct proxy_resource *pool=create(PROXY_DESCRIPTOR_POOL), *layout=create(PROXY_SET_LAYOUT);
    const void *layouts[]={layout,layout}; void *sets[2]={0};
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=2,.pSetLayouts=layouts};
    size_t owned=device.registry.owned_count;
    fail_allocation=allocation_calls+2;
    assert(proxy_AllocateDescriptorSets(&device, &ai, sets)==VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(device.registry.owned_count==owned && device.registry.active_count==owned);
    fail_allocation=0; bad_ids=1;
    assert(proxy_AllocateDescriptorSets(&device, &ai, sets)==VK_ERROR_INITIALIZATION_FAILED && device.registry.owned_count==owned);
    bad_ids=0; assert(proxy_AllocateDescriptorSets(&device, &ai, sets)==VK_SUCCESS);
    assert(device.registry.active_count==owned+2);
    proxy_DestroyDescriptorPool(&device, pool, NULL);
    assert(!submit_find(&device, (uintptr_t)sets[0], PROXY_DESCRIPTOR_SET) && !submit_find(&device, (uintptr_t)sets[1], PROXY_DESCRIPTOR_SET));
    finish();
}
static void tokens(void) {
    setup(); struct dd_interop_input input={0}; struct dd_interop_output out={0};
    fail_allocation=allocation_calls+2;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_CREATE, &input, &out)==VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!device.registry.owned_count && allocated==freed);
    fail_allocation=0; bad_ids=1;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_CREATE, &input, &out)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!device.registry.owned_count && allocated==freed);
    bad_ids=0;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_CREATE, &input, &out)==VK_SUCCESS);
    uint32_t token=out.token; assert(device.registry.active_count==3);
    assert(proxy_DroidDeckInteropTEST(&device, DD_FENCE_CREATE, &input, &out)==VK_SUCCESS);
    input.fence=out.fence;
    assert(proxy_DroidDeckInteropTEST(&device, DD_SYNC_EXPORT, &input, &out)==VK_SUCCESS);
    input.sync=out.sync; input.token=token;
    assert(proxy_DroidDeckInteropTEST(&device, DD_SYNC_WAIT, &input, &out)==VK_SUCCESS);
    size_t live=device.registry.active_count;
    rpc_result=VK_ERROR_DEVICE_LOST;
    assert(proxy_DroidDeckInteropTEST(&device, DD_SYNC_CLOSE, &input, &out)==rpc_result);
    assert(device.registry.active_count==live);
    rpc_result=VK_SUCCESS;
    assert(proxy_DroidDeckInteropTEST(&device, DD_SYNC_CLOSE, &input, &out)==VK_SUCCESS);
    assert(device.registry.active_count==live-1);
    unsigned before=rpc_calls;
    assert(proxy_DroidDeckInteropTEST(&device, DD_SYNC_WAIT, &input, &out)==VK_ERROR_INITIALIZATION_FAILED && rpc_calls==before);
    rpc_result=VK_ERROR_DEVICE_LOST;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_RELEASE, &input, &out)==rpc_result);
    assert(device.registry.active_count==live-1);
    rpc_result=VK_SUCCESS;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_RELEASE, &input, &out)==VK_SUCCESS);
    assert(device.registry.active_count==live-2 && device.registry.owned_count==live);
    before=rpc_calls;
    assert(proxy_DroidDeckInteropTEST(&device, DD_AHB_RELEASE, &input, &out)==VK_ERROR_INITIALIZATION_FAILED && rpc_calls==before);
    finish();
}
int main(int argc, char **argv) {
    assert(argc==2);
    if (!strcmp(argv[1], "stress")) stress();
    else if (!strcmp(argv[1], "collisions")) collisions();
    else if (!strcmp(argv[1], "lifecycle")) lifecycle();
    else if (!strcmp(argv[1], "errors")) errors_case();
    else if (!strcmp(argv[1], "tokens")) tokens();
    else return 1;
    printf("%s PASS\n", argv[1]); return 0;
}
