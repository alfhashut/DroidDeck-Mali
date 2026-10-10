/* Minimal types for the standalone normal-renderer helper tests. */
#pragma once
#include <cstdint>
#define VKAPI_PTR
#define VK_NULL_HANDLE 0
#define VK_WHOLE_SIZE UINT64_MAX
#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_ERROR_INITIALIZATION_FAILED -3
#define VK_ERROR_DEVICE_LOST -4
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define VK_ERROR_FEATURE_NOT_PRESENT -8
#define VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO 1000207004
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT 1
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 2
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 4
using VkDevice = void *;
using VkResult = int;
using VkBool32 = uint32_t;
using VkSemaphore = uint64_t;
using VkBuffer = uint64_t;
using VkDeviceMemory = uint64_t;
using VkCommandBuffer = void *;
struct VkSemaphoreWaitInfo {
    unsigned sType; const void *pNext; unsigned flags, semaphoreCount;
    const VkSemaphore *pSemaphores; const uint64_t *pValues;
};
using VkSemaphoreWaitInfoKHR = VkSemaphoreWaitInfo;
using PFN_vkVoidFunction = void (*)();
using PFN_vkWaitSemaphores = VkResult (*)(VkDevice, const VkSemaphoreWaitInfo *, uint64_t);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction (*)(VkDevice, const char *);
struct VkBufferCreateInfo { unsigned sType; uint64_t size; unsigned usage; };
struct VkMemoryRequirements { uint64_t size, alignment; unsigned memoryTypeBits; };
struct VkMemoryAllocateInfo { unsigned sType; uint64_t allocationSize; unsigned memoryTypeIndex; };
