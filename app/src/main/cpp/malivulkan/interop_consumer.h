#ifndef DROIDDECK_INTEROP_CONSUMER_H
#define DROIDDECK_INTEROP_CONSUMER_H
/* Synchronous: retains the actual AHB through transaction completion and release fence.
 * The passed producer fd is borrowed; only a duplicate is transferred to SurfaceControl. */
int mb_consumer_present(AHardwareBuffer *, const AHardwareBuffer_Desc *, int);
#endif

int mb_consumer_present_renderer(AHardwareBuffer *, const AHardwareBuffer_Desc *, int);
