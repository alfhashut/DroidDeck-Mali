/* Diagnostic mappings are local mirrors. RPC copies only bounded ranges of actual
 * native mapped allocations; coherent unmap/flush uploads, invalidate downloads. */
#include "interop_test_api.h"
static VkResult interop_rpc(struct proxy_logical *d, uint32_t op, const uint8_t *args, uint32_t n,
        uint8_t *out, uint32_t expected) {
    if (!d || d->owner->wire_version < MB_INTEROP_VERSION || n > MB_INTEROP_CHUNK + 16 || expected > MB_INTEROP_CHUNK) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t request[MB_INTEROP_CHUNK + 20], reply[MB_PREFIX_BYTES + MB_INTEROP_CHUNK]; uint32_t bytes = 0;
    mb_put_u32(request, d->id); if (n) memcpy(request + 4, args, n);
    pthread_mutex_lock(&d->owner->lock);
    VkResult r = rpc(d->owner, op, request, n + 4, reply, &bytes, sizeof(reply));
    if (bytes && (bytes != MB_PREFIX_BYTES + (r == VK_SUCCESS ? expected : 0) || mb_get_u32(reply + 8) != (r == VK_SUCCESS && expected ? 1u : 0u))) {
        shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (r == VK_SUCCESS && expected && out) memcpy(out, reply + MB_PREFIX_BYTES, expected);
    pthread_mutex_unlock(&d->owner->lock); return r;
}
static void interop_publish(struct proxy_logical *d, struct proxy_resource *o, enum proxy_resource_kind kind, uint32_t id) {
    set_loader_magic_value(o); o->owner = d; o->kind = kind; o->id = id; o->live = 1;
    pthread_mutex_lock(&d->owner->lock); o->next = d->resources; d->resources = o; pthread_mutex_unlock(&d->owner->lock);
}
static VkResult interop_new(struct proxy_logical *d, uint32_t op, enum proxy_resource_kind kind, const uint8_t *args, uint32_t n, struct proxy_resource **out) {
    *out = NULL; struct proxy_resource *o = calloc(1, sizeof(*o)); if (!o) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t reply[4]; VkResult r = interop_rpc(d, op, args, n, reply, 4);
    if (r == VK_SUCCESS && !mb_get_u32(reply)) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; }
    if (r != VK_SUCCESS) { free(o); return r; }
    interop_publish(d, o, kind, mb_get_u32(reply)); *out = o; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateBuffer(VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || ci->pNext || ci->flags || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE || ci->queueFamilyIndexCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[12]; mb_put_u64(args, ci->size); mb_put_u32(args + 8, ci->usage); struct proxy_resource *o;
    VkResult r = interop_new((struct proxy_logical *)device, MB_BUFFER_CREATE, PROXY_BUFFER, args, sizeof(args), &o);
    if (r == VK_SUCCESS) { *out = (VkBuffer)(uintptr_t)o; }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateImage(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    if (device && ((struct proxy_logical *)device)->owner->wire_version == MB_RENDERER_VERSION) return renderer_CreateImage(device, ci, a, out);
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || ci->pNext || ci->flags ||
        ci->imageType != VK_IMAGE_TYPE_2D || ci->format != VK_FORMAT_R8G8B8A8_UNORM || ci->extent.depth != 1 || ci->mipLevels != 1 || ci->arrayLayers != 1 || ci->samples != VK_SAMPLE_COUNT_1_BIT ||
        ci->tiling != VK_IMAGE_TILING_OPTIMAL || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE || ci->queueFamilyIndexCount || ci->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[12]; mb_put_u32(args, ci->extent.width); mb_put_u32(args + 4, ci->extent.height); mb_put_u32(args + 8, ci->usage); struct proxy_resource *o;
    VkResult r = interop_new((struct proxy_logical *)device, MB_IMAGE_CREATE, PROXY_IMAGE, args, sizeof(args), &o);
    if (r == VK_SUCCESS) { *out = (VkImage)(uintptr_t)o; }
    return r;
}
#define INTEROP_DESTROY(Name, Type, KIND, OP) \
static VKAPI_ATTR void VKAPI_CALL proxy_Destroy##Name(VkDevice device, Type handle, const VkAllocationCallbacks *a) { \
    if (!device || !handle) return; \
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *o = submit_find(d, (uintptr_t)handle, KIND); \
    uint8_t args[4]; VkResult r = VK_ERROR_INITIALIZATION_FAILED; \
    if (o && !a) { mb_put_u32(args, o->id); r = interop_rpc(d, OP, args, 4, NULL, 0); } \
    if (r == VK_SUCCESS) { o->live = 0; } submit_void_error(d, "vkDestroy" #Name, r); \
}
INTEROP_DESTROY(Buffer, VkBuffer, PROXY_BUFFER, MB_BUFFER_DESTROY)
INTEROP_DESTROY(Image, VkImage, PROXY_IMAGE, MB_IMAGE_DESTROY)
#undef INTEROP_DESTROY
static void interop_requirements(VkDevice device, uintptr_t handle, enum proxy_resource_kind kind, uint32_t op, VkMemoryRequirements *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out)); if (!device) return;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *o = submit_find(d, handle, kind); uint8_t args[4], reply[20]; VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (o) { mb_put_u32(args, o->id); r = interop_rpc(d, op, args, 4, reply, 20); }
    if (r == VK_SUCCESS) { out->size = mb_get_u64(reply); out->alignment = mb_get_u64(reply + 8); out->memoryTypeBits = mb_get_u32(reply + 16); }
    submit_void_error(d, "memory requirements", r);
}
static VKAPI_ATTR void VKAPI_CALL proxy_GetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out) { interop_requirements(d, (uintptr_t)b, PROXY_BUFFER, MB_BUFFER_REQUIREMENTS, out); }
static VKAPI_ATTR void VKAPI_CALL proxy_GetImageMemoryRequirements(VkDevice d, VkImage im, VkMemoryRequirements *out) { interop_requirements(d, (uintptr_t)im, PROXY_IMAGE, MB_IMAGE_REQUIREMENTS, out); }
static VKAPI_ATTR VkResult VKAPI_CALL proxy_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ai || a || ai->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || ai->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device;
    if (ai->memoryTypeIndex >= d->memory_properties.memoryTypeCount) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[12]; mb_put_u64(args, ai->allocationSize); mb_put_u32(args + 8, ai->memoryTypeIndex); struct proxy_resource *o;
    VkResult r = interop_new(d, MB_MEMORY_ALLOCATE, PROXY_MEMORY, args, 12, &o);
    if (r == VK_SUCCESS) { o->allocation = ai->allocationSize; o->pool = d->memory_properties.memoryTypes[ai->memoryTypeIndex].propertyFlags; *out = (VkDeviceMemory)(uintptr_t)o; } return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    if (!device || !memory) return;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *o = submit_find(d, (uintptr_t)memory, PROXY_MEMORY); VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (o && !a && !o->mirror) { uint8_t args[4]; mb_put_u32(args, o->id); r = interop_rpc(d, MB_MEMORY_FREE, args, 4, NULL, 0); }
    if (r == VK_SUCCESS) { o->live = 0; } submit_void_error(d, "vkFreeMemory", r);
}
static VkResult interop_bind(VkDevice device, uintptr_t handle, enum proxy_resource_kind kind, uint32_t op, VkDeviceMemory memory, VkDeviceSize offset) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *o = submit_find(d, handle, kind), *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY);
    if (!o || !m) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[16]; mb_put_u32(args, o->id); mb_put_u32(args + 4, m->id); mb_put_u64(args + 8, offset);
    return interop_rpc(d, op, args, 16, NULL, 0);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { return interop_bind(d, (uintptr_t)b, PROXY_BUFFER, MB_BUFFER_BIND, m, o); }
