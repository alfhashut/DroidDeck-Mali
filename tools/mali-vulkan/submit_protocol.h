/* Version 5: bounded core-1.0 submission subset. All values LE, no handles. */
#ifndef DROIDDECK_SUBMIT_PROTOCOL_H
#define DROIDDECK_SUBMIT_PROTOCOL_H
#include "device_protocol.h"
#define MB_SUBMIT_VERSION 5u
#define MB_SUBMIT_MAX_OBJECTS 32u
#define MB_SUBMIT_TIMEOUT_NS UINT64_C(5000000000)
enum {
    MB_POOL_CREATE = 17, MB_POOL_DESTROY, MB_COMMAND_ALLOCATE, MB_COMMAND_FREE,
    MB_COMMAND_BEGIN, MB_COMMAND_END, MB_EVENT_CREATE, MB_EVENT_DESTROY,
    MB_EVENT_STATUS, MB_COMMAND_SET_EVENT, MB_FENCE_CREATE, MB_FENCE_DESTROY,
    MB_FENCE_STATUS, MB_FENCE_WAIT, MB_QUEUE_SUBMIT
};
/* Every request starts with device ID. Remaining u32 fields:
 * POOL_CREATE family,flags; POOL_DESTROY pool;
 * COMMAND_ALLOCATE pool,level,count; COMMAND_FREE pool,command;
 * COMMAND_BEGIN command,flags; COMMAND_END command;
 * EVENT/FENCE_CREATE flags; EVENT/FENCE_DESTROY/STATUS object;
 * COMMAND_SET_EVENT command,event,stage;
 * FENCE_WAIT fence,waitAll,timeout (LE u64);
 * QUEUE_SUBMIT queue,command,fence (0 means no fence).
 * Creates/allocate: prefix count=1 + u32 ID. Others: prefix count=0.
 * Status/wait replies preserve VkResult including positive status values.
 * No pNext/allocator/semantics hidden in unimplemented fields: ICD rejects them.
 */
#endif
