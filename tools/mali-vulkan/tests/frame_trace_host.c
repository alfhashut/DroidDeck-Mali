/* Tiny real-journal harness: no graphics or Android runtime. */
#define _POSIX_C_SOURCE 200809L
#include "../frame_trace.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void *flood(void *arg) {
    struct dd_ft_id id={0}; id.frame=8; id.key=(uint32_t)(uintptr_t)arg;
    for (unsigned i=0;i<50000;i++) dd_ft_event(DD_FT_PUBLISH,id,0,i,0,1);
    return NULL;
}
int main(int argc,char **argv) {
    dd_ft_init("gamescope");
    printf("%llu\n",(unsigned long long)now());fflush(stdout);
    struct timespec delay={0,1000000};
    if (argc>1 && !strcmp(argv[1],"disabled")) {
        flood((void *)1);dd_ft_shutdown();return 0;
    }
    uint64_t deadline=now()+3000000000ULL;
    while (!dd_ft_active() && now()<deadline) nanosleep(&delay,NULL);
    if (!dd_ft_active()) return 2;
    /* Wait for the controller's future start, without assuming active == in-window. */
    struct timespec warm={0,180000000};nanosleep(&warm,NULL);
    if (argc>1 && !strcmp(argv[1],"flood")) {
        pthread_t a,b;pthread_create(&a,NULL,flood,(void *)1);pthread_create(&b,NULL,flood,(void *)2);
        pthread_join(a,NULL);pthread_join(b,NULL);
    } else {
        struct dd_ft_id id={0};id.client=8;id.attempt=8;id.frame=8;id.key=3;id.sync=4;id.fence=5;
        for (unsigned i=0;i<100;i++) {
            dd_ft_event(DD_FT_RENDER_BEGIN,id,2,i,0,1);
            for (volatile unsigned j=0;j<5000;j++) {}
            dd_ft_event(DD_FT_RENDER_END,id,2,i,0,1);
            nanosleep(&delay,NULL);
        }
    }
    while (dd_ft_active() && now()<deadline) nanosleep(&delay,NULL);
    dd_ft_shutdown();return 0;
}
