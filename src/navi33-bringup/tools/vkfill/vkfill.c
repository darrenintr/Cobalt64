// SPDX-License-Identifier: MIT
// vkfill: the first Vulkan program on the RX 7600 under macOS. Loads RADV's ICD
// (libvulkan_radeon.dylib, Mesa + mesa-patches/) directly, without a Vulkan loader,
// creates a device on the one GPU the Darwin backend reports (the Navi33 kext's N48N
// client), runs a compute shader that writes d[i] = i * 3 + 7 into a host-visible
// buffer and checks every word on the CPU. Run as root (the N48N client is root-only).
//   vkfill [path/to/libvulkan_radeon.dylib] [words]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vkfill_spv.h"     // fill.comp as SPIR-V (glslc, see Makefile)

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "./libvulkan_radeon.dylib";
    const uint32_t words = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 65536u;
    if (!words || words % 64u) { fprintf(stderr, "words must be a non-zero multiple of 64\n"); return 2; }
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
    PFN_negotiate negotiate = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!negotiate || !gipa) { fprintf(stderr, "not a Vulkan ICD: %s\n", lib); return 1; }
    uint32_t ver = 5; negotiate(&ver);

#define GI(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(NULL, vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkfill", 1, "none", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    GI(inst, vkEnumeratePhysicalDevices); GI(inst, vkGetPhysicalDeviceProperties);
    GI(inst, vkGetPhysicalDeviceQueueFamilyProperties); GI(inst, vkGetPhysicalDeviceMemoryProperties);
    GI(inst, vkCreateDevice); GI(inst, vkGetDeviceProcAddr);
    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !n) { fprintf(stderr, "no Vulkan device (%d): see radv-darwin lines above\n", er); return 1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    printf("device: %s (vendor 0x%04x device 0x%04x), Vulkan %u.%u.%u\n", pp.deviceName, pp.vendorID, pp.deviceID,
           VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        printf("queue family %u: flags 0x%x count %u\n", i, qf[i].queueFlags, qf[i].queueCount);
        if (qfi == UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi = i;
    }
    for (uint32_t i = 0; i < qn && qfi == UINT32_MAX; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) qfi = i;
    if (qfi == UINT32_MAX) { fprintf(stderr, "no compute queue family\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfi, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define GD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    GD(vkGetDeviceQueue); GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory);
    GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateShaderModule); GD(vkCreateDescriptorSetLayout);
    GD(vkCreatePipelineLayout); GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool); GD(vkAllocateDescriptorSets);
    GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer);
    GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch); GD(vkEndCommandBuffer);
    GD(vkCreateFence); GD(vkQueueSubmit); GD(vkWaitForFences); GD(vkDestroyDevice); GD(vkCmdFillBuffer);
    GD(vkCmdPipelineBarrier);
    VkQueue q; vkGetDeviceQueue(dev, qfi, 0, &q);

    const VkDeviceSize bytes = (VkDeviceSize)words * 4u;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) mt = i;
    if (mt == UINT32_MAX) { fprintf(stderr, "no host-visible coherent memory type\n"); return 1; }
    printf("memory type %u flags 0x%x heap %u\n", mt, mp.memoryTypes[mt].propertyFlags, mp.memoryTypes[mt].heapIndex);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t *cpu; CHECK(vkMapMemory(dev, mem, 0, bytes, 0, (void **)&cpu));
    memset(cpu, 0xa5, bytes);

    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fill_spv), fill_spv};
    VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}, pl, VK_NULL_HANDLE, 0};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = {buf, 0, bytes};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfi};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, words / 64u, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    if (wr != VK_SUCCESS) { fprintf(stderr, "vkWaitForFences: %d (GPU did not finish in 5 s)\n", wr); return 1; }
    uint32_t bad = 0, first = UINT32_MAX;
    for (uint32_t i = 0; i < words; i++) if (cpu[i] != i * 3u + 7u) { if (first == UINT32_MAX) first = i; bad++; }
    printf("%u words, %u wrong", words, bad);
    if (bad) printf(" (first at %u: 0x%08x, want 0x%08x)", first, cpu[first], first * 3u + 7u);
    printf("\n");
    vkDestroyDevice(dev, NULL);
    printf(bad ? "VKFILL FAILED\n" : "VKFILL OK: a Vulkan compute shader ran on the RX 7600 through RADV and every word matched\n");
    return bad ? 1 : 0;
}
