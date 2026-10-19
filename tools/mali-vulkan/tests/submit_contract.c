/* Real host loader -> proxy -> Android broker source with HOST MOCK backend. */
#include <assert.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
int main(void) {
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance; assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    VkPhysicalDevice physical; uint32_t count = 1;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && count == 1);
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device; assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
#define MB_SUBMIT_ENTRY(n) assert(vkGetDeviceProcAddr(device, "vk" #n) && vkGetInstanceProcAddr(instance, "vk" #n));
#include "submit_entries.def"
#undef MB_SUBMIT_ENTRY
    assert(!vkGetDeviceProcAddr(device, "vkCreateBuffer") && !vkGetDeviceProcAddr(device, "vkCmdSetEvent2"));
    VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue); assert(queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool;
    pci.queueFamilyIndex = 1; assert(vkCreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS && !pool);
    pci.queueFamilyIndex = 0; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_ERROR_FEATURE_NOT_PRESENT && !pool);
    pci.flags = 0; pci.pNext = &ici;
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_ERROR_FEATURE_NOT_PRESENT && !pool);
    pci.pNext = NULL; assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    assert(vkAllocateCommandBuffers(device, &ai, &command) == VK_ERROR_FEATURE_NOT_PRESENT && !command);
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    assert(vkAllocateCommandBuffers(device, &ai, &command) == VK_SUCCESS && command);
    VkEventCreateInfo eci = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VkEvent event;
    eci.flags = VK_EVENT_CREATE_DEVICE_ONLY_BIT;
    assert(vkCreateEvent(device, &eci, NULL, &event) == VK_ERROR_FEATURE_NOT_PRESENT && !event);
    eci.flags = 0; assert(vkCreateEvent(device, &eci, NULL, &event) == VK_SUCCESS);
    assert(vkGetEventStatus(device, event) == VK_EVENT_RESET);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    assert(vkCreateFence(device, &fci, NULL, &fence) == VK_ERROR_FEATURE_NOT_PRESENT && !fence);
    fci.flags = 0; assert(vkCreateFence(device, &fci, NULL, &fence) == VK_SUCCESS);
    assert(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS); /* unended */
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    bi.pInheritanceInfo = &inheritance;
    assert(vkBeginCommandBuffer(command, &bi) == VK_ERROR_FEATURE_NOT_PRESENT);
    bi.pInheritanceInfo = NULL; bi.pNext = &ici;
    assert(vkBeginCommandBuffer(command, &bi) == VK_ERROR_FEATURE_NOT_PRESENT);
    bi.pNext = NULL; assert(vkBeginCommandBuffer(command, &bi) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(command, &bi) != VK_SUCCESS); /* no re-record */
    vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(vkGetEventStatus(device, event) == VK_EVENT_RESET); /* recording is not execution */
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    si.pNext = &ici; assert(vkQueueSubmit(queue, 1, &si, fence) == VK_ERROR_FEATURE_NOT_PRESENT);
    si.pNext = NULL; si.waitSemaphoreCount = 1;
    assert(vkQueueSubmit(queue, 1, &si, fence) == VK_ERROR_FEATURE_NOT_PRESENT);
    si.waitSemaphoreCount = 0; si.signalSemaphoreCount = 1;
    assert(vkQueueSubmit(queue, 1, &si, fence) == VK_ERROR_FEATURE_NOT_PRESENT);
    si.signalSemaphoreCount = 0;
    assert(vkQueueSubmit(queue, 0, &si, fence) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    assert(vkGetFenceStatus(device, fence) == VK_SUCCESS);
    assert(vkGetEventStatus(device, event) == VK_EVENT_SET);
    vkDestroyFence(device, fence, NULL); vkDestroyEvent(device, event, NULL);
    vkFreeCommandBuffers(device, pool, 1, &command); vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    puts("submit contracts: real loader dispatch, strict structures, record RESET -> submit -> fence -> SET, cleanup passed (HOST MOCK ONLY)");
    return 0;
}
