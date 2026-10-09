/* Normal-session report formatting only. No wire or hot-path changes. */
#ifndef DD_NORMAL_OPCODE_PERF_H
#define DD_NORMAL_OPCODE_PERF_H
#include "normal_perf.h"
#include <stdio.h>

enum dd_perf_category {
    DD_PERF_MEMORY, DD_PERF_RECORDING, DD_PERF_UPDATES, DD_PERF_SUBMIT,
    DD_PERF_SYNC, DD_PERF_RESOURCES, DD_PERF_PRESENT, DD_PERF_OTHER,
    DD_PERF_CATEGORIES
};
static const char *const dd_perf_category_names[DD_PERF_CATEGORIES] = {
    "mapped-memory-transfer", "command-recording", "descriptor-resource-updates",
    "queue-submit", "timeline-synchronization", "allocation-resource-management",
    "AHB-presentation", "other"
};
struct dd_perf_opcode_info { const char *name; enum dd_perf_category category; };
/* Indexed by the existing wire opcode. The parity test checks every entry
 * against protocol headers (including the implicit submit enum values).
 * Classify by opcode, not payload: generic RENDERER_DESTROY is management,
 * even for a semaphore; semaphore/fence/event creation is also management.
 * Memory binds are updates, map/unmap/flush/invalidate are mapped transfer,
 * and normal/diagnostic output-session lifecycle/STATS are presentation. */
