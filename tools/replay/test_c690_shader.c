/*
 * Standalone headless Vulkan compute test for the c690 (SPU 0x6D68) position
 * unpack shader.
 *
 * It runs the already-validated native AVX2/FMA CPU kernel on the captured
 * real state (ls.bin / gpr.bin), then runs the GLSL compute shader on the same
 * captured local store and compares the 192 output slots (0xC00 bytes at
 * 0x1F7C0) byte-for-byte.
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/include \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/runtime/spu \
 *       -I/home/m/Documents/mlb16-online/mlb16-recomp/tools/replay \
 *       -o /tmp/opencode/fncap/test_c690_shader \
 *       /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/test_c690_shader.c \
 *       -l:libvulkan.so.1 -lm
 *
 * Run:
 *   /tmp/opencode/fncap/test_c690_shader /tmp/opencode/fncap
 *
 * No game launch and no window/surface: instance -> physical device -> queue ->
 * storage buffers -> compute pipeline -> dispatch -> fence -> read back.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/wait.h>
#include <vulkan/vulkan.h>

#include "spu_context.h"
#include "spu_helpers.h"

/* Minimal SPU runtime stubs (same as the native equivalence test). */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

#include "ref_spu_func_6d68.c"
#include "native_6d68.c"

#define GPR_BYTES (128u * 16u)
#define OUT_BASE  0x1F7C0u
#define OUT_BYTES 0xC00u
#define DISPATCHES_PER_TIMED_RECORD 1000u

#define CHECK(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", \
                (int)_r, __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint32_t in_base;
    uint32_t out_base;
    uint32_t vertex_count;
    uint32_t stride_bits;
    uint32_t bits_x;
    uint32_t bits_y;
    uint32_t bits_z;
    uint32_t pad0;
    uint32_t A[4];
    uint32_t B[4];
    uint32_t C[4];
} PushConstants;

static uint64_t fnv1a_update(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void load_exact(const char* path, void* dst, size_t want)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t got = fread(dst, 1, want, f);
    if (got != want) {
        fprintf(stderr, "%s: short read %zu/%zu\n", path, got, want);
        exit(1);
    }
    if (fgetc(f) != EOF) {
        fprintf(stderr, "%s: larger than %zu bytes\n", path, want);
        exit(1);
    }
    fclose(f);
}

static uint32_t* read_file_u32(const char* path, size_t* out_words)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) {
        fprintf(stderr, "%s: bad SPIR-V size %ld\n", path, sz);
        exit(1);
    }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (!p) { perror("malloc"); exit(1); }
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        exit(1);
    }
    fclose(f);
    if (p[0] != 0x07230203u) {
        fprintf(stderr, "%s: not a SPIR-V module (magic %08x)\n", path, p[0]);
        exit(1);
    }
    *out_words = (size_t)sz / 4;
    return p;
}

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits,
                                 VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    fprintf(stderr, "no memory type for requirements %08x\n", bits);
    exit(1);
}

