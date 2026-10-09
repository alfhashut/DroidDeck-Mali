/* Private v7 persistent-session opt-in, device-owned. No native handle on wire. */
static void native_session_counts(struct native_device *d, uint32_t counts[20]) {
    memset(counts, 0, 20 * sizeof(*counts));
    counts[0] = !!d->handle;
    for (unsigned i = 0; i < MB_DEVICE_MAX_QUEUES; ++i) counts[1] += !!d->queues[i];
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        counts[2] += !!d->submit.pools[i].id; counts[3] += !!d->submit.commands[i].id;
        counts[5] += !!d->submit.fences[i].id; counts[6] += !!d->interop.buffers[i].id;
        counts[7] += !!d->interop.memories[i].id; counts[8] += !!d->interop.images[i].id;
        counts[16] += !!d->interop.ahbs[i].id; counts[17] += !!d->interop.syncs[i].id;
        counts[18] += d->interop.syncs[i].id && d->interop.syncs[i].fd >= 0;
        counts[19] += d->submit.commands[i].id && d->submit.commands[i].state == 3;
    }
    if (d->renderer.objects) for (unsigned i = 0; i < MB_RENDERER_MAX_OBJECTS; ++i) {
        struct native_renderer_object *o = &d->renderer.objects[i];
        if (!o->id) continue;
        switch (o->kind) {
        case MB_R_SEMAPHORE: ++counts[4]; break;
        case MB_R_VIEW: ++counts[9]; break;
        case MB_R_SAMPLER: ++counts[10]; break;
        case MB_R_POOL: ++counts[11]; break;
        case MB_R_SET: ++counts[12]; break;
        case MB_R_SHADER: ++counts[13]; break;
        case MB_R_PIPELINE: ++counts[14]; break;
        case MB_R_PIPELINE_LAYOUT: case MB_R_SET_LAYOUT: ++counts[15]; break;
        }
    }
}
static void native_session_log(struct native_device *d, const char *stage) {
    uint32_t c[20]; native_session_counts(d, c);
    int quiet = session_quiet; session_quiet = 0;
    LOG("session resources %s: devices=%u queues=%u pools=%u commands=%u semaphores=%u fences=%u buffers=%u memory=%u images=%u views=%u samplers=%u descriptorPools=%u sets=%u shaders=%u pipelines=%u layouts=%u AHB=%u sync=%u FDs=%u pending=%u AndroidHeld=%u",
        stage,c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],c[8],c[9],c[10],c[11],c[12],c[13],c[14],c[15],c[16],c[17],c[18],c[19],d->session.held);
    session_quiet = quiet;
}
static VkResult native_session_end(struct native_device *d) {
    if (d->session.release_timeouts) return VK_TIMEOUT;
    uint32_t outstanding = !!d->session.held + !!d->session.pending;
    int result = mb_consumer_session_close(d->session.consumer);
    if (result == VK_TIMEOUT) {
        d->session.release_timeouts = 1; session_quiet = 0;
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t token = i ? d->session.pending : d->session.held;
            if (!token) continue;
            uint32_t sync_id = i ? d->session.pending_sync : d->session.last_sync;
            struct native_sync *sync = interop_find_sync(d, sync_id);
            LOG("Android release wait TIMEOUT: buffer token=%u frame=%u state=ANDROID_OWNED outstanding releases=%u presented=%u released=%u last sync token=%u sync live=%u fence ID=%u sync FD=%d sync completed=%u handoff pending=%u; release timeouts=1",
                token, i ? d->session.pending_frame : d->session.presented, outstanding, d->session.presented, d->session.released,
                sync_id, !!sync, i ? d->session.pending_fence : d->session.last_fence, sync ? sync->fd : -1, sync ? sync->completed : 0, i);
        }
        LOG("Android release wait: outstanding at stop=%u released=0 timed out=%u", outstanding, outstanding);
        return VK_TIMEOUT; /* keep consumer/token/allocation ownership intact */
    }
    d->session.consumer = NULL;
    if (d->session.held) ++d->session.released;
    d->session.held = 0;
    d->session.pending = 0;
    int quiet = session_quiet; session_quiet = 0;
    LOG("Android release wait: outstanding at stop=%u released=%u timed out=0; release timeouts=0", outstanding, outstanding);
    session_quiet = quiet;
    return (VkResult)result;
}
static uint32_t native_session_command(struct vk_session *s, uint32_t op, const uint8_t *w, uint32_t bytes,
        uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    if (bytes != (op == MB_SESSION_PRESENT ? 12u : 4u)) return MB_PROTOCOL_ERROR;
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle && s->logical[i].id == mb_get_u32(w)) d = &s->logical[i];
    if (!d || !d->renderer.enabled || !d->interop.ahb_enabled) return MB_PROTOCOL_ERROR;
    if (op == MB_SESSION_BEGIN) {
        if (d->session.enabled) return MB_PROTOCOL_ERROR;
        d->session.consumer = mb_consumer_session_open();
        if (!d->session.consumer) { *result = VK_ERROR_INITIALIZATION_FAILED; return MB_VULKAN_ERROR; }
        d->session.enabled = 1; native_session_log(d, "start (initialized renderer; connection baseline zero)");
    } else {
        if (!d->session.enabled) return MB_PROTOCOL_ERROR;
        if (d->session.release_timeouts && op != MB_SESSION_END && op != MB_SESSION_STATS) {
            *result = VK_TIMEOUT; return MB_OK;
        }
        if (op == MB_SESSION_PRESENT) {
            uint32_t token = mb_get_u32(w + 4);
            struct native_ahb *a = interop_find_ahb(d, token);
            struct native_sync *sync = interop_find_sync(d, mb_get_u32(w + 8));
            struct native_image *im = a ? interop_find_image(d, a->image) : NULL;
            if (!d->session.consumer || !a || !sync || !im || token == d->session.held) return MB_PROTOCOL_ERROR;
            int producer = 0;
            for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
                if (d->submit.commands[i].id && d->submit.commands[i].fence == sync->fence && d->submit.commands[i].image_id == im->id) producer = 1;
            if (!producer) return MB_PROTOCOL_ERROR;
            *result = interop_wait_sync(d, sync, 5000);
            if (*result == VK_SUCCESS && (!im->foreign || im->layout != VK_IMAGE_LAYOUT_GENERAL)) return MB_PROTOCOL_ERROR;
            if (*result == VK_SUCCESS) {
                d->session.pending = token; d->session.pending_frame = d->session.presented + 1;
                d->session.pending_sync = sync->id; d->session.pending_fence = sync->fence;
                *result = (VkResult)mb_consumer_session_present(d->session.consumer, a->handle, &a->desc, sync->fd);
                /* Even timeout may have published the next buffer. End/drain must
                 * retire the consumer before any Vulkan image can be destroyed. */
                if (*result == VK_SUCCESS) {
                    d->session.pending = 0;
                    mb_put_u32(reply, d->session.held);
                    if (d->session.held) ++d->session.released;
                    d->session.max_owned = d->session.held ? 2 : d->session.max_owned ? d->session.max_owned : 1;
                    d->session.held = token; ++d->session.presented; session_quiet = d->session.presented >= 3; *extra = 4; *count = 1;
                    d->session.last_sync = sync->id; d->session.last_fence = sync->fence;
                } else {
                    if (!mb_consumer_session_owns(d->session.consumer, a->handle)) d->session.pending = 0;
                    // A failed present may already have published the next AHB.
                    // Drain that consumer before replying, so even a caller that
                    // continues after timeout cannot render into an untracked AHB.
                    VkResult original = *result; (void)native_session_end(d); *result = original;
                }
            }
        } else if (op == MB_SESSION_END) {
            uint32_t last = d->session.held;
            *result = native_session_end(d);
            if (*result == VK_SUCCESS) { mb_put_u32(reply, last); *extra = 4; *count = 1; }
        } else if (op == MB_SESSION_STATS) {
            uint32_t c[20]; native_session_counts(d, c);
            for (unsigned i = 0; i < 20; ++i) mb_put_u32(reply + i * 4, c[i]);
            mb_put_u32(reply + 80, d->session.presented); mb_put_u32(reply + 84, d->session.released);
            mb_put_u32(reply + 88, d->session.fds_created); mb_put_u32(reply + 92, d->session.fds_closed);
            mb_put_u32(reply + 96, d->session.max_owned);
            *extra = 100; *count = 1; native_session_log(d, "snapshot");
        }
    }
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
