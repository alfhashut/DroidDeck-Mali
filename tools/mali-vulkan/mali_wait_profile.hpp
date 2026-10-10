/* Included only by the renderer TU. Normal-only completion knowledge; never
 * infer completion from submission, elapsed time or Android ownership. */
#pragma once
#include "normal_api.h"
static thread_local bool maliWaitProfilingActive = false;
struct MaliCompletedTimeline {
    VkDevice device{}; VkSemaphore semaphore{}; uint64_t value = 0;
    VkDevice producerDevice{}; VkSemaphore producerSemaphore{}; uint64_t producerValue = 0;
    bool valid = false, producerSucceeded = false;
    void reset() { *this = {}; }
    bool covers(VkDevice d, VkSemaphore s, uint64_t v) const {
        return valid && device == d && semaphore == s && v <= value;
    }
    void observe(VkDevice d, VkSemaphore s, uint64_t v, VkResult result) {
        if (!d || !s || result != VK_SUCCESS) return;
        if (!valid || device != d || semaphore != s) { device = d; semaphore = s; value = v; valid = true; }
        else if (v > value) value = v;
    }
    bool producerCovers(VkDevice d, VkSemaphore s, uint64_t v) const {
        return producerSucceeded && producerDevice == d && producerSemaphore == s && producerValue == v && covers(d, s, v);
    }
};
static thread_local MaliCompletedTimeline maliCompletedTimeline;
struct MaliLocalWaitCounters {
    uint64_t requests[DD_WAIT_REASONS]{}, skipped[DD_WAIT_REASONS]{}, forwarded[DD_WAIT_REASONS]{};
};
static thread_local MaliLocalWaitCounters maliLocalWaits;
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
    const unsigned reason = maliWaitContext.reason;
    const bool single = info && info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO && !info->pNext && !info->flags && info->semaphoreCount == 1 && info->pSemaphores && info->pValues;
    if (reason < DD_WAIT_REASONS) ++maliLocalWaits.requests[reason];
    if (reason == DD_WAIT_OUTPUT_COMPLETION) maliCompletedTimeline.producerSucceeded = false;
    const bool reusable = reason == DD_WAIT_DESCRIPTOR_REUSE || reason == DD_WAIT_SHM_STAGING_REUSE;
    if (single && ((reusable && maliCompletedTimeline.covers(device, info->pSemaphores[0], info->pValues[0])) ||
        (reason == DD_WAIT_OUTPUT_RETIRE && maliCompletedTimeline.producerCovers(device, info->pSemaphores[0], info->pValues[0])))) {
        ++maliLocalWaits.skipped[reason]; return VK_SUCCESS;
    }
    if (reason < DD_WAIT_REASONS) ++maliLocalWaits.forwarded[reason];
    static auto profiled = reinterpret_cast<PFN_vkDroidDeckProfiledWaitMALI>(lookup(device, "vkDroidDeckProfiledWaitMALI"));
    // Mixed/older assets keep the real wait. The aggregate coverage reports
    // missing attribution instead of guessing a reason or claiming native data.
    VkResult result = profiled ? profiled(device, info, timeout, reason, maliWaitContext.resource) : original(device, info, timeout);
    if (single) {
        maliCompletedTimeline.observe(device, info->pSemaphores[0], info->pValues[0], result);
        if (reason == DD_WAIT_OUTPUT_COMPLETION && result == VK_SUCCESS) {
            maliCompletedTimeline.producerDevice = device; maliCompletedTimeline.producerSemaphore = info->pSemaphores[0];
            maliCompletedTimeline.producerValue = info->pValues[0]; maliCompletedTimeline.producerSucceeded = true;
        }
    }
    return result;
}
