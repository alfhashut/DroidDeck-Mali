#ifndef DD_NORMAL_API_H
#define DD_NORMAL_API_H
#include <vulkan/vulkan.h>
#include "normal_protocol.h"
#include "normal_perf.h"
struct dd_normal_input { uint32_t token, sync, frame, verbose; };
struct dd_normal_output { uint32_t key, counts[20], presented, released, owned, timeouts, fds_created, fds_closed; };
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckWaylandMALI)(VkDevice, uint32_t, const struct dd_normal_input *, struct dd_normal_output *);
/* Snapshot/reset local counters under the existing connection lock; no RPC.
 * Performance2 returns the full struct including wait/resource attribution.
 * The original Performance entrypoint writes only the original prefix, ending
 * before wait[], so older callers retain their original buffer size.
 * These private entrypoints are not advertised as Vulkan extensions. */
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckPerformanceMALI)(VkDevice, struct dd_perf_rpc *, VkBool32);
/* Normal-only wait attribution; invokes the same real wait, with optional v7
 * measurement metadata. Resource is a buffer handle for SHM or a slot index. */
typedef VkResult (VKAPI_PTR *PFN_vkDroidDeckProfiledWaitMALI)(VkDevice, const VkSemaphoreWaitInfoKHR *, uint64_t, uint32_t, uint64_t);
#endif