static const struct dd_perf_opcode_info dd_perf_opcodes[] = {
    {"UNKNOWN", DD_PERF_OTHER}, /* 0 */
    {"MB_ENUMERATE", DD_PERF_OTHER}, /* 1 */
    {"MB_CREATE", DD_PERF_RESOURCES}, /* 2 */
    {"MB_DESTROY", DD_PERF_RESOURCES}, /* 3 */
    {"MB_LIST", DD_PERF_OTHER}, /* 4 */
    {"MB_PROPERTIES", DD_PERF_OTHER}, /* 5 */
    {"MB_GLOBAL", DD_PERF_OTHER}, /* 6 */
    {"MB_CAPS", DD_PERF_OTHER}, /* 7 */
    {"MB_FORMAT", DD_PERF_OTHER}, /* 8 */
    {"MB_IMAGE", DD_PERF_OTHER}, /* 9 */
    {"MB_BUFFER", DD_PERF_OTHER}, /* 10 */
    {"MB_SEMAPHORE", DD_PERF_OTHER}, /* 11 */
    {"MB_FENCE", DD_PERF_OTHER}, /* 12 */
    {"MB_SPARSE", DD_PERF_OTHER}, /* 13 */
    {"MB_DEVICE_CREATE", DD_PERF_RESOURCES}, /* 14 */
    {"MB_DEVICE_QUEUE", DD_PERF_RESOURCES}, /* 15 */
    {"MB_DEVICE_DESTROY", DD_PERF_RESOURCES}, /* 16 */
    {"MB_POOL_CREATE", DD_PERF_RESOURCES}, /* 17 */
    {"MB_POOL_DESTROY", DD_PERF_RESOURCES}, /* 18 */
    {"MB_COMMAND_ALLOCATE", DD_PERF_RESOURCES}, /* 19 */
    {"MB_COMMAND_FREE", DD_PERF_RESOURCES}, /* 20 */
    {"MB_COMMAND_BEGIN", DD_PERF_RECORDING}, /* 21 */
    {"MB_COMMAND_END", DD_PERF_RECORDING}, /* 22 */
    {"MB_EVENT_CREATE", DD_PERF_RESOURCES}, /* 23 */
    {"MB_EVENT_DESTROY", DD_PERF_RESOURCES}, /* 24 */
    {"MB_EVENT_STATUS", DD_PERF_SYNC}, /* 25 */
    {"MB_COMMAND_SET_EVENT", DD_PERF_RECORDING}, /* 26 */
    {"MB_FENCE_CREATE", DD_PERF_RESOURCES}, /* 27 */
    {"MB_FENCE_DESTROY", DD_PERF_RESOURCES}, /* 28 */
    {"MB_FENCE_STATUS", DD_PERF_SYNC}, /* 29 */
    {"MB_FENCE_WAIT", DD_PERF_SYNC}, /* 30 */
    {"MB_QUEUE_SUBMIT", DD_PERF_SUBMIT}, /* 31 */
    {"MB_BUFFER_CREATE", DD_PERF_RESOURCES}, /* 32 */
    {"MB_BUFFER_DESTROY", DD_PERF_RESOURCES}, /* 33 */
    {"MB_BUFFER_REQUIREMENTS", DD_PERF_RESOURCES}, /* 34 */
    {"MB_MEMORY_ALLOCATE", DD_PERF_RESOURCES}, /* 35 */
    {"MB_MEMORY_FREE", DD_PERF_RESOURCES}, /* 36 */
    {"MB_BUFFER_BIND", DD_PERF_UPDATES}, /* 37 */
    {"MB_MEMORY_MAP", DD_PERF_MEMORY}, /* 38 */
    {"MB_MEMORY_UNMAP", DD_PERF_MEMORY}, /* 39 */
    {"MB_MEMORY_READ", DD_PERF_MEMORY}, /* 40 */
    {"MB_MEMORY_WRITE", DD_PERF_MEMORY}, /* 41 */
    {"MB_MEMORY_FLUSH", DD_PERF_MEMORY}, /* 42 */
    {"MB_MEMORY_INVALIDATE", DD_PERF_MEMORY}, /* 43 */
    {"MB_IMAGE_CREATE", DD_PERF_RESOURCES}, /* 44 */
    {"MB_IMAGE_DESTROY", DD_PERF_RESOURCES}, /* 45 */
    {"MB_IMAGE_REQUIREMENTS", DD_PERF_RESOURCES}, /* 46 */
    {"MB_IMAGE_BIND", DD_PERF_UPDATES}, /* 47 */
    {"MB_COMMAND_FILL", DD_PERF_RECORDING}, /* 48 */
    {"MB_COMMAND_BARRIER", DD_PERF_RECORDING}, /* 49 */
    {"MB_COMMAND_CLEAR", DD_PERF_RECORDING}, /* 50 */
    {"MB_COMMAND_COPY", DD_PERF_RECORDING}, /* 51 */
    {"MB_AHB_CREATE", DD_PERF_PRESENT}, /* 52 */
    {"MB_AHB_RELEASE", DD_PERF_PRESENT}, /* 53 */
    {"MB_SYNC_FENCE_CREATE", DD_PERF_RESOURCES}, /* 54 */
    {"MB_SYNC_EXPORT", DD_PERF_SYNC}, /* 55 */
    {"MB_SYNC_WAIT", DD_PERF_SYNC}, /* 56 */
    {"MB_SYNC_CLOSE", DD_PERF_SYNC}, /* 57 */
    {"MB_AHB_INSPECT", DD_PERF_PRESENT}, /* 58 */
    {"MB_AHB_PRESENT", DD_PERF_PRESENT}, /* 59 */
    {"MB_RENDERER_SEMAPHORE_CREATE", DD_PERF_RESOURCES}, /* 60 */
    {"MB_RENDERER_DESTROY", DD_PERF_RESOURCES}, /* 61 */
    {"MB_RENDERER_COUNTER", DD_PERF_SYNC}, /* 62 */
    {"MB_RENDERER_WAIT", DD_PERF_SYNC}, /* 63 */
    {"MB_RENDERER_VIEW", DD_PERF_RESOURCES}, /* 64 */
    {"MB_RENDERER_SAMPLER", DD_PERF_RESOURCES}, /* 65 */
    {"MB_RENDERER_SET_LAYOUT", DD_PERF_RESOURCES}, /* 66 */
    {"MB_RENDERER_PIPELINE_LAYOUT", DD_PERF_RESOURCES}, /* 67 */
    {"MB_RENDERER_POOL", DD_PERF_RESOURCES}, /* 68 */
    {"MB_RENDERER_SETS", DD_PERF_RESOURCES}, /* 69 */
    {"MB_RENDERER_UPDATE", DD_PERF_UPDATES}, /* 70 */
    {"MB_RENDERER_SHADER", DD_PERF_RESOURCES}, /* 71 */
    {"MB_RENDERER_PIPELINE", DD_PERF_RESOURCES}, /* 72 */
    {"MB_RENDERER_BIND_PIPELINE", DD_PERF_RECORDING}, /* 73 */
    {"MB_RENDERER_BIND_SET", DD_PERF_RECORDING}, /* 74 */
    {"MB_RENDERER_DISPATCH", DD_PERF_RECORDING}, /* 75 */
    {"MB_RENDERER_SUBMIT", DD_PERF_SUBMIT}, /* 76 */
    {"MB_RENDERER_RESET_COMMAND", DD_PERF_RECORDING}, /* 77 */
    {"MB_RENDERER_IMAGE", DD_PERF_RESOURCES}, /* 78 */
    {"MB_RENDERER_BARRIER", DD_PERF_RECORDING}, /* 79 */
    {"MB_RENDERER_COPY", DD_PERF_RECORDING}, /* 80 */
    {"MB_RENDERER_CLEAR", DD_PERF_RECORDING}, /* 81 */
    {"MB_RENDERER_IDLE", DD_PERF_SYNC}, /* 82 */
    {"MB_SESSION_BEGIN", DD_PERF_PRESENT}, /* 83 */
    {"MB_SESSION_PRESENT", DD_PERF_PRESENT}, /* 84 */
    {"MB_SESSION_END", DD_PERF_PRESENT}, /* 85 */
    {"MB_SESSION_STATS", DD_PERF_PRESENT}, /* 86 */
    {"MB_NORMAL_BEGIN", DD_PERF_PRESENT}, /* 87 */
    {"MB_NORMAL_REGISTER", DD_PERF_PRESENT}, /* 88 */
    {"MB_NORMAL_PUBLISH", DD_PERF_PRESENT}, /* 89 */
    {"MB_NORMAL_END", DD_PERF_PRESENT}, /* 90 */
    {"MB_NORMAL_STATS", DD_PERF_PRESENT}, /* 91 */
};
#ifdef __cplusplus
static_assert(sizeof(dd_perf_opcodes) / sizeof(dd_perf_opcodes[0]) == DD_PERF_OPS, "opcode profile coverage");
#else
_Static_assert(sizeof(dd_perf_opcodes) / sizeof(dd_perf_opcodes[0]) == DD_PERF_OPS, "opcode profile coverage");
#endif

