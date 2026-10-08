/* Normal host Vulkan loader -> actual v6 ICD -> actual broker with explicit mock driver. */
#include <assert.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "interop_test_api.h"
int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1; VkPhysicalDevice p; assert(vkEnumeratePhysicalDevices(instance, &count, &p) == VK_SUCCESS && count == 1);
    float priority = 1;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures disabled = {0};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q, .pEnabledFeatures = &disabled};
    VkDevice d, other; assert(vkCreateDevice(p, &dci, NULL, &d) == VK_SUCCESS); assert(vkCreateDevice(p, &dci, NULL, &other) == VK_SUCCESS);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b = VK_NULL_HANDLE, bad = VK_NULL_HANDLE; assert(vkCreateBuffer(d, &bci, NULL, &b) == VK_SUCCESS);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d, b, &req); assert(req.size == 4096 && req.memoryTypeBits == 2);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = 1};
    VkDeviceMemory m, bad_m; assert(vkAllocateMemory(d, &ai, NULL, &m) == VK_SUCCESS);
    assert(vkBindBufferMemory(other, b, m, 0) != VK_SUCCESS);
    assert(vkBindBufferMemory(d, b, m, 1) != VK_SUCCESS);
    assert(vkBindBufferMemory(d, b, m, 0) == VK_SUCCESS);
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ai.pNext = &dedicated; assert(vkAllocateMemory(d, &ai, NULL, &bad_m) == VK_ERROR_FEATURE_NOT_PRESENT); ai.pNext = NULL;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    bci.pNext = &external; assert(vkCreateBuffer(d, &bci, NULL, &bad) == VK_ERROR_FEATURE_NOT_PRESENT); bci.pNext = NULL;
    void *ptr = NULL;
    assert(vkMapMemory(d, m, 0, 4097, 0, &ptr) != VK_SUCCESS && !ptr);
    assert(vkMapMemory(other, m, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS && !ptr);
    assert(vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 1, &ptr) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS && ((uintptr_t)ptr % 4096) == 0);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .pNext = &dedicated, .memory = m, .size = VK_WHOLE_SIZE};
    assert(vkFlushMappedMemoryRanges(d, 1, &range) == VK_ERROR_FEATURE_NOT_PRESENT);
    range.pNext = NULL; range.offset = 1; range.size = 256; assert(vkFlushMappedMemoryRanges(d, 1, &range) != VK_SUCCESS);
    vkUnmapMemory(d, m);
    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage im; imci.pNext = &external; assert(vkCreateImage(d, &imci, NULL, &im) == VK_ERROR_FEATURE_NOT_PRESENT); imci.pNext = NULL;
    imci.format = VK_FORMAT_B8G8R8A8_UNORM; assert(vkCreateImage(d, &imci, NULL, &im) == VK_ERROR_FEATURE_NOT_PRESENT);
    PFN_vkDroidDeckInteropTEST api = (PFN_vkDroidDeckInteropTEST)vkGetDeviceProcAddr(d, "vkDroidDeckInteropTEST"); assert(api);
    struct dd_interop_input in = {.token = 999, .sync = 999}; struct dd_interop_output out;
    assert(api(d, DD_AHB_PRESENT, &in, &out) != VK_SUCCESS);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool;
    assert(vkCreateCommandPool(d, &pci, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}; VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(d, &cai, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    vkCmdFillBuffer(c, b, 1, 4, 0x12345678); assert(vkEndCommandBuffer(c) != VK_SUCCESS); /* void rejection cannot turn into PASS */
    vkFreeCommandBuffers(d, pool, 1, &c); vkDestroyCommandPool(d, pool, NULL);
    vkDestroyBuffer(d, b, NULL); vkFreeMemory(d, m, NULL); vkDestroyDevice(other, NULL); vkDestroyDevice(d, NULL); vkDestroyInstance(instance, NULL);
    puts("v6 loader contract: pNext/format/map flags, wrong parent, alignment, invalid tokens and rejected recording PASS");
    return 0;
}
