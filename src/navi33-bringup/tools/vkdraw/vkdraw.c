// SPDX-License-Identifier: MIT
// A real gfx-queue draw/readback through the explicitly supplied RADV ICD.
// No loader, device-name heuristic, compute substitute or software fallback.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "draw_spv.h"

#define CHECK(call) do { VkResult r_=(call); if(r_!=VK_SUCCESS) { \
    fprintf(stderr,"%s failed: %d\n",#call,r_); return 1; } } while(0)
#define REQUIRE(expr,why) do { if(!(expr)) { fprintf(stderr,"%s\n",why); return 1; } } while(0)
#define GI(instance,name) PFN_##name name=(PFN_##name)gipa(instance,#name); REQUIRE(name,"missing " #name)
#define GD(name) PFN_##name name=(PFN_##name)vkGetDeviceProcAddr(dev,#name); REQUIRE(name,"missing " #name)

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *mp,
                            uint32_t bits,VkMemoryPropertyFlags required) {
    for(uint32_t i=0;i<mp->memoryTypeCount;i++)
        if((bits & (1u<<i)) && (mp->memoryTypes[i].propertyFlags & required)==required) return i;
    return UINT32_MAX;
}

int main(int argc,char **argv) {
    if(argc!=2) { fprintf(stderr,"usage: vkdraw /path/to/libvulkan_radeon.dylib\n"); return 64; }
    setbuf(stdout,NULL);
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library) { fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
    typedef VkResult (VKAPI_PTR *Negotiate)(uint32_t *);
    Negotiate negotiate=(Negotiate)dlsym(library,"vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");
    REQUIRE(negotiate && gipa,"not a Vulkan ICD");
    uint32_t version=5; CHECK(negotiate(&version));
    GI(NULL,vkCreateInstance);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="n33-vkdraw",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(vkCreateInstance(&ici,NULL,&instance));
    GI(instance,vkEnumeratePhysicalDevices); GI(instance,vkGetPhysicalDeviceProperties);
    GI(instance,vkGetPhysicalDeviceQueueFamilyProperties); GI(instance,vkGetPhysicalDeviceMemoryProperties);
    GI(instance,vkGetPhysicalDeviceFormatProperties); GI(instance,vkCreateDevice);
    GI(instance,vkGetDeviceProcAddr); GI(instance,vkDestroyInstance);
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    REQUIRE(count==1,"expected exactly one RX 7600 from the explicit ICD");
    VkPhysicalDevice physical; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical,&props);
    REQUIRE(props.vendorID==0x1002u && props.deviceID==0x7480u &&
            props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,"ICD does not expose a discrete RX 7600");
    printf("device: %s vendor=%04x device=%04x driver=%08x\n",props.deviceName,
           props.vendorID,props.deviceID,props.driverVersion);
    uint32_t queue_count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,NULL);
    REQUIRE(queue_count && queue_count<=32,"unexpected queue family count");
    VkQueueFamilyProperties families[32]; vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,families);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<queue_count;i++)
        if(families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family=i; break; }
    REQUIRE(family!=UINT32_MAX,"no graphics queue");
    printf("graphics queue family: %u\n",family);
    const VkFormat format=VK_FORMAT_B8G8R8A8_UNORM;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical,format,&fp);
    REQUIRE((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))==
            (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),"BGRA8 render/copy unsupported");
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CHECK(vkCreateDevice(physical,&dci,NULL,&dev));
    GD(vkGetDeviceQueue); GD(vkCreateImage); GD(vkGetImageMemoryRequirements);
    GD(vkAllocateMemory); GD(vkBindImageMemory); GD(vkCreateImageView); GD(vkCreateBuffer);
    GD(vkGetBufferMemoryRequirements); GD(vkBindBufferMemory); GD(vkMapMemory);
    GD(vkCreateRenderPass); GD(vkCreateFramebuffer); GD(vkCreateShaderModule);
    GD(vkCreatePipelineLayout); GD(vkCreateGraphicsPipelines); GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkCmdBeginRenderPass);
    GD(vkCmdBindPipeline); GD(vkCmdDraw); GD(vkCmdEndRenderPass); GD(vkCmdPipelineBarrier);
    GD(vkCmdCopyImageToBuffer); GD(vkEndCommandBuffer); GD(vkCreateFence);
    GD(vkQueueSubmit); GD(vkWaitForFences); GD(vkDestroyDevice); GD(vkResetFences);
    GD(vkDestroyFence); GD(vkDestroyCommandPool); GD(vkDestroyPipeline);
    GD(vkDestroyPipelineLayout); GD(vkDestroyShaderModule); GD(vkDestroyFramebuffer);
    GD(vkDestroyRenderPass); GD(vkDestroyImageView); GD(vkDestroyImage);
    GD(vkDestroyBuffer); GD(vkFreeMemory); GD(vkUnmapMemory);
    VkQueue queue; vkGetDeviceQueue(dev,family,0,&queue);
    const uint32_t width=64,height=64;
    const VkDeviceSize bytes=width*height*4u;
    VkImageCreateInfo imci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=format,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; CHECK(vkCreateImage(dev,&imci,NULL,&image));
    VkMemoryRequirements imreq; vkGetImageMemoryRequirements(dev,image,&imreq);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    uint32_t imtype=memory_type(&memory,imreq.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    REQUIRE(imtype!=UINT32_MAX,"no device-local image memory");
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=imreq.size,.memoryTypeIndex=imtype};
    VkDeviceMemory immem; CHECK(vkAllocateMemory(dev,&alloc,NULL,&immem)); CHECK(vkBindImageMemory(dev,image,immem,0));
    VkImageViewCreateInfo ivci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(vkCreateImageView(dev,&ivci,NULL,&view));
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer; CHECK(vkCreateBuffer(dev,&bci,NULL,&buffer));
    VkMemoryRequirements breq; vkGetBufferMemoryRequirements(dev,buffer,&breq);
    uint32_t btype=memory_type(&memory,breq.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(btype!=UINT32_MAX,"no coherent readback memory");
    alloc.allocationSize=breq.size; alloc.memoryTypeIndex=btype;
    VkDeviceMemory bmem; CHECK(vkAllocateMemory(dev,&alloc,NULL,&bmem)); CHECK(vkBindBufferMemory(dev,buffer,bmem,0));
    uint8_t *pixels; CHECK(vkMapMemory(dev,bmem,0,bytes,0,(void **)&pixels));
    VkAttachmentDescription attachment={.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&reference};
    VkSubpassDependency dependencies[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=dependencies};
    VkRenderPass pass; CHECK(vkCreateRenderPass(dev,&rpci,NULL,&pass));
    VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=1,.pAttachments=&view,.width=width,.height=height,.layers=1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(dev,&fbci,NULL,&framebuffer));
    VkShaderModuleCreateInfo smci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(vert_spv),.pCode=vert_spv};
    VkShaderModule vertex,fragment; CHECK(vkCreateShaderModule(dev,&smci,NULL,&vertex));
    smci.codeSize=sizeof(frag_spv); smci.pCode=frag_spv; CHECK(vkCreateShaderModule(dev,&smci,NULL,&fragment));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,(float)width,(float)height,0,1}; VkRect2D scissor={{16,16},{32,32}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend};
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev,&plci,NULL,&layout));
    VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=layout,.renderPass=pass,.basePipelineIndex=-1};
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev,&cpci,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(dev,&cai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command,&begin));
    VkClearValue clear={.color={{0,0,0,1}}};
    VkRenderPassBeginInfo rpbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,
        .renderArea={{0,0},{width,height}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); vkCmdDraw(command,3,1,0,0);
    vkCmdEndRenderPass(command);
    VkImageMemoryBarrier imb={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&imb);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    CHECK(vkCreateFence(dev,&fci,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    // Repeat after CPU poison to establish resource reuse, not a stale first image.
    for(uint32_t frame=0;frame<3;frame++) {
        memset(pixels,0xa5,(size_t)bytes);
        CHECK(vkQueueSubmit(queue,1,&submit,fence));
        VkResult waited=vkWaitForFences(dev,1,&fence,VK_TRUE,2000000000ull);
        if(waited!=VK_SUCCESS) {
            fprintf(stderr,"graphics fence failed: %d; no replay or resource destruction after uncertain completion\n",waited);
            return 1;
        }
        uint32_t bad=0;
        for(uint32_t y=0;y<height;y++) for(uint32_t x=0;x<width;x++) {
            const uint8_t *p=pixels+4u*(y*width+x);
            const uint8_t red=(x>=16 && x<48 && y>=16 && y<48)?255:0;
            if(p[0] || p[1] || p[2]!=red || p[3]!=255) {
                if(bad<4) fprintf(stderr,"frame %u (%u,%u): BGRA %02x%02x%02x%02x expected 0000%02xff\n",
                                  frame,x,y,p[0],p[1],p[2],p[3],red);
                bad++;
            }
        }
        printf("frame %u: checked %u pixels, wrong %u\n",frame,width*height,bad);
        if(bad) return 1;
        CHECK(vkResetFences(dev,1,&fence));
    }
    vkDestroyFence(dev,fence,NULL); vkDestroyCommandPool(dev,pool,NULL);
    vkDestroyPipeline(dev,pipeline,NULL); vkDestroyPipelineLayout(dev,layout,NULL);
    vkDestroyShaderModule(dev,vertex,NULL); vkDestroyShaderModule(dev,fragment,NULL);
    vkDestroyFramebuffer(dev,framebuffer,NULL); vkDestroyRenderPass(dev,pass,NULL);
    vkDestroyImageView(dev,view,NULL); vkDestroyImage(dev,image,NULL); vkFreeMemory(dev,immem,NULL);
    vkUnmapMemory(dev,bmem); vkDestroyBuffer(dev,buffer,NULL); vkFreeMemory(dev,bmem,NULL);
    vkDestroyDevice(dev,NULL); vkDestroyInstance(instance,NULL); dlclose(library);
    puts("VKDRAW OK: RX 7600 graphics draw, 3 frames, 12288 pixels exact (not desktop proof)");
    return 0;
}
