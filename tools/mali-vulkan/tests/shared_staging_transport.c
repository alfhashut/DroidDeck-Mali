#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "shared_staging.h"
#include "frame_trace_proxy.h"
static uint64_t proxy_trace_upload_completed(void *device) { (void)device;return 0; }
static unsigned open_fds(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;
    while(readdir(d)) ++n;
    closedir(d);return n;
}
static void fd_cases(void) {
    struct mb_shared_region region={0};int fd=mb_shared_create(&region,4097);assert(fd>=0);
    assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);assert(ftruncate(fd,0)<0);assert(ftruncate(fd,100000)<0);
    assert(!mb_shared_file_bytes(0)&&!mb_shared_file_bytes(UINT64_MAX));
    assert(mb_shared_file_bytes(MB_SHARED_MAX_BYTES)==MB_SHARED_MAX_BYTES+64u);
    assert(!mb_shared_map_writer(fd,MB_SHARED_MAX_BYTES+1u,64));
    assert(!mb_shared_map_writer(fd,4097,3));
    assert(!mb_shared_map_writer(fd,4096,64)); // exact rounded file size required
    int unsealed=(int)syscall(SYS_memfd_create,"unsealed-test",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    assert(unsealed>=0 && !ftruncate(unsealed,(off_t)mb_shared_file_bytes(4097)));
    assert(!mb_shared_map_writer(unsealed,4097,64));close(unsealed);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    for(unsigned count=0;count<=10;++count) {
        unsigned before=open_fds();char byte='S';struct iovec io={&byte,1};
        union { struct cmsghdr align;char data[CMSG_SPACE(10*sizeof(int))]; } control={0};
        struct msghdr msg={.msg_iov=&io,.msg_iovlen=1};
        if(count) {
            msg.msg_control=control.data;msg.msg_controllen=CMSG_SPACE(count*sizeof(int));
            struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
            for(unsigned i=0;i<count;++i)memcpy((char *)CMSG_DATA(c)+i*sizeof(int),&fd,sizeof(fd));
        }
        assert(sendmsg(sockets[0],&msg,MSG_NOSIGNAL)==1);int received=mb_shared_recv_fd(sockets[1]);
        if(count==1){assert(received>=0&&(fcntl(received,F_GETFD)&FD_CLOEXEC));close(received);}
        else assert(received<0);
        assert(open_fds()==before); // every delivered extra/truncated FD closed
    }
    char wrong='X';assert(send(sockets[0],&wrong,1,0)==1);assert(mb_shared_recv_fd(sockets[1])<0);
    close(sockets[0]);close(sockets[1]);close(fd);mb_shared_release(&region);mb_shared_release(&region);
}
static void processes(void) {
    unsigned before=open_fds();struct mb_shared_region region={0};int fd=mb_shared_create(&region,4097);assert(fd>=0);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));pid_t child=fork();assert(child>=0);
    if(!child) {
        close(sockets[0]);close(fd);mb_shared_release(&region);
        int received=mb_shared_recv_fd(sockets[1]);assert(received>=0);
        void *map=mb_shared_map_writer(received,4097,64);close(received);assert(map);
        memset(map,0x5d,4097);__atomic_store_n(mb_shared_sequence(map,4097),1,__ATOMIC_RELEASE);
        uint8_t ack=1;assert(!mb_write(sockets[1],&ack,1));
        munmap(map,mb_shared_file_bytes(4097));close(sockets[1]);_exit(0);
    }
    close(sockets[1]);assert(!mb_shared_send_fd(sockets[0],fd));close(fd);uint8_t ack=0;assert(!mb_read(sockets[0],&ack,1)&&ack==1);
    assert(__atomic_load_n(mb_shared_sequence(region.map,4097),__ATOMIC_ACQUIRE)==1);
    for(unsigned i=0;i<4097;++i)assert(((uint8_t *)region.map)[i]==0x5d);
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    close(sockets[0]);mb_shared_release(&region);assert(open_fds()==before);
}
/* The real proxy exchange is extracted below; Vulkan is not linked. */
CONSTANTS
struct proxy_instance { uint32_t wire_version;int fd, shared_received_fd; };
static unsigned session_frames;
#define ERROR(...) ((void)0)
RPC_EXCHANGE
struct server { int fd, decline, malformed; struct mb_shared_region region; };
static void *server_reply(void *arg) {
    struct server *s=arg;uint8_t header[16],body[16],reply[12]={0};
    assert(!mb_read(s->fd,header,16)&&mb_get_u32(header+8)==MB_SHARED_REGISTER&&mb_get_u32(header+12)==16);
    assert(!mb_read(s->fd,body,16));
    if(s->decline){mb_put_u32(reply,MB_UNSUPPORTED);mb_put_u32(reply+4,(uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);}
    mb_put_u32(header+12,12);assert(!mb_write(s->fd,header,16)&&!mb_write(s->fd,reply,12));
    if(!s->decline) {
        int fd=mb_shared_create(&s->region,4097);assert(fd>=0);
        if(s->malformed) { char byte='X';assert(send(s->fd,&byte,1,0)==1); }
        else assert(!mb_shared_send_fd(s->fd,fd));
        close(fd);
    }
    return NULL;
}
static void exchange(int decline, int malformed) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct server server={.fd=sockets[1],.decline=decline,.malformed=malformed};
    pthread_t thread;assert(!pthread_create(&thread,NULL,server_reply,&server));
    struct proxy_instance instance={7,sockets[0],-1};uint8_t request[16]={0},reply[12];uint32_t bytes=0;
    VkResult result=rpc_exchange(&instance,MB_SHARED_REGISTER,request,16,reply,&bytes,12);
    assert(!pthread_join(thread,NULL));
    if(decline)assert(result==VK_ERROR_FEATURE_NOT_PRESENT&&instance.shared_received_fd<0);
    else if(malformed)assert(result!=VK_SUCCESS&&instance.shared_received_fd<0);
    else {
        assert(result==VK_SUCCESS&&bytes==12&&instance.shared_received_fd>=0);
        void *map=mb_shared_map_writer(instance.shared_received_fd,4097,64);assert(map);close(instance.shared_received_fd);
        memset(map,42,4097);__atomic_store_n(mb_shared_sequence(map,4097),7,__ATOMIC_RELEASE);
        assert(__atomic_load_n(mb_shared_sequence(server.region.map,4097),__ATOMIC_ACQUIRE)==7);
        assert(((char *)server.region.map)[4096]==42);munmap(map,mb_shared_file_bytes(4097));
    }
    mb_shared_release(&server.region);close(sockets[0]);close(sockets[1]);
}
struct probe_server { int fd, old; };
static void *probe_reply(void *arg) {
    struct probe_server *s=arg;int fd=accept(s->fd,NULL,NULL);assert(fd>=0);uint8_t header[16],reply[24]={0};
    assert(!mb_read(fd,header,16)&&mb_get_u32(header+8)==MB_SHARED_PROBE);
    mb_put_u32(header+12,s->old?12:24);mb_put_u32(reply,s->old?MB_PROTOCOL_ERROR:MB_OK);mb_put_u32(reply+8,s->old?0:1);
    mb_put_u32(reply+12,MB_SHARED_VERSION);mb_put_u32(reply+16,MB_SHARED_MAX_BYTES);mb_put_u32(reply+20,MB_SHARED_MAX_REGIONS);
    assert(!mb_write(fd,header,16)&&!mb_write(fd,reply,s->old?12:24));close(fd);return NULL;
}
static void probe(int old) {
    char path[100];snprintf(path,sizeof(path),"/tmp/dd-shared-probe-%d",(int)getpid());
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);assert(!bind(fd,(void *)&address,sizeof(address))&&!listen(fd,1));
    struct probe_server server={fd,old};pthread_t thread;assert(!pthread_create(&thread,NULL,probe_reply,&server));
    assert(mb_shared_probe(path)==!old);assert(!pthread_join(thread,NULL));close(fd);unlink(path);
}
int main(int argc,char **argv) {
    assert(argc==2);(void)proxy_DroidDeckFrameTraceMALI;
    if(!strcmp(argv[1],"fds"))fd_cases();else if(!strcmp(argv[1],"processes"))processes();
    else if(!strcmp(argv[1],"exchange"))exchange(0,0);else if(!strcmp(argv[1],"decline"))exchange(1,0);
    else if(!strcmp(argv[1],"malformed"))exchange(0,1);else if(!strcmp(argv[1],"old_probe"))probe(1);else if(!strcmp(argv[1],"probe"))probe(0);
    else assert(0);
    puts("PASS");return 0;
}
