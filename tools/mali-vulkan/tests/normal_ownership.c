/* Real ownership registry; simulated release callbacks. No GPU claim. */
#define _POSIX_C_SOURCE 200809L
#include "../../../app/src/main/cpp/malivulkan/normal_ownership.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
struct AHardwareBuffer { unsigned refs; AHardwareBuffer_Desc desc; };
void AHardwareBuffer_acquire(AHardwareBuffer *a) { assert(a->refs); ++a->refs; }
void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a->refs); --a->refs; }
void AHardwareBuffer_describe(const AHardwareBuffer *a, AHardwareBuffer_Desc *desc) { *desc = a->desc; }
static void *release_later(void *data) {
    struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    mb_normal_release(*(uint32_t *)data); return NULL;
}
int main(void) {
    struct AHardwareBuffer buffers[3]; memset(buffers, 0, sizeof(buffers)); uint32_t keys[3];
    for (unsigned i = 0; i < 3; ++i) {
        buffers[i].refs = 1; buffers[i].desc.width = 1280; buffers[i].desc.height = 720;
        keys[i] = mb_normal_register(&buffers[i]); assert(keys[i]);
        assert(!mb_normal_import(keys[i], 256, 256));
        assert(mb_normal_import(keys[i], 1280, 720) == &buffers[i]);
        assert(!mb_normal_import(keys[i], 1280, 720));
        assert(mb_normal_publish(keys[i], i + 1, 100 + i, 200 + i));
        assert(mb_normal_submit(keys[i])); assert(mb_normal_owned(keys[i]));
        mb_normal_present_timing(keys[i], 1234);
        struct mb_normal_state measured; assert(mb_normal_snapshot(keys[i], &measured));
        assert(measured.published_ns && measured.submitted_ns >= measured.published_ns);
        assert(measured.timing.present_calls == 1 && measured.timing.present_ns == 1234);
        assert(measured.timing.held_ns == 0 && measured.released == 0);
        assert(!mb_normal_publish(keys[i], 10, 10, 10));
        mb_normal_not_submitted(keys[i]); assert(mb_normal_owned(keys[i]));
    }
    mb_normal_release(keys[0]); mb_normal_release(keys[1]);
    pthread_t thread; assert(!pthread_create(&thread, NULL, release_later, &keys[2]));
    assert(mb_normal_wait(keys, 3, 200)); pthread_join(thread, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        struct mb_normal_state state; assert(mb_normal_snapshot(keys[i], &state));
        assert(!state.owned && state.presented == 1 && state.released == 1);
        assert(state.timing.held_ns > 0); /* Actual callback, never a timeout. */
    }
    assert(mb_normal_publish(keys[2], 99, 77, 88)); assert(mb_normal_submit(keys[2]));
    struct mb_normal_state before_timeout; assert(mb_normal_snapshot(keys[2], &before_timeout));
    mb_normal_retire(keys[2]); assert(!mb_normal_wait(keys, 3, 30));
    struct mb_normal_state state; assert(mb_normal_snapshot(keys[2], &state));
    assert(state.owned && state.frame == 99 && state.sync == 77 && state.fence == 88 && state.presented == 2 && state.released == 1);
    assert(state.timing.held_ns == before_timeout.timing.held_ns);
    assert(!mb_normal_publish(keys[2], 100, 78, 89)); assert(mb_normal_submit(keys[2])); /* same held Android buffer, no producer reuse */
    mb_normal_detach(keys[2]); AHardwareBuffer_release(&buffers[2]);
    mb_normal_forget(keys[2]); assert(mb_normal_owned(keys[2])); assert(buffers[2].refs == 2);
    mb_normal_release(keys[2]); assert(!mb_normal_owned(keys[2])); assert(buffers[2].refs == 1);
    for (unsigned i = 0; i < 2; ++i) {
        mb_normal_retire(keys[i]); mb_normal_forget(keys[i]); mb_normal_detach(keys[i]);
        AHardwareBuffer_release(&buffers[i]); assert(buffers[i].refs == 1);
    }
    uint32_t key = mb_normal_register(&buffers[0]); assert(key && key != keys[0]);
    assert(mb_normal_import(key, 1280, 720)); assert(mb_normal_publish(key, 1, 2, 3));
    mb_normal_retire(key); mb_normal_detach(key); AHardwareBuffer_release(&buffers[0]);
    assert(mb_normal_wait(&key, 1, 1)); mb_normal_forget(key); assert(buffers[0].refs == 1);
    puts("normal ownership: real release before reuse; delayed release; bounded missing release; retained allocation; balanced references; restart passed");
    return 0;
}
