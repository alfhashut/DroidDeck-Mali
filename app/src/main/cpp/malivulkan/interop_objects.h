struct native_interop {
#define MB_INTEROP_ENTRY(n) PFN_vk##n n;
#include "interop_entries.def"
#undef MB_INTEROP_ENTRY
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahb_properties;
    PFN_vkGetFenceFdKHR fence_fd;
    VkPhysicalDeviceMemoryProperties properties;
    VkDeviceSize atom;
    int enabled, ahb_enabled;
    struct native_buffer { uint32_t id, memory, usage; VkDeviceSize size, offset; VkMemoryRequirements req; VkBuffer handle; } buffers[MB_SUBMIT_MAX_OBJECTS];
    struct native_memory { uint32_t id, type, bound, ahb; VkDeviceSize size, map_offset, map_size; void *map; VkDeviceMemory handle; } memories[MB_SUBMIT_MAX_OBJECTS];
    struct native_image { uint32_t id, memory, width, height, usage, ahb, foreign; VkDeviceSize offset; VkImageLayout layout; VkMemoryRequirements req; VkImage handle; } images[MB_SUBMIT_MAX_OBJECTS];
    struct native_ahb { uint32_t id, image, memory; AHardwareBuffer *handle; AHardwareBuffer_Desc desc; } ahbs[MB_SUBMIT_MAX_OBJECTS];
    struct native_sync { uint32_t id, fence; int fd, completed; } syncs[MB_SUBMIT_MAX_OBJECTS];
};
