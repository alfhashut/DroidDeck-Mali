/* Private normal-renderer contract, not a Vulkan extension or wire opcode.
 * MapStaging maps a whole fresh coherent allocation without downloading bytes;
 * the caller must overwrite the complete SHM image before arming its upload.
 * (memory=NULL, out=NULL) probes wire-v7/normal-mode availability without RPCs.
 * Staging with NULL command can also register an existing whole mapping.
 * Before submitting each upload, arm its command after writing the safe slot.
 * The proxy acknowledges the existing bulk upload before that QueueSubmit and
 * does not re-upload this allocation for unrelated submits. Unmap ends opt-in. */
#ifndef DD_STAGING_API_H
#define DD_STAGING_API_H
#include <vulkan/vulkan.h>
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckStagingMALI)(VkDevice, VkDeviceMemory, VkCommandBuffer);
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckMapStagingMALI)(VkDevice, VkDeviceMemory, void **);
#endif
