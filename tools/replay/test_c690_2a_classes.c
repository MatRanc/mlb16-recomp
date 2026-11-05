/*
 * Offline runner for the c690 0x2A per-class record kernels.
 *
 * The job file carries one job: the class field bases (stream LS addresses),
 * the SPU-exported per-record source entry index, the captured IN Local Store
 * and the seed image (IN plus every byte the SPU writes itself).  The runner
 * uploads them, dispatches the class shader once, and writes the resulting
 * Local Store image; the Python gate does the per-field diff.
 *
 * Build:
 *   clang -O2 -o /tmp/opencode/test_c690_2a_classes \
 *     mlb16-recomp/tools/replay/test_c690_2a_classes.c -l:libvulkan.so.1 -lm
 *
 * Run:
 *   test_c690_2a_classes job.bin c690_2a_<class>.comp out.spv --out gpu.bin
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: Vulkan error %d\n", __FILE__, __LINE__, (int)_r); exit(1); } } while (0)

#define LS_BYTES 262144u

typedef struct {
    char magic[8];              /* "C69CLS01" */
    uint32_t count;
    uint32_t out_base;
    uint32_t rec_size;
    uint32_t base[7];
} JobHdr;

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
}

static uint32_t* read_file_u32(const char* path, size_t* out_words)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: bad SPIR-V size %ld\n", path, sz); exit(1); }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (!p || fread(p, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
    if (p[0] != 0x07230203u) { fprintf(stderr, "%s: not SPIR-V\n", path); exit(1); }
    *out_words = (size_t)sz / 4;
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s job.bin shader.comp out.spv [--out gpu.bin]\n", argv[0]);
        return 2;
    }
    const char* job_path = argv[1];
    const char* comp_path = argv[2];
    const char* spv_path = argv[3];
    const char* out_path = "/tmp/opencode/c690_2a_classes_out.bin";
    for (int i = 4; i < argc; i++)
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];

    FILE* jf = fopen(job_path, "rb");
    if (!jf) { perror(job_path); return 1; }
    JobHdr hdr;
    if (fread(&hdr, sizeof hdr, 1, jf) != 1 || memcmp(hdr.magic, "C69CLS01", 7)) {
        fprintf(stderr, "%s: bad job header\n", job_path); return 1;
    }
    if (hdr.count == 0 || hdr.count > 8192 || hdr.rec_size == 0 || hdr.rec_size > 128) {
        fprintf(stderr, "bad count/rec_size %u/%u\n", hdr.count, hdr.rec_size); return 1;
    }
    uint32_t* idx = (uint32_t*)calloc(hdr.count, sizeof(uint32_t));
    static uint8_t in_ls[LS_BYTES], seed_ls[LS_BYTES];
    if (!idx) { fprintf(stderr, "oom\n"); return 1; }
    if (fread(idx, 4, hdr.count, jf) != hdr.count ||
        fread(in_ls, 1, LS_BYTES, jf) != LS_BYTES ||
        fread(seed_ls, 1, LS_BYTES, jf) != LS_BYTES) {
        fprintf(stderr, "%s: short job\n", job_path); return 1;
    }
    fclose(jf);

    char compile_cmd[8192];
    /* include path = directory of the shader (it #includes the shared .inc) */
    char incdir[4096];
    snprintf(incdir, sizeof incdir, "%s", comp_path);
    char* slash = strrchr(incdir, '/');
    if (slash) *slash = 0; else snprintf(incdir, sizeof incdir, ".");
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -I\"%s\" -o \"%s\" \"%s\"",
             incdir, spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-2a-classes", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t phys_count = 0; CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, NULL));
    if (!phys_count) { fprintf(stderr, "no Vulkan devices\n"); return 1; }
    VkPhysicalDevice* phys_list = (VkPhysicalDevice*)malloc(phys_count * sizeof(*phys_list));
    CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, phys_list));
    VkPhysicalDevice phys = VK_NULL_HANDLE; uint32_t qfamily = UINT32_MAX;
    for (uint32_t d = 0; d < phys_count && phys == VK_NULL_HANDLE; d++) {
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, NULL);
        VkQueueFamilyProperties* qp = (VkQueueFamilyProperties*)malloc(qn * sizeof(*qp));
        vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, qp);
        for (uint32_t q = 0; q < qn; q++)
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { phys = phys_list[d]; qfamily = q; break; }
        free(qp);
    }
    if (phys == VK_NULL_HANDLE) { fprintf(stderr, "no compute queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    uint32_t idx_bytes = hdr.count * 4u;
    VkBuffer in_buf, idx_buf, out_buf; VkDeviceMemory in_mem, idx_mem, out_mem;
    void *in_map, *idx_map, *out_map;
    make_buffer(dev, phys, LS_BYTES, &in_buf, &in_mem, &in_map);
    make_buffer(dev, phys, LS_BYTES, &out_buf, &out_mem, &out_map);
    make_buffer(dev, phys, idx_bytes ? idx_bytes : 4u, &idx_buf, &idx_mem, &idx_map);
    memcpy(in_map, in_ls, LS_BYTES);
    memcpy(out_map, seed_ls, LS_BYTES);
    memcpy(idx_map, idx, idx_bytes);

    VkDescriptorSetLayoutBinding bind[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bind };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    struct { uint32_t out_base, count, rec_size, b[7]; } pc;
    memset(&pc, 0, sizeof pc);
    pc.out_base = hdr.out_base; pc.count = hdr.count; pc.rec_size = hdr.rec_size;
    for (int i = 0; i < 7; i++) pc.b[i] = hdr.base[i];
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(pc) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_words * 4, .pCode = spv };
    VkShaderModule module; CHECK(vkCreateShaderModule(dev, &smci, NULL, &module));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
        .layout = layout };
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[3] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = idx_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wds[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[2] } };
    vkUpdateDescriptorSets(dev, 3, wds, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily };
    VkCommandPool cmd_pool; CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    uint32_t groups = (hdr.count + 63u) / 64u;

    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    FILE* of = fopen(out_path, "wb");
    if (!of) { perror(out_path); return 1; }
    if (fwrite(out_map, 1, LS_BYTES, of) != LS_BYTES) { perror(out_path); return 1; }
    fclose(of);
    return 0;
}
