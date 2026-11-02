/*
 * test_vk_sync.c - standalone headless Vulkan submit/fence microbenchmark.
 *
 * Measures, from a worker pthread (never the main thread):
 *   1. wait path   : record tiny compute dispatch + submit + vkWaitForFences
 *   2. submit-only : pre-recorded dispatches, vkQueueSubmit with no CPU wait,
 *                    single vkQueueWaitIdle at the very end
 *   3. batched     : N dispatches in one command buffer, one submit + one wait
 *                    per batch, N = 1, 8, 64
 *
 * Shader is embedded as SPIR-V (out[gid] = in[gid] + 1u, local_size_x = 64,
 * 16 KB in + 16 KB out storage buffers).  GPU work is negligible on purpose;
 * this is a CPU submit/fence latency benchmark.
 *
 * Build (Fedora has libvulkan.so.1 but no -dev symlink):
 *   mkdir -p /tmp/opencode/vklib
 *   ln -sf /lib64/libvulkan.so.1 /tmp/opencode/vklib/libvulkan.so
 *   cc -O2 -std=c11 -o test_vk_sync test_vk_sync.c \
 *      -L/tmp/opencode/vklib -lvulkan -lpthread -lm
 *   (or link the loader directly: cc ... test_vk_sync.c /lib64/libvulkan.so.1 -lpthread -lm)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <vulkan/vulkan.h>

#define LOCAL_SIZE      64u
#define ELEMS           4096u                      /* 16 KB of uint32 */
#define BUF_BYTES       (ELEMS * 4u)
#define WG_COUNT        (ELEMS / LOCAL_SIZE)

#define WAIT_ITERS      10000
#define WARMUP_ITERS    1000
#define SUBMIT_ITERS    10000
#define BATCH_WARM      1024
#define BATCH_MEASURED  16384                      /* divisible by 64 and 8 */

static const unsigned char add_spv[] __attribute__((aligned(4))) = {
    0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x08, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x47, 0x4c, 0x53, 0x4c, 0x2e, 0x73, 0x74, 0x64, 0x2e, 0x34, 0x35, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x06, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00,
    0xc2, 0x01, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x67, 0x69, 0x64, 0x00, 0x05, 0x00, 0x08, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x67, 0x6c, 0x5f, 0x47, 0x6c, 0x6f, 0x62, 0x61,
    0x6c, 0x49, 0x6e, 0x76, 0x6f, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x49,
    0x44, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x11, 0x00, 0x00, 0x00,
    0x4f, 0x75, 0x74, 0x42, 0x75, 0x66, 0x00, 0x00, 0x06, 0x00, 0x06, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0x75, 0x74, 0x5f,
    0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00,
    0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x49, 0x6e, 0x42, 0x75, 0x66, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x05, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x69, 0x6e, 0x5f, 0x64, 0x61, 0x74, 0x61, 0x00, 0x05, 0x00, 0x03, 0x00,
    0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
    0x47, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00, 0x11, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00, 0x11, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x00, 0x04, 0x00, 0x13, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x13, 0x00, 0x00, 0x00,
    0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
    0x17, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x47, 0x00, 0x03, 0x00, 0x18, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x48, 0x00, 0x05, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
    0x1a, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x23, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x13, 0x00, 0x02, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x21, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x17, 0x00, 0x04, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x0d, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x03, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x03, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00,
    0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
    0x3b, 0x00, 0x04, 0x00, 0x12, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00,
    0x14, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1d, 0x00, 0x03, 0x00, 0x17, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x1e, 0x00, 0x03, 0x00, 0x18, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x04, 0x00, 0x19, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x19, 0x00, 0x00, 0x00,
    0x1a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00,
    0x1c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x22, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x06, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00,
    0x1f, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x36, 0x00, 0x05, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x3b, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x41, 0x00, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x3d, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x0f, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x41, 0x00, 0x06, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
    0x1a, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
    0x3d, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
    0x1d, 0x00, 0x00, 0x00, 0x80, 0x00, 0x05, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00,
    0x41, 0x00, 0x06, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
    0x13, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00,
    0x3e, 0x00, 0x03, 0x00, 0x21, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0xfd, 0x00, 0x01, 0x00, 0x38, 0x00, 0x01, 0x00,
};
static const unsigned int add_spv_words = 263;

#define CK(x) do {                                                        \
    VkResult _r = (x);                                                    \
    if (_r != VK_SUCCESS) {                                               \
        fprintf(stderr, "Vulkan error %d at %s:%d\n", (int)_r,            \
                __FILE__, __LINE__);                                      \
        exit(1);                                                          \
    }                                                                     \
} while (0)

