/* Android-only handles; resource IDs are unique across all child types. */
struct native_submit {
#define MB_SUBMIT_ENTRY(n) PFN_vk##n n;
#include "submit_entries.def"
#undef MB_SUBMIT_ENTRY
    PFN_vkDeviceWaitIdle idle;
    int lost;
    struct native_pool { uint32_t id, family; VkCommandPool handle; } pools[MB_SUBMIT_MAX_OBJECTS];
    struct native_command {
        uint32_t id, pool, family, event, fence;
        /* 0 initial, 1 recording, 2 executable, 3 pending, 4 completed, 5 invalid. */
        unsigned state;
        VkCommandBuffer handle;
    } commands[MB_SUBMIT_MAX_OBJECTS];
    struct native_event { uint32_t id; VkEvent handle; } events[MB_SUBMIT_MAX_OBJECTS];
    struct native_fence { uint32_t id; int submitted; VkFence handle; } fences[MB_SUBMIT_MAX_OBJECTS];
};
