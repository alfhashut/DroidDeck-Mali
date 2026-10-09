/* HOST MODEL ONLY: real broker/registry, real wlroots outer server, modeled
 * Android release callbacks. Actual Gamescope CWaylandBackend connects here. */
#define main checkpoint_broker_main
#include "broker_mock.c"
#undef main
#include <wayland-server.h>
#include <wlr/render/pixman.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <drm_fourcc.h>
#include "presentation-time-protocol.h"
#include "banner-ahb-v1-protocol.h"
#include "linux-dmabuf-v1-protocol.h"

static struct wl_display *outer_display;
static struct wlr_renderer *outer_renderer;
static struct wlr_seat *outer_seat;
static struct wlr_keyboard outer_keyboard;
static struct wlr_xdg_toplevel *outer_top;
static pthread_t outer_thread;
static atomic_int outer_stop, outer_frames;
static unsigned outer_motions, outer_clicks, outer_keys, outer_sent, outer_cycle_base;
static int outer_faulted;
struct outer_buffer { struct wlr_buffer base; struct wl_resource *resource; AHardwareBuffer *ahb; uint32_t key; struct wl_listener released; };
static void outer_resource_destroy(struct wl_resource *r) {
    struct outer_buffer *b = wl_resource_get_user_data(r); b->resource = NULL; wlr_buffer_drop(&b->base);
}
static void outer_buffer_request_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface outer_buffer_impl = {.destroy=outer_buffer_request_destroy};
static void outer_buffer_free(struct wlr_buffer *base) {
    struct outer_buffer *b = (void *)base; wl_list_remove(&b->released.link);
    mb_normal_detach(b->key); AHardwareBuffer_release(b->ahb); free(b);
}
static bool outer_buffer_access(struct wlr_buffer *base, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct outer_buffer *b = (void *)base; assert(flags == WLR_BUFFER_DATA_PTR_ACCESS_READ);
    *data = b->ahb->pixels; *format = DRM_FORMAT_ABGR8888; *stride = b->ahb->desc.stride * 4; return true;
}
static void outer_buffer_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl outer_wlr_buffer_impl = {.destroy=outer_buffer_free,.begin_data_ptr_access=outer_buffer_access,.end_data_ptr_access=outer_buffer_end};
static void outer_released(struct wl_listener *listener, void *data) {
    (void)data; struct outer_buffer *b = wl_container_of(listener, b, released);
    struct mb_normal_state state; mb_normal_snapshot(b->key, &state);
    if (state.owned && mode != 71) mb_normal_release(b->key); // modeled real presentation release
    if (b->resource && !mb_normal_owned(b->key)) wl_buffer_send_release(b->resource);
}
static bool outer_is_buffer(struct wl_resource *r) { return wl_resource_instance_of(r, &wl_buffer_interface, &outer_buffer_impl); }
static struct wlr_buffer *outer_from_resource(struct wl_resource *r) {
    struct outer_buffer *b = wl_resource_get_user_data(r);
    assert(mb_normal_submit(b->key)); ++outer_frames;
    assert(b->ahb->pixels[3] == 255); return &b->base;
}
static const struct wlr_buffer_resource_interface outer_buffer_resource_impl = {.name="host-AHB-model",.is_instance=outer_is_buffer,.from_resource=outer_from_resource};
static void outer_create_buffer(struct wl_client *c, struct wl_resource *manager, uint32_t id, uint32_t key, uint32_t width, uint32_t height) {
    (void)manager; struct outer_buffer *b = calloc(1, sizeof(*b)); assert(b);
    b->ahb = mb_normal_import(key,width,height); assert(b->ahb); b->key=key;
    wlr_buffer_init(&b->base, &outer_wlr_buffer_impl, width,height);
    b->resource=wl_resource_create(c,&wl_buffer_interface,1,id); assert(b->resource);
    wl_resource_set_implementation(b->resource,&outer_buffer_impl,b,outer_resource_destroy);
    b->released.notify=outer_released; wl_signal_add(&b->base.events.release,&b->released);
}
static void outer_manager_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct banner_ahb_v1_interface outer_ahb_impl = {.destroy=outer_manager_destroy,.create_broker_buffer=outer_create_buffer};
static void outer_bind_ahb(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource *r=wl_resource_create(c,&banner_ahb_v1_interface,version,id);
    wl_resource_set_implementation(r,&outer_ahb_impl,NULL,NULL); banner_ahb_v1_send_mode(r,1);
}
static void outer_bind_dmabuf(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource *r=wl_resource_create(c,&zwp_linux_dmabuf_v1_interface,version,id);
    /* Truthful empty format list. Normal AHB path must never create a DMA-BUF. */
    static const struct zwp_linux_dmabuf_v1_interface impl={.destroy=outer_manager_destroy};
    wl_resource_set_implementation(r,&impl,NULL,NULL);
}
static void outer_bind_presentation(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource *r=wl_resource_create(c,&wp_presentation_interface,version,id);
    static const struct wp_presentation_interface impl={.destroy=outer_manager_destroy};
    wl_resource_set_implementation(r,&impl,NULL,NULL); wp_presentation_send_clock_id(r,CLOCK_MONOTONIC);
}
static struct wl_listener outer_new_top, outer_commit, outer_destroy;
static void outer_top_commit(struct wl_listener *l, void *data) {
    (void)l;(void)data;
    if (outer_top->base->initial_commit) {
        wlr_xdg_toplevel_set_size(outer_top,640,360);
        wlr_xdg_toplevel_set_fullscreen(outer_top,true);
        wlr_xdg_toplevel_set_activated(outer_top,true);
    }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    wlr_surface_send_frame_done(outer_top->base->surface,&now);
}
static void outer_top_destroy(struct wl_listener *l, void *data) {
    (void)l;(void)data; wl_list_remove(&outer_commit.link); wl_list_remove(&outer_destroy.link); outer_top=NULL;
}
static void outer_toplevel(struct wl_listener *l, void *data) {
    (void)l; outer_top=data; outer_sent=0; outer_cycle_base=outer_frames; outer_commit.notify=outer_top_commit; outer_destroy.notify=outer_top_destroy;
    wl_signal_add(&outer_top->base->surface->events.commit,&outer_commit); wl_signal_add(&outer_top->events.destroy,&outer_destroy);
}
static void *outer_run(void *unused) {
    (void)unused;
    while (!outer_stop) {
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(outer_display),5)>=0);
        if (outer_top && outer_frames - outer_cycle_base >= 10 && !outer_sent) {
            struct wlr_surface *s=outer_top->base->surface;
            wlr_seat_pointer_notify_enter(outer_seat,s,160,90);
            wlr_seat_pointer_notify_motion(outer_seat,100,200,100); ++outer_motions;
            wlr_seat_pointer_notify_button(outer_seat,101,BTN_LEFT,WL_POINTER_BUTTON_STATE_PRESSED);
            wlr_seat_pointer_notify_button(outer_seat,102,BTN_LEFT,WL_POINTER_BUTTON_STATE_RELEASED); ++outer_clicks;
            wlr_seat_pointer_notify_frame(outer_seat);
            wlr_seat_touch_notify_down(outer_seat,s,103,0,240,120);
            wlr_seat_touch_notify_motion(outer_seat,104,0,300,150); ++outer_motions;
            wlr_seat_touch_notify_up(outer_seat,105,0); ++outer_clicks;
            wlr_seat_touch_notify_frame(outer_seat);
            wlr_seat_keyboard_notify_enter(outer_seat,s,NULL,0,&outer_keyboard.modifiers);
            wlr_seat_keyboard_notify_key(outer_seat,106,KEY_D,WL_KEYBOARD_KEY_STATE_PRESSED);
            wlr_seat_keyboard_notify_key(outer_seat,107,KEY_D,WL_KEYBOARD_KEY_STATE_RELEASED); ++outer_keys;
            outer_sent=1;
        }
        if (outer_top && outer_frames - outer_cycle_base >= 100 && outer_sent==1) {
            wlr_seat_keyboard_notify_key(outer_seat,108,KEY_ESC,WL_KEYBOARD_KEY_STATE_PRESSED); ++outer_keys; outer_sent=2;
        }
        if (outer_top && outer_frames >= 15 && !outer_faulted && (mode == 73 || mode == 74)) {
            outer_faulted = 1;
            if (mode == 74) wl_client_destroy(wl_resource_get_client(outer_top->base->surface->resource));
            else for (unsigned i = 0; i < MAX_CLIENTS; ++i)
                if (clients[i].started && clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
        }
        wl_display_flush_clients(outer_display);
    }
    wl_display_destroy_clients(outer_display);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==4); path=argv[2]; mode=atoi(argv[3]);
    outer_display=wl_display_create(); assert(outer_display); outer_renderer=wlr_pixman_renderer_create(); assert(outer_renderer);
    assert(wlr_renderer_init_wl_display(outer_renderer,outer_display));
    assert(wlr_compositor_create(outer_display,5,outer_renderer)); assert(wlr_subcompositor_create(outer_display));
    assert(wlr_viewporter_create(outer_display)); assert(wlr_pointer_constraints_v1_create(outer_display)); assert(wlr_relative_pointer_manager_v1_create(outer_display));
    assert(wl_global_create(outer_display,&wp_presentation_interface,1,NULL,outer_bind_presentation));
    struct wlr_xdg_shell *shell=wlr_xdg_shell_create(outer_display,1); assert(shell);
    outer_new_top.notify=outer_toplevel; wl_signal_add(&shell->events.new_toplevel,&outer_new_top);
    outer_seat=wlr_seat_create(outer_display,"Android seat model"); assert(outer_seat);
    wlr_keyboard_init(&outer_keyboard,NULL,"model"); struct xkb_context *context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap=xkb_keymap_new_from_names(context,NULL,XKB_KEYMAP_COMPILE_NO_FLAGS); assert(wlr_keyboard_set_keymap(&outer_keyboard,keymap));
    xkb_keymap_unref(keymap); xkb_context_unref(context); wlr_seat_set_keyboard(outer_seat,&outer_keyboard);
    wlr_seat_set_capabilities(outer_seat,WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD|WL_SEAT_CAPABILITY_TOUCH);
    wlr_buffer_register_resource_interface(&outer_buffer_resource_impl);
    assert(wl_global_create(outer_display,&banner_ahb_v1_interface,3,NULL,outer_bind_ahb));
    assert(wl_global_create(outer_display,&zwp_linux_dmabuf_v1_interface,3,NULL,outer_bind_dmabuf));
    assert(wl_display_add_socket(outer_display,"wayland-0")==0);
    start(); assert(!thrown); assert(pthread_create(&outer_thread,NULL,outer_run,NULL)==0); puts("READY"); fflush(stdout);
    (void)getchar(); outer_stop=1; pthread_join(outer_thread,NULL); stop();
    wlr_keyboard_finish(&outer_keyboard); wl_list_remove(&outer_new_top.link); wl_display_destroy(outer_display); wlr_renderer_destroy(outer_renderer);
    if (mode==71) { assert(quarantined_sessions); puts("normal timeout quarantine retained; no fake release"); }
    else {
        assert(!quarantined_sessions && creates==destroys && device_creates==device_destroys);
        assert(images_created==images_destroyed && memories_created==memories_freed && ahb_created==ahb_freed);
        assert(buffers_created==buffers_destroyed && renderer_created==renderer_destroyed && cpu_locks==0);
        printf("normal baseline zero: AHB=%d/%d sync FDs closed; readbacks=0 frames=%d input=%u/%u/%u\n",ahb_created,ahb_freed,outer_frames,outer_motions,outer_clicks,outer_keys);
    }
    return 0;
}
