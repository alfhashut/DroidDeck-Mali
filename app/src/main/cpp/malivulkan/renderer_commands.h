/* Explicit v7 Gamescope compute subset. All driver operations are real. */
static struct native_renderer_object *renderer_find(struct native_device *d, uint32_t id, uint32_t kind) {
    if (!id || !d->renderer.objects) return NULL;
    for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i)
        if (d->renderer.objects[i].id == id && d->renderer.objects[i].kind == kind) return &d->renderer.objects[i];
    return NULL;
}
static int native_renderer_init(struct native_device *d) {
#define MB_RENDERER_ENTRY(n) d->renderer.n = (PFN_vk##n)d->gdpa(d->handle, "vk" #n); if (!d->renderer.n) return -1;
#include "renderer_entries.def"
#undef MB_RENDERER_ENTRY
    d->renderer.objects = calloc(MB_RENDERER_MAX_OBJECTS, sizeof(*d->renderer.objects));
    if (!d->renderer.objects) return -1;
    d->renderer.enabled = 1; return 0;
}
static int native_renderer_image_referenced(struct native_device *d, uint32_t id) {
    if (!d->renderer.objects) return 0;
    for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i)
        if (d->renderer.objects[i].id && d->renderer.objects[i].kind == MB_R_VIEW && d->renderer.objects[i].image == id) return 1;
    return 0;
}
static int renderer_image_state(struct native_device *d, struct native_command *c, uint32_t id) {
    struct native_image *im = interop_find_image(d, id);
    if (!im || !im->memory) return -1;
    for (unsigned i = 0; i < c->renderer_image_count; ++i) if (c->renderer_images[i].id == id) return (int)i;
    if (c->renderer_image_count >= 16 || interop_ref(c, id)) return -1;
    unsigned i = c->renderer_image_count++;
    c->renderer_images[i].id = id; c->renderer_images[i].initial = im->layout;
    c->renderer_images[i].final = im->layout; c->renderer_images[i].initial_foreign = im->foreign; c->renderer_images[i].foreign = im->foreign;
    c->renderer = 1; return (int)i;
}
static void native_renderer_complete(struct native_device *d, struct native_command *c) {
    for (unsigned i = 0; i < c->renderer_image_count; ++i) {
        struct native_image *im = interop_find_image(d, c->renderer_images[i].id);
        if (im) { im->layout = c->renderer_images[i].final; im->foreign = c->renderer_images[i].foreign; }
    }
}
static int renderer_descriptors_live(struct native_device *d, struct native_command *c) {
    if (!c->pipeline && !c->descriptor_set) return 1;
    struct native_renderer_object *p = renderer_find(d, c->pipeline, MB_R_PIPELINE);
    struct native_renderer_object *set = renderer_find(d, c->descriptor_set, MB_R_SET);
    struct native_renderer_object *l = p ? renderer_find(d, p->parent, MB_R_PIPELINE_LAYOUT) : NULL;
    if (!p || !set || !l || !renderer_find(d, l->parent, MB_R_SET_LAYOUT) || l->parent != set->type || set->descriptor_count != 39 || c->descriptor_revision != set->descriptor_revision) return 0;
    for (unsigned i = 0; i < set->descriptor_count; ++i) {
        if (set->descriptors[i].buffer) { if (!interop_find_buffer(d, set->descriptors[i].buffer)) return 0; }
        else if (!renderer_find(d, set->descriptors[i].view, MB_R_VIEW) || (set->descriptors[i].sampler && !renderer_find(d, set->descriptors[i].sampler, MB_R_SAMPLER))) return 0;
    }
    return 1;
}
static int renderer_has_children(struct native_device *d, struct native_renderer_object *o) {
    for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i) {
        struct native_renderer_object *child = &d->renderer.objects[i];
        if (!child->id) continue;
        if (o->kind == MB_R_PIPELINE_LAYOUT && child->kind == MB_R_PIPELINE && child->parent == o->id) return 1;
        if (o->kind == MB_R_SET_LAYOUT && ((child->kind == MB_R_PIPELINE_LAYOUT && child->parent == o->id) || (child->kind == MB_R_SET && child->type == o->id))) return 1;
        if (o->kind == MB_R_SAMPLER && child->kind == MB_R_SET_LAYOUT) for (unsigned j = 0; j < 7; ++j) if (child->bindings[j].sampler == o->id) return 1;
    }
    return 0;
}
static int native_renderer_can_submit(struct native_device *d, struct native_command *c) {
    if (d->session.release_timeouts) return 0;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->interop.ahbs[i].id && interop_ahb_held(d, &d->interop.ahbs[i]))
            for (unsigned j = 0; j < c->renderer_image_count; ++j)
                if (c->renderer_images[j].id == d->interop.ahbs[i].image) return 0;
    if (!renderer_descriptors_live(d, c)) return 0;
    for (unsigned i = 0; i < c->renderer_image_count; ++i) {
        struct native_image *im = interop_find_image(d, c->renderer_images[i].id);
        if (!im || im->layout != c->renderer_images[i].initial || im->foreign != c->renderer_images[i].initial_foreign) return 0;
    }
    for (unsigned i = 0; i < c->ref_count; ++i) {
        struct native_buffer *b = interop_find_buffer(d, c->refs[i]);
        struct native_image *im = interop_find_image(d, c->refs[i]);
        struct native_memory *m = interop_find_memory(d, b ? b->memory : im ? im->memory : 0);
        if ((!b && !im) || !m || interop_referenced(d, c->refs[i], 1)) return 0;
        /* The ICD synchronously copies mapped coherent mirrors before submission. */
        if (m->map && !(d->interop.properties.memoryTypes[m->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return 0;
    }
    return 1;
}
static void renderer_destroy_object(struct native_device *d, struct native_renderer_object *o) {
    struct native_renderer *v = &d->renderer;
    switch (o->kind) {
    case MB_R_SEMAPHORE: v->DestroySemaphore(d->handle, o->handle.semaphore, NULL); break;
    case MB_R_VIEW: v->DestroyImageView(d->handle, o->handle.view, NULL); break;
    case MB_R_SAMPLER: v->DestroySampler(d->handle, o->handle.sampler, NULL); break;
    case MB_R_SET_LAYOUT: v->DestroyDescriptorSetLayout(d->handle, o->handle.set_layout, NULL); break;
    case MB_R_PIPELINE_LAYOUT: v->DestroyPipelineLayout(d->handle, o->handle.pipeline_layout, NULL); break;
    case MB_R_POOL:
        v->DestroyDescriptorPool(d->handle, o->handle.pool, NULL);
        for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i)
            if (v->objects[i].id && v->objects[i].kind == MB_R_SET && v->objects[i].parent == o->id) memset(&v->objects[i], 0, sizeof(v->objects[i]));
        break;
    case MB_R_SHADER: v->DestroyShaderModule(d->handle, o->handle.shader, NULL); break;
    case MB_R_PIPELINE: v->DestroyPipeline(d->handle, o->handle.pipeline, NULL); break;
    default: break;
    }
    memset(o, 0, sizeof(*o));
}
static void native_renderer_close(struct native_device *d) {
    if (!d->renderer.objects) return;
    const unsigned order[] = {MB_R_PIPELINE, MB_R_SHADER, MB_R_POOL, MB_R_PIPELINE_LAYOUT, MB_R_SET_LAYOUT, MB_R_VIEW, MB_R_SAMPLER, MB_R_SEMAPHORE};
    for (unsigned k = 0; k < sizeof(order)/sizeof(*order); ++k)
        for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i)
            if (d->renderer.objects[i].id && d->renderer.objects[i].kind == order[k]) renderer_destroy_object(d, &d->renderer.objects[i]);
    free(d->renderer.objects); d->renderer.objects = NULL;
}
static uint32_t native_renderer_command(struct vk_session *s, uint32_t op, const uint8_t *w, uint32_t bytes,
        uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    if (bytes < 4) return MB_PROTOCOL_ERROR;
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i) if (s->logical[i].handle && s->logical[i].id == mb_get_u32(w)) d = &s->logical[i];
    if (!d || !d->renderer.enabled) return MB_PROTOCOL_ERROR;
    struct native_renderer *v = &d->renderer;
    if (d->submit.lost && op != MB_RENDERER_DESTROY) { *result = VK_ERROR_DEVICE_LOST; return MB_VULKAN_ERROR; }
    uint32_t id = bytes >= 8 ? mb_get_u32(w + 4) : 0;
    struct native_renderer_object *o = NULL;
    int creating = op == MB_RENDERER_SEMAPHORE_CREATE || (op >= MB_RENDERER_VIEW && op <= MB_RENDERER_POOL) || op == MB_RENDERER_SHADER || op == MB_RENDERER_PIPELINE;
    if (creating) {
        if (s->next_resource_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
        for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i) if (!v->objects[i].id) { o = &v->objects[i]; break; }
        if (!o) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
        memset(o, 0, sizeof(*o));
    }