typedef struct {
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         qfamily;
    VkPhysicalDeviceProperties props;

    VkBuffer         in_buf, out_buf;
    VkDeviceMemory   in_mem, out_mem;

    VkDescriptorSetLayout dsl;
    VkDescriptorPool      dpool;
    VkDescriptorSet       dset;
    VkPipelineLayout      playout;
    VkPipeline            pipeline;
} Ctx;

static pthread_t g_main_tid;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* nearest-rank percentile, p in [0,100], s sorted ascending */
static double pct(const double *s, size_t n, double p)
{
    size_t idx = (size_t)ceil(p / 100.0 * (double)n);
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    return s[idx - 1];
}

static void stats(const char *mode, int n, const char *unit,
                  double *s, size_t cnt, size_t iters)
{
    double mean = 0.0;
    size_t i;
    qsort(s, cnt, sizeof(double), cmp_double);
    for (i = 0; i < cnt; i++) mean += s[i];
    mean /= (double)cnt;
    printf("%-26s %4d %8.2f %8.2f %8.2f %8.2f %8zu  %s\n",
           mode, n, pct(s, cnt, 50.0), pct(s, cnt, 90.0),
           pct(s, cnt, 99.0), mean, iters, unit);
}

static uint32_t find_mem_type(Ctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(c->phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    fprintf(stderr, "no suitable memory type\n");
    exit(1);
}

static void make_buffer(Ctx *c, VkDeviceSize size, VkBuffer *buf,
                        VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    };
    CK(vkCreateBuffer(c->device, &bci, NULL, buf));
    vkGetBufferMemoryRequirements(c->device, *buf, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem_type(c, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(c->device, &mai, NULL, mem));
    CK(vkBindBufferMemory(c->device, *buf, *mem, 0));
    if (map) CK(vkMapMemory(c->device, *mem, 0, size, 0, map));
}

static void record_dispatch(Ctx *c, VkCommandBuffer cb, uint32_t n)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    uint32_t i;
    CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            c->playout, 0, 1, &c->dset, 0, NULL);
    for (i = 0; i < n; i++)
        vkCmdDispatch(cb, WG_COUNT, 1, 1);
    CK(vkEndCommandBuffer(cb));
}

