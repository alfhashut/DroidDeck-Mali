#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "mali_wait_profile.hpp"
#include "mali_staging_pool.hpp"
static unsigned waits, resets, created, destroyed, allocated, freed, bound;
static int waitResult, createFailure;
static bool missingProfile;
static uint64_t nextId, expectedValue, expectedSemaphore = 10;
static std::map<uint64_t, uint64_t> sizes, bindings;
static std::map<uint64_t, std::vector<uint8_t>> memory;
static VkResult realWait(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    assert(device && info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    assert(info->semaphoreCount == 1 && info->pSemaphores[0] == expectedSemaphore && info->pValues[0] == expectedValue);
    assert(timeout == 5000000000ull); ++waits; return waitResult;
}
static VkResult profiledWait(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t timeout, uint32_t, uint64_t) {
    return realWait(d, i, timeout);
}
static PFN_vkVoidFunction lookup(VkDevice, const char *name) {
    assert(!std::strcmp(name, "vkDroidDeckProfiledWaitMALI"));
    return missingProfile ? nullptr : reinterpret_cast<PFN_vkVoidFunction>(profiledWait);
}
static VkResult createBuffer(VkDevice, const VkBufferCreateInfo *info, void *, VkBuffer *out) {
    assert(info->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO && info->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (createFailure == 1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = ++nextId; sizes[*out] = info->size; ++created; return 0;
}
static void requirements(VkDevice, VkBuffer b, VkMemoryRequirements *out) { *out = {sizes.at(b), 4, 1}; }
static VkResult allocate(VkDevice, const VkMemoryAllocateInfo *info, void *, VkDeviceMemory *out) {
    assert(info->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO && info->memoryTypeIndex == 0);
    if (createFailure == 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = ++nextId; memory[*out].resize(info->allocationSize); ++allocated; return 0;
}
static VkResult bindBuffer(VkDevice, VkBuffer b, VkDeviceMemory m, uint64_t offset) {
    assert(!offset && memory.at(m).size() >= sizes.at(b));
    if (createFailure == 4) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bindings[b] = m; ++bound; return 0;
}
static void destroyBuffer(VkDevice, VkBuffer b, void *) { assert(sizes.erase(b)); bindings.erase(b); ++destroyed; }
static void freeMemory(VkDevice, VkDeviceMemory m, void *) { assert(memory.erase(m)); ++freed; }
static struct TestDevice {
    VkDevice device() const { return (void *)1; }
    VkSemaphore maliScratchTimeline() const { return 10; }
    unsigned findMemoryType(unsigned flags, unsigned bits) {
        assert(flags == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && bits == 1);
        return createFailure == 2 ? ~0u : 0;
    }
    void resetCmdBuffers(uint64_t value) { assert(maliCompletedTimeline.covers(device(), 10, value)); ++resets; }
    struct {
        decltype(&realWait) WaitSemaphores = realWait;
        decltype(&lookup) GetDeviceProcAddr = lookup;
        decltype(&createBuffer) CreateBuffer = createBuffer;
        decltype(&requirements) GetBufferMemoryRequirements = requirements;
        decltype(&allocate) AllocateMemory = allocate;
        decltype(&bindBuffer) BindBufferMemory = bindBuffer;
        decltype(&destroyBuffer) DestroyBuffer = destroyBuffer;
        decltype(&freeMemory) FreeMemory = freeMemory;
    } vk;
} g_device;
#include "mali_staging.inc"
static int call(unsigned reason, uint64_t value, VkSemaphore semaphore = 10, VkDevice device = (void *)1) {
    expectedValue = value; expectedSemaphore = semaphore;
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &value};
    MaliWaitScope scope(reason, 0);
    return MaliProfiledWait(realWait, lookup, device, &info, 5000000000ull);
}
static void watermark() {
    maliWaitProfilingActive = true;
    // Only successful synchronization/query observations advance knowledge.
    waitResult = VK_TIMEOUT;
    assert(call(DD_WAIT_DESCRIPTOR_REUSE, 5) == VK_TIMEOUT && !maliCompletedTimeline.valid);
    waitResult = VK_ERROR_DEVICE_LOST;
    assert(call(DD_WAIT_DESCRIPTOR_REUSE, 5) == VK_ERROR_DEVICE_LOST && !maliCompletedTimeline.valid);
    waitResult = 0; assert(call(DD_WAIT_OUTPUT_COMPLETION, 10) == 0);
    unsigned before = waits;
    assert(call(DD_WAIT_DESCRIPTOR_REUSE, 9) == 0 && waits == before);
    assert(call(DD_WAIT_OUTPUT_RETIRE, 10) == 0 && waits == before);
    // Known completion alone is insufficient for retirement without that proof.
    maliCompletedTimeline.producerSucceeded = false;
    assert(call(DD_WAIT_OUTPUT_RETIRE, 10) == 0 && waits == before + 1);
    maliCompletedTimeline.observe((void *)1, 10, 30, VK_SUCCESS);
    maliCompletedTimeline.observe((void *)1, 10, 4, VK_SUCCESS);
    maliCompletedTimeline.observe((void *)1, 10, 40, VK_TIMEOUT);
    assert(maliCompletedTimeline.value == 30);
    // A failed preceding producer wait invalidates even an earlier proof.
    assert(call(DD_WAIT_OUTPUT_COMPLETION, 20) == 0);
    waitResult = VK_TIMEOUT; assert(call(DD_WAIT_OUTPUT_COMPLETION, 20) == VK_TIMEOUT);
    before = waits; assert(call(DD_WAIT_OUTPUT_RETIRE, 20) == VK_TIMEOUT && waits == before + 1);
    waitResult = 0;
    before = waits; assert(call(DD_WAIT_DESCRIPTOR_REUSE, 31) == 0 && waits == before + 1);
    before = waits; assert(call(DD_WAIT_DESCRIPTOR_REUSE, 31, 11) == 0 && waits == before + 1);
    before = waits; assert(call(DD_WAIT_DESCRIPTOR_REUSE, 31, 11, (void *)2) == 0 && waits == before + 1);
    // Reset protects restarts and re-used numeric device/semaphore handles.
    maliCompletedTimeline.reset(); before = waits;
    assert(call(DD_WAIT_DESCRIPTOR_REUSE, 1) == 0 && waits == before + 1);
    maliLocalWaits = {}; assert(maliCompletedTimeline.covers((void *)1, 10, 1));
    maliWaitProfilingActive = false; before = waits;
    assert(call(DD_WAIT_DESCRIPTOR_REUSE, 1) == 0 && waits == before + 1);
    maliWaitProfilingActive = true; maliCompletedTimeline.reset();
}
static void write(MaliStagingPool<MaliStagingBackend>::Slot *s, uint8_t pattern) {
    assert(s->reserved && (!s->sequence || maliCompletedTimeline.covers((void *)1, 10, s->sequence)));
    auto &bytes = memory.at(s->resource.memory);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(pattern + i);
    for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == uint8_t(pattern + i));
}
static void pool() {
    using Lease = MaliStagingPool<MaliStagingBackend>::Lease;
    using Slot = MaliStagingPool<MaliStagingBackend>::Slot;
    Slot *first = nullptr;
    for (uint64_t i = 1; i <= 3; ++i) {
        Lease lease; assert(maliStaging.acquire(64, lease.slot) == 0); write(lease.slot, uint8_t(i));
        if (i == 1) first = lease.slot;
        lease.submitted(i);
    }
    assert(maliStaging.live() == 3 && maliStaging.stats.created == 3 && !maliStaging.stats.waits);
    expectedValue = 1; expectedSemaphore = 10; unsigned before = waits;
    { Lease lease; assert(maliStaging.acquire(64, lease.slot) == 0 && lease.slot == first);
      assert(waits == before + 1 && maliStaging.stats.waits == 1 && resets == 1); write(lease.slot, 4); lease.submitted(4); }
    // All slots pending; neither timeout nor device loss permits reuse/free.
    for (int error : {VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
        expectedValue = 2; waitResult = error; Slot *out = nullptr;
        auto saved = memory; unsigned oldDestroyed = destroyed;
        assert(maliStaging.acquire(64, out) == error && !out);
        assert(memory == saved && destroyed == oldDestroyed && maliStaging.live() == 3);
    }
    // During shutdown, clean completed entries even when another times out.
    waitResult = VK_TIMEOUT; expectedValue = 4;
    maliCompletedTimeline.observe((void *)1, 10, 3, VK_SUCCESS);
    assert(maliStaging.shutdown() == VK_TIMEOUT && maliStaging.live() == 1);
    assert(first->live && first->sequence == 4 && sizes.count(first->resource.buffer));
    waitResult = 0; expectedValue = 4;
    assert(maliStaging.shutdown() == 0 && !maliStaging.live() && sizes.empty() && memory.empty());
    assert(created == destroyed && allocated == freed);
    maliCompletedTimeline.reset(); maliStaging.stats = {};
    // Steady frames complete R after U: two warmed slots, no reuse waits/churn.
    unsigned oldCreated = created; before = waits;
    for (uint64_t frame = 1; frame <= 40; ++frame) {
        Lease lease; assert(maliStaging.acquire(64, lease.slot) == 0); write(lease.slot, uint8_t(frame));
        lease.submitted(frame * 2 - 1);
        maliCompletedTimeline.observe((void *)1, 10, frame * 2, VK_SUCCESS);
    }
    assert(created == oldCreated + 2 && maliStaging.stats.hits == 38 && !maliStaging.stats.waits && waits == before);
    // Compatible larger slots preserve bytes; larger requests resize only safe slots.
    { Lease lease; assert(maliStaging.acquire(128, lease.slot) == 0); write(lease.slot, 99); }
    assert(maliStaging.stats.resized == 1 && maliStaging.live() == 2);
    assert(maliStaging.shutdown() == 0 && memory.empty());
    for (createFailure = 1; createFailure <= 4; ++createFailure) {
        Slot *out = nullptr; assert(maliStaging.acquire(64, out) != 0 && !out);
        assert(!maliStaging.live() && memory.empty() && sizes.empty());
    }
    createFailure = 0;
    { Lease lease; assert(maliStaging.acquire(64, lease.slot) == 0);
      assert(maliStaging.shutdown() != 0 && maliStaging.live() == 1); }
    assert(maliStaging.shutdown() == 0 && memory.empty());
    assert(created == destroyed && allocated == freed);
    { MaliStagingUploadScope outer; assert(maliStagingUploadRecording);
      { MaliStagingUploadScope inner; assert(maliStagingUploadRecording); } assert(maliStagingUploadRecording); }
    assert(!maliStagingUploadRecording);
}
int main(int argc, char **) {
    missingProfile = argc > 1;
    watermark(); pool();
    std::puts("completed watermark and real staging backend PASS");
}
