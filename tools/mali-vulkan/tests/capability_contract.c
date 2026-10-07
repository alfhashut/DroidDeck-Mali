/* Real glibc loader test; deliberately synthetic Android backend, never a Mali profile. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "capabilities.h"

static void codec(void) {
    struct mb_capabilities a = {0}, b;
    uint8_t wire[MB_CAPS_BYTES];
    a.queried = MB_Q_FEATURES2; a.core.shaderInt16 = 1;
    a.queue_count = 1; a.queues[0].queueCount = 3;
    a.memory.memoryHeapCount = 1; a.memory.memoryTypeCount = 1;
    a.memory.memoryHeaps[0].size = UINT64_C(0xfedcba9876543210);
    a.extension_count = 1; strcpy(a.extensions[0].extensionName, "HOST_CODEC_ONLY");
    a.extensions[0].specVersion = 37;
    mb_encode_capabilities(wire, &a);
    assert(mb_decode_capabilities(wire, &b) == 0);
    assert(b.core.shaderInt16 == 1 && b.core.textureCompressionBC == 0);
    assert(b.memory.memoryHeaps[0].size == a.memory.memoryHeaps[0].size);
    assert(!strcmp(b.extensions[0].extensionName, "HOST_CODEC_ONLY"));
    assert(b.extensions[0].specVersion == 37 && b.queues[0].queueCount == 3);
    mb_put_u32(wire + 4, MB_MAX_EXTENSIONS + 1); assert(mb_decode_capabilities(wire, &b) < 0);
    mb_encode_capabilities(wire, &a); mb_put_u32(wire + 12, 2); assert(mb_decode_capabilities(wire, &b) < 0);
    a.memory.memoryTypes[0].heapIndex = 1; mb_encode_capabilities(wire, &a); assert(mb_decode_capabilities(wire, &b) < 0);
    a.memory.memoryTypes[0].heapIndex = 0; mb_encode_capabilities(wire, &a);
    memset(wire + MB_CAPS_BYTES - MB_MAX_EXTENSIONS * MB_EXTENSION_BYTES, 'x', MB_NAME_BYTES);
    assert(mb_decode_capabilities(wire, &b) < 0);
}
int main(void) {
    codec();
    uint32_t n = 0; assert(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties extensions[32]; assert(n <= 32);
    assert(vkEnumerateInstanceExtensionProperties(NULL, &n, extensions) == VK_SUCCESS);
    const char *enabled[4]; uint32_t enabled_count = 0;
    for (uint32_t i = 0; i < n; ++i) if (!strcmp(extensions[i].extensionName, "VK_KHR_get_physical_device_properties2") ||
        !strcmp(extensions[i].extensionName, "VK_KHR_external_memory_capabilities") ||
        !strcmp(extensions[i].extensionName, "VK_KHR_external_semaphore_capabilities") ||
        !strcmp(extensions[i].extensionName, "VK_KHR_external_fence_capabilities")) enabled[enabled_count++] = extensions[i].extensionName;
    assert(enabled_count == 4); /* Android surface must not be advertised by the ICD. */
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app,
        .enabledExtensionCount=enabled_count, .ppEnabledExtensionNames=enabled};
    VkInstance instance; assert(vkCreateInstance(&ci, NULL, &instance) == VK_SUCCESS);
    VkPhysicalDevice d; n=1; assert(vkEnumeratePhysicalDevices(instance, &n, &d) == VK_SUCCESS && n==1);
    n=0; assert(vkEnumerateDeviceExtensionProperties(d,NULL,&n,NULL)==VK_SUCCESS && n==9);
    n=1; assert(vkEnumerateDeviceExtensionProperties(d,NULL,&n,extensions)==VK_INCOMPLETE && n==1);
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(d,&features);
    assert(features.samplerAnisotropy==1 && features.textureCompressionBC==0);
    VkQueueFamilyProperties q[2]; n=1; vkGetPhysicalDeviceQueueFamilyProperties(d,&n,q);
    assert(n==1 && q[0].queueCount==1 && q[0].minImageTransferGranularity.height==2);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(d,&memory);
    assert(memory.memoryTypeCount==2 && memory.memoryHeaps[0].size==UINT64_C(0x123456789abcdef));
#define GET(name) PFN_vk##name name=(PFN_vk##name)vkGetInstanceProcAddr(instance,"vk" #name "KHR"); assert(name)
    GET(GetPhysicalDeviceFeatures2); GET(GetPhysicalDeviceProperties2); GET(GetPhysicalDeviceFormatProperties2);
    GET(GetPhysicalDeviceImageFormatProperties2); GET(GetPhysicalDeviceExternalBufferProperties);
    GET(GetPhysicalDeviceExternalSemaphoreProperties); GET(GetPhysicalDeviceExternalFenceProperties);
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, .scalarBlockLayout=99};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext=&scalar};
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext=&timeline};
    GetPhysicalDeviceFeatures2(d,&f2); assert(timeline.timelineSemaphore==1 && scalar.scalarBlockLayout==0 && timeline.pNext==&scalar);
    VkPhysicalDeviceIDProperties id={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=&id};
    GetPhysicalDeviceProperties2(d,&p2); assert(id.deviceUUID[15]==16 && p2.properties.apiVersion==VK_MAKE_VERSION(1,1,131));
    VkDrmFormatModifierPropertiesEXT modifier;
    VkDrmFormatModifierPropertiesListEXT list={.sType=VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, .drmFormatModifierCount=1, .pDrmFormatModifierProperties=&modifier};
    VkFormatProperties2 fmt={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext=&list};
    GetPhysicalDeviceFormatProperties2(d,VK_FORMAT_R8G8B8A8_UNORM,&fmt);
    assert(list.drmFormatModifierCount==1 && modifier.drmFormatModifier==UINT64_C(0x123456789abcdef0));
    VkPhysicalDeviceExternalImageFormatInfo ei={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 ii={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext=&ei,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT};
    VkExternalImageFormatProperties ep={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&ep};
    assert(GetPhysicalDeviceImageFormatProperties2(d,&ii,&ip)==VK_SUCCESS);
    assert(ep.externalMemoryProperties.externalMemoryFeatures==VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    assert(ip.imageFormatProperties.maxResourceSize==UINT64_C(0x123456789abcdef));
    VkPhysicalDeviceExternalBufferInfo bi={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkExternalBufferProperties bp={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    GetPhysicalDeviceExternalBufferProperties(d,&bi,&bp); assert(bp.externalMemoryProperties.externalMemoryFeatures==(VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT));
    VkPhysicalDeviceExternalSemaphoreInfo si={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties sp={.sType=VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    GetPhysicalDeviceExternalSemaphoreProperties(d,&si,&sp); assert(sp.externalSemaphoreFeatures==VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
    VkPhysicalDeviceExternalFenceInfo fi={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,.handleType=VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalFenceProperties fp={.sType=VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
    GetPhysicalDeviceExternalFenceProperties(d,&fi,&fp); assert(fp.externalFenceFeatures==VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT);
    vkDestroyInstance(instance,NULL);
    puts("capability contracts: codec, counts, false bits, pNext, 64-bit values, external flags, cleanup OK");
    return 0;
}
