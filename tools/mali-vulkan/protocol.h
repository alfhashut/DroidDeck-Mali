/* Versioned Vulkan diagnostic protocol: no native Vulkan objects on the wire. */
#ifndef DROIDDECK_MALI_PROTOCOL_H
#define DROIDDECK_MALI_PROTOCOL_H

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

/* Version 1: all integers are uint32 little-endian byte sequences, never native structs.
 * Request:  magic, version, opcode, payload bytes (zero).
 * Response: same header, then status, VkResult bits, device count, device records.
 * Record:   256-byte NUL-terminated name, vendorID, deviceID, apiVersion,
 *           driverVersion, deviceType. Driver version is vendor-specific raw data.
 * One request/response per connection. Errors have zero device records.
 */
#define MB_MAGIC UINT32_C(0x564d4444) /* "DDMV" */
#define MB_VERSION 1u
#define MB_ENUMERATE 1u
#define MB_HEADER_BYTES 16u
#define MB_PREFIX_BYTES 12u
#define MB_NAME_BYTES 256u
#define MB_RECORD_BYTES (MB_NAME_BYTES + 5u * 4u)
#define MB_MAX_DEVICES 16u
#define MB_MAX_PAYLOAD (MB_PREFIX_BYTES + MB_MAX_DEVICES * MB_RECORD_BYTES)
#define MB_OK 0u
#define MB_LOADER_ERROR 1u
#define MB_VULKAN_ERROR 2u
#define MB_PROTOCOL_ERROR 3u
#define MB_INTERNAL_ERROR 4u
#define MB_NO_DEVICES 5u

/* Version 1 / MB_ENUMERATE stays unchanged for broker_probe.
 * Version 2 uses one connection per instance, with no native handles on the wire:
 * CREATE: uint32 API version; response prefix only.
 * DESTROY: empty; response prefix only, then connection closes.
 * LIST: empty; response prefix/count followed by count uint32 device IDs.
 * PROPERTIES: uint32 device ID; response prefix/count=1 plus properties.h encoding.
 * IDs are session-local, monotonically indexed from 1, never Android handles.
 * Every response echoes the request version/opcode; errors have count=0.
 */
#define MB_SESSION_VERSION 2u
#define MB_CREATE 2u
#define MB_DESTROY 3u
#define MB_LIST 4u
#define MB_PROPERTIES 5u

/* Version 3 is opt-in capability inspection. Versions 1/2 retain their wire contract.
 * GLOBAL (empty, one-shot): Android loader version + complete instance extension list.
 * CREATE/LIST/PROPERTIES/DESTROY retain version-2 layouts on a version-3 connection.
 * CAPS (device ID): fixed capability snapshot defined by capabilities_fields.def.
 * FORMAT (ID, format): base format flags + bounded DRM modifier records.
 * IMAGE (ID, format, type, tiling, usage, flags, handle type, modifier-present,
 *        uint64 modifier): image limits + external-memory flags.
 * BUFFER (ID, flags, usage, handle type), SEMAPHORE/FENCE (ID, handle type).
 * SPARSE (ID, format, type, samples, usage, tiling): bounded sparse-format records.
 * All fields use LE u32/u64 or fixed byte arrays. No pNext, native structs or handles.
 * MB_UNSUPPORTED means NOT QUERIED; it is distinct from a successful zero capability.
 */
#define MB_CAP_VERSION 3u
#define MB_GLOBAL 6u
#define MB_CAPS 7u
#define MB_FORMAT 8u
#define MB_IMAGE 9u
#define MB_BUFFER 10u
#define MB_SEMAPHORE 11u
#define MB_FENCE 12u
#define MB_SPARSE 13u
#define MB_UNSUPPORTED 6u
#define MB_CAP_MAX_PAYLOAD 131072u
#define MB_MAX_EXTENSIONS 256u
#define MB_MAX_QUEUES 64u
#define MB_MAX_MODIFIERS 128u
#define MB_MAX_SPARSE 64u
#define MB_EXTENSION_BYTES 260u

/* Version 4 opts into logical-device/queue lifecycle; see device_protocol.h for
 * the bounded scalar schema. Versions 1–3 remain query-only and retain layouts.
 * Version 5 adds the submit-only subset in submit_protocol.h; v4 stays lifecycle-only.
 */

static inline uint32_t mb_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void mb_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline void mb_header(uint8_t *p, uint32_t bytes) {
    mb_put_u32(p, MB_MAGIC); mb_put_u32(p + 4, MB_VERSION);
    mb_put_u32(p + 8, MB_ENUMERATE); mb_put_u32(p + 12, bytes);
}

static inline void mb_session_header(uint8_t *p, uint32_t opcode, uint32_t bytes) {
    mb_put_u32(p, MB_MAGIC); mb_put_u32(p + 4, MB_SESSION_VERSION);
    mb_put_u32(p + 8, opcode); mb_put_u32(p + 12, bytes);
}

static inline uint64_t mb_get_u64(const uint8_t *p) {
    return (uint64_t)mb_get_u32(p) | (uint64_t)mb_get_u32(p + 4) << 32;
}

static inline void mb_put_u64(uint8_t *p, uint64_t v) {
    mb_put_u32(p, (uint32_t)v); mb_put_u32(p + 4, (uint32_t)(v >> 32));
}

/* Fixed upper bounds at call sites; handle stream fragmentation and EINTR. */
static inline int mb_read(int fd, uint8_t *p, uint32_t bytes) {
    while (bytes) {
        ssize_t n = recv(fd, p, bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = ECONNRESET; return -1; }
        p += n; bytes -= (uint32_t)n;
    }
    return 0;
}

static inline int mb_write(int fd, const uint8_t *p, uint32_t bytes) {
    while (bytes) {
        ssize_t n = send(fd, p, bytes, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EPIPE; return -1; }
        p += n; bytes -= (uint32_t)n;
    }
    return 0;
}
#endif
