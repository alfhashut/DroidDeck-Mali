/* Real Android Vulkan calls for the deliberately small v6 diagnostic subset. */
static void native_renderer_complete(struct native_device *, struct native_command *);
static int native_renderer_can_submit(struct native_device *, struct native_command *);
static int native_renderer_image_referenced(struct native_device *, uint32_t);
static void native_complete_fence(struct native_device *, uint32_t);
static void native_drain_device(struct native_device *);
static int native_interop_init(struct native_device *d, int ahb) {
#define MB_INTEROP_ENTRY(n) d->interop.n = (PFN_vk##n)d->gdpa(d->handle, "vk" #n); if (!d->interop.n) return -1;
#include "interop_entries.def"
#undef MB_INTEROP_ENTRY
    if (ahb) {
        d->interop.ahb_properties = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(d->handle, "vkGetAndroidHardwareBufferPropertiesANDROID");
        d->interop.fence_fd = (PFN_vkGetFenceFdKHR)d->gdpa(d->handle, "vkGetFenceFdKHR");
        if (!d->interop.ahb_properties || !d->interop.fence_fd || !d->image_properties || !d->fence_properties) return -1;
    }
    d->interop.enabled = 1; d->interop.ahb_enabled = ahb; return 0;
}
#define INTEROP_FIND(type, field) \
static struct native_##type *interop_find_##type(struct native_device *d, uint32_t id) { \
    if (!id) return NULL; \
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->interop.field[i].id == id) return &d->interop.field[i]; \
    return NULL; \
}
INTEROP_FIND(buffer, buffers)
INTEROP_FIND(memory, memories)
INTEROP_FIND(image, images)
INTEROP_FIND(ahb, ahbs)
INTEROP_FIND(sync, syncs)
#undef INTEROP_FIND
static int interop_ahb_held(struct native_device *d, const struct native_ahb *a) {
    return a && a->id && (a->id == d->session.held || a->id == d->session.pending || mb_normal_owned(a->normal_key));
}
static int interop_normal_reference(struct native_device *d, uint32_t id) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_ahb *a = &d->interop.ahbs[i];
        if (interop_ahb_held(d, a) && (id == a->image || id == a->memory)) return 1;
    }
    return 0;
}
static int interop_referenced(struct native_device *d, uint32_t id, int pending_only) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->interop.ahbs[i].id && interop_ahb_held(d, &d->interop.ahbs[i]) && (d->interop.ahbs[i].image == id || d->interop.ahbs[i].memory == id)) return 1;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_command *c = &d->submit.commands[i];
        if (!c->id || (pending_only && c->state != 3)) continue;
        for (unsigned j = 0; j < c->ref_count; ++j) if (c->refs[j] == id) return 1;
    }
    return 0;
}
static int interop_ref(struct native_command *c, uint32_t id) {
    for (unsigned i = 0; i < c->ref_count; ++i) if (c->refs[i] == id) return 0;
    if (c->ref_count == MB_INTEROP_MAX_REFS) return -1;
    c->refs[c->ref_count++] = id; return 0;
}
static struct native_command *interop_recording(struct native_device *d, uint32_t id) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->submit.commands[i].id == id && id && d->submit.commands[i].state == 1) return &d->submit.commands[i];
    return NULL;
}
static void native_interop_complete(struct native_device *d, struct native_command *c) {
    if (c->renderer) { native_renderer_complete(d, c); return; }
    struct native_image *image = interop_find_image(d, c->image_id);
    if (image) { image->layout = c->final_layout; image->foreign = c->image_foreign; }
}
static int native_interop_can_submit(struct native_device *d, struct native_command *c) {
    if (d->session.release_timeouts) return 0;
    if (!d->interop.enabled) return 1;
    if (c->renderer) return native_renderer_can_submit(d, c);
    struct native_image *image = interop_find_image(d, c->image_id);
    if (c->image_id && (!image || image->foreign != c->initial_foreign || image->layout != c->initial_layout)) return 0;
    for (unsigned i = 0; i < c->ref_count; ++i) {
        struct native_buffer *b = interop_find_buffer(d, c->refs[i]);
        struct native_image *im = interop_find_image(d, c->refs[i]);
        struct native_memory *m = interop_find_memory(d, b ? b->memory : im ? im->memory : 0);
        if ((!b && !im) || !m || m->map || interop_referenced(d, c->refs[i], 1)) return 0;
    }
    return 1;
}
static void native_interop_close(struct native_device *d) {
    struct native_interop *v = &d->interop;
    if (!v->enabled) return;
    /* A timed-out session retains its exact Android-owned image/allocation/AHB.
     * Everything else is safe after native_drain_device. */
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        if (v->syncs[i].id && v->syncs[i].fd >= 0) { close(v->syncs[i].fd); if (d->session.enabled || d->normal.enabled) ++d->session.fds_closed; }
        if (v->buffers[i].id) v->DestroyBuffer(d->handle, v->buffers[i].handle, NULL);
        if (v->images[i].id && !interop_normal_reference(d, v->images[i].id)) v->DestroyImage(d->handle, v->images[i].handle, NULL);
    }
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        if (v->memories[i].id && !interop_normal_reference(d, v->memories[i].id)) {
            if (v->memories[i].map) v->UnmapMemory(d->handle, v->memories[i].handle);
            v->FreeMemory(d->handle, v->memories[i].handle, NULL);
        }
        if (v->ahbs[i].id && !interop_ahb_held(d, &v->ahbs[i])) AHardwareBuffer_release(v->ahbs[i].handle);
    }
}
static int interop_range(uint64_t total, uint64_t offset, uint64_t size) {
    return size && offset <= total && size <= total - offset;
}
static int interop_memory_pending(struct native_device *d, struct native_memory *m) {
    return m->bound && interop_referenced(d, m->bound, 1);
}
static VkResult interop_wait_sync_impl(struct native_device *d, struct native_sync *sync, uint32_t ms) {
    if (sync->completed) return VK_SUCCESS;
    if (sync->fd >= 0) {
        struct pollfd p = {.fd = sync->fd, .events = POLLIN}; int n;
        do { n = poll(&p, 1, (int)ms); } while (n < 0 && errno == EINTR);
        if (!n) return VK_TIMEOUT;
        if (n < 0 || !(p.revents & POLLIN) || (p.revents & (POLLERR | POLLNVAL))) return VK_ERROR_UNKNOWN;
    }
    sync->completed = 1; native_complete_fence(d, sync->fence);
    if (native_verbose(d)) LOG("SYNC_FD producer completed sync ID=%u; command resources now safe", sync->id);
    return VK_SUCCESS;
}
static VkResult interop_wait_sync(struct native_device *d, struct native_sync *sync, uint32_t ms) {
    uint64_t start = native_perf_start(d);
    VkResult result = interop_wait_sync_impl(d, sync, ms);
    native_perf_end(d, 3, start);
    return result;
}
static uint32_t native_interop_command(struct vk_session *s, uint32_t op, const uint8_t *w,
        uint32_t bytes, uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    uint32_t expected = 8;
    switch (op) {
    case MB_BUFFER_CREATE: case MB_MEMORY_ALLOCATE: case MB_IMAGE_CREATE: expected = 16; break;
    case MB_BUFFER_BIND: case MB_IMAGE_BIND: case MB_MEMORY_READ: case MB_MEMORY_WRITE: expected = 20; break;
    case MB_MEMORY_MAP: case MB_MEMORY_FLUSH: case MB_MEMORY_INVALIDATE: expected = 24; break;
    case MB_COMMAND_FILL: case MB_COMMAND_CLEAR: case MB_COMMAND_COPY: expected = 32; break;
    case MB_COMMAND_BARRIER: expected = 48; break;
    case MB_AHB_CREATE: case MB_SYNC_EXPORT: case MB_SYNC_WAIT: case MB_AHB_PRESENT: expected = 12; break;
    case MB_AHB_INSPECT: expected = 16; break;
    }
    if (bytes < expected || (op != MB_MEMORY_WRITE && bytes != expected)) return MB_PROTOCOL_ERROR;
    uint32_t device = mb_get_u32(w), id = mb_get_u32(w + 4);
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle && s->logical[i].id == device) { d = &s->logical[i]; break; }
    if (!d || !d->interop.enabled) return MB_PROTOCOL_ERROR;
    struct native_interop *v = &d->interop;
    if (d->submit.lost) { *result = VK_ERROR_DEVICE_LOST; return MB_VULKAN_ERROR; }
    uint32_t new_id = 0;
