#ifndef DD_NORMAL_API_H
#define DD_NORMAL_API_H
#include <vulkan/vulkan.h>
#include "normal_protocol.h"
struct dd_normal_input { uint32_t token, sync, frame, verbose; };
struct dd_normal_output { uint32_t key, counts[20], presented, released, owned, timeouts, fds_created, fds_closed; };
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckWaylandMALI)(VkDevice, uint32_t, const struct dd_normal_input *, struct dd_normal_output *);
#endif
