/* Narrow real-renderer marshaling. Only explicit fields and broker-owned IDs. */
static VkResult renderer_rpc(struct proxy_logical *d, uint32_t op, const uint8_t *args, uint32_t n, uint8_t *out, uint32_t expected) {
    if (!d || d->owner->wire_version != MB_RENDERER_VERSION || n > MB_RENDERER_MAX_REQUEST - 4 || expected > 256) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t *request = malloc(n + 4), reply[MB_PREFIX_BYTES + 256]; uint32_t bytes = 0;
    if (!request) return VK_ERROR_OUT_OF_HOST_MEMORY;
    mb_put_u32(request, d->id); if (n) memcpy(request + 4, args, n);
    pthread_mutex_lock(&d->owner->lock);
    VkResult r = rpc(d->owner, op, request, n + 4, reply, &bytes, sizeof(reply));
    if (bytes && (bytes != MB_PREFIX_BYTES + (r == VK_SUCCESS ? expected : 0) || mb_get_u32(reply + 8) != (r == VK_SUCCESS && expected ? 1u : 0u))) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; }
    if (r == VK_SUCCESS && expected && out) memcpy(out, reply + MB_PREFIX_BYTES, expected);
    pthread_mutex_unlock(&d->owner->lock); free(request); return r;
}
static VkResult renderer_new(struct proxy_logical *d, uint32_t op, enum proxy_resource_kind kind, const uint8_t *args, uint32_t n, struct proxy_resource **out) {
    *out = NULL; struct proxy_resource *o = calloc(1, sizeof(*o)); if (!o) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t reply[4]; VkResult r = renderer_rpc(d, op, args, n, reply, 4);
    if (r == VK_SUCCESS && !mb_get_u32(reply)) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; }
    if (r != VK_SUCCESS) { free(o); return r; }
    interop_publish(d, o, kind, mb_get_u32(reply)); *out = o; return VK_SUCCESS;
}
#define R_DESTROY(Name, KIND, RKIND) \
static VKAPI_ATTR void VKAPI_CALL proxy_Destroy##Name(VkDevice device, Vk##Name handle, const VkAllocationCallbacks *a) { \
    if (!device || !handle) return; \
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *o = submit_find(d, (uintptr_t)handle, KIND); \
    VkResult r = VK_ERROR_INITIALIZATION_FAILED; uint8_t args[8]; \
    if (o && !a) { mb_put_u32(args, RKIND); mb_put_u32(args + 4, o->id); r = renderer_rpc(d, MB_RENDERER_DESTROY, args, 8, NULL, 0); } \
    if (r == VK_SUCCESS) { pthread_mutex_lock(&d->owner->lock); if (RKIND == MB_R_POOL) proxy_registry_retire_children(&d->registry, o, PROXY_DESCRIPTOR_SET); else proxy_registry_retire(&d->registry, o); pthread_mutex_unlock(&d->owner->lock); } \
    submit_void_error(d, "vkDestroy" #Name, r); \
}
R_DESTROY(Semaphore, PROXY_SEMAPHORE, MB_R_SEMAPHORE)
R_DESTROY(ImageView, PROXY_VIEW, MB_R_VIEW)
R_DESTROY(Sampler, PROXY_SAMPLER, MB_R_SAMPLER)
R_DESTROY(DescriptorSetLayout, PROXY_SET_LAYOUT, MB_R_SET_LAYOUT)
R_DESTROY(PipelineLayout, PROXY_PIPELINE_LAYOUT, MB_R_PIPELINE_LAYOUT)
R_DESTROY(DescriptorPool, PROXY_DESCRIPTOR_POOL, MB_R_POOL)
R_DESTROY(ShaderModule, PROXY_SHADER, MB_R_SHADER)
R_DESTROY(Pipeline, PROXY_PIPELINE, MB_R_PIPELINE)
#undef R_DESTROY
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO || ci->flags || !ci->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkSemaphoreTypeCreateInfoKHR *t = ci->pNext;
    if (t->sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR || t->pNext || t->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE_KHR) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[8]; mb_put_u64(args, t->initialValue); struct proxy_resource *o;
    VkResult r = renderer_new((struct proxy_logical *)device, MB_RENDERER_SEMAPHORE_CREATE, PROXY_SEMAPHORE, args, 8, &o);
    if (r == VK_SUCCESS) *out = (VkSemaphore)(uintptr_t)o;
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_GetSemaphoreCounterValueKHR(VkDevice device, VkSemaphore semaphore, uint64_t *out) {
    if (!out || !device) return VK_ERROR_INITIALIZATION_FAILED;
    *out = 0; struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *s = submit_find(d, (uintptr_t)semaphore, PROXY_SEMAPHORE);
    if (!s) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[4], reply[8]; mb_put_u32(args, s->id); VkResult r = renderer_rpc(d, MB_RENDERER_COUNTER, args, 4, reply, 8);
    if (r == VK_SUCCESS) *out = mb_get_u64(reply);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_WaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfoKHR *info, uint64_t timeout) {
    if (!device || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR || info->pNext || info->flags || info->semaphoreCount != 1 || !info->pSemaphores || !info->pValues || timeout > MB_SUBMIT_TIMEOUT_NS) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *s = submit_find(d, (uintptr_t)info->pSemaphores[0], PROXY_SEMAPHORE);
    if (!s) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[20]; mb_put_u32(args, s->id); mb_put_u64(args + 4, info->pValues[0]); mb_put_u64(args + 12, timeout);
    return renderer_rpc(d, MB_RENDERER_WAIT, args, 20, NULL, 0);
}
/* Private normal-session entrypoint. Legacy Vulkan waits retain their exact
 * 24-byte request and empty reply; profiling only adds reason/sample metadata. */
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckProfiledWaitMALI(VkDevice device,
        const VkSemaphoreWaitInfoKHR *info, uint64_t timeout, uint32_t reason, uint64_t resource) {
    if (!device || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR || info->pNext || info->flags || info->semaphoreCount != 1 || !info->pSemaphores || !info->pValues || timeout > MB_SUBMIT_TIMEOUT_NS) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_instance *s = d->owner;
    if (!s->perf_enabled || s->wire_version != MB_RENDERER_VERSION || reason >= DD_WAIT_REASONS) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_resource *sem = submit_find(d, (uintptr_t)info->pSemaphores[0], PROXY_SEMAPHORE);
    if (!sem) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t detail = (uint32_t)resource;
    if (reason == DD_WAIT_SHM_STAGING_DESTROY) {
        struct proxy_resource *buffer = submit_find(d, (uintptr_t)resource, PROXY_BUFFER);
        if (!buffer) return VK_ERROR_INITIALIZATION_FAILED;
        detail = buffer->id;
    } else if (resource > UINT32_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t request[DD_WAIT_REQUEST_BYTES], reply[MB_PREFIX_BYTES + DD_WAIT_SAMPLE_BYTES]; uint32_t bytes = 0;
    mb_put_u32(request, d->id); mb_put_u32(request + 4, sem->id); mb_put_u64(request + 8, info->pValues[0]);
    mb_put_u64(request + 16, timeout); mb_put_u32(request + 24, reason); mb_put_u32(request + 28, detail);
    pthread_mutex_lock(&s->lock);
    uint64_t before_rpc = s->perf.op[MB_RENDERER_WAIT].ns;
    VkResult r = rpc(s, MB_RENDERER_WAIT, request, sizeof(request), reply, &bytes, sizeof(reply));
    uint64_t rtt = s->perf.op[MB_RENDERER_WAIT].ns - before_rpc;
    struct dd_wait_sample sample; int available = 0;
    if (r >= VK_SUCCESS && bytes == sizeof(reply) && mb_get_u32(reply + 8) == 1) {
        dd_wait_decode(reply + MB_PREFIX_BYTES, &sample); available = 1;
    } else if (r >= VK_SUCCESS || (bytes && (bytes != MB_PREFIX_BYTES || mb_get_u32(reply + 8) != 0))) {
        shutdown(s->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED;
    }
    dd_wait_add(&s->perf.wait[reason], rtt, sem->id, info->pValues[0], detail, (uint32_t)r, available ? &sample : NULL);
    pthread_mutex_unlock(&s->lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateSampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || ci->pNext || ci->flags || ci->magFilter > VK_FILTER_LINEAR || ci->minFilter != ci->magFilter || ci->mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST || ci->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || ci->addressModeV != ci->addressModeU || ci->addressModeW != ci->addressModeU || ci->mipLodBias || ci->anisotropyEnable || ci->maxAnisotropy || ci->compareEnable || ci->compareOp || ci->minLod || ci->maxLod || ci->borderColor != VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK || ci->unnormalizedCoordinates > 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[8]; mb_put_u32(args, ci->magFilter == VK_FILTER_NEAREST); mb_put_u32(args + 4, ci->unnormalizedCoordinates); struct proxy_resource *o;
    VkResult r = renderer_new((struct proxy_logical *)device, MB_RENDERER_SAMPLER, PROXY_SAMPLER, args, 8, &o);
    if (r == VK_SUCCESS) *out = (VkSampler)(uintptr_t)o;
    return r;
}
static VkResult renderer_CreateImage(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || ci->format != VK_FORMAT_R8G8B8A8_UNORM || ci->mipLevels != 1 || ci->arrayLayers != 1 || ci->samples != VK_SAMPLE_COUNT_1_BIT || ci->tiling != VK_IMAGE_TILING_OPTIMAL || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE || ci->queueFamilyIndexCount || ci->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkImageFormatListCreateInfoKHR *list = ci->pNext;
    if (list && (list->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR || list->pNext || list->viewFormatCount != 2 || !list->pViewFormats)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[36]; uint32_t fields[] = {ci->imageType, ci->extent.width, ci->extent.height, ci->extent.depth, ci->usage, ci->flags, list ? 2u : 0u};
    for (unsigned i = 0; i < 7; ++i) mb_put_u32(args + i * 4, fields[i]);
    if (list) { mb_put_u32(args + 28, list->pViewFormats[0]); mb_put_u32(args + 32, list->pViewFormats[1]); }
    struct proxy_resource *o; VkResult r = renderer_new((struct proxy_logical *)device, MB_RENDERER_IMAGE, PROXY_IMAGE, args, list ? 36 : 28, &o);
    if (r == VK_SUCCESS) *out = (VkImage)(uintptr_t)o;
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || ci->flags || ci->components.r || ci->components.g || ci->components.b || ci->components.a || !interop_color_range(&ci->subresourceRange)) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkImageViewUsageCreateInfo *usage = ci->pNext;
    if (usage && (usage->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO || usage->pNext)) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *im = submit_find(d, (uintptr_t)ci->image, PROXY_IMAGE); if (!im) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[16]; const uint32_t fields[] = {im->id, ci->viewType, ci->format, usage ? usage->usage : 0}; for (unsigned i = 0; i < 4; ++i) mb_put_u32(args + i * 4, fields[i]);
    struct proxy_resource *o; VkResult r = renderer_new(d, MB_RENDERER_VIEW, PROXY_VIEW, args, 16, &o); if (r == VK_SUCCESS) { o->image_id = im->id; *out = (VkImageView)(uintptr_t)o; } return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO || ci->pNext || ci->flags || ci->bindingCount != 7 || !ci->pBindings) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; uint8_t args[116]; mb_put_u32(args, 7);
    for (unsigned i = 0; i < 7; ++i) {
        const VkDescriptorSetLayoutBinding *b = ci->pBindings + i; uint32_t sampler = 0;
        if (b->stageFlags != VK_SHADER_STAGE_COMPUTE_BIT || b->descriptorCount > 16) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (b->pImmutableSamplers) { for (unsigned j = 0; j < b->descriptorCount; ++j) { struct proxy_resource *sam = submit_find(d, (uintptr_t)b->pImmutableSamplers[j], PROXY_SAMPLER); if (!sam || (sampler && sampler != sam->id)) return VK_ERROR_INITIALIZATION_FAILED; sampler = sam->id; } }
        uint32_t fields[] = {b->binding, b->descriptorType, b->descriptorCount, sampler}; for (unsigned j = 0; j < 4; ++j) mb_put_u32(args + 4 + i * 16 + j * 4, fields[j]);
    }
    struct proxy_resource *o; VkResult r = renderer_new(d, MB_RENDERER_SET_LAYOUT, PROXY_SET_LAYOUT, args, sizeof(args), &o); if (r == VK_SUCCESS) *out = (VkDescriptorSetLayout)(uintptr_t)o; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO || ci->pNext || ci->flags || ci->setLayoutCount != 1 || !ci->pSetLayouts || ci->pushConstantRangeCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *l = submit_find(d, (uintptr_t)ci->pSetLayouts[0], PROXY_SET_LAYOUT); if (!l) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[4]; mb_put_u32(args, l->id); struct proxy_resource *o; VkResult r = renderer_new(d, MB_RENDERER_PIPELINE_LAYOUT, PROXY_PIPELINE_LAYOUT, args, 4, &o); if (r == VK_SUCCESS) *out = (VkPipelineLayout)(uintptr_t)o; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO || ci->pNext || ci->flags || ci->poolSizeCount != 3 || !ci->pPoolSizes) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[32]; mb_put_u32(args, ci->maxSets); mb_put_u32(args + 4, 3); for (unsigned i = 0; i < 3; ++i) { mb_put_u32(args + 8 + i * 8, ci->pPoolSizes[i].type); mb_put_u32(args + 12 + i * 8, ci->pPoolSizes[i].descriptorCount); }
    struct proxy_resource *o; VkResult r = renderer_new((struct proxy_logical *)device, MB_RENDERER_POOL, PROXY_DESCRIPTOR_POOL, args, sizeof(args), &o); if (r == VK_SUCCESS) *out = (VkDescriptorPool)(uintptr_t)o; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    if (!device || !ai || !out || ai->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO || ai->pNext || !ai->descriptorSetCount || ai->descriptorSetCount > 24 || !ai->pSetLayouts) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *pool = submit_find(d, (uintptr_t)ai->descriptorPool, PROXY_DESCRIPTOR_POOL); if (!pool) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_resource *sets[24] = {0}; uint8_t args[104], reply[96]; uint32_t n = ai->descriptorSetCount; mb_put_u32(args, pool->id); mb_put_u32(args + 4, n);
    VkResult r = VK_SUCCESS;
    for (unsigned i = 0; i < n; ++i) { out[i] = VK_NULL_HANDLE; struct proxy_resource *l = submit_find(d, (uintptr_t)ai->pSetLayouts[i], PROXY_SET_LAYOUT); if (!l) { r = VK_ERROR_INITIALIZATION_FAILED; break; } sets[i] = calloc(1, sizeof(*sets[i])); if (!sets[i]) { r = VK_ERROR_OUT_OF_HOST_MEMORY; break; } mb_put_u32(args + 8 + i * 4, l->id); }
    if (r == VK_SUCCESS) r = renderer_rpc(d, MB_RENDERER_SETS, args, 8 + n * 4, reply, n * 4);
    if (r == VK_SUCCESS) for (unsigned i = 0; i < n; ++i) {
        if (!mb_get_u32(reply + i * 4)) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; break; }
        for (unsigned j = 0; j < i; ++j) if (mb_get_u32(reply + j * 4) == mb_get_u32(reply + i * 4)) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; }
    }
    for (unsigned i = 0; i < n; ++i) { if (r == VK_SUCCESS) { interop_publish(d, sets[i], PROXY_DESCRIPTOR_SET, mb_get_u32(reply + i * 4)); sets[i]->pool = pool->id; out[i] = (VkDescriptorSet)(uintptr_t)sets[i]; } else free(sets[i]); }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_UpdateDescriptorSets(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)copy; if (!device) return; struct proxy_logical *d = (struct proxy_logical *)device;
    VkResult r = VK_ERROR_FEATURE_NOT_PRESENT; uint8_t args[2048]; unsigned at = 4; mb_put_u32(args, count);
    if (count != 7 || !writes || copies) goto done;
    for (unsigned i = 0; i < count; ++i) {
        const VkWriteDescriptorSet *w = writes + i; struct proxy_resource *set = submit_find(d, (uintptr_t)w->dstSet, PROXY_DESCRIPTOR_SET);
        if (!set || w->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w->pNext || !w->descriptorCount || w->descriptorCount > 16 || at > sizeof(args) - 20) goto done;
        uint32_t fields[] = {set->id, w->dstBinding, w->dstArrayElement, w->descriptorType, w->descriptorCount}; for (unsigned j = 0; j < 5; ++j) mb_put_u32(args + at + j * 4, fields[j]); at += 20;
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (w->descriptorCount != 1 || !w->pBufferInfo || at > sizeof(args) - 20) goto done;
            const VkDescriptorBufferInfo *bi = w->pBufferInfo; struct proxy_resource *b = submit_find(d, (uintptr_t)bi->buffer, PROXY_BUFFER); if (!b) goto done;
            mb_put_u32(args + at, b->id); mb_put_u64(args + at + 4, bi->offset); mb_put_u64(args + at + 12, bi->range); at += 20;
        } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (!w->pImageInfo || w->descriptorCount > (sizeof(args) - at)/12) goto done;
            for (unsigned j = 0; j < w->descriptorCount; ++j) {
                const VkDescriptorImageInfo *ii = w->pImageInfo + j; struct proxy_resource *view = submit_find(d, (uintptr_t)ii->imageView, PROXY_VIEW), *sam = ii->sampler ? submit_find(d, (uintptr_t)ii->sampler, PROXY_SAMPLER) : NULL;
                if (!view || (ii->sampler && !sam)) goto done;
                mb_put_u32(args + at, sam ? sam->id : 0); mb_put_u32(args + at + 4, view->id); mb_put_u32(args + at + 8, ii->imageLayout); at += 12;
            }
        } else goto done;
    }
    r = renderer_rpc(d, MB_RENDERER_UPDATE, args, at, NULL, 0);
    if (r == VK_SUCCESS) for (unsigned i = 0; i < count; ++i) {
        const VkWriteDescriptorSet *w = writes + i;
        if (w->dstBinding != 1 && w->dstBinding != 3) continue;
        struct proxy_resource *set = submit_find(d, (uintptr_t)w->dstSet, PROXY_DESCRIPTOR_SET);
        for (unsigned j = 0; j < w->descriptorCount; ++j) {
            const VkDescriptorImageInfo *ii = w->pImageInfo + j;
            struct proxy_resource *view = submit_find(d, (uintptr_t)ii->imageView, PROXY_VIEW);
            struct proxy_resource *sam = submit_find(d, (uintptr_t)ii->sampler, PROXY_SAMPLER);
            LOG("BLIT %s descriptor set ID=%u binding=%u array index=%u image broker ID=%u image-view broker ID=%u sampler broker ID=%u layout=%u",
                w->dstBinding == 1 ? "output storage" : "source/sampler", set->id, w->dstBinding, w->dstArrayElement + j,
                view->image_id, view->id, sam ? sam->id : 0, ii->imageLayout);
        }
    }
