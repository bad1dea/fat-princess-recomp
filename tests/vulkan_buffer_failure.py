#!/usr/bin/env python3
"""Fault-inject the production Vulkan buffer constructor at every ownership step."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'third_party/ps3recomp/libs/video/rsx_vulkan_backend.c').read_text()
start=s.index('typedef struct vk_buffer {');structure=s[start:s.index('} vk_buffer;',start)+len('} vk_buffer;')]
start=s.index('static int vk_create_host_buffer(');function=s[start:s.index('\nstatic void vk_destroy_buffer(',start)]
fixture=r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint32_t u32;
static struct {VkDevice device;}s_vk;
static int fail, buffers, memories, mappings;
#define VK_LOG(...) do {if(0)fprintf(stderr,__VA_ARGS__);}while(0)
static VkResult pvkCreateBuffer(VkDevice d,const VkBufferCreateInfo*i,const VkAllocationCallbacks*a,VkBuffer*b){if(fail==1)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*b=(VkBuffer)(uintptr_t)1;buffers++;return VK_SUCCESS;}
static void pvkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements*r){*r=(VkMemoryRequirements){.size=4096,.alignment=256,.memoryTypeBits=1};}
static int vk_find_memory_type(u32 bits,VkMemoryPropertyFlags flags,u32*t){if(fail==5)return -1;*t=0;return 0;}
static VkResult pvkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo*i,const VkAllocationCallbacks*a,VkDeviceMemory*m){if(fail==2)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*m=(VkDeviceMemory)(uintptr_t)2;memories++;return VK_SUCCESS;}
static VkResult pvkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){return fail==3?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
static VkResult pvkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags flags,void**p){if(fail==4)return VK_ERROR_MEMORY_MAP_FAILED;*p=(void*)(uintptr_t)3;mappings++;return VK_SUCCESS;}
static void pvkUnmapMemory(VkDevice d,VkDeviceMemory m){assert(mappings==1);mappings--;}
static void pvkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks*a){assert(buffers==1);buffers--;}
static void pvkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks*a){assert(memories==1);memories--;}
'''
test=r'''
int main(void){
 for(unsigned pass=0;pass<500;pass++)for(fail=1;fail<=5;fail++){
  vk_buffer b={0},zero={0};
  assert(vk_create_host_buffer(&b,1024,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)==-1);
  assert(!buffers&&!memories&&!mappings);assert(!memcmp(&b,&zero,sizeof b));
 }
 fail=0;
 for(unsigned pass=0;pass<500;pass++){
  vk_buffer b={0};assert(!vk_create_host_buffer(&b,1024,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
  assert(b.size==1024 && b.coherent && buffers==1 && memories==1 && mappings==1);
  pvkUnmapMemory(s_vk.device,b.mem);pvkDestroyBuffer(s_vk.device,b.buf,NULL);pvkFreeMemory(s_vk.device,b.mem,NULL);
 }
 assert(!buffers&&!memories&&!mappings);
 puts("PASS: 2500 failed Vulkan constructions release partial ownership and reset handles; 500 successful constructions preserve mapping/size/coherence");
}
'''
with tempfile.TemporaryDirectory(prefix='fp-buffer-failure-')as tmp:
 source=Path(tmp)/'test.c';source.write_text(fixture+structure+'\n'+function+'\n'+test)
 binary=Path(tmp)/'test'
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG',*shlex.split(os.environ.get('CPPFLAGS','')),str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