static VKAPI_ATTR VkResult VKAPI_CALL proxy_BindImageMemory(VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize o) { return interop_bind(d, (uintptr_t)im, PROXY_IMAGE, MB_IMAGE_BIND, m, o); }
static VkResult interop_copy_mapping(struct proxy_resource *m, uint64_t offset, uint64_t size, int upload) {
    if (!m->mirror || offset < m->map_offset || offset - m->map_offset > m->map_size || !size || size > m->map_size - (offset - m->map_offset)) return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t args[16 + MB_INTEROP_CHUNK];
    for (uint64_t n = 0; n < size;) {
        uint32_t length = size - n > MB_INTEROP_CHUNK ? MB_INTEROP_CHUNK : (uint32_t)(size - n);
        mb_put_u32(args, m->id); mb_put_u64(args + 4, offset + n); mb_put_u32(args + 12, length);
        uint8_t *p = m->mirror + offset + n - m->map_offset;
        if (upload) memcpy(args + 16, p, length);
        VkResult r = interop_rpc(m->owner, upload ? MB_MEMORY_WRITE : MB_MEMORY_READ, args, 16 + (upload ? length : 0), upload ? NULL : p, upload ? 0 : length);
        if (r != VK_SUCCESS) return r;
        n += length;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL; if (!device || flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY);
    if (!m || m->mirror || offset >= m->allocation) return VK_ERROR_MEMORY_MAP_FAILED;
    if (size == VK_WHOLE_SIZE) size = m->allocation - offset;
    size_t align = d->map_alignment < sizeof(void *) ? sizeof(void *) : d->map_alignment;
    if (!size || size > m->allocation - offset || (align & (align - 1)) || offset % align) return VK_ERROR_MEMORY_MAP_FAILED;
    void *ptr = NULL; if (posix_memalign(&ptr, align, (size_t)size)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t args[20]; mb_put_u32(args, m->id); mb_put_u64(args + 4, offset); mb_put_u64(args + 12, size);
    VkResult r = interop_rpc(d, MB_MEMORY_MAP, args, 20, NULL, 0);
    if (r != VK_SUCCESS) { free(ptr); return r; }
    m->mirror = ptr; m->map_offset = offset; m->map_size = size; memset(ptr, 0, (size_t)size);
    if (m->pool & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) r = interop_copy_mapping(m, offset, size, 0);
    if (r == VK_SUCCESS) *out = ptr;
    else { mb_put_u32(args, m->id); (void)interop_rpc(d, MB_MEMORY_UNMAP, args, 4, NULL, 0); free(m->mirror); m->mirror = NULL; }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    if (!device) return;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *m = submit_find(d, (uintptr_t)memory, PROXY_MEMORY); VkResult r = VK_ERROR_MEMORY_MAP_FAILED;
    if (m && m->mirror) {
        r = m->pool & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? interop_copy_mapping(m, m->map_offset, m->map_size, 1) : VK_SUCCESS;
        if (r == VK_SUCCESS) { uint8_t args[4]; mb_put_u32(args, m->id); r = interop_rpc(d, MB_MEMORY_UNMAP, args, 4, NULL, 0); }
        if (r == VK_SUCCESS) { free(m->mirror); m->mirror = NULL; m->map_size = 0; }
    }
    submit_void_error(d, "vkUnmapMemory", r);
}
static VkResult interop_mapped_range(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges, int flush) {
    if (!device || count != 1 || !ranges || ranges->sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || ranges->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device; struct proxy_resource *m = submit_find(d, (uintptr_t)ranges->memory, PROXY_MEMORY);
    if (!m || !m->mirror || ranges->offset >= m->allocation) return VK_ERROR_MEMORY_MAP_FAILED;
    uint64_t size = ranges->size == VK_WHOLE_SIZE ? m->allocation - ranges->offset : ranges->size;
    if (ranges->offset < m->map_offset || ranges->offset - m->map_offset > m->map_size || !size || size > m->map_size - (ranges->offset - m->map_offset)) return VK_ERROR_MEMORY_MAP_FAILED;
    if (!d->atom || ranges->offset % d->atom || (size % d->atom && size != m->allocation - ranges->offset)) return VK_ERROR_MEMORY_MAP_FAILED;
    uint8_t args[20]; mb_put_u32(args, m->id); mb_put_u64(args + 4, ranges->offset); mb_put_u64(args + 12, size);
    VkResult r = flush ? interop_copy_mapping(m, ranges->offset, size, 1) : VK_SUCCESS;
    if (r == VK_SUCCESS) r = interop_rpc(d, flush ? MB_MEMORY_FLUSH : MB_MEMORY_INVALIDATE, args, 20, NULL, 0);
    if (r == VK_SUCCESS && !flush) r = interop_copy_mapping(m, ranges->offset, size, 0);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_FlushMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { return interop_mapped_range(d, n, r, 1); }
static VKAPI_ATTR VkResult VKAPI_CALL proxy_InvalidateMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { return interop_mapped_range(d, n, r, 0); }
static struct proxy_resource *interop_command(VkCommandBuffer command) {
    if (!command) return NULL;
    struct proxy_resource *c = (struct proxy_resource *)command;
    return c->kind == PROXY_COMMAND && c->live ? c : NULL;
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdFillBuffer(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    struct proxy_resource *c = interop_command(command); if (!c) return;
    struct proxy_resource *b = submit_find(c->owner, (uintptr_t)buffer, PROXY_BUFFER); VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (b) { uint8_t args[28]; mb_put_u32(args, c->id); mb_put_u32(args + 4, b->id); mb_put_u64(args + 8, offset); mb_put_u64(args + 16, size); mb_put_u32(args + 24, data); r = interop_rpc(c->owner, MB_COMMAND_FILL, args, 28, NULL, 0); }
    submit_void_error(c->owner, "vkCmdFillBuffer", r);
}
static int interop_color_range(const VkImageSubresourceRange *r) { return r && r->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && !r->baseMipLevel && r->levelCount == 1 && !r->baseArrayLayer && r->layerCount == 1; }
static VKAPI_ATTR void VKAPI_CALL proxy_CmdPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
        uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images) {
    if (command && ((struct proxy_resource *)command)->owner->owner->wire_version == MB_RENDERER_VERSION && !buffer_count) { renderer_Barrier(command, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images); return; }
    (void)memory; struct proxy_resource *c = interop_command(command); if (!c) return;
    VkResult r = VK_ERROR_FEATURE_NOT_PRESENT; uint8_t args[44]; struct proxy_resource *o = NULL;
    uint32_t sa = 0, da = 0, old = 0, next = 0, sf = 0, df = 0, kind = 0;
    if (!flags && !memory_count && buffer_count == 1 && !image_count && buffers && buffers->sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER && !buffers->pNext && !buffers->offset && buffers->size == VK_WHOLE_SIZE) {
        o = submit_find(c->owner, (uintptr_t)buffers->buffer, PROXY_BUFFER); sa = buffers->srcAccessMask; da = buffers->dstAccessMask; sf = buffers->srcQueueFamilyIndex; df = buffers->dstQueueFamilyIndex;
    } else if (!flags && !memory_count && !buffer_count && image_count == 1 && images && images->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER && !images->pNext && interop_color_range(&images->subresourceRange)) {
        kind = 1; o = submit_find(c->owner, (uintptr_t)images->image, PROXY_IMAGE); sa = images->srcAccessMask; da = images->dstAccessMask; old = images->oldLayout; next = images->newLayout; sf = images->srcQueueFamilyIndex; df = images->dstQueueFamilyIndex;
    }
    if (o) {
        const uint32_t a[] = {c->id, src, dst, kind, o->id, sa, da, old, next, sf, df};
        for (unsigned i = 0; i < 11; ++i) mb_put_u32(args + i * 4, a[i]);
        r = interop_rpc(c->owner, MB_COMMAND_BARRIER, args, 44, NULL, 0);
    }
    submit_void_error(c->owner, "vkCmdPipelineBarrier", r);
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdClearColorImage(VkCommandBuffer command, VkImage image, VkImageLayout layout, const VkClearColorValue *color, uint32_t n, const VkImageSubresourceRange *ranges) {
    if (command && ((struct proxy_resource *)command)->owner->owner->wire_version == MB_RENDERER_VERSION) { renderer_Clear(command, image, layout, color, n, ranges); return; }
    struct proxy_resource *c = interop_command(command); if (!c) return;
    struct proxy_resource *im = submit_find(c->owner, (uintptr_t)image, PROXY_IMAGE); VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (im && color && n == 1 && interop_color_range(ranges)) {
        uint8_t args[28]; mb_put_u32(args, c->id); mb_put_u32(args + 4, im->id); mb_put_u32(args + 8, layout);
        for (unsigned i = 0; i < 4; ++i) { uint32_t bits; memcpy(&bits, &color->float32[i], 4); mb_put_u32(args + 12 + i * 4, bits); }
        r = interop_rpc(c->owner, MB_COMMAND_CLEAR, args, 28, NULL, 0);
    }
    submit_void_error(c->owner, "vkCmdClearColorImage", r);
}
static void interop_copy_image(VkCommandBuffer command, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t n, const VkBufferImageCopy *regions, uint32_t direction) {
    if (command && ((struct proxy_resource *)command)->owner->owner->wire_version == MB_RENDERER_VERSION) { renderer_Copy(command, image, layout, buffer, n, regions, direction); return; }
    struct proxy_resource *c = interop_command(command); if (!c) return;
    struct proxy_resource *im = submit_find(c->owner, (uintptr_t)image, PROXY_IMAGE), *b = submit_find(c->owner, (uintptr_t)buffer, PROXY_BUFFER); VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (im && b && n == 1 && regions && !regions->bufferOffset && !regions->bufferRowLength && !regions->bufferImageHeight &&
        regions->imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && !regions->imageSubresource.mipLevel && !regions->imageSubresource.baseArrayLayer && regions->imageSubresource.layerCount == 1 &&
        !regions->imageOffset.x && !regions->imageOffset.y && !regions->imageOffset.z && regions->imageExtent.depth == 1) {
        uint8_t args[28]; const uint32_t a[] = {c->id, im->id, b->id, layout, regions->imageExtent.width, regions->imageExtent.height, direction};
        for (unsigned i = 0; i < 7; ++i) mb_put_u32(args + i * 4, a[i]);
        r = interop_rpc(c->owner, MB_COMMAND_COPY, args, 28, NULL, 0);
    }
    submit_void_error(c->owner, "buffer/image copy", r);
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdCopyImageToBuffer(VkCommandBuffer c, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { interop_copy_image(c, im, l, b, n, r, 0); }
static VKAPI_ATTR void VKAPI_CALL proxy_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { interop_copy_image(c, im, l, b, n, r, 1); }
static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckInteropTEST(VkDevice device, uint32_t op, const struct dd_interop_input *in, struct dd_interop_output *out) {
    if (!device || !in || !out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof(*out)); struct proxy_logical *d = (struct proxy_logical *)device;
    uint8_t args[12], reply[36]; VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (op == DD_AHB_CREATE) {
        struct proxy_resource *im = calloc(1, sizeof(*im)), *m = calloc(1, sizeof(*m)), *a = calloc(1, sizeof(*a));
        if (!im || !m || !a) { free(im); free(m); free(a); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        mb_put_u32(args, in->width); mb_put_u32(args + 4, in->height);
        r = interop_rpc(d, MB_AHB_CREATE, args, 8, reply, 36);
        if (r == VK_SUCCESS && (!mb_get_u32(reply) || !mb_get_u32(reply + 4) || !mb_get_u32(reply + 8) || mb_get_u32(reply) == mb_get_u32(reply + 4) || mb_get_u32(reply) == mb_get_u32(reply + 8) || mb_get_u32(reply + 4) == mb_get_u32(reply + 8))) { shutdown(d->owner->fd, SHUT_RDWR); r = VK_ERROR_INITIALIZATION_FAILED; }
        if (r != VK_SUCCESS) { free(im); free(m); free(a); return r; }
        interop_publish(d, im, PROXY_IMAGE, mb_get_u32(reply)); interop_publish(d, m, PROXY_MEMORY, mb_get_u32(reply + 4)); interop_publish(d, a, PROXY_AHB, mb_get_u32(reply + 8));
        out->image = (VkImage)(uintptr_t)im; out->memory = (VkDeviceMemory)(uintptr_t)m; out->token = a->id;
        if (d->owner->wire_version == MB_RENDERER_VERSION) LOG("BLIT final AHB image broker ID=%u memory ID=%u AHB token=%u", im->id, m->id, a->id);
        out->width = mb_get_u32(reply + 12); out->height = mb_get_u32(reply + 16); out->format = mb_get_u32(reply + 20); out->layers = mb_get_u32(reply + 24); out->usage = mb_get_u64(reply + 28); return VK_SUCCESS;
    }
    if (op == DD_FENCE_CREATE || op == DD_SYNC_EXPORT) {
        struct proxy_resource *o;
        uint32_t n = 4, rpc_op = MB_SYNC_FENCE_CREATE; enum proxy_resource_kind kind = PROXY_FENCE;
        mb_put_u32(args, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
        if (op == DD_SYNC_EXPORT) {
            struct proxy_resource *f = submit_find(d, (uintptr_t)in->fence, PROXY_FENCE); if (!f) return VK_ERROR_INITIALIZATION_FAILED;
            mb_put_u32(args, f->id); mb_put_u32(args + 4, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT); n = 8; rpc_op = MB_SYNC_EXPORT; kind = PROXY_SYNC;
        }
        r = interop_new(d, rpc_op, kind, args, n, &o);
        if (r == VK_SUCCESS) { if (kind == PROXY_FENCE) out->fence = (VkFence)(uintptr_t)o; else out->sync = o->id; } return r;
    }
    struct proxy_resource *a = NULL, *sync = NULL;
    pthread_mutex_lock(&d->owner->lock);
    for (struct proxy_resource *o = d->resources; o; o = o->next) if (o->live) {
        if (o->kind == PROXY_AHB && o->id == in->token) a = o;
        if (o->kind == PROXY_SYNC && o->id == in->sync) sync = o;
    }
    pthread_mutex_unlock(&d->owner->lock);
    if (op == DD_SYNC_WAIT || op == DD_SYNC_CLOSE) {
        if (!sync) return VK_ERROR_INITIALIZATION_FAILED;
        mb_put_u32(args, sync->id); mb_put_u32(args + 4, 5000);
        r = interop_rpc(d, op == DD_SYNC_WAIT ? MB_SYNC_WAIT : MB_SYNC_CLOSE, args, op == DD_SYNC_WAIT ? 8 : 4, NULL, 0);
        if (r == VK_SUCCESS && op == DD_SYNC_CLOSE) sync->live = 0;
    } else if (op == DD_AHB_RELEASE) {
        if (!a) return VK_ERROR_INITIALIZATION_FAILED;
        mb_put_u32(args, a->id); r = interop_rpc(d, MB_AHB_RELEASE, args, 4, NULL, 0); if (r == VK_SUCCESS) a->live = 0;
    } else if (op == DD_AHB_INSPECT || op == DD_AHB_PRESENT) {
        if (!a || !sync) return VK_ERROR_INITIALIZATION_FAILED;
        mb_put_u32(args, a->id); mb_put_u32(args + 4, sync->id); mb_put_u32(args + 8, in->pattern);
        r = interop_rpc(d, op == DD_AHB_INSPECT ? MB_AHB_INSPECT : MB_AHB_PRESENT, args, op == DD_AHB_INSPECT ? 12 : 8, reply, op == DD_AHB_INSPECT ? 4 : 0);
        if (r == VK_SUCCESS && op == DD_AHB_INSPECT) out->cpu_checked = mb_get_u32(reply);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckSessionTEST(VkDevice device, uint32_t op, const struct dd_interop_input *in, struct dd_session_output *out) {
    if (!device || !in || !out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof(*out)); struct proxy_logical *d = (struct proxy_logical *)device;
    uint8_t args[8], reply[100]; VkResult r;
    if (op >= DD_SESSION_BEGIN && op <= DD_SESSION_STATS) {
        const char *enabled = getenv("MALI_VULKAN_SESSION_TEST");
        if (d->owner->wire_version != MB_RENDERER_VERSION || !enabled || strcmp(enabled, "1")) return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t rpc_op = op == DD_SESSION_BEGIN ? MB_SESSION_BEGIN : op == DD_SESSION_PRESENT ? MB_SESSION_PRESENT : op == DD_SESSION_END ? MB_SESSION_END : MB_SESSION_STATS;
        mb_put_u32(args, in->token); mb_put_u32(args + 4, in->sync);
        uint32_t expected = op == DD_SESSION_STATS ? 100 : op == DD_SESSION_BEGIN ? 0 : 4;
        r = interop_rpc(d, rpc_op, args, op == DD_SESSION_PRESENT ? 8 : 0, reply, expected);
        if (r == VK_SUCCESS && op == DD_SESSION_BEGIN) session_frames = 0;
        if (r == VK_SUCCESS && op == DD_SESSION_PRESENT) ++session_frames;
        if (r == VK_SUCCESS && op == DD_SESSION_STATS) {
            for (unsigned i = 0; i < 20; ++i) out->counts[i] = mb_get_u32(reply + i * 4);
            out->presented = mb_get_u32(reply + 80); out->released = mb_get_u32(reply + 84);
            out->fds_created = mb_get_u32(reply + 88); out->fds_closed = mb_get_u32(reply + 92); out->max_owned = mb_get_u32(reply + 96);
        } else if (r == VK_SUCCESS && expected) out->token = mb_get_u32(reply);
        return r;
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;

}

static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckPerformanceMALI(VkDevice device, struct dd_perf_rpc *out, VkBool32 reset) {
    if (!device || !out) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_instance *s = ((struct proxy_logical *)device)->owner;
    if (!s->perf_enabled || s->wire_version != MB_RENDERER_VERSION) return VK_ERROR_FEATURE_NOT_PRESENT;
    pthread_mutex_lock(&s->lock);
    *out = s->perf;
    if (reset) memset(&s->perf, 0, sizeof(s->perf));
    pthread_mutex_unlock(&s->lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckWaylandMALI(VkDevice device, uint32_t op, const struct dd_normal_input *in, struct dd_normal_output *out) {
    const char *enabled = getenv("MALI_VULKAN_NORMAL_SESSION");
    if (!device || !in || !out || !enabled || strcmp(enabled, "1")) return VK_ERROR_FEATURE_NOT_PRESENT;
    memset(out, 0, sizeof(*out)); struct proxy_logical *d = (struct proxy_logical *)device;
    if (d->owner->wire_version != MB_RENDERER_VERSION || op < MB_NORMAL_BEGIN || op > MB_NORMAL_STATS) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t args[12], reply[104]; uint32_t n = 0, expected = 0;
    if (op == MB_NORMAL_BEGIN) { mb_put_u32(args, in->verbose); n = 4; }
    if (op == MB_NORMAL_REGISTER) { mb_put_u32(args, in->token); n = 4; expected = 4; }
    if (op == MB_NORMAL_PUBLISH) { mb_put_u32(args, in->token); mb_put_u32(args + 4, in->sync); mb_put_u32(args + 8, in->frame); n = 12; }
    if (op == MB_NORMAL_STATS) expected = 104;
    VkResult r = interop_rpc(d, op, args, n, reply, expected);
    if (r == VK_SUCCESS && op == MB_NORMAL_REGISTER) out->key = mb_get_u32(reply);
    if (r == VK_SUCCESS && op == MB_NORMAL_STATS) {
        for (unsigned i = 0; i < 20; ++i) out->counts[i] = mb_get_u32(reply + i * 4);
        out->presented = mb_get_u32(reply + 80); out->released = mb_get_u32(reply + 84);
        out->owned = mb_get_u32(reply + 88); out->timeouts = mb_get_u32(reply + 92);
        out->fds_created = mb_get_u32(reply + 96); out->fds_closed = mb_get_u32(reply + 100);
    }
    return r;
}
