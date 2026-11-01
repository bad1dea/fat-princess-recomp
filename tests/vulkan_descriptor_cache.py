#!/usr/bin/env python3
"""Exercise the actual Vulkan descriptor-update helper without a GPU."""
from pathlib import Path
import subprocess
import os
import shlex
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "third_party/ps3recomp/libs/video/rsx_vulkan_backend.c").read_text()
struct_start = source.index("struct vk_eng_desc_state {")
state = source[struct_start:source.index("\n};", struct_start) + 3]
helper_start = source.index("static int vk_desc_buffer_equal(")
helper = source[helper_start:source.index("/* Fill the set the bound pipeline uses:", helper_start)]
preamble = r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t u32;
#define RSX_MAX_TEXTURES 16
#define RSX_MAX_VERTEX_TEXTURES 4
#define RSX_SPIRV_VPCONST_BINDING 0
#define RSX_SPIRV_PSCONST_BINDING 1
#define RSX_SPIRV_TEXTURE_BINDING 16
#define RSX_SPIRV_SAMPLER_BINDING 48
static struct {VkDevice device;} s_vk;
static uint64_t s_descriptor_epoch=1;
static uint64_t s_descriptor_calls,s_descriptor_skipped,s_descriptor_written,s_descriptor_reused;
static void update(VkDevice,u32,const VkWriteDescriptorSet*,u32,const VkCopyDescriptorSet*);
static PFN_vkUpdateDescriptorSets pvkUpdateDescriptorSets=update;
'''
# Exercise the production invalidation call sites as well as the update helper.
slot_start = source.index("static struct vk_draw_slot {")
slot = source[slot_start:source.index("} s_draw_slot[", slot_start)].replace("static struct", "struct", 1) + "};\n"
prepare_start = source.index("static int vk_draw_slot_prepare(")
prepare = source[prepare_start:source.index("\n}", prepare_start) + 2]
destroy_start = source.index("static void vk_destroy_image(")
destroy = source[destroy_start:source.index("\n}", destroy_start) + 2]
invalidation = r'''
typedef struct {VkBuffer buf;VkDeviceSize size;int coherent;} vk_buffer;
typedef struct {VkImageView view;VkImage img;VkDeviceMemory mem;} vk_image;
static struct {struct {VkDeviceSize size;} ubo;} s_g;
static int vk_ensure_buffer(vk_buffer*b,VkDeviceSize size,VkBufferUsageFlags usage){
 (void)usage;if(!b->buf||b->size<size){b->buf=(VkBuffer)(uintptr_t)9;b->size=size;b->coherent=1;}return 0;
}
static VkResult pool_create(VkDevice d,const VkDescriptorPoolCreateInfo*i,const VkAllocationCallbacks*a,VkDescriptorPool*p){
 (void)d;(void)a;assert(i->maxSets==2);*p=(VkDescriptorPool)(uintptr_t)7;return VK_SUCCESS;
}
static PFN_vkCreateDescriptorPool pvkCreateDescriptorPool=pool_create;
static int destroyed_views;
static int vk_flush_at(const char*r){(void)r;return 0;}
static void view_destroy(VkDevice d,VkImageView v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;++destroyed_views;}
static void image_destroy(VkDevice d,VkImage v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;}
static void memory_free(VkDevice d,VkDeviceMemory v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;}
static PFN_vkDestroyImageView pvkDestroyImageView=view_destroy;
static PFN_vkDestroyImage pvkDestroyImage=image_destroy;
static PFN_vkFreeMemory pvkFreeMemory=memory_free;
''' + slot + prepare + destroy
body = r'''
#define H(type,n) ((type)(uintptr_t)(n))
struct actual {VkDescriptorBufferInfo bi[2];VkDescriptorImageInfo ii[16],si[16],vi[4],vs[4];};
static struct actual actual[8];
static int split_mode[8];
static u32 last_writes,last_descriptors;
static void update(VkDevice device,u32 n,const VkWriteDescriptorSet*w,u32 copies,const VkCopyDescriptorSet*c){
 (void)device;(void)c;assert(!copies);last_writes=n;last_descriptors=0;
 for(u32 j=0;j<n;j++){
  unsigned set=(unsigned)(uintptr_t)w[j].dstSet;assert(set<8);
  struct actual*a=&actual[set];unsigned b=w[j].dstBinding,u=w[j].dstArrayElement;
  last_descriptors+=w[j].descriptorCount;
  if(w[j].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){assert(b<2);a->bi[b]=*w[j].pBufferInfo;continue;}
  VkDescriptorImageInfo*dst=0;unsigned start=0,max=0;
  if(w[j].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE){
   if(b>=32){assert(b<36);dst=a->vi;start=b-32+u;max=4;}
   else {assert(b>=16);dst=a->ii;start=(split_mode[set]?b-16:0)+u;max=16;}
  }else{
   assert(w[j].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER);
   if(b>=64){assert(b<68);dst=a->vs;start=b-64+u;max=4;}
   else{assert(b==48);dst=a->si;start=u;max=16;}
  }
  assert(start+w[j].descriptorCount<=max);
  for(unsigned k=0;k<w[j].descriptorCount;k++)dst[start+k]=w[j].pImageInfo[k];
 }
}
static void verify(unsigned set,const struct actual*expected){
 struct actual*a=&actual[set];
 for(int i=0;i<2;i++)assert(vk_desc_buffer_equal(&a->bi[i],&expected->bi[i]));
 for(int i=0;i<16;i++){assert(vk_desc_image_equal(&a->ii[i],&expected->ii[i]));assert(vk_desc_image_equal(&a->si[i],&expected->si[i]));}
 for(int i=0;i<4;i++){assert(vk_desc_image_equal(&a->vi[i],&expected->vi[i]));assert(vk_desc_image_equal(&a->vs[i],&expected->vs[i]));}
}
static void draw(unsigned set,int split,struct vk_eng_desc_state*cache,struct actual*values){
 split_mode[set]=split;last_writes=last_descriptors=0;
 vk_eng_update_descriptor_state(H(VkDescriptorSet,set),split,cache,values->bi,values->ii,values->si,values->vi,values->vs);
 verify(set,values);
}
static uint32_t random_state=0xdeadbeef;
static unsigned rnd(void){random_state=random_state*1664525u+1013904223u;return random_state;}
int main(void){
 struct vk_draw_slot slot={0};s_g.ubo.size=64;slot.descriptors.valid=1;
 assert(!vk_draw_slot_prepare(&slot,64,64));assert(!slot.descriptors.valid);
 slot.descriptors.valid=1;assert(!vk_draw_slot_prepare(&slot,64,64));assert(slot.descriptors.valid);
 VkBuffer old_buffer=slot.constants.buf;s_g.ubo.size=128;
 assert(!vk_draw_slot_prepare(&slot,64,64));assert(!slot.descriptors.valid&&slot.constants.buf==old_buffer);
 vk_image image={H(VkImageView,8),H(VkImage,9),H(VkDeviceMemory,10)};
 uint64_t epoch=s_descriptor_epoch;vk_destroy_image(&image);
 assert(s_descriptor_epoch==epoch+1&&destroyed_views==1&&!image.view);
 vk_destroy_image(&image);assert(s_descriptor_epoch==epoch+1&&destroyed_views==1);
 struct actual values={0};struct vk_eng_desc_state cache={0};
 for(unsigned i=0;i<2;i++)values.bi[i]=(VkDescriptorBufferInfo){H(VkBuffer,i+1),0,16};
 for(unsigned i=0;i<16;i++){values.ii[i]=(VkDescriptorImageInfo){0,H(VkImageView,i+10),VK_IMAGE_LAYOUT_GENERAL};values.si[i]=(VkDescriptorImageInfo){H(VkSampler,i+20),0,0};}
 for(unsigned i=0;i<4;i++){values.vi[i]=values.ii[i];values.vs[i]=values.si[i];}
 draw(1,0,&cache,&values);assert(last_descriptors==42);
 for(unsigned i=0;i<200;i++){draw(1,0,&cache,&values);assert(!last_writes);}
 values.bi[0].range=32;draw(1,0,&cache,&values);assert(last_descriptors==1);
 values.bi[1].offset=256;draw(1,0,&cache,&values);assert(last_descriptors==1);
 values.ii[0].imageView=H(VkImageView,99);values.ii[15].imageView=H(VkImageView,98);
 draw(1,0,&cache,&values);assert(last_writes==2&&last_descriptors==2);
 for(int i=1;i<=3;i++)values.ii[i].imageView=H(VkImageView,100+i);
 draw(1,0,&cache,&values);assert(last_writes==1&&last_descriptors==3);
 values.si[2].sampler=H(VkSampler,111);draw(1,0,&cache,&values);assert(last_descriptors==1);
 values.vi[3].imageView=H(VkImageView,120);values.vs[0].sampler=H(VkSampler,130);
 draw(1,0,&cache,&values);assert(last_descriptors==2);
 draw(2,1,&cache,&values);assert(last_descriptors==42);draw(2,1,&cache,&values);assert(!last_writes);
 for(int i=1;i<=3;i++)values.ii[i].imageView=H(VkImageView,150+i);
 draw(2,1,&cache,&values);assert(last_writes==3&&last_descriptors==3);
 ++s_descriptor_epoch;draw(2,1,&cache,&values);assert(last_descriptors==42);
 cache.valid=0;draw(2,1,&cache,&values);assert(last_descriptors==42);
 draw(1,0,&cache,&values);assert(last_descriptors==42);
 // Poison only padding: equality must inspect descriptor fields, not memcmp.
 for(unsigned i=0;i<16;i++){
  VkDescriptorImageInfo item=values.ii[i];memset(&values.ii[i],0xa5,sizeof values.ii[i]);
  values.ii[i].sampler=item.sampler;values.ii[i].imageView=item.imageView;values.ii[i].imageLayout=item.imageLayout;
 }
 draw(1,0,&cache,&values);assert(!last_writes);
 for(unsigned i=0;i<5000;i++){
  unsigned j=rnd()%16;
  switch(rnd()%9){
  case 0:values.bi[j%2].offset=(rnd()%4)*256;break;
  case 1:values.bi[j%2].range=16+(rnd()%2)*16;break;
  case 2:values.bi[j%2].buffer=H(VkBuffer,10000+i);break;
  case 3:values.ii[j].imageView=H(VkImageView,10000+i);break;
  case 4:values.si[j].sampler=H(VkSampler,10000+i);break;
  case 5:values.vi[j%4].imageView=H(VkImageView,10000+i);break;
  case 6:values.vs[j%4].sampler=H(VkSampler,10000+i);break;
  case 7:values.ii[j].imageLayout=(i&1)?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;break;
  default:break;
  }
  if(i%37==0)++s_descriptor_epoch;
  if(i%71==0)cache.valid=0;
  unsigned set=(i%19==0)?2:1;draw(set,set==2,&cache,&values);
  draw(3,0,0,&values);assert(last_descriptors==42); // full-write baseline
 }
 printf("PASS: 5000 descriptor transitions, normal/split layouts, partial arrays, buffer offsets/ranges, epoch/slot invalidation, padding and uncached baseline; reused=%llu\n",(unsigned long long)s_descriptor_reused);
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="fp-descriptors-") as tmp:
        path=Path(tmp);(path/"test.c").write_text(preamble+state+helper+invalidation+body)
        subprocess.run(["cc",*shlex.split(os.environ.get("CPPFLAGS", "")),"-std=gnu17","-O2","-Wall","-Wextra",str(path/"test.c"),"-o",str(path/"test")],check=True)
        subprocess.run([str(path/"test")],check=True)
