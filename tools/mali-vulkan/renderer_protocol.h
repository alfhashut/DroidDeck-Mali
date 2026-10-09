/* v7: isolated real Gamescope BLIT renderer subset. LE scalars; no native handles. */
#ifndef DD_RENDERER_PROTOCOL_H
#define DD_RENDERER_PROTOCOL_H
#define MB_RENDERER_VERSION 7u
#define MB_RENDERER_MAX_SHADER (512u * 1024u)
#define MB_RENDERER_MAX_REQUEST (MB_RENDERER_MAX_SHADER + 256u)
#define MB_RENDERER_MAX_OBJECTS 128u
#define MB_RENDERER_MAX_DESCRIPTORS 64u
#define MB_RENDERER_SEMAPHORE_CREATE 60u /* device,u64 initial -> ID */
#define MB_RENDERER_DESTROY 61u /* device,kind,ID */
#define MB_RENDERER_COUNTER 62u /* device,semaphore -> u64 counter */
#define MB_RENDERER_WAIT 63u /* device,semaphore,u64 value,u64 timeout */
#define MB_RENDERER_VIEW 64u /* device,image,type,format,usage -> ID */
#define MB_RENDERER_SAMPLER 65u /* device,nearest,unnormalized -> ID */
#define MB_RENDERER_SET_LAYOUT 66u /* device,count; binding,type,count,immutableSampler each -> ID */
#define MB_RENDERER_PIPELINE_LAYOUT 67u /* device,setLayout -> ID */
#define MB_RENDERER_POOL 68u /* device,maxSets,count; type,count each -> ID */
#define MB_RENDERER_SETS 69u /* device,pool,count,layout[count] -> IDs[count] */
#define MB_RENDERER_UPDATE 70u /* device,count; set,binding,element,type,count then descriptors */
#define MB_RENDERER_SHADER 71u /* device,byteLength,exact aligned SPIR-V -> ID */
#define MB_RENDERER_PIPELINE 72u /* device,shader,layout,7 specialization u32 -> ID */
#define MB_RENDERER_BIND_PIPELINE 73u /* device,command,pipeline */
#define MB_RENDERER_BIND_SET 74u /* device,command,layout,set */
#define MB_RENDERER_DISPATCH 75u /* device,command,x,y,z */
#define MB_RENDERER_SUBMIT 76u /* device,queue,command,fence,semaphore,u64 signal */
#define MB_RENDERER_RESET_COMMAND 77u /* device,command */
#define MB_RENDERER_IMAGE 78u /* device,type,width,height,depth,usage,flags,formatCount,formats[] -> ID */
#define MB_RENDERER_BARRIER 79u /* device,command,srcStage,dstStage,count; image,srcAccess,dstAccess,old,new,srcFamily,dstFamily each */
#define MB_RENDERER_COPY 80u /* device,command,image,buffer,layout,direction,u64 offset,width,height,depth */
#define MB_RENDERER_CLEAR 81u /* device,command,image,layout,4 binary32 */
#define MB_RENDERER_IDLE 82u /* device */
/* Explicit opt-in sub-protocol on v7; legacy renderer requests are unchanged. */
#define MB_SESSION_BEGIN 83u /* device -> no payload */
#define MB_SESSION_PRESENT 84u /* device,AHB,sync -> previous released AHB ID */
#define MB_SESSION_END 85u /* device -> last released AHB ID */
#define MB_SESSION_STATS 86u /* device -> 20 live counts + presented/released/FD totals */
#define MB_SESSION_COUNTS 20u
/* devices,queues,command pools,commands,semaphores,fences,buffers,memory,
 * images,views,samplers,descriptor pools,sets,shaders,pipelines,layouts,
 * AHB tokens,sync tokens,open exported FDs,pending commands. */
enum mb_renderer_kind { MB_R_SEMAPHORE=1, MB_R_VIEW, MB_R_SAMPLER, MB_R_SET_LAYOUT,
 MB_R_PIPELINE_LAYOUT, MB_R_POOL, MB_R_SET, MB_R_SHADER, MB_R_PIPELINE };
#endif
