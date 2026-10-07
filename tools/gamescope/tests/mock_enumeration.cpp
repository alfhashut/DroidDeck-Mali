// Host-only Vulkan functions for the real Gamescope diagnostic translation unit.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vulkan/vulkan.h>

static int creates, destroys, lists, queries;
static const char *mode() { const char *m = std::getenv("ENUM_TEST_MODE"); return m ? m : "success"; }
static bool is(const char *value) { return std::strcmp(mode(), value) == 0; }
static struct CleanupCheck {
    ~CleanupCheck() {
        assert(destroys == (is("create-error") ? 0 : 1));
        std::printf("MOCK cleanup creates=%d destroys=%d lists=%d properties=%d\n", creates, destroys, lists, queries);
    }
} cleanup;

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *out) {
    assert(info->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO && !info->pNext && !info->flags);
    assert(!info->enabledExtensionCount && !info->enabledLayerCount && !allocator);
    assert(info->pApplicationInfo && info->pApplicationInfo->apiVersion == VK_API_VERSION_1_0);
    ++creates;
    if (is("create-error")) return VK_ERROR_INCOMPATIBLE_DRIVER;
    *out = reinterpret_cast<VkInstance>(uintptr_t(1)); return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    assert(instance == reinterpret_cast<VkInstance>(uintptr_t(1)) && !allocator); ++destroys;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance,
        uint32_t *count, VkPhysicalDevice *devices) {
    assert(instance == reinterpret_cast<VkInstance>(uintptr_t(1)));
    if (!devices) {
        if (is("count-error")) return VK_ERROR_INITIALIZATION_FAILED;
        *count = is("zero") ? 0 : 2; return VK_SUCCESS;
    }
    ++lists;
    assert(*count == 2);
    if (is("list-error")) return VK_ERROR_INITIALIZATION_FAILED;
    if (is("always-incomplete") || (is("retry") && lists == 1)) { *count = 1; return VK_INCOMPLETE; }
    for (unsigned i = 0; i < 2; ++i) devices[i] = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x42 + i));
    *count = 2; return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice device,
        VkPhysicalDeviceProperties *p) {
    unsigned i = unsigned(reinterpret_cast<uintptr_t>(device) - 0x42);
    assert(i < 2); ++queries;
    *p = {};
    std::snprintf(p->deviceName, sizeof(p->deviceName), "HOST MOCK GPU %u", i);
    p->vendorID = 0x13b5; p->deviceID = 0x74021000 + i;
    p->apiVersion = VK_MAKE_VERSION(1, 1, 131); p->driverVersion = 109051904;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}
