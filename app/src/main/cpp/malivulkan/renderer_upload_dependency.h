/* Only the normal SHM upload -> sampled source edge may overlap submissions.
 * A real same-queue transfer-write -> shader-read barrier proves visibility.
 * Neither queued layout nor submission alone marks a command complete. */
static struct native_command *renderer_pending_upload(struct native_device *d, uint32_t id) {
    struct native_image *im = interop_find_image(d, id);
    if (!d->normal.enabled || !im || im->ahb) return NULL;
    struct native_command *found = NULL;
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_command *p = &d->submit.commands[i];
        if (!p->id || p->state != 3) continue;
        int referenced = 0;
        for (unsigned j = 0; j < p->ref_count; ++j) referenced |= p->refs[j] == id;
        if (!referenced) continue;
        if (found || !p->renderer || p->pipeline || p->descriptor_set || p->image_id ||
            p->renderer_upload_image != id || p->renderer_transfer_ops != 1 || !p->renderer_upload_visible ||
            p->renderer_image_count != 1 || p->renderer_images[0].id != id || p->renderer_images[0].foreign ||
            p->renderer_images[0].final != VK_IMAGE_LAYOUT_GENERAL || p->ref_count != 2) return NULL;
        found = p;
    }
    return found;
}
static int renderer_descriptor_update_allowed(struct native_device *d, uint32_t set) {
    for (unsigned i = 0; i < MB_SUBMIT_MAX_OBJECTS; ++i) {
        struct native_command *c = &d->submit.commands[i];
        if (!c->id) continue;
        if (c->descriptor_set == set && c->state != 0 && c->state != 4) return 0;
        /* Only copy-only uploads with no descriptor use may remain pending. */
        if (c->state == 3 && (!d->normal.enabled || renderer_pending_upload(d, c->renderer_upload_image) != c)) return 0;
    }
    return 1;
}
static int renderer_upload_consumer(struct native_device *d, struct native_command *c,
        struct native_command *p, uint32_t image, uint32_t queue, uint32_t semaphore, uint64_t value) {
    if (!d->normal.enabled || !p || !queue || !semaphore || p == c || !c->pipeline || !c->descriptor_set || c->renderer_transfer_ops ||
        p->renderer_submit_queue != queue || p->semaphore != semaphore || p->signal_value >= value) return 0;
    struct native_renderer_object *set = renderer_find(d, c->descriptor_set, MB_R_SET);
    if (!set) return 0;
    int sampled = 0;
    for (unsigned i = 0; i < set->descriptor_count; ++i) {
        if (!set->descriptors[i].view) continue;
        struct native_renderer_object *view = renderer_find(d, set->descriptors[i].view, MB_R_VIEW);
        if (!view || view->image != image) continue;
        if (set->descriptors[i].binding != 3) return 0; // never concurrent output/storage writes
        sampled = 1;
    }
    return sampled;
}
static void renderer_upload_copy(struct native_command *c, struct native_image *im, uint32_t direction) {
    /* Counts transfer operations, including clears, to keep consumers read-only. */
    if (c->renderer_transfer_ops < 2) ++c->renderer_transfer_ops;
    c->renderer_upload_image = direction && !im->ahb ? im->id : 0;
    c->renderer_upload_visible = 0;
}
static void renderer_upload_barrier(struct native_device *d, struct native_command *c,
        struct native_image *im, uint32_t old, uint32_t next, uint32_t sa, uint32_t da,
        uint32_t src, uint32_t dst, int acquire, int release) {
    if (c->renderer_upload_image != im->id) return;
    c->renderer_upload_visible = d->normal.enabled && c->renderer_transfer_ops == 1 &&
        !im->ahb && !acquire && !release && old == VK_IMAGE_LAYOUT_GENERAL && next == VK_IMAGE_LAYOUT_GENERAL &&
        (sa & VK_ACCESS_TRANSFER_WRITE_BIT) && (da & VK_ACCESS_SHADER_READ_BIT) &&
        (src & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) && (dst & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}
