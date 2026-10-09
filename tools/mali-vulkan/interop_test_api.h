/* Private diagnostic ABI, not an advertised Vulkan extension. No native handles.
 * VkImage/Memory/Fence below are ordinary LOCAL ICD proxy handles. */
#ifndef DROIDDECK_INTEROP_TEST_API_H
#define DROIDDECK_INTEROP_TEST_API_H
enum { DD_AHB_CREATE = 1, DD_FENCE_CREATE, DD_SYNC_EXPORT, DD_SYNC_WAIT,
       DD_AHB_INSPECT, DD_AHB_PRESENT, DD_AHB_RELEASE, DD_SYNC_CLOSE };
struct dd_interop_input {
    VkFence fence;
    uint32_t token, sync, width, height, pattern;
};
struct dd_interop_output {
    VkImage image;
    VkDeviceMemory memory;
    VkFence fence;
    uint32_t token, sync, width, height, format, layers, cpu_checked;
    uint64_t usage;
};
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckInteropTEST)(VkDevice, uint32_t,
    const struct dd_interop_input *, struct dd_interop_output *);
/* Separate ABI: old checkpoint callers keep their original output size. */
enum { DD_SESSION_BEGIN = 1, DD_SESSION_PRESENT, DD_SESSION_END, DD_SESSION_STATS };
struct dd_session_output {
    uint32_t token, counts[20], presented, released, fds_created, fds_closed, max_owned;
};
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckSessionTEST)(VkDevice, uint32_t,
    const struct dd_interop_input *, struct dd_session_output *);
#endif
