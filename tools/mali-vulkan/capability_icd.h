/* Query-only ICD helpers. Real driver inventory does not imply a logical device implementation. */
static VkResult cap_InstanceExtensions(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    VkExtensionProperties raw[MB_MAX_EXTENSIONS], exposed[4]; uint32_t version, n, found = 0;
    VkResult result = mb_global_inventory(&version, &n, raw);
    if (result != VK_SUCCESS) { *count = 0; return result; }
    const char *const names[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
    for (unsigned j = 0; j < 4; ++j)
        for (uint32_t i = 0; i < n; ++i) if (!strcmp(raw[i].extensionName, names[j])) { exposed[found++] = raw[i]; break; }
    if (!out) { *count = found; return VK_SUCCESS; }
    uint32_t written = *count < found ? *count : found;
    memcpy(out, exposed, written * sizeof(*out)); *count = written;
    return written < found ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult cap_DeviceExtensions(VkPhysicalDevice d, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    const struct mb_capabilities *p = &((struct proxy_device *)d)->capabilities;
    if (!out) { *count = p->extension_count; return VK_SUCCESS; }
    uint32_t written = *count < p->extension_count ? *count : p->extension_count;
    memcpy(out, p->extensions, written * sizeof(*out)); *count = written;
    return written < p->extension_count ? VK_INCOMPLETE : VK_SUCCESS;
}
static void cap_Queues(VkPhysicalDevice d, uint32_t *count, VkQueueFamilyProperties *out) {
    const struct mb_capabilities *p = &((struct proxy_device *)d)->capabilities;
    if (!out) { *count = p->queue_count; return; }
    uint32_t written = *count < p->queue_count ? *count : p->queue_count;
    memcpy(out, p->queues, written * sizeof(*out)); *count = written;
}
static VkResult cap_rpc(VkPhysicalDevice d, uint32_t op, uint8_t *request, uint32_t size,
                        uint8_t *reply, uint32_t capacity, uint32_t *bytes) {
    struct proxy_device *p = (struct proxy_device *)d;
    mb_put_u32(request, p->id);
    pthread_mutex_lock(&p->owner->lock);
    VkResult result = rpc(p->owner, op, request, size, reply, bytes, capacity);
    pthread_mutex_unlock(&p->owner->lock);
    return result;
}
static void cap_Format(VkPhysicalDevice d, VkFormat f, VkFormatProperties *out, VkDrmFormatModifierPropertiesListEXT *list) {
    uint8_t r[8], reply[MB_PREFIX_BYTES + 16 + MB_MAX_MODIFIERS * 16]; uint32_t bytes;
    memset(out, 0, sizeof(*out)); mb_put_u32(r + 4, f);
    VkResult result = cap_rpc(d, MB_FORMAT, r, sizeof(r), reply, sizeof(reply), &bytes);
    if (result != VK_SUCCESS || bytes < MB_PREFIX_BYTES + 16 || mb_get_u32(reply + 8) != 1) {
        LOG("format query failed: %d (not a supported-capability result)", result); return;
    }
    const uint8_t *v = reply + MB_PREFIX_BYTES;
    uint32_t n = mb_get_u32(v + 12);
    if (n > MB_MAX_MODIFIERS || bytes != MB_PREFIX_BYTES + 16 + n * 16) { LOG("invalid modifier reply"); return; }
    *out = (VkFormatProperties){mb_get_u32(v), mb_get_u32(v + 4), mb_get_u32(v + 8)};
    if (list) {
        uint32_t written = list->pDrmFormatModifierProperties && list->drmFormatModifierCount < n ? list->drmFormatModifierCount : n;
        if (list->pDrmFormatModifierProperties) {
            for (uint32_t i = 0; i < written; ++i) {
                const uint8_t *m = v + 16 + i * 16;
                list->pDrmFormatModifierProperties[i] = (VkDrmFormatModifierPropertiesEXT){mb_get_u64(m), mb_get_u32(m+8), mb_get_u32(m+12)};
            }
        }
        list->drmFormatModifierCount = written;
    }
}
static VkResult cap_Image(VkPhysicalDevice d, VkFormat f, VkImageType type, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags, uint32_t handle, uint32_t modifier_present,
        uint64_t modifier, VkImageFormatProperties *out, VkExternalMemoryProperties *external) {
    uint8_t r[40], reply[MB_PREFIX_BYTES + 44]; uint32_t bytes;
    mb_put_u32(r+4, f); mb_put_u32(r+8, type); mb_put_u32(r+12, tiling); mb_put_u32(r+16, usage);
    mb_put_u32(r+20, flags); mb_put_u32(r+24, handle); mb_put_u32(r+28, modifier_present); mb_put_u64(r+32, modifier);
    VkResult result = cap_rpc(d, MB_IMAGE, r, sizeof(r), reply, sizeof(reply), &bytes);
    if (result != VK_SUCCESS) return result;
    if (bytes != sizeof(reply) || mb_get_u32(reply + 8) != 1) return VK_ERROR_INITIALIZATION_FAILED;
    const uint8_t *v = reply + MB_PREFIX_BYTES;
    out->maxExtent = (VkExtent3D){mb_get_u32(v), mb_get_u32(v+4), mb_get_u32(v+8)};
    out->maxMipLevels = mb_get_u32(v+12); out->maxArrayLayers = mb_get_u32(v+16);
    out->sampleCounts = mb_get_u32(v+20); out->maxResourceSize = mb_get_u64(v+24);
    if (external) *external = (VkExternalMemoryProperties){mb_get_u32(v+32), mb_get_u32(v+36), mb_get_u32(v+40)};
    return VK_SUCCESS;
}
static void cap_Sparse(VkPhysicalDevice d, VkFormat f, VkImageType type, VkSampleCountFlagBits samples,
        VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *out) {
    uint8_t r[24], reply[MB_PREFIX_BYTES + MB_MAX_SPARSE * 20]; uint32_t bytes;
    mb_put_u32(r+4, f); mb_put_u32(r+8, type); mb_put_u32(r+12, samples); mb_put_u32(r+16, usage); mb_put_u32(r+20, tiling);
    VkResult result = cap_rpc(d, MB_SPARSE, r, sizeof(r), reply, sizeof(reply), &bytes);
    if (result != VK_SUCCESS) { *count = 0; LOG("sparse query failed (not an absence result): %d", result); return; }
    uint32_t n = mb_get_u32(reply + 8);
    if (n > MB_MAX_SPARSE || bytes != MB_PREFIX_BYTES + n * 20) { *count = 0; LOG("invalid sparse reply"); return; }
    uint32_t written = out && *count < n ? *count : n;
    if (out) for (uint32_t i = 0; i < written; ++i) {
        const uint8_t *v = reply + MB_PREFIX_BYTES + i * 20;
        out[i] = (VkSparseImageFormatProperties){mb_get_u32(v), {mb_get_u32(v+4), mb_get_u32(v+8), mb_get_u32(v+12)}, mb_get_u32(v+16)};
    }
    *count = written;
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2 *out) {
    const struct mb_capabilities *p = &((struct proxy_device *)d)->capabilities;
    out->features = p->core; unsigned nodes = 0;
    for (VkBaseOutStructure *v = out->pNext; v && ++nodes <= 16; v = v->pNext) {
#define CHAIN(type, member, tag, bit) case tag: { \
        if (!(p->queried & bit)) { LOG("feature sType=%u was not queryable on Android", tag); break; } \
        type *target = (type *)v; void *next = target->pNext; *target = p->member; target->sType = tag; target->pNext = next; break; }
        switch (v->sType) {
            CHAIN(VkPhysicalDeviceTimelineSemaphoreFeatures, timeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, MB_Q_TIMELINE)
            CHAIN(VkPhysicalDeviceScalarBlockLayoutFeatures, scalar, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, MB_Q_SCALAR)
            CHAIN(VkPhysicalDeviceSamplerYcbcrConversionFeatures, ycbcr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, MB_Q_YCBCR)
            CHAIN(VkPhysicalDeviceShaderFloat16Int8Features, float16, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, MB_Q_FLOAT16)
            CHAIN(VkPhysicalDeviceRobustness2FeaturesEXT, robustness, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, MB_Q_ROBUSTNESS)
            CHAIN(VkPhysicalDeviceDynamicRenderingFeatures, dynamic, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, MB_Q_DYNAMIC)
            CHAIN(VkPhysicalDevicePresentIdFeaturesKHR, present_id, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, MB_Q_PRESENT_ID)
            CHAIN(VkPhysicalDevicePresentWaitFeaturesKHR, present_wait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, MB_Q_PRESENT_WAIT)
            default: LOG("unbridged features2 sType=%u left untouched", v->sType); break;
        }
#undef CHAIN
    }
    if (nodes > 16) LOG("features2 chain exceeds bound");
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *out) {
    struct proxy_device *device = (struct proxy_device *)d;
    out->properties = device->properties; unsigned nodes = 0;
    for (VkBaseOutStructure *v = out->pNext; v && ++nodes <= 16; v = v->pNext) {
        if (v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES && (device->capabilities.queried & MB_Q_ID)) {
            VkPhysicalDeviceIDProperties *target = (VkPhysicalDeviceIDProperties *)v;
            void *next = target->pNext; *target = device->capabilities.id; target->pNext = next;
            target->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        } else if (v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && (device->capabilities.queried & MB_Q_DRM)) {
            VkPhysicalDeviceDrmPropertiesEXT *target = (VkPhysicalDeviceDrmPropertiesEXT *)v;
            void *next = target->pNext; *target = device->capabilities.drm; target->pNext = next;
            target->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        } else LOG("unqueried/unbridged properties2 sType=%u left untouched", v->sType);
    }
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice d, uint32_t *count, VkQueueFamilyProperties2 *out) {
    const struct mb_capabilities *p = &((struct proxy_device *)d)->capabilities;
    uint32_t n = out && *count < p->queue_count ? *count : p->queue_count;
    if (out) for (uint32_t i = 0; i < n; ++i) out[i].queueFamilyProperties = p->queues[i];
    *count = n;
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties2 *out) {
    out->memoryProperties = ((struct proxy_device *)d)->capabilities.memory;
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice d, VkFormat f, VkFormatProperties2 *out) {
    VkDrmFormatModifierPropertiesListEXT *list = NULL; unsigned nodes = 0;
    for (VkBaseOutStructure *v = out->pNext; v && ++nodes <= 16; v = v->pNext) {
        if (v->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) list = (void *)v;
        else LOG("unbridged format properties sType=%u left untouched", v->sType);
    }
    cap_Format(d, f, &out->formatProperties, list);
}
static VKAPI_ATTR VkResult VKAPI_CALL cap_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice d,
        const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
    uint32_t handle = 0, modifier_present = 0; uint64_t modifier = 0; unsigned nodes = 0;
    for (const VkBaseInStructure *v = info->pNext; v; v = v->pNext) {
        if (++nodes > 16) return VK_ERROR_INITIALIZATION_FAILED;
        if (v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)
            handle = ((const VkPhysicalDeviceExternalImageFormatInfo *)v)->handleType;
        else if (v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT) {
            const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *p = (const void *)v;
            if (p->sharingMode != VK_SHARING_MODE_EXCLUSIVE || p->queueFamilyIndexCount) return VK_ERROR_FEATURE_NOT_PRESENT;
            modifier_present = 1; modifier = p->drmFormatModifier;
        } else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkExternalImageFormatProperties *external = NULL; nodes = 0;
    for (VkBaseOutStructure *v = out->pNext; v; v = v->pNext) {
        if (++nodes > 16) return VK_ERROR_INITIALIZATION_FAILED;
        if (v->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) external = (void *)v;
        else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return cap_Image(d, info->format, info->type, info->tiling, info->usage, info->flags, handle,
        modifier_present, modifier, &out->imageFormatProperties, external ? &external->externalMemoryProperties : NULL);
}
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice d,
        const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out) {
    uint8_t r[16], reply[MB_PREFIX_BYTES + 12]; uint32_t bytes;
    mb_put_u32(r+4, info->flags); mb_put_u32(r+8, info->usage); mb_put_u32(r+12, info->handleType);
    VkResult result = info->pNext ? VK_ERROR_FEATURE_NOT_PRESENT : cap_rpc(d, MB_BUFFER, r, sizeof(r), reply, sizeof(reply), &bytes);
    if (result != VK_SUCCESS || bytes != sizeof(reply) || mb_get_u32(reply+8) != 1) { LOG("external buffer NOT QUERIED: %d", result); return; }
    const uint8_t *v = reply + MB_PREFIX_BYTES;
    out->externalMemoryProperties = (VkExternalMemoryProperties){mb_get_u32(v), mb_get_u32(v+4), mb_get_u32(v+8)};
}
#define SYNC_QUERY(kind, opcode, properties) \
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceExternal##kind##Properties(VkPhysicalDevice d, \
        const VkPhysicalDeviceExternal##kind##Info *info, VkExternal##kind##Properties *out) { \
    uint8_t r[8], reply[MB_PREFIX_BYTES + 12]; uint32_t bytes; mb_put_u32(r+4, info->handleType); \
    VkResult result = info->pNext ? VK_ERROR_FEATURE_NOT_PRESENT : cap_rpc(d, opcode, r, sizeof(r), reply, sizeof(reply), &bytes); \
    if (result != VK_SUCCESS || bytes != sizeof(reply) || mb_get_u32(reply+8) != 1) { LOG("external " #kind " NOT QUERIED: %d", result); return; } \
    const uint8_t *v = reply + MB_PREFIX_BYTES; out->properties = mb_get_u32(v); \
    out->exportFromImportedHandleTypes = mb_get_u32(v+4); out->compatibleHandleTypes = mb_get_u32(v+8); \
}
SYNC_QUERY(Semaphore, MB_SEMAPHORE, externalSemaphoreFeatures)
SYNC_QUERY(Fence, MB_FENCE, externalFenceFeatures)
#undef SYNC_QUERY
static VKAPI_ATTR void VKAPI_CALL cap_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice d,
        const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count, VkSparseImageFormatProperties2 *out) {
    VkSparseImageFormatProperties p[MB_MAX_SPARSE]; uint32_t n = MB_MAX_SPARSE;
    cap_Sparse(d, info->format, info->type, info->samples, info->usage, info->tiling, &n, p);
    uint32_t written = out && *count < n ? *count : n;
    if (out) for (uint32_t i = 0; i < written; ++i) out[i].properties = p[i];
    *count = written;
}
static PFN_vkVoidFunction cap_proc(const char *name) {
#define CAP_ENTRY(n) if (!strcmp(name, "vk" #n) || !strcmp(name, "vk" #n "KHR")) return (PFN_vkVoidFunction)cap_##n;
    CAP_ENTRY(GetPhysicalDeviceFeatures2)
    CAP_ENTRY(GetPhysicalDeviceProperties2)
    CAP_ENTRY(GetPhysicalDeviceQueueFamilyProperties2)
    CAP_ENTRY(GetPhysicalDeviceMemoryProperties2)
    CAP_ENTRY(GetPhysicalDeviceFormatProperties2)
    CAP_ENTRY(GetPhysicalDeviceImageFormatProperties2)
    CAP_ENTRY(GetPhysicalDeviceSparseImageFormatProperties2)
    CAP_ENTRY(GetPhysicalDeviceExternalBufferProperties)
    CAP_ENTRY(GetPhysicalDeviceExternalSemaphoreProperties)
    CAP_ENTRY(GetPhysicalDeviceExternalFenceProperties)
#undef CAP_ENTRY
    return NULL;
}
