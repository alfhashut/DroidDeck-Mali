/* Narrow synchronous recording RPCs, never a general command stream. */
static int native_submit_init(struct native_device *d) {
#define MB_SUBMIT_ENTRY(n) d->submit.n = (PFN_vk##n)d->gdpa(d->handle, "vk" #n); \
    if (!d->submit.n) return -1;
#include "submit_entries.def"
#undef MB_SUBMIT_ENTRY
    d->submit.idle = (PFN_vkDeviceWaitIdle)d->gdpa(d->handle, "vkDeviceWaitIdle");
    return d->submit.idle ? 0 : -1;
}
#define FIND_CHILD(type, field) \
static struct native_##type *native_find_##type(struct native_device *d, uint32_t id) { \
    if (!id) return NULL; \
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) \
        if (d->submit.field[i].id == id) return &d->submit.field[i]; \
    return NULL; \
}
FIND_CHILD(pool, pools)
FIND_CHILD(command, commands)
FIND_CHILD(event, events)
FIND_CHILD(fence, fences)
#undef FIND_CHILD
static int native_pending(struct native_device *d, uint32_t pool, uint32_t event, uint32_t fence) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_command *c = &d->submit.commands[i];
        if (c->id && c->state == 3 && ((!pool && !event && !fence) ||
            (pool && c->pool == pool) || (event && c->event == event) || (fence && c->fence == fence))) return 1;
    }
    return 0;
}
static void native_complete_fence(struct native_device *d, uint32_t fence) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->submit.commands[i].id && d->submit.commands[i].state == 3 &&
            d->submit.commands[i].fence == fence) {
            native_interop_complete(d, &d->submit.commands[i]); d->submit.commands[i].state = 4;
        }
}
static void native_free_command(struct native_device *d, struct native_command *c) {
    struct native_pool *p = native_find_pool(d, c->pool);
    d->submit.FreeCommandBuffers(d->handle, p->handle, 1, &c->handle);
    LOG("command buffer freed device ID=%u pool ID=%u command ID=%u", d->id, p->id, c->id);
    memset(c, 0, sizeof(*c));
}
static void native_drain_device(struct native_device *d) {
    if (native_pending(d, 0, 0, 0)) {
        /* Never free pending objects after a finite fence timeout or disconnect. */
        VkResult result = d->submit.idle(d->handle);
        LOG("safe cleanup vkDeviceWaitIdle device ID=%u result=%d", d->id, (int)result);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            /* Retry rather than release resources potentially used by the GPU. */
            do {
                usleep(10000); result = d->submit.idle(d->handle);
                LOG("safe cleanup vkDeviceWaitIdle retry device ID=%u result=%d", d->id, (int)result);
            }
            while (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST);
        }
    }
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->submit.commands[i].id && d->submit.commands[i].state == 3) {
            if (!d->submit.lost) native_interop_complete(d, &d->submit.commands[i]);
            d->submit.commands[i].state = 4;
        }
}
static void native_close_device(struct native_device *d) {
    native_drain_device(d);
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_fence *f = &d->submit.fences[i];
        if (f->id) { d->submit.DestroyFence(d->handle, f->handle, NULL); LOG("fence destroyed device ID=%u fence ID=%u (cleanup)", d->id, f->id); }
    }
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_event *e = &d->submit.events[i];
        if (e->id) { d->submit.DestroyEvent(d->handle, e->handle, NULL); LOG("event destroyed device ID=%u event ID=%u (cleanup)", d->id, e->id); }
    }
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
        if (d->submit.commands[i].id) native_free_command(d, &d->submit.commands[i]);
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_pool *p = &d->submit.pools[i];
        if (p->id) { d->submit.DestroyCommandPool(d->handle, p->handle, NULL); LOG("command pool destroyed device ID=%u pool ID=%u (cleanup)", d->id, p->id); }
    }
    native_interop_close(d);
    d->destroy(d->handle, NULL); LOG("vkDestroyDevice id=%u", d->id);
    memset(d, 0, sizeof(*d));
}
static uint32_t native_submit_command(struct vk_session *s, uint32_t op, const uint8_t *wire,
        uint32_t bytes, uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    uint32_t expected = 8;
    switch (op) {
        case MB_POOL_CREATE: case MB_COMMAND_FREE: case MB_COMMAND_BEGIN: expected = 12; break;
        case MB_COMMAND_ALLOCATE: case MB_COMMAND_SET_EVENT: case MB_QUEUE_SUBMIT: expected = 16; break;
        case MB_FENCE_WAIT: expected = 20; break;
    }
    if (bytes != expected) return MB_PROTOCOL_ERROR;
    uint32_t device = mb_get_u32(wire), id = mb_get_u32(wire + 4);
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle && s->logical[i].id == device) { d = &s->logical[i]; break; }
    if (!d || !d->submit.idle) return MB_PROTOCOL_ERROR;
    struct native_submit *v = &d->submit;
    if (v->lost && op != MB_POOL_DESTROY && op != MB_COMMAND_FREE &&
        op != MB_EVENT_DESTROY && op != MB_FENCE_DESTROY) {
        *result = VK_ERROR_DEVICE_LOST; return MB_VULKAN_ERROR;
    }
    uint32_t new_id = 0;
    if (op == MB_POOL_CREATE || op == MB_COMMAND_ALLOCATE || op == MB_EVENT_CREATE || op == MB_FENCE_CREATE) {
        if (s->next_resource_id == UINT32_MAX) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
        new_id = s->next_resource_id + 1;
    }
