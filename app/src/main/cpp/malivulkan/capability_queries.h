/* Instance/physical-device queries only. No logical device, FD operation or WSI call. */
static VkResult native_global(void *library, uint32_t *version, uint32_t *count, VkExtensionProperties *extensions) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    if (!gipa) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceVersion get_version = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    *version = VK_API_VERSION_1_0;
    VkResult result = get_version ? get_version(version) : VK_SUCCESS;
    if (result != VK_SUCCESS || !enumerate) return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        *count = 0;
        result = enumerate(NULL, count, NULL);
        if (result != VK_SUCCESS) return result;
        if (*count > MB_MAX_EXTENSIONS) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (!*count) return VK_SUCCESS;
        result = enumerate(NULL, count, extensions);
        if (result == VK_SUCCESS && *count <= MB_MAX_EXTENSIONS) return VK_SUCCESS;
        if (result != VK_INCOMPLETE) return result;
    }
    return VK_INCOMPLETE;
}
static PFN_vkVoidFunction native_query11(struct vk_session *s, const char *core, const char *khr, unsigned bit) {
    if (s->native_api >= VK_API_VERSION_1_1) return s->gipa(s->instance, core);
    return (s->enabled_queries & (1u << bit)) ? s->gipa(s->instance, khr) : NULL;
}
#define QUERY11(s, n, bit) ((PFN_vk##n)native_query11(s, "vk" #n, "vk" #n "KHR", bit))

static uint32_t native_snapshot(struct vk_session *s, uint32_t id, uint8_t *wire, VkResult *result) {
    struct mb_capabilities *p = calloc(1, sizeof(*p));
    if (!p) { *result = VK_ERROR_OUT_OF_HOST_MEMORY; return MB_INTERNAL_ERROR; }
    VkPhysicalDevice device = s->devices[id - 1];
    PFN_vkEnumerateDeviceExtensionProperties extensions =
        (PFN_vkEnumerateDeviceExtensionProperties)s->gipa(s->instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures features = (PFN_vkGetPhysicalDeviceFeatures)s->gipa(s->instance, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)s->gipa(s->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)s->gipa(s->instance, "vkGetPhysicalDeviceMemoryProperties");
    uint32_t status = MB_UNSUPPORTED;
    if (!extensions || !features || !queues || !memory) goto done;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        p->extension_count = 0;
        *result = extensions(device, NULL, &p->extension_count, NULL);
        if (*result != VK_SUCCESS) { status = MB_VULKAN_ERROR; goto done; }
        if (p->extension_count > MB_MAX_EXTENSIONS) { status = MB_INTERNAL_ERROR; goto done; }
        if (!p->extension_count) break;
        *result = extensions(device, NULL, &p->extension_count, p->extensions);
        if (*result == VK_SUCCESS && p->extension_count <= MB_MAX_EXTENSIONS) break;
        if (*result != VK_INCOMPLETE || attempt == 3) { status = MB_VULKAN_ERROR; goto done; }
    }
    VkPhysicalDeviceProperties properties;
    get_properties(s, id - 1, &properties);
    features(device, &p->core);
    PFN_vkGetPhysicalDeviceFeatures2 f2 = QUERY11(s, GetPhysicalDeviceFeatures2, 0);
    if (f2) {
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        void **tail = &f.pNext;
#define FEATURE(member, type, tag, bit, core_api, extension) do { \
        if (properties.apiVersion >= core_api || mb_has_extension(p->extensions, p->extension_count, extension)) { \
            p->member.sType = tag; *tail = &p->member; tail = &p->member.pNext; p->queried |= bit; \
        } \
    } while (0)
        FEATURE(timeline, VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                MB_Q_TIMELINE, VK_API_VERSION_1_2, "VK_KHR_timeline_semaphore");
        FEATURE(scalar, VkPhysicalDeviceScalarBlockLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                MB_Q_SCALAR, VK_API_VERSION_1_2, "VK_EXT_scalar_block_layout");
        FEATURE(ycbcr, VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
                MB_Q_YCBCR, VK_API_VERSION_1_1, "VK_KHR_sampler_ycbcr_conversion");
        FEATURE(float16, VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                MB_Q_FLOAT16, VK_API_VERSION_1_2, "VK_KHR_shader_float16_int8");
        FEATURE(robustness, VkPhysicalDeviceRobustness2FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                MB_Q_ROBUSTNESS, UINT32_MAX, "VK_EXT_robustness2");
        FEATURE(dynamic, VkPhysicalDeviceDynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                MB_Q_DYNAMIC, VK_API_VERSION_1_3, "VK_KHR_dynamic_rendering");
        FEATURE(present_id, VkPhysicalDevicePresentIdFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                MB_Q_PRESENT_ID, UINT32_MAX, "VK_KHR_present_id");
        FEATURE(present_wait, VkPhysicalDevicePresentWaitFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                MB_Q_PRESENT_WAIT, UINT32_MAX, "VK_KHR_present_wait");
#undef FEATURE
        f2(device, &f); p->core = f.features; p->queried |= MB_Q_FEATURES2;
    }
    PFN_vkGetPhysicalDeviceProperties2 p2 = QUERY11(s, GetPhysicalDeviceProperties2, 0);
    if (p2) {
        VkPhysicalDeviceProperties2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        void **tail = &q.pNext;
        if (properties.apiVersion >= VK_API_VERSION_1_1 || mb_has_extension(p->extensions, p->extension_count, "VK_KHR_external_memory")) {
            p->id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            *tail = &p->id; tail = &p->id.pNext; p->queried |= MB_Q_ID;
        }
        if (mb_has_extension(p->extensions, p->extension_count, "VK_EXT_physical_device_drm")) {
            p->drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            *tail = &p->drm; p->queried |= MB_Q_DRM;
        }
        p2(device, &q); properties = q.properties; p->queried |= MB_Q_PROPERTIES2;
    }
    queues(device, &p->queue_count, NULL);
    if (p->queue_count > MB_MAX_QUEUES) { status = MB_INTERNAL_ERROR; goto done; }
    if (p->queue_count) queues(device, &p->queue_count, p->queues);
    if (p->queue_count > MB_MAX_QUEUES) { status = MB_INTERNAL_ERROR; goto done; }
    memory(device, &p->memory);
    if (p->memory.memoryTypeCount > 32 || p->memory.memoryHeapCount > 16) { status = MB_INTERNAL_ERROR; goto done; }
    mb_encode_properties(wire, &properties);
    mb_encode_capabilities(wire + MB_PROPERTIES_BYTES, p);
    LOG("capabilities[%u]: extensions=%u queues=%u memory types=%u heaps=%u queried=0x%x",
        id, p->extension_count, p->queue_count, p->memory.memoryTypeCount, p->memory.memoryHeapCount, p->queried);
    status = MB_OK;