#define NEW_INTEROP(field, type) \
    struct native_##type *o = NULL; \
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (!v->field[i].id) { o = &v->field[i]; break; } \
    if (!o || s->next_resource_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; } \
    new_id = s->next_resource_id + 1
#define FAIL_NATIVE(r) do { *result = (r); goto finished; } while (0)
    switch (op) {
    case MB_BUFFER_CREATE: {
        uint64_t size = mb_get_u64(w + 4); uint32_t usage = mb_get_u32(w + 12);
        if (!size || size > (d->normal.enabled ? MB_NORMAL_MAX_MEMORY : MB_INTEROP_MAX_MEMORY) || !usage || (usage & ~(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (d->renderer.enabled ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0)))) return MB_PROTOCOL_ERROR;
        NEW_INTEROP(buffers, buffer);
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        *result = v->CreateBuffer(d->handle, &ci, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; o->size = size; o->usage = usage; v->GetBufferMemoryRequirements(d->handle, o->handle, &o->req); }
        break;
    }
    case MB_IMAGE_CREATE: {
        uint32_t width = id, height = mb_get_u32(w + 8), usage = mb_get_u32(w + 12);
        if (!width || !height || width > (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) || height > (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) || !usage || (usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))) return MB_PROTOCOL_ERROR;
        NEW_INTEROP(images, image);
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        *result = v->CreateImage(d->handle, &ci, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; o->width = width; o->height = height; o->type = VK_IMAGE_TYPE_2D; o->depth = 1; o->usage = usage; v->GetImageMemoryRequirements(d->handle, o->handle, &o->req); }
        break;
    }
    case MB_BUFFER_REQUIREMENTS: case MB_IMAGE_REQUIREMENTS: {
        struct native_buffer *b = interop_find_buffer(d, id); struct native_image *im = interop_find_image(d, id);
        if ((op == MB_BUFFER_REQUIREMENTS && !b) || (op == MB_IMAGE_REQUIREMENTS && !im)) return MB_PROTOCOL_ERROR;
        VkMemoryRequirements *r = op == MB_BUFFER_REQUIREMENTS ? &b->req : &im->req;
        mb_put_u64(reply, r->size); mb_put_u64(reply + 8, r->alignment); mb_put_u32(reply + 16, r->memoryTypeBits); *extra = 20; *count = 1; break;
    }
    case MB_BUFFER_DESTROY: case MB_IMAGE_DESTROY: {
        struct native_buffer *b = interop_find_buffer(d, id); struct native_image *im = interop_find_image(d, id);
        if ((op == MB_BUFFER_DESTROY && !b) || (op == MB_IMAGE_DESTROY && !im) || interop_referenced(d, id, 0) || (im && native_renderer_image_referenced(d, id))) return MB_PROTOCOL_ERROR;
        struct native_memory *m = interop_find_memory(d, op == MB_BUFFER_DESTROY ? b->memory : im->memory);
        if (m) m->bound = 0;
        if (op == MB_BUFFER_DESTROY) { v->DestroyBuffer(d->handle, b->handle, NULL); memset(b, 0, sizeof(*b)); }
        else { v->DestroyImage(d->handle, im->handle, NULL); memset(im, 0, sizeof(*im)); }
        break;
    }
    case MB_MEMORY_ALLOCATE: {
        uint64_t size = mb_get_u64(w + 4); uint32_t type = mb_get_u32(w + 12);
        if (!size || size > (d->normal.enabled ? MB_NORMAL_MAX_MEMORY : MB_INTEROP_MAX_MEMORY) || type >= v->properties.memoryTypeCount) return MB_PROTOCOL_ERROR;
        NEW_INTEROP(memories, memory);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = type};
        *result = v->AllocateMemory(d->handle, &ai, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; o->size = size; o->type = type; } break;
    }
    case MB_MEMORY_FREE: {
        struct native_memory *m = interop_find_memory(d, id);
        if (!m || m->bound || m->map || interop_memory_pending(d, m)) return MB_PROTOCOL_ERROR;
        v->FreeMemory(d->handle, m->handle, NULL); memset(m, 0, sizeof(*m)); break;
    }
    case MB_BUFFER_BIND: case MB_IMAGE_BIND: {
        struct native_buffer *b = interop_find_buffer(d, id); struct native_image *im = interop_find_image(d, id);
        struct native_memory *m = interop_find_memory(d, mb_get_u32(w + 8)); uint64_t offset = mb_get_u64(w + 12);
        if (!m || m->bound || m->map || m->ahb || (op == MB_BUFFER_BIND ? !b || b->memory : !im || im->memory)) return MB_PROTOCOL_ERROR;
        VkMemoryRequirements *r = op == MB_BUFFER_BIND ? &b->req : &im->req;
        if (!r->alignment || offset % r->alignment || !(r->memoryTypeBits & (1u << m->type)) || !interop_range(m->size, offset, r->size)) return MB_PROTOCOL_ERROR;
        *result = op == MB_BUFFER_BIND ? v->BindBufferMemory(d->handle, b->handle, m->handle, offset) : v->BindImageMemory(d->handle, im->handle, m->handle, offset);
        if (*result == VK_SUCCESS) { m->bound = id; if (op == MB_BUFFER_BIND) { b->memory = m->id; b->offset = offset; } else { im->memory = m->id; im->offset = offset; } } break;
    }
    case MB_MEMORY_MAP: case MB_MEMORY_UNMAP: case MB_MEMORY_READ: case MB_MEMORY_WRITE: case MB_MEMORY_FLUSH: case MB_MEMORY_INVALIDATE: {
        struct native_memory *m = interop_find_memory(d, id);
        if (!m || m->ahb || !(v->properties.memoryTypes[m->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || interop_memory_pending(d, m)) return MB_PROTOCOL_ERROR;
        uint64_t offset = bytes >= 16 ? mb_get_u64(w + 8) : 0, size = 0;
        if (op == MB_MEMORY_UNMAP) {
            if (!m->map) return MB_PROTOCOL_ERROR;
            v->UnmapMemory(d->handle, m->handle); m->map = NULL; m->map_size = 0; break;
        }
        size = op == MB_MEMORY_READ || op == MB_MEMORY_WRITE ? mb_get_u32(w + 16) : mb_get_u64(w + 16);
        if (size == VK_WHOLE_SIZE && offset <= m->size) size = m->size - offset;
        if (!interop_range(m->size, offset, size)) return MB_PROTOCOL_ERROR;
        if (op == MB_MEMORY_MAP) {
            if (m->map) return MB_PROTOCOL_ERROR;
            *result = v->MapMemory(d->handle, m->handle, offset, size, 0, &m->map);
            if (*result == VK_SUCCESS) { m->map_offset = offset; m->map_size = size; } break;
        }
        if (!m->map || !mb_interop_mapped_range(m->size, m->map_offset, m->map_size, offset, size)) return MB_PROTOCOL_ERROR;
        if (op == MB_MEMORY_READ || op == MB_MEMORY_WRITE) {
            if (op == MB_MEMORY_WRITE ? !mb_interop_write_size(s->wire_version, bytes, size) : size > MB_INTEROP_CHUNK) return MB_PROTOCOL_ERROR;
            uint8_t *ptr = (uint8_t *)m->map + offset - m->map_offset;
            if (op == MB_MEMORY_WRITE) memcpy(ptr, w + 20, (size_t)size);
            else { memcpy(reply, ptr, (size_t)size); *extra = (uint32_t)size; *count = 1; } break;
        }
        if (!v->atom || offset % v->atom || (size % v->atom && size != m->size - offset)) return MB_PROTOCOL_ERROR;
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = m->handle, .offset = offset, .size = size};
        *result = op == MB_MEMORY_FLUSH ? v->FlushMappedMemoryRanges(d->handle, 1, &range) : v->InvalidateMappedMemoryRanges(d->handle, 1, &range); break;
    }
    case MB_COMMAND_FILL: case MB_COMMAND_BARRIER: case MB_COMMAND_CLEAR: case MB_COMMAND_COPY: {
        struct native_command *c = interop_recording(d, id);
        if (!c || !(d->queue_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) return MB_PROTOCOL_ERROR;
        if (op == MB_COMMAND_FILL) {
            struct native_buffer *b = interop_find_buffer(d, mb_get_u32(w + 8));
            uint64_t offset = mb_get_u64(w + 12), size = mb_get_u64(w + 20);
            if (!b || !b->memory || !(b->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || offset % 4) return MB_PROTOCOL_ERROR;
            if (size == VK_WHOLE_SIZE && offset <= b->size) size = (b->size - offset) & ~UINT64_C(3);
            if (size % 4 || !interop_range(b->size, offset, size) || interop_ref(c, b->id)) return MB_PROTOCOL_ERROR;
            v->CmdFillBuffer(c->handle, b->handle, offset, size, mb_get_u32(w + 28));
        } else if (op == MB_COMMAND_BARRIER) {
            uint32_t src = mb_get_u32(w + 8), dst = mb_get_u32(w + 12), kind = mb_get_u32(w + 16), resource = mb_get_u32(w + 20);
            uint32_t sa = mb_get_u32(w + 24), da = mb_get_u32(w + 28), old = mb_get_u32(w + 32), next = mb_get_u32(w + 36);
            uint32_t sf = mb_get_u32(w + 40), df = mb_get_u32(w + 44);
            if (!kind) {
                struct native_buffer *b = interop_find_buffer(d, resource);
                if (!b || !b->memory || src != VK_PIPELINE_STAGE_TRANSFER_BIT || dst != VK_PIPELINE_STAGE_HOST_BIT ||
                    sa != VK_ACCESS_TRANSFER_WRITE_BIT || da != VK_ACCESS_HOST_READ_BIT || old || next ||
                    sf != VK_QUEUE_FAMILY_IGNORED || df != VK_QUEUE_FAMILY_IGNORED || interop_ref(c, b->id)) return MB_PROTOCOL_ERROR;
                VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                    .srcQueueFamilyIndex = sf, .dstQueueFamilyIndex = df, .buffer = b->handle, .offset = 0, .size = VK_WHOLE_SIZE};
                v->CmdPipelineBarrier(c->handle, src, dst, 0, 0, NULL, 1, &barrier, 0, NULL);
            } else if (kind == 1) {
                struct native_image *im = interop_find_image(d, resource);
                if (!im || !im->memory || (c->image_id && c->image_id != im->id)) return MB_PROTOCOL_ERROR;
                uint32_t current = c->image_id ? (uint32_t)c->final_layout : (uint32_t)im->layout;
                if (old != current) return MB_PROTOCOL_ERROR;
                int acquire = im->ahb && old == VK_IMAGE_LAYOUT_UNDEFINED && next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && sf == VK_QUEUE_FAMILY_FOREIGN_EXT && df == d->family;
                uint32_t foreign = c->image_id ? c->image_foreign : im->foreign;
                if (foreign && !acquire) return MB_PROTOCOL_ERROR;
                int release = next == VK_IMAGE_LAYOUT_GENERAL && im->ahb && sf == d->family && df == VK_QUEUE_FAMILY_FOREIGN_EXT;
                if (!release && !acquire && (sf != VK_QUEUE_FAMILY_IGNORED || df != VK_QUEUE_FAMILY_IGNORED)) return MB_PROTOCOL_ERROR;
                int valid = (old == VK_IMAGE_LAYOUT_UNDEFINED && next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && src == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT && !sa && da == VK_ACCESS_TRANSFER_WRITE_BIT) ||
                    (old == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && next == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT && sa == VK_ACCESS_TRANSFER_WRITE_BIT && da == VK_ACCESS_TRANSFER_READ_BIT) ||
                    (release && old == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && sa == VK_ACCESS_TRANSFER_READ_BIT && !da);
                if (!valid || interop_ref(c, im->id)) return MB_PROTOCOL_ERROR;
                if (!c->image_id) { c->image_id = im->id; c->initial_layout = im->layout; c->initial_foreign = im->foreign; }
                c->final_layout = (VkImageLayout)next; c->image_foreign = release;
                VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                    .oldLayout = (VkImageLayout)old, .newLayout = (VkImageLayout)next, .srcQueueFamilyIndex = sf, .dstQueueFamilyIndex = df,
                    .image = im->handle, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                v->CmdPipelineBarrier(c->handle, src, dst, 0, 0, NULL, 0, NULL, 1, &barrier);
            } else return MB_PROTOCOL_ERROR;
        } else {
            struct native_image *im = interop_find_image(d, mb_get_u32(w + 8));
            uint32_t layout = mb_get_u32(w + (op == MB_COMMAND_CLEAR ? 12 : 16));
            if (!im || !im->memory || c->image_id != im->id || c->image_foreign || layout != (uint32_t)c->final_layout) return MB_PROTOCOL_ERROR;
            if (op == MB_COMMAND_CLEAR) {
                if (layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || !(im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !(d->queue_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) return MB_PROTOCOL_ERROR;
                VkClearColorValue color;
                for (unsigned i = 0; i < 4; ++i) { uint32_t bits = mb_get_u32(w + 16 + i * 4); memcpy(&color.float32[i], &bits, 4); if (!isfinite(color.float32[i]) || color.float32[i] < 0 || color.float32[i] > 1) return MB_PROTOCOL_ERROR; }
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                v->CmdClearColorImage(c->handle, im->handle, (VkImageLayout)layout, &color, 1, &range);
            } else {
                struct native_buffer *b = interop_find_buffer(d, mb_get_u32(w + 12));
                uint32_t width = mb_get_u32(w + 20), height = mb_get_u32(w + 24), direction = mb_get_u32(w + 28);
                if (!b || !b->memory || width != im->width || height != im->height || b->size < (uint64_t)width * height * 4 || direction > 1 ||
                    (direction ? layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || !(b->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !(im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) :
                        layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || !(b->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !(im->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) || interop_ref(c, b->id)) return MB_PROTOCOL_ERROR;
                VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {width, height, 1}};
                if (direction) v->CmdCopyBufferToImage(c->handle, b->handle, im->handle, (VkImageLayout)layout, 1, &region);
                else v->CmdCopyImageToBuffer(c->handle, im->handle, (VkImageLayout)layout, b->handle, 1, &region);
            }
        }
        ++c->recorded; break;
    }
    case MB_AHB_CREATE: {
        if (!v->ahb_enabled) FAIL_NATIVE(VK_ERROR_EXTENSION_NOT_PRESENT);
        uint32_t width = id, height = mb_get_u32(w + 8);
        if (!width || !height || width > (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) || height > (d->normal.enabled ? MB_NORMAL_MAX_DIMENSION : 256u) || s->next_resource_id > UINT32_MAX - 3) return MB_PROTOCOL_ERROR;
        struct native_image *im = NULL; struct native_memory *m = NULL; struct native_ahb *a = NULL;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
            if (!im && !v->images[i].id) im = &v->images[i];
            if (!m && !v->memories[i].id) m = &v->memories[i];
            if (!a && !v->ahbs[i].id) a = &v->ahbs[i];
        }
        if (!im || !m || !a) FAIL_NATIVE(VK_ERROR_TOO_MANY_OBJECTS);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (d->renderer.enabled ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        VkPhysicalDeviceExternalImageFormatInfo external = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &external,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
        VkExternalImageFormatProperties ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext};
        *result = d->image_properties(d->physical, &query, &props);
        if (*result != VK_SUCCESS) goto finished;
        if (!(ext.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(ext.externalMemoryProperties.compatibleHandleTypes & external.handleType) || width > props.imageFormatProperties.maxExtent.width || height > props.imageFormatProperties.maxExtent.height || !(props.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) FAIL_NATIVE(VK_ERROR_FORMAT_NOT_SUPPORTED);
        AHardwareBuffer_Desc desc = {.width = width, .height = height, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY};
        AHardwareBuffer *ahb = NULL;
        int ar = AHardwareBuffer_allocate(&desc, &ahb);
        if (ar && !ahb) {
            LOG("AHB CPU-readable allocation unavailable result=%d; trying actual GPU-only usage", ar);
            desc.usage &= ~((uint64_t)AHARDWAREBUFFER_USAGE_CPU_READ_MASK);
            ar = AHardwareBuffer_allocate(&desc, &ahb);
        }
        if (ar || !ahb) { LOG("AHardwareBuffer_allocate failed errno/result=%d (configuration unsupported)", ar); FAIL_NATIVE(VK_ERROR_FORMAT_NOT_SUPPORTED); }
        AHardwareBuffer_describe(ahb, &desc);
        VkAndroidHardwareBufferFormatPropertiesANDROID format = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID ap = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &format};
        *result = v->ahb_properties(d->handle, ahb, &ap);
        if (*result == VK_SUCCESS && (format.format != VK_FORMAT_R8G8B8A8_UNORM || desc.width != width || desc.height != height || desc.layers != 1 || desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || !ap.allocationSize || !(format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !(format.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || (d->renderer.enabled && !(format.formatFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)))) *result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        uint32_t type = UINT32_MAX;
        for (unsigned pass = 0; pass < 2 && type == UINT32_MAX; ++pass)
            for (uint32_t i = 0; i < v->properties.memoryTypeCount; ++i)
                if ((ap.memoryTypeBits & (1u << i)) && (pass || (v->properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) { type = i; break; }
        if (*result == VK_SUCCESS && type == UINT32_MAX) *result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
        VkExternalMemoryImageCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = external.handleType};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ei, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (*result == VK_SUCCESS) *result = v->CreateImage(d->handle, &ci, NULL, &image);
        VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image};
        VkImportAndroidHardwareBufferInfoANDROID import = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = &dedicated, .buffer = ahb};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import, .allocationSize = ap.allocationSize, .memoryTypeIndex = type};
        if (*result == VK_SUCCESS) *result = v->AllocateMemory(d->handle, &ai, NULL, &memory);
        if (*result == VK_SUCCESS) *result = v->BindImageMemory(d->handle, image, memory, 0);
        if (*result != VK_SUCCESS) {
            if (image) v->DestroyImage(d->handle, image, NULL);
            if (memory) v->FreeMemory(d->handle, memory, NULL);
            AHardwareBuffer_release(ahb); goto finished;
        }
        /* AHB image requirements can only be queried AFTER binding (VUID 04004). */
        v->GetImageMemoryRequirements(d->handle, image, &im->req);
        im->id = ++s->next_resource_id; m->id = ++s->next_resource_id; a->id = ++s->next_resource_id;
        im->handle = image; im->memory = m->id; im->ahb = a->id; im->foreign = 1; im->type = VK_IMAGE_TYPE_2D; im->depth = 1; im->width = width; im->height = height; im->usage = usage;
        m->handle = memory; m->size = ap.allocationSize; m->type = type; m->bound = im->id; m->ahb = a->id;
        a->handle = ahb; a->desc = desc; a->image = im->id; a->memory = m->id;
        mb_put_u32(reply, im->id); mb_put_u32(reply + 4, m->id); mb_put_u32(reply + 8, a->id);
        mb_put_u32(reply + 12, desc.width); mb_put_u32(reply + 16, desc.height); mb_put_u32(reply + 20, desc.format); mb_put_u32(reply + 24, desc.layers); mb_put_u64(reply + 28, desc.usage);
        *extra = 36; *count = 1;
        LOG("AHB imported with dedicated image allocation: image=%u memory=%u AHB=%u size=%llu type=%u dimensions=%ux%u", im->id, m->id, a->id, (unsigned long long)m->size, type, width, height); break;
    }
    case MB_AHB_RELEASE: {
        struct native_ahb *a = interop_find_ahb(d, id);
        if (!a || interop_ahb_held(d, a) || interop_find_image(d, a->image) || interop_find_memory(d, a->memory)) return MB_PROTOCOL_ERROR;
        AHardwareBuffer_release(a->handle); memset(a, 0, sizeof(*a)); break;
    }
    case MB_SYNC_FENCE_CREATE: {
        if (!v->ahb_enabled) FAIL_NATIVE(VK_ERROR_EXTENSION_NOT_PRESENT);
        if (id != VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) return MB_PROTOCOL_ERROR;
        VkPhysicalDeviceExternalFenceInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalFenceProperties properties = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
        d->fence_properties(d->physical, &query, &properties);
        if (!(properties.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT) || !(properties.compatibleHandleTypes & query.handleType)) FAIL_NATIVE(VK_ERROR_FEATURE_NOT_PRESENT);
        struct native_fence *f = NULL;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (!d->submit.fences[i].id) { f = &d->submit.fences[i]; break; }
        if (!f || s->next_resource_id == UINT32_MAX) FAIL_NATIVE(VK_ERROR_TOO_MANY_OBJECTS);
        VkExportFenceCreateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, .handleTypes = query.handleType};
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &export};
        *result = d->submit.CreateFence(d->handle, &ci, NULL, &f->handle);
        if (*result == VK_SUCCESS) { f->id = ++s->next_resource_id; f->exportable = 1; mb_put_u32(reply, f->id); *extra = 4; *count = 1; } break;
    }
    case MB_SYNC_EXPORT: {
        struct native_fence *f = NULL;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (id && d->submit.fences[i].id == id) f = &d->submit.fences[i];
        if (!v->ahb_enabled || !f || !f->exportable || !f->submitted || f->exported || mb_get_u32(w + 8) != VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) return MB_PROTOCOL_ERROR;
        NEW_INTEROP(syncs, sync);
        VkFenceGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = f->handle, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        uint64_t perf_start = native_perf_start(d);
        int fd = -1; *result = v->fence_fd(d->handle, &info, &fd);
        native_perf_end(d, 2, perf_start);
        if (*result == VK_SUCCESS) { o->id = new_id; o->fence = f->id; o->fd = fd; f->exported = 1; if ((d->session.enabled || d->normal.enabled) && fd >= 0) ++d->session.fds_created; }
        else if (fd >= 0) { close(fd); }
        break;
    }
    case MB_SYNC_WAIT: case MB_SYNC_CLOSE: {
        struct native_sync *sync = interop_find_sync(d, id);
        if (!sync) return MB_PROTOCOL_ERROR;
        if (op == MB_SYNC_WAIT) {
            uint32_t ms = mb_get_u32(w + 8); if (ms > 5000) return MB_PROTOCOL_ERROR;
            *result = interop_wait_sync(d, sync, ms);
        } else {
            if (!sync->completed) return MB_PROTOCOL_ERROR;
            if (sync->fd >= 0) { close(sync->fd); if (d->session.enabled || d->normal.enabled) ++d->session.fds_closed; }
            memset(sync, 0, sizeof(*sync));
        } break;
    }
    case MB_AHB_INSPECT: case MB_AHB_PRESENT: {
        struct native_ahb *a = interop_find_ahb(d, id); struct native_sync *sync = interop_find_sync(d, mb_get_u32(w + 8));
        struct native_image *im = a ? interop_find_image(d, a->image) : NULL;
        if (!a || !sync || !im) return MB_PROTOCOL_ERROR;
        /* Sync must be the producer of THIS image; a completed unrelated fence is insufficient. */
        int producer = 0;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
            if (d->submit.commands[i].id && d->submit.commands[i].fence == sync->fence && d->submit.commands[i].image_id == im->id) producer = 1;
        if (!producer) return MB_PROTOCOL_ERROR;
        *result = interop_wait_sync(d, sync, 5000);
        if (*result != VK_SUCCESS) goto finished;
        if (!im->foreign || im->layout != VK_IMAGE_LAYOUT_GENERAL) return MB_PROTOCOL_ERROR;
        if (op == MB_AHB_PRESENT) {
            int cr = d->renderer.enabled ? mb_consumer_present_renderer(a->handle, &a->desc, sync->fd) : mb_consumer_present(a->handle, &a->desc, sync->fd);
            LOG("AHB presentation consumer result=%d (0 means completion and previous-buffer release observed)", cr);
            if (cr) FAIL_NATIVE(VK_ERROR_INITIALIZATION_FAILED);
        } else {
            uint32_t pattern = mb_get_u32(w + 12); if (pattern > (d->renderer.enabled ? 2u : 1u)) return MB_PROTOCOL_ERROR;
            uint32_t checked = 0;
            if (a->desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) {
                void *ptr = NULL; int ar = AHardwareBuffer_lock(a->handle, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &ptr);
                if (ar || !ptr) { LOG("AHB CPU read lock failed Android result=%d", ar); FAIL_NATIVE(VK_ERROR_MEMORY_MAP_FAILED); }
                uint64_t bad = 0;
                for (uint32_t y = 0; y < a->desc.height; ++y) for (uint32_t x = 0; x < a->desc.width; ++x) {
                    const uint8_t clear[4] = {64, 128, 191, 255}; uint8_t q[4] = {0, 0, 0, 255};
                    if (y < a->desc.height / 2) q[x < a->desc.width / 2 ? 0 : 1] = 255;
                    else if (x < a->desc.width / 2) q[2] = 255; else q[0] = q[1] = q[2] = 255;
                    const uint8_t *p = (const uint8_t *)ptr + ((uint64_t)y * a->desc.stride + x) * 4;
                    if (pattern == 2) {
                        q[0] = q[1] = q[2] = 0;
                        if (x >= 64 && x < 192 && y >= 64 && y < 192) {
                            if (y < 128) q[x < 128 ? 0 : 1] = 255;
                            else if (x < 128) q[2] = 255; else q[0] = q[1] = q[2] = 255;
                        }
                        if ((x == 96 || x == 160) && (y == 96 || y == 160)) LOG("actual final AHB CPU pixel(%u,%u) RGBA=(%u,%u,%u,%u)", x, y, p[0], p[1], p[2], p[3]);
                    }
                    if (memcmp(p, pattern ? q : clear, 4)) ++bad;
                }
                int release = -1; ar = AHardwareBuffer_unlock(a->handle, &release);
                if (release >= 0) { struct pollfd p = {.fd = release, .events = POLLIN}; while (poll(&p, 1, -1) < 0 && errno == EINTR) {} close(release); }
                LOG("AHB CPU bytes verified mismatched pixels=%llu unlock=%d", (unsigned long long)bad, ar);
                if (bad || ar) FAIL_NATIVE(VK_ERROR_UNKNOWN);
                checked = 1;
            } else LOG("AHB not CPU-lockable for actual usage; verify same-image GPU readback separately");
            mb_put_u32(reply, checked); *extra = 4; *count = 1;
        } break;
    }
    default: return MB_PROTOCOL_ERROR;
    }
finished:
#undef NEW_INTEROP
#undef FAIL_NATIVE
    if (*result == VK_ERROR_DEVICE_LOST) { d->submit.lost = 1; native_drain_device(d); }
    if (*result < 0) return MB_VULKAN_ERROR;
    if (new_id && *result == VK_SUCCESS) { s->next_resource_id = new_id; mb_put_u32(reply, new_id); *extra = 4; *count = 1; }
    return MB_OK;
}