static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap";
    const char* comp_path = argc > 2 ? argv[2]
        : "/home/m/Documents/mlb16-online/ps3recomp-probe/libs/video/vulkan/c690_vert.comp";
    const char* spv_path = argc > 3 ? argv[3] : "/tmp/opencode/fncap/c690_vert.spv";

    char path[1024];

    /* ---------------- CPU reference on the captured state ---------------- */
    static spu_context ctx_ref, ctx_nat;
    spu_context_init(&ctx_ref, 0);
    spu_context_init(&ctx_nat, 0);

    uint8_t* ls0 = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* gpr0 = (uint8_t*)malloc(GPR_BYTES);
    if (!ls0 || !gpr0) { perror("malloc"); return 1; }

    snprintf(path, sizeof path, "%s/ls.bin", dir);
    load_exact(path, ls0, SPU_LS_SIZE);
    snprintf(path, sizeof path, "%s/gpr.bin", dir);
    load_exact(path, gpr0, GPR_BYTES);

    ctx_ref.pc = ctx_nat.pc = 0x06D68;
    ctx_ref.image_id = ctx_nat.image_id = 6;
    memcpy(ctx_ref.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_nat.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_ref.gpr, gpr0, GPR_BYTES);
    memcpy(ctx_nat.gpr, gpr0, GPR_BYTES);

    jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx_ref);
    native_spu_func_00006D68(&ctx_nat);

    uint64_t ref_all = fnv1a_update(0xcbf29ce484222325ull, ctx_ref.ls, SPU_LS_SIZE);
    ref_all = fnv1a_update(ref_all, ctx_ref.gpr, GPR_BYTES);
    uint64_t nat_all = fnv1a_update(0xcbf29ce484222325ull, ctx_nat.ls, SPU_LS_SIZE);
    nat_all = fnv1a_update(nat_all, ctx_nat.gpr, GPR_BYTES);

    const uint8_t* cpu_out = ctx_nat.ls + OUT_BASE;

    printf("=================================================================\n");
    printf("c690 / SPU 0x6D68 position unpack: Vulkan compute vs CPU kernel\n");
    printf("Capture: %s\n", dir);
    printf("FNV-1a64 LS+GPR lifted 0x%016llx  native 0x%016llx\n",
           (unsigned long long)ref_all, (unsigned long long)nat_all);
    printf("Reference checksum: %s (expected 0xfc2e9569dc2feb11)\n",
           (nat_all == 0xfc2e9569dc2feb11ull && ref_all == 0xfc2e9569dc2feb11ull)
               ? "OK" : "MISMATCH");
    printf("=================================================================\n");

    /* ---------------- Compile GLSL -> SPIR-V ---------------- */
    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"",
             spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        fprintf(stderr, "shader compilation failed: %s\n", compile_cmd);
        return 1;
    }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    /* ---------------- Vulkan setup ---------------- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-vert-compute-test",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t phys_count = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, NULL));
    if (phys_count == 0) { fprintf(stderr, "no Vulkan physical devices\n"); return 1; }
    VkPhysicalDevice* phys_list = (VkPhysicalDevice*)malloc(phys_count * sizeof(*phys_list));
    CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, phys_list));

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t qfamily = UINT32_MAX;
    for (uint32_t d = 0; d < phys_count && phys == VK_NULL_HANDLE; d++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, NULL);
        VkQueueFamilyProperties* qp = (VkQueueFamilyProperties*)malloc(qn * sizeof(*qp));
        vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, qp);
        for (uint32_t q = 0; q < qn; q++) {
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                phys = phys_list[d];
                qfamily = q;
                break;
            }
        }
        free(qp);
    }
    if (phys == VK_NULL_HANDLE) { fprintf(stderr, "no compute queue\n"); return 1; }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("Device: %s (API %u.%u.%u), compute queue family %u\n",
           props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), qfamily);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfamily, 0, &queue);

    /* ---------------- Buffers ---------------- */
    VkBuffer in_buf, out_buf;
    VkDeviceMemory in_mem, out_mem;
    void *in_map, *out_map;
    make_buffer(dev, phys, SPU_LS_SIZE, &in_buf, &in_mem, &in_map);
    make_buffer(dev, phys, SPU_LS_SIZE, &out_buf, &out_mem, &out_map);

    memcpy(in_map, ls0, SPU_LS_SIZE);
    memcpy(out_map, ls0, SPU_LS_SIZE);   /* upload output slots as-is */

    /* ---------------- Descriptors / pipeline ---------------- */
    VkDescriptorSetLayoutBinding bind[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bind,
    };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    PushConstants pc = {
        .in_base = 0x17350u,
        .out_base = OUT_BASE,
        .vertex_count = 192u,
        .stride_bits = 35u,
        .bits_x = 12u,
        .bits_y = 12u,
        .bits_z = 11u,
        .pad0 = 0u,
    };
    uint32_t* g10 = (uint32_t*)&gpr0[10 * 16];
    uint32_t* g11 = (uint32_t*)&gpr0[11 * 16];
    uint32_t* g12 = (uint32_t*)&gpr0[12 * 16];
    for (int i = 0; i < 4; i++) {
        pc.B[i] = g10[i];
        pc.A[i] = g11[i];
        pc.C[i] = g12[i];
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_words * 4,
        .pCode = spv,
    };
    VkShaderModule module;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &module));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = layout,
    };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &dps,
    };
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet dset;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkDescriptorBufferInfo dbi[2] = {
        { .buffer = in_buf,  .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE },
    };
    VkWriteDescriptorSet wds[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = dset, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &dbi[1] },
    };
    vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

    /* ---------------- Command buffer ---------------- */
    VkCommandPoolCreateInfo cpci2 = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily,
    };
    VkCommandPool cmd_pool;
    CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    uint32_t groups = (pc.vertex_count + 63u) / 64u;

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                            0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    double t0 = now_sec();
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    double t1 = now_sec();
    CHECK(vkResetFences(dev, 1, &fence));

    /* ---------------- Compare ---------------- */
    const uint8_t* gpu_out = (const uint8_t*)out_map + OUT_BASE;
    const uint8_t* gpu_all = (const uint8_t*)out_map;

    size_t diff = 0;
    int first = -1;
    for (size_t i = 0; i < OUT_BYTES; i++) {
        if (gpu_out[i] != cpu_out[i]) {
            if (first < 0) first = (int)i;
            diff++;
        }
    }

    size_t outside = 0;
    for (size_t i = 0; i < SPU_LS_SIZE; i++) {
        if (i >= OUT_BASE && i < OUT_BASE + OUT_BYTES) continue;
        if (gpu_all[i] != ls0[i]) outside++;
    }

    uint64_t gpu_cksum = fnv1a_update(0xcbf29ce484222325ull, gpu_out, OUT_BYTES);
    uint64_t cpu_cksum = fnv1a_update(0xcbf29ce484222325ull, cpu_out, OUT_BYTES);

    printf("Output region 0x%05X..0x%05X (%u slots, %u bytes)\n",
           OUT_BASE, OUT_BASE + OUT_BYTES - 1, OUT_BYTES / 16, OUT_BYTES);
    printf("CPU region FNV-1a64: 0x%016llx\n", (unsigned long long)cpu_cksum);
    printf("GPU region FNV-1a64: 0x%016llx\n", (unsigned long long)gpu_cksum);
    printf("Differing bytes: %zu", diff);
    if (first >= 0) {
        size_t n = (size_t)first;
        printf("  first at region+0x%zX (LS 0x%zX): cpu=%02x gpu=%02x",
               n, (size_t)OUT_BASE + n, cpu_out[n], gpu_out[n]);
    }
    printf("\nWrites outside output region: %zu byte(s)\n", outside);
    printf("Dispatch: %u workgroup(s) of 64, 1 submission + fence: %.3f ms\n",
           groups, (t1 - t0) * 1e3);

    /* ---------------- Timing: many dispatches in one submission ------------- */
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                            0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    for (uint32_t i = 0; i < DISPATCHES_PER_TIMED_RECORD; i++)
        vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));

    t0 = now_sec();
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    t1 = now_sec();
    double per = (t1 - t0) * 1e3 / DISPATCHES_PER_TIMED_RECORD;
    printf("Timing: %u dispatches in one submission: %.3f ms total, "
           "%.4f ms/dispatch\n", DISPATCHES_PER_TIMED_RECORD,
           (t1 - t0) * 1e3, per);

    int pass = (diff == 0 && outside == 0 &&
                nat_all == 0xfc2e9569dc2feb11ull &&
                ref_all == 0xfc2e9569dc2feb11ull);
    printf("RESULT: %s\n", pass ? "PASS (bit-exact)" : "FAIL");
    printf("=================================================================\n");

    /* ---------------- Teardown ---------------- */
    vkUnmapMemory(dev, in_mem);
    vkUnmapMemory(dev, out_mem);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyBuffer(dev, in_buf, NULL);
    vkDestroyBuffer(dev, out_buf, NULL);
    vkFreeMemory(dev, in_mem, NULL);
    vkFreeMemory(dev, out_mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    free(spv);
    free(phys_list);
    free(ls0);
    free(gpr0);

    return pass ? 0 : 1;
}
