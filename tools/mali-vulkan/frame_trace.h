/* Development-only journal. No renderer/wire/ownership changes. */
#ifndef DD_FRAME_TRACE_H
#define DD_FRAME_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum dd_ft_stage {
    DD_FT_COMMIT, DD_FT_SELECT, DD_FT_COALESCE, DD_FT_DISCARD,
    DD_FT_FRAME_BEGIN, DD_FT_FRAME_END, DD_FT_SHM_BEGIN, DD_FT_SHM_END,
    DD_FT_ACQUIRE_BEGIN, DD_FT_ACQUIRE_END, DD_FT_RENDER_BEGIN, DD_FT_RENDER_END,
    DD_FT_WAIT_BEGIN, DD_FT_WAIT_END, DD_FT_SUBMIT_BEGIN, DD_FT_SUBMIT_END,
    DD_FT_PUBLISH, DD_FT_OUTER_COMMIT, DD_FT_OUTER_RELEASE, DD_FT_POOL_BLOCKED,
    DD_FT_HANDOFF_BEGIN, DD_FT_HANDOFF_END, DD_FT_ANDROID_SUBMIT, DD_FT_ANDROID_REPEAT,
    DD_FT_RELEASE_CALLBACK, DD_FT_RELEASE_QUEUED, DD_FT_RELEASE_FENCE_PENDING,
    DD_FT_RELEASE_FENCE_OBSERVED, DD_FT_POOL_RETURN, DD_FT_CANCEL,
    DD_FT_TRANSACTION_COMPLETE, DD_FT_LATCH_TIME, DD_FT_COPY_BEGIN, DD_FT_COPY_END,
    DD_FT_SHM_LOOKUP_BEGIN, DD_FT_SHM_LOOKUP_END, DD_FT_SHM_SOURCE_BEGIN, DD_FT_SHM_SOURCE_END,
    DD_FT_STAGING_ACQUIRE_BEGIN, DD_FT_STAGING_ACQUIRE_END,
    DD_FT_STAGING_CREATE_BEGIN, DD_FT_STAGING_CREATE_END,
    DD_FT_STAGING_MAP_BEGIN, DD_FT_STAGING_MAP_END,
    DD_FT_STAGING_VALIDATE_BEGIN, DD_FT_STAGING_VALIDATE_END,
    DD_FT_SHM_SOURCE_RELEASE_BEGIN, DD_FT_SHM_SOURCE_RELEASE_END,
    DD_FT_TEXTURE_CREATE_BEGIN, DD_FT_TEXTURE_CREATE_END,
    DD_FT_UPLOAD_RECORD_BEGIN, DD_FT_UPLOAD_RECORD_END,
    DD_FT_UPLOAD_PREPARE_BEGIN, DD_FT_UPLOAD_PREPARE_END,
    DD_FT_UPLOAD_SUBMIT_BEGIN, DD_FT_UPLOAD_SUBMIT_END,
    DD_FT_UPLOAD_FINALIZE_BEGIN, DD_FT_UPLOAD_FINALIZE_END,
    DD_FT_UPLOAD_QUEUE_BEGIN, DD_FT_UPLOAD_QUEUE_END,
    DD_FT_UPLOAD_WAIT_BEGIN, DD_FT_UPLOAD_WAIT_END,
    DD_FT_SHM_CLEANUP_BEGIN, DD_FT_SHM_CLEANUP_END, DD_FT_COUNT
};
/* fence is the broker producer-fence resource ID, not a VkFence handle or FD.
 * sync is the broker resource ID of the exported SYNC_FD wrapper, not its FD. */
struct dd_ft_id { uint64_t client, attempt, scope; uint32_t frame, key, sync, fence; };
/* Call init only at normal-session start, after an explicit adb-created arm
 * marker. Disabled calls do no clocks, locks, allocations, or file access. */
void dd_ft_init(const char *role);
void dd_ft_shutdown(void);
int dd_ft_active(void);
/* Native-only diagnostic namespace; never sent over the renderer protocol. */
uint64_t dd_ft_new_scope(void);
void dd_ft_event(enum dd_ft_stage stage, struct dd_ft_id id,
                 uint64_t object, uint64_t value, int32_t status, int sample_cpu);
#ifdef __cplusplus
}
struct DDFrameScope {
    dd_ft_stage end; dd_ft_id id; uint64_t object, value;
    DDFrameScope(dd_ft_stage begin, dd_ft_stage finishStage, dd_ft_id identity,
                 uint64_t obj = 0, uint64_t val = 0, bool enabled = true)
        : end(enabled ? finishStage : DD_FT_COUNT), id(identity), object(obj), value(val) {
        if (enabled) dd_ft_event(begin, id, object, value, 0, 1);
    }
    DDFrameScope(const DDFrameScope &) = delete;
    DDFrameScope &operator=(const DDFrameScope &) = delete;
    void finish() {
        if (end != DD_FT_COUNT) { dd_ft_event(end, id, object, value, 0, 1); end = DD_FT_COUNT; }
    }
    ~DDFrameScope() { finish(); }
};
#endif
#endif
