/* Opt-in lifecycle subset. Local dispatchable objects contain broker IDs only. */
#include <stdatomic.h>
struct proxy_queue {
    VK_LOADER_DATA loader;
    uint32_t id;
    struct proxy_logical *owner;
};
struct proxy_resource;
struct proxy_logical {
    VK_LOADER_DATA loader;
    struct proxy_instance *owner;
    struct proxy_logical *next;
    uint32_t id, family, count;
    struct proxy_queue queues[MB_DEVICE_MAX_QUEUES];
    struct proxy_resource *resources;
    atomic_int submit_failed;
    VkPhysicalDeviceMemoryProperties memory_properties;
    size_t map_alignment;
    VkDeviceSize atom;
};
_Static_assert(offsetof(struct proxy_logical, loader) == 0, "device dispatch word");
_Static_assert(offsetof(struct proxy_queue, loader) == 0, "queue dispatch word");

static void proxy_free_resources(struct proxy_logical *d);

static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkDevice *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!physical) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_device *p = (struct proxy_device *)physical;
    struct proxy_instance *s = p->owner;
    if (s->wire_version < MB_DEVICE_VERSION || !info || allocator || info->flags ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t timeline = 0, scalar = 0;
    int renderer = s->wire_version == MB_RENDERER_VERSION;
    /* Ignore only loader-owned bookkeeping; never transport any pointer chain. */
    unsigned nodes = 0;
    for (const VkBaseInStructure *v = info->pNext; v; v = v->pNext) {
        if (++nodes > 16) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (v->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        if (renderer && v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR && !timeline) {
            timeline = ((const VkPhysicalDeviceTimelineSemaphoreFeaturesKHR *)v)->timelineSemaphore;
            if (timeline != 1 || !(p->capabilities.queried & MB_Q_TIMELINE) || !p->capabilities.timeline.timelineSemaphore) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (renderer && v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT && !scalar) {
            scalar = ((const VkPhysicalDeviceScalarBlockLayoutFeaturesEXT *)v)->scalarBlockLayout;
            if (scalar != 1 || !(p->capabilities.queried & MB_Q_SCALAR) || !p->capabilities.scalar.scalarBlockLayout) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (renderer && (timeline != 1 || scalar != 1)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    /* Inventory is not command support: this diagnostic can enable no device extensions. */
    if (info->enabledExtensionCount && !renderer) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (renderer && info->enabledExtensionCount != 3) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->queueCreateInfoCount != 1 || !info->pQueueCreateInfos) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDeviceQueueCreateInfo *q = info->pQueueCreateInfos;
    if (q->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || q->flags || q->pNext ||
        !q->pQueuePriorities || !q->queueCount || q->queueCount > MB_DEVICE_MAX_QUEUES ||
        q->queueFamilyIndex >= p->capabilities.queue_count ||
        q->queueCount > p->capabilities.queues[q->queueFamilyIndex].queueCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct mb_device_request request = {.physical_id = p->id, .family = q->queueFamilyIndex, .count = q->queueCount};
    if (info->pEnabledFeatures) request.features = *info->pEnabledFeatures;
    if (renderer) {
        const char *required[] = {"VK_KHR_timeline_semaphore", "VK_EXT_scalar_block_layout", "VK_KHR_image_format_list"};
        if (!info->ppEnabledExtensionNames) return VK_ERROR_EXTENSION_NOT_PRESENT;
        for (unsigned i = 0; i < 3; ++i) {
            if (!info->ppEnabledExtensionNames[i] || strcmp(info->ppEnabledExtensionNames[i], required[i]) || !mb_has_extension(p->capabilities.extensions, p->capabilities.extension_count, required[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
            strcpy(request.extensions[request.extension_count++], required[i]);
        }
    }
#define MB_FEATURE(n) if (request.features.n > 1 || (request.features.n && !p->capabilities.core.n)) return VK_ERROR_FEATURE_NOT_PRESENT;
#include "features_fields.def"
#undef MB_FEATURE
    for (uint32_t i = 0; i < q->queueCount; ++i) request.priorities[i] = q->pQueuePriorities[i];
    uint8_t wire[MB_DEVICE_MAX_REQUEST + 16], reply[MB_PREFIX_BYTES + 4]; uint32_t bytes;
    uint32_t size = mb_encode_device_request(wire, &request);
    if (!size) return VK_ERROR_INITIALIZATION_FAILED;
    if (s->wire_version >= MB_INTEROP_VERSION) {
        const char *ahb = getenv("MALI_VULKAN_AHB_TEST");
        mb_put_u32(wire + size, ahb && !strcmp(ahb, "1")); size += 4;
    }
    if (renderer) { mb_put_u32(wire + size, timeline); mb_put_u32(wire + size + 4, scalar); mb_put_u32(wire + size + 8, 0); size += 12; }
    struct proxy_logical *d = calloc(1, sizeof(*d));
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_lock(&s->lock);
    VkResult result = rpc(s, MB_DEVICE_CREATE, wire, size, reply, &bytes, sizeof(reply));
    if (result == VK_SUCCESS && (bytes != sizeof(reply) || mb_get_u32(reply + 8) != 1 || !mb_get_u32(reply + 12))) {
        shutdown(s->fd, SHUT_RDWR); result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (result == VK_SUCCESS) {
        d->atom = p->properties.limits.nonCoherentAtomSize; d->memory_properties = p->capabilities.memory; d->map_alignment = p->properties.limits.minMemoryMapAlignment;
        d->owner = s; d->id = mb_get_u32(reply + 12); d->family = q->queueFamilyIndex; d->count = q->queueCount;
        set_loader_magic_value(d);
        for (uint32_t i = 0; i < d->count; ++i) {
            set_loader_magic_value(&d->queues[i]); d->queues[i].owner = d;
        }
        d->next = s->logical; s->logical = d; *out = (VkDevice)d;
        LOG("broker vkCreateDevice = VK_SUCCESS; proxy device ID=%u", d->id);
    } else free(d);
    pthread_mutex_unlock(&s->lock);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
    if (!out) return;
    *out = VK_NULL_HANDLE;
    if (!device) return;
    struct proxy_logical *d = (struct proxy_logical *)device;
    if (family != d->family || index >= d->count) { LOG("invalid local queue family/index"); return; }
    struct proxy_instance *s = d->owner;
    pthread_mutex_lock(&s->lock);
    if (!d->queues[index].id) {
        uint8_t request[12], reply[MB_PREFIX_BYTES + 4]; uint32_t bytes;
        mb_put_u32(request, d->id); mb_put_u32(request + 4, family); mb_put_u32(request + 8, index);
        VkResult result = rpc(s, MB_DEVICE_QUEUE, request, sizeof(request), reply, &bytes, sizeof(reply));
        if (result == VK_SUCCESS && bytes == sizeof(reply) && mb_get_u32(reply + 8) == 1 && mb_get_u32(reply + 12)) {
            d->queues[index].id = mb_get_u32(reply + 12);
            LOG("real queue obtained; proxy queue ID=%u", d->queues[index].id);
        } else { LOG("queue acquisition failed"); shutdown(s->fd, SHUT_RDWR); }
    }
    if (d->queues[index].id) *out = (VkQueue)&d->queues[index];
    pthread_mutex_unlock(&s->lock);
}
static VKAPI_ATTR void VKAPI_CALL proxy_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    if (!device) return;
    struct proxy_logical *d = (struct proxy_logical *)device;
    struct proxy_instance *s = d->owner;
    pthread_mutex_lock(&s->lock);
    uint8_t request[4], reply[MB_PREFIX_BYTES]; uint32_t bytes;
    mb_put_u32(request, d->id);
    VkResult result = rpc(s, MB_DEVICE_DESTROY, request, sizeof(request), reply, &bytes, sizeof(reply));
    if (result != VK_SUCCESS || bytes != sizeof(reply) || mb_get_u32(reply + 8)) {
        session_frames = 0;
        s->device_destroy_result = result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
        ERROR("device destroy acknowledgement failed; disconnect for broker cleanup"); shutdown(s->fd, SHUT_RDWR);
    } else LOG("logical device destroyed; broker ID=%u", d->id);
    struct proxy_logical **link = &s->logical;
    while (*link && *link != d) link = &(*link)->next;
    if (*link) *link = d->next;
    proxy_free_resources(d); free(d);
    pthread_mutex_unlock(&s->lock);
}
static void proxy_free_logical(struct proxy_instance *s) {
    while (s->logical) { struct proxy_logical *next = s->logical->next; proxy_free_resources(s->logical); free(s->logical); s->logical = next; }
}
#include "renderer_icd_fwd.h"
#include "submit_icd.h"
#include "interop_icd.h"
#include "renderer_icd.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proxy_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!device || !name) return NULL;
#define DEVICE_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
    DEVICE_ENTRY(GetDeviceProcAddr)
    DEVICE_ENTRY(DestroyDevice)
    DEVICE_ENTRY(GetDeviceQueue)
#undef DEVICE_ENTRY
    if (((struct proxy_logical *)device)->owner->wire_version >= MB_SUBMIT_VERSION) {
#define MB_SUBMIT_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
#include "submit_entries.def"
#undef MB_SUBMIT_ENTRY
    }
    if (((struct proxy_logical *)device)->owner->wire_version >= MB_INTEROP_VERSION) {
#define MB_INTEROP_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
#include "interop_entries.def"
#undef MB_INTEROP_ENTRY
        if (!strcmp(name, "vkDroidDeckWaylandMALI")) return (PFN_vkVoidFunction)proxy_DroidDeckWaylandMALI;
        if (!strcmp(name, "vkDroidDeckPerformanceMALI")) return (PFN_vkVoidFunction)proxy_DroidDeckPerformanceMALI;
        if (!strcmp(name, "vkDroidDeckInteropTEST")) return (PFN_vkVoidFunction)proxy_DroidDeckInteropTEST;
        if (!strcmp(name, "vkDroidDeckSessionTEST")) return (PFN_vkVoidFunction)proxy_DroidDeckSessionTEST;
    }
    if (((struct proxy_logical *)device)->owner->wire_version == MB_RENDERER_VERSION) {
#define MB_RENDERER_ENTRY(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)proxy_##n;
#include "renderer_entries.def"
#undef MB_RENDERER_ENTRY
        if (!strcmp(name, "vkWaitSemaphores")) return (PFN_vkVoidFunction)proxy_WaitSemaphoresKHR;
        if (!strcmp(name, "vkGetSemaphoreCounterValue")) return (PFN_vkVoidFunction)proxy_GetSemaphoreCounterValueKHR;
    }
    return NULL;
}