#define SIZE(n) do { if (bytes != (uint32_t)(n)) return MB_PROTOCOL_ERROR; } while (0)
#define REQUIRE(x) do { if (!(x)) return MB_PROTOCOL_ERROR; } while (0)
    switch (op) {
    case MB_RENDERER_SEMAPHORE_CREATE: {
        SIZE(12);
        VkSemaphoreTypeCreateInfoKHR type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR, .initialValue = mb_get_u64(w + 4)};
        VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        *result = v->CreateSemaphore(d->handle, &ci, NULL, &o->handle.semaphore); o->kind = MB_R_SEMAPHORE; o->last_signal = type.initialValue; break;
    }
    case MB_RENDERER_DESTROY: {
        SIZE(12); o = renderer_find(d, mb_get_u32(w + 8), id); REQUIRE(o && id != MB_R_SET);
        REQUIRE((d->submit.lost || !native_pending(d, 0, 0, 0)) && !renderer_has_children(d, o));
        renderer_destroy_object(d, o); o = NULL; break;
    }
    case MB_RENDERER_COUNTER: case MB_RENDERER_WAIT: {
        SIZE(op == MB_RENDERER_COUNTER ? 8 : 24);
        struct native_renderer_object *sem = renderer_find(d, id, MB_R_SEMAPHORE); REQUIRE(sem);
        uint64_t value = 0;
        if (op == MB_RENDERER_COUNTER) {
            *result = v->GetSemaphoreCounterValueKHR(d->handle, sem->handle.semaphore, &value);
            if (*result == VK_SUCCESS) { mb_put_u64(reply, value); *extra = 8; *count = 1; }
        } else {
            value = mb_get_u64(w + 8); uint64_t timeout = mb_get_u64(w + 16); REQUIRE(timeout <= MB_SUBMIT_TIMEOUT_NS);
            VkSemaphoreWaitInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR, .semaphoreCount = 1, .pSemaphores = &sem->handle.semaphore, .pValues = &value};
            *result = v->WaitSemaphoresKHR(d->handle, &info, timeout);
        }
        if (*result == VK_SUCCESS) for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
            struct native_command *c = &d->submit.commands[i];
            if (c->id && c->state == 3 && c->semaphore == id && c->signal_value <= value) { native_renderer_complete(d, c); c->state = 4; }
        }
        break;
    }
    case MB_RENDERER_VIEW: {
        SIZE(20); struct native_image *im = interop_find_image(d, id); REQUIRE(im && im->memory);
        uint32_t type = mb_get_u32(w + 8), format = mb_get_u32(w + 12), usage = mb_get_u32(w + 16);
        uint32_t expected = im->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : im->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        REQUIRE(type == expected && (format == VK_FORMAT_R8G8B8A8_UNORM || (format == VK_FORMAT_R8G8B8A8_SRGB && (im->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))) && (!usage || !(usage & ~im->usage)));
        VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
        VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = usage ? &vu : NULL,
            .image = im->handle, .viewType = (VkImageViewType)type, .format = (VkFormat)format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        *result = v->CreateImageView(d->handle, &ci, NULL, &o->handle.view); o->kind = MB_R_VIEW; o->image = id; o->type = type; o->usage = usage ? usage : im->usage; o->format = format; break;
    }
    case MB_RENDERER_SAMPLER: {
        SIZE(12); REQUIRE(id <= 1 && mb_get_u32(w + 8) <= 1);
        VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = id ? VK_FILTER_NEAREST : VK_FILTER_LINEAR, .minFilter = id ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = mb_get_u32(w + 8)};
        *result = v->CreateSampler(d->handle, &ci, NULL, &o->handle.sampler); o->kind = MB_R_SAMPLER; o->type = ci.unnormalizedCoordinates; break;
    }
    case MB_RENDERER_SET_LAYOUT: {
        REQUIRE(id == 7); SIZE(8 + id * 16); VkDescriptorSetLayoutBinding b[7] = {0}; VkSampler immutable[16];
        for (unsigned i = 0; i < id; ++i) {
            const uint8_t *a = w + 8 + i * 16;
            uint32_t t = mb_get_u32(a + 4), n = mb_get_u32(a + 8), sid = mb_get_u32(a + 12);
            REQUIRE(mb_get_u32(a) == i && t == (i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            REQUIRE(n == (i < 3 ? 1u : i < 5 ? 16u : 2u) && (!sid || i == 4));
            b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = (VkDescriptorType)t, .descriptorCount = n, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
            if (sid) { struct native_renderer_object *sam = renderer_find(d, sid, MB_R_SAMPLER); REQUIRE(sam && !sam->type); for (unsigned j = 0; j < n; ++j) immutable[j] = sam->handle.sampler; b[i].pImmutableSamplers = immutable; }
            o->bindings[i].binding = i; o->bindings[i].type = t; o->bindings[i].count = n; o->bindings[i].sampler = sid;
        }
        VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = id, .pBindings = b};
        *result = v->CreateDescriptorSetLayout(d->handle, &ci, NULL, &o->handle.set_layout); o->kind = MB_R_SET_LAYOUT; break;
    }
    case MB_RENDERER_PIPELINE_LAYOUT: {
        SIZE(8); struct native_renderer_object *l = renderer_find(d, id, MB_R_SET_LAYOUT); REQUIRE(l);
        const VkPhysicalDeviceLimits *limits = &v->limits;
        if (limits->maxBoundDescriptorSets < 1 || limits->maxPerStageDescriptorSamplers < 36 || limits->maxPerStageDescriptorSampledImages < 36 || limits->maxDescriptorSetSamplers < 36 || limits->maxDescriptorSetSampledImages < 36 || limits->maxPerStageDescriptorStorageImages < 2 || limits->maxDescriptorSetStorageImages < 2 || limits->maxPerStageDescriptorUniformBuffers < 1 || limits->maxDescriptorSetUniformBuffers < 1 || limits->maxPerStageResources < 39) {
            ERROR("required native descriptor limits: 36 samplers/images, 2 storage images, 1 UBO, 39 resources"); *result = VK_ERROR_FEATURE_NOT_PRESENT; break;
        }
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &l->handle.set_layout};
        *result = v->CreatePipelineLayout(d->handle, &ci, NULL, &o->handle.pipeline_layout); o->kind = MB_R_PIPELINE_LAYOUT; o->parent = id; break;
    }
    case MB_RENDERER_POOL: {
        REQUIRE(bytes >= 12); uint32_t n = mb_get_u32(w + 8); REQUIRE(id && id <= 24 && n == 3); SIZE(12 + n * 8);
        VkDescriptorPoolSize sizes[3];
        for (unsigned i = 0; i < 3; ++i) { sizes[i].type = (VkDescriptorType)mb_get_u32(w + 12 + i * 8); sizes[i].descriptorCount = mb_get_u32(w + 16 + i * 8); REQUIRE(sizes[i].type == (i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && sizes[i].descriptorCount <= 4096 && sizes[i].descriptorCount); }
        VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = id, .poolSizeCount = n, .pPoolSizes = sizes};
        *result = v->CreateDescriptorPool(d->handle, &ci, NULL, &o->handle.pool); o->kind = MB_R_POOL; o->count = id; break;
    }
    case MB_RENDERER_SETS: {
        REQUIRE(bytes >= 12); uint32_t n = mb_get_u32(w + 8); REQUIRE(n && n <= 24); SIZE(12 + n * 4);
        struct native_renderer_object *pool = renderer_find(d, id, MB_R_POOL); REQUIRE(pool && n <= pool->count);
        VkDescriptorSetLayout layouts[24]; VkDescriptorSet sets[24]; unsigned slots[24], found = 0;
        for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS && found < n; ++i) if (!v->objects[i].id) slots[found++] = i;
        if (found != n || s->next_resource_id > UINT32_MAX - n) { *result = VK_ERROR_TOO_MANY_OBJECTS; break; }
        for (unsigned i = 0; i < n; ++i) { struct native_renderer_object *l = renderer_find(d, mb_get_u32(w + 12 + i * 4), MB_R_SET_LAYOUT); REQUIRE(l); layouts[i] = l->handle.set_layout; }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool->handle.pool, .descriptorSetCount = n, .pSetLayouts = layouts};
        *result = v->AllocateDescriptorSets(d->handle, &ai, sets);
        if (*result == VK_SUCCESS) { for (unsigned i = 0; i < n; ++i) { struct native_renderer_object *set = &v->objects[slots[i]]; memset(set, 0, sizeof(*set)); set->id = ++s->next_resource_id; set->kind = MB_R_SET; set->parent = id; set->type = mb_get_u32(w + 12 + i * 4); set->handle.set = sets[i]; mb_put_u32(reply + i * 4, set->id); } *extra = n * 4; *count = 1; pool->count -= n; }
        break;
    }
    case MB_RENDERER_SHADER: {
        REQUIRE(id >= 20 && id <= MB_RENDERER_MAX_SHADER && !(id % 4)); SIZE(8 + id);
        REQUIRE(mb_get_u32(w + 8) == 0x07230203 && mb_get_u32(w + 12) >= 0x00010000 && mb_get_u32(w + 12) <= 0x00010300 && !(mb_get_u32(w + 12) & 0xff) && mb_get_u32(w + 20) && !mb_get_u32(w + 24));
        uint32_t *words = malloc(id); if (!words) { *result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        for (unsigned i = 0; i < id / 4; ++i) words[i] = mb_get_u32(w + 8 + i * 4);
        VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = id, .pCode = words};
        *result = v->CreateShaderModule(d->handle, &ci, NULL, &o->handle.shader); free(words); o->kind = MB_R_SHADER; break;
    }
    case MB_RENDERER_PIPELINE: {
        SIZE(40); struct native_renderer_object *shader = renderer_find(d, id, MB_R_SHADER), *layout = renderer_find(d, mb_get_u32(w + 8), MB_R_PIPELINE_LAYOUT); REQUIRE(shader && layout);
        uint32_t data[7]; VkSpecializationMapEntry entries[7]; for (unsigned i = 0; i < 7; ++i) { data[i] = mb_get_u32(w + 12 + i * 4); entries[i] = (VkSpecializationMapEntry){i, i * 4, 4}; }
        REQUIRE(data[0] == 1 && !data[1] && !data[2] && !data[3] && (data[4] == 1 || data[4] == 4) && data[5] == 2 && !data[6]);
        VkSpecializationInfo spec = {.mapEntryCount = 7, .pMapEntries = entries, .dataSize = sizeof(data), .pData = data};
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader->handle.shader, .pName = "main", .pSpecializationInfo = &spec}, .layout = layout->handle.pipeline_layout};
        if (d->interop.ahb_enabled && native_verbose(d)) LOG("BLIT native specialization: layer count=%u ycbcrMask=%u debug=%u blur layers=%u colorspaceMask=%u output EOTF=%u ITM=%u", data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
        *result = v->CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &o->handle.pipeline); o->kind = MB_R_PIPELINE; o->parent = layout->id; break;
    }
    case MB_RENDERER_UPDATE: {
        REQUIRE(bytes >= 8 && id == 7 && !native_pending(d, 0, 0, 0));
        VkWriteDescriptorSet writes[7] = {0}; VkDescriptorImageInfo images[64] = {0}; VkDescriptorBufferInfo buffer = {0};
        struct native_renderer_object staged = {0}, *set = NULL;
        unsigned at = 8, image_at = 0;
        for (unsigned i = 0; i < id; ++i) {
            REQUIRE(at <= bytes && bytes - at >= 20);
            uint32_t set_id = mb_get_u32(w + at), binding = mb_get_u32(w + at + 4), element = mb_get_u32(w + at + 8), type = mb_get_u32(w + at + 12), n = mb_get_u32(w + at + 16); at += 20;
            struct native_renderer_object *current = renderer_find(d, set_id, MB_R_SET); REQUIRE(current && (!set || set == current)); set = current;
            struct native_renderer_object *l = renderer_find(d, set->type, MB_R_SET_LAYOUT); REQUIRE(l && binding == i && !element && n == l->bindings[i].count && type == l->bindings[i].type);
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set->handle.set, .dstBinding = binding, .descriptorCount = n, .descriptorType = (VkDescriptorType)type};
            if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                REQUIRE(bytes - at >= 20); uint32_t bid = mb_get_u32(w + at); uint64_t offset = mb_get_u64(w + at + 4), range = mb_get_u64(w + at + 12); at += 20;
                struct native_buffer *b = interop_find_buffer(d, bid); REQUIRE(b && b->memory && (b->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
                if (range == VK_WHOLE_SIZE) range = b->size - offset;
                REQUIRE(v->limits.minUniformBufferOffsetAlignment && !(offset % v->limits.minUniformBufferOffsetAlignment) && interop_range(b->size, offset, range) && range <= v->limits.maxUniformBufferRange);
                buffer = (VkDescriptorBufferInfo){b->handle, offset, range}; writes[i].pBufferInfo = &buffer;
                staged.descriptors[staged.descriptor_count].buffer = bid; staged.descriptors[staged.descriptor_count].offset = offset; staged.descriptors[staged.descriptor_count].range = range;
                staged.descriptors[staged.descriptor_count].type = type; staged.descriptors[staged.descriptor_count++].binding = binding;
            } else {
                REQUIRE(n <= 64 - image_at && n <= (bytes - at)/12); writes[i].pImageInfo = images + image_at;
                for (unsigned j = 0; j < n; ++j) {
                    uint32_t sid = mb_get_u32(w + at), vid = mb_get_u32(w + at + 4), layout = mb_get_u32(w + at + 8); at += 12;
                    struct native_renderer_object *view = renderer_find(d, vid, MB_R_VIEW); REQUIRE(view);
                    struct native_image *im = interop_find_image(d, view->image); REQUIRE(im && im->memory && layout == VK_IMAGE_LAYOUT_GENERAL);
                    struct native_renderer_object *sampler = sid ? renderer_find(d, sid, MB_R_SAMPLER) : NULL;
                    uint32_t expected = binding == 5 ? VK_IMAGE_VIEW_TYPE_1D : binding == 6 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
                    REQUIRE(view->type == expected && (view->usage & (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT)) && (type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || view->format == VK_FORMAT_R8G8B8A8_UNORM));
                    if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                        if (l->bindings[i].sampler) { sampler = renderer_find(d, l->bindings[i].sampler, MB_R_SAMPLER); REQUIRE(sampler); }
                        REQUIRE(sampler && (expected == VK_IMAGE_VIEW_TYPE_2D || !sampler->type));
                    } else REQUIRE(!sid);
                    images[image_at++] = (VkDescriptorImageInfo){sampler ? sampler->handle.sampler : VK_NULL_HANDLE, view->handle.view, (VkImageLayout)layout};
                    if (d->interop.ahb_enabled && native_verbose(d) && j == 0 && (binding == 1 || binding == 3))
                        LOG("BLIT native descriptor set ID=%u binding=%u array index=%u image broker ID=%u image-view broker ID=%u sampler broker ID=%u viewType=%u format=%u usage=0x%x layout=%u AHB=%u",
                            set->id, binding, j, im->id, view->id, sampler ? sampler->id : 0, view->type, view->format, view->usage, layout, im->ahb);
                    unsigned k = staged.descriptor_count++; staged.descriptors[k].binding = binding; staged.descriptors[k].element = j; staged.descriptors[k].type = type; staged.descriptors[k].view = vid; staged.descriptors[k].sampler = sampler ? sampler->id : 0;
                }
            }
        }
        REQUIRE(at == bytes && set && set->descriptor_revision != UINT64_MAX);
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) REQUIRE(!d->submit.commands[i].id || d->submit.commands[i].descriptor_set != set->id || d->submit.commands[i].state == 0 || d->submit.commands[i].state == 4);
        v->UpdateDescriptorSets(d->handle, 7, writes, 0, NULL);
        memcpy(set->descriptors, staged.descriptors, sizeof(set->descriptors)); set->descriptor_count = staged.descriptor_count; ++set->descriptor_revision; break;
    }
    case MB_RENDERER_BIND_PIPELINE: case MB_RENDERER_BIND_SET: case MB_RENDERER_DISPATCH: {
        SIZE(op == MB_RENDERER_BIND_PIPELINE ? 12 : op == MB_RENDERER_BIND_SET ? 16 : 20);
        struct native_command *c = native_find_command(d, id); REQUIRE(c && c->state == 1); c->renderer = 1;
        if (op == MB_RENDERER_BIND_PIPELINE) {
            struct native_renderer_object *p = renderer_find(d, mb_get_u32(w + 8), MB_R_PIPELINE); REQUIRE(p);
            v->CmdBindPipeline(c->handle, VK_PIPELINE_BIND_POINT_COMPUTE, p->handle.pipeline); c->pipeline = p->id;
        } else if (op == MB_RENDERER_BIND_SET) {
            struct native_renderer_object *l = renderer_find(d, mb_get_u32(w + 8), MB_R_PIPELINE_LAYOUT), *set = renderer_find(d, mb_get_u32(w + 12), MB_R_SET);
            REQUIRE(l && set && set->type == l->parent);
            v->CmdBindDescriptorSets(c->handle, VK_PIPELINE_BIND_POINT_COMPUTE, l->handle.pipeline_layout, 0, 1, &set->handle.set, 0, NULL); c->descriptor_set = set->id;
        } else {
            struct native_renderer_object *p = renderer_find(d, c->pipeline, MB_R_PIPELINE), *set = renderer_find(d, c->descriptor_set, MB_R_SET);
            struct native_renderer_object *l = p ? renderer_find(d, p->parent, MB_R_PIPELINE_LAYOUT) : NULL;
            REQUIRE(p && set && l && l->parent == set->type && set->descriptor_count == 39);
            uint32_t x = mb_get_u32(w + 8), y = mb_get_u32(w + 12), z = mb_get_u32(w + 16); REQUIRE(x && y && x <= (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION / 8 : 32u) && y <= (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION / 8 : 32u) && z == 1 && x <= v->limits.maxComputeWorkGroupCount[0] && y <= v->limits.maxComputeWorkGroupCount[1]);
            if (v->limits.maxComputeWorkGroupInvocations < 64 || v->limits.maxComputeWorkGroupSize[0] < 8 || v->limits.maxComputeWorkGroupSize[1] < 8) { *result = VK_ERROR_FEATURE_NOT_PRESENT; break; }
            for (unsigned i = 0; i < set->descriptor_count; ++i) {
                if (set->descriptors[i].buffer) { REQUIRE(!interop_ref(c, set->descriptors[i].buffer)); continue; }
                struct native_renderer_object *view = renderer_find(d, set->descriptors[i].view, MB_R_VIEW); REQUIRE(view);
                int j = renderer_image_state(d, c, view->image); REQUIRE(j >= 0 && !c->renderer_images[j].foreign && c->renderer_images[j].final == VK_IMAGE_LAYOUT_GENERAL);
                struct native_image *im = interop_find_image(d, view->image);
                if (set->descriptors[i].binding == 1) { REQUIRE(x * 8 >= im->width && y * 8 >= im->height); if (im->ahb) c->image_id = im->id; }
            }
            c->descriptor_revision = set->descriptor_revision;
            if (d->interop.ahb_enabled && native_verbose(d)) LOG("BLIT native before dispatch: pipeline ID=%u descriptor set ID=%u command ID=%u final AHB image broker ID=%u dispatch X/Y/Z=%u/%u/%u; all sampled/storage layouts GENERAL, queue owned", p->id, set->id, c->id, c->image_id, x, y, z);
            v->CmdDispatch(c->handle, x, y, z);
        }
        ++c->recorded; break;
    }
    case MB_RENDERER_SUBMIT: {
        SIZE(28); VkQueue queue = VK_NULL_HANDLE; for (unsigned i = 0; i < d->count; ++i) if (d->queue_ids[i] == id && id) queue = d->queues[i];
        struct native_command *c = native_find_command(d, mb_get_u32(w + 8));
        uint32_t fid = mb_get_u32(w + 12); struct native_fence *f = native_find_fence(d, fid);
        struct native_renderer_object *sem = renderer_find(d, mb_get_u32(w + 16), MB_R_SEMAPHORE); uint64_t value = mb_get_u64(w + 20);
        REQUIRE(queue && c && c->renderer && c->state == 2 && c->recorded && c->family == d->family && sem && value > sem->last_signal && (!fid || (f && !f->submitted)) && native_renderer_can_submit(d, c));
        VkTimelineSemaphoreSubmitInfoKHR timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline, .commandBufferCount = 1, .pCommandBuffers = &c->handle, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem->handle.semaphore};
        *result = d->submit.QueueSubmit(queue, 1, &si, f ? f->handle : VK_NULL_HANDLE);
        if (*result == VK_SUCCESS) { c->state = 3; c->semaphore = sem->id; c->signal_value = value; sem->last_signal = value; c->fence = fid; if (f) f->submitted = 1; }
        break;
    }
    case MB_RENDERER_RESET_COMMAND: {
        SIZE(8); struct native_command *c = native_find_command(d, id); REQUIRE(c && c->state != 3 && c->state != 1);
        *result = v->ResetCommandBuffer(c->handle, 0);
        if (*result == VK_SUCCESS) { uint32_t pool = c->pool, family = c->family; VkCommandBuffer handle = c->handle; memset(c, 0, sizeof(*c)); c->id = id; c->pool = pool; c->family = family; c->handle = handle; }
        break;
    }
    case MB_RENDERER_IMAGE: {
        REQUIRE(bytes >= 32); uint32_t width = mb_get_u32(w + 8), height = mb_get_u32(w + 12), depth = mb_get_u32(w + 16), usage = mb_get_u32(w + 20), flags = mb_get_u32(w + 24), n = mb_get_u32(w + 28);
        REQUIRE(id <= VK_IMAGE_TYPE_3D && width && height && depth && width <= (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) && height <= (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) && depth <= 1 && (id != VK_IMAGE_TYPE_1D || height == 1) && usage && !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) && !(flags & ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && n <= 2);
        SIZE(32 + n * 4); VkFormat formats[2]; for (unsigned i = 0; i < n; ++i) { formats[i] = (VkFormat)mb_get_u32(w + 32 + i * 4); REQUIRE(formats[i] == (i ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM)); }
        REQUIRE((flags && n == 2) || (!flags && !n));
        struct native_image *im = NULL; for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (!d->interop.images[i].id) { im = &d->interop.images[i]; break; }
        if (!im || s->next_resource_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; break; }
        VkImageFormatListCreateInfoKHR list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR, .viewFormatCount = n, .pViewFormats = formats};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = n ? &list : NULL, .flags = flags, .imageType = (VkImageType)id,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {width, height, depth}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        *result = d->interop.CreateImage(d->handle, &ci, NULL, &im->handle);
        if (*result == VK_SUCCESS) { im->id = ++s->next_resource_id; im->width = width; im->height = height; im->depth = depth; im->type = id; im->usage = usage; im->flags = flags; d->interop.GetImageMemoryRequirements(d->handle, im->handle, &im->req); mb_put_u32(reply, im->id); *extra = 4; *count = 1; }
        break;
    }
    case MB_RENDERER_BARRIER: {
        REQUIRE(bytes >= 20); uint32_t src = mb_get_u32(w + 8), dst = mb_get_u32(w + 12), n = mb_get_u32(w + 16); REQUIRE(n <= 16); SIZE(20 + n * 28);
        const uint32_t stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        REQUIRE(src && dst && !(src & ~stages) && !(dst & ~stages));
        struct native_command *c = native_find_command(d, id); REQUIRE(c && c->state == 1); c->renderer = 1;
        VkImageMemoryBarrier barriers[16] = {0};
        const uint32_t accesses = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        for (unsigned i = 0; i < n; ++i) {
            const uint8_t *a = w + 20 + i * 28; uint32_t iid = mb_get_u32(a), sa = mb_get_u32(a + 4), da = mb_get_u32(a + 8), old = mb_get_u32(a + 12), next = mb_get_u32(a + 16), sf = mb_get_u32(a + 20), df = mb_get_u32(a + 24);
            struct native_image *im = interop_find_image(d, iid); int j = renderer_image_state(d, c, iid); REQUIRE(im && j >= 0 && !(sa & ~accesses) && !(da & ~accesses));
            REQUIRE((old == VK_IMAGE_LAYOUT_UNDEFINED || old == (uint32_t)c->renderer_images[j].final) && (next == VK_IMAGE_LAYOUT_GENERAL || next == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
            int acquire = sf == VK_QUEUE_FAMILY_FOREIGN_EXT && df == d->family, release = sf == d->family && df == VK_QUEUE_FAMILY_FOREIGN_EXT;
            REQUIRE(acquire ? im->ahb && c->renderer_images[j].foreign : release ? im->ahb && !c->renderer_images[j].foreign && !da : !c->renderer_images[j].foreign && ((sf == d->family && df == d->family) || (sf == VK_QUEUE_FAMILY_IGNORED && df == VK_QUEUE_FAMILY_IGNORED)));
            REQUIRE(!(da & VK_ACCESS_SHADER_WRITE_BIT) || (im->usage & VK_IMAGE_USAGE_STORAGE_BIT));
            barriers[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = (VkImageLayout)old, .newLayout = (VkImageLayout)next, .srcQueueFamilyIndex = sf, .dstQueueFamilyIndex = df, .image = im->handle, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (d->interop.ahb_enabled && native_verbose(d)) LOG("BLIT native image barrier: image broker ID=%u layout=%u->%u access=0x%x->0x%x queue=%u->%u stages=0x%x->0x%x", iid, old, next, sa, da, sf, df, src, dst);
            c->renderer_images[j].final = (VkImageLayout)next; c->renderer_images[j].foreign = release;
        }
        d->interop.CmdPipelineBarrier(c->handle, src, dst, 0, 0, NULL, 0, NULL, n, barriers); ++c->recorded; break;
    }
    case MB_RENDERER_COPY: case MB_RENDERER_CLEAR: {
        SIZE(op == MB_RENDERER_COPY ? 44 : 32); struct native_command *c = native_find_command(d, id); REQUIRE(c && c->state == 1);
        uint32_t iid = mb_get_u32(w + 8); struct native_image *im = interop_find_image(d, iid); int j = renderer_image_state(d, c, iid); REQUIRE(im && j >= 0 && !c->renderer_images[j].foreign);
        uint32_t layout = mb_get_u32(w + (op == MB_RENDERER_COPY ? 16 : 12)); REQUIRE(layout == (uint32_t)c->renderer_images[j].final);
        if (op == MB_RENDERER_CLEAR) {
            REQUIRE((im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && (layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
            VkClearColorValue color; for (unsigned i = 0; i < 4; ++i) { uint32_t bits = mb_get_u32(w + 16 + i * 4); memcpy(&color.float32[i], &bits, 4); REQUIRE(isfinite(color.float32[i]) && color.float32[i] >= 0 && color.float32[i] <= 1); }
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; d->interop.CmdClearColorImage(c->handle, im->handle, (VkImageLayout)layout, &color, 1, &range);
        } else {
            uint32_t bid = mb_get_u32(w + 12), direction = mb_get_u32(w + 20); uint64_t offset = mb_get_u64(w + 24);
            uint32_t width = mb_get_u32(w + 32), height = mb_get_u32(w + 36), depth = mb_get_u32(w + 40);
            struct native_buffer *b = interop_find_buffer(d, bid); REQUIRE(b && b->memory && direction <= 1 && width == im->width && height == im->height && depth == (im->depth ? im->depth : 1) && !(offset % 4) && interop_range(b->size, offset, (uint64_t)width * height * depth * 4) && !interop_ref(c, bid));
            REQUIRE(direction ? (im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && (b->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && (layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) : (im->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (b->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) && (layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
            VkBufferImageCopy region = {.bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {width, height, depth}};
            if (d->interop.ahb_enabled && native_verbose(d)) LOG("BLIT native %s: image broker ID=%u buffer ID=%u offset=%llu extent=%ux%ux%u layout=%u", direction ? "source upload" : "image GPU readback", iid, bid, (unsigned long long)offset, width, height, depth, layout);
            if (direction) d->interop.CmdCopyBufferToImage(c->handle, b->handle, im->handle, (VkImageLayout)layout, 1, &region); else d->interop.CmdCopyImageToBuffer(c->handle, im->handle, (VkImageLayout)layout, b->handle, 1, &region);
        }
        ++c->recorded; break;
    }
    case MB_RENDERER_IDLE: SIZE(4); *result = v->DeviceWaitIdle(d->handle); if (*result == VK_SUCCESS) native_drain_device(d); break;
    default: return MB_PROTOCOL_ERROR;
    }
#undef SIZE
#undef REQUIRE
    if (native_verbose(d) || *result != VK_SUCCESS) LOG("renderer native RPC=%u device=%u VkResult=%d", op, d->id, (int)*result);
    if (creating && o) {
        if (*result == VK_SUCCESS) { o->id = ++s->next_resource_id; mb_put_u32(reply, o->id); *extra = 4; *count = 1; }
        else { if (o->kind == MB_R_PIPELINE && o->handle.pipeline) v->DestroyPipeline(d->handle, o->handle.pipeline, NULL); memset(o, 0, sizeof(*o)); }
    }
    if (*result == VK_ERROR_DEVICE_LOST) { d->submit.lost = 1; native_drain_device(d); }
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
