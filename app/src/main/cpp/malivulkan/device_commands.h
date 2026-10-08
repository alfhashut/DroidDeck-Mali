/* Only device creation, queue lookup and destruction. No renderer operations. */
static uint32_t native_device_command(struct vk_session *s, uint32_t op, const uint8_t *wire,
        uint32_t bytes, uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    if (op == MB_DEVICE_CREATE) {
        struct mb_device_request r;
        if (mb_decode_device_request(wire, bytes, &r) || r.physical_id > s->count) return MB_PROTOCOL_ERROR;
        uint8_t *snapshot = malloc(MB_PROPERTIES_BYTES + MB_CAPS_BYTES);
        struct mb_capabilities *caps = calloc(1, sizeof(*caps));
        if (!snapshot || !caps) { free(snapshot); free(caps); *result = VK_ERROR_OUT_OF_HOST_MEMORY; return MB_INTERNAL_ERROR; }
        uint32_t status = native_snapshot(s, r.physical_id, snapshot, result);
        if (status == MB_OK && mb_decode_capabilities(snapshot + MB_PROPERTIES_BYTES, caps)) status = MB_INTERNAL_ERROR;
        free(snapshot);
        if (status != MB_OK) { free(caps); return status; }
        if (r.family >= caps->queue_count || r.count > caps->queues[r.family].queueCount) {
            free(caps); ERROR("invalid queue family/count: %u/%u", r.family, r.count); return MB_PROTOCOL_ERROR;
        }
        for (uint32_t i = 0; i < r.extension_count; ++i)
            if (!mb_has_extension(caps->extensions, caps->extension_count, r.extensions[i])) {
                ERROR("unsupported requested extension: %s", r.extensions[i]);
                free(caps); *result = VK_ERROR_EXTENSION_NOT_PRESENT; return MB_VULKAN_ERROR;
            }
#define MB_FEATURE(n) if (r.features.n && !caps->core.n) { \
        ERROR("unsupported requested feature: " #n); free(caps); \
        *result = VK_ERROR_FEATURE_NOT_PRESENT; return MB_VULKAN_ERROR; }
#include "features_fields.def"
#undef MB_FEATURE
        uint32_t queue_flags = caps->queues[r.family].queueFlags;
        free(caps);
        struct native_device *d = NULL;
        for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
            if (!s->logical[i].handle) { d = &s->logical[i]; break; }
        if (!d || s->next_device_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
        PFN_vkCreateDevice create = (PFN_vkCreateDevice)s->gipa(s->instance, "vkCreateDevice");
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)s->gipa(s->instance, "vkGetDeviceProcAddr");
        PFN_vkDestroyDevice fallback_destroy = (PFN_vkDestroyDevice)s->gipa(s->instance, "vkDestroyDevice");
        if (!create || !gdpa || !fallback_destroy) return MB_LOADER_ERROR;
        const char *extensions[MB_DEVICE_MAX_EXTENSIONS];
        for (uint32_t i = 0; i < r.extension_count; ++i) extensions[i] = r.extensions[i];
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = r.family, .queueCount = r.count, .pQueuePriorities = r.priorities};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .enabledExtensionCount = r.extension_count,
            .ppEnabledExtensionNames = extensions, .pEnabledFeatures = &r.features};
        VkPhysicalDeviceProperties properties; get_properties(s, r.physical_id - 1, &properties);
        LOG("device test selected physical ID=%u name=%s API=%u.%u.%u", r.physical_id, properties.deviceName,
            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        LOG("device test queue family=%u count=%u", r.family, r.count);
        LOG("enabled extensions count=%u", r.extension_count);
        for (uint32_t i = 0; i < r.extension_count; ++i) LOG("enabled extension: %s", r.extensions[i]);
        uint32_t enabled_features = 0;
#define MB_FEATURE(n) if (r.features.n) { LOG("enabled feature: " #n "=1"); ++enabled_features; }
#include "features_fields.def"
#undef MB_FEATURE
        LOG("enabled core feature bits=%u; extension feature bits=0", enabled_features);
        VkDevice handle = VK_NULL_HANDLE;
        *result = create(s->devices[r.physical_id - 1], &info, NULL, &handle);
        LOG("broker vkCreateDevice result=%d", (int)*result);
        if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(handle, "vkDestroyDevice");
        PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)gdpa(handle, "vkGetDeviceQueue");
        /* Android core entry points must exist; retain a destroy fallback for cleanup. */
        if (!destroy) destroy = fallback_destroy;
        if (!get) {
            destroy(handle, NULL);
            LOG("vkDestroyDevice (missing queue entry point after create)");
            *result = VK_ERROR_INITIALIZATION_FAILED; return MB_LOADER_ERROR;
        }
        memset(d, 0, sizeof(*d)); d->handle = handle; d->destroy = destroy; d->get_queue = get;
        d->id = ++s->next_device_id; d->family = r.family; d->count = r.count; d->gdpa = gdpa; d->queue_flags = queue_flags;
        if (s->wire_version == MB_SUBMIT_VERSION && native_submit_init(d)) {
            native_close_device(d); *result = VK_ERROR_INITIALIZATION_FAILED; return MB_LOADER_ERROR;
        }
        mb_put_u32(reply, d->id); *extra = 4; *count = 1; return MB_OK;
    }
    if (bytes != (op == MB_DEVICE_QUEUE ? 12u : 4u)) return MB_PROTOCOL_ERROR;
    uint32_t id = mb_get_u32(wire);
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle && s->logical[i].id == id) { d = &s->logical[i]; break; }
    if (!d) { ERROR("invalid logical device ID=%u", id); return MB_PROTOCOL_ERROR; }
    if (op == MB_DEVICE_DESTROY) {
        native_close_device(d); return MB_OK;
    }
    uint32_t family = mb_get_u32(wire + 4), index = mb_get_u32(wire + 8);
    if (family != d->family || index >= d->count) return MB_PROTOCOL_ERROR;
    if (!d->queue_ids[index]) {
        if (s->next_queue_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
        VkQueue queue = VK_NULL_HANDLE; d->get_queue(d->handle, family, index, &queue);
        if (!queue) { *result = VK_ERROR_INITIALIZATION_FAILED; return MB_VULKAN_ERROR; }
        d->queues[index] = queue; d->queue_ids[index] = ++s->next_queue_id;
        LOG("queue acquired device ID=%u family=%u index=%u queue ID=%u", id, family, index, d->queue_ids[index]);
    }
    mb_put_u32(reply, d->queue_ids[index]); *extra = 4; *count = 1; return MB_OK;
}
