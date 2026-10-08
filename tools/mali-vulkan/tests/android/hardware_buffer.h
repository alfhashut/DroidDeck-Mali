#ifndef MB_MOCK_HARDWARE_BUFFER_H
#define MB_MOCK_HARDWARE_BUFFER_H
#include <stdint.h>
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_CPU_READ_RARELY 2u
#define AHARDWAREBUFFER_USAGE_CPU_READ_MASK 15u
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE 256u
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT 512u
#define AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY 2048u
struct AHardwareBuffer;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *, AHardwareBuffer **);
void AHardwareBuffer_acquire(AHardwareBuffer *);
void AHardwareBuffer_release(AHardwareBuffer *);
void AHardwareBuffer_describe(const AHardwareBuffer *, AHardwareBuffer_Desc *);
int AHardwareBuffer_lock(AHardwareBuffer *, uint64_t, int, const void *, void **);
int AHardwareBuffer_unlock(AHardwareBuffer *, int *);
#endif
