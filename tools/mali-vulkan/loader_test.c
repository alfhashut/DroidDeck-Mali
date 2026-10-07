/* A normal glibc Vulkan application: opens the loader, never the proxy ICD. */
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#ifndef __GLIBC__
#error This test must be built against glibc
#endif

int main(void) {
    int code = 1;
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) { fprintf(stderr, "load libvulkan.so.1: %s\n", dlerror()); return 1; }
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)dlsym(loader, "vkDestroyInstance");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    if (!create || !destroy) { fprintf(stderr, "loader missing instance entry points\n"); goto done; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "DroidDeck query-only ICD test", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkResult result = create(&info, NULL, &instance);
    printf("glibc libvulkan.so.1: vkCreateInstance=%d\n", (int)result);
    if (result != VK_SUCCESS) { instance = VK_NULL_HANDLE; goto done; }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    if (!enumerate || !properties) { fprintf(stderr, "loader missing query entry points\n"); goto done; }
    VkPhysicalDevice devices[16];
    uint32_t count = 0;
    result = enumerate(instance, &count, NULL);
    if (result != VK_SUCCESS || !count || count > 16) {
        fprintf(stderr, "device count query: result=%d count=%u\n", (int)result, count); goto done;
    }
    result = enumerate(instance, &count, devices);
    if (result != VK_SUCCESS || !count || count > 16) {
        fprintf(stderr, "device list query: result=%d count=%u\n", (int)result, count); goto done;
    }
    printf("Vulkan physical-device count: %u\n", count);
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        properties(devices[i], &p);
        printf("Device %u\ndeviceName: %s\nvendorID: 0x%08x\ndeviceID: 0x%08x\n"
               "apiVersion: %u.%u.%u\ndriverVersion: %u\ndeviceType: %u\n", i, p.deviceName,
               p.vendorID, p.deviceID, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
               VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, (unsigned)p.deviceType);
    }
    code = 0;
done:
    if (instance) { destroy(instance, NULL); puts("Destroyed Vulkan instance"); }
    dlclose(loader);
    return code;
}
