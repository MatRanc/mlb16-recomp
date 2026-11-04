/*
 * Offline gate for the c690 0x2A vertex-assembler compute kernel.
 *
 * Runs the captured 0x10F78 entry through the lifted SPU module (the reference)
 * and through c690_2a_assembler.comp on the same LS image, then diffs the
 * 193 x 36-byte output records at LS 0x2F870 byte-for-byte.  Exits non-zero on
 * any mismatch, so it is also the unit test for the diff.
 *
 * Build (research; needs the lifted module from the game capture):
 *   clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *     -I /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu \
 *     -I /home/m/Documents/mlb16-online/ps3recomp-play/include \
 *     -I /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay \
 *     -o /tmp/opencode/gpu2a/test_c690_2a_shader test_c690_2a_shader.c \
 *     /tmp/opencode/c690gpu/reg_c690.c \
 *     /home/m/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
 *     /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu/job_c690_native.c \
 *     -l:libvulkan.so.1 -lm
 *
 * Run:
 *   test_c690_2a_shader /tmp/opencode/fncap21b/0x10F78 \
 *       c690_2a_assembler.comp /tmp/opencode/gpu2a/c690_2a.spv
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "spu_context.h"
#include "spu_helpers.h"

volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;
int g_spu_direct_dummy;

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;
static int g_terminal;
static spu_fn lookup_fn(uint32_t a) {
    a &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++) if (g_c690_reg[i].addr == a) return g_c690_reg[i].fn;
    return NULL;
}
void c690_stats_on_direct(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }
void spu_indirect_branch(spu_context* ctx) {
    spu_fn fn = lookup_fn(ctx->pc);
    if (!fn) { g_terminal = 1; return; }
    fn(ctx);
}
void spu_drain_call(spu_context* ctx, uint32_t ret) {
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch && (ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) { g_spu_trampoline_fn = NULL; return; }
            g_spu_trampoline_fn = NULL; tf(ctx);
            if (g_terminal) return;
        }
        if ((ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) return;
        if (g_terminal) return;
        spu_fn fn = lookup_fn(ctx->pc);
        if (fn) { g_spu_trampoline_fn = spu_indirect_branch; continue; }
        g_terminal = 1; return;
    }
}
void spu_img_restore(spu_context* c, int32_t i) { c->image_id = i; }
void spu_stop(spu_context* ctx) { (void)ctx; }
int g_c690_stats = 0, g_c690_corpus_on = 0;
int c690_native_cull_maybe(void) { return 0; }
int c690_native_cull_enter(spu_context* c) { (void)c; return 0; }
void c690_native_cull_leave(spu_context* c) { (void)c; }
void c690_corpus_stage_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_stage_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
int c690_cull_bypass_mode(void) { return 0; }
int c690_cull_fix_begin(spu_context* c, int f) { (void)c; (void)f; return 0; }
void c690_cull_fix_mid(spu_context* c) { (void)c; }
void c690_cull_fix_end(spu_context* c) { (void)c; }
int c690_native_screen_try(spu_context* c, uint32_t r, void* l) { (void)c; (void)r; (void)l; return 0; }
void c690_corpus_cull_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_cull_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
int c690_cull_fix_maybe(void) { return 0; }
int c690_cull_fix_enter(spu_context* c, int f) { (void)c; (void)f; return 0; }
void c690_cull_fix_leave(spu_context* c) { (void)c; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    switch (channel) {
    case MFC_LSA: ctx->mfc_lsa = value._u32[0]; break;
    case MFC_EAH: ctx->mfc_eah = value._u32[0]; break;
    case MFC_EAL: ctx->mfc_eal = value._u32[0]; break;
    case MFC_Size: ctx->mfc_size = value._u32[0]; break;
    case MFC_TagID: ctx->mfc_tag = value._u32[0]; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = value._u32[0]; break;
    case MFC_WrTagUpdate: ctx->mfc_tag_status = 0; break;
    default: break;
    }
}
u128 spu_rdch(spu_context* ctx, uint32_t channel) {
    u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = 0;
    switch (channel) {
    case MFC_RdTagStat: r._u32[0] = 0; break;
    case MFC_RdTagMask: r._u32[0] = ctx->mfc_tag_mask; break;
    case MFC_RdAtomicStat: r._u32[0] = 0; break;
    case SPU_RdDec: r._u32[0] = 0xFFFFFFFFu; break;
    case SPU_RdMachStat: r._u32[0] = 1; break;
    default: break;
    }
    return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t c) { (void)ctx; (void)c; return 0; }
void spu_fncap_capture(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { (void)a; (void)f; }

#define CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)_r, __FILE__, __LINE__, #expr); exit(1); } } while (0)

#define OUT_BASE  0x2F870u
#define REC_SIZE  36u
#define REC_COUNT 193u
#define OUT_BYTES (REC_SIZE * REC_COUNT)

typedef struct {
    uint32_t uv_base, col_base, pos_base, nrm_base;
    uint32_t out_base, count, pad0, pad1;
} PushConstants;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void load_exact(const char* path, void* dst, size_t want) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fread(dst, 1, want, f) != want) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
}
static uint32_t* read_file_u32(const char* path, size_t* out_words) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: bad SPIR-V size %ld\n", path, sz); exit(1); }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (!p) exit(1);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
    if (p[0] != 0x07230203u) { fprintf(stderr, "%s: not SPIR-V\n", path); exit(1); }
    *out_words = (size_t)sz / 4;
    return p;
}
static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}
static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
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

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap21b/0x10F78";
    const char* comp_path = argc > 2 ? argv[2] : "c690_2a_assembler.comp";
    const char* spv_path = argc > 3 ? argv[3] : "/tmp/opencode/gpu2a/c690_2a.spv";
    char path[1024];

    static spu_context ctx_ref;
    spu_context_init(&ctx_ref, 0);
    static spu_context c_ref;
    spu_context_init(&c_ref, 0);
    uint8_t ls0[SPU_LS_SIZE], gpr0[128 * 16];
    snprintf(path, sizeof path, "%s/ls.bin", dir);  load_exact(path, ls0, sizeof ls0);
    snprintf(path, sizeof path, "%s/gpr.bin", dir); load_exact(path, gpr0, sizeof gpr0);

    ctx_ref.image_id = 6; ctx_ref.pc = 0x10F78;
    memcpy(ctx_ref.ls, ls0, sizeof ls0);
    memcpy(ctx_ref.gpr, gpr0, sizeof gpr0);
    spu_fn f = lookup_fn(0x10F78);
    if (!f) { fprintf(stderr, "no lifted 0x10F78\n"); return 1; }

    /* Reference: lifted SPU assembler, timed over 50 iterations. */
    double t0 = now_sec();
    const int REF_ITERS = 50;
    for (int i = 0; i < REF_ITERS; i++) {
        memcpy(c_ref.ls, ls0, sizeof ls0);
        memcpy(c_ref.gpr, gpr0, sizeof gpr0);
        c_ref.image_id = 6; c_ref.pc = 0x10F78; c_ref.status = 0; c_ref.stop_code = 0;
        g_spu_trampoline_fn = NULL; g_terminal = 0;
        f(&c_ref);
        while (!g_terminal && g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            g_spu_trampoline_fn = NULL; tf(&c_ref);
        }
    }
    double ref_us = (now_sec() - t0) * 1e6 / REF_ITERS;
    const uint8_t* cpu_out = c_ref.ls + OUT_BASE;

    /* Compile GLSL -> SPIR-V with the repo's usual tool. */
    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd, "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"", spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-2a-compute-test", .apiVersion = VK_API_VERSION_1_0 };
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
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    printf("Device: %s, compute queue family %u\n", props.deviceName, qfamily);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    VkBuffer in_buf, out_buf; VkDeviceMemory in_mem, out_mem; void *in_map, *out_map;
    make_buffer(dev, phys, SPU_LS_SIZE, &in_buf, &in_mem, &in_map);
    make_buffer(dev, phys, SPU_LS_SIZE, &out_buf, &out_mem, &out_map);
    memcpy(in_map, ls0, sizeof ls0);
    memcpy(out_map, ls0, sizeof ls0);

    VkDescriptorSetLayoutBinding bind[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bind };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    PushConstants pc = { .uv_base = 0x21C30, .col_base = 0x20FB0, .pos_base = 0x20330,
                         .nrm_base = 0x1F6B0, .out_base = OUT_BASE, .count = REC_COUNT };
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
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[2] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wds[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] } };
    vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

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
    uint32_t groups = (pc.count + 63u) / 64u;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };

    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    double g0 = now_sec();
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    double g1 = now_sec();
    CHECK(vkResetFences(dev, 1, &fence));

    const uint8_t* gpu_all = (const uint8_t*)out_map;
    const uint8_t* gpu_out = gpu_all + OUT_BASE;
    size_t diff = 0, first = (size_t)-1;
    for (size_t i = 0; i < OUT_BYTES; i++)
        if (gpu_out[i] != cpu_out[i]) { if (first == (size_t)-1) first = i; diff++; }
    size_t outside = 0;
    for (size_t i = 0; i < SPU_LS_SIZE; i++) {
        if (i >= OUT_BASE && i < OUT_BASE + OUT_BYTES) continue;
        if (gpu_all[i] != ls0[i]) outside++;
    }

    /* Field-wise max error over the packed records. */
    double max_f32 = 0.0; int max_u8 = 0; size_t field_bad = 0;
    for (uint32_t v = 0; v < REC_COUNT; v++) {
        const uint8_t* r = cpu_out + v * REC_SIZE;
        const uint8_t* q = gpu_out + v * REC_SIZE;
        for (int k = 0; k < 4; k++) { int d = abs((int)r[8 + k] - (int)q[8 + k]); if (d > max_u8) max_u8 = d; if (d) field_bad++; }
        for (int k = 0; k < 7; k++) { /* uv.xy + pos.xyz + nrm.xyz */
            uint32_t bo = (k == 0) ? 0u : (k == 1) ? 4u : (k == 2) ? 12u : (k == 3) ? 16u : (k == 4) ? 20u : (k == 5) ? 24u : 28u;
            float a, b; memcpy(&a, r + bo, 4); memcpy(&b, q + bo, 4);
            uint32_t ab, bb; memcpy(&ab, r + bo, 4); memcpy(&bb, q + bo, 4);
            ab = ((ab >> 24) | ((ab >> 8) & 0xFF00) | ((ab << 8) & 0xFF0000) | (ab << 24));
            bb = ((bb >> 24) | ((bb >> 8) & 0xFF00) | ((bb << 8) & 0xFF0000) | (bb << 24));
            memcpy(&a, &ab, 4); memcpy(&b, &bb, 4);
            double d = fabs((double)a - (double)b);
            if (d > max_f32) max_f32 = d;
            if (d != 0.0) field_bad++;
        }
    }

    printf("c690 0x2A assembler: Vulkan compute vs lifted SPU\n");
    printf("Capture: %s  (%u records, %u bytes at LS 0x%05X)\n", dir, REC_COUNT, OUT_BYTES, OUT_BASE);
    printf("Differing bytes: %zu", diff);
    if (first != (size_t)-1)
        printf("  first region+0x%zX (LS 0x%zX) cpu=%02x gpu=%02x",
               first, (size_t)OUT_BASE + first, cpu_out[first], gpu_out[first]);
    printf("\nMax float error: %.9g  max u8 error: %d  fields differing: %zu\n",
           max_f32, max_u8, field_bad);
    printf("Writes outside output region: %zu byte(s)\n", outside);
    printf("Lifted 0x10F78: %.2f us/call (%d iters)\n", ref_us, REF_ITERS);
    printf("GPU single dispatch+submit+wait: %.3f ms (%.2f us)\n", (g1 - g0) * 1e3, (g1 - g0) * 1e6);

    /* Batched burst: 64 and 429 dispatches in one submission. */
    for (int rep = 0; rep < 2; rep++) {
        uint32_t n = rep ? 429u : 64u;
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        for (uint32_t i = 0; i < n; i++) vkCmdDispatch(cmd, groups, 1, 1);
        CHECK(vkEndCommandBuffer(cmd));
        double b0 = now_sec();
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        double b1 = now_sec();
        CHECK(vkResetFences(dev, 1, &fence));
        printf("GPU batched N=%u in one submission: %.3f ms total, %.4f us/dispatch\n",
               n, (b1 - b0) * 1e3, (b1 - b0) * 1e6 / n);
    }

    int pass = (diff == 0 && outside == 0);
    printf("RESULT: %s\n", pass ? "PASS (bit-exact)" : "FAIL");

    vkUnmapMemory(dev, in_mem); vkUnmapMemory(dev, out_mem);
    vkDestroyFence(dev, fence, NULL); vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyBuffer(dev, in_buf, NULL); vkDestroyBuffer(dev, out_buf, NULL);
    vkFreeMemory(dev, in_mem, NULL); vkFreeMemory(dev, out_mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    free(spv); free(phys_list);
    return pass ? 0 : 1;
}
