/* ICD observations go into the owning Gamescope journal through a local sink. */
#ifndef DD_FRAME_TRACE_PROXY_H
#define DD_FRAME_TRACE_PROXY_H
#include "frame_trace.h"
static __thread dd_ft_sink proxy_trace_sink;
static __thread struct dd_ft_id proxy_trace_id;
static __thread int proxy_trace_detailed;
static __thread uint64_t proxy_trace_completed;
static uint64_t proxy_trace_upload_completed(void *device);
static void proxy_DroidDeckFrameTraceMALI(void *device, dd_ft_sink sink, const struct dd_ft_id *id) {
    proxy_trace_completed = sink && device ? proxy_trace_upload_completed(device) : 0;
    proxy_trace_detailed = sink && dd_ft_upload_detail_selected(proxy_trace_completed);
    proxy_trace_sink = sink;
    proxy_trace_id = id ? *id : (struct dd_ft_id){0};
}
static void proxy_trace(enum dd_ft_stage stage, uint64_t object, uint64_t value, int32_t result) {
    if (proxy_trace_sink && proxy_trace_detailed)
        proxy_trace_sink(stage, proxy_trace_id, object, value, result, 1);
}
struct proxy_trace_guard { enum dd_ft_stage end; uint64_t object, value; };
static void proxy_trace_finish(struct proxy_trace_guard *g) {
    if (g->end != DD_FT_COUNT) { proxy_trace(g->end, g->object, g->value, 0); g->end = DD_FT_COUNT; }
}
#define PROXY_TRACE_SCOPE(var, name, object, value) \
    struct proxy_trace_guard var __attribute__((cleanup(proxy_trace_finish))) = {DD_FT_##name##_END, object, value}; \
    proxy_trace(DD_FT_##name##_BEGIN, object, value, 0)
/* Wrappers retain short-circuit read/write order and original return values. */
static int proxy_trace_write(int fd, const void *data, size_t size) {
    PROXY_TRACE_SCOPE(trace, RPC_SEND, 0, 0);
    return mb_write(fd, data, size);
}
static int proxy_trace_read(int fd, void *data, size_t size) {
    PROXY_TRACE_SCOPE(trace, RPC_RECEIVE, 0, 0);
    return mb_read(fd, data, size);
}
#endif
