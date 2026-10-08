/* Version 4 device-test schema. Scalars only; never Vulkan handles or pNext. */
#ifndef DROIDDECK_DEVICE_PROTOCOL_H
#define DROIDDECK_DEVICE_PROTOCOL_H
#include <math.h>
#include <string.h>
#include "protocol.h"
#define MB_DEVICE_VERSION 4u
#define MB_DEVICE_CREATE 14u
#define MB_DEVICE_QUEUE 15u
#define MB_DEVICE_DESTROY 16u
#define MB_MAX_LOGICAL_DEVICES 16u
#define MB_DEVICE_MAX_QUEUES 4u
#define MB_DEVICE_MAX_EXTENSIONS 32u
#define MB_CORE_FEATURE_COUNT 55u
/* One queue create info: family/count, count IEEE binary32 priorities.
 * Physical ID, family, count, extension count, 55 explicit core VkBool32 fields,
 * priorities, then 256-byte NUL-terminated extension names. No extension features
 * are accepted in v4; all advanced selections are explicitly absent/disabled.
 * CREATE reply: prefix count=1, u32 logical ID. QUEUE request: device ID/family/index;
 * reply: prefix count=1, u32 queue ID. DESTROY request: device ID; reply prefix count=0.
 * IDs are connection-local, monotonic, never reused; disconnect destroys all devices.
 */
#define MB_DEVICE_BASE_BYTES (16u + MB_CORE_FEATURE_COUNT * 4u)
#define MB_DEVICE_MAX_REQUEST (MB_DEVICE_BASE_BYTES + MB_DEVICE_MAX_QUEUES * 4u + MB_DEVICE_MAX_EXTENSIONS * MB_NAME_BYTES)
_Static_assert(sizeof(float) == 4, "wire queue priorities require binary32");
struct mb_device_request {
    uint32_t physical_id, family, count, extension_count;
    VkPhysicalDeviceFeatures features;
    float priorities[MB_DEVICE_MAX_QUEUES];
    char extensions[MB_DEVICE_MAX_EXTENSIONS][MB_NAME_BYTES];
};
static inline int mb_device_request_valid(const struct mb_device_request *r) {
    if (!r->physical_id || !r->count || r->count > MB_DEVICE_MAX_QUEUES ||
        r->extension_count > MB_DEVICE_MAX_EXTENSIONS) return -1;
#define MB_FEATURE(n) if (r->features.n > 1) return -1;
#include "features_fields.def"
#undef MB_FEATURE
    for (uint32_t i = 0; i < r->count; ++i)
        if (!isfinite(r->priorities[i]) || r->priorities[i] < 0 || r->priorities[i] > 1) return -1;
    for (uint32_t i = 0; i < r->extension_count; ++i) {
        if (!r->extensions[i][0] || !memchr(r->extensions[i], 0, MB_NAME_BYTES)) return -1;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(r->extensions[i], r->extensions[j])) return -1;
    }
    return 0;
}
static inline uint32_t mb_encode_device_request(uint8_t *wire, const struct mb_device_request *r) {
    if (mb_device_request_valid(r)) return 0;
    mb_put_u32(wire, r->physical_id); mb_put_u32(wire + 4, r->family);
    mb_put_u32(wire + 8, r->count); mb_put_u32(wire + 12, r->extension_count);
    uint32_t offset = 16;
#define MB_FEATURE(n) mb_put_u32(wire + offset, r->features.n); offset += 4;
#include "features_fields.def"
#undef MB_FEATURE
    for (uint32_t i = 0; i < r->count; ++i) {
        uint32_t bits; memcpy(&bits, &r->priorities[i], 4);
        mb_put_u32(wire + offset, bits); offset += 4;
    }
    for (uint32_t i = 0; i < r->extension_count; ++i) {
        memset(wire + offset, 0, MB_NAME_BYTES);
        memcpy(wire + offset, r->extensions[i], strlen(r->extensions[i])); offset += MB_NAME_BYTES;
    }
    return offset;
}
static inline int mb_decode_device_request(const uint8_t *wire, uint32_t bytes, struct mb_device_request *r) {
    if (bytes < MB_DEVICE_BASE_BYTES) return -1;
    memset(r, 0, sizeof(*r));
    r->physical_id = mb_get_u32(wire); r->family = mb_get_u32(wire + 4);
    r->count = mb_get_u32(wire + 8); r->extension_count = mb_get_u32(wire + 12);
    if (r->count > MB_DEVICE_MAX_QUEUES || r->extension_count > MB_DEVICE_MAX_EXTENSIONS ||
        bytes != MB_DEVICE_BASE_BYTES + r->count * 4 + r->extension_count * MB_NAME_BYTES) return -1;
    uint32_t offset = 16;
#define MB_FEATURE(n) r->features.n = mb_get_u32(wire + offset); offset += 4;
#include "features_fields.def"
#undef MB_FEATURE
    for (uint32_t i = 0; i < r->count; ++i) {
        uint32_t bits = mb_get_u32(wire + offset); memcpy(&r->priorities[i], &bits, 4); offset += 4;
    }
    for (uint32_t i = 0; i < r->extension_count; ++i) {
        memcpy(r->extensions[i], wire + offset, MB_NAME_BYTES); offset += MB_NAME_BYTES;
    }
    return mb_device_request_valid(r);
}
#endif
