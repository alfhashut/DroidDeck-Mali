/* Host-only normal Vulkan loader checks for the explicit v7 structure contract. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
int main(void) {
    assert(!setenv("MALI_VULKAN_RENDERER_TEST", "1", 1));
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    const char *query = "VK_KHR_get_physical_device_properties2";
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &query};
    VkInstance instance; assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1; VkPhysicalDevice physical; assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && count == 1);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props); assert(props.apiVersion == VK_MAKE_VERSION(1, 1, 131));
    VkPhysicalDeviceScalarBlockLayoutFeaturesEXT scalar = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT, .scalarBlockLayout = 1};
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR, .pNext = &scalar, .timelineSemaphore = 1};
    const char *ext[] = {"VK_KHR_timeline_semaphore", "VK_EXT_scalar_block_layout", "VK_KHR_image_format_list"};
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &timeline, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = ext};
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceVulkan12Features fake12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .timelineSemaphore = 1, .scalarBlockLayout = 1};
    ci.pNext = &fake12; assert(vkCreateDevice(physical, &ci, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    VkPhysicalDeviceRobustness2FeaturesEXT robustness = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .nullDescriptor = 1};
    scalar.pNext = &robustness; ci.pNext = &timeline;
    assert(vkCreateDevice(physical, &ci, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    scalar.pNext = NULL;
    assert(vkCreateDevice(physical, &ci, NULL, &device) == VK_SUCCESS);
#define FN(n) PFN_vk##n n = (PFN_vk##n)vkGetDeviceProcAddr(device, "vk" #n); assert(n)
    FN(CreateSemaphore); FN(DestroySemaphore); FN(GetSemaphoreCounterValueKHR); FN(WaitSemaphoresKHR); FN(CreateShaderModule);
    assert(vkGetDeviceProcAddr(device, "vkGetSemaphoreCounterValue") == (PFN_vkVoidFunction)GetSemaphoreCounterValueKHR);
    assert(vkGetDeviceProcAddr(device, "vkWaitSemaphores") == (PFN_vkVoidFunction)WaitSemaphoresKHR);
    VkSemaphoreTypeCreateInfoKHR type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR, .initialValue = 9};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type}; VkSemaphore sem;
    type.pNext = &robustness; assert(CreateSemaphore(device, &sci, NULL, &sem) == VK_ERROR_FEATURE_NOT_PRESENT && !sem);
    type.pNext = NULL; assert(CreateSemaphore(device, &sci, NULL, &sem) == VK_SUCCESS);
    uint64_t value = 0; assert(GetSemaphoreCounterValueKHR(device, sem, &value) == VK_SUCCESS && value == 9);
    VkSemaphoreWaitInfoKHR wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &value};
    assert(WaitSemaphoresKHR(device, &wait, 0) == VK_SUCCESS);
    value = 10; assert(WaitSemaphoresKHR(device, &wait, 0) == VK_TIMEOUT);
    wait.pNext = &robustness; assert(WaitSemaphoresKHR(device, &wait, 0) == VK_ERROR_FEATURE_NOT_PRESENT);
    uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0};
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pNext = &robustness, .codeSize = sizeof(code), .pCode = code}; VkShaderModule module;
    assert(CreateShaderModule(device, &shader, NULL, &module) == VK_ERROR_FEATURE_NOT_PRESENT && !module);
    shader.pNext = NULL; shader.codeSize = 19;
    assert(CreateShaderModule(device, &shader, NULL, &module) == VK_ERROR_FEATURE_NOT_PRESENT && !module);
    DestroySemaphore(device, sem, NULL); vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    puts("renderer contracts: API 1.1.131, real KHR aliases, unknown pNext/Vulkan12/robustness rejected, timeline timeout preserved");
    return 0;
}