static inline void dd_perf_op_merge(struct dd_perf_op *p, const struct dd_perf_op *q) {
    p->count += q->count; p->ns += q->ns;
    if (q->worst > p->worst) p->worst = q->worst;
}
static inline double dd_perf_percent(uint64_t value, uint64_t total) {
    return total ? 100.0 * (double)value / (double)total : 0;
}
static inline struct dd_perf_op dd_perf_categories(const struct dd_perf_rpc *rpc,
        struct dd_perf_op categories[DD_PERF_CATEGORIES]) {
    struct dd_perf_op total = {0, 0, 0};
    for (unsigned i = 0; i < DD_PERF_CATEGORIES; ++i) categories[i] = total;
    for (unsigned i = 0; i < DD_PERF_OPS; ++i) {
        dd_perf_op_merge(&total, &rpc->op[i]);
        dd_perf_op_merge(&categories[dd_perf_opcodes[i].category], &rpc->op[i]);
    }
    return total;
}
/* Called only at the existing two-second report boundary. All values are
 * client request/reply wall time, including failed calls. No timers, heap
 * allocations, locks or RPCs here. Frames retains the existing count scope;
 * outside includes periodic STATS/events, excluding startup and teardown. */
static inline void dd_perf_report_opcodes(FILE *out, const struct dd_perf_rpc *frame,
        const struct dd_perf_rpc *outside, unsigned frames, unsigned attempts) {
    struct dd_perf_rpc window = *frame;
    struct dd_perf_op categories[DD_PERF_CATEGORIES], frame_categories[DD_PERF_CATEGORIES];
    dd_perf_merge(&window, outside);
    struct dd_perf_op total = dd_perf_categories(&window, categories);
    struct dd_perf_op frame_total = dd_perf_categories(frame, frame_categories);
    double denominator = frames ? frames : 1;
    unsigned most_count = DD_PERF_OPS, most_time = DD_PERF_OPS, most_category = DD_PERF_CATEGORIES;
    fprintf(out, "MaliPerf opcode-window: scope=normal-loop frames=%u attempts=%u count=%llu frame-count=%llu outside-count=%llu frame-RPCs/frame=%.2f client-RTT=%.3fms (frame counts match existing RPC report; includes failed calls)\n",
        frames, attempts, (unsigned long long)total.count, (unsigned long long)frame_total.count,
        (unsigned long long)(total.count - frame_total.count), frame_total.count / denominator, total.ns / 1e6);
    for (unsigned i = 0; i < DD_PERF_OPS; ++i) {
        const struct dd_perf_op *op = &window.op[i];
        if (!op->count) continue;
        if (frame->op[i].count && (most_count == DD_PERF_OPS || frame->op[i].count > frame->op[most_count].count)) most_count = i;
        if (most_time == DD_PERF_OPS || op->ns > window.op[most_time].ns) most_time = i;
        fprintf(out, "MaliPerf opcode: op=%u name=%s category=%s count=%llu count-pct=%.2f frame-count=%llu outside-count=%llu frame-RPCs/frame=%.2f RTT-total=%.3fms RTT-avg=%.3fms RTT-max=%.3fms\n",
            i, dd_perf_opcodes[i].name, dd_perf_category_names[dd_perf_opcodes[i].category],
            (unsigned long long)op->count, dd_perf_percent(op->count, total.count),
            (unsigned long long)frame->op[i].count, (unsigned long long)outside->op[i].count,
            frame->op[i].count / denominator, op->ns / 1e6, op->ns / (double)op->count / 1e6, op->worst / 1e6);
    }
    for (unsigned i = 0; i < DD_PERF_CATEGORIES; ++i) {
        const struct dd_perf_op *op = &categories[i];
        if (op->count && (most_category == DD_PERF_CATEGORIES || op->ns > categories[most_category].ns)) most_category = i;
        fprintf(out, "MaliPerf category: name=%s count=%llu count-pct=%.2f frame-count=%llu outside-count=%llu frame-RPCs/frame=%.2f RTT-total=%.3fms RTT-pct=%.2f RTT-avg=%.3fms RTT-max=%.3fms\n",
            dd_perf_category_names[i], (unsigned long long)op->count, dd_perf_percent(op->count, total.count),
            (unsigned long long)frame_categories[i].count, (unsigned long long)(op->count - frame_categories[i].count),
            frame_categories[i].count / denominator, op->ns / 1e6, dd_perf_percent(op->ns, total.ns),
            op->count ? op->ns / (double)op->count / 1e6 : 0, op->worst / 1e6);
    }
    fprintf(out, "MaliPerf RPC leaders: most-frame-count-op=%s largest-RTT-op=%s largest-RTT-category=%s command-recording-RTT-pct=%.2f\n",
        most_count < DD_PERF_OPS ? dd_perf_opcodes[most_count].name : "none",
        most_time < DD_PERF_OPS ? dd_perf_opcodes[most_time].name : "none",
        most_category < DD_PERF_CATEGORIES ? dd_perf_category_names[most_category] : "none",
        dd_perf_percent(categories[DD_PERF_RECORDING].ns, total.ns));
}
#endif
