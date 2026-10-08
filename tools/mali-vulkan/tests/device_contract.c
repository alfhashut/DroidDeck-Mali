/* Exercise real glibc loader -> ICD -> mock Android broker lifecycle. */
#include <assert.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "device_protocol.h"
int main(void) {
    struct mb_device_request r = {.physical_id = 7, .family = 3, .count = 2, .extension_count = 1,
        .priorities = {1.0f, 0.5f}};
    r.features.samplerAnisotropy = 1; snprintf(r.extensions[0], MB_NAME_BYTES, "HOST_WIRE_ONLY");
    uint8_t wire[MB_DEVICE_MAX_REQUEST]; uint32_t size = mb_encode_device_request(wire, &r);
    assert(size == MB_DEVICE_BASE_BYTES + 8 + 256 && mb_get_u32(wire) == 7);
    assert(mb_get_u32(wire + MB_DEVICE_BASE_BYTES) == 0x3f800000);
    struct mb_device_request decoded; assert(!mb_decode_device_request(wire, size, &decoded));
    assert(decoded.family == 3 && decoded.count == 2 && decoded.features.samplerAnisotropy == 1);
    assert(decoded.priorities[1] == 0.5f && !strcmp(decoded.extensions[0], "HOST_WIRE_ONLY"));
    assert(mb_decode_device_request(wire, size - 1, &decoded));
    mb_put_u32(wire + 16, 2); assert(mb_decode_device_request(wire, size, &decoded));
    size = mb_encode_device_request(wire, &r);
    mb_put_u32(wire + MB_DEVICE_BASE_BYTES, 0x7fc00000); assert(mb_decode_device_request(wire, size, &decoded));
    size = mb_encode_device_request(wire, &r); memset(wire + MB_DEVICE_BASE_BYTES + 8, 'x', 256);
    assert(mb_decode_device_request(wire, size, &decoded));
    r.count = MB_DEVICE_MAX_QUEUES + 1; assert(!mb_encode_device_request(wire, &r));

    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance; assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1; VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && count == 1);
    float priority = 1;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {0};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &q, .pEnabledFeatures = &features};
    VkDevice device = VK_NULL_HANDLE;
    q.queueFamilyIndex = 999; assert(vkCreateDevice(physical, &info, NULL, &device) != VK_SUCCESS && !device);
    q.queueFamilyIndex = 0;
    features.textureCompressionBC = 1;
    assert(vkCreateDevice(physical, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    features.textureCompressionBC = 0;
    const char *extension = "HOST_UNSUPPORTED"; info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &extension;
    assert(vkCreateDevice(physical, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);
    info.enabledExtensionCount = 0;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = 1};
    info.pNext = &timeline;
    assert(vkCreateDevice(physical, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    info.pNext = NULL;
    for (unsigned i = 0; i < 3; ++i) {
        /* Verify one genuinely supported core feature also survives the serialization. */
        features.samplerAnisotropy = i == 1;
        assert(vkCreateDevice(physical, &info, NULL, &device) == VK_SUCCESS && device);
        PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(device, "vkDestroyDevice");
        assert(get && destroy);
        assert(!vkGetDeviceProcAddr(device, "vkCreateImage") && !vkGetDeviceProcAddr(device, "vkQueueSubmit"));
        VkQueue one, two; get(device, 0, 0, &one); get(device, 0, 0, &two);
        assert(one && one == two);
        get(device, 999, 0, &two); assert(!two);
        get(device, 0, 99, &two); assert(!two);
        destroy(device, NULL);
    }
    vkDestroyInstance(instance, NULL);
    puts("device contracts: codec, truthful rejection, real loader dispatch, repeated queue/device lifecycle passed (HOST MOCK ONLY)");
    return 0;
}
