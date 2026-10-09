/* Version 6: bounded diagnostic operations, little endian scalars and byte payloads.
 * Every request starts with device ID. IDs are connection-local, typed, never reused.
 * No addresses, descriptors, Vulkan structs or arbitrary pNext chains on the wire.
 * Nonempty replies have prefix count=1, followed by the explicitly described data.
 */
#ifndef DROIDDECK_INTEROP_PROTOCOL_H
#define DROIDDECK_INTEROP_PROTOCOL_H
#include <stdint.h>
#define MB_INTEROP_VERSION 6u
#define MB_INTEROP_MAX_MEMORY (1024u * 1024u)
#define MB_INTEROP_CHUNK 4096u /* legacy v6 reads/writes */
#define MB_INTEROP_BULK_VERSION 7u
/* Per-message bound, not an allocation/mapped-range limit. Fits the existing
 * renderer request buffer (512 KiB + 256); larger ranges use multiple writes. */
#define MB_INTEROP_WRITE_MAX (512u * 1024u)
/* Existing broker reply bound, minus its 12-byte status/result/count prefix.
 * This is a message limit; larger mapped ranges use multiple bulk reads. */
#define MB_INTEROP_READ_MAX (128u * 1024u - 12u)
#define MB_MEMORY_WRITE_HEADER 20u
#define MB_INTEROP_MAX_REFS 16u
#define MB_BUFFER_CREATE 32u /* device,u64 size,u32 usage -> ID */
#define MB_BUFFER_DESTROY 33u /* device,ID */
#define MB_BUFFER_REQUIREMENTS 34u /* device,ID -> u64 size,u64 alignment,u32 bits */
#define MB_MEMORY_ALLOCATE 35u /* device,u64 size,u32 type -> ID */
#define MB_MEMORY_FREE 36u /* device,ID */
#define MB_BUFFER_BIND 37u /* device,buffer,memory,u64 offset */
#define MB_MEMORY_MAP 38u /* device,memory,u64 offset,u64 size */
#define MB_MEMORY_UNMAP 39u /* device,memory */
#define MB_MEMORY_READ 40u /* device,memory,u64 absolute offset,u32 length -> bytes; v7 bulk */
#define MB_MEMORY_WRITE 41u /* same header, then length bytes; v7 bulk writes allowed */
#define MB_MEMORY_FLUSH 42u /* device,memory,u64 offset,u64 size */
#define MB_MEMORY_INVALIDATE 43u /* same; native range, atom alignment enforced */
#define MB_IMAGE_CREATE 44u /* device,width,height,usage -> ID; fixed optimal RGBA8 */
#define MB_IMAGE_DESTROY 45u /* device,ID */
#define MB_IMAGE_REQUIREMENTS 46u /* same layout as buffer requirements */
#define MB_IMAGE_BIND 47u /* same layout as buffer bind */
#define MB_COMMAND_FILL 48u /* device,command,buffer,u64 offset,u64 size,pattern */
#define MB_COMMAND_BARRIER 49u /* device,command,srcStage,dstStage,kind,resource,
                               srcAccess,dstAccess,oldLayout,newLayout,srcFamily,dstFamily */
#define MB_COMMAND_CLEAR 50u /* device,command,image,layout,4 binary32 color components */
#define MB_COMMAND_COPY 51u /* device,command,image,buffer,layout,width,height,direction
                            direction=0 image->buffer,1 buffer->image; tightly packed */
#define MB_AHB_CREATE 52u /* device,width,height -> image,memory,AHB IDs,description(4 u32),usage(u64) */
#define MB_AHB_RELEASE 53u /* device,AHB ID; image/memory must already be destroyed */
#define MB_SYNC_FENCE_CREATE 54u /* device,handleType(SYNC_FD) -> fence ID */
#define MB_SYNC_EXPORT 55u /* device,fence,handleType(SYNC_FD) -> sync ID */
#define MB_SYNC_WAIT 56u /* device,sync ID,u32 timeout milliseconds <=5000 */
#define MB_SYNC_CLOSE 57u /* device,sync ID; pending producer rejected */
#define MB_AHB_INSPECT 58u /* device,AHB,sync,pattern(0 clear,1 quadrants) -> CPU checked (0/1) */
#define MB_AHB_PRESENT 59u /* device,AHB,sync -> completed consumer handoff; no pixel copying */
static inline uint32_t mb_interop_write_limit(uint32_t version) {
    return version >= MB_INTEROP_BULK_VERSION ? MB_INTEROP_WRITE_MAX : MB_INTEROP_CHUNK;
}
static inline uint32_t mb_interop_read_limit(uint32_t version) {
    return version >= MB_INTEROP_BULK_VERSION ? MB_INTEROP_READ_MAX : MB_INTEROP_CHUNK;
}
/* Subtraction-based checks never evaluate offset+length or map_offset+map_size. */
static inline int mb_interop_mapped_range(uint64_t allocation, uint64_t map_offset,
        uint64_t map_size, uint64_t offset, uint64_t length) {
    return length && map_offset <= allocation && map_size <= allocation - map_offset &&
        offset >= map_offset && offset - map_offset <= map_size &&
        length <= map_size - (offset - map_offset);
}
static inline int mb_interop_write_size(uint32_t version, uint32_t bytes, uint64_t length) {
    return bytes >= MB_MEMORY_WRITE_HEADER && length && length <= mb_interop_write_limit(version) &&
        length == bytes - MB_MEMORY_WRITE_HEADER;
}
#endif
