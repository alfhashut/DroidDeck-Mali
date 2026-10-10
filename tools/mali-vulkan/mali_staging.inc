/* Normal renderer staging backend. Per-use Map/Unmap stays identical to SHM
 * import; only buffer/allocation/binding lifetimes change. */
struct MaliStagingBackend {
    struct Resource { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; };
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
        if (r.buffer) g_device.vk.DestroyBuffer(g_device.device(), r.buffer, nullptr);
        if (r.memory) g_device.vk.FreeMemory(g_device.device(), r.memory, nullptr);
        r = {};
    }
    int create(uint64_t bytes, Resource &r) {
        VkBufferCreateInfo bufferInfo{}; bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkResult result = g_device.vk.CreateBuffer(g_device.device(), &bufferInfo, nullptr, &r.buffer);
        if (result != VK_SUCCESS) return result;
        VkMemoryRequirements requirements{};
        g_device.vk.GetBufferMemoryRequirements(g_device.device(), r.buffer, &requirements);
        uint32_t type = g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, requirements.memoryTypeBits);
        if (type == ~0u || requirements.size < bytes) { destroy(r); return VK_ERROR_FEATURE_NOT_PRESENT; }
        VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        result = g_device.vk.AllocateMemory(g_device.device(), &allocation, nullptr, &r.memory);
        if (result == VK_SUCCESS) result = g_device.vk.BindBufferMemory(g_device.device(), r.buffer, r.memory, 0);
        if (result != VK_SUCCESS) destroy(r);
        return result;
    }
};
static MaliStagingPool<MaliStagingBackend> maliStaging;
