/* Private normal-renderer contract, not a Vulkan extension or wire opcode.
 * NULL command opts a whole coherent mapping into pool-managed uploads.
 * Before submitting each upload, arm its command after writing the safe slot.
 * The proxy acknowledges the existing bulk upload before that QueueSubmit and
 * does not re-upload this allocation for unrelated submits. Unmap ends opt-in. */
#ifndef DD_STAGING_API_H
#define DD_STAGING_API_H
#include <vulkan/vulkan.h>
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckStagingMALI)(VkDevice, VkDeviceMemory, VkCommandBuffer);
#endif