#define NEW_SLOT(field, type) \
    struct native_##type *o = NULL; \
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (!v->field[i].id) { o = &v->field[i]; break; } \
    if (!o) { *result = VK_ERROR_TOO_MANY_OBJECTS; return MB_VULKAN_ERROR; }
    switch (op) {
    case MB_POOL_CREATE: {
        if (id != d->family || mb_get_u32(wire + 8)) return MB_PROTOCOL_ERROR;
        NEW_SLOT(pools, pool)
        VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = id};
        *result = v->CreateCommandPool(d->handle, &ci, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; o->family = id; LOG("command pool created device ID=%u pool ID=%u family=%u", device, new_id, id); }
        break;
    }
    case MB_POOL_DESTROY: {
        struct native_pool *p = native_find_pool(d, id);
        if (!p || (!v->lost && native_pending(d, id, 0, 0))) return MB_PROTOCOL_ERROR;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
            if (v->commands[i].id && v->commands[i].pool == id) native_free_command(d, &v->commands[i]);
        v->DestroyCommandPool(d->handle, p->handle, NULL);
        LOG("command pool destroyed device ID=%u pool ID=%u", device, id); memset(p, 0, sizeof(*p)); break;
    }
    case MB_COMMAND_ALLOCATE: {
        struct native_pool *p = native_find_pool(d, id);
        if (!p || mb_get_u32(wire + 8) != VK_COMMAND_BUFFER_LEVEL_PRIMARY || mb_get_u32(wire + 12) != 1) return MB_PROTOCOL_ERROR;
        NEW_SLOT(commands, command)
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = p->handle, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        *result = v->AllocateCommandBuffers(d->handle, &ai, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; o->pool = id; o->family = p->family; LOG("primary command allocated device ID=%u pool ID=%u command ID=%u", device, id, new_id); }
        break;
    }
    case MB_COMMAND_FREE: {
        struct native_pool *p = native_find_pool(d, id);
        struct native_command *c = native_find_command(d, mb_get_u32(wire + 8));
        if (!p || !c || c->pool != id || (!v->lost && c->state == 3)) return MB_PROTOCOL_ERROR;
        native_free_command(d, c); break;
    }
    case MB_COMMAND_BEGIN: case MB_COMMAND_END: case MB_COMMAND_SET_EVENT: {
        struct native_command *c = native_find_command(d, id);
        if (!c) return MB_PROTOCOL_ERROR;
        if (op == MB_COMMAND_BEGIN) {
            if (c->state || mb_get_u32(wire + 8)) return MB_PROTOCOL_ERROR;
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            *result = v->BeginCommandBuffer(c->handle, &bi);
            c->state = *result == VK_SUCCESS ? 1 : 5;
            LOG("vkBeginCommandBuffer device ID=%u command ID=%u result=%d", device, id, (int)*result);
        } else if (op == MB_COMMAND_SET_EVENT) {
            struct native_event *e = native_find_event(d, mb_get_u32(wire + 8));
            if (!(d->queue_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) || !e || c->state != 1 || c->event || mb_get_u32(wire + 12) != VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) return MB_PROTOCOL_ERROR;
            v->CmdSetEvent(c->handle, e->handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); c->event = e->id;
            LOG("vkCmdSetEvent recorded device ID=%u command ID=%u event ID=%u stage=ALL_COMMANDS", device, id, e->id);
        } else {
            if (c->state != 1 || (!c->recorded && (!c->event || !native_find_event(d, c->event)))) return MB_PROTOCOL_ERROR;
            *result = v->EndCommandBuffer(c->handle); c->state = *result == VK_SUCCESS ? 2 : 5;
            LOG("vkEndCommandBuffer device ID=%u command ID=%u result=%d", device, id, (int)*result);
        }
        break;
    }
    case MB_EVENT_CREATE: {
        if (id) return MB_PROTOCOL_ERROR;
        NEW_SLOT(events, event)
        VkEventCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        *result = v->CreateEvent(d->handle, &ci, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; LOG("event created device ID=%u event ID=%u (normal RESET event)", device, new_id); }
        break;
    }
    case MB_EVENT_DESTROY: case MB_EVENT_STATUS: {
        struct native_event *e = native_find_event(d, id);
        if (!e) return MB_PROTOCOL_ERROR;
        if (op == MB_EVENT_STATUS) {
            *result = v->GetEventStatus(d->handle, e->handle);
            LOG("vkGetEventStatus device ID=%u event ID=%u result=%d", device, id, (int)*result);
        } else {
            if (!v->lost && native_pending(d, 0, id, 0)) return MB_PROTOCOL_ERROR;
            v->DestroyEvent(d->handle, e->handle, NULL); LOG("event destroyed device ID=%u event ID=%u", device, id);
            memset(e, 0, sizeof(*e));
            /* Vulkan allows destruction here but referenced executable buffers become invalid. */
            for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
                if (v->commands[i].id && v->commands[i].event == id) v->commands[i].state = 5;
        }
        break;
    }
    case MB_FENCE_CREATE: {
        if (id) return MB_PROTOCOL_ERROR;
        NEW_SLOT(fences, fence)
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        *result = v->CreateFence(d->handle, &ci, NULL, &o->handle);
        if (*result == VK_SUCCESS) { o->id = new_id; LOG("unsignaled fence created device ID=%u fence ID=%u", device, new_id); }
        break;
    }
    case MB_FENCE_DESTROY: case MB_FENCE_STATUS: case MB_FENCE_WAIT: {
        struct native_fence *f = native_find_fence(d, id);
        if (!f || (f->exported && op != MB_FENCE_DESTROY)) return MB_PROTOCOL_ERROR;
        if (op == MB_FENCE_DESTROY) {
            if (!v->lost && native_pending(d, 0, 0, id)) return MB_PROTOCOL_ERROR;
            v->DestroyFence(d->handle, f->handle, NULL); LOG("fence destroyed device ID=%u fence ID=%u", device, id);
            memset(f, 0, sizeof(*f));
        } else {
            if (op == MB_FENCE_WAIT) {
                uint32_t all = mb_get_u32(wire + 8); uint64_t timeout = mb_get_u64(wire + 12);
                if (all > 1 || timeout > MB_SUBMIT_TIMEOUT_NS) return MB_PROTOCOL_ERROR;
                *result = v->WaitForFences(d->handle, 1, &f->handle, all, timeout);
            } else *result = v->GetFenceStatus(d->handle, f->handle);
            LOG("%s device ID=%u fence ID=%u result=%d", op == MB_FENCE_WAIT ? "vkWaitForFences" : "vkGetFenceStatus", device, id, (int)*result);
            if (*result == VK_SUCCESS) native_complete_fence(d, id);
        }
        break;
    }
    case MB_QUEUE_SUBMIT: {
        VkQueue queue = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < d->count; ++i) if (d->queue_ids[i] && d->queue_ids[i] == id) queue = d->queues[i];
        struct native_command *c = native_find_command(d, mb_get_u32(wire + 8));
        uint32_t fence_id = mb_get_u32(wire + 12);
        struct native_fence *f = native_find_fence(d, fence_id);
        if (!queue || !c || c->family != d->family || c->state != 2 || (!c->recorded && !native_find_event(d, c->event)) ||
            !native_interop_can_submit(d, c) ||
            (fence_id && (!f || f->submitted))) return MB_PROTOCOL_ERROR;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->handle};
        LOG("queue submission device ID=%u queue ID=%u command ID=%u fence ID=%u", device, id, c->id, fence_id);
        *result = v->QueueSubmit(queue, 1, &si, f ? f->handle : VK_NULL_HANDLE);
        LOG("vkQueueSubmit device ID=%u queue ID=%u result=%d", device, id, (int)*result);
        if (*result == VK_SUCCESS) { c->state = 3; c->fence = fence_id; if (f) f->submitted = 1; }
        break;
    }
    default: return MB_PROTOCOL_ERROR;
    }
#undef NEW_SLOT
    if (*result == VK_ERROR_DEVICE_LOST) {
        v->lost = 1; native_drain_device(d); /* Lost-device child handles still need explicit cleanup. */
    }
    if (*result < 0) return MB_VULKAN_ERROR;
    if (new_id && *result == VK_SUCCESS) {
        s->next_resource_id = new_id; mb_put_u32(reply, new_id); *extra = 4; *count = 1;
    }
    return MB_OK;
}
