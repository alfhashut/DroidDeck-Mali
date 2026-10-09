#ifndef MB_NORMAL_RELEASE_WAIT_MS
#define MB_NORMAL_RELEASE_WAIT_MS 5000u
#endif
/* Normal output uses Android's existing Wayland/sc_layer release path. */
static unsigned native_normal_held(struct native_device *d) {
    unsigned held = 0;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) held += mb_normal_owned(d->interop.ahbs[i].normal_key);
    return held;
}
static void native_normal_totals(struct native_device *d, uint32_t *presented, uint32_t *released) {
    *presented = d->normal.presented; *released = d->normal.released;
    if (d->normal.stopping == 2) return;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct mb_normal_state state;
        if (mb_normal_snapshot(d->interop.ahbs[i].normal_key, &state)) { *presented += state.presented; *released += state.released; }
    }
}
static VkResult native_normal_end(struct native_device *d) {
    if (d->normal.timeouts) return VK_TIMEOUT;
    if (!d->normal.enabled || d->normal.stopping == 2) return d->normal.timeouts ? VK_TIMEOUT : VK_SUCCESS;
    uint32_t keys[3] = {0}; unsigned n = 0;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) if (d->interop.ahbs[i].normal_key) {
        if (n < 3) keys[n++] = d->interop.ahbs[i].normal_key;
        mb_normal_retire(d->interop.ahbs[i].normal_key);
    }
    d->normal.stopping = 1;
    unsigned outstanding = native_normal_held(d);
    if (!mb_normal_wait(keys, n, MB_NORMAL_RELEASE_WAIT_MS)) {
        d->normal.timeouts = 1; d->session.release_timeouts = 1; session_quiet = 0;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
            struct native_ahb *a = &d->interop.ahbs[i]; struct mb_normal_state state;
            if (!mb_normal_snapshot(a->normal_key, &state) || !state.owned) continue;
            struct native_sync *sync = interop_find_sync(d, state.sync);
            ERROR("Android release wait TIMEOUT: buffer token=%u key=%u frame=%u state=ANDROID_OWNED submitted=%u consumers=%u retired=%u sync=%u fence=%u FD=%d sync completed=%u outstanding releases=%u; no PASS",
                a->id, state.key, state.frame, state.submitted, state.consumers, state.retired, state.sync, state.fence,
                sync ? sync->fd : -1, sync ? sync->completed : 0, native_normal_held(d));
        }
        ERROR("Android release wait: outstanding at stop=%u released=%u timed out=%u; release timeouts=1",
              outstanding, outstanding - native_normal_held(d), native_normal_held(d));
        return VK_TIMEOUT;
    }
    native_normal_totals(d, &d->normal.presented, &d->normal.released);
    d->normal.stopping = 2;
    for (unsigned i = 0; i < n; ++i) mb_normal_forget(keys[i]);
    session_quiet = 0;
    LOG("Android release wait: outstanding at stop=%u released=%u timed out=0; release timeouts=0 normal presented=%u released=%u", outstanding, outstanding, d->normal.presented, d->normal.released);
    return VK_SUCCESS;
}
static uint32_t native_normal_command(struct vk_session *s, uint32_t op, const uint8_t *w, uint32_t bytes,
        uint8_t *reply, uint32_t *extra, uint32_t *count, VkResult *result) {
    uint32_t expected = op == MB_NORMAL_PUBLISH ? 16 : (op == MB_NORMAL_BEGIN || op == MB_NORMAL_REGISTER) ? 8 : 4;
    if (bytes != expected) return MB_PROTOCOL_ERROR;
    struct native_device *d = NULL;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle && s->logical[i].id == mb_get_u32(w)) d = &s->logical[i];
    if (!d || !d->renderer.enabled || !d->interop.ahb_enabled || d->session.enabled) return MB_PROTOCOL_ERROR;
    if (op == MB_NORMAL_BEGIN) {
        if (d->normal.enabled || mb_get_u32(w + 4) > 1) return MB_PROTOCOL_ERROR;
        d->normal.enabled = 1; d->normal.verbose = mb_get_u32(w + 4);
        native_session_log(d, "normal start (connection baseline zero)"); session_quiet = !d->normal.verbose;
        return MB_OK;
    }
    if (!d->normal.enabled) return MB_PROTOCOL_ERROR;
    if (op != MB_NORMAL_END && op != MB_NORMAL_STATS && d->normal.stopping) { *result = VK_TIMEOUT; return MB_OK; }
    if (op == MB_NORMAL_REGISTER) {
        struct native_ahb *a = interop_find_ahb(d, mb_get_u32(w + 4)); unsigned n = 0;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) n += !!d->interop.ahbs[i].normal_key;
        if (!a || a->normal_key || n >= 3) return MB_PROTOCOL_ERROR;
        a->normal_key = mb_normal_register(a->handle);
        if (!a->normal_key) { *result = VK_ERROR_OUT_OF_HOST_MEMORY; return MB_VULKAN_ERROR; }
        mb_put_u32(reply, a->normal_key); *extra = 4; *count = 1;
    } else if (op == MB_NORMAL_PUBLISH) {
        struct native_ahb *a = interop_find_ahb(d, mb_get_u32(w + 4));
        struct native_sync *sync = interop_find_sync(d, mb_get_u32(w + 8));
        struct native_image *im = a ? interop_find_image(d, a->image) : NULL;
        if (!a || !a->normal_key || !sync || !im || !mb_get_u32(w + 12)) return MB_PROTOCOL_ERROR;
        int producer = 0;
        for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i)
            if (d->submit.commands[i].id && d->submit.commands[i].fence == sync->fence && d->submit.commands[i].image_id == im->id) producer = 1;
        if (!producer) return MB_PROTOCOL_ERROR;
        *result = interop_wait_sync(d, sync, 5000);
        if (*result != VK_SUCCESS) return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
        if (!im->foreign || im->layout != VK_IMAGE_LAYOUT_GENERAL || !mb_normal_publish(a->normal_key, mb_get_u32(w + 12), sync->id, sync->fence)) return MB_PROTOCOL_ERROR;
    } else if (op == MB_NORMAL_END) {
        native_drain_device(d); *result = native_normal_end(d);
    } else if (op == MB_NORMAL_STATS) {
        uint32_t c[20], presented, released; native_session_counts(d, c); native_normal_totals(d, &presented, &released);
        for (unsigned i = 0; i < 20; ++i) mb_put_u32(reply + i * 4, c[i]);
        uint32_t totals[] = {presented, released, native_normal_held(d), d->normal.timeouts, d->session.fds_created, d->session.fds_closed};
        for (unsigned i = 0; i < 6; ++i) mb_put_u32(reply + 80 + i * 4, totals[i]);
        *extra = 104; *count = 1;
    }
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