static void init_vulkan(Ctx *c)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vk-sync-bench",
        .applicationVersion = 1,
        .pEngineName = "none",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    uint32_t ndev = 0, i, chosen = UINT32_MAX;
    VkPhysicalDevice *devs;
    VkQueueFamilyProperties *qfp;
    uint32_t nq = 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
    };
    float prio = 1.0f;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)add_spv_words * 4u,
        .pCode = (const uint32_t *)add_spv,
    };
    VkShaderModule module;
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .pName = "main",
        },
    };
    VkDescriptorSetLayoutBinding binds[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds,
    };
    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps,
    };
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
    };
    VkDescriptorBufferInfo dbi[2];
    VkWriteDescriptorSet wr[2];
    void *map = NULL;
    uint32_t *host;
    int d;

    CK(vkCreateInstance(&ici, NULL, &c->instance));

    CK(vkEnumeratePhysicalDevices(c->instance, &ndev, NULL));
    if (ndev == 0) { fprintf(stderr, "no Vulkan devices\n"); exit(1); }
    devs = calloc(ndev, sizeof(*devs));
    CK(vkEnumeratePhysicalDevices(c->instance, &ndev, devs));

    printf("Vulkan physical devices:\n");
    for (i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("  [%u] %s  vendor=0x%04x type=%d api=%u.%u.%u\n",
               i, p.deviceName, p.vendorID, (int)p.deviceType,
               VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion));
        if (chosen == UINT32_MAX && p.vendorID == 0x10DE)
            chosen = i;
    }
    if (chosen == UINT32_MAX) {
        fprintf(stderr, "no NVIDIA (0x10DE) device found\n");
        exit(1);
    }
    c->phys = devs[chosen];
    vkGetPhysicalDeviceProperties(c->phys, &c->props);

    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    qfp = calloc(nq, sizeof(*qfp));
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qfp);
    for (i = 0; i < nq; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            c->qfamily = i;
            break;
        }
    }
    if (i == nq) { fprintf(stderr, "no compute queue family\n"); exit(1); }

    qci.queueFamilyIndex = c->qfamily;
    qci.pQueuePriorities = &prio;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    CK(vkCreateDevice(c->phys, &dci, NULL, &c->device));
    vkGetDeviceQueue(c->device, c->qfamily, 0, &c->queue);

    printf("using: %s (vendor 0x%04x, type %d)\n",
           c->props.deviceName, c->props.vendorID, (int)c->props.deviceType);
    printf("queue family %u (compute), driver 0x%08x, api %u.%u.%u\n",
           c->qfamily, c->props.driverVersion,
           VK_VERSION_MAJOR(c->props.apiVersion),
           VK_VERSION_MINOR(c->props.apiVersion),
           VK_VERSION_PATCH(c->props.apiVersion));

    make_buffer(c, BUF_BYTES, &c->in_buf, &c->in_mem, &map);
    host = (uint32_t *)map;
    for (i = 0; i < ELEMS; i++) host[i] = i;
    vkUnmapMemory(c->device, c->in_mem);
    make_buffer(c, BUF_BYTES, &c->out_buf, &c->out_mem, NULL);

    CK(vkCreateDescriptorSetLayout(c->device, &dlci, NULL, &c->dsl));
    CK(vkCreateDescriptorPool(c->device, &dpci, NULL, &c->dpool));
    dsai.descriptorPool = c->dpool;
    dsai.pSetLayouts = &c->dsl;
    CK(vkAllocateDescriptorSets(c->device, &dsai, &c->dset));

    dbi[0] = (VkDescriptorBufferInfo){ c->in_buf, 0, VK_WHOLE_SIZE };
    dbi[1] = (VkDescriptorBufferInfo){ c->out_buf, 0, VK_WHOLE_SIZE };
    memset(wr, 0, sizeof(wr));
    for (d = 0; d < 2; d++) {
        wr[d].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[d].dstSet = c->dset;
        wr[d].dstBinding = (uint32_t)d;
        wr[d].descriptorCount = 1;
        wr[d].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[d].pBufferInfo = &dbi[d];
    }
    vkUpdateDescriptorSets(c->device, 2, wr, 0, NULL);

    plci.pSetLayouts = &c->dsl;
    CK(vkCreatePipelineLayout(c->device, &plci, NULL, &c->playout));
    CK(vkCreateShaderModule(c->device, &smci, NULL, &module));
    cpci.stage.module = module;
    cpci.layout = c->playout;
    CK(vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &cpci,
                                NULL, &c->pipeline));
    vkDestroyShaderModule(c->device, module, NULL);

    free(devs);
    free(qfp);
}

static void destroy_vulkan(Ctx *c)
{
    vkDeviceWaitIdle(c->device);
    vkDestroyPipeline(c->device, c->pipeline, NULL);
    vkDestroyPipelineLayout(c->device, c->playout, NULL);
    vkDestroyDescriptorPool(c->device, c->dpool, NULL);
    vkDestroyDescriptorSetLayout(c->device, c->dsl, NULL);
    vkDestroyBuffer(c->device, c->in_buf, NULL);
    vkDestroyBuffer(c->device, c->out_buf, NULL);
    vkFreeMemory(c->device, c->in_mem, NULL);
    vkFreeMemory(c->device, c->out_mem, NULL);
    vkDestroyDevice(c->device, NULL);
    vkDestroyInstance(c->instance, NULL);
}

/* ---------------- mode 1: record + submit + wait ---------------- */

static void bench_wait(Ctx *c)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->qfamily,
    };
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    double *s = malloc(sizeof(double) * WAIT_ITERS);
    size_t total = WARMUP_ITERS + WAIT_ITERS, i;

    CK(vkCreateCommandPool(c->device, &pci, NULL, &pool));
    cai.commandPool = pool;
    CK(vkAllocateCommandBuffers(c->device, &cai, &cb));
    CK(vkCreateFence(c->device, &fci, NULL, &fence));

    for (i = 0; i < total; i++) {
        double t0, t1;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cb,
        };
        CK(vkResetFences(c->device, 1, &fence));
        t0 = now_us();
        CK(vkResetCommandBuffer(cb, 0));
        record_dispatch(c, cb, 1);
        CK(vkQueueSubmit(c->queue, 1, &si, fence));
        CK(vkWaitForFences(c->device, 1, &fence, VK_TRUE, UINT64_MAX));
        t1 = now_us();
        if (i >= WARMUP_ITERS) s[i - WARMUP_ITERS] = t1 - t0;
    }

    stats("wait (rec+submit+wait)", 1, "us/iter", s, WAIT_ITERS, WAIT_ITERS);

    vkDestroyFence(c->device, fence, NULL);
    vkDestroyCommandPool(c->device, pool, NULL);
    free(s);
}

/* ---------------- mode 2: submit only, wait once ---------------- */

