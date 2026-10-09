#ifndef DROIDDECK_INTEROP_CONSUMER_H
#define DROIDDECK_INTEROP_CONSUMER_H
/* Producer FD is borrowed; SurfaceControl takes a duplicate. */
int mb_consumer_present(AHardwareBuffer *, const AHardwareBuffer_Desc *, int);
int mb_consumer_present_renderer(AHardwareBuffer *, const AHardwareBuffer_Desc *, int);
struct mb_consumer_session;
struct mb_consumer_session *mb_consumer_session_open(void);
/* Previous AHB is released only after its real Android release fence. */
int mb_consumer_session_present(struct mb_consumer_session *, AHardwareBuffer *, const AHardwareBuffer_Desc *, int);
int mb_consumer_session_owns(struct mb_consumer_session *, AHardwareBuffer *);
/* Five-second stop budget. VK_TIMEOUT (2) retains the consumer/current AHB;
 * caller must not destroy their native Vulkan allocation/parents or retry reuse. */
int mb_consumer_session_close(struct mb_consumer_session *);
#endif
