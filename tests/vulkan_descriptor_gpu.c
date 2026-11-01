/* Runs the production descriptor-update helper against a real Vulkan device.
 * The Python runner extracts that helper into descriptor-cache-under-test.h. */
#include "descriptor-cache-under-test.h"
#include <stdlib.h>
#include <math.h>
#define VK(call) do{VkResult r=(call);if(r!=VK_SUCCESS){fprintf(stderr,"Vulkan failure %d at line %d\n",r,__LINE__);abort();}}while(0)
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory;
static VkQueue queue;
static VkCommandBuffer command;
static VkFence fence;
struct buffer {VkBuffer handle;VkDeviceMemory memory;void* data;};
struct image {VkImage handle;VkDeviceMemory memory;VkImageView view;};
static unsigned memory_type(unsigned bits,VkMemoryPropertyFlags flags){
 for(unsigned i=0;i<memory.memoryTypeCount;i++)if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
 abort();
}
static struct buffer make_buffer(VkDeviceSize size,VkBufferUsageFlags usage){
 struct buffer b={0};VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
 VK(vkCreateBuffer(s_vk.device,&info,0,&b.handle));VkMemoryRequirements req;vkGetBufferMemoryRequirements(s_vk.device,b.handle,&req);
 VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
 VK(vkAllocateMemory(s_vk.device,&alloc,0,&b.memory));VK(vkBindBufferMemory(s_vk.device,b.handle,b.memory,0));VK(vkMapMemory(s_vk.device,b.memory,0,VK_WHOLE_SIZE,0,&b.data));return b;
}
static void drop_buffer(struct buffer*b){vkUnmapMemory(s_vk.device,b->memory);vkDestroyBuffer(s_vk.device,b->handle,0);vkFreeMemory(s_vk.device,b->memory,0);}
static void make_view(struct image*im){
 VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=im->handle,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
 VK(vkCreateImageView(s_vk.device,&view,0,&im->view));
}
static struct image make_image(VkImageUsageFlags usage){
 struct image im={0};VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
 VK(vkCreateImage(s_vk.device,&info,0,&im.handle));VkMemoryRequirements req;vkGetImageMemoryRequirements(s_vk.device,im.handle,&req);
 VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(req.memoryTypeBits,0)};
 VK(vkAllocateMemory(s_vk.device,&alloc,0,&im.memory));VK(vkBindImageMemory(s_vk.device,im.handle,im.memory,0));make_view(&im);return im;
}
static void begin(void){VkCommandBufferBeginInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};VK(vkBeginCommandBuffer(command,&info));}
static void submit(void){
 VK(vkEndCommandBuffer(command));VkSubmitInfo info={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
 VK(vkQueueSubmit(queue,1,&info,fence));VK(vkWaitForFences(s_vk.device,1,&fence,VK_TRUE,UINT64_MAX));VK(vkResetFences(s_vk.device,1,&fence));VK(vkResetCommandBuffer(command,0));
}
static void image_barrier(struct image*im,VkImageLayout old,VkAccessFlags from,VkAccessFlags to,VkPipelineStageFlags src,VkPipelineStageFlags dst){
 VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=from,.dstAccessMask=to,.oldLayout=old,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=im->handle,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
 vkCmdPipelineBarrier(command,src,dst,0,0,0,0,0,1,&b);
}
static void clear_texture(struct image*im,VkImageLayout old,float r,float g,float b){
 begin();image_barrier(im,old,old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkClearColorValue color={{r,g,b,1}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,im->handle,VK_IMAGE_LAYOUT_GENERAL,&color,1,&range);
 image_barrier(im,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);submit();
}
static VkPipeline make_pipeline(const char*path,VkPipelineLayout layout){
 FILE*f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);uint32_t*code=malloc(size);assert(fread(code,1,size,f)==(size_t)size);fclose(f);
 VkShaderModule module;VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=code};VK(vkCreateShaderModule(s_vk.device,&sm,0,&module));free(code);
 VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};
 VkPipeline result;VK(vkCreateComputePipelines(s_vk.device,0,1,&pi,0,&result));vkDestroyShaderModule(s_vk.device,module,0);return result;
}
int main(int argc,char**argv){
 assert(argc==4);int cached=atoi(argv[3]);VkInstance instance;
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="descriptor reuse regression",.apiVersion=VK_API_VERSION_1_1};VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VK(vkCreateInstance(&ici,0,&instance));
 uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,0));assert(count);VkPhysicalDevice*devices=malloc(count*sizeof(*devices));VK(vkEnumeratePhysicalDevices(instance,&count,devices));physical=devices[0];
 for(unsigned i=0;i<count;i++){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){physical=devices[i];break;}}free(devices);
 VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);printf("GPU: %s\n",props.deviceName);vkGetPhysicalDeviceMemoryProperties(physical,&memory);
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,0);VkQueueFamilyProperties*families=malloc(count*sizeof(*families));vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);unsigned family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;assert(family<count);free(families);
 float priority=1;VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};VkDeviceCreateInfo d={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&q};VK(vkCreateDevice(physical,&d,0,&s_vk.device));vkGetDeviceQueue(s_vk.device,family,0,&queue);
 VkCommandPool pool;VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VK(vkCreateCommandPool(s_vk.device,&cp,0,&pool));VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VK(vkAllocateCommandBuffers(s_vk.device,&ca,&command));VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(s_vk.device,&fi,0,&fence));
 struct buffer a=make_buffer(1024,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),b=make_buffer(1024,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),read=make_buffer(4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
 struct image textures[2]={make_image(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),make_image(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)},out=make_image(VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
 clear_texture(&textures[0],VK_IMAGE_LAYOUT_UNDEFINED,1,1,1);clear_texture(&textures[1],VK_IMAGE_LAYOUT_UNDEFINED,1,1,0);begin();image_barrier(&out,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);submit();
 VkSampler samplers[2];for(int i=0;i<2;i++){VkSamplerCreateInfo s={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=i?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.minFilter=i?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};VK(vkCreateSampler(s_vk.device,&s,0,&samplers[i]));}
 VkDescriptorSetLayout layouts[2];VkPipelineLayout pipeline_layouts[2];VkPipeline pipelines[2];VkDescriptorSet sets[2];
 VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,40},{VK_DESCRIPTOR_TYPE_SAMPLER,40},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};VkDescriptorPool descriptors;VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=4,.pPoolSizes=sizes};VK(vkCreateDescriptorPool(s_vk.device,&dp,0,&descriptors));
 for(int split=0;split<2;split++){
  VkDescriptorSetLayoutBinding bindings[32];unsigned n=0;bindings[n++]=(VkDescriptorSetLayoutBinding){0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};bindings[n++]=(VkDescriptorSetLayoutBinding){1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};
  if(split)for(unsigned i=0;i<16;i++)bindings[n++]=(VkDescriptorSetLayoutBinding){16+i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,0};else bindings[n++]=(VkDescriptorSetLayoutBinding){16,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16,VK_SHADER_STAGE_COMPUTE_BIT,0};
  bindings[n++]=(VkDescriptorSetLayoutBinding){48,VK_DESCRIPTOR_TYPE_SAMPLER,16,VK_SHADER_STAGE_COMPUTE_BIT,0};
  for(unsigned i=0;i<4;i++){bindings[n++]=(VkDescriptorSetLayoutBinding){32+i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,0};bindings[n++]=(VkDescriptorSetLayoutBinding){64+i,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};}
  bindings[n++]=(VkDescriptorSetLayoutBinding){80,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,0};
  VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=bindings};VK(vkCreateDescriptorSetLayout(s_vk.device,&li,0,&layouts[split]));
  VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layouts[split]};VK(vkCreatePipelineLayout(s_vk.device,&pl,0,&pipeline_layouts[split]));pipelines[split]=make_pipeline(argv[split+1],pipeline_layouts[split]);
  VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&layouts[split]};VK(vkAllocateDescriptorSets(s_vk.device,&ai,&sets[split]));VkDescriptorImageInfo target={0,out.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[split],.dstBinding=80,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&target};vkUpdateDescriptorSets(s_vk.device,1,&w,0,0);
 }
 VkDescriptorBufferInfo bi[2]={{a.handle,0,16},{b.handle,0,16}};VkDescriptorImageInfo ii[16],si[16],vi[4],vs[4];
 for(unsigned i=0;i<16;i++){ii[i]=(VkDescriptorImageInfo){0,textures[0].view,VK_IMAGE_LAYOUT_GENERAL};si[i]=(VkDescriptorImageInfo){samplers[0],0,0};}for(unsigned i=0;i<4;i++){vi[i]=ii[i];vs[i]=si[i];}
 struct vk_eng_desc_state cache={0};unsigned chosen=0;int green=0;
 for(unsigned phase=0;phase<12;phase++){
  unsigned split=phase>=5&&phase<=7;
  uint32_t*base=a.data;uint32_t color0[4]={64+phase*8,128,192,255},color1[4]={32,160,224,255};memcpy(base,color0,16);memcpy((char*)base+256,color1,16);((uint32_t*)b.data)[0]=phase==6?0:1;
  if(phase==2){chosen=1;ii[0].imageView=textures[1].view;si[0].sampler=samplers[1];}
  if(phase==3)bi[0].range=32;
  if(phase==4)bi[0].offset=256;
  if(phase==7){chosen=0;ii[0].imageView=textures[0].view;vi[3].imageView=textures[1].view;}
  if(phase==9){VkImageView old=textures[0].view;vkDestroyImageView(s_vk.device,old,0);++s_descriptor_epoch;make_view(&textures[0]);for(unsigned i=0;i<16;i++)if(ii[i].imageView==old)ii[i].imageView=textures[0].view;for(unsigned i=0;i<4;i++)if(vi[i].imageView==old)vi[i].imageView=textures[0].view;}
  if(phase==10){drop_buffer(&a);a=make_buffer(2048,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);memcpy(a.data,color0,16);memcpy((char*)a.data+256,color1,16);bi[0].buffer=a.handle;cache.valid=0;}
  if(phase==11){clear_texture(&textures[0],VK_IMAGE_LAYOUT_GENERAL,0,1,0);green=1;}
  for(unsigned repeat=0;repeat<30;repeat++){
   vk_eng_update_descriptor_state(sets[split],split,cached?&cache:0,bi,ii,si,vi,vs);
   begin();VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,0,0,0);
   image_barrier(&out,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[split]);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layouts[split],0,1,&sets[split],0,0);vkCmdDispatch(command,1,1,1);
   image_barrier(&out,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={1,1,1}};vkCmdCopyImageToBuffer(command,out.handle,VK_IMAGE_LAYOUT_GENERAL,read.handle,1,&copy);
   VkMemoryBarrier readback={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,0,0,0);submit();
   const uint32_t*color=bi[0].offset?color1:color0;unsigned char*pixel=read.data;
   for(unsigned k=0;k<4;k++){unsigned expected=(unsigned)floorf(color[k]*(phase==6?.5f:1.f)+.5f);if((chosen==1&&k==2)||(green&&chosen==0&&(k==0||k==2)))expected=0;assert(abs((int)pixel[k]-(int)expected)<=1);}
  }
  unsigned char*p=read.data;printf("PIXEL phase=%u %u %u %u %u\n",phase,p[0],p[1],p[2],p[3]);
 }
 printf("DESCRIPTORS calls=%llu skipped=%llu written=%llu reused=%llu\n",(unsigned long long)s_descriptor_calls,(unsigned long long)s_descriptor_skipped,(unsigned long long)s_descriptor_written,(unsigned long long)s_descriptor_reused);
 if(cached){assert(s_descriptor_skipped>300);assert(s_descriptor_written<500);}else{assert(s_descriptor_calls==360);assert(s_descriptor_written==360*42);}
 VK(vkDeviceWaitIdle(s_vk.device));vkDestroyDescriptorPool(s_vk.device,descriptors,0);for(int i=0;i<2;i++){vkDestroyPipeline(s_vk.device,pipelines[i],0);vkDestroyPipelineLayout(s_vk.device,pipeline_layouts[i],0);vkDestroyDescriptorSetLayout(s_vk.device,layouts[i],0);vkDestroySampler(s_vk.device,samplers[i],0);vkDestroyImageView(s_vk.device,textures[i].view,0);vkDestroyImage(s_vk.device,textures[i].handle,0);vkFreeMemory(s_vk.device,textures[i].memory,0);}vkDestroyImageView(s_vk.device,out.view,0);vkDestroyImage(s_vk.device,out.handle,0);vkFreeMemory(s_vk.device,out.memory,0);drop_buffer(&a);drop_buffer(&b);drop_buffer(&read);vkDestroyFence(s_vk.device,fence,0);vkDestroyCommandPool(s_vk.device,pool,0);vkDestroyDevice(s_vk.device,0);vkDestroyInstance(instance,0);puts("PASS: real GPU pixels preserve live constant changes, resource changes, normal/split layouts and slot reuse");
}
