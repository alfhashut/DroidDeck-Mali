#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MB_SUBMIT_MAX_OBJECTS 8
#define MB_RENDERER_MAX_OBJECTS 4
#define MB_INTEROP_MAX_REFS 16
#define MB_R_SET 7
#define MB_R_VIEW 2
#define VK_IMAGE_LAYOUT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_GENERAL 1
#define VK_ACCESS_TRANSFER_WRITE_BIT 1u
#define VK_ACCESS_SHADER_READ_BIT 2u
#define VK_PIPELINE_STAGE_ALL_COMMANDS_BIT 4u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 4u
typedef unsigned VkImageLayout;
typedef uint64_t VkCommandBuffer;
#include "command.inc"
struct native_buffer { uint32_t id, memory; };
struct native_memory { uint32_t id, type; void *map; };
struct native_image { uint32_t id, memory, ahb, foreign; VkImageLayout layout; };
struct native_ahb { uint32_t id, image, memory, held; };
struct native_renderer_object {
    uint32_t id, kind, image, descriptor_count;
    struct { uint32_t view, binding; } descriptors[4];
};
struct native_device {
    struct { int enabled; } normal;
    struct { int release_timeouts; } session;
    struct { struct native_command commands[MB_SUBMIT_MAX_OBJECTS]; } submit;
    struct { struct native_renderer_object *objects; } renderer;
    struct {
        struct native_buffer buffers[3]; struct native_memory memories[3];
        struct native_image images[2]; struct native_ahb ahbs[MB_SUBMIT_MAX_OBJECTS];
        struct { struct { unsigned propertyFlags; } memoryTypes[1]; } properties;
    } interop;
};
static struct native_image *interop_find_image(struct native_device *d, uint32_t id) {
    for (unsigned i = 0; i < 2; ++i) if (id && d->interop.images[i].id == id) return &d->interop.images[i];
    return NULL;
}
static struct native_buffer *interop_find_buffer(struct native_device *d, uint32_t id) {
    for (unsigned i = 0; i < 3; ++i) if (id && d->interop.buffers[i].id == id) return &d->interop.buffers[i];
    return NULL;
}
static struct native_memory *interop_find_memory(struct native_device *d, uint32_t id) {
    for (unsigned i = 0; i < 3; ++i) if (id && d->interop.memories[i].id == id) return &d->interop.memories[i];
    return NULL;
}
static int interop_ahb_held(struct native_device *d, const struct native_ahb *a) { (void)d; return a->held; }
#include "refs.inc"
#include "renderer_find.inc"
#include "renderer_upload_dependency.h"
#include "renderer_state.inc"
static int renderer_descriptors_live(struct native_device *d, struct native_command *c) { (void)d; (void)c; return 1; }
#include "renderer_guard.inc"
int main(void) {
    struct native_device d = {0}; struct native_renderer_object objects[MB_RENDERER_MAX_OBJECTS] = {0};
    d.normal.enabled = 1; d.renderer.objects = objects;
    d.interop.images[0] = (struct native_image){100, 200, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    d.interop.images[1] = (struct native_image){101, 201, 1, 1, VK_IMAGE_LAYOUT_GENERAL};
    d.interop.buffers[0] = (struct native_buffer){300, 400};
    d.interop.buffers[1] = (struct native_buffer){500, 600};
    d.interop.memories[0] = (struct native_memory){200, 0, NULL};
    d.interop.memories[1] = (struct native_memory){400, 0, NULL};
    d.interop.memories[2] = (struct native_memory){600, 0, NULL};
    d.interop.properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    objects[0].id = 11; objects[0].kind = MB_R_SET; objects[0].descriptor_count = 1;
    objects[0].descriptors[0].view = 12; objects[0].descriptors[0].binding = 3;
    objects[1].id = 12; objects[1].kind = MB_R_VIEW; objects[1].image = 100;
    struct native_command *u = &d.submit.commands[0], *r = &d.submit.commands[1];
    u->id = 1; u->renderer = 1; u->state = 3; u->semaphore = 10; u->signal_value = 20; u->renderer_submit_queue = 7;
    u->ref_count = 2; u->refs[0] = 100; u->refs[1] = 300;
    u->renderer_image_count = 1; u->renderer_images[0].id = 100; u->renderer_images[0].final = VK_IMAGE_LAYOUT_GENERAL;
    renderer_upload_copy(u, &d.interop.images[0], 1);
    assert(!renderer_pending_upload(&d, 100));
    // Missing access/stage scopes do not establish the queued dependency.
    renderer_upload_barrier(&d, u, &d.interop.images[0], 1, 1, 1, 0, 4, 4, 0, 0);
    assert(!renderer_pending_upload(&d, 100));
    renderer_upload_barrier(&d, u, &d.interop.images[0], 1, 1, 1, 2, 0, 4, 0, 0);
    assert(!renderer_pending_upload(&d, 100));
    renderer_upload_barrier(&d, u, &d.interop.images[0], 1, 1, 1, 2, 4, 4, 0, 0);
    assert(renderer_pending_upload(&d, 100) == u);
    // Later incompatible barriers invalidate an earlier visibility proof.
    renderer_upload_barrier(&d, u, &d.interop.images[0], 1, 1, 1, 0, 4, 4, 0, 0);
    assert(!renderer_pending_upload(&d, 100));
    renderer_upload_barrier(&d, u, &d.interop.images[0], 1, 1, 1, 2, 4, 4, 0, 0);
    assert(renderer_descriptor_update_allowed(&d, 11));
    r->id = 2; r->state = 2; r->pipeline = 9; r->descriptor_set = 11;
    assert(!renderer_descriptor_update_allowed(&d, 11)); // recorded set still protected
    assert(renderer_descriptor_update_allowed(&d, 13)); // unrelated unbound set
    r->state = 3; assert(!renderer_descriptor_update_allowed(&d, 13)); r->state = 2;
    assert(renderer_image_state(&d, r, 100) == 0 && r->renderer_images[0].initial == VK_IMAGE_LAYOUT_GENERAL);
    assert(!interop_ref(r, 500));
    assert(native_renderer_can_submit_ordered(&d, r, 7, 10, 21));
    // The exception is sampled-image consumption, never transfer/clear reuse.
    r->renderer_transfer_ops = 1;
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21)); r->renderer_transfer_ops = 0;
    // Validation/planned state never claims native GPU completion.
    assert(u->state == 3 && d.interop.images[0].layout == VK_IMAGE_LAYOUT_UNDEFINED);
    assert(!native_renderer_can_submit(&d, r));
    assert(!native_renderer_can_submit_ordered(&d, r, 8, 10, 21));
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 12, 21));
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 20));
    objects[0].descriptors[0].binding = 1;
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21)); objects[0].descriptors[0].binding = 3;
    r->refs[r->ref_count++] = 300;
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21)); --r->ref_count;
    // Existing pending host map/write/destruction guards still see the upload.
    assert(interop_referenced(&d, 300, 1) && interop_referenced(&d, 100, 1));
    d.normal.enabled = 0; assert(!renderer_pending_upload(&d, 100));
    assert(!renderer_descriptor_update_allowed(&d, 13));
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21)); d.normal.enabled = 1;
    d.interop.images[0].ahb = 1; assert(!renderer_pending_upload(&d, 100)); d.interop.images[0].ahb = 0;
    u->renderer_transfer_ops = 2; assert(!renderer_pending_upload(&d, 100)); u->renderer_transfer_ops = 1;
    u->pipeline = 9; assert(!renderer_pending_upload(&d, 100)); u->pipeline = 0;
    u->renderer_upload_visible = 0; assert(!renderer_pending_upload(&d, 100)); u->renderer_upload_visible = 1;
    d.submit.commands[2].id = 3; d.submit.commands[2].state = 3;
    d.submit.commands[2].ref_count = 1; d.submit.commands[2].refs[0] = 100;
    assert(!renderer_descriptor_update_allowed(&d, 13));
    assert(!renderer_pending_upload(&d, 100)); memset(&d.submit.commands[2], 0, sizeof(d.submit.commands[2]));
    d.session.release_timeouts = 1; assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21)); d.session.release_timeouts = 0;
    // Only actual completion updates completed global layout/state.
    native_renderer_complete(&d, u); u->state = 4;
    assert(renderer_descriptor_update_allowed(&d, 13));
    assert(d.interop.images[0].layout == VK_IMAGE_LAYOUT_GENERAL && native_renderer_can_submit(&d, r));
    assert(renderer_image_state(&d, r, 101) == 1);
    d.interop.ahbs[0] = (struct native_ahb){1, 101, 201, 1};
    assert(!native_renderer_can_submit_ordered(&d, r, 7, 10, 21));
    puts("native ordered upload guard PASS");
}
