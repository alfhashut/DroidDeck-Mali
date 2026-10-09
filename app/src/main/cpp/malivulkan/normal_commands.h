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
static void native_normal_performance(struct native_device *d) {
    uint64_t calls = 0, ns = 0, worst = 0; unsigned worst_op = 0;
    for (unsigned i = 0; i < DD_PERF_OPS; ++i) {
        calls += d->normal.perf.op[i].count; ns += d->normal.perf.op[i].ns;
        if (d->normal.perf.op[i].worst > worst) { worst = d->normal.perf.op[i].worst; worst_op = i; }
    }
    /* One aggregated line per guest reporting window, even in quiet mode. */
    __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker",
        "MaliPerf native: RPCs=%llu service=%.3fms worst=%.3fms opcode=%u vkQueueSubmit=%.3fms/%llu timeline=%.3fms/%llu SYNC_FD-export=%.3fms/%llu sync-wait=%.3fms/%llu broker-thread-CPU=%.3fms (window totals; CPU includes reply/loop work; service includes payload read/validation, excludes reply; native calls overlap service)",
        (unsigned long long)calls, ns / 1e6, worst / 1e6, worst_op,
        d->normal.native_ns[0] / 1e6, (unsigned long long)d->normal.native_calls[0],
        d->normal.native_ns[1] / 1e6, (unsigned long long)d->normal.native_calls[1],
        d->normal.native_ns[2] / 1e6, (unsigned long long)d->normal.native_calls[2],
        d->normal.native_ns[3] / 1e6, (unsigned long long)d->normal.native_calls[3],
        (dd_perf_cpu_now() - d->normal.cpu_start_ns) / 1e6);
    struct mb_normal_timing total = {0}; uint32_t presented = 0, released = 0;
    native_normal_totals(d, &presented, &released);
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct mb_normal_state state;
        if (!mb_normal_snapshot(d->interop.ahbs[i].normal_key, &state)) continue;
        total.present_calls += state.timing.present_calls; total.present_ns += state.timing.present_ns;
        total.queue_ns += state.timing.queue_ns; total.held_ns += state.timing.held_ns;
        if (state.owned) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker",
            "MaliPerf held: key=%u frame=%u submitted=%u sync=%u fence=%u age=%.3fms",
            state.key, state.frame, state.submitted, state.sync, state.fence, (dd_perf_now() - state.published_ns) / 1e6);
    }
    struct mb_normal_timing *old = &d->normal.previous_android;
    __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker",
        "MaliPerf Android: transaction-enqueue=%.3fms/%llu publish-to-submit=%.3fms release-held=%.3fms presented-total=%u released-total=%u owned=%u (window totals; release-held is async buffer lifetime, not CPU blocking or scanout latency)",
        (total.present_ns - old->present_ns) / 1e6, (unsigned long long)(total.present_calls - old->present_calls),
        (total.queue_ns - old->queue_ns) / 1e6, (total.held_ns - old->held_ns) / 1e6,
        presented, released, native_normal_held(d));
    *old = total;
    memset(&d->normal.perf, 0, sizeof(d->normal.perf));
    memset(d->normal.native_ns, 0, sizeof(d->normal.native_ns));
    memset(d->normal.native_calls, 0, sizeof(d->normal.native_calls));
    d->normal.cpu_start_ns = dd_perf_cpu_now();
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
    uint64_t release_start = dd_perf_now();
    int all_released = mb_normal_wait(keys, n, MB_NORMAL_RELEASE_WAIT_MS);
    __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", "MaliPerf teardown: Android-release-wait=%.3fms outstanding-at-stop=%u timeout=%u",
                        (dd_perf_now() - release_start) / 1e6, outstanding, !all_released);
    native_normal_performance(d);
    if (!all_released) {
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
        d->normal.cpu_start_ns = dd_perf_cpu_now();
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
        if (d->normal.stopping != 2) native_normal_performance(d);
        uint32_t c[20], presented, released; native_session_counts(d, c); native_normal_totals(d, &presented, &released);
        for (unsigned i = 0; i < 20; ++i) mb_put_u32(reply + i * 4, c[i]);
        uint32_t totals[] = {presented, released, native_normal_held(d), d->normal.timeouts, d->session.fds_created, d->session.fds_closed};
        for (unsigned i = 0; i < 6; ++i) mb_put_u32(reply + 80 + i * 4, totals[i]);
        *extra = 104; *count = 1;
    }
    return *result < 0 ? MB_VULKAN_ERROR : MB_OK;
}
