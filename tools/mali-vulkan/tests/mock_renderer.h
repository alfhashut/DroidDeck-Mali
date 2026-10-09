/* HOST MOCK ONLY: model native renderer objects; actual ICD/broker execute their
 * production marshaling and validation. BLIT model runs only at GPU completion.
 */
static atomic_int renderer_created, renderer_destroyed, dispatches, shader_bytes;
struct mock_render {
    struct mock_logical *device; uint32_t kind, type, parent_count;
    uint64_t value;
    uint32_t colorspace, format, nearest;
    struct mock_storage *image;
    struct mock_render *parent, *children[24];
    struct { struct mock_render *view, *sampler; struct mock_storage *buffer; uint64_t offset, range; } descriptors[7][16];
};
static struct mock_render *mock_render_new(VkDevice device, uint32_t kind) {
    struct mock_render *o = calloc(1, sizeof(*o)); assert(o); o->device = (void *)device; o->kind = kind; ++renderer_created; return o;
}
static void mock_render_free(VkDevice device, struct mock_render *o, uint32_t kind) {
    assert(o && o->device == (void *)device && o->kind == kind); free(o); ++renderer_destroyed;
}
#define MOCK_RENDER_DESTROY(Name, Type, Kind) \
static void mock_Destroy##Name(VkDevice device, Type handle, const VkAllocationCallbacks *a) { assert(!a); mock_render_free(device, (void *)handle, Kind); }
MOCK_RENDER_DESTROY(Semaphore, VkSemaphore, MB_R_SEMAPHORE)
MOCK_RENDER_DESTROY(ImageView, VkImageView, MB_R_VIEW)
MOCK_RENDER_DESTROY(Sampler, VkSampler, MB_R_SAMPLER)
MOCK_RENDER_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, MB_R_SET_LAYOUT)
MOCK_RENDER_DESTROY(PipelineLayout, VkPipelineLayout, MB_R_PIPELINE_LAYOUT)
MOCK_RENDER_DESTROY(ShaderModule, VkShaderModule, MB_R_SHADER)
MOCK_RENDER_DESTROY(Pipeline, VkPipeline, MB_R_PIPELINE)
#undef MOCK_RENDER_DESTROY
static VkResult mock_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    assert(!a && !ci->flags && ci->pNext);
    const VkSemaphoreTypeCreateInfoKHR *t = ci->pNext; assert(t->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR && !t->pNext && t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE_KHR);
    struct mock_render *o = mock_render_new(device, MB_R_SEMAPHORE); o->value = t->initialValue; *out = (VkSemaphore)o; return VK_SUCCESS;
}
static VkResult mock_GetSemaphoreCounterValueKHR(VkDevice device, VkSemaphore semaphore, uint64_t *value) {
    struct mock_render *s = (void *)semaphore; assert(s->device == (void *)device && s->kind == MB_R_SEMAPHORE); *value = s->value; return VK_SUCCESS;
}
static VkResult mock_WaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfoKHR *info, uint64_t timeout) {
    struct mock_logical *d = (void *)device; assert(!info->pNext && !info->flags && info->semaphoreCount == 1 && timeout <= MB_SUBMIT_TIMEOUT_NS);
    struct mock_render *s = (void *)info->pSemaphores[0]; assert(s->device == d && s->kind == MB_R_SEMAPHORE);
    if (mode == 50) return VK_TIMEOUT;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->commands[i] && d->commands[i]->pending && d->commands[i]->timeline == s) mock_execute(d->commands[i]);
    return s->value >= info->pValues[0] ? VK_SUCCESS : VK_TIMEOUT;
}
static VkResult mock_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    assert(!a && !ci->flags); struct mock_storage *im = (void *)ci->image; assert(im->memory && im->device == (void *)device);
    assert(ci->viewType == (im->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : im->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D));
    if (mode == 51) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct mock_render *o = mock_render_new(device, MB_R_VIEW); o->image = im; o->type = ci->viewType; o->format = ci->format; *out = (VkImageView)o; return VK_SUCCESS;
}
static VkResult mock_CreateSampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    struct mock_render *o = mock_render_new(device, MB_R_SAMPLER); o->type = ci->unnormalizedCoordinates; o->nearest = ci->magFilter == VK_FILTER_NEAREST; *out = (VkSampler)o; return VK_SUCCESS;
}
static VkResult mock_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->bindingCount == 7);
    for (unsigned i = 0; i < 7; ++i) { assert(ci->pBindings[i].binding == i); if (i == 4) assert(ci->pBindings[i].pImmutableSamplers); }
    *out = (VkDescriptorSetLayout)mock_render_new(device, MB_R_SET_LAYOUT); return VK_SUCCESS;
}
static VkResult mock_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->setLayoutCount == 1 && !ci->pushConstantRangeCount);
    struct mock_render *o = mock_render_new(device, MB_R_PIPELINE_LAYOUT); o->parent = (void *)ci->pSetLayouts[0]; assert(o->parent->kind == MB_R_SET_LAYOUT && o->parent->device == o->device); *out = (VkPipelineLayout)o; return VK_SUCCESS;
}
static VkResult mock_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->maxSets <= 24 && ci->poolSizeCount == 3);
    *out = (VkDescriptorPool)mock_render_new(device, MB_R_POOL); return VK_SUCCESS;
}
static void mock_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *a) {
    assert(!a); struct mock_render *p = (void *)pool;
    for (unsigned i = 0; i < p->parent_count; ++i) mock_render_free(device, p->children[i], MB_R_SET);
    mock_render_free(device, p, MB_R_POOL);
}
static VkResult mock_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *out) {
    struct mock_render *pool = (void *)info->descriptorPool; assert(pool->device == (void *)device && pool->kind == MB_R_POOL && !info->pNext && info->descriptorSetCount <= 24 - pool->parent_count);
    for (unsigned i = 0; i < info->descriptorSetCount; ++i) { struct mock_render *set = mock_render_new(device, MB_R_SET); set->parent = (void *)info->pSetLayouts[i]; pool->children[pool->parent_count++] = set; out[i] = (VkDescriptorSet)set; } return VK_SUCCESS;
}
static void mock_UpdateDescriptorSets(VkDevice device, uint32_t n, const VkWriteDescriptorSet *writes, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)copy; assert(n == 7 && !copies);
    for (unsigned i = 0; i < n; ++i) {
        const VkWriteDescriptorSet *w = writes + i; struct mock_render *set = (void *)w->dstSet;
        assert(set->kind == MB_R_SET && set->device == (void *)device && w->dstBinding == i && !w->dstArrayElement);
        for (unsigned j = 0; j < w->descriptorCount; ++j) {
            if (!i) { set->descriptors[i][j].buffer = (void *)w->pBufferInfo[j].buffer; set->descriptors[i][j].offset = w->pBufferInfo[j].offset; set->descriptors[i][j].range = w->pBufferInfo[j].range; }
            else { struct mock_render *view = (void *)w->pImageInfo[j].imageView; assert(view && view->kind == MB_R_VIEW && view->device == set->device); set->descriptors[i][j].view = view; set->descriptors[i][j].sampler = (void *)w->pImageInfo[j].sampler; assert(w->pImageInfo[j].imageLayout == VK_IMAGE_LAYOUT_GENERAL); }
        }
    }
}
static VkResult mock_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->codeSize >= 20 && !(ci->codeSize % 4) && ci->pCode[0] == 0x07230203);
    if (mode == 48) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t hash = UINT64_C(14695981039346656037); const uint8_t *p = (void *)ci->pCode;
    for (unsigned i = 0; i < ci->codeSize; ++i) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    fprintf(stderr, "HOST shader transport exact bytes=%zu FNV64=%llu\n", ci->codeSize, (unsigned long long)hash);
    shader_bytes += (int)ci->codeSize; *out = (VkShaderModule)mock_render_new(device, MB_R_SHADER); return VK_SUCCESS;
}
static VkResult mock_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    assert(!cache && n == 1 && !a && !ci->pNext && !ci->flags && ci->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && !strcmp(ci->stage.pName, "main"));
    const VkSpecializationInfo *spec = ci->stage.pSpecializationInfo; assert(spec && spec->mapEntryCount == 7 && spec->dataSize == 28);
    const uint32_t *d = spec->pData; assert(d[0] == 1 && !d[1] && (d[4] == 1 || d[4] == 4) && d[5] == 2);
    if (mode == 49) return VK_ERROR_INITIALIZATION_FAILED;
    struct mock_render *o = mock_render_new(device, MB_R_PIPELINE); o->parent = (void *)ci->layout; o->colorspace = d[4]; *out = (VkPipeline)o; return VK_SUCCESS;
}
static void mock_CmdBindPipeline(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    struct mock_object *c = (void *)command; struct mock_render *p = (void *)pipeline; assert(c->state == 1 && point == VK_PIPELINE_BIND_POINT_COMPUTE && p->kind == MB_R_PIPELINE && p->device == c->device); c->pipeline = p;
}
static void mock_CmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t dynamic, const uint32_t *offsets) {
    (void)offsets; struct mock_object *c = (void *)command; struct mock_render *l = (void *)layout, *s = (void *)sets[0]; assert(c->state == 1 && point == VK_PIPELINE_BIND_POINT_COMPUTE && !first && n == 1 && !dynamic && l->parent == s->parent); c->descriptor = s;
}
static void mock_CmdDispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    struct mock_object *c = (void *)command; assert(c->state == 1 && c->pipeline && c->descriptor && x == 32 && y == 32 && z == 1 && c->op_count < 64);
    c->ops[c->op_count++] = (struct mock_op){.kind = 4, .renderer_set = c->descriptor, .pattern = ((struct mock_render *)c->pipeline)->colorspace}; ++dispatches;
}
static void mock_renderer_execute(struct mock_op *op) {
    struct mock_render *set = op->renderer_set; assert(set && set->kind == MB_R_SET);
    struct mock_storage *uniform = set->descriptors[0][0].buffer, *src = set->descriptors[3][0].view->image, *dst = set->descriptors[1][0].view->image;
    assert(src != dst && dst->external && src->width == 256 && src->height == 256 && dst->width == 256 && dst->height == 256);
    const uint8_t *data = uniform->memory->gpu + uniform->offset + set->descriptors[0][0].offset;
    float scale[2], offset[2]; memcpy(scale, data, 8); memcpy(offset, data + 8 * 8, 8);
    assert(scale[0] == 2 && scale[1] == 2 && offset[0] == -63.75f && offset[1] == -63.75f);
    assert(set->descriptors[5][0].view->type == VK_IMAGE_VIEW_TYPE_1D && set->descriptors[6][0].view->type == VK_IMAGE_VIEW_TYPE_3D);
    assert(set->descriptors[3][0].sampler->type && set->descriptors[3][0].sampler->nearest);
    assert(set->descriptors[3][0].view->format == VK_FORMAT_R8G8B8A8_UNORM && set->descriptors[1][0].view->format == VK_FORMAT_R8G8B8A8_UNORM);
    for (unsigned j = 1; j < 16; ++j) assert(set->descriptors[3][j].view->image->width == 1 && set->descriptors[3][j].view->image != src);
    assert(set->descriptors[2][0].view->image != dst && !set->descriptors[2][0].view->image->external);
    float opacity, ctm[12]; uint32_t border, filter, alpha, rotation;
    memcpy(&opacity, data + 128, 4); memcpy(ctm, data + 160, sizeof(ctm));
    memcpy(&border, data + 544, 4); memcpy(&filter, data + 556, 4);
    memcpy(&alpha, data + 560, 4); memcpy(&rotation, data + 580, 4);
    assert(set->descriptors[0][0].range == 584 && opacity == 1 && border == 1 && filter == 1 && !alpha && !rotation);
    for (unsigned j = 0; j < 12; ++j) assert(ctm[j] == (j == 0 || j == 5 || j == 10 ? 1.0f : 0.0f));
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) {
        uint8_t *p = dst->memory->gpu + dst->offset + (y * 256 + x) * 4;
        float sx = (x + offset[0]) * scale[0], sy = (y + offset[1]) * scale[1];
        if (sx < 0 || sy < 0 || sx >= 256 || sy >= 256) { p[0] = p[1] = p[2] = 0; p[3] = 255; }
        else {
            memcpy(p, src->memory->gpu + src->offset + (((unsigned)sy * 256 + (unsigned)sx) * 4), 4);
            /* Match composite.h: valid dummy LUT has one mip even when output
             * color management is disabled. Only PASSTHRU skips this branch. */
            if (op->pattern != 4) {
                struct mock_storage *lut = set->descriptors[6][0].view->image;
                memcpy(p, lut->memory->gpu + lut->offset, 3);
            }
        }
    }
    if (mode == 53) dst->memory->gpu[0] = 99;
}
static VkResult mock_ResetCommandBuffer(VkCommandBuffer command, VkCommandBufferResetFlags flags) {
    struct mock_object *c = (void *)command; assert(!flags && !c->pending);
    c->state = 0; c->op_count = 0; c->fence = NULL; c->pipeline = c->descriptor = c->timeline = NULL; return VK_SUCCESS;
}
