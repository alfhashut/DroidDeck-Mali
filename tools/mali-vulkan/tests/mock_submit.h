/* HOST MOCK ONLY: CmdSetEvent records; execution happens at completion, never on host recording. */
static atomic_int pools_created, pools_destroyed, commands_allocated, commands_freed;
static atomic_int events_created, events_destroyed, fences_created, fences_destroyed, submits, executions, idle_calls;
struct mock_object {
    struct mock_logical *device;
    unsigned kind, state, pending;
    struct mock_object *pool, *event, *fence;
    unsigned op_count; struct mock_op ops[64];
    int exportable, exported, writer;
};
static struct mock_object *mock_object_new(VkDevice device, unsigned kind) {
    struct mock_object *o = calloc(1, sizeof(*o)); assert(o); o->device = (struct mock_logical *)device; o->kind = kind; o->writer = -1; return o;
}
static void mock_child(VkDevice device, struct mock_object *o, unsigned kind) { assert(o && o->device == (struct mock_logical *)device && o->kind == kind); }
static void mock_execute(struct mock_object *c) {
    assert(c->pending && c->state == 2 && (c->event || c->op_count));
    mock_interop_execute(c);
    if (c->event) { c->event->state = mode == 25 ? VK_EVENT_RESET : VK_EVENT_SET; c->event->pending = 0; }
    if (c->fence) { c->fence->state = 1; c->fence->pending = 0;
        if (c->fence->writer >= 0) { assert(write(c->fence->writer, "x", 1) == 1); close(c->fence->writer); c->fence->writer = -1; }
    }
    c->pending = 0; ++executions;
}
static VkResult mock_CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    assert(!a && !ci->pNext && !ci->flags && ci->queueFamilyIndex == ((struct mock_logical *)device)->family);
    if (mode == 15) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkCommandPool)mock_object_new(device, 1); ++pools_created; return VK_SUCCESS;
}
static void mock_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *a) {
    struct mock_object *p = (struct mock_object *)pool; assert(!a); mock_child(device, p, 1); free(p); ++pools_destroyed;
}
static VkResult mock_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    mock_child(device, (struct mock_object *)ai->commandPool, 1);
    assert(!ai->pNext && ai->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY && ai->commandBufferCount == 1);
    if (mode == 16) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct mock_object *c = mock_object_new(device, 2); c->pool = (struct mock_object *)ai->commandPool;
    struct mock_logical *d = (struct mock_logical *)device; unsigned slot = 0;
    while (slot < MB_SUBMIT_MAX_OBJECTS && d->commands[slot]) ++slot;
    assert(slot < MB_SUBMIT_MAX_OBJECTS); d->commands[slot] = c;
    *out = (VkCommandBuffer)c; ++commands_allocated; return VK_SUCCESS;
}
static void mock_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *buffers) {
    assert(count == 1); struct mock_object *c = (struct mock_object *)buffers[0]; mock_child(device, c, 2);
    struct mock_logical *d = (struct mock_logical *)device; assert(c->pool == (struct mock_object *)pool && (!c->pending || d->lost));
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->commands[i] == c) d->commands[i] = NULL;
    free(c); ++commands_freed;
}
static VkResult mock_BeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo *bi) {
    struct mock_object *c = (struct mock_object *)buffer; assert(c->kind == 2 && !c->state && !bi->flags && !bi->pNext && !bi->pInheritanceInfo);
    if (mode == 18) return VK_ERROR_OUT_OF_HOST_MEMORY;
    c->state = 1; return VK_SUCCESS;
}
static VkResult mock_EndCommandBuffer(VkCommandBuffer buffer) {
    struct mock_object *c = (struct mock_object *)buffer; assert(c->kind == 2 && c->state == 1 && (c->event || c->op_count));
    if (mode == 19) return VK_ERROR_OUT_OF_HOST_MEMORY;
    c->state = 2; return VK_SUCCESS;
}
static VkResult mock_CreateEvent(VkDevice device, const VkEventCreateInfo *ci, const VkAllocationCallbacks *a, VkEvent *out) {
    assert(!a && !ci->pNext && !ci->flags);
    if (mode == 17) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct mock_object *e = mock_object_new(device, 3); e->state = VK_EVENT_RESET;
    *out = (VkEvent)e; ++events_created; return VK_SUCCESS;
}
static void mock_DestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks *a) {
    struct mock_object *e = (struct mock_object *)event; assert(!a); mock_child(device, e, 3);
    assert(!e->pending || ((struct mock_logical *)device)->lost); free(e); ++events_destroyed;
}
static VkResult mock_GetEventStatus(VkDevice device, VkEvent event) {
    struct mock_object *e = (struct mock_object *)event; mock_child(device, e, 3); return (VkResult)e->state;
}
static void mock_CmdSetEvent(VkCommandBuffer buffer, VkEvent event, VkPipelineStageFlags stage) {
    struct mock_object *c = (struct mock_object *)buffer, *e = (struct mock_object *)event;
    assert(c->kind == 2 && c->state == 1 && !c->event && c->device == e->device && stage == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    c->event = e; /* Event remains RESET until mock queue execution. */
}
static VkResult mock_CreateFence(VkDevice device, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    assert(!a && !ci->flags);
    const VkExportFenceCreateInfo *export = ci->pNext;
    if (export) assert(export->sType == VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO && !export->pNext && export->handleTypes == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
    if (mode == 20) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkFence)mock_object_new(device, 4); ((struct mock_object *)*out)->exportable = export != NULL; ++fences_created; return VK_SUCCESS;
}
static void mock_DestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *a) {
    struct mock_object *f = (struct mock_object *)fence; assert(!a); mock_child(device, f, 4);
    assert(!f->pending || ((struct mock_logical *)device)->lost); free(f); ++fences_destroyed;
}
static VkResult mock_GetFenceStatus(VkDevice device, VkFence fence) {
    struct mock_object *f = (struct mock_object *)fence; mock_child(device, f, 4);
    if (mode == 26 && f->state) { ((struct mock_logical *)device)->lost = 1; return VK_ERROR_DEVICE_LOST; }
    return f->state ? VK_SUCCESS : VK_NOT_READY;
}
static VkResult mock_WaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    assert(count == 1 && all <= 1 && timeout <= MB_SUBMIT_TIMEOUT_NS);
    struct mock_object *f = (struct mock_object *)fences[0]; mock_child(device, f, 4);
    struct mock_logical *d = (struct mock_logical *)device;
    if (mode == 24) { d->lost = 1; return VK_ERROR_DEVICE_LOST; }
    if (mode == 22 || mode == 27 || !timeout) return VK_TIMEOUT;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->commands[i] && d->commands[i]->pending && d->commands[i]->fence == f) mock_execute(d->commands[i]);
    return f->state ? VK_SUCCESS : VK_TIMEOUT;
}
static VkResult mock_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    struct mock_logical *d = (struct mock_logical *)queue;
    assert(count == 1 && !si->pNext && !si->waitSemaphoreCount && !si->signalSemaphoreCount && si->commandBufferCount == 1);
    struct mock_object *c = (struct mock_object *)si->pCommandBuffers[0], *f = (struct mock_object *)fence;
    assert(c->device == d && c->state == 2 && !c->pending && (c->event || c->op_count));
    if (mode == 21) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (mode == 23) { d->lost = 1; return VK_ERROR_DEVICE_LOST; }
    if (f) { mock_child((VkDevice)d, f, 4); assert(!f->state && !f->pending); f->pending = 1; }
    c->fence = f; c->pending = 1; if (c->event) c->event->pending = 1; ++submits; return VK_SUCCESS;
}
static VkResult mock_DeviceWaitIdle(VkDevice device) {
    struct mock_logical *d = (struct mock_logical *)device;
    if (mode == 27 && !d->idle_retries++) { ++idle_calls; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    ++idle_calls;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->commands[i] && d->commands[i]->pending) mock_execute(d->commands[i]);
    return VK_SUCCESS;
}
