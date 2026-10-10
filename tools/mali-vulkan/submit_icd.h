/* Local opaque resources and a loader-dispatchable primary command buffer. */
static void proxy_free_resources(struct proxy_logical *d) {
    free(d->write_request); d->write_request = NULL; d->write_capacity = 0;
    free(d->read_reply); d->read_reply = NULL; d->read_capacity = 0;
    proxy_registry_cleanup(&d->registry);
}
static struct proxy_resource *submit_find(struct proxy_logical *d, uintptr_t handle, enum proxy_resource_kind kind) {
    struct proxy_resource *found = NULL;
    pthread_mutex_lock(&d->owner->lock);
    uint64_t visited = 0;
    found = proxy_registry_find(&d->registry, handle, kind, &visited);
    if (d->owner->perf_enabled) {
        struct dd_memory_profile *p = &d->owner->memory_perf;
        ++p->lookups; p->visited += visited;
        if (visited > p->max_visit) p->max_visit = visited;
    }
    pthread_mutex_unlock(&d->owner->lock);
    return found;
}
/* Logical retirement occurs only after the existing successful native ACK. */
static void submit_retire(struct proxy_logical *d, struct proxy_resource *o) {
    pthread_mutex_lock(&d->owner->lock);
    proxy_registry_retire(&d->registry, o);
    pthread_mutex_unlock(&d->owner->lock);
}
static VkResult submit_rpc(struct proxy_logical *d, uint32_t op, const uint32_t *args, unsigned n,
        uint64_t timeout, uint32_t *id) {
    if (!d || d->owner->wire_version < MB_SUBMIT_VERSION) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint8_t request[24], reply[MB_PREFIX_BYTES + 4]; uint32_t bytes = 0;
    mb_put_u32(request, d->id);
    for (unsigned i = 0; i < n; ++i) mb_put_u32(request + 4 + i * 4, args[i]);
    uint32_t size = 4 + n * 4;
    if (op == MB_FENCE_WAIT) { mb_put_u64(request + size, timeout); size += 8; }
    struct proxy_instance *s = d->owner;
    pthread_mutex_lock(&s->lock);
    VkResult result = rpc(s, op, request, size, reply, &bytes, sizeof(reply));
    if (bytes && (bytes != MB_PREFIX_BYTES + (id && result == VK_SUCCESS ? 4u : 0u) ||
        mb_get_u32(reply + 8) != (id && result == VK_SUCCESS ? 1u : 0u) ||
        (id && result == VK_SUCCESS && !mb_get_u32(reply + 12)))) {
        shutdown(s->fd, SHUT_RDWR); result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (id && result == VK_SUCCESS) *id = mb_get_u32(reply + 12);
    pthread_mutex_unlock(&s->lock);
    return result;
}
static VkResult submit_new(struct proxy_logical *d, uint32_t op, enum proxy_resource_kind kind,
        const uint32_t *args, unsigned n, struct proxy_resource **out) {
    *out = NULL;
    struct proxy_resource *o = calloc(1, sizeof(*o));
    if (!o) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = submit_rpc(d, op, args, n, 0, &o->id);
    if (result != VK_SUCCESS) { free(o); return result; }
    set_loader_magic_value(o); o->owner = d; o->kind = kind; o->live = 1;
    if (kind == PROXY_COMMAND) o->pool = args[0];
    pthread_mutex_lock(&d->owner->lock);
    proxy_registry_insert(&d->registry, o); *out = o;
    pthread_mutex_unlock(&d->owner->lock);
    return VK_SUCCESS;
}
static void submit_void_error(struct proxy_logical *d, const char *name, VkResult r) {
    if (r != VK_SUCCESS) { LOG("%s rejected/result=%d; no successful recording assumed", name, (int)r); int expected = 0; atomic_compare_exchange_strong(&d->submit_failed, &expected, d->owner->wire_version == MB_RENDERER_VERSION ? (int)r : 1); }
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *ci,
        const VkAllocationCallbacks *a, VkCommandPool *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ci || a || ci->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || ci->pNext || (ci->flags && (((struct proxy_logical *)device)->owner->wire_version != MB_RENDERER_VERSION || ci->flags != VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device;
    if (ci->queueFamilyIndex != d->family) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t args[] = {ci->queueFamilyIndex, ci->flags}; struct proxy_resource *o;
    VkResult r = submit_new(d, MB_POOL_CREATE, PROXY_POOL, args, 2, &o);
    if (r == VK_SUCCESS) *out = (VkCommandPool)(uintptr_t)o;
    return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *a) {
    if (!device || !pool) return;
    struct proxy_logical *d = (struct proxy_logical *)device;
    struct proxy_resource *o = submit_find(d, (uintptr_t)pool, PROXY_POOL);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (o && !a) { uint32_t args[] = {o->id}; r = submit_rpc(d, MB_POOL_DESTROY, args, 1, 0, NULL); }
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&d->owner->lock);
        proxy_registry_retire_children(&d->registry, o, PROXY_COMMAND);
        pthread_mutex_unlock(&d->owner->lock);
    }
    submit_void_error(d, "vkDestroyCommandPool", r);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (!device || !ai || ai->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || ai->pNext ||
        ai->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || ai->commandBufferCount != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device;
    struct proxy_resource *p = submit_find(d, (uintptr_t)ai->commandPool, PROXY_POOL), *o;
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t args[] = {p->id, ai->level, ai->commandBufferCount};
    VkResult r = submit_new(d, MB_COMMAND_ALLOCATE, PROXY_COMMAND, args, 3, &o);
    if (r == VK_SUCCESS) *out = (VkCommandBuffer)o;
    return r;
}
static VKAPI_ATTR void VKAPI_CALL proxy_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *buffers) {
    if (!device) return;
    struct proxy_logical *d = (struct proxy_logical *)device;
    struct proxy_resource *p = submit_find(d, (uintptr_t)pool, PROXY_POOL);
    struct proxy_resource *c = count == 1 && buffers ? submit_find(d, (uintptr_t)buffers[0], PROXY_COMMAND) : NULL;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (p && c && c->pool == p->id) { uint32_t args[] = {p->id, c->id}; r = submit_rpc(d, MB_COMMAND_FREE, args, 2, 0, NULL); }
    if (r == VK_SUCCESS) submit_retire(d, c);
    submit_void_error(d, "vkFreeCommandBuffers", r);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_BeginCommandBuffer(VkCommandBuffer command, const VkCommandBufferBeginInfo *bi) {
    if (!command) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_resource *c = (struct proxy_resource *)command;
    if (!c->live || c->kind != PROXY_COMMAND) return VK_ERROR_INITIALIZATION_FAILED;
    if (!bi || bi->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || bi->pNext || bi->pInheritanceInfo || (bi->flags && (c->owner->owner->wire_version != MB_RENDERER_VERSION || bi->flags != VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t args[] = {c->id, bi->flags}; return submit_rpc(c->owner, MB_COMMAND_BEGIN, args, 2, 0, NULL);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_EndCommandBuffer(VkCommandBuffer command) {
    if (!command) return VK_ERROR_INITIALIZATION_FAILED;
    struct proxy_resource *c = (struct proxy_resource *)command;
    if (!c->live || c->kind != PROXY_COMMAND) return VK_ERROR_INITIALIZATION_FAILED;
    if (c->owner->submit_failed) return c->owner->owner->wire_version == MB_RENDERER_VERSION ? (VkResult)atomic_load(&c->owner->submit_failed) : VK_ERROR_INITIALIZATION_FAILED;
    uint32_t args[] = {c->id}; return submit_rpc(c->owner, MB_COMMAND_END, args, 1, 0, NULL);
}
static VKAPI_ATTR void VKAPI_CALL proxy_CmdSetEvent(VkCommandBuffer command, VkEvent event, VkPipelineStageFlags stage) {
    if (!command) return;
    struct proxy_resource *c = (struct proxy_resource *)command;
    struct proxy_logical *d = c->owner;
    struct proxy_resource *e = submit_find(d, (uintptr_t)event, PROXY_EVENT);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (c->live && c->kind == PROXY_COMMAND && e && stage == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) {
        uint32_t args[] = {c->id, e->id, stage}; r = submit_rpc(d, MB_COMMAND_SET_EVENT, args, 3, 0, NULL);
    }
    submit_void_error(d, "vkCmdSetEvent", r);
}
#define SIMPLE_CREATE(Name, Type, KIND, OP, STYPE) \
static VKAPI_ATTR VkResult VKAPI_CALL proxy_Create##Name(VkDevice device, const Vk##Name##CreateInfo *ci, \
        const VkAllocationCallbacks *a, Vk##Name *out) { \
    if (!out) return VK_ERROR_INITIALIZATION_FAILED; \
    *out = VK_NULL_HANDLE; \
    if (!device || !ci || a || ci->sType != STYPE || ci->pNext || ci->flags) return VK_ERROR_FEATURE_NOT_PRESENT; \
    uint32_t args[] = {ci->flags}; struct proxy_resource *o; \
    VkResult r = submit_new((struct proxy_logical *)device, OP, KIND, args, 1, &o); \
    if (r == VK_SUCCESS) *out = (Type)(uintptr_t)o; \
    return r; \
}
SIMPLE_CREATE(Event, VkEvent, PROXY_EVENT, MB_EVENT_CREATE, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO)
SIMPLE_CREATE(Fence, VkFence, PROXY_FENCE, MB_FENCE_CREATE, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO)
#undef SIMPLE_CREATE
#define SIMPLE_OBJECT(Name, KIND, DESTROY, STATUS) \
static VKAPI_ATTR void VKAPI_CALL proxy_Destroy##Name(VkDevice device, Vk##Name handle, const VkAllocationCallbacks *a) { \
    if (!device || !handle) return; \
    struct proxy_logical *d = (struct proxy_logical *)device; \
    struct proxy_resource *o = submit_find(d, (uintptr_t)handle, KIND); VkResult r = VK_ERROR_INITIALIZATION_FAILED; \
    if (o && !a) { uint32_t args[] = {o->id}; r = submit_rpc(d, DESTROY, args, 1, 0, NULL); } \
    if (r == VK_SUCCESS) submit_retire(d, o); \
    submit_void_error(d, "vkDestroy" #Name, r); \
} \
static VKAPI_ATTR VkResult VKAPI_CALL proxy_Get##Name##Status(VkDevice device, Vk##Name handle) { \
    if (!device) return VK_ERROR_INITIALIZATION_FAILED; \
    struct proxy_logical *d = (struct proxy_logical *)device; \
    struct proxy_resource *o = submit_find(d, (uintptr_t)handle, KIND); \
    if (!o) return VK_ERROR_INITIALIZATION_FAILED; \
    uint32_t args[] = {o->id}; return submit_rpc(d, STATUS, args, 1, 0, NULL); \
}
SIMPLE_OBJECT(Event, PROXY_EVENT, MB_EVENT_DESTROY, MB_EVENT_STATUS)
SIMPLE_OBJECT(Fence, PROXY_FENCE, MB_FENCE_DESTROY, MB_FENCE_STATUS)
#undef SIMPLE_OBJECT
static VKAPI_ATTR VkResult VKAPI_CALL proxy_WaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    if (!device || count != 1 || !fences || all > 1 || timeout > MB_SUBMIT_TIMEOUT_NS) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_logical *d = (struct proxy_logical *)device;
    struct proxy_resource *f = submit_find(d, (uintptr_t)fences[0], PROXY_FENCE);
    if (!f) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t args[] = {f->id, all}; return submit_rpc(d, MB_FENCE_WAIT, args, 2, timeout, NULL);
}
static VKAPI_ATTR VkResult VKAPI_CALL proxy_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    if (queue && ((struct proxy_queue *)queue)->owner->owner->wire_version == MB_RENDERER_VERSION) return renderer_QueueSubmit(queue, count, si, fence);
    if (!queue || count != 1 || !si || si->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || si->pNext ||
        si->waitSemaphoreCount || si->signalSemaphoreCount || si->commandBufferCount != 1 || !si->pCommandBuffers)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct proxy_queue *q = (struct proxy_queue *)queue;
    struct proxy_logical *d = q->owner;
    struct proxy_resource *c = submit_find(d, (uintptr_t)si->pCommandBuffers[0], PROXY_COMMAND);
    struct proxy_resource *f = fence ? submit_find(d, (uintptr_t)fence, PROXY_FENCE) : NULL;
    if (!q->id || !c || (fence && !f) || d->submit_failed) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t args[] = {q->id, c->id, f ? f->id : 0}; return submit_rpc(d, MB_QUEUE_SUBMIT, args, 3, 0, NULL);
}
