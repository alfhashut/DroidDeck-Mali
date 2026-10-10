/* Included only by the renderer TU. Scopes annotate existing waits; they do
 * not acquire resources, alter values, wait, reset, or issue RPCs themselves. */
#pragma once
#include "normal_api.h"
static bool maliWaitProfilingActive = false;
struct MaliWaitContext { uint32_t reason = DD_WAIT_UNATTRIBUTED; uint64_t resource = 0; };
static thread_local MaliWaitContext maliWaitContext;
struct MaliWaitScope {
    MaliWaitContext previous;
    MaliWaitScope(uint32_t reason, uint64_t resource) : previous(maliWaitContext) { maliWaitContext = {reason, resource}; }
    ~MaliWaitScope() { maliWaitContext = previous; }
};
static VkResult MaliProfiledWait(PFN_vkWaitSemaphores original, PFN_vkGetDeviceProcAddr lookup,
        VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    if (!maliWaitProfilingActive) return original(device, info, timeout);
    static auto profiled = reinterpret_cast<PFN_vkDroidDeckProfiledWaitMALI>(lookup(device, "vkDroidDeckProfiledWaitMALI"));
    // Mixed/older assets keep the real wait. The aggregate coverage reports
    // missing attribution instead of guessing a reason or claiming native data.
    if (!profiled) return original(device, info, timeout);
    return profiled(device, info, timeout, maliWaitContext.reason, maliWaitContext.resource);
}
