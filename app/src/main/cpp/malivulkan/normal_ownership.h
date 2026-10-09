/* Android-local AHB registry shared by broker and normal Wayland compositor.
 * IDs cross the guest boundary; pointers and native handles never do. */
#ifndef DROIDDECK_NORMAL_OWNERSHIP_H
#define DROIDDECK_NORMAL_OWNERSHIP_H
#include <android/hardware_buffer.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct mb_normal_state {
    uint32_t key, frame, sync, fence, owned, submitted, consumers, retired, presented, released;
};
uint32_t mb_normal_register(AHardwareBuffer *buffer);
AHardwareBuffer *mb_normal_import(uint32_t key, uint32_t width, uint32_t height);
int mb_normal_publish(uint32_t key, uint32_t frame, uint32_t sync, uint32_t fence);
int mb_normal_submit(uint32_t key);
void mb_normal_release(uint32_t key);
void mb_normal_not_submitted(uint32_t key);
void mb_normal_detach(uint32_t key);
int mb_normal_owned(uint32_t key);
int mb_normal_snapshot(uint32_t key, struct mb_normal_state *state);
/* Retire forbids any new import/submission. It never acknowledges a release. */
void mb_normal_retire(uint32_t key);
void mb_normal_forget(uint32_t key);
int mb_normal_wait(const uint32_t *keys, unsigned count, unsigned timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
