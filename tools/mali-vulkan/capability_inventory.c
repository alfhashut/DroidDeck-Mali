/* Raw Android global inventory, printed separately from proxy-supported extensions. */
#define _GNU_SOURCE
#include <stdio.h>
#include "capability_transport.h"
int main(void) {
    uint32_t version, count; VkExtensionProperties extensions[MB_MAX_EXTENSIONS];
    VkResult result = mb_global_inventory(&version, &count, extensions);
    if (result != VK_SUCCESS) { fprintf(stderr, "Android inventory query failed: %d\n", (int)result); return 1; }
    printf("Android system loader instance API: %u.%u.%u\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    printf("Android instance extensions (raw inventory, NOT proxy support): %u\n", count);
    for (uint32_t i = 0; i < count; ++i) printf("  %s specVersion=%u\n", extensions[i].extensionName, extensions[i].specVersion);
    puts("Proxy API ceiling: 1.0.0; raw Android inventory is not a rendering/WSI claim.");
    return 0;
}
