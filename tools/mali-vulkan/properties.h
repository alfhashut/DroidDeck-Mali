/* Full Vulkan 1.0 properties, encoded field-by-field rather than copying an ABI struct. */
#ifndef DROIDDECK_MALI_PROPERTIES_H
#define DROIDDECK_MALI_PROPERTIES_H
#include <float.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "protocol.h"

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24, "IEEE binary32");
_Static_assert(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE == MB_NAME_BYTES && VK_UUID_SIZE == 16, "wire sizes");
enum { MB_PROPERTIES_BYTES = MB_NAME_BYTES + VK_UUID_SIZE
#define MB_U32(f) + 4
#define MB_I32(f) + 4
#define MB_F32(f) + 4
#define MB_U64(f) + 8
#define MB_SIZE(f) + 8
#include "properties_fields.def"
#undef MB_U32
#undef MB_I32
#undef MB_F32
#undef MB_U64
#undef MB_SIZE
};
_Static_assert(MB_PREFIX_BYTES + MB_PROPERTIES_BYTES <= MB_MAX_PAYLOAD, "payload bound");

static inline void mb_encode_properties(uint8_t *wire, const VkPhysicalDeviceProperties *p) {
    memset(wire, 0, MB_NAME_BYTES);
    memcpy(wire, p->deviceName, strnlen(p->deviceName, MB_NAME_BYTES - 1));
    wire += MB_NAME_BYTES;
    memcpy(wire, p->pipelineCacheUUID, VK_UUID_SIZE); wire += VK_UUID_SIZE;
#define MB_U32(f) mb_put_u32(wire, (uint32_t)p->f); wire += 4;
#define MB_I32(f) MB_U32(f)
#define MB_U64(f) mb_put_u64(wire, (uint64_t)p->f); wire += 8;
#define MB_SIZE(f) MB_U64(f)
#define MB_F32(f) do { uint32_t bits; memcpy(&bits, &p->f, 4); mb_put_u32(wire, bits); wire += 4; } while (0);
#include "properties_fields.def"
#undef MB_U32
#undef MB_I32
#undef MB_F32
#undef MB_U64
#undef MB_SIZE
}

static inline int mb_decode_properties(const uint8_t *wire, VkPhysicalDeviceProperties *p) {
    if (!memchr(wire, 0, MB_NAME_BYTES)) return -1;
    memset(p, 0, sizeof(*p));
    memcpy(p->deviceName, wire, MB_NAME_BYTES); wire += MB_NAME_BYTES;
    memcpy(p->pipelineCacheUUID, wire, VK_UUID_SIZE); wire += VK_UUID_SIZE;
#define MB_U32(f) p->f = mb_get_u32(wire); wire += 4;
#define MB_I32(f) do { uint32_t bits = mb_get_u32(wire); p->f = (int32_t)(bits >= UINT32_C(0x80000000) ? (int64_t)bits - INT64_C(0x100000000) : bits); wire += 4; } while (0);
#define MB_U64(f) p->f = mb_get_u64(wire); wire += 8;
#define MB_SIZE(f) do { uint64_t value = mb_get_u64(wire); if (value > SIZE_MAX) return -1; p->f = (size_t)value; wire += 8; } while (0);
#define MB_F32(f) do { uint32_t bits = mb_get_u32(wire); memcpy(&p->f, &bits, 4); wire += 4; } while (0);
#include "properties_fields.def"
#undef MB_U32
#undef MB_I32
#undef MB_F32
#undef MB_U64
#undef MB_SIZE
    return 0;
}
#endif
