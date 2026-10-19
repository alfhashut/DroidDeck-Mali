/* Direct driver-interface contract checks; loader_test.c separately uses the real loader. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <vulkan/vk_icd.h>
#include "properties.h"

static void codec_test(void) {
    VkPhysicalDeviceProperties a = {0}, b;
    strcpy(a.deviceName, "codec");
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) a.pipelineCacheUUID[i] = (uint8_t)i;
#define MB_U32(f) a.f = 12345;
#define MB_I32(f) a.f = -123;
#define MB_F32(f) a.f = 2.5f;
#define MB_U64(f) a.f = UINT64_C(0x123456789abcdef0);
#define MB_SIZE(f) a.f = 4096;
#include "properties_fields.def"
#undef MB_U32
#undef MB_I32
#undef MB_F32
#undef MB_U64
#undef MB_SIZE
    uint8_t wire[MB_PROPERTIES_BYTES];
    mb_encode_properties(wire, &a);
    assert(!mb_decode_properties(wire, &b));
    assert(!memcmp(&a, &b, sizeof(a)));
    /* Golden endian check for the first five scalar fields after name/UUID. */
    assert(wire[MB_NAME_BYTES + VK_UUID_SIZE] == 0x39);
    assert(wire[MB_NAME_BYTES + VK_UUID_SIZE + 1] == 0x30);
    memset(wire, 'X', MB_NAME_BYTES);
    assert(mb_decode_properties(wire, &b) == -1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    codec_test();
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(library);
    PFN_vkNegotiateLoaderICDInterfaceVersion negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)
        dlsym(library, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
    PFN_GetPhysicalDeviceProcAddr gpdpa = (PFN_GetPhysicalDeviceProcAddr)
        dlsym(library, "vk_icdGetPhysicalDeviceProcAddr");
    assert(negotiate && gipa && gpdpa);
    uint32_t version = 7;
    assert(negotiate(&version) == VK_SUCCESS && version == 5);
    for (version = 2; version <= 5; ++version) {
        uint32_t v = version; assert(negotiate(&v) == VK_SUCCESS && v == version);
    }
    version = 1; assert(negotiate(&version) == VK_ERROR_INCOMPATIBLE_DRIVER);
    assert(negotiate(NULL) == VK_ERROR_INCOMPATIBLE_DRIVER);
    assert(!gipa(NULL, "vkQueueSubmit") && !gipa(NULL, NULL));
    assert(!gipa(NULL, "vkDestroyInstance") && !gpdpa(NULL, "vkGetPhysicalDeviceProperties"));
#define PROC(type, n) PFN_vk##type n = (PFN_vk##type)gipa(NULL, "vk" #type); assert(n)
    PROC(CreateInstance, create);
    PROC(EnumerateInstanceVersion, instance_version);
    PROC(EnumerateInstanceExtensionProperties, extensions);
#undef PROC
    assert(instance_version(&version) == VK_SUCCESS && version == VK_API_VERSION_1_0);
    uint32_t count = 100; assert(extensions(NULL, &count, NULL) == VK_SUCCESS && !count);
    assert(extensions("unsupported", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance = (VkInstance)(uintptr_t)1;
    assert(create(&info, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER && !instance);
    app.apiVersion = VK_API_VERSION_1_0;
    const char *extension = "VK_KHR_wayland_surface";
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &extension;
    assert(create(&info, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT && !instance);
    info.enabledExtensionCount = 0;
    VkAllocationCallbacks allocator = {0};
    assert(create(&info, &allocator, &instance) == VK_ERROR_INITIALIZATION_FAILED && !instance);
    assert(create(&info, NULL, &instance) == VK_SUCCESS);
    assert(valid_loader_magic_value(instance));
    VkInstance second;
    assert(create(&info, NULL, &second) == VK_SUCCESS && second != instance);
    assert(!gipa(instance, "vkGetPhysicalDeviceProperties2") && !gipa(instance, "vkCreateWaylandSurfaceKHR"));
    assert(!gipa(instance, "vkQueueSubmit") && !gipa(instance, "vkAllocateMemory"));
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)gpdpa(instance, "vkGetPhysicalDeviceProperties");
    assert(destroy && enumerate && properties);
    assert(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 1);
    VkPhysicalDevice device, repeated;
    count = 0; assert(enumerate(instance, &count, &device) == VK_INCOMPLETE && !count);
    count = 1; assert(enumerate(instance, &count, &device) == VK_SUCCESS && count == 1);
    assert(valid_loader_magic_value(device));
    count = 1; assert(enumerate(instance, &count, &repeated) == VK_SUCCESS && device == repeated);
    count = 1; assert(enumerate(second, &count, &repeated) == VK_SUCCESS && device != repeated);
    VkPhysicalDeviceProperties p;
    properties(device, &p);
    assert(!strcmp(p.deviceName, "HOST TEST ONLY") && p.vendorID == 0x13b5 && p.deviceID == 0x74021000);
    assert(p.apiVersion == VK_MAKE_API_VERSION(0, 1, 1, 131) && p.driverVersion == 109051904);
    assert(p.limits.maxImageDimension2D == 8192 && p.limits.bufferImageGranularity == UINT64_C(0x123456789));
    assert(p.limits.minMemoryMapAlignment == 4096 && p.limits.minTexelOffset == -8 && p.limits.timestampPeriod == 2.5f);
    assert(p.sparseProperties.residencyStandard2DBlockShape == VK_TRUE);
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) assert(p.pipelineCacheUUID[i] == i);
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    VkDevice logical = (VkDevice)(uintptr_t)1;
    assert(create_device(device, NULL, NULL, &logical) == VK_ERROR_INITIALIZATION_FAILED && !logical);
    count = 100;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))(device, &count, NULL);
    assert(!count);
    VkPhysicalDeviceFeatures features, empty_features = {0};
    ((PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures"))(device, &features);
    assert(!memcmp(&features, &empty_features, sizeof(features)));
    VkPhysicalDeviceMemoryProperties memory, empty_memory = {0};
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(device, &memory);
    assert(!memcmp(&memory, &empty_memory, sizeof(memory)));
    VkFormatProperties format, empty_format = {0};
    ((PFN_vkGetPhysicalDeviceFormatProperties)gipa(instance, "vkGetPhysicalDeviceFormatProperties"))(device, VK_FORMAT_R8G8B8A8_UNORM, &format);
    assert(!memcmp(&format, &empty_format, sizeof(format)));
    VkImageFormatProperties image;
    assert(((PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties"))(
        device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, 0, 0, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    count = 100;
    assert(((PFN_vkEnumerateDeviceExtensionProperties)gipa(instance, "vkEnumerateDeviceExtensionProperties"))(
        device, NULL, &count, NULL) == VK_SUCCESS && !count);
    count = 100;
    ((PFN_vkGetPhysicalDeviceSparseImageFormatProperties)gipa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties"))(
        device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 0, VK_IMAGE_TILING_OPTIMAL, &count, NULL);
    assert(!count);
    assert(!((PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr"))(NULL, "vkQueueSubmit"));
    destroy(second, NULL); destroy(instance, NULL); destroy(NULL, NULL);
    dlclose(library);
    puts("ICD contracts: negotiation, dispatch magic, unsupported operations, stable objects, full property codec, cleanup passed");
    return 0;
}
