/* Normal staging slots own their whole-allocation coherent mapping. The pool
 * proves completion before CPU reuse, resizing, unmapping or destruction. */
#include "staging_api.h"
struct MaliStagingBackend {
    struct Resource {
        VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr; uint64_t mappedBytes = 0;
    };
    struct MapCounters { uint64_t created = 0, unmaps = 0, failures = 0; } maps;
    PFN_vkDroidDeckStagingMALI staging = nullptr;
    PFN_vkDroidDeckMapStagingMALI mapStaging = nullptr;
    VkResult resolve() {
        staging = reinterpret_cast<PFN_vkDroidDeckStagingMALI>(g_device.vk.GetDeviceProcAddr(g_device.device(), "vkDroidDeckStagingMALI"));
        mapStaging = reinterpret_cast<PFN_vkDroidDeckMapStagingMALI>(g_device.vk.GetDeviceProcAddr(g_device.device(), "vkDroidDeckMapStagingMALI"));
        if (!staging || !mapStaging) {
            std::fprintf(stderr, "persistent staging API unavailable: device lookup returned null for %s\n",
                         !staging ? "vkDroidDeckStagingMALI" : "vkDroidDeckMapStagingMALI");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult result = mapStaging(g_device.device(), VK_NULL_HANDLE, nullptr);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "persistent staging API unavailable: wire-v7/normal-mode probe VkResult=%d (see proxy reason)\n", int(result));
            staging = nullptr; mapStaging = nullptr;
            return result;
        }
        std::fputs("persistent staging API: available\n", stderr);
        return VK_SUCCESS;
    }
    static constexpr int invalid = VK_ERROR_INITIALIZATION_FAILED;
    bool completed(uint64_t value) {
        return maliCompletedTimeline.covers(g_device.device(), g_device.maliScratchTimeline(), value);
    }
    int wait(uint64_t value, const Resource &r) {
        VkSemaphore semaphore = g_device.maliScratchTimeline();
        VkSemaphoreWaitInfo info{}; info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1; info.pSemaphores = &semaphore; info.pValues = &value;
        MaliWaitScope reason(DD_WAIT_SHM_STAGING_REUSE, (uint64_t)(uintptr_t)r.buffer);
        VkResult result = MaliProfiledWait(g_device.vk.WaitSemaphores, g_device.vk.GetDeviceProcAddr, g_device.device(), &info, 5000000000ull);
        if (result == VK_SUCCESS) g_device.resetCmdBuffers(value);
        return result;
    }
    void destroy(Resource &r, uint64_t completedSequence = 0) {
        // Buffer destruction also requires recorded references to be retired.
        // The pool only supplies a nonzero sequence after proving completion.
        if (completedSequence) g_device.resetCmdBuffers(completedSequence);
        if (r.mapped) {
            g_device.vk.UnmapMemory(g_device.device(), r.memory); ++maps.unmaps;
            r.mapped = nullptr; r.mappedBytes = 0;
        }
        if (r.buffer) g_device.vk.DestroyBuffer(g_device.device(), r.buffer, nullptr);
        if (r.memory) g_device.vk.FreeMemory(g_device.device(), r.memory, nullptr);
        r = {};
    }
    int create(uint64_t bytes, Resource &r) {
        if (!staging || !mapStaging) { VkResult result = resolve(); if (result != VK_SUCCESS) return result; }
        VkBufferCreateInfo bufferInfo{}; bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkResult result = g_device.vk.CreateBuffer(g_device.device(), &bufferInfo, nullptr, &r.buffer);
        if (result != VK_SUCCESS) return result;
        VkMemoryRequirements requirements{};
        g_device.vk.GetBufferMemoryRequirements(g_device.device(), r.buffer, &requirements);
        uint32_t type = g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, requirements.memoryTypeBits);
        if (type == ~0u || requirements.size < bytes) {
            std::fprintf(stderr, "persistent staging acquire unavailable: host-visible/coherent type=%u memoryTypeBits=0x%x allocation=%llu requested=%llu\n",
                         type, requirements.memoryTypeBits, (unsigned long long)requirements.size, (unsigned long long)bytes);
            destroy(r); return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        result = g_device.vk.AllocateMemory(g_device.device(), &allocation, nullptr, &r.memory);
        if (result == VK_SUCCESS) result = g_device.vk.BindBufferMemory(g_device.device(), r.buffer, r.memory, 0);
        if (result == VK_SUCCESS) {
            result = mapStaging(g_device.device(), r.memory, &r.mapped);
            if (result == VK_SUCCESS) { ++maps.created; r.mappedBytes = requirements.size; }
            else {
                ++maps.failures; r.mapped = nullptr;
                std::fprintf(stderr, "persistent staging acquire failed: vkDroidDeckMapStagingMALI VkResult=%d\n", int(result));
            }
        }
        if (result != VK_SUCCESS) destroy(r);
        return result;
    }
    VkResult prepare(const Resource &r, VkCommandBuffer command) {
        if (!r.mapped || !r.mappedBytes || !staging || !command) return VK_ERROR_INITIALIZATION_FAILED;
        return staging(g_device.device(), r.memory, command);
    }
};
static MaliStagingPool<MaliStagingBackend> maliStaging;