done: submit_void_error(d, "vkUpdateDescriptorSets", r);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || ci->pNext || ci->flags || !ci->pCode || ci->codeSize < 20 || ci->codeSize > MB_RENDERER_MAX_SHADER || ci->codeSize % 4 || (uintptr_t)ci->pCode % 4) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t *args = malloc(ci->codeSize + 4); if (!args) return VK_ERROR_OUT_OF_HOST_MEMORY;
    mb_put_u32(args, (uint32_t)ci->codeSize); for (unsigned i = 0; i < ci->codeSize/4; ++i) mb_put_u32(args + 4 + i * 4, ci->pCode[i]);
    struct proxy_resource *o; VkResult r = renderer_new((struct proxy_logical *)device, MB_RENDERER_SHADER, PROXY_SHADER, args, (uint32_t)ci->codeSize + 4, &o); free(args); if (r == VK_SUCCESS) *out = (VkShaderModule)(uintptr_t)o; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || cache || n != 1 || !ci || a || ci->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || ci->pNext || ci->flags || ci->basePipelineHandle || ci->basePipelineIndex || ci->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || ci->stage.pNext || ci->stage.flags || ci->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT || !ci->stage.pName || strcmp(ci->stage.pName, "main")) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkSpecializationInfo *s = ci->stage.pSpecializationInfo;
    if (!s || s->mapEntryCount != 7 || !s->pMapEntries || s->dataSize != 28 || !s->pData) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *shader = submit_find(d, (uintptr_t)ci->stage.module, PROXY_SHADER), *layout = submit_find(d, (uintptr_t)ci->layout, PROXY_PIPELINE_LAYOUT); if (!shader || !layout) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[36]; mb_put_u32(args, shader->id); mb_put_u32(args + 4, layout->id);
    for (unsigned i = 0; i < 7; ++i) { if (s->pMapEntries[i].constantID != i || s->pMapEntries[i].offset != i * 4 || s->pMapEntries[i].size != 4) return VK_ERROR_FEATURE_NOT_PRESENT; uint32_t value; memcpy(&value, (const uint8_t *)s->pData + i * 4, 4); mb_put_u32(args + 8 + i * 4, value); }
    struct proxy_resource *o; VkResult r = renderer_new(d, MB_RENDERER_PIPELINE, PROXY_PIPELINE, args, sizeof(args), &o); if (r == VK_SUCCESS) *out = (VkPipeline)(uintptr_t)o; return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdBindPipeline(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    struct proxy_resource *c = interop_command(command); if (!c) return; struct proxy_resource *p = submit_find(c->owner, (uintptr_t)pipeline, PROXY_PIPELINE); VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (p && point == VK_PIPELINE_BIND_POINT_COMPUTE) { uint8_t args[8]; mb_put_u32(args, c->id); mb_put_u32(args + 4, p->id); r = renderer_rpc(c->owner, MB_RENDERER_BIND_PIPELINE, args, 8, NULL, 0); } if (r == VK_SUCCESS) LOG("BLIT pipeline ID=%u command ID=%u", p->id, c->id); submit_void_error(c->owner, "vkCmdBindPipeline", r);
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets) {
    (void)offsets; struct proxy_resource *c = interop_command(command); if (!c) return;
    struct proxy_resource *l = submit_find(c->owner, (uintptr_t)layout, PROXY_PIPELINE_LAYOUT), *set = count == 1 && sets ? submit_find(c->owner, (uintptr_t)sets[0], PROXY_DESCRIPTOR_SET) : NULL; VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (l && set && point == VK_PIPELINE_BIND_POINT_COMPUTE && !first && !dynamic_count) { uint8_t args[12]; mb_put_u32(args, c->id); mb_put_u32(args + 4, l->id); mb_put_u32(args + 8, set->id); r = renderer_rpc(c->owner, MB_RENDERER_BIND_SET, args, 12, NULL, 0); } if (r == VK_SUCCESS) LOG("BLIT bound descriptor set ID=%u pipeline layout ID=%u command ID=%u set index=0", set->id, l->id, c->id); submit_void_error(c->owner, "vkCmdBindDescriptorSets", r);
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdDispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    struct proxy_resource *c = interop_command(command); if (!c) return; uint8_t args[16]; uint32_t fields[] = {c->id, x, y, z}; for (unsigned i = 0; i < 4; ++i) mb_put_u32(args + i * 4, fields[i]); VkResult r = renderer_rpc(c->owner, MB_RENDERER_DISPATCH, args, 16, NULL, 0); submit_void_error(c->owner, "vkCmdDispatch", r);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_ResetCommandBuffer(VkCommandBuffer command, VkCommandBufferResetFlags flags) {
    struct proxy_resource *c = interop_command(command); if (!c || flags) return VK_ERROR_FEATURE_NOT_PRESENT; uint8_t args[4]; mb_put_u32(args, c->id); return renderer_rpc(c->owner, MB_RENDERER_RESET_COMMAND, args, 4, NULL, 0);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DeviceWaitIdle(VkDevice device) {
    struct proxy_logical *d = (struct proxy_logical *)device;
    VkResult r = renderer_rpc(d, MB_RENDERER_IDLE, NULL, 0, NULL, 0);
    return r == VK_SUCCESS && d->submit_failed ? (VkResult)atomic_load(&d->submit_failed) : r;
}
static VkResult renderer_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    if (!queue || count != 1 || !si || si->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || !si->pNext || si->waitSemaphoreCount || si->commandBufferCount != 1 || !si->pCommandBuffers || si->signalSemaphoreCount != 1 || !si->pSignalSemaphores) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkTimelineSemaphoreSubmitInfoKHR *t = si->pNext;
    if (t->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR || t->pNext || t->waitSemaphoreValueCount || t->signalSemaphoreValueCount != 1 || !t->pSignalSemaphoreValues) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_queue *q = (struct proxy_queue *)queue; struct proxy_logical *d = q->owner;
    struct proxy_resource *c = submit_find(d, (uintptr_t)si->pCommandBuffers[0], PROXY_COMMAND), *s = submit_find(d, (uintptr_t)si->pSignalSemaphores[0], PROXY_SEMAPHORE), *f = fence ? submit_find(d, (uintptr_t)fence, PROXY_FENCE) : NULL;
    if (!q->id || !c || !s || (fence && !f)) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->submit_failed) return (VkResult)atomic_load(&d->submit_failed);
    /* Persistently mapped coherent upload data must reach the REAL mapped allocation. */
    uint64_t nodes = 0, live = 0, mapped = 0;
    /* Active links mutate on retirement. Keep the connection lock throughout
     * discovery/uploads; the locked helper uses the identical ACK/range path. */
    pthread_mutex_lock(&d->owner->lock);
    for (struct proxy_resource *m = d->registry.active; m; m = m->active_next) {
        int is_live = m->live;
        if (d->owner->perf_enabled) {
            ++nodes; live += !!is_live; mapped += is_live && m->kind == PROXY_MEMORY && m->mirror;
        }
        if (is_live && m->kind == PROXY_MEMORY && m->mirror && (m->pool & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            /* A pool slot is written only for its explicitly armed upload command.
             * Re-uploading it for R would race the still-pending upload U. */
            if (m->staging_managed && m->staging_command != c->id) continue;
            unsigned previous_phase = dd_memory_submit_phase;
            dd_memory_submit_phase = c->profile_upload ? DD_WRITE_U : f ? DD_WRITE_R : DD_WRITE_OUTSIDE;
            VkResult r = m->upload_ring ? interop_upload_ring_locked(m, c->id) :
                interop_copy_upload_locked(m, m->map_offset, m->map_size);
            dd_memory_submit_phase = previous_phase;
            if (r != VK_SUCCESS) { pthread_mutex_unlock(&d->owner->lock); return r; }
        }
    }
    pthread_mutex_unlock(&d->owner->lock);
    uint8_t args[24]; uint32_t fields[] = {q->id, c->id, f ? f->id : 0, s->id}; for (unsigned i = 0; i < 4; ++i) mb_put_u32(args + i * 4, fields[i]); mb_put_u64(args + 16, t->pSignalSemaphoreValues[0]);
    VkResult result = renderer_rpc(d, MB_RENDERER_SUBMIT, args, 24, NULL, 0);
    if (result == VK_SUCCESS) {
        pthread_mutex_lock(&d->owner->lock);
        if (d->owner->perf_enabled) {
            d->owner->memory_perf.submit_nodes = nodes; d->owner->memory_perf.submit_live = live;
            d->owner->memory_perf.submit_mapped = mapped; ++d->owner->memory_perf.submits;
        }
        c->profile_upload = 0;
        for (struct proxy_resource *m = d->registry.active; m; m = m->active_next) {
            if (m->upload_ring) dd_ring_submitted(m->upload_ring, c->id);
            if (m->staging_managed && m->staging_command == c->id) m->staging_command = 0;
        }
        pthread_mutex_unlock(&d->owner->lock);
    }
    return result;
}
/* Private, local known-writer contract. Neither discovery nor any marker sends
 * an RPC. Device+instance dispatch expose this explicitly like staging APIs. */
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckUploadRingMALI(VkDevice device,
        VkDeviceMemory memory, VkBuffer buffer, VkCommandBuffer command,
        uint32_t operation, VkDeviceSize offset, VkDeviceSize length) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_logical *d = (struct proxy_logical *)device;
    if (d->owner->wire_version != MB_RENDERER_VERSION || !d->owner->perf_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (operation == DD_RING_PROBE) return !memory && !buffer && !command && !offset && !length ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_resource *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY);
    struct proxy_resource *b = buffer ? submit_find(d, (uintptr_t)buffer, PROXY_BUFFER) : NULL;
    struct proxy_resource *c = command ? submit_find(d, (uintptr_t)command, PROXY_COMMAND) : NULL;
    if (!m || m->owner != d) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&d->owner->lock);
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (operation == DD_RING_REGISTER) {
        uint32_t usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (!command && !offset && b && b->owner == d && b->bound_memory == (uintptr_t)m && !b->bound_offset &&
            (b->profile_usage & usage) == usage && length == b->allocation && !m->upload_ring &&
            !m->staging_managed && m->mirror &&
            (m->pool & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
            mb_interop_mapped_range(m->allocation, m->map_offset, m->map_size, 0, m->allocation) &&
            length && length <= m->allocation) {
            m->upload_ring = calloc(1, sizeof(*m->upload_ring));
            if (m->upload_ring) { dd_ring_init(m->upload_ring, length); result = VK_SUCCESS; }
            else result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    } else if (m->upload_ring) {
        if (buffer || (command && operation != DD_RING_SEAL)) {
            dd_ring_change(m->upload_ring); m->upload_ring->uncertain |= DD_RING_UNSEALED;
        }
        else if (operation == DD_RING_RESERVE) result = dd_ring_reserve(m->upload_ring, offset, length) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
        else if (operation == DD_RING_MODIFIED) result = dd_ring_modified(m->upload_ring, offset, length) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
        else if (operation == DD_RING_SEAL && c && !offset && !length) { dd_ring_seal(m->upload_ring, c->id); result = VK_SUCCESS; }
        else { dd_ring_change(m->upload_ring); m->upload_ring->uncertain |= DD_RING_UNSEALED; }
    }
    pthread_mutex_unlock(&d->owner->lock);
    return result;
}
/* Local capability probe. Guards are also enforced on every private operation. */
static VkResult renderer_staging_available(struct proxy_logical *d) {
    if (d->owner->wire_version != MB_RENDERER_VERSION) {
        LOG("persistent staging API unavailable: wire version=%u required=%u", d->owner->wire_version, MB_RENDERER_VERSION);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!d->owner->perf_enabled) {
        LOG("persistent staging API unavailable: normal-session profiling mode disabled");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return VK_SUCCESS;
}
/* Local staging contract only. No global dirty tracking or fake completion. */
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckStagingMALI(VkDevice device, VkDeviceMemory memory, VkCommandBuffer command) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_logical *d = (struct proxy_logical *)device;
    VkResult available = renderer_staging_available(d);
    if (available != VK_SUCCESS) return available;
    struct proxy_resource *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY);
    struct proxy_resource *c = command ? submit_find(d, (uintptr_t)command, PROXY_COMMAND) : NULL;
    if (!m || m->owner != d || !m->mirror || !(m->pool & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !mb_interop_mapped_range(m->allocation, m->map_offset, m->map_size, 0, m->allocation) ||
        (command && (!c || c->owner != d || !c->id))) {
        LOG("persistent staging arm/register failed: memory identity, coherent whole mapping or command identity invalid");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pthread_mutex_lock(&d->owner->lock);
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (!command && !m->staging_managed && !m->staging_command) { m->staging_managed = 1; result = VK_SUCCESS; }
    else if (command && m->staging_managed && !m->staging_command) {
        m->staging_command = c->id; result = VK_SUCCESS;
        if (d->owner->perf_enabled) c->profile_upload = 1;
    }
    pthread_mutex_unlock(&d->owner->lock);
    return result;
}
/* Upload-only pool allocations: native map once, zeroed local mirror, no READ.
 * Ordinary vkMapMemory still downloads coherent memory. QueueSubmit still ACKs
 * all armed uploads before U; unrelated R submissions skip these mappings. */
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckMapStagingMALI(VkDevice device, VkDeviceMemory memory, void **out) {
    if (out) *out = NULL;
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_logical *d = (struct proxy_logical *)device;
    VkResult result = renderer_staging_available(d);
    if (result != VK_SUCCESS) return result;
    if (!memory && !out) return VK_SUCCESS; /* availability probe */
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_resource *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY);
    uint32_t required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!m || m->owner != d || !m->id || !m->allocation || m->mirror ||
        m->staging_managed || m->staging_command || (m->pool & required) != required) {
        LOG("persistent staging map failed: memory identity, unmapped allocation or host-visible/coherent type invalid");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    result = interop_map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, out, 0);
    if (result == VK_SUCCESS) {
        pthread_mutex_lock(&d->owner->lock);
        m->staging_managed = 1;
        pthread_mutex_unlock(&d->owner->lock);
    }
    return result;
}
static void renderer_Barrier(VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers, uint32_t n, const VkImageMemoryBarrier *images) {
    (void)memory; (void)buffers; struct proxy_resource *c = interop_command(command); if (!c) return; VkResult r = VK_ERROR_FEATURE_NOT_PRESENT; uint8_t args[464];
    if (flags || memory_count || buffer_count || n > 16 || (n && !images)) goto done;
    mb_put_u32(args, c->id); mb_put_u32(args + 4, src); mb_put_u32(args + 8, dst); mb_put_u32(args + 12, n);
    for (unsigned i = 0; i < n; ++i) { const VkImageMemoryBarrier *im = images + i; struct proxy_resource *o = submit_find(c->owner, (uintptr_t)im->image, PROXY_IMAGE); if (!o || im->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || im->pNext || !interop_color_range(&im->subresourceRange)) goto done; uint32_t fields[] = {o->id, im->srcAccessMask, im->dstAccessMask, im->oldLayout, im->newLayout, im->srcQueueFamilyIndex, im->dstQueueFamilyIndex}; for (unsigned j = 0; j < 7; ++j) mb_put_u32(args + 16 + i * 28 + j * 4, fields[j]); }
    r = renderer_rpc(c->owner, MB_RENDERER_BARRIER, args, 16 + n * 28, NULL, 0);
done: submit_void_error(c->owner, "vkCmdPipelineBarrier", r);
}
static void renderer_Clear(VkCommandBuffer command, VkImage image, VkImageLayout layout, const VkClearColorValue *color, uint32_t n, const VkImageSubresourceRange *ranges) {
    struct proxy_resource *c = interop_command(command); if (!c) return; struct proxy_resource *im = submit_find(c->owner, (uintptr_t)image, PROXY_IMAGE); VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (im && color && n == 1 && interop_color_range(ranges)) { uint8_t args[28]; mb_put_u32(args, c->id); mb_put_u32(args + 4, im->id); mb_put_u32(args + 8, layout); for (unsigned i = 0; i < 4; ++i) { uint32_t bits; memcpy(&bits, color->float32 + i, 4); mb_put_u32(args + 12 + i * 4, bits); } r = renderer_rpc(c->owner, MB_RENDERER_CLEAR, args, 28, NULL, 0); } submit_void_error(c->owner, "vkCmdClearColorImage", r);
}
static void renderer_Copy(VkCommandBuffer command, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t n, const VkBufferImageCopy *regions, uint32_t direction) {
    struct proxy_resource *c = interop_command(command); if (!c) return; struct proxy_resource *im = submit_find(c->owner, (uintptr_t)image, PROXY_IMAGE), *b = submit_find(c->owner, (uintptr_t)buffer, PROXY_BUFFER); VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (im && b && n == 1 && regions && (!regions->bufferRowLength || regions->bufferRowLength == regions->imageExtent.width) && !regions->bufferImageHeight && regions->imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && !regions->imageSubresource.mipLevel && !regions->imageSubresource.baseArrayLayer && regions->imageSubresource.layerCount == 1 && !regions->imageOffset.x && !regions->imageOffset.y && !regions->imageOffset.z) {
        uint8_t args[40]; uint32_t fields[] = {c->id, im->id, b->id, layout, direction}; for (unsigned i = 0; i < 5; ++i) mb_put_u32(args + i * 4, fields[i]); mb_put_u64(args + 20, regions->bufferOffset); mb_put_u32(args + 28, regions->imageExtent.width); mb_put_u32(args + 32, regions->imageExtent.height); mb_put_u32(args + 36, regions->imageExtent.depth); r = renderer_rpc(c->owner, MB_RENDERER_COPY, args, 40, NULL, 0);
    } submit_void_error(c->owner, "buffer/image renderer copy", r);
}
