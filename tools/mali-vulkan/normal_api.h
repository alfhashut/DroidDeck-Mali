#ifndef DD_NORMAL_API_H
#define DD_NORMAL_API_H
#include <vulkan/vulkan.h>
#include "normal_protocol.h"
#include "normal_perf.h"
struct dd_normal_input { uint32_t token, sync, frame, verbose; };
struct dd_normal_output { uint32_t key, counts[20], presented, released, owned, timeouts, fds_created, fds_closed; };
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckWaylandMALI)(VkDevice, uint32_t, const struct dd_normal_input *, struct dd_normal_output *);
/* Snapshot/reset local counters under the existing connection lock; no RPC.
 * This private entrypoint is not advertised as a Vulkan extension. */
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckPerformanceMALI)(VkDevice, struct dd_perf_rpc *, VkBool32);
#endif
