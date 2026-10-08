int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    if (mode == 34) return -22;
    AHardwareBuffer *a = calloc(1, sizeof(*a)); assert(a); a->desc = *desc; a->desc.stride = desc->width;
    if (mode == 35) a->desc.usage &= ~AHARDWAREBUFFER_USAGE_CPU_READ_MASK;
    a->pixels = calloc((size_t)desc->width * desc->height, 4); assert(a->pixels); a->refs = 1; *out = a; ++ahb_created; return 0;
}
void AHardwareBuffer_acquire(AHardwareBuffer *a) { assert(a && a->refs); ++a->refs; }
void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a && a->refs); if (!--a->refs) { free(a->pixels); free(a); ++ahb_freed; } }
void AHardwareBuffer_describe(const AHardwareBuffer *a, AHardwareBuffer_Desc *desc) { assert(a && a->refs); *desc = a->desc; }
int AHardwareBuffer_lock(AHardwareBuffer *a, uint64_t usage, int fd, const void *rect, void **out) {
    assert(a->refs && fd == -1 && !rect && usage == AHARDWAREBUFFER_USAGE_CPU_READ_RARELY && (a->desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK));
    ++cpu_locks; *out = a->pixels; return 0;
}
int AHardwareBuffer_unlock(AHardwareBuffer *a, int *fd) { assert(a && a->refs); *fd = -1; return 0; }
int mb_consumer_present(AHardwareBuffer *a, const AHardwareBuffer_Desc *desc, int fd) {
    assert(a && a->refs && desc->width == a->desc.width && desc->height == a->desc.height);
    if (fd >= 0) { struct pollfd p = {.fd = fd, .events = POLLIN}; assert(poll(&p, 1, 0) == 1 && (p.revents & POLLIN)); }
    AHardwareBuffer_acquire(a); ++consumers;
    assert(a->pixels[0] == 255 && a->pixels[1] == 0 && a->pixels[2] == 0 && a->pixels[3] == 255);
    usleep(1000); AHardwareBuffer_release(a); return mode == 41 ? -1 : 0;
}
static VkResult mock_CreateBuffer(VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->sharingMode == VK_SHARING_MODE_EXCLUSIVE);
    if (mode == 30) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct mock_storage *b = calloc(1, sizeof(*b)); assert(b); b->device = (struct mock_logical *)device; b->size = ci->size;
    *out = (VkBuffer)b; ++buffers_created; return VK_SUCCESS;
}
static void mock_DestroyBuffer(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks *a) {
    struct mock_storage *b = (void *)buffer; assert(!a && !b->image && b->device == (struct mock_logical *)d); free(b); ++buffers_destroyed;
}
static void mock_GetBufferMemoryRequirements(VkDevice d, VkBuffer buffer, VkMemoryRequirements *r) {
    struct mock_storage *b = (void *)buffer; assert(b->device == (struct mock_logical *)d); *r = (VkMemoryRequirements){(b->size + 255) & ~UINT64_C(255), 256, 2};
}
static VkResult mock_CreateImage(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    assert(!a && !ci->flags && ci->format == VK_FORMAT_R8G8B8A8_UNORM && ci->tiling == VK_IMAGE_TILING_OPTIMAL && ci->mipLevels == 1 && ci->arrayLayers == 1);
    if (mode == 31) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (ci->pNext) { const VkExternalMemoryImageCreateInfo *e = ci->pNext; assert(e->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO && e->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID && !e->pNext); }
    struct mock_storage *im = calloc(1, sizeof(*im)); assert(im); im->device = (struct mock_logical *)device; im->image = 1; im->external = ci->pNext != NULL; im->width = ci->extent.width; im->height = ci->extent.height; im->size = (uint64_t)im->width * im->height * 4;
    *out = (VkImage)im; ++images_created; return VK_SUCCESS;
}
static void mock_DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a) {
    struct mock_storage *im = (void *)image; assert(!a && im->image && im->device == (struct mock_logical *)d); free(im); ++images_destroyed;
}
static void mock_GetImageMemoryRequirements(VkDevice d, VkImage image, VkMemoryRequirements *r) {
    struct mock_storage *im = (void *)image; assert(im->device == (struct mock_logical *)d && (!im->external || im->memory)); *r = (VkMemoryRequirements){im->size, 256, 1};
}
static VkResult mock_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    assert(!a && ai->allocationSize && ai->memoryTypeIndex < 2);
    if (mode == 32) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct mock_memory *m = calloc(1, sizeof(*m)); assert(m); m->device = (struct mock_logical *)device; m->size = ai->allocationSize; m->type = ai->memoryTypeIndex;
    if (ai->pNext) {
        const VkImportAndroidHardwareBufferInfoANDROID *import = ai->pNext;
        assert(import->sType == VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID && import->buffer && import->pNext);
        const VkMemoryDedicatedAllocateInfo *di = import->pNext;
        assert(di->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && di->image && !di->buffer && !di->pNext);
        struct mock_storage *im = (void *)di->image; assert(im->external && im->device == m->device && import->buffer->desc.width == im->width && import->buffer->desc.height == im->height);
        assert(ai->allocationSize == (uint64_t)im->width * im->height * 4 && ai->memoryTypeIndex == 0);
        m->ahb = import->buffer; AHardwareBuffer_acquire(m->ahb); m->gpu = m->ahb->pixels;
    } else { m->gpu = calloc(1, (size_t)m->size); assert(m->gpu); }
    m->host = mode == 29 && m->type == 1 ? calloc(1, (size_t)m->size) : m->gpu; assert(m->host);
    *out = (VkDeviceMemory)m; ++memories_created; return VK_SUCCESS;
}
static void mock_FreeMemory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    struct mock_memory *m = (void *)memory; assert(!a && m->device == (struct mock_logical *)d && !m->mapped);
    if (m->host != m->gpu) free(m->host);
    if (m->ahb) AHardwareBuffer_release(m->ahb); else free(m->gpu);
    free(m); ++memories_freed;
}
static VkResult mock_BindBufferMemory(VkDevice d, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    struct mock_storage *b = (void *)buffer; struct mock_memory *m = (void *)memory;
    assert(b->device == (struct mock_logical *)d && m->device == b->device && !b->memory && m->type == 1 && !(offset % 256) && offset + b->size <= m->size);
    b->memory = m; b->offset = offset; return VK_SUCCESS;
}
static VkResult mock_BindImageMemory(VkDevice d, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    struct mock_storage *im = (void *)image; struct mock_memory *m = (void *)memory;
    assert(im->device == (struct mock_logical *)d && m->device == im->device && !im->memory && m->type == 0 && !(offset % 256) && offset + im->size <= m->size);
    im->memory = m; im->offset = offset; return VK_SUCCESS;
}
static VkResult mock_MapMemory(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    struct mock_memory *m = (void *)memory; assert(m->device == (struct mock_logical *)d && !m->mapped && !flags && m->type == 1 && offset + size <= m->size);
    m->mapped = 1; *out = m->host + offset; return VK_SUCCESS;
}
static void mock_UnmapMemory(VkDevice d, VkDeviceMemory memory) { struct mock_memory *m = (void *)memory; assert(m->device == (struct mock_logical *)d && m->mapped); m->mapped = 0; }
static VkResult mock_FlushMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) {
    struct mock_memory *m = (void *)r->memory; assert(n == 1 && m->device == (struct mock_logical *)d && m->mapped && !r->pNext); memcpy(m->gpu + r->offset, m->host + r->offset, (size_t)r->size); ++flushes; return VK_SUCCESS;
}
static VkResult mock_InvalidateMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) {
    struct mock_memory *m = (void *)r->memory; assert(n == 1 && m->device == (struct mock_logical *)d && m->mapped && !r->pNext); memcpy(m->host + r->offset, m->gpu + r->offset, (size_t)r->size); ++invalidates; return VK_SUCCESS;
}
static struct mock_op *mock_record(VkCommandBuffer command, unsigned kind) {
    struct mock_object *c = (void *)command; assert(c->state == 1 && c->op_count < 64); struct mock_op *op = &c->ops[c->op_count++]; op->kind = kind; return op;
}
static void mock_CmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize offset, VkDeviceSize size, uint32_t pattern) {
    struct mock_op *op = mock_record(c, 1); op->buffer = (void *)b; op->offset = offset; op->size = size; op->pattern = pattern;
}
static void mock_CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *b, uint32_t ic, const VkImageMemoryBarrier *im) {
    (void)s; (void)d; (void)b; (void)im; assert(!flags && !mc && !m && bc + ic == 1); (void)mock_record(c, 4);
}
static void mock_CmdClearColorImage(VkCommandBuffer c, VkImage im, VkImageLayout l, const VkClearColorValue *color, uint32_t n, const VkImageSubresourceRange *r) {
    assert(l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n == 1 && r->levelCount == 1 && r->layerCount == 1);
    struct mock_op *op = mock_record(c, 2); op->image = (void *)im;
    for (unsigned i = 0; i < 4; ++i) op->color[i] = (uint8_t)(color->float32[i] * 255.0f + 0.5f);
}
static void mock_copy(VkCommandBuffer c, VkImage im, VkBuffer b, uint32_t n, const VkBufferImageCopy *r, unsigned kind) {
    struct mock_op *op = mock_record(c, kind); assert(n == 1 && !r->bufferOffset && !r->bufferRowLength && !r->bufferImageHeight); op->image = (void *)im; op->buffer = (void *)b;
    assert(r->imageExtent.width == op->image->width && r->imageExtent.height == op->image->height && r->imageExtent.depth == 1);
}
static void mock_CmdCopyImageToBuffer(VkCommandBuffer c, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { assert(l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); mock_copy(c, im, b, n, r, 3); }
static void mock_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { assert(l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); mock_copy(c, im, b, n, r, 5); }
static void mock_interop_execute(struct mock_object *c) {
    for (unsigned i = 0; i < c->op_count; ++i) {
        struct mock_op *op = &c->ops[i]; struct mock_storage *b = op->buffer, *im = op->image;
        if (op->kind == 1) for (uint64_t j = 0; j < op->size; j += 4) memcpy(b->memory->gpu + b->offset + op->offset + j, &op->pattern, 4);
        if (op->kind == 2) for (uint64_t j = 0; j < im->size; j += 4) memcpy(im->memory->gpu + im->offset + j, op->color, 4);
        if (op->kind == 3) memcpy(b->memory->gpu + b->offset, im->memory->gpu + im->offset, (size_t)im->size);
        if (op->kind == 5) memcpy(im->memory->gpu + im->offset, b->memory->gpu + b->offset, (size_t)im->size);
        if (mode == 36 && op->kind == 3) b->memory->gpu[b->offset] ^= 255; /* Channel/content corruption must fail. */
    }
}
static VkResult mock_GetAndroidHardwareBufferPropertiesANDROID(VkDevice d, const AHardwareBuffer *a, VkAndroidHardwareBufferPropertiesANDROID *p) {
    assert(d && a && p->pNext); VkAndroidHardwareBufferFormatPropertiesANDROID *f = p->pNext;
    assert(f->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
    p->allocationSize = (uint64_t)a->desc.width * a->desc.height * 4; p->memoryTypeBits = 1;
    f->format = VK_FORMAT_R8G8B8A8_UNORM; f->formatFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    return mode == 37 ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_SUCCESS;
}
static VkResult mock_GetFenceFdKHR(VkDevice device, const VkFenceGetFdInfoKHR *info, int *out) {
    struct mock_object *f = (void *)info->fence; struct mock_logical *d = (void *)device;
    assert(f->device == d && f->exportable && !f->exported && info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
    if (mode == 38) return VK_ERROR_TOO_MANY_OBJECTS;
    int p[2]; assert(pipe(p) == 0); *out = p[0]; f->writer = p[1]; f->exported = 1;
    if (mode != 40) for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->commands[i] && d->commands[i]->pending && d->commands[i]->fence == f) mock_execute(d->commands[i]);
    return VK_SUCCESS;
}
