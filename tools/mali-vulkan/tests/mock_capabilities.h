/* Deterministic host capabilities, explicitly NOT a Mali-G52 capability profile. */
static const char *const instance_names[] = {
    "VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities", "VK_KHR_android_surface"
};
static const char *const device_names[] = {
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_fence_fd", "VK_KHR_timeline_semaphore", "VK_EXT_scalar_block_layout",
    "VK_KHR_shader_float16_int8", "VK_EXT_robustness2", "VK_EXT_image_drm_format_modifier",
    "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"
};
static VkResult mock_extension_list(const char *const *names, uint32_t total, uint32_t *count, VkExtensionProperties *p) {
    if (!p) { *count = total; return VK_SUCCESS; }
    uint32_t written = *count < total ? *count : total;
    for (uint32_t i = 0; i < written; ++i) { memset(&p[i], 0, sizeof(p[i])); strcpy(p[i].extensionName, names[i]); p[i].specVersion = i + 1; }
    *count = written; return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult mock_version(uint32_t *v) { *v = mode == 6 ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1; return VK_SUCCESS; }
static VkResult mock_instance_extensions(const char *layer, uint32_t *count, VkExtensionProperties *p) {
    assert(!layer); return mock_extension_list(instance_names, mode == 6 ? 0 : 5, count, p);
}
static VkResult mock_device_extensions(VkPhysicalDevice d, const char *layer, uint32_t *count, VkExtensionProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42 && !layer);
    return mock_extension_list(device_names, mode == 6 ? 0 : mode >= 29 && mode != 33 ? 11 : 9, count, p);
}
static void mock_features(VkPhysicalDevice d, VkPhysicalDeviceFeatures *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42); memset(p, 0, sizeof(*p));
    p->samplerAnisotropy = mode == 7 ? 2 : VK_TRUE; p->shaderInt16 = VK_TRUE; p->textureCompressionBC = VK_FALSE;
}
static void mock_features2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2 *p) {
    assert(mode != 6); mock_features(d, &p->features);
    for (VkBaseOutStructure *v = p->pNext; v; v = v->pNext) {
        switch (v->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            ((VkPhysicalDeviceTimelineSemaphoreFeatures *)v)->timelineSemaphore = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
            ((VkPhysicalDeviceScalarBlockLayoutFeatures *)v)->scalarBlockLayout = VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
            ((VkPhysicalDeviceSamplerYcbcrConversionFeatures *)v)->samplerYcbcrConversion = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            ((VkPhysicalDeviceShaderFloat16Int8Features *)v)->shaderFloat16 = VK_FALSE;
            ((VkPhysicalDeviceShaderFloat16Int8Features *)v)->shaderInt8 = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
            ((VkPhysicalDeviceRobustness2FeaturesEXT *)v)->robustBufferAccess2 = VK_FALSE;
            ((VkPhysicalDeviceRobustness2FeaturesEXT *)v)->robustImageAccess2 = VK_FALSE;
            ((VkPhysicalDeviceRobustness2FeaturesEXT *)v)->nullDescriptor = VK_FALSE; break;
        default: assert(!"unsupported extension queried on mock driver");
        }
    }
}
static void mock_properties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    mock_properties(d, &p->properties);
    for (VkBaseOutStructure *v = p->pNext; v; v = v->pNext) {
        assert(v->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
        VkPhysicalDeviceIDProperties *id = (void *)v;
        for (unsigned i = 0; i < 16; ++i) { id->deviceUUID[i] = (uint8_t)(i + 1); id->driverUUID[i] = (uint8_t)(i + 2); }
        memset(id->deviceLUID, 0, sizeof(id->deviceLUID)); id->deviceNodeMask = 0; id->deviceLUIDValid = VK_FALSE;
    }
}
static void mock_queues(VkPhysicalDevice d, uint32_t *count, VkQueueFamilyProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42);
    if (!p) { *count = 2; return; }
    uint32_t written = *count < 2 ? *count : 2;
    for (uint32_t i = 0; i < written; ++i) p[i] = (VkQueueFamilyProperties){i ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
        i + 1, 32, {1, 2, 3}};
    if (mode == 12) for (uint32_t i = 0; i < written; ++i) p[i].queueFlags = VK_QUEUE_TRANSFER_BIT;
    if (mode == 14 && written == 2) { p[0].queueFlags = VK_QUEUE_TRANSFER_BIT; p[1].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT; }
    *count = written;
}
static void mock_memory(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42); memset(p, 0, sizeof(*p));
    p->memoryTypeCount = 2; p->memoryHeapCount = 1;
    p->memoryTypes[0] = (VkMemoryType){VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mode == 8 ? 1u : 0u};
    p->memoryTypes[1] = (VkMemoryType){VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | (mode == 29 ? 0 : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), 0};
    p->memoryHeaps[0] = (VkMemoryHeap){UINT64_C(0x123456789abcdef), VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}
static void mock_format(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42); (void)f;
    *p = (VkFormatProperties){VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, 0};
}
static void mock_format2(VkPhysicalDevice d, VkFormat f, VkFormatProperties2 *p) {
    mock_format(d, f, &p->formatProperties);
    if (p->pNext) {
        VkDrmFormatModifierPropertiesListEXT *list = p->pNext;
        assert(list->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
        if (list->pDrmFormatModifierProperties && list->drmFormatModifierCount)
            list->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT){UINT64_C(0x123456789abcdef0), 2, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT};
        list->drmFormatModifierCount = 1;
    }
}
static VkResult mock_image(VkPhysicalDevice d, VkFormat f, VkImageType t, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42); (void)f; (void)t; (void)tiling; (void)usage; (void)flags;
    *p = (VkImageFormatProperties){{2048, 1024, 1}, 12, 8, VK_SAMPLE_COUNT_1_BIT, UINT64_C(0x123456789abcdef)};
    return VK_SUCCESS;
}
static VkResult mock_image2(VkPhysicalDevice d, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *p) {
    VkResult result = mock_image(d, info->format, info->type, info->tiling, info->usage, info->flags, &p->imageFormatProperties);
    if (p->pNext) {
        VkExternalImageFormatProperties *ext = p->pNext;
        const VkPhysicalDeviceExternalImageFormatInfo *external = info->pNext;
        ext->externalMemoryProperties = (VkExternalMemoryProperties){VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT, external->handleType, external->handleType};
    }
    return result;
}
static void mock_buffer(VkPhysicalDevice d, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42);
    p->externalMemoryProperties = (VkExternalMemoryProperties){VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
        info->handleType, info->handleType};
}
static void mock_semaphore(VkPhysicalDevice d, const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42);
    p->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    p->compatibleHandleTypes = info->handleType; p->exportFromImportedHandleTypes = 0;
}
static void mock_fence(VkPhysicalDevice d, const VkPhysicalDeviceExternalFenceInfo *info, VkExternalFenceProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42);
    p->externalFenceFeatures = VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
    p->compatibleHandleTypes = info->handleType; p->exportFromImportedHandleTypes = 0;
}
static void mock_sparse(VkPhysicalDevice d, VkFormat f, VkImageType t, VkSampleCountFlagBits samples,
        VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *p) {
    assert(d == (VkPhysicalDevice)(uintptr_t)0x42); (void)f; (void)t; (void)samples; (void)usage; (void)tiling; (void)p; *count = 0;
}
