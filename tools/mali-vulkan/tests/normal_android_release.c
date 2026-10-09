/* Execute the actual Android Wayland AHB release code with mock AHB references.
 * Wayland event loop and poll are real; eventfd models a display release fence. */
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
int AHardwareBuffer_recvHandleFromUnixSocket(int, AHardwareBuffer **);
#include "../../../app/src/main/cpp/waylandcomp/src/ahb_swapchain.c"
#include <assert.h>
#include <sys/eventfd.h>
struct AHardwareBuffer { unsigned refs; AHardwareBuffer_Desc desc; };
struct dmabuf_buffer { void *state; };
static unsigned acknowledgements;
void AHardwareBuffer_acquire(AHardwareBuffer *a) { assert(a->refs); ++a->refs; }
void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a->refs); --a->refs; }
void AHardwareBuffer_describe(const AHardwareBuffer *a, AHardwareBuffer_Desc *d) { *d=a->desc; }
void **droiddeck_dmabuf_ahb_slot(struct dmabuf_buffer *b) { return &b->state; }
void droiddeck_dmabuf_unref(struct dmabuf_buffer *b) { (void)b; }
int droiddeck_dmabuf_fd(const struct dmabuf_buffer *b) { (void)b; return -1; }
void droiddeck_log(const char *tag,const char *fmt,...) { (void)tag;(void)fmt; }
void droiddeck_release_buffer(struct surface *s,struct wl_resource *r,int p,int64_t t) { (void)s;(void)r;(void)p;(void)t; ++acknowledgements; }
int main(void) {
    struct AHardwareBuffer a={.refs=1,.desc={.width=640,.height=360}};
    uint32_t key=mb_normal_register(&a); assert(key && mb_normal_import(key,640,360));
    struct dmabuf_buffer b={0};
    struct ahb_buf ab={.id=1,.broker_key=key,.b=&b,.ahb=&a,.on_layer=1,.resource=(void *)1,.release_pending=1,.fence_fd=-1};
    b.state=&ab;wl_list_init(&g_bufs);wl_list_insert(&g_bufs,&ab.link);
    g_loop=wl_event_loop_create();assert(g_loop);
    assert(mb_normal_publish(key,1,2,3) && mb_normal_submit(key));
    int fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);assert(fd>=0);int signal=dup(fd);assert(signal>=0);
    handle_released(1,fd);
    assert(ab.fence_src && mb_normal_owned(key) && acknowledgements==0);
    assert(!mb_normal_wait(&key,1,5));
    on_fence_readable(fd,WL_EVENT_ERROR,&ab);assert(mb_normal_owned(key) && acknowledgements==0);
    uint64_t value=1;assert(write(signal,&value,8)==8);close(signal);
    assert(wl_event_loop_dispatch(g_loop,10)==0);
    struct mb_normal_state state;assert(mb_normal_snapshot(key,&state));
    assert(!state.owned && state.released==1 && acknowledgements==1 && ab.fence_fd==-1 && !ab.fence_src);
    assert(mb_normal_publish(key,2,4,5));ab.on_layer=0;
    assert(ahb_swapchain_defer_release(&b,(void *)1,NULL,0)==0);
    assert(mb_normal_snapshot(key,&state) && !state.owned && state.released==1); // cancellation never counts as release
    assert(mb_normal_publish(key,3,6,7) && mb_normal_submit(key));ab.on_layer=1;
    assert(ahb_swapchain_defer_release(&b,(void *)1,NULL,0)==1);
    mb_normal_retire(key);assert(!mb_normal_wait(&key,1,5));assert(mb_normal_owned(key));
    handle_released(1,-1);assert(mb_normal_wait(&key,1,1)); // real callback with no fence
    assert(mb_normal_snapshot(key,&state) && state.released==2 && acknowledgements==2);
    mb_normal_forget(key);mb_normal_detach(key);AHardwareBuffer_release(&a);assert(a.refs==1);
    wl_list_remove(&ab.link);wl_event_loop_destroy(g_loop);
    puts("actual Wayland AHB release: delayed fence, error not ACK, retained timeout, unpublished cancellation, no-fence real callback passed");
}
