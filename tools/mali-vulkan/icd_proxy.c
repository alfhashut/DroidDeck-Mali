/* Debug-only glibc ICD: query and opt-in logical-device lifecycle subset. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vk_icd.h>
#include "capability_transport.h"
#include "submit_protocol.h"
#include "interop_protocol.h"
#ifndef __GLIBC__
#error This ICD must be built against glibc, not Bionic
#endif

#define EXPORT __attribute__((visibility("default")))
#define LOG(...) do { fprintf(stderr, "MaliProxyICD: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

struct proxy_device {
    VK_LOADER_DATA loader;
    VkPhysicalDeviceProperties properties;
    struct proxy_instance *owner;
    uint32_t id;
    struct mb_capabilities capabilities;
};
struct proxy_instance {
    VK_LOADER_DATA loader;
    int fd, listed;
    pthread_mutex_t lock;
    uint32_t count;
    uint32_t wire_version;
    struct proxy_logical *logical;
    struct proxy_device devices[MB_MAX_DEVICES];
};
_Static_assert(offsetof(struct proxy_instance, loader) == 0, "instance dispatch word");
_Static_assert(offsetof(struct proxy_device, loader) == 0, "physical-device dispatch word");

/* A connection owns the remote instance; IDs only have meaning on that connection. */
static VkResult rpc(struct proxy_instance *s, uint32_t op, const uint8_t *request,
                    uint32_t request_bytes, uint8_t *reply, uint32_t *reply_bytes, uint32_t capacity) {
    int teardown = s->wire_version >= MB_SUBMIT_VERSION && (op == MB_DEVICE_DESTROY || op == MB_DESTROY || op == MB_AHB_PRESENT);
    /* Await safe GPU teardown or consumer release acknowledgement, even after a finite producer timeout. */
    if (teardown) {
        struct timeval no_timeout = {0};
        if (setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout))) goto broken;
    }
    uint8_t header[MB_HEADER_BYTES];
    mb_session_header(header, op, request_bytes);
    mb_put_u32(header + 4, s->wire_version);
    if (mb_write(s->fd, header, sizeof(header)) || mb_write(s->fd, request, request_bytes) ||
        mb_read(s->fd, header, sizeof(header))) goto broken;
    uint32_t bytes = mb_get_u32(header + 12);
    if (mb_get_u32(header) != MB_MAGIC || mb_get_u32(header + 4) != s->wire_version ||
        mb_get_u32(header + 8) != op || bytes < MB_PREFIX_BYTES || bytes > capacity) goto broken;
    if (mb_read(s->fd, reply, bytes)) goto broken;
    if (teardown) {
        struct timeval normal_timeout = {.tv_sec = 10};
        if (setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &normal_timeout, sizeof(normal_timeout))) goto broken;
    }
    *reply_bytes = bytes;
    if (mb_get_u32(reply) == MB_OK && mb_get_u32(reply + 4) == VK_SUCCESS) return VK_SUCCESS;
    LOG("broker opcode=%u status=%u VkResult bits=0x%x", op, mb_get_u32(reply), mb_get_u32(reply + 4));
    /* Preserve the native submission result, including statuses and device loss. */
    if (s->wire_version >= MB_SUBMIT_VERSION &&
        (mb_get_u32(reply) == MB_OK || mb_get_u32(reply) == MB_VULKAN_ERROR)) {
        int32_t native_result; uint32_t bits = mb_get_u32(reply + 4);
        memcpy(&native_result, &bits, 4);
        if ((mb_get_u32(reply) == MB_OK && native_result >= 0) ||
            (mb_get_u32(reply) == MB_VULKAN_ERROR && native_result < 0)) return (VkResult)native_result;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Preserve historical v1-v4 query error handling. */
    uint32_t result = mb_get_u32(reply + 4);
    if (result == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (result == (uint32_t)VK_ERROR_INCOMPATIBLE_DRIVER) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (result == (uint32_t)VK_ERROR_EXTENSION_NOT_PRESENT) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (result == (uint32_t)VK_ERROR_TOO_MANY_OBJECTS) return VK_ERROR_TOO_MANY_OBJECTS;
    if (result == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT) return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_ERROR_INITIALIZATION_FAILED;
broken:
    LOG("socket/protocol failure on opcode=%u: %s", op, strerror(errno));
    /* Prevent a partial/bad stream from being reused; disconnect cleans up remotely. */
    shutdown(s->fd, SHUT_RDWR);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void proxy_free_logical(struct proxy_instance *);

static VkResult cap_InstanceExtensions(const char *, uint32_t *, VkExtensionProperties *);

static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!info || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        info->flags || allocator) {
        LOG("unsupported create inputs: pNext=%s flags=%u allocator=%s", info && info->pNext ? "present" : "absent",
            info ? info->flags : 0, allocator ? "present" : "absent");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Loader-private bookkeeping nodes are not application extensions. Never forward
     * their callbacks/pointers to Android, and reject every other pNext structure. */
    unsigned nodes = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (++nodes > 16 || p->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (info->enabledExtensionCount) {
        if (!mb_capability_mode()) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkExtensionProperties extensions[4]; uint32_t count = 4;
        VkResult result = cap_InstanceExtensions(NULL, &count, extensions);
        if (result != VK_SUCCESS) return result;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            if (!mb_has_extension(extensions, count, info->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    uint32_t api = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    if (!api) api = VK_API_VERSION_1_0;
    if (VK_API_VERSION_VARIANT(api) || VK_API_VERSION_MAJOR(api) != 1 || VK_API_VERSION_MINOR(api) != 0)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (info->pApplicationInfo && info->pApplicationInfo->pNext) return VK_ERROR_INITIALIZATION_FAILED;
    const char *path = getenv("MALI_VULKAN_BROKER_SOCKET");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address.sun_path)) {
        LOG("set MALI_VULKAN_BROKER_SOCKET to the app-private broker socket");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct proxy_instance *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->wire_version = mb_interop_mode() ? MB_INTEROP_VERSION : mb_submit_mode() ? MB_SUBMIT_VERSION : mb_device_mode() ? MB_DEVICE_VERSION : (mb_capability_mode() ? MB_CAP_VERSION : MB_SESSION_VERSION);
    s->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    strcpy(address.sun_path, path);
    struct timeval timeout = { .tv_sec = 10 };
    if (s->fd < 0 || setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        connect(s->fd, (struct sockaddr *)&address, sizeof(address))) {
        LOG("connect %s: %s", path, strerror(errno));
        if (s->fd >= 0) close(s->fd);
        free(s); return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_init(&s->lock, NULL)) { close(s->fd); free(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint8_t request[4], reply[MB_MAX_PAYLOAD]; uint32_t bytes;
    mb_put_u32(request, api);
    VkResult result = rpc(s, MB_CREATE, request, sizeof(request), reply, &bytes, sizeof(reply));
    if (result == VK_SUCCESS && (bytes != MB_PREFIX_BYTES || mb_get_u32(reply + 8)))
        result = VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) { close(s->fd); pthread_mutex_destroy(&s->lock); free(s); return result; }
    set_loader_magic_value(s);
    *out = (VkInstance)s;
    LOG("created local proxy instance; Android instance stays in broker");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL proxy_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    if (!instance) return;
    struct proxy_instance *s = (struct proxy_instance *)instance;
    uint8_t reply[MB_MAX_PAYLOAD]; uint32_t bytes;
    pthread_mutex_lock(&s->lock);
    VkResult result = rpc(s, MB_DESTROY, NULL, 0, reply, &bytes, sizeof(reply));
    if (result == VK_SUCCESS && (bytes != MB_PREFIX_BYTES || mb_get_u32(reply + 8)))
        LOG("invalid destroy acknowledgement");
    proxy_free_logical(s);
    close(s->fd);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
    LOG("destroyed local proxy instance");
}

static VkResult cache_devices(struct proxy_instance *s) {
    uint8_t reply[MB_MAX_PAYLOAD]; uint32_t bytes;
    VkResult result = rpc(s, MB_LIST, NULL, 0, reply, &bytes, sizeof(reply));
    if (result != VK_SUCCESS) return result;
    uint32_t count = mb_get_u32(reply + 8), ids[MB_MAX_DEVICES];
    if (count > MB_MAX_DEVICES || bytes != MB_PREFIX_BYTES + count * 4) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; ++i) {
        ids[i] = mb_get_u32(reply + MB_PREFIX_BYTES + i * 4);
        if (ids[i] != i + 1) return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t request[4]; mb_put_u32(request, ids[i]);
        result = rpc(s, MB_PROPERTIES, request, sizeof(request), reply, &bytes, sizeof(reply));
        if (result != VK_SUCCESS) return result;
        if (bytes != MB_PREFIX_BYTES + MB_PROPERTIES_BYTES || mb_get_u32(reply + 8) != 1 ||
            mb_decode_properties(reply + MB_PREFIX_BYTES, &s->devices[i].properties))
            return VK_ERROR_INITIALIZATION_FAILED;
        set_loader_magic_value(&s->devices[i]);
        s->devices[i].owner = s; s->devices[i].id = ids[i];
        if (s->wire_version >= MB_CAP_VERSION) {
            uint8_t *wide = malloc(MB_CAP_MAX_PAYLOAD);
            if (!wide) return VK_ERROR_OUT_OF_HOST_MEMORY;
            result = rpc(s, MB_CAPS, request, sizeof(request), wide, &bytes, MB_CAP_MAX_PAYLOAD);
            if (result == VK_SUCCESS && (bytes != MB_PREFIX_BYTES + MB_PROPERTIES_BYTES + MB_CAPS_BYTES ||
                mb_get_u32(wide + 8) != 1 || mb_decode_properties(wide + MB_PREFIX_BYTES, &s->devices[i].properties) ||
                mb_decode_capabilities(wide + MB_PREFIX_BYTES + MB_PROPERTIES_BYTES, &s->devices[i].capabilities)))
            {
                LOG("invalid capability snapshot; no proxy device published");
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
            free(wide);
            if (result != VK_SUCCESS) return result;
        }
    }
    /* Properties are immutable for an instance. Fetch before publishing any handles,
     * so a failed RPC can be reported by enumeration rather than a void query. */
    s->count = count; s->listed = 1;
    LOG("cached %u real Android physical-device properties", count);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL proxy_EnumeratePhysicalDevices(VkInstance instance,
        uint32_t *count, VkPhysicalDevice *devices) {
    if (!instance || !count) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_instance *s = (struct proxy_instance *)instance;
    pthread_mutex_lock(&s->lock);
    VkResult result = s->listed ? VK_SUCCESS : cache_devices(s);
    if (result == VK_SUCCESS) {
        if (!devices) *count = s->count;
        else {
            uint32_t written = *count < s->count ? *count : s->count;
            for (uint32_t i = 0; i < written; ++i) devices[i] = (VkPhysicalDevice)&s->devices[i];
            *count = written;
            if (written < s->count) result = VK_INCOMPLETE;
        }
    } else *count = 0;
    pthread_mutex_unlock(&s->lock);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties *out) {
    *out = ((struct proxy_device *)device)->properties;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_EnumerateInstanceVersion(uint32_t *version) {
    *version = VK_API_VERSION_1_0; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_EnumerateInstanceExtensionProperties(const char *layer,
        uint32_t *count, VkExtensionProperties *extensions) {
    if (mb_capability_mode()) return cap_InstanceExtensions(layer, count, extensions);
    (void)extensions;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    *count = 0; return VK_SUCCESS;
}

#include "capability_icd.h"

/* Core dispatch slots are required even for enumeration. Version 2 retains its
 * empty capabilities; the opt-in version-3 session forwards real query data.
 * These calls never create a device, submit work or allocate GPU resources. */
static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceFeatures(VkPhysicalDevice d, VkPhysicalDeviceFeatures *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) { *p = ((struct proxy_device *)d)->capabilities.core; return; }
    (void)d; memset(p, 0, sizeof(*p));
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceFormatProperties(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) { cap_Format(d, f, p, NULL); return; }
    (void)d; (void)f; memset(p, 0, sizeof(*p));
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice d, VkFormat f,
        VkImageType t, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) return cap_Image(d, f, t, tiling, usage, flags, 0, 0, 0, p, NULL);
    (void)d; (void)f; (void)t; (void)tiling; (void)usage; (void)flags; memset(p, 0, sizeof(*p));
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t *count,
        VkQueueFamilyProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) { cap_Queues(d, count, p); return; }
    (void)d; (void)p; *count = 0;
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) { *p = ((struct proxy_device *)d)->capabilities.memory; return; }
    (void)d; memset(p, 0, sizeof(*p));
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_EnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char *layer,
        uint32_t *count, VkExtensionProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) return cap_DeviceExtensions(d, layer, count, p);
    (void)d; return proxy_EnumerateInstanceExtensionProperties(layer, count, p);
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice d, VkFormat f,
        VkImageType t, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
        uint32_t *count, VkSparseImageFormatProperties *p) {
    if (((struct proxy_device *)d)->owner->wire_version >= MB_CAP_VERSION) { cap_Sparse(d, f, t, samples, usage, tiling, count, p); return; }
    (void)d; (void)f; (void)t; (void)samples; (void)usage; (void)tiling; (void)p; *count = 0;
}
#include "device_icd.h"

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    if (!version || *version < 2) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (*version > 5) *version = 5;
    return VK_SUCCESS;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
static PFN_vkVoidFunction physical_proc(const char *name) {
#define ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
    ENTRY(GetPhysicalDeviceProperties)
    ENTRY(GetPhysicalDeviceFeatures)
    ENTRY(GetPhysicalDeviceFormatProperties)
    ENTRY(GetPhysicalDeviceImageFormatProperties)
    ENTRY(GetPhysicalDeviceQueueFamilyProperties)
    ENTRY(GetPhysicalDeviceMemoryProperties)
    ENTRY(GetPhysicalDeviceSparseImageFormatProperties)
    ENTRY(EnumerateDeviceExtensionProperties)
    ENTRY(CreateDevice)
    if (mb_capability_mode()) return cap_proc(name);
    return NULL;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    return instance && name ? physical_proc(name) : NULL;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    ENTRY(CreateInstance)
    ENTRY(EnumerateInstanceVersion)
    ENTRY(EnumerateInstanceExtensionProperties)
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (!strcmp(name, "vk_icdGetPhysicalDeviceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    if (!instance) return NULL;
    ENTRY(DestroyInstance)
    ENTRY(EnumeratePhysicalDevices)
    ENTRY(GetDeviceProcAddr)
    ENTRY(DestroyDevice)
    ENTRY(GetDeviceQueue)
#undef ENTRY
    if (((struct proxy_instance *)instance)->wire_version >= MB_SUBMIT_VERSION) {
#define MB_SUBMIT_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
#include "submit_entries.def"
#undef MB_SUBMIT_ENTRY
    }
    if (((struct proxy_instance *)instance)->wire_version == MB_INTEROP_VERSION) {
#define MB_INTEROP_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
#include "interop_entries.def"
#undef MB_INTEROP_ENTRY
        if (!strcmp(name, "vkDroidDeckInteropTEST")) return (PFN_vkVoidFunction)proxy_DroidDeckInteropTEST;
    }
    return physical_proc(name);
}
