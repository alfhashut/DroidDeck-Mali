/* Only actual wait-case/proxy helpers, with fake clocks and Vulkan callbacks. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static uint64_t wall = 1000000000, cpu = 100000000;
static int fake_clock(clockid_t id, struct timespec *out) {
    uint64_t ns = id == CLOCK_THREAD_CPUTIME_ID ? cpu : wall;
    out->tv_sec = ns / 1000000000; out->tv_nsec = ns % 1000000000; return 0;
}
#define clock_gettime fake_clock
#include "protocol.h"
#include "renderer_protocol.h"
#include "interop_protocol.h"
#include "normal_opcode_perf.h"
#define MB_SUBMIT_TIMEOUT_NS UINT64_C(5000000000)
#define MB_SUBMIT_MAX_OBJECTS 32u
#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_FEATURE_NOT_PRESENT (-8)
#define VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR 1000207004
typedef int VkResult;
typedef void *VkDevice;
typedef uintptr_t VkSemaphore;
typedef struct {
    unsigned sType; const void *pNext; unsigned flags, semaphoreCount;
    const VkSemaphore *pSemaphores; const uint64_t *pValues;
} VkSemaphoreWaitInfoKHR;
typedef VkResult (*PFN_vkGetSemaphoreCounterValueKHR)(VkDevice, VkSemaphore, uint64_t *);
typedef VkResult (*PFN_vkWaitSemaphoresKHR)(VkDevice, const VkSemaphoreWaitInfoKHR *, uint64_t);
static uint64_t counter_value = 41, wait_wall = 2000000, wait_cpu = 100000, expected_target = 42;
static int counter_result, wait_result, wait_calls, counter_calls, completed, disconnects, bad_reply;
static VkResult counter_call(VkDevice device, VkSemaphore sem, uint64_t *value) {
    assert(device == (void *)1 && sem == 123); ++counter_calls;
    wall += 100000; cpu += 20000; *value = counter_value; return counter_result;
}
static VkResult wait_call(VkDevice device, const VkSemaphoreWaitInfoKHR *info, uint64_t timeout) {
    assert(device == (void *)1 && info->pSemaphores[0] == 123 && info->pValues[0] == expected_target);
    assert(info->semaphoreCount == 1 && !info->flags && !info->pNext && timeout == MB_SUBMIT_TIMEOUT_NS);
    ++wait_calls; wall += wait_wall; cpu += wait_cpu;
    if (wait_result == VK_SUCCESS) counter_value = expected_target;
    return wait_result;
}
#include "native_wait.inc"
struct native_command { unsigned id, state, semaphore; uint64_t signal_value; };
struct native_renderer_object { uint32_t id; struct { VkSemaphore semaphore; } handle; uint64_t last_signal; } native_sem = {10, {123}, 42};
struct native_renderer { PFN_vkGetSemaphoreCounterValueKHR GetSemaphoreCounterValueKHR; PFN_vkWaitSemaphoresKHR WaitSemaphoresKHR; };
struct native_buffer { uint32_t id; } native_buffer = {20};
struct native_device {
    VkDevice handle; struct native_renderer renderer;
    struct { unsigned enabled; uint64_t native_ns[4], native_calls[4]; struct dd_wait_producer wait_history[DD_WAIT_HISTORY]; } normal;
    struct { struct native_command commands[MB_SUBMIT_MAX_OBJECTS]; } submit;
} native;
static struct native_renderer_object *renderer_find(struct native_device *d, uint32_t id, unsigned kind) {
    assert(d == &native && kind == MB_R_SEMAPHORE); return id == native_sem.id ? &native_sem : NULL;
}
static struct native_buffer *interop_find_buffer(struct native_device *d, uint32_t id) { assert(d == &native); return id == native_buffer.id ? &native_buffer : NULL; }
static void native_renderer_complete(struct native_device *d, struct native_command *c) { (void)d; (void)c; ++completed; }
static uint64_t native_perf_start(struct native_device *d) { (void)d; return dd_perf_now(); }
static void native_perf_end(struct native_device *d, unsigned k, uint64_t start) { d->normal.native_ns[k] += dd_perf_now() - start; ++d->normal.native_calls[k]; }
static uint32_t native_request(unsigned version, uint32_t op, const uint8_t *w, unsigned bytes,
        uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    if (bytes < 8 || mb_get_u32(w) != 1) return MB_PROTOCOL_ERROR;
    struct { unsigned wire_version; } session = {version}, *s = &session;
    struct native_device *d = &native; struct native_renderer *v = &d->renderer; uint32_t id = mb_get_u32(w + 4);
#define SIZE(n) do { if (bytes != (unsigned)(n)) return MB_PROTOCOL_ERROR; } while (0)
#define REQUIRE(ok) do { if (!(ok)) return MB_PROTOCOL_ERROR; } while (0)
    switch (op) {
#include "native_case.inc"
    default: return MB_PROTOCOL_ERROR;
    }
#undef REQUIRE
#undef SIZE
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
struct proxy_instance { pthread_mutex_t lock; int fd, perf_enabled; unsigned wire_version; struct dd_perf_rpc perf; } connection;
struct proxy_logical { struct proxy_instance *owner; unsigned id; } device;
enum proxy_resource_kind { PROXY_SEMAPHORE, PROXY_BUFFER };
struct proxy_resource { unsigned id; enum proxy_resource_kind kind; } proxy_sem = {10, PROXY_SEMAPHORE}, proxy_buffer = {20, PROXY_BUFFER};
static struct proxy_resource *submit_find(struct proxy_logical *d, uintptr_t handle, enum proxy_resource_kind kind) {
    assert(d == &device); struct proxy_resource *p = (void *)handle;
    return (p == &proxy_sem || p == &proxy_buffer) && p->kind == kind ? p : NULL;
}
static int fake_shutdown(int fd, int how) { (void)fd; assert(how == SHUT_RDWR); ++disconnects; return 0; }
#define shutdown fake_shutdown
static VkResult rpc_exchange(struct proxy_instance *s, uint32_t op, const uint8_t *request,
        uint32_t bytes, uint8_t *reply, uint32_t *reply_bytes, uint32_t capacity) {
    assert(pthread_mutex_trylock(&s->lock) != 0);
    wall += 300000; cpu += 20000;
    uint32_t extra = 0, count = 0; VkResult result = VK_SUCCESS;
    uint32_t status = op == MB_RENDERER_DESTROY ? MB_OK : native_request(s->wire_version, op, request, bytes, reply + MB_PREFIX_BYTES, &extra, &count, &result);
    if (status != MB_OK) { extra = 0; count = 0; }
    assert(MB_PREFIX_BYTES + extra <= capacity);
    mb_put_u32(reply, status); mb_put_u32(reply + 4, (uint32_t)result); mb_put_u32(reply + 8, count);
    *reply_bytes = MB_PREFIX_BYTES + extra;
    wall += 200000; cpu += 20000;
    if (bad_reply == 1) *reply_bytes = 0;
    if (bad_reply == 2) --*reply_bytes;
    if (bad_reply == 3) ++*reply_bytes;
    if (bad_reply == 4) mb_put_u32(reply + 8, 0);
    return status == MB_OK || status == MB_VULKAN_ERROR ? result : VK_ERROR_INITIALIZATION_FAILED;
}
#include "metric.inc"
#include "proxy_wait.inc"
static VkResult profile(unsigned reason, uint64_t resource) {
    VkSemaphore sem = (uintptr_t)&proxy_sem;
    VkSemaphoreWaitInfoKHR info = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR, NULL, 0, 1, &sem, &expected_target};
    return proxy_DroidDeckProfiledWaitMALI(&device, &info, MB_SUBMIT_TIMEOUT_NS, reason, resource);
}
int main(void) {
    assert(!pthread_mutex_init(&connection.lock, NULL)); connection.wire_version = 7; connection.perf_enabled = 1;
    device.owner = &connection; device.id = 1; native.handle = (void *)1;
    native.renderer.GetSemaphoreCounterValueKHR = counter_call; native.renderer.WaitSemaphoresKHR = wait_call; native.normal.enabled = 1;
    native.normal.wait_history[0] = (struct dd_wait_producer){42, 10, 8, 30, 20, 21, 31, 40};
    native.submit.commands[0] = (struct native_command){8, 3, 10, 42};
    assert(profile(DD_WAIT_SHM_STAGING_DESTROY, (uintptr_t)&proxy_buffer) == VK_SUCCESS);
    struct dd_wait_stats *p = &connection.perf.wait[DD_WAIT_SHM_STAGING_DESTROY];
    assert(wait_calls == 1 && counter_calls == 1 && completed == 1 && native.submit.commands[0].state == 4);
    assert(p->count == 1 && p->material == 1 && p->last.before == 41 && p->target == 42 && p->detail == 20);
    assert(p->rtt_ns == 2600000 && p->wall_ns == 2000000 && p->cpu_ns == 100000 && p->off_cpu_ns == 1900000 && p->query_ns == 100000);
    assert(p->last.producer_found && p->last.command == 8 && p->last.image == 30 && p->last.buffer == 20 && p->last.memory == 21 && p->last.descriptor_set == 31 && p->last.fence == 40);
    assert(connection.perf.op[MB_RENDERER_WAIT].count == 1 && connection.perf.op[MB_RENDERER_WAIT].ns == p->rtt_ns);
    // Already satisfied still calls the real wait, even with large driver wall time.
    assert(profile(DD_WAIT_DESCRIPTOR_REUSE, 2) == VK_SUCCESS);
    assert(connection.perf.wait[DD_WAIT_DESCRIPTOR_REUSE].satisfied == 1 && wait_calls == 2);
    counter_value = 41; wait_wall = 100000; wait_cpu = 20000;
    assert(profile(DD_WAIT_OUTPUT_COMPLETION, 0) == VK_SUCCESS && connection.perf.wait[DD_WAIT_OUTPUT_COMPLETION].brief == 1);
    counter_result = VK_ERROR_DEVICE_LOST;
    assert(profile(DD_WAIT_OUTPUT_RETIRE, 0) == VK_SUCCESS && connection.perf.wait[DD_WAIT_OUTPUT_RETIRE].unknown == 1);
    counter_result = 0; wait_result = VK_TIMEOUT; native.submit.commands[0].state = 3;
    assert(profile(DD_WAIT_OUTPUT_RETIRE, 0) == VK_TIMEOUT && native.submit.commands[0].state == 3);
    wait_result = VK_ERROR_DEVICE_LOST;
    assert(profile(DD_WAIT_OUTPUT_RETIRE, 0) == VK_ERROR_DEVICE_LOST && native.submit.commands[0].state == 3);
    wait_result = VK_SUCCESS;
    for (bad_reply = 1; bad_reply <= 4; ++bad_reply) assert(profile(DD_WAIT_OUTPUT_RETIRE, 0) == VK_ERROR_INITIALIZATION_FAILED);
    assert(disconnects == 4); bad_reply = 0;
    // Native history survives command retirement/reset; unknown history is explicit.
    memset(native.submit.commands, 0, sizeof(native.submit.commands));
    assert(profile(DD_WAIT_OUTPUT_RETIRE, 0) == VK_SUCCESS && connection.perf.wait[DD_WAIT_OUTPUT_RETIRE].last.producer_found);
    expected_target = 100;
    assert(profile(DD_WAIT_UNATTRIBUTED, 0) == VK_SUCCESS && !connection.perf.wait[DD_WAIT_UNATTRIBUTED].last.producer_found);
    // Legacy request: no measurement query, one unchanged wait, empty reply.
    uint8_t request[32] = {0}, reply[DD_WAIT_SAMPLE_BYTES]; uint32_t extra = 0, count = 0; VkResult result = 0;
    mb_put_u32(request, 1); mb_put_u32(request + 4, 10); mb_put_u64(request + 8, expected_target); mb_put_u64(request + 16, MB_SUBMIT_TIMEOUT_NS);
    int queries = counter_calls, calls = wait_calls;
    assert(native_request(7, MB_RENDERER_WAIT, request, 24, reply, &extra, &count, &result) == MB_OK);
    assert(wait_calls == calls + 1 && counter_calls == queries && !extra && !count);
    mb_put_u32(request + 24, DD_WAIT_REASONS);
    assert(native_request(7, MB_RENDERER_WAIT, request, 32, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    mb_put_u32(request + 24, 0);
    assert(native_request(6, MB_RENDERER_WAIT, request, 32, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    assert(native_request(7, MB_RENDERER_WAIT, request, 31, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    native.normal.enabled = 0;
    assert(native_request(7, MB_RENDERER_WAIT, request, 32, reply, &extra, &count, &result) == MB_PROTOCOL_ERROR);
    assert(wait_calls == calls + 1 && counter_calls == queries); native.normal.enabled = 1;
    connection.perf_enabled = 0;
    assert(profile(DD_WAIT_OUTPUT_COMPLETION, 0) == VK_ERROR_FEATURE_NOT_PRESENT);
    connection.perf_enabled = 1;
    assert(profile(DD_WAIT_DESCRIPTOR_REUSE, UINT64_MAX) == VK_ERROR_INITIALIZATION_FAILED);
    // Explicit wire round trip includes 64-bit values without truncation.
    struct dd_wait_sample sample = {0}, decoded = {0}; sample.before = UINT64_MAX - 2; sample.wall_ns = UINT64_MAX - 3; sample.command = UINT32_MAX; sample.before_result = (uint32_t)-4;
    dd_wait_encode(reply, &sample); dd_wait_decode(reply, &decoded); assert(!memcmp(&sample, &decoded, sizeof(sample)));
    struct dd_perf_rpc frame = {0}, outside = {0}, merged = {0};
    dd_perf_merge(&frame, &connection.perf); dd_perf_merge(&merged, &frame);
    assert(merged.wait[1].off_cpu_ns == 1900000 && merged.wait[1].count == 1);
    for (unsigned i = 0; i < DD_WAIT_REASONS; ++i) dd_wait_report(stdout, i, &frame.wait[i], &outside.wait[i], 1);
    // FrameInfo keeps source alive: 12 frame management calls, four outside.
    memset(&frame, 0, sizeof(frame));
    unsigned ops[] = {32, 34, 35, 78, 46, 35, 64, 64, 54, 33, 36, 28};
    for (unsigned i = 0; i < sizeof(ops)/sizeof(ops[0]); ++i) dd_perf_add(&frame, ops[i], 1);
    dd_perf_add(&outside, 61, 1); dd_perf_add(&outside, 61, 1); dd_perf_add(&outside, 45, 1); dd_perf_add(&outside, 36, 1);
    uint8_t destroy[12] = {0}, ack[12]; uint32_t ack_bytes = 0; mb_put_u32(destroy + 4, MB_R_VIEW);
    pthread_mutex_lock(&connection.lock);
    assert(rpc(&connection, MB_RENDERER_DESTROY, destroy, 12, ack, &ack_bytes, 12) == VK_SUCCESS);
    assert(rpc(&connection, MB_RENDERER_DESTROY, destroy, 12, ack, &ack_bytes, 12) == VK_SUCCESS);
    pthread_mutex_unlock(&connection.lock);
    outside.renderer_destroy_kind[MB_R_VIEW] = connection.perf.renderer_destroy_kind[MB_R_VIEW];
    dd_perf_report_resources(stdout, &frame, &outside, 1);
    assert(!pthread_mutex_destroy(&connection.lock)); puts("wait attribution PASS");
}
