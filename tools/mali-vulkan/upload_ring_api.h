/* Client-local private opt-in for Gamescope's exclusively instrumented ring.
 * RESERVE precedes CPU access, MODIFIED follows the complete store, SEAL covers
 * all stores before one command submission. No new wire opcode or broker ABI. */
#ifndef DD_UPLOAD_RING_API_H
#define DD_UPLOAD_RING_API_H
#include <vulkan/vulkan.h>
#include "upload_ring.h"
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckUploadRingMALI)(VkDevice, VkDeviceMemory,
    VkBuffer, VkCommandBuffer, uint32_t operation, VkDeviceSize offset, VkDeviceSize length);
#endif
