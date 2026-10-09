/* Optional normal Wayland transport on wire v7. Diagnostic 83-86 stay unchanged. */
#ifndef DD_NORMAL_PROTOCOL_H
#define DD_NORMAL_PROTOCOL_H
#define MB_NORMAL_BEGIN 87u /* device,verbose -> no payload */
#define MB_NORMAL_REGISTER 88u /* device,AHB -> Android-local registry key */
#define MB_NORMAL_PUBLISH 89u /* device,AHB,sync,frame -> no payload */
#define MB_NORMAL_END 90u /* device -> no payload; real bounded release wait */
#define MB_NORMAL_STATS 91u /* device -> 20 resources + presented,released,owned,timeouts,FDcreated,FDclosed */
#define MB_NORMAL_MAX_DIMENSION 4096u
#define MB_NORMAL_MAX_MEMORY (64u * 1024u * 1024u)
#endif
