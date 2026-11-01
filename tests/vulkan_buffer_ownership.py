#!/usr/bin/env python3
"""Exercise production retirement/fence and pooled draw-buffer preparation."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'third_party/ps3recomp/libs/video/rsx_vulkan_backend.c').read_text()
def section(begin,end):
 a=s.index(begin);return s[a:s.index(end,a)]
structure=section('typedef struct vk_buffer {','\ntypedef struct vk_image')
retired=section('typedef struct { vk_buffer buffer; unsigned pending_sets; }','\nstatic int vk_ensure_buffer(')
wait=section('static int vk_set_wait(unsigned set, const char* reason)\n{','\nstatic void vk_free_retired_buffers(void);')
preamble=r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define VK_ASYNC_SETS 3
#define ST_SET_WAIT 0
#define VK_CHECK(expr,label) do{if((expr)!=VK_SUCCESS)return -1;}while(0)
static struct{VkDevice device;}s_vk;
static VkDeviceSize s_buffer_live_bytes,s_buffer_peak_bytes;
static unsigned s_buffer_live_count;
static unsigned s_cur_set,s_batch_recording,s_set_inflight[3];
static VkFence s_set_fence[3];
static struct{double submit_wait_ms;}s_debug_stats;
static int async=1,wait_error,wait_calls,drains;
static unsigned destroyed[64];
static int vk_async_enabled(void){return async;}
static uint64_t SDL_GetPerformanceCounter(void){return 1;}
static double vk_ms_since(uint64_t t){return 0;}
static int vk_stall_enabled(void){return 0;}
static void vk_stall_end(unsigned k,uint64_t t,uint64_t n){}
static void vk_trace_submit(const char*r,unsigned n,double t){}
static VkResult pvkWaitForFences(VkDevice d,uint32_t n,const VkFence*f,VkBool32 all,uint64_t t){wait_calls++;return wait_error?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
static void pvkUnmapMemory(VkDevice d,VkDeviceMemory m){}
static void pvkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks*a){assert(++destroyed[(uintptr_t)b]==1);}
static void pvkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks*a){}
'''
prototypes='''static void vk_retired_complete_set(unsigned);\nstatic void vk_free_retired_buffers(void);\nstatic void vk_destroy_buffer(vk_buffer*);\n'''
test=r'''
static void vk_destroy_buffer(vk_buffer*b){drains++;vk_destroy_buffer_now(b);}
static vk_buffer make(unsigned id,unsigned size){s_buffer_live_count++;s_buffer_live_bytes+=size;return(vk_buffer){.buf=(VkBuffer)(uintptr_t)id,.mem=(VkDeviceMemory)(uintptr_t)id,.ptr=(void*)(uintptr_t)id,.size=size,.coherent=1};}
int main(void){
 s_set_inflight[0]=s_set_inflight[2]=1;s_cur_set=1;s_batch_recording=1;
 for(unsigned i=1;i<=3;i++){vk_buffer b=make(i,1024);vk_retire_buffer(&b);assert(!b.buf);}
 assert(s_retired_count==3 && s_buffer_live_count==3 && !wait_calls);
 assert(!vk_set_wait(0,"test"));assert(s_retired_count==3);
 /* A failed wait must not release anything or forget that set's fence. */
 wait_error=1;assert(vk_set_wait(2,"failure")==-1);assert(s_set_inflight[2] && s_buffer_live_count==3);
 wait_error=0;assert(!vk_set_wait(2,"test"));assert(s_buffer_live_count==3);
 /* The recording batch has not completed; reuse of another set cannot free it. */
 s_set_inflight[0]=1;assert(!vk_set_wait(0,"new generation"));assert(s_buffer_live_count==3);
 s_batch_recording=0;s_set_inflight[1]=1;assert(!vk_set_wait(1,"recorded batch"));
 assert(!s_retired_count && !s_buffer_live_bytes && !s_buffer_live_count && !drains);
 vk_buffer b=make(4,128);vk_retire_buffer(&b);assert(destroyed[4] && !s_retired_count);
 s_set_inflight[0]=1;b=make(5,256);vk_retire_buffer(&b);
 s_set_inflight[1]=1;b=make(6,512);vk_retire_buffer(&b);
 assert(!vk_set_wait(0,"only first can die"));assert(destroyed[5] && !destroyed[6] && s_retired_count==1);
 assert(!vk_set_wait(1,"second"));vk_free_retired_buffers();assert(destroyed[6]);
 assert(!s_retired_count && !s_buffer_live_count && !s_buffer_live_bytes);
 puts("PASS: actual fence waits retain recording/in-flight buffers, failed waits retain ownership, completed generations reclaim safely without added waits or global drains");
}
'''
prepare=section('static int vk_draw_slot_prepare(','\n/* Descriptor contents')
poolfixture=r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define RSX_MAX_TEXTURES 16
#define RSX_MAX_VERTEX_TEXTURES 4
static struct{VkDevice device;}s_vk;
static struct{vk_buffer ubo;}s_g;
struct vk_draw_slot{vk_buffer vertices,indices,constants;struct{int valid;}descriptors;VkDescriptorPool pool;};
static unsigned vertex_calls,index_calls,constant_calls;
static struct vk_draw_slot*active;
static int vk_ensure_buffer(vk_buffer*b,VkDeviceSize n,VkBufferUsageFlags usage){assert(n);if(b==&active->vertices)vertex_calls++;else if(b==&active->indices)index_calls++;else constant_calls++;b->coherent=1;b->size=n;b->buf=(VkBuffer)(uintptr_t)1;return 0;}
static VkResult pvkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo*i,const VkAllocationCallbacks*a,VkDescriptorPool*p){*p=(VkDescriptorPool)(uintptr_t)1;return VK_SUCCESS;}
'''
pooltest=r'''
int main(void){s_g.ubo.size=4096;
 for(unsigned v=0;v<2;v++)for(unsigned i=0;i<2;i++){
  struct vk_draw_slot slot={0};active=&slot;vertex_calls=index_calls=constant_calls=0;
  assert(!vk_draw_slot_prepare(&slot,v?0:2097152,i?0:1048576));
  assert(vertex_calls==!v && index_calls==!i && constant_calls==1);
  assert((slot.vertices.buf!=0)==!v && (slot.indices.buf!=0)==!i && slot.constants.buf);
 }
 puts("PASS: real draw-slot preparation allocates only unpooled vertex/index data; pooled geometry needs constants/descriptors only");}
'''
with tempfile.TemporaryDirectory(prefix='fp-buffer-ownership-')as tmp:
 for name,code in [('retirement',preamble+structure+prototypes+wait+retired+test),('pooled','#include <vulkan/vulkan.h>\n'+structure+poolfixture+prepare+pooltest)]:
  src=Path(tmp)/(name+'.c');src.write_text(code);binary=Path(tmp)/name
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG',*shlex.split(os.environ.get('CPPFLAGS','')),str(src),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
