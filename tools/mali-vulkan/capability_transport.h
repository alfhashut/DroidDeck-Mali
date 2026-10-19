/* Bounded one-shot Android inventory query. No Vulkan instance is created. */
#ifndef DROIDDECK_MALI_CAPABILITY_TRANSPORT_H
#define DROIDDECK_MALI_CAPABILITY_TRANSPORT_H
#include <stdlib.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "capabilities.h"

static inline int mb_renderer_mode(void) {
    const char *v = getenv("MALI_VULKAN_RENDERER_TEST"); return v && !strcmp(v, "1");
}
static inline int mb_interop_mode(void) {
    const char *value = getenv("MALI_VULKAN_INTEROP_TEST");
    return mb_renderer_mode() || (value && !strcmp(value, "1"));
}
static inline int mb_submit_mode(void) {
    const char *value = getenv("MALI_VULKAN_SUBMIT_TEST");
    return mb_interop_mode() || (value && !strcmp(value, "1"));
}
static inline int mb_device_mode(void) {
    const char *value = getenv("MALI_VULKAN_DEVICE_TEST");
    return mb_submit_mode() || (value && !strcmp(value, "1"));
}
static inline int mb_capability_mode(void) {
    const char *value = getenv("MALI_VULKAN_QUERY_CAPABILITIES");
    return mb_device_mode() || (value && !strcmp(value, "1"));
}
static inline int mb_cap_connect(void) {
    const char *path = getenv("MALI_VULKAN_BROKER_SOCKET");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address.sun_path)) { errno = EINVAL; return -1; }
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 10};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address))) { close(fd); return -1; }
    return fd;
}
static inline VkResult mb_global_inventory(uint32_t *version, uint32_t *count, VkExtensionProperties *extensions) {
    int fd = mb_cap_connect();
    if (fd < 0) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t header[MB_HEADER_BYTES], reply[MB_PREFIX_BYTES + 4 + MB_MAX_EXTENSIONS * MB_EXTENSION_BYTES];
    mb_session_header(header, MB_GLOBAL, 0); mb_put_u32(header + 4, MB_CAP_VERSION);
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (mb_write(fd, header, sizeof(header)) || mb_read(fd, header, sizeof(header))) goto done;
    uint32_t bytes = mb_get_u32(header + 12);
    if (mb_get_u32(header) != MB_MAGIC || mb_get_u32(header + 4) != MB_CAP_VERSION ||
        mb_get_u32(header + 8) != MB_GLOBAL || bytes < MB_PREFIX_BYTES + 4 || bytes > sizeof(reply) ||
        mb_read(fd, reply, bytes)) goto done;
    uint32_t n = mb_get_u32(reply + 8);
    if (mb_get_u32(reply) != MB_OK || mb_get_u32(reply + 4) != VK_SUCCESS || n > MB_MAX_EXTENSIONS ||
        bytes != MB_PREFIX_BYTES + 4 + n * MB_EXTENSION_BYTES ||
        mb_decode_extensions(reply + MB_PREFIX_BYTES + 4, extensions, n)) goto done;
    *version = mb_get_u32(reply + MB_PREFIX_BYTES); *count = n; result = VK_SUCCESS;
done:
    close(fd);
    return result;
}
#endif
