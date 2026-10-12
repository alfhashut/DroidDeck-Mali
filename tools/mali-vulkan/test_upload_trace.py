#!/usr/bin/env python3
"""Small host-only transport regression; no Vulkan headers, driver, or renderer build."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parent

class UploadTransportTests(unittest.TestCase):
    def test_actual_rpc_short_circuit_cleanup_sampling_and_local_sink(self):
        compiler=shutil.which('cc')
        if not compiler: self.skipTest('host C compiler unavailable')
        source=(ROOT/'icd_proxy.c').read_text().split('static VkResult rpc_exchange(',1)[1].split('/* Callers already hold',1)[0]
        function='static VkResult rpc_exchange('+source
        vk=sorted(set(re.findall(r'\bVK_[A-Z_]+\b',function))-{'VK_SUCCESS'})
        constants='enum { VK_SUCCESS=0, '+', '.join(f'{name}={-i}' for i,name in enumerate(vk,1))+' };\n'
        mb=sorted(set(re.findall(r'\bMB_[A-Z_]+\b',function))-{'MB_HEADER_BYTES','MB_PREFIX_BYTES','MB_MAGIC','MB_SUBMIT_VERSION','MB_OK'})
        constants+='enum { MB_HEADER_BYTES=16, MB_PREFIX_BYTES=12, MB_MAGIC=0x12345678, MB_SUBMIT_VERSION=5, MB_OK=0, '+', '.join(f'{name}={20+i}' for i,name in enumerate(mb))+' };\n'
        harness=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int VkResult;
CONSTANTS
static int writes, reads, fail_write, bad_header, records, session_frames;
static unsigned begin_count[256], end_count[256];
static void mb_put_u32(void *p, uint32_t x) { memcpy(p,&x,4); }
static uint32_t mb_get_u32(const void *p) { uint32_t x;memcpy(&x,p,4);return x; }
static void mb_session_header(uint8_t *p, uint32_t op, uint32_t n) {
    mb_put_u32(p,MB_MAGIC);mb_put_u32(p+4,2);mb_put_u32(p+8,op);mb_put_u32(p+12,n);
}
static int mb_write(int fd, const void *p, size_t n) {
    (void)fd;(void)p;(void)n;return ++writes==fail_write ? -1:0;
}
static int mb_read(int fd, void *p, size_t n) {
    (void)fd; ++reads;memset(p,0,n);
    if (reads==1) { mb_session_header(p,777,MB_PREFIX_BYTES);if(bad_header)mb_put_u32(p,0); }
    return 0;
}
#include "frame_trace_proxy.h"
static void sink(enum dd_ft_stage stage, struct dd_ft_id id, uint64_t object,
                 uint64_t value, int32_t status, int cpu) {
    (void)object;(void)value;(void)status;(void)cpu;
    assert(id.attempt==8); ++records;
    if (stage==DD_FT_RPC_EXCHANGE_BEGIN || stage==DD_FT_RPC_HEADER_BEGIN || stage==DD_FT_RPC_SEND_BEGIN ||
        stage==DD_FT_RPC_RECEIVE_BEGIN || stage==DD_FT_RPC_DECODE_BEGIN) ++begin_count[stage];
    else ++end_count[stage];
}
struct proxy_instance { uint32_t wire_version; int fd; };
#define ERROR(...) ((void)0)
FUNCTION
static void reset(void) { writes=reads=records=fail_write=bad_header=0;memset(begin_count,0,sizeof(begin_count));memset(end_count,0,sizeof(end_count)); }
static int exchange(void) {
    struct proxy_instance s={2,-1};uint8_t request[4]={0}, reply[12];uint32_t n=0;
    return rpc_exchange(&s,777,request,sizeof(request),reply,&n,sizeof(reply));
}
int main(void) {
    struct dd_ft_id id={0};id.attempt=8;
    proxy_DroidDeckFrameTraceMALI(NULL,sink,&id);
    reset();assert(exchange()==VK_SUCCESS);assert(writes==2&&reads==2);
    assert(begin_count[DD_FT_RPC_SEND_BEGIN]==2&&end_count[DD_FT_RPC_SEND_END]==2);
    assert(begin_count[DD_FT_RPC_RECEIVE_BEGIN]==2&&end_count[DD_FT_RPC_RECEIVE_END]==2);
    assert(begin_count[DD_FT_RPC_EXCHANGE_BEGIN]==1&&end_count[DD_FT_RPC_EXCHANGE_END]==1);
    reset();fail_write=2;assert(exchange()!=VK_SUCCESS);assert(writes==2&&reads==0);
    assert(end_count[DD_FT_RPC_DECODE_END]==0&&end_count[DD_FT_RPC_EXCHANGE_END]==1);
    reset();bad_header=1;assert(exchange()!=VK_SUCCESS);assert(reads==1);
    assert(end_count[DD_FT_RPC_DECODE_END]==0&&end_count[DD_FT_RPC_EXCHANGE_END]==1);
    reset();id.attempt=7;proxy_DroidDeckFrameTraceMALI(NULL,sink,&id);
    assert(exchange()==VK_SUCCESS);assert(writes==2&&reads==2&&records==0);
    reset();proxy_DroidDeckFrameTraceMALI(NULL,NULL,NULL);
    assert(exchange()==VK_SUCCESS);assert(writes==2&&reads==2&&records==0);
    return 0;
}
'''.replace('CONSTANTS',constants).replace('FUNCTION',function)
        with tempfile.TemporaryDirectory(prefix='dd-upload-transport-') as tmp:
            path=Path(tmp);(path/'test.c').write_text(harness)
            subprocess.run([compiler,'-std=c11','-O0','-Wall','-Wextra','-Werror','-I'+str(ROOT),
                            str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

    def test_actual_broker_receive_scope_selection_and_decoded_ids(self):
        compiler=shutil.which('cc')
        if not compiler: self.skipTest('host C compiler unavailable')
        source=(ROOT/'../../app/src/main/cpp/malivulkan/system_broker.c').read_text()
        functions='static int broker_trace_read_request('+source.split('static int broker_trace_read_request(',1)[1].split('static void serve(',1)[0]
        harness=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "frame_trace.h"
enum { MB_MAX_LOGICAL_DEVICES=4, MB_MEMORY_WRITE=42, MB_RENDERER_SUBMIT=100 };
struct native_device { struct { int enabled; uint64_t trace_scope; } normal; uint32_t id; };
struct vk_session { struct native_device logical[4]; };
static int active=1, reads, result, events;static uint64_t scope, object, value;static int status;
int dd_ft_active(void) { return active; }
void dd_ft_event(enum dd_ft_stage stage, struct dd_ft_id id, uint64_t obj, uint64_t val, int32_t code, int cpu) {
    (void)stage;(void)cpu; ++events;scope=id.scope;object=obj;value=val;status=code;
}
static int mb_read(int fd, uint8_t *p, uint32_t n) { (void)fd;(void)p;(void)n;++reads;return result; }
static uint32_t mb_get_u32(const uint8_t *p) { uint32_t x;memcpy(&x,p,4);return x; }
static uint64_t mb_get_u64(const uint8_t *p) { uint64_t x;memcpy(&x,p,8);return x; }
FUNCTIONS
int main(void) {
    struct vk_session s={0};uint8_t request[28]={0};uint32_t device=3, resource=77;uint64_t offset=524288;
    assert(!broker_trace_read_request(&s,-1,request,28,MB_MEMORY_WRITE));assert(reads==1&&events==0);
    s.logical[0].normal.enabled=1;s.logical[0].normal.trace_scope=9;s.logical[0].id=device;
    reads=events=0;assert(!broker_trace_read_request(&s,-1,request,28,MB_MEMORY_WRITE));
    assert(reads==1&&events==2&&scope==9&&object==MB_MEMORY_WRITE&&value==28);
    s.logical[1].normal.enabled=1;s.logical[1].normal.trace_scope=10;s.logical[1].id=4;
    reads=events=0;assert(!broker_trace_read_request(&s,-1,request,28,MB_MEMORY_WRITE));assert(reads==1&&events==0);
    memcpy(request,&device,4);memcpy(request+4,&resource,4);memcpy(request+8,&offset,8);
    struct dd_ft_id id=broker_trace_request(&s,MB_MEMORY_WRITE,request,20,&object,&value);
    assert(id.scope==9&&object==77&&value==offset); /* exact device, not first measured device guess */
    s.logical[1].normal.enabled=0;reads=events=0;result=-1;
    assert(broker_trace_read_request(&s,-1,request,28,MB_MEMORY_WRITE)==-1);assert(reads==1&&events==2&&status==-1);
    active=0;reads=events=0;assert(broker_trace_read_request(&s,-1,request,28,MB_MEMORY_WRITE)==-1);
    assert(reads==1&&events==0);assert(!broker_trace_request(&s,MB_MEMORY_WRITE,request,20,&object,&value).scope);
    return 0;
}
'''.replace('FUNCTIONS',functions)
        with tempfile.TemporaryDirectory(prefix='dd-upload-receive-') as tmp:
            path=Path(tmp);(path/'test.c').write_text(harness)
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT),
                            str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__': unittest.main()
