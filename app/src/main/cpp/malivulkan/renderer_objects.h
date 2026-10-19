/* Android-only renderer handles, typed IDs with strict device ownership. */
struct native_renderer {
#define MB_RENDERER_ENTRY(n) PFN_vk##n n;
#include "renderer_entries.def"
#undef MB_RENDERER_ENTRY
    int enabled;
    struct native_renderer_object {
        uint32_t id, kind, parent, image, type, count, usage, format;
        union { VkSemaphore semaphore; VkImageView view; VkSampler sampler;
            VkDescriptorSetLayout set_layout; VkPipelineLayout pipeline_layout;
            VkDescriptorPool pool; VkDescriptorSet set; VkShaderModule shader; VkPipeline pipeline; } handle;
        uint64_t last_signal, descriptor_revision;
        struct { uint32_t binding, type, count, sampler; } bindings[7];
        struct { uint32_t binding, element, type, view, sampler, buffer; uint64_t offset, range; } descriptors[64];
        uint32_t descriptor_count;
    } *objects;
    VkPhysicalDeviceLimits limits;
};