done:
    free(p);
    return status;
}
/* Device extension checks are independent of the proxy's supported command set. */
static int native_has(struct vk_session *s, VkPhysicalDevice device, const char *name) {
    PFN_vkEnumerateDeviceExtensionProperties get =
        (PFN_vkEnumerateDeviceExtensionProperties)s->gipa(s->instance, "vkEnumerateDeviceExtensionProperties");
    if (!get) return 0;
    VkExtensionProperties p[MB_MAX_EXTENSIONS]; uint32_t n = MB_MAX_EXTENSIONS;
    return get(device, NULL, &n, p) == VK_SUCCESS && n <= MB_MAX_EXTENSIONS && mb_has_extension(p, n, name);
}
static void encode_image(uint8_t *wire, const VkImageFormatProperties *p) {
    mb_put_u32(wire, p->maxExtent.width); mb_put_u32(wire + 4, p->maxExtent.height); mb_put_u32(wire + 8, p->maxExtent.depth);
    mb_put_u32(wire + 12, p->maxMipLevels); mb_put_u32(wire + 16, p->maxArrayLayers); mb_put_u32(wire + 20, p->sampleCounts);
    mb_put_u64(wire + 24, p->maxResourceSize);
}
static void encode_external(uint8_t *wire, const VkExternalMemoryProperties *p) {
    mb_put_u32(wire, p->externalMemoryFeatures); mb_put_u32(wire + 4, p->exportFromImportedHandleTypes);
    mb_put_u32(wire + 8, p->compatibleHandleTypes);
}
static uint32_t native_extra(struct vk_session *s, uint32_t op, const uint8_t *r, uint32_t size,
                             uint8_t *wire, uint32_t *bytes, uint32_t *count, VkResult *result) {
    if (size < 4) return MB_PROTOCOL_ERROR;
    uint32_t id = mb_get_u32(r);
    if (!id || id > s->count || !s->listed) return MB_PROTOCOL_ERROR;
    VkPhysicalDevice d = s->devices[id - 1];
    if (op == MB_FORMAT && size == 8) {
        PFN_vkGetPhysicalDeviceFormatProperties get =
            (PFN_vkGetPhysicalDeviceFormatProperties)s->gipa(s->instance, "vkGetPhysicalDeviceFormatProperties");
        if (!get) return MB_UNSUPPORTED;
        VkFormatProperties p = {0}; get(d, (VkFormat)mb_get_u32(r + 4), &p);
        mb_put_u32(wire, p.linearTilingFeatures); mb_put_u32(wire + 4, p.optimalTilingFeatures);
        mb_put_u32(wire + 8, p.bufferFeatures); uint32_t n = 0;
        PFN_vkGetPhysicalDeviceFormatProperties2 get2 = QUERY11(s, GetPhysicalDeviceFormatProperties2, 0);
        if (get2 && native_has(s, d, "VK_EXT_image_drm_format_modifier")) {
            VkDrmFormatModifierPropertiesEXT modifiers[MB_MAX_MODIFIERS];
            VkDrmFormatModifierPropertiesListEXT list = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
            VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
            get2(d, (VkFormat)mb_get_u32(r + 4), &p2);
            if (list.drmFormatModifierCount > MB_MAX_MODIFIERS) return MB_INTERNAL_ERROR;
            list.pDrmFormatModifierProperties = modifiers;
            if (list.drmFormatModifierCount) get2(d, (VkFormat)mb_get_u32(r + 4), &p2);
            n = list.drmFormatModifierCount;
            if (n > MB_MAX_MODIFIERS) return MB_INTERNAL_ERROR;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t *v = wire + 16 + i * 16;
                mb_put_u64(v, modifiers[i].drmFormatModifier); mb_put_u32(v + 8, modifiers[i].drmFormatModifierPlaneCount);
                mb_put_u32(v + 12, modifiers[i].drmFormatModifierTilingFeatures);
            }
        }
        mb_put_u32(wire + 12, n); *bytes = 16 + n * 16; *count = 1; return MB_OK;
    }
    if (op == MB_IMAGE && size == 40) {
        VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = (VkFormat)mb_get_u32(r + 4), .type = (VkImageType)mb_get_u32(r + 8),
            .tiling = (VkImageTiling)mb_get_u32(r + 12), .usage = mb_get_u32(r + 16), .flags = mb_get_u32(r + 20)};
        uint32_t handle = mb_get_u32(r + 24), modifier = mb_get_u32(r + 28);
        VkPhysicalDeviceExternalImageFormatInfo external = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = (VkExternalMemoryHandleTypeFlagBits)handle};
        VkExternalImageFormatProperties ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = mb_get_u64(r + 32), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (handle) {
            if (handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
                if (!native_has(s, d, "VK_ANDROID_external_memory_android_hardware_buffer")) return MB_UNSUPPORTED;
            } else if (handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
                if (!native_has(s, d, "VK_EXT_external_memory_dma_buf")) return MB_UNSUPPORTED;
            } else if (handle != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT || !native_has(s, d, "VK_KHR_external_memory_fd")) return MB_UNSUPPORTED;
            info.pNext = &external; p.pNext = &ext;
        }
        if (modifier) {
            if (info.tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT || !native_has(s, d, "VK_EXT_image_drm_format_modifier")) return MB_UNSUPPORTED;
            if (handle) external.pNext = &mod; else info.pNext = &mod;
        } else if (info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) return MB_PROTOCOL_ERROR;
        PFN_vkGetPhysicalDeviceImageFormatProperties2 get2 = QUERY11(s, GetPhysicalDeviceImageFormatProperties2, 0);
        if (get2) *result = get2(d, &info, &p);
        else {
            PFN_vkGetPhysicalDeviceImageFormatProperties get =
                (PFN_vkGetPhysicalDeviceImageFormatProperties)s->gipa(s->instance, "vkGetPhysicalDeviceImageFormatProperties");
            if (!get || handle || modifier) return MB_UNSUPPORTED;
            *result = get(d, info.format, info.type, info.tiling, info.usage, info.flags, &p.imageFormatProperties);
        }
        if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
        encode_image(wire, &p.imageFormatProperties); encode_external(wire + 32, &ext.externalMemoryProperties);
        *bytes = 44; *count = 1; return MB_OK;
    }
    if (op == MB_BUFFER && size == 16) {
        PFN_vkGetPhysicalDeviceExternalBufferProperties get = QUERY11(s, GetPhysicalDeviceExternalBufferProperties, 1);
        uint32_t handle = mb_get_u32(r + 12);
        if (!get || (handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ?
            !native_has(s, d, "VK_EXT_external_memory_dma_buf") :
            handle != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT || !native_has(s, d, "VK_KHR_external_memory_fd"))) return MB_UNSUPPORTED;
        VkPhysicalDeviceExternalBufferInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .flags = mb_get_u32(r + 4), .usage = mb_get_u32(r + 8), .handleType = (VkExternalMemoryHandleTypeFlagBits)handle};
        VkExternalBufferProperties p = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        get(d, &info, &p); encode_external(wire, &p.externalMemoryProperties); *bytes = 12; *count = 1; return MB_OK;
    }
    if ((op == MB_SEMAPHORE || op == MB_FENCE) && size == 8) {
        uint32_t handle = mb_get_u32(r + 4);
        if (op == MB_SEMAPHORE) {
            PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get = QUERY11(s, GetPhysicalDeviceExternalSemaphoreProperties, 2);
            if (!get || !native_has(s, d, "VK_KHR_external_semaphore_fd") ||
                (handle != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT && handle != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) return MB_UNSUPPORTED;
            VkPhysicalDeviceExternalSemaphoreInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                .handleType = (VkExternalSemaphoreHandleTypeFlagBits)handle};
            VkExternalSemaphoreProperties p = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
            get(d, &info, &p);
            mb_put_u32(wire, p.externalSemaphoreFeatures); mb_put_u32(wire + 4, p.exportFromImportedHandleTypes);
            mb_put_u32(wire + 8, p.compatibleHandleTypes);
        } else {
            PFN_vkGetPhysicalDeviceExternalFenceProperties get = QUERY11(s, GetPhysicalDeviceExternalFenceProperties, 3);
            if (!get || !native_has(s, d, "VK_KHR_external_fence_fd") ||
                (handle != VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT && handle != VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT)) return MB_UNSUPPORTED;
            VkPhysicalDeviceExternalFenceInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
                .handleType = (VkExternalFenceHandleTypeFlagBits)handle};
            VkExternalFenceProperties p = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES}; get(d, &info, &p);
            mb_put_u32(wire, p.externalFenceFeatures); mb_put_u32(wire + 4, p.exportFromImportedHandleTypes);
            mb_put_u32(wire + 8, p.compatibleHandleTypes);
        }
        *bytes = 12; *count = 1; return MB_OK;
    }
    if (op == MB_SPARSE && size == 24) {
        PFN_vkGetPhysicalDeviceSparseImageFormatProperties get =
            (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)s->gipa(s->instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
        if (!get) return MB_UNSUPPORTED;
        uint32_t n = 0; VkSparseImageFormatProperties p[MB_MAX_SPARSE];
#define SPARSE_ARGS d, (VkFormat)mb_get_u32(r+4), (VkImageType)mb_get_u32(r+8), \
        (VkSampleCountFlagBits)mb_get_u32(r+12), mb_get_u32(r+16), (VkImageTiling)mb_get_u32(r+20), &n
        get(SPARSE_ARGS, NULL);
        if (n > MB_MAX_SPARSE) return MB_INTERNAL_ERROR;
        if (n) get(SPARSE_ARGS, p);
#undef SPARSE_ARGS
        if (n > MB_MAX_SPARSE) return MB_INTERNAL_ERROR;
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t *v = wire + i * 20; mb_put_u32(v, p[i].aspectMask);
            mb_put_u32(v+4, p[i].imageGranularity.width); mb_put_u32(v+8, p[i].imageGranularity.height);
            mb_put_u32(v+12, p[i].imageGranularity.depth); mb_put_u32(v+16, p[i].flags);
        }
        *bytes = n * 20; *count = n; return MB_OK;
    }
    return MB_PROTOCOL_ERROR;
}
