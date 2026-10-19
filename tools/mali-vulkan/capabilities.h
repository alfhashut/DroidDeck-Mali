/* Local Vulkan structures are conveniences only: none is copied onto the wire. */
#ifndef DROIDDECK_MALI_CAPABILITIES_H
#define DROIDDECK_MALI_CAPABILITIES_H
#include "properties.h"

#define MB_Q_FEATURES2 (1u << 0)
#define MB_Q_PROPERTIES2 (1u << 1)
#define MB_Q_TIMELINE (1u << 2)
#define MB_Q_SCALAR (1u << 3)
#define MB_Q_YCBCR (1u << 4)
#define MB_Q_FLOAT16 (1u << 5)
#define MB_Q_ROBUSTNESS (1u << 6)
#define MB_Q_DYNAMIC (1u << 7)
#define MB_Q_PRESENT_ID (1u << 8)
#define MB_Q_PRESENT_WAIT (1u << 9)
#define MB_Q_ID (1u << 10)
#define MB_Q_DRM (1u << 11)

struct mb_capabilities {
    uint32_t queried, extension_count, queue_count;
    VkPhysicalDeviceFeatures core;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr;
    VkPhysicalDeviceShaderFloat16Int8Features float16;
    VkPhysicalDeviceRobustness2FeaturesEXT robustness;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic;
    VkPhysicalDevicePresentIdFeaturesKHR present_id;
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait;
    VkPhysicalDeviceIDProperties id;
    VkPhysicalDeviceDrmPropertiesEXT drm;
    VkQueueFamilyProperties queues[MB_MAX_QUEUES];
    VkPhysicalDeviceMemoryProperties memory;
    VkExtensionProperties extensions[MB_MAX_EXTENSIONS];
};
enum { MB_CAPS_BYTES = MB_MAX_QUEUES * 24 + 8 + 32 * 8 + 16 * 12 + MB_MAX_EXTENSIONS * MB_EXTENSION_BYTES
#define MB_U32(f) + 4
#define MB_BOOL(f) + 4
#define MB_U64(f) + 8
#define MB_BYTES(f, n) + n
#include "capabilities_fields.def"
#undef MB_U32
#undef MB_BOOL
#undef MB_U64
#undef MB_BYTES
};
_Static_assert(MB_PREFIX_BYTES + MB_CAPS_BYTES + MB_PROPERTIES_BYTES <= MB_CAP_MAX_PAYLOAD, "capability bound");

static inline int mb_has_extension(const VkExtensionProperties *p, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i) if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}
static inline void mb_encode_extensions(uint8_t *wire, const VkExtensionProperties *p, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i, wire += MB_EXTENSION_BYTES) {
        memset(wire, 0, MB_NAME_BYTES);
        memcpy(wire, p[i].extensionName, strnlen(p[i].extensionName, MB_NAME_BYTES - 1));
        mb_put_u32(wire + MB_NAME_BYTES, p[i].specVersion);
    }
}
static inline int mb_decode_extensions(const uint8_t *wire, VkExtensionProperties *p, uint32_t count) {
    if (count > MB_MAX_EXTENSIONS) return -1;
    for (uint32_t i = 0; i < count; ++i, wire += MB_EXTENSION_BYTES) {
        if (!memchr(wire, 0, MB_NAME_BYTES)) return -1;
        memcpy(p[i].extensionName, wire, MB_NAME_BYTES);
        p[i].specVersion = mb_get_u32(wire + MB_NAME_BYTES);
    }
    return 0;
}
static inline void mb_encode_capabilities(uint8_t *wire, const struct mb_capabilities *p) {
#define MB_U32(f) mb_put_u32(wire, (uint32_t)p->f); wire += 4;
#define MB_BOOL(f) MB_U32(f)
#define MB_U64(f) mb_put_u64(wire, (uint64_t)p->f); wire += 8;
#define MB_BYTES(f, n) memcpy(wire, p->f, n); wire += n;
#include "capabilities_fields.def"
#undef MB_U32
#undef MB_BOOL
#undef MB_U64
#undef MB_BYTES
    for (uint32_t i = 0; i < MB_MAX_QUEUES; ++i) {
        const VkQueueFamilyProperties *q = &p->queues[i];
        mb_put_u32(wire, q->queueFlags); mb_put_u32(wire + 4, q->queueCount);
        mb_put_u32(wire + 8, q->timestampValidBits);
        mb_put_u32(wire + 12, q->minImageTransferGranularity.width);
        mb_put_u32(wire + 16, q->minImageTransferGranularity.height);
        mb_put_u32(wire + 20, q->minImageTransferGranularity.depth); wire += 24;
    }
    mb_put_u32(wire, p->memory.memoryTypeCount); mb_put_u32(wire + 4, p->memory.memoryHeapCount); wire += 8;
    for (uint32_t i = 0; i < 32; ++i) {
        mb_put_u32(wire, p->memory.memoryTypes[i].propertyFlags);
        mb_put_u32(wire + 4, p->memory.memoryTypes[i].heapIndex); wire += 8;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        mb_put_u64(wire, p->memory.memoryHeaps[i].size);
        mb_put_u32(wire + 8, p->memory.memoryHeaps[i].flags); wire += 12;
    }
    mb_encode_extensions(wire, p->extensions, MB_MAX_EXTENSIONS);
}
static inline int mb_decode_capabilities(const uint8_t *wire, struct mb_capabilities *p) {
    memset(p, 0, sizeof(*p));
#define MB_U32(f) p->f = mb_get_u32(wire); wire += 4;
#define MB_BOOL(f) MB_U32(f) if (p->f > 1) return -1;
#define MB_U64(f) p->f = mb_get_u64(wire); wire += 8;
#define MB_BYTES(f, n) memcpy(p->f, wire, n); wire += n;
#include "capabilities_fields.def"
#undef MB_U32
#undef MB_BOOL
#undef MB_U64
#undef MB_BYTES
    if (p->extension_count > MB_MAX_EXTENSIONS || p->queue_count > MB_MAX_QUEUES || p->queried & ~4095u) return -1;
    for (uint32_t i = 0; i < MB_MAX_QUEUES; ++i) {
        VkQueueFamilyProperties *q = &p->queues[i];
        q->queueFlags = mb_get_u32(wire); q->queueCount = mb_get_u32(wire + 4);
        q->timestampValidBits = mb_get_u32(wire + 8);
        q->minImageTransferGranularity = (VkExtent3D){mb_get_u32(wire + 12), mb_get_u32(wire + 16), mb_get_u32(wire + 20)};
        wire += 24;
    }
    p->memory.memoryTypeCount = mb_get_u32(wire); p->memory.memoryHeapCount = mb_get_u32(wire + 4); wire += 8;
    if (p->memory.memoryTypeCount > 32 || p->memory.memoryHeapCount > 16) return -1;
    for (uint32_t i = 0; i < 32; ++i) {
        p->memory.memoryTypes[i].propertyFlags = mb_get_u32(wire);
        p->memory.memoryTypes[i].heapIndex = mb_get_u32(wire + 4); wire += 8;
        if (i < p->memory.memoryTypeCount && p->memory.memoryTypes[i].heapIndex >= p->memory.memoryHeapCount) return -1;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        p->memory.memoryHeaps[i].size = mb_get_u64(wire);
        p->memory.memoryHeaps[i].flags = mb_get_u32(wire + 8); wire += 12;
    }
    return mb_decode_extensions(wire, p->extensions, MB_MAX_EXTENSIONS);
}
#endif
