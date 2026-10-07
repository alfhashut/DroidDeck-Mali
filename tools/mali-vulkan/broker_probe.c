#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "protocol.h"

#ifndef __GLIBC__
#error "broker_probe must be built against glibc, not Bionic"
#endif

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /absolute/path/to/broker.sock\n", argv[0]);
        return 2;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(argv[1]) >= sizeof(address.sun_path)) {
        fprintf(stderr, "Socket path is too long\n");
        return 2;
    }
    strcpy(address.sun_path, argv[1]);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int result = 1;
    struct timeval timeout = { .tv_sec = 10 };
    uint8_t header[MB_HEADER_BYTES], payload[MB_MAX_PAYLOAD];
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        perror("socket timeout"); goto done;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        perror("connect"); goto done;
    }
    mb_header(header, 0);
    if (mb_write(fd, header, sizeof(header)) || mb_read(fd, header, sizeof(header))) {
        perror("request/response header"); goto done;
    }
    uint32_t bytes = mb_get_u32(header + 12);
    if (mb_get_u32(header) != MB_MAGIC || mb_get_u32(header + 4) != MB_VERSION ||
        mb_get_u32(header + 8) != MB_ENUMERATE ||
        bytes < MB_PREFIX_BYTES || bytes > MB_MAX_PAYLOAD) {
        fprintf(stderr, "Invalid broker response header\n"); goto done;
    }
    if (mb_read(fd, payload, bytes)) { perror("response payload"); goto done; }
    uint32_t status = mb_get_u32(payload), vk_bits = mb_get_u32(payload + 4);
    uint32_t count = mb_get_u32(payload + 8);
    int64_t vk_result = vk_bits >= UINT32_C(0x80000000)
        ? (int64_t)vk_bits - INT64_C(0x100000000) : (int64_t)vk_bits;
    if (count > MB_MAX_DEVICES || bytes != MB_PREFIX_BYTES + count * MB_RECORD_BYTES ||
        (status != MB_OK && count != 0)) {
        fprintf(stderr, "Invalid broker response length/count\n"); goto done;
    }
    if (status != MB_OK || vk_result != 0 || count == 0) {
        fprintf(stderr, "Broker failed: status=%" PRIu32 " VkResult=%" PRId64
                        " count=%" PRIu32 " (see MaliVulkanBroker logcat)\n",
                status, vk_result, count);
        goto done;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *p = payload + MB_PREFIX_BYTES + i * MB_RECORD_BYTES;
        if (!memchr(p, 0, MB_NAME_BYTES)) {
            fprintf(stderr, "Invalid unterminated device name\n"); goto done;
        }
    }
    printf("Android Vulkan physical-device count: %" PRIu32 "\n", count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *p = payload + MB_PREFIX_BYTES + i * MB_RECORD_BYTES;
        const uint8_t *v = p + MB_NAME_BYTES;
        uint32_t api = mb_get_u32(v + 8);
        printf("Device %" PRIu32 "\n  deviceName: %s\n  vendorID: 0x%08" PRIx32
               "\n  deviceID: 0x%08" PRIx32 "\n  apiVersion: %" PRIu32
               " (%" PRIu32 ".%" PRIu32 ".%" PRIu32 ", variant %" PRIu32 ")"
               "\n  driverVersion: %" PRIu32 " (vendor-specific raw value)"
               "\n  deviceType: %" PRIu32 "\n",
               i, (const char *)p, mb_get_u32(v), mb_get_u32(v + 4), api,
               (api >> 22) & 0x7fu, (api >> 12) & 0x3ffu, api & 0xfffu, api >> 29,
               mb_get_u32(v + 12), mb_get_u32(v + 16));
    }
    result = 0;
done:
    close(fd);
    return result;
}
