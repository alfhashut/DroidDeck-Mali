/* Ordinary CPU shared memory only. No GPU external-memory or Vulkan sync ABI. */
#ifndef DD_SHARED_STAGING_H
#define DD_SHARED_STAGING_H
#include "protocol.h"
#include "shared_staging_types.h"
#include <fcntl.h>
#include <linux/memfd.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
/* Opcodes are outside the legacy performance snapshot array (ABI unchanged).
 * Existing frame-journal exchange/shared scopes observe their timing/counts. */
#define MB_SHARED_PROBE 92u /* isolated connection, empty -> version,max bytes,max regions */
#define MB_SHARED_REGISTER 93u /* device,memory,u64 bytes -> ACK, then one SCM_RIGHTS FD */
#define MB_SHARED_WRITE 94u /* device,memory,u64 offset,u64 bytes,u64 sequence -> ACK */
#define MB_SHARED_DROP 95u /* device,memory -> ACK; does not unmap Vulkan memory */
/* One byte prevents stream partial-send ambiguity: SCM_RIGHTS is sent once. */
static inline int mb_shared_send_fd(int socket, int fd) {
    char byte = 'S'; struct iovec io = {&byte, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr msg = {.msg_iov=&io, .msg_iovlen=1, .msg_control=control.bytes, .msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS; c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd)); ssize_t n;
    do { n=sendmsg(socket, &msg, MSG_NOSIGNAL); } while (n<0 && errno==EINTR);
    return n==1 ? 0 : -1;
}
/* Close every delivered descriptor on malformed/truncated control data. */
static inline int mb_shared_recv_fd(int socket) {
    char byte = 0; struct iovec io = {&byte, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(8*sizeof(int))]; } control = {0};
    struct msghdr msg = {.msg_iov=&io, .msg_iovlen=1, .msg_control=control.bytes, .msg_controllen=sizeof(control.bytes)};
    ssize_t n; do { n=recvmsg(socket, &msg, MSG_CMSG_CLOEXEC); } while(n<0 && errno==EINTR);
    int received[8], count=0, bad=n!=1 || byte!='S' || (msg.msg_flags & (MSG_CTRUNC|MSG_TRUNC));
    if (n<0) return -1;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c)) {
        if (c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0)) { bad=1; continue; }
        size_t bytes=c->cmsg_len-CMSG_LEN(0); if (bytes%sizeof(int)) bad=1;
        for (size_t i=0; i<bytes/sizeof(int); ++i) {
            int fd; memcpy(&fd, (char *)CMSG_DATA(c)+i*sizeof(int),sizeof(fd));
            if (count<8) received[count++]=fd; else { close(fd); bad=1; }
        }
    }
    if (!bad && count==1) return received[0];
    for (int i=0; i<count; ++i) close(received[i]);
    errno=EPROTO; return -1;
}
/* Broker creates storage, so no guest FD or untrusted file size is mapped. */
static inline int mb_shared_create(struct mb_shared_region *region, uint64_t bytes) {
    size_t length=mb_shared_file_bytes(bytes); if (!length) { errno=EINVAL; return -1; }
#ifdef SYS_memfd_create
    int fd=(int)syscall(SYS_memfd_create, "droiddeck-cpu-staging", MFD_CLOEXEC|MFD_ALLOW_SEALING);
#else
    int fd=-1; errno=ENOSYS;
#endif
    if (fd<0) return -1;
    if (ftruncate(fd,(off_t)length) || fcntl(fd,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0) { close(fd); return -1; }
    void *map=mmap(NULL,length,PROT_READ,MAP_SHARED,fd,0);
    if (map==MAP_FAILED) { close(fd); return -1; }
    *region=(struct mb_shared_region){map,bytes,0}; return fd;
}
static inline void *mb_shared_map_writer(int fd, uint64_t bytes, size_t alignment) {
    size_t length=mb_shared_file_bytes(bytes); struct stat st;
    int seals=fcntl(fd,F_GET_SEALS);
    if (!length || !alignment || (alignment&(alignment-1)) || fstat(fd,&st) || !S_ISREG(st.st_mode) ||
        st.st_size!=(off_t)length || seals<0 || (seals&(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))!=(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)) return NULL;
    void *map=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (map==MAP_FAILED) return NULL;
    if ((uintptr_t)map%alignment) { munmap(map,length); return NULL; }
    return map;
}
/* Unsupported/old broker errors close only this disposable probe connection. */
static inline int mb_shared_probe(const char *path) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if (!path || strlen(path)>=sizeof(address.sun_path)) return 0;
    strcpy(address.sun_path,path); int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); if(fd<0)return 0;
    struct timeval timeout={.tv_sec=2}; uint8_t header[MB_HEADER_BYTES], reply[MB_PREFIX_BYTES+12]; int supported=0;
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) || setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        connect(fd,(struct sockaddr *)&address,sizeof(address))) goto done;
    mb_session_header(header,MB_SHARED_PROBE,0); mb_put_u32(header+4,7u);
    if (mb_write(fd,header,sizeof(header)) || mb_read(fd,header,sizeof(header)) ||
        mb_get_u32(header)!=MB_MAGIC || mb_get_u32(header+4)!=7u || mb_get_u32(header+8)!=MB_SHARED_PROBE) goto done;
    if (mb_get_u32(header+12)==MB_PREFIX_BYTES) { (void)mb_read(fd,reply,MB_PREFIX_BYTES); goto done; }
    if (mb_get_u32(header+12)!=sizeof(reply) || mb_read(fd,reply,sizeof(reply))) goto done;
    supported=mb_get_u32(reply)==MB_OK && !mb_get_u32(reply+4) && mb_get_u32(reply+8)==1 &&
        mb_get_u32(reply+12)==MB_SHARED_VERSION && mb_get_u32(reply+16)==MB_SHARED_MAX_BYTES && mb_get_u32(reply+20)==MB_SHARED_MAX_REGIONS;
done: close(fd); return supported;
}
#endif
