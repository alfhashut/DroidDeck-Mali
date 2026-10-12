/* Exercise real transaction registration/callback; API responses are host models. */
#define _GNU_SOURCE
#include "../../../app/src/main/cpp/waylandcomp/src/sc_layer.c"
#include <assert.h>
static struct complete_ctx *saved;
static unsigned released;
static uint32_t replacement_frame;
struct dd_ft_id ahb_swapchain_trace_identity(void *token) {
    struct dd_ft_id id={0};id.key=(uint32_t)(uintptr_t)token;
    id.frame=replacement_frame ? replacement_frame : id.key*10;id.sync=id.key+100;id.fence=id.key+200;
    return id;
}
void ahb_swapchain_layer_released(void *token,int fd) { assert((uintptr_t)token==1 && fd==-1);released++; }
static void remember(ASurfaceTransaction *tx,void *ctx,sc_complete_fn callback) { (void)tx;assert(callback==on_complete);saved=ctx; }
static int release_fd(ASurfaceTransactionStats *stats,ASurfaceControl *sc) { (void)stats;(void)sc;return -1; }
static int64_t latch_time(ASurfaceTransactionStats *stats) { (void)stats;return now_ns()-1000; }
int main(void) {
    dd_ft_init("android");
    int64_t deadline=now_ns()+3000000000LL;struct timespec delay={0,1000000};
    while (!dd_ft_active() && now_ns()<deadline) nanosleep(&delay,NULL);
    assert(dd_ft_active());struct timespec warm={0,180000000};nanosleep(&warm,NULL);
    layers_init();api.setOnComplete=remember;api.prevReleaseFence=release_fd;api.latchTime=latch_time;
    assert(add_complete_on((void *)1,&g_layers[0],(void *)1,-1,(void *)1,0,(void *)2)==0);
    /* Registry/pool identity changes before Binder delivery must not change
     * either the incoming or replaced frame retained in the callback context. */
    replacement_frame=999;
    on_complete(saved,(void *)1);assert(released==1 && g_pending_cb==0);
    while (dd_ft_active() && now_ns()<deadline) nanosleep(&delay,NULL);
    dd_ft_shutdown();return 0;
}