static void bench_submit(Ctx *c)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = c->qfamily,
    };
    VkCommandPool pool;
    VkCommandBuffer *cbs = malloc(sizeof(VkCommandBuffer) * SUBMIT_ITERS);
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = SUBMIT_ITERS,
    };
    double *s = malloc(sizeof(double) * SUBMIT_ITERS);
    uint32_t i;

    CK(vkCreateCommandPool(c->device, &pci, NULL, &pool));
    cai.commandPool = pool;
    CK(vkAllocateCommandBuffers(c->device, &cai, cbs));

    /* pre-record; recording is not part of the measured submit cost */
    for (i = 0; i < SUBMIT_ITERS; i++)
        record_dispatch(c, cbs[i], 1);

    CK(vkDeviceWaitIdle(c->device));
    for (i = 0; i < SUBMIT_ITERS; i++) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cbs[i],
        };
        double t0 = now_us();
        CK(vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE));
        s[i] = now_us() - t0;
    }
    vkQueueWaitIdle(c->queue);

    stats("submit-only (no wait)", 1, "us/submit", s, SUBMIT_ITERS,
          SUBMIT_ITERS);

    vkDestroyCommandPool(c->device, pool, NULL);
    free(cbs);
    free(s);
}

/* ---------------- mode 3: N dispatches per submit ---------------- */

static void bench_batch(Ctx *c, int n)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->qfamily,
    };
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    size_t warm_b = (size_t)BATCH_WARM / (size_t)n;
    size_t meas_b = (size_t)BATCH_MEASURED / (size_t)n;
    size_t total_b = warm_b + meas_b;
    double *s = malloc(sizeof(double) * meas_b);
    double *per = malloc(sizeof(double) * meas_b);
    size_t b, i;
    char label[64];

    CK(vkCreateCommandPool(c->device, &pci, NULL, &pool));
    cai.commandPool = pool;
    CK(vkAllocateCommandBuffers(c->device, &cai, &cb));
    CK(vkCreateFence(c->device, &fci, NULL, &fence));

    for (b = 0; b < total_b; b++) {
        double t0, t1;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cb,
        };
        CK(vkResetFences(c->device, 1, &fence));
        t0 = now_us();
        CK(vkResetCommandBuffer(cb, 0));
        record_dispatch(c, cb, (uint32_t)n);
        CK(vkQueueSubmit(c->queue, 1, &si, fence));
        CK(vkWaitForFences(c->device, 1, &fence, VK_TRUE, UINT64_MAX));
        t1 = now_us();
        if (b >= warm_b) s[b - warm_b] = t1 - t0;
    }

    for (i = 0; i < meas_b; i++) per[i] = s[i] / (double)n;
    snprintf(label, sizeof(label), "batched/dispatch N=%d", n);
    stats(label, n, "us/dispatch", per, meas_b, meas_b * (size_t)n);
    snprintf(label, sizeof(label), "batched N=%d", n);
    stats(label, n, "us/batch", s, meas_b, meas_b);

    vkDestroyFence(c->device, fence, NULL);
    vkDestroyCommandPool(c->device, pool, NULL);
    free(s);
    free(per);
}

static void *bench_thread(void *arg)
{
    Ctx *c = arg;
    double t0, t1;

    printf("[thread] main=0x%lx worker=0x%lx%s\n",
           (unsigned long)g_main_tid, (unsigned long)pthread_self(),
           pthread_equal(g_main_tid, pthread_self()) ? "  (SAME!)" : "");

    t0 = now_us();
    bench_wait(c);
    bench_submit(c);
    bench_batch(c, 1);
    bench_batch(c, 8);
    bench_batch(c, 64);
    t1 = now_us();
    printf("benchmark wall time: %.3f ms\n", (t1 - t0) / 1e3);
    return NULL;
}

int main(void)
{
    Ctx c;
    pthread_t th;
    double t0, t1;

    g_main_tid = pthread_self();
    memset(&c, 0, sizeof(c));

    printf("=== headless Vulkan submit/fence microbenchmark ===\n");
    printf("dispatch: local_size=%u, %u workgroups, %u bytes in + %u bytes out\n",
           LOCAL_SIZE, WG_COUNT, BUF_BYTES, BUF_BYTES);

    t0 = now_us();
    init_vulkan(&c);
    t1 = now_us();
    printf("init: %.3f ms\n\n", (t1 - t0) / 1e3);

    if (pthread_create(&th, NULL, bench_thread, &c) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
    pthread_join(th, NULL);

    destroy_vulkan(&c);
    return 0;
}
